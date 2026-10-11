// SPDX-License-Identifier: GPL-3.0-or-later
// PartyDirector and the coop main-thread tick (see party_director.h). The test mode follows
// droogie/bbhost src/engine/np_test.cpp @8f2746c (GPL-3.0-or-later): world-up detection, the
// Insight write and the bell events.
#include "party_director.h"
#include "../net/netsim.h"
#include "party_hud.h"
#include "party_balance.h"
#include "party_async_io.h"

#include "coop_hooks.h"
#include "game_state.h"
#include "lua_events.h"
#include "party_b2.h"
#include "party_fourp.h"
#include "party_link.h"
#include "party_story.h"
#include "party_catchup.h"
#include "party_status_bridge.h"
#include "party_chalice.h"
#include "party_items.h"
#include "party_npc_test.h"
#include "party_guestsvc.h"
#include "party_split.h"
#include "party_phantom.h"
#include "party_start.h"
#include "party_runtime.h"
#include "party_progress.h"
#include "party_travel.h"
#include "seamless_rules.h"
#include "test_newgame.h"
#include "../bbport_game_menu.h"
#include "party_online.h"
#include "party_perf.h"
#include "party_watchdog.h"
#include "party_worldact.h"
#include "../bbport_game_menu.h"

#include "../net/bbnet_internal.h"
#include "../net/party_host_service.h"
#include "../../party_env.h"

#include <algorithm>
#include <cmath>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <chrono>
#include <cstdio>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <vector>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

extern "C" void runtime_pad_director_press(std::uint32_t buttons); // src/runtime_pad.c
extern "C" std::uint32_t runtime_pad_last_buttons(); // src/runtime_pad.c: the last sample the game read

namespace coop {
namespace {

using Clock = std::chrono::steady_clock;

constexpr const char* kHostBell = "OnEvent_Call_SOS";
constexpr const char* kGuestBell = "OnEvent_SendSoulSign_NormalCoop";

// SprjFlipper::Update (bbhost 0x2434770): push rbp; mov rbp, rsp; push r15, r14, r13, r12, rbx;
// sub rsp, 0x38 - 17 bytes, no relative operands. Its one caller is the frame function 0x2018d20
// (SprjWindow update, this, then the SprjTask step 0x20512a0).
constexpr std::uint64_t kFlipperUpdate = 0x2034770;
constexpr std::uint64_t kDispatchByName = 0x1339870;

std::uint64_t ThreadId() {
#ifdef _WIN32
    return GetCurrentThreadId();
#else
    return std::uint64_t(pthread_self());
#endif
}

double Seconds(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double>(b - a).count();
}

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[768];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    perf::Scope z(perf::Zone::Log);
    coop::asyncio::Print(std::string("Party: ") + PartyLogStamp() + line + "\n"); // never blocks the caller (party_async_io.h)
}

/// A live travel test step (BB_PARTY_DIRECTOR_TEST host_warp=ID@S, host_die@S, guest_die@S):
/// runs S seconds after this player's next (re)join - the host's cooperators 0 -> >= 1, the
/// guest's client session with the world loaded - on the main thread, in list order.
struct TravelStep {
    // HostMark / GuestMark (steady-world clock, as host_move; %MAP filter): the Hunter's Mark (the Lua event "OnReviveMagic", what the player's
    // special-effect update 0x18FEF50 raises for a stateInfo 0x17 effect). HostStage: Lua
    // WarpNextStage 0x132E010 (args area:block:region:index:warp point), or WarpNextStage_Bonfire
    // 0x132E050 (args bonfire:ID). HostWalk: the host's chr put at each x:y:z:yaw (separated by
    // '/') 1 s apart (repeat a spot to stay there), no load (streamed blocks). HostHeadstone: talk to the headstone t<id> (stand
    // 1.2 m in front, Cross), wait for its menu, Down N times, Cross (args id:N).
    enum Kind {
        HostWarp, HostDie, GuestDie, HostKill, HostMove, GuestMove,
        HostPress, GuestPress, HostMark, GuestMark, HostStage, HostWalk, HostHeadstone, HostFlag, HostSkipRemo, GuestSkipRemo, HostLastLamp,
        // Split travel (docs/party/split_travel.md): host_lamp=ID / guest_lamp=ID use lamp ID as the
        // player would (0x13CDF30 through the interceptor: the "Travel with the party?" question);
        // host_alone=ID: the host travels to ID alone, decided (no question).
        HostLamp, GuestLamp, HostAlone,
        HostOutage, GuestOutage, // a simulated total network outage of `buttons` ms (BB_NET_SIM on)
        HostObj, GuestObj, HostChr, GuestChr // stand at a map object / character: ENTITY[:DIST[:ANGLE]]
    } kind = HostWarp;
    std::uint32_t id = 0; // HostWarp: the ReturnPointParam row for 0x13CDF30; HostKill: the entity id
    float pos[3] = {};    // HostMove / GuestMove: the chr warp target (map coordinates)
    float yaw = 0;        // HostMove / GuestMove: rotation about Y (radians) when has_yaw
    bool has_yaw = false;
    std::uint32_t buttons = 0;  // HostPress / GuestPress: pad buttons tapped (0.15 s)
    std::string map;      // HostMove / GuestMove "%NAME": only while the current map name contains it
    std::string args;     // HostStage / HostWalk / HostHeadstone
    double delay = 15.0;
    int min_join = 0;     // "#N": not before this player's Nth join
};

struct Test {
    bool on = false;
    bool log_state = false;
    int insight = -1;
    int echoes = -1; // echoes=N: writes the player's Blood Echoes once (test saves)
    // approach=X:Z:R[:BUTTON]: walk (pad file sticks, camera-relative, closed loop on the position)
    // until within R m of (X, Z), facing it; then BUTTON (cross / triangle / square / circle) once.
    bool approach = false, approach_done = false;
    float ap_x = 0, ap_z = 0, ap_r = 1.0f;
    std::uint32_t ap_button = 0;
    int ap_steps = 0, ap_phase = 0; // 0 idle, 1 moving, 2 settling, 3 arrived, 4 pressing
    double ap_angle = 0;
    float ap_from[2] = {0, 0};
    Clock::time_point ap_at{};
    // stand=X:Y:Z:YAW[:BUTTON]: puts the player there in place (PlayerIns vfunc +0x5F8, no map
    // reload; yaw t faces (-sin t, -cos t), agent/guestsvc's Doll fixture), then BUTTON once 1.5 s on.
    bool stand = false, stand_done = false;
    float st_pos[3] = {0, 0, 0}, st_yaw = 0;
    std::uint32_t st_button = 0;
    int st_phase = 0;
    MenuScript st_seq; // seq=...: after stand (and its button), the menu walk (test_newgame.h)
    bool st_seq_on = false;
    std::string st_last_texts;
    Clock::time_point st_at{};
    bool log_pos = false; // log_pos: the player's area and position (FrpgNetMan +0xa7c / +0xaac) every 3 s
    Clock::time_point pos_at{};
    const char* ring = nullptr; // the event to raise once
    double delay = 10.0;
    bool start = false, grant_bells = false, drop_bells = false, drop_done = false;
    int award_lot = -1; // award_lot=N: AwardItemLot(N) once (call-shape probe, test saves)
    std::vector<std::pair<int, int>> goods; // goods=ID:N: tops goods ID up to N once (test saves)
    bool goods_done = false;
    bool place = false, place_done = false; // place=MAP:X:Y:Z:YAW (hex map): one forced placement
    std::uint32_t place_map = 0;
    float place_pos[3] = {0, 0, 0}, place_yaw = 0;
    double place_at = 15.0;
    std::uint32_t warp_point = 0; // warp=MAP:POINT (hex map): one stage warp, place's timing
    std::vector<std::uint32_t> set_flags; // set_flag=ID[:ID...]: SetEventFlag(ID, 1) once (C2 live check)
    bool coop_seen = false;               // host in a party: the actions wait for a cooperator
    Clock::time_point coop_at{};
    bool insight_done = false, ring_done = false;
    const char* bell = nullptr; // bell_host / bell_guest: Ring() once ready (the real bell path)
    int bell_area = -1;         // bell_host@AA: only in a map of area AA (e.g. 29: inside a Chalice Dungeon)
    bool bell_done = false;
    Clock::time_point rung_at{};
    // Travel steps (see TravelStep): the next one waits for a join after the previous one ran.
    std::vector<TravelStep> steps;
    std::size_t next_step = 0;
    bool joined = false;       // in the party now (host: cooperators >= 1; guest: client, loaded)
    bool armed = false;        // a join since the last step ran
    int joins = 0;
    Clock::time_point joined_at{};
    Clock::time_point step_at{}; // the last step's run, for "back after" times
    bool ran = false;            // a step of this role ran
    TravelStep::Kind last_kind = TravelStep::HostWarp;
    // HostKill waits for no re-join (a B2 host keeps its cooperators through a load): it runs S s
    // after the world was steady again with a cooperator, counted from here.
    Clock::time_point steady_at{};
    bool steady = false;
    // A running multi-tick step (HostWalk / HostHeadstone).
    struct Script {
        TravelStep::Kind kind = TravelStep::HostWalk;
        std::string args;
        int stage = 0, presses = 0;
        Clock::time_point at{};
        bool pressed = false;
        std::uint64_t text_seq = 0;
    } script;
    bool script_on = false;
    Clock::time_point release_at{};  // a press step's buttons are released then
    bool pressing = false;
};

struct State {
    std::mutex mu; // role / link (set from other threads)
    PartyRole role = PartyRole::None;
    int max_players = 3;
    party::PartyLink* link = nullptr;
    bool auto_ring = true;
    int auto_continue = -1; // BB_PARTY_AUTOCONTINUE: -1 default (on in party mode), 0 off, 1 on
    double ring_every = 30.0;
    bool a11_waiting = false;  // A11: the host's bell held (boss fight / cutscene)
    // BB_PARTY_FAST_JOIN (default on; docs/party/join_timing.md): shorter director pauses.
    bool fast_join = true;
    double ring_delay = 0.3;        // s in a steady world before the first bell (5 without fast join)
    double continue_settle = 1.0;   // s on the title before the first Continue press (4)
    double continue_every = 1.0;    // s between Continue presses (2.5)
    StartMode start_mode = StartMode::PrologueSolo;
    bool grant_bells = true;
    Test test;

    // Main thread only.
    bool started = false;
    Clock::time_point start{};
    GameSnapshot last{};
    bool have_last = false;
    bool world_seen = false;
    Clock::time_point world_at{};
    Clock::time_point last_full{}, last_log{}, last_ring{}, chalice_wait_at{};
    bool rung_ever = false;
    bool poll_logged = true; // the sign polling 2.5 s after a bell (join timing)
    std::uint64_t signs_seen = 0; // host: bbnet::sign_board_changes() last acted on
    std::uint64_t summons_seen = 0; // host: bbnet::summon_requests() last seen
    Clock::time_point summon_at{};  // host: when our game last asked to summon someone
    std::vector<std::string> restarted; // host: members whose game restarted, stale peer not cleared yet
    std::uint64_t tick_thread = 0;
    std::uint64_t ticks = 0, ticks_at_log = 0, foreign_ticks = 0;
    party::MemberState sent_state = party::MemberState::Title;
    std::uint32_t sent_map = 0;
    bool sent_any = false;
    // Campaign start (party_start.h), refreshed with the full reads.
    StartFlags cs{};
    bool start_ready = false, start_logged = false;
    StartStep start_step = StartStep::NoWorld;
    bool granted = false; // bells granted (or found owned) in this world
    // Title "Continue" (AutoContinue).
    bool continue_done = false, continue_held = false;
    int continue_presses = 0;
    Clock::time_point continue_ready{}, continue_last{};
    bool continue_waiting = false;
    bool continue_signin = false;
    std::string last_from_client;
    // Host session watchdog (party_watchdog.h).
    HostWatchdog watchdog;
    int watchdog_stops = 0;
    bool watchdog_check = false; // after a stop: log the session / phantoms once it is idle
    Clock::time_point watchdog_at{};
};

State& S() {
    static State s;
    return s;
}

std::atomic<bool> g_tick_installed{false};

// ---- The game's own Lua dispatches (test mode: log_state) ----
std::atomic<std::uint64_t> g_dispatch_main{0}, g_dispatch_other{0};
std::mutex g_dispatch_mu;
std::map<std::string, std::uint64_t> g_dispatch_seen; // under g_dispatch_mu

BB_COOP_SYSV void DispatchEntry(std::uint64_t ctx, std::uint64_t name_ptr, std::uint64_t, std::uint64_t,
                                std::uint64_t, std::uint64_t) {
    char name[80] = "?";
    for (std::size_t i = 0; name_ptr && i + 1 < sizeof name; ++i) {
        char c = 0;
        if (!SafeGet(name_ptr + i, &c) || !c) {
            name[i] = 0;
            break;
        }
        name[i] = (c >= 0x20 && c < 0x7f) ? c : '?';
        name[i + 1] = 0;
    }
    const std::uint64_t tid = ThreadId();
    const bool main = tid == S().tick_thread;
    (main ? g_dispatch_main : g_dispatch_other).fetch_add(1, std::memory_order_relaxed);
    std::uint64_t n;
    {
        std::lock_guard<std::mutex> lk(g_dispatch_mu);
        n = ++g_dispatch_seen[name];
    }
    if (n <= 3 || (n & 255) == 0) {
        Log("game Lua event %s (#%llu, ctx 0x%llx, thread %llu%s)", name, static_cast<unsigned long long>(n),
            static_cast<unsigned long long>(ctx), static_cast<unsigned long long>(tid),
            main ? ", the tick's" : ", NOT the tick's thread");
    }
}

BB_COOP_SYSV void FlipperEntry(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                               std::uint64_t) {
    CoopTick();
}

// ss.info parser (0x1eb65e0(alloc, text, &N, &L, out)) at its one call 0x1e89a21 in the
// ss.info completion 0x1e89850: logs what the game parsed and the result (title debugging;
// BB_PARTY_TRACE_SSINFO=1).
using SsParseFn = std::uint64_t(BB_COOP_SYSV*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                               std::uint64_t);
constexpr std::uint64_t kSsParse = 0x1eb65e0, kSsParseCall = 0x1e89a21;
__attribute__((noinline)) BB_COOP_SYSV std::uint64_t SsParseHook(std::uint64_t a0, std::uint64_t text,
                                                                 std::uint64_t n, std::uint64_t l, std::uint64_t out) {
    std::uint32_t nv = 0, lv = 0;
    SafeGet(n, &nv);
    SafeGet(l, &lv);
    // An MSVC-style std::string: +8 inline buffer or pointer (capacity +0x20 > 15), size +0x18.
    std::uint64_t size = 0, cap = 0, data = text + 8;
    SafeGet(text + 0x18, &size);
    SafeGet(text + 0x20, &cap);
    if (cap > 15) {
        SafeGet(text + 8, &data);
    }
    char head[161] = {};
    SafeRead(data, head, size < 160 ? size : 160);
    for (char& c : head) {
        if (c == '\n' || c == '\r') c = ' ';
    }
    const std::uint64_t r = reinterpret_cast<SsParseFn>(Guest(kSsParse))(a0, text, n, l, out);
    Log("ss.info parse: N %u, lang %u, %llu bytes (cap %llu) -> %s; text: %s", nv, lv,
        static_cast<unsigned long long>(size), static_cast<unsigned long long>(cap), (r & 0xff) ? "ok" : "FAILED",
        head);
    return r;
}

// BB_PARTY_TRACE_BELL=1: entry logs along the bell / sign / summon path (first 5 calls of each,
// then every 500th), to see how far a bell gets.
struct TracePoint {
    std::uint64_t off;
    const char* name;
    std::atomic<std::uint64_t> calls{0};
};
TracePoint g_trace_points[] = {
    {0x1900500, "begin sign (0x1900500)"},
    {0x130cde0, "sign Lua task op (0x130cde0)"},
    {0x1901010, "sign task body (0x1901010)"},
    {0x1901320, "SendSign"},
    {0x14b9180, "CreateSign"},
    {0x14b5e10, "SummonStepManager tick"},
    {0x1e90e30, "summon_messenger/create builder"},
    {0x1e946e0, "summon_messenger/get builder"},
    {0x14ba980, "SSM add received sign"},
    {0x14baec0, "host picks a sign (messenger target)"},
    {0x1e98650, "summon_messenger/request builder"},
};
template <int I>
BB_COOP_SYSV void TraceEntry(std::uint64_t a0, std::uint64_t a1, std::uint64_t a2, std::uint64_t, std::uint64_t,
                             std::uint64_t) {
    TracePoint& t = g_trace_points[I];
    const std::uint64_t n = ++t.calls;
    if (n <= 5 || n % 500 == 0) {
        Log("bell trace: %s (+0x%llx) call #%llu (a0 0x%llx a1 0x%llx a2 0x%llx)", t.name,
            static_cast<unsigned long long>(t.off), static_cast<unsigned long long>(n),
            static_cast<unsigned long long>(a0), static_cast<unsigned long long>(a1),
            static_cast<unsigned long long>(a2));
    }
}
template <int... I>
void InstallBellTrace(std::integer_sequence<int, I...>) {
    (HookPrologue(g_trace_points[I].off, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}, &TraceEntry<I>,
                  g_trace_points[I].name),
     ...);
}

party::MemberState LocalState(const GameSnapshot& s, bool start_ready) {
    if (s.loading) {
        return party::MemberState::Loading;
    }
    if (!s.world_up) {
        return party::MemberState::Title;
    }
    if (s.session_role == RoleClient) {
        return party::MemberState::InHostWorld;
    }
    if (s.session_role == RoleTryingToJoin) {
        return party::MemberState::Joining;
    }
    return OwnWorldState(start_ready);
}

bool InWorld(const GameSnapshot& s) {
    return s.world_up && !s.loading;
}

/// The start flags (full ticks): logs step and readiness changes.
void UpdateStart(State& st, const GameSnapshot& s, StartMode mode) {
    if (s.world_up && s.loading) {
        return; // a loading screen keeps the last reading (the flags do not go back)
    }
    const bool in_world = InWorld(s);
    if (in_world) {
        st.cs = ReadStartFlags();
    } else if (!s.world_up) {
        st.cs = StartFlags{};
        st.granted = false;
    }
    const StartStep step = ClassifyStart(st.cs, in_world);
    const bool ready = StartReady(mode, st.cs, in_world);
    if (!st.start_logged || step != st.start_step || ready != st.start_ready) {
        if (in_world || st.start_logged) {
            Log("start (%s): %s -> %s", StartModeName(mode), DescribeStart(st.cs, in_world).c_str(),
                ready ? "READY for party summons" : "not ready");
        }
        st.start_logged = st.start_logged || in_world;
    }
    st.start_step = step;
    st.start_ready = ready;
}

/// The bells a ready player lacks (§4.3), once per world, in its own world only.
void GrantBells(State& st, const GameSnapshot& s) {
    if (st.granted || !st.start_ready || !InWorld(s) || s.session_role != RoleIdle || st.cs.cutscene) {
        return;
    }
    const BellGrant g = PlanBellGrant(st.cs);
    if (st.cs.beckoning_count < 0 || st.cs.resonant_count < 0) {
        return; // inventory not readable yet
    }
    st.granted = true;
    if (!g.any()) {
        return;
    }
    Log("bells: before: %s", DescribeStart(st.cs, true).c_str());
    if (g.beckoning_lot) {
        Log("bells: Beckoning Bell (lot %u): %s", kLotBeckoning, AwardItemLot(kLotBeckoning) ? "awarded" : "NOT awarded");
    }
    if (g.beckoning_goods) {
        Log("bells: Beckoning Bell (goods %u, 6622 already on): %s", kGoodsBeckoning,
            GiveGoods(kGoodsBeckoning, 1) ? "given" : "NOT given");
    }
    if (g.resonant) {
        const bool ok = GiveGoods(kGoodsSmallResonant, 1);
        const bool flag = ok && WriteEventFlag(kFlagResonantShop, true);
        Log("bells: Small Resonant Bell (goods %u): %s, flag %u %s", kGoodsSmallResonant, ok ? "given" : "NOT given",
            kFlagResonantShop, flag ? "set" : "not set");
    }
    st.cs = ReadStartFlags();
    Log("bells: after: %s", DescribeStart(st.cs, true).c_str());
}

void LogTransitions(State& st, const GameSnapshot& now) {
    if (!st.have_last) {
        Log("state at the first tick: %s", Describe(now).c_str());
        return;
    }
    const GameSnapshot& was = st.last;
    if (now.world_up != was.world_up) {
        Log("world %s: %s", now.world_up ? "up" : "down", Describe(now).c_str());
        if (!now.world_up) BossBarReset();  // A11: a load ends any boss fight
    }
    if (now.loading != was.loading) {
        Log("loading screen %s", now.loading ? "up" : "gone");
    }
    if (now.session_role != was.session_role || now.session_sub_state != was.session_sub_state) {
        Log("session %s (%d, sub %d) -> %s (%d, sub %d) (t %.1f s)", SessionRoleName(was.session_role), was.session_role,
            was.session_sub_state, SessionRoleName(now.session_role), now.session_role, now.session_sub_state,
            Seconds(st.start, Clock::now()));
    }
    if (now.matching_status != was.matching_status) {
        Log("matching status %d -> %d", was.matching_status, now.matching_status);
    }
    if (now.map_id != was.map_id && now.world_up) {
        Log("map %s -> %s", MapName(was.map_id).c_str(), MapName(now.map_id).c_str());
    }
    if (now.cooperators != was.cooperators && now.cooperators >= 0 && was.cooperators >= 0) {
        Log("cooperators %d -> %d (t %.1f s)", was.cooperators, now.cooperators, Seconds(st.start, Clock::now()));
    }
    if (now.online_mode != was.online_mode) {
        Log("online mode %d -> %d", was.online_mode, now.online_mode);
    }
    if (now.insight != was.insight && now.insight >= 0 && was.insight >= 0) {
        Log("Insight %d -> %d", was.insight, now.insight);
    }
}

std::string DescribeFromClient();
std::string DescribeSignPolling();

// The bells as the items ring them: the goods' SpEffect on the local player (Beckoning Bell 200
// -> 9000, Small Resonant Bell 205 -> 9005). The player's update (0x18FEF50) sees the effect's
// param flag and calls 0x1900500(player, sign type), which queues the Lua task (0x1314A40, op
// 0x130CDE0) that runs SendSign 0x1901010/0x1901320 -> CreateSign -> summon_messenger/create; the
// active-bell updater 0x1506820 keeps the searching visuals (9003/9004, 9008/9009). The named Lua
// events OnEvent_Call_SOS / OnEvent_SendSoulSign_NormalCoop are what the game raises AFTER a
// summon / a sign (0x130CA87, 0x1901794): raising them only runs the Lua reaction (animation,
// RecallMenuEvent), never a sign (BB_PARTY_TRACE_BELL=1 showed no SendSign / CreateSign).
// Apply: ChrIns vtable +0x3F0 (chr, id, source chr, 0, 1, 0, 0; xmm0-4 = 1.0), the call the
// updater itself makes at 0x1506A29.
constexpr int kHostBellEffect = 9000, kGuestBellEffect = 9005;
constexpr int kChaliceWaitEffect = 9900; // the Dream bell's chalice co-op wait (stateInfo 269)
using AddSpEffectFn = void(BB_COOP_SYSV*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                          std::uint64_t, std::uint64_t, float, float, float, float, float);

std::uint64_t LocalPlayer() {
    std::uint64_t wcm = 0, player = 0;
    SafeGet(Guest(0x553e878), &wcm);
    if (wcm) {
        SafeGet(wcm + 0x60, &player);
    }
    return player;
}

/// The player has SpEffect `id` (list at ChrIns +0x1C8: first node at +8, id +0x40, next +0x58).
bool HasSpEffect(std::uint64_t player, int id) {
    std::uint64_t list = 0, node = 0;
    if (!player || !SafeGet(player + 0x1c8, &list) || !list || !SafeGet(list + 8, &node)) {
        return false;
    }
    for (int guard = 0; node && guard < 512; ++guard) {
        std::int32_t nid = 0;
        if (!SafeGet(node + 0x40, &nid)) {
            return false;
        }
        if (nid == id) {
            return true;
        }
        if (!SafeGet(node + 0x58, &node)) {
            return false;
        }
    }
    return false;
}

/// Main thread only (the director's tick).
bool ApplySpEffect(int id) {
    const std::uint64_t player = LocalPlayer();
    std::uint64_t vt = 0, fn = 0;
    if (!player || !SafeGet(player, &vt) || !vt || !SafeGet(vt + 0x3f0, &fn) || !fn) {
        return false;
    }
    reinterpret_cast<AddSpEffectFn>(fn)(player, static_cast<std::uint64_t>(id), player, 0, 1, 0, 0, 1.0f, 1.0f, 1.0f,
                                        1.0f, 1.0f);
    return true;
}

/// Fast join, host: the game's peer pool (SummonStepManager [FrpgNetMan+0xc50] +0x48; used list
/// sentinel at +0x58, node {next, prev, value +0x10}, peer = [value+0x20]) keeps a peer reserved
/// for a summoned player's online id (+0x64, 16 bytes) while its keep flag (+0xe4) is set; a new
/// summon of the same id (0x147af20) renews and reuses it even when its connection belongs to a
/// crashed game process, so the summon data goes nowhere (docs/party/join_timing.md). For a
/// member that restarted, the stale peers lose the id and the keep: the next summon reserves a
/// fresh peer. Main thread only. Returns how many peers were cleared.
int ClearStalePeers(const std::string& online_id) {
    std::uint64_t fm = 0, ssm = 0, pool = 0, head = 0, node = 0;
    if (!SafeGet(Guest(0x553b120), &fm) || !fm || !SafeGet(fm + 0xc50, &ssm) || !ssm || !SafeGet(ssm + 0x48, &pool) ||
        !pool || !SafeGet(pool + 0x58, &head) || !head || !SafeGet(head, &node)) {
        return -1;
    }
    int cleared = 0;
    for (int guard = 0; node && node != head && guard < 64; ++guard) {
        std::uint64_t value = 0, peer = 0;
        char id[17] = {};
        if (SafeGet(node + 0x10, &value) && value && SafeGet(value + 0x20, &peer) && peer &&
            SafeRead(peer + 0x64, id, 16) && std::strncmp(id, online_id.c_str(), 16) == 0) {
            const char mark = '#'; // no online id starts with it
            const std::uint8_t off = 0;
            SafeWrite(peer + 0x64, &mark, 1);
            SafeWrite(peer + 0xe4, &off, 1);
            ++cleared;
        }
        if (!SafeGet(node, &node)) {
            break;
        }
    }
    return cleared;
}

void Ring(State& st, const char* event, const char* why, Clock::time_point now) {
    if (!st.rung_ever && st.world_seen) {  // A4 join speed: the first bell's wait in the world
        Log("first bell %.1f s after the world was steady", Seconds(st.world_at, now));
    }
    st.last_ring = now;
    st.rung_ever = true;
    st.poll_logged = false;
    const bool host = std::strcmp(event, kHostBell) == 0;
    const char* mode = std::getenv("BB_PARTY_BELL_MODE");
    if (mode && std::strcmp(mode, "lua") == 0) {
        if (LuaEventQueue(event)) {
            party::bridge::OnRing();
            Log("ringing %s (%s; Lua event)", event, why);
        } else {
            Log("could not queue %s (%s)", event, why);
        }
        return;
    }
    const int effect = host ? kHostBellEffect : kGuestBellEffect;
    const std::uint64_t player = LocalPlayer();
    const bool had = HasSpEffect(player, effect);
    const bool ok = ApplySpEffect(effect);
    // A guest while the host is in a Chalice Dungeon: also the vanilla Dream bell's effect 9900
    // (SpEffectParam stateInfo 269). The player's update (0x193ac10) turns it into FrpgNetMan +0xb9c
    // bit 0 -> +0xb14 bit 0; only with it does the summon handler 0x14ba340 take the message's
    // ChannelId (state 2), fetch the dungeon (get_details_info) and quick-join it before loading
    // the host's map (0x14bb200 -> 0x1aa5280). Without it the guest loads its own Dream (seen
    // live). The party's Dream bell patch (0x157F6D1) rings the normal 9005 instead.
    if (!host && ok) {
        std::uint32_t host_map = 0;
        party::PartyLink* link = nullptr;
        {
            std::lock_guard<std::mutex> lk(st.mu);
            link = st.link;
        }
        if (link) {
            for (const party::RosterEntry& e : link->roster()) {
                if (e.slot == party::kHostSlot) {
                    host_map = e.map_id;
                }
            }
        }
        if (((host_map >> 24) & 0xff) == 29 && host_map != 0xffffffffu) {
            const bool chalice_ok = ApplySpEffect(kChaliceWaitEffect);
            Log("bell: the host is in a chalice dungeon (%s): SpEffect %d %s", MapName(host_map).c_str(),
                kChaliceWaitEffect, chalice_ok ? "applied too (quick join on summon)" : "NOT applied");
        }
    }
    if (ok) {
        party::bridge::OnRing();
    }
    Log("bell: %s", DescribeFromClient().c_str());
    Log("bell: %s", DescribeSignPolling().c_str());
    Log("ringing the %s (%s): SpEffect %d %s%s", host ? "Beckoning Bell" : "Small Resonant Bell", why, effect,
        ok ? "applied" : "NOT applied (no player)", ok ? (HasSpEffect(player, effect) ? (had ? ", was already on" : ", now on")
                                                                              : ", not on the list after the call")
                                          : "");
}

constexpr std::uint32_t kPadCross = 0x4000;

/// The game's FROM-server client as it stands (title debugging): SprjNetworkClientMan (slot
/// 0x5540288: +0x18 parsed ss.info, its +8 <ss> status; +0x3e4 ss.info failures; +0x60 UserId,
/// +0xd0 ss.info reload timer) and FrpgNetMan (slot 0x553b120: +0xa online flag, +0xa30 queued
/// error message, +0xa50 server-offline flag).
std::string DescribeFromClient() {
    std::uint64_t cm = 0, fm = 0, ss = 0;
    SafeGet(Guest(0x5540288), &cm);
    SafeGet(Guest(0x553b120), &fm);
    std::int32_t ss_status = -1, fails = -1, msg = -1;
    std::int64_t user = -1;
    float reload = -1;
    std::uint8_t online = 0xff, offline = 0xff, b8 = 0xff, b9f6 = 0xff, b9f8 = 0xff, bb = 0xff;
    if (cm) {
        SafeGet(cm + 0x18, &ss);
        SafeGet(cm + 0x3e4, &fails);
        SafeGet(cm + 0x60, &user);
        SafeGet(cm + 0xd0, &reload);
        if (ss) {
            SafeGet(ss + 8, &ss_status);
        }
    }
    if (fm) {
        SafeGet(fm + 0xa, &online);
        SafeGet(fm + 0xa30, &msg);
        SafeGet(fm + 0xa50, &offline);
        SafeGet(fm + 8, &b8);
        SafeGet(fm + 0xb, &bb);
        SafeGet(fm + 0x9f6, &b9f6);
        SafeGet(fm + 0x9f8, &b9f8);
    }
    char b[256];
    std::snprintf(b, sizeof b,
                  "FROM client: ss.info %s (status %d, failures %d, reload in %.0f s), user id %lld; FrpgNetMan online %d, "
                  "error msg 0x%x, server-offline %d (+8 %d, +0xb %d, +0x9f6 %d, +0x9f8 %d)",
                  ss ? "parsed" : "none", ss_status, fails, reload, static_cast<long long>(user), online, msg, offline,
                  b8, bb, b9f6, b9f8);
    return b;
}

/// The sign polling as the game holds it (join timing, docs/party/join_timing.md): the parsed
/// ss.info ([cm+0x18]: +0x8a0 SummonDataGetListInterval, +0x8a8..+0x8b4 the WaitCounts) and the
/// FROM client's own countdown (+0x100) and per-type wait counters (+0xb4 normal coop, +0xbc/+0xc0
/// invasion, +0xc4 natural enemy), read by the get-list builder 0x1e946e0.
std::string DescribeSignPolling() {
    std::uint64_t cm = 0, ss = 0;
    SafeGet(Guest(0x5540288), &cm);
    if (cm) {
        SafeGet(cm + 0x18, &ss);
    }
    std::int32_t interval = -1, w0 = -1, w1 = -1, w2 = -1, w3 = -1, c0 = -1, c1 = -1, c2 = -1, c3 = -1;
    float countdown = -1;
    if (ss) {
        SafeGet(ss + 0x8a0, &interval);
        SafeGet(ss + 0x8a8, &w0);
        SafeGet(ss + 0x8ac, &w1);
        SafeGet(ss + 0x8b0, &w2);
        SafeGet(ss + 0x8b4, &w3);
    }
    if (cm) {
        SafeGet(cm + 0x100, &countdown);
        SafeGet(cm + 0xb4, &c0);
        SafeGet(cm + 0xbc, &c1);
        SafeGet(cm + 0xc0, &c2);
        SafeGet(cm + 0xc4, &c3);
    }
    char b[256];
    std::snprintf(b, sizeof b,
                  "sign polling: interval %d s (next in %.2f s), wait counts coop %d / invasion %d / enemy %d / all-area %d; "
                  "counters %d %d %d %d",
                  interval, countdown, w0, w1, w2, w3, c0, c1, c2, c3);
    return b;
}

/// Title screen: confirms "Continue" (the main menu's default item once a save exists) with the
/// game's own pad path (runtime_pad_director_press: no OS input) until the world loads. Cross is
/// pressed 0.15 s every 2 s while no world was seen in this run, never in the world or while a
/// loading screen is up; the party link must be up first (the online login runs through it).
void AutoContinue(State& st, const GameSnapshot& s, PartyRole role, party::PartyLink* link, Clock::time_point now) {
    if (st.continue_done) {
        return;
    }
    auto release = [&] {
        if (st.continue_held) {
            runtime_pad_director_press(0);
            st.continue_held = false;
        }
    };
    if (s.world_up || st.world_seen) {
        release();
        st.continue_done = true;
        Log("auto-continue: world up after %d press(es)", st.continue_presses);
        return;
    }
    if (s.loading) {
        release();
        st.continue_waiting = false;
        return;
    }
    const party::LinkState ls = link ? link->state() : party::LinkState::Idle;
    const bool link_ok = role == PartyRole::Host ? ls == party::LinkState::Hosting : ls == party::LinkState::Connected;
    if (!link_ok) {
        release();
        st.continue_waiting = false;
        return;
    }
    if (!st.continue_waiting) {
        st.continue_waiting = true;
        st.continue_ready = now;
        st.continue_last = now;
        Log("auto-continue: party link up; confirming Continue on the title (%s)",
            party::runtime().restarted() ? "crash restart" : "party mode");
    }
    // Let the title settle (logos, online login) before the first press.
    if (Seconds(st.continue_ready, now) < st.continue_settle) {
        return;
    }
    if (st.continue_held) {
        if (Seconds(st.continue_last, now) >= 0.15) {
            release();
        }
        return;
    }
    // The FROM sign-in (ss.info -> login -> sync_chara_id -> notices) starts once the title takes
    // its first press; Continue while it runs leaves the game offline: wait for it (<= 30 s).
    const bbnet::OnlineProgress op = bbnet::online_progress();
    const bool signing_in = op.ss_info > 0 && op.notice == 0 && op.since_last >= 0 && op.since_last < 30.0;
    if (signing_in != st.continue_signin) {
        st.continue_signin = signing_in;
        Log("auto-continue: %s (ss.info %d, login %d, chara id %d, notice %d, failures %d); %s",
            signing_in ? "the FROM sign-in runs; holding Continue" : "the FROM sign-in is over", op.ss_info, op.login,
            op.chara_id, op.notice, op.failures, DescribeFromClient().c_str());
    }
    if (signing_in) {
        st.continue_last = now;
        return;
    }
    if (Seconds(st.continue_last, now) >= st.continue_every) {
        st.continue_last = now;
        st.continue_held = true;
        ++st.continue_presses;
        runtime_pad_director_press(kPadCross);
        if (st.continue_presses <= 5 || st.continue_presses % 10 == 0) {
            Log("auto-continue: Cross #%d (%s; %s)", st.continue_presses, Describe(s).c_str(), DescribeFromClient().c_str());
        }
    }
}

constexpr std::uint64_t kLampWarp = 0x13cdf30; // void (u32 ReturnPointParam id): lamp / Dream / headstone
constexpr std::uint64_t kForceDead = 0x13320c0; // Lua ForceDead(ctx unused, entity id; 10000 = local player)

const char* TravelStepName(TravelStep::Kind k) {
    switch (k) {
    case TravelStep::HostWarp: return "host_warp";
    case TravelStep::HostDie: return "host_die";
    case TravelStep::GuestDie: return "guest_die";
    case TravelStep::HostKill: return "host_kill";
    case TravelStep::HostMove: return "host_move";
    case TravelStep::GuestMove: return "guest_move";
    case TravelStep::HostPress: return "host_press";
    case TravelStep::GuestPress: return "guest_press";
    case TravelStep::HostOutage: return "host_outage";
    case TravelStep::GuestOutage: return "guest_outage";
    case TravelStep::HostMark: return "host_mark";
    case TravelStep::GuestMark: return "guest_mark";
    case TravelStep::HostStage: return "host_stage";
    case TravelStep::HostLastLamp: return "host_lastlamp";
    case TravelStep::HostLamp: return "host_lamp";
    case TravelStep::HostWalk: return "host_walk";
    case TravelStep::HostHeadstone: return "host_headstone";
    case TravelStep::HostFlag: return "host_flag";
    case TravelStep::HostObj: return "host_obj";
    case TravelStep::GuestObj: return "guest_obj";
    case TravelStep::HostChr: return "host_chr";
    case TravelStep::GuestChr: return "guest_chr";
    case TravelStep::GuestLamp: return "guest_lamp";
    case TravelStep::HostAlone: return "host_alone";
    case TravelStep::HostSkipRemo: return "host_skipremo";
    case TravelStep::GuestSkipRemo: return "guest_skipremo";
    }
    return "?";
}

constexpr std::uint64_t kWarpNextStage = 0x132e010; // Lua WarpNextStage(ctx unused, area, block, region, index, point)
constexpr std::uint64_t kWarpNextStageBonfire = 0x132e050; // Lua WarpNextStage_Bonfire(ctx unused, id)
constexpr std::uint32_t kPadDownTest = 0x40;

/// The local chr's map stage (PlayerIns +0x3F8, what the stage-UID check 0x193C537 compares) and position.
std::string DescribeHere(const GameSnapshot& s) {
    std::uint32_t stage = 0;
    float p[4] = {};
    if (s.player) {
        // The physics position (chr +0x58 -> +8 -> +0x3B0 -> +0x68, +0x1E0; +0xC0 lags behind a warp).
        std::uint64_t a = 0;
        SafeGet(s.player + 0x3f8, &stage);
        if (SafeGet(s.player + 0x58, &a) && a && SafeGet(a + 8, &a) && a && SafeGet(a + 0x3b0, &a) && a &&
            SafeGet(a + 0x68, &a) && a) {
            SafeRead(a + 0x1e0, p, 12);
        }
    }
    char b[200];
    std::snprintf(b, sizeof b, "stage %#x pos (%.2f, %.2f, %.2f), %s", stage, p[0], p[1], p[2], Describe(s).c_str());
    return b;
}

/// Puts the local chr at `xyz` / yaw (PlayerIns vfunc +0x5F8 = 0x18CD600, the guest services
/// fixture's call): no load.
bool PutChr(const GameSnapshot& s, const float xyz[3], float yaw) {
    alignas(16) float pos[4] = {xyz[0], xyz[1], xyz[2], 1.0f};
    alignas(16) float rot[4] = {0, yaw, 0, 0};
    std::uint64_t vtbl = 0, fn = 0;
    if (!s.player || !SafeGet(s.player, &vtbl) || !SafeGet(vtbl + 0x5f8, &fn) || !fn) {
        return false;
    }
    using WarpFn = void(BB_COOP_SYSV*)(std::uint64_t, const float*, const float*, std::uint64_t);
    reinterpret_cast<WarpFn>(fn)(s.player, pos, rot, 0);
    return true;
}

/// HostWalk / HostHeadstone, one tick.
void RunTravelScript(State& st, const GameSnapshot& s, Clock::time_point now) {
    Test::Script& sc = st.test.script;
    auto tap = [&](std::uint32_t buttons) {
        runtime_pad_director_press(buttons);
        sc.pressed = true;
        sc.at = now;
    };
    if (sc.pressed) { // short taps: 0.12 s
        if (Seconds(sc.at, now) < 0.12) {
            return;
        }
        runtime_pad_director_press(0);
        sc.pressed = false;
    }
    const double since = Seconds(sc.at, now);
    auto done = [&](const char* how) {
        st.test.script_on = false;
        if (sc.kind == TravelStep::HostWalk && st.test.joined) {
            // No load, so no re-join follows: the next step counts from here.
            st.test.armed = true;
            st.test.joined_at = now;
            st.test.step_at = now;
        }
        Log("test: %s %s (%s)", TravelStepName(sc.kind), how, DescribeHere(s).c_str());
    };
    if (sc.kind == TravelStep::HostWalk) {
        if (!s.world_up || s.loading) {
            done("FAILED: a load started");
            return;
        }
        if (sc.stage > 0 && since < 1.0) {
            return;
        }
        const char* at = sc.args.c_str();
        for (int k = 0; k < sc.stage && at; ++k) {
            at = std::strchr(at, '/');
            at = at ? at + 1 : nullptr;
        }
        if (sc.stage > 0) {
            Log("test: host_walk hop %d settled: %s", sc.stage, DescribeHere(s).c_str());
        }
        float p[3] = {};
        float yaw = 0;
        unsigned near_id = 0;
        if (at && std::sscanf(at, "chr%u", &near_id) == 1) {
            // "chrENTITY": 2 m beside that map character (e.g. Queen Yharnam for the Blood Moon).
            const std::uint64_t chr = GuestSvcFindChr(near_id);
            float d[4] = {};
            if (!chr || !SafeRead(chr + 0xc0, d, sizeof d)) {
                done("FAILED: no character with that entity id");
                return;
            }
            p[0] = d[0] + 2.0f;
            p[1] = d[1] + 0.3f;
            p[2] = d[2];
            Log("test: host_walk: chr %u stands at (%.2f, %.2f, %.2f)", near_id, d[0], d[1], d[2]);
        } else if (!at || std::sscanf(at, "%f:%f:%f:%f", &p[0], &p[1], &p[2], &yaw) != 4) {
            done("done");
            return;
        }
        const bool ok = PutChr(s, p, yaw);
        Log("test: host_walk hop %d to (%.2f, %.2f, %.2f)%s", sc.stage + 1, p[0], p[1], p[2], ok ? "" : " FAILED");
        ++sc.stage;
        sc.at = now;
        return;
    }
    // HostHeadstone: 0 stand in front, 1 Cross (examine), 2 wait for the menu's texts, 3 Downs + Cross,
    // 4 wait for the stage request (Cross again on a confirmation).
    unsigned talk = 0;
    int downs = 0;
    std::sscanf(sc.args.c_str(), "%u:%d", &talk, &downs);
    std::uint64_t seq = 0;
    std::int64_t last_ms = 0;
    BbGameMenu::TextActivity(&seq, &last_ms);
    const std::int64_t now_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    switch (sc.stage) {
    case 0: {
        const std::uint64_t chr = GuestSvcFindChr(talk);
        float d[4] = {}, r[4] = {};
        if (!chr || !SafeRead(chr + 0xc0, d, sizeof d) || !SafeRead(chr + 0xd0, r, sizeof r)) {
            if (since > 6.0) {
                done("FAILED: no character with that talk id");
            }
            return;
        }
        const float yaw = r[1];
        const float p[3] = {d[0] - 1.2f * std::sin(yaw), d[1], d[2] - 1.2f * std::cos(yaw)};
        PutChr(s, p, yaw + 3.14159265f);
        Log("test: host_headstone: t%u at (%.2f, %.2f, %.2f) yaw %.3f; host 1.2 m in front", talk, d[0], d[1], d[2],
            yaw);
        sc.stage = 1;
        sc.at = now;
        return;
    }
    case 1:
        if (since >= 1.5) {
            sc.text_seq = seq;
            tap(kPadCross);
            Log("test: host_headstone: Cross (examine)");
            sc.stage = 2;
        }
        return;
    case 2:
        // The menu builds its texts: fresh lookups, then quiet for 0.6 s.
        if (seq != sc.text_seq && since >= 1.0 && last_ms > 0 && now_ms - last_ms > 600) {
            Log("test: host_headstone: menu texts up (%llu new lookups)",
                static_cast<unsigned long long>(seq - sc.text_seq));
            sc.stage = 3;
            sc.presses = 0;
            sc.at = now;
        } else if (since > 6.0) {
            if (++sc.presses > 3) {
                done("FAILED: no menu after 3 presses");
                return;
            }
            sc.text_seq = seq;
            tap(kPadCross);
            Log("test: host_headstone: no menu yet; Cross again");
        }
        return;
    case 3:
        if (since >= 0.6) {
            if (sc.presses < downs) {
                ++sc.presses;
                tap(kPadDownTest);
                Log("test: host_headstone: Down %d/%d", sc.presses, downs);
            } else {
                tap(kPadCross);
                Log("test: host_headstone: Cross (travel)");
                sc.stage = 4;
                sc.presses = 0;
            }
        }
        return;
    case 4: {
        std::uint64_t wts = 0;
        std::uint8_t req = 0;
        SafeGet(Guest(0x5556678), &wts);
        if (wts) {
            SafeGet(wts + 8, &req);
        }
        if (req || s.loading || !s.world_up) {
            done("travel started");
        } else if (since >= 1.5) {
            if (++sc.presses > 3) {
                done("FAILED: no travel after the menu");
                return;
            }
            tap(kPadCross); // a confirmation dialog
            Log("test: host_headstone: no travel yet; Cross (confirm %d)", sc.presses);
        }
        return;
    }
    default:
        done("?");
    }
}

/// The live travel test chain (TravelStep): joins are tracked in any state; a step runs in the
/// world, with no loading screen and no stage request pending.
void RunTravelSteps(State& st, const GameSnapshot& s, PartyRole role, Clock::time_point now) {
    Test& t = st.test;
    if (t.steps.empty() || role == PartyRole::None) {
        return;
    }
    // Guest: in once a client with the world loaded; out only when the session ends (the summon's
    // own map reload into the host world is still in).
    const bool joined = role == PartyRole::Host ? s.cooperators >= 1
                        : t.joined                ? s.session_role == RoleClient
                                                  : s.session_role == RoleClient && s.world_up && !s.loading;
    if (joined != t.joined) {
        t.joined = joined;
        if (joined) {
            ++t.joins;
            t.joined_at = now;
            t.armed = true;
            if (t.ran) {
                Log("test: travel steps: (re)joined #%d, %.1f s after %s", t.joins, Seconds(t.step_at, now),
                    TravelStepName(t.last_kind));
            } else {
                Log("test: travel steps: joined #%d", t.joins);
            }
        } else {
            Log("test: travel steps: out of the party (%s)", Describe(s).c_str());
        }
    }
    // The other role's steps are that instance's: skip them.
    auto mine = [&](const TravelStep& step) {
        return (step.kind == TravelStep::GuestDie || step.kind == TravelStep::GuestMove ||
                step.kind == TravelStep::GuestPress || step.kind == TravelStep::GuestMark ||
                step.kind == TravelStep::GuestLamp || step.kind == TravelStep::GuestSkipRemo || step.kind == TravelStep::GuestOutage ||
                step.kind == TravelStep::GuestObj || step.kind == TravelStep::GuestChr) == (role == PartyRole::Guest);
    };
    if (t.script_on) {
        RunTravelScript(st, s, now);
        return;
    }
    while (t.next_step < t.steps.size() && !mine(t.steps[t.next_step])) {
        ++t.next_step;
    }
    // Steady world with the party (HostKill's clock): restarts at every load / leave.
    // Split travel: members apart (no cooperator here) still count as the party being together.
    const bool steady = (joined || b2::MembersApart() > 0) && s.world_up && !s.loading;
    if (steady != t.steady) {
        t.steady = steady;
        t.steady_at = now;
    }
    const TravelStep::Kind next_kind = t.next_step < t.steps.size() ? t.steps[t.next_step].kind : TravelStep::HostWarp;
    if (t.pressing && now >= t.release_at) {
        t.pressing = false;
        runtime_pad_director_press(0);
    }
    if (t.next_step < t.steps.size() &&
        (next_kind == TravelStep::HostKill || next_kind == TravelStep::HostMove || next_kind == TravelStep::GuestMove ||
         next_kind == TravelStep::HostPress || next_kind == TravelStep::GuestPress || next_kind == TravelStep::HostWalk ||
         next_kind == TravelStep::HostOutage || next_kind == TravelStep::GuestOutage ||
         next_kind == TravelStep::HostLastLamp || next_kind == TravelStep::HostLamp || next_kind == TravelStep::HostObj ||
         next_kind == TravelStep::GuestObj || next_kind == TravelStep::HostChr || next_kind == TravelStep::GuestChr ||
         next_kind == TravelStep::HostMark || next_kind == TravelStep::GuestMark || !t.steps[t.next_step].map.empty())) {
        if (!steady || (!t.ran && !t.armed) || t.joins < t.steps[t.next_step].min_join) {
            return; // as the first step of a chain it still waits for the join
        }
        // S s after the later of: the last load (steady again), the previous step (or the join).
        const Clock::time_point prev = t.ran ? t.step_at : t.joined_at;
        const Clock::time_point from = t.steady_at > prev ? t.steady_at : prev;
        if (Seconds(from, now) < t.steps[t.next_step].delay) {
            return;
        }
        if (!t.steps[t.next_step].map.empty() && MapName(s.map_id).find(t.steps[t.next_step].map) == std::string::npos) {
            return;
        }
    } else if (t.next_step >= t.steps.size() || !t.armed || !joined || !s.world_up || s.loading ||
               t.joins < t.steps[t.next_step].min_join || Seconds(t.joined_at, now) < t.steps[t.next_step].delay) {
        return;
    }
    std::uint64_t wts = 0;
    std::uint8_t requested = 1;
    if (!SafeGet(Guest(0x5556678), &wts) || !wts || !SafeGet(wts + 8, &requested) || requested) {
        return; // a stage request is pending
    }
    const TravelStep step = t.steps[t.next_step++];
    t.armed = false;
    t.step_at = now;
    t.ran = true;
    t.last_kind = step.kind;
    Log("test: travel step %zu/%zu: %s%s%u (%s)", t.next_step, t.steps.size(), TravelStepName(step.kind),
        step.kind == TravelStep::HostWarp ? " to lamp " : step.kind == TravelStep::HostKill ? " entity " : "",
        step.kind == TravelStep::HostWarp || step.kind == TravelStep::HostKill ? step.id : 0u, Describe(s).c_str());
    if (step.kind == TravelStep::HostWarp) {
        RunLampWarpDecided(step.id, false);  // the party move (no split-travel question)
    } else if (step.kind == TravelStep::HostAlone) {
        b2::HostTravelAloneNext();
        RunLampWarpDecided(step.id, true);
    } else if (step.kind == TravelStep::HostLamp || step.kind == TravelStep::GuestLamp) {
        reinterpret_cast<void(BB_COOP_SYSV*)(std::uint32_t)>(Guest(kLampWarp))(step.id);  // as the player
    } else if (step.kind == TravelStep::HostMove || step.kind == TravelStep::GuestMove) {
        // The local chr to a spot (the chr warp vfunc +0x5F8 = 0x18CD600(chr, pos, rot, 1), as the
        // NPC summon step and the B2 spacing do), keeping its rotation.
        const std::uint64_t player = LocalPlayer();
        std::uint64_t pm = 0, a = 0, b = 0, vt = 0, fn = 0;
        alignas(16) float pos[4] = {step.pos[0], step.pos[1], step.pos[2], 1.0f}, rot[4] = {0, 0, 0, 0};
        if (player && SafeGet(player + 0x58, &a) && a && SafeGet(a + 8, &b) && b && SafeGet(b + 0x3b0, &pm) && pm &&
            SafeGet(pm + 0x68, &pm) && pm && SafeRead(pm + 0x1d0, rot, sizeof rot) && SafeGet(player, &vt) && vt &&
            SafeGet(vt + 0x5f8, &fn) && fn) {
            if (step.has_yaw) {
                rot[0] = 0;
                rot[1] = step.yaw;  // the B2 "dest" event's rot is {0, yaw, 0, 0}
                rot[2] = 0;
                rot[3] = 0;
            }
            reinterpret_cast<void(BB_COOP_SYSV*)(std::uint64_t, const float*, const float*, std::uint32_t)>(fn)(
                player, pos, rot, 1);
            Log("test: %s to (%.2f, %.2f, %.2f) rot y %.3f", TravelStepName(step.kind), double(pos[0]),
                double(pos[1]), double(pos[2]), double(rot[1]));
        } else {
            Log("test: %s: no player / transform", TravelStepName(step.kind));
        }
    } else if (step.kind == TravelStep::HostLamp) {
        // host_lamp=ID@S: the lamp warp 0x13CDF30(ID) on the steady-world clock (host_warp waits for
        // a re-join, which a B2 host never sees): e.g. a Chalice Dungeon lamp's "back to the Dream".
        reinterpret_cast<void(BB_COOP_SYSV*)(std::uint32_t)>(Guest(kLampWarp))(step.id);
    } else if (step.kind == TravelStep::HostLastLamp) {
        // Test fixture: the host's last-lamp record (WorldTransitionState +0x1528 {id, aux}) names
        // a lamp of another map - the state after walking into the next area without resting.
        std::uint64_t w = 0, rec = 0;
        if (SafeGet(Guest(0x5556678), &w) && w && SafeGet(w + 0x1528, &rec)) {
            const std::uint64_t now_rec = (rec & 0xffffffff00000000ull) | step.id;
            SafeWrite(w + 0x1528, &now_rec, 8);
            Log("test: host_lastlamp %#llx -> %#llx", static_cast<unsigned long long>(rec),
                static_cast<unsigned long long>(now_rec));
        }
    } else if (step.kind == TravelStep::HostOutage || step.kind == TravelStep::GuestOutage) {
        bbnet::netsim::outage_now(double(step.buttons));
        Log("test: %s %u ms (%s)", TravelStepName(step.kind), step.buttons, Describe(s).c_str());
    } else if (step.kind == TravelStep::HostPress || step.kind == TravelStep::GuestPress) {
        // In-process pad input (runtime_pad_director_press, OR-ed into every pad sample; no OS
        // input): a tap, released 0.15 s later.
        runtime_pad_director_press(step.buttons);
        t.pressing = true;
        t.release_at = now + std::chrono::milliseconds(150);
        Log("test: %s 0x%x", TravelStepName(step.kind), step.buttons);
    } else if (step.kind == TravelStep::HostMark || step.kind == TravelStep::GuestMark) {
        if (step.args == "lua") {
            std::int64_t r = -1;
            const bool ok = LuaEventRaiseNow("OnReviveMagic", &r);
            Log("test: Lua event OnReviveMagic %s (dispatcher %lld)", ok ? "raised" : "NOT raised",
                static_cast<long long>(r));
        } else {
            // The Hunter's Mark item (EquipParamGoods 100) is SpEffect 2100 (stateInfo 16): applied
            // to the local player as the item use does; the game takes it from there.
            const int effect = step.args.empty() ? 2100 : std::atoi(step.args.c_str());
            const bool ok = ApplySpEffect(effect);
            Log("test: Hunter's Mark: SpEffect %d %s (%s)", effect, ok ? "applied" : "NOT applied",
                DescribeHere(s).c_str());
        }
    } else if (step.kind == TravelStep::HostObj || step.kind == TravelStep::GuestObj ||
               step.kind == TravelStep::HostChr || step.kind == TravelStep::GuestChr) {
        int entity = 0;
        float dist = 1.2f, angle = 0;
        std::sscanf(step.args.c_str(), "%d:%f:%f", &entity, &dist, &angle);
        std::string what;
        const bool obj = step.kind == TravelStep::HostObj || step.kind == TravelStep::GuestObj;
        const bool ok = obj ? worldact::StandAtObject(entity, dist, angle, &what)
                            : worldact::StandAtChr(entity, dist, angle, &what);
        Log("test: %s %d (%.1f m, %.0f deg): %s", TravelStepName(step.kind), entity, double(dist), double(angle),
            ok ? what.c_str() : "FAILED: not loaded");
    } else if (step.kind == TravelStep::HostSkipRemo || step.kind == TravelStep::GuestSkipRemo) {
        // The game's own cutscene skip request (what the skip input does; in a session it is this
        // player's skip vote).
        bool skippable = false;
        const bool ok = RequestRemoSkip(&skippable);
        Log("test: %s: skip request %s (skippable bit %d)", TravelStepName(step.kind), ok ? "sent" : "NOT possible",
            skippable ? 1 : 0);
    } else if (step.kind == TravelStep::HostFlag) {
        // A story choice the game makes in a talk menu (e.g. 72100131 = refuse Gehrman): SetEventFlag.
        const std::uint32_t id = static_cast<std::uint32_t>(std::strtoul(step.args.c_str(), nullptr, 10));
        bool before = false, after = false;
        ReadEventFlag(id, &before);
        const bool ok = WriteEventFlag(id, true);
        ReadEventFlag(id, &after);
        Log("test: host_flag %u: %d -> %d (%s)", id, int(before), int(after), ok ? "written" : "NOT written");
    } else if (step.kind == TravelStep::HostStage) {
        unsigned a = 0, b = 0, r = 0, i = 0;
        int wp = -1;
        if (step.args.rfind("bonfire:", 0) == 0) {
            const std::uint32_t id = static_cast<std::uint32_t>(std::strtoul(step.args.c_str() + 8, nullptr, 10));
            Log("test: WarpNextStage_Bonfire(%u) %s", id, DescribeHere(s).c_str());
            reinterpret_cast<void(BB_COOP_SYSV*)(std::uint64_t, std::uint32_t)>(Guest(kWarpNextStageBonfire))(0, id);
        } else if (std::sscanf(step.args.c_str(), "%u:%u:%u:%u:%d", &a, &b, &r, &i, &wp) >= 4) {
            Log("test: WarpNextStage(m%02u_%02u_%02u_%02u, point %d) %s", a, b, r, i, wp, DescribeHere(s).c_str());
            reinterpret_cast<std::uint64_t(BB_COOP_SYSV*)(std::uint64_t, std::uint32_t, std::uint32_t, std::uint32_t,
                                                          std::uint32_t, std::uint32_t)>(Guest(kWarpNextStage))(
                0, a, b, r, i, static_cast<std::uint32_t>(wp));
        } else {
            Log("test: host_stage '%s' is not area:block:region:index[:point] or bonfire:ID", step.args.c_str());
        }
    } else if (step.kind == TravelStep::HostWalk || step.kind == TravelStep::HostHeadstone) {
        t.script = Test::Script{};
        t.script.kind = step.kind;
        t.script.args = step.args;
        t.script.at = now;
        t.script_on = true;
        Log("test: %s %s from %s", TravelStepName(step.kind), step.args.c_str(), DescribeHere(s).c_str());
        return;
    } else if (step.kind == TravelStep::HostKill) {
        // A boss (or any enemy) killed as the game's own death path: ForceDead on its entity id; the
        // boss-defeat EMEVD (4[00] death state -> 2003[12]) and the Lua boss clear follow.
        {
            const std::uint64_t chr = GuestSvcFindChr(step.id);
            float d[4] = {};
            if (chr) {
                SafeRead(chr + 0xc0, d, sizeof d);
            }
            Log("test: host_kill: entity %u %s (%.1f, %.1f, %.1f)", step.id, chr ? "loaded at" : "NOT loaded here", d[0], d[1],
                d[2]);
        }
        reinterpret_cast<void(BB_COOP_SYSV*)(std::uint64_t, std::uint32_t)>(Guest(kForceDead))(0, step.id);
    } else {
        reinterpret_cast<void(BB_COOP_SYSV*)(std::uint64_t, std::uint32_t)>(Guest(kForceDead))(0, 10000);
    }
    std::uint8_t after = 0;
    SafeGet(wts + 8, &after);
    Log("test: travel step %s done; stage request %s", TravelStepName(step.kind), after ? "armed" : "not armed");
}

/// The title takes scripted input (BB_TEST_NEWGAME): no world; in party mode the link is up and
/// the FROM sign-in (ss.info -> login -> notice) is over: the title menu shows after it (before,
/// "Creating save data" / "Checking network status" swallow presses).
/// The player's area and position (FrpgNetMan +0xa7c / +0xaac); false when not readable.
bool ReadPlayerPos(std::uint32_t* area, float pos[3]) {
    std::uint64_t man = 0;
    const std::uint64_t at = Guest(0x553b120);
    return at && SafeGet(at, &man) && man && SafeGet(man + 0xa7c, area) && SafeRead(man + 0xaac, pos, 3 * sizeof(float));
}

/// Test fixtures: the left stick through the BB_PAD_FILE pad script (src/runtime_pad.c re-reads
/// it when it changes); "" releases it.
void WritePadFile(const char* tokens) {
    const char* path = std::getenv("BB_PAD_FILE");
    if (!path || !path[0]) {
        return;
    }
    if (FILE* f = std::fopen(path, "w")) {
        std::fputs(tokens, f);
        std::fclose(f);
    }
}

bool TitleReadyForInput(PartyRole role, party::PartyLink* link) {
    if (role == PartyRole::None) {
        return true;
    }
    const party::LinkState ls = link ? link->state() : party::LinkState::Idle;
    const bool link_ok = role == PartyRole::Host ? ls == party::LinkState::Hosting : ls == party::LinkState::Connected;
    if (!link_ok) {
        return false;
    }
    const bbnet::OnlineProgress op = bbnet::online_progress();
    return op.notice > 0 && op.since_last >= 2.0;
}

// ---- Host session watchdog (party_watchdog.h) ----
// 0x13CDDA0: BeginLeaveSession(SprjSessionManager) 0x1ED07A0, ReloadPhase+0xf0 = chr type on
// success, then a tail jump to 0x15BED10(network flow +0x16F8) (drops the presentation members).
constexpr std::uint64_t kLeaveSessionAndDrop = 0x13cdda0;
using VoidFn = void(BB_COOP_SYSV*)();

/// SprjSessionManager peer count (+0xF8..+0x100, stride 0x20), presentation count (network flow
/// +0x16F8 -> +0x18) and the WorldChrMan phantoms (chr set 1 count, +0x850 table): for the logs.
std::string DescribeSessionMembers() {
    std::uint64_t sm = 0, flow = 0, pres = 0, b = 0, e = 0;
    SafeGet(Guest(0x5540290), &sm);
    SafeGet(Guest(0x5556678), &flow);
    if (sm) {
        SafeGet(sm + 0xf8, &b);
        SafeGet(sm + 0x100, &e);
    }
    std::int32_t pn = -1;
    if (flow && SafeGet(flow + 0x16f8, &pres) && pres) {
        SafeGet(pres + 0x18, &pn);
    }
    std::int32_t slots[4] = {-1, -1, -1, -1};
    if (pres) {
        SafeRead(pres + 0x80, slots, sizeof slots);
    }
    char out[200];
    std::snprintf(out, sizeof out, "session peers %lld, presentation count %d (handles %x %x %x %x)",
                  (b && e >= b) ? static_cast<long long>((e - b) / 0x20) : -1LL, pn, unsigned(slots[0]),
                  unsigned(slots[1]), unsigned(slots[2]), unsigned(slots[3]));
    return out;
}

} // namespace

namespace {
constexpr std::uint64_t kOnLeaveLimit = 0x138a4a0;
bool OnLeaveLimitOk() {
    static const bool ok = Matches(kOnLeaveLimit, {0x55, 0x48, 0x89, 0xe5}) && CallsTo(kOnLeaveLimit + 0x77, 0x1332bc0);
    return ok;
}
/// The game's own send-home of a phantom (OnLeave_Limit), let through party_travel's retarget.
void RunOnLeaveLimit() {
    AllowNextSendHome();
    using LimitFn = void(BB_COOP_SYSV*)(std::uint64_t);
    reinterpret_cast<LimitFn>(Guest(kOnLeaveLimit))(0);
}
std::atomic<bool> g_send_home_request{false};
std::string g_send_home_why;  // under g_send_home_mu
std::mutex g_send_home_mu;
} // namespace

void RequestGuestSendHome(const std::string& why) {
    {
        std::lock_guard<std::mutex> lk(g_send_home_mu);
        g_send_home_why = why;
    }
    g_send_home_request = true;
}

bool LeaveGameSession() {
    // A guest (a phantom in the host's world): the game's own voluntary leave - OnLeave_Limit
    // (0x138A4A0, what OnLeaveMenu_Yes 0x1389FB0 runs for a phantom after the session leave): the
    // pre-summon spot, then the funnel (BeginLeave + the load home) at once. The session leave
    // alone left the game to notice a dead room 10 s later ("Session lost" 0xcc/5001, then home).
    const GameSnapshot before = ReadGameState(false);
    if (before.session_role == RoleClient && OnLeaveLimitOk()) {
        RunOnLeaveLimit();
        const GameSnapshot after = ReadGameState(false);
        Log("leave: OnLeave_Limit (the guest's own leave): role %s (%d)", SessionRoleName(after.session_role),
            after.session_role);
        return true;
    }
    static const bool ok =
        Matches(kLeaveSessionAndDrop, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x50, 0x48, 0x8b, 0x05,
                                       0xc7, 0x88, 0x18, 0x04, 0x48, 0x8b, 0x98, 0xf8, 0x16, 0x00, 0x00}) &&
        CallsTo(kLeaveSessionAndDrop + 0x55, 0x1ed07a0);
    if (!ok) return false;
    reinterpret_cast<VoidFn>(Guest(kLeaveSessionAndDrop))();
    const GameSnapshot after = ReadGameState(false);
    Log("leave: the game's session leave: role %s (%d); %s", SessionRoleName(after.session_role), after.session_role,
        DescribeSessionMembers().c_str());
    return true;
}

namespace {

void HostWatchdogTick(State& st, const GameSnapshot& s, party::PartyLink* link, Clock::time_point now) {
    HostWatchdogInput in;
    in.now_s = Seconds(st.start, now);
    in.in_world = InWorld(s) && st.world_seen;
    in.session_role = s.session_role;
    in.cooperators = s.cooperators;
    in.members_apart = b2::MembersApart();
    if (link) {
        in.roster = link->roster();
    }
    if (st.watchdog_check && s.session_role == RoleIdle && Seconds(st.watchdog_at, now) >= 2.0) {
        st.watchdog_check = false;
        Log("host watchdog: session idle %.1f s after the stop; %s; cooperators %d", Seconds(st.watchdog_at, now),
            DescribeSessionMembers().c_str(), s.cooperators);
    }
    if (!st.watchdog.Update(in)) {
        return;
    }
    static int code_ok = 0;
    if (!code_ok) {
        code_ok = Matches(kLeaveSessionAndDrop, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x50, 0x48, 0x8b,
                                                 0x05, 0xc7, 0x88, 0x18, 0x04, 0x48, 0x8b, 0x98, 0xf8, 0x16, 0x00, 0x00})
                      ? 1
                      : -1;
        if (code_ok < 0) {
            Log("host watchdog: 0x13CDDA0 is not the 1.09 leave helper; the watchdog cannot stop sessions");
        }
    }
    ++st.watchdog_stops;
    Log("host watchdog: stop #%d (t %.1f s): %s; %s; roster: %s", st.watchdog_stops, in.now_s, st.watchdog.why().c_str(),
        DescribeSessionMembers().c_str(), [&] {
            std::string r;
            for (const party::RosterEntry& e : in.roster) {
                char b[96];
                std::snprintf(b, sizeof b, "%s%s(slot %d, %s%s)", r.empty() ? "" : ", ", e.name.c_str(), e.slot,
                              party::member_state_name(e.state), e.connected ? "" : ", lost");
                r += b;
            }
            return r;
        }().c_str());
    if (code_ok < 0 || !CallsTo(kLeaveSessionAndDrop + 0x55, 0x1ed07a0)) {
        Log("host watchdog: no stop (code check failed)");
        return;
    }
    reinterpret_cast<VoidFn>(Guest(kLeaveSessionAndDrop))();
    const GameSnapshot after = ReadGameState(false);
    Log("host watchdog: left the session: role %s (%d); %s", SessionRoleName(after.session_role), after.session_role,
        DescribeSessionMembers().c_str());
    party::status::SetLastEvent("Summon stuck: session restarted");
    st.watchdog_check = true;
    st.watchdog_at = now;
    st.rung_ever = false; // ring again as soon as the session is idle and a member waits
}

void RunTest(State& st, const GameSnapshot& s, Clock::time_point now) {
    Test& t = st.test;
    if (!st.world_seen) {
        return;
    }
    // bell_host / bell_guest: the bell through the item path (SpEffect -> SendSign -> sign
    // create / search on the FROM server) once this player is ready, bells granted, 5 s idle.
    // bell_host@AA: the host's own map in area AA; bell_guest@AA: the host's (roster) map.
    std::uint32_t bell_map = s.map_id;
    if (t.bell_area >= 0 && t.bell && std::strcmp(t.bell, kGuestBell) == 0 && st.link) {
        bell_map = 0xffffffffu;
        for (const party::RosterEntry& e : st.link->roster()) {
            if (e.slot == party::kHostSlot) bell_map = e.map_id;
        }
    }
    if (t.bell && !t.bell_done && st.start_ready && st.granted && s.world_up && !s.loading &&
        (t.bell_area < 0 || (bell_map != 0xffffffffu && bell_map != 0 && int(bell_map >> 24) == t.bell_area)) &&
        s.session_role == RoleIdle && Seconds(st.world_at, now) >= t.delay) {
        t.bell_done = true;
        Log("test: %s once (ready: %s)", t.bell, DescribeStart(st.cs, true).c_str());
        Ring(st, t.bell, "test bell", now);
    }
    if (t.insight >= 0 && !t.insight_done && Seconds(st.world_at, now) >= 2.0 && !s.loading) {
        t.insight_done = true;
        const int before = s.insight;
        const bool ok = WritePlayerInsight(t.insight);
        Log("test: Insight %d -> %d %s", before, t.insight, ok ? "written" : "NOT written (no player record)");
    }
    if (t.log_pos && s.world_up && !s.loading && Seconds(t.pos_at, now) >= 1.0) {
        // Every second: the physics position (as DescribeHere; the map man's +0xAAC is the last
        // grounded / area position and goes stale after a re-created PlayerIns) and the area id.
        t.pos_at = now;
        std::uint64_t man = 0;
        std::uint32_t area = 0;
        const std::uint64_t at = Guest(0x553b120);
        if (at && SafeGet(at, &man) && man) {
            SafeGet(man + 0xa7c, &area);
        }
        float p[3] = {};
        std::uint64_t a = 0;
        if (s.player && SafeGet(s.player + 0x58, &a) && a && SafeGet(a + 8, &a) && a && SafeGet(a + 0x3b0, &a) && a &&
            SafeGet(a + 0x68, &a) && a) {
            SafeRead(a + 0x1e0, p, 12);
        }
        Log("test: pos area 0x%08x (%.2f, %.2f, %.2f)", area, p[0], p[1], p[2]);
    }
    if (t.echoes >= 0 && Seconds(st.world_at, now) >= 2.0 && !s.loading) {
        const bool ok = WritePlayerEchoes(t.echoes);
        Log("test: Blood Echoes -> %d %s", t.echoes, ok ? "written" : "NOT written (no player record)");
        t.echoes = -1;
    }
    if (!t.goods.empty() && !t.goods_done && Seconds(st.world_at, now) >= 8.0 && s.world_up && !s.loading) {
        t.goods_done = true;
        for (const auto& [id, n] : t.goods) {
            const int before = GoodsCount(std::uint32_t(id));
            const bool ok = before >= 0 && before < n ? GiveGoods(std::uint32_t(id), n - before) : before >= n;
            Log("test: goods %d x%d -> x%d (%s)", id, before, GoodsCount(std::uint32_t(id)), ok ? "ok" : "failed");
        }
    }
    // With place= the stand waits for the placed map (the Ritual Altar is in m21_01, not the Dream
    // block m21_00 a save starts in: a stand there kept the player where it was).
    const bool stand_map_ok =
        !t.place || (t.place_done && (s.map_id & 0xffff0000u) == (t.place_map & 0xffff0000u));
    if (t.stand && !t.stand_done && stand_map_ok && s.world_up && !s.loading && s.player &&
        Seconds(st.world_at, now) >= 10.0 && Seconds(t.st_at, now) >= 0.0) {
        if (t.st_phase == 0) {
            alignas(16) float pos[4] = {t.st_pos[0], t.st_pos[1], t.st_pos[2], 1.0f};
            alignas(16) float rot[4] = {0, t.st_yaw, 0, 0};
            std::uint64_t vtbl = 0, fn = 0;
            if (SafeGet(s.player, &vtbl) && SafeGet(vtbl + 0x5f8, &fn) && fn) {
                using WarpFn = void(BB_COOP_SYSV*)(std::uint64_t, const float*, const float*, std::uint64_t);
                reinterpret_cast<WarpFn>(fn)(s.player, pos, rot, 0);
                float at[3] = {};
                SafeRead(s.player + 0xc0, at, sizeof at);
                Log("test: stand at (%.2f, %.2f, %.2f) yaw %.3f, in place (now at %.2f, %.2f, %.2f)", pos[0], pos[1],
                    pos[2], rot[1], at[0], at[1], at[2]);
            } else {
                Log("test: stand: no PlayerIns warp");
            }
            t.st_phase = 1;
            t.st_at = now + std::chrono::milliseconds(1500);
        } else if (t.st_phase == 1) {
            float at[3] = {}, r[4] = {};
            SafeRead(s.player + 0xc0, at, sizeof at);
            SafeRead(s.player + 0xd0, r, sizeof r);
            if (t.st_button) {
                runtime_pad_director_press(t.st_button);
            }
            Log("test: stand: at (%.2f, %.2f, %.2f) yaw %.3f; pressed 0x%x", at[0], at[1], at[2], r[1], t.st_button);
            t.st_phase = 2;
            t.st_at = now + std::chrono::milliseconds(250);
        } else if (t.st_phase == 2) {
            runtime_pad_director_press(0);
            t.st_phase = 3;
            t.st_at = now + std::chrono::milliseconds(600);
        } else if (t.st_phase == 3) {
            // The menu walk, event driven (MenuScript: '+' waits for the next screen's texts).
            std::uint64_t seq = 0;
            std::int64_t last_ms = 0;
            BbGameMenu::TextActivity(&seq, &last_ms);
            const std::int64_t now_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
            std::string line;
            const double secs = std::chrono::duration<double>(now.time_since_epoch()).count();
            const std::uint32_t held = t.st_seq.Step(
                secs, seq, seq ? double(now_ms - last_ms) / 1000.0 : 1e9, &line, [](const std::string& text) {
                    const std::int64_t ms = BbGameMenu::TextSeenMs(text.c_str());
                    return ms > 0 ? double(ms) / 1000.0 : -1.0;
                });
            runtime_pad_director_press(held);
            // The raw pad state the game read while a press is held (once per press).
            static std::uint32_t logged_held = 0;
            if (held && held != logged_held) {
                const std::uint32_t read = runtime_pad_last_buttons();
                if (read & held) {
                    Log("test: seq: the game read pad buttons 0x%x (held 0x%x)", read, held);
                    logged_held = held;
                }
            } else if (!held) {
                logged_held = 0;
            }
            if (!line.empty()) {
                Log("test: seq %s (texts seq %llu)", line.c_str(), static_cast<unsigned long long>(seq));
            }
            // Every second: the texts on screen now (first 60 ASCII chars each), to learn the real ones.
            static std::int64_t texts_logged = 0;
            if (now_ms - texts_logged >= 1000) {
                std::string all;
                BbGameMenu::RecentTexts(now_ms - 600, [](const char16_t* text, std::int64_t, void* user) {
                    std::string& out = *static_cast<std::string*>(user);
                    std::string a;
                    for (const char16_t* c = text; *c && a.size() < 60; ++c) a += *c < 128 && *c >= 32 ? char(*c) : '.';
                    if (out.find(a) == std::string::npos && out.size() < 1500) out += " | " + a;
                }, &all);
                if (all != t.st_last_texts) {
                    t.st_last_texts = all;
                    Log("test: texts now:%s", all.c_str());
                }
                texts_logged = now_ms;
            }
            if (t.st_seq.finished()) {
                runtime_pad_director_press(0);
                t.stand_done = true;
                Log("test: seq %s", t.st_seq.stuck() ? "STUCK" : "done");
            }
        }
    }
    if (t.approach && !t.approach_done && s.world_up && !s.loading && Seconds(st.world_at, now) >= 12.0 &&
        (!t.place || t.place_done) && Seconds(t.ap_at, now) >= 0.0) {
        std::uint32_t area = 0;
        float pos[3] = {0, 0, 0};
        if (ReadPlayerPos(&area, pos)) {
            const double dx = t.ap_x - pos[0], dz = t.ap_z - pos[2];
            const double dist = std::sqrt(dx * dx + dz * dz);
            const double want = std::atan2(dx, dz);
            auto wrap = [](double a) { while (a > M_PI) a -= 2 * M_PI; while (a < -M_PI) a += 2 * M_PI; return a; };
            if (t.ap_phase == 1) { // a step ends: release, let it settle
                WritePadFile("");
                t.ap_phase = 2;
                t.ap_at = now + std::chrono::milliseconds(300);
            } else if (t.ap_phase == 0 || t.ap_phase == 2) {
                if (t.ap_phase == 2) {
                    const double mx = pos[0] - t.ap_from[0], mz = pos[2] - t.ap_from[1];
                    if (std::sqrt(mx * mx + mz * mz) > 0.05) {
                        // Camera-relative stick: correct the stick angle by how far the walk missed.
                        t.ap_angle = wrap(t.ap_angle + wrap(want - std::atan2(mx, mz)));
                    }
                }
                Log("test: approach: (%.2f, %.2f) -> (%.2f, %.2f): %.2f m, stick %.0f deg (step %d)", pos[0], pos[2],
                    t.ap_x, t.ap_z, dist, t.ap_angle * 180 / M_PI, t.ap_steps);
                if (dist <= t.ap_r || t.ap_steps >= 40) {
                    t.ap_phase = 3;
                    t.ap_at = now + std::chrono::milliseconds(1200);
                    Log("test: approach: %s at %.2f m", dist <= t.ap_r ? "arrived" : "GAVE UP", dist);
                } else {
                    ++t.ap_steps;
                    t.ap_from[0] = pos[0];
                    t.ap_from[1] = pos[2];
                    const int lx = 128 + static_cast<int>(std::lround(127 * std::sin(t.ap_angle)));
                    const int ly = 128 - static_cast<int>(std::lround(127 * std::cos(t.ap_angle)));
                    char tok[48];
                    std::snprintf(tok, sizeof tok, "lx=%d ly=%d", std::clamp(lx, 0, 255), std::clamp(ly, 0, 255));
                    WritePadFile(tok);
                    t.ap_phase = 1;
                    const double secs = std::clamp(dist / 3.0, 0.12, 0.45);
                    t.ap_at = now + std::chrono::milliseconds(static_cast<int>(secs * 1000));
                }
            } else if (t.ap_phase == 3) { // arrived: the button
                if (t.ap_button) {
                    runtime_pad_director_press(t.ap_button);
                    Log("test: approach: pressed 0x%x (map 0x%08x)", t.ap_button, area);
                    t.ap_phase = 4;
                    t.ap_at = now + std::chrono::milliseconds(250);
                } else {
                    t.approach_done = true;
                }
            } else if (t.ap_phase == 4) {
                runtime_pad_director_press(0);
                t.approach_done = true;
            }
        }
    }
    if (t.place && !t.place_done && Seconds(st.world_at, now) >= t.place_at && s.world_up && !s.loading) {
        t.place_done = true;
        const bool ok = t.warp_point ? TravelWarpForTest(t.place_map, t.warp_point)
                                     : TravelPlaceForTest(t.place_map, t.place_pos, t.place_yaw);
        Log("test: place: %s", ok ? "requested" : "refused (unknown map)");
    }
    // Host of a party: award_lot / set_flag wait until a cooperator has been in the world for the
    // test delay (the live-sync checks need a summoned guest); without a party, 12 s / delay.
    bool party_host;
    {
        std::lock_guard<std::mutex> lk(st.mu);
        party_host = st.role == PartyRole::Host;
    }
    if (party_host && s.cooperators > 0 && !t.coop_seen) {
        t.coop_seen = true;
        t.coop_at = now;
        Log("test: a cooperator is in the world; party test actions in %.0f s", t.delay);
    }
    const bool party_ready = !party_host || (t.coop_seen && Seconds(t.coop_at, now) >= t.delay);
    if (!t.set_flags.empty() && party_ready && Seconds(st.world_at, now) >= t.delay && s.world_up && !s.loading) {
        for (const std::uint32_t id : t.set_flags) {
            bool before = false, after = false;
            ReadEventFlag(id, &before);
            const bool ok = WriteEventFlag(id, true);
            ReadEventFlag(id, &after);
            Log("test: set_flag %u: %d -> %d (%s)", id, int(before), int(after), ok ? "written" : "NOT written");
        }
        t.set_flags.clear();
    }
    if (t.award_lot > 0 && party_ready && Seconds(st.world_at, now) >= 12.0 && s.world_up && !s.loading) {
        const int before = GoodsCount(kGoodsBeckoning);
        const bool ok = AwardItemLot(std::uint32_t(t.award_lot));
        Log("test: award_lot %d: %s; goods 200 x%d -> x%d", t.award_lot, ok ? "awarded" : "NOT awarded", before,
            GoodsCount(kGoodsBeckoning));
        t.award_lot = -1;
    }
    // PR1 probe: a character without the bells (removes goods 200 / 205 through GiveItemDirect
    // with a negative count, 10 s after the world is up). Test saves only.
    if (t.drop_bells && !t.drop_done && Seconds(st.world_at, now) >= 10.0 && s.world_up && !s.loading) {
        t.drop_done = true;
        const int b = GoodsCount(kGoodsBeckoning), r = GoodsCount(kGoodsSmallResonant);
        const bool ok_b = b > 0 ? GiveGoods(kGoodsBeckoning, -b) : true;
        const bool ok_r = r > 0 ? GiveGoods(kGoodsSmallResonant, -r) : true;
        Log("test: drop_bells: goods 200 x%d -> x%d (%s), 205 x%d -> x%d (%s)", b, GoodsCount(kGoodsBeckoning),
            ok_b ? "ok" : "failed", r, GoodsCount(kGoodsSmallResonant), ok_r ? "ok" : "failed");
    }
    if (t.ring && !t.ring_done && Seconds(st.world_at, now) >= t.delay && s.world_up && !s.loading) {
        t.ring_done = true;
        t.rung_at = now;
        Log("test: before %s: %s", t.ring, Describe(s).c_str());
        Log("test: before %s: start %s", t.ring, DescribeStart(ReadStartFlags(), true).c_str());
        std::int64_t r = 0;
        const bool ok = LuaEventRaiseNow(t.ring, &r);
        Log("test: %s raised: %s, dispatcher returned %lld (1: a handler ran)", t.ring, ok ? "yes" : "no (no event manager)",
            static_cast<long long>(r));
        // A name no handler table has, for comparison (the dispatcher answers 0).
        std::int64_t none = -1;
        LuaEventRaiseNow("OnEvent_BbportNoSuchEvent", &none);
        Log("test: control event OnEvent_BbportNoSuchEvent: dispatcher returned %lld", static_cast<long long>(none));
        const GameSnapshot after = ReadGameState(true);
        Log("test: right after %s: %s", t.ring, Describe(after).c_str());
    }
}

} // namespace

PartyDirector& PartyDirector::Get() {
    static PartyDirector d;
    return d;
}

void PartyDirector::Configure(PartyRole role, int max_players) {
    State& st = S();
    std::lock_guard<std::mutex> lk(st.mu);
    st.role = role;
    st.max_players = max_players < 2 ? 2 : max_players > party::kMaxPartyPlayers ? party::kMaxPartyPlayers : max_players;
}

void PartyDirector::ConfigureFromEnv() {
    State& st = S();
    PartyRole role = PartyRole::None;
    if (const char* p = bb_party_value()) {
        std::string lower(p);
        for (char& c : lower) {
            c = char(std::tolower(static_cast<unsigned char>(c)));
        }
        role = lower == "host" ? PartyRole::Host : PartyRole::Guest;
        if (const char* h = std::getenv("BB_PARTY_HOST"); h && h[0]) {
            role = h[0] == '1' ? PartyRole::Host : PartyRole::Guest;
        }
    }
    int max_players = 3;
    if (const char* m = std::getenv("BB_PARTY_MAX"); m && m[0]) {
        max_players = std::atoi(m);
    }
    Configure(role, max_players);
    progress::SetRole(role == PartyRole::Host    ? progress::Role::Host  // C2 progress sync
                      : role == PartyRole::Guest ? progress::Role::Guest
                                                 : progress::Role::None);
    std::lock_guard<std::mutex> lk(st.mu);
    if (const char* e = std::getenv("BB_PARTY_RING_EVERY"); e && e[0] && std::atof(e) >= 1.0) {
        st.ring_every = std::atof(e);
    }
    if (!bb_party_switch_on("BB_PARTY_FAST_JOIN")) {
        st.fast_join = false;
        st.ring_delay = 5.0;
        st.continue_settle = 4.0;
        st.continue_every = 2.5;
    }
    if (const char* d = std::getenv("BB_PARTY_RING_DELAY"); d && d[0] && std::atof(d) >= 0.0) {
        st.ring_delay = std::atof(d);
    }
    if (const char* a = std::getenv("BB_PARTY_AUTO"); a && a[0] == '0') {
        st.auto_ring = false;
    }
    if (const char* c = std::getenv("BB_PARTY_AUTOCONTINUE"); c && c[0]) {
        st.auto_continue = c[0] == '0' ? 0 : 1;
    }
    if (const char* t = std::getenv("BB_PARTY_DIRECTOR_TEST"); t && t[0]) {
        st.test.on = true;
        std::string list = t;
        std::size_t at = 0;
        while (at <= list.size()) {
            std::size_t end = list.find(',', at);
            if (end == std::string::npos) {
                end = list.size();
            }
            const std::string item = list.substr(at, end - at);
            at = end + 1;
            if (item == "log_state") {
                st.test.log_state = true;
            } else if (item == "start") {
                st.test.start = true;
            } else if (item.rfind("award_lot=", 0) == 0) {
                st.test.award_lot = std::atoi(item.c_str() + 10);
            } else if (item.rfind("goods=", 0) == 0) {
                int id = 0, n = 0;
                if (std::sscanf(item.c_str() + 6, "%d:%d", &id, &n) == 2 && id > 0 && n > 0 && n <= 99) {
                    st.test.goods.emplace_back(id, n);
                }
            } else if (item.rfind("warp=", 0) == 0) {
                unsigned map = 0, point = 0;
                if (std::sscanf(item.c_str() + 5, "%x:%u", &map, &point) == 2) {
                    st.test.place = true;
                    st.test.place_map = map;
                    st.test.warp_point = point;
                }
            } else if (item.rfind("place=", 0) == 0) {
                unsigned map = 0;
                float x = 0, y = 0, z = 0, yaw = 0;
                if (std::sscanf(item.c_str() + 6, "%x:%f:%f:%f:%f", &map, &x, &y, &z, &yaw) >= 4) {
                    st.test.place = true;
                    st.test.place_map = map;
                    st.test.place_pos[0] = x;
                    st.test.place_pos[1] = y;
                    st.test.place_pos[2] = z;
                    st.test.place_yaw = yaw;
                }
            } else if (item.rfind("set_flag=", 0) == 0) {
                for (const char* p = item.c_str() + 9; *p;) {
                    char* end = nullptr;
                    const unsigned long id = std::strtoul(p, &end, 10);
                    if (end == p) {
                        break;
                    }
                    st.test.set_flags.push_back(static_cast<std::uint32_t>(id));
                    p = *end ? end + 1 : end;
                }
            } else if (item == "drop_bells") {
                st.test.drop_bells = true;
            } else if (item == "grant_bells") {
                st.test.grant_bells = true;
            } else if (item == "ring_host") {
                st.test.ring = kHostBell;
            } else if (item == "ring_guest") {
                st.test.ring = kGuestBell;
            } else if (item.rfind("stand=", 0) == 0) {
                float x = 0, y = 0, z = 0, yaw = 0;
                char button[16] = {0};
                const int n = std::sscanf(item.c_str() + 6, "%f:%f:%f:%f:%15s", &x, &y, &z, &yaw, button);
                if (n >= 4) {
                    st.test.stand = true;
                    st.test.st_pos[0] = x;
                    st.test.st_pos[1] = y;
                    st.test.st_pos[2] = z;
                    st.test.st_yaw = yaw;
                    const std::string b = button;
                    st.test.st_button = b == "cross" ? 0x4000u : b == "triangle" ? 0x1000u : b == "square" ? 0x8000u
                                      : b == "circle" ? 0x2000u : 0u;
                }
            } else if (item.rfind("seq=", 0) == 0) {
                std::string bad;
                if (!st.test.st_seq.Parse(item.substr(4), &bad)) {
                    Log("BB_PARTY_DIRECTOR_TEST: seq token '%s' unknown; seq ignored", bad.c_str());
                    st.test.st_seq = MenuScript{};
                }
            } else if (item.rfind("approach=", 0) == 0) {
                float x = 0, z = 0, r = 1.0f;
                char button[16] = {0};
                const int n = std::sscanf(item.c_str() + 9, "%f:%f:%f:%15s", &x, &z, &r, button);
                if (n >= 2) {
                    st.test.approach = true;
                    st.test.ap_x = x;
                    st.test.ap_z = z;
                    st.test.ap_r = n >= 3 ? r : 1.0f;
                    const std::string b = button;
                    st.test.ap_button = b == "cross" ? 0x4000u : b == "triangle" ? 0x1000u : b == "square" ? 0x8000u
                                      : b == "circle" ? 0x2000u : 0u;
                }
            } else if (item == "log_pos") {
                st.test.log_pos = true;
            } else if (item.rfind("echoes=", 0) == 0) {
                st.test.echoes = std::atoi(item.c_str() + 7);
            } else if (item.rfind("bell_guest@", 0) == 0) {
                st.test.bell = kGuestBell;
                st.test.bell_area = std::atoi(item.c_str() + 11);
            } else if (item.rfind("bell_host@", 0) == 0) {
                st.test.bell = kHostBell;
                st.test.bell_area = std::atoi(item.c_str() + 10);
            } else if (item == "bell_host") {
                st.test.bell = kHostBell;
            } else if (item == "bell_guest") {
                st.test.bell = kGuestBell;
            } else if (item.rfind("insight=", 0) == 0) {
                st.test.insight = std::atoi(item.c_str() + 8);
            } else if (item.rfind("host_warp=", 0) == 0 || item.rfind("host_die", 0) == 0 ||
                       item.rfind("guest_die", 0) == 0 || item.rfind("host_kill=", 0) == 0 ||
                       item.rfind("host_move=", 0) == 0 || item.rfind("guest_move=", 0) == 0 ||
                       item.rfind("host_press=", 0) == 0 || item.rfind("guest_press=", 0) == 0 ||
                       item.rfind("host_lastlamp=", 0) == 0 || item.rfind("host_lamp=", 0) == 0 ||
                       item.rfind("guest_lamp=", 0) == 0 || item.rfind("host_alone=", 0) == 0 ||
                       item.rfind("host_outage=", 0) == 0 || item.rfind("guest_outage=", 0) == 0) {
                TravelStep step;
                step.kind = item.rfind("guest_outage", 0) == 0  ? TravelStep::GuestOutage
                            : item.rfind("host_outage", 0) == 0   ? TravelStep::HostOutage
                            : item.rfind("guest_move", 0) == 0    ? TravelStep::GuestMove
                            : item.rfind("guest_press", 0) == 0  ? TravelStep::GuestPress
                            : item.rfind("host_press", 0) == 0   ? TravelStep::HostPress
                            : item.rfind("host_lastlamp", 0) == 0 ? TravelStep::HostLastLamp
                            : item.rfind("host_lamp=", 0) == 0   ? TravelStep::HostLamp
                            : item.rfind("guest_lamp=", 0) == 0  ? TravelStep::GuestLamp
                            : item.rfind("host_alone=", 0) == 0  ? TravelStep::HostAlone
                            : item[0] == 'g'                     ? TravelStep::GuestDie
                            : item.rfind("host_die", 0) == 0     ? TravelStep::HostDie
                            : item.rfind("host_kill", 0) == 0    ? TravelStep::HostKill
                            : item.rfind("host_move", 0) == 0    ? TravelStep::HostMove
                                                                 : TravelStep::HostWarp;
                if (step.kind == TravelStep::HostWarp || step.kind == TravelStep::HostKill) {
                    step.id = static_cast<std::uint32_t>(std::strtoul(item.c_str() + 10, nullptr, 10));
                }
                if (step.kind == TravelStep::HostLamp) {
                    step.id = static_cast<std::uint32_t>(std::strtoul(item.c_str() + 10, nullptr, 10));
                }
                if (step.kind == TravelStep::HostLastLamp) {
                    step.id = static_cast<std::uint32_t>(std::strtoul(item.c_str() + 14, nullptr, 10));
                }
                if (step.kind == TravelStep::HostLamp || step.kind == TravelStep::GuestLamp ||
                    step.kind == TravelStep::HostAlone) {
                    step.id = static_cast<std::uint32_t>(std::strtoul(item.c_str() + item.find('=') + 1, nullptr, 10));
                }
                if ((step.kind == TravelStep::HostDie || step.kind == TravelStep::GuestDie) && item.find('%') != std::string::npos) {
                    // host_die@S%MAP: S s after the last load / previous step, in that map (no re-join)
                    step.map = item.substr(item.find('%') + 1);
                }
                if (step.kind == TravelStep::HostOutage || step.kind == TravelStep::GuestOutage) {
                    step.buttons = static_cast<std::uint32_t>(std::strtoul(item.c_str() + item.find('=') + 1, nullptr, 10));
                }
                if (step.kind == TravelStep::HostPress || step.kind == TravelStep::GuestPress) {
                    step.buttons = static_cast<std::uint32_t>(std::strtoul(item.c_str() + item.find('=') + 1, nullptr, 0));
                    if (const std::size_t pc = item.find('%'); pc != std::string::npos) {
                        step.map = item.substr(pc + 1);
                    }
                }
                if (step.map.empty() && item.find('%') != std::string::npos) {
                    // Any step with %MAP runs on the steady-world clock (S s after the last load or
                    // the previous step, in that map): chains of warps / kills without re-joins.
                    step.map = item.substr(item.find('%') + 1);
                }
                if (step.kind == TravelStep::HostMove || step.kind == TravelStep::GuestMove) {
                    // host_move=X:Y:Z@S[%MAP] (':' separates: ',' separates the list items)
                    const char* p = item.c_str() + item.find('=') + 1;
                    for (int k = 0; k < 3; ++k) {
                        char* end = nullptr;
                        step.pos[k] = std::strtof(p, &end);
                        p = (end && *end == ':') ? end + 1 : end;
                    }
                    if (p && p > item.c_str() && p[-1] == ':') {  // X:Y:Z:YAW
                        step.yaw = std::strtof(p, nullptr);
                        step.has_yaw = true;
                    }
                    if (const std::size_t pc = item.find('%'); pc != std::string::npos) {
                        step.map = item.substr(pc + 1);
                    }
                }
                if (const std::size_t at_sign = item.find('@'); at_sign != std::string::npos) {
                    step.delay = std::atof(item.c_str() + at_sign + 1);
                }
                if (const std::size_t hash = item.find('#'); hash != std::string::npos) {
                    step.min_join = std::atoi(item.c_str() + hash + 1);
                }
                st.test.steps.push_back(step);
            } else if (item.rfind("host_mark", 0) == 0 || item.rfind("guest_mark", 0) == 0 ||
                       item.rfind("host_stage=", 0) == 0 || item.rfind("host_walk=", 0) == 0 ||
                       item.rfind("host_headstone=", 0) == 0 || item.rfind("host_flag=", 0) == 0 ||
                       item.rfind("host_obj=", 0) == 0 || item.rfind("guest_obj=", 0) == 0 ||
                       item.rfind("host_chr=", 0) == 0 || item.rfind("guest_chr=", 0) == 0 ||
                       item.rfind("host_skipremo", 0) == 0 || item.rfind("guest_skipremo", 0) == 0) {
                TravelStep step;
                step.kind = item.rfind("host_obj=", 0) == 0     ? TravelStep::HostObj
                            : item.rfind("guest_obj=", 0) == 0  ? TravelStep::GuestObj
                            : item.rfind("host_chr=", 0) == 0   ? TravelStep::HostChr
                            : item.rfind("guest_chr=", 0) == 0  ? TravelStep::GuestChr
                            : item.rfind("guest_mark", 0) == 0    ? TravelStep::GuestMark
                            : item.rfind("host_mark", 0) == 0   ? TravelStep::HostMark
                            : item.rfind("host_stage=", 0) == 0 ? TravelStep::HostStage
                            : item.rfind("host_walk=", 0) == 0  ? TravelStep::HostWalk
                            : item.rfind("host_flag=", 0) == 0  ? TravelStep::HostFlag
                            : item.rfind("host_skipremo", 0) == 0  ? TravelStep::HostSkipRemo
                            : item.rfind("guest_skipremo", 0) == 0 ? TravelStep::GuestSkipRemo
                                                                : TravelStep::HostHeadstone;
                if (const std::size_t pc = item.find('%'); pc != std::string::npos) {
                    step.map = item.substr(pc + 1); // only while the current map name contains it
                }
                if (const std::size_t eq = item.find('='); eq != std::string::npos) {
                    const std::size_t stop = item.find_first_of("@#%", eq);
                    step.args = item.substr(eq + 1, stop == std::string::npos ? std::string::npos : stop - eq - 1);
                }
                if (const std::size_t at_sign = item.find('@'); at_sign != std::string::npos) {
                    step.delay = std::atof(item.c_str() + at_sign + 1);
                }
                if (const std::size_t hash = item.find('#'); hash != std::string::npos) {
                    step.min_join = std::atoi(item.c_str() + hash + 1);
                }
                st.test.steps.push_back(step);
            } else if (!item.empty()) {
                Log("BB_PARTY_DIRECTOR_TEST: '%s' is not log_state, start, grant_bells, drop_bells, award_lot=N, set_flag=N[:N], insight=N, "
                    "ring_host, ring_guest, bell_host, bell_guest, host_warp=ID@S, host_kill=ENTITY@S, host_move=X:Y:Z[:YAW]@S[%MAP], guest_move=..., host_press=BUTTONS@S, guest_press=BUTTONS@S, host_die@S, guest_die@S, "
                    "host_mark[=lua|=EFFECT]@S, guest_mark@S, host_stage=A:B:R:I:P|bonfire:ID@S, host_walk=X:Y:Z:YAW[/...]@S or host_headstone=TALK:DOWNS@S; ignored",
                    item.c_str());
            }
        }
        if (const char* d = std::getenv("BB_PARTY_DIRECTOR_TEST_DELAY"); d && d[0]) {
            st.test.delay = std::atof(d);
        }
    }
    bool known = true;
    st.start_mode = ParseStartMode(std::getenv("BB_PARTY_START"), &known);
    if (!known) {
        Log("BB_PARTY_START='%s' is not prologue_solo or immediate; using prologue_solo", std::getenv("BB_PARTY_START"));
    }
    if (const char* g = std::getenv("BB_PARTY_GRANT_BELLS"); g && g[0] == '0') {
        st.grant_bells = false;
    }
    st.watchdog = HostWatchdog(HostWatchdogConfigFromEnv());
    Log("director: role %s, max %d players, %s every %.0f s (first %.1f s in the world), fast join %s, start %s, "
        "bell grants %s%s%s",
        role == PartyRole::Host ? "host" : role == PartyRole::Guest ? "guest" : "none", st.max_players,
        st.auto_ring ? "bell" : "no automatic bell", st.ring_every, st.ring_delay, st.fast_join ? "on" : "off",
        StartModeName(st.start_mode),
        st.grant_bells ? "on" : "off", st.test.on ? "; test: " : "", st.test.on ? std::getenv("BB_PARTY_DIRECTOR_TEST") : "");
}

void PartyDirector::SetLink(party::PartyLink* link) {
    State& st = S();
    std::lock_guard<std::mutex> lk(st.mu);
    st.link = link;
}

PartyRole PartyDirector::Role() const {
    State& st = S();
    std::lock_guard<std::mutex> lk(st.mu);
    return st.role;
}

bool GatherGuest(const GameSnapshot& s, party::PartyLink* link, Clock::time_point now);

void PartyDirector::Tick() {
    State& st = S();
    const auto now = Clock::now();
    const std::uint64_t tid = ThreadId();
    if (!st.started) {
        st.started = true;
        st.start = now;
        st.last_log = now;
        st.tick_thread = tid;
        Log("tick: first call on thread %llu (SprjFlipper::Update entry)", static_cast<unsigned long long>(tid));
    } else if (tid != st.tick_thread) {
        if (st.foreign_ticks++ < 4) {
            Log("tick: called on thread %llu, not %llu", static_cast<unsigned long long>(tid),
                static_cast<unsigned long long>(st.tick_thread));
        }
        return;
    }
    ++st.ticks;
    // The cheap reads every frame; the game's cooperator count twice a second.
    const bool full = Seconds(st.last_full, now) >= 0.5;
    if (full) {
        st.last_full = now;
    }
    perf::Scope zone(perf::Zone::DirReadState);
    GameSnapshot s = ReadGameState(full);
    zone.End();
    if (!full && st.have_last) {
        s.cooperators = st.last.cooperators;
    }
    LogTransitions(st, s);
    // A6: the host ended the party (or is gone) while this guest stands in its world: home at once
    // through the game's own send-home (OnLeave_Limit) instead of the game's room-gone path 10 s
    // later with "Session lost" (0xcc/5001 from OnRoomDisappeared 0x138A590, live).
    if (g_send_home_request.load(std::memory_order_relaxed) && s.world_up && !s.loading) {
        g_send_home_request = false;
        std::uint64_t wts = 0;
        std::uint8_t own = 1;
        SafeGet(Guest(0x5556678), &wts);
        if (wts) SafeGet(wts + 0x1592, &own);
        std::string why;
        {
            std::lock_guard<std::mutex> lk(g_send_home_mu);
            why = g_send_home_why;
        }
        if (own == 0 && OnLeaveLimitOk()) {
            RunOnLeaveLimit();
            Log("host lost (%s): home now (OnLeave_Limit)", why.c_str());
        } else {
            Log("host lost (%s): already in the own world", why.c_str());
        }
    }
    if (s.world_up && !s.loading && !st.world_seen) {
        st.world_seen = true;
        st.world_at = now;
    } else if (!s.world_up) {
        st.world_seen = false;
    }
    st.last = s;
    st.have_last = true;
    zone.Next(perf::Zone::DirNpcTest);
    NpcTestTick(s); // BB_PARTY_TEST_NPC fixture (party_npc_test.h)
    b2::Tick(s);    // B2 seamless prototype (party_b2.h; BB_PARTY_B2=1)
    zone.End();

    PartyRole role;
    party::PartyLink* link;
    StartMode start_mode;
    bool grant_bells;
    {
        std::lock_guard<std::mutex> lk(st.mu);
        role = st.role;
        link = st.link;
        start_mode = st.start_mode;
        grant_bells = st.grant_bells;
    }
    // Test aids: scripted new game, prologue fast-forward (test_newgame.h).
    NewGameTestTick(s, TitleReadyForInput(role, link));
    // Late joiner catch-up (party_catchup.h): a guest behind the host is brought up to it and
    // gathered at the host's lamp before any summon.
    {
        std::uint32_t host_map = 0;
        if (role == PartyRole::Guest && link) {
            for (const party::RosterEntry& e : link->roster()) {
                if (e.slot == party::kHostSlot) {
                    host_map = e.map_id;
                }
            }
        }
        CatchupTick(s, role == PartyRole::Guest && link && link->state() == party::LinkState::Connected,
                    Seconds(st.start, now), host_map);
    }
    // C1: the campaign start (readiness, bells).
    if (full || !st.start_logged) {
        perf::Scope z(perf::Zone::DirStart);
        UpdateStart(st, s, start_mode);
        if (((grant_bells && role != PartyRole::None) || st.test.grant_bells) && st.world_seen &&
            Seconds(st.world_at, now) >= std::min(5.0, st.ring_delay) && !PrologueSkipBusy() && !CatchupBusy()) {
            GrantBells(st, s);
        }
    }
    zone.Next(perf::Zone::Progress);
    progress::DirectorTick(s, link); // C2: flag capture / apply, host -> guest sync (party_progress.h)
    zone.Next(perf::Zone::DirTitle);
    // Title: the FROM client's state when it changes (sign-in debugging).
    if (role != PartyRole::None && full && !s.world_up) {
        std::string fc = DescribeFromClient();
        if (fc != st.last_from_client) {
            Log("title: %s", fc.c_str());
            st.last_from_client = std::move(fc);
        }
    }
    // Title: Continue on our own (party mode default; always after a crash restart).
    if (role != PartyRole::None) {
        int ac;
        {
            std::lock_guard<std::mutex> lk(st.mu);
            ac = st.auto_continue;
        }
        if ((ac != 0 || party::runtime().restarted()) && !NewGameTestRequested()) {
            AutoContinue(st, s, role, link, now);
        }
    }
    zone.Next(perf::Zone::DirSend);
    // The roster entry.
    if (link) {
        const party::MemberState ms = LocalState(s, st.start_ready);
        const std::uint32_t map = s.world_up ? s.map_id : 0;
        if (!st.sent_any || ms != st.sent_state || map != st.sent_map) {
            st.sent_any = true;
            st.sent_state = ms;
            st.sent_map = map;
            link->set_local_state(ms, map);
        }
    }
    // The party HUD: our vitals out, the companions' board for the overlay (party_hud.h).
    if (role != PartyRole::None) PartyHudTick(s, link, LocalState(s, st.start_ready));
    // C4 rot, D7 enemy scaling, E2 extras: the host's rules for the party (party_balance.h).
    if (role != PartyRole::None) balance::Tick(s, link, role == PartyRole::Host);
    // B1: the host's warps go to every guest (reliable: replayed to a member who reconnects).
    if (link && role == PartyRole::Guest) {
        StoryIntent up;
        while (PopGuestStoryUp(&up)) {  // D10: the guest's own cutscene, for the host to share
            link->send_event(0, kStoryUpEventName, StoryToJsonText(up));
            Log("story: own cutscene %u sent to the host (D10)", up.id);
        }
    }
    if (link && role == PartyRole::Host) {
        TravelIntent t;
        while (PopHostTravel(&t)) {
            // Into (or respawning in) a Chalice Dungeon: which one (guests follow only into it).
            if (TravelIntoChalice(t) || static_cast<std::uint32_t>(t.last_lamp) / 100000 == 29) {
                t.chalice_channel = chalice::Read().channel;
            }
            link->post_event(party::kBroadcast, kTravelEventName, TravelToJsonText(t));
            party::bridge::OnTravel(TravelKindName(t.kind));
            Log("travel #%llu (%s) sent to the party", static_cast<unsigned long long>(t.seq), TravelKindName(t.kind));
            PhantomNoteHostTravel(t); // a rest (Dream, death, Mark): guests refill too
        }
        // C4: the host's cutscenes, endings and time of day (party_story.h).
        StoryIntent si;
        while (PopHostStory(&si)) {
            link->post_event(party::kBroadcast, kStoryEventName, StoryToJsonText(si));
            Log("story #%llu (%s %u) sent to the party", static_cast<unsigned long long>(si.seq),
                StoryKindName(si.kind), si.id);
        }
        // C3: the host's item lots to every guest; the full list to a member that (re)joined.
        std::vector<ItemGrant> items;
        ItemGrant g;
        while (PopHostItems(&g)) {
            items.push_back(g);
        }
        if (!items.empty()) {
            link->post_event(party::kBroadcast, kItemsEventName, ItemsToJsonText(items));
            Log("items: %zu lots sent to the party", items.size());
        }
        // The host's chalices (its own and those the guests sent): every guest gets the ones it
        // lacks (party_chalice.h).
        std::vector<std::uint32_t> chalices;
        if (chalice::PopHostGoods(&chalices)) {
            link->post_event(party::kBroadcast, chalice::kGoodsEventName, chalice::GoodsToJsonText(chalices));
        }
        int slot = -1;
        while (TakeHostFullItems(&slot, &items)) {
            link->post_event(slot, kItemsFullEventName, ItemsToJsonText(items));
            Log("items: full list (%zu lots) sent to slot %d", items.size(), slot);
        }
    }
    // A guest's chalices go to the host (any member can perform the ritual; party_chalice.h).
    if (link && role == PartyRole::Guest) {
        std::vector<std::uint32_t> chalices;
        if (chalice::PopHostGoods(&chalices)) {
            link->post_event(party::kHostSlot, chalice::kGoodsEventName, chalice::GoodsToJsonText(chalices));
        }
    }
    // B2 (party_b2.h): guest "arrived" / host notes.
    if (link) {
        b2::SetLocalSlot(link->local_slot());
        {
            // Split travel: where every member is (party_b2 SplitTick; docs/party/split_travel.md).
            party::MemberVitals vit[party::kMaxVitals];
            const int vn = link->vitals(vit, party::kMaxVitals);
            const std::vector<party::RosterEntry> roster = link->roster();
            std::vector<b2::Where> where;
            for (int i = 0; i < vn; ++i) {
                b2::Where w;
                w.slot = vit[i].slot;
                w.map = vit[i].map_id;
                w.has_pos = (vit[i].flags & party::kVitalsHasPos) != 0;
                std::memcpy(w.pos, vit[i].pos, sizeof w.pos);
                w.age_ms = vit[i].age_ms;
                w.connected = false;
                for (const party::RosterEntry& e : roster) {
                    if (e.slot == w.slot) {
                        w.connected = e.connected;
                    }
                }
                where.push_back(w);
            }
            b2::SetWhere(where);
        }
        int b2slot = party::kBroadcast;
        std::string b2body;
        while (b2::PopOutgoing(&b2slot, &b2body)) {
            link->post_event(b2slot, b2::kEventName, b2body);
        }
    }
    zone.Next(perf::Zone::StatusBridge);
    // The overlay's Party tab: status board and its commands (party_status_bridge.h).
    if (role != PartyRole::None &&
        party::bridge::Tick({role == PartyRole::Host, link, s.world_up, s.loading, s.session_role, s.cooperators,
                             link ? link->max_players() : 3, InWorld(s) && !st.start_ready,
                             StartStepName(st.start_step)})
            .ring_now) {
        st.rung_ever = false; // Rejoin while connected: the next bell goes up at once
    }
    // A host summon that never completes: end the session and ring again (party_watchdog.h).
    if (role == PartyRole::Host && full) {
        HostWatchdogTick(st, s, link, now);
    }
    // Phantoms as full players: the host's lamp / rest / boss Insight out, the guest's refill and
    // Insight in (party_phantom.h).
    zone.Next(perf::Zone::Phantom);
    PhantomTick(link);
    // Guests use the Doll, shops and workshop in the host's Dream (party_guestsvc.h).
    GuestServicesTick(s, link);
    // A guest's levers / doors / NPC talk change the host's world (party_worldact.h).
    worldact::Tick(s, link);
    split::Tick(s);  // split travel: the lamp choice dialog (party_split.h)
    zone.Next(perf::Zone::DirRing);
    if (st.test.on) {
        RunTravelSteps(st, s, role, now);
        RunTest(st, s, now);
        if ((st.test.log_state || st.test.start) && Seconds(st.last_log, now) >= 5.0) {
            const double secs = Seconds(st.last_log, now);
            if (st.test.start) {
                Log("start: %s", DescribeStart(st.cs, InWorld(s)).c_str());
            }
            Log("state: %s; %llu ticks in %.1f s on thread %llu; game Lua dispatches %llu on it, %llu elsewhere",
                Describe(s).c_str(), static_cast<unsigned long long>(st.ticks - st.ticks_at_log), secs,
                static_cast<unsigned long long>(st.tick_thread),
                static_cast<unsigned long long>(g_dispatch_main.load()),
                static_cast<unsigned long long>(g_dispatch_other.load()));
            st.last_log = now;
            st.ticks_at_log = st.ticks;
        }
    }
    // Fast join, host: a member's new sign is on our board - let the game's get-list poll run on
    // its next frame (the FROM client's countdown [0x5540288]+0x100, set to the 1 s interval after
    // each poll; the builder 0x1e946e0 requests once it is <= 0) instead of up to 1 s later.
    if (role == PartyRole::Host && st.fast_join) {
        const std::uint64_t signs = bbnet::sign_board_changes();
        if (signs != st.signs_seen) {
            st.signs_seen = signs;
            std::uint64_t cm = 0;
            float countdown = 0;
            if (SafeGet(Guest(0x5540288), &cm) && cm && SafeGet(cm + 0x100, &countdown) && countdown > 0) {
                const float zero = 0;
                SafeWrite(cm + 0x100, &zero, sizeof zero);
                Log("sign on the board: the game polls it now (was in %.2f s)", countdown);
            }
        }
    }
    if (role == PartyRole::Host) {
        const std::uint64_t summons = bbnet::summon_requests();
        if (summons != st.summons_seen) {
            st.summons_seen = summons;
            st.summon_at = now;
        }
    }
    if (role == PartyRole::Host && st.fast_join) {
        for (std::string& name : bbnet::party::PartyHostService::instance().take_restarted()) {
            Log("%s restarted its game: its old peer is cleared once our game is out of that session", name.c_str());
            st.restarted.push_back(std::move(name));
        }
        // Not while our game is still in the session with the dead peer (it ends that one itself).
        if (!st.restarted.empty() && s.session_role == RoleIdle) {
            for (const std::string& name : st.restarted) {
                Log("%s: %d stale peer(s) of our game cleared", name.c_str(), ClearStalePeers(name));
            }
            st.restarted.clear();
        }
    }
    if (!st.poll_logged && Seconds(st.last_ring, now) >= 2.5) {
        st.poll_logged = true;
        Log("bell +2.5 s: %s", DescribeSignPolling().c_str());
    }
    // Automatic summoning. The host also rings while hosting: the 2nd / 3rd cooperator of a
    // 3- / 4-player party (HostMayRing checks the count).
    if (!st.auto_ring || role == PartyRole::None || !link || !s.world_up || s.loading || !st.world_seen ||
        Seconds(st.world_at, now) < st.ring_delay || !SessionMayRing(role == PartyRole::Host, s.session_role) ||
        PrologueSkipBusy() || CatchupBusy()) {
        return;
    }
    if (role == PartyRole::Guest && st.rung_ever && Seconds(st.last_ring, now) < st.ring_every) {
        // The host entered its Chalice Dungeon after this guest rang: the waiting sign gets the
        // chalice wait (9900) too, else the host's summon loads the guest without the quick join
        // (live: the guest rang ~10 s before the host's roster showed area 29).
        const std::uint64_t player = LocalPlayer();
        std::uint32_t host_map = 0;
        for (const party::RosterEntry& e : link->roster()) {
            if (e.slot == party::kHostSlot) host_map = e.map_id;
        }
        if (((host_map >> 24) & 0xff) == 29 && host_map != 0xffffffffu && HasSpEffect(player, kGuestBellEffect) &&
            !HasSpEffect(player, kChaliceWaitEffect) && Seconds(st.chalice_wait_at, now) >= 5.0) {
            st.chalice_wait_at = now;
            const bool ok = ApplySpEffect(kChaliceWaitEffect);
            Log("bell: the host entered a chalice dungeon (%s) while this guest waits: SpEffect %d %s",
                MapName(host_map).c_str(), kChaliceWaitEffect, ok ? "applied" : "NOT applied");
        }
    }
    if (st.rung_ever && Seconds(st.last_ring, now) < st.ring_every) {
        return;
    }
    // A11 (ERSC_PARITY.md, user decision): a player joining while the host fights a boss or
    // watches a cutscene waits outside - the host does not ring until it ends (the guest's sign
    // waits at the host's lamp in its own world, GatherGuest).
    if (role == PartyRole::Host) {
        const bool boss = BossBarShown(), remo = CutscenePlaying();
        if (boss || remo) {
            if (!st.a11_waiting) {
                st.a11_waiting = true;
                Log("A11: no bell while the host %s: joiners wait outside", boss ? "fights a boss" : "watches a cutscene");
            }
            return;
        }
        if (st.a11_waiting) {
            st.a11_waiting = false;
            Log("A11: the boss fight / cutscene is over: ringing for waiting joiners again");
        }
    }
    // C1 (party_start.h): no bell before this player finished its start (clinic / first death
    // pitfalls); the host rings only for members that are ready (roster Home / Joining).
    if (role == PartyRole::Guest) {
        if (StoryBusy()) {
            return; // C4: a cutscene / ending replay first, then the rejoin
        }
        if (GuestMayRing(st.start_ready, link->state() == party::LinkState::Connected)) {
            if (GatherGuest(s, link, now)) {
                return; // travelling to the host's lamp in its own world first; ring there
            }
            Ring(st, kGuestBell, "guest, ready and idle in its own world", now);
        }
    } else {
        int max_players;
        {
            std::lock_guard<std::mutex> lk(st.mu);
            max_players = st.max_players;
        }
        // Not while a summon our game asked for is under way (a bell then restarts its search
        // and the summon fails: the guest waits for the summon data until it gives up).
        if (st.summons_seen && Seconds(st.summon_at, now) < 20.0) {
            return;
        }
        if (link->state() == party::LinkState::Hosting &&
            HostMayRing(st.start_ready, s.cooperators, max_players, link->roster())) {
            Ring(st, kHostBell, "host ready, a ready party member waits", now);
        }
    }
}

std::string PartyLogStamp() {
    static const bool on = [] {
        const char* t = std::getenv("BB_PARTY_DIRECTOR_TEST");
        const char* l = std::getenv("BB_PARTY_LOG_TIME");
        return (t && t[0]) || (l && l[0] == '1');
    }();
    if (!on) {
        return {};
    }
    const auto now = std::chrono::system_clock::now();
    const std::time_t secs = std::chrono::system_clock::to_time_t(now);
    const int ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &secs);
#else
    localtime_r(&secs, &tm);
#endif
    char b[32];
    std::snprintf(b, sizeof b, "[%02d:%02d:%02d.%03d] ", tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
    return b;
}

/// Gather (BB_PARTY_GATHER, default on): before a guest rings, its own world goes to the host's
/// last lamp when the host's map (roster slot 0) is another area/block than the guest's own, so
/// the summon brings it into the host's map and "home" always follows the host (a fresh session,
/// or after any return home). The lamp id is the host's last lamp (party_phantom / travel
/// events: GuestDeathRedirectLamp). One gather per 90 s; logs why it does or does not run.
bool GatherGuest(const GameSnapshot& s, party::PartyLink* link, Clock::time_point now) {
    static const bool on = !(std::getenv("BB_PARTY_GATHER") && std::getenv("BB_PARTY_GATHER")[0] == '0');
    static Clock::time_point last{};
    static bool ever = false;
    if (!on || !link || !s.world_up || s.loading || s.session_role != RoleIdle) {
        return false;
    }
    std::uint32_t host_map = 0;
    for (const party::RosterEntry& e : link->roster()) {
        if (e.slot == party::kHostSlot) {
            host_map = e.map_id;
        }
    }
    // The host's last lamp unless it lies in another map than the one the host stands in (a fresh
    // host's default record): then a lamp of the host's map.
    const std::uint32_t lamp = GatherLampFor(GuestDeathRedirectLamp(), host_map);
    if (!host_map || host_map == 0xffffffffu || s.map_id == 0xffffffffu || lamp == kTravelNone || lamp == 0) {
        return false;
    }
    if ((host_map >> 16) == (s.map_id >> 16)) {
        // Same area / block: the summon lands in the host's map; step aside first (B2 spacing).
        return b2::GuestPreRing(s, host_map);
    }
    // The host in a Chalice Dungeon (or its last lamp in one): no gather. A warp into area 29 would
    // build this game's own slot's dungeon (or none, seen live); the summon itself brings the guest
    // in (quick join by ChannelId, docs/party/chalice.md 4), so it rings where it stands.
    if (((host_map >> 24) & 0xff) == 29 || lamp / 100000 == 29) {
        static Clock::time_point said{};
        if (Seconds(said, now) >= 60.0) {
            said = now;
            Log("gather: the host is in a chalice dungeon (%s, lamp %u); ringing here, the summon quick-joins it",
                MapName(host_map).c_str(), lamp);
        }
        return false;
    }
    if (ever && Seconds(last, now) < 90.0) {
        return false;
    }
    ever = true;
    last = now;
    TravelIntent t;
    t.kind = TravelKind::Lamp;
    t.lamp_id = lamp;
    Log("gather: own world %s, host in %s; to the host's lamp %u before ringing", MapName(s.map_id).c_str(),
        MapName(host_map).c_str(), lamp);
    return ReplayTravelNow(t);
}

bool PartyRequested() {
    const char* t = std::getenv("BB_PARTY_DIRECTOR_TEST");
    const char* i = std::getenv("BB_PARTY_ITEMS_TEST"); // C3 single-instance check
    return bb_party_enabled() || (t && t[0]) || (i && i[0]) || NpcTestRequested() || StoryTestRequested() ||
           b2::Requested() || NewGameTestRequested();
}

void CoopTick() {
    using perf::Zone;
    perf::FrameBoundary(); // BB_PARTY_PERF=1: closes the last frame's per-zone sums
    perf::Scope total(Zone::Tick);
    perf::Scope z(Zone::LuaEvents);
    LuaEventsTick();
    z.Next(Zone::Director);
    PartyDirector::Get().Tick();
    chalice::Tick(PartyDirector::Get().Role() == PartyRole::Host); // the local dungeon, the party's chalices
    z.Next(Zone::Travel);
    TravelTick(); // B1 guest replay (party_travel.h)
    z.Next(Zone::Story);
    StoryTick(); // C4 cutscenes / endings (party_story.h)
    z.Next(Zone::Items);
    ItemsTick();    // C3 guest item replay (party_items.h)
    GameSaveTick(); // save requests after summoned writes (game_state.h)
    z.Next(Zone::Seamless);
    SeamlessRulesTick(); // A6 param rules, EMEVD filter stats (seamless_rules.h)
    z.Next(Zone::Fourp);
    fourp::FourpTick(); // 4-player rules: the party's max players (party_fourp.h)
}

void PartyInit(unsigned char* image, std::uint64_t size) {
    perf::Init(); // BB_PARTY_PERF=1 (party_perf.h), before any hook goes in
    if (!HooksInit(image, size)) {
        Log("no image; party director off");
        return;
    }
    LuaEventsInit();
    progress::Init();         // C2 progress sync (party_progress.h)
    progress::InstallHooks(); // byte-verified flag hooks (BB_PARTY_PROGRESS=0: none)
    chalice::InstallProbes(); // summon message log (party_chalice.h)
    OnlineDiagInit();    // FROM client drop-offline log (party_online.h)
    SeamlessRulesInit(); // A6: party patch report, EMEVD filter (seamless_rules.h)
    fourp::FourpInit();  // 4-player parties: H1-H4, E6, P5 (party_fourp.h)
    PartyDirector::Get().ConfigureFromEnv();
    if (const char* t = std::getenv("BB_PARTY_TRACE_BELL"); t && t[0] == '1') {
        InstallBellTrace(std::make_integer_sequence<int, sizeof(g_trace_points) / sizeof(g_trace_points[0])>{});
    }
    if (const char* t = std::getenv("BB_PARTY_TRACE_SSINFO"); t && t[0] == '1') {
        HookCallSite(kSsParseCall, kSsParse, reinterpret_cast<const void*>(&SsParseHook), "ss.info parse log");
    }
    InstallTravelPatches(); // B1: Dream gate + travel hooks (byte-verified)
    b2::Init();             // B2 seamless prototype (BB_PARTY_B2=1; byte-verified)
    split::Init();          // split travel's "Travel with the party?" (needs B2)
    InstallStoryHooks();    // C4: bank-2002 capture, guest mirror / replay (byte-verified)
    InstallItemsPatches();  // C3: award hook, parity patches (byte-verified)
    PhantomInit();          // guest respawn at the host's lamp, refill, boss Insight
    worldact::Init();       // guest world actions, instanced treasure pickups (byte-verified)
    GuestServicesInit();    // guest Doll / shops / workshop in the host's Dream (byte-verified)
    balance::Init();        // C4 rot hook, D7 scaling (party_balance.h)
    g_tick_installed = HookPrologue(kFlipperUpdate,
                                    {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48,
                                     0x83, 0xec, 0x38},
                                    &FlipperEntry, "main-thread tick (SprjFlipper::Update)");
    if (!g_tick_installed) {
        Log("no main-thread tick: the director and Lua events are off");
        return;
    }
    if (S().test.on && S().test.log_state) {
        // The game's own Lua events: their thread (the tick's?) and what follows a bell.
        HookPrologue(kDispatchByName, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53},
                     &DispatchEntry, "Lua event log (LuaEvent_DispatchByName)");
    }
}

} // namespace coop

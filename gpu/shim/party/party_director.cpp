// SPDX-License-Identifier: GPL-3.0-or-later
// PartyDirector and the coop main-thread tick (see party_director.h). The test mode follows
// droogie/bbhost src/engine/np_test.cpp @8f2746c (GPL-3.0-or-later): world-up detection, the
// Insight write and the bell events.
#include "party_director.h"

#include "coop_hooks.h"
#include "game_state.h"
#include "lua_events.h"
#include "party_fourp.h"
#include "party_link.h"
#include "party_story.h"
#include "party_status_bridge.h"
#include "party_items.h"
#include "party_npc_test.h"
#include "party_phantom.h"
#include "party_start.h"
#include "party_runtime.h"
#include "party_progress.h"
#include "party_travel.h"
#include "seamless_rules.h"

#include "../net/bbnet_internal.h"

#include <atomic>
#include <cctype>
#include <cstdarg>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

extern "C" void runtime_pad_director_press(std::uint32_t buttons); // src/runtime_pad.c

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
    std::printf("Party: %s\n", line);
    std::fflush(stdout);
}

struct Test {
    bool on = false;
    bool log_state = false;
    int insight = -1;
    const char* ring = nullptr; // the event to raise once
    double delay = 10.0;
    bool start = false, grant_bells = false, drop_bells = false, drop_done = false;
    int award_lot = -1; // award_lot=N: AwardItemLot(N) once (call-shape probe, test saves)
    bool insight_done = false, ring_done = false;
    Clock::time_point rung_at{};
};

struct State {
    std::mutex mu; // role / link (set from other threads)
    PartyRole role = PartyRole::None;
    int max_players = 3;
    party::PartyLink* link = nullptr;
    bool auto_ring = true;
    int auto_continue = -1; // BB_PARTY_AUTOCONTINUE: -1 default (on in party mode), 0 off, 1 on
    double ring_every = 30.0;
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
    Clock::time_point last_full{}, last_log{}, last_ring{};
    bool rung_ever = false;
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
    }
    if (now.loading != was.loading) {
        Log("loading screen %s", now.loading ? "up" : "gone");
    }
    if (now.session_role != was.session_role || now.session_sub_state != was.session_sub_state) {
        Log("session %s (%d, sub %d) -> %s (%d, sub %d)", SessionRoleName(was.session_role), was.session_role,
            was.session_sub_state, SessionRoleName(now.session_role), now.session_role, now.session_sub_state);
    }
    if (now.matching_status != was.matching_status) {
        Log("matching status %d -> %d", was.matching_status, now.matching_status);
    }
    if (now.map_id != was.map_id && now.world_up) {
        Log("map %s -> %s", MapName(was.map_id).c_str(), MapName(now.map_id).c_str());
    }
    if (now.cooperators != was.cooperators && now.cooperators >= 0 && was.cooperators >= 0) {
        Log("cooperators %d -> %d", was.cooperators, now.cooperators);
    }
    if (now.online_mode != was.online_mode) {
        Log("online mode %d -> %d", was.online_mode, now.online_mode);
    }
    if (now.insight != was.insight && now.insight >= 0 && was.insight >= 0) {
        Log("Insight %d -> %d", was.insight, now.insight);
    }
}

std::string DescribeFromClient();

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

void Ring(State& st, const char* event, const char* why, Clock::time_point now) {
    st.last_ring = now;
    st.rung_ever = true;
    const bool host = event == kHostBell;
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
    if (ok) {
        party::bridge::OnRing();
    }
    Log("bell: %s", DescribeFromClient().c_str());
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
    if (Seconds(st.continue_ready, now) < 4.0) {
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
    if (Seconds(st.continue_last, now) >= 2.5) {
        st.continue_last = now;
        st.continue_held = true;
        ++st.continue_presses;
        runtime_pad_director_press(kPadCross);
        if (st.continue_presses <= 5 || st.continue_presses % 10 == 0) {
            Log("auto-continue: Cross #%d (%s; %s)", st.continue_presses, Describe(s).c_str(), DescribeFromClient().c_str());
        }
    }
}

void RunTest(State& st, const GameSnapshot& s, Clock::time_point now) {
    Test& t = st.test;
    if (!st.world_seen) {
        return;
    }
    if (t.insight >= 0 && !t.insight_done && Seconds(st.world_at, now) >= 2.0 && !s.loading) {
        t.insight_done = true;
        const int before = s.insight;
        const bool ok = WritePlayerInsight(t.insight);
        Log("test: Insight %d -> %d %s", before, t.insight, ok ? "written" : "NOT written (no player record)");
    }
    if (t.award_lot > 0 && Seconds(st.world_at, now) >= 12.0 && s.world_up && !s.loading) {
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
    st.max_players = max_players < 2 ? 2 : max_players > 4 ? 4 : max_players;
}

void PartyDirector::ConfigureFromEnv() {
    State& st = S();
    PartyRole role = PartyRole::None;
    if (const char* p = std::getenv("BB_PARTY"); p && p[0]) {
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
            } else if (item == "drop_bells") {
                st.test.drop_bells = true;
            } else if (item == "grant_bells") {
                st.test.grant_bells = true;
            } else if (item == "ring_host") {
                st.test.ring = kHostBell;
            } else if (item == "ring_guest") {
                st.test.ring = kGuestBell;
            } else if (item.rfind("insight=", 0) == 0) {
                st.test.insight = std::atoi(item.c_str() + 8);
            } else if (!item.empty()) {
                Log("BB_PARTY_DIRECTOR_TEST: '%s' is not log_state, start, grant_bells, drop_bells, award_lot=N, insight=N, ring_host or "
                    "ring_guest; ignored",
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
    Log("director: role %s, max %d players, %s every %.0f s, start %s, bell grants %s%s%s",
        role == PartyRole::Host ? "host" : role == PartyRole::Guest ? "guest" : "none", st.max_players,
        st.auto_ring ? "bell" : "no automatic bell", st.ring_every, StartModeName(st.start_mode),
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
    GameSnapshot s = ReadGameState(full);
    if (!full && st.have_last) {
        s.cooperators = st.last.cooperators;
    }
    LogTransitions(st, s);
    if (s.world_up && !s.loading && !st.world_seen) {
        st.world_seen = true;
        st.world_at = now;
    } else if (!s.world_up) {
        st.world_seen = false;
    }
    st.last = s;
    st.have_last = true;
    NpcTestTick(s); // BB_PARTY_TEST_NPC fixture (party_npc_test.h)

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
    // C1: the campaign start (readiness, bells).
    if (full || !st.start_logged) {
        UpdateStart(st, s, start_mode);
        if (((grant_bells && role != PartyRole::None) || st.test.grant_bells) && st.world_seen &&
            Seconds(st.world_at, now) >= 5.0) {
            GrantBells(st, s);
        }
    }
    progress::DirectorTick(s, link); // C2: flag capture / apply, host -> guest sync (party_progress.h)
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
        if (ac != 0 || party::runtime().restarted()) {
            AutoContinue(st, s, role, link, now);
        }
    }
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
    // B1: the host's warps go to every guest (reliable: replayed to a member who reconnects).
    if (link && role == PartyRole::Host) {
        TravelIntent t;
        while (PopHostTravel(&t)) {
            link->send_event(party::kBroadcast, kTravelEventName, TravelToJsonText(t));
            party::bridge::OnTravel(TravelKindName(t.kind));
            Log("travel #%llu (%s) sent to the party", static_cast<unsigned long long>(t.seq), TravelKindName(t.kind));
            PhantomNoteHostTravel(t); // a rest (Dream, death, Mark): guests refill too
        }
        // C4: the host's cutscenes, endings and time of day (party_story.h).
        StoryIntent si;
        while (PopHostStory(&si)) {
            link->send_event(party::kBroadcast, kStoryEventName, StoryToJsonText(si));
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
            link->send_event(party::kBroadcast, kItemsEventName, ItemsToJsonText(items));
            Log("items: %zu lots sent to the party", items.size());
        }
        int slot = -1;
        while (TakeHostFullItems(&slot, &items)) {
            link->send_event(slot, kItemsFullEventName, ItemsToJsonText(items));
            Log("items: full list (%zu lots) sent to slot %d", items.size(), slot);
        }
    }
    // The overlay's Party tab: status board and its commands (party_status_bridge.h).
    if (role != PartyRole::None &&
        party::bridge::Tick({role == PartyRole::Host, link, s.world_up, s.loading, s.session_role, s.cooperators,
                             link ? link->max_players() : 3, InWorld(s) && !st.start_ready,
                             StartStepName(st.start_step)})
            .ring_now) {
        st.rung_ever = false; // Rejoin while connected: the next bell goes up at once
    }
    // Phantoms as full players: the host's lamp / rest / boss Insight out, the guest's refill and
    // Insight in (party_phantom.h).
    PhantomTick(link);
    if (st.test.on) {
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
    // Automatic summoning.
    if (!st.auto_ring || role == PartyRole::None || !link || !s.world_up || s.loading || !st.world_seen ||
        Seconds(st.world_at, now) < 5.0 || s.session_role != RoleIdle) {
        return;
    }
    if (st.rung_ever && Seconds(st.last_ring, now) < st.ring_every) {
        return;
    }
    // C1 (party_start.h): no bell before this player finished its start (clinic / first death
    // pitfalls); the host rings only for members that are ready (roster Home / Joining).
    if (role == PartyRole::Guest) {
        if (StoryBusy()) {
            return; // C4: a cutscene / ending replay first, then the rejoin
        }
        if (GuestMayRing(st.start_ready, link->state() == party::LinkState::Connected)) {
            Ring(st, kGuestBell, "guest, ready and idle in its own world", now);
        }
    } else {
        int max_players;
        {
            std::lock_guard<std::mutex> lk(st.mu);
            max_players = st.max_players;
        }
        if (link->state() == party::LinkState::Hosting &&
            HostMayRing(st.start_ready, s.cooperators, max_players, link->roster())) {
            Ring(st, kHostBell, "host ready, a ready party member waits", now);
        }
    }
}

bool PartyRequested() {
    const char* p = std::getenv("BB_PARTY");
    const char* t = std::getenv("BB_PARTY_DIRECTOR_TEST");
    const char* i = std::getenv("BB_PARTY_ITEMS_TEST"); // C3 single-instance check
    return (p && p[0]) || (t && t[0]) || (i && i[0]) || NpcTestRequested() || StoryTestRequested();
}

void CoopTick() {
    LuaEventsTick();
    PartyDirector::Get().Tick();
    TravelTick();        // B1 guest replay (party_travel.h)
    StoryTick();         // C4 cutscenes / endings (party_story.h)
    ItemsTick();         // C3 guest item replay (party_items.h)
    SeamlessRulesTick(); // A6 param rules, EMEVD filter stats (seamless_rules.h)
    fourp::FourpTick();  // 4-player rules: the party's max players (party_fourp.h)
}

void PartyInit(unsigned char* image, std::uint64_t size) {
    if (!HooksInit(image, size)) {
        Log("no image; party director off");
        return;
    }
    LuaEventsInit();
    progress::Init();         // C2 progress sync (party_progress.h)
    progress::InstallHooks(); // byte-verified flag hooks (BB_PARTY_PROGRESS=0: none)
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
    InstallStoryHooks();    // C4: bank-2002 capture, guest mirror / replay (byte-verified)
    InstallItemsPatches();  // C3: award hook, parity patches (byte-verified)
    PhantomInit();          // guest respawn at the host's lamp, refill, boss Insight
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

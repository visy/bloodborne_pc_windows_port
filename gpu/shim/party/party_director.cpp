// SPDX-License-Identifier: GPL-3.0-or-later
// PartyDirector and the coop main-thread tick (see party_director.h). The test mode follows
// droogie/bbhost src/engine/np_test.cpp @8f2746c (GPL-3.0-or-later): world-up detection, the
// Insight write and the bell events.
#include "party_director.h"

#include "coop_hooks.h"
#include "game_state.h"
#include "lua_events.h"
#include "party_link.h"
#include "party_start.h"
#include "party_status.h"
#include "party_travel.h"
#include "seamless_rules.h"

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
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

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
    bool start = false, grant_bells = false;
    bool insight_done = false, ring_done = false;
    Clock::time_point rung_at{};
};

struct State {
    std::mutex mu; // role / link (set from other threads)
    PartyRole role = PartyRole::None;
    int max_players = 3;
    party::PartyLink* link = nullptr;
    bool auto_ring = true;
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
    std::string status_sig; // last board content published (party_status.h)
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
    if (now.insight != was.insight && now.insight >= 0 && was.insight >= 0) {
        Log("Insight %d -> %d", was.insight, now.insight);
    }
}

void Ring(State& st, const char* event, const char* why, Clock::time_point now) {
    st.last_ring = now;
    st.rung_ever = true;
    if (LuaEventQueue(event)) {
        Log("ringing %s (%s)", event, why);
    } else {
        Log("could not queue %s (%s)", event, why);
    }
}

/// The overlay's Party board (party_status.h): this player's lobby / readiness state and the
/// roster with each member's state. Published only on change.
void PublishStatus(State& st, const GameSnapshot& s, PartyRole role, party::PartyLink* link, Clock::time_point now) {
    namespace ps = party::status;
    if (role == PartyRole::None || !link) {
        return;
    }
    const party::LinkState ls = link->state();
    ps::State state;
    std::string detail;
    if (ls == party::LinkState::Rejected) {
        state = ps::State::Error;
        detail = link->reject_reason();
    } else if (ls == party::LinkState::Reconnecting) {
        state = ps::State::Reconnecting;
    } else if (ls == party::LinkState::Connecting) {
        state = ps::State::Connecting;
    } else if (!s.world_up || s.loading) {
        state = ps::State::WaitingForWorld;
        detail = s.loading ? "loading" : "title / character creation";
    } else if (s.session_role == RoleClient || (role == PartyRole::Host && s.cooperators > 0)) {
        state = ps::State::Joined;
        if (role == PartyRole::Host) {
            detail = std::to_string(s.cooperators) + " in";
        }
    } else if (!st.start_ready) {
        state = ps::State::WaitingForWorld;
        detail = std::string("prologue (") + StartStepName(st.start_step) + "), solo until ready";
    } else if (st.rung_ever && Seconds(st.last_ring, now) < st.ring_every) {
        state = ps::State::RingingBell;
        detail = role == PartyRole::Host ? "Beckoning Bell" : "Small Resonant Bell";
    } else {
        state = role == PartyRole::Host ? ps::State::Hosting : ps::State::WaitingForWorld;
        detail = role == PartyRole::Host ? "ready; waiting for a ready member" : "ready; waiting for the host";
    }
    std::vector<ps::Member> members;
    const int me = link->local_slot();
    for (const party::RosterEntry& e : link->roster()) {
        ps::Member m;
        m.name = e.name;
        m.connected = e.connected;
        m.in_world = e.state == party::MemberState::InHostWorld;
        m.ping_ms = int(e.ping_ms);
        m.slot = e.slot;
        m.local = e.slot == me;
        m.area = std::string(party::member_state_name(e.state)) +
                 (e.map_id && e.map_id != 0xffffffffu ? " " + MapName(e.map_id) : std::string());
        members.push_back(m);
    }
    std::string sig = std::to_string(int(state)) + "|" + detail;
    for (const ps::Member& m : members) {
        sig += "|" + m.name + "," + std::to_string(m.connected) + "," + m.area;
    }
    if (sig == st.status_sig) {
        return;
    }
    st.status_sig = sig;
    ps::SetRole(role == PartyRole::Host ? ps::Role::Host : ps::Role::Guest);
    ps::SetState(state, detail);
    ps::SetMembers(members);
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
    std::lock_guard<std::mutex> lk(st.mu);
    if (const char* e = std::getenv("BB_PARTY_RING_EVERY"); e && e[0] && std::atof(e) >= 1.0) {
        st.ring_every = std::atof(e);
    }
    if (const char* a = std::getenv("BB_PARTY_AUTO"); a && a[0] == '0') {
        st.auto_ring = false;
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
            } else if (item == "grant_bells") {
                st.test.grant_bells = true;
            } else if (item == "ring_host") {
                st.test.ring = kHostBell;
            } else if (item == "ring_guest") {
                st.test.ring = kGuestBell;
            } else if (item.rfind("insight=", 0) == 0) {
                st.test.insight = std::atoi(item.c_str() + 8);
            } else if (!item.empty()) {
                Log("BB_PARTY_DIRECTOR_TEST: '%s' is not log_state, start, grant_bells, insight=N, ring_host or "
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
    if (full) {
        PublishStatus(st, s, role, link, now);
    }
    // B1: the host's warps go to every guest (reliable: replayed to a member who reconnects).
    if (link && role == PartyRole::Host) {
        TravelIntent t;
        while (PopHostTravel(&t)) {
            link->send_event(party::kBroadcast, kTravelEventName, TravelToJsonText(t));
            Log("travel #%llu (%s) sent to the party", static_cast<unsigned long long>(t.seq), TravelKindName(t.kind));
        }
    }
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
    return (p && p[0]) || (t && t[0]);
}

void CoopTick() {
    LuaEventsTick();
    PartyDirector::Get().Tick();
    TravelTick();        // B1 guest replay (party_travel.h)
    SeamlessRulesTick(); // A6 param rules, EMEVD filter stats (seamless_rules.h)
}

void PartyInit(unsigned char* image, std::uint64_t size) {
    if (!HooksInit(image, size)) {
        Log("no image; party director off");
        return;
    }
    LuaEventsInit();
    SeamlessRulesInit(); // A6: party patch report, EMEVD filter (seamless_rules.h)
    PartyDirector::Get().ConfigureFromEnv();
    InstallTravelPatches(); // B1: Dream gate + travel hooks (byte-verified)
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

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
#include "party_runtime.h"
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

party::MemberState LocalState(const GameSnapshot& s) {
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
    return party::MemberState::Home;
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

/// A party member (not us) is connected and not yet in a world with us.
bool MemberWaits(party::PartyLink* link) {
    for (const party::RosterEntry& e : link->roster()) {
        if (e.slot != party::kHostSlot && e.connected &&
            (e.state == party::MemberState::Home || e.state == party::MemberState::Joining)) {
            return true;
        }
    }
    return false;
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
    std::uint8_t online = 0xff, offline = 0xff;
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
    }
    char b[256];
    std::snprintf(b, sizeof b,
                  "FROM client: ss.info %s (status %d, failures %d, reload in %.0f s), user id %lld; FrpgNetMan online %d, "
                  "error msg 0x%x, server-offline %d",
                  ss ? "parsed" : "none", ss_status, fails, reload, static_cast<long long>(user), online, msg, offline);
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
    if (t.ring && !t.ring_done && Seconds(st.world_at, now) >= t.delay && s.world_up && !s.loading) {
        t.ring_done = true;
        t.rung_at = now;
        Log("test: before %s: %s", t.ring, Describe(s).c_str());
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
            } else if (item == "ring_host") {
                st.test.ring = kHostBell;
            } else if (item == "ring_guest") {
                st.test.ring = kGuestBell;
            } else if (item.rfind("insight=", 0) == 0) {
                st.test.insight = std::atoi(item.c_str() + 8);
            } else if (!item.empty()) {
                Log("BB_PARTY_DIRECTOR_TEST: '%s' is not log_state, insight=N, ring_host or ring_guest; ignored",
                    item.c_str());
            }
        }
        if (const char* d = std::getenv("BB_PARTY_DIRECTOR_TEST_DELAY"); d && d[0]) {
            st.test.delay = std::atof(d);
        }
    }
    Log("director: role %s, max %d players, %s every %.0f s%s%s", role == PartyRole::Host ? "host"
                                                                 : role == PartyRole::Guest ? "guest"
                                                                                            : "none",
        st.max_players, st.auto_ring ? "bell" : "no automatic bell", st.ring_every, st.test.on ? "; test: " : "",
        st.test.on ? std::getenv("BB_PARTY_DIRECTOR_TEST") : "");
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
    {
        std::lock_guard<std::mutex> lk(st.mu);
        role = st.role;
        link = st.link;
    }
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
        const party::MemberState ms = LocalState(s);
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
            Log("travel #%llu (%s) sent to the party", static_cast<unsigned long long>(t.seq), TravelKindName(t.kind));
        }
    }
    if (st.test.on) {
        RunTest(st, s, now);
        if (st.test.log_state && Seconds(st.last_log, now) >= 5.0) {
            const double secs = Seconds(st.last_log, now);
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
    if (role == PartyRole::Guest) {
        if (link->state() == party::LinkState::Connected) {
            Ring(st, kGuestBell, "guest, idle in its own world", now);
        }
    } else {
        int max_players;
        {
            std::lock_guard<std::mutex> lk(st.mu);
            max_players = st.max_players;
        }
        if (link->state() == party::LinkState::Hosting && s.cooperators >= 0 && s.cooperators < max_players - 1 &&
            MemberWaits(link)) {
            Ring(st, kHostBell, "host, a party member waits", now);
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
    fourp::FourpTick();  // 4-player rules: the party's max players (party_fourp.h)
}

void PartyInit(unsigned char* image, std::uint64_t size) {
    if (!HooksInit(image, size)) {
        Log("no image; party director off");
        return;
    }
    LuaEventsInit();
    SeamlessRulesInit(); // A6: party patch report, EMEVD filter (seamless_rules.h)
    fourp::FourpInit();  // 4-player parties: H1-H4, E6, P5 (party_fourp.h)
    PartyDirector::Get().ConfigureFromEnv();
    if (const char* t = std::getenv("BB_PARTY_TRACE_SSINFO"); t && t[0] == '1') {
        HookCallSite(kSsParseCall, kSsParse, reinterpret_cast<const void*>(&SsParseHook), "ss.info parse log");
    }
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

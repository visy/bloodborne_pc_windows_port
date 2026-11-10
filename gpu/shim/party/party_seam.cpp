// SPDX-License-Identifier: GPL-3.0-or-later
// 0.8.1 seamless join and party travel: the held / settled loading screen drop and the frame-end quiet point
// (party_seam.h, seam_rules.h; research/reseam081.md 3.1 / 3.2). The frame end / render thread quiet point was
// found with droogie/bbhost (GPL-3.0-or-later, src/engine/live_resolution.cpp at bdc5cdf); ported by intent.
#include "party_seam.h"

#include "coop_hooks.h"
#include "game_state.h"
#include "party_async_io.h"
#include "seam_rules.h"
#include "../bbport_settings.h"
#include "../bbport_stall_probe.h"
#include "../net/bbnet_internal.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <utility>

namespace coop::seam {

namespace {

using u64 = std::uint64_t;
using u8 = std::uint8_t;

// Our offsets (bbhost - 0x400000).
constexpr u64 kInGameStartClearCall = 0x193a755;  // InGameStart 0x193A3E0: call NowLoading clear (result unused)
constexpr u64 kNowLoadingClear = 0x176d3c0;       // request byte 0x556286B = 0, fade out (+0x804 = 3)
constexpr u64 kNowLoadingByte = 0x556286b;
constexpr u64 kWorldChrMan = 0x553e878;           // +0x60 the local PlayerIns (null while a map loads)
constexpr double kHoldCapS = 60.0;                // a hold never outlives this (the B2 phases end sooner)
constexpr double kQuietBudgetMs = 4.0;

std::atomic<bool> g_hook{false}, g_frame{false};
std::atomic<bool> g_hold{false};          // A
std::atomic<bool> g_pending{false};       // InGameStart's clear not run yet (held or settling)
double g_hold_at = 0, g_pending_at = 0;   // main thread (ms)
std::string g_hold_why;
SettleWatch g_watch;                      // B (main thread)
bool g_watch_for_hold = false;            // the settle watch started at a hold's release

struct Quiet {
    const char* name;
    std::function<void()> fn;
};
std::mutex g_qmu;
std::deque<Quiet> g_queue;

double NowMs() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    coop::asyncio::Print(std::string("[coop] Party seam: ") + line + "\n");
}

bool PartyProcess() { return bbnet::settings().party; }

bool QuietPointOn() {
    static const bool on = !(std::getenv("BB_PARTY_QUIET_POINT") && std::getenv("BB_PARTY_QUIET_POINT")[0] == '0');
    return on && g_frame.load();
}

u64 Slot(u64 off) {
    u64 v = 0;
    return SafeGet(Guest(off), &v) ? v : 0;
}

bool MapLoading() {
    u64 player = 0;
    const u64 wcm = Slot(kWorldChrMan);
    return !wcm || !SafeGet(wcm + 0x60, &player) || !player;
}

void Clear(const char* why) {
    const double now = NowMs();
    const double after = g_pending.load() ? now - g_pending_at : 0.0;
    g_pending = false;
    using Fn = u64(BB_COOP_SYSV*)();
    reinterpret_cast<Fn>(Guest(kNowLoadingClear))();
    Log("loading screen dropped (%s), %.0f ms after InGameStart", why, after);
}

/// The settle watch's end: the drop, at the quiet point (we are in it).
void SettledDrop(const char* how) {
    char why[160];
    std::snprintf(why, sizeof why, "%s: %d frame(s) in %.0f ms, worst %.0f ms%s", how, g_watch.frames,
                  g_watch.HeldMs(NowMs()), g_watch.worst_ms, g_watch_for_hold ? ", after the hold" : "");
    Clear(why);
}

BB_COOP_SYSV u64 InGameStartClearHook() {
    const double now = NowMs();
    if (g_hold.load()) {
        g_pending = true;
        g_pending_at = now;
        Log("InGameStart: the loading screen stays up (held: %s)", g_hold_why.c_str());
        return 0;
    }
    if (SettledDropOn() && g_frame.load()) {
        g_pending = true;
        g_pending_at = now;
        g_watch_for_hold = false;
        g_watch.Start(now);
        Log("InGameStart: the loading screen stays up until two frames under %.0f ms (at most %.0f ms)",
            kSettledFrameMs, kSettleCapMs);
        return 0;
    }
    using Fn = u64(BB_COOP_SYSV*)();
    const u64 r = reinterpret_cast<Fn>(Guest(kNowLoadingClear))();
    if (PartyProcess()) Log("InGameStart: loading screen dropped (vanilla)");
    return r;
}

void RunQueue(double budget_ms) {
    const double start = NowMs();
    for (;;) {
        Quiet q;
        {
            std::lock_guard<std::mutex> lk(g_qmu);
            if (g_queue.empty()) return;
            q = std::move(g_queue.front());
            g_queue.pop_front();
        }
        const double t0 = NowMs();
        q.fn();
        const double dt = NowMs() - t0;
        if (dt > 2.0) Log("quiet point: %s took %.1f ms", q.name, dt);
        if (NowMs() - start >= budget_ms) return;
    }
}

void OnFrameEnd(u64 prev_exit, u64 /*entry*/, u64 exit) {
    RunQueue(kQuietBudgetMs);
    if (!g_pending.load()) return;
    const double now = NowMs();
    u8 req = 0;
    if (SafeGet(Guest(kNowLoadingByte), &req) && !req) {
        g_pending = false;  // cleared by someone else meanwhile (FallbackToB1's drop, a vanilla path)
        g_watch.active = false;
        Log("held drop: the screen was cleared meanwhile; nothing to do");
        return;
    }
    if (MapLoading()) {
        // A new map load started under the held screen: its own InGameStart drops it.
        g_pending = false;
        g_watch.active = false;
        Log("held drop: a map load started under the screen; its InGameStart clears it");
        return;
    }
    if (g_hold.load()) {
        if (now - g_hold_at > kHoldCapS * 1000.0) {
            g_hold = false;
            Log("hold (%s): over %.0f s; the screen goes", g_hold_why.c_str(), kHoldCapS);
            Clear("hold cap");
        }
        return;
    }
    if (g_watch.active) {
        const double frame_ms = prev_exit ? double(exit - prev_exit) / 1e6 : -1.0;
        if (g_watch.Frame(frame_ms, now)) SettledDrop(g_watch.capped ? "cap" : "settled");
        return;
    }
    Clear("released at the quiet point");
}

}  // namespace

bool ParallelFollowOn() {
    return PartyProcess() && BbSettings::Get().party_parallel_follow.load(std::memory_order_relaxed);
}
bool SettledDropOn() {
    return PartyProcess() && BbSettings::Get().party_settled_drop.load(std::memory_order_relaxed);
}
bool HoldHookIn() { return g_hook.load(); }
bool Holding() { return g_hold.load(); }
bool ScreenHeld() { return g_hold.load() || g_pending.load(); }

void Hold(const char* why) {
    if (!g_hook.load()) return;
    g_hold_why = why ? why : "";
    g_hold_at = NowMs();
    g_hold = true;
    Log("hold: the loading screen stays up past InGameStart (%s)", g_hold_why.c_str());
}

bool ReleaseHold(const char* why, bool drop) {
    if (!g_hold.exchange(false)) return false;
    const bool was = g_pending.load();
    const double now = NowMs();
    Log("hold released (%s) after %.0f ms; %s", why, now - g_hold_at,
        !was ? "InGameStart had not run under it" : drop ? "the screen goes" : "the screen stays for the next load");
    if (!was) return false;
    if (!drop) {
        g_pending = false;
        return true;
    }
    if (SettledDropOn() && g_frame.load()) {
        g_watch_for_hold = true;
        g_watch.Start(now);
        return true;
    }
    if (g_frame.load()) return true;  // OnFrameEnd drops it at the next quiet point
    Clear(why);
    return true;
}

void AtQuietPoint(const char* name, std::function<void()> fn) {
    if (!QuietPointOn() || !SettledDropOn()) {
        fn();
        return;
    }
    std::lock_guard<std::mutex> lk(g_qmu);
    g_queue.push_back({name, std::move(fn)});
}

void Install() {
    if (std::getenv("BB_PARTY_SEAM_HOOKS") && std::getenv("BB_PARTY_SEAM_HOOKS")[0] == '0') {
        Log("hooks off (BB_PARTY_SEAM_HOOKS=0): no held / settled screen drop, no quiet point");
        return;
    }
    // "probe" in the names keeps both out of the party's hook-set check: the hooks are pass-through unless
    // this game's own (local) toggles use them, so two games may differ there.
    g_hook = HookCallSite(kInGameStartClearCall, kNowLoadingClear, reinterpret_cast<const void*>(&InGameStartClearHook),
                          "seam probe: InGameStart screen clear");
    g_frame = BbStallProbe::SetFrameEndCallback(&OnFrameEnd);
    Log("InGameStart clear hook %s, frame-end quiet point %s (party_parallel_follow / party_settled_drop)",
        g_hook.load() ? "in" : "NOT in (the code differs)", g_frame.load() ? "in" : "NOT in");
}

}  // namespace coop::seam

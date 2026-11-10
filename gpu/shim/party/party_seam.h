// SPDX-License-Identifier: GPL-3.0-or-later
// 0.8.1 seamless join and party travel (seam081; design research/reseam081.md 3).
//
// Two hooks, both pass-through unless a toggle uses them (BB_PARTY_SEAM_HOOKS=0: neither is installed):
//  - InGameStart's NowLoading clear (call 0x176D3C0 at 0x193A755, inside InGameStart 0x193A3E0): the loading
//    screen's drop can be held (A: until the guest stands beside the host) or deferred (B: until frames settle).
//  - the GX frame end (bbport_stall_probe.cpp's call-site hook at 0x1d97853): the quiet point after the frame
//    end returned (the render thread idle, the next frame not handed yet). The held drop and queued party
//    work run there.
// Toggles (My party: Loading, launcher My Party tab, bbport.ini; off by default):
//  A party_parallel_follow (BB_PARTY_PARALLEL_FOLLOW): the guest loads the host's party lamp at once (party_b2.cpp
//    GuestPhase::ParallelLoading), the screen held until it is placed beside the host.
//  B party_settled_drop (BB_PARTY_SETTLED_DROP): in a party, every InGameStart drop waits for two frames under 50 ms
//    in a row (at most 2 s) and runs at the quiet point; BB_PARTY_QUIET_POINT=0 runs the queued work in the tick.
#pragma once

#include <functional>

namespace coop::seam {

/// bbgpu_patch_image after coop::HooksInit (every mode; nothing runs unless a toggle is on).
void Install();

bool ParallelFollowOn();  ///< A's setting, party process only
bool SettledDropOn();     ///< B's setting, party process only
bool HoldHookIn();        ///< the InGameStart clear hook is in (the screen can be held)

/// A (main thread): the next / current loading screen stays up past InGameStart until ReleaseHold.
void Hold(const char* why);
/// Ends the hold. `drop`: the screen goes now (B on: once frames settled, at the quiet point). Not `drop`:
/// the hold is forgotten and the screen stays up for the caller's next load (whose InGameStart clears it).
/// Returns whether InGameStart's clear had been held (the load ended under the hold).
bool ReleaseHold(const char* why, bool drop);
bool Holding();
/// A drop is held or deferred right now (the party overlay stays hidden as under a loading screen).
bool ScreenHeld();

/// Runs `fn` on the main thread at the next frame end's quiet point (B on and the frame-end hook in), else at
/// once. In order; at most ~4 ms of them per frame (the rest the next frame).
void AtQuietPoint(const char* name, std::function<void()> fn);

}  // namespace coop::seam

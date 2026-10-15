// SPDX-License-Identifier: GPL-3.0-or-later
// Fast loading: while the game's loading screen is up, frames are not paced, so the loaders (which
// step once a frame) run as fast as the machine allows.
//
// Idea from droogie/bbhost (GPL-3.0-or-later, BBHOST_FAST_LOADING, src/engine/loading.cpp), the
// community's fast-load patch done natively; this is our own code on our present thread.
//
// A load is "up" when the NowLoading request byte 0x556286B is set and CSNowLoadingHelper
// (slot 0x553E8B8) +0x50 / +0x51 are clear (no in-game frame ran: game_state.h). While it is up,
// no Remo is queued or playing (SprjRemo 0x5540058 -> +8 -> +0x168 bit 0, +0xF8 queued count; a
// cutscene's timing is tied to the frame rate) and the frame-rate preset steps the game by the
// measured frame time (BB_FPS 60 / 90 / uncap: the FPS++ patches), the present thread
//   - flips each frame as soon as it arrives (no vblank tick, no BB_FPS_LIMIT slot), and
//   - sets SprjFlipper (slot 0x55404F8) +0x273 "no sleep this frame" (0x2034770 reads it before
//     its wait for the frame's target time and clears it at the end).
// Vanilla 30 FPS (BB_FPS=30 or BB_FPS_PATCH=0) is never touched: the game steps a fixed 1/30 s per
// frame there, so its timers (the network flow's, the loading screen's) would run faster.
//
// Opt-in (seam1 measurement 2026-10-16, 60 FPS preset, one session, A/B per load): loads were only
// 3-8 % shorter (host death 11.4 s vs 12.3 s, lamp travel 2.0-4.8 s vs 2.2-4.9 s; they are I/O and
// loader-thread bound here, not frame bound) and the game drew no frames while unpaced (its frame
// skip): the loading screen's animation stood still. Not worth a visible change by default.
//
// BB_FAST_LOADING: 1 on, 0 / unset off, `ab` / `ab:N` measurement mode: off for N loads, then on
// for N loads, and so on (N = 2 by default), so loads of the same kinds are compared in one session.
// Every load logs one line: `Fast loading: load N (on|off|n/a): S s, F frames (R per s)`.
#pragma once

#include <cstdlib>
#include <cstring>

namespace bbport::fastload {

/// Present thread, once per loop: reads the game's state (a few guarded reads) and returns true
/// while the current frame should not be paced.
bool Tick();
/// The last Tick's answer.
bool Active();
/// Present thread: a flip was made (counted while a load is up).
void NoteFlip();

/// Pure rule (unit-tested): may this load be uncapped?
struct Inputs {
    bool enabled = true;        ///< BB_FAST_LOADING (after the A/B pattern)
    bool loading = false;       ///< request byte set, no in-game frame
    bool remo = false;          ///< a cutscene queued or playing
    bool measured_steps = false;///< the preset steps by the measured frame time (60 / 90 / uncap)
};
inline bool Uncapped(const Inputs& in) { return in.enabled && in.loading && !in.remo && in.measured_steps; }

/// BB_FPS value -> the game steps by the measured frame time (60 / 90 / uncap with the FPS patch).
/// Unset (started without run.bat): the patches are unknown, so no.
inline bool MeasuredSteps(const char* bb_fps, const char* bb_fps_patch) {
    if (bb_fps_patch && (!std::strcmp(bb_fps_patch, "0") || !std::strcmp(bb_fps_patch, "off") ||
                         !std::strcmp(bb_fps_patch, "false"))) {
        return false;
    }
    if (!bb_fps || !*bb_fps) return false;
    return !std::strcmp(bb_fps, "60") || !std::strcmp(bb_fps, "90") || !std::strcmp(bb_fps, "uncap");
}

/// BB_FAST_LOADING parsed: mode 0 off, 1 on, 2 A/B with `period` loads per phase.
struct Mode {
    int mode = 0;
    int period = 2;
};
inline Mode ParseMode(const char* value) {
    Mode m;
    if (!value || !*value) return m;
    if (value[0] == '1' || !std::strcmp(value, "on")) {
        m.mode = 1;
    } else if (!std::strncmp(value, "ab", 2)) {
        m.mode = 2;
        if (value[2] == ':') {
            const int n = std::atoi(value + 3);
            if (n > 0) m.period = n;
        }
    }
    return m;
}
/// The A/B pattern: is load number `n` (0-based) one with fast loading?
inline bool AbOn(int n, int period) { return period > 0 && (n / period) % 2 == 1; }

}  // namespace bbport::fastload

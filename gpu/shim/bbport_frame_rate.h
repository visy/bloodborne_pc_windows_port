// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the game's frame rate set free by hooks in its code, not by patches: its frame pacer no
// longer waits for a 30 FPS frame, its step gets the frame's real time, and what it ties to a
// frame (a 30 FPS frame's time, a per-frame step, smoothing and damping) reads values the hooks
// recompute from that time every frame (bbport_frame_rate.cpp).
//
// Ported from deadinside's 0.6-pre1 (Supermedo/bloodborne_pc 72e5c8ce, GPL-2.0-or-later) to the
// Windows port: the near-image memory and page protection are Windows calls, the per-frame call
// keeps the flags and xmm registers, and BB_FRAME_HOOKS=0 keeps the community FPS++ patches
// (scripts/patches.py) as the fallback.
#pragma once

#include <cstdint>

namespace BbFrameRate {
/// Installs the hooks (at load, before the game runs: bbgpu_patch_image), when HooksWanted.
void PatchImage(unsigned char* image, std::uint64_t size);
/// Whether the hooks run the game's frame rate: the video out then shows a frame as soon as the
/// game submits it (the game's own flip rate asks for 30 FPS).
bool Unlocked();
/// Game time stepped so far (the sum of the steps' dt, seconds) and the number of steps.
/// Diagnostics (the frame-rate measurement): 0 when the hooks are off.
double GameSeconds();
std::uint64_t Steps();
} // namespace BbFrameRate

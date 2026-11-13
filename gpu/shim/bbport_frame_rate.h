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
#include <string>

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
/// The pacer's frame rate target from now on (30..MaxRate; diagnostics: the frame-rate sweep,
/// bbport_frame_sweep.cpp). False when the hooks are off.
bool SetTargetRate(int rate);
/// Live clock (Mrsuss60's a7cbfae6): the presenter's frame limit while live mode is requested (0 when it
/// is not; the target, 240 for uncapped; 60 when the eboot did not take the hook: the patch's static
/// rate). Read every present loop, so the menu's target changes it at once.
unsigned LivePresenterLimit();
/// True once after the live clock asked for a swapchain re-create (issue #35, bbport_live_refresh_logic.h): the presenter
/// does it at its next present.
bool TakeSwapchainRefresh();
/// Variable refresh rate (bbport.ini vrr, at start): the highest frame rate that stays inside the display's VRR
/// window (VrrFrameCap of its refresh rate); 0 = vrr off or the refresh rate unknown.
unsigned VrrCap();
/// `limit` (0 = none) lowered to VrrCap() when vrr is on; unchanged otherwise.
unsigned ApplyVrrCap(unsigned limit);
/// The window thread's reading of the display's refresh rate in Hz (0 = unknown).
void SetDisplayRefresh(float hz);
/// The live clock hook is installed and steps the game.
bool LiveActive();
/// Sound census (BB_SOUND_CENSUS / BB_SOUND_WAKE_PER_FRAME on): totals since start. False when it is off.
struct SoundCountsOut {
    std::uint64_t starts = 0, decoded = 0, frames = 0, passes = 0;
};
bool SoundCounts(SoundCountsOut* out);
/// Cloth fix diagnostics (GitHub #37, the sweep): the switches from a spec (bbport_frame_rate_logic.h ParseClothTune; false for
/// an unknown word), their name, the cloth data pointers the Verlet step has shown since ClothTrace(true), and the trace.
bool ClothSetFix(const char* spec);
std::string ClothFixName();
int ClothSeen(std::uint64_t* out, int max);
void ClothTrace(bool on);
/// The last step's dt (s; 0 when the hooks are off).
float LastDt();
/// Whether the frame in progress closes a whole 1/30 s of game time (30 such frames a game second at any rate; every
/// frame at 30 and while the hooks are off): the water wakes and the lights' blink step only on these, as at 30
/// (bbport_hifps.cpp, droogie/bbhost 57d2b9f, f24d18f).
bool WholeStep30();
/// The same with a step closed only once a whole 1/30 s has passed (not three quarters of it): for what counts the time
/// between steps, the lights' blink.
bool WholeStep30Exact();
/// Diagnostics (the sweep): the live / patched wind stub halves the step (on) or leaves it (off); the steps it saw summed (s)
/// and counted (needs BB_HIFPS_SWEEP or BB_HIFPS_LOG at start).
void SetWindPace(bool on);
void WindStats(double* sum, unsigned long long* calls);
} // namespace BbFrameRate

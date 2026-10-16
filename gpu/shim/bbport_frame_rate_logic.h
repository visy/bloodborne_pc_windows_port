// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the pure rules of the frame rate by hooks (bbport_frame_rate.cpp): which mode runs, and
// the per-frame values the redirected constants read. The values are deadinside's
// (Supermedo/bloodborne_pc 72e5c8ce, gpu/shim/bbport_frame_rate.cpp FrameStep), split out here
// so they can be unit-tested (ninja -C out/gpu frame-rate-test).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace BbFrameRate {

constexpr float Frame30 = 1.0f / 30.0f;
/// The highest frame rate the pacer allows ("uncap", and the most BB_FPS_LIMIT can ask for).
/// Upstream: on loading screens at 880-1000 FPS the main loop outran its worker threads (a pool
/// allocator popped a freed node); at 480 all loads went through. Our uncapped vblank is 480 Hz too.
constexpr int MaxRate = 480;

/// BB_FRAME_HOOKS default: the hooks (1) or the community FPS++ patches (0). scripts/patches.py
/// FRAME_HOOKS_DEFAULT must say the same.
constexpr bool HooksDefault = true;

/// Is the frame rate run by the hooks? BB_FPS (run.bat: uncap / 60 / 90 / 30; unset = started
/// without run.bat, no frame-rate choice: off), BB_FPS_PATCH (launcher "FPS patch": off = the game's
/// own 30 FPS, no patches and no hooks), BB_FRAME_HOOKS (0 = the FPS++ patches instead).
inline bool HooksWanted(const char* bb_fps, const char* bb_fps_patch, const char* bb_frame_hooks) {
    auto off = [](const char* v) {
        return v && (!std::strcmp(v, "0") || !std::strcmp(v, "off") || !std::strcmp(v, "false"));
    };
    if (!bb_fps || !*bb_fps || !std::strcmp(bb_fps, "30")) return false;
    if (off(bb_fps_patch)) return false;
    if (bb_frame_hooks && *bb_frame_hooks) return !off(bb_frame_hooks);
    return HooksDefault;
}

/// The pacer's shortest frame: BB_FPS (uncap = MaxRate, 60, 90, or any number) and BB_FPS_LIMIT
/// (the launcher's limit), clamped to 30..MaxRate.
inline int TargetRate(const char* bb_fps, const char* bb_fps_limit) {
    int rate = !bb_fps || !std::strcmp(bb_fps, "uncap") ? MaxRate : std::atoi(bb_fps);
    rate = std::clamp(rate <= 0 ? MaxRate : rate, 30, MaxRate);
    if (bb_fps_limit && std::atoi(bb_fps_limit) > 0) {
        rate = std::min(rate, std::clamp(std::atoi(bb_fps_limit), 30, MaxRate));
    }
    return rate;
}

/// What the redirected instructions read, rewritten for each frame (FrameStep). The layout is
/// read by the game's code (offsetof in the redirect table; decay95 as one 16-byte vector).
struct alignas(64) Live {
    float dt = Frame30;               ///< the frame's time: the game steps by it
    float shortest = 1.0f / MaxRate;  ///< the pacer's frame time target
    float pad[2] = {};
    float decay95[4] = {0.95f, 0.95f, 0.95f, 0.95f}; ///< 0.95 per 30 FPS frame (read as four)
    float approach30 = Frame30;       ///< a 1/30 approach per 30 FPS frame
    float approach10 = 0.1f;          ///< a 0.1 approach per 30 FPS frame
    float step_down = -0.1f;          ///< -0.1 per 30 FPS frame
    float step_up = 0.1f;             ///< +0.1 per 30 FPS frame
    float decay08 = 0.8f;             ///< 0.8 per 30 FPS frame
    float cloth = 1.0f;               ///< cloth constraints' stiffness
};

/// The step for a measured frame time: clamped to [shortest, 1/30 s] (below 30 FPS the game slows
/// down, as it always did); a non-finite measurement counts as 1/30 s.
inline float StepFor(float measured, float shortest) {
    return std::clamp(std::isfinite(measured) ? measured : Frame30, shortest, Frame30);
}

/// The values for a frame of `dt` seconds. `cloth_override` > 0: BB_CLOTH_STIFFNESS.
inline void Compute(Live& live, float dt, float cloth_override = -1.0f) {
    const float frames = dt * 30.0f; // the frame's length in 30 FPS frames
    live.dt = dt;
    const float decay95 = std::pow(0.95f, frames);
    for (float& d : live.decay95) d = decay95;
    live.approach30 = 1.0f - std::pow(1.0f - Frame30, frames);
    live.approach10 = 1.0f - std::pow(0.9f, frames);
    live.step_down = -0.1f * frames;
    live.step_up = 0.1f * frames;
    live.decay08 = std::pow(0.8f, frames);
    // Cloth is stiffer the more often it is solved: the game's 1.0 at 30 FPS, 0.5 from 60 FPS up
    // (the value the community's 60 FPS patches settled on).
    live.cloth = cloth_override > 0.0f ? cloth_override : std::clamp(frames, 0.5f, 1.0f);
}

} // namespace BbFrameRate

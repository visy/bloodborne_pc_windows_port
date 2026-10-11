// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the game camera of the last finished frame, for the overlay (party HUD name tags and
// direction arrows). Published by CameraMotion::CommitFrameCamera (render thread, once a frame,
// from the scene constants: docs/upscaler.md), read by the overlay (present thread). A plain copy
// under a mutex both sides hold for a few dozen bytes.

#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>

namespace BbCamera {

struct State {
    bool valid = false;
    std::array<float, 12> view{}; ///< world to view, 3x4 rows (view z > 0 in front)
    std::array<float, 4> proj{};  ///< x scale, y scale (signed: ndc +y down the screen), z scale, z offset
    std::int64_t at_ms = 0;       ///< steady clock of the publish
};

inline std::mutex mutex;
inline State state;

inline std::int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

inline void Publish(const std::array<float, 12>& view, const std::array<float, 4>& proj) {
    std::lock_guard<std::mutex> lk(mutex);
    state.valid = proj[0] != 0.0f && proj[1] != 0.0f;
    state.view = view;
    state.proj = proj;
    state.at_ms = NowMs();
}

/// The latest camera; invalid when none was published in the last `max_age_ms`.
inline State Latest(std::int64_t max_age_ms = 250) {
    std::lock_guard<std::mutex> lk(mutex);
    State s = state;
    if (NowMs() - s.at_ms > max_age_ms) s.valid = false;
    return s;
}

/// The camera's world position (-R^T t).
inline std::array<float, 3> Position(const State& s) {
    const auto& v = s.view;
    return {-(v[0] * v[3] + v[4] * v[7] + v[8] * v[11]), -(v[1] * v[3] + v[5] * v[7] + v[9] * v[11]),
            -(v[2] * v[3] + v[6] * v[7] + v[10] * v[11])};
}

} // namespace BbCamera

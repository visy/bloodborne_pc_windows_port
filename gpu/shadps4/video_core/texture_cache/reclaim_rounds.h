// SPDX-FileCopyrightText: Copyright 2026 bbport
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace VideoCore {

/// bbport: out of GPU memory for a new image, past the driver's budget too (4 GB cards on
/// entering the Hunter's Dream): unused textures go, more each round, and the image is tried
/// again after each (from Supermedo/bloodborne_pc 5f56ed6a).
inline constexpr std::array<std::uint64_t, 3> kReclaimRounds{
    std::uint64_t(64) << 20, std::uint64_t(256) << 20, std::uint64_t(1024) << 20};

/// Each round asks `reclaim(bytes)` for at least `min_bytes` (twice the image, say) and then
/// `retry()`; true once a retry succeeds. `freed` adds up what the rounds freed.
template <typename Reclaim, typename Retry>
bool ReclaimAndRetry(std::uint64_t min_bytes, Reclaim&& reclaim, Retry&& retry,
                     std::uint64_t& freed) {
    for (const std::uint64_t round : kReclaimRounds) {
        freed += reclaim(std::max(round, min_bytes));
        if (retry()) {
            return true;
        }
    }
    return false;
}

} // namespace VideoCore

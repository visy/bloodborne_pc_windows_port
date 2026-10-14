// SPDX-License-Identifier: GPL-3.0-or-later
// bbport: the pure decisions of bbport_stability.cpp (unit-tested in tests/test_stability_logic.cpp).
#pragma once

#include <cstdint>
#include <cstdlib>

namespace BbStability {

/// The game's per-frame linear allocator behind each frame context's command arenas: 256 KiB.
constexpr std::uint64_t kFrameArenaStock = 0x40000;
/// Ours unless BB_FRAME_ARENA_KIB says otherwise: 1 MiB (the two buffers take 1.5 MiB more).
constexpr std::uint64_t kFrameArenaDefault = 0x100000;

/// The frame allocator's size in bytes for BB_FRAME_ARENA_KIB = `env`: unset or not a number:
/// the default; 0: the game's own 256 KiB; else that many KiB, 256 KiB .. 16 MiB, in 64 KiB steps.
inline std::uint64_t FrameArenaBytes(const char* env) {
    if (!env || !*env) {
        return kFrameArenaDefault;
    }
    char* end = nullptr;
    const unsigned long long kib = std::strtoull(env, &end, 10);
    if (end == env || *end) {
        return kFrameArenaDefault;
    }
    if (kib == 0) {
        return kFrameArenaStock;
    }
    std::uint64_t bytes = kib > 16384 ? 16384ull << 10 : kib << 10;
    bytes = bytes < kFrameArenaStock ? kFrameArenaStock : bytes;
    return bytes & ~std::uint64_t(0xffff);
}

/// The frame allocator's fill in eighths (0..8): a log line each time a frame reaches a new one.
inline unsigned FillEighths(std::uint64_t used, std::uint64_t capacity) {
    if (capacity == 0) {
        return 0;
    }
    const std::uint64_t e = used >= capacity ? 8 : used * 8 / capacity;
    return unsigned(e);
}

/// A new fullest frame worth a log line: 64 KiB or more, and a quarter more than the last one logged.
inline bool HighWaterNews(std::uint64_t used, std::uint64_t logged) {
    return used >= 0x10000 && used >= logged + logged / 4 && used > logged;
}

/// BB_ARENA_FAIL_TEST=N: the n-th pass (0-based) through the guarded allocation is made to fail
/// when it is the N-th of its group.
inline bool FailTestHit(std::uint64_t n, std::uint64_t every) {
    return every != 0 && n % every == every - 1;
}

} // namespace BbStability

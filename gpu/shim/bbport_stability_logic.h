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

// ---------------------------------------------------------------------------------------------
// The GX range allocator size (BB_GX_REGION_MB). Ported from Nightwatch c6305d7 (engine/start/
// patches.cpp), Lantern Engine by Awaken (https://github.com/Awakenchan/LanternEngine); modified
// (a pure decision here, applied by GxRegion() in bbport_stability.cpp). The game's range
// allocator for CPU-updated GPU buffers is the GX manager's 128 MiB plus 32 MiB, carved at start
// from GFX_GraphicsPrivate; Ebrietas' second phase drives it empty (device lost).
// ---------------------------------------------------------------------------------------------
constexpr unsigned kGxRegionGame = 160, kGxRegionDefault = 288, kGxRegionMax = 416; // MiB

/// The size asked for by BB_GX_REGION_MB's text (null/empty/not digits: the default).
inline unsigned GxRegionRequest(const char* env) {
    if (!env || !*env) {
        return kGxRegionDefault;
    }
    unsigned long long v = 0;
    for (const char* c = env; *c; ++c) {
        if (*c < '0' || *c > '9') {
            return kGxRegionDefault;
        }
        v = v * 10 + unsigned(*c - '0');
        if (v > 100000) {
            v = 100000;
        }
    }
    return unsigned(v);
}

/// The size to apply in MiB: 0 when the game's own (request <= 160) is kept, else the request capped.
inline unsigned GxRegionApplied(unsigned request) {
    if (request <= kGxRegionGame) {
        return 0;
    }
    return request > kGxRegionMax ? kGxRegionMax : request;
}

/// The immediate the allocator's size is written as: the GX manager's default 128 MiB plus the growth.
inline std::uint32_t GxRegionImmediate(unsigned mib) {
    return std::uint32_t(mib - kGxRegionGame + 128) << 20;
}

/// MiB GFX_GraphicsPrivate must grow by for the region: the growth itself, unless the heap is already
/// enlarged (a resolution growth or the Increased Graphics Heap Sizes patch: it holds the region),
/// so nothing grows twice.
inline std::uint64_t GxRegionHeapMore(unsigned mib, bool heap_stock) {
    return heap_stock ? mib - kGxRegionGame : 0;
}

// ---------------------------------------------------------------------------------------------
// BB_FAST_RESPAWN_SECONDS: the hold timer of a death respawn's loading screen (the game's fixed 12.0 s).
// ---------------------------------------------------------------------------------------------
constexpr float kRespawnGame = 12.0f, kRespawnDefault = 2.0f, kRespawnMin = 0.5f;

/// Seconds from the env text: null/empty/not a number: the default; clamped to 0.5 .. 12.
inline float RespawnSeconds(const char* env) {
    if (!env || !*env) {
        return kRespawnDefault;
    }
    char* end = nullptr;
    const double v = std::strtod(env, &end);
    if (end == env || *end || !(v == v)) {
        return kRespawnDefault;
    }
    return v < kRespawnMin ? kRespawnMin : v > kRespawnGame ? kRespawnGame : float(v);
}

} // namespace BbStability

// SPDX-License-Identifier: GPL-3.0-or-later
// The pure decisions of gpu/shim/bbport_stability.cpp (frame allocator size, fill, fail test).
// Build and run: ninja -C out/gpu stability-logic-test && out/gpu/stability-logic-test.exe
#include "bbport_stability_logic.h"

#include <cstdio>

using namespace BbStability;

static int g_failures = 0, g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_failures; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

int main() {
    // BB_FRAME_ARENA_KIB
    CHECK(FrameArenaBytes(nullptr) == kFrameArenaDefault);
    CHECK(FrameArenaBytes("") == kFrameArenaDefault);
    CHECK(FrameArenaBytes("abc") == kFrameArenaDefault);
    CHECK(FrameArenaBytes("12x") == kFrameArenaDefault);
    CHECK(FrameArenaBytes("0") == kFrameArenaStock);
    CHECK(FrameArenaBytes("256") == 0x40000);
    CHECK(FrameArenaBytes("100") == 0x40000);            // below the game's own: the game's own
    CHECK(FrameArenaBytes("2048") == 0x200000);
    CHECK(FrameArenaBytes("1000") == (1000ull << 10 & ~0xffffull)); // 64 KiB steps
    CHECK(FrameArenaBytes("99999999") == 16384ull << 10);
    // Fill in eighths
    CHECK(FillEighths(0, 0x40000) == 0);
    CHECK(FillEighths(0x7fff, 0x40000) == 0);
    CHECK(FillEighths(0x8000, 0x40000) == 1);
    CHECK(FillEighths(0x3ffff, 0x40000) == 7);
    CHECK(FillEighths(0x40000, 0x40000) == 8);
    CHECK(FillEighths(0x50000, 0x40000) == 8);
    CHECK(FillEighths(5, 0) == 0);
    // High-water log lines
    CHECK(!HighWaterNews(0xffff, 0) && HighWaterNews(0x10000, 0));
    CHECK(!HighWaterNews(0x13fff, 0x10000) && HighWaterNews(0x14000, 0x10000));
    CHECK(!HighWaterNews(0x10000, 0x10000));
    // BB_ARENA_FAIL_TEST
    CHECK(!FailTestHit(0, 0) && !FailTestHit(5, 0));
    CHECK(FailTestHit(0, 1) && FailTestHit(7, 1));
    CHECK(!FailTestHit(0, 3) && !FailTestHit(1, 3) && FailTestHit(2, 3) && FailTestHit(5, 3));
    std::printf("stability-logic-test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

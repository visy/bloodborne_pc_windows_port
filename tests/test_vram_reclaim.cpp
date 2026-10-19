// SPDX-License-Identifier: GPL-2.0-or-later
// Standalone CPU checks of the out-of-VRAM reclaim rounds (texture_cache/reclaim_rounds.h).
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>
#include "gpu/shadps4/video_core/texture_cache/reclaim_rounds.h"

using VideoCore::ReclaimAndRetry;
constexpr std::uint64_t MiB = std::uint64_t(1) << 20;

int main() {
    // Memory turns up in the second round: two rounds asked for, 64 then 256 MiB.
    std::vector<std::uint64_t> asked;
    std::uint64_t freed = 0;
    int retries = 0;
    bool ok = ReclaimAndRetry(
        0, [&](std::uint64_t want) { asked.push_back(want); return want / 2; },
        [&] { return ++retries == 2; }, freed);
    assert(ok && retries == 2 && asked.size() == 2);
    assert(asked[0] == 64 * MiB && asked[1] == 256 * MiB && freed == 160 * MiB);

    // Never enough: three rounds (64, 256, 1024 MiB), then given up on.
    asked.clear();
    freed = 0;
    retries = 0;
    ok = ReclaimAndRetry(0, [&](std::uint64_t want) { asked.push_back(want); return std::uint64_t(0); },
                         [&] { ++retries; return false; }, freed);
    assert(!ok && retries == 3 && asked.size() == 3 && asked[2] == 1024 * MiB && freed == 0);

    // A big image asks for at least twice its size every round.
    asked.clear();
    freed = 0;
    ok = ReclaimAndRetry(300 * MiB, [&](std::uint64_t want) { asked.push_back(want); return want; },
                         [&] { return asked.size() == 3; }, freed);
    assert(ok && asked[0] == 300 * MiB && asked[1] == 300 * MiB && asked[2] == 1024 * MiB);
    assert(freed == 1624 * MiB);

    // The first retry succeeding stops after one round.
    asked.clear();
    freed = 0;
    ok = ReclaimAndRetry(0, [&](std::uint64_t want) { asked.push_back(want); return want; },
                         [] { return true; }, freed);
    assert(ok && asked.size() == 1 && freed == 64 * MiB);

    std::puts("vram-reclaim-test: ok");
    return 0;
}

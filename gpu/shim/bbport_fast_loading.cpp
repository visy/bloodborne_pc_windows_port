// SPDX-License-Identifier: GPL-3.0-or-later
// Fast loading (bbport_fast_loading.h). Idea: droogie/bbhost BBHOST_FAST_LOADING (GPL-3.0+).
#include "bbport_fast_loading.h"

#include "party/coop_hooks.h"
#include "party/game_state.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace bbport::fastload {

namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using Clock = std::chrono::steady_clock;

constexpr u64 kNowLoadingRequested = 0x556286b;  // u8
constexpr u64 kNowLoadingHelper = 0x553e8b8;     // CSNowLoadingHelper*
constexpr u64 kSprjRemo = 0x5540058;             // SprjRemo* (+8 the remo manager)
constexpr u64 kSprjFlipper = 0x55404f8;          // SprjFlipper* (Update 0x2034770)
constexpr u64 kFlipperNoSleep = 0x273;           // u8: no wait this frame (cleared by Update)

std::atomic<bool> g_active{false};
std::atomic<u64> g_flips{0};

template <class T>
T Read(u64 address, T fallback) {
    T v{};
    return address && coop::SafeGet(address, &v) ? v : fallback;
}
u64 Slot(u64 off) {
    const u64 at = coop::Guest(off);
    return at ? Read<u64>(at, 0) : 0;
}

struct Load {
    bool up = false;
    bool fast = false;     // decided at the load's start (A/B)
    bool eligible = false; // the preset allows it
    bool remo_seen = false;
    Clock::time_point start{};
    u64 flips0 = 0;
    int number = 0;        // loads seen so far
};

}  // namespace

bool Active() { return g_active.load(std::memory_order_relaxed); }

void NoteFlip() { g_flips.fetch_add(1, std::memory_order_relaxed); }

bool Tick() {
    static const Mode mode = ParseMode(std::getenv("BB_FAST_LOADING"));
    static const bool measured = MeasuredSteps(std::getenv("BB_FPS"), std::getenv("BB_FPS_PATCH"));
    static bool announced = false;
    static Load ld;
    if (!coop::Image()) return false;
    if (!announced) {
        announced = true;
        std::printf("Fast loading: %s (BB_FAST_LOADING=%s, frame-rate preset %s)\n",
                    !measured      ? "n/a for this frame-rate preset (fixed 1/30 s steps)"
                    : mode.mode == 0 ? "off"
                    : mode.mode == 2 ? "A/B measurement"
                                     : "on",
                    std::getenv("BB_FAST_LOADING") ? std::getenv("BB_FAST_LOADING") : "(default 1)",
                    std::getenv("BB_FPS") ? std::getenv("BB_FPS") : "(unset)");
        std::fflush(stdout);
    }
    const bool requested = Read<u8>(coop::Guest(kNowLoadingRequested), 0) != 0;
    const u64 helper = Slot(kNowLoadingHelper);
    const bool in_game = helper && (Read<u8>(helper + 0x50, 0) || Read<u8>(helper + 0x51, 0));
    const bool loading = requested && !in_game;
    bool remo = false;
    if (const u64 remo_obj = Slot(kSprjRemo)) {
        if (const u64 man = Read<u64>(remo_obj + 8, 0)) {
            remo = (Read<u32>(man + 0x168, 0) & 1) != 0 || Read<u64>(man + 0xf8, 0) != 0;
        }
    }
    const auto now = Clock::now();
    if (loading && !ld.up) {
        ld.up = true;
        ld.eligible = measured;
        ld.fast = mode.mode == 1 || (mode.mode == 2 && AbOn(ld.number, mode.period));
        ld.remo_seen = false;
        ld.start = now;
        ld.flips0 = g_flips.load(std::memory_order_relaxed);
    } else if (!loading && ld.up) {
        ld.up = false;
        const double s = std::chrono::duration<double>(now - ld.start).count();
        const u64 frames = g_flips.load(std::memory_order_relaxed) - ld.flips0;
        std::printf("Fast loading: load %d (%s): %.2f s, %llu frames (%.0f per s)%s\n", ld.number + 1,
                    !ld.eligible ? "n/a" : ld.fast ? "on" : "off", s, static_cast<unsigned long long>(frames),
                    s > 0 ? double(frames) / s : 0.0, ld.remo_seen ? ", a cutscene kept it paced" : "");
        std::fflush(stdout);
        ++ld.number;
    }
    if (loading && remo) ld.remo_seen = true;
    Inputs in;
    in.enabled = ld.fast;
    in.loading = loading;
    in.remo = remo;
    in.measured_steps = measured;
    const bool fast = Uncapped(in);
    if (fast) {
        // No sleep in SprjFlipper::Update this frame (it clears the byte itself).
        if (const u64 flipper = Slot(kSprjFlipper)) {
            const u8 one = 1;
            coop::SafeWrite(flipper + kFlipperNoSleep, &one, 1);
        }
    }
    g_active.store(fast, std::memory_order_relaxed);
    return fast;
}

}  // namespace bbport::fastload

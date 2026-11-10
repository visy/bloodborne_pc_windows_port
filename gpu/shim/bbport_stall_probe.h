// SPDX-License-Identifier: GPL-3.0-or-later
// Stall probe (BB_STALL_PROBE=1 or =<ms>, off by default): for every main-loop frame longer than
// the threshold (250 ms by default) one "Stall probe:" line saying where the time went: the main
// thread's own work vs. waiting in the frame end for the game's render thread, and where each of
// those threads was (sampled every ms while the frame runs long). Rules and the frame-end / render
// thread background: bbport_stall_probe_rules.h.
#pragma once

#include <atomic>

namespace BbStallProbe {

/// What the main thread runs now, for the samples (set by our own per-frame code).
enum Zone : int { kZoneGame = 0, kZonePartyTick = 1, kZoneCount };
extern std::atomic<int> g_zone;
extern bool g_on;

/// Marks a section of the main thread as ours (CoopTick). One relaxed store each way.
class ZoneScope {
public:
    explicit ZoneScope(Zone z) : prev_(g_zone.exchange(z, std::memory_order_relaxed)) {}
    ~ZoneScope() { g_zone.store(prev_, std::memory_order_relaxed); }
    ZoneScope(const ZoneScope&) = delete;
    ZoneScope& operator=(const ZoneScope&) = delete;

private:
    int prev_;
};

/// bbgpu_patch_image, after coop::HooksInit: reads BB_STALL_PROBE, installs the two call-site hooks
/// (frame end 0x1d97853, render-thread done 0x21b6a68) and starts the sampler. Nothing when off.
void Install();

} // namespace BbStallProbe

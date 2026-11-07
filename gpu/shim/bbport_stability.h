// SPDX-License-Identifier: GPL-3.0-or-later
// bbport: long-session stability fixes in the game's code, most of them from droogie/bbhost
// (GPL-3.0-or-later) - see bbport_stability.cpp. Each site is checked byte for byte first; a site
// that differs is left alone with a log line. Each fix has a BB_* switch (default on).
#pragma once

#include <cstdint>

namespace BbStability {
/// Patches the loaded image (after relocations and patches.bin, before the game runs).
void PatchImage(unsigned char* image, std::uint64_t size);
/// One "CPU save:" line with the counters of the job barrier steal and the work-pool node cache
/// (nothing when neither is installed; BB_LIVE_FPS_LOG calls it once a second).
void LogCpuSave();
} // namespace BbStability

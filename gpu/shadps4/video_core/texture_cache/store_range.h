// SPDX-License-Identifier: GPL-2.0-or-later
// Part of bbport. The logic is ported from Nightwatch (NW 403ba62), whose engine is based on
// "Lantern Engine by Awaken (https://github.com/Awakenchan/LanternEngine)", GPL-3 with the GPL
// section 7 additional terms of its NOTICE (kept in licenses/). MODIFIED for bbport: reduced to a
// pure function so it can be tested; the texture cache calls it.
#pragma once

#include <algorithm>
#include <cstdint>
#include <utility>

namespace VideoCore {

/// The part of a GPU-written image's guest memory that is written back: the pages the image still
/// watches. A guest write beside the image on its first or last page (the game's heap header in
/// front of it) ends the watch of that page (UntrackImageHead / UntrackImageTail); once the game
/// freed that block its heap keeps the free list in those bytes, and writing the whole image back
/// put pixels over it (crash in the game's heap after a death and a reload). Those pages keep the
/// game's bytes. An image that is not tracked at all (the game rewrote it: CPU dirty) is written
/// whole, as before. Returns {address, size}; size 0 = nothing to write.
inline std::pair<std::uint64_t, std::uint64_t> WatchedStoreRange(
    std::uint64_t address, std::uint64_t size, bool tracked, std::uint64_t track_addr,
    std::uint64_t track_addr_end, bool enabled = true) {
    if (!enabled || !tracked) {
        return {address, size};
    }
    const std::uint64_t begin = std::max(address, track_addr);
    const std::uint64_t end = std::min(address + size, track_addr_end);
    if (begin >= end) {
        return {address, 0};
    }
    return {begin, end - begin};
}

} // namespace VideoCore

// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: what a texture format the GPU has no Vulkan format for turns into, kept free of Vulkan
// types so a unit test can check it (tests/test_surface_format_rules.cpp).
//
// Supermedo/bloodborne_pc 5a09b8ff: a stale descriptor can name a data/number format pair that GCN
// has no format for (32_32_32_32 Snorm); the game stopped on it. The same data format with another
// number format has the same size and layout, so the image keeps working.
#pragma once

#include <cstddef>

namespace BbSurfaceFormat {

/// Tries `lookup(candidate)` for each candidate in order (Uint, Unorm, Float as the caller passes
/// them); returns the first result that is not `undefined`, else `undefined`.
template <typename Format, typename Number, typename Lookup>
Format FirstDefined(const Number* candidates, size_t count, Format undefined, Lookup&& lookup) {
    for (size_t i = 0; i < count; ++i) {
        const Format format = lookup(candidates[i]);
        if (format != undefined) {
            return format;
        }
    }
    return undefined;
}

} // namespace BbSurfaceFormat

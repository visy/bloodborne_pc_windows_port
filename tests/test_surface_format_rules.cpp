// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: an unknown texture format falls back to a same-size one (surface_format_rules.h).
#include <cassert>
#include <cstdio>
#include "gpu/shadps4/video_core/renderer_vulkan/surface_format_rules.h"

namespace {
enum class Num { Unorm, Snorm, Uint, Sint, Float };
constexpr int kUndefined = 0;
} // namespace

int main() {
    const Num order[] = {Num::Uint, Num::Unorm, Num::Float};
    // 32_32_32_32 has Uint and Float but no Snorm: the first defined candidate (Uint) is taken.
    auto rgba32 = [](Num n) { return n == Num::Uint ? 107 : n == Num::Float ? 109 : kUndefined; };
    assert(BbSurfaceFormat::FirstDefined(order, 3, kUndefined, rgba32) == 107);
    // Only Float exists.
    auto only_float = [](Num n) { return n == Num::Float ? 5 : kUndefined; };
    assert(BbSurfaceFormat::FirstDefined(order, 3, kUndefined, only_float) == 5);
    // Unorm before Float.
    auto unorm_float = [](Num n) { return n == Num::Unorm ? 3 : n == Num::Float ? 5 : kUndefined; };
    assert(BbSurfaceFormat::FirstDefined(order, 3, kUndefined, unorm_float) == 3);
    // Nothing: undefined comes back (the caller then reports it).
    auto none = [](Num) { return kUndefined; };
    assert(BbSurfaceFormat::FirstDefined(order, 3, kUndefined, none) == kUndefined);
    assert(BbSurfaceFormat::FirstDefined(order, 0, kUndefined, rgba32) == kUndefined);
    std::puts("surface format rules ok");
    return 0;
}

// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: motion blur resolution (shipped in 0.6.2 as "Volumetric effects resolution"; idea from
// a playtester). The one reduced-resolution chain of Bloodborne's frame turned out to be its
// motion blur, not smoke or mist (those are drawn blended into the full-size scene): a
// depth/stencil target half the scene depth's size (downsampled from the scene's linear depth by
// a full-screen pass), an RGBA8 velocity target drawn with it by the moving characters and
// objects (RG = screen velocity around 0.5, alpha = covered; 0.6.3 dumps of it), two dilation
// passes and a pass building a half-size RGBA16F velocity/depth buffer, which two full-size
// gather passes sample to blur the scene. With "Disable Motion Blur" the chain is not drawn. At
// high output resolutions its half size makes the blur's edges around moving characters blocky.
// The option renders exactly that chain into
// larger proxies (SceneTargets): the game keeps its half-size allocations, the passes draw into
// proxies of 2x the pixels or of the scene's size, with viewport / scissor scaled and FragCoord
// converted back to the game's coordinates (push constant scene_size), and the composite passes
// sample the proxies with their normalized coordinates. Anything else reading these images
// natively (compute, integer loads, copies) gets them resolved back to the game's size.
//
// This header holds the pure rules (unit-tested in tests/test_scene_resolution.cpp).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "video_core/renderer_vulkan/scene_resolution.h"

namespace Vulkan::VolumetricResolution {

/// bbport.ini volumetric_res / BB_VOLUMETRIC_RES / menus.
enum Level : int { Game = 0, Double = 1, Full = 2, LevelCount };

inline int ClampLevel(int level) {
    return std::clamp(level, 0, int(LevelCount) - 1);
}

/// Linear factor per axis over the game's half-size buffer: 1 (game), 7/5 (1.96x the pixels),
/// 2 (the scene's size). 7/5 keeps the FragCoord conversion exact for the usual sizes.
struct Factor {
    uint32_t num = 1, den = 1;
};
inline Factor FactorOf(int level) {
    switch (ClampLevel(level)) {
    case Double:
        return {7, 5};
    case Full:
        return {2, 1};
    default:
        return {1, 1};
    }
}

/// The proxy size of a volumetric image whose unscaled size (the game's, or the live path's
/// half-resolution proxy size) is `base`.
inline SceneResolution::Size Scaled(SceneResolution::Size base, int level) {
    const Factor f = FactorOf(level);
    const auto scale = [&](uint32_t v) {
        return std::max<uint32_t>(1, uint32_t((uint64_t(v) * f.num + f.den / 2) / f.den));
    };
    return {scale(base.width), scale(base.height)};
}

/// The push constant scene_size of a pass drawing into proxies of size `proxy` that stand for
/// images of the game's size `guest`: the shaders convert FragCoord with 1920 / width and
/// 1080 / height (emit_spirv_context_get_set.cpp), which must equal guest / proxy.
inline uint32_t PackFragScale(SceneResolution::Size proxy, SceneResolution::Size guest) {
    const auto dim = [](uint32_t native, uint32_t p, uint32_t g) {
        return uint32_t(std::clamp<uint64_t>((uint64_t(native) * p + g / 2) / std::max(1u, g), 1, 65535));
    };
    return SceneResolution::Pack({dim(1920, proxy.width, guest.width), dim(1080, proxy.height, guest.height)});
}

/// The viewport / scissor scale of such a pass (proxy / guest per axis).
inline void TargetScale(SceneResolution::Size proxy, SceneResolution::Size guest, float out[2]) {
    out[0] = float(proxy.width) / float(std::max(1u, guest.width));
    out[1] = float(proxy.height) / float(std::max(1u, guest.height));
}

/// The half-resolution size of a scene depth of `scene` (the game rounds up odd sizes).
inline SceneResolution::Size HalfOf(SceneResolution::Size scene) {
    return {(scene.width + 1) / 2, (scene.height + 1) / 2};
}
inline bool IsHalfOf(SceneResolution::Size image, SceneResolution::Size scene) {
    if (scene.width < 64 || scene.height < 64) return false;
    const auto half = HalfOf(scene);
    const auto close_to = [](uint32_t a, uint32_t b) { return a == b || a + 1 == b || a == b + 1; };
    return close_to(image.width, half.width) && close_to(image.height, half.height);
}

/// The classification rules, on the facts the rasterizer has at a pass's start and at a draw.
/// Rule 1: the half-size depth/stencil target (a depth+stencil image of half the G-buffer depth's
/// size, used as a pass's depth attachment after this frame's G-buffer pass) is volumetric. (The
/// Forbidden Woods draw a few sky pieces into another half-size depth/color pair before the
/// G-buffer, whose memory a compute pass clears as a buffer: those stay at the game's size.) Rule 2: the color targets drawn with a
/// volumetric depth are volumetric (the effect buffer). Rule 3: a pass whose color targets all
/// have the volumetric depth's size, without a depth attachment, and that samples a volumetric
/// color image is volumetric too (the blur ping-pong and the depth-aware upsample). The composite
/// into the full-size scene and the quarter-size depth fail the size test and stay as they are.
struct PassFacts {
    SceneResolution::Size scene_depth{0, 0};   ///< the G-buffer's depth (0: not seen yet)
    bool has_depth = false;
    bool depth_has_stencil = false;
    bool depth_volumetric = false;
    SceneResolution::Size depth_size{0, 0};
    bool after_gbuffer = true; ///< this frame's G-buffer pass came before this pass
};
inline bool DepthIsVolumetric(const PassFacts& p) {
    return p.has_depth && (p.depth_volumetric || (p.after_gbuffer && p.depth_has_stencil &&
                                                  IsHalfOf(p.depth_size, p.scene_depth)));
}
/// Rule 2 for one color attachment of such a pass (same size as its depth).
inline bool ColorWithVolumetricDepth(const PassFacts& p, SceneResolution::Size color) {
    return DepthIsVolumetric(p) && color == p.depth_size;
}
/// Rule 3: `color` is an attachment of a pass without depth that samples a volumetric color
/// image; `half` is the volumetric depth's size.
inline bool ColorFromVolumetricSample(bool pass_has_depth, SceneResolution::Size color,
                                      SceneResolution::Size half) {
    return !pass_has_depth && half.width > 0 && color == half;
}

} // namespace Vulkan::VolumetricResolution

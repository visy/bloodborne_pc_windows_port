// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: native HDR output (docs/party/ERSC_PARITY.md I4). Bloodborne renders and grades an SDR
// picture; with HDR on (bbport.ini hdr=1, BB_HDR=1) the presenter keeps that picture as it is
// up to the final frame and the HDR compose pass (host_shaders/hdr_compose.frag) turns it into
// HDR10 (SMPTE ST 2084 PQ, Rec.2020 primaries) or scRGB for the swapchain:
//
//   1. decode: the game's encoded value e (graded for a gamma 2.2 TV, as BB_HDR_GAMMA assumes)
//      to linear Rec.709 light, rgb = e^2.2 (1.0 = the SDR picture's white);
//   2. inverse tone mapping on luminance only: Y = dot(rgb, Rec.709 luma); below the knee
//      (Y <= Knee) nothing changes, the mid-tones stay where paper white puts them; above it
//      f(Y) = Y + (T - 1) * ((Y - Knee) / (1 - Knee))^2, which meets the identity with the same
//      slope at the knee and reaches T = 1 + strength * (peak / paper white - 1) at Y = 1. The
//      colour is scaled by f(Y) / Y, so a highlight keeps its chromaticity (no hue shift, no
//      per-channel clipping to white);
//   3. saturation about luminance (1.0 = the game's own), still in linear Rec.709: values
//      outside Rec.709 stay, Rec.2020 holds them;
//   4. the port's overlay (menu, party HUD, performance HUD, captions) blended at paper white in
//      a gamma 2.2 space relative to paper white, which is how it blends on an SDR swapchain;
//   5. Rec.709 -> Rec.2020 (BT.2087), negatives clamped, the brightest channel limited to the
//      peak by scaling the colour (hue kept), times paper white in nits, PQ encoded.
//
// This header is the reference the shader mirrors (tests/test_hdr.cpp checks the numbers, and
// the GPU readback check compares the shader's output with it). No Vulkan, no game code.
#pragma once

#include <algorithm>
#include <cmath>

namespace BbHdr {

/// Swapchain encodings: SDR (the ordinary path), HDR10 PQ, scRGB (linear Rec.709, 1.0 = 80 nits).
enum Encoding : int { Sdr = 0, Pq = 1, ScRgb = 2 };

/// The settings that shape the curve (BbSettings: hdr_peak_nits, hdr_paper_white, ...).
struct Params {
    float peak_nits = 1000.0f;
    float paper_white_nits = 200.0f;
    float strength = 1.0f;   ///< 0: the SDR picture at paper white, 1: SDR white reaches the peak
    float saturation = 1.0f; ///< 1: the game's colours
};

inline constexpr float Knee = 0.5f;          ///< linear SDR luminance (encoded ~0.73) left as it is
inline constexpr float SdrGamma = 2.2f;      ///< the display gamma the game was graded for
inline constexpr float DefaultPeakNits = 1000.0f; ///< when the display reports none
inline constexpr float DefaultPaperWhite = 200.0f;
inline constexpr float MinPeakNits = 100.0f, MaxPeakNits = 10000.0f;
inline constexpr float MinPaperWhite = 80.0f, MaxPaperWhite = 500.0f;
inline constexpr float ScRgbUnitNits = 80.0f;

// SMPTE ST 2084.
inline constexpr float PqM1 = 2610.0f / 16384.0f;
inline constexpr float PqM2 = 2523.0f / 4096.0f * 128.0f;
inline constexpr float PqC1 = 3424.0f / 4096.0f;
inline constexpr float PqC2 = 2413.0f / 4096.0f * 32.0f;
inline constexpr float PqC3 = 2392.0f / 4096.0f * 32.0f;

/// Absolute luminance (cd/m2, 0..10000) to the PQ signal (0..1).
inline float PqEncode(float nits) {
    const float y = std::pow(std::clamp(nits / 10000.0f, 0.0f, 1.0f), PqM1);
    return std::pow((PqC1 + PqC2 * y) / (1.0f + PqC3 * y), PqM2);
}
/// The PQ signal (0..1) back to cd/m2.
inline float PqDecode(float signal) {
    const float p = std::pow(std::clamp(signal, 0.0f, 1.0f), 1.0f / PqM2);
    return 10000.0f * std::pow(std::max(p - PqC1, 0.0f) / (PqC2 - PqC3 * p), 1.0f / PqM1);
}

struct Rgb {
    float r = 0, g = 0, b = 0;
};

inline float Luminance709(Rgb c) {
    return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
}

/// BT.2087 Rec.709 -> Rec.2020 (linear light) and back.
inline Rgb Rec709To2020(Rgb c) {
    return {0.6274040f * c.r + 0.3292820f * c.g + 0.0433136f * c.b,
            0.0690970f * c.r + 0.9195400f * c.g + 0.0113612f * c.b,
            0.0163916f * c.r + 0.0880132f * c.g + 0.8955950f * c.b};
}
inline Rgb Rec2020To709(Rgb c) {
    return {1.6604910f * c.r - 0.5876411f * c.g - 0.0728499f * c.b,
            -0.1245505f * c.r + 1.1328999f * c.g - 0.0083494f * c.b,
            -0.0181508f * c.r - 0.1005789f * c.g + 1.1187297f * c.b};
}

/// The SDR picture's encoded value (0..1) to linear light (1.0 = SDR white).
inline float SdrDecode(float e) {
    return std::pow(std::clamp(e, 0.0f, 1.0f), SdrGamma);
}

/// Where SDR white lands, in multiples of paper white.
inline float WhiteTarget(const Params& p) {
    const float ratio = std::max(p.peak_nits, p.paper_white_nits) / p.paper_white_nits;
    return 1.0f + std::max(p.strength, 0.0f) * (ratio - 1.0f);
}

/// The inverse tone curve on linear SDR luminance (0..1), in multiples of paper white.
inline float Expand(float y, const Params& p) {
    if (y <= Knee) return y;
    const float t = (std::min(y, 1.0f) - Knee) / (1.0f - Knee);
    return y + (WhiteTarget(p) - 1.0f) * t * t;
}

/// Steps 1-3: the encoded SDR colour to linear Rec.709 in multiples of paper white.
inline Rgb ExpandColor(Rgb encoded, const Params& p) {
    Rgb c{SdrDecode(encoded.r), SdrDecode(encoded.g), SdrDecode(encoded.b)};
    const float y = Luminance709(c);
    if (y > Knee) {
        const float scale = Expand(y, p) / y;
        c = {c.r * scale, c.g * scale, c.b * scale};
    }
    const float y2 = Luminance709(c);
    return {y2 + (c.r - y2) * p.saturation, y2 + (c.g - y2) * p.saturation,
            y2 + (c.b - y2) * p.saturation};
}

/// Step 5: linear Rec.709 relative to paper white to Rec.2020 cd/m2, limited to the peak with
/// the colour's hue kept.
inline Rgb ToRec2020Nits(Rgb rel709, const Params& p) {
    Rgb c = Rec709To2020(rel709);
    c = {std::max(c.r, 0.0f) * p.paper_white_nits, std::max(c.g, 0.0f) * p.paper_white_nits,
         std::max(c.b, 0.0f) * p.paper_white_nits};
    const float peak = std::max(p.peak_nits, p.paper_white_nits);
    const float m = std::max({c.r, c.g, c.b});
    if (m > peak) {
        const float s = peak / m;
        c = {c.r * s, c.g * s, c.b * s};
    }
    return c;
}

/// The whole mapping of one game pixel without overlay: HDR10 signal values.
inline Rgb EncodePq(Rgb encoded_sdr, const Params& p) {
    const Rgb n = ToRec2020Nits(ExpandColor(encoded_sdr, p), p);
    return {PqEncode(n.r), PqEncode(n.g), PqEncode(n.b)};
}

/// Step 4: an overlay pixel (premultiplied colour `c` in the SDR swapchain's encoding, coverage
/// `a`) over a background in linear Rec.709 relative to paper white.
inline Rgb BlendOverlay(Rgb background, Rgb c, float a) {
    if (a <= 0.0f) return background;
    const auto enc = [](float x) { return std::copysign(std::pow(std::abs(x), 1.0f / SdrGamma), x); };
    const auto dec = [](float x) { return std::copysign(std::pow(std::abs(x), SdrGamma), x); };
    return {dec(c.r + enc(background.r) * (1.0f - a)), dec(c.g + enc(background.g) * (1.0f - a)),
            dec(c.b + enc(background.b) * (1.0f - a))};
}

/// The peak the curve uses: the setting (nits, 0 = automatic), else the display's reported
/// maximum, else 1000; within MinPeakNits..MaxPeakNits.
inline float ResolvePeak(int setting_nits, float display_nits) {
    float peak = setting_nits > 0 ? float(setting_nits) : display_nits > 0.0f ? display_nits : DefaultPeakNits;
    return std::clamp(peak, MinPeakNits, MaxPeakNits);
}

// ---- The calibration pattern (hdr_compose.frag draws it, the overlay labels it). ----
// Placed in a band of the screen (fractions of the swapchain) that leaves the game's System menu
// rows readable: the patches sit above them.
inline constexpr float PatternLeft = 0.06f, PatternTop = 0.04f, PatternRight = 0.94f, PatternBottom = 0.30f;
/// Its parts, as fractions of the band: the peak check (a 10000-nit field with a square at the
/// peak setting inside: the square vanishes when the setting matches the display), the paper
/// white patch, and a wedge of eleven SDR levels (0, 0.1 ... 1.0 encoded) through the curve.
inline constexpr float PatternSplitY = 0.62f; ///< boxes above, the wedge below
inline constexpr int WedgeSteps = 11;
inline constexpr float PeakBoxLeft = 0.0f, PeakBoxRight = 0.30f;
inline constexpr float PaperBoxLeft = 0.35f, PaperBoxRight = 0.65f;
inline constexpr float RampLeft = 0.70f, RampRight = 1.0f; ///< nits ramp 0 .. peak (PQ steps)

} // namespace BbHdr

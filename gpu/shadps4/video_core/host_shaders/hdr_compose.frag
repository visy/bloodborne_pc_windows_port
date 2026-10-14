// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: native HDR output (I4): the game's SDR frame, letterboxed into the swapchain, expanded
// to HDR10 (PQ, Rec.2020) or scRGB, with the port's overlay composited at paper white. The
// reference for every step is gpu/shim/bbport_hdr.h (same constants, same order).
#version 450

layout (location = 0) in vec2 uv_unused;
layout (location = 0) out vec4 color;

layout (binding = 0) uniform sampler2D frame_tex;   // the SDR frame (encoded), fitted size
layout (binding = 1) uniform sampler2D overlay_tex; // the overlay (premultiplied), swapchain size

layout (push_constant) uniform Compose {
    vec4 fit;          // the frame's rectangle in the swapchain: x, y, width, height (pixels)
    vec4 pattern_rect; // the calibration band: left, top, right, bottom (pixels)
    float peak;        // cd/m2
    float paper_white; // cd/m2
    float strength;
    float saturation;
    int encoding;      // 1 PQ, 2 scRGB
    int flags;         // 1 frame, 2 overlay, 4 pattern
    float background;  // encoded SDR level outside the frame (or without one)
    float pad;
} pc;

const float Knee = 0.5;
const float SdrGamma = 2.2;
const float m1 = 2610.0 / 16384.0, m2 = 2523.0 / 4096.0 * 128.0;
const float c1 = 3424.0 / 4096.0, c2 = 2413.0 / 4096.0 * 32.0, c3 = 2392.0 / 4096.0 * 32.0;
const vec3 Luma709 = vec3(0.2126, 0.7152, 0.0722);
// Column-major: Rec709To2020 / Rec2020To709 of bbport_hdr.h.
const mat3 To2020 = mat3(0.6274040, 0.0690970, 0.0163916,
                         0.3292820, 0.9195400, 0.0880132,
                         0.0433136, 0.0113612, 0.8955950);
const mat3 To709 = mat3(1.6604910, -0.1245505, -0.0181508,
                        -0.5876411, 1.1328999, -0.1005789,
                        -0.0728499, -0.0083494, 1.1187297);

vec3 pq_encode(vec3 nits) {
    vec3 y = pow(clamp(nits / 10000.0, 0.0, 1.0), vec3(m1));
    return pow((c1 + c2 * y) / (1.0 + c3 * y), vec3(m2));
}

float white_target() {
    float ratio = max(pc.peak, pc.paper_white) / pc.paper_white;
    return 1.0 + max(pc.strength, 0.0) * (ratio - 1.0);
}

float expand(float y) {
    if (y <= Knee) return y;
    float t = (min(y, 1.0) - Knee) / (1.0 - Knee);
    return y + (white_target() - 1.0) * t * t;
}

// Linear Rec.709 relative to paper white.
vec3 expand_color(vec3 encoded) {
    vec3 c = pow(clamp(encoded, 0.0, 1.0), vec3(SdrGamma));
    float y = dot(c, Luma709);
    if (y > Knee) c *= expand(y) / y;
    float y2 = dot(c, Luma709);
    return y2 + (c - y2) * pc.saturation;
}

vec3 spow(vec3 x, float e) {
    return sign(x) * pow(abs(x), vec3(e));
}

vec3 blend_overlay(vec3 background, vec4 over) {
    if (over.a <= 0.0) return background;
    return spow(over.rgb + spow(background, 1.0 / SdrGamma) * (1.0 - over.a), SdrGamma);
}

// Rec.2020 cd/m2, the brightest channel limited to the peak (hue kept).
vec3 to_2020_nits(vec3 rel709) {
    vec3 c = max(To2020 * rel709, vec3(0.0)) * pc.paper_white;
    float peak = max(pc.peak, pc.paper_white);
    float m = max(c.r, max(c.g, c.b));
    if (m > peak) c *= peak / m;
    return c;
}

vec4 encode(vec3 nits2020) {
    if (pc.encoding == 2) return vec4(To709 * nits2020 / 80.0, 1.0);
    return vec4(pq_encode(nits2020), 1.0);
}

// The calibration band (layout: BbHdr::Pattern* in bbport_hdr.h). Returns cd/m2 (Rec.2020,
// neutral) of the pattern, or -1 outside its patches.
float pattern_nits(vec2 p, out bool sdr_level, out float level) {
    sdr_level = false;
    level = 0.0;
    vec2 q = (p - pc.pattern_rect.xy) / (pc.pattern_rect.zw - pc.pattern_rect.xy);
    if (any(lessThan(q, vec2(0.0))) || any(greaterThanEqual(q, vec2(1.0)))) return -1.0;
    const float split = 0.62;
    if (q.y < split) {
        float y = q.y / split;
        if (q.x < 0.30) {
            // Peak check: a 10000-nit field, the inner square at the peak setting.
            vec2 b = vec2(q.x / 0.30, y);
            bool inner = all(greaterThan(b, vec2(0.3))) && all(lessThan(b, vec2(0.7)));
            return inner ? pc.peak : 10000.0;
        }
        if (q.x >= 0.35 && q.x < 0.65) return pc.paper_white; // paper white patch
        if (q.x >= 0.70) {
            // 0 .. peak in 16 PQ-uniform steps.
            float s = floor((q.x - 0.70) / 0.30 * 16.0) / 15.0;
            float signal = s * pq_encode(vec3(max(pc.peak, pc.paper_white))).r;
            float pp = pow(signal, 1.0 / m2);
            return 10000.0 * pow(max(pp - c1, 0.0) / (c2 - c3 * pp), 1.0 / m1);
        }
        return -1.0;
    }
    // The wedge: eleven SDR levels through the curve.
    sdr_level = true;
    level = min(floor(q.x * 11.0), 10.0) / 10.0;
    return 0.0;
}

void main() {
    vec2 p = gl_FragCoord.xy;
    vec3 encoded = vec3(pc.background);
    if ((pc.flags & 1) != 0) {
        vec2 f = (p - pc.fit.xy) / pc.fit.zw;
        if (all(greaterThanEqual(f, vec2(0.0))) && all(lessThan(f, vec2(1.0)))) {
            encoded = texture(frame_tex, f).rgb;
        }
    }
    vec3 rel = expand_color(encoded);
    if ((pc.flags & 4) != 0) {
        bool sdr_level;
        float level;
        float nits = pattern_nits(p, sdr_level, level);
        if (sdr_level) {
            rel = expand_color(vec3(level));
        } else if (nits >= 0.0) {
            // Exact luminance, past the peak limit (the peak check needs 10000 nits).
            vec3 out_nits = vec3(nits);
            if ((pc.flags & 2) != 0) {
                vec4 over = texelFetch(overlay_tex, ivec2(p), 0);
                // Labels over a patch: neutral, so Rec.709 and Rec.2020 agree.
                if (over.a > 0.0) out_nits = to_2020_nits(blend_overlay(vec3(nits / pc.paper_white), over));
            }
            color = encode(out_nits);
            return;
        }
    }
    if ((pc.flags & 2) != 0) {
        rel = blend_overlay(rel, texelFetch(overlay_tex, ivec2(p), 0));
    }
    color = encode(to_2020_nits(rel));
}

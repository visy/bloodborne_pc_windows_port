// SPDX-License-Identifier: GPL-2.0-or-later
// In-game map (Hunter's Atlas by MikeTat1988, github.com/MikeTat1988/HuntersAtlas commit 06d4021,
// GPL-2.0-or-later): the pure part. The map data format the baker writes from the generator's output
// (tools/map/bake_bbmap.py), its bounds-checked reader, the view transform, floor choice and lamp
// hit test. No game, no ImGui: unit test tests/test_map_core.cpp.
//
// The geometry model (floors as triangles per height layer, floor outlines, wall segments, lamps)
// is Hunter's Atlas's own (its generator's JSON: source/generator/src/generate_map_v13.py); the floor
// choice follows its AutoLayer (source/native/gpu/shim/bbport_map.cpp: the layer whose floor surface
// under the player is closest to the player's height).
//
// .bbm file (little endian; one per area, written by the baker, read here):
//   "BBM1"  u32 version(=1)  u32 packed_map (0xAABBCCDD of the area's main block)
//   f32 bounds[4] (xmin, zmin, xmax, zmax)   u16 name_len, name (UTF-8)   u16 id_len, id ("m24_00_00_00")
//   u32 layer_count, per layer: f32 height, span, anchor; u32 tri_count, f32 tri[7] (x0 z0 x1 z1 x2 z2 y);
//     u32 ring_count, per ring: u32 pts, f32 (x z)*pts; u32 wall_count, f32 wall[4] (x0 z0 x1 z1)
//   u32 lamp_count, per lamp: u32 return_point, f32 x y z, u16 name_len, name
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace bbmap {

constexpr std::uint32_t kVersion = 1;
// Bounds on what a file may hold (the file is the player's own, but a corrupt one must not
// allocate without end).
constexpr std::uint32_t kMaxLayers = 64;
constexpr std::uint32_t kMaxTris = 2u << 20;
constexpr std::uint32_t kMaxRingPts = 1u << 20;
constexpr std::uint32_t kMaxWalls = 1u << 20;
constexpr std::uint32_t kMaxLamps = 256;

struct Layer {
    float height = 0, span = 3, anchor = 0;
    std::vector<float> tris;                ///< 7 floats per triangle: x0 z0 x1 z1 x2 z2 y (mean floor height)
    std::vector<std::vector<float>> rings;  ///< floor outlines: x z pairs
    std::vector<float> walls;               ///< 4 floats per segment
    std::size_t TriCount() const { return tris.size() / 7; }
};

struct Lamp {
    std::uint32_t return_point = 0;
    float x = 0, y = 0, z = 0;
    std::string name;
};

struct Area {
    std::string id, name;
    std::uint32_t packed = 0;
    float bounds[4] = {0, 0, 1, 1};
    std::vector<Layer> layers;
    std::vector<Lamp> lamps;
    bool loaded = false;  ///< layers and lamps are read (the header alone otherwise)
};

// ---- Reader / writer ----
struct Reader {
    const unsigned char* p;
    std::size_t n, at = 0;
    bool bad = false;
    bool Take(void* out, std::size_t len) {
        if (bad || len > n - at) return bad = true, false;
        std::memcpy(out, p + at, len);
        at += len;
        return true;
    }
    std::uint32_t U32() {
        std::uint32_t v = 0;
        Take(&v, 4);
        return v;
    }
    std::uint16_t U16() {
        std::uint16_t v = 0;
        Take(&v, 2);
        return v;
    }
    float F32() {
        float v = 0;
        Take(&v, 4);
        if (!std::isfinite(v)) return bad = true, 0.0f;
        return v;
    }
    std::string Str() {
        const std::uint16_t len = U16();
        if (bad || len > n - at) return bad = true, std::string();
        std::string s(reinterpret_cast<const char*>(p + at), len);
        at += len;
        return s;
    }
    bool Floats(std::vector<float>* out, std::uint64_t count) {
        if (bad || count > (n - at) / 4) return bad = true, false;
        out->resize(static_cast<std::size_t>(count));
        if (count) std::memcpy(out->data(), p + at, static_cast<std::size_t>(count) * 4);
        at += static_cast<std::size_t>(count) * 4;
        for (float f : *out)
            if (!std::isfinite(f)) return bad = true, false;
        return true;
    }
};

inline bool ParseHeader(const unsigned char* data, std::size_t size, Area* out, std::size_t* end = nullptr) {
    Reader r{data, size};
    char magic[4] = {};
    r.Take(magic, 4);
    if (r.bad || std::memcmp(magic, "BBM1", 4) != 0) return false;
    if (r.U32() != kVersion) return false;
    Area a;
    a.packed = r.U32();
    for (float& b : a.bounds) b = r.F32();
    a.name = r.Str();
    a.id = r.Str();
    if (r.bad || !(a.bounds[0] < a.bounds[2]) || !(a.bounds[1] < a.bounds[3]) || a.id.empty()) return false;
    *out = std::move(a);
    if (end) *end = r.at;
    return true;
}

inline bool Parse(const unsigned char* data, std::size_t size, Area* out) {
    Area a;
    std::size_t at = 0;
    if (!ParseHeader(data, size, &a, &at)) return false;
    Reader r{data, size, at};
    const std::uint32_t layers = r.U32();
    if (r.bad || layers > kMaxLayers) return false;
    for (std::uint32_t i = 0; i < layers; ++i) {
        Layer l;
        l.height = r.F32();
        l.span = r.F32();
        l.anchor = r.F32();
        const std::uint32_t tris = r.U32();
        if (r.bad || tris > kMaxTris || !r.Floats(&l.tris, std::uint64_t(tris) * 7)) return false;
        const std::uint32_t rings = r.U32();
        if (r.bad || rings > kMaxWalls) return false;
        for (std::uint32_t k = 0; k < rings; ++k) {
            const std::uint32_t pts = r.U32();
            std::vector<float> ring;
            if (r.bad || pts > kMaxRingPts || !r.Floats(&ring, std::uint64_t(pts) * 2)) return false;
            l.rings.push_back(std::move(ring));
        }
        const std::uint32_t walls = r.U32();
        if (r.bad || walls > kMaxWalls || !r.Floats(&l.walls, std::uint64_t(walls) * 4)) return false;
        a.layers.push_back(std::move(l));
    }
    const std::uint32_t lamps = r.U32();
    if (r.bad || lamps > kMaxLamps) return false;
    for (std::uint32_t i = 0; i < lamps; ++i) {
        Lamp l;
        l.return_point = r.U32();
        l.x = r.F32();
        l.y = r.F32();
        l.z = r.F32();
        l.name = r.Str();
        if (r.bad) return false;
        a.lamps.push_back(std::move(l));
    }
    if (r.bad) return false;
    a.loaded = true;
    *out = std::move(a);
    return true;
}

inline void Put(std::string& s, const void* p, std::size_t n) { s.append(static_cast<const char*>(p), n); }
inline void PutU32(std::string& s, std::uint32_t v) { Put(s, &v, 4); }
inline void PutU16(std::string& s, std::uint16_t v) { Put(s, &v, 2); }
inline void PutF32(std::string& s, float v) { Put(s, &v, 4); }
inline void PutStr(std::string& s, const std::string& v) {
    PutU16(s, static_cast<std::uint16_t>(std::min<std::size_t>(v.size(), 0xffff)));
    s.append(v, 0, 0xffff);
}

/// The file's bytes (what the baker writes; the unit test builds fixtures with it).
inline std::string Serialize(const Area& a) {
    std::string s = "BBM1";
    PutU32(s, kVersion);
    PutU32(s, a.packed);
    for (float b : a.bounds) PutF32(s, b);
    PutStr(s, a.name);
    PutStr(s, a.id);
    PutU32(s, static_cast<std::uint32_t>(a.layers.size()));
    for (const Layer& l : a.layers) {
        PutF32(s, l.height);
        PutF32(s, l.span);
        PutF32(s, l.anchor);
        PutU32(s, static_cast<std::uint32_t>(l.tris.size() / 7));
        for (float f : l.tris) PutF32(s, f);
        PutU32(s, static_cast<std::uint32_t>(l.rings.size()));
        for (const auto& ring : l.rings) {
            PutU32(s, static_cast<std::uint32_t>(ring.size() / 2));
            for (float f : ring) PutF32(s, f);
        }
        PutU32(s, static_cast<std::uint32_t>(l.walls.size() / 4));
        for (float f : l.walls) PutF32(s, f);
    }
    PutU32(s, static_cast<std::uint32_t>(a.lamps.size()));
    for (const Lamp& l : a.lamps) {
        PutU32(s, l.return_point);
        PutF32(s, l.x);
        PutF32(s, l.y);
        PutF32(s, l.z);
        PutStr(s, l.name);
    }
    return s;
}

// ---- Map ids ----
/// The area (index) whose main block is `map_id`'s area and block (0xAABBCCDD: AA and BB; the
/// generator's maps are the blocks mAA_BB_00_00), -1 when none.
inline int AreaOfMap(const std::vector<Area>& areas, std::uint32_t map_id) {
    if (map_id == 0 || map_id == 0xffffffffu) return -1;
    for (std::size_t i = 0; i < areas.size(); ++i)
        if ((areas[i].packed & 0xffff0000u) == (map_id & 0xffff0000u)) return static_cast<int>(i);
    return -1;
}
/// "m24_01_00_00" -> 0x18010000; 0 when it is not a map id.
inline std::uint32_t PackMapId(const std::string& id) {
    unsigned a, b, c, d;
    if (id.size() != 12 || id[0] != 'm' || std::sscanf(id.c_str(), "m%2u_%2u_%2u_%2u", &a, &b, &c, &d) != 4) return 0;
    return (a << 24) | (b << 16) | (c << 8) | d;
}

// ---- View ----
struct Pt {
    float x = 0, y = 0;
};
/// The screen rectangle the map is drawn in, the world point at its centre and the scale. North
/// (+z) is up the screen.
struct View {
    float cx = 0, cz = 0;  ///< world point at the centre of the rectangle
    float zoom = 4;        ///< pixels per metre
    float x0 = 0, y0 = 0, w = 1280, h = 720;
};
inline Pt ToScreen(const View& v, float x, float z) {
    return {v.x0 + v.w * 0.5f + (x - v.cx) * v.zoom, v.y0 + v.h * 0.5f - (z - v.cz) * v.zoom};
}
inline void ToWorld(const View& v, Pt s, float* x, float* z) {
    *x = v.cx + (s.x - v.x0 - v.w * 0.5f) / v.zoom;
    *z = v.cz - (s.y - v.y0 - v.h * 0.5f) / v.zoom;
}
/// The zoom at which the whole area fits the rectangle.
inline float FitZoom(const View& v, const float bounds[4], float margin_px = 24) {
    const float w = std::max(1.0f, bounds[2] - bounds[0]), h = std::max(1.0f, bounds[3] - bounds[1]);
    return std::max(0.05f, std::min((v.w - 2 * margin_px) / w, (v.h - 2 * margin_px) / h));
}
inline void Fit(View* v, const float bounds[4]) {
    v->cx = (bounds[0] + bounds[2]) * 0.5f;
    v->cz = (bounds[1] + bounds[3]) * 0.5f;
    v->zoom = FitZoom(*v, bounds);
}
/// Zoom by `factor` keeping the world point under screen point `at` fixed; the zoom stays within
/// [FitZoom * 0.5, 24].
inline void ZoomAt(View* v, Pt at, float factor, const float bounds[4]) {
    float wx, wz;
    ToWorld(*v, at, &wx, &wz);
    const float lo = FitZoom(*v, bounds) * 0.5f;
    v->zoom = std::min(24.0f, std::max(lo, v->zoom * factor));
    v->cx = wx - (at.x - v->x0 - v->w * 0.5f) / v->zoom;
    v->cz = wz + (at.y - v->y0 - v->h * 0.5f) / v->zoom;
}
inline void PanPixels(View* v, float dx, float dy) {
    v->cx -= dx / v->zoom;
    v->cz += dy / v->zoom;
}
/// The centre stays within the area's bounds (a little beyond).
inline void ClampCentre(View* v, const float bounds[4]) {
    const float pad = 40;
    v->cx = std::min(std::max(v->cx, bounds[0] - pad), bounds[2] + pad);
    v->cz = std::min(std::max(v->cz, bounds[1] - pad), bounds[3] + pad);
}

/// A direction on the screen from a character's yaw (radians; the party HUD's convention:
/// forward = (sin yaw, cos yaw) in world x, z). Screen y runs against z.
inline Pt FacingOnScreen(float yaw) { return {std::sin(yaw), -std::cos(yaw)}; }

// ---- Floors ----
inline bool InTriangle(const float* t, float x, float z) {
    auto cross = [&](int u, int v) { return (t[v] - t[u]) * (z - t[u + 1]) - (t[v + 1] - t[u + 1]) * (x - t[u]); };
    const float ab = cross(0, 2), bc = cross(2, 4), ca = cross(4, 0);
    return (ab >= -1e-4f && bc >= -1e-4f && ca >= -1e-4f) || (ab <= 1e-4f && bc <= 1e-4f && ca <= 1e-4f);
}
/// The layer to show for a character at (x, y, z): of the layers with floor under (x, z) the one
/// whose floor is closest to y (Hunter's Atlas's AutoLayer); with none under it the layer whose
/// anchor is closest to y. -1 for an area without layers.
inline int PickLayer(const Area& a, float x, float y, float z) {
    int near_anchor = -1, under = -1;
    float anchor_d = 1e30f, under_d = 1e30f;
    for (std::size_t i = 0; i < a.layers.size(); ++i) {
        const Layer& l = a.layers[i];
        const float d = std::fabs(y - l.anchor);
        if (d < anchor_d) anchor_d = d, near_anchor = static_cast<int>(i);
        for (std::size_t t = 0; t + 7 <= l.tris.size(); t += 7) {
            const float* tr = &l.tris[t];
            if (x < std::min({tr[0], tr[2], tr[4]}) || x > std::max({tr[0], tr[2], tr[4]}) ||
                z < std::min({tr[1], tr[3], tr[5]}) || z > std::max({tr[1], tr[3], tr[5]}))
                continue;
            if (!InTriangle(tr, x, z)) continue;
            const float sd = std::fabs(y - tr[6]);
            if (sd < under_d) under_d = sd, under = static_cast<int>(i);
        }
    }
    return under >= 0 ? under : near_anchor;
}
/// The layer with the largest footprint (the area's main floor), for an area the player is not in.
inline int MainLayer(const Area& a) {
    int best = -1;
    float best_area = -1;
    for (std::size_t i = 0; i < a.layers.size(); ++i) {
        float sum = 0;
        const auto& t = a.layers[i].tris;
        for (std::size_t k = 0; k + 7 <= t.size(); k += 7)
            sum += std::fabs((t[k + 2] - t[k]) * (t[k + 5] - t[k + 1]) - (t[k + 4] - t[k]) * (t[k + 3] - t[k + 1])) * 0.5f;
        if (sum > best_area) best_area = sum, best = static_cast<int>(i);
    }
    return best;
}

/// The lamp (index into `lamps`) nearest to screen point `at` within `radius_px`, among those
/// `shown[i]`; -1 none.
inline int LampAt(const std::vector<Lamp>& lamps, const std::vector<bool>& shown, const View& v, Pt at,
                  float radius_px) {
    int best = -1;
    float best_d = radius_px * radius_px;
    for (std::size_t i = 0; i < lamps.size(); ++i) {
        if (i >= shown.size() || !shown[i]) continue;
        const Pt p = ToScreen(v, lamps[i].x, lamps[i].z);
        const float d = (p.x - at.x) * (p.x - at.x) + (p.y - at.y) * (p.y - at.y);
        if (d <= best_d) best_d = d, best = static_cast<int>(i);
    }
    return best;
}

/// Next lamp in direction `dir` (screen unit vector) from lamp `from` among `shown` (controller
/// selection): the nearest within a 60-degree cone, else any; -1 when `shown` has no other lamp.
inline int NextLamp(const std::vector<Lamp>& lamps, const std::vector<bool>& shown, const View& v, int from,
                    Pt dir) {
    if (from < 0 || from >= static_cast<int>(lamps.size())) {
        for (std::size_t i = 0; i < lamps.size(); ++i)
            if (i < shown.size() && shown[i]) return static_cast<int>(i);
        return -1;
    }
    const Pt a = ToScreen(v, lamps[from].x, lamps[from].z);
    int best = -1;
    float best_score = 1e30f;
    for (std::size_t i = 0; i < lamps.size(); ++i) {
        if (static_cast<int>(i) == from || i >= shown.size() || !shown[i]) continue;
        const Pt p = ToScreen(v, lamps[i].x, lamps[i].z);
        const float dx = p.x - a.x, dy = p.y - a.y, d = std::sqrt(dx * dx + dy * dy);
        if (d < 1e-3f) continue;
        const float along = (dx * dir.x + dy * dir.y) / d;
        if (along < 0.5f) continue;  // outside +-60 degrees
        const float score = d * (2.0f - along);
        if (score < best_score) best_score = score, best = static_cast<int>(i);
    }
    return best;
}

}  // namespace bbmap

// SPDX-License-Identifier: GPL-2.0-or-later
// The in-game map's pure core (gpu/shim/map/map_core.h): the .bbm data reader and writer, the view
// transform, floor choice and lamp hit test. Synthetic data only.
// Build and run: ninja -C out/gpu map-core-test && out/gpu/map-core-test.exe
#include "map/map_core.h"

#include <cstdio>

using namespace bbmap;

static int g_failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)
#define NEAR(a, b) CHECK(std::fabs((a) - (b)) < 1e-3f)

static Area Sample() {
    Area a;
    a.id = "m24_01_00_00";
    a.name = "Central Yharnam";
    a.packed = PackMapId(a.id);
    a.bounds[0] = -50, a.bounds[1] = -20, a.bounds[2] = 150, a.bounds[3] = 80;
    Layer ground;  // a 10 x 10 square at y 0 (two triangles)
    ground.height = 0, ground.span = 3, ground.anchor = 1.5f;
    ground.tris = {0, 0, 10, 0, 10, 10, 0.0f, 0, 0, 10, 10, 0, 10, 0.0f};
    ground.rings = {{0, 0, 10, 0, 10, 10, 0, 10}};
    ground.walls = {0, 0, 10, 0};
    Layer upper;  // a balcony over it at y 6, half the square
    upper.height = 6, upper.span = 3, upper.anchor = 7.5f;
    upper.tris = {0, 0, 5, 0, 5, 10, 6.0f};
    a.layers = {ground, upper};
    a.lamps = {{2412951, 3, 0, 4, "Central Yharnam"}, {2412952, 80, 2, 60, "Great Bridge"}};
    a.loaded = true;
    return a;
}

static void TestFormat() {
    const Area a = Sample();
    const std::string bytes = Serialize(a);
    Area b;
    CHECK(Parse(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), &b));
    CHECK(b.id == a.id && b.name == a.name && b.packed == 0x18010000u);
    CHECK(b.layers.size() == 2 && b.layers[0].TriCount() == 2 && b.layers[1].TriCount() == 1);
    CHECK(b.layers[0].rings.size() == 1 && b.layers[0].rings[0].size() == 8 && b.layers[0].walls.size() == 4);
    CHECK(b.lamps.size() == 2 && b.lamps[1].return_point == 2412952 && b.lamps[1].name == "Great Bridge");
    NEAR(b.bounds[2], 150.0f);
    CHECK(Serialize(b) == bytes);  // a stable round trip

    // The header alone.
    Area h;
    std::size_t end = 0;
    CHECK(ParseHeader(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), &h, &end));
    CHECK(h.id == a.id && !h.loaded && h.layers.empty() && end > 20 && end < bytes.size());

    // Every truncation is refused, none reads past the end.
    for (std::size_t n = 0; n < bytes.size(); ++n) {
        Area t;
        CHECK(!Parse(reinterpret_cast<const unsigned char*>(bytes.data()), n, &t));
    }
    // Wrong magic / version, absurd counts, non-finite floats.
    std::string bad = bytes;
    bad[0] = 'X';
    Area t;
    CHECK(!Parse(reinterpret_cast<const unsigned char*>(bad.data()), bad.size(), &t));
    bad = bytes;
    bad[4] = 9;
    CHECK(!Parse(reinterpret_cast<const unsigned char*>(bad.data()), bad.size(), &t));
    {
        Area big = a;
        std::string s = Serialize(big);
        // The layer count sits right after the strings: patch it to 1e6.
        const std::size_t at = 4 + 4 + 4 + 16 + 2 + a.name.size() + 2 + a.id.size();
        const std::uint32_t huge = 1000000;
        std::memcpy(&s[at], &huge, 4);
        CHECK(!Parse(reinterpret_cast<const unsigned char*>(s.data()), s.size(), &t));
    }
    {
        std::string s = bytes;
        const float nan = std::nanf("");
        std::memcpy(&s[12], &nan, 4);  // bounds[0]
        CHECK(!Parse(reinterpret_cast<const unsigned char*>(s.data()), s.size(), &t));
    }
    {
        Area flat = a;  // empty bounds
        flat.bounds[2] = flat.bounds[0];
        const std::string s = Serialize(flat);
        CHECK(!Parse(reinterpret_cast<const unsigned char*>(s.data()), s.size(), &t));
    }
}

// The bytes scripts/bbmap_bake.py makes for a tiny area (tests/test_map_bake.py GOLDEN_TINY): the reader and the baker agree.
static void TestGolden() {
    static const char* hex =
        "42424d31010000000000011500000000000000000000a04100002041040054696e790c006d32315f30315f30305f30300100000000000000000040400000c03f0100000000000000000000000000804000000000000000000000804000000040010000000300000000000000000000000000804000000000000000000000804001000000000000000000000000008040000000000100000097d124000000803f000000400000404004004c616d70";
    std::string bytes;
    for (const char* p = hex; *p; p += 2) {
        unsigned v = 0;
        std::sscanf(p, "%2x", &v);
        bytes.push_back(static_cast<char>(v));
    }
    CHECK(bytes.size() == 174);
    Area a;
    CHECK(Parse(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), &a));
    CHECK(a.id == "m21_01_00_00" && a.name == "Tiny" && a.packed == 0x15010000u);
    NEAR(a.bounds[2], 20.0f);
    CHECK(a.layers.size() == 1 && a.layers[0].TriCount() == 1);
    NEAR(a.layers[0].tris[6], 2.0f);  // the mean of 1, 2, 3
    CHECK(a.layers[0].rings.size() == 1 && a.layers[0].rings[0].size() == 6 && a.layers[0].walls.size() == 4);
    CHECK(a.lamps.size() == 1 && a.lamps[0].return_point == 2412951 && a.lamps[0].name == "Lamp");
    NEAR(a.lamps[0].z, 3.0f);
    CHECK(Serialize(a) == bytes);
}

static void TestIds() {
    CHECK(PackMapId("m24_01_00_00") == 0x18010000u);
    CHECK(PackMapId("m21_00_00_00") == 0x15000000u);
    CHECK(PackMapId("x24_01_00_00") == 0);
    CHECK(PackMapId("m24_01") == 0);
    std::vector<Area> areas(2);
    areas[0].packed = 0x18000000u;  // m24_00
    areas[1].packed = 0x18010000u;  // m24_01
    CHECK(AreaOfMap(areas, 0x18010000u) == 1);
    CHECK(AreaOfMap(areas, 0x18010100u) == 1);  // another sub block of m24_01
    CHECK(AreaOfMap(areas, 0x18000000u) == 0);
    CHECK(AreaOfMap(areas, 0x18020000u) == -1);
    CHECK(AreaOfMap(areas, 0xffffffffu) == -1);
    CHECK(AreaOfMap(areas, 0) == -1);
}

static void TestView() {
    View v;
    v.x0 = 100, v.y0 = 50, v.w = 800, v.h = 600;
    v.cx = 10, v.cz = 20, v.zoom = 4;
    Pt c = ToScreen(v, 10, 20);  // the centre of the rectangle
    NEAR(c.x, 500); NEAR(c.y, 350);
    Pt n = ToScreen(v, 10, 30);  // north (+z) is up the screen
    CHECK(n.y < c.y);
    NEAR(n.y, 350 - 40);
    Pt e = ToScreen(v, 20, 20);  // east (+x) is to the right
    NEAR(e.x, 540);
    float x, z;
    ToWorld(v, {540, 310}, &x, &z);
    NEAR(x, 20); NEAR(z, 30);
    // Round trip.
    for (float px : {120.0f, 333.3f, 899.0f})
        for (float py : {60.0f, 400.0f, 640.0f}) {
            ToWorld(v, {px, py}, &x, &z);
            const Pt back = ToScreen(v, x, z);
            NEAR(back.x, px); NEAR(back.y, py);
        }

    const float b[4] = {0, 0, 200, 100};
    Fit(&v, b);
    NEAR(v.cx, 100); NEAR(v.cz, 50);
    NEAR(v.zoom, (800 - 48) / 200.0f);  // width limits

    // Zoom keeps the point under the cursor.
    const Pt cursor{300, 200};
    ToWorld(v, cursor, &x, &z);
    ZoomAt(&v, cursor, 2.0f, b);
    float x2, z2;
    ToWorld(v, cursor, &x2, &z2);
    NEAR(x, x2); NEAR(z, z2);
    for (int i = 0; i < 40; ++i) ZoomAt(&v, cursor, 2.0f, b);
    NEAR(v.zoom, 24.0f);  // capped
    for (int i = 0; i < 60; ++i) ZoomAt(&v, cursor, 0.5f, b);
    NEAR(v.zoom, FitZoom(v, b) * 0.5f);  // floor: half the fit zoom

    // Pan by a drag: the map follows the cursor.
    View p;
    p.cx = 0, p.cz = 0, p.zoom = 2;
    ToWorld(p, {640, 360}, &x, &z);
    PanPixels(&p, 20, -10);  // dragged right 20 px and up 10 px
    const Pt after = ToScreen(p, x, z);
    NEAR(after.x, 660); NEAR(after.y, 350);
    p.cx = 5000, p.cz = -5000;
    ClampCentre(&p, b);
    NEAR(p.cx, 240); NEAR(p.cz, -40);

    // Facing: yaw 0 looks along +z = up the screen; yaw +90 deg along +x = right.
    NEAR(FacingOnScreen(0).x, 0); NEAR(FacingOnScreen(0).y, -1);
    NEAR(FacingOnScreen(1.5707963f).x, 1); NEAR(FacingOnScreen(1.5707963f).y, 0);
}

static void TestLayers() {
    const Area a = Sample();
    // On the ground floor under the balcony: the closest floor is the ground (y 0), not the balcony (y 6).
    CHECK(PickLayer(a, 2, 0.1f, 3) == 0);
    // On the balcony.
    CHECK(PickLayer(a, 2, 6.2f, 3) == 1);
    // Where only the ground has floor, whatever the height.
    CHECK(PickLayer(a, 8, 5.9f, 8) == 0);
    // Off every floor: the layer whose anchor is closest to the height.
    CHECK(PickLayer(a, 100, 7.0f, 100) == 1);
    CHECK(PickLayer(a, 100, 0.0f, 100) == 0);
    // No layers.
    Area empty;
    CHECK(PickLayer(empty, 0, 0, 0) == -1);
    CHECK(MainLayer(empty) == -1);
    CHECK(MainLayer(a) == 0);  // 100 m2 vs 25 m2
}

static void TestLamps() {
    const Area a = Sample();
    View v;
    v.cx = 40, v.cz = 30, v.zoom = 5, v.x0 = 0, v.y0 = 0, v.w = 1000, v.h = 600;
    std::vector<bool> shown = {true, true};
    const Pt p0 = ToScreen(v, a.lamps[0].x, a.lamps[0].z), p1 = ToScreen(v, a.lamps[1].x, a.lamps[1].z);
    CHECK(LampAt(a.lamps, shown, v, {p0.x + 3, p0.y - 2}, 14) == 0);
    CHECK(LampAt(a.lamps, shown, v, {p1.x, p1.y}, 14) == 1);
    CHECK(LampAt(a.lamps, shown, v, {p0.x + 60, p0.y}, 14) == -1);
    shown[0] = false;  // an unlit lamp is not on the map
    CHECK(LampAt(a.lamps, shown, v, {p0.x, p0.y}, 14) == -1);
    shown[0] = true;
    // Controller selection: from lamp 0 to the right reaches lamp 1 (80 m east); up/left/down find none.
    CHECK(NextLamp(a.lamps, shown, v, 0, {1, 0}) == 1);
    CHECK(NextLamp(a.lamps, shown, v, 0, {-1, 0}) == -1);
    CHECK(NextLamp(a.lamps, shown, v, 1, {-1, 0}) == 0);
    CHECK(NextLamp(a.lamps, shown, v, -1, {1, 0}) == 0);  // nothing selected: the first shown
    shown[0] = shown[1] = false;
    CHECK(NextLamp(a.lamps, shown, v, -1, {1, 0}) == -1);
}

int main() {
    TestFormat();
    TestGolden();
    TestIds();
    TestView();
    TestLayers();
    TestLamps();
    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("map-core-test: all passed\n");
    return 0;
}

// SPDX-License-Identifier: GPL-2.0-or-later
// The in-game map (map_view.h). After Hunter's Atlas by MikeTat1988 (GPL-2.0-or-later), see map_core.h.
#include "map_view.h"

#include "bbport_map_controller.h"
#include "map_core.h"

#include "../bbport_settings.h"
#include "../party/lamp_travel.h"
#include "../party/party_hud.h"
#include "../party/party_status.h"

#include "imgui.h"
#include "imgui_internal.h"
#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace BbMapView {

namespace {

namespace fs = std::filesystem;
using bbmap::Pt;

std::atomic<bool> g_open{false};
std::atomic<std::int64_t> g_notice_until{0};
std::string g_notice;  // present thread (the overlay's mutex)
std::atomic<bool> g_notice_set{false};

std::int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// ---- Data: the area files ----
struct Slot {
    bbmap::Area area;  // header; layers and lamps once `ready`
    fs::path file;
    std::future<bbmap::Area> job;
    bool loading = false, ready = false, failed = false;
    std::vector<int> order;                      // layer indices by anchor, lowest first
    std::vector<std::array<float, 4>> ring_box;  // per ring of all layers, in layer order
    std::vector<std::size_t> ring_first;         // first ring_box index of each layer
};
std::vector<Slot> g_areas;
bool g_scanned = false;
std::int64_t g_scan_at = 0;

fs::path DataDir() {
    if (const char* e = std::getenv("BB_MAP_DIR"); e && *e) return e;
    if (const char* u = std::getenv("BB_USER_DIR"); u && *u) return fs::path(u) / "map";
    if (const char* d = std::getenv("BB_DATA_DIR"); d && *d) return fs::path(d) / "user" / "map";
    return fs::path("user") / "map";
}

bool ReadFile(const fs::path& p, std::string* out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    if (size <= 0 || size > (512ll << 20)) return false;
    f.seekg(0);
    out->resize(static_cast<std::size_t>(size));
    return static_cast<bool>(f.read(out->data(), size));
}

void Scan() {
    g_scanned = true;
    g_scan_at = NowMs();
    g_areas.clear();
    std::error_code ec;
    const fs::path dir = DataDir();
    if (!fs::exists(dir / "map.stamp", ec)) return;  // the baker writes it last
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.path().extension() != ".bbm") continue;
        std::ifstream f(e.path(), std::ios::binary);
        std::string head(4096, '\0');
        f.read(head.data(), 4096);
        head.resize(static_cast<std::size_t>(f.gcount()));
        Slot s;
        if (!bbmap::ParseHeader(reinterpret_cast<const unsigned char*>(head.data()), head.size(), &s.area)) continue;
        s.file = e.path();
        g_areas.push_back(std::move(s));
    }
    std::sort(g_areas.begin(), g_areas.end(), [](const Slot& a, const Slot& b) { return a.area.packed < b.area.packed; });
}

bbmap::Area LoadJob(fs::path file) {
    std::string bytes;
    bbmap::Area a;
    if (!ReadFile(file, &bytes) || !bbmap::Parse(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), &a))
        a.loaded = false;
    return a;
}

void Finish(Slot& s) {
    s.loading = false;
    bbmap::Area a = s.job.get();
    if (!a.loaded) {
        s.failed = true;
        std::printf("Map: %s could not be read\n", s.file.string().c_str());
        return;
    }
    s.area = std::move(a);
    s.order.resize(s.area.layers.size());
    for (std::size_t i = 0; i < s.order.size(); ++i) s.order[i] = static_cast<int>(i);
    std::stable_sort(s.order.begin(), s.order.end(),
                     [&](int x, int y) { return s.area.layers[x].anchor < s.area.layers[y].anchor; });
    s.ring_box.clear();
    s.ring_first.clear();
    for (const auto& l : s.area.layers) {
        s.ring_first.push_back(s.ring_box.size());
        for (const auto& ring : l.rings) {
            std::array<float, 4> b{1e30f, 1e30f, -1e30f, -1e30f};
            for (std::size_t k = 0; k + 1 < ring.size(); k += 2) {
                b[0] = std::min(b[0], ring[k]), b[2] = std::max(b[2], ring[k]);
                b[1] = std::min(b[1], ring[k + 1]), b[3] = std::max(b[3], ring[k + 1]);
            }
            s.ring_box.push_back(b);
        }
    }
    s.ready = true;
}

/// Starts / advances the load of area `i`; true when it can be drawn.
bool Ensure(int i) {
    if (i < 0 || i >= static_cast<int>(g_areas.size())) return false;
    Slot& s = g_areas[static_cast<std::size_t>(i)];
    if (s.ready) return true;
    if (s.failed) return false;
    if (!s.loading) {
        s.loading = true;
        s.job = std::async(std::launch::async, LoadJob, s.file);
    } else if (s.job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        Finish(s);
    }
    return s.ready;
}

/// The shown-area index whose block is `map_id`'s (-1 none).
int AreaIndexOfMap(std::uint32_t map_id) {
    if (map_id == 0 || map_id == 0xffffffffu) return -1;
    for (std::size_t i = 0; i < g_areas.size(); ++i)
        if ((g_areas[i].area.packed & 0xffff0000u) == (map_id & 0xffff0000u)) return static_cast<int>(i);
    return -1;
}

// ---- State ----
bbmap::View g_view;
int g_area = -1;          // shown area (index into g_areas)
int g_manual_layer = -1;  // a layer picked by the player (index into order), -1 none
bool g_follow = true;
int g_selected = -1;      // lamp (index into the shown area's lamps)
int g_dialog = -1;        // lamp the travel question is about
bbmap::Pt g_mouse{};
bool g_mouse_in = false;
bool g_dragging = false, g_dragged = false;
bbmap::Pt g_drag_last{};
bool g_click = false;     // a left click (no drag) to act on in Draw
std::set<SDL_Keycode> g_keys;
float g_pad[4] = {};      // left stick x, y, right stick x, y
int g_cmd_area = 0, g_cmd_layer = 0;
bool g_cmd_recenter = false, g_cmd_confirm = false, g_cmd_back = false;
int g_cmd_nav_x = 0, g_cmd_nav_y = 0;
float g_cmd_wheel = 0;
bool g_view_ready = false;  // the view was fitted for g_area
std::int64_t g_pose_lost_ms = 0, g_opened_ms = 0;
std::string g_message;      // under the map: the last travel answer
std::int64_t g_message_until = 0;
bool g_use_pad_glyphs = false;
BbMapControllerChord g_chord;

// Test driver (BB_MAP_TEST): "open", "travel=ID|any", "delay=MS" (first action after the world is up).
struct TestScript {
    bool parsed = false, on = false, open = false, travel = false, members = false;
    std::uint32_t to = 0;
    std::int64_t delay_ms = 4000, world_since = 0, opened_at = 0, travelled_at = 0, closed_at = 0;
    bool travel_done = false;
    int close_after_ms = 0;
} g_test;

void ParseTest() {
    g_test.parsed = true;
    const char* v = std::getenv("BB_MAP_TEST");
    if (!v || !*v) return;
    g_test.on = true;
    std::string s = v;
    std::size_t at = 0;
    while (at <= s.size()) {
        std::size_t end = s.find(',', at);
        if (end == std::string::npos) end = s.size();
        const std::string part = s.substr(at, end - at);
        if (part == "open") g_test.open = true;
        else if (part == "members") g_test.members = true;  // not before a party member's position is known
        else if (part.rfind("delay=", 0) == 0) g_test.delay_ms = std::atoll(part.c_str() + 6);
        else if (part.rfind("close=", 0) == 0) g_test.close_after_ms = std::atoi(part.c_str() + 6);
        else if (part.rfind("travel=", 0) == 0) {
            g_test.travel = true;
            g_test.open = true;
            g_test.to = part.substr(7) == "any" ? 0 : static_cast<std::uint32_t>(std::strtoul(part.c_str() + 7, nullptr, 10));
        }
        at = end + 1;
    }
}


void SetNotice(const std::string& text, int ms = 4500) {
    g_notice = text;
    g_notice_set = true;
    g_notice_until = NowMs() + ms;
}

bool PoseNow(coop::MapPose* p) { return coop::MapSelfPose(p); }

const char* TravelReason(coop::lamptravel::MapTravel s) {
    using coop::lamptravel::MapTravel;
    switch (s) {
    case MapTravel::Ready: return "";
    case MapTravel::Off:
        return BbSettings::MenuText("Lamp travel is off (launcher: Direct lamp travel)",
                                    "Переход между фонарями выключен (лаунчер: Direct lamp travel)");
    case MapTravel::NotInWorld: return BbSettings::MenuText("Not in the world right now", "Сейчас не в мире");
    case MapTravel::Cutscene: return BbSettings::MenuText("Not during a cutscene", "Не во время ролика");
    case MapTravel::Boss: return BbSettings::MenuText("Not during a boss fight", "Не во время боя с боссом");
    case MapTravel::Chalice: return BbSettings::MenuText("Not in a Chalice Dungeon", "Не в подземелье Чаши");
    case MapTravel::Dead: return BbSettings::MenuText("Not while dead", "Не пока вы мертвы");
    case MapTravel::Busy: return BbSettings::MenuText("A travel is already under way", "Переход уже идёт");
    }
    return "";
}

}  // namespace

bool Enabled() {
    if (const char* e = std::getenv("BB_MAP"); e && *e) return e[0] != '0';
    return BbSettings::Get().map_enabled.load();
}
bool IsOpen() { return g_open.load(std::memory_order_acquire); }
bool NoticeShown() { return g_notice_set.load() && NowMs() < g_notice_until.load(); }

void Close() {
    g_open = false;
    g_keys.clear();
    g_dragging = g_dragged = g_click = false;
    g_dialog = -1;
    g_pad[0] = g_pad[1] = g_pad[2] = g_pad[3] = 0;
    g_cmd_area = g_cmd_layer = g_cmd_nav_x = g_cmd_nav_y = 0;
    g_cmd_recenter = g_cmd_confirm = g_cmd_back = false;
    g_cmd_wheel = 0;
}

void Open() {
    if (!Enabled()) return;
    coop::MapPose pose;
    if (!PoseNow(&pose)) {
        SetNotice(BbSettings::MenuText("The map opens in the world, not on the title or in a loading screen.",
                                       "Карта открывается в мире, не на титульном экране и не при загрузке."));
        return;
    }
    if (!g_scanned || (g_areas.empty() && NowMs() - g_scan_at > 1000)) Scan();
    if (g_areas.empty()) {
        SetNotice(BbSettings::MenuText(
            "No map data yet. In the launcher press \"Prepare the map\" (it reads your own game files once).",
            "Данных карты пока нет. В лаунчере нажмите \"Prepare the map\" (один раз читает ваши файлы игры)."));
        return;
    }
    const int here = AreaIndexOfMap(pose.map_id);
    if (here < 0) {
        // A Chalice Dungeon and the like: the generator maps the 16 areas of the world.
        SetNotice(BbSettings::MenuText("There is no map of this place.", "Для этого места нет карты."));
        return;
    }
    g_area = here;
    g_follow = true;
    g_manual_layer = -1;
    g_selected = -1;
    g_dialog = -1;
    g_view_ready = false;
    g_pose_lost_ms = 0;
    g_opened_ms = NowMs();
    g_message.clear();
    g_open = true;
    std::printf("Map: opened (area %s)\n", g_areas[static_cast<std::size_t>(g_area)].area.id.c_str());
}

bool HandleEvent(const SDL_Event& e, float density) {
    // The controller shortcut (hold L1 + R1 + Start): its buttons are consumed once the chord is complete.
    g_chord.SetEnabled(Enabled());
    const bool chord_eats = g_chord.HandleEvent(e);
    if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || e.type == SDL_EVENT_GAMEPAD_BUTTON_UP) g_use_pad_glyphs = true;
    if (!Enabled()) {
        if (IsOpen()) Close();
        return false;
    }
    if (!IsOpen()) {
        if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_M &&
            !(e.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT))) {
            g_use_pad_glyphs = false;
            Open();
            return true;
        }
        return chord_eats;
    }
    switch (e.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const bool down = e.type == SDL_EVENT_KEY_DOWN;
        g_use_pad_glyphs = false;
        if (e.key.key == SDLK_INSERT) return false;  // the settings menu (it closes the map when it opens)
        if (down) g_keys.insert(e.key.key);
        else g_keys.erase(e.key.key);
        if (!down || e.key.repeat) return true;
        switch (e.key.key) {
        case SDLK_M:
        case SDLK_ESCAPE:
            g_cmd_back = true;
            break;
        case SDLK_TAB: g_cmd_area += (e.key.mod & SDL_KMOD_SHIFT) ? -1 : 1; break;
        case SDLK_Q:
        case SDLK_PAGEDOWN: g_cmd_layer -= 1; break;
        case SDLK_E:
        case SDLK_PAGEUP: g_cmd_layer += 1; break;
        case SDLK_SPACE:
        case SDLK_F: g_cmd_recenter = true; break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER: g_cmd_confirm = true; break;
        case SDLK_EQUALS:
        case SDLK_PLUS:
        case SDLK_KP_PLUS: g_cmd_wheel += 1; break;
        case SDLK_MINUS:
        case SDLK_KP_MINUS: g_cmd_wheel -= 1; break;
        default: break;
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        if (e.type == SDL_EVENT_GAMEPAD_BUTTON_UP) return true;
        switch (e.gbutton.button) {
        case SDL_GAMEPAD_BUTTON_EAST: g_cmd_back = true; break;           // Circle / B
        case SDL_GAMEPAD_BUTTON_SOUTH: g_cmd_confirm = true; break;       // Cross / A
        case SDL_GAMEPAD_BUTTON_NORTH: g_cmd_recenter = true; break;      // Triangle / Y
        case SDL_GAMEPAD_BUTTON_WEST: g_cmd_area += 1; break;             // Square / X
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: g_cmd_layer -= 1; break;
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: g_cmd_layer += 1; break;
        case SDL_GAMEPAD_BUTTON_DPAD_UP: g_cmd_nav_y -= 1; break;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: g_cmd_nav_y += 1; break;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: g_cmd_nav_x -= 1; break;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: g_cmd_nav_x += 1; break;
        default: break;
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        const float v = e.gaxis.value / 32767.0f;
        const float dead = std::fabs(v) < 0.18f ? 0.0f : v;
        switch (e.gaxis.axis) {
        case SDL_GAMEPAD_AXIS_LEFTX: g_pad[0] = dead; break;
        case SDL_GAMEPAD_AXIS_LEFTY: g_pad[1] = dead; break;
        case SDL_GAMEPAD_AXIS_RIGHTX: g_pad[2] = dead; break;
        case SDL_GAMEPAD_AXIS_RIGHTY: g_pad[3] = dead; break;
        default: break;
        }
        return false;  // the game's input is held neutral while the map is open
    }
    case SDL_EVENT_MOUSE_MOTION: {
        g_use_pad_glyphs = false;
        g_mouse = {e.motion.x * density, e.motion.y * density};
        g_mouse_in = true;
        if (g_dragging) {
            const float dx = g_mouse.x - g_drag_last.x, dy = g_mouse.y - g_drag_last.y;
            if (g_dragged || std::fabs(dx) + std::fabs(dy) > 3.0f) {
                g_dragged = true;
                bbmap::PanPixels(&g_view, dx, dy);
                g_follow = false;
                g_drag_last = g_mouse;
            }
        }
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        g_use_pad_glyphs = false;
        g_mouse = {e.button.x * density, e.button.y * density};
        g_mouse_in = true;
        if (e.button.button == SDL_BUTTON_LEFT) {
            if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                g_dragging = true;
                g_dragged = false;
                g_drag_last = g_mouse;
            } else {
                if (g_dragging && !g_dragged) g_click = true;
                g_dragging = false;
            }
        } else if (e.button.button == SDL_BUTTON_RIGHT && e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
            g_cmd_back = true;
        }
        return true;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        g_cmd_wheel += e.wheel.y;
        return true;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        g_keys.clear();
        g_dragging = false;
        g_pad[0] = g_pad[1] = g_pad[2] = g_pad[3] = 0;
        return false;
    default:
        return false;
    }
}

void PollWindowThread() {
    if (Enabled() && g_chord.Tick(SDL_GetTicks()) && !IsOpen()) {
        g_use_pad_glyphs = true;
        Open();
    }
}

namespace {

ImU32 Col(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }

/// One party slot's colour.
ImU32 SlotColor(int slot) {
    static const ImU32 c[5] = {IM_COL32(222, 168, 82, 255), IM_COL32(112, 178, 224, 255), IM_COL32(150, 206, 118, 255),
                               IM_COL32(214, 122, 168, 255), IM_COL32(178, 150, 224, 255)};
    return c[(slot < 0 ? 0 : slot) % 5];
}

void DrawText(ImDrawList* d, float size, Pt at, ImU32 col, const char* text, bool shadow = true) {
    ImFont* font = ImGui::GetFont();
    if (shadow) d->AddText(font, size, ImVec2(at.x + size * 0.07f, at.y + size * 0.07f), Col(0, 0, 0, 200), text);
    d->AddText(font, size, ImVec2(at.x, at.y), col, text);
}
Pt TextSize(float size, const char* text) {
    const ImVec2 v = ImGui::GetFont()->CalcTextSizeA(size, 1e9f, 0.0f, text);
    return {v.x, v.y};
}

/// An arrow (the player): tip toward `dir` (screen unit vector).
void DrawArrow(ImDrawList* d, Pt c, Pt dir, float r, ImU32 fill, ImU32 edge) {
    const Pt n{-dir.y, dir.x};
    const ImVec2 tip{c.x + dir.x * r, c.y + dir.y * r};
    const ImVec2 l{c.x - dir.x * r * 0.7f + n.x * r * 0.7f, c.y - dir.y * r * 0.7f + n.y * r * 0.7f};
    const ImVec2 rr{c.x - dir.x * r * 0.7f - n.x * r * 0.7f, c.y - dir.y * r * 0.7f - n.y * r * 0.7f};
    const ImVec2 back{c.x - dir.x * r * 0.25f, c.y - dir.y * r * 0.25f};
    d->AddQuadFilled(tip, l, back, rr, fill);
    d->AddQuad(tip, l, back, rr, edge, std::max(1.0f, r * 0.12f));
}

void DrawLamp(ImDrawList* d, Pt p, float r, bool available, bool selected, bool hovered) {
    const ImU32 gold = available ? Col(238, 202, 118) : Col(150, 140, 120);
    if (selected || hovered) d->AddCircle(ImVec2(p.x, p.y), r * 1.9f, Col(255, 236, 170, selected ? 255 : 170), 28, std::max(1.5f, r * 0.2f));
    d->AddQuadFilled(ImVec2(p.x, p.y - r * 1.25f), ImVec2(p.x + r, p.y), ImVec2(p.x, p.y + r * 1.25f), ImVec2(p.x - r, p.y), gold);
    d->AddQuad(ImVec2(p.x, p.y - r * 1.25f), ImVec2(p.x + r, p.y), ImVec2(p.x, p.y + r * 1.25f), ImVec2(p.x - r, p.y), Col(30, 22, 12), std::max(1.0f, r * 0.15f));
}

/// Floors of one layer as one filled mesh (no antialiasing: shared edges leave no seams), in chunks that
/// stay within one draw command's 16-bit vertex range.
void DrawFloors(ImDrawList* d, const bbmap::Layer& l, const bbmap::View& v, ImU32 col) {
    const float wx0 = v.cx - v.w * 0.5f / v.zoom, wx1 = v.cx + v.w * 0.5f / v.zoom;
    const float wz0 = v.cz - v.h * 0.5f / v.zoom, wz1 = v.cz + v.h * 0.5f / v.zoom;
    const ImVec2 uv = ImGui::GetDrawListSharedData()->TexUvWhitePixel;
    const std::size_t n = l.TriCount();
    constexpr std::size_t kChunk = 6000;  // triangles per reserve: 18k vertices
    std::size_t i = 0;
    std::vector<const float*> visible;
    while (i < n) {
        visible.clear();
        for (; i < n && visible.size() < kChunk; ++i) {
            const float* t = &l.tris[i * 7];
            if (std::max({t[0], t[2], t[4]}) < wx0 || std::min({t[0], t[2], t[4]}) > wx1 ||
                std::max({t[1], t[3], t[5]}) < wz0 || std::min({t[1], t[3], t[5]}) > wz1)
                continue;
            visible.push_back(t);
        }
        if (visible.empty()) continue;
        d->PrimReserve(static_cast<int>(visible.size() * 3), static_cast<int>(visible.size() * 3));
        const unsigned base = d->_VtxCurrentIdx;
        unsigned k = 0;
        for (const float* t : visible) {
            for (int c = 0; c < 3; ++c) {
                const Pt p = bbmap::ToScreen(v, t[c * 2], t[c * 2 + 1]);
                d->PrimWriteVtx(ImVec2(p.x, p.y), uv, col);
                d->PrimWriteIdx(static_cast<ImDrawIdx>(base + k++));
            }
        }
    }
}

}  // namespace

namespace {

int LayerIndexOf(const Slot& s, int layer) {
    for (std::size_t i = 0; i < s.order.size(); ++i)
        if (s.order[i] == layer) return static_cast<int>(i);
    return 0;
}

bool InRect(Pt p, float x, float y, float w, float h) { return p.x >= x && p.x <= x + w && p.y >= y && p.y <= y + h; }

/// Asks the game for the travel (the lamp warp); true when it took the request.
bool TravelTo(const bbmap::Lamp& lamp, const char* how) {
    const bool ok = coop::lamptravel::RequestMapTravel(lamp.return_point);
    std::printf("Map: travel to %s (lamp %u) %s (%s)\n", lamp.name.c_str(), lamp.return_point,
                ok ? "asked" : "NOT asked", how);
    return ok;
}

}  // namespace

void TestTick() {}

void Draw(float dw, float dh) {
    ImDrawList* d = ImGui::GetForegroundDrawList();
    const float sc = std::max(0.75f, dh / 1080.0f);
    const std::int64_t now = NowMs();
    if (!g_test.parsed) ParseTest();

    // Test driver (BB_MAP_TEST): open and travel without a keyboard.
    bool test_travel = false;
    if (g_test.on) {
        coop::MapPose p;
        bool world = PoseNow(&p);
        if (world && g_test.members) {
            party::hud::Board b;
            party::hud::Read(&b);
            world = b.active && b.count > 0 && b.c[0].have_pos;
        }
        if (!world) g_test.world_since = 0;
        else if (!g_test.world_since) g_test.world_since = now;
        if (world && !IsOpen() && g_test.open && !g_test.opened_at && now - g_test.world_since >= g_test.delay_ms) {
            g_test.opened_at = now;
            Open();
        }
        if (IsOpen() && g_test.travel && !g_test.travel_done && g_test.opened_at && now - g_test.opened_at >= 6000) {
            // The lamp may be in another area than the one shown: switch to it (as Tab does), draw it, then travel.
            const int want = g_test.to ? AreaIndexOfMap(coop::lamptravel::MapOf(g_test.to)) : -1;
            if (want >= 0 && want != g_area) {
                g_area = want;
                g_view_ready = false;
                g_manual_layer = -1;
                g_selected = g_dialog = -1;
                g_follow = false;
            } else {
                test_travel = true;
            }
        }
        if (IsOpen() && g_test.close_after_ms && g_test.opened_at && !g_test.closed_at &&
            now - g_test.opened_at >= g_test.close_after_ms) {
            g_test.closed_at = now;
            Close();
            std::printf("Map: test closed the map\n");
        }
    }

    if (!IsOpen()) {
        if (NoticeShown()) {
            const float size = 22 * sc;
            const Pt ts = TextSize(size, g_notice.c_str());
            const float w = ts.x + 40 * sc, h = ts.y + 26 * sc, x = (dw - w) * 0.5f, y = dh * 0.12f;
            d->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), Col(20, 17, 14, 235), 6 * sc);
            d->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), Col(200, 170, 110, 200), 6 * sc, 0, 1.5f * sc);
            DrawText(d, size, {x + 20 * sc, y + 13 * sc}, Col(238, 226, 196), g_notice.c_str());
        }
        return;
    }

    // ---- State for this frame ----
    const float dt = std::min(0.1f, std::max(0.001f, ImGui::GetIO().DeltaTime));
    coop::MapPose pose;
    const bool have_pose = PoseNow(&pose);
    if (!have_pose) {
        if (!g_pose_lost_ms) g_pose_lost_ms = now;
        if (now - g_pose_lost_ms > 1500) {  // a loading screen: the map goes
            Close();
            return;
        }
    } else {
        g_pose_lost_ms = 0;
    }
    if (g_areas.empty() || g_area < 0 || g_area >= static_cast<int>(g_areas.size())) {
        Close();
        return;
    }
    if (g_cmd_back) {
        g_cmd_back = false;
        if (g_dialog >= 0) {
            g_dialog = -1;
        } else {
            Close();
            std::printf("Map: closed\n");
            return;
        }
    }
    if (g_cmd_area) {
        const int n = static_cast<int>(g_areas.size());
        g_area = ((g_area + g_cmd_area) % n + n) % n;
        g_cmd_area = 0;
        g_view_ready = false;
        g_manual_layer = -1;
        g_selected = g_dialog = -1;
        g_follow = false;
    }
    Slot& slot = g_areas[static_cast<std::size_t>(g_area)];
    const bool ready = Ensure(g_area);
    const bbmap::Area& area = slot.area;
    const bool player_here = have_pose && (area.packed & 0xffff0000u) == (pose.map_id & 0xffff0000u);

    // The map rectangle.
    const float margin = 14 * sc, head = 74 * sc, foot = 40 * sc;
    g_view.x0 = margin;
    g_view.y0 = head;
    g_view.w = dw - 2 * margin;
    g_view.h = dh - head - foot;
    if (!g_view_ready) {
        bbmap::Fit(&g_view, area.bounds);
        g_view_ready = true;
        if (player_here) {
            g_view.zoom = std::max(g_view.zoom, 4.0f * sc);
            g_view.cx = pose.pos[0];
            g_view.cz = pose.pos[2];
        }
    }

    // Layer: the player's floor, or the picked one.
    int layer = -1;
    if (ready && !area.layers.empty()) {
        if (g_cmd_layer && !slot.order.empty()) {
            int cur = g_manual_layer >= 0
                          ? g_manual_layer
                          : LayerIndexOf(slot, player_here ? bbmap::PickLayer(area, pose.pos[0], pose.pos[1], pose.pos[2])
                                                           : bbmap::MainLayer(area));
            cur = std::min(std::max(cur + g_cmd_layer, 0), static_cast<int>(slot.order.size()) - 1);
            g_manual_layer = cur;
        }
        g_cmd_layer = 0;
        if (g_manual_layer >= 0) layer = slot.order[static_cast<std::size_t>(g_manual_layer)];
        else if (player_here) layer = bbmap::PickLayer(area, pose.pos[0], pose.pos[1], pose.pos[2]);
        else layer = bbmap::MainLayer(area);
    } else {
        g_cmd_layer = 0;
    }

    // Travel state and the lit lamps (the main thread's snapshot).
    coop::lamptravel::MapTravel travel_state = coop::lamptravel::MapTravel::Off;
    std::vector<std::uint32_t> lit;
    const bool watching = coop::lamptravel::MapWatch(&travel_state, &lit);
    std::vector<bool> shown(area.lamps.size(), false), can_travel(area.lamps.size(), false);
    for (std::size_t i = 0; i < area.lamps.size(); ++i) {
        const auto& l = area.lamps[i];
        const bool is_lit = std::find(lit.begin(), lit.end(), l.return_point) != lit.end();
        shown[i] = is_lit || l.return_point == 2102950;
        can_travel[i] = is_lit && watching && travel_state == coop::lamptravel::MapTravel::Ready;
    }

    // ---- Input ----
    {
        float px = 0, py = 0;
        auto held = [&](SDL_Keycode k) { return g_keys.count(k) != 0; };
        if (held(SDLK_LEFT) || held(SDLK_A)) px -= 1;
        if (held(SDLK_RIGHT) || held(SDLK_D)) px += 1;
        if (held(SDLK_UP) || held(SDLK_W)) py -= 1;
        if (held(SDLK_DOWN) || held(SDLK_S)) py += 1;
        px += g_pad[0];
        py += g_pad[1];
        if (g_dialog < 0 && (px != 0 || py != 0)) {
            const float speed = 600.0f * sc * dt;
            bbmap::PanPixels(&g_view, -px * speed, -py * speed);
            g_follow = false;
        }
        if (g_dialog < 0 && g_pad[3] != 0)
            bbmap::ZoomAt(&g_view, {g_view.x0 + g_view.w * 0.5f, g_view.y0 + g_view.h * 0.5f},
                          std::pow(2.2f, -g_pad[3] * dt), area.bounds);
        if (g_cmd_wheel != 0) {
            if (g_dialog < 0) {
                const Pt at = g_mouse_in && !g_use_pad_glyphs ? g_mouse
                                                               : Pt{g_view.x0 + g_view.w * 0.5f, g_view.y0 + g_view.h * 0.5f};
                bbmap::ZoomAt(&g_view, at, std::pow(1.25f, g_cmd_wheel), area.bounds);
            }
            g_cmd_wheel = 0;
        }
        if (g_cmd_recenter && player_here) {
            g_follow = true;
            g_manual_layer = -1;
        }
        g_cmd_recenter = false;
        if (g_follow && player_here) {
            g_view.cx = pose.pos[0];
            g_view.cz = pose.pos[2];
        }
        bbmap::ClampCentre(&g_view, area.bounds);
    }

    // Lamp selection with the pad's d-pad: toward a lamp in that direction.
    if ((g_cmd_nav_x || g_cmd_nav_y) && ready && g_dialog < 0) {
        if (g_selected < 0 || g_selected >= static_cast<int>(area.lamps.size())) {
            float best = 1e30f;
            g_selected = -1;
            for (std::size_t i = 0; i < area.lamps.size(); ++i) {
                if (!shown[i]) continue;
                const Pt p = bbmap::ToScreen(g_view, area.lamps[i].x, area.lamps[i].z);
                const float dx = p.x - (g_view.x0 + g_view.w * 0.5f), dy = p.y - (g_view.y0 + g_view.h * 0.5f);
                if (dx * dx + dy * dy < best) best = dx * dx + dy * dy, g_selected = static_cast<int>(i);
            }
        } else {
            const int next = bbmap::NextLamp(area.lamps, shown, g_view, g_selected,
                                             {static_cast<float>(g_cmd_nav_x), static_cast<float>(g_cmd_nav_y)});
            if (next >= 0) g_selected = next;
        }
        if (g_selected >= 0) {
            const bbmap::Lamp& sl = area.lamps[static_cast<std::size_t>(g_selected)];
            const Pt p = bbmap::ToScreen(g_view, sl.x, sl.z);
            if (p.x < g_view.x0 + 40 * sc || p.x > g_view.x0 + g_view.w - 40 * sc || p.y < g_view.y0 + 40 * sc ||
                p.y > g_view.y0 + g_view.h - 40 * sc) {
                g_view.cx = sl.x;
                g_view.cz = sl.z;
                g_follow = false;
            }
        }
    }
    g_cmd_nav_x = g_cmd_nav_y = 0;

    const int hover = (ready && g_mouse_in && !g_use_pad_glyphs && g_dialog < 0)
                          ? bbmap::LampAt(area.lamps, shown, g_view, g_mouse, 15 * sc)
                          : -1;

    // ---- The scene ----
    d->AddRectFilled(ImVec2(0, 0), ImVec2(dw, dh), Col(8, 6, 5, 232));
    d->AddRectFilled(ImVec2(g_view.x0, g_view.y0), ImVec2(g_view.x0 + g_view.w, g_view.y0 + g_view.h), Col(30, 25, 20, 255));
    d->PushClipRect(ImVec2(g_view.x0, g_view.y0), ImVec2(g_view.x0 + g_view.w, g_view.y0 + g_view.h), true);
    if (ready && layer >= 0) {
        const bbmap::Layer& l = area.layers[static_cast<std::size_t>(layer)];
        const int pos = LayerIndexOf(slot, layer);
        // The floor below the shown one, faint, so the map reads as a building.
        if (pos > 0)
            DrawFloors(d, area.layers[static_cast<std::size_t>(slot.order[static_cast<std::size_t>(pos - 1)])], g_view, Col(60, 53, 44, 255));
        DrawFloors(d, l, g_view, Col(104, 92, 72, 255));
        const std::size_t first = slot.ring_first[static_cast<std::size_t>(layer)];
        const float wx0 = g_view.cx - g_view.w * 0.5f / g_view.zoom, wx1 = g_view.cx + g_view.w * 0.5f / g_view.zoom;
        const float wz0 = g_view.cz - g_view.h * 0.5f / g_view.zoom, wz1 = g_view.cz + g_view.h * 0.5f / g_view.zoom;
        std::vector<ImVec2> pts;
        for (std::size_t r = 0; r < l.rings.size(); ++r) {
            const auto& b = slot.ring_box[first + r];
            if (b[2] < wx0 || b[0] > wx1 || b[3] < wz0 || b[1] > wz1) continue;
            const auto& ring = l.rings[r];
            pts.clear();
            for (std::size_t k = 0; k + 1 < ring.size(); k += 2) {
                const Pt p = bbmap::ToScreen(g_view, ring[k], ring[k + 1]);
                pts.emplace_back(p.x, p.y);
            }
            if (pts.size() > 1)
                d->AddPolyline(pts.data(), static_cast<int>(pts.size()), Col(214, 190, 138, 255), ImDrawFlags_None, std::max(1.0f, 1.4f * sc));
        }
        for (std::size_t k = 0; k + 3 < l.walls.size(); k += 4) {
            const float* w = &l.walls[k];
            if (std::max(w[0], w[2]) < wx0 || std::min(w[0], w[2]) > wx1 || std::max(w[1], w[3]) < wz0 || std::min(w[1], w[3]) > wz1)
                continue;
            const Pt a = bbmap::ToScreen(g_view, w[0], w[1]), b2 = bbmap::ToScreen(g_view, w[2], w[3]);
            d->AddLine(ImVec2(a.x, a.y), ImVec2(b2.x, b2.y), Col(170, 146, 104, 255), std::max(1.0f, 1.0f * sc));
        }
    }
    // Lamps.
    const float lr = 6.5f * sc;
    for (std::size_t i = 0; i < area.lamps.size(); ++i) {
        if (!shown[i]) continue;
        const Pt p = bbmap::ToScreen(g_view, area.lamps[i].x, area.lamps[i].z);
        if (p.x < g_view.x0 - 20 || p.x > g_view.x0 + g_view.w + 20 || p.y < g_view.y0 - 20 || p.y > g_view.y0 + g_view.h + 20)
            continue;
        const bool sel = static_cast<int>(i) == g_selected, hov = static_cast<int>(i) == hover;
        DrawLamp(d, p, lr, can_travel[i], sel, hov);
        if (sel || hov || g_view.zoom > 7.0f * sc)
            DrawText(d, 15 * sc, {p.x + lr * 1.6f, p.y - 8 * sc}, Col(244, 232, 200), area.lamps[i].name.c_str());
    }
    // Party members on this map, then this player.
    party::hud::Board board;
    party::hud::Read(&board);
    for (int i = 0; board.active && i < board.count; ++i) {
        const party::hud::Companion& c = board.c[i];
        if (!c.have_pos || !c.connected) continue;
        if ((c.map_id & 0xffff0000u) != (area.packed & 0xffff0000u)) continue;
        const Pt p = bbmap::ToScreen(g_view, c.pos[0], c.pos[2]);
        const bool same_floor = !ready || layer < 0 || bbmap::PickLayer(area, c.pos[0], c.pos[1], c.pos[2]) == layer;
        const ImU32 col = SlotColor(c.slot);
        const ImU32 fill = same_floor ? col : ((col & 0x00ffffffu) | (110u << 24));
        d->AddCircleFilled(ImVec2(p.x, p.y), 7.5f * sc, Col(10, 8, 6, same_floor ? 255 : 120), 20);
        d->AddCircleFilled(ImVec2(p.x, p.y), 5.5f * sc, fill, 20);
        d->AddCircle(ImVec2(p.x, p.y), 8.5f * sc, Col(255, 255, 255, same_floor ? 220 : 90), 20, std::max(1.0f, 1.2f * sc));
        DrawText(d, 15 * sc, {p.x + 11 * sc, p.y - 8 * sc}, same_floor ? col : ((col & 0x00ffffffu) | (150u << 24)), c.name);
    }
    {
        // One line a second while the map is open: what is drawn (the smoke checks the dots move).
        static std::int64_t logged = 0;
        if (now - logged >= 1000) {
            logged = now;
            std::string line;
            for (int i = 0; board.active && i < board.count; ++i) {
                const party::hud::Companion& c = board.c[i];
                if (!c.have_pos || (c.map_id & 0xffff0000u) != (area.packed & 0xffff0000u)) continue;
                char t[96];
                std::snprintf(t, sizeof t, " | %s (%.1f %.1f)", c.name, c.pos[0], c.pos[2]);
                line += t;
            }
            std::printf("Map: shown %s floor %d, me (%.1f %.1f)%s%s\n", area.id.c_str(), layer,
                        double(pose.pos[0]), double(pose.pos[2]), player_here ? "" : " (elsewhere)", line.c_str());
        }
    }
    if (player_here) {
        const Pt p = bbmap::ToScreen(g_view, pose.pos[0], pose.pos[2]);
        DrawArrow(d, p, bbmap::FacingOnScreen(pose.yaw), 11 * sc, Col(176, 214, 206), Col(14, 22, 20));
    }
    d->PopClipRect();
    d->AddRect(ImVec2(g_view.x0, g_view.y0), ImVec2(g_view.x0 + g_view.w, g_view.y0 + g_view.h), Col(160, 132, 84, 255), 0, 0, 2 * sc);

    // Title and floor.
    {
        const float ts = 34 * sc;
        const std::string title = area.name.empty() ? area.id : area.name;
        const Pt sz = TextSize(ts, title.c_str());
        DrawText(d, ts, {(dw - sz.x) * 0.5f, 6 * sc}, Col(238, 214, 150), title.c_str());
        if (ready && layer >= 0) {
            char buf[96];
            std::snprintf(buf, sizeof buf, BbSettings::MenuText("Floor %d / %d%s", "Этаж %d / %d%s"),
                          LayerIndexOf(slot, layer) + 1, static_cast<int>(slot.order.size()),
                          (player_here && g_manual_layer < 0) ? BbSettings::MenuText("  (yours)", "  (ваш)") : "");
            const Pt s2 = TextSize(17 * sc, buf);
            DrawText(d, 17 * sc, {(dw - s2.x) * 0.5f, 6 * sc + ts + 2 * sc}, Col(190, 170, 130), buf);
        } else if (!ready) {
            const char* t = slot.failed ? BbSettings::MenuText("This area's data could not be read", "Не удалось прочитать данные области")
                                        : BbSettings::MenuText("Loading...", "Загрузка...");
            const Pt s2 = TextSize(20 * sc, t);
            DrawText(d, 20 * sc, {g_view.x0 + (g_view.w - s2.x) * 0.5f, g_view.y0 + g_view.h * 0.5f}, Col(214, 196, 150), t);
        }
    }
    // Footer: the keys, and the last answer.
    {
        const float fs = 16 * sc;
        const char* keys =
            g_use_pad_glyphs
                ? BbSettings::MenuText("Left stick: move   Right stick: zoom   D-pad: lamp   Cross: travel   L1/R1: floor   Square: area   Triangle: me   Circle: close",
                                       "Лев. стик: сдвиг   Прав. стик: масштаб   Крест: фонарь   Cross: переход   L1/R1: этаж   Square: область   Triangle: я   Circle: закрыть")
                : BbSettings::MenuText("Drag/WASD: move   Wheel: zoom   Click a lamp: travel   Q/E: floor   Tab: area   Space: me   M/Esc: close",
                                       "Тяните/WASD: сдвиг   Колесо: масштаб   Клик по фонарю: переход   Q/E: этаж   Tab: область   Пробел: я   M/Esc: закрыть");
        const Pt ks = TextSize(fs, keys);
        DrawText(d, fs, {(dw - ks.x) * 0.5f, dh - foot + 10 * sc}, Col(190, 170, 130), keys);
        if (!g_message.empty() && now < g_message_until) {
            const Pt ms = TextSize(fs * 1.1f, g_message.c_str());
            DrawText(d, fs * 1.1f, {(dw - ms.x) * 0.5f, dh - foot - 26 * sc}, Col(244, 208, 140), g_message.c_str());
        } else if (!watching || travel_state != coop::lamptravel::MapTravel::Ready) {
            // No snapshot after 1.5 s: lamp travel is off (its tick never runs); before that, just not yet.
            const char* why = TravelReason(watching ? travel_state
                                           : now - g_opened_ms > 1500 ? coop::lamptravel::MapTravel::Off
                                                                      : coop::lamptravel::MapTravel::NotInWorld);
            if (*why) {
                const Pt ms = TextSize(fs, why);
                DrawText(d, fs, {(dw - ms.x) * 0.5f, dh - foot - 24 * sc}, Col(200, 150, 120), why);
            }
        }
    }

    // ---- Click, confirm and the travel question ----
    const bool click = g_click;
    g_click = false;
    const bool confirm = g_cmd_confirm;
    g_cmd_confirm = false;
    if (g_dialog < 0) {
        if (click && hover >= 0) g_selected = g_dialog = hover;
        else if (confirm && g_selected >= 0 && g_selected < static_cast<int>(area.lamps.size()) && shown[static_cast<std::size_t>(g_selected)])
            g_dialog = g_selected;
    }
    if (test_travel && ready) {
        g_test.travel_done = true;
        // The first lamp the map may travel to (or the test's), without a pointer.
        for (std::size_t i = 0; i < area.lamps.size(); ++i) {
            if (!can_travel[i] || (g_test.to && area.lamps[i].return_point != g_test.to)) continue;
            g_selected = static_cast<int>(i);
            if (TravelTo(area.lamps[i], "test")) {
                Close();
                return;
            }
            break;
        }
        std::printf("Map: test travel: no lamp to travel to in %s (state %d, %zu lit)\n", area.id.c_str(), int(travel_state), lit.size());
    }
    if (g_dialog >= 0 && g_dialog < static_cast<int>(area.lamps.size())) {
        const bbmap::Lamp& l = area.lamps[static_cast<std::size_t>(g_dialog)];
        const bool ok = can_travel[static_cast<std::size_t>(g_dialog)];
        const float w = 540 * sc, h = 176 * sc, x = (dw - w) * 0.5f, y = (dh - h) * 0.5f;
        const float bw = 190 * sc, bh = 40 * sc, by = y + h - bh - 20 * sc;
        const float yes_x = x + w * 0.5f - bw - 16 * sc, no_x = x + w * 0.5f + 16 * sc;
        const bool over_yes = g_mouse_in && !g_use_pad_glyphs && InRect(g_mouse, yes_x, by, bw, bh);
        const bool over_no = g_mouse_in && !g_use_pad_glyphs && InRect(g_mouse, no_x, by, bw, bh);
        const bool do_yes = ok && ((click && over_yes) || confirm);
        const bool do_no = click && (over_no || !InRect(g_mouse, x, y, w, h));
        if (do_yes) {
            if (TravelTo(l, "map")) {
                Close();
                return;
            }
            g_message = BbSettings::MenuText("The travel could not start.", "Переход не удалось начать.");
            g_message_until = now + 4000;
            g_dialog = -1;
        } else if (do_no || (confirm && !ok)) {
            g_dialog = -1;
        }
        if (g_dialog >= 0) {
            d->AddRectFilled(ImVec2(0, 0), ImVec2(dw, dh), Col(0, 0, 0, 90));
            d->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), Col(18, 15, 12, 250), 8 * sc);
            d->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), Col(214, 180, 112, 255), 8 * sc, 0, 2 * sc);
            char q[200];
            if (ok) std::snprintf(q, sizeof q, BbSettings::MenuText("Travel to %s?", "Перейти к фонарю: %s?"), l.name.c_str());
            else if (l.return_point == 2102950)
                std::snprintf(q, sizeof q, "%s", BbSettings::MenuText("The Hunter's Dream: use a lamp's own menu.", "Сон Охотника: через меню фонаря."));
            else
                std::snprintf(q, sizeof q, "%s", *TravelReason(travel_state) ? TravelReason(travel_state) : BbSettings::MenuText("Not possible right now", "Сейчас невозможно"));
            const Pt qs = TextSize(24 * sc, q);
            DrawText(d, 24 * sc, {x + (w - qs.x) * 0.5f, y + 28 * sc}, Col(244, 232, 200), q);
            const auto role = party::status::Snapshot().role;
            const char* note = role == party::status::Role::Host
                                   ? BbSettings::MenuText("Your party is asked to come with you.", "Группу спросят, идти ли с вами.")
                               : role == party::status::Role::Guest
                                   ? BbSettings::MenuText("Only you travel; the host stays.", "Переходите только вы; хост остаётся.")
                                   : "";
            if (ok && *note) {
                const Pt ns = TextSize(16 * sc, note);
                DrawText(d, 16 * sc, {x + (w - ns.x) * 0.5f, y + 66 * sc}, Col(190, 170, 130), note);
            }
            auto button = [&](float bx, const char* label, bool enabled, bool over) {
                d->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw, by + bh), enabled ? (over ? Col(96, 78, 46) : Col(66, 54, 34)) : Col(40, 36, 30), 5 * sc);
                d->AddRect(ImVec2(bx, by), ImVec2(bx + bw, by + bh), enabled ? Col(214, 180, 112) : Col(90, 82, 66), 5 * sc, 0, 1.5f * sc);
                const Pt ls = TextSize(20 * sc, label);
                DrawText(d, 20 * sc, {bx + (bw - ls.x) * 0.5f, by + (bh - ls.y) * 0.5f}, enabled ? Col(244, 232, 200) : Col(120, 110, 92), label);
            };
            button(yes_x, BbSettings::MenuText("Travel", "Перейти"), ok, over_yes);
            button(no_x, BbSettings::MenuText("Cancel", "Отмена"), true, over_no);
        }
    }
}

}  // namespace BbMapView

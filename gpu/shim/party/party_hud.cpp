// SPDX-License-Identifier: GPL-3.0-or-later
// Party HUD board: composition (pure) and the game -> overlay hand-over (party_hud.h).
#include "party_hud.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>

namespace party::hud {

namespace {

std::mutex g_mu;
Board g_board;  // g_mu
std::atomic<bool> g_shown{false};
std::atomic<bool> g_cutscene{false};  // a remo plays: the panel and tags stay hidden

void CopyName(char (&dst)[kNameBytes], const std::string& src) {
    const std::size_t n = std::min<std::size_t>(src.size(), kNameBytes - 1);
    std::memcpy(dst, src.data(), n);
    dst[n] = 0;
}

bool KnownMap(std::uint32_t m) {
    return m != 0 && m != 0xffffffffu;
}

// In a world at all (not loading, not at the title).
bool InAWorld(MemberState s) {
    return s == MemberState::Home || s == MemberState::Joining || s == MemberState::InHostWorld ||
           s == MemberState::Prologue || s == MemberState::Dead;
}

}  // namespace

bool SameWorld(int local_slot, MemberState local_state, int other_slot, MemberState other_state) {
    if (local_slot < 0 || other_slot < 0 || local_slot == other_slot) return false;
    if (!InAWorld(local_state) || !InAWorld(other_state)) return false;
    if (local_slot == kHostSlot)  // the host: its summoned guests
        return other_state == MemberState::InHostWorld && local_state != MemberState::Prologue;
    if (local_state != MemberState::InHostWorld) return false;  // a guest at home is alone
    // A summoned guest: the host (whose world it is) and the other summoned guests.
    return other_slot == kHostSlot || other_state == MemberState::InHostWorld;
}

void Compose(const ComposeIn& in, Board* out) {
    Board b;
    b.active = true;
    b.me_have_pos = in.me_have_pos;
    std::memcpy(b.me_pos, in.me_pos, sizeof b.me_pos);
    b.me_yaw = in.me_yaw;
    b.me_map = in.local_map;
    // Companions in slot order.
    for (int slot = 0; slot < kMaxVitals; ++slot) {
        if (slot == in.local_slot) continue;
        const RosterEntry* r = nullptr;
        for (int i = 0; i < in.roster_n; ++i)
            if (in.roster[i].slot == slot) r = &in.roster[i];
        if (!r || b.count >= kMaxCompanions) continue;
        Companion& c = b.c[b.count++];
        CopyName(c.name, r->name);
        c.slot = slot;
        c.state = r->state;
        c.connected = r->connected;
        c.map_id = r->map_id;
        c.same_world = r->connected && SameWorld(in.local_slot, in.local_state, slot, r->state);
        for (int i = 0; i < in.vitals_n; ++i) {
            const MemberVitals& v = in.vitals[i];
            if (v.slot != slot) continue;
            c.age_ms = v.age_ms;
            c.level = v.level;
            c.deaths = v.deaths;
            c.rot = v.rot;
            if (v.flags & kVitalsValid) {
                c.have_hp = true;
                c.hp = static_cast<int>(v.hp);
                c.max_hp = static_cast<int>(v.max_hp);
                c.alive = (v.flags & kVitalsAlive) != 0;
                c.have_stamina = v.max_stamina > 0;
                c.stamina = v.stamina;
                c.max_stamina = v.max_stamina;
            }
            if (KnownMap(v.map_id)) c.map_id = v.map_id;
            if (v.flags & kVitalsHasPos) {
                c.have_pos = true;
                std::memcpy(c.pos, v.pos, sizeof c.pos);
            }
        }
        // Its PlayerIns here: the game's own copy, current every frame.
        if (c.same_world)
            for (int i = 0; i < in.chrs_n; ++i) {
                const LocalChr& l = in.chrs[i];
                if (std::strncmp(l.name, c.name, kNameBytes) != 0) continue;
                c.local_read = true;
                c.age_ms = 0;
                c.local_dead = l.have_hp && l.hp <= 0;
                if (l.have_hp && !c.have_hp) {  // VITALS first: the remote data module lags
                    c.have_hp = true;
                    c.hp = l.hp;
                    c.max_hp = l.max_hp;
                    c.alive = l.hp > 0;
                    c.have_stamina = l.max_stamina > 0;
                    c.stamina = l.stamina;
                    c.max_stamina = l.max_stamina;
                }
                if (l.have_pos) {
                    c.have_pos = true;
                    std::memcpy(c.pos, l.pos, sizeof c.pos);
                }
                c.map_id = in.local_map;  // it stands in our world, on our map
            }
        if (!c.connected) c.have_pos = false;  // a lost member's last position means nothing
        c.same_map = c.have_pos && in.me_have_pos && KnownMap(c.map_id) && c.map_id == in.local_map;
    }
    *out = b;
}

void Publish(const Board& b) {
    std::lock_guard<std::mutex> lk(g_mu);
    const std::uint64_t seq = g_board.seq + 1;
    g_board = b;
    g_board.seq = seq;
    g_board.published_ms = NowMs();
    g_shown.store(b.active && b.count > 0, std::memory_order_relaxed);
}

std::int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool TagVisible(const Companion& c, std::int64_t board_age_ms, const char** why) {
    const char* w = nullptr;
    if (!c.connected) w = "not connected";
    else if (!c.same_world) w = "not in this world";
    else if (!c.local_read) w = "no chr here";  // only the live chr's position, never a network report
    else if (!c.have_pos || !c.same_map) w = "not on this map";
    else if (!c.alive || (c.have_hp && c.hp <= 0) || c.local_dead) w = "dead";
    else if (board_age_ms < 0 || board_age_ms > kTagFreshMs) w = "stale";
    if (why) *why = w ? w : "";
    return w == nullptr;
}

float PlateScale(float depth) {
    if (!std::isfinite(depth) || depth <= 0.0f) return kPlateMaxScale;
    return std::clamp(kPlateFarDepth / depth, 1.0f, kPlateMaxScale);
}

PlateLayout LayoutPlate(float ax, float ay, float w, float h, float depth) {
    const float k = PlateScale(depth) * h;
    PlateLayout l;
    l.font = kPlateFontH * k;
    const float bar_w = kPlateBarW * w * PlateScale(depth);
    const float bar_h = std::max(2.0f, std::round(kPlateBarH * k));
    l.bar_x0 = std::round(ax - bar_w * 0.5f);
    l.bar_x1 = std::round(ax + bar_w * 0.5f);
    l.bar_y0 = std::round(ay);
    l.bar_y1 = l.bar_y0 + bar_h;
    l.cap_w = std::max(1.0f, std::round(0.0021f * k));
    l.cap_y0 = l.bar_y0 - std::max(1.0f, std::round(kPlateCapRise * k));
    l.name_x = l.bar_x0 + kPlateNameInset * k;
    l.name_baseline = l.bar_y0 - kPlateBaselineGap * k;
    l.outline = std::max(1.0f, std::round(0.0021f * k));
    return l;
}

float TagTextTop(float anchor_y, float text_h, bool with_bar, float bar_h, float gap) {
    return anchor_y - (with_bar ? bar_h + gap : 0.0f) - text_h;
}

void Read(Board* out) {
    std::lock_guard<std::mutex> lk(g_mu);
    *out = g_board;
}

bool Shown() {
    return g_shown.load(std::memory_order_relaxed) && !g_cutscene.load(std::memory_order_relaxed);
}

void SetCutscene(bool playing) {
    g_cutscene.store(playing, std::memory_order_relaxed);
}

float Distance(const float a[3], const float b[3]) {
    const float dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

float Bearing(const float from[3], const float to[3], float fx, float fz, float rx, float rz) {
    const float dx = to[0] - from[0], dz = to[2] - from[2];
    const float ahead = dx * fx + dz * fz, right = dx * rx + dz * rz;
    if (ahead == 0.0f && right == 0.0f) return 0.0f;
    return std::atan2(right, ahead);
}

bool Project(const float view[12], const float proj[4], const float p[3], float* u, float* v, float* depth) {
    const float x = view[0] * p[0] + view[1] * p[1] + view[2] * p[2] + view[3];
    const float y = view[4] * p[0] + view[5] * p[1] + view[6] * p[2] + view[7];
    const float z = view[8] * p[0] + view[9] * p[1] + view[10] * p[2] + view[11];
    if (!(z > 0.3f) || !std::isfinite(x) || !std::isfinite(y)) return false;
    *u = (x * proj[0] / z) * 0.5f + 0.5f;
    *v = (y * proj[1] / z) * 0.5f + 0.5f;
    *depth = z;
    return std::isfinite(*u) && std::isfinite(*v);
}

ScreenPoint WorldToScreen(const float view[12], const float proj[4], const float p[3], float w, float h,
                          float margin) {
    ScreenPoint sp;
    float u = 0, v = 0, depth = 0;
    if (!Project(view, proj, p, &u, &v, &depth)) return sp;
    sp.in_front = true;
    sp.x = u * w;
    sp.y = v * h;
    sp.depth = depth;
    sp.visible = u >= margin && u <= 1.0f - margin && v >= margin && v <= 1.0f - margin;
    return sp;
}

void MemberColour(int slot, float rgb[3]) {
    // Host gold, then pale blue, green, violet: readable on Yharnam's greys, distinct from the
    // game's own blue phantom label and red HP.
    static const float kColours[4][3] = {
        {0.96f, 0.80f, 0.42f}, {0.55f, 0.78f, 1.00f}, {0.55f, 0.92f, 0.55f}, {0.86f, 0.62f, 1.00f}};
    static const float kCream[3] = {0.96f, 0.88f, 0.70f};
    const float* c = slot >= 0 && slot < 4 ? kColours[slot] : kCream;
    for (int i = 0; i < 3; ++i) rgb[i] = c[i];
}

void ResetForTest() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_board = Board{};
    g_shown = false;
}

}  // namespace party::hud

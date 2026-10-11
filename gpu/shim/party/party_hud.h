// SPDX-License-Identifier: GPL-3.0-or-later
// Party HUD (Seamless-Co-op style): every member sees its companions' names and health at a
// glance, greyed while they are not in this player's world, with the distance and a direction
// arrow while they are on the same map (and, with the game camera, a name tag above them).
//
//   game (main thread, party_hud_game.cpp, ~10 Hz): reads this player's HP / stamina / map /
//     position and sends them as PartyLink VITALS (party_link.h MemberVitals, ~4 Hz); reads the
//     remote players the game has in this world (their PlayerIns: exact and current); composes
//     the board (Compose: pure) and publishes it.
//   overlay (present thread, bbport_overlay.cpp PartyHud): Read() into its own Board (a fixed
//     size copy under a mutex: no allocation), draws the panel and the tags.
//
// Data per companion: from its PlayerIns when the game has it here (same world), else from the
// VITALS its own game sent (through the host), else only the roster (state, map).
// Env: BB_PARTY_HUD_LOG=1 logs the reads every 5 s ("Party HUD: ..."); BB_PARTY_HUD_TAGS=0 turns
// the floating name tags off. Setting: bbport.ini party_hud (default on; menus: Party members
// page "Party HUD", overlay Party tab, launcher Party tab).
#pragma once

#include "party_link.h"

#include <cstdint>

namespace party::hud {

constexpr int kMaxCompanions = kMaxVitals - 1;
constexpr int kNameBytes = 17;  // 16 + NUL (valid_member_name)

struct Companion {
    char name[kNameBytes] = {};
    int slot = -1;
    MemberState state = MemberState::Title;
    bool connected = false;
    bool same_world = false;  // in this player's world (summoned / the host we are summoned to)
    bool local_read = false;  // HP / position read from its PlayerIns here (not from VITALS)
    bool have_hp = false;
    bool alive = true;
    int hp = 0, max_hp = 0;
    bool have_stamina = false;
    int stamina = 0, max_stamina = 0;
    std::uint32_t map_id = 0;  // its map (VITALS, else the roster); 0 unknown
    bool have_pos = false;
    bool same_map = false;     // both on one map with positions: distance / direction mean something
    float pos[3] = {0, 0, 0};
    std::uint32_t age_ms = 0;  // VITALS age (0 for a local read)
    int level = 0;             // VITALS extras (0 = unknown): the overhead display (E2)
    int deaths = 0;
    int rot = 0;
};

struct Board {
    bool active = false;  // party mode with a link: the HUD may show
    int count = 0;        // companions (never this player)
    Companion c[kMaxCompanions];
    bool me_have_pos = false;
    float me_pos[3] = {0, 0, 0};
    float me_yaw = 0;     // facing (forward = (sin, 0, cos)): the direction fallback without a camera
    std::uint32_t me_map = 0;
    std::uint64_t seq = 0;  // +1 per Publish
};

/// A remote player's PlayerIns as read in this world, matched to a member by name.
struct LocalChr {
    char name[kNameBytes] = {};
    bool have_hp = false;
    int hp = 0, max_hp = 0;
    int stamina = 0, max_stamina = 0;
    bool have_pos = false;
    float pos[3] = {0, 0, 0};
};

struct ComposeIn {
    int local_slot = -1;
    MemberState local_state = MemberState::Title;
    std::uint32_t local_map = 0;
    bool me_have_pos = false;
    float me_pos[3] = {0, 0, 0};
    float me_yaw = 0;
    const RosterEntry* roster = nullptr;
    int roster_n = 0;
    const MemberVitals* vitals = nullptr;
    int vitals_n = 0;
    const LocalChr* chrs = nullptr;
    int chrs_n = 0;
};

/// Whether a member is in this player's world: summoned guests share the host's world; a guest
/// at home (or loading, at the title, in its prologue) shares nobody's.
bool SameWorld(int local_slot, MemberState local_state, int other_slot, MemberState other_state);
/// The companions (by slot, this player left out) from the roster, the VITALS and the local reads.
void Compose(const ComposeIn& in, Board* out);

/// Publish (game): the overlay's next Read sees it. Read: the latest (inactive before the first).
void Publish(const Board& b);
void Read(Board* out);
/// Cheap (atomic): something to show (active with at least one companion).
bool Shown();

// ---- geometry (overlay; pure) ----
/// The distance (3D, metres) from `a` to `b`.
float Distance(const float a[3], const float b[3]);
/// The direction to `to` from `from`, radians in [-pi, pi]: 0 straight ahead along the forward
/// axis (fx, fz), positive to the right (rx, rz).
float Bearing(const float from[3], const float to[3], float fx, float fz, float rx, float rz);
/// World -> screen with the game camera (view: world to view, 3x4 rows; proj: x scale, y scale
/// as the motion shader uses them, ndc +y down): false behind the camera / too close to it.
/// u, v in 0..1 across the scene (outside when off screen).
bool Project(const float view[12], const float proj[4], const float p[3], float* u, float* v, float* depth);

/// World point -> display pixels: the reusable world-space marker projection (E2 name tags; I2
/// pings and through-wall markers reuse it). `w` x `h`: the display size in pixels. `visible`: in
/// front of the camera and inside the display minus `margin` (a fraction of each side); x / y are
/// set whenever the point is in front (an off-screen marker can be clamped to the edge by the caller).
struct ScreenPoint {
    bool in_front = false, visible = false;
    float x = 0, y = 0, depth = 0;  // pixels, metres from the camera
};
ScreenPoint WorldToScreen(const float view[12], const float proj[4], const float p[3], float w, float h,
                          float margin = 0.02f);
/// A party member's colour by slot (0 host .. 3), RGB 0..1: the same everywhere a member is drawn
/// (tags now, I2 pings later). Slot outside 0..3: the game's cream.
void MemberColour(int slot, float rgb[3]);

/// Tests: back to the start.
void ResetForTest();

}  // namespace party::hud

namespace coop {
struct GameSnapshot;  // game_state.h
/// The game side (party_hud_game.cpp): the director, once a frame on the main thread (works
/// every 100 ms): this player's VITALS out, the remote PlayerIns read, the board published.
/// `link` null: the board goes inactive.
void PartyHudTick(const GameSnapshot& s, party::PartyLink* link, party::MemberState local_state);
}  // namespace coop

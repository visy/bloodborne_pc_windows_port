// SPDX-License-Identifier: GPL-3.0-or-later
// Party HUD board (gpu/shim/party/party_hud.cpp): who shares this player's world, the companions
// from the roster + VITALS + local PlayerIns reads, same-map distance / bearing, the camera
// projection, the publish / read hand-over.
// Build and run: cmake --build out/gpu --target party-hud-test && out/gpu/party-hud-test.exe
#include "party/party_hud.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace party;
using party::hud::Board;
using party::hud::ComposeIn;

static RosterEntry Entry(int slot, const char* name, MemberState st, std::uint32_t map, bool connected = true) {
    RosterEntry e;
    e.slot = slot;
    e.name = name;
    e.state = st;
    e.map_id = map;
    e.connected = connected;
    return e;
}

static bool Near(float a, float b) {
    return std::fabs(a - b) < 1e-3f;
}

static void SameWorldRules() {
    using MS = MemberState;
    // The host shares its world with summoned guests only.
    assert(hud::SameWorld(0, MS::Home, 1, MS::InHostWorld));
    assert(!hud::SameWorld(0, MS::Home, 1, MS::Home));
    assert(!hud::SameWorld(0, MS::Loading, 1, MS::InHostWorld));
    assert(!hud::SameWorld(0, MS::Prologue, 1, MS::InHostWorld));
    // A summoned guest: the host and the other summoned guests.
    assert(hud::SameWorld(1, MS::InHostWorld, 0, MS::Home));
    assert(hud::SameWorld(1, MS::InHostWorld, 2, MS::InHostWorld));
    assert(!hud::SameWorld(1, MS::InHostWorld, 2, MS::Home));
    assert(!hud::SameWorld(1, MS::InHostWorld, 0, MS::Loading));
    // A guest at home: nobody.
    assert(!hud::SameWorld(1, MS::Home, 0, MS::Home));
    assert(!hud::SameWorld(1, MS::Home, 2, MS::InHostWorld));
    assert(!hud::SameWorld(1, MS::InHostWorld, 1, MS::InHostWorld));
}

static void ComposeBoard() {
    const std::uint32_t map = 0x18000000, other = 0x15010000;
    std::vector<RosterEntry> roster = {Entry(0, "Hunter0", MemberState::Home, map),
                                       Entry(1, "Hunter1", MemberState::InHostWorld, map),
                                       Entry(2, "Hunter2", MemberState::Home, other),
                                       Entry(3, "Hunter3", MemberState::Loading, 0, false)};
    MemberVitals v[3];
    v[0].slot = 0;  // the host's own VITALS (what a guest gets relayed)
    v[0].flags = kVitalsValid | kVitalsAlive | kVitalsHasPos;
    v[0].hp = 500;
    v[0].max_hp = 1000;
    v[0].map_id = map;
    v[0].pos[0] = 10;
    v[0].age_ms = 200;
    v[1].slot = 2;
    v[1].flags = kVitalsValid | kVitalsHasPos;  // dead in its own world
    v[1].hp = 0;
    v[1].max_hp = 800;
    v[1].stamina = 0;
    v[1].max_stamina = 90;
    v[1].map_id = other;
    v[1].pos[2] = 5;
    v[2].slot = 1;  // ourselves: never a companion
    v[2].flags = kVitalsValid | kVitalsAlive;
    v[2].hp = 1;
    v[2].max_hp = 1;
    hud::LocalChr chr;
    std::snprintf(chr.name, sizeof chr.name, "Hunter0");
    chr.have_hp = true;
    chr.hp = 420;
    chr.max_hp = 1000;
    chr.stamina = 50;
    chr.max_stamina = 100;
    chr.have_pos = true;
    chr.pos[0] = 3;
    chr.pos[2] = 4;

    ComposeIn in;
    in.local_slot = 1;
    in.local_state = MemberState::InHostWorld;
    in.local_map = map;
    in.me_have_pos = true;
    in.roster = roster.data();
    in.roster_n = int(roster.size());
    in.vitals = v;
    in.vitals_n = 3;
    in.chrs = &chr;
    in.chrs_n = 1;
    Board b;
    hud::Compose(in, &b);
    assert(b.active && b.count == 3);
    // The host: in our world; its position from its PlayerIns here, HP from its VITALS (a remote
    // data module lags).
    const hud::Companion& h = b.c[0];
    assert(std::strcmp(h.name, "Hunter0") == 0 && h.slot == 0 && h.same_world && h.local_read);
    assert(h.have_hp && h.hp == 500 && h.max_hp == 1000 && h.alive && !h.have_stamina);
    assert(h.have_pos && h.same_map && Near(h.pos[0], 3) && h.age_ms == 0);
    assert(Near(hud::Distance(b.me_pos, h.pos), 5));
    // The other guest at home: from its VITALS, greyed (not our world), other map, dead.
    const hud::Companion& g = b.c[1];
    assert(g.slot == 2 && !g.same_world && !g.local_read && g.have_hp && !g.alive && g.max_hp == 800);
    assert(g.map_id == other && g.have_pos && !g.same_map);
    // A lost member: roster only, no position.
    const hud::Companion& l = b.c[2];
    assert(l.slot == 3 && !l.connected && !l.same_world && !l.have_hp && !l.have_pos);

    // Without VITALS HP, the local read's HP counts.
    v[0].flags = kVitalsHasPos;
    hud::Compose(in, &b);
    assert(b.c[0].have_hp && b.c[0].hp == 420 && b.c[0].have_stamina && b.c[0].stamina == 50);
    v[0].flags = kVitalsValid | kVitalsAlive | kVitalsHasPos;
    // A chr that did not match by name changes nothing; without the local read the host comes
    // from its VITALS (same map: distance).
    std::snprintf(chr.name, sizeof chr.name, "Nobody");
    hud::Compose(in, &b);
    assert(!b.c[0].local_read && b.c[0].hp == 500 && b.c[0].age_ms == 200 && b.c[0].same_map);
    assert(Near(hud::Distance(b.me_pos, b.c[0].pos), 10));
    // Positions are not compared across maps, nor without our own.
    in.me_have_pos = false;
    hud::Compose(in, &b);
    assert(!b.c[0].same_map);
}

static void Geometry() {
    const float o[3] = {0, 0, 0}, ahead[3] = {0, 0, 5}, right[3] = {5, 0, 0}, behind[3] = {0, 0, -5};
    // Forward +z, right +x.
    assert(Near(hud::Bearing(o, ahead, 0, 1, 1, 0), 0.0f));
    assert(Near(hud::Bearing(o, right, 0, 1, 1, 0), 3.14159265f / 2));
    assert(Near(std::fabs(hud::Bearing(o, behind, 0, 1, 1, 0)), 3.14159265f));
    // Identity view, 90 degree projection: a point ahead lands mid-screen, one up-right of it
    // right of the centre (and below it: ndc +y is down for a positive y scale).
    const float view[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    const float proj[4] = {1, 1, 0, 0};
    float u = 0, v = 0, d = 0;
    assert(hud::Project(view, proj, ahead, &u, &v, &d) && Near(u, 0.5f) && Near(v, 0.5f) && Near(d, 5));
    const float p[3] = {2.5f, 2.5f, 5};
    assert(hud::Project(view, proj, p, &u, &v, &d) && Near(u, 0.75f) && Near(v, 0.75f));
    assert(!hud::Project(view, proj, behind, &u, &v, &d));
}

static void Handover() {
    hud::ResetForTest();
    Board b;
    hud::Read(&b);
    assert(!b.active && !hud::Shown());
    Board in;
    in.active = true;
    hud::Publish(in);
    assert(!hud::Shown());  // nobody to show
    in.count = 1;
    std::snprintf(in.c[0].name, sizeof in.c[0].name, "Hunter1");
    hud::Publish(in);
    hud::Read(&b);
    assert(hud::Shown() && b.count == 1 && b.seq == 2 && std::strcmp(b.c[0].name, "Hunter1") == 0);
    hud::Publish(Board{});
    assert(!hud::Shown());
}

int main() {
    SameWorldRules();
    ComposeBoard();
    Geometry();
    Handover();
    std::printf("party hud: ok\n");
    return 0;
}

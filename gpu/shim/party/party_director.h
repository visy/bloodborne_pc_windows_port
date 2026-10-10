// SPDX-License-Identifier: GPL-3.0-or-later
// PartyDirector (plan phase A5): automatic summoning, driven once a frame on the game's main
// thread (coop tick: the SprjFlipper::Update entry, 0x2034770, which the frame function
// 0x2018d20 calls before the task system runs - in menus and in the world alike).
//
//   guest: world up, no loading screen, session idle, party link connected
//            -> raise OnEvent_SendSoulSign_NormalCoop (Small Resonant Bell) every BB_PARTY_RING_EVERY s
//   host:  world up, no loading screen, session idle, cooperators < max - 1 and a party member
//          waits (roster: connected, Home or Joining)
//            -> raise OnEvent_Call_SOS (Beckoning Bell) every BB_PARTY_RING_EVERY s
// The local roster state (Title / Loading / Prologue / Home / InHostWorld) and map go to PartyLink.
// Campaign start (C1, party_start.h): nothing is rung before the local player is "ready"
// (BB_PARTY_START=prologue_solo: prologue done; immediate: opening cutscene done); until then the
// roster state is Prologue, so the host does not ring for that member either. Once ready, in its
// own world, the player gets the bells it lacks (BB_PARTY_GRANT_BELLS, default on).
//
// Env: BB_PARTY=host|join|<code> (role: host when "host" or BB_PARTY_HOST=1, else guest),
//      BB_PARTY_MAX (2..4, default 3), BB_PARTY_RING_EVERY (s, default 30),
//      BB_PARTY_AUTO=0 (no automatic bells: state tracking and logs only),
//      BB_PARTY_START=prologue_solo (default) | immediate, BB_PARTY_GRANT_BELLS=0 (no bell grants),
//      BB_PARTY_DIRECTOR_TEST=item[,item..] - offline test mode:
//        log_state    the state every 5 s and the game's own Lua event dispatches
//        start        the campaign start flags and bell counts every 5 s (and on each change)
//        grant_bells  grants the missing bells once ready (also without a party / with
//                     BB_PARTY_GRANT_BELLS=0) and logs the counts before and after
//        insight=N    writes the player's Insight once the world is up
//        ring_host / ring_guest  raises that bell once, BB_PARTY_DIRECTOR_TEST_DELAY s (default 10)
//                     after the world is up, and logs what follows (and the start flags/bells)
#pragma once

#include <cstdint>

namespace party {
class PartyLink;
}

namespace coop {

enum class PartyRole { None, Host, Guest };

class PartyDirector {
public:
    static PartyDirector& Get();
    /// From the environment (BB_PARTY, BB_PARTY_MAX, BB_PARTY_RING_EVERY, BB_PARTY_DIRECTOR_TEST).
    void ConfigureFromEnv();
    void Configure(PartyRole role, int max_players);
    /// The party's link (A3/A4 wiring); nullptr detaches. The link must outlive its attachment.
    void SetLink(party::PartyLink* link);
    PartyRole Role() const;
    /// Main thread, once a frame.
    void Tick();

private:
    PartyDirector() = default;
};

/// BB_PARTY or BB_PARTY_DIRECTOR_TEST is set (non-empty).
bool PartyRequested();
/// bbgpu_patch_image: binds the image, checks the Lua dispatcher and installs the main-thread
/// tick (only call it when PartyRequested()).
void PartyInit(unsigned char* image, std::uint64_t size);
/// The main-thread tick: Lua events, then the director.
void CoopTick();

} // namespace coop

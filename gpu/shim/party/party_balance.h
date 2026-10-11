// SPDX-License-Identifier: GPL-3.0-or-later
// Party balance, the game side (ERSC parity C4 death penalty, D7 enemy scaling, E2 overhead
// display; docs/party/balance.md). The rules themselves are pure: party_balance_rules.h.
//
// The party's rules are the host's settings (bbport.ini party_rot, party_boss_* /
// party_enemy_*), sent to every guest as EVENT "balance" (on change and every 15 s); a guest uses
// them as soon as they arrive (its own settings until then).
//
//   D7  SpEffect rows 7500 / 7501 / 7502 (the game's boss "doping" for 1 / 2 / 3 cooperators)
//       get the boss values, row 7503 (unused in 1.09) the normal enemy values for the current
//       cooperator count (param writes, re-checked every second; a row whose maxHpRate is
//       neither the 1.09 value nor ours is left alone). With cooperators present and enemy
//       scaling not zero, every normal enemy (NpcParam teamType 23, not a boss soul) in this
//       world gets SpEffect 7503 (chr vfunc +0x3f0); a boss that gets its doping loses 7503
//       (0x17e1d90, EMEVD 2004[21]'s remove). The game's own per-frame HP update (0x1a0d7d0)
//       raises current HP with the max, so full-health enemies stay full.
//   C4  "Rot": each death of the local player (HP > 0 -> 0 in the world) adds a stack (max 5);
//       each stack lowers max HP by 10 % through the max-HP rate function 0x17e46e0 (prologue
//       replaced: the local player's SpEffect list only). A lamp / headstone use started by the
//       player (party_travel SetLampUseObserver; not the respawn warp of a death) cleanses it.
//       Kept in <user>/party_rot.txt across restarts. Echoes are not touched (the bloodstain is
//       the hostdeath work).
//   E2  This player's level, death count and rot stacks go out with the party HUD's VITALS.
//
// Env: BB_PARTY_BALANCE=0 turns all of it off (no hook, no param writes, no events);
//      BB_PARTY_BALANCE_LOG=1 logs each enemy pass.
#pragma once

#include "party_balance_rules.h"

#include <string>

namespace party {
class PartyLink;
}

namespace coop {
struct GameSnapshot;
}

namespace coop::balance {

constexpr const char* kEventName = "balance";

/// PartyInit (bbgpu_patch_image, after HooksInit): the max-HP rate hook, the lamp observer.
void Init();
/// The director's tick (main thread, party mode only). `host`: this game hosts (or plays without
/// a link): its own settings are the party's.
void Tick(const GameSnapshot& s, party::PartyLink* link, bool host);
/// Guest: the host's EVENT "balance" (any thread). False (and *error) when malformed.
bool OnHostRules(const std::string& json, std::string* error = nullptr);
/// The rules in force now.
Rules InForce();
/// The local settings as rules.
Rules FromSettings();
/// For the party HUD's VITALS.
int LocalDeaths();
int LocalRotStacks();

}  // namespace coop::balance

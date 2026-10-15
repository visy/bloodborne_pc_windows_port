// SPDX-License-Identifier: GPL-3.0-or-later
// Split travel's choice (docs/party/split_travel.md): in a party, a lamp or headstone warp asks
// "Travel with the party?" in a game dialog (YES: the whole party travels, the B2 party move as
// before; NO: only this player travels, party_b2 TravelAlone / HostTravelAloneNext).
//
//   game thread: the lamp warp interceptor (party_guestsvc GuestLampIntercept) calls OnLampWarp
//                first; a warp started by the player in a party is consumed and queued.
//   main thread: Tick opens the game's generic dialog (the struct at *(0x5562878 +0xD08), opened
//                by 0x16BEBF0(dialog, 7) as the Lua OnYesNoDialog 0x132D2C0 does: [0] message id,
//                [1] 1 / [2] 2 = YES / NO buttons, [6] result -1 until answered) with the port's
//                text (BbGameMenu::Pages::SplitTravelText, answered by the menu's text lookup);
//                on the answer it runs the warp the chosen way.
//
// Env: BB_PARTY_TRAVEL_CHOICE = ask (default) | party (always the party move, no question) |
//      alone (always alone, no question); BB_PARTY_SPLIT_TEST = yes | no (tests: answer the
//      dialog with the in-process pad: YES = Cross, NO = Right then Cross).
//
// Only the player who used the lamp / headstone is asked (playtest 1 bug 20): its "used" flag
// reaches every peer (packet 0x0f) and runs the same warp event on their games too. A warp is
// another member's when this game got a lamp flag from a peer and did not set one itself
// (party_death_rules PlanLampOrigin): it is dropped with a log line, the member who used it
// chooses, and its EVENT split_choosing names that member in the others' logs.
#pragma once

#include <cstdint>
#include <string>

namespace coop {
struct GameSnapshot;
}
namespace party {
class PartyLink;
}

namespace coop::split {

enum class Mode { Ask, Party, Alone };
Mode ModeFromEnv(const char* v);
const char* ModeName(Mode m);

/// After b2::Init (party mode): installs the Hunter's Mark interceptor (party_travel).
void Init();
/// Game thread: a lamp / headstone warp the player started (not a replay). True = consumed
/// (the choice decides what runs).
bool OnLampWarp(std::uint32_t id);
/// Test steps that call the lamp warp "as the player" (no talk script, so no lamp flag): the
/// next warp is this player's own use.
void NoteLocalUse(std::uint32_t id);
/// Game thread: a story warp of this player's game (deathrules::StoryWarpEvent) is about to run:
/// this player's own use, and its question waits until the scene before it is over.
void NoteStoryWarp(std::uint32_t id);
/// Main thread, once a frame (the director); `link` sends EVENT split_choosing.
void Tick(const GameSnapshot& s, party::PartyLink* link);

/// EVENT split_choosing {"lamp": id}: a member opened the question on its game.
constexpr const char* kChoosingEventName = "split_choosing";
/// Party callback thread: that event from `slot` (roster name `who`).
void OnChoosingEvent(int slot, const std::string& who, const std::string& body);

} // namespace coop::split

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
#pragma once

#include <cstdint>

namespace coop {
struct GameSnapshot;
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
/// Main thread, once a frame (the director).
void Tick(const GameSnapshot& s);

} // namespace coop::split

// SPDX-License-Identifier: GPL-3.0-or-later
// Reads of the game's state for the party director: is the world up, is a loading screen up,
// the session role, the local player's record (Insight, level), the current map and the number
// of cooperators. Reads go through SafeRead (a fault returns false: the runtime's
// runtime_fault_recover); pointers are checked before each step.
//
// Offsets (ours = bbhost - 0x400000), checked in the 1.09 eboot:
//   WorldChrMan slot 0x553e878 (+0x60 the local PlayerIns; PlayerIns +0x400 position history,
//     whose +0x48 is the player's map id: PlayerIns_GetBloodMarkMap 0x19046c0)
//   SprjSessionManager slot 0x5540290 (+0x120 sub state, +0x124 role, +0x270 matching status)
//   GameDataMan slot 0x553b130 (+8 the local PlayerGameData: +0x84 Insight, +0x90 level)
//   NowLoading request byte 0x556286b (set by 0x176d380, cleared by 0x176d3c0);
//     CSNowLoadingHelper slot 0x553e8b8 (+0x50 / +0x51: an in-game frame ran)
//   Network flow slot 0x5556678 (+0x1590 online mode, +0x16f8 the session slot table)
//   NP manager slot 0x56c7048 (+0xd8 IsOnline, what lua_cli_IsOnline reads via 0xcb4320)
//   Cooperator count 0x15bdc20(slot table) - a game function: main thread only
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace coop {

struct StartFlags; // party_start.h

/// Copies `n` bytes from `address`; false (nothing copied for sure) if it faults or is implausible.
bool SafeRead(std::uint64_t address, void* out, std::size_t n);
template <class T>
bool SafeGet(std::uint64_t address, T* v) {
    return SafeRead(address, v, sizeof(T));
}
bool SafeWrite(std::uint64_t address, const void* in, std::size_t n);

enum SessionRole : int {
    RoleIdle = 0,
    RoleTryingToHost = 1,
    RoleFailedToHost = 2,
    RoleHost = 3,
    RoleTryingToJoin = 4,
    RoleJoinFailed = 5,
    RoleClient = 6,
    RoleLeaving = 7,
};
const char* SessionRoleName(int role);

struct GameSnapshot {
    bool image = false;          ///< offsets bound (coop::HooksInit ran)
    bool world_up = false;       ///< WorldChrMan, its local player and SprjSessionManager exist
    bool loading = false;        ///< a loading screen is up (requested, no in-game frame)
    bool in_game_frame = false;  ///< CSNowLoadingHelper saw an in-game frame
    std::uint64_t world_chr_man = 0, player = 0, session_man = 0, game_data_man = 0, player_rec = 0;
    int session_role = -1, session_sub_state = -1, matching_status = -1;
    int insight = -1, level = -1;
    std::uint32_t map_id = 0xffffffffu; ///< 0xAABBCCDD = mAA_BB_CC_DD; all ones when unknown
    int cooperators = -1;        ///< -1: not asked (or not safe to ask)
    int online_mode = -1;        ///< network flow +0x1590
    int np_online = -1;          ///< NP manager +0xd8
};

/// Reads the state. `call_game` also asks the game's cooperator count (main thread only, world up).
GameSnapshot ReadGameState(bool call_game);
/// "m24_01_00_00" (or "-" when unknown).
std::string MapName(std::uint32_t map_id);
/// One line for logs.
std::string Describe(const GameSnapshot& s);
/// Writes the local player's Insight (PlayerGameData +0x84); false when there is no record.
bool WritePlayerInsight(int insight);

// ---- Campaign start (C1, docs/party/campaign_start.md). Game functions: main thread only. ----
// Each function's first bytes are compared with the 1.09 code before the first call; a mismatch
// turns that function off (logged once).
//   GetEventFlagValue 0x13CFD80(EventFlagMan *0x553B100, flag, bits 1) -> 0/1
//   SetEventFlag      0x13CFCC0(EventFlagMan, flag, on)
//   inventory index   0x14D9E80(PlayerGameData + 0x328, type bits, id) -> index or -1 (a leaf,
//     pure hash lookup); entry = index < [list+0x24] ? [list+0x58] + 16 i : [list+0x48] + 16 (i - n),
//     count at entry +8 (as GiveItemDirect reads it)
//   AwardItemLot      0x17DDC50(*0x553D6E0, lot, 0)   (EMEVD 2003[4], call 0x17C1806)
//   GiveItemDirect    0x131CB70(0, type bits, id, n)  (EMEVD 2003[43], call 0x17C2ACB)

/// The flag's value; false when it cannot be read (no EventFlagMan, code differs).
bool ReadEventFlag(std::uint32_t flag, bool* value);
bool WriteEventFlag(std::uint32_t flag, bool value);
/// How many of goods `id` the local player holds: 0 = none, -1 = unknown (no record / code differs).
int GoodsCount(std::uint32_t goods_id);
/// The start flags and the bell counts (party_start.h).
StartFlags ReadStartFlags();
/// Awards item lot `lot` (with the item popup); false when the code differs or there is no player.
bool AwardItemLot(std::uint32_t lot);
/// Gives `count` of goods `id` directly; false when the code differs or there is no player.
bool GiveGoods(std::uint32_t goods_id, int count);

} // namespace coop

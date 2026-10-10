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

// Item type bits of the inventory / GiveItemDirect (EMEVD 2003[43] type 0..3).
constexpr std::uint32_t kItemWeapon = 0x00000000u, kItemProtector = 0x10000000u, kItemAccessory = 0x20000000u,
                        kItemGoods = 0x40000000u;
/// How many of item `id` (type bits above) the local player holds: 0 none, -1 unknown.
int ItemCount(std::uint32_t type_bits, std::uint32_t id);
/// GiveItemDirect 0x131CB70(0, type bits, id, count); false when the code differs or no player.
/// It shows nothing; ShowItemGet adds the native "item acquired" entry.
bool GiveItem(std::uint32_t type_bits, std::uint32_t id, int count);
/// The native item-get dialog entry, exactly as the lot award's give list (0x17D89F0 at
/// 0x17D9348) queues it: 0x16D8850(*0x553B1C0, &gaitem handle, type bits, id, count, 0) - pushes
/// the entry onto the item-get list and plays sound 300. The handle is the inventory entry's
/// (+0), so call it after the item is held. False (nothing queued) when the code differs, there
/// is no list / free node or the item is not in the inventory.
bool ShowItemGet(std::uint32_t type_bits, std::uint32_t id, int count);
/// The lamp / ReturnPoint warp 0x13CDF30(id) (EMEVD 2003[49], the lamp menu); main thread, own
/// world. False when the code differs from 1.09 or there is no player.
bool LampWarp(std::uint32_t return_point);

// ---- Cutscene skip ----
/// The game's own remo skip request, the path its skip input takes: 0x1D07400(SprjRemo
/// *0x5540058, &id) -> 0x1CF9D40(remo player, &id): with a skippable remo playing (player +0x168
/// bit 1) and id matching (-1 = any), solo it sets +0x168 |= 0x20 (the player ends the scene with
/// its fade), in multiplayer it sets the scene's own skip vote (+0x13D). Main thread. False when
/// the code differs or there is no remo object; *skippable reports bit 1.
bool RequestRemoSkip(bool* skippable = nullptr);

// ---- Save requests (summoned writes: C2 pool flags, C3 direct gives, C4 story state) ----
// A guest that stays summoned (seamless play) may go a long time without the game saving: what
// the party code put into its own pool / inventory would be lost by a crash or a kill. The Lua
// binding SaveRequest 0x1336DA0 (registered at 0x1342F89: GSM +0x1578 "save allowed" -> +0x1548 = 1,
// +0x1547 profile, +0x1549 = 1) asks the game's own save task to write.
/// Any thread: asks for one save soon (coalesced; `why` goes to the log).
void RequestGameSave(const char* why);
/// Main thread, once a frame (CoopTick): runs a pending request once the world is steady, at most
/// one per 15 s; byte-verified (0x1336DA0 differs -> off, logged once).
void GameSaveTick();

} // namespace coop

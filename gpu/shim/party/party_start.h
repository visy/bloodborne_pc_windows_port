// SPDX-License-Identifier: GPL-3.0-or-later
// Party campaign start (C1, docs/party/campaign_start.md §4): when is a player "ready" for the
// party's summons, and which bells it still lacks. Pure logic, no game access (the director feeds
// it the flags read by game_state.h ReadStartFlags), so it is unit-tested on its own
// (tests/test_party_start.cpp, target party-start-test).
//
// BB_PARTY_START=prologue_solo (default): everyone plays the prologue alone, in parallel; a player
//   is ready at 12410000 (opening cutscene done) && 9401 (first Dream cutscene) && 12101020 &&
//   12101021 (both starting weapons taken) && 9180 OFF (no scripted cutscene). Until then the
//   member's roster state is Prologue (not Home), so the host does not ring for it, and it rings
//   nothing itself.
// BB_PARTY_START=immediate (experimental, §4.5): ready as soon as 12410000 is ON and 9180 OFF
//   (summons in the clinic, with the `Bells anywhere` patches; guests are unarmed).
#pragma once

#include "party_link.h"

#include <cstdint>
#include <string>
#include <vector>

namespace coop {

enum class StartMode { PrologueSolo, Immediate };

/// "prologue_solo" / "immediate" (case-insensitive; also "solo", "prologue", "clinic"); empty or
/// unknown -> PrologueSolo (*known = false for an unknown non-empty value).
StartMode ParseStartMode(const char* text, bool* known = nullptr);
const char* StartModeName(StartMode m);

// Flags (GetEventFlagValue 0x13CFD80 on EventFlagMan *0x553B100).
constexpr std::uint32_t kFlagOpeningDone = 12410000;  // cutscene after character creation
constexpr std::uint32_t kFlagCutscenePending = 9180;  // a scripted cutscene pending / playing
constexpr std::uint32_t kFlagFirstDream = 9401;       // first Hunter's Dream cutscene done
constexpr std::uint32_t kFlagFirstDeathToBase = 9402; // first death in m24_01 seen
constexpr std::uint32_t kFlagWeaponRight = 12101020;  // right-hand starting weapon taken
constexpr std::uint32_t kFlagWeaponLeft = 12101021;   // left-hand starting weapon taken
constexpr std::uint32_t kFlagDollAwake = 12100105;
constexpr std::uint32_t kFlagBeckoningLot = 6622;     // lot 10010 (Beckoning Bell) received
constexpr std::uint32_t kFlagResonantShop = 6610;     // Small Resonant Bell bought
constexpr std::uint32_t kGoodsBeckoning = 200;
constexpr std::uint32_t kGoodsSmallResonant = 205;
constexpr std::uint32_t kLotBeckoning = 10010;

/// What the director read this tick. `flags_ok` false: EventFlagMan missing (title) or the
/// flag function's code differs; then every flag is unknown.
struct StartFlags {
    bool flags_ok = false;
    bool opening_done = false, cutscene = false, first_dream = false, first_death = false;
    bool weapon_right = false, weapon_left = false, doll_awake = false;
    bool beckoning_lot = false, resonant_shop = false;
    int beckoning_count = -1, resonant_count = -1; ///< goods in the inventory; -1 unknown
};

/// Where a player is in the start sequence (§4.4). Ordered: a later step implies the earlier ones.
enum class StartStep {
    NoWorld = 0,   ///< title / character creation / no player record
    Unknown,       ///< in the world, flags unreadable
    Opening,       ///< 12410000 OFF: the opening cutscene (or before it)
    Clinic,        ///< opening done, 9401 OFF: clinic / Central Yharnam until the first death
    FirstDream,    ///< 9401 ON, starting weapons not both taken
    Ready,
};
const char* StartStepName(StartStep s);

/// The step from the flags. `in_world`: world up and not loading.
StartStep ClassifyStart(const StartFlags& f, bool in_world);

/// Ready for the party's summons under `mode`: in the world, flags readable, no cutscene pending,
/// and (prologue_solo) step Ready or (immediate) the opening done.
bool StartReady(StartMode mode, const StartFlags& f, bool in_world);

/// One line for logs ("step clinic: 12410000 1, 9180 0, 9401 0, ...").
std::string DescribeStart(const StartFlags& f, bool in_world);

/// The bells a ready player in its own world should be given (BB_PARTY_GRANT_BELLS, §4.3).
struct BellGrant {
    bool beckoning_lot = false;   ///< award lot 10010 (Beckoning Bell, sets 6622)
    bool beckoning_goods = false; ///< 6622 already ON but no bell: goods 200 directly
    bool resonant = false;        ///< goods 205 + flag 6610
    bool any() const { return beckoning_lot || beckoning_goods || resonant; }
};
/// Nothing when the counts are unknown (< 0) or the flags unreadable.
BellGrant PlanBellGrant(const StartFlags& f);

/// The roster state of a player idle in its own world: Home once ready, else Prologue.
party::MemberState OwnWorldState(bool ready);
/// A party member (not the host) waits for the host's bell: connected and Home or Joining
/// (a member still in its Prologue does not count).
bool MemberWaits(const std::vector<party::RosterEntry>& roster);
/// Lobby decision for the automatic bells (the session / loading / interval gates are the
/// director's). Host: ready itself, room for a cooperator, and a member waits. Guest: ready itself
/// and connected to the host.
bool HostMayRing(bool host_ready, int cooperators, int max_players, const std::vector<party::RosterEntry>& roster);
bool GuestMayRing(bool guest_ready, bool connected);

} // namespace coop

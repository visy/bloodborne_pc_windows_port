// SPDX-License-Identifier: GPL-3.0-or-later
// The party game-data tables derived from the player's own game files (docs/party/gamedata.md). The same
// derivation the Python tools of scripts/partydata did (kept there as the test oracle), in C++.
#pragma once

#include "events.h"
#include "param.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace coop::gamedata {

using Row = std::vector<std::string>;
using Sections = std::map<std::string, std::vector<Row>>;  // section name -> rows of fields (as the cache text)

struct FileEvents {
    std::string stem;  // file name up to the first '.'
    std::vector<bbdata::EventRec> recs;
};

struct ParamSet {
    bbdata::ParamTable item_lot, goods, weapon, protector, return_point, shop;
};

enum class Cat : std::uint8_t {
    none, boss_defeated, boss_area, lamp_unlocked, shortcut_door, npc_quest, item_lot_picked, key_event,
    cutscene_seen, world_state, session_runtime, never_sync, time_of_day, covenant_outcome
};

/// One row of the category table: {first, last, category, note}.
struct FlagCatRow {
    std::int64_t lo, hi;
    Cat cat;
    std::string note;
};
const char* CatName(Cat c);

/// The event flag category table of the game (first match wins: covenant rows, overrides, the rows the scripts
/// and params give, the global and per-map defaults).
class FlagCategories {
public:
    FlagCategories(const std::vector<FileEvents>& events, const ParamSet& params);
    /// Cat::none when no row covers the flag.
    Cat Lookup(std::int64_t flag) const;
    /// The whole ordered table (the flagRows section).
    std::vector<FlagCatRow> Rows() const;

private:
    struct Spec {
        Cat cat;
        std::string note;
        int prio;
    };
    std::map<std::int64_t, Spec> spec_;
};

/// itemLotFlag, itemLedger, itemAwardSpot, itemUnshared, itemSharedTreasure, itemBossDrop, itemRequireShared,
/// itemPerPlayer, itemKillAward.
Sections DeriveItems(const ParamSet& params, const std::vector<FileEvents>& events, const FlagCategories& cats);
/// flagRows.
Sections DeriveFlagRows(const FlagCategories& cats);
/// warperRight, warperLeft, warperArmor.
Sections DeriveWarper(const ParamSet& params);

/// Text of a float with `digits` decimals, correctly rounded (what Python's "%.Nf" prints).
std::string FixedText(double v, int digits);

}  // namespace coop::gamedata

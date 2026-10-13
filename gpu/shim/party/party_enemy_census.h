// SPDX-License-Identifier: GPL-3.0-or-later
// H4 "no unnecessary respawns": a census of the loaded map's enemies (entity id, alive) and the
// difference between two censuses. Pure logic (unit test: tests/test_party_enemy_census.cpp); the
// game side is CensusTick in party_balance.cpp (BB_PARTY_ENEMY_CENSUS=1).
//
// A regular enemy that was dead in one census and alive in a later one WITHOUT a load in between
// respawned: the only thing that may do that in a party is a rest (a load) or a wipe (a load), so
// a respawn seen in the same loaded world after a member's death is an H4 bug.
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

struct EnemyCensusEntry {
    std::uint32_t id = 0;  ///< entity id (chr +0x3B8); 0 = no entity id (skipped)
    bool alive = false;    ///< HP > 0
};

struct EnemyCensusDiff {
    int alive = 0;                       ///< alive in the new census
    int dead = 0;                        ///< dead in the new census
    std::vector<std::uint32_t> respawned;  ///< dead before, alive now (same loaded world)
    std::vector<std::uint32_t> died;       ///< alive before, dead now
    std::vector<std::uint32_t> gone;       ///< in the old census, not in the new one (unloaded)
    bool Changed() const { return !respawned.empty() || !died.empty() || !gone.empty(); }
};

/// before / after: two censuses of the same loaded world (no load in between). Entities without
/// an id are ignored; an entity new in `after` only counts (it streamed in).
inline EnemyCensusDiff DiffEnemyCensus(const std::vector<EnemyCensusEntry>& before,
                                       const std::vector<EnemyCensusEntry>& after) {
    EnemyCensusDiff d;
    for (const auto& a : after) {
        if (!a.id) continue;
        a.alive ? ++d.alive : ++d.dead;
        const auto it = std::find_if(before.begin(), before.end(), [&](const EnemyCensusEntry& b) { return b.id == a.id; });
        if (it == before.end()) continue;
        if (!it->alive && a.alive) d.respawned.push_back(a.id);
        if (it->alive && !a.alive) d.died.push_back(a.id);
    }
    for (const auto& b : before) {
        if (!b.id) continue;
        const bool still = std::any_of(after.begin(), after.end(), [&](const EnemyCensusEntry& a) { return a.id == b.id; });
        if (!still) d.gone.push_back(b.id);
    }
    return d;
}

/// The census to compare the next one with: the new census, plus the dead entities of the old one
/// that left it (unloaded chrs of the same world stay dead: they must not come back alive when they
/// stream in again).
inline std::vector<EnemyCensusEntry> CarryEnemyCensus(const std::vector<EnemyCensusEntry>& before,
                                                      const std::vector<EnemyCensusEntry>& after) {
    std::vector<EnemyCensusEntry> out = after;
    for (const auto& b : before) {
        if (!b.id || b.alive) continue;
        const bool in = std::any_of(after.begin(), after.end(), [&](const EnemyCensusEntry& a) { return a.id == b.id; });
        if (!in) out.push_back(b);
    }
    return out;
}

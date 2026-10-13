// SPDX-License-Identifier: GPL-3.0-or-later
// H4 enemy census (party_enemy_census.h).
#include "party/party_enemy_census.h"

#include <cassert>
#include <cstdio>

int main() {
    using V = std::vector<EnemyCensusEntry>;
    // Nothing changes: no diff.
    {
        const V a{{1, true}, {2, false}};
        const auto d = DiffEnemyCensus(a, a);
        assert(!d.Changed() && d.alive == 1 && d.dead == 1);
    }
    // A kill and a respawn in the same world.
    {
        const V a{{1, true}, {2, false}, {3, true}};
        const V b{{1, false}, {2, true}, {3, true}};
        const auto d = DiffEnemyCensus(a, b);
        assert(d.Changed() && d.died.size() == 1 && d.died[0] == 1);
        assert(d.respawned.size() == 1 && d.respawned[0] == 2);
        assert(d.alive == 2 && d.dead == 1 && d.gone.empty());
    }
    // Id 0 is ignored; a new entity only counts; an unloaded one is gone.
    {
        const V a{{0, false}, {5, true}};
        const V b{{0, true}, {6, true}};
        const auto d = DiffEnemyCensus(a, b);
        assert(d.respawned.empty() && d.died.empty() && d.alive == 1 && d.dead == 0);
        assert(d.gone.size() == 1 && d.gone[0] == 5);
    }
    // A dead enemy that streams out and back in alive is a respawn (the carry keeps it).
    {
        const V a{{7, false}, {8, true}};
        const V b{{8, true}};
        const V carried = CarryEnemyCensus(a, b);
        assert(carried.size() == 2);
        const V c{{7, true}, {8, true}};
        const auto d = DiffEnemyCensus(carried, c);
        assert(d.respawned.size() == 1 && d.respawned[0] == 7);
        // An alive one that left is not carried.
        assert(CarryEnemyCensus(V{{9, true}}, V{}).empty());
    }
    std::puts("party-enemy-census-test: ok");
    return 0;
}

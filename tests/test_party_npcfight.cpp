// SPDX-License-Identifier: GPL-3.0-or-later
// NPC hunter fights with the whole party (gpu/shim/party/party_npcfight_rules.h, ERSC parity D11).
#include "party/party_npcfight_rules.h"

#include <cassert>
#include <cstdio>
#include <iterator>
#include <set>

using namespace coop::npcfight;

int main() {
    // Slots: only the AI-player summons use them. The party's default (3 players) leaves one; a
    // 4-player party fills the game's four-player limit; the 5-entry table never overflows.
    assert(AiSummonFits(1, 0) && AiSummonFits(2, 1) && AiSummonFits(3, 0));
    assert(!AiSummonFits(3, 1) && !AiSummonFits(4, 0) && !AiSummonFits(5, 0) && !AiSummonFits(0, 0));
    for (int h = 1; h <= kSlotEntries; ++h)
        for (int n = 0; n <= kSlotEntries; ++n)
            if (AiSummonFits(h, n)) assert(h + n + 1 <= kVanillaMaxPlayers && h + n + 1 <= kSlotEntries);

    // The table: one gate per event, sorted out of the scan; every map one the party plays.
    std::set<int> seen;
    for (const GuestRun& g : kGuestRuns) {
        assert(seen.insert(g.event).second && g.index >= 0 && g.who && g.area_block >= 2100);
        assert(GuestRunFor(g.event) == &g);
    }
    assert(!GuestRunFor(12410811) && !GuestRunFor(12410806));  // Eileen's ally / hostile at the Tomb run everywhere

    // Alfred in Cainhurst (12500053: 1003[6] "end if client" at 0, team 27 at 7, flags at 8 / 9, save at 10).
    assert(Decide(12500053, 0, 1003, 6, true, true) == Act::pass_gate);
    assert(Decide(12500053, 0, 1003, 6, false, true) == Act::restore_gate);  // the host: the data's own
    assert(Decide(12500053, 0, 1003, 6, true, false) == Act::restore_gate);  // the rule off: vanilla
    assert(Decide(12500053, 7, 2004, 2, true, true) == Act::none);           // the team switch runs
    assert(Decide(12500053, 6, 2003, 2, true, true) == Act::skip);           // its flag writes do not
    assert(Decide(12500053, 8, 2003, 22, true, true) == Act::skip);
    assert(Decide(12500053, 10, 2000, 5, true, true) == Act::skip);
    assert(Decide(12500053, 6, 2003, 2, false, true) == Act::none);           // the host writes them
    assert(Decide(12500053, 6, 2003, 2, true, false) == Act::none);
    // The blood-drunk Eileen (12400656: 4[3] at 1): only that 4[3] is the gate.
    assert(Decide(12400656, 1, 4, 3, true, true) == Act::pass_gate);
    assert(Decide(12400656, 2, 4, 3, true, true) == Act::none);
    assert(Decide(12400656, 1, 3, 0, true, true) == Act::none);
    assert(Decide(12400656, 9, 2004, 2, true, true) == Act::none);
    // Another event is never touched, whatever it holds.
    assert(Decide(12410803, 0, 4, 3, true, true) == Act::none && Decide(12410803, 5, 2003, 22, true, true) == Act::none);
    assert(ProgressWrite(2003, 2) && ProgressWrite(2000, 5) && !ProgressWrite(2004, 2) && !ProgressWrite(2004, 8) &&
           !ProgressWrite(2003, 4));

    // Assassins: shown while the host's appear flag is on, hidden again only when we showed it.
    assert(PlanAssassin(true, true, false, true, false).show);
    assert(!PlanAssassin(true, true, false, false, true).show && !PlanAssassin(true, true, false, false, true).hide);
    assert(PlanAssassin(true, false, false, false, true).hide);
    assert(!PlanAssassin(true, false, false, false, false).hide);  // the guest's own state: left alone
    assert(!PlanAssassin(true, true, true, true, false).show);     // repelled / dead: nothing
    assert(PlanAssassin(false, true, false, false, true).hide && !PlanAssassin(false, true, false, true, false).show);
    for (const Assassin& a : kAssassins) assert(a.appeared != a.done && a.entity / 100000 >= 35);

    // Covenant outcomes are per player: Annalise's flesh (1042), Alfred in the castle (1348), Valtr.
    assert(CovenantOutcomeFlag(1042) && CovenantOutcomeFlag(1348) && CovenantOutcomeFlag(1805));
    assert(!CovenantOutcomeFlag(1367) && !CovenantOutcomeFlag(12410810) && !CovenantOutcomeFlag(1039));
    std::puts("party-npcfight-test: ok");
    return 0;
}

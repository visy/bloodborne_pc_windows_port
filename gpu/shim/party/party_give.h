// SPDX-License-Identifier: GPL-3.0-or-later
// 0.7.8 (giveitem078, CL-63e235): give items to a party member, the game side. The rules, the wire and the two state
// machines are party_give_rules.h (unit test tests/test_party_give.cpp); this file reads the inventory, moves the
// items and talks over the party link.
//
//   player   Insert menu > Party tab > "Give items": an item (only what the player holds and may give), a count and
//            a member, then Give. Needs the own switch (bbport.ini party_give_items, default OFF; System > My party:
//            Your items; the launcher's My Party tab) - the receiver's switch is its consent.
//   link     EVENT give {op: offer / ready / refuse / commit / abort / done}. A guest sends to the host, who relays to
//            the addressed member and sets "from" to the sender's slot (so host -> guest, guest -> host and
//            guest -> guest all work). Nothing is stored: the transfer lives in memory, a restarted game knows no id.
//   giver    OFFER, then on READY removes the items (the count must drop by exactly n, else they are put back), then
//            COMMIT, then DONE {got}; a missing DONE is asked for (ABORT); only a receiver that cannot be asked for
//            kAbortWaitS / kLinkDownS makes the giver put the whole amount back (the "rolled back" notice).
//   receiver checks the moment (loading / cutscene / boss fight / dead / travelling), the goods rules on its own param
//            data and its room (inventory up to maxNum, then the storage box), answers READY, and gives on COMMIT with
//            the game's own item-get dialog and the notice "<item> xN from <name>".
//   nothing  during a loading screen, a cutscene, a boss fight, a party travel, while dead; no key / unique / story
//            items (party_give_rules.h GoodsAllowed), a quick-slot item always keeps one.
//
// Env: BB_PARTY_GIVE_ITEMS=1 switches the feature on whatever the ini says (tests). Test aids: BB_PARTY_GIVE_TEST=
// "<goods>:<n>:<slot>@<s>[,...]" (this player gives n of goods to the member in `slot` s seconds after its world is
// steady; every request is logged with the reason when refused) and BB_PARTY_GIVE_FAULT=mute (a receiver ignores COMMIT
// and ABORT: the giver's timeout roll-back) | linkdrop (the giver drops its link right after COMMIT).
#pragma once

#include "party_give_rules.h"

#include <cstdint>
#include <string>
#include <vector>

namespace party {
class PartyLink;
}
namespace coop {
struct GameSnapshot;
}

namespace coop::give {

/// The own switch (BbSettings party_give_items, or BB_PARTY_GIVE_ITEMS=1). Any thread.
bool Enabled();

/// What the overlay shows (a copy; refreshed by Tick every ~0.5 s while UiWanted was called in the last 2 s).
struct UiItem {
    std::uint32_t id = 0;
    int held = 0;
    int max_give = 0;  ///< the most one give may carry
    std::string name;
};
struct UiMember {
    int slot = 0;
    std::string name;  ///< the shown (filtered) name
};
struct UiView {
    bool available = false;  ///< a party runs, the switch is on, the inventory was read
    bool busy = false;       ///< a transfer is open
    std::vector<UiItem> items;
    std::vector<UiMember> members;
    std::string status;      ///< the last result line ("Gave Blood Vial x3 to Ann" ...)
};
/// The overlay is open on the Party tab (any thread): keeps the snapshot fresh.
void UiWanted();
UiView UiSnapshot();
/// The player pressed Give (any thread): handled on the next tick, the result goes to the party notice corner.
void RequestGive(std::uint32_t goods, int n, int to_slot);

/// The link's callback thread: EVENT give from `slot`. Only queues.
void OnEvent(int slot, const std::string& body);
/// Main thread, once a frame (the director). `host`: this game hosts the party.
void Tick(const GameSnapshot& s, party::PartyLink* link, bool host);

}  // namespace coop::give

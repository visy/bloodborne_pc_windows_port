// SPDX-License-Identifier: GPL-3.0-or-later
// Party items (gpu/shim/party/party_items, phase C3), the parts without the game: the lot / flag /
// ledger tables, the deny rules, award and flag classification, the EVENT "items" JSON round trip,
// the host's send-once ledger and the guest's queue / apply gate with an injected game state.
// Build and run: ninja -C out/gpu party-items-test && out/gpu/party-items-test.exe
#include "party/party_items.h"

#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace coop;

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        ++g_checks;                                                                    \
        if (!(cond)) {                                                                 \
            ++g_failures;                                                              \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                              \
    } while (0)

static void test_tables() {
    CHECK(ItemLotFlagCount() > 1000);
    CHECK(ItemLedgerCount() >= 40);
    // Treasure: flag = 50,000,000 + lot (docs/party/items.md 2.1; probe I2's Hunter Chief Emblem).
    CHECK(ItemLotFlag(2400450) == 52400450);
    CHECK(ItemLotForFlag(52400450) == 2400450);
    CHECK(ItemLotFlag(2300090) == 52300090); // the BB_PARTY_ITEMS_TEST Blood Vial
    // Non-treasure rows: flag != 50M + lot.
    CHECK(ItemLotFlag(2700990) == 52700950);
    CHECK(ItemLotForFlag(52700950) == 2700990);
    CHECK(ItemLotFlag(80000000) == 5000);
    CHECK(ItemLotForFlag(5000) == 80000000);
    // Shared flag (NG variants / chains): the lowest lot.
    CHECK(ItemLotFlag(33000) == 50001700 && ItemLotFlag(33010) == 50001700);
    CHECK(ItemLotForFlag(50001700) == 33000);
    CHECK(ItemLotForFlag(6622) == 10010); // 10010 / 10011 chain
    // Flagless boss lots are not in the flag table.
    CHECK(ItemLotFlag(31000) == kItemNone);
    CHECK(ItemLotFlag(50000001) == kItemNone);
    CHECK(ItemLotForFlag(12345) == kItemNone);
    CHECK(ItemLotForFlag(-1) == kItemNone);
    // Chalice Dungeons are not in the table.
    CHECK(ItemLotFlag(110001000) == kItemNone);
    // Ledger: stable indices (append only), done flag = the awarding event's completion flag.
    CHECK(ItemLedgerIndex(31000) == 25);           // Oedon Tomb Key, Gascoigne
    CHECK(ItemLedgerDoneFlag(25) == 12411800);
    CHECK(ItemLedgerLot(25) == 31000);
    CHECK(ItemLedgerIndex(50000001) == 35);        // Gold Pendant, Cleric Beast
    CHECK(ItemLedgerDoneFlag(35) == 12401800);
    CHECK(ItemLedgerIndex(14000) == 0);
    CHECK(ItemLedgerIndex(22000) >= 0 && ItemLedgerDoneFlag(ItemLedgerIndex(22000)) == 9045); // common 9040 slot 5
    CHECK(ItemLedgerIndex(17010) == kItemNone);    // repeatable (common 9100)
    CHECK(ItemLedgerIndex(43000) == kItemNone);    // repeatable (13501940)
    CHECK(ItemLedgerIndex(2400450) == kItemNone);  // flagged
    for (std::size_t i = 0; i < ItemLedgerCount(); ++i) {
        const int lot = ItemLedgerLot(static_cast<std::int32_t>(i));
        CHECK(lot > 0);
        CHECK(ItemLotFlag(lot) == kItemNone);
        CHECK(!ItemLotDenied(lot, kItemNone));
    }
}

static void test_deny() {
    CHECK(ItemLotDenied(0, kItemNone));
    CHECK(ItemLotDenied(10010, 6622));       // Messenger: Beckoning Bell (per player)
    CHECK(ItemLotDenied(10040, 50000100));   // Messenger, flag outside the personal block
    CHECK(ItemLotDenied(10050, 6670));
    CHECK(ItemLotDenied(43120, 6676));       // DLC NPC gift with a personal flag
    CHECK(!ItemLotDenied(3401800, 6674));    // Ludwig's reward: exception
    CHECK(!ItemLotDenied(3401850, 6673));    // Laurence's reward: exception
    CHECK(ItemLotDenied(100500, kItemNone)); // rune use
    CHECK(ItemLotDenied(5510, kItemNone));   // PvP
    CHECK(ItemLotDenied(16581, kItemNone));
    CHECK(ItemLotDenied(2100910, kItemNone));
    CHECK(ItemLotDenied(110001000, kItemNone));
    CHECK(ItemLotDenied(2900100, 52900100)); // area 29
    CHECK(ItemLotDenied(43000, kItemNone));
    CHECK(!ItemLotDenied(2400450, 52400450));
    CHECK(!ItemLotDenied(31000, kItemNone));
    CHECK(!ItemLotDenied(80000000, 5000));
}

static void test_classify() {
    // Client lots (hostOnly 0) never: clients get them in vanilla.
    CHECK(!ClassifyAward(2400450, false));
    auto a = ClassifyAward(31000, true);
    CHECK(a && a->lot == 31000 && a->flag == kItemNone && a->ledger == 25 && a->source == ItemSource::Award);
    CHECK(a && a->DoneFlag() == kItemLedgerBase + 25);
    a = ClassifyAward(80000000, true);
    CHECK(a && a->flag == 5000 && a->ledger == kItemNone && a->DoneFlag() == 5000);
    CHECK(!ClassifyAward(10010, true));   // denied
    CHECK(!ClassifyAward(17010, true));   // flagless, not in the ledger (repeatable)
    CHECK(!ClassifyAward(999999, true));  // unknown lot
    a = ClassifyAward(3401800, true);
    CHECK(a && a->flag == 6674);

    std::map<std::int32_t, std::int32_t> captured;
    auto f = ClassifyFlag(52400450, captured);
    CHECK(f && f->lot == 2400450 && f->flag == 52400450 && f->source == ItemSource::Flag);
    f = ClassifyFlag(50001700, captured);
    CHECK(f && f->lot == 33000);
    captured[50001700] = 33010; // the hook saw the NG variant
    f = ClassifyFlag(50001700, captured);
    CHECK(f && f->lot == 33010);
    CHECK(!ClassifyFlag(6622, captured));      // per-player Messenger flag
    CHECK(!ClassifyFlag(12411800, captured));  // not an item flag
    CHECK(!ClassifyFlag(52900100, captured));  // chalice
}

static void test_json() {
    std::vector<ItemGrant> in(3);
    in[0] = {1700000000123ull, 2400450, 52400450, kItemNone, ItemSource::Flag};
    in[1] = {1700000000124ull, 31000, kItemNone, 25, ItemSource::Award};
    in[2] = {7, 80000000, 5000, kItemNone, ItemSource::Full};
    const std::string text = ItemsToJsonText(in);
    std::vector<ItemGrant> out;
    std::string err;
    CHECK(ItemsFromJsonText(text, &out, &err));
    CHECK(out.size() == 3);
    for (std::size_t i = 0; i < out.size() && i < in.size(); ++i) {
        CHECK(out[i].seq == in[i].seq && out[i].lot == in[i].lot && out[i].flag == in[i].flag &&
              out[i].ledger == in[i].ledger && out[i].source == in[i].source);
    }
    CHECK(ItemsFromJsonText("{\"items\":[]}", &out, &err) && out.empty());
    CHECK(!ItemsFromJsonText("{\"items\":[[1,2,3]]}", &out, &err));
    CHECK(!ItemsFromJsonText("{\"items\":[[1,31000,-1,-1,\"award\"]]}", &out, &err)); // neither flag nor ledger
    CHECK(!ItemsFromJsonText("{\"items\":[[1,31000,-1,1000,\"award\"]]}", &out, &err)); // ledger out of range
    CHECK(!ItemsFromJsonText("{\"items\":[[1,0.5,5,-1,\"flag\"]]}", &out, &err));
    CHECK(!ItemsFromJsonText("[]", &out, &err));
    CHECK(!ItemsFromJsonText("not json", &out, &err));
    CHECK(ItemSourceFromName("mark") == ItemSource::Mark);
    CHECK(ItemSourceFromName("zzz") == ItemSource::Unknown);
}

static void test_host() {
    HostItems h;
    h.SeedSeq(100);
    ItemGrant g{0, 2400450, 52400450, kItemNone, ItemSource::Award};
    CHECK(h.Offer(g));
    CHECK(!h.Offer(g)); // same flag: once
    ItemGrant byflag{0, 2400450, 52400450, kItemNone, ItemSource::Flag};
    CHECK(!h.Offer(byflag)); // the C2 report after the hook
    CHECK(h.Offer({0, 31000, kItemNone, 25, ItemSource::Award}));
    CHECK(!h.Offer({0, 31000, kItemNone, 25, ItemSource::Award}));
    CHECK(!h.Offer({0, 1, kItemNone, kItemNone, ItemSource::Award})); // no key
    ItemGrant out;
    CHECK(h.Pop(&out) && out.lot == 2400450 && out.seq == 101);
    CHECK(h.Pop(&out) && out.lot == 31000 && out.seq == 102);
    CHECK(!h.Pop(&out));
    h.Capture(50001700, 33010);
    CHECK(h.Captured().at(50001700) == 33010);
}

static ItemApplyState Home() {
    ItemApplyState s;
    s.world_up = true;
    s.loading = false;
    s.load_mode = 0;
    s.own_world = 1;
    s.session_role = 0;
    s.game_data = true;
    return s;
}

static void test_gate() {
    CHECK(ItemApplyAllowedNow(Home()));
    ItemApplyState s = Home();
    s.load_mode = 1; // summoned: the overlay
    CHECK(!ItemApplyAllowedNow(s));
    s = Home();
    s.own_world = 0;
    CHECK(!ItemApplyAllowedNow(s));
    s = Home();
    s.loading = true;
    CHECK(!ItemApplyAllowedNow(s));
    s = Home();
    s.session_role = 6; // client
    CHECK(!ItemApplyAllowedNow(s));
    s.session_role = 4; // joining
    CHECK(!ItemApplyAllowedNow(s));
    s.session_role = 3; // hosting in the own world
    CHECK(ItemApplyAllowedNow(s));
    s = Home();
    s.game_data = false;
    CHECK(!ItemApplyAllowedNow(s));
    s = Home();
    s.world_up = false;
    CHECK(!ItemApplyAllowedNow(s));
}

static void test_guest() {
    GuestItems q;
    std::vector<ItemGrant> in = {{1, 2400450, 52400450, kItemNone, ItemSource::Flag},
                                 {2, 31000, kItemNone, 25, ItemSource::Award}};
    CHECK(q.Offer(in) == 2);
    CHECK(q.Offer(in) == 0); // already queued
    CHECK(q.Pending() == 2);
    double t = 100.0;
    ItemApplyState away = Home();
    away.load_mode = 1;
    // Summoned: nothing, however long.
    for (int i = 0; i < 500; ++i) {
        CHECK(!q.Step(away, t));
        t += 0.016;
    }
    // A mark that arrives while summoned goes first.
    CHECK(q.Offer({{0, 43710, 50002560, kItemNone, ItemSource::Mark}}) == 1);
    // Home: the gate must hold kStableTicks frames first.
    int got_at = -1;
    std::optional<ItemGrant> g;
    for (int i = 0; i < GuestItems::kStableTicks + 5; ++i) {
        g = q.Step(Home(), t);
        t += 0.016;
        if (g) {
            got_at = i;
            break;
        }
    }
    CHECK(got_at == GuestItems::kStableTicks - 1);
    CHECK(g && g->source == ItemSource::Mark && g->lot == 43710);
    q.Done(g->Key());
    // The next (a real award) right after a mark (no popup spacing for marks).
    g = q.Step(Home(), t);
    CHECK(g && g->lot == 2400450);
    q.Done(g->Key());
    // Then spacing.
    t += 0.016;
    CHECK(!q.Step(Home(), t));
    t += GuestItems::kSpacing;
    g = q.Step(Home(), t);
    CHECK(g && g->lot == 31000);
    q.Done(g->Key());
    CHECK(q.Pending() == 0);
    // Done keys are not taken again (a resent full list).
    CHECK(q.Offer(in) == 0);
    CHECK(q.IsDone(52400450));
    CHECK(q.IsDone(std::int64_t(kItemLedgerBase) + 25));
    // A loading screen resets the stability counter.
    CHECK(q.Offer({{3, 80000000, 5000, kItemNone, ItemSource::Flag}}) == 1);
    ItemApplyState loading = Home();
    loading.loading = true;
    CHECK(!q.Step(loading, t + 10));
    CHECK(!q.Step(Home(), t + 10)); // stable count restarted at 1
}

int main() {
    test_tables();
    test_deny();
    test_classify();
    test_json();
    test_host();
    test_gate();
    test_guest();
    std::printf("party-items-test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

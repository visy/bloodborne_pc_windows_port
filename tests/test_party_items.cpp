// SPDX-License-Identifier: GPL-3.0-or-later
// Party items (gpu/shim/party/party_items, phase C3), the parts without the game: the lot / flag /
// ledger tables, the deny rules, award and flag classification, the EVENT "items" JSON round trip,
// the host's send-once ledger and the guest's queue / apply gate with an injected game state.
// Build and run: ninja -C out/gpu party-items-test && out/gpu/party-items-test.exe
#include "party/party_items.h"

#include <cstdio>
#include <map>
#include <set>
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
    q.Awarded(t); // it opened a popup
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

// The Cathedral Ward report: lot 50000010 (ledger 36, event 12411700) gives goods 4114 (Sword
// Hunter Badge, maxNum 1, no storage); a guest whose save already holds it got the blocking
// "inventory or storage max" dialog. Such a lot is now skipped.
static void test_fit() {
    LotSlotState badge;
    badge.category = 4;
    badge.id = 4114;
    badge.num = 1;
    badge.owned = 1;
    badge.max_num = 1;
    badge.max_storage = 0;
    std::string why;
    CHECK(ClassifyLotFit({badge}, &why) == LotFit::Owned);
    CHECK(why.find("4114") != std::string::npos);
    badge.owned = 0;
    CHECK(ClassifyLotFit({badge}) == LotFit::Give);
    // Unknown count: never refused (the old behaviour).
    badge.owned = -1;
    CHECK(ClassifyLotFit({badge}) == LotFit::Give);
    // Consumables: 18 vials + 3 > 20 with storage room -> fits (overflow goes to storage).
    LotSlotState vials;
    vials.category = 4;
    vials.id = 1000;
    vials.num = 3;
    vials.owned = 18;
    vials.max_num = 20;
    vials.max_storage = 2;
    CHECK(ClassifyLotFit({vials}) == LotFit::Give);
    vials.max_storage = 0;
    CHECK(ClassifyLotFit({vials}) == LotFit::Full);
    vials.owned = 17;
    CHECK(ClassifyLotFit({vials}) == LotFit::Give);
    // Weapons have no count limit here; num 0 counts as 1.
    LotSlotState weapon;
    weapon.category = 0;
    weapon.id = 18100000;
    weapon.owned = 5;
    weapon.max_num = 1;
    CHECK(ClassifyLotFit({weapon}) == LotFit::Give);
    LotSlotState key = badge;
    key.id = 4000;
    key.owned = 1;
    key.num = 0;
    // One refused slot refuses the lot; Owned wins over Full.
    vials.owned = 20;
    CHECK(ClassifyLotFit({weapon, vials}) == LotFit::Full);
    CHECK(ClassifyLotFit({vials, key}) == LotFit::Owned);
    CHECK(ClassifyLotFit({}) == LotFit::Give);
    // A fight holds every popup back; marks still go.
    GuestItems q;
    CHECK(q.Offer({{1, 31000, kItemNone, 25, ItemSource::Full}}) == 1);
    ItemApplyState fight = Home();
    fight.calm = false;
    double t = 100;
    for (int i = 0; i < GuestItems::kStableTicks + 5; ++i, t += 0.016) {
        CHECK(!q.Step(fight, t));
    }
    CHECK(q.Step(Home(), t).has_value());
}

// Ledger lots a save earned solo (live run, first party session: 29000, 30000, 31000, 50000001,
// 50000010 given again) get their bits before C2 writes any host flag, so the host's full list
// skips them - and a boss the save never beat (its flag later synced by C2) keeps its lot.
static void test_ledger_seed() {
    std::set<std::uint32_t> save = {9066, 9067, 12401800, 12411700, 12411800, 12101800};
    auto is_set = [&save](std::uint32_t id) { return save.count(id) != 0; };
    const std::vector<std::uint32_t> bits = LedgerBitsToSeed(is_set);
    const std::set<std::uint32_t> got(bits.begin(), bits.end());
    CHECK(got.size() == bits.size()); // no duplicates
    for (std::int32_t idx = 0; idx < std::int32_t(ItemLedgerCount()); ++idx) {
        const bool want = save.count(std::uint32_t(ItemLedgerDoneFlag(idx))) != 0;
        CHECK(got.count(kItemLedgerBase + std::uint32_t(idx)) == (want ? 1u : 0u));
    }
    for (std::int32_t lot : {29000, 30000, 31000, 50000001, 50000010}) {
        const std::int32_t idx = ItemLedgerIndex(lot);
        CHECK(idx >= 0);
        CHECK(got.count(kItemLedgerBase + std::uint32_t(idx)) == 1);
        ItemGrant g{1, lot, kItemNone, idx, ItemSource::Full};
        CHECK(g.DoneFlag() == std::int64_t(kItemLedgerBase) + idx);
    }
    // m22's boss (12201800, lot 21002950) was not beaten here: no bit, the replay gives it even
    // after C2 has synced 12201800 into this save.
    CHECK(got.count(kItemLedgerBase + std::uint32_t(ItemLedgerIndex(21002950))) == 0);
    save.insert(kItemLedgerBase + std::uint32_t(ItemLedgerIndex(29000)));
    CHECK(LedgerBitsToSeed(is_set).size() == bits.size() - 1);
    save.insert(kItemLedgerSeeded);
    CHECK(LedgerBitsToSeed(is_set).empty());
    CHECK(ItemLedgerCount() < std::size_t(kItemLedgerSize - 1));
    CHECK(kItemLedgerSeeded == kItemLedgerBase + kItemLedgerSize - 1);
}


static ItemApplyState Summoned() {
    ItemApplyState s;
    s.world_up = true;
    s.load_mode = 1;
    s.own_world = 0;
    s.session_role = 6;
    s.game_data = true;
    return s;
}

static LotRowData Row(std::int32_t lot, std::int32_t flag) {
    LotRowData r;
    r.lot = lot;
    r.flag = flag;
    for (int i = 0; i < 8; ++i) {
        r.category[i] = 0xffffffffu;
    }
    return r;
}

static void Slot(LotRowData* r, int i, std::uint32_t cat, std::int32_t id, int num, int base) {
    r->category[i] = cat;
    r->id[i] = id;
    r->num[i] = static_cast<std::uint8_t>(num);
    r->base[i] = static_cast<std::uint16_t>(base);
}

static void test_summoned_gate() {
    CHECK(ItemSummonedApplyAllowedNow(Summoned()));
    CHECK(ItemApplyModeNow(Summoned()) == ItemApplyMode::Summoned);
    CHECK(ItemApplyModeNow(Home()) == ItemApplyMode::Home);
    ItemApplyState s = Summoned();
    s.load_mode = 0; // flag store not switched yet: neither
    CHECK(ItemApplyModeNow(s) == ItemApplyMode::None);
    s = Summoned();
    s.session_role = 4; // joining
    CHECK(ItemApplyModeNow(s) == ItemApplyMode::None);
    s = Summoned();
    s.loading = true;
    CHECK(ItemApplyModeNow(s) == ItemApplyMode::None);
    s = Summoned();
    s.game_data = false;
    CHECK(ItemApplyModeNow(s) == ItemApplyMode::None);
}

static void test_plan() {
    auto none = [](std::int32_t) { return false; };
    // Treasure: one row, one slot (Hunter Chief Emblem style).
    LotRowData t = Row(2400450, 52400450);
    Slot(&t, 0, 4, 4002, 1, 100);
    LotPlan p = PlanSummonedLot({t}, {}, none);
    CHECK(p.result == LotPlanResult::Give && p.items.size() == 1);
    CHECK(p.items[0] == (GrantItem{4, 4002, 1}));
    CHECK(p.row_flags == std::vector<std::int32_t>{52400450});
    // lotItemNum 0 counts as 1; a count is taken as is.
    t.num[0] = 0;
    CHECK(PlanSummonedLot({t}, {}, none).items[0].count == 1);
    t.num[0] = 3;
    CHECK(PlanSummonedLot({t}, {}, none).items[0].count == 3);
    // A chain (cap + garb) with a second row of its own flag already held: only the first.
    LotRowData a = Row(2110010, 52110010), b = Row(2110011, 52110011);
    Slot(&a, 0, 1, 230000, 1, 100);
    Slot(&b, 0, 1, 231000, 1, 100);
    p = PlanSummonedLot({a, b}, {}, [](std::int32_t f) { return f == 52110011; });
    CHECK(p.result == LotPlanResult::Give && p.items.size() == 1 && p.items[0].id == 230000);
    CHECK(p.row_flags == std::vector<std::int32_t>{52110010});
    p = PlanSummonedLot({a, b}, {}, none);
    CHECK(p.items.size() == 2 && p.row_flags.size() == 2);
    // A blood gem: own world only.
    LotRowData g = Row(17000, 50000200);
    Slot(&g, 0, 8, 10001, 1, 100);
    CHECK(PlanSummonedLot({g}, {}, none).result == LotPlanResult::Unsupported);
    // A random row without the host's list: own world only; with a matching list: that list.
    LotRowData r = Row(22502610, -1);
    Slot(&r, 0, 4, 1000, 2, 50);
    Slot(&r, 1, 4, 900, 5, 50);
    CHECK(PlanSummonedLot({r}, {}, none).result == LotPlanResult::Random);
    CHECK(LotChainRandom({r}) && !LotChainRandom({t}));
    p = PlanSummonedLot({r}, {{4, 900, 5}}, none);
    CHECK(p.result == LotPlanResult::Give && p.items.size() == 1 && p.items[0].id == 900);
    // A host list naming an item the lot cannot give, or a higher count: ignored (rows decide).
    CHECK(PlanSummonedLot({r}, {{4, 4114, 1}}, none).result == LotPlanResult::Random);
    CHECK(PlanSummonedLot({r}, {{4, 900, 6}}, none).result == LotPlanResult::Random);
    CHECK(PlanSummonedLot({t}, {{4, 4002, 1}, {4, 4002, 1}}, none).items.size() == 1); // 2 items, 1 row
    // The host's capture of a deterministic lot is used as is.
    p = PlanSummonedLot({t}, {{4, 4002, 2}}, none);
    CHECK(p.why == "host's item list" && p.items[0].count == 2);
    // An empty deterministic row gives nothing but still counts (its flag is set).
    LotRowData e = Row(5000, 50005000);
    e.base[0] = 100; // id 0: "nothing"
    p = PlanSummonedLot({e}, {}, none);
    CHECK(p.result == LotPlanResult::Give && p.items.empty() && p.row_flags.size() == 1);
    CHECK(PlanSummonedLot({}, {}, none).result == LotPlanResult::Missing);
    // The host's roll: always a slot of the row, the same for the same seed.
    for (std::uint64_t seed = 1; seed < 200; ++seed) {
        const std::vector<GrantItem> rolled = RollLotChain({r}, seed);
        CHECK(rolled.size() == 1 && (rolled[0].id == 1000 || rolled[0].id == 900));
        CHECK(rolled == RollLotChain({r}, seed));
        CHECK(PlanSummonedLot({r}, rolled, none).result == LotPlanResult::Give);
    }
    std::set<std::int32_t> seen;
    for (std::uint64_t seed = 1; seed < 200; ++seed) {
        seen.insert(RollLotChain({r}, seed)[0].id);
    }
    CHECK(seen.size() == 2);
    CHECK(GrantCategorySupported(0) && GrantCategorySupported(4) && !GrantCategorySupported(8) &&
          !GrantCategorySupported(3));
}

static void test_items_json() {
    ItemGrant g{5, 22502610, kItemNone, 30, ItemSource::Award};
    g.items = {{4, 900, 5}, {0, 12080000, 1}};
    std::vector<ItemGrant> out;
    std::string err;
    CHECK(ItemsFromJsonText(ItemsToJsonText({g}), &out, &err));
    CHECK(out.size() == 1 && out[0].items == g.items);
    // Old rows (5 elements) still parse, with no items.
    CHECK(ItemsFromJsonText(R"({"items":[[1,2400450,52400450,-1,"flag"]]})", &out, &err) && out[0].items.empty());
    // Bad item lists are refused.
    CHECK(!ItemsFromJsonText(R"({"items":[[1,2400450,52400450,-1,"flag",[[8,1,1]]]]})", &out, &err));
    CHECK(!ItemsFromJsonText(R"({"items":[[1,2400450,52400450,-1,"flag",[[4,0,1]]]]})", &out, &err));
    CHECK(!ItemsFromJsonText(R"({"items":[[1,2400450,52400450,-1,"flag",[[4,1,0]]]]})", &out, &err));
    CHECK(!ItemsFromJsonText(R"({"items":[[1,2400450,52400450,-1,"flag",[[4,1,256]]]]})", &out, &err));
    CHECK(!ItemsFromJsonText(R"({"items":[[1,2400450,52400450,-1,"flag",[[4,1]]]]})", &out, &err));
    CHECK(!ItemsFromJsonText(R"({"items":[[1,2400450,52400450,-1,"flag",{"a":1}]]})", &out, &err));
    CHECK(!ItemsFromJsonText(R"({"items":[[1,2400450,52400450,-1,"flag",[[-4,1,1]]]]})", &out, &err));
    std::string many = R"({"items":[[1,2400450,52400450,-1,"flag",[)";
    for (int i = 0; i < 17; ++i) {
        many += std::string(i ? "," : "") + "[4,1000,1]";
    }
    many += "]]]}";
    CHECK(!ItemsFromJsonText(many, &out, &err));
}

static void test_guest_summoned() {
    GuestItems q;
    std::vector<ItemGrant> in = {{1, 2400450, 52400450, kItemNone, ItemSource::Flag},
                                 {2, 17000, 50000200, kItemNone, ItemSource::Flag},
                                 {3, 31000, kItemNone, 25, ItemSource::Award}};
    CHECK(q.Offer(in) == 3);
    double t = 100.0;
    std::optional<ItemGrant> g;
    int got_at = -1;
    for (int i = 0; i < GuestItems::kStableTicks + 5 && !g; ++i) {
        g = q.Step(Summoned(), t);
        t += 0.016;
        got_at = i;
    }
    // Summoned: the same 90-frame settle, then in order.
    CHECK(got_at == GuestItems::kStableTicks - 1);
    CHECK(g && g->lot == 2400450 && q.LastMode() == ItemApplyMode::Summoned);
    q.Done(g->Key());
    q.Awarded(t);
    // The spacing / calm rules hold while summoned too.
    CHECK(!q.Step(Summoned(), t + 1));
    ItemApplyState fight = Summoned();
    fight.calm = false;
    CHECK(!q.Step(fight, t + GuestItems::kSpacing + 1));
    t += GuestItems::kSpacing + 2;
    g = q.Step(Summoned(), t);
    CHECK(g && g->lot == 17000);
    // A blood gem lot: deferred to the own world; the next one is not blocked by it.
    q.Defer(*g);
    CHECK(q.HomeOnly() == 1 && q.Pending() == 2);
    g = q.Step(Summoned(), t + 0.016);
    CHECK(g && g->lot == 31000);
    q.Done(g->Key());
    // Nothing else while summoned (the deferred one waits), however long.
    for (int i = 0; i < 300; ++i) {
        t += 0.1;
        CHECK(!q.Step(Summoned(), t));
    }
    // A mark arriving while summoned goes first and is applied at once (own pool).
    CHECK(q.Offer({{0, 43710, 50002560, kItemNone, ItemSource::Mark}}) == 1);
    g = q.Step(Summoned(), t);
    CHECK(g && g->source == ItemSource::Mark);
    q.Done(g->Key());
    // Home: the deferred lot comes (after the gate settles in the new mode).
    g.reset();
    for (int i = 0; i < GuestItems::kStableTicks + 5 && !g; ++i) {
        t += 0.016;
        g = q.Step(Home(), t);
    }
    CHECK(g && g->lot == 17000 && q.LastMode() == ItemApplyMode::Home);
    q.Done(g->Key());
    CHECK(q.Pending() == 0 && q.HomeOnly() == 0);
    // Done keys stay done (a resent full list after a rejoin).
    CHECK(q.Offer(in) == 0);
}

int main() {
    test_tables();
    test_fit();
    test_ledger_seed();
    test_deny();
    test_classify();
    test_json();
    test_host();
    test_gate();
    test_guest();
    test_summoned_gate();
    test_plan();
    test_items_json();
    test_guest_summoned();
    std::printf("party-items-test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

// SPDX-License-Identifier: GPL-3.0-or-later
// Boss fights after a party load (gpu/shim/party/party_bossfog_rules.h): which overlay groups the
// guard restores after a load, and when a guest's boss fog counts as walked through - for every
// boss fog of the game (worldact::BossGates).
// cmake --build out/gpu --target party-bossfog-rules-test && out/gpu/party-bossfog-rules-test
#include "party/party_bossfog_rules.h"
#include "party/party_progress.h"
#include "party/party_worldact.h"

#include <cstdio>
#include <map>
#include <set>

using namespace coop::bossfog;
namespace pg = coop::progress;
namespace wa = coop::worldact;

static int g_fail = 0;
#define CHECK(x)                                                        \
    do {                                                                \
        if (!(x)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);    \
            ++g_fail;                                                   \
        }                                                               \
    } while (0)

namespace {

// The guard over a fake overlay: block -> its 1000 flags (only the set ones).
using Overlay = std::map<std::uint32_t, std::set<std::uint32_t>>;
void Guard(const Overlay& saved, Overlay* now) {
    for (auto& [block, bits] : *now) {
        const auto it = saved.find(block);
        if (GuardRestores(block, bits.empty(), it != saved.end() && !it->second.empty())) {
            bits = it->second;
        }
    }
}
bool Has(const Overlay& o, std::uint32_t id) {
    const auto it = o.find(id / 1000);
    return it != o.end() && it->second.count(id);
}

// The guest's fog watch over one stay (party_worldact GuestTick): `reads` the guest-entered flag per
// scan; returns how many reports went to the host.
int Stay(std::initializer_list<bool> reads) {
    bool seen_out = false, handled = false, stale = false;
    int sent = 0;
    for (bool in : reads) {
        if (!in) {
            seen_out = true;
            stale = false;
        }
        if (StaleFogFlag(seen_out, in, handled || stale)) {
            stale = true;
        }
        if (stale) {
            continue;
        }
        if (FogWalkedThrough(seen_out, in, handled)) {
            handled = true;
            ++sent;
        } else if (!in) {
            handled = false;
        }
    }
    return sent;
}

} // namespace

int main() {
    // Fight-state groups: T1 zones 4-6 of a map (1AAB4xxx boss fight / fog, 1AAB5xxx enemy AI,
    // 1AAB6xxx boss parts); not zone 0 / 1 (map objects, boss defeated), not the global groups.
    CHECK(FightStateFlag(12414800) && FightStateFlag(12414801) && FightStateFlag(12414802));
    CHECK(FightStateFlag(12415350) && FightStateFlag(12416000) && FightStateFlag(13204831));  // Amygdala's fog too
    CHECK(!FightStateFlag(12411800) && !FightStateFlag(12411802) && !FightStateFlag(12410000));
    CHECK(!FightStateFlag(12417810) && !FightStateFlag(12413000));
    CHECK(!FightStateFlag(9401) && !FightStateFlag(2410) && !FightStateFlag(14000) && !FightStateFlag(50002800));
    CHECK(!FightStateFlag(52114000) && !FightStateFlag(62414000) && !FightStateFlag(72414000));  // other types
    CHECK(!FightStateBlock(14) && !FightStateBlock(10004));  // global type 1 zone 4: no map

    // Every boss fog of the game (flag_tool emevd, every `ゲストがボス部屋入場` event): its host- and
    // guest-entered flags are fight state (the host's load resets them; never restored), its boss
    // and first-fight (intro) flags are not (saved: the guard and the mirror keep them).
    CHECK(wa::BossGates().size() == 19);
    for (const wa::BossGate& g : wa::BossGates()) {
        CHECK(FightStateFlag(g.host_flag) && FightStateFlag(g.guest_flag));
        CHECK(g.host_flag / 1000 == g.guest_flag / 1000);
        CHECK(!FightStateFlag(g.boss_flag));
        CHECK(g.intro_flag == 0 || !FightStateFlag(g.intro_flag));
        CHECK(pg::CategoryOf(g.host_flag) == pg::Category::boss_area);
        CHECK(!GuardRestores(g.host_flag / 1000, true, true));
        CHECK(GuardRestores(g.boss_flag / 1000, true, true));
        // the vanilla snapshot alone carries them (the world mirror leaves them out)
        CHECK(!pg::WorldMirrorKey(pg::PoolKeyOfId(g.host_flag)));
        CHECK(pg::WorldMirrorKey(pg::PoolKeyOfId(g.boss_flag)));
    }
    // One rule with the world mirror: for every pool group of a mirrored map, fight state == the
    // T1 groups the mirror leaves to the snapshot.
    int checked = 0;
    for (int key = 0; key < int(pg::kPoolBlocks); ++key) {
        const std::uint32_t block = pg::BlockOfPoolKey(key);
        if (block == 0xffffffffu || block / 10000 != 1 || block / 100 % 100 == 0 || block / 100 % 100 == 29 ||
            (block / 100 % 100 >= 40 && block / 100 % 100 <= 47)) {
            continue;
        }
        ++checked;
        CHECK(pg::WorldMirrorKey(key) == !FightStateBlock(block));
    }
    std::printf("mirror cross-check: %d map T1 groups\n", checked);
    CHECK(checked >= 17 * 3);  // at least zones 4-6 of every table map

    // The guard: only groups that came back zero with bits saved; never fight state.
    CHECK(GuardRestores(12410, true, true) && GuardRestores(9, true, true));
    CHECK(!GuardRestores(12410, false, true) && !GuardRestores(12410, true, false));
    CHECK(!GuardRestores(12414, true, true) && !GuardRestores(12415, true, true) && !GuardRestores(12416, true, true));
    CHECK(GuardRestores(12417, true, true));  // zone 7 lamps: as before

    // The 0.7.1 wipe_late run: before the wipe the guest's overlay had the running Gascoigne fight
    // (12414800 host in, 12414801 this guest in, 12414802 the fight) and the opening flags; the
    // snapshot after the host's wipe load came back with all of them zero (run 18's zeroed overlay
    // shape for the opening, the host's reset for the fight).
    {
        const Overlay saved = {{12410, {12410000}}, {9, {9401, 9402}}, {12414, {12414800, 12414801, 12414802}},
                               {12411, {12411802}}};
        Overlay now = {{12410, {}}, {9, {}}, {12414, {}}, {12411, {12411802}}};
        Guard(saved, &now);
        CHECK(Has(now, 12410000) && Has(now, 9401) && Has(now, 12411802));  // run 18: the opening stays done
        CHECK(!Has(now, 12414800) && !Has(now, 12414801) && !Has(now, 12414802));  // the fight as the host reset it
        // ... for every boss alike
        for (const wa::BossGate& g : wa::BossGates()) {
            const std::uint32_t b = g.host_flag / 1000;
            const Overlay s2 = {{b, {g.host_flag, g.guest_flag}}};
            Overlay n2 = {{b, {}}};
            Guard(s2, &n2);
            CHECK(!Has(n2, g.host_flag) && !Has(n2, g.guest_flag));
        }
        // the host's fight still runs (a lone guest's death and load): the snapshot has it, kept
        Overlay live = {{12414, {12414800}}};
        Guard(saved, &live);
        CHECK(Has(live, 12414800) && !Has(live, 12414801));
    }

    // The guest's fog report: only a walk through the fog this stay (0 -> 1), once.
    CHECK(StaleFogFlag(false, true, false) && !StaleFogFlag(true, true, false) && !StaleFogFlag(false, false, false));
    CHECK(!StaleFogFlag(false, true, true));
    CHECK(FogWalkedThrough(true, true, false) && !FogWalkedThrough(false, true, false) &&
          !FogWalkedThrough(true, false, false) && !FogWalkedThrough(true, true, true));
    CHECK(Stay({false, false, true, true, true}) == 1);  // a walk-in at the lamp after the wipe: sent once
    CHECK(Stay({true, true, true, true}) == 0);          // the 0.7.1 bug: 1 from the load on: never sent
    CHECK(Stay({true, true, false, false, true}) == 1);  // stale, then reset, then a real walk-in
    CHECK(Stay({false, true, false, true}) == 2);        // in, out (a death), in again: two walk-ins
    CHECK(Stay({}) == 0 && Stay({false}) == 0);

    if (g_fail) {
        std::printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("party bossfog rules: all checks passed\n");
    return 0;
}

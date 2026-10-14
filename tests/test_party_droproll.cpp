// SPDX-License-Identifier: GPL-3.0-or-later
// D4b shared enemy drop roll (gpu/shim/party/party_droproll), the parts without the game: the
// per-enemy seed, the MT19937 seeding the game RNG gets, the EVENT "drop_seed" JSON and the switch.
// Build and run: cmake --build out/gpu --target party-droproll-test && out/gpu/party-droproll-test.exe
#include "party/party_droproll.h"

#include <cstdio>
#include <set>
#include <string>

using namespace coop::droproll;

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

static DropKey Enemy(std::int32_t entity, std::uint32_t handle = 0x14005) {
    DropKey k;
    k.map = 0x18010000; // m24_01
    k.entity = entity;
    k.entity2 = entity;
    k.handle = handle;
    k.lot = 240000000 + entity % 1000;
    return k;
}

// The host and every guest compute the same seed for the same kill.
static void test_seed_shared() {
    const std::uint64_t salt = 0x1234567890abcdefull;
    CHECK(DropRollSeed(salt, 3, Enemy(2410700)) == DropRollSeed(salt, 3, Enemy(2410700)));
    // An enemy with entity ids: the chr table slot does not matter (it may differ between games).
    CHECK(DropRollSeed(salt, 3, Enemy(2410700, 0x14005)) == DropRollSeed(salt, 3, Enemy(2410700, 0x18009)));
    // The census key (+0x3B8) differs between games: it never changes the seed.
    DropKey other_key = Enemy(2410700);
    other_key.entity2 = 77;
    CHECK(DropRollSeed(salt, 3, Enemy(2410700)) == DropRollSeed(salt, 3, other_key));
    // Without an entity id the slot is all there is.
    CHECK(DropRollSeed(salt, 3, Enemy(0, 0x14005)) != DropRollSeed(salt, 3, Enemy(0, 0x14006)));
    CHECK(DropRollSeed(salt, 3, Enemy(0, 0x14005)) == DropRollSeed(salt, 3, Enemy(0, 0xff814005)));
}

// Another enemy, another respawn (generation) or another host run rolls anew.
static void test_seed_varies() {
    const std::uint64_t salt = 0x1234567890abcdefull;
    std::set<std::uint32_t> seen;
    for (std::uint32_t gen = 1; gen <= 50; ++gen) {
        for (std::int32_t e = 2410700; e < 2410720; ++e) {
            seen.insert(DropRollSeed(salt, gen, Enemy(e)));
        }
    }
    CHECK(seen.size() == 1000); // no collisions in a realistic set
    CHECK(DropRollSeed(salt, 1, Enemy(2410700)) != DropRollSeed(salt + 1, 1, Enemy(2410700)));
    DropKey other_map = Enemy(2410700);
    other_map.map = 0x18000000;
    CHECK(DropRollSeed(salt, 1, Enemy(2410700)) != DropRollSeed(salt, 1, other_map));
    DropKey other_lot = Enemy(2410700);
    other_lot.lot += 1;
    CHECK(DropRollSeed(salt, 1, Enemy(2410700)) != DropRollSeed(salt, 1, other_lot));
}

// The seeding is the reference MT19937 (the game's RNG at GSM +0xA78 refills with 0x181E5C0 and
// tempers like the reference).
static void test_mt() {
    CHECK(Mt19937First(5489) == 3499211612u); // the reference generator's first output
    std::uint32_t st[624];
    SeedMt19937(st, 5489);
    CHECK(st[0] == 5489 && st[1] == 1301868182u);
    CHECK(Mt19937First(1) == 1791095845u);
}

static void test_json() {
    DropSeed s;
    s.salt = 0xfedcba9876543210ull;
    s.gen = 7;
    s.discovery = 110.5f;
    DropSeed back;
    std::string err;
    CHECK(DropSeedFromJson(DropSeedToJson(s), &back, &err));
    CHECK(back == s);
    s.discovery = 0;
    CHECK(DropSeedFromJson(DropSeedToJson(s), &back, &err) && back == s);
    // Refused: salt 0, discovery out of range, garbage.
    DropSeed bad = s;
    bad.salt = 0;
    CHECK(!DropSeedFromJson(DropSeedToJson(bad), &back, &err));
    bad = s;
    bad.discovery = 5000.0f;
    CHECK(!DropSeedFromJson(DropSeedToJson(bad), &back, &err));
    bad.discovery = 0.5f;
    CHECK(!DropSeedFromJson(DropSeedToJson(bad), &back, &err));
    CHECK(!DropSeedFromJson("{\"salt\":\"0x1\",\"gen\":-1}", &back, &err));
    CHECK(!DropSeedFromJson("[1,2]", &back, &err));
    CHECK(!DropSeedFromJson("not json", &back, &err));
}

// BB_PARTY_SHARED_DROP_ROLL: on by default, 0 = the old own roll per player.
static void test_switch() {
    CHECK(SharedRollFromEnv(nullptr));
    CHECK(SharedRollFromEnv(""));
    CHECK(SharedRollFromEnv("1"));
    CHECK(!SharedRollFromEnv("0"));
}

int main() {
    test_seed_shared();
    test_seed_varies();
    test_mt();
    test_json();
    test_switch();
    std::printf("party-droproll-test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

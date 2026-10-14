// SPDX-License-Identifier: GPL-3.0-or-later
// Party balance rules (gpu/shim/party/party_balance_rules.h): scaling rows, rot, JSON.
// cmake --build out/gpu --target party-balance-test && out/gpu/party-balance-test
#include "party/party_balance_rules.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>

using namespace coop::balance;

static bool Near(float a, float b) {
    return std::fabs(a - b) < 1e-4f;
}

int main() {
    // Vanilla rules reproduce the game's own rows 7500 / 7501 / 7502 / (7503 unused).
    const Rules v = Vanilla();
    assert(!v.rot);
    for (int n = 1; n <= 3; ++n) {
        const Doping d = Boss(v, n);
        assert(Near(d.hp_rate, 1.0f + 0.5f * n) && Near(d.resist_rate, d.hp_rate));
        assert(Near(d.attack_rate, 1.0f) && d.super_armor == 30 * n);
        assert(Enemy(v, n) == Doping{});
    }
    assert(!EnemyScalingActive(v));
    assert(Boss(v, 0) == Doping{} && Enemy(Rules{}, 0) == Doping{});

    // The defaults: harder than vanilla (user decision 2026-10-11).
    const Rules d;
    assert(d.rot && d.boss_hp == 150 && d.boss_damage == 10 && d.enemy_hp == 50);
    const Doping b1 = Boss(d, 1), b2 = Boss(d, 2), e2 = Enemy(d, 2);
    assert(Near(b1.hp_rate, 1.75f) && Near(b2.hp_rate, 2.5f) && Near(b1.attack_rate, 1.1f) && b1.super_armor == 30);
    assert(Near(e2.hp_rate, 1.5f) && Near(e2.attack_rate, 1.0f) && e2.super_armor == 0);
    assert(EnemyScalingActive(d));
    // Poise below zero: easier to stagger with more hunters (ERSC's 3-4 player note).
    Rules p;
    p.enemy_poise = -50;
    assert(Enemy(p, 3).super_armor == -45);
    // Counts past 3 count as 3 (the game's slots).
    assert(Boss(d, 9) == Boss(d, 3));

    // Slider tables.
    assert(AtPosition(kHpTable, Position(kHpTable, 150)) == 150 && Position(kHpTable, 100) == 4);
    assert(Position(kPoiseTable, 0) == 4 && AtPosition(kDamageTable, 99) == 100 && AtPosition(kDamageTable, -3) == 0);

    // JSON round trip, clamping, malformed input.
    Rules r;
    r.rot = false;
    r.boss_hp = 300;
    r.enemy_poise = -75;
    Rules back;
    assert(FromJson(ToJson(r), &back) && back == r);
    Rules c;
    assert(FromJson("{\"boss_hp\":100000,\"enemy_damage\":-5,\"rot\":7}", &c));
    assert(c.boss_hp == 400 && c.enemy_damage == 0 && c.rot);
    std::string err;
    assert(!FromJson("[1,2]", &c, &err) && !err.empty());
    assert(!FromJson("{", &c, &err));
    assert(!FromJson(std::string(5000, ' '), &c, &err));

    // Rot.
    assert(Near(RotHpFactor(0), 1.0f) && Near(RotHpFactor(2), 0.8f) && Near(RotHpFactor(5), 0.5f));
    assert(Near(RotHpFactor(9), 0.5f) && Near(RotHpFactor(-1), 1.0f));
    // What cleanses the rot: a lamp warp is the respawn only while the player is down from a death.
    assert(LampWarpIsRespawn(false, 3.0, false));        // dead at the warp: the respawn
    assert(LampWarpIsRespawn(true, 5.0, false));         // just respawned, not up yet
    assert(!LampWarpIsRespawn(true, 19.0, true));        // playtest 1 H5:6072: revived in place, then a lamp
    assert(!LampWarpIsRespawn(true, 60.0, false));       // long after
    assert(!LampWarpIsRespawn(true, -1.0, false));       // no death yet
    // #21: the lamp menu opened beside a lamp is a rest.
    assert(LampMenuRest(true, true, 1.5f, 0.2f, -1.0f));
    assert(!LampMenuRest(false, true, 1.5f, 0.2f, -1.0f));  // no dialog
    assert(!LampMenuRest(true, false, 1.5f, 0.2f, -1.0f));  // dead
    assert(!LampMenuRest(true, true, 6.0f, 0.0f, 0.0f));    // an item box elsewhere
    assert(!LampMenuRest(true, true, 1.0f, 5.0f, 0.0f));    // a floor above

    // Menu texts.
    assert(DescribeHp(150, false).find("+75 %") != std::string::npos);
    assert(DescribePoise(-100, false).find("-30") != std::string::npos);
    std::printf("party balance rules: ok\n");
    return 0;
}

// SPDX-License-Identifier: GPL-3.0-or-later
// Seamless join rules (gpu/shim/party/seam_rules.h). ninja -C out/gpu seam-rules-test
#include "party/seam_rules.h"

#include <cassert>
#include <cstdio>
#include <cstring>

using namespace coop::seam;

static void TestSettleTwoGoodFrames() {
    SettleWatch w;
    assert(!w.Frame(10, 0));  // not started: nothing
    w.Start(1000);
    assert(!w.Frame(400, 1400));   // the first heavy frame after the load
    assert(!w.Frame(30, 1430));    // one good
    assert(!w.Frame(180, 1610));   // a spike resets the run
    assert(!w.Frame(20, 1630));
    assert(w.Frame(25, 1655));     // two in a row: drop
    assert(!w.active && !w.capped);
    assert(w.frames == 5);
    assert(w.worst_ms == 400);
    assert(!w.Frame(10, 1700));    // ended
}

static void TestSettleCap() {
    SettleWatch w;
    w.Start(0);
    double t = 0;
    bool dropped = false;
    for (int i = 0; i < 100 && !dropped; ++i) {
        t += 60;  // a slow PC: every frame over 50 ms
        dropped = w.Frame(60, t);
    }
    assert(dropped && w.capped);
    assert(t >= kSettleCapMs && t < kSettleCapMs + 61);
}

static void TestSettleEdge() {
    SettleWatch w;
    w.Start(0);
    assert(!w.Frame(kSettledFrameMs, 50));  // exactly 50 ms is not under 50
    assert(!w.Frame(49.9, 100));
    assert(w.Frame(49.9, 150));
    w.Start(0);
    assert(!w.Frame(-1, 10));  // an unknown frame time (no previous frame end) is not good
    assert(!w.Frame(10, 20));
    assert(w.Frame(10, 30));
}

static FollowCase Good() {
    FollowCase c;
    c.lamp_kind = true;
    c.lamp = 2412953;  // a lamp id shape (Tomb of Oedon), no game data
    c.spot_known = true;
    c.in_world = true;
    c.idle = true;
    c.hp_left = true;
    c.hold_hook = true;
    return c;
}

static void TestRefusal() {
    assert(ParallelFollowRefusal(Good()) == nullptr);
    FollowCase c = Good();
    c.hold_hook = false;
    assert(ParallelFollowRefusal(c) && std::strstr(ParallelFollowRefusal(c), "InGameStart"));
    c = Good();
    c.alone = true;
    assert(ParallelFollowRefusal(c));
    c = Good();
    c.host_death = true;
    assert(ParallelFollowRefusal(c) && std::strstr(ParallelFollowRefusal(c), "death"));
    c = Good();
    c.lamp_kind = false;
    assert(ParallelFollowRefusal(c));
    c = Good();
    c.hp_left = false;
    assert(ParallelFollowRefusal(c));
    c = Good();
    c.dead_follow = true;
    assert(ParallelFollowRefusal(c));
    c = Good();
    c.boss_fight = true;
    assert(ParallelFollowRefusal(c));
    c = Good();
    c.lamp = 2900001;  // a Chalice Dungeon lamp range
    assert(ParallelFollowRefusal(c) && std::strstr(ParallelFollowRefusal(c), "Chalice"));
    c = Good();
    c.lamp = 0xffffffffu;
    assert(ParallelFollowRefusal(c));
    c = Good();
    c.spot_known = false;
    assert(ParallelFollowRefusal(c));
    c = Good();
    c.idle = false;
    assert(ParallelFollowRefusal(c));
    c = Good();
    c.in_world = false;
    assert(ParallelFollowRefusal(c));
}

static void TestSameBlock() {
    assert(SameBlock(0x18010000u, 0x18010000u));
    assert(SameBlock(0x18010000u, 0x1801ffffu));
    assert(!SameBlock(0x18010000u, 0x18000000u));
    assert(!SameBlock(0x18010000u, 0x15000000u));
    assert(!SameBlock(0, 0));
    assert(!SameBlock(0xffffffffu, 0xffffffffu));
}

int main() {
    TestSettleTwoGoodFrames();
    TestSettleCap();
    TestSettleEdge();
    TestRefusal();
    TestSameBlock();
    std::printf("seam rules: all tests passed\n");
    return 0;
}

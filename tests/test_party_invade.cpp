// SPDX-License-Identifier: GPL-3.0-or-later
// Invasions into open public games (0.7.8, CL-4cc63c): the allow / deny table, the rate limit, the block book, how an
// invasion ends and the wire (gpu/shim/party/party_invade_rules.h).
// Build and run: ninja -C out/gpu party-invade-test && out/gpu/party-invade-test.exe
#include "party/party_invade_rules.h"

#include <cstdio>
#include <string>

using namespace coop::invade;

static int g_failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

static HostView Good() {
    HostView h;
    h.allow = true;
    h.world_players = 2;  // the host and one guest
    return h;
}

static void AllowDenyTable() {
    CHECK(Admit(Good(), false, false) == Deny::None);
    // The host's switch decides first, whatever else is true.
    HostView h = Good();
    h.allow = false;
    CHECK(Admit(h, false, false) == Deny::Off);
    CHECK(Admit(h, true, true, true) == Deny::Off);
    struct Row {
        const char* what;
        void (*set)(HostView&);
        Deny want;
    };
    const Row rows[] = {
        {"host stays", [](HostView& v) { v.host_stays = true; }, Deny::HostStays},
        {"no world", [](HostView& v) { v.world_up = false; }, Deny::NotInWorld},
        {"prologue", [](HostView& v) { v.prologue = true; }, Deny::NotInWorld},
        {"loading", [](HostView& v) { v.loading = true; }, Deny::Loading},
        {"cutscene", [](HostView& v) { v.cutscene = true; }, Deny::Cutscene},
        {"boss fog", [](HostView& v) { v.boss_fight = true; }, Deny::BossFight},
        {"host dead", [](HostView& v) { v.alive = false; }, Deny::HostDead},
        {"travelling", [](HostView& v) { v.travelling = true; }, Deny::Travelling},
        {"an invader in", [](HostView& v) { v.invaders = 1; }, Deny::InvaderPresent},
        {"four in the world", [](HostView& v) { v.world_players = 4; }, Deny::NoSlot},
    };
    for (const Row& r : rows) {
        HostView v = Good();
        r.set(v);
        const Deny d = Admit(v, false, false);
        if (d != r.want) std::fprintf(stderr, "row '%s': got %s\n", r.what, DenyWord(d));
        CHECK(d == r.want);
        CHECK(!DenyText(d).empty());
    }
    // The invader's own state and the books come before the host's moment.
    HostView v = Good();
    v.boss_fight = true;
    CHECK(Admit(v, true, false) == Deny::Blocked);
    CHECK(Admit(v, false, true) == Deny::RateLimited);
    CHECK(Admit(v, true, true) == Deny::Blocked);
    CHECK(Admit(v, false, false, true) == Deny::InvaderStays);
    CHECK(Admit(v, false, false) == Deny::BossFight);
    CHECK(DenyText(Deny::None).empty());
}

static void FourPlayers() {
    // Host + two guests = 3 in the world: the fourth slot is the invader's.
    HostView h = Good();
    h.world_players = 3;
    CHECK(Admit(h, false, false) == Deny::None);
    h.world_players = 4;  // no invader slot
    CHECK(Admit(h, false, false) == Deny::NoSlot);
    // A party of 5 (the link's max) still has the game's four slots.
    h.party_max = 5;
    CHECK(Capacity(5) == 4);
    CHECK(Admit(h, false, false) == Deny::NoSlot);
    // A party set to two players: host + guest fill it.
    h = Good();
    h.party_max = 2;
    CHECK(Capacity(2) == 2);
    CHECK(Admit(h, false, false) == Deny::NoSlot);
    h.world_players = 1;
    CHECK(Admit(h, false, false) == Deny::None);
    // An invader already in blocks the next one before the slot count does.
    h = Good();
    h.invaders = 1;
    CHECK(Admit(h, false, false) == Deny::InvaderPresent);
}

static void ListedAsInvadable() {
    CHECK(Invadable(true, 1, 0, 4));
    CHECK(!Invadable(false, 1, 0, 4));  // off: never listed
    CHECK(!Invadable(true, 4, 0, 4));   // full
    CHECK(!Invadable(true, 3, 1, 4));   // an invader in already
    CHECK(Invadable(true, 3, 0, 4));
    CHECK(!Invadable(true, 2, 0, 2));
}

static void RateLimit() {
    RateBook b;
    CHECK(b.Allow("a", 0));
    CHECK(b.Allow("a", 10));
    CHECK(b.Allow("a", 20));
    CHECK(b.Limited("a", 30));
    CHECK(!b.Allow("a", 30));  // the 4th inside the window
    CHECK(b.Allow("b", 30));   // another address is its own key
    CHECK(!b.Allow("a", kRateWindowS - 1));
    CHECK(b.Allow("a", kRateWindowS + 1));  // the first aged out
    b.Clear();
    CHECK(!b.Limited("a", 0));
    // A refused attempt is not recorded: waiting for the oldest to age out is enough.
    RateBook c;
    for (int i = 0; i < kRateMax; ++i) CHECK(c.Allow("k", i));
    for (int i = 0; i < 20; ++i) CHECK(!c.Allow("k", 100 + i));
    CHECK(c.Allow("k", kRateWindowS + 1));
    // A flood of keys does not grow the book without bound.
    RateBook d;
    for (int i = 0; i < 2000; ++i) d.Allow("k" + std::to_string(i), i * 1000.0);
    CHECK(d.Allow("fresh", 3e6));
}

static void Blocking() {
    BlockBook k;
    CHECK(!k.Blocked("Bad", "10.0.0.5"));
    k.Block("Bad", "10.0.0.5");
    CHECK(k.Blocked("Bad", "10.0.0.9"));    // the name changes address
    CHECK(k.Blocked("Other", "10.0.0.5"));  // the address changes name
    CHECK(!k.Blocked("Other", "10.0.0.6"));
    CHECK(!k.Blocked("", ""));
    k.Block("", "");
    CHECK(k.Size() == 1);
    k.Clear();
    CHECK(!k.Blocked("Bad", "10.0.0.5"));
}

static void Ending() {
    Live l;
    CHECK(CheckEnd(l) == End::None);
    l.invader_alive = false;
    CHECK(CheckEnd(l) == End::InvaderDied);
    l.host_alive = false;  // both: the host's death wins (the invader's side)
    CHECK(CheckEnd(l) == End::HostDied);
    l = Live{};
    l.host_alive = false;
    CHECK(CheckEnd(l) == End::HostDied);
    l = Live{};
    l.host_loading = true;
    CHECK(CheckEnd(l) == End::Travel);
    l = Live{};
    l.boss_fight = true;
    CHECK(CheckEnd(l) == End::BossFight);
    l = Live{};
    l.allow = false;
    CHECK(CheckEnd(l) == End::Disabled);
    l.blocked = true;
    CHECK(CheckEnd(l) == End::Blocked);
    l = Live{};
    l.link_up = false;
    CHECK(CheckEnd(l) == End::HostGone);
    // Pending: only the timeout ends it; the host's world events do not count before the invader is in.
    l = Live{};
    l.in_world = false;
    l.pending_s = 5;
    l.host_loading = true;
    CHECK(CheckEnd(l) == End::None);
    l.pending_s = kPendingMaxS + 1;
    CHECK(CheckEnd(l) == End::Timeout);
    for (int i = 1; i <= static_cast<int>(End::Timeout); ++i) {
        const End e = static_cast<End>(i);
        End back = End::None;
        CHECK(ParseEnd(EndWord(e), &back) && back == e);
        CHECK(!EndTextForHost(e, "Bob").empty());
        CHECK(!EndTextForInvader(e, "Host").empty());
    }
    End junk = End::None;
    CHECK(!ParseEnd("nope", &junk));
    CHECK(EndTextForHost(End::None, "x").empty());
}

static void Wire() {
    Msg m;
    CHECK(Decode(Encode({2, "Bob_1", End::None}), &m) && m.state == 2 && m.name == "Bob_1" && m.why == End::None);
    CHECK(Decode(Encode({1, "Bob", End::None}), &m) && m.state == 1);
    CHECK(Decode(Encode({0, "Bob", End::HostDied}), &m) && m.state == 0 && m.why == End::HostDied);
    CHECK(Decode(Encode({0, "Bob", End::None}), &m) && m.state == 0 && m.why == End::None);
    CHECK(!Decode("", &m));
    CHECK(!Decode("{\"s\":3,\"n\":\"Bob\"}", &m));
    CHECK(!Decode("{\"s\":2,\"n\":\"\"}", &m));
    CHECK(!Decode("{\"s\":2,\"n\":\"Bob Smith\"}", &m));
    CHECK(!Decode("{\"s\":2,\"n\":\"Bob\"} x", &m));
    CHECK(!Decode("{\"s\":2,\"n\":\"Bob\",\"w\":\"nope\"}", &m));
    CHECK(!Decode("{\"s\":1,\"n\":\"Bob\",\"w\":\"host_died\"}", &m));  // an end word only with state 0
    CHECK(!Decode("{\"s\":2,\"n\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\"}", &m));
    CHECK(NameOk("a-b_9") && !NameOk("a b") && !NameOk(""));
    CHECK(RefusedText(DenyText(Deny::BossFight)) == "invasion refused: a boss fight is on in the host's world");
}

int main() {
    AllowDenyTable();
    FourPlayers();
    ListedAsInvadable();
    RateLimit();
    Blocking();
    Ending();
    Wire();
    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("party-invade-test: OK");
    return 0;
}

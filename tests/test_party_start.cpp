// SPDX-License-Identifier: GPL-3.0-or-later
// Party campaign start (gpu/shim/party/party_start, C1): BB_PARTY_START parsing, the start step
// and readiness from the flags, the bell grant plan and the lobby's ring decisions.
// Build and run: ninja -C out/gpu party-start-test && out/gpu/party-start-test.exe
#include "party/party_start.h"

#include <cstdio>
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

static StartFlags Flags(bool opening, bool dream, bool right, bool left, bool cutscene = false) {
    StartFlags f;
    f.flags_ok = true;
    f.opening_done = opening;
    f.first_dream = dream;
    f.weapon_right = right;
    f.weapon_left = left;
    f.cutscene = cutscene;
    f.beckoning_count = 0;
    f.resonant_count = 0;
    return f;
}

static void test_mode() {
    bool known = false;
    CHECK(ParseStartMode(nullptr, &known) == StartMode::PrologueSolo && known);
    CHECK(ParseStartMode("", &known) == StartMode::PrologueSolo && known);
    CHECK(ParseStartMode("prologue_solo", &known) == StartMode::PrologueSolo && known);
    CHECK(ParseStartMode("Immediate", &known) == StartMode::Immediate && known);
    CHECK(ParseStartMode("clinic", &known) == StartMode::Immediate && known);
    CHECK(ParseStartMode("bogus", &known) == StartMode::PrologueSolo && !known);
    CHECK(std::string(StartModeName(StartMode::Immediate)) == "immediate");
    CHECK(std::string(StartModeName(StartMode::PrologueSolo)) == "prologue_solo");
}

static void test_steps() {
    // The vanilla timeline (campaign_start.md §1.5).
    CHECK(ClassifyStart(StartFlags{}, false) == StartStep::NoWorld);
    CHECK(ClassifyStart(Flags(true, true, true, true), false) == StartStep::NoWorld);
    CHECK(ClassifyStart(StartFlags{}, true) == StartStep::Unknown); // flags unreadable
    CHECK(ClassifyStart(Flags(false, false, false, false), true) == StartStep::Opening);
    CHECK(ClassifyStart(Flags(true, false, false, false), true) == StartStep::Clinic);
    CHECK(ClassifyStart(Flags(true, true, false, false), true) == StartStep::FirstDream);
    CHECK(ClassifyStart(Flags(true, true, true, false), true) == StartStep::FirstDream);
    CHECK(ClassifyStart(Flags(true, true, false, true), true) == StartStep::FirstDream);
    CHECK(ClassifyStart(Flags(true, true, true, true), true) == StartStep::Ready);
    CHECK(std::string(StartStepName(StartStep::Clinic)) == "clinic");
}

static void test_ready() {
    const StartMode solo = StartMode::PrologueSolo, now = StartMode::Immediate;
    // prologue_solo: only at the end of the prologue, in the world, no cutscene.
    CHECK(!StartReady(solo, Flags(false, false, false, false), true));
    CHECK(!StartReady(solo, Flags(true, false, false, false), true)); // clinic: no summons
    CHECK(!StartReady(solo, Flags(true, true, true, false), true));   // one weapon
    CHECK(StartReady(solo, Flags(true, true, true, true), true));
    CHECK(!StartReady(solo, Flags(true, true, true, true), false));       // title / loading
    CHECK(!StartReady(solo, Flags(true, true, true, true, true), true));  // 9180: cutscene
    CHECK(!StartReady(solo, StartFlags{}, true));                         // unreadable
    // immediate: from the end of the opening cutscene on.
    CHECK(!StartReady(now, Flags(false, false, false, false), true));
    CHECK(!StartReady(now, Flags(false, false, false, false, true), true));
    CHECK(StartReady(now, Flags(true, false, false, false), true));
    CHECK(!StartReady(now, Flags(true, false, false, false, true), true));
    CHECK(StartReady(now, Flags(true, true, true, true), true));
    CHECK(!StartReady(now, StartFlags{}, true));
    // The log line names the step and the flags.
    const std::string d = DescribeStart(Flags(true, false, false, false), true);
    CHECK(d.find("step clinic") == 0 && d.find("12410000 1") != std::string::npos &&
          d.find("9401 0") != std::string::npos && d.find("goods 200 x0") != std::string::npos);
    CHECK(DescribeStart(StartFlags{}, true).find("unreadable") != std::string::npos);
}

static void test_grants() {
    StartFlags f = Flags(true, true, true, true);
    BellGrant g = PlanBellGrant(f); // a fresh character: neither bell
    CHECK(g.beckoning_lot && !g.beckoning_goods && g.resonant && g.any());
    f.beckoning_lot = true; // lot flag set, but the bell is gone
    g = PlanBellGrant(f);
    CHECK(!g.beckoning_lot && g.beckoning_goods && g.resonant);
    f.beckoning_count = 1;
    f.resonant_count = 1;
    CHECK(!PlanBellGrant(f).any()); // owns both
    f.beckoning_count = -1;         // inventory unreadable: nothing
    f.resonant_count = -1;
    CHECK(!PlanBellGrant(f).any());
    CHECK(!PlanBellGrant(StartFlags{}).any());
}

static party::RosterEntry Member(int slot, party::MemberState st, bool connected = true) {
    party::RosterEntry e;
    e.slot = slot;
    e.name = "m" + std::to_string(slot);
    e.state = st;
    e.connected = connected;
    return e;
}

static void test_lobby() {
    using MS = party::MemberState;
    CHECK(OwnWorldState(true) == MS::Home);
    CHECK(OwnWorldState(false) == MS::Prologue);

    const std::vector<party::RosterEntry> host_only = {Member(0, MS::Home)};
    const std::vector<party::RosterEntry> guest_prologue = {Member(0, MS::Home), Member(1, MS::Prologue)};
    const std::vector<party::RosterEntry> guest_ready = {Member(0, MS::Home), Member(1, MS::Home)};
    const std::vector<party::RosterEntry> guest_joining = {Member(0, MS::Home), Member(1, MS::Joining)};
    const std::vector<party::RosterEntry> guest_lost = {Member(0, MS::Home), Member(1, MS::Home, false)};
    const std::vector<party::RosterEntry> guest_in = {Member(0, MS::Home), Member(1, MS::InHostWorld)};
    const std::vector<party::RosterEntry> mixed = {Member(0, MS::Home), Member(1, MS::InHostWorld),
                                                   Member(2, MS::Prologue), Member(3, MS::Home)};

    CHECK(!MemberWaits(host_only));
    CHECK(!MemberWaits(guest_prologue)); // still in its prologue: not summoned
    CHECK(MemberWaits(guest_ready));
    CHECK(MemberWaits(guest_joining));
    CHECK(!MemberWaits(guest_lost));
    CHECK(!MemberWaits(guest_in));
    CHECK(MemberWaits(mixed));

    // Host: only when itself ready and a ready member waits, with room left.
    CHECK(HostMayRing(true, 0, 3, guest_ready));
    CHECK(!HostMayRing(false, 0, 3, guest_ready)); // host still in its prologue
    CHECK(!HostMayRing(true, 0, 3, guest_prologue));
    CHECK(!HostMayRing(true, -1, 3, guest_ready)); // cooperator count unknown
    CHECK(!HostMayRing(true, 2, 3, guest_ready));  // full
    CHECK(HostMayRing(true, 1, 4, mixed));
    CHECK(!HostMayRing(true, 1, 2, mixed));

    // Guest: ready and connected.
    CHECK(GuestMayRing(true, true));
    CHECK(!GuestMayRing(false, true));
    CHECK(!GuestMayRing(true, false));

    // Two members finishing the prologue at different times: the host rings once both are ready.
    StartFlags host = Flags(true, true, true, true), guest = Flags(true, false, false, false);
    const StartMode solo = StartMode::PrologueSolo;
    std::vector<party::RosterEntry> roster = {Member(0, OwnWorldState(StartReady(solo, host, true))),
                                              Member(1, OwnWorldState(StartReady(solo, guest, true)))};
    CHECK(!HostMayRing(StartReady(solo, host, true), 0, 2, roster));
    CHECK(!GuestMayRing(StartReady(solo, guest, true), true));
    guest = Flags(true, true, true, true);
    roster[1].state = OwnWorldState(StartReady(solo, guest, true));
    CHECK(HostMayRing(StartReady(solo, host, true), 0, 2, roster));
    CHECK(GuestMayRing(StartReady(solo, guest, true), true));
}

int main() {
    test_mode();
    test_steps();
    test_ready();
    test_grants();
    test_lobby();
    std::printf("party-start-test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

// SPDX-License-Identifier: GPL-3.0-or-later
// Lifts in a party (gpu/shim/party/party_lift_rules.h): the table of every lift and the decisions.
// cmake --build out/gpu --target party-lift-rules-test && out/party-lift-rules-test
#include "party/party_lift_rules.h"

#include <cstdio>
#include <set>

using namespace coop::liftrules;

static int g_fail = 0;
#define CHECK(x)                                                        \
    do {                                                                \
        if (!(x)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);    \
            ++g_fail;                                                   \
        }                                                               \
    } while (0)

int main() {
    // The table: every lift of the main game and the DLC (tools/party/lift_tool.py check: the up /
    // down events of every map, named or not), one entry per car and per position flag.
    struct Expect {
        std::int32_t car;
        std::uint32_t pos;
    };
    const Expect all[] = {
        {2201210, 12200125}, {2401101, 12400150}, {2401102, 12400827}, {2411320, 12410350}, {2421250, 12420154},
        {2501010, 12500074}, {2601210, 12601250}, {2601220, 12601251}, {2601230, 12601252}, {2601240, 12601256},
        {2601280, 12601254}, {2601290, 12601255}, {2601260, 12601253}, {2701032, 12700134}, {2701042, 12700144},
        {2801400, 12800610}, {3301000, 13300115}, {3501100, 13501106}, {3501105, 13501116}, {3501110, 13501126},
        {3601100, 13601106},
    };
    CHECK(Lifts().size() == sizeof all / sizeof all[0]);
    std::set<std::int32_t> cars;
    std::set<std::uint32_t> flags;
    for (const Expect& e : all) {
        const Lift* l = LiftByCar(e.car);
        CHECK(l && l->pos == e.pos && LiftByPosFlag(e.pos) == l);
    }
    for (const Lift& l : Lifts()) {
        CHECK(cars.insert(l.car).second && flags.insert(l.pos).second);
        CHECK(l.busy == 0 || (!LiftByPosFlag(l.busy) && flags.count(l.busy) == 0));
        CHECK(l.pos / 100000u % 100u == std::uint32_t(l.car) / 100000u % 100u);  // flag and car of one area
        // Only the lifts with no busy flag replay (their ride ends with the flag alone).
        CHECK(l.Replays() == (l.busy == 0 && l.host_state == 0));
    }
    CHECK(!LiftByPosFlag(0) && !LiftByPosFlag(12410351) && !LiftByCar(0) && !LiftByCar(2601261));

    // The library lift (12601352): from the top (pos 1) car 6, 3; from the bottom 8, 5; its ends:
    // pos 1 car 7 (init 2), pos 0 car 9 (init 4).
    const Lift* lib = LiftByPosFlag(12601253);
    CHECK(lib && lib->Replays() && lib->car == 2601260 && lib->gate == 2601261);
    for (const int a : {6, 3, 8, 5}) {
        CHECK(PhaseOf(*lib, a) == CarPhase::riding);
    }
    CHECK(PhaseOf(*lib, 7) == CarPhase::at_on && PhaseOf(*lib, 2) == CarPhase::at_on);
    CHECK(PhaseOf(*lib, 9) == CarPhase::at_off && PhaseOf(*lib, 4) == CarPhase::at_off);
    CHECK(PhaseOf(*lib, -1) == CarPhase::unknown && PhaseOf(*lib, 1) == CarPhase::unknown);
    CHECK(CarAgreesWithFlag(*lib, true, 7) && CarAgreesWithFlag(*lib, true, 2) && CarAgreesWithFlag(*lib, false, 9) &&
          CarAgreesWithFlag(*lib, false, 4));
    CHECK(!CarAgreesWithFlag(*lib, true, 9) && !CarAgreesWithFlag(*lib, false, 7) && !CarAgreesWithFlag(*lib, true, 3));
    const Lift* cy = LiftByPosFlag(12410350);  // Central Yharnam: a busy flag, network-synced, no replay
    CHECK(cy && !cy->Replays() && PhaseOf(*cy, 6) == CarPhase::unknown && !HoldMirrorFlip(*cy, 6) &&
          !MirrorHold(*cy, 6, true));

    // The flag hold (the 0.7.1 full smoke's el ride 5: both rode down; the host's copy ended first,
    // and the host's flip reached the guest in the very tick the guest's own end animation 9 stopped:
    // 0.7.1 held it - the guest stood in the car - and put the guest's own end back; its flag stayed 1
    // with the car at the bottom until the next ride).
    CHECK(HoldMirrorFlip(*lib, 3) && HoldMirrorFlip(*lib, 5));     // the ride, or the car arrived and
    CHECK(HoldMirrorFlip(*lib, 6) && HoldMirrorFlip(*lib, 8));     // its player still in it: not read yet
    CHECK(!HoldMirrorFlip(*lib, 9) && !HoldMirrorFlip(*lib, 7));   // the copy's end: it has read the flag
    CHECK(!HoldMirrorFlip(*lib, 2) && !HoldMirrorFlip(*lib, 4) && !HoldMirrorFlip(*lib, -1));
    CHECK(!LiftNeedsReplay(*lib, false, 9));                       // ... and its end needs no replay
    // The mirror's hold: from the ride's start until the copy's end animation stops (it writes then).
    CHECK(MirrorHold(*lib, 6, true) && MirrorHold(*lib, 3, true) && MirrorHold(*lib, 3, false));
    CHECK(MirrorHold(*lib, 9, true) && MirrorHold(*lib, 7, true));
    CHECK(!MirrorHold(*lib, 9, false) && !MirrorHold(*lib, 7, false) && !MirrorHold(*lib, 2, false) &&
          !MirrorHold(*lib, -1, false));

    // The replay: another game's ride (the car stands at the other end, or no pose yet).
    CHECK(!LiftNeedsReplay(*lib, true, 7) && !LiftNeedsReplay(*lib, true, 2) && !LiftNeedsReplay(*lib, false, 4));
    CHECK(LiftNeedsReplay(*lib, true, 9) && LiftNeedsReplay(*lib, false, 7) && LiftNeedsReplay(*lib, true, -1) &&
          LiftNeedsReplay(*lib, false, 2));
    CHECK(!LiftNeedsReplay(*cy, true, 0) && !LiftNeedsReplay(*cy, false, 30));  // network-synced: never

    // The host's answer to a guest's finished ride.
    CHECK(HostLiftSync(true, false, false, true) == LiftSync::same);
    CHECK(HostLiftSync(false, false, true, false) == LiftSync::same);
    CHECK(HostLiftSync(false, true, false, true) == LiftSync::busy);
    CHECK(HostLiftSync(false, true, true, true) == LiftSync::busy);
    CHECK(HostLiftSync(false, false, true, true) == LiftSync::loaded);
    CHECK(HostLiftSync(false, false, false, true) == LiftSync::apply);
    CHECK(HostLiftSync(true, false, false, false) == LiftSync::apply);
    CHECK(HostLiftSync(false, false, true, true, true, false) == LiftSync::apply);
    CHECK(HostLiftSync(false, false, true, true, true, true) == LiftSync::occupied);
    CHECK(HostLiftSync(true, false, true, true, true, true) == LiftSync::same);
    CHECK(GuestLiftRideDone(true, true, false) && GuestLiftRideDone(false, false, false));
    CHECK(!GuestLiftRideDone(true, true, true) && !GuestLiftRideDone(false, true, false));

    // The guest's report: every ride to an end the host's world lacks goes up once; again after the
    // host's world had the guest's end in between (0.7.1: once per (flag, value) for the whole stay).
    CHECK(GuestLiftReport(true, false, true, -1) == GuestReport::wait);   // the ride runs
    CHECK(GuestLiftReport(true, false, false, -1) == GuestReport::send);  // over: up to the host
    CHECK(GuestLiftReport(true, false, false, 1) == GuestReport::sent);   // not again
    CHECK(GuestLiftReport(true, true, false, 1) == GuestReport::same);    // the host's world has it
    CHECK(GuestLiftReport(false, true, false, 1) == GuestReport::send);   // the ride back down
    CHECK(GuestLiftReport(true, false, false, 0) == GuestReport::send);   // and up again
    CHECK(GuestLiftReport(false, false, false, -1) == GuestReport::same);

    if (g_fail) {
        std::printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("party-lift-rules-test: all checks passed\n");
    return 0;
}

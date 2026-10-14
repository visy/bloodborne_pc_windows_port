// SPDX-License-Identifier: GPL-3.0-or-later
// The world warper's rules (gpu/shim/party/world_warper_rules.h): options, the route through
// every ReturnPointParam row, the progress file and resume after a crash, the scripted steps.
// Build and run: ninja -C out/gpu world-warper-test && out/gpu/world-warper-test.exe
#include "party/party_ids.h"
#include "party/world_warper_rules.h"

#include <cstdio>
#include <iterator>
#include <set>

using namespace coop::warper;

static int g_failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

static bool Spot(std::uint32_t rp) {
    float sp[4];
    return coop::ids::ReturnSpot(rp, sp);
}

int main() {
    // Options.
    CHECK(ParseOptions(nullptr).mode == Mode::Off);
    CHECK(ParseOptions("").mode == Mode::Off);
    CHECK(ParseOptions("0").mode == Mode::Off);
    CHECK(ParseOptions("1").mode == Mode::Tour);
    CHECK(ParseOptions("tour").mode == Mode::Tour);
    CHECK(ParseOptions("replay").mode == Mode::Replay);
    {
        const Options o = ParseOptions("measure,lamps=2412950+2402950,replay=2102950,max=5,noenemy");
        CHECK(o.mode == Mode::Measure);
        CHECK(o.lamps.size() == 2 && o.lamps[0] == 2412950 && o.lamps[1] == 2402950);
        CHECK(o.replay.size() == 1 && o.replay[0] == 2102950);
        CHECK(o.max == 5);
        CHECK(o.act && !o.enemies && o.kill);
    }
    {
        const Options o = ParseOptions("lamps=2412950;noact;nokill");
        CHECK(o.mode == Mode::Tour);
        CHECK(o.lamps.size() == 1);
        CHECK(!o.act && o.enemies && !o.kill);
    }
    CHECK(ParseIds("1+x+3 4").size() == 3);

    // The route: every row with a spot, Dream first, then the lamps, Chalice rows never.
    const std::vector<std::uint32_t> all(std::begin(coop::ids::kPartyReturnPoints),
                                         std::end(coop::ids::kPartyReturnPoints));
    const auto route = BuildRoute(all, Spot);
    CHECK(!route.empty());
    CHECK(route.front() == coop::lamptravel::kDreamReturnPoint);
    CHECK(route[1] == coop::lamptravel::ReturnPoint(coop::lamptravel::kLamps[0]));
    std::set<std::uint32_t> uniq(route.begin(), route.end());
    CHECK(uniq.size() == route.size());
    for (std::uint32_t rp : route) {
        CHECK(!ChaliceRow(rp));
        CHECK(Spot(rp));
    }
    std::size_t lamps_in = 0;
    for (const auto& l : coop::lamptravel::kLamps) lamps_in += uniq.count(coop::lamptravel::ReturnPoint(l));
    CHECK(lamps_in == coop::lamptravel::kLampCount);  // every lamp is visited
    std::printf("route: %zu rows (%zu lamps + the Dream + %zu other rows)\n", route.size(), lamps_in,
                route.size() - lamps_in - 1);
    {
        Options o;
        o.lamps = {2402950, 2902950, 2402950, 99999999};
        const auto r = BuildRoute(all, Spot, o);
        CHECK(r.size() == 1 && r[0] == 2402950);  // chalice, duplicate and unknown rows dropped
        Options m;
        m.max = 3;
        CHECK(BuildRoute(all, Spot, m).size() == 3);
    }

    // Progress and resume.
    {
        const std::string text = "start 2102950\ndone 2102950 120 2 80 45 m21_00_00_00\nstart 2412950\r\n"
                                 "start 2412950\nstart 2412951\nskip 2412952 no load\n";
        const Progress p = ParseProgress(text);
        CHECK(p.done.count(2102950));
        CHECK(p.starts.at(2412950) == 2);
        CHECK(!p.finished);
        CHECK(SkipReason(p, 2102950) != nullptr);
        CHECK(SkipReason(p, 2412950) != nullptr);  // crashed there twice
        CHECK(SkipReason(p, 2412951) == nullptr);  // once: tried again
        CHECK(SkipReason(p, 2412952) != nullptr);
        const std::vector<std::uint32_t> r = {2102950, 2412950, 2412951, 2412952, 2412953};
        CHECK(NextIndex(r, p, 0) == 2);
        CHECK(NextIndex(r, p, 3) == 4);
        CHECK(NextIndex(r, p, 5) == 5);
        CHECK(HandledCount(r, p) == 3);
        CHECK(ParseProgress(text + "finished\n").finished);
        CHECK(ParseProgress("garbage\n\nstart x\n").starts.empty());
    }
    CHECK(DoneLine(2402950, 10, 2, 120.4, 50.2, "m24_00_00_00") == "done 2402950 10 2 120 50 m24_00_00_00");
    CHECK(ParseProgress(DoneLine(2402950, 10, 2, 1, 1, "m") + "\n").done.count(2402950));

    // Steps: the routine stays short (75 rows must fit in about an hour).
    CHECK(Seconds(LookSteps()) > 5.0f && Seconds(LookSteps()) < 15.0f);
    CHECK(Seconds(WalkSteps()) > 5.0f && Seconds(WalkSteps()) < 15.0f);
    CHECK(Seconds(ActionSteps()) < 30.0f);
    CHECK(Seconds(LoadoutSteps()) < 90.0f);
    for (const auto& st : ActionSteps()) CHECK(st.what && st.what[0]);
    CHECK(std::string(AreaName(2402950)) == "Cathedral Ward");
    CHECK(std::string(AreaName(9990000)).empty());

    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("world warper rules: all checks passed\n");
    return 0;
}

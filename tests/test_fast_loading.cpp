// Fast loading rules (gpu/shim/bbport_fast_loading.h). ninja -C out/gpu fast-loading-test
#include "bbport_fast_loading.h"
#include "bbport_load_policy.h"

#include <cassert>
#include <cstdio>

using namespace bbport::fastload;

int main() {
    // The frame-rate presets: only those that step by the measured frame time.
    assert(MeasuredSteps("60", nullptr));
    assert(MeasuredSteps("90", nullptr));
    assert(MeasuredSteps("uncap", nullptr));
    assert(MeasuredSteps("uncap", "1"));
    assert(!MeasuredSteps("30", nullptr));       // vanilla: fixed 1/30 s steps
    assert(!MeasuredSteps(nullptr, nullptr));    // without run.bat: unknown patches
    assert(!MeasuredSteps("", nullptr));
    assert(!MeasuredSteps("uncap", "0"));        // the FPS patch off: vanilla steps
    assert(!MeasuredSteps("60", "off"));

    // BB_FAST_LOADING.
    assert(ParseMode(nullptr).mode == 0);  // opt-in
    assert(ParseMode("on").mode == 1);
    assert(ParseMode("1").mode == 1);
    assert(ParseMode("0").mode == 0);
    assert(ParseMode("off").mode == 0);
    assert(ParseMode("ab").mode == 2 && ParseMode("ab").period == 2);
    assert(ParseMode("ab:3").mode == 2 && ParseMode("ab:3").period == 3);
    assert(ParseMode("ab:0").period == 2);

    // A/B: off for N loads, then on for N loads.
    assert(!AbOn(0, 2) && !AbOn(1, 2) && AbOn(2, 2) && AbOn(3, 2) && !AbOn(4, 2));
    assert(!AbOn(0, 1) && AbOn(1, 1) && !AbOn(2, 1));

    // Uncapped only in a load, without a cutscene, with measured steps and enabled.
    Inputs in{true, true, false, true};
    assert(Uncapped(in));
    in.remo = true;
    assert(!Uncapped(in));
    in = {true, false, false, true};
    assert(!Uncapped(in));
    in = {false, true, false, true};
    assert(!Uncapped(in));
    in = {true, true, false, false};
    assert(!Uncapped(in));

    // 0.8.1 (loadux081): a real frame every 16 ms while unpaced.
    using namespace bbport::loadpolicy;
    assert(DrawDue(5, 0, 16'000'000));                       // the first one at once
    assert(!DrawDue(20'000'000, 10'000'000, 16'000'000));    // 10 ms after the last
    assert(DrawDue(26'000'000, 10'000'000, 16'000'000));     // exactly 16 ms
    assert(DrawDue(90'000'000, 10'000'000, 16'000'000));

    // Load threads: min(4, max(3, CPUs - 2)), never below the game's 3, 0 when switched off.
    assert(DefaultWorkers(16, true) == 4 && DefaultWorkers(8, true) == 4 && DefaultWorkers(6, true) == 4);
    assert(DefaultWorkers(5, true) == 3 && DefaultWorkers(4, true) == 3 && DefaultWorkers(2, true) == 3);
    assert(DefaultWorkers(1, true) == 3 && DefaultWorkers(0, true) == 3);
    assert(DefaultWorkers(16, false) == 0);
    assert(ParseWorkers(nullptr, 4) == 4 && ParseWorkers("", 4) == 4);
    assert(ParseWorkers("0", 4) == 0 && ParseWorkers("off", 4) == 0 && ParseWorkers("false", 4) == 0);
    assert(ParseWorkers("6", 4) == 6 && ParseWorkers("99", 4) == 8 && ParseWorkers("-2", 4) == 0);

    // Fades twice as fast: the extra countdown after the game's own step, never to 0, never from a jump.
    assert(FadeExtra(0.5f, 0.4f, 2.0f, 0.25f) > 0.099f && FadeExtra(0.5f, 0.4f, 2.0f, 0.25f) < 0.101f);
    assert(FadeExtra(0.5f, 0.5f, 2.0f, 0.25f) == 0.0f);      // not moving
    assert(FadeExtra(0.0f, 0.5f, 2.0f, 0.25f) == 0.0f);      // a new fade started (prev 0)
    assert(FadeExtra(0.2f, 0.5f, 2.0f, 0.25f) == 0.0f);      // counting up
    assert(FadeExtra(0.9f, 0.3f, 2.0f, 0.25f) == 0.0f);      // a jump, not the game's step
    assert(FadeExtra(0.05f, 0.04f, 2.0f, 0.25f) <= 0.04f * 0.9f + 1e-6f && FadeExtra(0.05f, 0.04f, 2.0f, 0.25f) > 0.0f);
    assert(FadeExtra(0.5f, 0.4f, 1.0f, 0.25f) == 0.0f);      // factor 1 = off
    assert(FadeExtra(0.5f, 0.4f, 3.0f, 0.25f) > 0.19f);      // other factors work the same way
    std::puts("fast-loading-test: ok");
    return 0;
}

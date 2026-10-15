// Fast loading rules (gpu/shim/bbport_fast_loading.h). ninja -C out/gpu fast-loading-test
#include "bbport_fast_loading.h"

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
    std::puts("fast-loading-test: ok");
    return 0;
}

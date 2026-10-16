// Frame rate by hooks: the pure rules (gpu/shim/bbport_frame_rate_logic.h). ninja -C out/gpu frame-rate-test
#include "bbport_frame_rate_logic.h"

#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdio>

using namespace BbFrameRate;

static bool Near(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) <= eps; }

int main() {
    // Which mode runs.
    assert(HooksWanted("uncap", nullptr, nullptr) == HooksDefault);
    assert(HooksWanted("60", nullptr, "1"));
    assert(HooksWanted("90", nullptr, "1"));
    assert(!HooksWanted("uncap", nullptr, "0"));     // the FPS++ patches instead
    assert(!HooksWanted("uncap", "0", "1"));         // FPS patch off: the game's own 30 FPS
    assert(!HooksWanted("30", nullptr, "1"));
    assert(!HooksWanted(nullptr, nullptr, "1"));     // without run.bat: no frame-rate choice
    assert(!HooksWanted("", nullptr, "1"));

    // The pacer's target.
    assert(TargetRate("uncap", nullptr) == MaxRate);
    assert(TargetRate("60", nullptr) == 60);
    assert(TargetRate("90", nullptr) == 90);
    assert(TargetRate("uncap", "144") == 144);
    assert(TargetRate("60", "144") == 60);
    assert(TargetRate("uncap", "1000") == MaxRate);
    assert(TargetRate("uncap", "10") == 30);
    assert(TargetRate("uncap", "0") == MaxRate);

    // The step: clamped to the shortest frame and 1/30 s.
    assert(Near(StepFor(1.0f / 60, 1.0f / 480), 1.0f / 60));
    assert(Near(StepFor(0.5f, 1.0f / 480), Frame30));          // below 30 FPS the game slows down
    assert(Near(StepFor(0.0001f, 1.0f / 480), 1.0f / 480));
    assert(Near(StepFor(NAN, 1.0f / 480), Frame30));

    // At 30 FPS the values are the game's own constants.
    Live l;
    Compute(l, Frame30);
    assert(Near(l.dt, Frame30) && Near(l.decay95[0], 0.95f) && Near(l.decay95[3], 0.95f));
    assert(Near(l.approach30, Frame30) && Near(l.approach10, 0.1f));
    assert(Near(l.step_down, -0.1f) && Near(l.step_up, 0.1f) && Near(l.decay08, 0.8f) && Near(l.cloth, 1.0f));

    // Per-frame factors compose: n frames of 1/(30 n) s give the same as one 30 FPS frame.
    for (const int n : {2, 3, 4, 8, 16}) {
        Compute(l, Frame30 / float(n));
        float d95 = 1, d08 = 1, rest30 = 1, rest10 = 1, up = 0;
        for (int i = 0; i < n; ++i) {
            d95 *= l.decay95[0];
            d08 *= l.decay08;
            rest30 *= 1 - l.approach30;
            rest10 *= 1 - l.approach10;
            up += l.step_up;
        }
        assert(Near(d95, 0.95f, 1e-4f) && Near(d08, 0.8f, 1e-4f));
        assert(Near(1 - rest30, Frame30, 1e-4f) && Near(1 - rest10, 0.1f, 1e-4f));
        assert(Near(up, 0.1f, 1e-4f));
        assert(Near(l.cloth, n >= 2 ? 0.5f : 1.0f));
    }
    Compute(l, Frame30, 0.75f); // BB_CLOTH_STIFFNESS
    assert(Near(l.cloth, 0.75f));

    // The layout the game's code reads.
    static_assert(offsetof(Live, decay95) % 16 == 0, "decay95 is read as one vector");
    static_assert(alignof(Live) == 64);
    std::puts("frame-rate-test: ok");
    return 0;
}

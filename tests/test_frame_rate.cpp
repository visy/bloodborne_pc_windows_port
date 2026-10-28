// Frame rate by hooks: the pure rules (gpu/shim/bbport_frame_rate_logic.h). ninja -C out/gpu frame-rate-test
#include "bbport_frame_rate_logic.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <utility>
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

    // The steady time (Supermedo 7a39487f): a stutter is no kick; a lasting change is followed.
    {
        Live st;
        st.steady = 0.0f;
        const float base = 1.0f / 120.0f;
        for (int i = 0; i < 40; ++i) Compute(st, base);
        assert(Near(st.steady, base, 1e-6f) && Near(st.cloth_ratio, 1.0f, 1e-4f));
        float before = st.steady;
        Compute(st, 0.035f); // the stall
        assert(Near(st.steady, before * SteadyUp, 1e-6f) && st.cloth_ratio < 1.2f);
        float worst = 0;
        for (int i = 0; i < 12; ++i) { // the catch-up frames of 4 ms
            const float prev = st.steady;
            Compute(st, 0.004f);
            worst = std::max(worst, std::fabs(st.steady / prev - 1.0f));
        }
        assert(worst <= 0.0501f);
        // The first frame after the stall: 35 ms against a steady 8.3 ms asks for x4.2; the cloth gets x1.05.
        Live a;
        a.steady = 1.0f / 120.0f;
        Compute(a, 0.035f);
        assert(Near(a.cloth_ratio * 0.035f, a.steady, 1e-6f) && a.steady < 0.0088f);
        // A lasting change 120 -> 60 FPS arrives in ~15 frames; never above 1/30 s.
        for (int i = 0; i < 15; ++i) Compute(a, 1.0f / 60.0f);
        assert(Near(a.steady, 1.0f / 60.0f, 1e-4f));
        Compute(a, 0.5f);
        for (int i = 0; i < 80; ++i) Compute(a, Frame30);
        assert(a.steady <= Frame30 + 1e-9f);
        assert(Near(SteadyStep(0.0f, 0.05f), Frame30) && Near(SteadyStep(NAN, 0.01f), 0.01f));
    }

    // Jumps (Supermedo a83f591d) and plants (e0bf9f48) at every frame rate: the horizontal air speed times
    // 0.95 per 30 FPS frame, integrated for 1.2 s from 6 m/s, lands the same distance at any rate; the
    // plants' clock gains one second per second. Frames of uneven length (a stutter) too.
    {
        auto jump = [](float fps, bool uneven) {
            Live l;
            l.steady = 0.0f;
            double x = 0, t = 0, clock = 0, speed = 6.0;
            for (int i = 0; t < 1.2; ++i) {
                float dt = 1.0f / fps;
                if (uneven && i % 9 == 4) dt *= 2.5f; // a stretched frame...
                if (uneven && i % 9 == 5) dt *= 0.4f; // ...and the catching up
                dt = StepFor(dt, 1.0f / 480);
                Compute(l, dt);
                x += speed * dt;
                speed *= l.decay95[0];
                t += dt;
                clock += l.steady; // plants advance by the steady time
            }
            return std::pair<double, double>{x, clock / t};
        };
        const double at30 = jump(30, false).first;
        for (const float fps : {60.0f, 90.0f, 120.0f}) {
            assert(std::fabs(jump(fps, false).first - at30) < 0.1);
            assert(std::fabs(jump(fps, true).first - at30) < 0.15);
            std::printf("jump 1.2 s from 6 m/s: %.0f FPS %.2f m (30 FPS %.2f m); fixed 0.95 a frame would give %.2f m\n", double(fps),
                        jump(fps, false).first, at30, [&] {
                            double x = 0, v = 6.0;
                            for (int i = 0; i < int(1.2 * fps); ++i) x += v / fps, v *= 0.95;
                            return x;
                        }());
            const double rate = jump(fps, false).second; // plant clock per second of game time: 1 (it may trail a little)
            assert(rate > 0.9 && rate <= 1.001);
        }
    }

    // Above 30 at all (the bbhost fixes, both modes).
    assert(HighRate("uncap", nullptr) && HighRate("60", nullptr) && HighRate("90", "1"));
    assert(!HighRate("30", nullptr) && !HighRate(nullptr, nullptr) && !HighRate("", nullptr));
    assert(!HighRate("uncap", "0") && !HighRate("60", "off"));

    // The effects' step in whole 1/60 s, the rest carried (bbhost 6f735f4).
    {
        auto pattern = [](float dt, int frames, float* out) {
            double carry = 0;
            for (int i = 0; i < frames; ++i) out[i] = SfxStep(carry, dt);
        };
        float s[12];
        pattern(1.0f / 60.0f, 12, s); // 60 FPS: 1/60 every frame, for good
        for (float v : s) assert(Near(v, 1.0f / 60.0f));
        double carry = 0;
        for (int i = 0; i < 1000000; ++i) assert(Near(SfxStep(carry, 1.0f / 60.0f), 1.0f / 60.0f));
        pattern(1.0f / 90.0f, 3, s); // 90: 0, 1/60, 1/60
        assert(Near(s[0], 0) && Near(s[1], 1.0f / 60.0f) && Near(s[2], 1.0f / 60.0f));
        pattern(1.0f / 120.0f, 4, s); // 120: 0, 1/60, 0, 1/60
        assert(Near(s[0], 0) && Near(s[1], 1.0f / 60.0f) && Near(s[2], 0) && Near(s[3], 1.0f / 60.0f));
        pattern(Frame30, 2, s); // 30: two sixtieths
        assert(Near(s[0], Frame30) && Near(s[1], Frame30));
        carry = 0;
        assert(Near(SfxStep(carry, 1.0f), Frame30)); // a long frame: at most 1/30, as the game's step
        assert(Near(SfxStep(carry, NAN), Frame30));
        // Time kept over a varying frame rate: the sum of the steps is the sum of the frames.
        carry = 0;
        double sum = 0, real = 0;
        for (int i = 0; i < 10000; ++i) {
            const float dt = 1.0f / float(70 + (i * 37) % 200);
            real += dt;
            sum += SfxStep(carry, dt);
        }
        assert(std::fabs(sum - real) < 1.0 / 60.0 + 1e-3);
    }

    // The layout the game's code reads.
    static_assert(offsetof(Live, decay95) % 16 == 0, "decay95 is read as one vector");
    static_assert(alignof(Live) == 64);
    std::puts("frame-rate-test: ok");
    return 0;
}

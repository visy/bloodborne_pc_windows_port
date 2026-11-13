// Frame rate by hooks: the pure rules (gpu/shim/bbport_frame_rate_logic.h). ninja -C out/gpu frame-rate-test
#include "bbport_frame_rate_logic.h"
#include "bbport_live_refresh_logic.h"

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


    // Live clock (Mrsuss60 a7cbfae6 / 83a168ee): when it is wanted.
    assert(!LiveWanted("uncap", nullptr, false));
    assert(LiveWanted("uncap", nullptr, true));
    assert(LiveWanted("60", nullptr, true));
    assert(!LiveWanted("30", nullptr, true));        // the game's own rate
    assert(!LiveWanted("uncap", "0", true));         // FPS patch off
    assert(LiveTargetFromRate(60) == 60 && LiveTargetFromRate(144) == 144);
    assert(LiveTargetFromRate(MaxRate) == 0 && LiveTargetFromRate(240) == 0 && LiveTargetFromRate(10) == 30);

    // Variable refresh rate cap (0.7.8): just under the display's refresh; unknown refresh = no cap.
    assert(VrrFrameCap(60.0f) == 59 && VrrFrameCap(59.94f) == 58 && VrrFrameCap(120.0f) == 116 && VrrFrameCap(144.0f) == 138);
    assert(VrrFrameCap(165.0f) == 157 && VrrFrameCap(240.0f) == 224 && VrrFrameCap(30.0f) == 29);
    assert(VrrFrameCap(0.0f) == 0 && VrrFrameCap(-5.0f) == 0 && VrrFrameCap(NAN) == 0 && VrrFrameCap(10.0f) == 0);
    for (float hz = 24.0f; hz <= 500.0f; hz += 0.5f) assert(VrrFrameCap(hz) < hz && VrrFrameCap(hz) >= 23);

    // Live clock step: the step is the frame's time (game time follows real time); only +-1.5% micro-jitter is
    // locked to the target, with the difference carried over.
    {
        LiveClockState st;
        float interval = 0;
        const float t60 = 1.0f / 60;
        assert(Near(LiveClockStep(st, t60 * 1.01f, 60, 30, interval), t60, 1e-6f) && Near(interval, t60, 1e-6f));
        // A long frame is stepped as it is (up to 1/30 s), no lag: time is conserved.
        { LiveClockState a; assert(Near(LiveClockStep(a, 0.025f, 60, 30, interval), 0.025f, 1e-6f)); }
        { LiveClockState a; assert(Near(LiveClockStep(a, 0.5f, 60, 30, interval), Frame30, 1e-6f)); }   // never above 1/30 s
        { LiveClockState a; assert(Near(LiveClockStep(a, 0.005f, 60, 30, interval), 0.005f, 1e-6f)); }  // a short frame too
        // Over a run of jittery frames the stepped time equals the real time (below the clamps).
        {
            LiveClockState sj;
            double real = 0, game = 0;
            for (int i = 0; i < 20000; ++i) {
                const float m = 1.0f / 60 * (1.0f + 0.4f * std::sin(float(i) * 0.7f)) * (1.0f + 0.01f * std::sin(float(i)));
                real += double(m);
                game += double(LiveClockStep(sj, m, 60, 30, interval));
            }
            assert(std::fabs(game / real - 1.0) < 0.002);
            // Frames a little over the target (a PC that holds 58 FPS): game time stays 1.0, not 0.97.
            LiveClockState sk;
            double r2 = 0, g2 = 0;
            for (int i = 0; i < 20000; ++i) {
                const float m = 1.0f / 58;
                r2 += double(m);
                g2 += double(LiveClockStep(sk, m, 60, 30, interval));
            }
            assert(std::fabs(g2 / r2 - 1.0) < 0.002);
        }
        // Invalid measurements count as the target; uncapped paces at the 240 FPS ceiling.
        LiveClockState st2;
        assert(Near(LiveClockStep(st2, NAN, 120, 30, interval), 1.0f / 120, 1e-6f));
        LiveClockState st3;
        assert(Near(LiveClockStep(st3, 0.0f, 0, 30, interval), 1.0f / 240, 1e-6f) && Near(interval, 1.0f / 240, 1e-6f));
        LiveClockState st4;
        assert(Near(LiveClockStep(st4, 0.2f, 30, 240, interval), 1.0f / 30, 1e-6f));
        // Switching the target resets the carry.
        LiveClockState st5;
        LiveClockStep(st5, 1.0f / 60 * 1.01f, 60, 30, interval);
        assert(Near(LiveClockStep(st5, 1.0f / 120, 120, 30, interval), 1.0f / 120, 1e-6f));
    }

    // The wind's step is half the task step (30's 1/60 per 1/30 s) and the whole-1/30-s gate steps 30 times a
    // game second at any rate, every frame at 30 (droogie/bbhost 5cee912, 57d2b9f, f24d18f).
    {
        assert(Near(WindStep(Frame30), 1.0f / 60, 1e-9f) && Near(WindStep(1.0f / 120), 1.0f / 240, 1e-9f));
        for (const int fps : {30, 60, 90, 120, 144, 240}) {
            float banked = 0.0f;
            int steps = 0, frames = fps * 600;
            for (int i = 0; i < frames; ++i) {
                const bool whole = Step30Gate(banked, 1.0f / float(fps));
                steps += whole;
                if (fps == 30) assert(whole);
            }
            assert(std::abs(steps - 30 * 600) <= 2);
        }
        // Uneven frames (1/240 .. 1/30 s) still give 30 steps a game second.
        float banked = 0.0f, game = 0.0f;
        int steps = 0;
        for (int i = 0; i < 100000; ++i) {
            const float dt = 1.0f / 240 + (1.0f / 30 - 1.0f / 240) * float((i * 7919) % 101) / 100.0f;
            steps += Step30Gate(banked, dt);
            game += dt;
        }
        assert(std::fabs(double(steps) / double(game) - 30.0) < 0.05);
        // The exact gate (the blink's) closes a step only on a whole 1/30 s: every 4th frame at 120, every frame at 30.
        for (const int fps : {30, 60, 120}) {
            float banked = 0.0f;
            int steps = 0;
            for (int i = 0; i < fps * 10; ++i) {
                const bool whole = Step30Gate(banked, 1.0f / float(fps), 0.98f);
                steps += whole;
                if (fps == 30) assert(whole);
                if (whole && fps > 30) assert(i % (fps / 30) == fps / 30 - 1);
            }
            assert(steps == 300);
        }
        Live l;
        Compute(l, 1.0f / 120);
        assert(Near(l.wind, l.steady * 0.5f, 1e-9f));
    }

    // Live FPS stuck after a respawn (issue #35): the swapchain is re-created once, only on a sustained drop.
    {
        using namespace BbLiveRefresh;
        State st;
        auto stay = [&](double fps, int secs, int& fires) {
            for (int i = 0; i < secs; ++i) fires += Second(st, fps, true, 0) ? 1 : 0;
        };
        int fires = 0;
        stay(95, 20, fires);                 // healthy first stay, no baseline yet
        assert(fires == 0);
        Second(st, 0, false, 0);             // a load
        stay(66, 30, fires);                 // stuck at 66 after the load: fires once after settle + sustain
        assert(fires == 1);
        int f2 = 0;
        Second(st, 0, false, 0);
        stay(66, 30, f2);                    // the stuck rate is the next baseline: nothing more
        assert(f2 == 0);
        // a recovered stay, then a load into a slower scene that is steady: not a drop of a stay but a baseline of 66
        State a;
        int fa = 0;
        for (int i = 0; i < 20; ++i) fa += Second(a, 95, true, 0);
        Second(a, 0, false, 0);
        for (int i = 0; i < 40; ++i) fa += Second(a, i % 2 ? 90 : 100, true, 0); // jitter, no drop
        assert(fa == 0);
        // short blips do not count
        State b;
        int fb = 0;
        for (int i = 0; i < 20; ++i) fb += Second(b, 95, true, 0);
        Second(b, 0, false, 0);
        for (int i = 0; i < 60; ++i) fb += Second(b, i % 5 == 0 ? 40 : 95, true, 0);
        assert(fb == 0);
        // a low baseline (a cap or a slow scene) never counts; a stay shorter than the window sets none
        State c;
        int fc = 0;
        for (int i = 0; i < 20; ++i) fc += Second(c, 30, true, 0);
        Second(c, 0, false, 0);
        for (int i = 0; i < 30; ++i) fc += Second(c, 10, true, 0);
        assert(fc == 0);
        State d;
        int fd = 0;
        for (int i = 0; i < 8; ++i) fd += Second(d, 95, true, 0); // 4 judged seconds < Window
        Second(d, 0, false, 0);
        for (int i = 0; i < 30; ++i) fd += Second(d, 50, true, 0);
        assert(fd == 0);
        // a changed frame limit drops the baseline
        State e;
        int fe = 0;
        for (int i = 0; i < 20; ++i) fe += Second(e, 95, true, 0);
        Second(e, 0, false, 0);
        for (int i = 0; i < 30; ++i) fe += Second(e, 60, true, 1);
        assert(fe == 0);
        // test aid: N in-world seconds after each load, once per stay
        State g;
        int fg = 0;
        for (int i = 0; i < 10; ++i) fg += Second(g, 95, true, 0, 5);
        assert(fg == 1);
        Second(g, 0, false, 0, 5);
        for (int i = 0; i < 10; ++i) fg += Second(g, 95, true, 0, 5);
        assert(fg == 2);
    }

    // Cloth fix (#37): the scales, the bend softening, the damping and the spec parser.
    {
        auto near = [](float a, float b) { return std::fabs(a - b) < 1e-5f; };
        assert(near(ClothScaleFor(ClothScale::Linear30, 1.0f / 30), 1.0f));
        assert(near(ClothScaleFor(ClothScale::Linear30, 1.0f / 60), 0.5f));
        assert(near(ClothScaleFor(ClothScale::Linear30, 1.0f / 120), 0.25f));
        assert(near(ClothScaleFor(ClothScale::Quad60, 1.0f / 60), 1.0f));
        assert(near(ClothScaleFor(ClothScale::Quad60, 1.0f / 90), 4.0f / 9.0f));
        assert(near(ClothScaleFor(ClothScale::Quad60, 1.0f / 120), 0.25f));
        assert(near(ClothScaleFor(ClothScale::Quad60, 1.0f / 30), 1.0f)); // capped: below 60 FPS the 60 tuning
        assert(near(ClothScaleFor(ClothScale::None, 1.0f / 120), 1.0f));
        assert(near(ClothScaleFor(ClothScale::Linear30, 0.0f), 1.0f));
        assert(near(ClothSoften(1.0f / 30), 0.0f));
        assert(near(ClothSoften(1.0f / 60), 1.0f));
        assert(near(ClothSoften(1.0f / 240), 3.0f)); // r capped at 4
        assert(near(ClothSoften(1.0f / 20), 0.0f));
        assert(near(ClothBend(1.0f, 3.0f), 1.0f));   // a rigid constraint stays rigid
        assert(near(ClothBend(0.5f, 0.0f), 0.5f));
        assert(near(ClothBend(0.5f, 1.0f), 0.5f / 1.5f));
        assert(ClothBend(0.2f, 3.0f) < 0.2f);
        assert(near(ClothDamping(0.9f, 1.0f / 30), 0.9f)); // 30 and 60 FPS: as the game's
        assert(near(ClothDamping(0.9f, 1.0f / 60), 0.9f));
        assert(near(ClothDamping(0.9f, 1.0f / 120), std::pow(0.9f, 0.5f)));
        assert(near(ClothDamping(1.0f, 1.0f / 120), 1.0f));
        assert(near(ClothDamping(0.0f, 1.0f / 120), 0.0f));
        ClothTune t;
        assert(ParseClothTune("cur", t) && t.scale == ClothScale::Linear30 && !t.all_types && !t.caves);
        assert(ParseClothTune("jg", t) && t.scale == ClothScale::Quad60 && t.caves && !t.bend);
        assert(ParseClothTune("nw", t) && t.bend && t.damp && t.scale == ClothScale::None);
        assert(ParseClothTune("lin,all,bend", t) && t.scale == ClothScale::Linear30 && t.all_types && t.bend && !t.damp);
        assert(ParseClothTune("stock", t) && ClothTuneName(t) == "stock");
        assert(ParseClothTune("pow2", t) && t.scale == ClothScale::Linear30 && near(t.power, 2.0f) && !t.caves);
        assert(near(ClothScaleFor(ClothScale::Linear30, 1.0f / 60, 2.0f), 0.25f));
        assert(near(ClothScaleFor(ClothScale::Linear30, 1.0f / 120, 2.0f), 0.0625f));
        assert(near(ClothScaleFor(ClothScale::Linear30, 1.0f / 30, 2.0f), 1.0f));
        assert(ClothTuneName(t) == "pow2,legacy,hook");
        assert(ParseClothTune("pow2,smooth", t) && t.smooth && ClothTuneName(t) == "pow2,legacy,hook,smooth");
        assert(!ParseClothTune("pow9", t) && !ParseClothTune("powx", t));
        ParseClothTune("stock", t);
        ClothTune keep = t;
        assert(!ParseClothTune("lin,bogus", t) && t.scale == keep.scale);
        ParseClothTune("quad,all,caves,bend,damp", t);
        ClothTune back;
        assert(ParseClothTune(ClothTuneName(t).c_str(), back) && back.scale == t.scale && back.all_types && back.caves &&
               back.bend && back.damp);
    }

    // The layout the game's code reads.
    static_assert(offsetof(Live, decay95) % 16 == 0, "decay95 is read as one vector");
    static_assert(alignof(Live) == 64);
    static_assert(offsetof(Live, caves) % 16 == 0, "the kernels read the scale as one vector");
    std::puts("frame-rate-test: ok");
    return 0;
}

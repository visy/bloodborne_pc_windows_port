// SPDX-License-Identifier: GPL-3.0-or-later
// bbport: the frame-rate sweep (diagnostics for bbport_frame_rate, the frame rate by hooks).
//
// BB_FRAME_SWEEP=30,60,90,120,144,480 runs, in ONE game session, the same scripted movement at each
// pacer target (BbFrameRate::SetTargetRate) and logs what the game did, measured in real time:
//   - steps per second and game time per real second (the sum of the steps' dt),
//   - run and sprint speed (m/s, the physics position, horizontal), forward and back,
//   - stamina drain while sprinting and regen after it (points per s, regen delay),
//   - a backstep and a forward roll (distance, how long the body moves: animation root motion),
//   - a jump from a sprint (air time and height: gravity).
// The 30 entry is the reference (dt = 1/30 s: the game's own constants). With BB_FRAME_HOOKS=0 (the
// FPS++ patches) the rate cannot be changed: one pass at the patch's rate ("patches").
// Input goes through the BB_PAD_FILE pad script (src/runtime_pad.c); without it only the timing is
// measured. BB_FRAME_SWEEP_WAIT (s, default 240): how long to wait for the world, else it gives up.
// Lines: "Frame sweep: <rate> <test>: ...", and "Frame sweep: done" at the end.
#include "bbport_frame_rate.h"

#include "party/coop_hooks.h"
#include "party/game_state.h"

#include <cstdarg>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace BbFrameRate {
namespace {
using u64 = std::uint64_t;
using Clock = std::chrono::steady_clock;

double Now() {
    static const Clock::time_point t0 = Clock::now();
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

template <class T>
T Field(u64 base, u64 off, T fallback = T{}) {
    T v{};
    return base && coop::SafeGet(base + off, &v) ? v : fallback;
}

struct Sample {
    double t = 0;
    float pos[3] = {};
    int stamina = -1;
    bool ok = false;
};

/// The local player's physics position and stamina (as party_hud_game reads them).
Sample Read() {
    Sample s;
    s.t = Now();
    const u64 chr = Field<u64>(Field<u64>(coop::Guest(0x553e878), 0), 0x60);
    if (!chr) return s;
    const u64 phys = Field<u64>(Field<u64>(Field<u64>(Field<u64>(chr, 0x58), 8), 0x3b0), 0x68);
    float p[4] = {};
    if (!phys || !coop::SafeRead(phys + 0x1e0, p, sizeof p)) return s;
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(p[i]) || std::fabs(p[i]) > 100000.0f) return s;
        s.pos[i] = p[i];
    }
    const u64 dm = Field<u64>(Field<u64>(chr, 0x3b0), 0x20);
    const int st = Field<int>(dm, 0x134, -1), smx = Field<int>(dm, 0x138, -1);
    if (smx > 0 && st >= 0 && st <= smx) s.stamina = st;
    s.ok = true;
    return s;
}

std::string g_pad;

void Pad(const char* tokens) {
    if (g_pad.empty()) return;
    if (FILE* f = std::fopen(g_pad.c_str(), "w")) {
        std::fputs(tokens, f);
        std::fclose(f);
    }
}

/// Samples every 4 ms for `seconds` while `tokens` are held (then released when `release`).
std::vector<Sample> Hold(const char* tokens, double seconds, bool release = true) {
    Pad(tokens);
    std::vector<Sample> out;
    const double end = Now() + seconds;
    while (Now() < end) {
        const Sample s = Read();
        if (s.ok) out.push_back(s);
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    if (release) Pad("");
    return out;
}

float Horizontal(const Sample& a, const Sample& b) {
    const float dx = b.pos[0] - a.pos[0], dz = b.pos[2] - a.pos[2];
    return std::sqrt(dx * dx + dz * dz);
}

/// Mean horizontal speed between t0 and t1 (s after the first sample).
double Speed(const std::vector<Sample>& v, double from, double to) {
    if (v.size() < 2) return -1;
    const double base = v.front().t;
    const Sample* a = nullptr;
    const Sample* b = nullptr;
    double path = 0;
    for (std::size_t i = 1; i < v.size(); ++i) {
        const double t = v[i].t - base;
        if (t < from) continue;
        if (t > to) break;
        if (!a) a = &v[i - 1];
        path += Horizontal(v[i - 1], v[i]);
        b = &v[i];
    }
    return a && b && b->t > a->t ? path / (b->t - a->t) : -1;
}

/// Stamina change per second between t0 and t1.
double StaminaRate(const std::vector<Sample>& v, double from, double to) {
    const Sample* a = nullptr;
    const Sample* b = nullptr;
    if (v.empty()) return 0;
    const double base = v.front().t;
    for (const Sample& s : v) {
        const double t = s.t - base;
        if (s.stamina < 0 || t < from) continue;
        if (t > to) break;
        if (!a) a = &s;
        b = &s;
    }
    return a && b && b->t > a->t ? double(b->stamina - a->stamina) / (b->t - a->t) : 0;
}

/// How long the body moves (speed above 0.3 m/s over 20 ms windows) and how far (horizontal).
void Motion(const std::vector<Sample>& v, double* seconds, double* metres) {
    *seconds = 0;
    *metres = 0;
    if (v.size() < 2) return;
    double first = -1, last = -1;
    std::size_t j = 0;
    for (std::size_t i = 1; i < v.size(); ++i) {
        while (j < i && v[i].t - v[j].t > 0.02) ++j;
        if (j == i) continue;
        const double sp = Horizontal(v[j], v[i]) / (v[i].t - v[j].t);
        if (sp > 0.3) {
            if (first < 0) first = v[j].t;
            last = v[i].t;
        }
    }
    if (first >= 0) {
        *seconds = last - first;
        *metres = Horizontal(v.front(), v.back());
    }
}

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    std::printf("Frame sweep: %s\n", line);
    std::fflush(stdout);
}

void Pass(const std::string& name) {
    const double r0 = Now();
    const double g0 = GameSeconds();
    const u64 s0 = Steps();
    // Idle: the step rate and game time per real second.
    std::this_thread::sleep_for(std::chrono::seconds(3));
    const double r1 = Now();
    Log("%s timing: %.1f steps/s, game time %.4f s per real s, dt %.5f s", name.c_str(),
        double(Steps() - s0) / (r1 - r0), Unlocked() ? (GameSeconds() - g0) / (r1 - r0) : -1.0, double(LastDt()));
    if (g_pad.empty()) return;

    auto run = Hold("ly=0", 3.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    auto back = Hold("ly=255", 3.0);
    Log("%s run: forward %.3f m/s, back %.3f m/s", name.c_str(), Speed(run, 1.0, 3.0), Speed(back, 1.0, 3.0));
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));

    auto sprint = Hold("ly=0 circle", 3.5);
    auto regen = Hold("", 3.5, false);
    // Regen delay: the first stamina rise after the release.
    double delay = -1;
    for (std::size_t i = 1; i < regen.size(); ++i) {
        if (regen[i].stamina > regen[i - 1].stamina && regen[i - 1].stamina >= 0) {
            delay = regen[i - 1].t - regen.front().t;
            break;
        }
    }
    // Regen rate: from the delay on, over 1 s (before stamina is full).
    const double rate = delay >= 0 ? StaminaRate(regen, delay + 0.05, delay + 1.05) : 0;
    auto sprint_back = Hold("ly=255 circle", 3.5);
    Log("%s sprint: forward %.3f m/s, back %.3f m/s; stamina %.1f per s while sprinting, regen %.1f per s after "
        "%.2f s",
        name.c_str(), Speed(sprint, 1.5, 3.5), Speed(sprint_back, 1.5, 3.5), StaminaRate(sprint, 0.6, 3.4), rate,
        delay);
    std::this_thread::sleep_for(std::chrono::milliseconds(3500));

    // Backstep (Circle without the stick): the root motion of one animation.
    Pad("circle");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto backstep = Hold("", 1.6, false);
    double bs_s = 0, bs_m = 0;
    Motion(backstep, &bs_s, &bs_m);
    // A forward roll (stick + Circle tap).
    Pad("ly=0");
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    const Sample before = Read();
    Pad("ly=0 circle");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto roll = Hold("", 1.6, false);
    if (before.ok) roll.insert(roll.begin(), before);
    double ro_s = 0, ro_m = 0;
    Motion(roll, &ro_s, &ro_m);
    Log("%s dodge: backstep %.3f m in %.3f s, roll %.3f m in %.3f s", name.c_str(), bs_m, bs_s, ro_m, ro_s);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    // A jump out of a sprint (L3 while sprinting): air time and height (gravity).
    auto lead = Hold("ly=0 circle", 1.6, false);
    const float ground = lead.empty() ? 0.0f : lead.back().pos[1];
    Pad("ly=0 circle l3");
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    auto jump = Hold("ly=0", 1.6);
    double up_at = -1, down_at = -1;
    float peak = ground;
    for (const Sample& s : jump) {
        if (s.pos[1] > ground + 0.05f && up_at < 0) up_at = s.t;
        if (up_at >= 0 && s.pos[1] > peak) peak = s.pos[1];
        if (up_at >= 0 && down_at < 0 && s.t > up_at + 0.05 && s.pos[1] <= ground + 0.05f) down_at = s.t;
    }
    Log("%s jump: air %.3f s, height %.3f m%s", name.c_str(), up_at >= 0 && down_at >= 0 ? down_at - up_at : -1.0,
        double(peak - ground), up_at < 0 ? " (no jump seen)" : "");
    auto home = Hold("ly=255 circle", 2.3);
    (void)home;
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
}

void Sweep(std::vector<int> rates) {
    const char* wait_env = std::getenv("BB_FRAME_SWEEP_WAIT");
    const double wait = wait_env ? std::atof(wait_env) : 240.0;
    const double start = Now();
    double steady = -1;
    for (;;) {
        if (Now() - start > wait) {
            Log("no world after %.0f s: not measured", wait);
            return;
        }
        const coop::GameSnapshot s = coop::ReadGameState(false);
        const bool up = s.world_up && !s.loading && Read().ok;
        if (!up) {
            steady = -1;
        } else if (steady < 0) {
            steady = Now();
        } else if (Now() - steady >= 8.0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    const Sample at = Read();
    Log("world up; start at (%.2f, %.2f, %.2f), map %08x", at.pos[0], at.pos[1], at.pos[2],
        coop::ReadGameState(false).map_id);
    if (!Unlocked()) rates = {0};
    for (const int rate : rates) {
        std::string name = "patches";
        if (rate > 0) {
            SetTargetRate(rate);
            name = std::to_string(rate);
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
        Pass(name);
    }
    Pad("");
    Log("done");
}
} // namespace

void StartSweep() {
    const char* v = std::getenv("BB_FRAME_SWEEP");
    if (!v || !*v) return;
    std::vector<int> rates;
    for (const char* p = v; *p;) {
        char* end = nullptr;
        const long r = std::strtol(p, &end, 10);
        if (end == p) break;
        if (r >= 30) rates.push_back(int(r));
        p = *end == ',' ? end + 1 : end;
    }
    if (const char* pad = std::getenv("BB_PAD_FILE")) g_pad = pad;
    std::thread([rates] { Sweep(rates); }).detach();
}
} // namespace BbFrameRate

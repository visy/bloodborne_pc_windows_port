// SPDX-License-Identifier: GPL-3.0-or-later
// Test aids: the scripted new game (BB_TEST_NEWGAME) and the prologue fast-forward
// (BB_TEST_PROLOGUE_SKIP); see test_newgame.h. The pure parts build without the game
// (BB_TEST_NEWGAME_NO_GAME, party-newgame-test).
#include "test_newgame.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <initializer_list>

namespace coop {

namespace {
constexpr double kHold = 0.12; // a tap: long enough for one game frame at 8 fps, short of auto-repeat
}

const char* NewGamePhaseName(NewGamePhase p) {
    switch (p) {
    case NewGamePhase::WaitTitle: return "wait-title";
    case NewGamePhase::NewGame: return "new-game";
    case NewGamePhase::Brightness: return "brightness";
    case NewGamePhase::Controls: return "controls";
    case NewGamePhase::WaitMovie: return "movie";
    case NewGamePhase::Contract: return "contract";
    case NewGamePhase::Finish: return "finish";
    case NewGamePhase::WaitLoad: return "wait-load";
    case NewGamePhase::Opening: return "opening";
    case NewGamePhase::Done: return "done";
    case NewGamePhase::Failed: return "FAILED";
    }
    return "?";
}

void NewGameDriver::Queue(std::initializer_list<Tap> taps) {
    queue_.assign(taps.begin(), taps.end());
    next_ = 0;
}

void NewGameDriver::Enter(NewGamePhase p, double t, std::string* log, const char* why) {
    if (log) {
        *log = std::string(NewGamePhaseName(phase_)) + " -> " + NewGamePhaseName(p) + ": " + why;
    }
    phase_ = p;
    phase_at_ = t;
    tries_ = 0;
}

std::uint32_t NewGameDriver::Step(const NewGameInput& in, std::string* log) {
    if (log) {
        log->clear();
    }
    // The tap queue first: hold, release, gap.
    if (next_ < queue_.size()) {
        const Tap& tap = queue_[next_];
        if (holding_) {
            if (in.t - tap_at_ < kHold) {
                return tap.buttons;
            }
            holding_ = false;
            busy_until_ = in.t + tap.gap;
            ++next_;
            return 0;
        }
        if (in.t >= busy_until_) {
            holding_ = true;
            tap_at_ = in.t;
            return tap.buttons;
        }
        return 0;
    }
    if (in.t < busy_until_ || finished()) {
        return 0;
    }
    const double in_phase = in.t - phase_at_;
    switch (phase_) {
    case NewGamePhase::WaitTitle:
        if (in.world_up || in.loading) {
            Enter(NewGamePhase::Opening, in.t, log, "a world is loading without New Game (not an empty save?)");
            break;
        }
        if (!in.title_ready) {
            ready_since_ = -1;
            break;
        }
        if (ready_since_ < 0) {
            ready_since_ = in.t;
        }
        if (in.t - ready_since_ >= 3.0) {
            Enter(NewGamePhase::NewGame, in.t, log, "title ready; Cross on New Game (the default item of an empty save)");
            Queue({{kPadCross, 4.0}});
        }
        break;
    case NewGamePhase::NewGame:
        if (in.world_up || in.loading) {
            Enter(NewGamePhase::WaitLoad, in.t, log, "loading after the first Cross: the save had a character (Continue)");
            break;
        }
        Enter(NewGamePhase::Brightness, in.t, log, "Down to Next, Cross");
        Queue({{kPadDown, 1.0}, {kPadCross, 4.0}});
        break;
    case NewGamePhase::Brightness:
        Enter(NewGamePhase::Controls, in.t, log, "Up (wraps to Next), Cross");
        movie_base_ = in.movie_count;
        Queue({{kPadUp, 1.0}, {kPadCross, 3.0}});
        break;
    case NewGamePhase::Controls:
        Enter(NewGamePhase::WaitMovie, in.t, log, "waiting for the movie");
        break;
    case NewGamePhase::WaitMovie:
        if (in.movie_count > movie_base_) {
            Enter(NewGamePhase::Contract, in.t, log, "movie started; Options skips it, then Cross on Enter Name");
            ime_base_ = in.ime_count;
            Queue({{0, 1.5}, {kPadOptions, 4.0}, {kPadCross, 3.0}});
        } else if (in_phase >= 10.0) {
            if (tries_ < 3) {
                ++tries_;
                phase_at_ = in.t;
                if (log) {
                    *log = "movie: none after 10 s; Cross on Next again (try " + std::to_string(tries_) + ")";
                }
                Queue({{kPadCross, 3.0}});
            } else {
                Enter(NewGamePhase::Contract, in.t, log, "no movie seen; trying the contract screen anyway");
                ime_base_ = in.ime_count;
                Queue({{kPadOptions, 4.0}, {kPadCross, 3.0}});
            }
        }
        break;
    case NewGamePhase::Contract:
        if (in.ime_count > ime_base_) {
            Enter(NewGamePhase::Finish, in.t, log,
                  "name entered; Up (wraps to Finish), Cross, Left (YES), Cross");
            Queue({{0, 2.0}, {kPadUp, 1.5}, {kPadCross, 2.5}, {kPadLeft, 1.0}, {kPadCross, 3.0}});
        } else if (tries_ < 6) {
            ++tries_;
            if (log) {
                *log = "contract: no name box yet; " + std::string(tries_ <= 3 ? "Cross" : "Options, Cross") +
                       " (try " + std::to_string(tries_) + ")";
            }
            if (tries_ <= 3) {
                Queue({{kPadCross, 3.0}});
            } else {
                Queue({{kPadOptions, 3.0}, {kPadCross, 3.0}});
            }
        } else {
            Enter(NewGamePhase::Failed, in.t, log, "the name box never opened (not on the contract screen?)");
        }
        break;
    case NewGamePhase::Finish:
        Enter(NewGamePhase::WaitLoad, in.t, log, "contract finalised; waiting for the loading screen");
        break;
    case NewGamePhase::WaitLoad:
        if (in.loading || in.world_up) {
            Enter(NewGamePhase::Opening, in.t, log, "loading the world");
        } else if (in_phase >= 20.0) {
            Enter(NewGamePhase::Failed, in.t, log, "no loading screen 20 s after Finish");
        }
        break;
    case NewGamePhase::Opening:
        if (!in.world_up || in.loading || !in.flags_ok) {
            break;
        }
        if (in.opening_done && !in.cutscene) {
            Enter(NewGamePhase::Done, in.t, log, "in the clinic world (12410000 ON, 9180 OFF)");
        } else if (!in.opening_done && in_phase >= 2.0) {
            if (tries_ < 10) {
                ++tries_;
                phase_at_ = in.t;
                if (log) {
                    *log = "opening cutscene (12410000 OFF): Options (try " + std::to_string(tries_) + ")";
                }
                Queue({{kPadOptions, 2.0}});
            } else {
                Enter(NewGamePhase::Failed, in.t, log, "the opening cutscene did not end after 10 Options");
            }
        }
        break;
    case NewGamePhase::Done:
    case NewGamePhase::Failed:
        break;
    }
    return 0;
}

std::uint32_t ParsePrologueWarp(const char* text) {
    if (!text || !*text) {
        return 2412951; // Central Yharnam lamp
    }
    if (!std::strcmp(text, "0") || !std::strcmp(text, "off") || !std::strcmp(text, "none")) {
        return 0;
    }
    if (!std::strcmp(text, "sickroom")) {
        return 2412950;
    }
    if (!std::strcmp(text, "dream")) {
        return 2102950;
    }
    if (!std::strcmp(text, "yharnam")) {
        return 2412951;
    }
    char* end = nullptr;
    const unsigned long v = std::strtoul(text, &end, 10);
    return end && !*end && v > 0 && v < 100000000ul ? std::uint32_t(v) : 2412951;
}

const std::vector<FlagWrite>& PrologueSkipFlags() {
    static const std::vector<FlagWrite> flags = {
        {12410000, true}, // opening cutscene done (normally already ON here)
        {9402, true},     // first death in m24_01 seen (common 9400)
        {9401, true},     // first Hunter's Dream cutscene done
        {12417810, true}, // sickroom lamp lit (9401 sets it too)
        {72101000, true}, // right-hand messenger choice: ShopLineupParam 2000 (weapon 7000000)
        {72101011, true}, // left-hand messenger choice: ShopLineupParam 2010 (weapon 14000000)
        {12101020, true}, // right-hand starting weapon taken
        {12101021, true}, // left-hand starting weapon taken
        {12100105, true}, // Doll awake (m21_00 12100110 with Insight >= 1)
        {9404, true},     // set by 12100110 together with 12100105
        {9180, false},    // no scripted cutscene pending
    };
    return flags;
}

const std::vector<StartWeapon>& PrologueSkipWeapons() {
    static const std::vector<StartWeapon> weapons = {
        {7000000, 72101000, "right"},
        {14000000, 72101011, "left"},
    };
    return weapons;
}

} // namespace coop

#ifndef BB_TEST_NEWGAME_NO_GAME

#include "game_state.h"
#include "party_start.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>

extern "C" void runtime_pad_director_press(std::uint32_t buttons); // src/runtime_pad.c
extern "C" int runtime_ime_dialog_count(void);                     // src/runtime_services.c

namespace {
std::atomic<int> g_movies{0};
}

// shadps4 avplayer: sceAvPlayerAddSource(Ex).
extern "C" void bbport_test_note_movie(void) {
    g_movies.fetch_add(1, std::memory_order_relaxed);
}

namespace coop {
namespace {

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[768];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    std::printf("Party: %s\n", line);
    std::fflush(stdout);
}

bool EnvOn(const char* name) {
    const char* v = std::getenv(name);
    return v && v[0] && v[0] != '0';
}

enum class SkipPhase { Wait, Apply, Warp, WaitWarp, Done };

struct T {
    bool init = false, newgame = false, skip = false;
    NewGameDriver drv;
    bool drv_on = false; // the driver runs (title on BB_TEST_NEWGAME; the opening only on skip)
    std::uint32_t held = 0;
    std::chrono::steady_clock::time_point t0{};
    double world_at = -1, flags_at = -1, phase_at = 0;
    StartFlags cs{};
    SkipPhase sp = SkipPhase::Wait;
    std::uint32_t warp = 0;
    bool loading_seen = false;
};

T& G() {
    static T t;
    return t;
}

void Init(T& g) {
    g.init = true;
    g.t0 = std::chrono::steady_clock::now();
    g.newgame = EnvOn("BB_TEST_NEWGAME");
    g.skip = EnvOn("BB_TEST_PROLOGUE_SKIP");
    g.warp = ParsePrologueWarp(std::getenv("BB_TEST_PROLOGUE_WARP"));
    g.drv_on = g.newgame || g.skip;
    if (g.newgame || g.skip) {
        const char* name = std::getenv("BB_TEST_NAME");
        Log("TEST AID: %s%s%s (name '%s'%s); test saves only", g.newgame ? "BB_TEST_NEWGAME (scripted new game)" : "",
            g.newgame && g.skip ? " + " : "", g.skip ? "BB_TEST_PROLOGUE_SKIP (prologue fast-forward)" : "",
            name && name[0] ? name : "BB_IME_TEXT / Hunter", g.skip ? (", warp " + std::to_string(g.warp)).c_str() : "");
    }
}

void Press(T& g, std::uint32_t buttons) {
    if (buttons != g.held) {
        runtime_pad_director_press(buttons);
        g.held = buttons;
    }
}

void ApplySkip(T& g) {
    Log("prologue skip: before: %s", DescribeStart(g.cs, true).c_str());
    int ok = 0, n = 0;
    for (const FlagWrite& w : PrologueSkipFlags()) {
        ++n;
        ok += WriteEventFlag(w.flag, w.on) ? 1 : 0;
    }
    Log("prologue skip: %d of %d flags written", ok, n);
    for (const StartWeapon& w : PrologueSkipWeapons()) {
        const int before = ItemCount(kItemWeapon, w.weapon);
        if (before == 0) {
            const bool given = GiveItem(kItemWeapon, w.weapon, 1);
            Log("prologue skip: %s-hand weapon %u: %s (now x%d)", w.hand, w.weapon, given ? "given" : "NOT given",
                ItemCount(kItemWeapon, w.weapon));
        } else {
            Log("prologue skip: %s-hand weapon %u: %s", w.hand, w.weapon,
                before > 0 ? "already owned" : "inventory unreadable; not given");
        }
    }
    g.cs = ReadStartFlags();
    Log("prologue skip: after: %s", DescribeStart(g.cs, true).c_str());
}

} // namespace

bool NewGameTestRequested() {
    return EnvOn("BB_TEST_NEWGAME") || EnvOn("BB_TEST_PROLOGUE_SKIP");
}

bool NewGameOwnsTitle() {
    T& g = G();
    return g.init && g.newgame && g.drv_on && !g.drv.finished();
}

void NewGameTestTick(const GameSnapshot& s, bool title_ready) {
    T& g = G();
    if (!g.init) {
        Init(g);
        if (g.skip && !g.newgame) {
            // Opening only: an existing clinic save; the driver skips the cutscene if it plays.
            std::string unused;
            NewGameInput in;
            in.world_up = true; // WaitTitle -> Opening at once
            g.drv.Step(in, &unused);
        }
    }
    if (!g.newgame && !g.skip) {
        return;
    }
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g.t0).count();
    const bool in_world = s.world_up && !s.loading;
    if (!in_world) {
        g.world_at = -1;
        if (!s.world_up) {
            g.cs = StartFlags{};
        }
    } else if (g.world_at < 0) {
        g.world_at = t;
    }
    if (in_world && t - g.flags_at >= 0.25) {
        g.flags_at = t;
        g.cs = ReadStartFlags();
    }
    // The title / creation / opening driver.
    if (g.drv_on && !g.drv.finished()) {
        NewGameInput in;
        in.t = t;
        in.title_ready = title_ready && !s.world_up && !s.loading;
        in.ime_count = runtime_ime_dialog_count();
        in.movie_count = g_movies.load(std::memory_order_relaxed);
        in.world_up = s.world_up;
        in.loading = s.loading;
        // A second in the world first: the opening cutscene's flags settle at the first frames.
        in.flags_ok = in_world && g.cs.flags_ok && t - g.world_at >= 1.0;
        in.opening_done = g.cs.opening_done;
        in.cutscene = g.cs.cutscene;
        std::string log;
        Press(g, g.drv.Step(in, &log));
        if (!log.empty()) {
            Log("newgame: %s", log.c_str());
        }
        if (g.drv.finished()) {
            Press(g, 0);
            Log("newgame: %s; %s", g.drv.phase() == NewGamePhase::Done ? "finished" : "GAVE UP",
                DescribeStart(g.cs, in_world).c_str());
        }
    }
    if (!g.skip) {
        return;
    }
    switch (g.sp) {
    case SkipPhase::Wait:
        if (!in_world || s.session_role != RoleIdle || t - g.world_at < 3.0 || !g.cs.flags_ok ||
            !g.cs.opening_done || g.cs.cutscene) {
            break;
        }
        if (g.cs.first_dream && g.cs.weapon_right && g.cs.weapon_left) {
            Log("prologue skip: already past the prologue (%s); nothing to do", DescribeStart(g.cs, true).c_str());
            g.sp = SkipPhase::Done;
            break;
        }
        g.sp = SkipPhase::Apply;
        break;
    case SkipPhase::Apply:
        ApplySkip(g);
        g.phase_at = t;
        g.sp = g.warp ? SkipPhase::Warp : SkipPhase::Done;
        break;
    case SkipPhase::Warp:
        if (t - g.phase_at < 1.0 || !in_world) {
            break;
        }
        {
            const bool ok = LampWarp(g.warp);
            Log("prologue skip: lamp warp to %u: %s (%s)", g.warp, ok ? "called" : "NOT called (code differs / no player)",
                Describe(s).c_str());
            g.phase_at = t;
            g.loading_seen = false;
            g.sp = ok ? SkipPhase::WaitWarp : SkipPhase::Done;
        }
        break;
    case SkipPhase::WaitWarp:
        if (s.loading || !s.world_up) {
            g.loading_seen = true;
        } else if (g.loading_seen && in_world && t - g.world_at >= 2.0) {
            Log("prologue skip: done; after the warp: %s; %s", Describe(s).c_str(), DescribeStart(g.cs, true).c_str());
            g.sp = SkipPhase::Done;
        } else if (!g.loading_seen && t - g.phase_at >= 20.0) {
            Log("prologue skip: done, but no loading screen 20 s after the warp (%s)", Describe(s).c_str());
            g.sp = SkipPhase::Done;
        }
        break;
    case SkipPhase::Done:
        break;
    }
}

} // namespace coop

#endif // BB_TEST_NEWGAME_NO_GAME

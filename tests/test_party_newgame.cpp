// SPDX-License-Identifier: GPL-3.0-or-later
// Test aids (gpu/shim/party/test_newgame): the scripted new game driver against a model of the
// title / character creation menus (as observed in 1.09: Brightness 2 rows, Controls 6 rows +
// Next, the Paleblood movie, Contract 5 rows + Finish, all wrapping; "Finalise contract?" with
// NO preselected), BB_TEST_PROLOGUE_WARP parsing and the fast-forward's flag / weapon lists.
// Build and run: ninja -C out/gpu party-newgame-test && out/gpu/party-newgame-test.exe
#include "party/test_newgame.h"

#include <cstdio>
#include <cstdlib>
#include <string>

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

namespace {

enum class Screen { Logos, Title, Fade, Brightness, Controls, Movie, Contract, Dialog, Loading, Opening, Clinic, Loaded };

struct Model {
    Screen screen = Screen::Logos;
    int row = 0;
    bool yes = false;
    int ime = 0, movies = 0;
    bool has_character = false; // title default = Continue
    double continue_fade = 0;   // Continue: the title fades this long before the loading screen
    bool ignore_next_cross = false;
    int drop_press = -1; // the game misses this press (0-based count of all presses)
    int presses = 0;
    double until = 0; // timed screens
    std::uint32_t last = 0;
    // A new screen builds its texts 0.2 s after the press (screen_seq +1) and takes input from
    // 0.45 s on: a press before that is lost (the event-driven driver must wait for it).
    double busy_until = 0, texts_at = -1, last_texts = -10;
    std::uint64_t seq = 0;
    bool texts = true; // false: no screen signal (the game menu hooks off)
    double loading_at = -1;
    double fade_until = 0;
    int presses_after_continue = 0;

    void Go(Screen s, double t, bool with_texts = true) {
        screen = s;
        row = 0;
        busy_until = t + 0.45;
        if (with_texts) texts_at = t + 0.2;
    }

    void Press(std::uint32_t b, double t) {
        if (screen == Screen::Fade || (has_character && screen == Screen::Loading)) ++presses_after_continue;
        if (t < busy_until) return; // still building
        auto wrap = [&](int n, int d) { row = (row + d + n) % n; };
        switch (screen) {
        case Screen::Title:
            if (b == kPadCross) {
                if (has_character) {
                    screen = continue_fade > 0 ? Screen::Fade : Screen::Loading;
                    until = t + continue_fade + 5;
                    fade_until = t + continue_fade;
                } else {
                    Go(Screen::Brightness, t);
                }
            }
            break;
        case Screen::Brightness:
            if (b == kPadDown) wrap(2, 1);
            if (b == kPadUp) wrap(2, -1);
            if (b == kPadCross && row == 1) Go(Screen::Controls, t);
            break;
        case Screen::Controls:
            if (b == kPadDown) wrap(7, 1);
            if (b == kPadUp) wrap(7, -1);
            if (b == kPadCross && row == 6) {
                if (ignore_next_cross) {
                    ignore_next_cross = false;
                } else {
                    Go(Screen::Movie, t, false);
                    ++movies;
                    until = t + 60;
                }
            }
            break;
        case Screen::Movie:
            if (b == kPadOptions) Go(Screen::Contract, t);
            break;
        case Screen::Contract:
            if (b == kPadDown) wrap(6, 1);
            if (b == kPadUp) wrap(6, -1);
            if (b == kPadCross && row == 0) ++ime;
            if (b == kPadCross && row == 5) {
                Go(Screen::Dialog, t);
                yes = false;
            }
            break;
        case Screen::Dialog:
            if (b == kPadLeft) yes = true;
            if (b == kPadRight) yes = false;
            if (b == kPadCross) {
                if (yes) {
                    screen = Screen::Loading;
                    until = t + 8;
                    loading_at = t;
                } else {
                    const int keep = 5;
                    Go(Screen::Contract, t);
                    row = keep; // back on Finish
                }
            }
            break;
        case Screen::Opening:
            if (b == kPadOptions) screen = Screen::Clinic;
            break;
        default:
            break;
        }
    }

    NewGameInput Observe(double t, bool title_ready) {
        if (screen == Screen::Logos && t >= 5) Go(Screen::Title, t);
        if (screen == Screen::Movie && t >= until) Go(Screen::Contract, t);
        if (texts_at >= 0 && t >= texts_at) {
            if (texts) {
                ++seq;
                last_texts = texts_at;
            }
            texts_at = -1;
        }
        if (screen == Screen::Fade && t >= fade_until) screen = Screen::Loading;
        if (screen == Screen::Loading && t >= until) screen = has_character ? Screen::Loaded : Screen::Opening;
        NewGameInput in;
        in.t = t;
        in.title_ready = title_ready && screen != Screen::Logos && screen != Screen::Loading;
        in.ime_count = ime;
        in.movie_count = movies;
        const bool world = screen == Screen::Opening || screen == Screen::Clinic || screen == Screen::Loaded;
        in.world_up = world || (screen == Screen::Loading && t >= until - 3);
        in.loading = screen == Screen::Loading;
        in.flags_ok = world;
        in.opening_done = screen == Screen::Clinic || screen == Screen::Loaded;
        in.cutscene = screen == Screen::Opening;
        in.screen_seq = seq;
        in.screen_quiet = seq ? t - last_texts : 1e9;
        return in;
    }

    // Edge-triggered: a button counts once when it goes down.
    void Feed(std::uint32_t held, double t) {
        const std::uint32_t down = held & ~last;
        last = held;
        for (std::uint32_t bit = 1; bit; bit <<= 1) {
            if (down & bit) {
                if (presses++ != drop_press) Press(bit, t);
            }
        }
    }
};

/// Runs the driver against the model at 30 Hz for up to `secs`; returns the final phase.
NewGamePhase Run(Model& m, double secs, bool title_ready = true, int* taps = nullptr) {
    NewGameDriver d;
    std::string log;
    int n = 0;
    std::uint32_t prev = 0;
    for (double t = 0; t < secs && !d.finished(); t += 1.0 / 30) {
        const NewGameInput in = m.Observe(t, title_ready);
        const std::uint32_t held = d.Step(in, &log);
        if (std::getenv("NEWGAME_TEST_VERBOSE") && (!log.empty() || (held && !prev)))
            std::printf("  %6.2f s %s%s\n", t, held && !prev ? ("press " + std::to_string(held) + " ").c_str() : "",
                        log.c_str());
        if (held && !prev) ++n;
        prev = held;
        m.Feed(held, t);
    }
    if (taps) *taps = n;
    return d.phase();
}

void TestHappyPath() {
    Model m;
    int taps = 0;
    CHECK(Run(m, 300, true, &taps) == NewGamePhase::Done);
    CHECK(m.screen == Screen::Clinic);
    CHECK(m.ime == 1);
    CHECK(m.movies == 1);
    CHECK(taps >= 10 && taps <= 14);
    // Event driven: from the title (5 s) to the contract's loading screen in a few seconds, every
    // press as soon as its screen took input (the fixed waits took ~30 s).
    std::printf("happy path: title at 5.0 s, loading at %.1f s, %d taps\n", m.loading_at, taps);
    CHECK(m.loading_at > 5 && m.loading_at < 14);
}

void TestNoScreenSignal() {
    // The game menu hooks off: no screen_seq; each step waits its maximum, still to the clinic.
    Model m;
    m.texts = false;
    CHECK(Run(m, 300) == NewGamePhase::Done);
    CHECK(m.screen == Screen::Clinic && m.ime == 1);
    std::printf("no screen signal: loading at %.1f s\n", m.loading_at);
    CHECK(m.loading_at < 40);
}

void TestTitleNeverReady() {
    Model m;
    CHECK(Run(m, 120, false) == NewGamePhase::WaitTitle);
    CHECK(m.screen == Screen::Title);
}

void TestSaveWithCharacter() {
    Model m;
    m.has_character = true;
    CHECK(Run(m, 120) == NewGamePhase::Done);
    CHECK(m.screen == Screen::Loaded);
    CHECK(m.ime == 0);
}

// Continue fades the title out before its loading screen (seen live: a save with a character sat
// at the title / got the empty save's Brightness presses). Nothing more is pressed after Continue.
void TestSaveWithCharacterFade() {
    Model m;
    m.has_character = true;
    m.continue_fade = 2.5;
    CHECK(Run(m, 120) == NewGamePhase::Done);
    CHECK(m.screen == Screen::Loaded);
    CHECK(m.presses_after_continue == 0);
}

void TestControlsCrossIgnoredOnce() {
    Model m;
    m.ignore_next_cross = true;
    CHECK(Run(m, 300) == NewGamePhase::Done);
    CHECK(m.screen == Screen::Clinic);
    CHECK(m.movies == 1);
}

void TestMissedPresses() {
    // Every single press of the run missed once in turn (title Cross, Brightness Down / Cross,
    // Controls Up / Cross, ...): the checkpoints get the run to the clinic anyway, or it stops
    // with Failed - never a wrong contract.
    int done = 0;
    for (int k = 0; k < 14; ++k) {
        Model m;
        m.drop_press = k;
        const NewGamePhase p = Run(m, 400);
        CHECK(p == NewGamePhase::Done || p == NewGamePhase::Failed);
        if (p == NewGamePhase::Done) {
            ++done;
            CHECK(m.screen == Screen::Clinic);
        } else {
            std::fprintf(stderr, "note: with press %d missed the driver gave up\n", k);
        }
    }
    CHECK(done == 14);
}

void TestWarpParse() {
    CHECK(ParsePrologueWarp(nullptr) == 2412951);
    CHECK(ParsePrologueWarp("") == 2412951);
    CHECK(ParsePrologueWarp("0") == 0);
    CHECK(ParsePrologueWarp("off") == 0);
    CHECK(ParsePrologueWarp("none") == 0);
    CHECK(ParsePrologueWarp("sickroom") == 2412950);
    CHECK(ParsePrologueWarp("dream") == 2102950);
    CHECK(ParsePrologueWarp("yharnam") == 2412951);
    CHECK(ParsePrologueWarp("2402950") == 2402950);
    CHECK(ParsePrologueWarp("junk") == 2412951);
}

void TestSkipLists() {
    bool r = false, l = false, dream = false, c9180 = false;
    for (const FlagWrite& w : PrologueSkipFlags()) {
        if (w.flag == 12101020 && w.on) r = true;
        if (w.flag == 12101021 && w.on) l = true;
        if (w.flag == 9401 && w.on) dream = true;
        if (w.flag == 9180 && !w.on) c9180 = true;
    }
    CHECK(r && l && dream && c9180);
    CHECK(PrologueSkipWeapons().size() == 2);
    for (const StartWeapon& w : PrologueSkipWeapons()) {
        bool flagged = false;
        for (const FlagWrite& f : PrologueSkipFlags()) flagged = flagged || (f.flag == w.choice_flag && f.on);
        CHECK(flagged);
    }
}

} // namespace

int main() {
    TestHappyPath();
    TestNoScreenSignal();
    TestTitleNeverReady();
    TestSaveWithCharacter();
    TestSaveWithCharacterFade();
    TestControlsCrossIgnoredOnce();
    TestMissedPresses();
    TestWarpParse();
    TestSkipLists();
    std::printf("party-newgame-test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

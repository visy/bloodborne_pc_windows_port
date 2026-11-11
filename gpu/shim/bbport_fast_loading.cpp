// SPDX-License-Identifier: GPL-3.0-or-later
// Fast loading (bbport_fast_loading.h). Idea: droogie/bbhost BBHOST_FAST_LOADING (GPL-3.0+).
#include "bbport_fast_loading.h"

#include "bbport_load_policy.h"
#include "bbport_settings.h"
#include "party/coop_hooks.h"
#include "party/game_state.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace bbport::fastload {

namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using Clock = std::chrono::steady_clock;

constexpr u64 kNowLoadingRequested = 0x556286b;  // u8
constexpr u64 kNowLoadingHelper = 0x553e8b8;     // CSNowLoadingHelper*
constexpr u64 kSprjRemo = 0x5540058;             // SprjRemo* (+8 the remo manager)
constexpr u64 kSprjFlipper = 0x55404f8;          // SprjFlipper* (Update 0x2034770)
constexpr u64 kFlipperUpdate = 0x2034770, kFlipperCall = 0x2018e22;  // the main loop's call of it        // SprjFlipper::Update(flipper), once per game frame (game thread)
constexpr u64 kFlipperNoSleep = 0x273;           // u8: no wait this frame (cleared by Update); the draw tasks skip too
constexpr u64 kFlipperPresent = 0x10;            // u32: flip interval of this frame, 0 = no flip (Update sets it)
constexpr u64 kFlipperSkip = 0x270;              // u8: this frame was late (the next ones may skip drawing)
constexpr std::int64_t kDrawPeriodNs = 16'000'000;  // an unpaced load still draws a real frame this often

std::atomic<bool> g_active{false};
std::atomic<bool> g_hooked{false};
std::atomic<bool> g_installed{false};   // Install() ran (the present thread may start before it)
std::atomic<u64> g_flips{0};
std::atomic<u64> g_draws{0}, g_skips{0};
std::int64_t g_last_draw_ns = 0;   // game thread only
u32 g_present_mode = 1;            // the last nonzero flip interval Update left (game thread only)

std::int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

template <class T>
T Read(u64 address, T fallback) {
    T v{};
    return address && coop::SafeGet(address, &v) ? v : fallback;
}
u64 Slot(u64 off) {
    const u64 at = coop::Guest(off);
    return at ? Read<u64>(at, 0) : 0;
}

using FlipperUpdateFn = void(BB_COOP_SYSV*)(u64);

// ---- SprjFade (diagnostic BB_FAST_FADES_LOG=1: every fade object's floats, printed when one changes) ----
constexpr u64 kSprjFadePtr = 0x5540068;   // SprjFade* ; fade objects at +0x10 / +0x20 / +0x28 / +0x38 / +0x40
void FadeLog() {
    static const bool on = [] { const char* e = std::getenv("BB_FAST_FADES_LOG"); return e && *e && *e != '0'; }();
    if (!on) return;
    static std::string last;
    static int lines = 0;
    static bool vt_done = false;
    const u64 fade = Slot(kSprjFadePtr);
    if (!fade || lines > 8000) return;
    if (!vt_done) {
        vt_done = true;
        const u64 base = coop::Guest(0), vt = Read<u64>(fade, 0);
        std::printf("Fade log: SprjFade %llx vtable guest+0x%llx slots:", (unsigned long long)fade, (unsigned long long)(vt - base));
        for (int k = 0; k < 8; ++k) std::printf(" +0x%llx", (unsigned long long)(Read<u64>(vt + 8 * k, 0) - base));
        for (u64 slot : {u64(0x10), u64(0x20), u64(0x28), u64(0x38), u64(0x40)}) {
            const u64 obj = Read<u64>(fade + slot, 0);
            const u64 ovt = obj ? Read<u64>(obj, 0) : 0;
            std::printf(" | obj+0x%llx vt+0x%llx", (unsigned long long)slot, (unsigned long long)(ovt ? ovt - base : 0));
        }
        std::printf("\n");
    }
    std::string body;
    char buf[64];
    for (u64 slot : {u64(0x10), u64(0x20), u64(0x28), u64(0x38), u64(0x40)}) {
        const u64 obj = Read<u64>(fade + slot, 0);
        body += " [" + std::to_string(slot >> 3) + ":";
        if (obj) {
            for (int k = 0; k < 14; ++k) {
                std::snprintf(buf, sizeof buf, " %.3g", double(Read<float>(obj + 0xc + 4 * k, 0.f)));
                body += buf;
            }
        }
        body += "]";
    }
    if (body == last) return;
    last = body;
    ++lines;
    std::printf("Fade log: %.3f%s\n", double(NowNs() % 1000000000000LL) / 1e9, body.c_str());
}

// ---- faster fades (BB_FAST_FADES=0 off; bbport.ini fast_fades) ----
// A map load's fades (black out before the loading screen, black in after it) are SprjFade's object at +0x20 (and
// +0x38): floats at +0xc.. (current RGBA, target RGBA), and +0x38 the remaining time, counting down from its 0.5 s by the
// game's own step every frame while the alpha follows it (alpha = remaining / 0.5 fading in, 1 - remaining / 0.5 fading
// out: measured with BB_FAST_FADES_LOG, research/loadux081.md). At the start of each frame the countdown is taken down
// once more by the step the game made last frame, so the fade runs twice as fast at any frame rate (the step is the
// game's own, live-clock or fixed). Black fades only, never during a cutscene.
std::atomic<bool> g_remo{false};
struct FadeTrack {
    float prev = 0;
    std::int64_t start_ns = 0;
    bool boosted = false;
} g_fade_track[2];

void FadesFrame() {
    static const bool enabled = BbSettings::Get().fast_fades.load();
    static const bool log = [] { const char* e = std::getenv("BB_FAST_FADES_LOG"); return e && *e && *e != '0'; }();
    if (!enabled && !log) return;
    const u64 fade = Slot(kSprjFadePtr);
    if (!fade) return;
    const bool remo = g_remo.load(std::memory_order_relaxed);
    int i = 0;
    for (const u64 slot : {u64(0x20), u64(0x38)}) {
        FadeTrack& t = g_fade_track[i++];
        const u64 obj = Read<u64>(fade + slot, 0);
        if (!obj) continue;
        float st = Read<float>(obj + 0x38, 0.f);
        if (!(st >= 0.f && st < 100.f)) st = 0.f;
        const bool black = Read<float>(obj + 0xc, 1.f) == 0.f && Read<float>(obj + 0x10, 1.f) == 0.f &&
                           Read<float>(obj + 0x14, 1.f) == 0.f && Read<float>(obj + 0x1c, 1.f) == 0.f &&
                           Read<float>(obj + 0x20, 1.f) == 0.f && Read<float>(obj + 0x24, 1.f) == 0.f;
        if (enabled && !remo && black) {
            const float extra = bbport::loadpolicy::FadeExtra(t.prev, st, 2.0f, 0.25f);
            if (extra > 0.f) {
                st -= extra;
                coop::SafeWrite(obj + 0x38, &st, 4);
                t.boosted = true;
            }
        }
        if (log) {
            const std::int64_t now = NowNs();
            if (st > 0.f && t.prev <= 0.f) {
                t.start_ns = now;
                t.boosted = false;
            } else if (st <= 0.f && t.prev > 0.f && t.start_ns) {
                std::printf("Fast fades: fade at +0x%llx ran %.3f s (%s)\n", (unsigned long long)slot,
                            double(now - t.start_ns) / 1e9, t.boosted ? "faster" : "game's own speed");
                std::fflush(stdout);
            }
        }
        t.prev = st;
    }
}

// The game thread, once per frame. Unpaced loading (bbhost's no-sleep idea, kept animating): Update is told not to wait
// for the frame's time (+0x273), so the loaders step as fast as the machine allows; Update then marks the frame late and
// the draw tasks skip it while +0x273 stays set. A real frame is let through about every 16 ms: the flag is left clear
// (the draw tasks run, the frame is flipped) and the flip interval Update zeroed is put back, so the loading screen
// animates at ~60 FPS while the loader steps between those frames.
BB_COOP_SYSV void FlipperUpdateHook(u64 flipper) {
    FadeLog();
    FadesFrame();
    const bool fast = flipper && g_active.load(std::memory_order_relaxed);
    const u8 one = 1, zero = 0;
    if (fast) coop::SafeWrite(flipper + kFlipperNoSleep, &one, 1);
    reinterpret_cast<FlipperUpdateFn>(coop::Guest(kFlipperUpdate))(flipper);
    if (!flipper) return;
    u32 mode = 0;
    coop::SafeGet(flipper + kFlipperPresent, &mode);
    if (!fast) {
        if (mode) g_present_mode = mode;
        return;
    }
    const std::int64_t now = NowNs();
    if (bbport::loadpolicy::DrawDue(now, g_last_draw_ns, kDrawPeriodNs)) {
        g_last_draw_ns = now;
        if (!mode) coop::SafeWrite(flipper + kFlipperPresent, &g_present_mode, 4);
        coop::SafeWrite(flipper + kFlipperSkip, &zero, 1);
        coop::SafeWrite(flipper + kFlipperNoSleep, &zero, 1);
        g_draws.fetch_add(1, std::memory_order_relaxed);
    } else {
        coop::SafeWrite(flipper + kFlipperNoSleep, &one, 1);
        g_skips.fetch_add(1, std::memory_order_relaxed);
    }
}

struct Load {
    bool up = false;
    bool fast = false;     // decided at the load's start (A/B)
    bool eligible = false; // the preset allows it
    bool remo_seen = false;
    Clock::time_point start{};
    u64 flips0 = 0;
    int number = 0;        // loads seen so far
};

}  // namespace

void Install() {
    struct Done {
        ~Done() { g_installed.store(true, std::memory_order_release); }
    } done;
    const char* e = std::getenv("BB_FAST_LOADING");
    const char* log = std::getenv("BB_FAST_FADES_LOG");
    const bool loading_on = e && *e ? std::strcmp(e, "0") != 0 : BbSettings::Get().fast_loading.load();
    if (!loading_on && !BbSettings::Get().fast_fades.load() && !(log && *log && *log != '0')) return;
    // The call site in the main loop (0x2018e22), not the pacer's entry: the party code hooks that entry already.
    g_hooked = coop::HookCallSite(kFlipperCall, kFlipperUpdate, reinterpret_cast<const void*>(&FlipperUpdateHook),
                                  "fast loading: SprjFlipper update call");
    std::printf("Fast loading: frame hook %s\n", g_hooked ? "installed" : "NOT installed (the game's executable differs): off");
    std::fflush(stdout);
}

bool Active() { return g_active.load(std::memory_order_relaxed); }

void NoteFlip() { g_flips.fetch_add(1, std::memory_order_relaxed); }

bool Tick() {
    if (!coop::Image() || !g_installed.load(std::memory_order_acquire)) return false;
    // BB_FAST_LOADING (env) wins over bbport.ini fast_loading; unset env = the ini (default on).
    static const Mode mode = [] {
        if (!g_hooked.load()) return Mode{0, 2};   // the frame hook is what keeps the screen animating: no hook, no unpaced loading
        const char* e = std::getenv("BB_FAST_LOADING");
        return e && *e ? ParseMode(e) : Mode{BbSettings::Get().fast_loading.load() ? 1 : 0, 2};
    }();
    static const bool measured = MeasuredSteps(std::getenv("BB_FPS"), std::getenv("BB_FPS_PATCH"));
    static bool announced = false;
    static Load ld;
    if (!coop::Image()) return false;
    if (!announced) {
        announced = true;
        std::printf("Fast loading: %s (BB_FAST_LOADING=%s, frame-rate preset %s)\n",
                    !measured      ? "n/a for this frame-rate preset (fixed 1/30 s steps)"
                    : mode.mode == 0 ? "off"
                    : mode.mode == 2 ? "A/B measurement"
                                     : "on",
                    std::getenv("BB_FAST_LOADING") ? std::getenv("BB_FAST_LOADING") : "(unset: the ini, default on)",
                    std::getenv("BB_FPS") ? std::getenv("BB_FPS") : "(unset)");
        std::fflush(stdout);
    }
    const bool requested = Read<u8>(coop::Guest(kNowLoadingRequested), 0) != 0;
    const u64 helper = Slot(kNowLoadingHelper);
    const bool in_game = helper && (Read<u8>(helper + 0x50, 0) || Read<u8>(helper + 0x51, 0));
    const bool loading = requested && !in_game;
    bool remo = false;
    if (const u64 remo_obj = Slot(kSprjRemo)) {
        if (const u64 man = Read<u64>(remo_obj + 8, 0)) {
            remo = (Read<u32>(man + 0x168, 0) & 1) != 0 || Read<u64>(man + 0xf8, 0) != 0;
        }
    }
    const auto now = Clock::now();
    if (loading && !ld.up) {
        ld.up = true;
        ld.eligible = measured;
        ld.fast = mode.mode == 1 || (mode.mode == 2 && AbOn(ld.number, mode.period));
        ld.remo_seen = false;
        ld.start = now;
        ld.flips0 = g_flips.load(std::memory_order_relaxed);
    } else if (!loading && ld.up) {
        ld.up = false;
        const double s = std::chrono::duration<double>(now - ld.start).count();
        const u64 frames = g_flips.load(std::memory_order_relaxed) - ld.flips0;
        std::printf("Fast loading: load %d (%s): %.2f s, %llu frames (%.0f per s)%s\n", ld.number + 1,
                    !ld.eligible ? "n/a" : ld.fast ? "on" : "off", s, static_cast<unsigned long long>(frames),
                    s > 0 ? double(frames) / s : 0.0, ld.remo_seen ? ", a cutscene kept it paced" : "");
        std::fflush(stdout);
        ++ld.number;
    }
    if (loading && remo) ld.remo_seen = true;
    Inputs in;
    in.enabled = ld.fast;
    in.loading = loading;
    in.remo = remo;
    g_remo.store(remo, std::memory_order_relaxed);
    in.measured_steps = measured;
    const bool fast = Uncapped(in);
    g_active.store(fast, std::memory_order_relaxed);
    return fast;
}

}  // namespace bbport::fastload

// SPDX-License-Identifier: GPL-3.0-or-later
// Split travel's choice dialog (see party_split.h; docs/party/split_travel.md).
#include "party_split.h"

#include "coop_hooks.h"
#include "game_state.h"
#include "party_async_io.h"
#include "party_b2.h"
#include "party_director.h"
#include "party_guestsvc.h"
#include "party_travel.h"
#include "../bbport_game_menu_pages.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

extern "C" void runtime_pad_director_press(std::uint32_t buttons); // src/runtime_pad.c

namespace coop::split {

namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

constexpr u64 kSlotMenuMan = 0x5562878;    // +0x78 dialog kind up (0 none), +0xD08 generic dialog struct
constexpr u64 kMenuBusyByte = 0x553b1ba;    // ShowGenDialog 0x1335110 opens nothing while set
constexpr u64 kOpenDialog = 0x16bebf0;      // (dialog struct, kind)
constexpr int kRoleHost = 3, kRoleClient = 6;
constexpr u32 kPadCross = 0x4000, kPadLeft = 0x80, kPadRight = 0x20;

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    coop::asyncio::Print(std::string("Party split: ") + line + "\n");
}

double Now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

template <class T>
T Field(u64 base, u64 off, T fallback = T{}) {
    T v{};
    return base && SafeGet(base + off, &v) ? v : fallback;
}

u64 Slot(u64 off) {
    u64 v = 0;
    const u64 at = Guest(off);
    return at && SafeGet(at, &v) ? v : 0;
}

int SessionRole() {
    return Field<int>(Slot(0x5540290), 0x124, -1);
}

Mode CurrentMode() {
    static const Mode m = ModeFromEnv(std::getenv("BB_PARTY_TRAVEL_CHOICE"));
    return m;
}

enum class Ask { None, Queued, Open, Answered };
struct Pending {
    Ask state = Ask::None;
    u32 id = 0;
    bool host = false;
    bool mark = false;  // a Hunter's Mark (id = the player's own last lamp)
    double since = 0;
    u64 dialog = 0;
    int test_step = 0;
    double test_at = 0;
};
std::mutex g_mu;
Pending g_p;  // under g_mu (OnLampWarp: game thread; Tick: main thread - the same thread in practice)

/// Opens the game's YES / NO dialog with our question; the dialog struct, 0 when it cannot open.
u64 OpenQuestion() {
    const u64 mm = Slot(kSlotMenuMan);
    u8 busy = 0;
    if (!mm || Field<int>(mm, 0x78, -1) != 0 || !SafeGet(Guest(kMenuBusyByte), &busy) || busy) {
        return 0;
    }
    const u64 dlg = Field<u64>(mm, 0xd08);
    const u32 text = BbGameMenu::Pages::SplitTravelText(0);
    if (!dlg || !text) {
        return 0;
    }
    // The Lua OnYesNoDialog 0x132D2C0 sequence (button type 1 = YES / NO, dialog kind 1 -> 7).
    const u32 one = 1, two = 2, zero = 0, minus = 0xffffffffu;
    const u8 b1 = 1;
    SafeWrite(dlg + 0x00, &text, 4);
    SafeWrite(dlg + 0x04, &one, 4);
    SafeWrite(dlg + 0x08, &two, 4);
    SafeWrite(dlg + 0x14, &zero, 4);
    SafeWrite(dlg + 0x10, &b1, 1);
    SafeWrite(dlg + 0x18, &minus, 4);
    SafeWrite(dlg + 0x1c, &zero, 4);
    SafeWrite(mm + 0x254, &zero, 4);
    using Open = void(BB_COOP_SYSV*)(u64, u32);
    reinterpret_cast<Open>(Guest(kOpenDialog))(dlg, 7);
    SafeWrite(mm + 0x16c, &minus, 4);
    SafeWrite(mm + 0x170, &minus, 4);
    return dlg;
}

/// Runs the warp the chosen way (main thread).
void Carry(const Pending& p, bool alone, const char* how) {
    Log("lamp %u (%s): %s -> %s", p.id, p.host ? "host" : "guest", how, alone ? "travel alone" : "travel with the party");
    if (p.host) {
        if (alone) {
            b2::HostTravelAloneNext();
        }
        RunLampWarpDecided(p.id, alone);
        return;
    }
    if (alone) {
        TravelIntent t;
        const b2::AloneWhy why = p.mark ? b2::AloneWhy::HuntersMark
                                 : p.id == 2102950 ? b2::AloneWhy::Lamp : b2::AloneWhy::Headstone;
        if (b2::LampTravelIntent(p.id, &t) && b2::TravelAlone(t, why)) {
            return;
        }
        Log("lamp %u: travelling alone is not possible now; the party travels together", p.id);
    }
    // With the party: a guest's Hunter's Mark takes the party to the host's last lamp.
    const u32 id = p.mark ? GuestDeathRedirectLamp() : p.id;
    if (p.mark && (id == kTravelNone || id == 0)) {
        Log("Hunter's Mark: the host's last lamp is not known; nothing runs");
        return;
    }
    if (!GuestLampRequestNow(id)) {
        Log("lamp %u: no party request possible (guest lamps off); nothing runs", p.id);
    }
}

} // namespace

Mode ModeFromEnv(const char* v) {
    if (v && std::strcmp(v, "party") == 0) {
        return Mode::Party;
    }
    if (v && std::strcmp(v, "alone") == 0) {
        return Mode::Alone;
    }
    return Mode::Ask;
}

const char* ModeName(Mode m) {
    return m == Mode::Party ? "party" : m == Mode::Alone ? "alone" : "ask";
}

namespace {
bool Hold(std::uint32_t id, bool mark);
bool OnMark(std::uint32_t last_lamp) {
    return Hold(last_lamp, true);
}
} // namespace

void Init() {
    if (!b2::Enabled()) {
        return;
    }
    SetMarkInterceptor(&OnMark);
    Log("on: a lamp, headstone or Hunter's Mark in a party asks \"Travel with the party?\" (mode %s)",
        ModeName(CurrentMode()));
}

bool OnLampWarp(std::uint32_t id) {
    return Hold(id, false);
}

namespace {
bool Hold(std::uint32_t id, bool mark) {
    if (!b2::Enabled()) {
        return false;
    }
    const PartyRole role = PartyDirector::Get().Role();
    const int sr = SessionRole();
    const bool guest = role == PartyRole::Guest && sr == kRoleClient;
    const bool host = role == PartyRole::Host && sr == kRoleHost && b2::HostHasMembers();
    if (!guest && !host) {
        return false;  // nobody to travel with: the plain warp
    }
    if (CurrentMode() == Mode::Party && host) {
        return false;  // the host's warp is the party move (the funnel capture)
    }
    if (CurrentMode() == Mode::Party && guest) {
        return false;  // the guest's request (party_guestsvc), as before
    }
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_p.state != Ask::None) {
        Log("lamp %u: a choice is already pending (lamp %u); this warp is dropped", id, g_p.id);
        return true;
    }
    g_p = Pending{};
    g_p.state = Ask::Queued;
    g_p.id = id;
    g_p.host = host;
    g_p.mark = mark;
    g_p.since = Now();
    Log("%s %u (%s): held for the choice (mode %s)", mark ? "Hunter's Mark to lamp" : "lamp", id, host ? "host" : "guest",
        ModeName(CurrentMode()));
    return true;
}
} // namespace

void Tick(const GameSnapshot& s) {
    static double release_at = 0;
    if (release_at > 0 && Now() >= release_at) {
        runtime_pad_director_press(0);  // the test press is a tap
        release_at = 0;
    }
    Pending p;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        p = g_p;
    }
    if (p.state == Ask::None) {
        return;
    }
    const double now = Now();
    const auto finish = [&] {
        std::lock_guard<std::mutex> lk(g_mu);
        g_p = Pending{};
    };
    const int sr = SessionRole();
    if ((p.host && sr != kRoleHost) || (!p.host && sr != kRoleClient)) {
        // The session ended while asking: the plain warp, as without a party.
        Log("lamp %u: the session changed (role %d) before the choice; the plain warp runs", p.id, sr);
        finish();
        RunLampWarpDecided(p.id, false);
        return;
    }
    if (CurrentMode() == Mode::Alone) {
        finish();
        Carry(p, true, "BB_PARTY_TRAVEL_CHOICE=alone");
        return;
    }
    if (p.state == Ask::Queued) {
        if (!s.world_up || s.loading) {
            return;
        }
        const u64 dlg = OpenQuestion();
        if (!dlg) {
            if (now - p.since > 5.0) {
                finish();
                Carry(p, false, "no dialog could open in 5 s");
            }
            return;
        }
        std::lock_guard<std::mutex> lk(g_mu);
        g_p.state = Ask::Open;
        g_p.dialog = dlg;
        g_p.since = now;
        Log("lamp %u: asking \"Travel with the party?\" (dialog %#llx)", p.id, static_cast<unsigned long long>(dlg));
        return;
    }
    if (p.state == Ask::Open) {
        const int r = Field<int>(p.dialog, 0x18, -1);
        // Test answer (BB_PARTY_SPLIT_TEST=yes|no): the cursor first (Left = YES, Right = NO), then Cross.
        static const char* test = std::getenv("BB_PARTY_SPLIT_TEST");
        if (test && test[0] && r == -1 && now - p.since > 1.5 && now - p.test_at > 0.6 && p.test_step < 6) {
            const bool yes = std::strcmp(test, "yes") == 0;
            const u32 b = p.test_step % 2 == 0 ? (yes ? kPadLeft : kPadRight) : kPadCross;
            runtime_pad_director_press(b);
            release_at = now + 0.12;
            Log("test: dialog answer %s: press %#x (step %d)", test, b, p.test_step);
            std::lock_guard<std::mutex> lk(g_mu);
            g_p.test_step = p.test_step + 1;
            g_p.test_at = now;
        }
        if (r == -1) {
            if (now - p.since > 120.0) {
                finish();
                Log("lamp %u: no answer in 120 s; nothing runs", p.id);
            }
            return;
        }
        Log("lamp %u: dialog result %d (menu +0x78 %d, +0x16c %d, +0x170 %d)", p.id, r,
            Field<int>(Slot(kSlotMenuMan), 0x78, -1), Field<int>(Slot(kSlotMenuMan), 0x16c, -1),
            Field<int>(Slot(kSlotMenuMan), 0x170, -1));
        finish();
        if (r == 1) {
            Carry(p, false, "YES");
        } else if (r == 0) {
            Carry(p, true, "NO");
        } else {
            Log("lamp %u: the question was closed (result %d); no travel", p.id, r);
        }
    }
}

} // namespace coop::split

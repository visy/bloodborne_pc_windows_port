// SPDX-License-Identifier: GPL-3.0-or-later
// Guest services in the host's Hunter's Dream (see party_guestsvc.h; the RE is
// docs/party/guest_services.md).
//
// Offsets are ours (raw ELF VA of eboot.elf 1.09), each read from the disassembly:
//   0x203AD60 talk environment test-function switch (rdi result {value, type}, rsi = r15 talk
//     environment, rdx = r14 call {vfunc +0x10 id, +0x18 argument count});
//   0x203BA66 case 19 IsClientPlayer: argument count 1, then
//   0x203BA78 41 0f b6 47 25   movzx eax, byte [r15+0x25]
//   0x203BA7D e9 4d 34 00 00   jmp 0x203EECF   ([r12+8] = 2 int, [r12] = eax)
#include "party_guestsvc.h"

#include <cstdio>
#include <cstdlib>

namespace coop {

bool IsDreamMap(std::uint32_t map_id) {
    return map_id != 0xffffffffu && (map_id >> 24) == 21;
}

bool GuestServicesGate(const GuestServicesInput& in) {
    return in.enabled && in.world_up && !in.loading && in.session_role == kSessionRoleClient &&
           IsDreamMap(in.map_id);
}

std::string DescribeRecordChange(const PlayerRecord& a, const PlayerRecord& b) {
    std::string out;
    auto field = [&](const char* name, int x, int y) {
        if (x < 0 || y < 0 || x == y) {
            return;
        }
        char buf[96];
        std::snprintf(buf, sizeof buf, "%s%s %d -> %d", out.empty() ? "" : ", ", name, x, y);
        out += buf;
    };
    field("level", a.level, b.level);
    field("echoes", a.echoes, b.echoes);
    field("Insight", a.insight, b.insight);
    return out;
}

bool GuestServiceTalk(std::uint32_t talk) {
    switch (talk) {
    case 210304: // Doll: level up (MSB entity 2100700)
    case 210691: // Bath Messenger: shop, sell (2100211)
    case 210692: // workbench: upgrade, Blood Gems, repair, crush (2100201)
    case 210693: // storage (2100200)
    case 210694: // Bath Messenger shop (2100215)
    case 210695: // Bath Messenger shop (2100216)
    case 210696: // mirror: appearance (2100202)
    case 210697: // dress-up messenger (2100218)
    case 210698: // Insight messenger: Insight shop (2100219)
    case 210699: // Bath Messenger shop (2100203)
        return true;
    default:
        return false;
    }
}

bool GuestServiceTalkAllowed(std::uint32_t talk, const std::string& extra) {
    if (GuestServiceTalk(talk) || extra == "all") {
        return true;
    }
    std::size_t at = 0;
    while (at < extra.size()) {
        std::size_t end = extra.find(',', at);
        if (end == std::string::npos) {
            end = extra.size();
        }
        const std::string item = extra.substr(at, end - at);
        char* stop = nullptr;
        const unsigned long v = std::strtoul(item.c_str(), &stop, 10);
        if (!item.empty() && stop && !*stop && v == talk) {
            return true;
        }
        at = end + 1;
    }
    return false;
}

bool GuestServicesEnabledFromEnv(const char* v) {
    return !(v && v[0] == '0' && !v[1]);
}

} // namespace coop

#ifndef BB_PARTY_GUESTSVC_NO_GAME
#include "coop_hooks.h"
#include "game_state.h"

#include <atomic>
#include <chrono>
#include <cstdarg>

namespace coop {
namespace {

constexpr u64 kIsClientLoad = 0x203BA78;   // movzx eax, byte [r15+0x25]
constexpr u64 kIsClientJmp = 0x203BA7D;    // jmp 0x203EECF
constexpr u64 kTestResultInt = 0x203EECF;  // [r12+8] = 2; [r12] = eax

std::atomic<bool> g_gate{false};
std::atomic<std::uint32_t> g_overrides{0};
// Talk environments seen asking while the gate was open (discovery log, tick side).
constexpr int kEnvSlots = 32;
std::atomic<std::uint64_t> g_envs[kEnvSlots];

struct State {
    bool init = false, enabled = false, installed = false;
    std::string extra; // BB_PARTY_GUEST_SERVICES_TALK
    bool open = false;
    PlayerRecord rec;
    std::uint32_t overrides_logged = 0;
    double last_check = 0;
    std::uint64_t logged_envs[kEnvSlots] = {};
    // BB_PARTY_GUESTSVC_TEST (live tests only): "warp=x,y,z,yaw;echoes=N", once, 5 s after the gate opened.
    std::string test;
    bool test_done = false;
    double open_at = 0;
    double changed_at = 0; // the last record change (0: none pending)
} g;

void NoteEnv(std::uint64_t env) {
    for (auto& slot : g_envs) {
        std::uint64_t cur = slot.load(std::memory_order_relaxed);
        if (cur == env) {
            return;
        }
        if (cur == 0 && slot.compare_exchange_strong(cur, env)) {
            return;
        }
    }
}

// Main thread (talk scripts): the game's answer `value` of IsClientPlayer for talk environment `env`.
std::uint64_t BB_COOP_SYSV IsClientFilter(std::uint64_t env, std::uint64_t value) {
    if (!g_gate.load(std::memory_order_relaxed) || (value & 0xff) == 0) {
        return value;
    }
    NoteEnv(env);
    // The talk script id (t210304 -> 210304; the case-19 environment is valid here: the game
    // just read +0x25).
    const std::uint32_t talk = *reinterpret_cast<const std::uint32_t*>(env + 0x10);
    if (!GuestServiceTalkAllowed(talk, g.extra)) {
        return value;
    }
    g_overrides.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    std::printf("Party guest services: %s\n", line);
    std::fflush(stdout);
}

double Now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

PlayerRecord RecordOf(const GameSnapshot& s) {
    PlayerRecord r;
    r.level = s.level;
    r.echoes = s.echoes;
    r.insight = s.insight;
    return r;
}

// Discovery: which fields of a talk environment name its script (t210140 = 210140 ...).
void LogEnv(std::uint64_t env) {
    std::string found;
    auto scan = [&](std::uint64_t base, const char* path, std::size_t n) {
        for (std::size_t off = 0; off + 4 <= n; off += 4) {
            std::int32_t v = 0;
            if (!SafeGet(base + off, &v)) {
                return;
            }
            if (v >= 100000 && v <= 999999) {
                char buf[96];
                std::snprintf(buf, sizeof buf, " %s+0x%zx=%d", path, off, v);
                found += buf;
            }
        }
    };
    scan(env, "", 0x200);
    for (std::size_t off = 0; off < 0x100; off += 8) {
        std::uint64_t p = 0;
        if (!SafeGet(env + off, &p) || p < 0x10000 || (p & 7)) {
            continue;
        }
        char path[32];
        std::snprintf(path, sizeof path, "[+0x%zx]", off);
        scan(p, path, 0x100);
    }
    std::string hex;
    for (std::size_t off = 0; off < 0x80; off += 8) {
        std::uint64_t q = 0;
        SafeGet(env + off, &q);
        char buf[24];
        std::snprintf(buf, sizeof buf, " %016llx", static_cast<unsigned long long>(q));
        hex += buf;
    }
    std::uint32_t talk = 0;
    SafeGet(env + 0x10, &talk);
    Log("talk script t%u (environment %p) asks IsClientPlayer: %s; ids:%s; head:%s", talk, reinterpret_cast<void*>(env),
        GuestServiceTalkAllowed(talk, g.extra) ? "allowed" : "kept vanilla", found.empty() ? " none" : found.c_str(),
        hex.c_str());
}

// Test fixture: stand the guest somewhere (PlayerIns vfunc +0x5F8 warp 0x18CD600: rsi float4
// position, rdx float4 rotation (0, yaw, 0, 0), ecx 0; the convention of 0x13AEADD) and/or set its
// echoes, so a scripted run can open a menu and spend them. Never on by default.
void RunTestFixture(const GameSnapshot& s) {
    alignas(16) float pos[4] = {0, 0, 0, 1};
    alignas(16) float rot[4] = {0, 0, 0, 0};
    const std::size_t w = g.test.find("warp=");
    if (w != std::string::npos &&
        std::sscanf(g.test.c_str() + w + 5, "%f,%f,%f,%f", &pos[0], &pos[1], &pos[2], &rot[1]) == 4 && s.player) {
        std::uint64_t vtbl = 0, fn = 0;
        if (SafeGet(s.player, &vtbl) && SafeGet(vtbl + 0x5f8, &fn) && fn) {
            using WarpFn = void(BB_COOP_SYSV*)(std::uint64_t, const float*, const float*, std::uint64_t);
            reinterpret_cast<WarpFn>(fn)(s.player, pos, rot, 0);
            Log("test: warped the guest to (%.2f, %.2f, %.2f) yaw %.2f", pos[0], pos[1], pos[2], rot[1]);
        }
    }
    const std::size_t e = g.test.find("echoes=");
    if (e != std::string::npos && s.player_rec) {
        const std::int32_t v = std::atoi(g.test.c_str() + e + 7);
        if (SafeWrite(s.player_rec + 0x94, &v, 4)) {
            Log("test: guest echoes set to %d", v);
        }
    }
}

} // namespace

void GuestServicesInit() {
    if (g.init) {
        return;
    }
    g.init = true;
    const char* party = std::getenv("BB_PARTY");
    g.enabled = party && party[0] && GuestServicesEnabledFromEnv(std::getenv("BB_PARTY_GUEST_SERVICES"));
    if (!g.enabled) {
        if (party && party[0]) {
            Log("off (BB_PARTY_GUEST_SERVICES=0): guests cannot use the Doll, shops or workshop in the host's Dream");
        }
        return;
    }
    if (const char* t = std::getenv("BB_PARTY_GUESTSVC_TEST")) {
        g.test = t;
    }
    if (const char* e = std::getenv("BB_PARTY_GUEST_SERVICES_TALK")) {
        g.extra = e;
    }
    if (!Matches(kIsClientJmp, {0xe9, 0x4d, 0x34, 0x00, 0x00})) {
        Log("guest +0x%llx is not the expected jmp; not installed", static_cast<unsigned long long>(kIsClientJmp));
        g.enabled = false;
        return;
    }
    // rdi = r15 (the talk environment), rsi = eax (the game's answer).
    g.installed = HookValueFilter(kIsClientLoad, {0x41, 0x0f, 0xb6, 0x47, 0x25}, {0x4c, 0x89, 0xff, 0x89, 0xc6},
                                  &IsClientFilter, kTestResultInt, "talk IsClientPlayer (guest services)");
    Log("%s: a guest in the host's Hunter's Dream can use the Doll, the messengers' shops and the workshop",
        g.installed ? "on" : "not installed");
}

void GuestServicesTick(const GameSnapshot& s) {
    if (!g.installed) {
        return;
    }
    GuestServicesInput in;
    in.enabled = g.enabled;
    in.world_up = s.world_up;
    in.loading = s.loading;
    in.session_role = s.session_role;
    in.map_id = s.map_id;
    const bool open = GuestServicesGate(in);
    if (open != g.open) {
        g.open = open;
        g_gate.store(open, std::memory_order_relaxed);
        if (open) {
            g.open_at = Now();
            g.rec = RecordOf(s);
            Log("open: guest in the host's %s; level %d, echoes %d, Insight %d", MapName(s.map_id).c_str(), g.rec.level,
                g.rec.echoes, g.rec.insight);
        } else {
            Log("closed (role %s, map %s%s)", SessionRoleName(s.session_role), MapName(s.map_id).c_str(),
                s.loading ? ", loading" : "");
        }
    }
    const double now = Now();
    if (g.open && !g.test.empty() && !g.test_done && now - g.open_at >= 5.0) {
        g.test_done = true;
        RunTestFixture(s);
        g.rec = RecordOf(ReadGameState(false)); // the fixture's echoes are not a purchase
    }
    if (!g.open || now - g.last_check < 1.0) {
        return;
    }
    g.last_check = now;
    for (int i = 0; i < kEnvSlots; ++i) {
        const std::uint64_t env = g_envs[i].load(std::memory_order_relaxed);
        if (!env || g.logged_envs[i] == env) {
            continue;
        }
        g.logged_envs[i] = env;
        LogEnv(env);
    }
    const std::uint32_t n = g_overrides.load(std::memory_order_relaxed);
    if (n != g.overrides_logged) {
        Log("talk scripts asked IsClientPlayer %u times (answered 0)", n - g.overrides_logged);
        g.overrides_logged = n;
    }
    const PlayerRecord r = RecordOf(s);
    if (r != g.rec) {
        const std::string d = DescribeRecordChange(g.rec, r);
        if (!d.empty()) {
            Log("own character: %s", d.c_str());
            g.changed_at = now;
        }
        g.rec = r;
    } else if (g.changed_at > 0 && now - g.changed_at >= 15.0) {
        g.changed_at = 0;
        Log("own character steady for 15 s: level %d, echoes %d, Insight %d", r.level, r.echoes, r.insight);
    }
}

bool GuestServicesOpen() {
    return g.open;
}

} // namespace coop
#endif

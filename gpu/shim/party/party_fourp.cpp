// SPDX-License-Identifier: GPL-3.0-or-later
// Four-player parties (see party_fourp.h and docs/party/four_players.md). Every site was read in
// the 1.09 eboot (capstone over out/eboot.elf); each install compares the bytes first.
#include "party_fourp.h"

#include <cstdlib>
#include <cstring>

#ifndef BB_PARTY_FOURP_NO_GAME
#include "coop_hooks.h"
#include "game_state.h"
#include "party_runtime.h"
#include "seamless_rules.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <set>
#endif

namespace coop::fourp {

namespace {

bool EnvIsZero(const char* name) {
    const char* v = std::getenv(name);
    return v && v[0] == '0' && !v[1];
}

} // namespace

// ---- Pure rules ----

Config ConfigFromEnv() {
    Config c;
    const char* party = std::getenv("BB_PARTY");
    c.on = party && party[0] && !EnvIsZero("BB_PARTY_FOURP");
    c.scaling = !EnvIsZero("BB_PARTY_FOURP_SCALING");
    c.npc_signs = !EnvIsZero("BB_PARTY_FOURP_NPC_SIGNS");
    c.recruit = !EnvIsZero("BB_PARTY_FOURP_RECRUIT");
    int m = 3;
    if (const char* v = std::getenv("BB_PARTY_MAX"); v && *v) {
        m = std::atoi(v);
    }
    c.local_max = m < 2 ? 2 : m > 4 ? 4 : m;
    return c;
}

std::string RulesTag(const Config& c) {
    if (!c.on) {
        return "4p:off";
    }
    std::string t = "4p:v1:H1";
    if (c.scaling) {
        t += ",H2,H3,H4";
    }
    if (c.npc_signs) {
        t += ",E6";
    }
    return t;
}

std::string FourpRulesTag() {
    return RulesTag(ConfigFromEnv());
}

int EffectiveMaxPlayers(int local_max, bool guest, bool connected, int host_max) {
    int m = guest && connected && host_max > 0 ? host_max : local_max;
    return m < 2 ? 2 : m > 4 ? 4 : m;
}

bool LiftCoopSlot(int rec0_status, int coop_count, int max_players) {
    // Status 2 = used; 3 (unavailable: no host, blocked area) and 0 (free) stay as they are.
    return max_players >= 4 && rec0_status == 2 && coop_count >= 0 && coop_count < max_players - 1;
}

int BossCount(int count) {
    return count > 2 ? 2 : count;
}

int DopingSpEffect(int sp_effect, int coop_count) {
    return sp_effect == 7501 && coop_count >= 3 ? 7502 : sp_effect;
}

bool IsNpcSignEvent(int event_id) {
    if (event_id == 12906962) {
        return true;
    }
    if (event_id < 10000000) {
        return false;
    }
    const int low = event_id % 10000;
    return low >= 4400 && low <= 4406;
}

int NpcSignCount(const std::uint8_t a[4], int max_players) {
    // [group][client type 0 = cooperators][comparison 3 = "<"][count]
    if (a[1] != 0 || a[2] != 3) {
        return -1;
    }
    if (max_players >= 4 && a[3] == 2) {
        return 3;
    }
    if (max_players < 4 && a[3] == 3) {
        return 2; // only for bytes we rewrote ourselves (the caller checks)
    }
    return -1;
}

#ifndef BB_PARTY_FOURP_NO_GAME

// ---- Game side ----

namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i32 = std::int32_t;
using ull = unsigned long long;
using Clock = std::chrono::steady_clock;

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    std::printf("Party 4p: %s\n", line);
    std::fflush(stdout);
}

// Our offsets (1.09).
constexpr u64 kSlotSummary = 0x186fe40;    // SessionSlotSummary(out, selector, debug)
constexpr u64 kCountKind = 0x18796c0;      // selector members of a session kind
constexpr u64 kBossCountCall = 0x17bf187;  // EMEVD 1003[109]: call 0x15bdc20
constexpr u64 kCooperatorCount = 0x15bdc20;
constexpr u64 kMultiDopingJne = 0x138bcbf; // cmp eax,2; jne
constexpr u64 kApplySpEffect = 0x18c6db0;  // chr vfunc +0x3f0
constexpr u64 kRecruitNum = 0x1e97770;     // mov ecx,2
constexpr u64 kNetFlow = 0x5556678;        // +0x16f8 the session slot table
constexpr u64 kWorldChrMan = 0x553e878;
constexpr u64 kSessionMan = 0x5540290;

Config g_cfg;
std::atomic<int> g_max{3};
bool g_init = false;
bool g_h1 = false, g_h2 = false, g_h3 = false, g_h4 = false, g_e6 = false, g_p5 = false;

std::atomic<u64> g_h1_lifts{0}, g_h1_calls{0}, g_h2_clamps{0}, g_h4_upgrades{0}, g_e6_rewrites{0},
    g_e6_reverts{0};

using SummaryFn = u64(BB_COOP_SYSV*)(u64 out, u64 sel, u64 dbg);
using CountKindFn = i32(BB_COOP_SYSV*)(u64 sel, i32 kind);
using CountFn = i32(BB_COOP_SYSV*)(u64 slots);
SummaryFn g_summary_original = nullptr;

// H1
BB_COOP_SYSV u64 SlotSummary(u64 out, u64 sel, u64 dbg) {
    const u64 r = g_summary_original(out, sel, dbg);
    g_h1_calls.fetch_add(1, std::memory_order_relaxed);
    const int max = g_max.load(std::memory_order_relaxed);
    if (max < 4 || !sel || !out) {
        return r;
    }
    i32 rec0 = -1;
    if (!SafeGet(out + 4, &rec0) || rec0 != 2) {
        return r;
    }
    const auto count_kind = reinterpret_cast<CountKindFn>(Guest(kCountKind));
    int coop = 0;
    for (const i32 kind : {7, 1, 0x15, 0x16, 0x17}) {
        coop += count_kind(sel, kind);
    }
    if (LiftCoopSlot(rec0, coop, max)) {
        const i32 free_status = 0;
        if (SafeWrite(out + 4, &free_status, 4) && g_h1_lifts.fetch_add(1) == 0) {
            Log("H1: a 3rd cooperator slot opened (%d cooperators, max players %d)", coop, max);
        }
    }
    return r;
}

// H2
BB_COOP_SYSV i32 BossCooperatorCount(u64 slots) {
    const i32 n = reinterpret_cast<CountFn>(Guest(kCooperatorCount))(slots);
    const i32 m = BossCount(n);
    if (m != n && g_h2_clamps.fetch_add(1) == 0) {
        Log("H2: boss scaling sees %d cooperators as 2 (then H4: SpEffect 7501 -> 7502)", n);
    }
    return m;
}

// The game's cooperator count, guarded like game_state.cpp (0x15bdc20 asserts without WorldChrMan
// or SprjSessionManager); -1 when it cannot be asked.
int CooperatorsNow() {
    u64 wcm = 0, player = 0, sm = 0, flow = 0, slots = 0;
    if (!SafeGet(Guest(kWorldChrMan), &wcm) || !wcm || !SafeGet(wcm + 0x60, &player) || !player ||
        !SafeGet(Guest(kSessionMan), &sm) || !sm || !SafeGet(Guest(kNetFlow), &flow) || !flow ||
        !SafeGet(flow + 0x16f8, &slots) || !slots) {
        return -1;
    }
    return reinterpret_cast<CountFn>(Guest(kCooperatorCount))(slots);
}

// E6
std::mutex g_e6_mu;
std::set<u64> g_e6_written; // argument bytes we changed (EVD data may be shared)

void NpcSignRewriter(int event_id, int bank, int id, u64 event, u64 instr) {
    if (bank != 3 || id != 29 || !IsNpcSignEvent(event_id)) {
        return;
    }
    u32 size = 0;
    const u64 args = EmevdInstructionArgs(event, instr, &size);
    u8 a[4] = {};
    if (!args || size < 4 || !SafeRead(args, a, 4)) {
        return;
    }
    const int max = g_max.load(std::memory_order_relaxed);
    const int n = NpcSignCount(a, max);
    if (n < 0) {
        return;
    }
    std::lock_guard<std::mutex> lk(g_e6_mu);
    if (n == 2 && !g_e6_written.count(args)) {
        return; // a "< 3" we did not write: the game's own
    }
    const u8 b = u8(n);
    if (!SafeWrite(args + 3, &b, 1)) {
        return;
    }
    if (n == 3) {
        g_e6_written.insert(args);
        if (g_e6_rewrites.fetch_add(1) < 32) {
            Log("E6: NPC sign event %d: 3[29] cooperators < 2 -> < 3", event_id);
        }
    } else {
        g_e6_written.erase(args);
        if (g_e6_reverts.fetch_add(1) < 32) {
            Log("E6: NPC sign event %d: back to cooperators < 2 (max players %d)", event_id, max);
        }
    }
}

struct TickState {
    Clock::time_point last_log{};
    u64 seen_h1 = 0, seen_h2 = 0, seen_h4 = 0, seen_e6 = 0;
};
TickState g_tick;

} // namespace
} // namespace coop::fourp

// H4: SpEffect 7501 -> 7502 at the entry of 0x18c6db0. Arguments: rdi chr, esi SpEffect id,
// rdx, rcx, r8, r9, xmm0-4 and a stack argument - all kept; the stub saves the System V
// caller-saved registers (and xmm0-7) around the check, then jumps to the trampoline.
extern "C" {
__attribute__((used)) void* bb_fourp_spfx_original = nullptr;
__attribute__((used)) BB_COOP_SYSV int bb_fourp_spfx_upgrade(void) {
    using namespace coop::fourp;
    // No max-players gate: 3 cooperators exist only once H1 let them in, and a guest must scale
    // like its host whatever it was told.
    const int n = CooperatorsNow();
    if (DopingSpEffect(7501, n) != 7502) {
        return 0;
    }
    if (g_h4_upgrades.fetch_add(1) < 16) {
        Log("H4: boss/chalice scaling SpEffect 7501 -> 7502 (%d cooperators)", n);
    }
    return 1;
}
void bb_fourp_spfx_stub(void);
}

asm(R"(
    .text
    .p2align 4
    .globl bb_fourp_spfx_stub
bb_fourp_spfx_stub:
    cmpl $7501, %esi
    jne 1f
    pushq %rdi
    pushq %rsi
    pushq %rdx
    pushq %rcx
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    subq $0x88, %rsp
    movdqu %xmm0, 0x00(%rsp)
    movdqu %xmm1, 0x10(%rsp)
    movdqu %xmm2, 0x20(%rsp)
    movdqu %xmm3, 0x30(%rsp)
    movdqu %xmm4, 0x40(%rsp)
    movdqu %xmm5, 0x50(%rsp)
    movdqu %xmm6, 0x60(%rsp)
    movdqu %xmm7, 0x70(%rsp)
    call bb_fourp_spfx_upgrade
    movdqu 0x00(%rsp), %xmm0
    movdqu 0x10(%rsp), %xmm1
    movdqu 0x20(%rsp), %xmm2
    movdqu 0x30(%rsp), %xmm3
    movdqu 0x40(%rsp), %xmm4
    movdqu 0x50(%rsp), %xmm5
    movdqu 0x60(%rsp), %xmm6
    movdqu 0x70(%rsp), %xmm7
    addq $0x88, %rsp
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rcx
    popq %rdx
    popq %rsi
    popq %rdi
    testl %eax, %eax
    jz 1f
    movl $7502, %esi
1:
    jmpq *bb_fourp_spfx_original(%rip)
)");

namespace coop::fourp {

void FourpInit() {
    if (g_init) {
        return;
    }
    g_cfg = ConfigFromEnv();
    g_max.store(g_cfg.local_max);
    if (!g_cfg.on) {
        const char* p = std::getenv("BB_PARTY");
        Log("off (%s); rules tag %s", p && p[0] ? "BB_PARTY_FOURP=0" : "BB_PARTY not set", RulesTag(g_cfg).c_str());
        return;
    }
    if (!Image()) {
        Log("no image; off");
        return;
    }
    g_init = true;
    const std::initializer_list<u8> kPrologue13 = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41,
                                                   0x56, 0x41, 0x55, 0x41, 0x54, 0x53};
    void* original = nullptr;
    g_h1 = ReplacePrologue(kSlotSummary, kPrologue13, reinterpret_cast<const void*>(&SlotSummary), &original,
                           "4p H1 cooperator slots (SessionSlotSummary)");
    if (g_h1) {
        g_summary_original = reinterpret_cast<SummaryFn>(original);
    }
    if (g_cfg.scaling) {
        g_h2 = HookCallSite(kBossCountCall, kCooperatorCount, reinterpret_cast<const void*>(&BossCooperatorCount),
                            "4p H2 boss scaling count (EMEVD 1003[109])");
        // cmp eax,2; jne +0x32 -> cmp eax,2; jl +0x32
        g_h3 = PatchBytes(kMultiDopingJne, {0x83, 0xf8, 0x02, 0x75, 0x32}, {0x83, 0xf8, 0x02, 0x7c, 0x32},
                          "4p H3 MultiDoping count >= 2");
        void* sp_original = nullptr;
        g_h4 = ReplacePrologue(kApplySpEffect, kPrologue13, reinterpret_cast<const void*>(&bb_fourp_spfx_stub),
                               &sp_original, "4p H4 SpEffect 7501 -> 7502 (chr ApplySpEffect)");
        if (g_h4) {
            bb_fourp_spfx_original = sp_original;
        }
    }
    if (g_cfg.npc_signs) {
        SetEmevdRewriter(&NpcSignRewriter);
        g_e6 = EnsureEmevdFilter();
        if (!g_e6) {
            SetEmevdRewriter(nullptr);
        }
    }
    if (g_cfg.recruit && g_cfg.local_max >= 4) {
        g_p5 = PatchBytes(kRecruitNum, {0xb9, 0x02, 0x00, 0x00, 0x00}, {0xb9, 0x03, 0x00, 0x00, 0x00},
                          "4p P5 RecruitNum 2 -> 3");
    }
    Log("rules %s: H1 %s, H2 %s, H3 %s, H4 %s, E6 %s, P5 %s; max players %d (local BB_PARTY_MAX; a guest takes "
        "the host's)",
        RulesTag(g_cfg).c_str(), g_h1 ? "on" : "OFF", g_h2 ? "on" : "off", g_h3 ? "on" : "off",
        g_h4 ? "on" : "off", g_e6 ? "on" : "off", g_p5 ? "on" : "off", g_cfg.local_max);
    if (!g_h1 && g_cfg.local_max >= 4) {
        Log("H1 not installed: the game stays at 2 cooperators");
    }
}

int FourpEffectiveMax() {
    return g_max.load(std::memory_order_relaxed);
}

void FourpTick() {
    if (!g_init) {
        return;
    }
    // The host's max players on a connected guest (WELCOME); our own otherwise.
    bool guest = false, connected = false;
    int host_max = 0;
    if (party::PartyLink* link = party::runtime_link()) {
        guest = !link->is_host();
        connected = link->state() == party::LinkState::Connected;
        host_max = link->max_players();
    }
    const int m = EffectiveMaxPlayers(g_cfg.local_max, guest, connected, host_max);
    const int old = g_max.exchange(m);
    if (old != m) {
        Log("max players %d -> %d (%s)", old, m,
            guest && connected ? "the host's setting" : "local BB_PARTY_MAX");
    }
    const auto now = Clock::now();
    if (now - g_tick.last_log >= std::chrono::seconds(30)) {
        g_tick.last_log = now;
        const u64 h1 = g_h1_lifts.load(), h2 = g_h2_clamps.load(), h4 = g_h4_upgrades.load(),
                  e6 = g_e6_rewrites.load() + g_e6_reverts.load();
        if (h1 != g_tick.seen_h1 || h2 != g_tick.seen_h2 || h4 != g_tick.seen_h4 || e6 != g_tick.seen_e6) {
            Log("max players %d; H1 slot lifts %llu (of %llu summaries), H2 clamps %llu, H4 upgrades %llu, E6 "
                "rewrites %llu",
                m, ull(h1), ull(g_h1_calls.load()), ull(h2), ull(h4), ull(e6));
            g_tick.seen_h1 = h1;
            g_tick.seen_h2 = h2;
            g_tick.seen_h4 = h4;
            g_tick.seen_e6 = e6;
        }
    }
}

#endif // BB_PARTY_FOURP_NO_GAME

} // namespace coop::fourp

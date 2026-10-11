// SPDX-License-Identifier: GPL-3.0-or-later
// Party travel B2 prototype (see party_b2.h; the RE is docs/party/b2_seamless.md).
//
// Every site was disassembled from the 1.09 eboot (capstone + Ghidra) and is compared byte for
// byte by coop_hooks before anything is written. Offsets are ours (raw ELF VA). The game's code
// is System V: every call into it goes through a BB_COOP_SYSV pointer at Guest(off).
#include "party_b2.h"
#include "party_async_io.h"
#include "party_fourp.h"

#include "coop_hooks.h"
#include "game_state.h"
#include "party_phantom.h"
#include "party_progress.h"
#include "party_travel.h"
#include "party_ids.h"
#include "lua_events.h"
#include "seamless_rules.h"
#include "../../party_env.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace coop::b2 {
namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using ull = unsigned long long;

// ---- Globals (singleton slots) ----
constexpr u64 kSlotWorldChrMan = 0x553e878;  // +0x30 player set {int n; entries @+0x38 (0x38)}, +0x60 local
constexpr u64 kSlotFlow = 0x553d6d0;         // network flow: +0x84 state, +0x68 members, +0x108 timer
constexpr u64 kSlotMultiPlay = 0x5540230;    // CSMultiPlayMan: human tasks +0x10/+0x18, NPC +0x38/+0x40
constexpr u64 kSlotSession = 0x5540290;      // SprjSessionManager +0x124 role
constexpr u64 kSlotGameData = 0x553b130;     // GameDataMan +0x68 (packet 0x19 payload)
constexpr u64 kSessionTypeDesc = 0x553d750;   // + st * 0x80: +0x15 net chr type (party_npc_test)
constexpr u64 kSlotGsm = 0x5556678;          // WorldTransitionState: +0x16f4 presentation slot byte, +0x16f8 slots
constexpr u64 kSlotBullet = 0x553e8d0;       // SprjBulletManager
constexpr u64 kSlotNetSyncArg = 0x553b148;   // *(+0x20) third argument of 0x18F0DB0 / 0x18F0E80

// ---- Functions ----
constexpr u64 kBeginLeave = 0x1ed07a0;   // bool (SprjSessionManager*)
constexpr u64 kMultiPlayReset = 0x1e54830; // void (CSMultiPlayMan*)
constexpr u64 kDropPresentation = 0x15bed10; // void (slots*)
constexpr u64 kJoinStartLua = 0x1317780; // void (SprjLuaEventMan*)
constexpr u64 kIdEquals = 0xca1c70;      // bool (const PlayerId*, const PlayerId*)
constexpr u64 kSendPacket = 0x17894e0;   // int (flow, const PlayerId*, type, const void*, size)
constexpr u64 kSendFlags = 0x1786050;    // bool (unused, const PlayerId*) packet 0x10
constexpr u64 kSendWorld = 0x1786360;    // bool (unused, const PlayerId*) packet 0x11
constexpr u64 kSendBullets = 0x1a04190;  // void (SprjBulletManager*, const PlayerId*)
constexpr u64 kNetSyncJoin = 0x18f0db0;  // void (WorldChrMan+0xa40 obj, const PlayerId*, u32)
constexpr u64 kNetSyncData = 0x18f0e80;  // same signature, the packet-0xa queue
constexpr u64 kSendNpcSummons = 0x15bd0e0; // void (slots*, const PlayerId*)

// ---- Sites ----
constexpr u64 kDtorLeaveCall = 0x19471b1;   // e8 ea 95 58 00 -> 0x1ed07a0 (XML retarget: e8 ea 8e 58 00)
constexpr u64 kDtorResetCall = 0x1947207;   // e8 24 d6 50 00 -> 0x1e54830
constexpr u64 kUidLeaveCall = 0x193c657;    // e8 44 41 59 00 -> 0x1ed07a0
constexpr u64 kUidDropCall = 0x193c689;     // e8 82 26 c8 ff -> 0x15bed10
constexpr u64 kMatchCheckCall = 0x13809b7;  // e8 e4 fd b4 00 -> 0x1ed07a0
constexpr u64 kJoinStartCall = 0x178524f;   // e8 2c 25 b9 ff -> 0x1317780
constexpr u64 kFunnelLeaveJne = 0x13cdeae;  // 75 6c (cmp [GSM+0x1592],0 ; jne skip-leave)
constexpr u64 kHostDeathReturnCall = 0x18c4d7d; // e8 2e 00 59 00 -> 0x1e54db0 (ChrIns_OnDeath, role 3)
constexpr u64 kRequestReturn = 0x1e54db0;       // void (CSMultiPlayMan*, const PlayerId*)
constexpr u64 kHostDead1Call = 0x1381a93;       // HostDead_1's send-home (B1 SendHomeHook site)
constexpr u64 kTaskUpdate = 0x1e52fe0;          // CSMultiPlayerInsTask update (vfunc +0xd0)

// Task (CSMultiPlayerInsTask, 0x100 bytes, vtable 0x534F650) fields.
constexpr u64 kTaskStep = 0x50, kTaskNext = 0x54, kTaskPeer = 0xc8, kTaskMode = 0xe8, kTaskSlot = 0xec,
              kTaskFlags = 0xed;
constexpr int kStepCreate = 4, kStepUpdate = 6;

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    coop::asyncio::Print(std::string("Party B2: ") + line + "\n"); // never blocks the caller (party_async_io.h)
}

bool EnvOn(const char* name) {
    const char* v = std::getenv(name);
    return v && v[0] && !(v[0] == '0' && !v[1]);
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

int Role() {
    return Field<int>(Slot(kSlotSession), 0x124, -1);
}

bool SessionUp() {
    const int r = Role();
    return r == RoleHost || r == RoleClient;
}

std::atomic<bool> g_on{false};
std::atomic<bool> g_force{false};
std::atomic<bool> g_rejoin{false};  // a B2 re-join is in flight (K5 / K6 armed)
std::atomic<bool> g_k1_xml{false};  // the XML retarget already keeps the room at 0x19471B1
std::atomic<int> g_apart_k5{0};     // members in another area (split travel): K5 stands aside
std::atomic<double> g_split_change_at{-100.0};  // the last split chr removal / meet rewind (Now())

bool IdEquals(u64 a, u64 b) {
    using Fn = bool(BB_COOP_SYSV*)(u64, u64);
    return reinterpret_cast<Fn>(Guest(kIdEquals))(a, b);
}

// ---- Reads ----

struct TaskInfo {
    u64 task = 0;
    int step = -1, next = -1, mode = -1;
    u8 slot = 0, flags = 0;
};

std::vector<TaskInfo> Tasks(bool npc) {
    std::vector<TaskInfo> out;
    const u64 man = Slot(kSlotMultiPlay);
    const u64 begin = Field<u64>(man, npc ? 0x38 : 0x10), end = Field<u64>(man, npc ? 0x40 : 0x18);
    if (!begin || end < begin || end - begin > 64 * 8) {
        return out;
    }
    for (u64 p = begin; p < end; p += 8) {
        TaskInfo t;
        t.task = Field<u64>(p, 0);
        if (!t.task) {
            continue;
        }
        t.step = Field<int>(t.task, kTaskStep, -1);
        t.next = Field<int>(t.task, kTaskNext, -1);
        t.mode = npc ? -1 : Field<int>(t.task, kTaskMode, -1);
        t.slot = npc ? 0 : Field<u8>(t.task, kTaskSlot);
        t.flags = npc ? Field<u8>(t.task, 0x128) : Field<u8>(t.task, kTaskFlags);
        out.push_back(t);
    }
    return out;
}

/// The player ChrSet entry whose peer id (+0x3D8) equals `id` (main thread: calls the game).
u64 PlayerForPeer(u64 id) {
    const u64 wcm = Slot(kSlotWorldChrMan);
    const int n = Field<int>(wcm, 0x30, 0);
    const u64 entries = Field<u64>(wcm, 0x38);
    if (!entries || n <= 0 || n > 8) {
        return 0;
    }
    for (int i = 0; i < n; ++i) {
        const u64 chr = Field<u64>(entries + u64(i) * 0x38, 0);
        if (chr && IdEquals(chr + 0x3d8, id)) {
            return chr;
        }
    }
    return 0;
}

/// The flow's member list (+0x68: std::list of {PlayerId @+0x10, int slot @+0x30}): the PlayerId
/// addresses of the other session members.
std::vector<u64> Members() {
    std::vector<u64> out;
    const u64 flow = Slot(kSlotFlow);
    const u64 head = Field<u64>(flow, 0x68);
    if (!head) {
        return out;
    }
    u64 node = Field<u64>(head, 0);
    for (int guard = 0; node && node != head && guard < 8; ++guard) {
        out.push_back(node + 0x10);
        node = Field<u64>(node, 0);
    }
    return out;
}

bool ChrPos(u64 chr, float out[3]) {
    const u64 a = Field<u64>(Field<u64>(Field<u64>(Field<u64>(chr, 0x58), 8), 0x3b0), 0x68);
    return a && SafeRead(a + 0x1e0, out, 12);
}

std::string DescribeChr(u64 chr) {
    float p[3] = {0, 0, 0};
    char b[128];
    if (!chr || !ChrPos(chr, p)) {
        return "-";
    }
    // +0x78 chr type (0 own body, 1 white phantom, ...); vfunc +0x558 is what the cooperator count
    // 0x15BDC20 tests (counted when in {1, 5, 7, 19}).
    using TypeFn = u32(BB_COOP_SYSV*)(u64);
    const u64 vt = Field<u64>(chr, 0);
    const u64 fn = vt ? Field<u64>(vt, 0x558) : 0;
    const u32 pres = fn ? reinterpret_cast<TypeFn>(fn)(chr) : ~0u;
    std::snprintf(b, sizeof b, "stage %#x type %d/%d pos (%.1f, %.1f, %.1f) hp %d", Field<u32>(chr, 0x3f8),
                  Field<int>(chr, 0x78, -1), int(pres), p[0], p[1], p[2],
                  Field<int>(Field<u64>(Field<u64>(chr, 0x3b0), 0x20), 0xf8, -1));
    return b;
}

std::string DescribeTasks() {
    std::string s;
    char b[192];
    for (const TaskInfo& t : Tasks(false)) {
        std::string chrs;
        if (t.mode == 1 || t.mode == 2) {
            const u64 c = PlayerForPeer(t.task + kTaskPeer);
            char h[48];
            std::snprintf(h, sizeof h, " handle %08x at %#llx", c ? Field<u32>(c, 8, ~0u) : ~0u, ull(c));
            chrs = c ? " chr " + DescribeChr(c) + h : " NO-chr";
        }
        const char* chr = chrs.c_str();
        std::snprintf(b, sizeof b, " [human mode %d step %d next %d slot %u flags 0x%02x%s]", t.mode, t.step, t.next,
                      unsigned(t.slot), unsigned(t.flags), chr);
        s += b;
    }
    for (const TaskInfo& t : Tasks(true)) {
        std::snprintf(b, sizeof b, " [npc step %d next %d life 0x%02x]", t.step, t.next, unsigned(t.flags));
        s += b;
    }
    return s.empty() ? std::string(" none") : s;
}

/// The presentation slots (GSM +0x16F8 table: +0x14 used, +0x18 count, 5 entries of 0x14 bytes
/// from +0x1C: {chr handle, kind, state, a, b}) - what the cooperator count 0x15BDC20 reads (a
/// handle counts when it resolves to a live chr in WorldChrMan +0x850).
std::string DescribePresentation() {
    const u64 t = Field<u64>(Slot(kSlotGsm), 0x16f8);
    if (!t) {
        return "pres -";
    }
    char b[96];
    std::snprintf(b, sizeof b, "pres used %d count %d max %d:", Field<int>(t, 0x14, -1), Field<int>(t, 0x18, -1),
                  Field<int>(Slot(kSlotFlow), 0xc, -1));
    std::string s = b;
    for (int i = 0; i < 5; ++i) {
        const u64 e = t + 0x1c + 0x14 * u64(i);
        const u32 h = Field<u32>(e, 0, ~0u);
        if (h == ~0u) {
            continue;
        }
        std::snprintf(b, sizeof b, " [%d %08x k%d s%d]", i, h, Field<int>(e, 4), Field<int>(e, 8));
        s += b;
    }
    return s;
}

// ---- Hooks ----

// K1: the stage payload dtor's BeginLeaveSession.
BB_COOP_SYSV u64 DtorLeaveHook(u64 session) {
    if (g_on.load() && SessionUp()) {
        Log("K1: stage unload with a session (role %d): room kept", Role());
        return 0;
    }
    using Fn = u64(BB_COOP_SYSV*)(u64);
    return reinterpret_cast<Fn>(Guest(kBeginLeave))(session);
}

/// The NPC part of 0x1E54830: destroy and free every NPC task (vfunc +8, then the owning heap's
/// free, +0x70 - the reset's own sequence), empty the vector.
void DestroyNpcTasks(u64 man) {
    using HeapOf = u64(BB_COOP_SYSV*)(u64);
    using Dtor = void(BB_COOP_SYSV*)(u64);
    using Free = void(BB_COOP_SYSV*)(u64, u64);
    const u64 begin = Field<u64>(man, 0x38), end = Field<u64>(man, 0x40);
    if (!begin || end < begin) {
        return;
    }
    int n = 0;
    for (u64 p = begin; p < end; p += 8) {
        const u64 task = Field<u64>(p, 0);
        if (!task) {
            continue;
        }
        const u64 heap = reinterpret_cast<HeapOf>(Guest(0x207b720))(task);
        const u64 vt = Field<u64>(task, 0);
        reinterpret_cast<Dtor>(Field<u64>(vt, 8))(task);
        reinterpret_cast<Free>(Field<u64>(Field<u64>(heap, 0), 0x70))(heap, task);
        ++n;
    }
    SafeWrite(man + 0x40, &begin, 8);
    if (n) {
        Log("K2: %d NPC task(s) destroyed as the reset would", n);
    }
}

// K2: the stage payload dtor's CSMultiPlayMan reset.
BB_COOP_SYSV void ResetHook(u64 man) {
    if (g_on.load() && (SessionUp() || g_force.load())) {
        Log("K2: CSMultiPlayMan reset skipped (role %d); tasks:%s", Role(), DescribeTasks().c_str());
        DestroyNpcTasks(man);
        return;
    }
    using Fn = void(BB_COOP_SYSV*)(u64);
    reinterpret_cast<Fn>(Guest(kMultiPlayReset))(man);
}

std::atomic<double> g_k3_at{-100.0};  // the last K3 (Now()), for the hit watch

// K3: in-game stage update, stage-record UID changed (the host walked into another streamed block,
// e.g. Great Bridge m24_01 <-> Cathedral Ward m24_00): the host's BeginLeaveSession. The room is
// kept; the log names how far the farthest member chr is. A member left in a block this game then
// streams out is handled by the stale map hit guard (ChrPreStepHook below): before it, the host
// crashed ~0.8 s after such a change in the remote chr's physics pre-step (0x18CB16A..0x18CB17D),
// also when the vanilla leave ran at the change (docs/party/travel.md 7).
BB_COOP_SYSV u64 UidLeaveHook(u64 session) {
    g_k3_at = Now();
    if (g_on.load() && SessionUp()) {
        const u64 me = Field<u64>(Slot(kSlotWorldChrMan), 0x60);
        float mp[3] = {};
        const bool have_me = me && ChrPos(me, mp);
        float farthest = 0.0f;
        int members = 0;
        for (const TaskInfo& t : Tasks(false)) {
            if (t.mode != 1 && t.mode != 2) {
                continue;
            }
            const u64 c = PlayerForPeer(t.task + kTaskPeer);
            float p[3] = {};
            if (!c || !have_me || !ChrPos(c, p)) {
                continue;
            }
            ++members;
            const float dx = p[0] - mp[0], dy = p[1] - mp[1], dz = p[2] - mp[2];
            farthest = std::max(farthest, std::sqrt(dx * dx + dy * dy + dz * dz));
        }
        Log("K3: stage-record change with a session (stage %#x, %d member chr(s), farthest %.1f m): no leave",
            me ? Field<u32>(me, 0x3f8) : 0u, members, farthest);
        return 0;
    }
    using Fn = u64(BB_COOP_SYSV*)(u64);
    return reinterpret_cast<Fn>(Guest(kBeginLeave))(session);
}

// K4: the 0x15BED10 that follows it (every role).
BB_COOP_SYSV void UidDropHook(u64 slots) {
    if (g_on.load() && SessionUp()) {
        return;
    }
    using Fn = void(BB_COOP_SYSV*)(u64);
    reinterpret_cast<Fn>(Guest(kDropPresentation))(slots);
}

// K8: ChrIns_OnDeath (local player, role 3) asks every human task to return: the host's death
// would end the party. With B2 the host respawns and the members follow (its travel event).
BB_COOP_SYSV void HostDeathReturnHook(u64 man, u64 id) {
    if (g_on.load() && Role() == RoleHost) {
        Log("K8: host death: member task kept (no return request)");
        return;
    }
    using Fn = void(BB_COOP_SYSV*)(u64, u64);
    reinterpret_cast<Fn>(Guest(kRequestReturn))(man, id);
}

/// Guest: HostDead_1's send-home is dropped (the host's death travel brings the guest along).
bool SplitHostReachable();
bool SendHomeVeto(u64 site) {
    // Split travel: OnLeave_Limit (0x138A51C: the room-gone / irregular-leave path) sent a guest
    // home 5 s after the host travelled alone (live split4) although the room and the party link
    // were up. While the host is apart (or just left alone) and its link is fresh, it is dropped.
    if (g_on.load() && Role() == RoleClient && site == 0x138a51c && SplitHostReachable()) {
        Log("guest: send-home at +0x138a51c (OnLeave_Limit) dropped: split travel, the host is in another area and "
            "connected (session %d, flow %d, flow +0xF4 %.1f, session +0x2E0 %.1f +0x2F0 %.1f)",
            Role(), Field<int>(Slot(kSlotFlow), 0x84, -1), double(Field<float>(Slot(kSlotFlow), 0xf4)),
            double(Field<float>(Slot(kSlotSession), 0x2e0)), double(Field<float>(Slot(kSlotSession), 0x2f0)));
        return true;
    }
    if (g_on.load() && Role() == RoleClient &&
        (site == kHostDead1Call || site == 0x13856f4 || site == 0x13859df)) {
        Log("guest: send-home at +0x%llx dropped (B2: %s)", ull(site),
            site == kHostDead1Call ? "the host's respawn travel follows" : "the party stays after the boss");
        return true;
    }
    return false;
}

// K10: the boss-clear Lua states' BeginLeaveSession (0x13858F5 in BlockClear2_1 0x13858A0,
// 0x1385A95 in BlockClear2_3 0x13859F0): the party stays in the session after the boss. Both
// roles run these states after a kill (live run r3: the host left 12 s after the kill, which
// ended the guest's session although its own leave was dropped), so both are kept.
BB_COOP_SYSV u64 BossClearLeaveHook(u64 session) {
    const int role = Role();
    if (g_on.load() && (role == RoleClient || role == RoleHost)) {
        Log("K10: boss-clear leave suppressed (%s; party stays)", role == RoleHost ? "host" : "guest");
        return 0;
    }
    using Fn = u64(BB_COOP_SYSV*)(u64);
    return reinterpret_cast<Fn>(Guest(kBeginLeave))(session);
}

// ---- Remote player chrs on a streamed-out block (host and guest) ----
// A chr keeps the map hit (collision part) it stands on in +0x288 (set by 0x18BBA70; +0x350 a
// second one the respawn path 0x18BB670 re-applies). A map's own chrs die with their block and the
// local player's block never unloads under it, but a *remote* player chr standing in a block this
// game streams out (the other player far away, past the confinement walls the party opens) keeps
// a pointer into the freed hit array: the physics pre-step 0x18CB0E0 then reads
// [[hit+0x18]+0x88] and the game faults at 0x18CB16A / 0x18CB17D (live, edgetravel 2026-10-11).
// Before that pre-step, a remote player chr's hit that is no longer in the hit registry
// (*0x553E850: +0x20 block count, +0x28 block records of 0xB8: +0x78 hit count, +0x80 hits of
// 0x138 - the table 0x13C9FB0 resolves hits from) is cleared; only pointers are compared, the
// stale hit is never read. The chr then has no ground hit until it stands on loaded collision
// again (its position keeps coming from its owner).
constexpr u64 kSlotHitMan = 0x553e850;
std::atomic<u32> g_stale_hits{0};
bool HitLive(u64 hit) {
    const u64 man = *reinterpret_cast<const u64*>(Guest(kSlotHitMan));
    if (!man) {
        return false;
    }
    const int blocks = *reinterpret_cast<const int*>(man + 0x20);
    const u64 recs = *reinterpret_cast<const u64*>(man + 0x28);
    if (!recs || blocks <= 0 || blocks > 4096) {
        return false;
    }
    for (int i = 0; i < blocks; ++i) {
        const u64 rec = recs + u64(i) * 0xb8;
        const int n = *reinterpret_cast<const int*>(rec + 0x78);
        const u64 base = *reinterpret_cast<const u64*>(rec + 0x80);
        if (base && n > 0 && hit >= base && hit < base + u64(n) * 0x138) {
            return (hit - base) % 0x138 == 0;
        }
    }
    return false;
}
/// Diagnostics: the hit of a chr and where the registry holds it.
std::string DescribeHit(u64 chr) {
    const u64 hit = Field<u64>(chr, 0x288);
    char b[200];
    if (!hit) {
        return "hit 0";
    }
    const u64 man = Slot(kSlotHitMan);
    const int blocks = Field<int>(man, 0x20);
    const u64 recs = Field<u64>(man, 0x28);
    int at = -1;
    for (int i = 0; recs && i < blocks && i < 4096; ++i) {
        const u64 rec = recs + u64(i) * 0xb8;
        const int n = Field<int>(rec, 0x78);
        const u64 base = Field<u64>(rec, 0x80);
        if (base && n > 0 && hit >= base && hit < base + u64(n) * 0x138) {
            at = i;
        }
    }
    const u64 r18 = Field<u64>(hit, 0x18);
    std::snprintf(b, sizeof b, "hit %#llx block #%d/%d +0xc0 %02x +0x18 %#llx (+0x344 %#x) +0x20 %#llx",
                  ull(hit), at, blocks, unsigned(Field<u8>(hit, 0xc0)), ull(r18), Field<u32>(r18, 0x344),
                  ull(Field<u64>(hit, 0x20)));
    std::string out = b;
    if (at >= 0) {
        const u64 rec = recs + u64(at) * 0xb8;
        std::snprintf(b, sizeof b, " rec[+0..+0x20] %llx %llx %llx %llx %llx", ull(Field<u64>(rec, 0)),
                      ull(Field<u64>(rec, 8)), ull(Field<u64>(rec, 0x10)), ull(Field<u64>(rec, 0x18)),
                      ull(Field<u64>(rec, 0x20)));
        out += b;
    }
    return out;
}

bool RemotePlayerChr(u64 chr) {
    const u64 wcm = *reinterpret_cast<const u64*>(Guest(kSlotWorldChrMan));
    if (!wcm || chr == *reinterpret_cast<const u64*>(wcm + 0x60)) {
        return false;
    }
    const int n = *reinterpret_cast<const int*>(wcm + 0x30);
    const u64 entries = *reinterpret_cast<const u64*>(wcm + 0x38);
    if (!entries || n <= 0 || n > 8) {
        return false;
    }
    for (int i = 0; i < n; ++i) {
        if (*reinterpret_cast<const u64*>(entries + u64(i) * 0x38) == chr) {
            return true;
        }
    }
    return false;
}
BB_COOP_SYSV void ChrPreStepHook(u64 chr, u64, u64, u64, u64, u64) {  // rdi = the chr (mov rbx, rdi)
    if (!chr) {
        return;
    }
    u64* hit = reinterpret_cast<u64*>(chr + 0x288);
    u64* hit2 = reinterpret_cast<u64*>(chr + 0x350);
    if ((!*hit || HitLive(*hit)) && (!*hit2 || HitLive(*hit2))) {
        return;
    }
    if (!RemotePlayerChr(chr)) {
        return;
    }
    if (*hit && !HitLive(*hit)) {
        *hit = 0;
    }
    if (*hit2 && !HitLive(*hit2)) {
        *hit2 = 0;
    }
    g_stale_hits.fetch_add(1, std::memory_order_relaxed);
}

// BeginLeaveSession entry (ReplacePrologue: jumped to, so the return address is the caller's):
// logs which call site ends a B2 session; the call itself is unchanged.
void* g_leave_orig = nullptr;
__attribute__((noinline)) BB_COOP_SYSV u64 LeaveEntryHook(u64 session) {
    const u64 ret = reinterpret_cast<u64>(__builtin_return_address(0));
    const int role = Role();
    if (g_on.load() && (role == RoleClient || role == RoleHost)) {
        const u64 base = Guest(0);
        if (ret > base + 5 && ret - base < 0x6000000) {
            Log("leave: BeginLeaveSession called from +0x%llx (role %d)", ull(ret - base - 5), role);
        } else {
            Log("leave: BeginLeaveSession called through a party hook (role %d)", role);
        }
    }
    using Fn = u64(BB_COOP_SYSV*)(u64);
    return reinterpret_cast<Fn>(g_leave_orig)(session);
}

// K11: the packet-0x10 apply 0x13BECA0(SprjEventFlagMan, blob) at 0x17851CE. At a vanilla join
// the guest is still in its own world (load mode 0) when it runs; a B2 re-entry receives it in
// load mode 1 (the host-world overlay active), and the result read back all zero (runs 18-20).
// So for a B2 re-entry the store is put back to load mode 0 (SetLoadMode 0x13BB590) first, as at
// a join; the guest's MoveMapStep sets mode 1 again from the fresh overlay a moment later.
BB_COOP_SYSV u64 FlagApplyHook(u64 man, u64 blob) {
    using Apply = u64(BB_COOP_SYSV*)(u64, u64);
    using Mode = void(BB_COOP_SYSV*)(u64, u32);
    const int mode = Field<int>(man, 0x80, -1);
    if (g_on.load() && g_rejoin.load() && Role() == RoleClient && mode == 1) {
        reinterpret_cast<Mode>(Guest(0x13bb590))(man, 0);
        const u64 r = reinterpret_cast<Apply>(Guest(0x13beca0))(man, blob);
        Log("K11: host flag snapshot applied in load mode 0 (was 1) -> %llu", ull(r));
        coop::progress::OnVanillaSnapshotApplied();  // the host's whole world into the new overlay
        return r;
    }
    const u64 r = reinterpret_cast<Apply>(Guest(0x13beca0))(man, blob);
    coop::progress::OnVanillaSnapshotApplied();  // the vanilla snapshot has 12 groups: the mirror adds the rest
    return r;
}

// K5: Lua OnMatchingCheck: a member's chr is missing (it is loading).
BB_COOP_SYSV u64 MatchCheckHook(u64 session) {
    // Split travel: a member apart (no chr here, by design), or a chr removed / re-created in the
    // last 15 s (live split1: the leave came 50 ms after a meet's rewind, the chr not built yet).
    const bool split = g_apart_k5.load() > 0 || Now() - g_split_change_at.load() < 15.0;
    if (g_on.load() && (g_rejoin.load() || split)) {
        Log("K5: OnMatchingCheck leave suppressed (%s)", g_rejoin.load() ? "re-join in flight" : "split travel: members apart");
        return 0;
    }
    using Fn = u64(BB_COOP_SYSV*)(u64);
    return reinterpret_cast<Fn>(Guest(kBeginLeave))(session);
}

// K6: Lua OnBeJoinStart_* after the flag snapshot.
BB_COOP_SYSV void JoinStartHook(u64 lua) {
    if (g_on.load() && g_rejoin.load()) {
        Log("K6: OnBeJoinStart suppressed (B2 re-join drives the reload)");
        return;
    }
    using Fn = void(BB_COOP_SYSV*)(u64);
    reinterpret_cast<Fn>(Guest(kJoinStartLua))(lua);
}

// K7: scoped write of the funnel's "own world" branch (main thread, around our own call).
bool WriteCode(u64 off, const u8* bytes, std::size_t n) {
    unsigned char* at = Image() + off;
#ifdef _WIN32
    DWORD old = 0;
    if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old)) {
        return false;
    }
    std::memcpy(at, bytes, n);
    DWORD unused = 0;
    VirtualProtect(at, n, old, &unused);
    FlushInstructionCache(GetCurrentProcess(), at, n);
#else
    std::memcpy(at, bytes, n);
#endif
    return true;
}

// The guest's loading screen from the host's travel event on (one continuous screen): what the
// map-load request at 0x1929007..0x192902B writes inline into the NowLoading object
// (*0x5562878): +0x808 screen kind 5, request byte 0x556286B = 1, +0x17C0 = 0, +0x804 = 1 unless 2.
// The guest's own MoveMapStep clears it at InGameStart (0x176D3C0 from 0x193A755).
constexpr u64 kSlotNowLoading = 0x5562878, kNowLoadingByte = 0x556286b, kNowLoadingClear = 0x176d3c0;
bool g_early_screen = false;

void RaiseLoadingScreen() {
    const u64 nl = Slot(kSlotNowLoading);
    if (!nl) {
        return;
    }
    const int kind = 5, one = 1;
    const u8 req = 1;
    const float zero = 0.0f;
    SafeWrite(nl + 0x808, &kind, 4);
    SafeWrite(Guest(kNowLoadingByte), &req, 1);
    SafeWrite(nl + 0x17c0, &zero, 4);
    if (Field<int>(nl, 0x804) != 2) {
        SafeWrite(nl + 0x804, &one, 4);
    }
    g_early_screen = true;
    Log("guest: loading screen raised at the host's travel");
}

void DropLoadingScreen(const char* why) {
    if (!g_early_screen) {
        return;
    }
    g_early_screen = false;
    using Fn = u64(BB_COOP_SYSV*)();
    reinterpret_cast<Fn>(Guest(kNowLoadingClear))();
    Log("guest: early loading screen dropped (%s)", why);
}

struct FunnelKeepRoom {
    bool armed = false;
    FunnelKeepRoom() {
        const u8 jmp[2] = {0xeb, 0x6c};
        armed = Matches(kFunnelLeaveJne, {0x75, 0x6c}) && WriteCode(kFunnelLeaveJne, jmp, 2);
    }
    ~FunnelKeepRoom() {
        if (armed) {
            const u8 jne[2] = {0x75, 0x6c};
            WriteCode(kFunnelLeaveJne, jne, 2);
        }
    }
};

// ---- Driver ----

/// Rewinds every remote task (mode 1 = guest member, 2 = session owner) whose PlayerIns is gone
/// to STEP_Create with the create bit. Returns how many.
int RewindRemoteTasks(const char* why) {
    int n = 0;
    for (const TaskInfo& t : Tasks(false)) {
        if ((t.mode != 1 && t.mode != 2) || t.step < kStepCreate || (t.flags & 2)) {
            continue; // not remote, not created yet (the join itself is still running), or leaving
        }
        if (PlayerForPeer(t.task + kTaskPeer)) {
            continue;
        }
        const int next = kStepCreate;
        const u8 flags = u8(t.flags | 1);
        SafeWrite(t.task + kTaskFlags, &flags, 1);
        SafeWrite(t.task + kTaskNext, &next, 4);
        ++n;
        Log("%s: task mode %d (step %d) rewound to STEP_Create", why, t.mode, t.step);
    }
    return n;
}

/// Host: the join-time snapshot again, to one member (after the host's own load).
void LogIntroFlags(const char* when);

void ResyncMember(u64 id) {
    LogIntroFlags("host at the re-send");
    const u64 flow = Slot(kSlotFlow);
    using Send = int(BB_COOP_SYSV*)(u64, u64, u32, const void*, u32);
    using Snap = u64(BB_COOP_SYSV*)(u64, u64);
    using Two = void(BB_COOP_SYSV*)(u64, u64);
    using Three = void(BB_COOP_SYSV*)(u64, u64, u32);
    alignas(8) u8 p19[0x44] = {};
    const u32 gd68 = Field<u32>(Slot(kSlotGameData), 0x68);
    std::memcpy(p19, &gd68, 4);
    reinterpret_cast<Send>(Guest(kSendPacket))(flow, id, 0x19, p19, 0x44);
    const bool flags = reinterpret_cast<Snap>(Guest(kSendFlags))(0, id) & 1;
    const bool world = reinterpret_cast<Snap>(Guest(kSendWorld))(0, id) & 1;
    if (const u64 bullets = Slot(kSlotBullet)) {
        reinterpret_cast<Two>(Guest(kSendBullets))(bullets, id);
    }
    const u64 wcm = Slot(kSlotWorldChrMan);
    const u64 netsync = Field<u64>(wcm, 0xa40);
    const u64 arg = Slot(kSlotNetSyncArg);
    if (netsync && arg) {
        const u32 a = Field<u32>(arg, 0x20);
        reinterpret_cast<Three>(Guest(kNetSyncJoin))(netsync, id, a);
        reinterpret_cast<Three>(Guest(kNetSyncData))(netsync, id, a);
    }
    if (const u64 slots = Field<u64>(Slot(kSlotGsm), 0x16f8)) {
        reinterpret_cast<Two>(Guest(kSendNpcSummons))(slots, id);
    }
    const u8 pslot = Field<u8>(Slot(kSlotGsm), 0x16f4);
    reinterpret_cast<Send>(Guest(kSendPacket))(flow, id, 0x42, &pslot, 1);
    Log("host: join snapshot re-sent (0x19, flags %s, world %s, bullets, net sync, NPC, 0x42)",
        flags ? "ok" : "FAILED", world ? "ok" : "FAILED");
}

/// The chr behind a WorldChrMan handle ((set << 14) | index; table +0x850, entries 0x38), as the
/// game resolves it (0x15BDC20), with every read checked. 0 when stale.
u64 ChrOfHandle(u32 h) {
    const u64 wcm = Slot(kSlotWorldChrMan);
    if (!wcm || h == 0xffffffffu) {
        return 0;
    }
    const u32 set = h >> 14 & 0x3f, idx = h & 0x3fff;
    if (int(set) >= Field<int>(Field<u64>(wcm, 8), 0x18, -2) + 2) {
        return 0;
    }
    const u64 tab = Field<u64>(wcm, 0x850 + u64(set) * 8);
    if (!tab || int(idx) >= Field<int>(tab, 0, 0)) {
        return 0;
    }
    return Field<u64>(Field<u64>(tab, 8) + u64(idx) * 0x38, 0);
}

/// The presentation slot table (GSM +0x16F8): 5 entries at +0x1C (stride 0x14: chr handle, kind,
/// state, a, b), the counts +0x8/+0xC/+0x10/+0x14, the handle list +0x80..+0x8C (+0x90 count)
/// and the max (flow +0xC). The game's cooperator count (0x15BDC20) walks the entries.
std::string DescribePresence(bool deep = false) {
    const u64 t = Field<u64>(Slot(kSlotGsm), 0x16f8);
    if (!t) {
        return "none";
    }
    std::string out;
    char b[160];
    std::snprintf(b, sizeof b, "count %d (h %d n %d e %d) max %d; local %x;", Field<int>(t, 0x14, -1),
                  Field<int>(t, 0x8, -1), Field<int>(t, 0xc, -1), Field<int>(t, 0x10, -1),
                  Field<int>(Slot(kSlotFlow), 0xc, -1), Field<u32>(Field<u64>(Slot(kSlotWorldChrMan), 0x60), 8, ~0u));
    out += b;
    for (int i = 0; i < 5; ++i) {
        const u64 e = t + 0x1c + u64(i) * 0x14;
        const u32 h = Field<u32>(e, 0, ~0u);
        if (h == 0xffffffffu) {
            continue;
        }
        const u64 chr = ChrOfHandle(h);
        std::snprintf(b, sizeof b, " [%d h %x kind %d st %d %d %d chr %s", i, h, Field<int>(e, 4), Field<int>(e, 8),
                      Field<int>(e, 0xc), Field<int>(e, 0x10),
                      !chr ? "STALE" : Field<u32>(chr, 8, ~0u) == h ? "ok" : "other");
        out += b;
        if (deep && chr && Field<u32>(chr, 8, ~0u) == h) {
            // What the cooperator count asks (vfunc +0x558: the net chr type, counted when in
            // {1,5,7,19}) and where it comes from: GameData (vfunc +0x1C8) +0x5D8 session type ->
            // SessionTypeDesc 0x553D750 + st * 0x80 +0x15; +0xA4 chr type.
            using Get = u64(BB_COOP_SYSV*)(u64);
            const u64 vt = Field<u64>(chr, 0);
            const u64 f558 = Field<u64>(vt, 0x558), f1c8 = Field<u64>(vt, 0x1c8);
            const int net = f558 ? int(u32(reinterpret_cast<Get>(f558)(chr))) : -1;
            const u64 gd = f1c8 ? reinterpret_cast<Get>(f1c8)(chr) : 0;
            const int st = Field<int>(gd, 0x5d8, -1);
            std::snprintf(b, sizeof b, " net %d (fn +%llx) gd st %d desc %d a4 %d", net,
                          ull(f558 ? f558 - Guest(0) : 0), st,
                          st >= 0 && st < 64 ? int(Field<u8>(Guest(kSessionTypeDesc + u64(st) * 0x80), 0x15, 0xff)) : -1,
                          Field<int>(gd, 0xa4, -1));
            out += b;
        }
        out += "]";
    }
    std::snprintf(b, sizeof b, " list %x %x %x %x (%d)", Field<u32>(t, 0x80), Field<u32>(t, 0x84), Field<u32>(t, 0x88),
                  Field<u32>(t, 0x8c), Field<int>(t, 0x90, -1));
    out += b;
    return out;
}

enum class GuestPhase { Idle, AwaitSnapshot, Loading, Rewinding, AwayLoading, InWorldFollow };
enum class HostPhase { Idle, Loading, Resynced };

struct State {
    std::mutex mu;
    // guest
    GuestPhase gphase = GuestPhase::Idle;
    bool have_intent = false;
    TravelIntent intent;  // under mu
    double since = 0;
    bool load_seen = false;
    bool screen_done = false;
    int stable = 0;
    // host
    HostPhase hphase = HostPhase::Idle;
    double hsince = 0;
    bool alone_load = false;  // host: this own load is an alone travel (split travel)
    bool arrived = false;  // a guest said it arrived (under mu)
    bool rejoined_logged = false; // host: an "arrived" came since the resync
    bool respawn_request = false;  // a guest died alone and reloads this world (under mu)
    bool respawn_live = false;     // ... or it travels alone into this world ("rejoin": not dead)
    bool away_request = false;     // a guest leaves this area alone ("away"): drop its chr (under mu)
    bool away_have_pos = false;
    float away_pos[3] = {};
    bool enter_state1 = false;     // guest: the follow's flow state 1 is entered by the main thread
    // guest: the running re-join is this guest's own travel (split travel): the destination is
    // its own (no transform beside the host, no summoned placement); the GSM +0x1521 it cleared.
    bool solo = false;
    bool blobs_saved = false;  // guest: this re-join's host world blobs were copied
    int saved_1521 = -1;
    u32 warp_lamp = 0;      // guest: the host's in-world party move to follow (main thread)
    double inworld_since = 0;  // guest: an in-world follow of a host load into this block
    bool respawn_have_pos = false;  // where it died (its member chr here stands there)
    float respawn_pos[3] = {};
    bool have_dest = false; // guest: the host's post-load transform (under mu)
    int hp_before = -1;      // guest: its HP at the host's travel event (kept through the B2 load)
    std::vector<std::pair<int, float>> buffs_before;  // guest: timed SpEffects (id, s left) at the travel
    u32 dest_stage = 0;
    float dest_pos[4] = {}, dest_rot[4] = {};
    // probe
    bool was_loading = false;
    u64 player_before = 0;
    std::deque<std::pair<int, std::string>> out;  // under mu
};
State& S() {
    static State s;
    return s;
}

void Send(int slot, const std::string& body) {
    std::lock_guard<std::mutex> lk(S().mu);
    S().out.emplace_back(slot, body);
}

std::string DescribeBlobs();
void ProbeTick(const GameSnapshot& s) {
    State& st = S();
    const bool loading = s.loading || !s.world_up;
    if (loading && !st.was_loading) {
        st.player_before = s.player;
        Log("load started: blobs %s", DescribeBlobs().c_str());
        Log("load started: local PlayerIns %#llx, role %d, flow %d; tasks:%s; presence %s", ull(s.player), Role(),
            Field<int>(Slot(kSlotFlow), 0x84, -1), DescribeTasks().c_str(), DescribePresence().c_str());
    } else if (!loading && st.was_loading) {
        Log("B2 load report: map %s, local PlayerIns %#llx -> %#llx (%s), player set %d, role %d, flow %d; tasks:%s",
            MapName(s.map_id).c_str(), ull(st.player_before), ull(s.player),
            st.player_before == s.player ? "SAME object" : "re-created",
            Field<int>(Slot(kSlotWorldChrMan), 0x30, -1), Role(), Field<int>(Slot(kSlotFlow), 0x84, -1),
            DescribeTasks().c_str());
        Log("B2 load report: presence %s", DescribePresence().c_str());
        const u64 gsm = Slot(kSlotGsm);
        Log("B2 load report: GSM +0x1593 %u +0x1594 %#x +0x1598 %u +0x159c %#x, GameDataMan +0x68 %#x, +0x0c %#x",
            unsigned(Field<u8>(gsm, 0x1593)), Field<u32>(gsm, 0x1594), unsigned(Field<u8>(gsm, 0x1598)),
            Field<u32>(gsm, 0x159c), Field<u32>(Slot(kSlotGameData), 0x68), Field<u32>(gsm, 0x0c));
        Log("B2 load report: blobs %s", DescribeBlobs().c_str());
    }
    st.was_loading = loading;
}

std::atomic<int> g_local_slot{-1};
std::atomic<bool> g_fell_back{false};  // a B1 fallback ran: clean the network flow once idle

// ---- Split travel (docs/party/split_travel.md): members in different areas ----
// Where every member is, from the party link's vitals (the director, every tick): map (0xAABBCCDD)
// and position as that member's own game reads them.
std::vector<Where> g_where;  // main thread
std::atomic<int> g_apart{0};  // members whose area differs from ours (K5, the host watchdog)
std::atomic<bool> g_host_alone_next{false};  // host: the next own load is an alone travel
std::atomic<double> g_host_alone_at{0};
std::atomic<bool> g_host_left{false};  // guest: the host travelled alone; drop its chr here
std::atomic<double> g_host_alone_seen{0};  // guest: when the host's last alone travel arrived (Now())
// A member that just travelled alone still reports its old map for a moment (its vitals stay
// until its load ends): no meet with it until its vitals show another area, or 30 s passed
// (live split3: the host re-created a guest's chr right after its "away").
std::atomic<double> g_away_until[8] = {};
void NoteAway(int slot) {
    if (slot >= 0 && slot < 8) {
        g_away_until[slot] = Now() + 30.0;
    }
}

void SendDeadEnemies(const GameSnapshot& s, const char* why);
u32 AreaOf(u32 map) {
    return (map == 0 || map == 0xffffffffu) ? 0u : map >> 24;
}

const Where* WhereOf(int slot) {
    for (const Where& w : g_where) {
        if (w.slot == slot) {
            return &w;
        }
    }
    return nullptr;
}

/// The host's current map as the vitals say (guest side); 0 unknown.
u32 HostMap() {
    const Where* w = WhereOf(0);
    return w && w->age_ms < 5000 ? w->map : 0u;
}

/// Removes a remote player chr as its task's STEP_DeleteWait 0x1E50A70 does (presentation slot
/// release 0x15BC080, WorldChrMan remove 0x19186E0); the task is kept (rewound to STEP_Create
/// when the two meet again). Main thread.
void RemoveRemoteChr(u64 chr, const char* why) {
    using Release = u64(BB_COOP_SYSV*)(u64, u32);
    using Remove = void(BB_COOP_SYSV*)(u64, u64);
    float p[3] = {};
    ChrPos(chr, p);
    reinterpret_cast<Release>(Guest(0x15bc080))(Field<u64>(Slot(kSlotGsm), 0x16f8), Field<u32>(chr, 8));
    reinterpret_cast<Remove>(Guest(0x19186e0))(Slot(kSlotWorldChrMan), chr);
    g_split_change_at = Now();
    Log("split: remote chr %#llx at (%.1f, %.1f, %.1f) removed (%s); its task is kept", ull(chr), p[0], p[1], p[2], why);
}

/// The member slot whose reported position is nearest to `p` (within `max_m`), -1 none.
int SlotNear(const float p[3], float max_m) {
    int best = -1;
    float bd = max_m * max_m;
    for (const Where& w : g_where) {
        if (w.slot == g_local_slot.load() || !w.has_pos || w.age_ms > 3000) {
            continue;
        }
        const float d = (w.pos[0] - p[0]) * (w.pos[0] - p[0]) + (w.pos[1] - p[1]) * (w.pos[1] - p[1]) +
                        (w.pos[2] - p[2]) * (w.pos[2] - p[2]);
        if (d < bd) {
            bd = d;
            best = w.slot;
        }
    }
    return best;
}

/// The remote chr standing nearest `p` (within 6 m), 0 none.
u64 ChrNear(const float p[3]) {
    u64 victim = 0;
    float best = 36.0f;
    for (const TaskInfo& t : Tasks(false)) {
        const u64 c = (t.mode == 1 || t.mode == 2) ? PlayerForPeer(t.task + kTaskPeer) : 0;
        float q[3];
        if (c && ChrPos(c, q)) {
            const float d = (q[0] - p[0]) * (q[0] - p[0]) + (q[1] - p[1]) * (q[1] - p[1]) + (q[2] - p[2]) * (q[2] - p[2]);
            if (d < best) {
                best = d;
                victim = c;
            }
        }
    }
    return victim;
}

/// Guest, main thread, before any flow state 1: the host's join-snapshot packets (0x19, 0x11,
/// 0x10) that arrived while this guest did not follow wait in the receive queue (0x1783160 pops
/// them only in state 1) and would be applied first - another map's, older flags. Popped and
/// dropped with the game's own receive 0x17893E0(flow, type, buf, size) (role 3/6 only).
void DrainSnapshotQueue(const char* why) {
    static std::vector<u8> buf(0x10000);
    using Recv = int(BB_COOP_SYSV*)(u64, u32, void*, u32);
    const u64 flow = Slot(kSlotFlow);
    if (!flow || Role() != RoleClient) {
        return;
    }
    std::string got;
    for (u32 type : {0x19u, 0x11u, 0x10u}) {
        int n = 0;
        for (int i = 0; i < 16; ++i) {
            const int r = reinterpret_cast<Recv>(Guest(0x17893e0))(flow, type, buf.data(), u32(buf.size()));
            if (r <= 0) {
                break;
            }
            ++n;
        }
        if (n) {
            char b[32];
            std::snprintf(b, sizeof b, " 0x%x x%d", type, n);
            got += b;
        }
    }
    if (!got.empty()) {
        Log("split: stale snapshot packets dropped before flow state 1 (%s):%s", why, got.c_str());
    }
}

/// Host: the per-peer net sync of this world (WorldChrMan +0xA40) for every member, without the
/// snapshot packets (a meet without a load: nobody is in flow state 1).
void NetSyncMembers() {
    using Three = void(BB_COOP_SYSV*)(u64, u64, u32);
    const u64 netsync = Field<u64>(Slot(kSlotWorldChrMan), 0xa40);
    const u64 arg = Slot(kSlotNetSyncArg);
    if (!netsync || !arg) {
        return;
    }
    const u32 a = Field<u32>(arg, 0x20);
    for (u64 id : Members()) {
        reinterpret_cast<Three>(Guest(kNetSyncJoin))(netsync, id, a);
        reinterpret_cast<Three>(Guest(kNetSyncData))(netsync, id, a);
    }
}

/// Both roles, main thread, steady world, no re-join running. (1) A remote chr whose owner (the
/// member reporting a position next to it) is in another area is removed; (2) members in our
/// area whose chr is missing here are built again (rewind), at most every 10 s: a meet without a
/// load (the host also re-registers its net sync); (3) the count of members apart.
void SplitTick(const GameSnapshot& s, double now) {
    static double last_meet = 0;
    static std::map<int, std::pair<u32, double>> seen;  // slot -> (area, since)
    if (!SessionUp() || !s.world_up || s.loading || !s.player) {
        return;
    }
    const u32 mine = AreaOf(s.map_id);
    if (mine == 0) {
        return;  // our own map not known yet (right after a load)
    }
    int apart = 0;
    bool meet = false;
    for (const Where& w : g_where) {
        if (w.slot == g_local_slot.load() || !w.connected) {
            continue;
        }
        const u32 a = w.age_ms < 5000 ? AreaOf(w.map) : 0u;
        auto& sn = seen[w.slot];
        if (sn.first != a) {
            sn = {a, now};
        }
        if (a == 0) {
            continue;  // loading / unknown: no decision
        }
        if (a != mine) {
            ++apart;
            if (w.slot >= 0 && w.slot < 8) {
                g_away_until[w.slot] = 0;  // elsewhere now: a later return to our area is a meet
            }
            continue;
        }
        // In our area for 2 s: its chr should be here.
        const float p[3] = {w.pos[0], w.pos[1], w.pos[2]};
        if (w.slot >= 0 && w.slot < 8 && g_away_until[w.slot].load() > now) {
            continue;  // left alone a moment ago; its vitals still show this area
        }
        if (now - sn.second >= 2.0 && w.has_pos && w.age_ms < 1500 && !ChrNear(p)) {
            meet = true;
        }
    }
    if (apart != g_apart.load()) {
        Log("split: %d member(s) in another area than ours (%s)", apart, MapName(s.map_id).c_str());
    }
    g_apart = apart;
    g_apart_k5 = apart;
    if (g_host_left.exchange(false) && Role() == RoleClient) {
        for (const TaskInfo& t : Tasks(false)) {
            if (t.mode == 2) {
                if (const u64 c = PlayerForPeer(t.task + kTaskPeer)) {
                    RemoveRemoteChr(c, "the host travelled alone");
                }
            }
        }
    }
    // (1) chrs of members that are elsewhere.
    for (const TaskInfo& t : Tasks(false)) {
        const u64 c = (t.mode == 1 || t.mode == 2) ? PlayerForPeer(t.task + kTaskPeer) : 0;
        float p[3];
        if (!c || !ChrPos(c, p)) {
            continue;
        }
        const int slot = SlotNear(p, 5.0f);
        const Where* w = slot >= 0 ? WhereOf(slot) : nullptr;
        if (w && AreaOf(w->map) != 0 && AreaOf(w->map) != mine) {
            char why[96];
            std::snprintf(why, sizeof why, "slot %d is in %s, we are in %s", slot, MapName(w->map).c_str(),
                          MapName(s.map_id).c_str());
            RemoveRemoteChr(c, why);
        }
    }
    // (2) a meet without a load.
    if (meet && now - last_meet >= 10.0) {
        bool chrless = false;
        for (const TaskInfo& t : Tasks(false)) {
            chrless = chrless || ((t.mode == 1 || t.mode == 2) && t.step >= kStepCreate && !(t.flags & 2) &&
                                  !PlayerForPeer(t.task + kTaskPeer));
        }
        if (chrless) {
            last_meet = now;
            if (Role() == RoleClient) {
                DrainSnapshotQueue("meet");
            } else {
                NetSyncMembers();
            }
            g_split_change_at = now;
            SendDeadEnemies(s, "meet without a load");
            const int n = RewindRemoteTasks("split meet");
            Log("split: a member is in our area (%s) without its chr here: %d task(s) rewound (no load)",
                MapName(s.map_id).c_str(), n);
        }
    }
}

/// Guest: the host is apart (another area, or it travelled alone in the last 60 s) and its party
/// link entry is connected and fresh.
bool SplitHostReachable() {
    const Where* h = WhereOf(0);
    const bool apart = g_apart.load() > 0 || Now() - g_host_alone_seen.load() < 60.0;
    return apart && h && h->connected && h->age_ms < 5000;
}

// ---- The host's world blobs of packet 0x11 (GSM +0x16C0/+0x16C8 decals, +0x16A0/+0x16A8 map
// items, +0x1684/+0x1688 CHR), kept for an alone load. A host-world load into a map the host has
// not loaded (no snapshot) built no world (live split5/split7: map "-", the player falling through
// nothing - the user's "grey screen"). The map items blob is what the host-world loader reads
// (getter 0x156C600: +0x16B0 own world / +0x16A0 host world); copies of the last ones the host sent
// are put back (game heap, *0x5540420 vfunc +0x58) when they are empty before an alone load.
struct Blob {
    u64 size_off, ptr_off;
    std::vector<u8> data;
};
Blob g_blobs[3] = {{0x16c0, 0x16c8, {}}, {0x16a0, 0x16a8, {}}, {0x1684, 0x1688, {}}};

std::string DescribeBlobs() {
    const u64 gsm = Slot(kSlotGsm);
    char b[160];
    std::snprintf(b, sizeof b, "decals %d, map items %d, chr %d (own: %d / %d / %d)", Field<int>(gsm, 0x16c0),
                  Field<int>(gsm, 0x16a0), Field<int>(gsm, 0x1684), Field<int>(gsm, 0x16d0), Field<int>(gsm, 0x16b0),
                  Field<int>(gsm, 0x1690));
    return b;
}

void SaveHostBlobs() {
    const u64 gsm = Slot(kSlotGsm);
    for (Blob& bl : g_blobs) {
        const int n = Field<int>(gsm, bl.size_off);
        const u64 p = Field<u64>(gsm, bl.ptr_off);
        if (n > 0 && n < (1 << 22) && p) {
            bl.data.resize(std::size_t(n));
            if (!SafeRead(p, bl.data.data(), bl.data.size())) {
                bl.data.clear();
            }
        }
    }
    Log("guest: host world blobs kept for alone loads (%s)", DescribeBlobs().c_str());
}

void RestoreHostBlobs() {
    const u64 gsm = Slot(kSlotGsm);
    const u64 heap = Slot(0x5540420);
    if (!gsm || !heap) {
        return;
    }
    using Alloc = u64(BB_COOP_SYSV*)(u64, u64, u64);
    const u64 alloc = Field<u64>(Field<u64>(heap, 0), 0x58);
    std::string put;
    for (Blob& bl : g_blobs) {
        if (bl.data.empty() || Field<int>(gsm, bl.size_off) > 0 || Field<u64>(gsm, bl.ptr_off) || !alloc) {
            continue;
        }
        const u64 p = reinterpret_cast<Alloc>(alloc)(heap, bl.data.size(), 1);
        if (!p || !SafeWrite(p, bl.data.data(), bl.data.size())) {
            continue;
        }
        const int n = int(bl.data.size());
        SafeWrite(gsm + bl.ptr_off, &p, 8);
        SafeWrite(gsm + bl.size_off, &n, 4);
        char b[32];
        std::snprintf(b, sizeof b, " +0x%llx (%d B)", ull(bl.size_off), n);
        put += b;
    }
    Log("guest: alone load: host world blobs before (%s); put back:%s", DescribeBlobs().c_str(),
        put.empty() ? " none" : put.c_str());
}

// ---- In-world warps: a move inside the loaded map needs no load (no loading screen) ----
void BesideHost(float pos[4], const float rot[4], int slot);
/// The local player's current map (packed), 0xffffffff unknown.
u32 LocalMap() {
    const GameSnapshot g = ReadGameState(false);
    return g.world_up && !g.loading ? g.map_id : 0xffffffffu;
}

/// `map` (packed) is the block the local player stands in: area and block equal.
bool InLoadedBlock(u32 map) {
    const u32 cur = LocalMap();
    return cur != 0xffffffffu && map != 0 && map != 0xffffffffu && (map >> 16) == (cur >> 16);
}

/// The chr warp vfunc +0x5F8 (0x18CD600(chr, pos, rot, 1), as the NPC summon step and the B2
/// spacing use it) on the local player: same map, no load. Main thread.
bool WarpLocal(const float pos[3], float yaw, const char* why) {
    const u64 player = Field<u64>(Slot(kSlotWorldChrMan), 0x60);
    const u64 fn = player ? Field<u64>(Field<u64>(player, 0), 0x5f8) : 0;
    if (!fn) {
        return false;
    }
    alignas(16) float p16[4] = {pos[0], pos[1], pos[2], 1.0f};
    alignas(16) float r16[4] = {0.0f, yaw, 0.0f, 0.0f};
    float before[3] = {};
    ChrPos(player, before);
    using Warp = void(BB_COOP_SYSV*)(u64, const float*, const float*, u32);
    reinterpret_cast<Warp>(fn)(player, p16, r16, 1);
    Log("in-world warp (%s): (%.1f, %.1f, %.1f) -> (%.1f, %.1f, %.1f) in %s, no load", why, before[0], before[1], before[2],
        pos[0], pos[1], pos[2], MapName(LocalMap()).c_str());
    return true;
}

/// Lamp / return point `id` is in the loaded block and its spot is known.
bool LampInLoadedBlock(u32 id, float spot[4]) {
    return id != 0 && id != kTravelNone && id / 100000 != 29 && ids::ReturnSpot(id, spot) &&
           InLoadedBlock(ids::EntityMap(id));
}

/// To lamp `id`'s spot without a load; beside it by party slot when `beside` (the spacing of
/// BesideHost: guests never stand inside each other).
bool WarpToLampInWorld(u32 id, bool beside, const char* why) {
    float spot[4];
    if (!LampInLoadedBlock(id, spot)) {
        return false;
    }
    float pos[4] = {spot[0], spot[1], spot[2], 1.0f};
    const float rot[4] = {0.0f, spot[3], 0.0f, 0.0f};
    if (beside && g_local_slot.load() > 0) {
        BesideHost(pos, rot, g_local_slot.load());
    }
    char b[96];
    std::snprintf(b, sizeof b, "%s, lamp %u", why, id);
    return WarpLocal(pos, spot[3], b);
}

/// An alone travel to a world lamp makes it this player's own last lamp (GSM +0x1528 {return
/// point entity, anim}: what the Hunter's Mark and a death go to), as the lamp warp 0x13CDF30
/// would have written it (our travel replaces that warp with a transform / an in-world move).
/// The Hunter's Dream is never a last lamp.
void SetOwnLastLamp(const TravelIntent& t) {
    const u32 id = t.lamp_id != kTravelNone && t.lamp_id ? t.lamp_id : 0;
    if (!id || id / 100000 == 21 || t.last_lamp == ~0ull || !t.last_lamp) {
        return;
    }
    const u64 gsm = Slot(kSlotGsm);
    const u64 was = Field<u64>(gsm, 0x1528);
    if (gsm && was != t.last_lamp && SafeWrite(gsm + 0x1528, &t.last_lamp, 8)) {
        Log("own last lamp %#llx -> %#llx (lamp %u, an alone travel)", ull(was), ull(t.last_lamp), id);
    }
}

// ---- Enemies killed while apart (reconciled when players meet again without a load) ----
// Each game simulates its own copies of a map's enemies while the players are apart (a map the
// host is not in, or the host away while a guest stays). Meeting again without a load (SplitTick,
// the in-world follow) each side sends the enemies of the shared block that are dead in its world
// (EVENT b2 {"op":"dead"}); the other side kills those that still live in its own (HP 0: the
// game's own death of the chr). A kill on either side stays a kill (the union); bosses (entity
// AAB08xx) stay with their defeat flags (D1 / D3), never by this.
template <class F>
void ForEachMapChr(F&& fn) {
    const u64 wcm = Slot(kSlotWorldChrMan);
    const int sets = Field<int>(wcm, 0x90);
    if (!wcm || sets <= 0 || sets > 4096) {
        return;
    }
    const int blocks = Field<int>(wcm, 0x20);
    const u64 base = Field<u64>(wcm, 0x28), list38 = Field<u64>(wcm, 0x38);
    for (int i = 0; i < sets; ++i) {
        const u64 set = Field<u64>(wcm, 0x98 + 8 * u64(i));
        const int n = Field<int>(set, 0xf0);
        const u64 arr = Field<u64>(set, 0xf8);
        if (!set || !arr || n <= 0 || n > 100000) {
            continue;
        }
        for (int k = 0; k < n; ++k) {
            const int id = Field<int>(arr + 8 * u64(k), 0);
            const int handle = Field<int>(arr + 8 * u64(k), 4, -1);
            if (id <= 0 || handle < 0) {
                continue;
            }
            const u32 block = (u32(handle) >> 14) & 0x3f, index = u32(handle) & 0x3fff;
            u64 entry = 0;
            if (int(block) < blocks) {
                const u64 lst = Field<u64>(base + u64(block) * 0x160, 0xc0);
                entry = lst ? lst + u64(index) * 0x38 : 0;
            } else {
                entry = list38 ? list38 + u64(index) * 0x38 : 0;
            }
            const u64 chr = Field<u64>(entry, 0);
            if (chr) {
                fn(u32(id), chr);
            }
        }
    }
}

bool EnemyReconcilable(u32 id, u32 block_map) {
    return id >= 1000000 && ids::EntityMap(id) == (block_map & 0xffff0000u) && id % 10000 / 100 != 8;
}

std::vector<u32> DeadEnemiesHere(u32 map) {
    std::vector<u32> out;
    ForEachMapChr([&](u32 id, u64 chr) {
        const u64 dm = Field<u64>(Field<u64>(chr, 0x3b0), 0x20);
        const int hp = Field<int>(dm, 0x128, 1), mx = Field<int>(dm, 0x12c, 0);
        if (dm && mx > 0 && hp <= 0 && EnemyReconcilable(id, map) && out.size() < 128) {
            out.push_back(id);
        }
    });
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

/// Sends the dead enemies of our block to the others (host: everyone; guest: the host, which
/// relays nothing - every guest sends its own).
void SendDeadEnemies(const GameSnapshot& s, const char* why) {
    const std::vector<u32> dead = DeadEnemiesHere(s.map_id);
    std::string body = "{\"op\":\"dead\",\"map\":" + std::to_string(s.map_id) + ",\"ids\":[";
    for (std::size_t i = 0; i < dead.size(); ++i) {
        body += (i ? "," : "") + std::to_string(dead[i]);
    }
    body += "]}";
    Send(Role() == RoleHost ? -1 : 0, body);
    Log("split: %zu dead enemies of %s sent (%s)", dead.size(), MapName(s.map_id).c_str(), why);
}

std::mutex g_dead_mu;
std::vector<std::pair<u32, std::vector<u32>>> g_dead_in;  // (map, ids) received

void ApplyDeadEnemies(const GameSnapshot& s) {
    std::vector<std::pair<u32, std::vector<u32>>> in;
    {
        std::lock_guard<std::mutex> lk(g_dead_mu);
        in.swap(g_dead_in);
    }
    for (const auto& [map, list] : in) {
        if ((map >> 16) != (s.map_id >> 16)) {
            Log("split: dead enemies of %s ignored (we are in %s)", MapName(map).c_str(), MapName(s.map_id).c_str());
            continue;
        }
        int killed = 0, already = 0;
        ForEachMapChr([&](u32 id, u64 chr) {
            if (!EnemyReconcilable(id, s.map_id) || !std::binary_search(list.begin(), list.end(), id)) {
                return;
            }
            const u64 dm = Field<u64>(Field<u64>(chr, 0x3b0), 0x20);
            const int hp = Field<int>(dm, 0x128, 0);
            if (dm && hp > 0) {
                const int zero = 0;
                SafeWrite(dm + 0x128, &zero, 4);
                ++killed;
            } else {
                ++already;
            }
        });
        Log("split: enemies killed by the other side while apart: %d here now dead too (%d already dead, %zu sent)",
            killed, already, list.size());
    }
}

/// Test aid: the nearest living enemy of our block within `max_m` (entity id, 0 none).
u32 NearestEnemy(const GameSnapshot& s, float max_m) {
    float me[3] = {};
    if (!ChrPos(s.player, me)) {
        return 0;
    }
    u32 best = 0;
    float bd = max_m * max_m;
    ForEachMapChr([&](u32 id, u64 chr) {
        const u64 dm = Field<u64>(Field<u64>(chr, 0x3b0), 0x20);
        float p[3];
        if (!EnemyReconcilable(id, s.map_id) || Field<int>(dm, 0x128, 0) <= 0 || !ChrPos(chr, p)) {
            return;
        }
        const float d = (p[0] - me[0]) * (p[0] - me[0]) + (p[1] - me[1]) * (p[1] - me[1]) + (p[2] - me[2]) * (p[2] - me[2]);
        if (d < bd) {
            bd = d;
            best = id;
        }
    });
    return best;
}

std::string DescribeWhere() {
    std::string out;
    for (const Where& w : g_where) {
        char b[96];
        std::snprintf(b, sizeof b, " [slot %d %s %s (%.0f, %.0f, %.0f) %u ms]", w.slot, w.connected ? "" : "lost",
                      MapName(w.map).c_str(), w.pos[0], w.pos[1], w.pos[2], w.age_ms);
        out += b;
    }
    return out;
}

/// A guest's spot beside the host (never on it: character collision shoves the host): BB_PARTY_SUMMON_OFFSET
/// (default 0.5 m, 0 = off), side by side in a line: right (party slot 1), left (2), 2x right (3+) of
/// the host's facing, same height and facing. Wider offsets (2.2 m) put guests into walls at narrow
/// lamp spots (Central Yharnam).
/// rot[1] is the yaw (radians); forward = (sin, 0, cos), right = (cos, 0, -sin).
float SummonOffset() {
    static const float d = [] {
        const char* v = std::getenv("BB_PARTY_SUMMON_OFFSET");
        return v && v[0] ? float(std::atof(v)) : 0.5f;
    }();
    return d;
}

// `spot` 1..3 beside the host (coop::fourp::SpotOffset: a line along the host's right axis, right,
// left, 2x right; a party slot is its own spot), neighbours SummonOffset() apart (A7, host + 3).
void BesideHost(float pos[4], const float rot[4], int spot) {
    const float yaw = rot[1];
    const float fx = std::sin(yaw), fz = std::cos(yaw), rx = std::cos(yaw), rz = -std::sin(yaw);
    float right = 0, forward = 0;
    coop::fourp::SpotOffset(spot, SummonOffset(), &right, &forward);
    pos[0] += rx * right + fx * forward;
    pos[2] += rz * right + fz * forward;
}

/// Guest, after any host-world arrival (a bell join or a B2 travel: flow 3 -> 4): once the host's chr
/// stands in this world, a guest closer than the offset - 0.5 m is moved beside it (by party slot,
/// the host's facing and height) with the chr warp vfunc +0x5F8 (0x18CD600(chr, pos, rot, 1), as
/// the NPC summon step 0x1E4B559 calls it). The game's own placement (the sign spot, usually the
/// host's lamp spot) is otherwise on top of the host. No ground probe yet: the host's height.
// ---- Host-world flag overlay guard (B2 re-entry) ----
// SprjEventFlagMan (*0x553B100) +0x88/+0x90: std::map<u32 group key, u8* 125-byte buffer>, the
// host-world overlay of load mode 1 (nodes: left +0, parent +8, right +0x10, is-nil +0x19,
// key +0x20, buffer +0x28). Run 18 showed the overlay all zero after the B2 re-sent snapshot was
// applied (9401 / 12410000 = 0 where the join's snapshot had 1), so the client replayed the
// opening cutscene. The guest keeps a copy of the overlay it had in the host's world and, after
// the new snapshot, restores every group that came back all zero.
std::map<u32, std::vector<u8>> g_overlay_saved;

template <class F>
void ForEachOverlayGroup(F&& fn) {
    const u64 man = Slot(0x553b100);
    const u64 head = Field<u64>(man, 0x90);
    if (!head) {
        return;
    }
    std::vector<u64> stack;
    const u64 root = Field<u64>(head, 8);
    if (root && root != head) {
        stack.push_back(root);
    }
    int guard = 0;
    while (!stack.empty() && guard++ < 4096) {
        const u64 n = stack.back();
        stack.pop_back();
        if (!n || n == head || Field<u8>(n, 0x19)) {
            continue;
        }
        fn(Field<u32>(n, 0x20), Field<u64>(n, 0x28));
        stack.push_back(Field<u64>(n, 0));
        stack.push_back(Field<u64>(n, 0x10));
    }
}

void SaveOverlay() {
    g_overlay_saved.clear();
    int nonzero = 0;
    ForEachOverlayGroup([&](u32 key, u64 buf) {
        std::vector<u8> v(125);
        if (buf && SafeRead(buf, v.data(), v.size())) {
            bool any = false;
            for (u8 b : v) {
                any = any || b;
            }
            nonzero += any ? 1 : 0;
            g_overlay_saved[key] = std::move(v);
        }
    });
    Log("overlay guard: %zu host-world groups saved (%d non-zero)", g_overlay_saved.size(), nonzero);
}

void RestoreZeroedOverlay() {
    int zero = 0, restored = 0, total = 0;
    ForEachOverlayGroup([&](u32 key, u64 buf) {
        ++total;
        u8 v[125];
        if (!buf || !SafeRead(buf, v, sizeof v)) {
            return;
        }
        bool any = false;
        for (u8 b : v) {
            any = any || b;
        }
        if (any) {
            return;
        }
        ++zero;
        auto it = g_overlay_saved.find(key);
        if (it == g_overlay_saved.end()) {
            return;
        }
        bool saved_any = false;
        for (u8 b : it->second) {
            saved_any = saved_any || b;
        }
        if (saved_any && SafeWrite(buf, it->second.data(), it->second.size())) {
            ++restored;
        }
    });
    Log("overlay guard: %d groups, %d all-zero after the new snapshot, %d restored from the saved overlay", total,
        zero, restored);
}

/// The flags the opening cutscene depends on, as the store answers now (overlay in load mode 1).
/// BB_PARTY_B2_TEST_SPEFFECT=<id>[,<id>...]: SpEffects the guest applies to itself at the host's
/// travel event (as a buff item would), to see what a B2 load does to them.
std::vector<int> TestSpEffects() {
    std::vector<int> ids;
    if (const char* v = std::getenv("BB_PARTY_B2_TEST_SPEFFECT"); v && v[0]) {
        for (const char* p = v; *p;) {
            char* end = nullptr;
            const long id = std::strtol(p, &end, 10);
            if (end == p) {
                break;
            }
            if (id > 0) {
                ids.push_back(int(id));
            }
            p = *end ? end + 1 : end;
        }
    }
    return ids;
}

/// The active SpEffects of a chr (ChrIns +0x1C8 -> +8 first node; node: param id +0x40, next
/// +0x58; party_director HasSpEffect; +0x00 s left, +0x04 duration (f32), runtime-checked with a
/// freshly applied 60 s buff: 60/60).
std::string DescribeSpEffects(u64 chr) {
    std::string out;
    u64 node = Field<u64>(Field<u64>(chr, 0x1c8), 8);
    int n = 0;
    for (int guard = 0; node && guard < 256; ++guard, ++n) {
        const int id = Field<int>(node, 0x40, -1);
        char b[48];
        const float left = Field<float>(node, 0), total = Field<float>(node, 4);
        if (total > 0.f) {
            std::snprintf(b, sizeof b, " %d(%.1f/%.0f s)", id, double(left), double(total));
        } else {
            std::snprintf(b, sizeof b, " %d", id);
        }
        out += b;
        node = Field<u64>(node, 0x58);
    }
    return std::to_string(n) + ":" + out;
}

/// Timed SpEffects (a duration, time left) of a chr: buffs a load drops. The multiplayer / bell
/// system effects (9000-9199) are left to the game.
std::vector<std::pair<int, float>> TimedSpEffects(u64 chr) {
    std::vector<std::pair<int, float>> out;
    u64 node = Field<u64>(Field<u64>(chr, 0x1c8), 8);
    for (int guard = 0; node && guard < 256; ++guard) {
        const int id = Field<int>(node, 0x40, -1);
        const float left = Field<float>(node, 0), total = Field<float>(node, 4);
        if (id > 0 && !(id >= 9000 && id < 9200) && total > 0.f && left > 0.f && left <= total) {
            out.emplace_back(id, left);
        }
        node = Field<u64>(node, 0x58);
    }
    return out;
}

/// Guest after a B2 load: the timed buffs it had at the host's travel event and lost in the load,
/// applied again (chr vfunc +0x3F0) with the time they had left then (the guest's load is a pause
/// for it, as for its HP). BB_PARTY_B2_KEEP_BUFFS=0: off.
void RestoreBuffs(u64 player, const std::vector<std::pair<int, float>>& before) {
    static const bool on = !(std::getenv("BB_PARTY_B2_KEEP_BUFFS") && std::getenv("BB_PARTY_B2_KEEP_BUFFS")[0] == '0');
    if (!on || before.empty() || !player) {
        return;
    }
    u64 vt = 0, fn = 0;
    if (!SafeGet(player, &vt) || !vt || !SafeGet(vt + 0x3f0, &fn) || !fn) {
        return;
    }
    using Fn = void(BB_COOP_SYSV*)(u64, u64, u64, u64, u64, u64, u64, float, float, float, float, float);
    std::string done;
    for (const auto& [id, left] : before) {
        bool have = false;
        for (u64 node = Field<u64>(Field<u64>(player, 0x1c8), 8); node && !have; node = Field<u64>(node, 0x58)) {
            have = Field<int>(node, 0x40, -1) == id;
        }
        if (have) {
            continue;
        }
        reinterpret_cast<Fn>(fn)(player, u64(id), player, 0, 1, 0, 0, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
        for (u64 node = Field<u64>(Field<u64>(player, 0x1c8), 8); node; node = Field<u64>(node, 0x58)) {
            if (Field<int>(node, 0x40, -1) == id) {
                if (left < Field<float>(node, 4)) {
                    SafeWrite(node, &left, 4);
                }
                char b[32];
                std::snprintf(b, sizeof b, " %d(%.1f s)", id, double(left));
                done += b;
                break;
            }
        }
    }
    if (!done.empty()) {
        Log("guest: buffs lost in the load restored:%s", done.c_str());
    }
}

/// Guest: the test SpEffects on itself (chr vfunc +0x3F0, the call shape of party_director's bell).
void ApplyTestSpEffects(u64 player) {
    u64 vt = 0, fn = 0;
    if (!player || !SafeGet(player, &vt) || !vt || !SafeGet(vt + 0x3f0, &fn) || !fn) {
        return;
    }
    using Fn = void(BB_COOP_SYSV*)(u64, u64, u64, u64, u64, u64, u64, float, float, float, float, float);
    for (int id : TestSpEffects()) {
        reinterpret_cast<Fn>(fn)(player, u64(id), player, 0, 1, 0, 0, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
        Log("test speffect: %d applied to the guest", id);
    }
}

/// The guest's consumables (goods counts), HP / stamina and SpEffects, for the "state kept" check.
void LogGuestState(const GameSnapshot& s, const char* when) {
    const u64 dm = Field<u64>(Field<u64>(s.player, 0x3b0), 0x20);
    std::string g;
    for (u32 id : {900u, 1000u, 1010u, 300u, 310u, 200u, 205u}) {
        char b[32];
        std::snprintf(b, sizeof b, " %u:%d", id, GoodsCount(id));
        g += b;
    }
    Log("guest state %s: HP %d/%d stamina %d/%d echoes %d insight %d; goods%s; speffects %s", when, Field<int>(dm, 0xf8, -1),
        Field<int>(dm, 0xfc, -1), Field<int>(dm, 0x134, -1), Field<int>(dm, 0x138, -1), s.echoes, s.insight, g.c_str(),
        DescribeSpEffects(s.player).c_str());
}

void LogIntroFlags(const char* when) {
    const u64 man = Slot(0x553b100);
    if (!man) {
        return;
    }
    using Get = u32(BB_COOP_SYSV*)(u64, u32, u32);
    const auto f = [&](u32 id) { return reinterpret_cast<Get>(Guest(0x13cfd80))(man, id, 1); };
    Log("flags %s (load mode %d, GSM+0x1592 %u): 12410000=%u 9401=%u 9402=%u 12417810=%u 9180=%u 12410105=%u "
        "12411800=%u 2410=%u",
        when, Field<int>(man, 0x80, -1), unsigned(Field<u8>(Slot(kSlotGsm), 0x1592)), f(12410000), f(9401), f(9402),
        f(12417810), f(9180), f(12410105), f(12411800), f(2410));
}

/// A remote member's net chr type (what the game's cooperator count 0x15BDC20 and the EMEVD
/// co-op client count ask: chr vfunc +0x558 = SessionTypeDesc[GameData +0x5D8] +0x15, counted
/// when in {1,5,7,19}) is remembered per member task while it counts; if a B2 re-creation leaves
/// the member's GameData with another session type, the remembered one is put back. A guest's own
/// player is checked the same way (key 0): its B2 host-world load sets its own +0x5D8 to 0 (live
/// r6: st 7 -> 0, so its record re-broadcast at InGameStart carried 0 to the host as well).
void MemberSessionTypeTick(const GameSnapshot& s) {
    static std::map<u64, int> good;  // task (0: the guest itself) -> session type while counted
    if (!SessionUp()) {
        good.clear();
        return;
    }
    if (!s.world_up || s.loading) {
        return;
    }
    using Get = u64(BB_COOP_SYSV*)(u64);
    std::vector<std::pair<u64, u64>> members;  // (key, chr)
    for (const TaskInfo& t : Tasks(false)) {
        if (t.mode == 1 || t.mode == 2) {
            members.emplace_back(t.task, PlayerForPeer(t.task + kTaskPeer));
        }
    }
    if (Role() == RoleClient) {
        members.emplace_back(0, s.player);
    }
    for (const auto& [key, chr] : members) {
        const u64 f1c8 = chr ? Field<u64>(Field<u64>(chr, 0), 0x1c8) : 0;
        const u64 gd = f1c8 ? reinterpret_cast<Get>(f1c8)(chr) : 0;
        if (!gd) {
            continue;
        }
        const int st = Field<int>(gd, 0x5d8, -1);
        const auto counted = [](int type) {
            if (type < 0 || type >= 64) {
                return false;
            }
            const u8 net = Field<u8>(Guest(kSessionTypeDesc + u64(type) * 0x80), 0x15, 0xff);
            return net < 20 && ((0x800a2u >> net) & 1);
        };
        if (counted(st)) {
            good[key] = st;
        } else if (const auto it = good.find(key); it != good.end() && it->second != st) {
            SafeWrite(gd + 0x5d8, &it->second, 4);
            Log("%s: session type %d after the load / re-creation (not a cooperator) -> %d as before",
                key ? "member task" : "guest (own player)", st, it->second);
        }
    }
}

/// After any host-world arrival (flow 3 -> 4): the local and the remote players' positions every
/// 0.5 s for 3 s, on both instances (placement stability; nothing is written after a load).
void ArrivalWatchTick(const GameSnapshot& s, double now) {
    static int last_flow = -1;
    static double until = 0, next = 0;
    const int fs = Field<int>(Slot(kSlotFlow), 0x84, -1);
    if (last_flow == 3 && fs == 4) {
        next = -1; // armed: starts when the world is up
    }
    last_flow = fs;
    if (next == 0 || !s.world_up || s.loading || !s.player) {
        return;
    }
    if (next < 0) {
        next = now;
        until = now + 3.0;
        LogIntroFlags("after the host-world load");
    }
    if (now < next) {
        return;
    }
    next = now + 0.5;
    const auto pos = [](u64 chr, float out[4]) {
        const u64 a = Field<u64>(Field<u64>(Field<u64>(Field<u64>(chr, 0x58), 8), 0x3b0), 0x68);
        return a && SafeRead(a + 0x1e0, out, 16);
    };
    float me[4] = {};
    pos(s.player, me);
    std::string others;
    for (const TaskInfo& t : Tasks(false)) {
        if (t.mode != 1 && t.mode != 2) {
            continue;
        }
        float o[4] = {};
        char b[128];
        const u64 c = PlayerForPeer(t.task + kTaskPeer);
        if (c && pos(c, o)) {
            std::snprintf(b, sizeof b, " [mode %d (%.2f, %.2f, %.2f) %.2f m]", t.mode, o[0], o[1], o[2],
                          std::sqrt((o[0] - me[0]) * (o[0] - me[0]) + (o[2] - me[2]) * (o[2] - me[2])));
        } else {
            std::snprintf(b, sizeof b, " [mode %d no chr]", t.mode);
        }
        others += b;
    }
    Log("arrival watch: me (%.2f, %.2f, %.2f)%s", me[0], me[1], me[2], others.c_str());
    if (now >= until) {
        next = 0;
    }
}

/// Host: the same 3 s watch when a member's chr appears (a join or a B2 re-creation).
void HostMemberWatchTick(const GameSnapshot& s, double now) {
    static int had = 0;
    static double until = 0, next = 0;
    int have = 0;
    for (const TaskInfo& t : Tasks(false)) {
        have += (t.mode == 1 && PlayerForPeer(t.task + kTaskPeer)) ? 1 : 0;
    }
    if (Role() == RoleHost && have > had) {
        next = now;
        until = now + 3.0;
    }
    had = have;
    if (next == 0 || now < next || !s.player) {
        return;
    }
    next = now + 0.5;
    const auto pos = [](u64 chr, float out[4]) {
        const u64 a = Field<u64>(Field<u64>(Field<u64>(Field<u64>(chr, 0x58), 8), 0x3b0), 0x68);
        return a && SafeRead(a + 0x1e0, out, 16);
    };
    float me[4] = {};
    pos(s.player, me);
    std::string others;
    for (const TaskInfo& t : Tasks(false)) {
        float o[4] = {};
        const u64 c = t.mode == 1 ? PlayerForPeer(t.task + kTaskPeer) : 0;
        if (c && pos(c, o)) {
            char b[128];
            std::snprintf(b, sizeof b, " [guest (%.2f, %.2f, %.2f) %.2f m]", o[0], o[1], o[2],
                          std::sqrt((o[0] - me[0]) * (o[0] - me[0]) + (o[2] - me[2]) * (o[2] - me[2])));
            others += b;
        }
    }
    Log("host watch: me (%.2f, %.2f, %.2f)%s", me[0], me[1], me[2], others.c_str());
    if (now >= until) {
        next = 0;
    }
}

/// Before a guest rings (its own world, before any load): when it stands in the host's map it steps
/// BB_PARTY_SUMMON_OFFSET aside in its own world - right / left / behind by slot, each try checked
/// 30 ticks later (reached, same height +-1.5 m: walkable), else the next side, else back to the
/// start. The sign is placed where it stands, so the join placement is beside the host once,
/// before the load; nothing moves it afterwards. True while it is still moving (no ring yet).
bool PreRingSpacing(const GameSnapshot& s, u32 host_map) {
    static int attempt = -1, wait = 0;
    static std::chrono::steady_clock::time_point moved_at{};
    static u64 done_for = 0;  // the PlayerIns it was done for (a new one after every load)
    alignas(16) static float start[4], target[4], start_rot[4];
    const float d = SummonOffset();
    if (d <= 0.0f || !s.player || !s.world_up || s.loading || done_for == s.player ||
        (host_map >> 16) != (s.map_id >> 16)) {
        return false;
    }
    const u64 pm = Field<u64>(Field<u64>(Field<u64>(Field<u64>(s.player, 0x58), 8), 0x3b0), 0x68);
    alignas(16) float me[4], rot[4];
    if (!pm || !SafeRead(pm + 0x1e0, me, 16) || !SafeRead(pm + 0x1d0, rot, 16)) {
        return false;
    }
    using Warp = void(BB_COOP_SYSV*)(u64, const float*, const float*, u32);
    if (attempt >= 0) {
        // 0.8 s after the warp (a fall off a ledge has lost > 1.5 m by then): 30 ticks were up to
        // 3.4 s per side at a slow frame rate (A4: the guest's first bell came 6.9 s late).
        (void)wait;
        if (std::chrono::steady_clock::now() - moved_at < std::chrono::milliseconds(800)) {
            return true;
        }
        const float off =
            std::sqrt((me[0] - target[0]) * (me[0] - target[0]) + (me[2] - target[2]) * (me[2] - target[2]));
        const bool ok = off < 0.8f && std::fabs(me[1] - start[1]) < 1.5f;
        Log("pre-ring spacing: side try %d %s: at (%.2f, %.2f, %.2f), %.2f m from the target", attempt,
            ok ? "OK" : "blocked", me[0], me[1], me[2], off);
        if (ok || attempt >= 2) {
            if (!ok) {
                reinterpret_cast<Warp>(Field<u64>(Field<u64>(s.player, 0), 0x5f8))(s.player, start, start_rot, 1);
                Log("pre-ring spacing: no side free; back to the start spot");
            }
            done_for = s.player;
            attempt = -1;
            return false;
        }
    } else {
        std::memcpy(start, me, sizeof me);
        std::memcpy(start_rot, rot, sizeof rot);
    }
    ++attempt;
    wait = 0;
    moved_at = std::chrono::steady_clock::now();
    const int side = coop::fourp::SpotForTry(g_local_slot.load(), attempt);
    std::memcpy(target, start, sizeof start);
    BesideHost(target, start_rot, side);
    reinterpret_cast<Warp>(Field<u64>(Field<u64>(s.player, 0), 0x5f8))(s.player, target, start_rot, 1);
    Log("pre-ring spacing: own world, from (%.2f, %.2f, %.2f) to side %d (%.2f, %.2f, %.2f), %.1f m", start[0],
        start[1], start[2], side, target[0], target[1], target[2], d);
    return true;
}

/// Safety net: a client in the host's world (GSM+0x1592 == 0) never plays the opening cutscene
/// (event 12410000's EMEVD 2002 remo): its flags come from the host's overlay, which may lack them.
bool ClientIntroSkip(int event_id, int bank, int id) {
    if (bank != 2002 || event_id != 12410000 || !g_on.load() || Role() != RoleClient ||
        Field<u8>(Slot(kSlotGsm), 0x1592) != 0) {
        return false;
    }
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 3) {
        Log("client: opening cutscene (event 12410000, 2002[%d]) skipped in the host's world", id);
    }
    return true;
}

/// Recovery net (any role, own world only): a player that keeps falling for 6 s (an endless fall
/// out of the map after a bad saved position - the game's own death plane did not catch it) is
/// warped to its own last lamp (0x13CDF30 with the low dword of WorldTransitionState +0x1528).
void FallRecoveryTick(const GameSnapshot& s, double now) {
    static float last_y = 0;
    static double last_t = 0, falling_since = 0;
    static u64 last_player = 0;
    if (!s.world_up || s.loading || !s.player || Field<u8>(Slot(kSlotGsm), 0x1592) != 1) {
        falling_since = 0;
        last_player = 0;
        return;
    }
    if (now - last_t < 1.0) {
        return;
    }
    const u64 pm = Field<u64>(Field<u64>(Field<u64>(Field<u64>(s.player, 0x58), 8), 0x3b0), 0x68);
    float p[4];
    if (!pm || !SafeRead(pm + 0x1e0, p, 16)) {
        return;
    }
    const float dy = last_player == s.player ? last_y - p[1] : 0.0f;
    last_y = p[1];
    last_t = now;
    last_player = s.player;
    if (dy > 1.5f) {
        if (falling_since == 0) {
            falling_since = now;
        } else if (now - falling_since >= 6.0) {
            falling_since = 0;
            // A stage request already pending (a death redirect's lamp warp): that one wins.
            if (Field<u8>(Slot(kSlotGsm), 8) != 0) {
                Log("recovery: falling at (%.1f, %.1f, %.1f), a stage request is pending; left to it", p[0], p[1], p[2]);
                return;
            }
            // A party guest goes to the host's last lamp (the gather / death-redirect lamp): its own
            // last lamp is often the Hunter's Dream, and a bell rung there joins the host's world at
            // Dream coordinates (live r5: fell in the arena, recovered to the Dream, re-summoned
            // into the host's m24_01 with stage 0x15000000 placement).
            const u32 own = Field<u32>(Slot(kSlotGsm), 0x1528);
            const u32 host = GuestDeathRedirectLamp();
            const bool use_host = bb_party_enabled() && host != kTravelNone && host != 0;
            const u32 lamp = use_host ? host : own;
            Log("recovery: falling for 6 s at (%.1f, %.1f, %.1f); warping to %s last lamp %u", p[0], p[1], p[2],
                use_host ? "the host's" : "the own", lamp);
            if (lamp && lamp != 0xffffffffu && WarpToLampInWorld(lamp, false, "fall recovery")) {
                return;  // the lamp is in this map: no load
            }
            if (lamp && lamp != 0xffffffffu) {
                using Fn = void(BB_COOP_SYSV*)(u32);
                reinterpret_cast<Fn>(Guest(0x13cdf30))(lamp);
            }
        }
    } else {
        falling_since = 0;
    }
}

/// Any failed or timed-out B2 step on a guest: hand the host's travel to B1 (party_travel): the
/// guest warps to the destination in its own world (the funnel leaves the session then, as
/// vanilla) and the director's bells re-summon it. Never leaves the guest stranded.
extern std::atomic<bool> g_dead_follow;
extern std::atomic<bool> g_solo_respawn;
bool BeginRejoin(const TravelIntent& t, bool main_thread, bool solo);

void FallbackToB1(const char* why) {
    State& st = S();
    TravelIntent t;
    bool have;
    {
        std::lock_guard<std::mutex> lk(st.mu);
        have = st.have_intent;
        t = st.intent;
        st.gphase = GuestPhase::Idle;
        st.solo = false;
        st.enter_state1 = false;
    }
    if (st.saved_1521 >= 0) {
        const u8 v = u8(st.saved_1521);
        SafeWrite(Slot(kSlotGsm) + 0x1521, &v, 1);
        st.saved_1521 = -1;
    }
    DropLoadingScreen(why);
    g_rejoin = false;
    const u64 flow = Slot(kSlotFlow);
    const int fs = Field<int>(flow, 0x84, -1);
    if (fs >= 1 && fs <= 3) {
        const int four = 4;
        SafeWrite(flow + 0x84, &four, 4);
    }
    g_fell_back = true;
    if (g_solo_respawn.exchange(false)) {
        // A lone death's respawn that failed (no snapshot): the old death redirect - the host's
        // last lamp in the own world, a leave, the bell (live r20: a plain B1 replay of the
        // synthetic intent was dropped as "not newer" and the guest stayed dead in the host world).
        g_dead_follow = false;
        Log("guest: FALLBACK (%s; flow was %d): the lone death's respawn failed; the usual death redirect runs", why,
            fs);
        GuestDeathRedirectNow();
        return;
    }
    Log("guest: FALLBACK to B1 (%s; flow was %d)%s", why, fs, have ? "" : " - no travel to replay");
    if (have) {
        RequestGuestTravel(t);
    }
}

/// After a B1 fallback the guest leaves the room through the funnel, but the network flow keeps the
/// old member list (+0x68 with the host's GameData slot): the next join's member event 0xE then
/// finds the host "already known" and creates no task, and the join is left at once (run 26).
/// Once the session is idle in the world, run the flow's own end-of-session cleanup 0x178A900
/// (member list, GameData rows, task returns) once.
void FallbackCleanupTick(const GameSnapshot& s) {
    if (!g_fell_back.load() || Role() != RoleIdle || !s.world_up || s.loading) {
        return;
    }
    g_fell_back = false;
    const u64 flow = Slot(kSlotFlow);
    u64 n = 0;
    const u64 head = Field<u64>(flow, 0x68);
    for (u64 node = Field<u64>(head, 0); head && node && node != head && n < 8; node = Field<u64>(node, 0)) {
        ++n;
    }
    const u8 zero = 0;
    SafeWrite(flow + 0xc5, &zero, 1);  // set by every MoveMapStep ctor; 0x178A900 defers while it is set
    using Fn = void(BB_COOP_SYSV*)(u64);
    reinterpret_cast<Fn>(Guest(0x178a900))(flow);
    Log("guest: after the fallback, flow cleanup 0x178A900 ran (%llu stale member(s), flow now %d); tasks:%s", ull(n),
        Field<int>(flow, 0x84, -1), DescribeTasks().c_str());
}

// A guest's own death in the host's world (PartyGhostDeath_2's funnel call) is held: when the
// host dies too (a wipe, a fall) the host's travel event follows within seconds and B2 brings
// the guest along with it - one loading screen, still in the session. Before this hold the
// guest's death went first (redirect to the host's lamp in its own world = a leave), the room
// was gone and the bell had to re-summon it (live r5). With no host travel within the grace
// (BB_PARTY_B2_DEATH_GRACE s, default 8; 0 = no hold) the usual redirect runs.
std::atomic<double> g_death_held_at{0};
// The guest is dead and follows the host's travel: its load is a death load, which goes to its
// own respawn records (GSM +0x1528 last lamp, +0x1538 mode, +0x153C respawn: often the Hunter's
// Dream) whatever the forced placement says (live r11: loaded m21_00 in host-world mode, 142 m
// from the host). The host's records are written for that load and the own ones put back after.
std::atomic<bool> g_dead_follow{false};
std::atomic<bool> g_solo_respawn{false};  // the dead follow is a lone death's respawn (stay at the lamp)
struct OwnRespawn {
    bool saved = false;
    u64 last_lamp = 0, respawn = 0;
    u32 mode = 0;
} g_own_respawn;

double DeathGrace() {
    static const double g = [] {
        const char* v = std::getenv("BB_PARTY_B2_DEATH_GRACE");
        return v && v[0] ? std::atof(v) : 8.0;
    }();
    return g;
}

bool HoldGuestDeath() {
    if (!g_on.load() || Role() != RoleClient || DeathGrace() <= 0) {
        return false;
    }
    GuestPhase phase;
    {
        std::lock_guard<std::mutex> lk(S().mu);
        phase = S().gphase;
    }
    g_death_held_at = Now();
    if (phase != GuestPhase::Idle) {
        g_dead_follow = true;
    }
    Log("guest: own death in the host's world held (%s)",
        phase != GuestPhase::Idle ? "the host's travel is in flight: it takes the guest along"
                                  : "waiting for a host travel");
    return true;
}

/// The host's lamp as a travel intent: ReturnPointParam row via 0x1F26B40(out, id) (out +0x10 =
/// row: +0 area, +1 block, +4 return point entity, +8 aux), the record 0x13CDF30 writes to
/// GSM +0x153C ({+4, +8}).
bool LampIntent(u32 id, TravelIntent* t) {
    alignas(16) u8 out[0x20] = {};
    reinterpret_cast<void(BB_COOP_SYSV*)(void*, u32)>(Guest(0x1f26b40))(out, id);
    u64 row = 0;
    std::memcpy(&row, out + 0x10, 8);
    if (!row) {
        return false;
    }
    const u8 area = Field<u8>(row, 0), block = Field<u8>(row, 1);
    const u64 rec = u64(Field<u32>(row, 4)) | (u64(Field<u32>(row, 8)) << 32);
    t->kind = TravelKind::Lamp;
    t->lamp_id = id;
    (void)block;
    // Lamp ids are AABnnnn (2412952: m24_01): the row's own map bytes did not read back right.
    t->packed_map = (id / 100000) << 24 | ((id / 10000) % 10) << 16;
    t->respawn_mode = id / 100000 == 21 ? 2 : 1;
    (void)area;
    t->respawn_record = rec;
    t->last_lamp = rec;
    return true;
}

/// ER-Seamless-style respawn of a guest that died alone (BB_PARTY_B2_SOLO_RESPAWN, default on):
/// still a client, it asks the host for a fresh snapshot ("respawn") and loads the host's world
/// again as a dead follow (the host's last-lamp records written before the death load), i.e. it
/// respawns at the host's last lamp in the host's world. No leave, no bell. False: not possible
/// (no session, no host lamp, off) - the caller runs the old redirect.
/// A lit lamp of the map the guest stands in (the host's world): the ReturnPointParam ids AAB295k
/// of that map whose lamp flag 1AAB7800 + 20k (2009[5] registration, party_flags.inc) is set in
/// the host-world overlay. The first one (lowest k); 0 if none.
u32 LitLampInMap(u32 map) {
    const u64 man = Slot(0x553b100);
    if (!man) {
        return 0;
    }
    using Get = u32(BB_COOP_SYSV*)(u64, u32, u32);
    u32 first = 0;
    std::string seen;
    for (u32 id : ids::kPartyReturnPoints) {
        if (ids::EntityMap(id) != (map & 0xffff0000u) || id % 10000 / 10 != 295) {
            continue;
        }
        // "Lamp lit": common event 7100's flag for lamp object AAB195k is 7AAB0200 + k (m24_01:
        // 72410200..203 for 2411950..953, from the EMEVD args), a T7 group the host-world mirror
        // (party_progress world_groups) copies into the guest's overlay. (The 2009[5] registration
        // flags 1AAB7800 + 20k read 0 here: T1 zone 7 is not mirrored.)
        const u32 flag = 70000000u + id / 10000 * 10000 + 200 + (id % 10);
        const u32 lit = reinterpret_cast<Get>(Guest(0x13cfd80))(man, flag, 1);
        seen += " " + std::to_string(id) + (lit ? ":lit" : ":-");
        if (lit) {
            Log("guest: lamps of %s:%s", MapName(map).c_str(), seen.c_str());
            return id;
        }
        if (!first) {
            first = id;
        }
    }
    // None reads lit here (the host-world overlay may not carry these flags): a lamp of this map
    // all the same - any ReturnPointParam row of the map is a valid respawn spot in it.
    Log("guest: lamps of %s:%s; none read lit, using %u", MapName(map).c_str(), seen.c_str(), first);
    return first;
}

bool SoloRespawn(double held_for, u32 map_now) {
    static const bool on = !(std::getenv("BB_PARTY_B2_SOLO_RESPAWN") && std::getenv("BB_PARTY_B2_SOLO_RESPAWN")[0] == '0');
    u32 lamp = GuestDeathRedirectLamp();
    if (on && lamp != kTravelNone && lamp != 0 && map_now != 0xffffffffu &&
        ids::EntityMap(lamp) != (map_now & 0xffff0000u)) {
        // The host's last lamp is in another map than the one the party is in (a stage warp, a
        // walk into the next area): a respawn there would load another map in host-world mode
        // (the Dream case of 6.8). A lit lamp of the current map instead.
        const u32 here = LitLampInMap(map_now);
        Log("guest: the host's last lamp %u is not in %s; lit lamp here: %u", lamp, MapName(map_now).c_str(), here);
        lamp = here;
    }
    TravelIntent t;
    if (!on || Role() != RoleClient || lamp == kTravelNone || lamp == 0 || !LampIntent(lamp, &t)) {
        return false;
    }
    g_dead_follow = true;
    g_solo_respawn = true;
    if (!BeginRejoin(t, true, false)) {
        g_dead_follow = false;
        g_solo_respawn = false;
        return false;
    }
    {
        // Where it died: the host removes the member chr standing there (the remote chrs' HP does
        // not tell the dead one apart: live r19 read hp 1 on both guests and removed both).
        float p[3] = {0, 0, 0};
        ChrPos(Field<u64>(Slot(kSlotWorldChrMan), 0x60), p);
        char b[128];
        std::snprintf(b, sizeof b, "{\"op\":\"respawn\",\"pos\":[%.3f,%.3f,%.3f]}", p[0], p[1], p[2]);
        Send(0, b);
    }
    Log("guest: died alone (no host travel in %.1f s): respawn at the host's lamp %u in the host's world (%s); "
        "asked the host for a snapshot",
        held_for, lamp, DescribeTravel(t).c_str());
    return true;
}

/// Main thread: a held guest death goes with the host's travel, or runs after the grace.
void HeldDeathTick(const GameSnapshot& s, double now) {
    if (g_dead_follow.load() && !SessionUp()) {
        // The follow ended without an arrival (B1 fallback): the host's records stay, which is
        // where the fallback and the gather take the guest anyway.
        g_dead_follow = false;
        g_solo_respawn = false;
        g_own_respawn.saved = false;
    }
    const double at = g_death_held_at.load();
    if (at <= 0) {
        return;
    }
    GuestPhase phase;
    {
        std::lock_guard<std::mutex> lk(S().mu);
        phase = S().gphase;
    }
    if (phase != GuestPhase::Idle) {
        g_death_held_at = 0;
        g_dead_follow = true;
        Log("guest: held death goes with the host's travel (%.1f s after it); no redirect, no leave", now - at);
        return;
    }
    if (now - at < DeathGrace() || s.loading) {
        return;
    }
    g_death_held_at = 0;
    if (SoloRespawn(now - at, s.map_id)) {
        return;
    }
    Log("guest: no host travel %.1f s after its death; the usual death redirect runs", now - at);
    GuestDeathRedirectNow();
}

void GuestTick(const GameSnapshot& s, double now) {
    State& st = S();
    const u64 flow = Slot(kSlotFlow);
    const int fstate = Field<int>(flow, 0x84, -1);
    GuestPhase phase;
    {
        std::lock_guard<std::mutex> lk(st.mu);
        phase = st.gphase;
    }
    switch (phase) {
    case GuestPhase::Idle: {
        u32 lamp = 0;
        {
            std::lock_guard<std::mutex> lk(st.mu);
            std::swap(lamp, st.warp_lamp);
        }
        if (lamp && s.world_up && !s.loading) {
            if (WarpToLampInWorld(lamp, true, "the host's party move")) {
                return;
            }
            // Not in this block (apart): the party move brings it along - one load, its own.
            TravelIntent t;
            if (LampIntent(lamp, &t) && TravelAlone(t, AloneWhy::Lamp)) {
                return;
            }
            Log("guest: could not follow the host's in-world move to lamp %u", lamp);
        }
        return;
    }
    case GuestPhase::InWorldFollow: {
        // The host loads a map this guest already stands in: no load here. Once the host is in
        // (its "dest"), stand beside it, drop the snapshot packets it sent, say "arrived" (the host
        // re-creates this guest's chr; the host's own chr here follows its new PlayerIns).
        bool have_dest;
        float pos[4], rot[4];
        {
            std::lock_guard<std::mutex> lk(st.mu);
            have_dest = st.have_dest;
            std::memcpy(pos, st.dest_pos, sizeof pos);
            std::memcpy(rot, st.dest_rot, sizeof rot);
        }
        if (have_dest && s.world_up && !s.loading) {
            DrainSnapshotQueue("in-world follow");
            BesideHost(pos, rot, g_local_slot.load());
            WarpLocal(pos, rot[1], "follow the host's load into this map");
            SendDeadEnemies(s, "in-world follow");
            const int n = RewindRemoteTasks("guest in-world follow");
            Send(0, "{\"op\":\"arrived\"}");
            Log("guest: followed the host inside the loaded map (%s): no load; %d task(s) rewound",
                MapName(s.map_id).c_str(), n);
            g_rejoin = false;
            std::lock_guard<std::mutex> lk(st.mu);
            st.gphase = GuestPhase::Idle;
        } else if (now - st.since > 60.0) {
            g_rejoin = false;
            Log("guest: in-world follow: no host destination within 60 s; staying");
            std::lock_guard<std::mutex> lk(st.mu);
            st.gphase = GuestPhase::Idle;
        }
        return;
    }
    case GuestPhase::AwayLoading: {
        // Split travel: this guest's alone load (flow 3 -> the game's 4 at the end of the load).
        const bool loading = s.loading || !s.world_up;
        st.load_seen = st.load_seen || loading;
        st.stable = (!loading && st.load_seen) ? st.stable + 1 : 0;
        if (st.stable >= 30 && fstate == 4) {
            if (st.saved_1521 >= 0) {
                const u8 v = u8(st.saved_1521);
                SafeWrite(Slot(kSlotGsm) + 0x1521, &v, 1);
                st.saved_1521 = -1;
            }
            g_rejoin = false;
            g_dead_follow = false;
            Log("guest: arrived alone in %s (host in %s), still a client (role %d); %s; tasks:%s",
                MapName(s.map_id).c_str(), MapName(HostMap()).c_str(), Role(), DescribeChr(s.player).c_str(),
                DescribeTasks().c_str());
            std::lock_guard<std::mutex> lk(st.mu);
            st.gphase = GuestPhase::Idle;
        } else if (now - st.since > 90.0) {
            if (fstate == 3) {
                const int four = 4;
                SafeWrite(flow + 0x84, &four, 4);
            }
            g_rejoin = false;
            Log("guest: the alone load did not finish within 90 s (flow %d, %s)", fstate, loading ? "loading" : "in world");
            std::lock_guard<std::mutex> lk(st.mu);
            st.gphase = GuestPhase::Idle;
        }
        return;
    }
    case GuestPhase::AwaitSnapshot:
        if (st.enter_state1) {
            // A follow taken on the link thread: flow state 1 from the main thread, after the
            // stale snapshot packets are gone (split travel; DrainSnapshotQueue).
            st.enter_state1 = false;
            DrainSnapshotQueue("the host travels");
            const int one = 1;
            const float zero = 0.0f;
            SafeWrite(flow + 0x84, &one, 4);
            SafeWrite(flow + 0x108, &zero, 4);
            return;
        }
        if (!st.screen_done) {
            st.screen_done = true;
            SaveOverlay();
            st.hp_before = Field<int>(Field<u64>(Field<u64>(s.player, 0x3b0), 0x20), 0xf8, -1);
            if (st.hp_before <= 0) {
                // Dead at the host's travel (a held death): back with full HP, as the host respawns.
                st.hp_before = Field<int>(Field<u64>(Field<u64>(s.player, 0x3b0), 0x20), 0xfc, -1);
                Log("guest: dead at the host's travel; full HP (%d) after the load", st.hp_before);
            }
            ApplyTestSpEffects(s.player);
            // A dead guest's buffs end with the death, as in vanilla (none restored after the load).
            st.buffs_before = g_dead_follow.load() ? std::vector<std::pair<int, float>>{} : TimedSpEffects(s.player);
            LogGuestState(s, "at the host's travel");
            static const bool early = !(std::getenv("BB_PARTY_B2_EARLY_SCREEN") &&
                                        std::getenv("BB_PARTY_B2_EARLY_SCREEN")[0] == '0');
            if (early) {
                RaiseLoadingScreen();
            }
        }
        bool have_dest;
        {
            std::lock_guard<std::mutex> lk(st.mu);
            have_dest = st.have_dest;
        }
        if (fstate == 2 && !st.blobs_saved) {
            st.blobs_saved = true;
            SaveHostBlobs();
        }
        if (fstate == 2 && have_dest) {
            // The game applied the host's 0x11 world snapshot and 0x10 flags (+0x84 = 2; K6 ate
            // the Lua join start). Load the host's destination in host-world mode.
            TravelIntent t;
            {
                std::lock_guard<std::mutex> lk(st.mu);
                t = st.intent;
                st.gphase = GuestPhase::Loading;
                st.since = now;
                st.load_seen = false;
                st.stable = 0;
            }
            const int three = 3;
            SafeWrite(flow + 0x84, &three, 4);  // the setter 0x178D9A0's write
            bool solo;
            {
                std::lock_guard<std::mutex> lk(st.mu);
                solo = st.solo;
            }
            if (solo) {
                // Split travel: the guest's own destination (its lamp), not the host's spot: the
                // summoned placement (GSM +0x1521 set by the join) would put it beside the host.
                const u64 gsm = Slot(kSlotGsm);
                const u8 was = Field<u8>(gsm, 0x1521), zero = 0;
                if (was) {
                    st.saved_1521 = was;
                    SafeWrite(gsm + 0x1521, &zero, 1);
                }
                {
                    // The host-world load takes its stage from the summoned placement +0x14C4 (live
                    // split10: still the Dream's after an alone load, so the return to m24_01
                    // landed in m21): the lamp's own spot goes there.
                    const std::uint32_t lamp = t.lamp_id != kTravelNone && t.lamp_id ? t.lamp_id
                                                                                     : static_cast<std::uint32_t>(t.last_lamp);
                    float spot[4];
                    if (lamp / 100000 != 29 && ids::ReturnSpot(lamp, spot)) {
                        const u32 stage = ids::EntityMap(lamp);
                        alignas(16) float p16[4] = {spot[0], spot[1], spot[2], 1.0f};
                        alignas(16) float r16[4] = {0.0f, spot[3], 0.0f, 0.0f};
                        SafeWrite(gsm + 0x14c4, &stage, 4);
                        SafeWrite(gsm + 0x14d0, p16, 16);
                        SafeWrite(gsm + 0x14e0, r16, 16);
                        t.kind = TravelKind::Transform;
                        t.has_pos = true;
                        t.pos_map = stage;
                        std::memcpy(t.pos, p16, sizeof t.pos);
                        std::memcpy(t.rot, r16, sizeof t.rot);
                    }
                }
                RestoreZeroedOverlay();
                coop::progress::OnVanillaSnapshotApplied();
                FunnelKeepRoom keep;
                Log("guest: host snapshot applied; travelling alone to %s in host-world mode (K7 %s, GSM +0x1521 %u)",
                    DescribeTravel(t).c_str(), keep.armed ? "armed" : "NOT armed: the room will close", unsigned(was));
                if (!ReplayTravelNow(t)) {
                    FallbackToB1("no usable destination");
                }
                return;
            }
            {
                // A guest's loads use the summoned placement (GSM +0x1521 set by the join):
                // +0x14C4 stage, +0x14D0 position, +0x14E0 rotation - the host's, 1.5 m aside.
                std::lock_guard<std::mutex> lk(st.mu);
                const u64 gsm = Slot(kSlotGsm);
                float pos[4];
                std::memcpy(pos, st.dest_pos, sizeof pos);
                BesideHost(pos, st.dest_rot, g_local_slot.load());
                SafeWrite(gsm + 0x14c4, &st.dest_stage, 4);
                SafeWrite(gsm + 0x14d0, pos, 16);
                SafeWrite(gsm + 0x14e0, st.dest_rot, 16);
                Log("guest: summoned placement = stage %#x pos (%.1f, %.1f, %.1f), GSM +0x1521 = %u", st.dest_stage,
                    pos[0], pos[1], pos[2], unsigned(Field<u8>(gsm, 0x1521)));
            }
            {
                // Written once, before the load: an exact transform (forced placement +0x14F0 map,
                // +0x1500 position, +0x1510 rotation, +0x1520) beside the host's arrival spot.
                std::lock_guard<std::mutex> lk(st.mu);
                if (SummonOffset() > 0.0f) {
                    t.kind = TravelKind::Transform;
                    t.has_pos = true;
                    t.pos_map = st.dest_stage;
                    std::memcpy(t.pos, st.dest_pos, sizeof t.pos);
                    BesideHost(t.pos, st.dest_rot, g_local_slot.load());
                    std::memcpy(t.rot, st.dest_rot, sizeof t.rot);
                }
            }
            if (g_dead_follow.load()) {
                const u64 gsm = Slot(kSlotGsm);
                OwnRespawn& o = g_own_respawn;
                o.saved = gsm && SafeGet(gsm + 0x1528, &o.last_lamp) && SafeGet(gsm + 0x1538, &o.mode) &&
                          SafeGet(gsm + 0x153c, &o.respawn);
                if (o.saved) {
                    const u64 lamp = t.last_lamp != ~0ull ? t.last_lamp : o.last_lamp;
                    const u64 resp = t.respawn_record != ~0ull ? t.respawn_record : lamp;
                    const u32 mode = t.respawn_mode ? t.respawn_mode : 1;
                    SafeWrite(gsm + 0x1528, &lamp, 8);
                    SafeWrite(gsm + 0x1538, &mode, 4);
                    SafeWrite(gsm + 0x153c, &resp, 8);
                    Log("guest: dead, follows the host: respawn records = the host's (last lamp %#llx -> %#llx, "
                        "mode %u -> %u, respawn %#llx -> %#llx)",
                        ull(o.last_lamp), ull(lamp), o.mode, mode, ull(o.respawn), ull(resp));
                }
            }
            RestoreZeroedOverlay();
            coop::progress::OnVanillaSnapshotApplied();  // the host's image wins over the saved copy
            LogIntroFlags("before the host-world load");
            FunnelKeepRoom keep;
            Log("guest: host snapshot applied; loading %s in host-world mode (K7 %s)", DescribeTravel(t).c_str(),
                keep.armed ? "armed" : "NOT armed: the room will close");
            if (!ReplayTravelNow(t)) {
                FallbackToB1("no usable destination");
            }
            return;
        }
        if (now - st.since > 45.0) {  // the flow leaves by itself after 60 s in states 1-3
            FallbackToB1(fstate == 2 ? "no host destination within 45 s" : "no host snapshot within 45 s");
        }
        return;
    case GuestPhase::Loading: {
        const bool loading = s.loading || !s.world_up;
        st.load_seen = st.load_seen || loading;
        st.stable = (!loading && st.load_seen) ? st.stable + 1 : 0;
        if (st.stable >= 30 && fstate == 4) {
            g_early_screen = false;  // the load's own InGameStart cleared it
            {
                // The guest's state is not the load's business (no rest): its HP as before the
                // host's travel (a load may refill it). Data module: chr +0x3B0 -> +0x20: HP +0xf8,
                // max +0xFC.
                const u64 dm = Field<u64>(Field<u64>(s.player, 0x3b0), 0x20);
                const int hp = Field<int>(dm, 0xf8, -1), mx = Field<int>(dm, 0xfc, -1);
                if (RestedFromTravel(st.intent)) {
                    Log("guest: HP %d after the load (a rest travel: the refill heals, max %d)", hp, mx);
                } else if (dm && st.hp_before > 0 && hp >= 0 && hp != st.hp_before) {
                    const int keep = st.hp_before < mx ? st.hp_before : mx;
                    SafeWrite(dm + 0xf8, &keep, 4);
                    Log("guest: HP %d after the load -> %d as before the travel (max %d)", hp, keep, mx);
                } else {
                    Log("guest: HP %d after the load (before %d, max %d)", hp, st.hp_before, mx);
                }
            }
            LogGuestState(s, "after the B2 load");
            RestoreBuffs(s.player, st.buffs_before);
            st.buffs_before.clear();
            if (!TestSpEffects().empty()) {
                LogGuestState(s, "after the buff restore");
            }
            if (st.saved_1521 >= 0) {
                const u8 v = u8(st.saved_1521);
                SafeWrite(Slot(kSlotGsm) + 0x1521, &v, 1);
                st.saved_1521 = -1;
            }
            {
                std::lock_guard<std::mutex> lk(st.mu);
                if (st.solo) {
                    Log("guest: arrived alone at %s in the host's area (%s)", DescribeTravel(st.intent).c_str(),
                        DescribeChr(s.player).c_str());
                }
                st.solo = false;
            }
            const int n = RewindRemoteTasks("guest");
            Log("guest: arrived in the host world (flow 4); %d remote task(s) rewound", n);
            if (g_dead_follow.exchange(false) && g_own_respawn.saved) {
                const u64 gsm = Slot(kSlotGsm);
                OwnRespawn& o = g_own_respawn;
                SafeWrite(gsm + 0x1528, &o.last_lamp, 8);
                SafeWrite(gsm + 0x1538, &o.mode, 4);
                SafeWrite(gsm + 0x153c, &o.respawn, 8);
                o.saved = false;
                Log("guest: own respawn records put back after the death follow (map now %s)", MapName(s.map_id).c_str());
                // A death load lands on the lamp spot itself (the forced placement is ignored):
                // the guest stood inside the host (live r13: 0.00 m, the host pushed). Beside it.
                float pos[4], rot[4];
                {
                    std::lock_guard<std::mutex> lk(st.mu);
                    std::memcpy(pos, st.dest_pos, sizeof pos);
                    std::memcpy(rot, st.dest_rot, sizeof rot);
                }
                if (g_solo_respawn.exchange(false)) {
                    Log("guest: respawned at the host's lamp in the host's world (%s), still a client",
                        DescribeChr(s.player).c_str());
                } else if (SummonOffset() > 0.0f && s.player) {
                    BesideHost(pos, rot, g_local_slot.load());
                    alignas(16) float p16[4] = {pos[0], pos[1], pos[2], 1.0f}, r16[4] = {rot[0], rot[1], rot[2], rot[3]};
                    using Warp = void(BB_COOP_SYSV*)(u64, const float*, const float*, u32);
                    if (const u64 fn = Field<u64>(Field<u64>(s.player, 0), 0x5f8)) {
                        reinterpret_cast<Warp>(fn)(s.player, p16, r16, 1);
                        Log("guest: placed beside the host after the death follow (%.2f, %.2f, %.2f)", double(pos[0]),
                            double(pos[1]), double(pos[2]));
                    }
                }
            }
            Send(0, "{\"op\":\"arrived\"}");  // guest: slot 0 = the host
            std::lock_guard<std::mutex> lk(st.mu);
            st.gphase = GuestPhase::Rewinding;
            st.since = now;
        } else if (now - st.since > 90.0) {
            FallbackToB1("host-world load not finished within 90 s");
        }
        return;
    }
    case GuestPhase::Rewinding: {
        bool all = true;
        for (const TaskInfo& t : Tasks(false)) {
            if ((t.mode == 1 || t.mode == 2) && !PlayerForPeer(t.task + kTaskPeer)) {
                all = false;
            }
        }
        if (all || now - st.since > 60.0) {
            const u64 remo = Field<u64>(Slot(0x5540058), 8);
            Log("guest: re-join %s after %.1f s (SprjRemo %d); tasks:%s", all ? "done" : "INCOMPLETE",
                now - st.since, remo ? int(Field<u8>(remo, 0x168) & 1) : -1, DescribeTasks().c_str());
            if (!all) {
                FallbackToB1("host chr not re-created within 60 s");
                return;
            }
            g_rejoin = false;
            std::lock_guard<std::mutex> lk(st.mu);
            st.gphase = GuestPhase::Idle;
        }
        return;
    }
    }
}

/// Host: where it stands, to the guests (their summoned placement / arrival spot).
void SendDest(const GameSnapshot& s) {
    const u64 a = Field<u64>(Field<u64>(Field<u64>(Field<u64>(s.player, 0x58), 8), 0x3b0), 0x68);
    float pos[4] = {}, rot[4] = {};
    if (a && SafeRead(a + 0x1e0, pos, 16) && SafeRead(a + 0x1d0, rot, 16)) {
        char b[320];
        std::snprintf(b, sizeof b,
                      "{\"op\":\"dest\",\"stage\":%u,\"pos\":[%.4f,%.4f,%.4f,%.4f],\"rot\":[%.6f,%.6f,%.6f,%.6f]}",
                      Field<u32>(s.player, 0x3f8), pos[0], pos[1], pos[2], pos[3], rot[0], rot[1], rot[2], rot[3]);
        Send(-1, b);
        Log("host: destination sent: %s", b);
    }
}

void HostTick(const GameSnapshot& s, double now) {
    State& st = S();
    const bool loading = s.loading || !s.world_up;
    if (Role() != RoleHost) {
        if (st.hphase != HostPhase::Idle && Role() != RoleClient) {
            // The room ended during a re-join (a member's respawn, a travel): nothing of it
            // carries over into the next room (r23: g_rejoin stayed set across two later rooms).
            g_rejoin = false;
            Log("host: the session ended during a re-join; re-join state cleared");
        }
        st.hphase = HostPhase::Idle;
        return;
    }
    bool respawn = false, away = false, away_have = false, live = false;
    float apos[3] = {};
    {
        std::lock_guard<std::mutex> lk(st.mu);
        std::swap(away, st.away_request);
        away_have = st.away_have_pos;
        std::memcpy(apos, st.away_pos, sizeof apos);
        if (!loading) {
            std::swap(respawn, st.respawn_request);
            live = st.respawn_live;
        }
    }
    if (away) {
        // A guest travels alone into another area (split travel): its chr here goes now (it would
        // stand frozen, then follow coordinates of another map); the task is kept.
        const u64 c = away_have ? ChrNear(apos) : 0;
        if (c) {
            RemoveRemoteChr(c, "the guest travels alone");
        }
        Log("host: a guest travels alone (%s); tasks:%s", c ? "its chr removed" : "no chr of it here", DescribeTasks().c_str());
    }
    if (respawn && live) {
        Log("host: a guest travels alone into this area (split travel): the snapshot again, as for a lone respawn");
    }
    if (respawn && !loading) {
        // A guest died alone and reloads this world (SoloRespawn): the join snapshot again and
        // where the host stands; its chr is re-created on "arrived" as after a host travel.
        Log("host: a guest respawns in this world; tasks:%s", DescribeTasks().c_str());
        // Its dead chr stays here otherwise (live r16: hp 1 at the death spot after the guest stood
        // at the lamp, 0 tasks rewound): removed as the task's own STEP_DeleteWait 0x1E50A70 does
        // (slot release 0x15BC080, WorldChrMan remove 0x19186E0); the task is kept and rewound to
        // STEP_Create on "arrived", which builds the chr again from the member's GameData row.
        bool have_pos;
        float dpos[3];
        {
            std::lock_guard<std::mutex> lk(st.mu);
            have_pos = st.respawn_have_pos;
            std::memcpy(dpos, st.respawn_pos, sizeof dpos);
        }
        // The member chr nearest to where the guest died (within 6 m); without a position, a lone
        // member's chr.
        u64 victim = 0;
        float best = 36.0f;
        int members = 0;
        for (const TaskInfo& t : Tasks(false)) {
            const u64 c = t.mode == 1 ? PlayerForPeer(t.task + kTaskPeer) : 0;
            if (!c) {
                continue;
            }
            ++members;
            float p[3];
            if (have_pos && ChrPos(c, p)) {
                const float d = (p[0] - dpos[0]) * (p[0] - dpos[0]) + (p[1] - dpos[1]) * (p[1] - dpos[1]) +
                                (p[2] - dpos[2]) * (p[2] - dpos[2]);
                if (d < best) {
                    best = d;
                    victim = c;
                }
            } else if (!have_pos) {
                victim = c;
            }
        }
        if (!have_pos && members != 1) {
            victim = 0;
        }
        for (const TaskInfo& t : Tasks(false)) {
            if (t.mode != 1) {
                continue;
            }
            const u64 chr = PlayerForPeer(t.task + kTaskPeer);
            const int hp = Field<int>(Field<u64>(Field<u64>(chr, 0x3b0), 0x20), 0xf8, -1);
            if (!chr || chr != victim) {
                continue;
            }
            using Release = u64(BB_COOP_SYSV*)(u64, u32);
            using Remove = void(BB_COOP_SYSV*)(u64, u64);
            reinterpret_cast<Release>(Guest(0x15bc080))(Field<u64>(Slot(kSlotGsm), 0x16f8), Field<u32>(chr, 8));
            reinterpret_cast<Remove>(Guest(0x19186e0))(Slot(kSlotWorldChrMan), chr);
            Log("host: dead member chr %#llx (hp %d) removed; its task is kept for the re-creation", ull(chr), hp);
        }
        static const bool fail = std::getenv("BB_PARTY_B2_TEST_FAIL") &&
                                 std::strcmp(std::getenv("BB_PARTY_B2_TEST_FAIL"), "resync") == 0;
        for (u64 id : Members()) {
            if (fail) {
                Log("test fail: join snapshot NOT re-sent (respawn)");
                continue;
            }
            ResyncMember(id);
        }
        SendDest(s);
        st.hphase = HostPhase::Resynced;
        st.hsince = now;
        g_rejoin = true;
        std::lock_guard<std::mutex> lk(st.mu);
        st.arrived = false;
        return;
    }
    switch (st.hphase) {
    case HostPhase::Idle:
        if (loading) {
            st.hphase = HostPhase::Loading;
            st.hsince = now;
            st.stable = 0;
            g_rejoin = true;
            st.alone_load = (g_host_alone_next.exchange(false) && now - g_host_alone_at.load() < 10.0) ||
                            LastAloneCaptureAgo() < 10.0;
            Log("host: own load with a session; members keep the room%s",
                st.alone_load ? " (an alone travel: the guests stay where they are)" : "");
        }
        return;
    case HostPhase::Loading:
        st.stable = loading ? 0 : st.stable + 1;
        if (st.stable >= 30 && st.alone_load) {
            // Split travel: nobody follows; no snapshot (it would wait in every guest's receive
            // queue). Members already in this area are met without a load (SplitTick).
            st.alone_load = false;
            st.hphase = HostPhase::Idle;
            g_rejoin = false;
            Log("host: arrived alone in %s; tasks:%s", MapName(s.map_id).c_str(), DescribeTasks().c_str());
            return;
        }
        if (st.stable >= 30) {
            // BB_PARTY_B2_TEST_FAIL=resync: the host sends nothing (the guests' B1 fallback test).
            static const bool fail = std::getenv("BB_PARTY_B2_TEST_FAIL") &&
                                     std::strcmp(std::getenv("BB_PARTY_B2_TEST_FAIL"), "resync") == 0;
            for (u64 id : Members()) {
                if (fail) {
                    Log("test fail: join snapshot NOT re-sent");
                    continue;
                }
                ResyncMember(id);
            }
            // Where the host stands now: the guests' summoned placement (GSM +0x14C4/+0x14D0/
            // +0x14E0, what the descriptor builder 0x1943F10 uses while GSM +0x1521 is set).
            {
                const u64 a = Field<u64>(Field<u64>(Field<u64>(Field<u64>(s.player, 0x58), 8), 0x3b0), 0x68);
                float pos[4] = {}, rot[4] = {};
                if (a && SafeRead(a + 0x1e0, pos, 16) && SafeRead(a + 0x1d0, rot, 16)) {
                    char b[320];
                    std::snprintf(b, sizeof b,
                                  "{\"op\":\"dest\",\"stage\":%u,\"pos\":[%.4f,%.4f,%.4f,%.4f],\"rot\":[%.6f,%.6f,%.6f,%.6f]}",
                                  Field<u32>(s.player, 0x3f8), pos[0], pos[1], pos[2], pos[3], rot[0], rot[1], rot[2], rot[3]);
                    Send(-1, b);
                    Log("host: destination sent: %s", b);
                }
            }
            st.hphase = HostPhase::Resynced;
            st.hsince = now;
            st.rejoined_logged = false;
            std::lock_guard<std::mutex> lk(st.mu);
            st.arrived = false;
        }
        return;
    case HostPhase::Resynced: {
        if (loading) {
            // Another own load (a death right after a warp, live r8) while the last re-join still
            // waits: re-sync after this one, or the guests wait for a snapshot that never comes.
            st.hphase = HostPhase::Loading;
            st.hsince = now;
            st.stable = 0;
            g_rejoin = true;
            st.alone_load = (g_host_alone_next.exchange(false) && now - g_host_alone_at.load() < 10.0) ||
                            LastAloneCaptureAgo() < 10.0;
            Log("host: another own load before the last re-join settled; members keep the room, re-sync after it%s",
                st.alone_load ? " (alone: no re-sync)" : "");
            return;
        }
        bool arrived;
        {
            std::lock_guard<std::mutex> lk(st.mu);
            arrived = st.arrived;
            st.arrived = false;
        }
        const bool timeout = now - st.hsince > 60.0;
        if (arrived || timeout) {
            const int n = RewindRemoteTasks("host");
            Log("host: %s; %d member task(s) rewound", arrived ? "a guest arrived" : "60 s without arrival", n);
            if (timeout) {
                st.hphase = HostPhase::Idle;
                g_rejoin = false;
                return;
            }
        }
        // Done once every member task has its chr again (each guest's "arrived" rewinds it).
        bool all = true;
        for (const TaskInfo& t : Tasks(false)) {
            if (t.mode == 1 && !PlayerForPeer(t.task + kTaskPeer)) {
                all = false;
            }
        }
        if (all && (arrived || st.rejoined_logged)) {
            st.hphase = HostPhase::Idle;
            g_rejoin = false;
            Log("host: re-join done after %.1f s; tasks:%s", now - st.hsince, DescribeTasks().c_str());
        }
        st.rejoined_logged = st.rejoined_logged || arrived;
        return;
    }
    }
}

void InstallCall(u64 off, u64 target, const void* handler, const char* name) {
    if (!HookCallSite(off, target, handler, name)) {
        Log("%s: NOT installed (bytes differ)", name);
    }
}

} // namespace

bool Requested() {
    // Default in party mode (BB_PARTY set): B2, with B1 as the automatic fallback. BB_PARTY_B2=0: B1
    // only; BB_PARTY_B2=1 also without BB_PARTY (test fixtures).
    const char* v = std::getenv("BB_PARTY_B2");
    if (v && v[0]) {
        return EnvOn("BB_PARTY_B2");
    }
    return bb_party_enabled() != 0;
}

void SetLocalSlot(int slot) {
    g_local_slot = slot;
}

bool GuestPreRing(const GameSnapshot& s, std::uint32_t host_map) {
    return g_on.load() && PreRingSpacing(s, host_map);
}

bool HostHasMemberTasks() {
    for (const TaskInfo& t : Tasks(false)) {
        if (t.mode == 1) {
            return true;
        }
    }
    return false;
}

bool Enabled() {
    return g_on.load();
}

void Init() {
    if (!Requested() || g_on.load()) {
        return;
    }
    if (!Image()) {
        Log("no image; B2 off");
        return;
    }
    g_force = EnvOn("BB_PARTY_B2_FORCE");
    // K1: the shadp2p XML retarget (call 0x1ED00A0, "is a session up") already keeps the room.
    if (Matches(kDtorLeaveCall, {0xe8, 0xea, 0x8e, 0x58, 0x00})) {
        g_k1_xml = true;
        Log("K1: XML \"Keep session on map reload\" already retargets +0x%llx", ull(kDtorLeaveCall));
    } else {
        InstallCall(kDtorLeaveCall, kBeginLeave, reinterpret_cast<const void*>(&DtorLeaveHook), "B2 K1 unload leave");
    }
    InstallCall(kDtorResetCall, kMultiPlayReset, reinterpret_cast<const void*>(&ResetHook), "B2 K2 CSMultiPlayMan reset");
    InstallCall(kUidLeaveCall, kBeginLeave, reinterpret_cast<const void*>(&UidLeaveHook), "B2 K3 stage-UID leave");
    InstallCall(kUidDropCall, kDropPresentation, reinterpret_cast<const void*>(&UidDropHook), "B2 K4 stage-UID drop");
    InstallCall(kMatchCheckCall, kBeginLeave, reinterpret_cast<const void*>(&MatchCheckHook),
                "B2 K5 OnMatchingCheck leave");
    InstallCall(kJoinStartCall, kJoinStartLua, reinterpret_cast<const void*>(&JoinStartHook), "B2 K6 OnBeJoinStart");
    InstallCall(kHostDeathReturnCall, kRequestReturn, reinterpret_cast<const void*>(&HostDeathReturnHook),
                "B2 K8 host death return-all");
    InstallCall(0x13858f5, kBeginLeave, reinterpret_cast<const void*>(&BossClearLeaveHook), "B2 K10 boss-clear leave (a)");
    InstallCall(0x1385a95, kBeginLeave, reinterpret_cast<const void*>(&BossClearLeaveHook), "B2 K10 boss-clear leave (b)");
    // Not B2-specific (B1 bell joins stream too): installed whenever B2 is.
    if (!HookPrologue(0x18cb0e0, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81,
                                  0xec, 0xf8, 0x07, 0x00, 0x00},
                      &ChrPreStepHook, "remote chr stale map hit guard (0x18CB0E0)")) {
        Log("remote chr stale map hit guard: NOT installed (bytes differ)");
    }
    SetSendHomeVeto(&SendHomeVeto);
    SetGuestDeathHold(&HoldGuestDeath);
    // Every BeginLeaveSession with a session logs its caller (which game path ends a party).
    // push rbp; mov rbp, rsp; push r15 (6 bytes, position-independent).
    if (!ReplacePrologue(kBeginLeave, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57},
                         reinterpret_cast<const void*>(&LeaveEntryHook), &g_leave_orig, "B2 leave trace (0x1ED07A0)")) {
        g_leave_orig = nullptr;
    }
    InstallCall(0x17851ce, 0x13beca0, reinterpret_cast<const void*>(&FlagApplyHook), "B2 K11 flag snapshot apply");
    SetEmevdSkipPredicate(&ClientIntroSkip);
    EnsureEmevdFilter();
    Log("K7 funnel branch +0x%llx %s", ull(kFunnelLeaveJne),
        Matches(kFunnelLeaveJne, {0x75, 0x6c}) ? "verified (75 6c), written only around the guest's own warp"
                                                : "MISMATCH: guest re-join will leave the room");
    g_on = true;
    Log("on%s", g_force.load() ? " (BB_PARTY_B2_FORCE: K2 keeps tasks without a session)" : "");
}

/// A rewound task whose requested step never got applied (+0x54 != +0x50 for 2 s): the game's own
/// task update (0x1E556F0 from the flow update 0x1789B60) did not run it. Run one update of that
/// task ourselves, as 0x1E556F0 does: vfunc +0xD0 = 0x1E52FE0(task, &{0x52EFD40, dt}).
void KickStalledTasks(const GameSnapshot& s, double now) {
    static double pending_since = 0, last_kick = 0;
    if (!s.world_up || s.loading) {
        pending_since = 0;
        return;
    }
    bool stalled = false;
    for (const TaskInfo& t : Tasks(false)) {
        stalled = stalled || ((t.mode == 1 || t.mode == 2) && t.next == kStepCreate && t.step != kStepCreate);
    }
    if (!stalled) {
        pending_since = 0;
        return;
    }
    if (pending_since == 0) {
        pending_since = now;
        return;
    }
    const u64 remo = Field<u64>(Slot(0x5540058), 8);
    const bool remo_on = remo && (Field<u8>(remo, 0x168) & 1);
    // While SprjRemo's +0x168 bit 0 is set the flow update skips every task (0x1789B60); live run 8
    // showed it stuck at 1 on the guest after its host-world load (no cutscene visible), so after
    // 3 s we run the update ourselves anyway.
    if (now - pending_since < (remo_on ? 3.0 : 2.0) || now - last_kick < 1.0) {
        return;
    }
    last_kick = now;
    struct alignas(8) Dt {
        u64 vt;
        float dt;
    } dt{Guest(0x52efd40), 1.0f / 30.0f};
    for (const TaskInfo& t : Tasks(false)) {
        if ((t.mode == 1 || t.mode == 2) && t.next == kStepCreate && t.step != kStepCreate) {
            Log("kick: task mode %d stalled at step %d next %d for %.1f s (remo %d); running its update", t.mode,
                t.step, t.next, now - pending_since, int(remo_on));
            using Fn = void(BB_COOP_SYSV*)(u64, const Dt*);
            reinterpret_cast<Fn>(Guest(kTaskUpdate))(t.task, &dt);
        }
    }
}

/// BB_PARTY_B2_TEST_BOSS=<Lua event name> (e.g. BlockClear2): the guest raises it once the host's
/// chr has stood in its world for 5 s - the boss-clear path (K10 and the send-home veto) without
/// a boss fight. Logs the dispatcher's answer.
void TestBossTick(const GameSnapshot& s, double now) {
    static const char* env = std::getenv("BB_PARTY_B2_TEST_BOSS");
    static bool done = false;
    static double since = 0;
    if (!env || !env[0] || done || Role() != RoleClient || !s.world_up || s.loading) {
        return;
    }
    bool ready = false;
    for (const TaskInfo& t : Tasks(false)) {
        ready = ready || (t.mode == 2 && t.step == kStepUpdate && PlayerForPeer(t.task + kTaskPeer));
    }
    if (!ready) {
        since = 0;
        return;
    }
    if (since == 0) {
        since = now;
        return;
    }
    if (now - since < 5.0) {
        return;
    }
    done = true;
    if (std::strncmp(env, "hp=", 3) == 0) {
        // BB_PARTY_B2_TEST_BOSS=hp=<n>: the guest's HP set to n (to see it kept through a B2 load).
        const int hp = std::atoi(env + 3);
        const u64 dm = Field<u64>(Field<u64>(s.player, 0x3b0), 0x20);
        if (dm && hp > 0) {
            SafeWrite(dm + 0xf8, &hp, 4);
        }
        Log("test hp: guest HP set to %d (max %d)", hp, Field<int>(dm, 0xfc, -1));
        return;
    }
    std::int64_t r = -1;
    const bool ok = LuaEventRaiseNow(env, &r);
    Log("test boss: Lua event %s raised (%s, dispatcher %lld); role %d", env, ok ? "ok" : "no event manager",
        static_cast<long long>(r), Role());
}

/// BB_PARTY_B2_TEST_WARP=<lamp id> | die: single-instance check of K2 - once an NPC task has existed
/// for 15 s in a steady world, warp once more (0x13CDF30) so the reset path runs with a task.
void TestWarpTick(const GameSnapshot& s, double now) {
    static const char* env = std::getenv("BB_PARTY_B2_TEST_WARP");
    static bool done = false;
    static double npc_since = 0;
    if (!env || !env[0] || done) {
        return;
    }
    // BB_PARTY_B2_TEST_HOSTWARP=1: the host instead, once a guest's chr has stood in its world.
    static const bool host_mode = EnvOn("BB_PARTY_B2_TEST_HOSTWARP");
    bool ready = !Tasks(true).empty();
    if (host_mode) {
        // BB_PARTY_B2_TEST_MEMBERS=<n> (default 1): wait for n guests' chrs in the host's world.
        static const int want = [] {
            const char* v = std::getenv("BB_PARTY_B2_TEST_MEMBERS");
            return v && v[0] ? std::atoi(v) : 1;
        }();
        int have = 0;
        if (Role() == RoleHost) {
            for (const TaskInfo& t : Tasks(false)) {
                have += (t.mode == 1 && t.step == kStepUpdate && PlayerForPeer(t.task + kTaskPeer)) ? 1 : 0;
            }
        }
        ready = have >= want;
    }
    if (!s.world_up || s.loading || !ready) {
        npc_since = 0;
        return;
    }
    if (npc_since == 0) {
        npc_since = now;
        return;
    }
    static const double delay = [] {
        const char* v = std::getenv("BB_PARTY_B2_TEST_DELAY");
        return v && v[0] ? std::atof(v) : 5.0;
    }();
    if (now - npc_since < delay) {
        return;
    }
    done = true;
    // BB_PARTY_B2_TEST_WARP=die: the local player dies instead (Lua ForceDead 0x13320C0(ctx, entity
    // 10000 = the local player): GetChrByEntityId, clear +0x1E3 bit 5, then the chr kill 0x18BF9A0).
    if (std::strcmp(env, "die") == 0) {
        if (!Matches(0x13320c0, {0x55, 0x48, 0x89, 0xe5, 0x48, 0x8d, 0x05})) {
            Log("test die: ForceDead 0x13320C0 bytes differ; not run");
            return;
        }
        Log("test die: ForceDead(10000) with tasks:%s", DescribeTasks().c_str());
        using Fn = void(BB_COOP_SYSV*)(u64, u32);
        reinterpret_cast<Fn>(Guest(0x13320c0))(0, 10000);
        return;
    }
    const u32 id = u32(std::strtoul(env, nullptr, 10));
    Log("test warp: lamp %u with tasks:%s", id, DescribeTasks().c_str());
    using Fn = void(BB_COOP_SYSV*)(u32);
    reinterpret_cast<Fn>(Guest(0x13cdf30))(id);
}

void Tick(const GameSnapshot& s) {
    if (!g_on.load()) {
        return;
    }
    const double now = Now();
    static double last_status = 0;
    if ((Role() == RoleHost || Role() == RoleClient) && s.world_up && !s.loading && now - last_status >= 10.0) {
        last_status = now;
        const u64 remo = Field<u64>(Slot(0x5540058), 8);
        Log("status: local %s (%#llx), role %d, flow %d (timer %.2f, GSM+0x1521 %u, remo %d), rejoin %d, cooperators %d; "
            "tasks:%s; presence %s; %s",
            DescribeChr(s.player).c_str(), ull(s.player), Role(), Field<int>(Slot(kSlotFlow), 0x84, -1),
            Field<float>(Slot(kSlotFlow), 0x98), unsigned(Field<u8>(Slot(kSlotGsm), 0x1521)),
            remo ? int(Field<u8>(remo, 0x168) & 1) : -1, int(g_rejoin.load()), s.cooperators, DescribeTasks().c_str(),
            DescribePresence(true).c_str(), DescribePresentation().c_str());
        Log("status: where%s; apart %d", DescribeWhere().c_str(), g_apart.load());
    }
    {
        static double last_hit_log = 0;
        if (s.world_up && !s.loading && now - last_hit_log >= 0.2 && now - g_k3_at.load() < 15.0) {
            last_hit_log = now;
            for (const TaskInfo& t : Tasks(false)) {
                if (t.mode == 1 || t.mode == 2) {
                    if (const u64 c = PlayerForPeer(t.task + kTaskPeer)) {
                        Log("hit watch: remote chr %#llx %s", ull(c), DescribeHit(c).c_str());
                    }
                }
            }
            Log("hit watch: local %s", DescribeHit(s.player).c_str());
        }
    }
    {
        // A stale "host gone" timer: the session event handler 0x178B5E0 arms network flow +0xF4 =
        // 10 s when a member leaves a guest in the host's world (0x178D35D); the flow update
        // 0x1789B60 counts it down and then leaves (0x178A6BE). A guest that left on its own (a
        // Hunter's Mark) and is summoned again within those 10 s carried it into the new session
        // and was dropped 3 s after joining (live, edgetravel). A (re)join starts with it off
        // (-1, the flow ctor's value).
        static int prev_role = -1;
        const int role = Role();
        if ((role == 4 || role == 6) && prev_role != 4 && prev_role != 6) {
            const u64 flow = Slot(kSlotFlow);
            const float t = Field<float>(flow, 0xf4, -1.0f);
            if (flow && t > 0.0f) {
                const float off = -1.0f;
                SafeWrite(flow + 0xf4, &off, 4);
                Log("join: stale leave timer (flow +0xF4 = %.1f s from the last session) cleared", double(t));
            }
        }
        prev_role = role;
    }
    if (const u32 n = g_stale_hits.exchange(0, std::memory_order_relaxed)) {
        Log("remote chr on a streamed-out block: stale map hit cleared %u time(s); tasks:%s", n, DescribeTasks().c_str());
    }
    ProbeTick(s);
    TestWarpTick(s, now);
    KickStalledTasks(s, now);
    TestBossTick(s, now);
    ArrivalWatchTick(s, now);
    MemberSessionTypeTick(s);
    HostMemberWatchTick(s, now);
    FallbackCleanupTick(s);
    FallRecoveryTick(s, now);
    HeldDeathTick(s, now);
    {
        GuestPhase gp;
        {
            std::lock_guard<std::mutex> lk(S().mu);
            gp = S().gphase;
        }
        if (gp == GuestPhase::Idle && S().hphase == HostPhase::Idle && !g_rejoin.load()) {
            SplitTick(s, now);
        }
        if (s.world_up && !s.loading) {
            ApplyDeadEnemies(s);
        }
    }
    if (Role() == RoleClient) {
        GuestTick(s, now);
    } else {
        GuestPhase gp;
        {
            std::lock_guard<std::mutex> lk(S().mu);
            gp = S().gphase;
        }
        if (gp != GuestPhase::Idle && Role() != RoleHost) {
            FallbackToB1("the session ended during the re-join");
        }
        HostTick(s, now);
    }
}

namespace {
/// The guest's B2 follow / re-join: flow state 1, then GuestTick. `main_thread`: state 1 now
/// (after dropping stale snapshot packets); else the next GuestTick enters it (the link thread
/// must not call the game). `solo`: the guest's own travel (split travel).
bool BeginRejoin(const TravelIntent& t, bool main_thread, bool solo) {
    State& st = S();
    {
        std::lock_guard<std::mutex> lk(st.mu);
        if (st.gphase != GuestPhase::Idle) {
            if (st.gphase == GuestPhase::AwayLoading || st.solo || solo) {
                return solo ? false : true;  // the guest's own travel runs: it keeps it
            }
            st.intent = t;  // newer destination; the running re-join takes it
            return true;
        }
        st.intent = t;
        st.have_intent = true;
        st.have_dest = false;
        st.screen_done = false;
        st.blobs_saved = false;
        st.solo = solo;
        st.gphase = GuestPhase::AwaitSnapshot;
        st.since = Now();
        st.enter_state1 = !main_thread;
    }
    g_rejoin = true;
    if (main_thread) {
        // Back to "joined, awaiting the host's snapshot" (event 8/9 state); the host's 0x11 / 0x10
        // are read by 0x1783160 only in state 1 (it also clears the 60 s state timer).
        DrainSnapshotQueue(solo ? "own travel" : "follow");
        const u64 flow = Slot(kSlotFlow);
        const int one = 1;
        const float zero = 0.0f;
        SafeWrite(flow + 0x84, &one, 4);
        SafeWrite(flow + 0x108, &zero, 4);
    }
    Log("guest: %s (%s); waiting in flow state 1 for the host's snapshot", solo ? "travels alone into the host's area"
                                                                               : "host travels",
        DescribeTravel(t).c_str());
    return true;
}
} // namespace

bool OnHostTravel(const TravelIntent& t) {
    if (!g_on.load() || Role() != RoleClient) {
        return false;
    }
    if (t.alone) {
        // Split travel: the host goes alone; this guest stays and does not load. Its chr here
        // goes (main thread, SplitTick); the two meet again when they share an area.
        {
            std::lock_guard<std::mutex> lk(S().mu);
            if (S().gphase != GuestPhase::Idle && !S().solo && S().gphase != GuestPhase::AwayLoading) {
                Log("guest: the host travels alone (%s) during a re-join; the re-join goes on", DescribeTravel(t).c_str());
                return true;
            }
        }
        g_host_left = true;
        g_host_alone_seen = Now();
        NoteAway(0);
        Log("guest: the host travels alone (%s); staying here (no load)", DescribeTravel(t).c_str());
        return true;
    }
    {
        // The host's destination is the block this guest stands in (a host death or a stage warp
        // inside the map): no load for the guest (B4 / H1), only a move beside the host.
        const GameSnapshot g = ReadGameState(false);
        const std::uint32_t dest = DestMapOf(t);
        static const bool inworld = !(std::getenv("BB_PARTY_INWORLD") && std::getenv("BB_PARTY_INWORLD")[0] == '0');
        if (inworld && dest != 0 && g.world_up && !g.loading && g.map_id != 0xffffffffu && (dest >> 16) == (g.map_id >> 16) &&
            !g_dead_follow.load()) {
            std::lock_guard<std::mutex> lk(S().mu);
            if (S().gphase == GuestPhase::Idle) {
                S().intent = t;
                S().have_intent = true;
                S().have_dest = false;
                S().gphase = GuestPhase::InWorldFollow;
                S().since = Now();
                g_rejoin = true;
                Log("guest: host travels inside this map (%s): no load here, a move beside it once it is in",
                    DescribeTravel(t).c_str());
                return true;
            }
        }
    }
    return BeginRejoin(t, false, false);
}

void OnLinkEvent(int from_slot, const std::string& body) {
    if (!g_on.load()) {
        return;
    }
    if (body.find("\"dest\"") != std::string::npos) {
        u32 stage = 0;
        float p[4] = {}, r[4] = {};
        const char* b = body.c_str();
        const char* ps = std::strstr(b, "\"stage\":");
        const char* pp = std::strstr(b, "\"pos\":[");
        const char* pr = std::strstr(b, "\"rot\":[");
        if (ps && pp && pr && std::sscanf(ps, "\"stage\":%u", &stage) == 1 &&
            std::sscanf(pp, "\"pos\":[%f,%f,%f,%f]", &p[0], &p[1], &p[2], &p[3]) == 4 &&
            std::sscanf(pr, "\"rot\":[%f,%f,%f,%f]", &r[0], &r[1], &r[2], &r[3]) == 4 &&
            TravelMapKnown(stage) && std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]) &&
            std::fabs(p[0]) < 1e5f && std::fabs(p[1]) < 1e5f && std::fabs(p[2]) < 1e5f) {
            std::lock_guard<std::mutex> lk(S().mu);
            S().have_dest = true;
            S().dest_stage = stage;
            std::memcpy(S().dest_pos, p, sizeof p);
            std::memcpy(S().dest_rot, r, sizeof r);
            Log("guest: host destination stage %#x pos (%.1f, %.1f, %.1f)", stage, p[0], p[1], p[2]);
        } else {
            Log("guest: bad destination event refused");
        }
        return;
    }
    if (body.find("\"dead\"") != std::string::npos) {
        unsigned map = 0;
        const char* pm = std::strstr(body.c_str(), "\"map\":");
        const char* pi = std::strstr(body.c_str(), "\"ids\":[");
        if (!pm || !pi || std::sscanf(pm, "\"map\":%u", &map) != 1 || !TravelMapKnown(map & 0xffff0000u)) {
            Log("split: bad dead-enemies event refused");
            return;
        }
        std::vector<u32> list;
        for (const char* p = pi + 7; *p && *p != ']' && list.size() < 128;) {
            char* end = nullptr;
            const unsigned long v = std::strtoul(p, &end, 10);
            if (end == p) {
                break;
            }
            if (EnemyReconcilable(u32(v), map)) {
                list.push_back(u32(v));
            }
            p = *end == ',' ? end + 1 : end;
        }
        std::sort(list.begin(), list.end());
        std::lock_guard<std::mutex> lk(g_dead_mu);
        if (g_dead_in.size() < 8) {
            g_dead_in.emplace_back(map, std::move(list));
        }
        return;
    }
    if (body.find("\"warp\"") != std::string::npos) {
        // The host's in-world party move (HostPartyWarpNoLoad): run on the main thread.
        unsigned id = 0;
        const char* pl = std::strstr(body.c_str(), "\"lamp\":");
        if (pl && std::sscanf(pl, "\"lamp\":%u", &id) == 1 && TravelLampKnown(id)) {
            std::lock_guard<std::mutex> lk(S().mu);
            S().warp_lamp = id;
            Log("guest: the host moved to lamp %u inside its map (no load); following", id);
        }
        return;
    }
    if (body.find("\"away\"") != std::string::npos) {
        float p[3] = {0, 0, 0};
        const char* pp = std::strstr(body.c_str(), "\"pos\":[");
        const bool have = pp && std::sscanf(pp, "\"pos\":[%f,%f,%f]", &p[0], &p[1], &p[2]) == 3 &&
                          std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]);
        std::lock_guard<std::mutex> lk(S().mu);
        S().away_request = true;
        NoteAway(from_slot);
        S().away_have_pos = have;
        std::memcpy(S().away_pos, p, sizeof p);
        Log("host: slot %d travels alone into another area", from_slot);
        return;
    }
    if (body.find("\"rejoin\"") != std::string::npos || body.find("\"respawn\"") != std::string::npos) {
        const bool live = body.find("\"rejoin\"") != std::string::npos;
        float p[3] = {0, 0, 0};
        const char* pp = std::strstr(body.c_str(), "\"pos\":[");
        const bool have = pp && std::sscanf(pp, "\"pos\":[%f,%f,%f]", &p[0], &p[1], &p[2]) == 3 &&
                          std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]);
        std::lock_guard<std::mutex> lk(S().mu);
        S().respawn_request = true;
        S().respawn_live = live;
        S().respawn_have_pos = have;
        std::memcpy(S().respawn_pos, p, sizeof p);
        Log("host: slot %d %s; re-sending the snapshot", from_slot,
            live ? "travels alone into this area" : "died alone and respawns in this world");
        return;
    }
    if (body.find("\"arrived\"") != std::string::npos) {
        std::lock_guard<std::mutex> lk(S().mu);
        S().arrived = true;
        Log("host: slot %d arrived", from_slot);
    }
}

bool LampTravelIntent(std::uint32_t id, TravelIntent* t) {
    return g_on.load() && LampIntent(id, t);
}

bool HostHasMembers() {
    return Role() == RoleHost && HostHasMemberTasks();
}

bool WarpToLampNoLoad(std::uint32_t id, bool beside) {
    if (!g_on.load() || !WarpToLampInWorld(id, beside, beside ? "party move" : "own move")) {
        return false;
    }
    TravelIntent t;
    if (LampIntent(id, &t)) {
        SetOwnLastLamp(t);  // a lamp reached in-world is the last lamp too (a Mark: unchanged anyway)
    }
    return true;
}

bool HostPartyWarpNoLoad(std::uint32_t id) {
    if (!g_on.load() || Role() != RoleHost || !WarpToLampInWorld(id, false, "the host's party move")) {
        return false;
    }
    if (TravelIntent t; LampIntent(id, &t)) {
        SetOwnLastLamp(t);
    }
    char b[64];
    std::snprintf(b, sizeof b, "{\"op\":\"warp\",\"lamp\":%u}", id);
    Send(-1, b);
    Log("host: party move to lamp %u inside the loaded map: no load; guests in this map follow in-world", id);
    return true;
}

void SetWhere(const std::vector<Where>& members) {
    g_where = members;
}

int MembersApart() {
    return g_apart.load();
}

const char* AloneWhyName(AloneWhy w) {
    switch (w) {
    case AloneWhy::Lamp: return "lamp";
    case AloneWhy::Headstone: return "headstone";
    case AloneWhy::HuntersMark: return "hunters_mark";
    case AloneWhy::Death: return "death";
    }
    return "?";
}

void HostTravelAloneNext() {
    g_host_alone_at = Now();
    g_host_alone_next = true;
    SetNextHostTravelAlone(true);
}

/// The destination map of an intent: the packed map, else the lamp / last-lamp record's map
/// (ReturnPointParam ids AAB295k: map mAA_0B), 0 unknown.
std::uint32_t DestMapOf(const TravelIntent& t) {
    if (t.packed_map != kTravelNone && t.packed_map != 0 && TravelMapKnown(t.packed_map & 0xffff0000u)) {
        return t.packed_map;
    }
    std::uint32_t id = t.lamp_id;
    if (id == kTravelNone || id == 0) {
        id = static_cast<std::uint32_t>(t.last_lamp);
    }
    if (id == kTravelNone || id == 0) {
        return 0;
    }
    return ((id / 100000) << 24) | (((id / 10000) % 10) << 16);
}

bool TravelAlone(const TravelIntent& t, AloneWhy why) {
    if (!g_on.load() || Role() != RoleClient) {
        return false;
    }
    const GameSnapshot s = ReadGameState(false);
    const std::uint32_t dest = DestMapOf(t);
    if (!s.world_up || s.loading || !s.player || dest == 0) {
        Log("guest: travel alone (%s) refused: %s", AloneWhyName(why), dest == 0 ? "no destination map" : "not in the world");
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(S().mu);
        if (S().gphase != GuestPhase::Idle) {
            Log("guest: travel alone (%s) refused: a re-join is running", AloneWhyName(why));
            return false;
        }
    }
    const std::uint32_t host = HostMap();
    float p[3] = {0, 0, 0};
    ChrPos(s.player, p);
    char pos[96];
    std::snprintf(pos, sizeof pos, "\"pos\":[%.3f,%.3f,%.3f]", p[0], p[1], p[2]);
    g_dead_follow = why == AloneWhy::Death;
    if (why == AloneWhy::Lamp || why == AloneWhy::Headstone) {
        SetOwnLastLamp(t);
    }
    if (host != 0 && AreaOf(host) == AreaOf(dest)) {
        // Into the host's area (the shared Dream included): a re-join, one load for this guest.
        if (!BeginRejoin(t, true, true)) {
            g_dead_follow = false;
            return false;
        }
        Send(0, std::string("{\"op\":\"rejoin\",") + pos + "}");
        Log("guest: travels alone (%s) to %s, the host's area (%s): a re-join with the host's snapshot",
            AloneWhyName(why), MapName(dest).c_str(), MapName(host).c_str());
        return true;
    }
    // Elsewhere: the alone load, host-world mode, no snapshot. The destination as an exact
    // transform (the lamp's MSB revive spot, party_ids ReturnSpot): the lamp warp itself landed
    // the guest at the map origin of a non-existent block (live split3: m24_9999 asked for,
    // (0, -5, 0), an endless fall) - without the snapshot states 1-2 a host-world load does not
    // resolve the lamp's map. The transform is the path every B2 load uses.
    TravelIntent tt = t;
    {
        const std::uint32_t lamp = t.lamp_id != kTravelNone && t.lamp_id != 0 ? t.lamp_id
                                                                                : static_cast<std::uint32_t>(t.last_lamp);
        float spot[4];
        if (why == AloneWhy::Death || lamp / 100000 == 29 || !ids::ReturnSpot(lamp, spot)) {
            Log("guest: travel alone (%s) to %u refused: %s", AloneWhyName(why), lamp,
                why == AloneWhy::Death ? "a death outside the host's area is not an alone load yet"
                                       : "no known spot for that lamp");
            g_dead_follow = false;
            return false;
        }
        tt.kind = TravelKind::Transform;
        tt.has_pos = true;
        tt.pos_map = ids::EntityMap(lamp);
        tt.pos[0] = spot[0], tt.pos[1] = spot[1], tt.pos[2] = spot[2], tt.pos[3] = 1.0f;
        tt.rot[0] = 0.0f, tt.rot[1] = spot[3], tt.rot[2] = 0.0f, tt.rot[3] = 0.0f;
    }
    State& st = S();
    const u64 flow = Slot(kSlotFlow);
    if (Field<int>(flow, 0x84, -1) != 4) {
        Log("guest: travel alone refused: flow %d (not in the host's world)", Field<int>(flow, 0x84, -1));
        g_dead_follow = false;
        return false;
    }
    DrainSnapshotQueue("alone load");
    const u64 gsm = Slot(kSlotGsm);
    const u8 was = Field<u8>(gsm, 0x1521), zero = 0;
    {
        std::lock_guard<std::mutex> lk(st.mu);
        st.intent = t;
        st.have_intent = true;
        st.solo = true;
        st.saved_1521 = was ? int(was) : -1;
        st.gphase = GuestPhase::AwayLoading;
        st.since = Now();
        st.load_seen = false;
        st.stable = 0;
    }
    if (was) {
        SafeWrite(gsm + 0x1521, &zero, 1);
    }
    {
        // The host's packet-0x19 value sticks in GSM +0x1598 / +0x159C and goes into every later
        // host-world load's descriptor (+0x8C -> GameDataMan +0x68); for a map the host is not in
        // it is the wrong one (live split5: an alone load to the Dream built no map, a grey screen).
        Log("guest: alone load: GSM +0x1593 %u +0x1594 %#x +0x1598 %u +0x159c %#x, GameDataMan +0x68 %#x",
            unsigned(Field<u8>(gsm, 0x1593)), Field<u32>(gsm, 0x1594), unsigned(Field<u8>(gsm, 0x1598)),
            Field<u32>(gsm, 0x159c), Field<u32>(Slot(kSlotGameData), 0x68));
        static const bool keep = std::getenv("BB_PARTY_SPLIT_KEEP_1598") != nullptr;
        if (!keep) {
            SafeWrite(gsm + 0x1598, &zero, 1);
            SafeWrite(gsm + 0x1593, &zero, 1);
        }
    }
    g_rejoin = true;
    RestoreHostBlobs();
    {
        // The summoned placement GSM +0x14C4 stage / +0x14D0 position / +0x14E0 rotation still held
        // the host's last spot (m24_01) and a host-world load takes its stage from it whatever the
        // forced transform says (live split7 / split9: PlayerIns stage 0x18010000 at the Dream's
        // coordinates, area-24 resources, no world). The destination's goes there too.
        alignas(16) float p16[4] = {tt.pos[0], tt.pos[1], tt.pos[2], 1.0f};
        alignas(16) float r16[4] = {tt.rot[0], tt.rot[1], tt.rot[2], tt.rot[3]};
        Log("guest: alone load: summoned placement stage %#x -> %#x", Field<u32>(gsm, 0x14c4), tt.pos_map);
        SafeWrite(gsm + 0x14c4, &tt.pos_map, 4);
        SafeWrite(gsm + 0x14d0, p16, 16);
        SafeWrite(gsm + 0x14e0, r16, 16);
    }
    const int three = 3;
    SafeWrite(flow + 0x84, &three, 4);
    Send(0, std::string("{\"op\":\"away\",") + pos + "}");
    bool ok;
    {
        FunnelKeepRoom keep;
        Log("guest: travels alone (%s) to %s, the host is in %s: alone load in host-world mode (K7 %s)", AloneWhyName(why),
            MapName(dest).c_str(), host ? MapName(host).c_str() : "an unknown map", keep.armed ? "armed" : "NOT armed");
        ok = ReplayTravelNow(tt);
    }
    if (!ok) {
        const int four = 4;
        SafeWrite(flow + 0x84, &four, 4);
        if (was) {
            SafeWrite(gsm + 0x1521, &was, 1);
        }
        g_rejoin = false;
        g_dead_follow = false;
        std::lock_guard<std::mutex> lk(st.mu);
        st.gphase = GuestPhase::Idle;
        st.solo = false;
        st.saved_1521 = -1;
        Log("guest: travel alone: no usable destination in %s", DescribeTravel(t).c_str());
        return false;
    }
    return true;
}

bool RespawnAloneAtOwnLamp() {
    const u32 own = Field<u32>(Slot(kSlotGsm), 0x1528);
    TravelIntent t;
    if (!g_on.load() || own == 0 || own == kTravelNone || !LampIntent(own, &t)) {
        Log("guest: own-lamp respawn refused (own last lamp %u)", own);
        return false;
    }
    return TravelAlone(t, AloneWhy::Death);
}

std::uint32_t NearestEnemyForTest(float max_m) {
    const GameSnapshot s = ReadGameState(false);
    return g_on.load() && s.world_up && !s.loading ? NearestEnemy(s, max_m) : 0u;
}

bool PopOutgoing(int* slot, std::string* body) {
    std::lock_guard<std::mutex> lk(S().mu);
    if (S().out.empty()) {
        return false;
    }
    *slot = S().out.front().first;
    *body = std::move(S().out.front().second);
    S().out.pop_front();
    return true;
}

} // namespace coop::b2

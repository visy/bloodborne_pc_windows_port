// SPDX-License-Identifier: GPL-3.0-or-later
// Party travel B2 prototype (see party_b2.h; the RE is docs/party/b2_seamless.md).
//
// Every site was disassembled from the 1.09 eboot (capstone + Ghidra) and is compared byte for
// byte by coop_hooks before anything is written. Offsets are ours (raw ELF VA). The game's code
// is System V: every call into it goes through a BB_COOP_SYSV pointer at Guest(off).
#include "party_b2.h"
#include "party_async_io.h"

#include "coop_hooks.h"
#include "game_state.h"
#include "party_travel.h"
#include "lua_events.h"
#include "seamless_rules.h"
#include "../../party_env.h"

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
    char b[96];
    if (!chr || !ChrPos(chr, p)) {
        return "-";
    }
    std::snprintf(b, sizeof b, "stage %#x pos (%.1f, %.1f, %.1f)", Field<u32>(chr, 0x3f8), p[0], p[1], p[2]);
    return b;
}

std::string DescribeTasks() {
    std::string s;
    char b[192];
    for (const TaskInfo& t : Tasks(false)) {
        std::string chrs;
        if (t.mode == 1 || t.mode == 2) {
            const u64 c = PlayerForPeer(t.task + kTaskPeer);
            chrs = c ? " chr " + DescribeChr(c) : " NO-chr";
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

// K3: in-game stage update, stage-record UID changed: the host's BeginLeaveSession.
BB_COOP_SYSV u64 UidLeaveHook(u64 session) {
    if (g_on.load() && SessionUp()) {
        Log("K3: stage-record change with a session: no leave");
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
bool SendHomeVeto(u64 site) {
    if (g_on.load() && Role() == RoleClient &&
        (site == kHostDead1Call || site == 0x13856f4 || site == 0x13859df)) {
        Log("guest: send-home at +0x%llx dropped (B2: %s)", ull(site),
            site == kHostDead1Call ? "the host's respawn travel follows" : "the party stays after the boss");
        return true;
    }
    return false;
}

// K10: the boss-clear Lua states' BeginLeaveSession (0x13858F5 in 0x13858A0, 0x1385A95 in
// 0x13859F0): a B2 guest stays in the session after the boss.
BB_COOP_SYSV u64 BossClearLeaveHook(u64 session) {
    if (g_on.load() && Role() == RoleClient) {
        Log("K10: boss-clear leave suppressed (party stays)");
        return 0;
    }
    using Fn = u64(BB_COOP_SYSV*)(u64);
    return reinterpret_cast<Fn>(Guest(kBeginLeave))(session);
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
        return r;
    }
    return reinterpret_cast<Apply>(Guest(0x13beca0))(man, blob);
}

// K5: Lua OnMatchingCheck: a member's chr is missing (it is loading).
BB_COOP_SYSV u64 MatchCheckHook(u64 session) {
    if (g_on.load() && g_rejoin.load()) {
        Log("K5: OnMatchingCheck leave suppressed (re-join in flight)");
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

enum class GuestPhase { Idle, AwaitSnapshot, Loading, Rewinding };
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
    bool arrived = false;  // a guest said it arrived (under mu)
    bool have_dest = false; // guest: the host's post-load transform (under mu)
    int hp_before = -1;      // guest: its HP at the host's travel event (kept through the B2 load)
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

void ProbeTick(const GameSnapshot& s) {
    State& st = S();
    const bool loading = s.loading || !s.world_up;
    if (loading && !st.was_loading) {
        st.player_before = s.player;
        Log("load started: local PlayerIns %#llx, role %d, flow %d; tasks:%s", ull(s.player), Role(),
            Field<int>(Slot(kSlotFlow), 0x84, -1), DescribeTasks().c_str());
    } else if (!loading && st.was_loading) {
        Log("B2 load report: map %s, local PlayerIns %#llx -> %#llx (%s), player set %d, role %d, flow %d; tasks:%s",
            MapName(s.map_id).c_str(), ull(st.player_before), ull(s.player),
            st.player_before == s.player ? "SAME object" : "re-created",
            Field<int>(Slot(kSlotWorldChrMan), 0x30, -1), Role(), Field<int>(Slot(kSlotFlow), 0x84, -1),
            DescribeTasks().c_str());
    }
    st.was_loading = loading;
}

std::atomic<int> g_local_slot{-1};

/// A guest's spot beside the host (never on it: character collision shoves the host): BB_PARTY_SUMMON_OFFSET
/// (default 2.2 m, 0 = off) to
/// the right (party slot 1), left (2) or behind (3+) of the host's facing, same height and facing.
/// rot[1] is the yaw (radians); forward = (sin, 0, cos), right = (cos, 0, -sin).
float SummonOffset() {
    static const float d = [] {
        const char* v = std::getenv("BB_PARTY_SUMMON_OFFSET");
        return v && v[0] ? float(std::atof(v)) : 2.2f;
    }();
    return d;
}

void BesideHost(float pos[4], const float rot[4], int slot) {
    const float yaw = rot[1], d = SummonOffset();
    const float fx = std::sin(yaw), fz = std::cos(yaw), rx = std::cos(yaw), rz = -std::sin(yaw);
    if (slot == 2) {
        pos[0] -= rx * d, pos[2] -= rz * d;
    } else if (slot >= 3) {
        pos[0] -= fx * d, pos[2] -= fz * d;
    } else {
        pos[0] += rx * d, pos[2] += rz * d;
    }
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
        if (++wait < 30) {
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
    const int slot = g_local_slot.load() == 2 ? 2 : 1;
    const int side = attempt == 0 ? slot : attempt == 1 ? (slot == 1 ? 2 : 1) : 3;
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
            const u32 lamp = Field<u32>(Slot(kSlotGsm), 0x1528);
            Log("recovery: falling for 6 s at (%.1f, %.1f, %.1f); warping to the own last lamp %u", p[0], p[1], p[2],
                lamp);
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
void FallbackToB1(const char* why) {
    State& st = S();
    TravelIntent t;
    bool have;
    {
        std::lock_guard<std::mutex> lk(st.mu);
        have = st.have_intent;
        t = st.intent;
        st.gphase = GuestPhase::Idle;
    }
    DropLoadingScreen(why);
    g_rejoin = false;
    const u64 flow = Slot(kSlotFlow);
    const int fs = Field<int>(flow, 0x84, -1);
    if (fs >= 1 && fs <= 3) {
        const int four = 4;
        SafeWrite(flow + 0x84, &four, 4);
    }
    Log("guest: FALLBACK to B1 (%s; flow was %d)%s", why, fs, have ? "" : " - no travel to replay");
    if (have) {
        RequestGuestTravel(t);
    }
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
    case GuestPhase::Idle:
        return;
    case GuestPhase::AwaitSnapshot:
        if (!st.screen_done) {
            st.screen_done = true;
            SaveOverlay();
            st.hp_before = Field<int>(Field<u64>(Field<u64>(s.player, 0x3b0), 0x20), 0x128, -1);
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
            RestoreZeroedOverlay();
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
                // host's travel (a load may refill it). Data module: chr +0x3B0 -> +0x20: HP +0x128,
                // max +0x12C.
                const u64 dm = Field<u64>(Field<u64>(s.player, 0x3b0), 0x20);
                const int hp = Field<int>(dm, 0x128, -1), mx = Field<int>(dm, 0x12c, -1);
                if (dm && st.hp_before > 0 && hp >= 0 && hp != st.hp_before) {
                    const int keep = st.hp_before < mx ? st.hp_before : mx;
                    SafeWrite(dm + 0x128, &keep, 4);
                    Log("guest: HP %d after the load -> %d as before the travel (max %d)", hp, keep, mx);
                } else {
                    Log("guest: HP %d after the load (before %d, max %d)", hp, st.hp_before, mx);
                }
            }
            const int n = RewindRemoteTasks("guest");
            Log("guest: arrived in the host world (flow 4); %d remote task(s) rewound", n);
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

void HostTick(const GameSnapshot& s, double now) {
    State& st = S();
    const bool loading = s.loading || !s.world_up;
    if (Role() != RoleHost) {
        st.hphase = HostPhase::Idle;
        return;
    }
    switch (st.hphase) {
    case HostPhase::Idle:
        if (loading) {
            st.hphase = HostPhase::Loading;
            st.hsince = now;
            st.stable = 0;
            g_rejoin = true;
            Log("host: own load with a session; members keep the room");
        }
        return;
    case HostPhase::Loading:
        st.stable = loading ? 0 : st.stable + 1;
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
            std::lock_guard<std::mutex> lk(st.mu);
            st.arrived = false;
        }
        return;
    case HostPhase::Resynced: {
        bool arrived;
        {
            std::lock_guard<std::mutex> lk(st.mu);
            arrived = st.arrived;
            st.arrived = false;
        }
        if (arrived || now - st.hsince > 60.0) {
            const int n = RewindRemoteTasks("host");
            Log("host: %s; %d member task(s) rewound", arrived ? "a guest arrived" : "60 s without arrival", n);
            if (now - st.hsince > 60.0 || n == 0) {
                st.hphase = HostPhase::Idle;
                g_rejoin = false;
            }
        }
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
    SetSendHomeVeto(&SendHomeVeto);
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
            SafeWrite(dm + 0x128, &hp, 4);
        }
        Log("test hp: guest HP set to %d (max %d)", hp, Field<int>(dm, 0x12c, -1));
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
        Log("status: local %s, role %d, flow %d (timer %.2f, GSM+0x1521 %u, remo %d), rejoin %d; tasks:%s",
            DescribeChr(s.player).c_str(), Role(), Field<int>(Slot(kSlotFlow), 0x84, -1),
            Field<float>(Slot(kSlotFlow), 0x98), unsigned(Field<u8>(Slot(kSlotGsm), 0x1521)),
            remo ? int(Field<u8>(remo, 0x168) & 1) : -1, int(g_rejoin.load()), DescribeTasks().c_str());
    }
    ProbeTick(s);
    TestWarpTick(s, now);
    KickStalledTasks(s, now);
    TestBossTick(s, now);
    ArrivalWatchTick(s, now);
    HostMemberWatchTick(s, now);
    FallRecoveryTick(s, now);
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

bool OnHostTravel(const TravelIntent& t) {
    if (!g_on.load() || Role() != RoleClient) {
        return false;
    }
    State& st = S();
    {
        std::lock_guard<std::mutex> lk(st.mu);
        if (st.gphase != GuestPhase::Idle) {
            st.intent = t;  // newer destination; the running re-join takes it
            return true;
        }
        st.intent = t;
        st.have_intent = true;
        st.have_dest = false;
        st.screen_done = false;
        st.gphase = GuestPhase::AwaitSnapshot;
        st.since = Now();
    }
    g_rejoin = true;
    // Back to "joined, awaiting the host's snapshot" (event 8/9 state); the host's 0x11 / 0x10
    // are read by 0x1783160 only in state 1. Main thread does the rest; this write is a plain
    // int the game polls (it also clears the 60 s state timer).
    const u64 flow = Slot(kSlotFlow);
    const int one = 1;
    const float zero = 0.0f;
    SafeWrite(flow + 0x84, &one, 4);
    SafeWrite(flow + 0x108, &zero, 4);
    Log("guest: host travels (%s); waiting in flow state 1 for its snapshot", DescribeTravel(t).c_str());
    return true;
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
    if (body.find("\"arrived\"") != std::string::npos) {
        std::lock_guard<std::mutex> lk(S().mu);
        S().arrived = true;
        Log("host: slot %d arrived", from_slot);
    }
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

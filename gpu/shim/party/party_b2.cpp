// SPDX-License-Identifier: GPL-3.0-or-later
// Party travel B2 prototype (see party_b2.h; the RE is docs/party/b2_seamless.md).
//
// Every site was disassembled from the 1.09 eboot (capstone + Ghidra) and is compared byte for
// byte by coop_hooks before anything is written. Offsets are ours (raw ELF VA). The game's code
// is System V: every call into it goes through a BB_COOP_SYSV pointer at Guest(off).
#include "party_b2.h"

#include "coop_hooks.h"
#include "game_state.h"
#include "party_travel.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
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
    std::printf("Party B2: %s\n", line);
    std::fflush(stdout);
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

std::string DescribeTasks() {
    std::string s;
    char b[96];
    for (const TaskInfo& t : Tasks(false)) {
        const char* chr = (t.mode == 1 || t.mode == 2) ? (PlayerForPeer(t.task + kTaskPeer) ? " chr" : " NO-chr") : "";
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
void ResyncMember(u64 id) {
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
    int stable = 0;
    // host
    HostPhase hphase = HostPhase::Idle;
    double hsince = 0;
    bool arrived = false;  // a guest said it arrived (under mu)
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
        if (fstate == 2) {
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
            FunnelKeepRoom keep;
            Log("guest: host snapshot applied; loading %s in host-world mode (K7 %s)", DescribeTravel(t).c_str(),
                keep.armed ? "armed" : "NOT armed: the room will close");
            if (!ReplayTravelNow(t)) {
                Log("guest: no usable destination; falling back to B1");
                g_rejoin = false;
                std::lock_guard<std::mutex> lk(st.mu);
                st.gphase = GuestPhase::Idle;
            }
            return;
        }
        if (now - st.since > 45.0) {  // the flow leaves by itself after 60 s in states 1-3
            Log("guest: no host snapshot within 45 s (flow %d); falling back to B1", fstate);
            const int four = 4;
            SafeWrite(flow + 0x84, &four, 4);
            g_rejoin = false;
            TravelIntent t;
            {
                std::lock_guard<std::mutex> lk(st.mu);
                t = st.intent;
                st.gphase = GuestPhase::Idle;
            }
            RequestGuestTravel(t);
        }
        return;
    case GuestPhase::Loading: {
        const bool loading = s.loading || !s.world_up;
        st.load_seen = st.load_seen || loading;
        st.stable = (!loading && st.load_seen) ? st.stable + 1 : 0;
        if (st.stable >= 30 && fstate == 4) {
            const int n = RewindRemoteTasks("guest");
            Log("guest: arrived in the host world (flow 4); %d remote task(s) rewound", n);
            Send(-1, "{\"op\":\"arrived\"}");
            std::lock_guard<std::mutex> lk(st.mu);
            st.gphase = GuestPhase::Rewinding;
            st.since = now;
        } else if (now - st.since > 90.0) {
            Log("guest: load did not finish in host-world mode within 90 s (flow %d)", fstate);
            g_rejoin = false;
            std::lock_guard<std::mutex> lk(st.mu);
            st.gphase = GuestPhase::Idle;
        }
        return;
    }
    case GuestPhase::Rewinding:
        if (now - st.since > 10.0) {
            Log("guest: re-join done; tasks:%s", DescribeTasks().c_str());
            g_rejoin = false;
            std::lock_guard<std::mutex> lk(st.mu);
            st.gphase = GuestPhase::Idle;
        }
        return;
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
            for (u64 id : Members()) {
                ResyncMember(id);
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
    return EnvOn("BB_PARTY_B2");
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
    Log("K7 funnel branch +0x%llx %s", ull(kFunnelLeaveJne),
        Matches(kFunnelLeaveJne, {0x75, 0x6c}) ? "verified (75 6c), written only around the guest's own warp"
                                                : "MISMATCH: guest re-join will leave the room");
    g_on = true;
    Log("on%s", g_force.load() ? " (BB_PARTY_B2_FORCE: K2 keeps tasks without a session)" : "");
}

/// BB_PARTY_B2_TEST_WARP=<lamp id>: single-instance check of K2 - once an NPC task has existed
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
        ready = false;
        if (Role() == RoleHost) {
            for (const TaskInfo& t : Tasks(false)) {
                ready = ready || (t.mode == 1 && t.step == kStepUpdate && PlayerForPeer(t.task + kTaskPeer));
            }
        }
    }
    if (!s.world_up || s.loading || !ready) {
        npc_since = 0;
        return;
    }
    if (npc_since == 0) {
        npc_since = now;
        return;
    }
    if (now - npc_since < 15.0) {
        return;
    }
    done = true;
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
        Log("status: map %s, role %d, flow %d, rejoin %d; tasks:%s", MapName(s.map_id).c_str(), Role(),
            Field<int>(Slot(kSlotFlow), 0x84, -1), int(g_rejoin.load()), DescribeTasks().c_str());
    }
    ProbeTick(s);
    TestWarpTick(s, now);
    if (Role() == RoleClient) {
        GuestTick(s, now);
    } else {
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

// SPDX-License-Identifier: GPL-3.0-or-later
// Party travel, phase B1 (see party_travel.h; the RE is docs/party/travel.md).
//
// Every site below was disassembled from smoketest/out/eboot.elf (1.09) and is compared byte for
// byte before anything is written. Offsets are ours (raw ELF VA). The game's code is System V:
// every call into it goes through a BB_COOP_SYSV pointer at Guest(off).
#include "party_travel.h"
#include "party_ids.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

#ifndef BB_PARTY_TRAVEL_NO_GAME
#include "coop_hooks.h"
#include "game_state.h"
#include "party_director.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdlib>
#include <mutex>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#endif

namespace coop {

// ---------------------------------------------------------------------------------------------
// Pure part (unit-tested)
// ---------------------------------------------------------------------------------------------

namespace {

struct KindName {
    TravelKind kind;
    const char* name;
};
constexpr KindName kKindNames[] = {
    {TravelKind::Unknown, "unknown"},
    {TravelKind::Lamp, "lamp"},
    {TravelKind::ScriptedWarp, "scripted_warp"},
    {TravelKind::LuaStageWarp, "lua_stage_warp"},
    {TravelKind::LuaBonfireWarp, "lua_bonfire_warp"},
    {TravelKind::LuaStageKick, "lua_stage_kick"},
    {TravelKind::HostDeath, "host_death"},
    {TravelKind::HuntersMark, "hunters_mark"},
    {TravelKind::Transform, "transform"},
    {TravelKind::GuestSentHome, "guest_sent_home"},
    {TravelKind::GuestDied, "guest_died"},
    {TravelKind::BossCleared, "boss_cleared"},
    {TravelKind::MissionOrPk, "mission_or_pk"},
    {TravelKind::TitleOrDebug, "title_or_debug"},
};

struct CallSiteKind {
    std::uint64_t site;
    TravelKind kind;
};
// travel.md 1.6, re-derived by scanning eboot.elf for every e8/e9 rel32 to 0x13CDE30 (15 calls,
// 4 tail jumps: 0x13175EF, 0x1383A5C, 0x13859DF, 0x1389F20).
constexpr CallSiteKind kCallSites[] = {
    {0x13ce002, TravelKind::Lamp},           // 0x13CDF30 lamp warp by id
    {0x17c0bed, TravelKind::ScriptedWarp},   // EMEVD 2003[14]
    {0x132e03e, TravelKind::LuaStageWarp},   // WarpNextStage
    {0x132e0b4, TravelKind::LuaBonfireWarp}, // WarpNextStage_Bonfire
    {0x138c5f0, TravelKind::LuaBonfireWarp}, // Lua_Warp_1
    {0x1332b84, TravelKind::LuaStageKick},   // WarpNextStageKick
    {0x1381ede, TravelKind::HostDeath},      // SoloPlayDeath_2
    {0x1389f20, TravelKind::HuntersMark},    // OnReviveMagic_1 (tail jump)
    {0x1381a93, TravelKind::GuestSentHome},  // HostDead_1
    {0x138a51c, TravelKind::GuestSentHome},  // OnLeave_Limit
    {0x1382663, TravelKind::GuestDied},      // PartyGhostDeath_2
    {0x13856f4, TravelKind::BossCleared},    // BlockClear2_1
    {0x13859df, TravelKind::BossCleared},    // BlockClear2_3 (tail jump)
    {0x138b513, TravelKind::MissionOrPk},    // Failed_BossAreaMission_LeaveMap
    {0x1383a5c, TravelKind::MissionOrPk},    // PlayerKill_4030_1 (tail jump)
    {0x132e11e, TravelKind::TitleOrDebug},   // 0x132E0C0, map 0x01000000
    {0x132e166, TravelKind::TitleOrDebug},
    {0x13175ef, TravelKind::TitleOrDebug},   // 0x1317550 debug presets (tail jump)
    {0x1317632, TravelKind::TitleOrDebug},
};

std::uint32_t Low(std::uint64_t v) {
    return static_cast<std::uint32_t>(v);
}

} // namespace

const char* TravelKindName(TravelKind k) {
    for (const KindName& n : kKindNames) {
        if (n.kind == k) {
            return n.name;
        }
    }
    return "unknown";
}

TravelKind TravelKindFromName(const std::string& name) {
    for (const KindName& n : kKindNames) {
        if (name == n.name) {
            return n.kind;
        }
    }
    return TravelKind::Unknown;
}

TravelKind TravelKindFromCallSite(std::uint64_t call_site) {
    for (const CallSiteKind& c : kCallSites) {
        if (c.site == call_site) {
            return c.kind;
        }
    }
    return TravelKind::Unknown;
}

bool TravelKindBroadcast(TravelKind k) {
    switch (k) {
    case TravelKind::Lamp:
    case TravelKind::ScriptedWarp:
    case TravelKind::LuaStageWarp:
    case TravelKind::LuaBonfireWarp:
    case TravelKind::LuaStageKick:
    case TravelKind::HostDeath:
    case TravelKind::HuntersMark:
    case TravelKind::Transform:
        return true;
    default:
        return false;
    }
}

std::string DescribeTravel(const TravelIntent& t) {
    char text[384];
    std::snprintf(text, sizeof text,
                  "#%llu %s (site +0x%x): lamp %d, map m%02u_%02u_%02u_%02u (0x%08x), warp point %d, mode %u, "
                  "respawn 0x%016llx, last lamp 0x%016llx%s",
                  static_cast<unsigned long long>(t.seq), TravelKindName(t.kind), t.call_site,
                  static_cast<int>(t.lamp_id), t.Area(), t.Block(), t.Region(), t.Index(), t.packed_map,
                  static_cast<int>(t.warp_point), t.respawn_mode, static_cast<unsigned long long>(t.respawn_record),
                  static_cast<unsigned long long>(t.last_lamp), t.has_pos ? ", exact position" : "");
    return text;
}

json::Value TravelToJson(const TravelIntent& t) {
    json::Value v = json::Value::make_object();
    v.set("seq", json::hex(t.seq));
    v.set("kind", TravelKindName(t.kind));
    v.set("site", t.call_site);
    v.set("lamp", t.lamp_id);
    v.set("map", t.packed_map);
    v.set("warp_point", t.warp_point);
    v.set("mode", t.respawn_mode);
    v.set("respawn", json::hex(t.respawn_record));
    v.set("last_lamp", json::hex(t.last_lamp));
    if (t.has_pos) {
        json::Value p = json::Value::make_object();
        p.set("map", t.pos_map);
        json::Value pos = json::Value::make_array(), rot = json::Value::make_array();
        for (int i = 0; i < 4; ++i) {
            pos.push(json::f32(t.pos[i])); // exact bits: the guest lands where the host stood
            rot.push(json::f32(t.rot[i]));
        }
        p.set("pos", pos);
        p.set("rot", rot);
        v.set("transform", p);
    }
    return v;
}

bool TravelFromJson(const json::Value& v, TravelIntent* out, std::string* error) {
    try {
        if (v.type != json::Value::Type::Object) {
            throw std::runtime_error("not an object");
        }
        TravelIntent t;
        t.seq = json::u64(v, "seq");
        t.kind = TravelKindFromName(json::str(v, "kind"));
        t.call_site = json::u32(v, "site");
        t.lamp_id = json::u32(v, "lamp");
        t.packed_map = json::u32(v, "map");
        t.warp_point = json::u32(v, "warp_point");
        t.respawn_mode = json::u32(v, "mode");
        t.respawn_record = json::u64(v, "respawn");
        t.last_lamp = json::u64(v, "last_lamp");
        if (const json::Value* p = v.find("transform")) {
            t.pos_map = json::u32(*p, "map");
            const std::vector<json::Value>& pos = json::arr(*p, "pos");
            const std::vector<json::Value>& rot = json::arr(*p, "rot");
            if (pos.size() != 4 || rot.size() != 4) {
                throw std::runtime_error("transform: pos / rot need 4 components");
            }
            for (int i = 0; i < 4; ++i) {
                t.pos[i] = json::as_f32(pos[i], "pos");
                t.rot[i] = json::as_f32(rot[i], "rot");
            }
            t.has_pos = true;
        }
        if (t.seq == 0) {
            throw std::runtime_error("seq 0");
        }
        *out = t;
        return true;
    } catch (const std::exception& e) {
        if (error) {
            *error = e.what();
        }
        return false;
    }
}

bool TravelMapKnown(std::uint32_t packed_map) {
    return ids::MapKnown(packed_map);
}

bool TravelLampKnown(std::uint32_t id) {
    return ids::ReturnPointKnown(id);
}

bool TravelBonfireKnown(std::uint32_t id) {
    // 0x132E050 derives the map from the id itself (area id / 100000, block id / 10000 % 10).
    return ids::EntityKnown(id) && ids::MapKnown(ids::EntityMap(id));
}

bool SanitizePeerTravel(TravelIntent* t, std::string* why) {
    if (!TravelKindBroadcast(t->kind)) {
        if (why) {
            *why = std::string("kind ") + TravelKindName(t->kind) + " is never broadcast";
        }
        return false;
    }
    auto lamp = [](std::uint32_t id) { return id == kTravelNone || TravelLampKnown(id) ? id : kTravelNone; };
    auto record = [&](std::uint64_t r) {
        const std::uint32_t lo = static_cast<std::uint32_t>(r);
        return lo == kTravelNone || TravelLampKnown(lo) ? r : ~0ull;
    };
    t->lamp_id = lamp(t->lamp_id);
    t->respawn_record = record(t->respawn_record);
    t->last_lamp = record(t->last_lamp);
    if (t->packed_map != kTravelNone && !TravelMapKnown(t->packed_map)) {
        t->packed_map = kTravelNone;
    }
    if (t->kind == TravelKind::LuaBonfireWarp) {
        if (t->warp_point != kTravelNone && !TravelBonfireKnown(t->warp_point)) {
            t->warp_point = kTravelNone;
        }
    } else if (t->warp_point != kTravelNone &&
               (t->packed_map == kTravelNone || !ids::WarpPointKnown(t->packed_map, t->warp_point))) {
        // A stage warp to a point that is not in the destination map: the map's default entry.
        t->warp_point = kTravelNone;
    }
    if (t->has_pos) {
        bool ok = TravelMapKnown(t->pos_map);
        for (int i = 0; i < 4; ++i) {
            ok = ok && std::isfinite(t->pos[i]) && std::fabs(t->pos[i]) < 100000.0f && std::isfinite(t->rot[i]) &&
                 std::fabs(t->rot[i]) < 1000.0f;
        }
        if (!ok) {
            t->has_pos = false;
            t->pos_map = kTravelNone;
        }
    }
    return true;
}

std::string TravelToJsonText(const TravelIntent& t) {
    return json::dump(TravelToJson(t), 0);
}

bool TravelFromJsonText(const std::string& text, TravelIntent* out, std::string* error) {
    json::Value v;
    std::string err;
    if (!json::parse(text, v, err)) {
        if (error) {
            *error = err;
        }
        return false;
    }
    return TravelFromJson(v, out, error);
}

const char* ReplayMethodName(ReplayMethod m) {
    switch (m) {
    case ReplayMethod::None: return "none";
    case ReplayMethod::LampWarp: return "lamp warp 0x13CDF30";
    case ReplayMethod::StageWarp: return "stage warp 0x132E010";
    case ReplayMethod::BonfireWarp: return "bonfire warp 0x132E050";
    case ReplayMethod::Transform: return "forced transform";
    }
    return "?";
}

ReplayPlan ChooseReplay(const TravelIntent& t) {
    ReplayPlan p;
    switch (t.kind) {
    case TravelKind::Lamp: {
        // The key 0x13CDF30 got; else the row's +4 it stored in +0x153C (equal in every observed
        // row, travel.md probe P5).
        const std::uint32_t id = t.lamp_id != kTravelNone ? t.lamp_id : Low(t.respawn_record);
        if (id != kTravelNone) {
            p = {ReplayMethod::LampWarp, id};
        }
        break;
    }
    case TravelKind::HostDeath:
    case TravelKind::HuntersMark:
        // Both respawn at the last lamp (+0x1528, the record 0x1332BC0 reads); same id space as
        // 0x13CDF30 (travel.md 1.1.0 / 3.2).
        if (Low(t.last_lamp) != kTravelNone) {
            p = {ReplayMethod::LampWarp, Low(t.last_lamp)};
        }
        break;
    case TravelKind::ScriptedWarp:
    case TravelKind::LuaStageWarp:
    case TravelKind::LuaStageKick:
        if (t.packed_map != kTravelNone) {
            p = {ReplayMethod::StageWarp, kTravelNone};
        }
        break;
    case TravelKind::LuaBonfireWarp:
        if (t.warp_point != kTravelNone) {
            p = {ReplayMethod::BonfireWarp, t.warp_point};
        }
        break;
    default:
        break;
    }
    if (p.method == ReplayMethod::None && t.has_pos && t.pos_map != kTravelNone) {
        p = {ReplayMethod::Transform, kTravelNone};
    }
    return p;
}

bool HostTravelQueue::Push(const TravelIntent& t) {
    if (t.seq <= last_seq_) {
        return false;
    }
    last_seq_ = t.seq;
    if (q_.size() >= kMax) {
        q_.pop_front();
    }
    q_.push_back(t);
    return true;
}

bool HostTravelQueue::Pop(TravelIntent* out) {
    if (q_.empty()) {
        return false;
    }
    *out = q_.front();
    q_.pop_front();
    return true;
}

bool ReplayAllowedNow(const ReplayState& s) {
    return s.world_up && !s.loading && !s.transition_requested && s.session_role != 4 /* joining */ &&
           s.session_role != 7 /* leaving */;
}

bool GuestTravel::Offer(const TravelIntent& t, double now) {
    if (t.seq <= last_seq_) {
        return false;
    }
    // The newest wins, also over one under way (a second warp while loading: travel.md 3.3).
    last_seq_ = t.seq;
    cur_ = t;
    phase_ = Phase::Pending;
    since_ = now;
    load_seen_ = false;
    return true;
}

std::optional<TravelIntent> GuestTravel::Step(const ReplayState& s, double now) {
    stable_ = ReplayAllowedNow(s) ? stable_ + 1 : 0;
    switch (phase_) {
    case Phase::Idle:
        return std::nullopt;
    case Phase::Pending:
        if (now - since_ > kPendingTimeout) {
            phase_ = Phase::Idle;
            return std::nullopt;
        }
        if (stable_ >= kStableTicks) {
            phase_ = Phase::Underway;
            since_ = now;
            load_seen_ = false;
            return cur_;
        }
        return std::nullopt;
    case Phase::Underway:
        // Arrived: a load happened and the world has been steady since.
        load_seen_ = load_seen_ || s.loading || !s.world_up;
        if ((load_seen_ && stable_ >= kStableTicks) || now - since_ > kActiveTimeout) {
            phase_ = Phase::Idle;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<TravelIntent> GuestTravel::Destination() const {
    if (phase_ == Phase::Idle) {
        return std::nullopt;
    }
    return cur_;
}

void GuestTravel::MarkReplayed(double now) {
    if (phase_ == Phase::Idle) {
        return;
    }
    phase_ = Phase::Underway;
    since_ = now;
    stable_ = 0;
    load_seen_ = false;
}

const char* GuestRedirectName(GuestRedirect r) {
    switch (r) {
    case GuestRedirect::None: return "none (home)";
    case GuestRedirect::TravelDestination: return "the party's travel destination";
    case GuestRedirect::HostLamp: return "the host's last lamp";
    }
    return "?";
}

GuestRedirect ChooseGuestRedirect(TravelKind kind, bool have_travel_destination, std::uint32_t host_lamp_id) {
    if (kind != TravelKind::GuestDied && kind != TravelKind::HuntersMark) {
        return GuestRedirect::None;
    }
    // A host warp in flight wins: the host will not be at its old lamp.
    if (have_travel_destination) {
        return GuestRedirect::TravelDestination;
    }
    // 0 is no lamp row either (WarpParam / bonfire ids are 7-digit map-based ids).
    if (host_lamp_id != kTravelNone && host_lamp_id != 0) {
        return GuestRedirect::HostLamp;
    }
    return GuestRedirect::None;
}

std::uint32_t HostLampFromTravel(const TravelIntent& t) {
    const std::uint32_t id = Low(t.last_lamp);
    return id == 0 ? kTravelNone : id;
}

// ---------------------------------------------------------------------------------------------
// Game part
// ---------------------------------------------------------------------------------------------
#ifndef BB_PARTY_TRAVEL_NO_GAME

namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using ull = unsigned long long;
using Clock = std::chrono::steady_clock;

// WorldTransitionState / GameStateMan slot: 0x13CDE3E `mov rax, [rip -> 0x5556678]`.
constexpr u64 kWts = 0x5556678;
constexpr u64 kWtsRequested = 0x08, kWtsMap = 0x0c, kWtsWarpPoint = 0x10, kWtsForced = 0x1520,
              kWtsLastLamp = 0x1528, kWtsRespawnMode = 0x1538, kWtsRespawn = 0x153c, kWtsOwnWorld = 0x1592;
constexpr u64 kWorldChrMan = 0x553e878; // +0x60 PlayerIns, +0x78 chr type

constexpr u64 kFunnel = 0x13cde30;        // SessionWorldTransition, no arguments, returns 0/1
constexpr u64 kLampWarp = 0x13cdf30;      // void (u32 id)
constexpr u64 kOnReviveMagic = 0x1389d20; // Lua state: Hunter's Mark (session left at 0x1389DDD)
constexpr u64 kBlockClear3 = 0x1385930;   // Lua state BlockClear2_3: tail jump 0x13859DF -> funnel
constexpr u64 kBlockClear3Jump = 0x13859df;
constexpr u64 kGuestDiedCall = 0x1382663; // PartyGhostDeath_2: `call 0x13CDE30` (e8 c8 b7 04 00)
constexpr u64 kMarkTailJump = 0x1389f20;  // OnReviveMagic_1: `jmp 0x13CDE30` (e9 0b 3f 04 00)
constexpr u64 kWarpNextStage = 0x132e010; // (ctx unused, area, block, region, u8 index, warp point)
constexpr u64 kWarpBonfire = 0x132e050;   // (ctx unused, id)
constexpr u64 kSetForcedMap = 0x156cf10;  // (const u32*) -> +0x14F0
constexpr u64 kSetForcedPos = 0x156cf20;  // (const vec4*, 16-aligned: vmovaps) -> +0x1500
constexpr u64 kSetForcedRot = 0x156cf40;  // (const vec4*, 16-aligned) -> +0x1510
constexpr u64 kSetForced = 0x156cf60;     // () -> +0x1520 = 1
constexpr u64 kSelectTarget = 0x1332bc0;  // (ctx unused, int) : +0x1520 ? +0x0C = +0x14F0 : last lamp

using FunnelFn = u64(BB_COOP_SYSV*)();
using LampWarpFn = void(BB_COOP_SYSV*)(u32);
using StageWarpFn = u64(BB_COOP_SYSV*)(u64, u32, u32, u32, u32, u32);
using BonfireWarpFn = void(BB_COOP_SYSV*)(u64, u32);
using PtrFn = void(BB_COOP_SYSV*)(const void*);
using VoidFn = void(BB_COOP_SYSV*)();
using SelectFn = u64(BB_COOP_SYSV*)(u64, u32);
using Lua6Fn = u64(BB_COOP_SYSV*)(u64, u64, u64, u64, u64, u64);

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[768];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    std::printf("Party travel: %s\n", line);
    std::fflush(stdout);
}

bool EnvOff(const char* name) {
    const char* v = std::getenv(name);
    return v && v[0] == '0' && !v[1];
}

double Now() {
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

std::atomic<bool> g_travel_on{false};
std::atomic<bool> g_in_replay{false};       // our own warp is running: the hooks stand aside
std::atomic<u32> g_lamp_key{kTravelNone};   // 0x13CDF30's edi, read by the funnel it calls
std::atomic<int> g_block_clear3{0};         // inside BlockClear2_3 (its funnel call is a tail jump)
std::atomic<u32> g_death_lamp{kTravelNone}; // guest: the host's last lamp (SetGuestDeathRedirect)
std::atomic<GuestRedirectFn> g_redirect_cb{nullptr};
std::atomic<bool> g_death_redirect_on{true};
void* g_funnel_orig = nullptr;
void* g_block_clear3_orig = nullptr;

// Seeded from the wall clock: a restarted host keeps counting upward, so guests that saw the
// old host's intents do not drop the new ones as duplicates.
std::atomic<u64> g_seq{0};
u64 NextSeq() {
    u64 cur = g_seq.load();
    if (cur == 0) {
        const u64 seed = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count());
        g_seq.compare_exchange_strong(cur, seed);
    }
    return g_seq.fetch_add(1) + 1;
}

std::mutex g_host_mu;
HostTravelQueue g_host_queue; // under g_host_mu
std::mutex g_guest_mu;
GuestTravel g_guest;          // under g_guest_mu

u64 Wts() {
    u64 p = 0;
    return SafeGet(Guest(kWts), &p) ? p : 0;
}

template <class T>
T WtsField(u64 wts, u64 off, T fallback) {
    T v{};
    return wts && SafeGet(wts + off, &v) ? v : fallback;
}

int LocalChrType() {
    u64 wcm = 0, player = 0;
    int type = -1;
    if (SafeGet(Guest(kWorldChrMan), &wcm) && wcm && SafeGet(wcm + 0x60, &player) && player) {
        SafeGet(player + 0x78, &type);
    }
    return type;
}

PartyRole Role() {
    return PartyDirector::Get().Role();
}

// ---- Host capture ----

void CaptureHost(u64 site, TravelKind kind) {
    const u64 wts = Wts();
    if (!wts) {
        return;
    }
    // Only a warp out of our own world (+0x1592 = 1; 0 = a summoned guest's) is the party's.
    if (WtsField<u8>(wts, kWtsOwnWorld, 0) == 0) {
        return;
    }
    TravelIntent t;
    t.seq = NextSeq();
    t.kind = kind;
    t.call_site = static_cast<u32>(site);
    t.packed_map = WtsField<u32>(wts, kWtsMap, kTravelNone);
    t.warp_point = WtsField<u32>(wts, kWtsWarpPoint, kTravelNone);
    t.respawn_mode = WtsField<u32>(wts, kWtsRespawnMode, 0);
    t.respawn_record = WtsField<u64>(wts, kWtsRespawn, ~0ull);
    t.last_lamp = WtsField<u64>(wts, kWtsLastLamp, ~0ull);
    if (kind == TravelKind::Lamp) {
        t.lamp_id = g_lamp_key.exchange(kTravelNone);
    }
    {
        std::lock_guard<std::mutex> lk(g_host_mu);
        g_host_queue.Push(t);
    }
    Log("host travel %s -> %s", DescribeTravel(t).c_str(), ReplayMethodName(ChooseReplay(t).method));
}

BB_COOP_SYSV void LampWarpEntry(u64 id, u64, u64, u64, u64, u64) {
    if (!g_in_replay.load()) {
        g_lamp_key = static_cast<u32>(id);
    }
}

BB_COOP_SYSV void ReviveMagicEntry(u64, u64, u64, u64, u64, u64) {
    // Hunter's Mark: OnReviveMagic leaves the session at 0x1389DDD and reaches the funnel only
    // later through OnReviveMagic_1's tail jump, so the intent is taken here. Phantoms (1, 2,
    // 0xC) are not the host; only an own-world body (0, or 8 dead/grey) is.
    if (!g_travel_on.load() || g_in_replay.load() || Role() != PartyRole::Host) {
        return;
    }
    const int type = LocalChrType();
    if (type == 0 || type == 8) {
        CaptureHost(kOnReviveMagic, TravelKind::HuntersMark);
    }
}

// ---- Guest replay ----

void ClearForcedPlacement() {
    // The send-home code set +0x1520 (forced = pre-summon spot); our target must win.
    if (const u64 wts = Wts()) {
        const u8 zero = 0;
        SafeWrite(wts + kWtsForced, &zero, 1);
    }
}

/// Runs the native warp for `t`; false when there is nothing to run.
bool Replay(const TravelIntent& t, const ReplayPlan& p) {
    if (p.method == ReplayMethod::None) {
        return false;
    }
    g_in_replay = true;
    switch (p.method) {
    case ReplayMethod::LampWarp:
        ClearForcedPlacement();
        reinterpret_cast<LampWarpFn>(Guest(kLampWarp))(p.id);
        break;
    case ReplayMethod::StageWarp:
        ClearForcedPlacement();
        reinterpret_cast<StageWarpFn>(Guest(kWarpNextStage))(0, t.Area(), t.Block(), t.Region(), t.Index(),
                                                              t.warp_point);
        break;
    case ReplayMethod::BonfireWarp:
        ClearForcedPlacement();
        reinterpret_cast<BonfireWarpFn>(Guest(kWarpBonfire))(0, p.id);
        break;
    case ReplayMethod::Transform: {
        // HostDead_1's own sequence with the host's transform (travel.md 3.2).
        alignas(16) float pos[4], rot[4];
        std::memcpy(pos, t.pos, sizeof pos);
        std::memcpy(rot, t.rot, sizeof rot);
        const u32 map = t.pos_map;
        reinterpret_cast<PtrFn>(Guest(kSetForcedMap))(&map);
        reinterpret_cast<PtrFn>(Guest(kSetForcedPos))(pos);
        reinterpret_cast<PtrFn>(Guest(kSetForcedRot))(rot);
        reinterpret_cast<VoidFn>(Guest(kSetForced))();
        reinterpret_cast<SelectFn>(Guest(kSelectTarget))(0, kTravelNone);
        reinterpret_cast<FunnelFn>(Guest(kFunnel))();
        break;
    }
    case ReplayMethod::None:
        break;
    }
    g_in_replay = false;
    return true;
}

/// A stock "send home" warp at `site` on a guest: go to the party's destination instead.
bool RetargetSendHome(u64 site) {
    if (!g_travel_on.load() || g_in_replay.load() || Role() != PartyRole::Guest) {
        return false;
    }
    std::optional<TravelIntent> dest;
    {
        std::lock_guard<std::mutex> lk(g_guest_mu);
        dest = g_guest.Destination();
    }
    if (!dest) {
        return false;
    }
    const ReplayPlan p = ChooseReplay(*dest);
    if (p.method == ReplayMethod::None) {
        Log("send-home at +0x%llx: travel %s has no usable destination; going home as usual", ull(site),
            DescribeTravel(*dest).c_str());
        return false;
    }
    Log("send-home at +0x%llx retargeted to %s via %s", ull(site), DescribeTravel(*dest).c_str(),
        ReplayMethodName(p.method));
    Replay(*dest, p);
    std::lock_guard<std::mutex> lk(g_guest_mu);
    if (g_guest.LastSeq() == dest->seq) {
        g_guest.MarkReplayed(Now());
    }
    return true;
}

/// A guest's own death (PartyGhostDeath_2) or Hunter's Mark (OnReviveMagic_1) at `site`: the
/// party's travel destination, else the host's last lamp, instead of home (phantom_limits.md 3.2).
/// Both callers already set the forced-placement byte (+0x1520 = pre-summon spot) and
/// PartyGhostDeath_2 also GameDataMan+0x70 (full recover after the next load), which stays.
bool RedirectGuest(u64 site, TravelKind kind) {
    if (!g_travel_on.load() || !g_death_redirect_on.load() || g_in_replay.load() || Role() != PartyRole::Guest) {
        return false;
    }
    bool have_dest;
    {
        std::lock_guard<std::mutex> lk(g_guest_mu);
        have_dest = g_guest.Destination().has_value();
    }
    const u32 lamp = g_death_lamp.load();
    const GuestRedirect how = ChooseGuestRedirect(kind, have_dest, lamp);
    bool done = false;
    GuestRedirect used = how;
    if (how == GuestRedirect::TravelDestination) {
        done = RetargetSendHome(site);
    }
    if (!done && lamp != kTravelNone && lamp != 0 && how != GuestRedirect::None) {
        used = GuestRedirect::HostLamp;
        TravelIntent t;
        t.kind = kind;
        t.lamp_id = lamp;
        Log("%s at +0x%llx: to the host's last lamp %u via %s", TravelKindName(kind), ull(site), lamp,
            ReplayMethodName(ReplayMethod::LampWarp));
        done = Replay(t, {ReplayMethod::LampWarp, lamp});
    }
    if (!done) {
        Log("%s at +0x%llx: no host lamp known and no travel pending; going home as usual", TravelKindName(kind),
            ull(site));
        return false;
    }
    if (GuestRedirectFn cb = g_redirect_cb.load()) {
        cb(kind, used, lamp);
    }
    return true;
}

// ---- Hooks ----

// Replaces 0x13CDE30 (jumped to from its entry, so the return address is the game caller's).
__attribute__((noinline)) BB_COOP_SYSV u64 FunnelHook() {
    const u64 ret = reinterpret_cast<u64>(__builtin_return_address(0));
    if (g_travel_on.load() && !g_in_replay.load()) {
        if (g_block_clear3.load() > 0) {
            if (RetargetSendHome(kBlockClear3Jump)) {
                return 1;
            }
        } else if (Role() == PartyRole::Host) {
            const u64 site = ret - Guest(0) - 5;
            const TravelKind kind = TravelKindFromCallSite(site);
            if (TravelKindBroadcast(kind) && kind != TravelKind::HuntersMark) {
                CaptureHost(site, kind);
            }
        } else if (ret - Guest(0) - 5 == kGuestDiedCall) {
            if (RedirectGuest(kGuestDiedCall, TravelKind::GuestDied)) {
                return 1;
            }
        }
    }
    return reinterpret_cast<FunnelFn>(g_funnel_orig)();
}

// The retargeted `call 0x13CDE30` at OnLeave_Limit / HostDead_1 / BlockClear2_1.
__attribute__((noinline)) BB_COOP_SYSV u64 SendHomeHook() {
    const u64 site = reinterpret_cast<u64>(__builtin_return_address(0)) - Guest(0) - 5;
    if (RetargetSendHome(site)) {
        return 1;
    }
    return reinterpret_cast<FunnelFn>(Guest(kFunnel))();
}

// OnReviveMagic_1's tail jump to the funnel (a guest's Hunter's Mark; the host's is captured at
// OnReviveMagic's entry). Entered by `jmp`, so returning returns to OnReviveMagic_1's caller.
__attribute__((noinline)) BB_COOP_SYSV u64 MarkTailHook() {
    if (RedirectGuest(kMarkTailJump, TravelKind::HuntersMark)) {
        return 1;
    }
    return reinterpret_cast<FunnelFn>(Guest(kFunnel))();
}

// BlockClear2_3 reaches the funnel by a tail jump: mark the call so FunnelHook knows it.
__attribute__((noinline)) BB_COOP_SYSV u64 BlockClear3Hook(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    g_block_clear3.fetch_add(1);
    const u64 r = reinterpret_cast<Lua6Fn>(g_block_clear3_orig)(a0, a1, a2, a3, a4, a5);
    g_block_clear3.fetch_sub(1);
    return r;
}

// ---- Data / code patches ----

bool WriteImage(u64 off, const u8* bytes, std::size_t n) {
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

struct PatchSite {
    u64 off;
    std::vector<u8> original;
    std::vector<u8> patched;
};

/// Writes a group of sites together: all original (write), all patched already (the XML patch
/// did it), else nothing.
void PatchGroup(const char* name, const std::vector<PatchSite>& sites) {
    bool all_orig = true, all_patched = true;
    for (const PatchSite& s : sites) {
        all_orig = all_orig && Matches(s.off, s.original.data(), s.original.size());
        all_patched = all_patched && Matches(s.off, s.patched.data(), s.patched.size());
    }
    if (all_patched) {
        Log("%s: already applied", name);
        return;
    }
    if (!all_orig) {
        for (const PatchSite& s : sites) {
            if (!Matches(s.off, s.original.data(), s.original.size()) && !Matches(s.off, s.patched.data(), s.patched.size())) {
                Log("%s: +0x%llx holds neither the 1.09 bytes nor the patch; skipped", name, ull(s.off));
            }
        }
        Log("%s: MISMATCH, not applied", name);
        return;
    }
    for (const PatchSite& s : sites) {
        if (!WriteImage(s.off, s.patched.data(), s.patched.size())) {
            Log("%s: +0x%llx could not be written", name, ull(s.off));
            return;
        }
    }
    Log("%s: applied", name);
}

bool JumpsTo(u64 off, u64 target) {
    const unsigned char* img = Image();
    if (!img || off + 5 > ImageSize() || img[off] != 0xe9) {
        return false;
    }
    std::int32_t rel;
    std::memcpy(&rel, img + off + 1, 4);
    return off + 5 + std::int64_t(rel) == target;
}

} // namespace

void InstallTravelPatches() {
    static std::atomic<bool> done{false};
    if (done.exchange(true)) {
        return;
    }
    if (!Image()) {
        Log("no image; travel off");
        return;
    }
    if (EnvOff("BB_PARTY_DREAM_GATE")) {
        Log("BB_PARTY_DREAM_GATE=0: Hunter's Dream stays closed to co-op");
    } else {
        // Area restriction table TBL1[m21][0][0] (read by the area validator 0x131D7B0 for all 14
        // bell / SOS / summon consumers): the Dream's flag 2100 (0x834), always on there -> -1
        // ("no flag"), so the Dream is no longer a restricted area (travel.md section 2).
        PatchGroup("Hunter's Dream area gate (0x47304B0 = -1)",
                   {{0x47304b0, {0x34, 0x08, 0x00, 0x00}, {0xff, 0xff, 0xff, 0xff}}});
        // Small Resonant / Sinister availability in 0x157F200: `mov bl, al` (responder search
        // result) -> `mov cl, 1`, then `jmp 0x15800C1` (the canary-checked epilogue returning cl)
        // over `mov rdi, r14; call 0x18C96D0`. The same bytes as the XML "Party: Bells anywhere".
        PatchGroup("Small Resonant Bell in the Dream (0x157F6D1 / 0x157F6D3)",
                   {{0x157f6d1, {0x88, 0xc3}, {0xb1, 0x01}},
                    {0x157f6d3, {0x4c, 0x89, 0xf7, 0xe8, 0xf5, 0x9f, 0x34, 0x00},
                     {0xe9, 0xe9, 0x09, 0x00, 0x00, 0x90, 0x90, 0x90}}});
    }
    if (EnvOff("BB_PARTY_TRAVEL")) {
        Log("BB_PARTY_TRAVEL=0: no follow travel");
        return;
    }
    // push rbp; mov rbp, rsp; push r15, r14, r13, r12, rbx; push rax (14 bytes; next is the
    // rip-relative load of the WorldTransitionState slot).
    const bool funnel = ReplacePrologue(
        kFunnel, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x50},
        reinterpret_cast<const void*>(&FunnelHook), &g_funnel_orig, "travel funnel (SessionWorldTransition)");
    // push rbp; mov rbp, rsp; push r15, r14, rbx; sub rsp, 0x38 (13 bytes; then mov ebx, edi).
    HookPrologue(kLampWarp, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x38},
                 &LampWarpEntry, "travel lamp key (0x13CDF30)");
    // push rbp; mov rbp, rsp; push r15, r14, r13, r12, rbx; push rax (14 bytes; then mov rbx, rsi).
    HookPrologue(kOnReviveMagic, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x50},
                 &ReviveMagicEntry, "travel Hunter's Mark (OnReviveMagic)");
    // Guest send-home sites: `call 0x13CDE30` right after the forced-placement selector.
    if (funnel) {
        HookCallSite(0x138a51c, kFunnel, reinterpret_cast<const void*>(&SendHomeHook), "send-home OnLeave_Limit");
        HookCallSite(0x1381a93, kFunnel, reinterpret_cast<const void*>(&SendHomeHook), "send-home HostDead_1");
        HookCallSite(0x13856f4, kFunnel, reinterpret_cast<const void*>(&SendHomeHook), "send-home BlockClear2_1");
        // BlockClear2_3: push rbp; mov rbp, rsp; push rbx; push rax (6 bytes), and its funnel tail
        // jump; FunnelHook is what sees that call.
        if (JumpsTo(kBlockClear3Jump, kFunnel)) {
            ReplacePrologue(kBlockClear3, {0x55, 0x48, 0x89, 0xe5, 0x53, 0x50},
                            reinterpret_cast<const void*>(&BlockClear3Hook), &g_block_clear3_orig,
                            "send-home BlockClear2_3");
        } else {
            Log("send-home BlockClear2_3: +0x%llx is not a jmp to the funnel; not installed", ull(kBlockClear3Jump));
        }
        // Guest death: the direct call 0x1382663 reaches FunnelHook with its own return address.
        if (!CallsTo(kGuestDiedCall, kFunnel)) {
            Log("guest death redirect: +0x%llx is not a call to the funnel; off", ull(kGuestDiedCall));
        } else {
            Log("guest death redirect: PartyGhostDeath_2 call +0x%llx seen by the funnel hook", ull(kGuestDiedCall));
        }
        HookTailJump(kMarkTailJump, kFunnel, reinterpret_cast<const void*>(&MarkTailHook),
                     "guest Hunter's Mark redirect (OnReviveMagic_1)");
    } else {
        Log("no funnel hook: the host cannot report travel and send-home retargets are off");
    }
    // The guest replay needs no hook (it runs from the coop tick).
    g_travel_on = true;
}

bool PopHostTravel(TravelIntent* out) {
    std::lock_guard<std::mutex> lk(g_host_mu);
    return g_host_queue.Pop(out);
}

void RequestGuestTravel(const TravelIntent& t) {
    if (!g_travel_on.load()) {
        return;
    }
    // Any intent with a last-lamp record also tells where the host respawns now.
    if (const u32 lamp = HostLampFromTravel(t); lamp != kTravelNone) {
        g_death_lamp = lamp;
    }
    bool taken;
    {
        std::lock_guard<std::mutex> lk(g_guest_mu);
        taken = g_guest.Offer(t, Now());
    }
    Log("guest travel %s %s", DescribeTravel(t).c_str(), taken ? "queued" : "ignored (not newer than the last)");
}

void TravelTick() {
    if (!g_travel_on.load()) {
        return;
    }
    const GameSnapshot g = ReadGameState(false);
    ReplayState s;
    s.world_up = g.world_up;
    s.loading = g.loading;
    s.session_role = g.session_role;
    s.transition_requested = WtsField<u8>(Wts(), kWtsRequested, 1) != 0;
    std::optional<TravelIntent> now;
    GuestTravel::Phase before, after;
    {
        std::lock_guard<std::mutex> lk(g_guest_mu);
        before = g_guest.GetPhase();
        now = g_guest.Step(s, Now());
        after = g_guest.GetPhase();
    }
    if (now) {
        const ReplayPlan p = ChooseReplay(*now);
        if (p.method == ReplayMethod::None) {
            Log("replay %s: no usable destination; staying", DescribeTravel(*now).c_str());
        } else {
            Log("replay %s via %s (%s)", DescribeTravel(*now).c_str(), ReplayMethodName(p.method),
                Describe(g).c_str());
            Replay(*now, p);
        }
    } else if (before != after && after == GuestTravel::Phase::Idle) {
        Log(before == GuestTravel::Phase::Pending ? "travel dropped: never replayable within %.0f s"
                                                  : "travel finished (arrived, or %.0f s passed)",
            before == GuestTravel::Phase::Pending ? GuestTravel::kPendingTimeout : GuestTravel::kActiveTimeout);
    }
}

bool TravelEnabled() {
    return g_travel_on.load();
}

void SetGuestDeathRedirect(std::uint32_t host_lamp_id) {
    const u32 was = g_death_lamp.exchange(host_lamp_id);
    if (was != host_lamp_id) {
        Log("guest death redirect: host's last lamp %d (was %d)", static_cast<int>(host_lamp_id), static_cast<int>(was));
    }
}

std::uint32_t GuestDeathRedirectLamp() {
    return g_death_lamp.load();
}

void SetGuestRedirectCallback(GuestRedirectFn fn) {
    g_redirect_cb = fn;
}

void SetGuestDeathRedirectEnabled(bool on) {
    g_death_redirect_on = on;
}

#endif // BB_PARTY_TRAVEL_NO_GAME

} // namespace coop

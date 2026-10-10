// SPDX-License-Identifier: GPL-3.0-or-later
// Game state reads for the party director (see game_state.h). Offsets from droogie/bbhost
// (GPL-3.0-or-later; src/engine/np_test.cpp, loading.cpp, player_data.cpp and
// include/bbhost/engine/sprj/*.hpp @8f2746c), each checked against our 1.09 eboot.
#include "game_state.h"

#include "coop_hooks.h"
#include "../bbport_threads.h"

#include <cstdio>
#include <cstring>

namespace coop {
namespace {

constexpr std::uint64_t kWorldChrMan = 0x553e878;
constexpr std::uint64_t kSessionManager = 0x5540290;
constexpr std::uint64_t kGameDataMan = 0x553b130;
constexpr std::uint64_t kNowLoadingRequested = 0x556286b;
constexpr std::uint64_t kNowLoadingHelper = 0x553e8b8;
constexpr std::uint64_t kNetFlow = 0x5556678;
constexpr std::uint64_t kNpManager = 0x56c7048;
constexpr std::uint64_t kCooperatorCount = 0x15bdc20;

constexpr std::uint64_t kRecInsight = 0x84, kRecLevel = 0x90;

std::uint64_t Slot(std::uint64_t off) {
    std::uint64_t v = 0;
    const std::uint64_t at = Guest(off);
    return at && SafeGet(at, &v) ? v : 0;
}

template <class T>
T Field(std::uint64_t base, std::uint64_t off, T fallback) {
    T v{};
    return base && SafeGet(base + off, &v) ? v : fallback;
}

bool Plausible(std::uint64_t address, std::size_t n) {
    return address >= 0x10000 && address + n < (1ull << 47) && address + n >= address;
}

} // namespace

__attribute__((noinline)) bool SafeRead(std::uint64_t address, void* out, std::size_t n) {
    if (!Plausible(address, n)) {
        return false;
    }
    BbRecoverBuf* const prev = runtime_fault_recover;
    BbRecoverBuf recover;
    if (BB_RECOVER_SET(recover)) {
        runtime_fault_recover = prev; // the fault handler cleared it
        return false;
    }
    runtime_fault_recover = &recover;
    const volatile unsigned char* src = reinterpret_cast<const volatile unsigned char*>(address);
    unsigned char* dst = static_cast<unsigned char*>(out);
    for (std::size_t i = 0; i < n; ++i) {
        dst[i] = src[i];
    }
    runtime_fault_recover = prev;
    return true;
}

__attribute__((noinline)) bool SafeWrite(std::uint64_t address, const void* in, std::size_t n) {
    if (!Plausible(address, n)) {
        return false;
    }
    BbRecoverBuf* const prev = runtime_fault_recover;
    BbRecoverBuf recover;
    if (BB_RECOVER_SET(recover)) {
        runtime_fault_recover = prev;
        return false;
    }
    runtime_fault_recover = &recover;
    volatile unsigned char* dst = reinterpret_cast<volatile unsigned char*>(address);
    const unsigned char* src = static_cast<const unsigned char*>(in);
    for (std::size_t i = 0; i < n; ++i) {
        dst[i] = src[i];
    }
    runtime_fault_recover = prev;
    return true;
}

const char* SessionRoleName(int role) {
    switch (role) {
    case RoleIdle: return "idle";
    case RoleTryingToHost: return "trying-to-host";
    case RoleFailedToHost: return "failed-to-host";
    case RoleHost: return "host";
    case RoleTryingToJoin: return "trying-to-join";
    case RoleJoinFailed: return "join-failed";
    case RoleClient: return "client";
    case RoleLeaving: return "leaving";
    default: return "?";
    }
}

GameSnapshot ReadGameState(bool call_game) {
    GameSnapshot s;
    if (!Image()) {
        return s;
    }
    s.image = true;
    s.world_chr_man = Slot(kWorldChrMan);
    s.player = Field<std::uint64_t>(s.world_chr_man, 0x60, 0);
    s.session_man = Slot(kSessionManager);
    s.world_up = s.world_chr_man && s.player && s.session_man;
    if (s.session_man) {
        s.session_sub_state = Field<std::int32_t>(s.session_man, 0x120, -1);
        s.session_role = Field<std::int32_t>(s.session_man, 0x124, -1);
        s.matching_status = Field<std::int32_t>(s.session_man, 0x270, -1);
    }
    std::uint8_t requested = 0;
    const std::uint64_t req_at = Guest(kNowLoadingRequested);
    SafeGet(req_at, &requested);
    const std::uint64_t helper = Slot(kNowLoadingHelper);
    s.in_game_frame = helper && (Field<std::uint8_t>(helper, 0x50, 0) || Field<std::uint8_t>(helper, 0x51, 0));
    s.loading = requested && !s.in_game_frame;
    s.game_data_man = Slot(kGameDataMan);
    s.player_rec = Field<std::uint64_t>(s.game_data_man, 8, 0);
    if (s.player_rec) {
        s.insight = Field<std::int32_t>(s.player_rec, kRecInsight, -1);
        s.level = Field<std::int32_t>(s.player_rec, kRecLevel, -1);
    }
    if (s.player) {
        // PlayerIns_GetBloodMarkMap (0x19046c0): *(*(player + 0x400) + 0x48).
        const std::uint64_t history = Field<std::uint64_t>(s.player, 0x400, 0);
        s.map_id = Field<std::uint32_t>(history, 0x48, 0xffffffffu);
    }
    const std::uint64_t flow = Slot(kNetFlow);
    if (flow) {
        s.online_mode = Field<std::uint8_t>(flow, 0x1590, 0xff);
    }
    if (const std::uint64_t np = Slot(kNpManager)) {
        s.np_online = Field<std::uint8_t>(np, 0xd8, 0xff);
    }
    // The game's own count (0x15bdc20): it asserts (and then reads through NULL) without
    // WorldChrMan or, for its own slot, SprjSessionManager; asked only in the world, not loading.
    const std::uint64_t table = Field<std::uint64_t>(flow, 0x16f8, 0);
    if (call_game && s.world_up && !s.loading && table) {
        using CountFn = std::int32_t(BB_COOP_SYSV*)(std::uint64_t);
        s.cooperators = reinterpret_cast<CountFn>(Guest(kCooperatorCount))(table);
    }
    return s;
}

std::string MapName(std::uint32_t map_id) {
    if (map_id == 0xffffffffu) {
        return "-";
    }
    char text[24];
    std::snprintf(text, sizeof text, "m%02u_%02u_%02u_%02u", (map_id >> 24) & 0xff, (map_id >> 16) & 0xff,
                  (map_id >> 8) & 0xff, map_id & 0xff);
    return text;
}

std::string Describe(const GameSnapshot& s) {
    char text[384];
    std::snprintf(text, sizeof text,
                  "world %s, loading %s, map %s (0x%08x), level %d, Insight %d, session %s (%d, sub %d, matching %d), "
                  "cooperators %d, online mode %d, NP online %d",
                  s.world_up ? "up" : "down", s.loading ? "yes" : "no", MapName(s.map_id).c_str(), s.map_id, s.level,
                  s.insight, SessionRoleName(s.session_role), s.session_role, s.session_sub_state, s.matching_status,
                  s.cooperators, s.online_mode, s.np_online);
    return text;
}

bool WritePlayerInsight(int insight) {
    const std::uint64_t gdm = Slot(kGameDataMan);
    const std::uint64_t rec = Field<std::uint64_t>(gdm, 8, 0);
    if (!rec) {
        return false;
    }
    const std::int32_t v = insight;
    return SafeWrite(rec + kRecInsight, &v, 4);
}

} // namespace coop

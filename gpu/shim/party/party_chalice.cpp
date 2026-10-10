// SPDX-License-Identifier: GPL-3.0-or-later
// Chalice Dungeons in party play: the local game's dungeon (party_chalice.h).
#include "party_chalice.h"

#ifndef BB_PARTY_CHALICE_NO_GAME
#include "game_state.h"

#include <atomic>
#include <chrono>
#include <cstdio>

namespace coop::chalice {

namespace {

constexpr std::uint64_t kEventFlagManSlot = 0x553b100;
constexpr std::uint64_t kLoadModeOff = 0x80;

std::atomic<std::uint64_t> g_local{0};

double Seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

} // namespace

State Read() {
    State s;
    std::uint64_t man = 0;
    if (!SafeGet(kEventFlagManSlot, &man) || man < 0x10000) {
        return s;
    }
    std::int32_t mode = -1;
    if (SafeGet(man + kLoadModeOff, &mode)) {
        s.load_mode = mode;
    }
    std::uint32_t bits = 0;
    if (!ReadEventFlagValue(kSlotFlag, kSlots, &bits)) {
        return s;
    }
    s.valid = true;
    s.slot = SlotFromBits(bits);
    if (s.slot >= 0) {
        const std::uint32_t base = kRecordBase + static_cast<std::uint32_t>(s.slot) * kRecordStride;
        std::uint32_t lo = 0, hi = 0x80000000u;
        if (ReadEventFlagValue(base + kChannelLoBit, 32, &lo) && ReadEventFlagValue(base + kChannelHiBit, 32, &hi)) {
            s.channel = ChannelFromDwords(lo, hi);
        }
    }
    return s;
}

void Tick() {
    static double next = 0;
    static State last;
    const double now = Seconds();
    if (now < next) {
        return;
    }
    next = now + 2.0;
    const GameSnapshot g = ReadGameState(false);
    if (!g.world_up || g.loading) {
        return;
    }
    const State s = Read();
    if (!s.valid) {
        return;
    }
    if (s.load_mode == 0) {
        g_local.store(s.slot >= 0 ? s.channel : 0);
    }
    if (s.slot != last.slot || s.channel != last.channel || s.load_mode != last.load_mode) {
        std::printf("Party: chalice: slot %d, dungeon %llu, load mode %d (map %s)\n", s.slot,
                    static_cast<unsigned long long>(s.channel), s.load_mode, MapName(g.map_id).c_str());
        std::fflush(stdout);
        last = s;
    }
}

std::uint64_t LocalChannel() {
    return g_local.load();
}

} // namespace coop::chalice
#endif

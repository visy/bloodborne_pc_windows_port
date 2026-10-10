// SPDX-License-Identifier: GPL-3.0-or-later
// Chalice Dungeons in party play (docs/party/chalice.md): which dungeon the local game has.
//
// A player's Chalice Dungeons live in the event flag store, not in a separate save structure:
// slot k (0..6) is selected by the one-hot flags 9020+k (GetEventFlagValue(9020, 7) -> slot
// = 7 - bit length), and the ritual helper CSDungeonRitualHelper writes the slot's dungeon
// record into the type-9 flags of area 40+k (block 0, zone 0: ids 94000000 + k * 100000):
//   +32 / +64   ChannelId, low / high dword (0x8000000000000000: never uploaded)  0x1ab2970
//   +96..+127   4 bytes (helper +0xe0)                                          0x1ab2b40
//   +128        32 bits (helper +0xe8)
//   +160..+735  9 x {u32, u32} (helper +0xf8, the layout parameters)            0x1ab2f70
//   +736        16 bytes (helper +0x140)
//   +992..      0x30 bytes (helper +0x158, the creator's online id serialization)
// (writers 0x1aa7c00 SetupHolygrail, 0x1aa7f80 / 0x1aa81d0 random join, 0x1aa8410 quick join,
// 0x1aaa3d0 state switch). The ChannelId is the board's id (from_api_channel.h), so two games
// with the same ChannelId in their current slot generate the same dungeon.
//
// The party layer only READS these flags: 9020-9026 and the chalice groups are never written
// from outside (party_progress NeverWritable).
//
// Main thread only (game calls).
#pragma once

#include <cstdint>

namespace coop::chalice {

constexpr std::uint32_t kSlotFlag = 9020;         ///< 9020..9026 one-hot
constexpr int kSlots = 7;
constexpr std::uint32_t kRecordBase = 94000000;   ///< T9, area 40 + slot, block 0, zone 0
constexpr std::uint32_t kRecordStride = 100000;
constexpr std::uint32_t kChannelLoBit = 32, kChannelHiBit = 64;

/// The slot from GetEventFlagValue(9020, 7) (9026 is bit 0); -1 when no slot flag is set.
constexpr int SlotFromBits(std::uint32_t bits7) {
    int len = 0;
    for (std::uint32_t v = bits7 & 0x7fu; v; v >>= 1) ++len;
    const int slot = kSlots - len;
    return slot >= 0 && slot < kSlots ? slot : -1;
}
/// The ChannelId of two dwords; 0 when it is the game's "none" (top bit set) or 0.
constexpr std::uint64_t ChannelFromDwords(std::uint32_t lo, std::uint32_t hi) {
    return (hi & 0x80000000u) ? 0 : (static_cast<std::uint64_t>(hi) << 32 | lo);
}

struct State {
    bool valid = false;         ///< the flags could be read
    int slot = -1;              ///< -1: no chalice slot selected
    std::uint64_t channel = 0;  ///< the slot's ChannelId, 0: none / never uploaded
    int load_mode = -1;         ///< SprjEventFlagMan +0x80: 0 own world, 1 another world's flags
};

#ifndef BB_PARTY_CHALICE_NO_GAME
/// Reads the local game's chalice slot and its ChannelId.
State Read();
/// Once a frame (coop tick): re-reads every ~2 s, logs changes, and keeps LocalChannel().
void Tick();
/// The dungeon the local game has in its current slot while it is in its own world (load mode
/// 0); 0 when none or unknown. Any thread (the last Tick's value).
std::uint64_t LocalChannel();
#endif

} // namespace coop::chalice

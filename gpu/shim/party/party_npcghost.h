// SPDX-License-Identifier: GPL-3.0-or-later
// Talk NPCs stay solid for guests (playtest 1 bugs 11 / 17, part of 5).
//
// On a client, chr creation 0x18E4F94..0x18E4FDC turns an NPC whose NpcParam row has
// isChangeWanderGhost (byte 0x145 bit 7) into a wandering ghost: chr type 4, translucent, no
// collision and no talk. Vanilla does that so a phantom cannot talk to the host's NPCs; the party
// lets guests talk to them (D8 world talk: the quest flags go to the host as world actions), so
// every talk NPC's row loses the bit. seamless_rules.cpp writes the rows (BB_PARTY_GUEST_SERVICES=0
// leaves them vanilla); only clients read the bit, so the host and solo play see no change.
//
// Rows (Bloodborne 1.09 gameparam NpcParam, tools/party/flag_tool.py load_params): every row with
// the bit that a talk NPC uses. Enemies with the bit (109100, 111010-111012, 252100, 402xxx,
// 405100, 409xxx, 451100, 504020/504030 the beast beggar, 513000, the chalice rows ...) keep it.
#pragma once

#include <cstddef>
#include <cstdint>

namespace coop::npcghost {

struct GhostRow {
    std::uint32_t row;
    std::uint8_t byte;  ///< 1.09 value of byte 0x145 (bit 7 set; 113010 also has isUnduration)
    const char* who;
};

constexpr std::uint32_t kField = 0x145;
constexpr std::uint8_t kBit = 0x80;

inline constexpr GhostRow kTalkNpcRows[] = {
    {6040, 0x80, "talk NPC 6040"},
    {6060, 0x80, "Iosefka (clinic door)"},
    {6070, 0x80, "the fake Iosefka"},
    {6071, 0x80, "the fake Iosefka (2)"},
    {6080, 0x80, "the old man (Cathedral Ward)"},
    {6090, 0x80, "the lonely old woman"},
    {6100, 0x80, "talk NPC 6100"},
    {6110, 0x80, "Arianna"},
    {6111, 0x80, "Arianna (2)"},
    {6112, 0x80, "Arianna (3)"},
    {6113, 0x80, "Arianna (after the birth)"},
    {6120, 0x80, "talk NPC 6120"},
    {6130, 0x80, "talk NPC 6130"},
    {6140, 0x80, "talk NPC 6140"},
    {6141, 0x80, "talk NPC 6141"},
    {6142, 0x80, "talk NPC 6142"},
    {6143, 0x80, "talk NPC 6143"},
    {6144, 0x80, "talk NPC 6144"},
    {6150, 0x80, "Alfred"},
    {6151, 0x80, "Alfred (Oedon Chapel steps)"},
    {6154, 0x80, "Alfred (3)"},
    {6155, 0x80, "Alfred (4)"},
    {6160, 0x80, "Eileen"},
    {6161, 0x80, "Eileen (2)"},
    {6180, 0x80, "talk NPC 6180"},
    {6210, 0x80, "talk NPC 6210"},
    {6500, 0x80, "talk NPC 6500"},
    {6510, 0x80, "talk NPC 6510"},
    {6511, 0x80, "talk NPC 6511"},
    {6521, 0x80, "talk NPC 6521"},
    {6590, 0x80, "talk NPC 6590"},
    {6970, 0x80, "talk NPC 6970"},
    {6980, 0x80, "talk NPC 6980"},
    {6990, 0x80, "talk NPC 6990"},
    {8120, 0x80, "talk NPC 8120"},
    {8121, 0x80, "talk NPC 8121"},
    {8122, 0x80, "talk NPC 8122"},
    {8123, 0x80, "talk NPC 8123"},
    {8125, 0x80, "talk NPC 8125"},
    {113010, 0xc0, "the Oedon Chapel Dweller"},
    {803000, 0x80, "the Doll"},
    {804000, 0x80, "Gehrman"},
};
inline constexpr std::size_t kTalkNpcRowCount = sizeof(kTalkNpcRows) / sizeof(kTalkNpcRows[0]);

inline const GhostRow* FindTalkNpcRow(std::uint32_t row) {
    for (const GhostRow& r : kTalkNpcRows) {
        if (r.row == row) {
            return &r;
        }
    }
    return nullptr;
}

enum class Verdict { write, holds, refuse };

/// What to do with a row whose byte 0x145 reads `now`: the 1.09 value -> write it without bit 7
/// (`*out`), already without it -> holds, anything else -> refuse (another data version: leave it).
inline Verdict Decide(const GhostRow& r, std::uint8_t now, std::uint8_t* out) {
    const std::uint8_t want = std::uint8_t(r.byte & ~kBit);
    if (out) {
        *out = want;
    }
    if (now == want) {
        return Verdict::holds;
    }
    return now == r.byte ? Verdict::write : Verdict::refuse;
}

} // namespace coop::npcghost

// SPDX-License-Identifier: GPL-3.0-or-later
// Guest services: a summoned party guest uses the Doll (level up), the Bath Messengers' shops
// (echoes and Insight) and the workshop tools (weapon upgrade, Blood Gem workbench, storage) in
// the host's Hunter's Dream, on its own character (docs/party/guest_services.md).
//
// The gate (static RE, docs/party/guest_services.md 2): every Hunter's Dream talk script that opens
// one of those menus (t210140 Doll, t21069x workshop / messenger menus, ...) parks in its
// "クライアント待機" (client wait) state machine while the talk test function 19
// `IsClientPlayer()` is 1. That function is case 19 of the talk environment's test-function switch
// 0x203AD60 (ids 0..62, `cmp eax,0x3e` at 0x203AD97): `movzx eax, byte [r15+0x25]` at 0x203BA78,
// then `jmp 0x203EECF` (result type 2 = int, value eax). The override returns 0 there while the
// gate is open; the scripts then run their normal (host) branch on the guest's own game. Menus
// act on the local PlayerGameData (level, echoes, Insight, inventory, storage) - the guest's own
// character, written to the guest's own save by the game's save writer; nothing goes to the host.
// Talk-script flags (SetFlagState) on a summoned guest go to the load-mode-1 overlay (discarded),
// except 6000-8999 (personal: Doll / messenger / shop states), which stay in the guest's own pool.
//
// Gate open = BB_PARTY_GUEST_SERVICES on (default in party mode) and the local player is a client
// (session role 6) in the world (not loading) on map m21 (Hunter's Dream). Everywhere else the
// game's own answer stands (quest NPCs in the host's world keep refusing guests).
//
// Env: BB_PARTY_GUEST_SERVICES=0 turns it off (vanilla: guests cannot use them in the host's world).
//
// The pure part has no game dependency and is unit-tested (tests/test_party_guestsvc.cpp, built
// with BB_PARTY_GUESTSVC_NO_GAME).
#pragma once

#include <cstdint>
#include <string>

namespace coop {

constexpr int kSessionRoleClient = 6;

/// m21_xx_xx_xx (Hunter's Dream, both versions). `map_id` = 0xAABBCCDD.
bool IsDreamMap(std::uint32_t map_id);

struct GuestServicesInput {
    bool enabled = false;   ///< BB_PARTY_GUEST_SERVICES (and party mode)
    bool world_up = false;  ///< WorldChrMan + local player + session manager
    bool loading = false;   ///< a loading screen is up
    int session_role = -1;  ///< SprjSessionManager +0x124
    std::uint32_t map_id = 0xffffffffu;
};
/// True: the talk test `IsClientPlayer()` answers 0 on this game.
bool GuestServicesGate(const GuestServicesInput& in);

/// The Hunter's Dream talk scripts a guest may use (the talk environment's +0x10 is the script id;
/// MSB m21_00_00_00 entity in brackets): 210304 Doll (2100700: level up). Not: Gehrman 210305 (the last choice), 210306, headstones
/// 210000-210003, the Dream lamp / warp menus 210720 / 210721 (travel: the lamp phase), chalice
/// altars 210050 / 210100 / 210110.
bool GuestServiceTalk(std::uint32_t talk);
/// Empty `extra` = the built-in list only; "all" = any talk script (diagnostics); else a comma
/// list of more talk script ids (BB_PARTY_GUEST_SERVICES_TALK).
bool GuestServiceTalkAllowed(std::uint32_t talk, const std::string& extra);

/// The local character's record (PlayerGameData +0x90 level, +0x94 echoes, +0x84 Insight).
struct PlayerRecord {
    int level = -1, echoes = -1, insight = -1;
    bool operator==(const PlayerRecord& o) const {
        return level == o.level && echoes == o.echoes && insight == o.insight;
    }
    bool operator!=(const PlayerRecord& o) const { return !(*this == o); }
};
/// "level 10 -> 11, echoes 5000 -> 3200" (the changed fields only; "" when equal or unknown).
std::string DescribeRecordChange(const PlayerRecord& before, const PlayerRecord& after);

/// "BB_PARTY_GUEST_SERVICES" value -> on/off (unset / "" / anything but "0" = on).
bool GuestServicesEnabledFromEnv(const char* value);

#ifndef BB_PARTY_GUESTSVC_NO_GAME
struct GameSnapshot;
/// PartyInit: installs the gated override (party mode and enabled only; byte-verified).
void GuestServicesInit();
/// Director tick (main thread): opens / closes the gate, logs the guest's record changes.
void GuestServicesTick(const GameSnapshot& s);
/// Open right now (diagnostics, the Party tab).
bool GuestServicesOpen();
#endif

} // namespace coop

// SPDX-License-Identifier: GPL-3.0-or-later
// Direct summon delivery (BB_PARTY_DIRECT_SUMMON): the guest's game joins the host's room from a
// summon message we hand to its own SummonStepManager, instead of waiting for the host's game to
// poll its bell sign and summon it (docs/party/join_timing.md "Direct summon").
//
// Ported from droogie/bbhost (GPL-3.0+), src/engine/summon_invite.{h,cpp}: the type-1 SummonData
// item built with the game's wrapper constructor and passed to the manager's handleIncomingItem.
// Here the item carries the host's real room data (the session descriptor the host's own summon
// message carries, party_b2 SerializeRoom), not bbhost's bare ConnectionArea.
//
// Wire (b2 link events): guest -> host {"op":"dsum_req"} when it rings; host -> guest
// {"op":"dsum_room","room":"<hex>","area":A,"pos":[x,y,z],"level":L} or {"op":"dsum_room","room":""}
// (refused: the bell sign path goes on); guest -> host {"op":"dsum_done","ok":0|1} (the host ends
// its hold on the guest's signs; on a failure they are offered to its game again at once).
// A11 (ERSC_PARITY.md, user decision: joiners wait outside fights and cutscenes): a refusal while
// the host fights a boss or watches a cutscene carries "wait":"boss"|"cutscene", and the host
// broadcasts {"op":"dsum_busy","wait":"boss"|"cutscene"|"none"} when that starts / ends, so the
// waiting guest shows why it waits (the host rings again right after; the sign path summons it).
// This file is game-free (unit tests: tests/test_party_dsummon.cpp); the game side is in party_b2.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace coop::dsummon {

/// BB_PARTY_DIRECT_SUMMON: unset -> `default_on`; "0" / "off" off; anything else on.
bool SwitchOn(const char* env_value, bool default_on);

struct Room {
    std::vector<std::uint8_t> data;  // head (8 bytes) + session descriptor, as the game's summon message
    std::uint32_t area = 0;          // the host's FrpgNetMan +0xa7c
    float pos[3] = {0, 0, 0};        // FrpgNetMan +0xaac
    std::uint32_t level = 0;         // FrpgNetMan +0xaa8
};

enum class Kind { None, Req, Room, Done, Busy };
Kind KindOf(const std::string& body);

/// A11: why the host holds every join now.
enum class Busy { None, Boss, Cutscene };
/// The boss bar wins over a cutscene (a boss intro cutscene ends in the fight).
Busy HostBusy(bool boss_bar_up, bool cutscene_playing);
/// "boss" / "cutscene" / "none" (the wire word).
const char* BusyWord(Busy b);
/// The player-facing wait line: the guest's ("Waiting: the host is in a boss fight") or the
/// host's own; empty for None.
std::string BusyText(Busy b, bool host);
/// The "wait" word of a dsum_room refusal or a dsum_busy event (None when absent / unknown).
Busy WaitOf(const std::string& body);

std::string EncodeReq();
std::string EncodeRoom(const Room& r);
std::string EncodeRefuse(Busy why = Busy::None);
std::string EncodeBusy(Busy b);
std::string EncodeDone(bool ok);
/// False for a refusal or a bad message (`err` says which); a refusal sets *refused.
bool DecodeRoom(const std::string& body, Room* out, bool* refused, std::string* err);
bool DecodeDone(const std::string& body, bool* ok);

constexpr std::size_t kHeadSize = 0x34;      // the type-1 payload before the room data
constexpr std::size_t kMaxRoomData = 0x400;  // a descriptor is ~200 bytes
/// The type-1 payload (the wrapper ctor 0x1486a10 copies it to buffer +4; handleIncomingItem
/// 0x14ba340 reads it): +0 the guest entry's 8-byte id, +0xc area, +0x10 pos, +0x1c level,
/// +0x28 ChannelId (none: 0x8000000000000000), +0x30 room data size, +0x34 room data.
std::vector<std::uint8_t> BuildPayload(const std::uint8_t entry_id[8], const Room& r);

/// Host: why a guest's request is refused now (nullptr: serve it).
struct HostView {
    bool world_up = false, loading = false;
    int cooperators = 0, max_cooperators = 3;
    long long ms_since_sign_summon = 1 << 30;  // the host's game asked to summon a sign this long ago
    long long other_hold_ms = 0;               // a crash hold running (C6)
    Busy busy = Busy::None;                    // A11: a boss fight / cutscene: joiners wait
};
const char* HostRefusal(const HostView& v);

/// Guest: the delivery steps around one bell.
enum class Phase { Idle, Asked, HaveRoom, Delivered, Done, Failed };
const char* PhaseName(Phase p);
struct Guest {
    Phase phase = Phase::Idle;
    double since = 0;
    int deliver_tries = 0;
};
enum class Action { None, Deliver, ReportOk, ReportFail };
constexpr double kAnswerWait = 4.0;   // no dsum_room within this: the host does not take part
constexpr double kEntryWait = 8.0;    // the bell's own entry (sign sender, ~2 s) not up within this
constexpr double kJoinWait = 8.0;     // delivered, but no session (client) within this
/// One tick. `entry_waiting`: the game has a passive guest entry not yet answered;
/// `joined`: the guest's session is a client of the host's room.
Action Step(Guest& g, double now, bool entry_waiting, bool joined);
void OnAsked(Guest& g, double now);
void OnRoom(Guest& g, double now, bool refused);
void OnDelivered(Guest& g, double now, bool ok);

}  // namespace coop::dsummon

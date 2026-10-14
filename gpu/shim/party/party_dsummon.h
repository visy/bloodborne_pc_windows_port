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
// Playtest 1 (docs/party/playtest1_triage.md #1 / #9): the host also refuses while it is still in its
// prologue ("wait":"prologue"), while the guest is in its own prologue, and while the guest stands
// in another map than the host ("wait":"map": the guest gathers to the host's lamp first, a vanilla
// loading screen, then rings again); a guest never hands the game a room of another map.
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

/// A11: why the host holds every join now. Prologue / OtherMap: playtest 1, only as the "wait"
/// word of a refusal (never broadcast as dsum_busy).
enum class Busy { None, Boss, Cutscene, Prologue, OtherMap };
/// The boss bar wins over a cutscene (a boss intro cutscene ends in the fight).
Busy HostBusy(bool boss_bar_up, bool cutscene_playing);
/// "boss" / "cutscene" / "prologue" / "map" / "none" (the wire word).
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

/// A map id (FrpgNetMan +0xa7c / the roster's map) is known: not 0, not 0xffffffff.
bool MapKnown(std::uint32_t map);
/// Two known maps share area and block (the summon load needs it, A5); unknown -> false.
bool SameMap(std::uint32_t a, std::uint32_t b);
/// The summon itself carries a joiner across maps: a Chalice Dungeon (area 29, quick join by
/// ChannelId), or the BB_PARTY_GATHER_DSUM experiment.
bool CarriesAcrossMaps(std::uint32_t host_map, bool gather_dsum_experiment);

/// Host: why a guest's request is refused now (nullptr: serve it).
struct HostView {
    bool world_up = false, loading = false;
    bool travel_pending = false;               // batch 6: the host's own stage request is armed (its load is next)
    bool host_ready = true;                    // the host finished its start (C1, party_start.h)
    bool guest_in_prologue = false;            // the guest's roster state is Prologue
    std::uint32_t host_map = 0, guest_map = 0; // 0 / 0xffffffff: not known (no map check)
    bool carry = false;                        // CarriesAcrossMaps: no map check
    int cooperators = 0, max_cooperators = 3;
    long long ms_since_sign_summon = 1 << 30;  // the host's game asked to summon a sign this long ago
    long long other_hold_ms = 0;               // a crash hold running (C6)
    Busy busy = Busy::None;                    // A11: a boss fight / cutscene: joiners wait
};
const char* HostRefusal(const HostView& v);
/// The "wait" word a refusal of `v` carries (the guest's Party page / its gather): Prologue,
/// OtherMap, Boss, Cutscene or None.
Busy RefusalWait(const HostView& v);

/// Guest: the delivery steps around one bell.
enum class Phase { Idle, Asked, HaveRoom, Delivered, Done, Failed };
const char* PhaseName(Phase p);
struct Guest {
    Phase phase = Phase::Idle;
    double since = 0;
    int deliver_tries = 0;
    const char* fail_why = "";  // why the last attempt failed (logged with "FAILED")
    bool gather = false;        // the last attempt failed for another map: gather first (OtherMap)
};
enum class Action { None, Deliver, ReportOk, ReportFail };
constexpr double kAnswerWait = 4.0;   // no dsum_room within this: the host does not take part
constexpr double kEntryWait = 8.0;    // the bell's own entry (sign sender, ~2 s) not up within this
constexpr double kJoinWait = 8.0;     // delivered, but no session (client) within this
/// One tick. `entry_waiting`: the game has a passive guest entry not yet answered;
/// `joined`: the guest's session is a client of the host's room.
Action Step(Guest& g, double now, bool entry_waiting, bool joined);
void OnAsked(Guest& g, double now);
/// A refusal's "wait" word: OtherMap sets g.gather (the director gathers, then rings again).
void OnRoom(Guest& g, double now, bool refused, Busy wait = Busy::None);
/// Guest, before delivery: the host's room (area `room_area`) is not in this guest's map
/// (`own_map`; unknown counts as another map): the game would join it without loading the host's
/// map (A5, playtest 1 #9). Fails the attempt with gather set and returns true (report a failure);
/// false when the room may be delivered (or the phase is not HaveRoom).
bool RoomElsewhere(Guest& g, double now, std::uint32_t room_area, std::uint32_t own_map, bool carry);
void OnDelivered(Guest& g, double now, bool ok);
/// A guest whose own world is in another map than the host's (A5, late joiner): gather to the
/// host's lamp first (one extra load) only when the direct summon cannot carry it: off, or its
/// last attempt failed (refused: boss fight / cutscene / full, or no answer). Used only by the
/// BB_PARTY_GATHER_DSUM experiment: live the joined guest's game did not load the host's map.
bool GatherFirst(bool direct_on, Phase last);

/// Host bell (playtest 1 #2): the host's Beckoning Bell rings for waiting members only as the
/// fallback of the direct summon, so the bell (SpEffect 9000 and its effect) does not show on the
/// host for a guest's own ring. Times in s; a large value = never.
struct HostBellView {
    bool direct_on = false;
    double waiting_s = 0;       // a ready member has waited this long (HostMayRing true)
    double since_req = 1e9;     // the last dsum_req arrived this long ago
    double since_served = 1e9;  // the host last sent its room this long ago
    double since_done = 1e9;    // the last dsum_done arrived this long ago
    bool done_ok = false;       // ... and said the guest joined
    bool refused_wait = false;  // the last request was refused with a wait word (prologue, map, ...)
    double since_refused = 1e9; // the host last refused a request this long ago
};
constexpr double kHostBellGrace = 15.0;  // no request yet: the guest's own ring comes first
constexpr double kHostBellHold = 20.0;   // a served room / a join: no bell for this long
/// True: the host does not ring now.
bool HostBellHeld(const HostBellView& v);

/// Host lamp travel (playtest 1 batch 6, H1:3630): a host that warps while a member it just
/// served a room is still loading in leaves that guest to arrive in an empty map (and the join
/// stalls). The host's lamp warp waits until the joiner is in its world, the guest reports a
/// failure, or kHostTravelHoldMax after the room was sent - whichever comes first.
struct HostTravelHoldView {
    bool direct_on = false;
    double since_served = 1e9;  // the host last sent its room this long ago
    double since_done = 1e9;    // the last dsum_done arrived this long ago
    bool done_ok = false;       // ... and said the guest joined
    bool joiner_in_world = false;  // every joining member's chr is in the host's world
};
constexpr double kHostTravelHoldMax = 30.0;
/// True: the host's lamp travel waits now.
bool HostTravelHeld(const HostTravelHoldView& v);

}  // namespace coop::dsummon

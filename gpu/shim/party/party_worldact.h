// SPDX-License-Identifier: GPL-3.0-or-later
// D3 / D8: a guest's actions change the host's world (docs/party/world_actions.md).
//
// A summoned guest plays in the host's world: its game reads and writes the host-world flag
// overlay (load mode 1, event_flags.md 3), which the host-world mirror keeps equal to the host's
// world (party_progress "world mirror"). What the guest's own game does there - a lever, a door
// (also a one-way door opened from the other side), a shortcut, an elevator, a ladder kicked
// down, a bridge, an illusory wall, an NPC talk - sets flags in that overlay only. Those flags are
// the guest's "local changes": overlay bits that differ from what the host's image last put there
// (GuestWorldMirror::LocalChanges).
//
// Guest (main thread, load mode 1, steady world): every 0.25 s the local changes of the allowed
// categories that are NEW since the guest's world became steady (a baseline taken 3 s after each
// load / full mirror copy holds back what the guest's own game set while loading the map - client
// branches of events end early and write their completion flags) go to the host as
// EVENT "world_action" {v, map, set:[ids], clr:[ids]}; the same set bits go into the guest's own
// save (the pool), so its own world keeps the progress.
// Host (main thread, own world): validates every id again (WorldActionAllowed: mirror group and
// mask, category whitelist, the host's area, rate limit) and sets / clears it with SetEventFlag;
// the mirror carries the result back to every guest, C2 into every guest's save.
//
// Categories: shortcut_door, world_state, key_event (set; shortcut_door / world_state also clear:
// elevators toggle), npc_quest (set and clear: quest steps move state flags; the user's decision:
// NPC quest choices are shared). Never:
// boss_defeated, boss_area, lamp_unlocked (EVENT lamp_lit), cutscene_seen, item_lot_picked
// (instanced per player, D4), session_runtime, time_of_day, never_sync.
//
// The pure part is unit-tested (tests/test_party_worldact.cpp) and fuzzed (tests/party_fuzz.cpp).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace coop {
struct GameSnapshot;
}
namespace party {
class PartyLink;
}

namespace coop::worldact {

constexpr const char* kEventName = "world_action";
constexpr std::size_t kMaxSet = 32, kMaxClear = 16;
/// Host: flags one guest may change per 10 s window.
constexpr int kRateFlags = 96;
constexpr double kRateWindow = 10.0;

/// May a guest set (clear = false) or clear (clear = true) flag `id` in the host's world, the host
/// standing in map `host_map` (0xAABBCCDD; areas must match for map flags)?
bool WorldActionAllowed(std::uint32_t id, bool clear, std::uint32_t host_map);

/// An NPC gift trigger: the flag a talk script sets to hand over its gift, which common event 9040
/// (slots 2-37) waits for to award the gift's item lot (9042, 9045-9052, 9054-9064, 9066, 9067, 9069,
/// 9071-9077; quest_audit.md P1). 9040 ends at once on a client, so a guest's talk awards nothing
/// in its own game: the guest sends the trigger to the host (set only), whose 9040 awards the lot
/// and C3 hands it to every member. 9043 is a repeatable 9100 slot, not one of these.
bool NpcGiftTrigger(std::uint32_t id);

constexpr std::size_t kMaxKicks = 4, kMaxFog = 2;
struct Request {
    std::uint32_t map = 0;  ///< the guest's map when it sent this (logged; not trusted)
    std::vector<std::uint32_t> set, clr;
    /// ObjAct kick event ids the guest's player kicked (ObjAct entry +0xC): the host replays the
    /// kick on its own object so it animates there too (when its game did not get it itself).
    std::vector<std::int32_t> kick;
    /// B2: guest-entered flags of boss fogs (BossGate::guest_flag) the guest just went through:
    /// the host opens that fight (BossGateHostOpens). Never written into anyone's save.
    std::vector<std::uint32_t> fog;
    bool operator==(const Request& o) const {
        return map == o.map && set == o.set && clr == o.clr && kick == o.kick && fog == o.fog;
    }
};
std::string RequestToJson(const Request& r);
/// Parses and validates the shape (sizes, duplicates, u32 ids, each id allowed in some map:
/// WorldActionAllowed with the id's own area). The host re-checks against its own map.
bool RequestFromJson(const std::string& text, Request* out, std::string* error);

/// Host: a sliding count of flags per guest slot.
class RateLimit {
public:
    /// True (and counted) when `n` more flags from `slot` at time `now` stay within the limit.
    bool Take(int slot, int n, double now);

private:
    struct Slot {
        double start = -1e18;
        int used = 0;
    };
    Slot slots_[8];
};

/// Talk scripts a guest may run in the host's world (D8): every world NPC's script - not the
/// Hunter's Dream's own (guest services decide those), not lamps / headstones (lamp requests),
/// not Gehrman's ending choice.
bool WorldTalkAllowed(std::uint32_t talk);

/// Catch-up (user decision: a guest behind the host gets every boss into its own save): the area
/// gate flags 2AAx / 3AAx a boss's defeat event writes next to its boss flag (EMEVD data,
/// docs/party/world_actions.md 7). C2 never syncs them (they are never_sync: 2410-style flags
/// block summons in the host's world); the guest derives them in its own pool from its own boss
/// flags. `pairs` = {boss flag, gate flag}; 2410 needs both 12411800 and 12411700.
struct GateRule {
    std::uint32_t boss, gate, also = 0;  ///< also: a second boss flag the gate needs (0: none)
};
const std::vector<GateRule>& GateRules();

/// B2 (ERSC parity): any party member may walk through a boss fog first. Vanilla lets a guest
/// through only after the host ("host entered" flag ON, condition 3[0] of each guest-entry event
/// `ゲストがボス部屋入場_*`). From every map EMEVD: the guest-entry event, the host-entered flag the
/// boss's start event waits on, the guest-entered flag the event sets (network-synced: the host's
/// game receives it), the boss-defeated flag. Amygdala (13204831) is left out: its guest flag is
/// an event argument.
struct BossGate {
    std::uint32_t guest_event, host_flag, guest_flag, boss_flag;
    /// The guest-entry event's other ON condition: the host's first-fight entry (usually an intro
    /// cutscene) has run. It stays a condition, so a FIRST fight still starts with the host; 0 none.
    std::uint32_t intro_flag = 0;
    std::int32_t fog_entity = 0;  ///< the fog's map object (the event's action-button target)
};
const std::vector<BossGate>& BossGates();
const BossGate* BossGateByEvent(std::int32_t guest_event);
const BossGate* BossGateByGuestFlag(std::uint32_t guest_flag);
/// Guest: skip condition instruction bank:id (3[0] IfEventFlag on `flag`) of event `event_id`?
/// Only the host-entered condition of a guest-entry event; the boss-alive and intro-seen checks stay.
bool BossGateSkipCondition(std::int32_t event_id, int bank, int id, std::uint32_t flag);
/// Host: open the boss fight for a guest who went through the fog first (set host_flag)? The boss
/// lives, a guest is in, the host's own flag is not yet set and the host stands in the boss's area
/// block (map 0xAABBCCDD).
bool BossGateHostOpens(const BossGate& g, bool guest_in, bool host_in, bool boss_dead, std::uint32_t host_map);
/// Guest at a boss fog (playtest 1 bug 8): the fog has no prompt because the host has not had its
/// first fight here yet (intro_flag off). The guest is told to wait for the host.
bool BossGateWaitsForHost(const BossGate& g, bool intro_done, bool host_in, bool boss_dead);
/// The map 0xAABBCCDD is the gate's area block.
bool BossGateInBlock(const BossGate& g, std::uint32_t map);
/// The guest's notice at such a fog.
constexpr const char* kBossFogWaitNotice = "Boss fog: the host goes in first (first fight here)";
/// Within this many metres of the fog object the notice shows.
constexpr float kBossFogNoticeRange = 5.0f;
/// The notice is due: the gate's flags were read, the gate waits for the host and the guest
/// stands within kBossFogNoticeRange of the fog (dist_m < 0: unknown, no notice).
bool BossFogNoticeDue(const BossGate& g, bool read, bool intro_done, bool host_in, bool boss_dead, float dist_m);
/// The local player's position for distance checks (playtest 1 bug 8, live 2026-10-14): the
/// physics position (ChrIns +0x58 -> +8 -> +0x3B0 -> +0x68: +0x1E0) when readable, else the chr's
/// +0xC0. On a guest in the host's world +0xC0 stays at its arrival spot (it read 19 m from the
/// fog while the guest stood 3 m from it), so the fog notice never fired. Null: neither readable.
const float* PickPlayerPos(bool physics_ok, const float* physics, bool drawn_ok, const float* drawn);

// ---- game side ----
void Init();
bool Enabled();
/// Host: EVENT world_action from `slot` (link thread): parsed, queued for the tick.
bool OnHostEvent(int slot, const std::string& body, std::string* why);
/// Main thread, once a frame (director tick).
void Tick(const GameSnapshot& s, party::PartyLink* link);
/// Test fixture: stand the local chr `dist` m from map object `entity`, on the side `angle_deg`
/// from its facing (0 = in front), facing it. False when the object is not loaded.
bool StandAtObject(std::int32_t entity, float dist, float angle_deg, std::string* what);
/// The point StandAtObject would use (DIST m from map object ENTITY at ANGLE degrees from its
/// front), without moving anyone; false when the object is not loaded.
bool ObjectPoint(std::int32_t entity, float dist, float angle_deg, float out[3]);
/// Test fixture: the same for a character (NPC) entity.
bool StandAtChr(std::int32_t entity, float dist, float angle_deg, std::string* what);
/// Counters for logs / tests.
struct Stats {
    std::uint64_t sent = 0, sent_flags = 0, received = 0, applied = 0, refused = 0, own_save = 0;
};
Stats GetStats();

} // namespace coop::worldact

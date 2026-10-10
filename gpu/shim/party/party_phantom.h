// SPDX-License-Identifier: GPL-3.0-or-later
// Party phantoms as full players (docs/party/phantom_limits.md): what a guest gets besides
// following the host (party_travel.h).
//
//   1. Death / Hunter's Mark: a guest that dies (PartyGhostDeath_2) or uses a Hunter's Mark goes
//      to the host's last lamp in its own world instead of its pre-summon spot (the redirect is in
//      party_travel's funnel hook; this file keeps the host's lamp id current there), then the
//      director's Small Resonant Bell brings it back (auto-rejoin).
//      host -> guests: EVENT "phantom" {kind "lamp"} whenever the host's last lamp
//      (WorldTransitionState +0x1528) changes or the party's membership changes.
//   2. Vials / bullets: the guest runs the game's own auto-replenish (0x14DBB70 Blood Bullets
//      reset, 0x14DACE0 vials and bullets from storage, the pair MoveMapStep 0x19398D9 runs) on
//      the main thread, world up and no load pending, after the host rested (EVENT "phantom"
//      {kind "rested"}: the host warped into the Hunter's Dream from a lamp, died, or used a
//      Hunter's Mark: the loads that refill the host) and after its own redirected respawn.
//   3. Confinement walls: the EMEVD skip rules "7600:2005:3@6" and "7600:2006:2@7" (common event
//      7600 マルチ閉じ込め壁: skip only "wall on" and "wall SFX on"), active only while B1 travel is
//      on. The rule syntax "event:bank:id[@index]" is parsed here (seamless_rules uses it).
//   4. Insight: a white phantom already gets +1 Insight natively on a boss kill (BlockClear2
//      0x13849A0, chr type 1 branch, SpEffect 4680 at 0x13852B1); the host's own boss Insight
//      (common event 9350 "SAN値獲得", N x SpEffect 4680, `End if Client` at instruction 0) never
//      runs on a guest. The host's EMEVD filter sees a boss-defeat instruction (2003[12]/[15]/[53])
//      and then `2000[0] InitializeEvent(slot, 9350, N)` in the same event, and sends EVENT
//      "phantom" {kind "insight", n}; the guest applies SpEffect 4680 (PlayerIns vfunc +0x3F0,
//      BlockClear2's own call) one at a time, 15 frames apart.
//   5. Hunter's Mark for guests: the EquipParamGoods enable_white / enable_multi bits of goods 100
//      and 1400 (seamless_rules param rules; off by default).
//
// Env (party mode): BB_PARTY_GUEST_RESPAWN=0 (guests go home on death, vanilla),
//   BB_PARTY_GUEST_REFILL=0 (no vial refill), BB_PARTY_OPEN_WORLD=0 (confinement walls stay),
//   BB_PARTY_GUEST_INSIGHT=parity|full|0 (default parity: the guest ends with the host's N for a
//   boss kill: N - 1 on top of the native +1; full: N on top), BB_PARTY_GUEST_MARK=1 (Hunter's
//   Mark usable by guests), BB_PARTY_PHANTOM_PROBE=insight,refill (offline probe: once, 5 s after
//   the world is up: one SpEffect 4680 / one refill, logging Insight / vial counts around it).
//
// The pure part has no game dependency and is unit-tested (tests/test_party_phantom.cpp, built
// with BB_PARTY_PHANTOM_NO_GAME and BB_PARTY_TRAVEL_NO_GAME).
#pragma once

#include "party_travel.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace party {
class PartyLink;
}

namespace coop {

// ---- EMEVD skip rules: "event:bank:id[@index]" ("*" = any event), comma separated ----
struct EmevdSkipRule {
    std::int32_t event = -1; ///< -1: any event
    std::int32_t bank = 0, id = 0;
    std::int32_t index = -1; ///< instruction index in the event (+0xA0); -1: any
    bool needs_travel = false; ///< only while B1 travel is on (built-in open-world rules)
};
/// Parses `text`; good items go to *out, each bad item adds a message to *errors (when given).
/// Returns the number of rules added.
std::size_t ParseEmevdSkipRules(const std::string& text, std::vector<EmevdSkipRule>* out,
                                std::vector<std::string>* errors = nullptr);
bool EmevdSkipMatches(const EmevdSkipRule& r, std::int32_t event, std::int32_t bank, std::int32_t id,
                      std::int32_t index);
std::string DescribeSkipRule(const EmevdSkipRule& r);
/// Common event 7600 instructions 6 (2005[3] object on) and 7 (2006[2] SFX on), needs_travel.
std::vector<EmevdSkipRule> ConfinementWallRules();

// ---- EVENT "phantom" (host -> guests) ----
constexpr const char* kPhantomEventName = "phantom";
enum class PhantomEventKind : std::uint8_t { Unknown = 0, Lamp, Rested, Insight };
const char* PhantomEventKindName(PhantomEventKind k);
struct PhantomEvent {
    PhantomEventKind kind = PhantomEventKind::Unknown;
    std::uint64_t seq = 0;        ///< host counter (de-duplication of rested / insight)
    std::uint32_t lamp = kTravelNone; ///< Lamp: the host's last lamp id (+0x1528 low dword)
    std::int32_t insight = 0;     ///< Insight: N of the host's 9350 start
    std::uint32_t source_event = 0; ///< Insight: the host's EMEVD event (diagnostics)
};
std::string PhantomEventToJsonText(const PhantomEvent& e);
bool PhantomEventFromJsonText(const std::string& text, PhantomEvent* out, std::string* error = nullptr);
/// A host's phantom event before it reaches the game (bbport security pass): false (with `why`)
/// for a Lamp whose id is no ReturnPointParam row (TravelLampKnown; the guest's death redirect
/// warps there through 0x13CDF30).
bool SanitizePeerPhantom(const PhantomEvent& e, std::string* why = nullptr);

/// A host travel that refills the host (and so the guests): lamp into the Hunter's Dream
/// (respawn mode 2), host death, Hunter's Mark.
bool RestedFromTravel(const TravelIntent& t);

// ---- Insight ----
enum class InsightMode : std::uint8_t { Off, Parity, Full };
InsightMode InsightModeFromString(const char* s); ///< nullptr / "" -> Parity
const char* InsightModeName(InsightMode m);
/// SpEffect 4680 applications for the guest when the host's boss kill granted `host_n`.
int GuestInsightGrant(int host_n, InsightMode mode);

/// Host: pairs a boss-defeat instruction with the 9350 start of the same event.
class BossInsightTracker {
public:
    static constexpr double kWindow = 120.0; ///< s between the defeat marker and the 9350 start
    static constexpr std::int32_t kInsightEvent = 9350;
    /// 2003[12] / [15] / [53] ran in `event`.
    void OnBossDefeat(std::int32_t event, double now);
    /// `2000[0]` started `target` with first argument `n` from `event`: N when this is a boss
    /// kill's Insight (once per defeat), else nothing.
    std::optional<int> OnInitializeEvent(std::int32_t event, std::int32_t target, std::int32_t n, double now);

private:
    std::unordered_map<std::int32_t, double> defeats_;
};

/// Guest: spaces SpEffect applications (the game's 9350 waits 10 frames between them).
class InsightDripper {
public:
    static constexpr int kInterval = 15; ///< ticks between applications
    static constexpr int kMaxPending = 99;
    void Add(int n);
    /// Once a frame; true = apply one now (then counted as done).
    bool Step(bool allowed);
    int Pending() const { return pending_; }

private:
    int pending_ = 0;
    int wait_ = 0;
};

// ---- Vial refill ----
enum class RefillReason : std::uint8_t { HostRested, GuestRespawn, Probe };
const char* RefillReasonName(RefillReason r);
class RefillScheduler {
public:
    static constexpr int kStableTicks = 30;     ///< world steady this many frames first
    static constexpr double kLoadWait = 45.0;   ///< s to wait for the expected load to start
    static constexpr double kTimeout = 240.0;   ///< s: give up
    /// `expect_load`: a warp is under way; refill after it, not before.
    void Request(RefillReason why, bool expect_load, double now);
    /// Once a frame with the game state; true = refill now (the request is then done).
    bool Step(const ReplayState& s, double now);
    bool Pending() const { return pending_; }
    RefillReason Reason() const { return why_; }

private:
    bool pending_ = false;
    bool expect_load_ = false, load_seen_ = false;
    RefillReason why_ = RefillReason::HostRested;
    double since_ = 0;
    int stable_ = 0;
};

/// Host: when to (re)send the last lamp: it changed, or the party's members changed.
class LampAnnouncer {
public:
    bool ShouldSend(std::uint32_t lamp, std::uint64_t members_key);

private:
    bool sent_ = false;
    std::uint32_t lamp_ = kTravelNone;
    std::uint64_t members_ = 0;
};

#ifndef BB_PARTY_PHANTOM_NO_GAME
/// PartyInit, after InstallTravelPatches: reads the env, connects the travel redirect.
void PhantomInit();
/// The EMEVD filter wants the boss-Insight observer (insight mode on, party mode).
bool PhantomWantsEmevd();
/// Built-in EMEVD skip rules (the confinement walls unless BB_PARTY_OPEN_WORLD=0).
std::vector<EmevdSkipRule> PhantomSkipRules();
/// From the EMEVD filter (game thread), for 2003[12/15/53] and 2000[0] only.
void PhantomEmevdInstruction(std::int32_t event, std::int32_t bank, std::int32_t id, const std::uint8_t* args,
                             std::size_t size);
/// Director (host), for every travel it sends.
void PhantomNoteHostTravel(const TravelIntent& t);
/// Director tick (main thread): host announcements over `link`, guest refill / Insight.
void PhantomTick(party::PartyLink* link);
/// Guest: EVENT "phantom" (PartyLink callback thread).
void PhantomOnEvent(const std::string& body);
#endif

} // namespace coop

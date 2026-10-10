// SPDX-License-Identifier: GPL-3.0-or-later
// Party status bridge: feeds the status board (party_status.h, the overlay's Party tab) from the
// party code and carries the tab's commands back.
//
//   runtime (any thread):  OnRuntimeStart, OnCode, OnRuntimeError, OnLinkState, OnRoster,
//                          OnMemberJoined / OnMemberLeft, OnTravel (guest: the host's warp)
//   director (main thread): Tick once a frame (cheap: compares a few fields, publishes only on a
//                          change, the roster every 3 s; pops the tab's commands), OnTravel (host),
//                          OnRing
// Commands: Leave -> party::runtime_leave() (BYE; a guest stops reconnecting), Rejoin -> reconnect
// now / a fresh link after Leave or a rejection / the next bell at once, Kick (host) ->
// PartyLink::kick_name (REJECT "kicked by the host", that name is refused for the session).
// BB_PARTY_STATUS_LOG=1: one "Party: status ..." line every 10 s and on each state change.
// BB_PARTY_STATUS_TEST=host:kick:Hunter1@40,guest:leave@60,rejoin@80: presses the tab's buttons
// that many s after the first tick ("host:" / "guest:": that role only; tests without keystrokes).
//
// The pure part (MapState, MapMembers, RejectText, SnapshotLine) is unit-tested by
// tests/test_party_status_bridge.cpp (built with BB_PARTY_BRIDGE_NO_GAME).
#pragma once

#include "party_link.h"
#include "party_status.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace party::bridge {

/// Everything the board's state is computed from.
struct Facts {
    status::Role role = status::Role::Off;
    bool have_link = false;          // the runtime made its PartyLink
    LinkState link = LinkState::Idle;
    std::string link_detail;         // the link's last state detail (reconnect reason ...)
    RejectCode reject = RejectCode::None;
    std::string reject_reason;
    std::string error;               // runtime failure before / without a link
    std::string host_address;        // guest: "ip:port"
    bool left = false;               // Leave pressed (until Rejoin)
    int members_connected = 0;       // roster entries connected, the host included
    int max_players = 0;
    // The game (director tick).
    bool world_up = false;
    bool loading = false;
    int session_role = -1;           // coop::SessionRole
    int cooperators = -1;
    bool travelling = false;
    bool bell_recent = false;        // our bell went up recently
};

struct Mapped {
    status::State state = status::State::Off;
    std::string detail;
    bool operator==(const Mapped& o) const { return state == o.state && detail == o.detail; }
};

Mapped MapState(const Facts& f);
/// The roster as the tab shows it; `area` names a map id (empty function / 0: no area).
std::vector<status::Member> MapMembers(const std::vector<RosterEntry>& roster, const std::string& local_name,
                                       bool host, const std::function<std::string(std::uint32_t)>& area = {});
/// "kicked by the host", "the host ended the party", the host's mismatch text ...
std::string RejectText(RejectCode code, const std::string& reason);
/// One line for BB_PARTY_STATUS_LOG.
std::string SnapshotLine(const status::Board& b);

// ---- producers (game build) ----
void OnRuntimeStart(bool host, const std::string& name);
void OnCode(const std::string& code);
void OnRuntimeError(const std::string& error);
void OnHostAddress(const std::string& address);
void OnLinkState(LinkState state, const std::string& detail, RejectCode reject, const std::string& reason);
void OnRoster(const std::vector<RosterEntry>& roster, int max_players);
void OnMemberJoined(const std::string& name, bool rejoined);
void OnMemberLeft(const std::string& name, bool slot_kept);
void OnTravel(const std::string& what);
void OnRing();
/// Leave / Rejoin done by the runtime (any thread).
void OnLeft(bool host);
void OnRejoining();

struct TickIn {
    bool host = false;
    PartyLink* link = nullptr;
    bool world_up = false;
    bool loading = false;
    int session_role = -1;
    int cooperators = -1;
    int max_players = 3;
};
struct TickOut {
    bool ring_now = false;  // Rejoin while connected: the director's next bell goes up at once
};
/// The director, once a frame on the main thread.
TickOut Tick(const TickIn& in);

/// What the tab's commands do (the game build: party_runtime / PartyLink).
struct Actions {
    std::function<void()> leave;
    std::function<int()> rejoin;  // 0 link up, 1 reconnecting now, 2 a fresh link, -1 nothing to do
    std::function<bool(const std::string&)> kick;
};

/// Tests: the bridge's own state back to the start (no actions until SetActionsForTest).
void ResetForTest();
void SetActionsForTest(Actions actions);

}  // namespace party::bridge

// SPDX-License-Identifier: GPL-3.0-or-later
// Party status bridge (gpu/shim/party/party_status_bridge.h): link / game facts -> board state,
// roster -> members, events, and the Party tab's commands -> actions.
// ninja -C out/gpu party-status-bridge-test && out/gpu/party-status-bridge-test
#include "party/party_status_bridge.h"

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

using namespace party;
using bridge::Facts;
using bridge::MapState;
using status::Role;
using status::State;

static int g_checks = 0;
#define CHECK(x)                                                                    \
    do {                                                                            \
        ++g_checks;                                                                 \
        if (!(x)) {                                                                 \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #x);              \
            return 1;                                                               \
        }                                                                           \
    } while (0)

static Facts up(Role role) {
    Facts f;
    f.role = role;
    f.have_link = true;
    f.link = role == Role::Host ? LinkState::Hosting : LinkState::Connected;
    f.members_connected = 2;
    f.max_players = 3;
    f.world_up = true;
    f.session_role = 0;
    f.cooperators = 0;
    return f;
}

static int test_map_state() {
    Facts f;
    CHECK(MapState(f).state == State::Off);
    f.role = Role::Guest;
    CHECK(MapState(f).state == State::Starting);
    f.error = "no party code: set BB_PARTY_CODE";
    CHECK(MapState(f).state == State::Error && MapState(f).detail == f.error);
    f.error.clear();
    f.have_link = true;
    f.link = LinkState::Connecting;
    f.host_address = "127.0.0.1:47600";
    CHECK(MapState(f).state == State::Connecting && MapState(f).detail == "127.0.0.1:47600");
    f.link = LinkState::Reconnecting;
    f.link_detail = "timeout; retrying in 2000 ms";
    CHECK(MapState(f).state == State::Reconnecting && MapState(f).detail == f.link_detail);
    f.link = LinkState::Rejected;
    f.reject = RejectCode::Kicked;
    f.reject_reason = "whatever";
    CHECK(MapState(f).state == State::Error && MapState(f).detail == "kicked by the host");
    f.reject = RejectCode::Mismatch;
    f.reject_reason = "eboot.bin differs";
    CHECK(MapState(f).detail == "host refused: eboot.bin differs");
    f.reject = RejectCode::Shutdown;
    f.reject_reason.clear();
    CHECK(MapState(f).state == State::Off && MapState(f).detail == "the host ended the party");
    f.link = LinkState::Stopped;
    CHECK(MapState(f).state == State::Off);
    f.left = true;
    CHECK(MapState(f).state == State::Off && MapState(f).detail == "you left the party");

    // Guest, link up: the game decides.
    f = up(Role::Guest);
    f.world_up = false;
    CHECK(MapState(f).state == State::WaitingForWorld && MapState(f).detail == "connected; title / menu");
    f.world_up = true;
    f.loading = true;
    CHECK(MapState(f).state == State::WaitingForWorld && MapState(f).detail == "connected; loading");
    f.loading = false;
    CHECK(MapState(f).state == State::WaitingForWorld);
    f.bell_recent = true;
    CHECK(MapState(f).state == State::RingingBell);
    f.bell_recent = false;
    f.session_role = 4;  // trying to join
    CHECK(MapState(f).state == State::RingingBell);
    f.session_role = 6;  // client
    CHECK(MapState(f).state == State::Joined && MapState(f).detail == "in the host's world");
    f.travelling = true;
    CHECK(MapState(f).state == State::Travelling);

    // Host.
    f = up(Role::Host);
    CHECK(MapState(f).state == State::Hosting && MapState(f).detail == "2/3");
    f.world_up = false;
    CHECK(MapState(f).state == State::Hosting && MapState(f).detail == "2/3, title / menu");
    f.world_up = true;
    f.session_role = 1;
    CHECK(MapState(f).state == State::RingingBell && MapState(f).detail == "2/3");
    f.session_role = 3;
    f.cooperators = 1;
    CHECK(MapState(f).state == State::Joined && MapState(f).detail == "2 in your world, 2/3 in the party");
    f.left = true;
    CHECK(MapState(f).state == State::Off && MapState(f).detail == "you ended the party");
    f = up(Role::Host);
    f.link = LinkState::Idle;
    CHECK(MapState(f).state == State::Starting);
    f.error = "cannot host: port taken";
    CHECK(MapState(f).state == State::Error);
    return 0;
}

static int test_members() {
    std::vector<RosterEntry> roster(3);
    roster[0] = {0, "Hunter0", MemberState::Home, true, 0x18010000u, 0};
    roster[1] = {1, "Hunter1", MemberState::InHostWorld, true, 0x18010000u, 34};
    roster[2] = {2, "Hunter2", MemberState::Loading, false, 0, 99};
    auto m = bridge::MapMembers(roster, "Hunter0", true);
    CHECK(m.size() == 3);
    CHECK(m[0].local && m[0].in_world && m[0].ping_ms == -1 && m[0].slot == 0 && m[0].area == "m24_01_00_00");
    CHECK(!m[1].local && m[1].in_world && m[1].connected && m[1].ping_ms == 34);
    CHECK(!m[2].connected && !m[2].in_world && m[2].ping_ms == -1 && m[2].area.empty());
    m = bridge::MapMembers(roster, "Hunter1", false, [](std::uint32_t id) { return id ? "Cathedral Ward" : ""; });
    CHECK(m[1].local && m[1].ping_ms == -1 && m[1].area == "Cathedral Ward" && !m[0].local);
    return 0;
}

// The producers through the board, and the commands through the actions.
static int test_board_and_commands() {
    status::ResetForTest(true);
    bridge::ResetForTest();
    int leaves = 0, rejoins = 0;
    std::vector<std::string> kicked;
    int rejoin_result = 0;
    bridge::SetActionsForTest({[&] { ++leaves; }, [&] {
                                   ++rejoins;
                                   return rejoin_result;
                               },
                               [&](const std::string& n) {
                                   kicked.push_back(n);
                                   return n == "Hunter1";
                               }});
    bridge::OnRuntimeStart(true, "Hunter0");
    status::Board b = status::Snapshot();
    CHECK(b.role == Role::Host && b.state == State::Starting);
    bridge::OnCode("BBP1-TEST");
    bridge::OnLinkState(LinkState::Hosting, "", RejectCode::None, "");
    std::vector<RosterEntry> roster(2);
    roster[0] = {0, "Hunter0", MemberState::Home, true, 0x18010000u, 0};
    roster[1] = {1, "Hunter1", MemberState::Home, true, 0x18010000u, 12};
    bridge::OnRoster(roster, 3);
    bridge::OnMemberJoined("Hunter1", false);
    bridge::TickIn in;
    in.host = true;
    in.world_up = true;
    in.session_role = 0;
    in.cooperators = 0;
    bridge::TickOut out = bridge::Tick(in);
    CHECK(!out.ring_now);
    b = status::Snapshot();
    CHECK(b.code == "BBP1-TEST" && b.state == State::Hosting && b.detail == "2/3");
    CHECK(b.members.size() == 2 && b.members[0].local && b.members[1].name == "Hunter1");
    CHECK(b.last_event == "Hunter1 joined");
    std::printf("  %s\n", bridge::SnapshotLine(b).c_str());

    // Unchanged facts: no new version.
    const auto v = status::Snapshot().version;
    bridge::Tick(in);
    CHECK(status::Snapshot().version == v);

    // Bell, then the guest is in.
    bridge::OnRing();
    bridge::Tick(in);
    CHECK(status::Snapshot().state == State::RingingBell);
    in.session_role = 3;
    in.cooperators = 1;
    bridge::Tick(in);
    CHECK(status::Snapshot().state == State::Joined);

    // Travel: until a loading screen came and went.
    bridge::OnTravel("lamp");
    CHECK(status::Snapshot().state == State::Travelling && status::Snapshot().last_event == "Travelling: lamp");
    in.loading = true;
    bridge::Tick(in);
    CHECK(status::Snapshot().state == State::Travelling);
    in.loading = false;
    bridge::Tick(in);
    CHECK(status::Snapshot().state == State::Joined);

    // Commands: Kick (host), Rejoin while up -> ring now, Leave.
    CHECK(status::KickMember("Hunter1"));
    out = bridge::Tick(in);
    CHECK(kicked.size() == 1 && kicked[0] == "Hunter1" && status::Snapshot().last_event == "Kicked Hunter1");
    status::RequestRejoin();
    out = bridge::Tick(in);
    CHECK(rejoins == 1 && out.ring_now);
    rejoin_result = 2;
    status::RequestRejoin();
    out = bridge::Tick(in);
    CHECK(rejoins == 2 && !out.ring_now);
    status::RequestLeave();
    bridge::Tick(in);
    CHECK(leaves == 1);
    bridge::OnLeft(true);
    bridge::OnLinkState(LinkState::Stopped, "", RejectCode::None, "");
    b = status::Snapshot();
    CHECK(b.state == State::Off && b.detail == "you ended the party" && b.last_event == "You ended the party");
    bridge::OnRejoining();
    bridge::OnLinkState(LinkState::Hosting, "", RejectCode::None, "");
    CHECK(status::Snapshot().state == State::Joined);  // the game facts still say 1 cooperator

    // Guest side: kicked, then reconnecting / connected events; Kick is host-only.
    status::ResetForTest(true);
    bridge::ResetForTest();
    kicked.clear();
    bridge::SetActionsForTest({[&] { ++leaves; }, [&] { return -1; }, [&](const std::string& n) {
                                   kicked.push_back(n);
                                   return true;
                               }});
    bridge::OnRuntimeStart(false, "Hunter1");
    bridge::OnHostAddress("127.0.0.1:47600");
    bridge::OnLinkState(LinkState::Connecting, "", RejectCode::None, "");
    CHECK(status::Snapshot().state == State::Connecting && status::Snapshot().detail == "127.0.0.1:47600");
    bridge::OnLinkState(LinkState::Connected, "", RejectCode::None, "");
    CHECK(status::Snapshot().last_event == "Joined the party");
    bridge::OnLinkState(LinkState::Reconnecting, "lost", RejectCode::None, "");
    CHECK(status::Snapshot().state == State::Reconnecting && status::Snapshot().last_event == "Connection to the host lost");
    bridge::OnLinkState(LinkState::Connected, "", RejectCode::None, "");
    CHECK(status::Snapshot().last_event == "Rejoined the party");
    bridge::OnLinkState(LinkState::Rejected, "kicked by the host", RejectCode::Kicked, "kicked by the host");
    CHECK(status::Snapshot().state == State::Error && status::Snapshot().detail == "kicked by the host");
    CHECK(status::Snapshot().last_event == "kicked by the host");
    bridge::TickIn gin;
    gin.host = false;
    bridge::Tick(gin);
    CHECK(status::Snapshot().state == State::Error);
    return 0;
}

// C1: a player still in its prologue (party_start.h) shows that instead of "waiting for the bell".
static int test_prologue() {
    using party::LinkState;
    using party::status::Role;
    using party::status::State;
    Facts f;
    f.role = Role::Guest;
    f.have_link = true;
    f.link = LinkState::Connected;
    f.world_up = true;
    f.session_role = 0;
    f.prologue = true;
    f.prologue_step = "clinic";
    CHECK(MapState(f).state == State::WaitingForWorld && MapState(f).detail == "prologue (clinic), solo until ready");
    f.prologue = false;
    CHECK(MapState(f).detail == "connected; waiting for the host's bell");
    f.role = Role::Host;
    f.link = LinkState::Hosting;
    f.members_connected = 2;
    f.max_players = 3;
    f.prologue = true;
    f.prologue_step = "first-dream";
    CHECK(MapState(f).state == State::Hosting && MapState(f).detail == "2/3, prologue (first-dream), solo until ready");
    f.session_role = 6;  // already summoned: the session wins
    f.role = Role::Guest;
    f.link = LinkState::Connected;
    CHECK(MapState(f).state == State::Joined);
    party::RosterEntry e;
    e.name = "Hunter1";
    e.slot = 1;
    e.connected = true;
    e.state = party::MemberState::Prologue;
    const auto m = bridge::MapMembers({e}, "Hunter0", true, [](std::uint32_t) { return std::string("m24_01"); });
    CHECK(m.size() == 1 && m[0].area == "m24_01 (prologue)" && !m[0].in_world);
    return 0;
}

int main() {
    if (test_map_state() || test_members() || test_board_and_commands() || test_prologue()) return 1;
    std::printf("%d checks OK\n", g_checks);
    return 0;
}

// SPDX-License-Identifier: GPL-3.0-or-later
// The party runtime: brings the party pieces up inside the game process (BB_PARTY=host|join).
//
//   host:  PartyLink listening on BB_PARTY_PORT (TCP; the game's P2P UDP socket uses the same
//          number), UPnP (BB_PARTY_UPNP, <= 3 s), the public address (BB_PARTY_PUBLIC_ADDR, STUN
//          sent from the game's own party UDP port, the UPnP router's address), the Internet and
//          LAN party codes (logged, written to <user>/party_code.txt and BB_PARTY_CODE_FILE),
//          guests' RPCs into the host service (party_transport.h host_handle) and one event pump
//          per guest (PartyHostService queue -> PartyLink EVENT).
//   guest: the party code (BB_PARTY_CODE, BB_PARTY=<code>, or BB_PARTY_CODE_FILE, waiting up to
//          60 s for it), PartyLink connecting with backoff; once connected the RemoteGuest
//          transport's RPCs go over the link, its events come from it, and the session layer's
//          STUN goes to the host's party port.
// Startup runs once on a background thread, triggered by the network library's first use
// (bbnet_resolve / bbnet_configure under BB_PARTY). Shutdown (bbnet_shutdown, from every exit
// path) sends BYE and removes the UPnP mappings; it is bounded (< 2.5 s) and idempotent.
// Local tests (BB_PARTY_LOOPBACK=1 or BB_MP_LOCAL_TEST=1): 127.0.0.1 everywhere.
#pragma once

#include "party_link.h"

#include <cstdint>
#include <string>
#include <vector>

namespace party {

enum class RuntimeRole { None, Host, Guest };
const char* runtime_role_name(RuntimeRole r);

struct RuntimeStatus {
    RuntimeRole role = RuntimeRole::None;
    bool started = false;              // the startup thread finished (successfully or not)
    bool restarted = false;            // runtime_restarted()
    LinkState state = LinkState::Idle;
    std::string name;                  // our member name / online id
    int local_slot = -1;
    std::uint16_t port = 0;            // party port (TCP link and UDP game traffic)
    std::string internet_code, lan_code;  // host
    std::string public_address;        // host: "ip:port (source)"
    std::string host_address;          // guest: the host we connect to
    std::string upnp;                  // host: UPnP outcome
    std::vector<RosterEntry> roster;
    std::string last_error;            // the latest failure / rejection, empty when none
};

// Crash recovery: this run restarted after an unclean exit - BB_PARTY_RESTARTED=1 (run.bat's
// restart loop) or <user>/party_state.json left by a run that did not shut down cleanly in the
// last 30 minutes (written at startup, removed by runtime_shutdown). Known from the first call,
// before the startup thread is done; the director uses it to continue past the title on its own.
bool runtime_restarted();

// Accessors for the director / UI: party::runtime().restarted(), .link(), .status() ...
struct RuntimeApi {
    bool restarted() const { return runtime_restarted(); }
    PartyLink* link() const;
    RuntimeStatus status() const;
    std::string status_line() const;
    void set_local_state(MemberState state, std::uint32_t map_id) const;
};
const RuntimeApi& runtime();

// Starts the runtime once (no-op without BB_PARTY). Returns immediately.
void runtime_start();
// BYE + UPnP unmap; idempotent; bounded.
void runtime_shutdown();
RuntimeStatus runtime_status();
// One line for the overlay / status: "party host Hunter0: hosting :47600, 2/3 (Hunter1 3 ms)".
std::string party_status_line();
// The live link (nullptr before startup or when it failed). Never deleted: safe to keep.
PartyLink* runtime_link();
// This player's roster entry (passthrough to PartyLink::set_local_state; kept until the link
// exists).
void set_local_state(MemberState state, std::uint32_t map_id);

}  // namespace party

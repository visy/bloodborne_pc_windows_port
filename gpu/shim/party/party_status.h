// SPDX-License-Identifier: GPL-3.0-or-later
// Party status board (plan phase A7): a small thread-safe snapshot between the party code
// (director / runtime: producers) and the UI (overlay Party tab, toasts: consumer). Neither side
// includes the other's headers.
//
//   producers (any thread):  SetRole, SetCode, SetState(state, detail), SetMembers, SetLastEvent
//   UI (any thread):         Snapshot() copy, EventSeq() to notice a new event cheaply,
//                            RequestLeave / RequestRejoin / KickMember (queued),
//                            RequestCopyCode (done at once on the calling thread, Win32 clipboard)
//   director (main thread):  PopCommand() once a frame, until it returns false
//
// No game, no link, no ImGui: unit-tested by tests/test_party_status.cpp.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace party::status {

enum class Role : std::uint8_t { Off, Host, Guest };

enum class State : std::uint8_t {
    Off,              // party mode not running
    Starting,         // runtime coming up (UPnP, public address, code)
    Hosting,          // host: listening, the world is open to the party
    Connecting,       // guest: link handshake in progress
    WaitingForWorld,  // link up, this player's world not loaded yet (title, loading)
    RingingBell,      // a bell is up (host: Beckoning, guest: Small Resonant)
    Joined,           // guest: in the host's world / host: members are in
    Travelling,       // the party moves (lamp, area load); rejoin follows
    Reconnecting,     // link lost, retrying with backoff
    Error,            // rejected or failed: see detail
};

const char* RoleName(Role role);     // "off" / "host" / "guest"
const char* StateName(State state);  // "off", "starting", ... (stable, for logs)

struct Member {
    std::string name;
    bool connected = false;  // false while the host keeps a lost member's slot
    bool in_world = false;   // in the host's world (summoned)
    int ping_ms = -1;        // -1: unknown
    int slot = -1;           // 0 = host
    std::string area;        // map / area text ("m24_01 Cathedral Ward"), may be empty
    bool local = false;      // this player
};

struct Board {
    bool enabled = false;  // BB_PARTY set for this run
    Role role = Role::Off;
    State state = State::Off;
    std::string detail;    // with the state ("2/3, Hunter1 waiting", the reject reason ...)
    std::string code;      // the party code to share (host) / joined (guest)
    std::vector<Member> members;
    std::string last_event;
    std::uint64_t event_seq = 0;  // +1 per SetLastEvent
    std::int64_t event_ms = 0;    // steady clock (ms) of the last event
    std::uint64_t version = 0;    // +1 per change of anything above
};

// --- producer side ---
void SetRole(Role role);
void SetCode(const std::string& code);
void SetState(State state, const std::string& detail = {});
void SetMembers(const std::vector<Member>& members);
/// A one-line event for the log line and the toast ("Hunter1 joined", "Travelling to ...").
void SetLastEvent(const std::string& text);

// --- consumer side ---
Board Snapshot();
/// Cheap (atomic): changes whenever SetLastEvent is called.
std::uint64_t EventSeq();
/// BB_PARTY is set (non-empty) for this run (read once, or forced by tests via ResetForTest).
bool Enabled();

// --- commands: UI -> party code ---
enum class CommandType : std::uint8_t { Leave, Rejoin, Kick };
const char* CommandName(CommandType type);

struct Command {
    CommandType type = CommandType::Leave;
    std::string name;  // Kick: the member's name
};

void RequestLeave();
void RequestRejoin();
/// Host only: false (nothing queued) when this player is not the host, the name is empty or
/// not a remote member of the current roster.
bool KickMember(const std::string& name);
/// Copies the current party code to the Windows clipboard (UI thread, no queue). False when
/// there is no code or the clipboard is busy.
bool RequestCopyCode();
/// The director, once a frame: true and *out filled while commands are queued (FIFO; at most 16
/// kept, a repeated Leave / Rejoin / Kick of the same name is coalesced).
bool PopCommand(Command* out);

/// Tests: clears everything; `enabled` replaces the BB_PARTY check.
void ResetForTest(bool enabled);

}  // namespace party::status

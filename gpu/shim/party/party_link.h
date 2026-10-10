// SPDX-License-Identifier: GPL-3.0-or-later
// PartyLink: the party's TCP control channel. The host's game listens on the party port
// (BB_PARTY_PORT, default 9307); guests connect to it directly (no server anywhere).
//
// Wire: every frame is [u32 len LE][u8 type][payload], len = 1 + payload size.
//   guest -> HELLO     {magic "BBPL", u16 version, str name, sha256 eboot[32], mods hash[32], token[16],
//                       gameplay patches hash[32], str patch names, str mod names,  (newline-separated)
//                       str party rules}  (max players + 4-player rule set, party_fourp.h)
//   host  -> CHALLENGE {nonce[24]}                         (or REJECT on version / hash / name; a
//                       Mismatch REJECT names what differs: identity_mismatch)
//   guest -> AUTH      {nonce[24], proof[32]}              proof = keyed BLAKE2b (party_crypto.h)
//   host  -> WELCOME   {slot, max players, resumed, host clock ms, observed ip[4] + port, token,
//                       roster} -- the first ENCRYPTED frame (proves the host knows the key too)
//         or REJECT    {u8 code, str reason}               plaintext, then the host closes
// From WELCOME on, every frame is [len][0xE0][mac 16][XChaCha20-Poly1305(type || payload)] with a
// per-direction key and an implicit per-direction counter nonce (party_crypto.h).
// Runtime: PING/PONG (1 Hz both ways, RTT), ROSTER, RPC_REQ/RPC_RESP, EVENT/EVENT_ACK (cursor;
// unacked events are replayed after a reconnect and de-duplicated by cursor), PARTY_CMD,
// PROGRESS, BYE. No frame for lost_timeout (10 s) = connection lost; the host keeps a lost
// member's slot (and its event queue) for slot_keep (60 s); the guest reconnects with
// exponential backoff 1, 2, 4 .. 10 s and gets the same slot back (resume token). A restarted
// host restores its member table (slot, name, token: kept_members / restore_members, from the
// party runtime's crash marker), so its guests get their slots back after a host crash too.
//
// Threads: one IO thread (WSAPoll) per PartyLink, plus one callback thread: every callback runs
// on the callback thread, one at a time, never while PartyLink's lock is held, so callbacks may
// call back into PartyLink (send_*, rpc_call_async, roster ...). Blocking rpc_call() from a
// callback works too (replies are matched on the IO thread) but stalls other callbacks meanwhile.
// Never call stop() / the destructor from a callback.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace party {

constexpr std::uint16_t kLinkProtocolVersion = 2;  // 2: HELLO carries the patch / mod sets
constexpr int kBroadcast = -1;  // send_* target: every member (host side)
constexpr int kHostSlot = 0;

enum class MemberState : std::uint8_t { Title = 0, Home = 1, Joining = 2, InHostWorld = 3, Dead = 4, Loading = 5 };
const char* member_state_name(MemberState s);

struct RosterEntry {
    int slot = 0;
    std::string name;
    MemberState state = MemberState::Title;
    bool connected = false;  // false while the host keeps a lost member's slot
    std::uint32_t map_id = 0;
    std::uint32_t ping_ms = 0;  // as measured by the host
};

enum class RejectCode : std::uint8_t {
    None = 0,
    Version = 1,   // different PartyLink protocol
    Mismatch = 2,  // different game version / gameplay patches / gameplay mods (the reason says which)
    Auth = 3,      // wrong password or party code
    Full = 4,
    Name = 5,      // invalid or duplicate name
    Protocol = 6,
    Shutdown = 7,  // host ended the party (BYE)
    Kicked = 8,
};

enum class LinkState : std::uint8_t {
    Idle,          // not started
    Hosting,       // host: listening
    Connecting,    // guest: TCP connect / handshake in progress
    Connected,     // guest: WELCOME received
    Reconnecting,  // guest: lost, waiting for the next attempt
    Rejected,      // guest: REJECT or BYE received; no more attempts (reject_code / reject_reason)
    Stopped,
};
const char* link_state_name(LinkState s);

struct LinkConfig {
    std::string name = "Hunter";  // <= 16 of [A-Za-z0-9_-]
    // The version check: players must match in all three hashes. The names only explain a
    // mismatch (the host lists what each side has); party_runtime fills them from
    // out/party_patch_hash.txt (scripts/patches.py) and out/party_mods.txt (scripts/mods.py).
    std::array<std::uint8_t, 32> eboot_sha256{};
    std::array<std::uint8_t, 32> patches_hash{};  // gameplay patches (zeros: none / not compared)
    std::array<std::uint8_t, 32> mods_hash{};     // gameplay mods (zeros: none)
    std::vector<std::string> patch_names, mod_names;
    // The party's game rules: "max N players, 4p:..." (party_runtime from BB_PARTY_MAX and
    // party_fourp.h FourpRulesTag). Part of the version check: must equal the host's.
    std::string rules;
    std::string password;               // BB_PARTY_PASSWORD (optional)
    std::array<std::uint8_t, 8> secret{};  // the party code's secret (zeros for plain host:port)
    std::uint16_t port = 9307;          // host: listen port (0 = ephemeral, see bound_port())
    std::string bind_addr = "0.0.0.0";  // host
    int max_players = 3;                // 2..4 including the host (BB_PARTY_MAX)
    // Timing (tests shrink these).
    int ping_interval_ms = 1000;
    int lost_timeout_ms = 10000;
    int slot_keep_ms = 60000;
    int backoff_initial_ms = 1000;
    int backoff_max_ms = 10000;  // a crashed host is back within seconds of its restart
    int connect_timeout_ms = 5000;
    int roster_refresh_ms = 5000;  // host re-broadcasts the roster (fresh pings) this often
};

// What differs between the host's and a guest's game (eboot.bin, gameplay patches, gameplay mods),
// one clause each, with the patch / mod names only one side has; empty when they match. The host
// sends it as the Mismatch REJECT reason.
std::string identity_mismatch(const LinkConfig& host, const LinkConfig& guest);
// Reads an identity file (out/party_patch_hash.txt, out/party_mods.txt): "hash <64 hex>" and
// "<item_key> <name>" lines; '#' lines are comments. False when it has no valid hash line.
bool parse_identity_file(const std::string& text, const std::string& item_key, std::array<std::uint8_t, 32>* hash,
                         std::vector<std::string>* items);

// BB_PARTY_PORT, BB_PARTY_MAX (clamped 2..4), BB_PARTY_PASSWORD over `cfg`.
void apply_link_env(LinkConfig& cfg);
bool valid_member_name(const std::string& name);

// Host: a member's slot as kept across a host restart.
struct KeptMember {
    int slot = 0;
    std::string name;
    std::array<std::uint8_t, 16> token{};
};

struct LinkCallbacks {
    // Host: a member completed the handshake (rejoined = it got its kept slot back).
    std::function<void(const RosterEntry& member, bool rejoined)> on_member_joined;
    // Host: slot_kept = connection lost, slot held for slot_keep_ms; false = slot released (BYE,
    // expiry, kick).
    std::function<void(const RosterEntry& member, bool slot_kept)> on_member_left;
    // Host: a guest's RPC_REQ; the returned JSON is the reply.
    std::function<std::string(int slot, const std::string& kind, const std::string& json)> on_rpc;
    // Both: an EVENT (host side: from guest `from_slot`; guest side: from_slot = 0).
    std::function<void(int from_slot, std::uint64_t cursor, const std::string& name, const std::string& json)> on_event;
    std::function<void(int from_slot, const std::string& cmd, const std::string& json)> on_party_cmd;
    std::function<void(int from_slot, const std::vector<std::uint8_t>& blob)> on_progress;
    std::function<void(const std::vector<RosterEntry>& roster)> on_roster;
    std::function<void(LinkState state, const std::string& detail)> on_state;
    std::function<void(const std::string& line)> on_log;
};

class PartyLink {
public:
    PartyLink(LinkConfig cfg, LinkCallbacks cb);
    ~PartyLink();  // stop()
    PartyLink(const PartyLink&) = delete;
    PartyLink& operator=(const PartyLink&) = delete;

    // Host: derives the key, listens, starts the threads. False + error when the port is taken.
    bool start_host(std::string* error = nullptr);
    // Guest: derives the key and starts connecting in the background (retries with backoff until
    // WELCOME or REJECT). `host` is an IPv4 literal or hostname.
    bool start_guest(const std::string& host, std::uint16_t port, std::string* error = nullptr);
    // Graceful: BYE to everyone (host: members get RejectCode::Shutdown), then closes.
    // graceful = false drops the sockets without BYE (tests: simulates a crash).
    void stop(bool graceful = true);

    bool is_host() const;
    LinkState state() const;
    bool wait_state(LinkState s, int timeout_ms) const;  // true once state() == s
    bool wait_connected(int timeout_ms) const { return wait_state(LinkState::Connected, timeout_ms); }
    int local_slot() const;  // host 0; guest its WELCOME slot (-1 before)
    std::uint16_t bound_port() const;
    int max_players() const;
    std::vector<RosterEntry> roster() const;
    std::int64_t host_clock_ms() const;  // host: its own; guest: estimated from PONGs
    std::uint32_t rtt_ms() const;        // guest: last RTT to the host
    std::string observed_address() const;  // guest: "ip:port" the host saw us at
    // Host: the IPv4 (network byte order) member `slot` is connected from; 0 when not connected.
    std::uint32_t member_ip(int slot) const;
    RejectCode reject_code() const;
    std::string reject_reason() const;

    // Guest -> host RPC. Blocking: false on timeout / disconnect / no handler (reply then holds
    // the reason). Async: `done` runs on the callback thread exactly once; returns the id (0 when
    // it failed immediately; `done` still runs).
    bool rpc_call(const std::string& kind, const std::string& json, std::string* reply, int timeout_ms);
    std::uint32_t rpc_call_async(const std::string& kind, const std::string& json,
                                 std::function<void(bool ok, const std::string& reply)> done, int timeout_ms);

    // Reliable ordered events. Host: to `slot` or kBroadcast (queued for a lost member and
    // replayed when it resumes); guest: slot ignored, to the host. Returns the cursor (the last
    // one for a broadcast; 0 when nobody is addressed).
    std::uint64_t send_event(int slot, const std::string& name, const std::string& json);
    // Fire-and-forget (dropped while disconnected). Host: slot or kBroadcast; guest: to the host.
    bool send_party_cmd(int slot, const std::string& cmd, const std::string& json);
    bool send_progress(int slot, const std::vector<std::uint8_t>& blob);
    // This player's roster entry (host: broadcast to all; guest: reported to the host).
    void set_local_state(MemberState state, std::uint32_t map_id);
    // Host: every member's slot, name and resume token (for the crash marker).
    std::vector<KeptMember> kept_members() const;
    // Host, before start_host: members of the previous (crashed) run, as lost members whose slot
    // is kept for slot_keep_ms from start_host. They come back by token or name; their WELCOME
    // says "not resumed" (the event streams start afresh: this process is new).
    void restore_members(const std::vector<KeptMember>& members);
    // Guest: the last WELCOME resumed our previous session (false: the host restarted or released
    // our slot - whatever the host knew about us is gone).
    bool last_welcome_resumed() const;

    // Host: drop a member and free its slot (BYE Kicked, reason default "kicked by the host").
    // The name is refused (REJECT Kicked) for the rest of this link's life: no auto-rejoin.
    bool kick(int slot, const std::string& reason);
    // Host: kick() by member name; false when no remote member has it.
    bool kick_name(const std::string& name, const std::string& reason = {});
    bool is_banned(const std::string& name) const;
    // Guest: while Reconnecting, try again now (backoff reset). False in any other state.
    bool reconnect_now();

    // Tests: stop all IO (no reads, writes or pings) for `ms`, simulating a frozen network.
    void debug_freeze(int ms);
    // Tests: abort the current connection(s) without BYE (guest then reconnects).
    void debug_drop_connections();

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace party

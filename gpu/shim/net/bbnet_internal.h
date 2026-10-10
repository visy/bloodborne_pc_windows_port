// SPDX-License-Identifier: GPL-3.0-or-later
// Shared declarations of the party network library (gpu/shim/net): the settings read from the
// environment, the logging helper, the guest-callable export tables, the P2P port services the
// session layer uses (STUN, relay, hole punching) and the guest-callback dispatcher.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#define BBNET_ABI __attribute__((sysv_abi))

// Runtime entry points (src/probe.c, src/runtime_thread.c, src/runtime.c).
extern "C" {
BBNET_ABI void restore_guest_fs(void);
void runtime_thread_attach_host(const char* name);
const char* runtime_symbol(const char* nid);
}

namespace bbnet {

// Every guest-callable function restores the guest FS base on its way out (as the runtime's
// mutex and semaphore functions do): Windows does not keep FS across a wait, and a blocking
// call (epoll, recv, a contended lock) would return to guest code with the TEB's FS.
struct GuestReturn {
    GuestReturn() = default;
    GuestReturn(const GuestReturn&) = delete;
    GuestReturn& operator=(const GuestReturn&) = delete;
    ~GuestReturn() { restore_guest_fs(); }
};
#define BBNET_GUEST_RETURN() ::bbnet::GuestReturn bbnet_guest_return_

struct Settings {
    bool party = false;          // BB_PARTY set (non-empty)
    bool host = false;           // BB_PARTY=host (or BB_PARTY_HOST=1): answers relay HELLOs
    std::string party_value;     // BB_PARTY as given
    std::uint16_t party_port = 9307;  // BB_PARTY_PORT: the UDP port the game's P2P port 3658 maps to
    std::string online_id;       // BB_PARTY_NAME (validated) else BB_USER_NAME, at most 16 characters
    bool np_state_callback = true;  // BB_NP_STATE_CB=0: sceNpCheckCallback fires nothing
    std::string app0, user_dir;  // from bbnet_configure
};
const Settings& settings();
void set_dirs(const char* app0, const char* user_dir);

void log(const char* fmt, ...) __attribute__((format(gnu_printf, 1, 2)));

struct Export {
    const char* name;
    void* fn;
};
// Guest-callable functions by symbol name; nullptr-terminated.
extern const Export kNetExports[];
extern const Export kNpExports[];

// --- The P2P port (net_socket.cpp) ---

// The UDP port the party traffic uses, once bound (0 before).
std::uint16_t p2p_bound_port();
// Opens the party UDP port now (the game opens it at its first P2P bind); false on failure.
bool p2p_open();
// Sends a STUN Binding Request from the party port to host:port and waits for the answer;
// with `want_relay` the request carries the relay HELLO and a relay in the answer turns
// relay framing on for that server's other ports.
struct RelayInfo {
    bool present = false;
    std::uint16_t vport = 0;
    std::uint32_t observed_addr = 0;  // network order
    std::uint16_t observed_port = 0;
};
bool p2p_stun(const char* host, std::uint16_t port, int timeout_ms, std::uint32_t* mapped_addr,
              std::uint16_t* mapped_port, bool want_relay = false, RelayInfo* relay = nullptr);
bool p2p_relay(std::uint32_t* server, std::uint16_t* vport);
void p2p_punch(const char* label, std::uint32_t addr, std::uint16_t port, std::uint32_t local_addr,
               std::uint16_t local_port);
// One line about the party port (counters), for the status line.
std::string p2p_status();
// This machine's LAN IPv4 address (network order), BB_PARTY_LOCAL_IP overriding; 0 when none.
std::uint32_t local_ipv4();

// --- NP (np_manager.cpp) ---
void fill_npid(void* out, const char* online);

// --- Guest callbacks (bbnet_glue.cpp) ---
// Calls the guest's System V function `fn` with up to six integer arguments on the dispatcher
// thread (attached to the runtime, guest FS restored before each call), in posting order.
void post_guest_call(std::uintptr_t fn, std::uint64_t a0 = 0, std::uint64_t a1 = 0, std::uint64_t a2 = 0,
                     std::uint64_t a3 = 0, std::uint64_t a4 = 0, std::uint64_t a5 = 0);
// Waits until every call posted so far has returned (tests, shutdown).
void drain_guest_calls();

}  // namespace bbnet

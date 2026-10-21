// SPDX-License-Identifier: GPL-3.0-or-later
// The party port's route table (F2, 0.7.1; docs/party/network_robustness.md 4a): IPv6 and the
// member relay under the IPv4 endpoints the game knows. Every datagram the party port sends or
// receives passes Router (net_socket.cpp), so the game, the party link's reliable UDP stream,
// STUN and the host relay keep seeing IPv4 endpoints ("keys") and need no change.
//
//  - IPv6: a key bound to an IPv6 endpoint (bind_v6) is sent there when its route is on, and
//    datagrams from that endpoint are presented as the key. An IPv6 source with no binding gets a
//    synthetic key 240.x.y.z:port (240/4 is reserved and never routed; a hash of the address, so
//    every PC derives the same key for the same peer).
//  - Member relay, frames [0xfc][type] on the party port:
//      Q1 hello     guest -> member  [fc 'Q' 1][client nonce 16][pad 21]   (padded: no amplification)
//      Q2 nonce     member -> guest  [fc 'Q' 2][client nonce 16][server nonce 16]  (stateless)
//      Q3 auth      guest -> member  [fc 'Q' 3][client nonce 16][server nonce 16][proof 16]
//      Q4 welcome   member -> guest  [fc 'Q' 4][client nonce 16][session u32][name len u8][name][proof 16]
//      Q5 refusal   member -> guest  [fc 'Q' 5][client nonce 16][code u8][wait s u16]   (a hint only)
//      T  to relay  guest -> member  [fc 'T'][session u32] + a datagram for the host (empty: keepalive)
//      V  to host   member -> host   [fc 'V'][guest addr 4][guest port 2] + that datagram
//      v  from host host -> member   [fc 'v'][guest addr 4][guest port 2] + a datagram for the guest
//      t  to guest  member -> guest  [fc 't'][session u32] + that datagram
//    Proofs are a keyed MAC (Mac: the party runtime's BLAKE2b under a subkey of the party key) over
//    a label and the nonces: only party members can use or serve a relay. The guest presents the
//    host at the host's own key; the host presents the guest at the guest's endpoint as the
//    member sees it (its link, STUN, room records and the game run end to end over it).
//  - Security (F3): a member serves only while told to (in the party, its own link not relayed, the
//    switch on); wrong proofs go to a Throttle (party_link.h AuthThrottle); it forwards only between
//    its sessions (kMaxSessions, rate-limited, idle kSessionIdle) and its own host endpoint. The
//    host accepts V only from a member (is_member) and, for a new route, only for a guest endpoint
//    it has not heard directly within kDirectGuard (a member cannot pose as another member).
// Pure state machine plus a mutex: no sockets, no threads (party-nat-test drives it over
// simulated NATs).
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace bbnet::udp {

using RClock = std::chrono::steady_clock;

struct Ip6 {
    std::array<std::uint8_t, 16> b{};
    bool operator<(const Ip6& o) const { return b < o.b; }
    bool operator==(const Ip6& o) const { return b == o.b; }
};
struct Ep6 {
    Ip6 addr;
    std::uint16_t port = 0;  // network order
    bool operator<(const Ep6& o) const { return addr < o.addr || (addr == o.addr && port < o.port); }
    bool operator==(const Ep6& o) const { return addr == o.addr && port == o.port; }
};

bool parse_ip6(const std::string& text, Ip6* out);  // "2001:db8::1", "::1" ("[...]" accepted)
std::string ip6_text(const Ip6& a);                 // compressed form
bool ip6_global(const Ip6& a);                      // 2000::/3 (global unicast)
bool ip6_loopback(const Ip6& a);                    // ::1
bool ip6_zero(const Ip6& a);
std::string key_text(std::uint32_t addr_nbo, std::uint16_t port_nbo);  // "a.b.c.d:port"

// The synthetic key of an IPv6 endpoint (240.0.0.0 - 247.255.255.255, port kept).
bool is_synthetic(std::uint32_t addr_nbo);
void synthetic_key(const Ep6& e, std::uint32_t* addr_nbo, std::uint16_t* port_nbo);

constexpr std::uint8_t kRouteMagic = 0xfc;
inline bool is_route_frame(const std::uint8_t* d, std::size_t n) { return n >= 2 && d[0] == kRouteMagic; }

// What goes on the wire: an IPv4 datagram or an IPv6 one.
struct Wire {
    bool v6 = false;
    std::uint32_t addr = 0;  // IPv4, network order
    std::uint16_t port = 0;  // network order (both families)
    Ip6 addr6;
    std::vector<std::uint8_t> data;
};

class Router {
public:
    // A keyed MAC (16 bytes) over a message; the same function on every party member.
    using Mac = std::function<void(const std::uint8_t* d, std::size_t n, std::uint8_t out[16])>;
    // Online password guessing at the member (party_link.h AuthThrottle over an address).
    struct Throttle {
        std::function<bool(std::uint32_t ip, std::int64_t* remaining_ms)> locked;
        std::function<void(std::uint32_t ip)> fail, success;
    };
    static constexpr std::size_t kMaxSessions = 4;
    static constexpr std::size_t kMaxHostRoutes = 16;
    static constexpr std::size_t kMaxV6 = 1024;
    static constexpr auto kSessionIdle = std::chrono::seconds(30);
    static constexpr auto kHostRouteIdle = std::chrono::seconds(60);
    static constexpr auto kDirectGuard = std::chrono::seconds(10);
    static constexpr auto kHandshakeTimeout = std::chrono::seconds(4);
    static constexpr auto kKeepalive = std::chrono::seconds(4);
    // Per relayed session at the member: datagrams and bytes per second (bursts twice that).
    static constexpr double kSessionFrameRate = 1000, kSessionByteRate = 2.0 * 1024 * 1024;
    enum Refusal : std::uint8_t { kWrongKey = 1, kLockedOut = 2, kNotServing = 3, kFull = 4, kTimeout = 0xff };

    // ---- IPv6 ----
    void set_ipv6(bool on);
    bool ipv6() const;
    // `key` (the IPv4 endpoint the game knows) is reached at `e6`: datagrams from e6 are presented
    // as the key; datagrams to the key go to e6 while `route` is on.
    void bind_v6(std::uint32_t key_addr, std::uint16_t key_port, const Ep6& e6, bool route);
    void route_v6(std::uint32_t key_addr, std::uint16_t key_port, bool route);
    void unbind_v6(std::uint32_t key_addr, std::uint16_t key_port);
    bool v6_of(std::uint32_t key_addr, std::uint16_t key_port, Ep6* e6, bool* route = nullptr) const;
    // The key an IPv6 source is presented as (its binding, else its synthetic key, remembered).
    void key_of(const Ep6& e6, std::uint32_t* addr, std::uint16_t* port);

    // ---- member relay: the guest that cannot reach the host ----
    enum class ClientState { Off, Handshake, Ready, Refused };
    // Reach our host (its key) through the member at `member` (its key): the handshake starts now.
    void relay_use(std::uint32_t member_addr, std::uint16_t member_port, std::uint32_t host_addr,
                   std::uint16_t host_port, Mac mac, RClock::time_point now);
    void relay_stop();
    // `name`: the member's name from its welcome; `code`: the refusal (Refusal); `wait_s`: its wait.
    ClientState relay_state(std::string* name = nullptr, int* code = nullptr, int* wait_s = nullptr) const;
    bool relay_member(std::uint32_t* addr, std::uint16_t* port) const;

    // ---- member relay: the member serving ----
    // Serve guests of our party that cannot reach our host (its key, as our link reaches it).
    void serve(bool on, std::uint32_t host_addr, std::uint16_t host_port, Mac mac, std::string own_name,
               Throttle throttle);
    bool serving() const;
    struct Served {
        std::uint32_t addr = 0;
        std::uint16_t port = 0;
        std::uint64_t up = 0, down = 0;  // datagrams to / from the host
    };
    std::vector<Served> served() const;

    // ---- the host ----
    // Accept guests relayed by a member (is_member: that address:port is a party member's).
    void accept_relayed(bool on, std::function<bool(std::uint32_t addr, std::uint16_t port)> is_member);
    struct HostRoute {
        std::uint32_t guest_addr = 0, member_addr = 0;
        std::uint16_t guest_port = 0, member_port = 0;
    };
    std::vector<HostRoute> host_routes() const;
    bool relayed_guest(std::uint32_t addr, std::uint16_t port, std::uint32_t* member_addr = nullptr,
                       std::uint16_t* member_port = nullptr) const;

    // ---- datagrams ----
    // A datagram for key to_addr:to_port: what goes on the wire, appended to *out (nothing when it
    // cannot be sent: a synthetic key without IPv6, the relay handshake still running).
    void outgoing(std::uint32_t to_addr, std::uint16_t to_port, const std::uint8_t* d, std::size_t n,
                  RClock::time_point now, std::vector<Wire>* out);
    // An IPv6 datagram for key to_addr:to_port regardless of its route (probes); false: no binding.
    bool outgoing_v6(std::uint32_t to_addr, std::uint16_t to_port, const std::uint8_t* d, std::size_t n,
                     std::vector<Wire>* out);
    // A datagram that arrived (`from`: its source and bytes). True: deliver `*data` from key
    // *src_addr:*src_port to the layers above (*via_v6: it came over IPv6; *via_relay: through a
    // member relay). False: consumed (answers, if any, appended to *out).
    bool incoming(const Wire& from, RClock::time_point now, std::vector<std::uint8_t>* data, std::uint32_t* src_addr,
                  std::uint16_t* src_port, std::vector<Wire>* out, bool* via_v6 = nullptr, bool* via_relay = nullptr);
    // Handshake retries, keepalives and expiry.
    void poll(RClock::time_point now, std::vector<Wire>* out);
    // Log lines since the last call (path changes, sessions, refusals).
    std::vector<std::string> take_notes();

private:
    struct V6 {
        Ep6 e6;
        bool route = false;
        bool synthetic = false;
        RClock::time_point seen{};
    };
    struct Bucket {
        double tokens = 0, rate = 0, burst = 0;
        RClock::time_point at{};
        bool take(double n, RClock::time_point now);
    };
    struct Session {
        std::uint32_t addr = 0;
        std::uint16_t port = 0;
        std::uint32_t id = 0;
        std::array<std::uint8_t, 16> cn{};
        RClock::time_point last{};
        Bucket frames, bytes;
        std::uint64_t up = 0, down = 0, dropped = 0;
    };
    struct HostRouteState {
        HostRoute r;
        RClock::time_point last{};
    };
    static std::uint64_t k(std::uint32_t a, std::uint16_t p) { return a | (static_cast<std::uint64_t>(p) << 32); }
    void note(std::string s);
    void key_of_locked(const Ep6& e6, std::uint32_t* addr, std::uint16_t* port);
    void wire_to(std::uint32_t addr, std::uint16_t port, std::vector<std::uint8_t> data, std::vector<Wire>* out) const;
    void server_nonce(std::uint32_t a, std::uint16_t p, const std::uint8_t* cn, std::int64_t minute,
                      std::uint8_t out[16]) const;
    void client_send_hello(RClock::time_point now, std::vector<Wire>* out);
    void client_send_auth(RClock::time_point now, std::vector<Wire>* out);
    bool on_frame(std::uint32_t sa, std::uint16_t sp, std::vector<std::uint8_t>& d, RClock::time_point now,
                  std::uint32_t* src_addr, std::uint16_t* src_port, std::vector<Wire>* out, bool* via_relay);

    mutable std::mutex mu_;
    std::vector<std::string> notes_;
    // IPv6
    bool ipv6_ = false;
    std::map<std::uint64_t, V6> v6_by_key_;
    std::map<Ep6, std::uint64_t> key_by_v6_;
    // client
    struct Client {
        ClientState st = ClientState::Off;
        std::uint32_t m_addr = 0, h_addr = 0;
        std::uint16_t m_port = 0, h_port = 0;
        Mac mac;
        std::array<std::uint8_t, 16> cn{}, sn{};
        bool have_sn = false;
        std::uint32_t session = 0;
        RClock::time_point started{}, next_send{}, last_sent{};
        std::string name;
        int code = 0, wait_s = 0;
    } cl_;
    // server
    bool serving_ = false;
    std::uint32_t sv_h_addr_ = 0;
    std::uint16_t sv_h_port_ = 0;
    Mac sv_mac_;
    std::string sv_name_;
    Throttle sv_throttle_;
    std::array<std::uint8_t, 16> sv_salt_{};
    std::map<std::uint64_t, Session> sessions_;  // by guest key
    // host
    bool accept_ = false;
    std::function<bool(std::uint32_t, std::uint16_t)> is_member_;
    std::map<std::uint64_t, HostRouteState> host_routes_;   // by guest key
    std::map<std::uint64_t, RClock::time_point> direct_;    // keys heard directly (kDirectGuard)
    RClock::time_point direct_pruned_{};
};

}  // namespace bbnet::udp

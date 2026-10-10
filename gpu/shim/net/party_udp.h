// SPDX-License-Identifier: GPL-3.0-or-later
// The party UDP port's framing and the host relay, without sockets: the game's P2P port
// (net_socket.cpp) and the in-process soak test (tests/party_soak.cpp) share it.
//
//  - vport header      [0xff][flags][src vport][dst vport]  (the PS4 kernel's P2P framing)
//  - hole-punch probe  fe 'b' 'b' 'h' 'p' 0 0 0
//  - relay frames      client -> host  [0xfb]['R'][token 8][dst relay port u16 BE] + datagram
//                      host -> client  [0xfb]['r'][src relay port u16 BE] + datagram
//    A client gets its token and relay port from the host's answer to a STUN Binding Request
//    carrying BBHOST-HELLO (net_stun.h). The game never sees a relay frame: a relayed peer is
//    host:relay-port both ways.
//
// Robustness (bbport): a relay port is derived from the token (stable across a host restart:
// a client that re-registers with its old token, by STUN HELLO or simply by sending a frame,
// keeps its port, so the addresses the other members hold stay valid); clients not heard for
// RelayServer::kIdleExpiry are forgotten.
#pragma once

#include "net_stun.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <string>

namespace bbnet::udp {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kMaxDatagram = 2048;  // the game's payload + vport header, at most

// ---- vport header ----
constexpr std::uint8_t kP2pMagic = 0xff;
constexpr std::uint8_t kP2pFlagP2p = 0x80, kP2pFlagByteVports = 0x40, kP2pFlagComid = 0x20, kP2pType = 0x03;
// Splits a datagram at the vport header; returns the header length (0 when there is none).
std::size_t p2p_header(const std::uint8_t* d, std::size_t n, std::uint16_t* src, std::uint16_t* dst);
// Writes the header for src -> dst into out (6 bytes room); returns its length.
std::size_t p2p_write_header(std::uint8_t* out, std::uint16_t src, std::uint16_t dst);

// ---- probe ----
constexpr std::uint8_t kProbe[8] = {0xfe, 'b', 'b', 'h', 'p', 0, 0, 0};
inline bool is_probe(const std::uint8_t* d, std::size_t n) {
    return n >= 8 && d[0] == kProbe[0] && d[1] == kProbe[1] && d[2] == kProbe[2] && d[3] == kProbe[3] &&
           d[4] == kProbe[4];
}

// ---- relay frames ----
constexpr std::uint8_t kRelayMagic = 0xfb, kRelayToServer = 'R', kRelayToClient = 'r';
constexpr std::size_t kRelayHeader = 12;    // [fb]['R'][token 8][port 2]
constexpr std::size_t kDeliveryHeader = 4;  // [fb]['r'][port 2]
inline bool is_relay_request(const std::uint8_t* d, std::size_t n) {
    return n >= kRelayHeader && d[0] == kRelayMagic && d[1] == kRelayToServer;
}
inline bool is_relay_delivery(const std::uint8_t* d, std::size_t n) {
    return n >= kDeliveryHeader && d[0] == kRelayMagic && d[1] == kRelayToClient;
}
inline std::uint64_t token_key(const std::uint8_t* t) {
    std::uint64_t k = 0;
    for (std::size_t i = 0; i < net::stun::kTokenLen; ++i) k |= static_cast<std::uint64_t>(t[i]) << (8 * i);
    return k;
}

// The host's relay. Thread-safe.
class RelayServer {
public:
    static constexpr std::uint16_t kFirstPort = 50001;
    static constexpr std::uint16_t kPortSpan = 12000;
    static constexpr auto kIdleExpiry = std::chrono::minutes(10);

    struct Stats {
        std::uint64_t answered = 0, registered = 0, reregistered = 0, forwarded = 0, forwarded_bytes = 0,
                      unknown_dst = 0, expired = 0;
    };

    // `own_port`: the party port (host order), never handed out as a relay port.
    explicit RelayServer(std::uint16_t own_port = 0);
    void set_own_port(std::uint16_t p);

    // A STUN Binding Request that reached the party port from addr:port (network order). Writes
    // the answer into `out` (net::stun::kMaxResponse bytes) and returns its length; 0 when `d`
    // is not a Binding Request. A HELLO gets (or keeps) a token and relay port when
    // `offer_relay`. `note` (optional) gets a log line for a new registration.
    std::size_t answer_stun(const std::uint8_t* d, std::size_t n, std::uint32_t addr, std::uint16_t port_nbo,
                            bool offer_relay, std::uint8_t* out, std::string* note = nullptr);

    // A relay request frame (is_relay_request) from addr:port. On success the delivery frame is
    // written in place (it starts at buf + kRelayHeader - kDeliveryHeader: *out / *out_len) and
    // its destination returned. A frame with a token this relay does not know (it restarted)
    // registers that token for its sender. False: no such destination port (dropped).
    bool forward(std::uint8_t* buf, std::size_t n, std::uint32_t addr, std::uint16_t port_nbo, std::uint8_t** out,
                 std::size_t* out_len, std::uint32_t* to_addr, std::uint16_t* to_port_nbo,
                 std::string* note = nullptr);

    // The relay port of the client last seen at addr (network order) : port (host order).
    bool vport_for(std::uint32_t addr, std::uint16_t port_host, std::uint16_t* vport) const;
    // Forgets clients idle longer than `idle` (called by the port's reader now and then).
    void expire(Clock::time_point now, Clock::duration idle = kIdleExpiry);
    std::size_t clients() const;
    Stats stats() const;
    // The relay port a token maps to when free (tests).
    static std::uint16_t preferred_port(std::uint64_t token);

private:
    struct Client {
        std::uint64_t token = 0;
        std::uint32_t addr = 0;   // network order
        std::uint16_t port = 0;   // network order
        std::uint16_t vport = 0;  // host order
        Clock::time_point seen;
    };
    Client& register_locked(std::uint64_t token, std::uint32_t addr, std::uint16_t port_nbo, std::string* note,
                            bool re);
    mutable std::mutex mu_;
    std::uint16_t own_port_ = 0;
    std::map<std::uint64_t, Client> by_token_;
    std::map<std::uint16_t, std::uint64_t> by_vport_;
    std::mt19937_64 rng_;
    Stats stats_;
};

// A guest's relay state (what the host's STUN answer gave it). Thread-safe.
class RelayClient {
public:
    // The answer to our HELLO from server:stun_port (network order): relay on with its token
    // and port, or off when the answer had none. True when something changed.
    bool on_answer(std::uint32_t server, std::uint16_t stun_port_nbo, const net::stun::Relay& relay);
    // The token to put in a HELLO to server:stun_port (false: zeros, a first request).
    bool token_for(std::uint32_t server, std::uint16_t stun_port_nbo, std::uint8_t token[net::stun::kTokenLen]) const;
    // A port on the relay server that is a peer's relay port (route_to_peer remembers the ones
    // it routes to). Only those are framed: the host is a player too, so other players can share
    // its address (one machine, one NAT) with ports that are theirs, not the relay's.
    void add_relay_port(std::uint16_t port_host) const;
    // True when addr:port is a known relay port on our relay server: writes the request header
    // into `hdr` (kRelayHeader bytes) and where to send the frame.
    bool frame_for(std::uint32_t addr, std::uint16_t port_nbo, std::uint8_t* hdr, std::uint32_t* stun_addr,
                   std::uint16_t* stun_port_nbo) const;
    // A datagram from addr:port: true when it is a delivery from our relay server; then its
    // payload starts at d + kDeliveryHeader and *src_port_nbo is the sender's relay port.
    bool unwrap(const std::uint8_t* d, std::size_t n, std::uint32_t addr, std::uint16_t port_nbo,
                std::uint16_t* src_port_nbo) const;
    bool get(std::uint32_t* server, std::uint16_t* vport) const;
    // addr:port (network order) is the relay server's STUN port (the host's party port).
    bool is_server(std::uint32_t addr, std::uint16_t port_nbo) const;
    bool on() const;
    void reset();

private:
    mutable std::mutex mu_;
    bool on_ = false;
    std::uint32_t server_ = 0;      // network order
    std::uint16_t stun_port_ = 0;   // network order
    std::uint8_t token_[net::stun::kTokenLen] = {};
    std::uint16_t vport_ = 0;       // host order
    mutable std::set<std::uint16_t> relay_ports_;  // host order
};

// BB_PARTY_FORCE_RELAY=1: a guest sends its game traffic for other guests through the host
// relay even when the direct path would work (tests, strict NATs). Traffic to the host goes to
// the host's party port, which is the relay itself.
bool force_relay_from_env();

// Where a guest sends a peer's game traffic: the peer's relay port on the host when relaying
// is forced (or `prefer_relay`, e.g. after the direct path failed), the relay is on and the peer
// has one; else the direct address. `peer_relay_port` 0 = the peer has none. The host itself
// (addr:port == the relay server's STUN port) is always direct.
struct PeerRoute {
    std::uint32_t addr = 0;  // network order
    std::uint16_t port = 0;  // host order
    bool relayed = false;
};
PeerRoute route_to_peer(const RelayClient& rc, std::uint32_t direct_addr, std::uint16_t direct_port,
                        std::uint16_t peer_relay_port, bool force_relay);

}  // namespace bbnet::udp

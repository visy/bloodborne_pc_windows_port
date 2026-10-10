// SPDX-License-Identifier: GPL-3.0-or-later
// Party UDP framing and the host relay (party_udp.h). Derived from the relay in
// droogie/bbhost src/hle/net.cpp @8f2746c (moved out of net_socket.cpp).
#include "party_udp.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace bbnet::udp {

namespace {

std::uint16_t bswap16(std::uint16_t v) { return static_cast<std::uint16_t>((v << 8) | (v >> 8)); }

std::string addr_text(std::uint32_t addr_nbo, std::uint16_t port_nbo) {
    const auto* b = reinterpret_cast<const std::uint8_t*>(&addr_nbo);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%u.%u.%u.%u:%u", b[0], b[1], b[2], b[3], bswap16(port_nbo));
    return buf;
}

void key_bytes(std::uint64_t k, std::uint8_t* out) {
    for (std::size_t i = 0; i < net::stun::kTokenLen; ++i) out[i] = static_cast<std::uint8_t>(k >> (8 * i));
}

}  // namespace

// ---- vport header ----

std::size_t p2p_header(const std::uint8_t* d, std::size_t n, std::uint16_t* src, std::uint16_t* dst) {
    if (n < 4 || d[0] != kP2pMagic) return 0;
    const std::uint8_t fl = d[1];
    std::size_t at = 2;
    if (fl & kP2pFlagComid) at += 4;
    std::size_t hdr;
    if (fl & kP2pFlagByteVports) {
        if (at + 2 > n) return 0;
        *src = d[at];
        *dst = d[at + 1];
        hdr = at + 2;
    } else {
        if (at + 4 > n) return 0;
        *src = static_cast<std::uint16_t>((d[at] << 8) | d[at + 1]);
        *dst = static_cast<std::uint16_t>((d[at + 2] << 8) | d[at + 3]);
        hdr = at + 4;
    }
    return hdr;
}

std::size_t p2p_write_header(std::uint8_t* out, std::uint16_t src, std::uint16_t dst) {
    out[0] = kP2pMagic;
    if (src < 256 && dst < 256) {
        out[1] = kP2pFlagP2p | kP2pFlagByteVports | kP2pType;
        out[2] = static_cast<std::uint8_t>(src);
        out[3] = static_cast<std::uint8_t>(dst);
        return 4;
    }
    out[1] = kP2pFlagP2p | kP2pType;
    out[2] = static_cast<std::uint8_t>(src >> 8);
    out[3] = static_cast<std::uint8_t>(src);
    out[4] = static_cast<std::uint8_t>(dst >> 8);
    out[5] = static_cast<std::uint8_t>(dst);
    return 6;
}

// ---- RelayServer ----

RelayServer::RelayServer(std::uint16_t own_port)
    : own_port_(own_port),
      rng_(std::random_device{}() ^ static_cast<std::uint64_t>(Clock::now().time_since_epoch().count())) {}

void RelayServer::set_own_port(std::uint16_t p) {
    std::lock_guard<std::mutex> lk(mu_);
    own_port_ = p;
}

std::uint16_t RelayServer::preferred_port(std::uint64_t token) {
    // splitmix64 finalizer: any token bits spread over the range.
    std::uint64_t z = token + 0x9e3779b97f4a7c15ull;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    z ^= z >> 31;
    return static_cast<std::uint16_t>(kFirstPort + z % kPortSpan);
}

RelayServer::Client& RelayServer::register_locked(std::uint64_t token, std::uint32_t addr, std::uint16_t port_nbo,
                                                  std::string* note, bool re) {
    Client c;
    c.token = token;
    c.addr = addr;
    c.port = port_nbo;
    c.seen = Clock::now();
    std::uint16_t vp = preferred_port(token);
    for (unsigned i = 0; i < kPortSpan; ++i) {
        if (vp != own_port_ && !by_vport_.count(vp)) break;
        vp = static_cast<std::uint16_t>(vp + 1 >= kFirstPort + kPortSpan ? kFirstPort : vp + 1);
    }
    c.vport = vp;
    by_vport_[vp] = token;
    if (re) ++stats_.reregistered;
    else ++stats_.registered;
    if (note) {
        *note = "relay: " + addr_text(addr, port_nbo) + " is relay port " + std::to_string(vp) +
                (re ? " (re-registered a token this relay did not know)" : "");
    }
    return by_token_.emplace(token, c).first->second;
}

std::size_t RelayServer::answer_stun(const std::uint8_t* d, std::size_t n, std::uint32_t addr, std::uint16_t port_nbo,
                                     bool offer_relay, std::uint8_t* out, std::string* note) {
    std::uint8_t txid[net::stun::kTxid];
    std::uint8_t token[net::stun::kTokenLen];
    bool hello = false;
    if (!net::stun::parse_binding_request(d, n, txid, &hello, token)) return 0;
    net::stun::Relay relay;
    {
        std::lock_guard<std::mutex> lk(mu_);
        ++stats_.answered;
        if (hello && offer_relay) {
            std::uint64_t key = token_key(token);
            auto it = by_token_.find(key);
            if (it == by_token_.end()) {
                if (key == 0) {
                    do {
                        key = rng_();
                    } while (key == 0 || by_token_.count(key));
                    register_locked(key, addr, port_nbo, note, false);
                } else {
                    // A token from before a restart of ours: the client keeps it (and, unless
                    // taken, its port).
                    register_locked(key, addr, port_nbo, note, true);
                }
                it = by_token_.find(key);
            }
            Client& c = it->second;
            c.addr = addr;
            c.port = port_nbo;
            c.seen = Clock::now();
            relay.present = true;
            key_bytes(c.token, relay.token);
            relay.vport = c.vport;
            relay.observed_addr = addr;
            relay.observed_port = bswap16(port_nbo);
        }
    }
    return net::stun::build_binding_response(out, txid, addr, bswap16(port_nbo), relay.present ? &relay : nullptr);
}

bool RelayServer::forward(std::uint8_t* buf, std::size_t n, std::uint32_t addr, std::uint16_t port_nbo,
                          std::uint8_t** out, std::size_t* out_len, std::uint32_t* to_addr, std::uint16_t* to_port_nbo,
                          std::string* note) {
    if (!is_relay_request(buf, n)) return false;
    const std::uint64_t src_key = token_key(buf + 2);
    const std::uint16_t dst_vport = static_cast<std::uint16_t>((buf[10] << 8) | buf[11]);
    std::uint16_t src_vport = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (src_key == 0) return false;
        auto src = by_token_.find(src_key);
        if (src == by_token_.end()) src = by_token_.find(register_locked(src_key, addr, port_nbo, note, true).token);
        src->second.addr = addr;  // follows the client's NAT rebinding
        src->second.port = port_nbo;
        src->second.seen = Clock::now();
        src_vport = src->second.vport;
        auto dv = by_vport_.find(dst_vport);
        if (dv == by_vport_.end()) {
            ++stats_.unknown_dst;
            return false;
        }
        const Client& dst = by_token_[dv->second];
        *to_addr = dst.addr;
        *to_port_nbo = dst.port;
        ++stats_.forwarded;
        stats_.forwarded_bytes += n - kRelayHeader;
    }
    std::uint8_t* frame = buf + kRelayHeader - kDeliveryHeader;
    frame[0] = kRelayMagic;
    frame[1] = kRelayToClient;
    frame[2] = static_cast<std::uint8_t>(src_vport >> 8);
    frame[3] = static_cast<std::uint8_t>(src_vport);
    *out = frame;
    *out_len = n - (kRelayHeader - kDeliveryHeader);
    return true;
}

bool RelayServer::vport_for(std::uint32_t addr, std::uint16_t port_host, std::uint16_t* vport) const {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& [key, c] : by_token_) {
        (void)key;
        if (c.addr == addr && c.port == bswap16(port_host)) {
            if (vport) *vport = c.vport;
            return true;
        }
    }
    return false;
}

void RelayServer::expire(Clock::time_point now, Clock::duration idle) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto it = by_token_.begin(); it != by_token_.end();) {
        if (now - it->second.seen > idle) {
            by_vport_.erase(it->second.vport);
            it = by_token_.erase(it);
            ++stats_.expired;
        } else {
            ++it;
        }
    }
}

std::size_t RelayServer::clients() const {
    std::lock_guard<std::mutex> lk(mu_);
    return by_token_.size();
}

RelayServer::Stats RelayServer::stats() const {
    std::lock_guard<std::mutex> lk(mu_);
    return stats_;
}

// ---- RelayClient ----

bool RelayClient::on_answer(std::uint32_t server, std::uint16_t stun_port_nbo, const net::stun::Relay& relay) {
    std::lock_guard<std::mutex> lk(mu_);
    const bool was = on_;
    const std::uint16_t was_vport = vport_;
    std::uint8_t was_token[net::stun::kTokenLen];
    std::memcpy(was_token, token_, sizeof was_token);
    if (relay.present) {
        on_ = true;
        server_ = server;
        stun_port_ = stun_port_nbo;
        std::memcpy(token_, relay.token, sizeof token_);
        vport_ = relay.vport;
    } else {
        on_ = false;  // a server without the relay: datagrams go as addressed
    }
    return on_ != was || vport_ != was_vport || std::memcmp(was_token, token_, sizeof was_token) != 0;
}

bool RelayClient::token_for(std::uint32_t server, std::uint16_t stun_port_nbo, std::uint8_t* token) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (!on_ || server_ != server || stun_port_ != stun_port_nbo) return false;
    std::memcpy(token, token_, net::stun::kTokenLen);
    return true;
}

void RelayClient::add_relay_port(std::uint16_t port_host) const {
    std::lock_guard<std::mutex> lk(mu_);
    relay_ports_.insert(port_host);
}

bool RelayClient::frame_for(std::uint32_t addr, std::uint16_t port_nbo, std::uint8_t* hdr, std::uint32_t* stun_addr,
                            std::uint16_t* stun_port_nbo) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (!on_ || addr != server_ || port_nbo == stun_port_) return false;
    if (!relay_ports_.count(bswap16(port_nbo))) return false;  // a player sharing the host's address
    hdr[0] = kRelayMagic;
    hdr[1] = kRelayToServer;
    std::memcpy(hdr + 2, token_, net::stun::kTokenLen);
    std::memcpy(hdr + 10, &port_nbo, 2);  // network order is the frame's big-endian
    *stun_addr = server_;
    *stun_port_nbo = stun_port_;
    return true;
}

bool RelayClient::unwrap(const std::uint8_t* d, std::size_t n, std::uint32_t addr, std::uint16_t port_nbo,
                         std::uint16_t* src_port_nbo) const {
    if (!is_relay_delivery(d, n)) return false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!on_ || addr != server_ || port_nbo != stun_port_) return false;
    }
    const std::uint16_t from = static_cast<std::uint16_t>((d[2] << 8) | d[3]);
    *src_port_nbo = bswap16(from);
    return true;
}

bool RelayClient::get(std::uint32_t* server, std::uint16_t* vport) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (server) *server = server_;
    if (vport) *vport = vport_;
    return on_;
}

bool RelayClient::is_server(std::uint32_t addr, std::uint16_t port_nbo) const {
    std::lock_guard<std::mutex> lk(mu_);
    return on_ && addr == server_ && port_nbo == stun_port_;
}

bool RelayClient::on() const {
    std::lock_guard<std::mutex> lk(mu_);
    return on_;
}

void RelayClient::reset() {
    std::lock_guard<std::mutex> lk(mu_);
    on_ = false;
    server_ = 0;
    stun_port_ = 0;
    relay_ports_.clear();
    std::memset(token_, 0, sizeof token_);
    vport_ = 0;
}

// ---- routing ----

bool force_relay_from_env() {
    const char* v = std::getenv("BB_PARTY_FORCE_RELAY");
    return v && *v && v[0] != '0';
}

PeerRoute route_to_peer(const RelayClient& rc, std::uint32_t direct_addr, std::uint16_t direct_port,
                        std::uint16_t peer_relay_port, bool force_relay) {
    PeerRoute r;
    r.addr = direct_addr;
    r.port = direct_port;
    if (!force_relay || !peer_relay_port) return r;
    std::uint32_t server = 0;
    std::uint16_t own = 0;
    if (!rc.get(&server, &own) || !server) return r;
    if (rc.is_server(direct_addr, bswap16(direct_port))) return r;  // the host: its port is the relay
    if (peer_relay_port == own) return r;  // ourselves
    r.addr = server;
    r.port = peer_relay_port;
    r.relayed = true;
    rc.add_relay_port(peer_relay_port);
    return r;
}

}  // namespace bbnet::udp

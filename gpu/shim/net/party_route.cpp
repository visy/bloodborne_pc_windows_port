// SPDX-License-Identifier: GPL-3.0-or-later
// The party port's route table (party_route.h): IPv6 bindings and the member relay.
#include "party_route.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <random>

namespace bbnet::udp {

namespace {

std::uint16_t sw16(std::uint16_t v) { return static_cast<std::uint16_t>((v << 8) | (v >> 8)); }

void random_fill(std::uint8_t* out, std::size_t n) {
    static std::mutex mu;
    static std::mt19937_64 rng(std::random_device{}() ^
                               static_cast<std::uint64_t>(RClock::now().time_since_epoch().count()));
    std::lock_guard<std::mutex> lk(mu);
    for (std::size_t i = 0; i < n; ++i) out[i] = static_cast<std::uint8_t>(rng() >> 24);
}

void put32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    for (int i = 3; i >= 0; --i) v.push_back(static_cast<std::uint8_t>(x >> (8 * i)));
}
std::uint32_t get32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) << 24 | static_cast<std::uint32_t>(p[1]) << 16 |
           static_cast<std::uint32_t>(p[2]) << 8 | p[3];
}
void put_key(std::vector<std::uint8_t>& v, std::uint32_t a, std::uint16_t p) {
    const auto* b = reinterpret_cast<const std::uint8_t*>(&a);
    v.insert(v.end(), b, b + 4);
    const auto* q = reinterpret_cast<const std::uint8_t*>(&p);
    v.insert(v.end(), q, q + 2);
}
void get_key(const std::uint8_t* d, std::uint32_t* a, std::uint16_t* p) {
    std::memcpy(a, d, 4);
    std::memcpy(p, d + 4, 2);
}
void append(std::vector<std::uint8_t>& v, const char* label) {
    v.insert(v.end(), label, label + std::strlen(label));
}
bool equal16(const std::uint8_t* a, const std::uint8_t* b) {
    std::uint8_t d = 0;
    for (int i = 0; i < 16; ++i) d |= static_cast<std::uint8_t>(a[i] ^ b[i]);
    return d == 0;
}
std::string clean_name(const std::uint8_t* d, std::size_t n) {
    std::string s;
    for (std::size_t i = 0; i < n && s.size() < 16; ++i) {
        const char c = static_cast<char>(d[i]);
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')
            s.push_back(c);
    }
    return s;
}

constexpr std::size_t kQ1 = 40, kQ2 = 35, kQ3 = 51, kQ5 = 22;  // frame sizes (Q4: 40 + name)
constexpr const char* kLabelNonce = "bbport relay nonce v1";
constexpr const char* kLabelAuth = "bbport relay auth v1";
constexpr const char* kLabelOk = "bbport relay ok v1";

}  // namespace

// ---- addresses ----

bool parse_ip6(const std::string& text_in, Ip6* out) {
    std::string t = text_in;
    if (!t.empty() && t.front() == '[') {
        const auto e = t.find(']');
        if (e == std::string::npos) return false;
        t = t.substr(1, e - 1);
    }
    if (const auto pct = t.find('%'); pct != std::string::npos) t = t.substr(0, pct);  // a zone id
    if (t.size() < 2 || t.size() > 39) return false;
    std::vector<std::uint16_t> head, tail;
    bool gap = false;
    std::size_t i = 0;
    if (t.compare(0, 2, "::") == 0) {
        gap = true;
        i = 2;
    } else if (t[0] == ':') {
        return false;
    }
    while (i < t.size()) {
        std::size_t j = i;
        unsigned v = 0;
        while (j < t.size() && j - i < 4 && std::isxdigit(static_cast<unsigned char>(t[j]))) {
            const char c = t[j];
            v = v * 16 + static_cast<unsigned>(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
            ++j;
        }
        if (j == i) return false;
        (gap ? tail : head).push_back(static_cast<std::uint16_t>(v));
        if (j == t.size()) break;
        if (t[j] != ':') return false;
        if (j + 1 < t.size() && t[j + 1] == ':') {
            if (gap) return false;
            gap = true;
            i = j + 2;
            if (i == t.size()) break;
        } else {
            i = j + 1;
            if (i == t.size()) return false;
        }
    }
    const std::size_t groups = head.size() + tail.size();
    if (groups > 8 || (!gap && groups != 8) || (gap && groups == 8)) return false;
    std::array<std::uint16_t, 8> g{};
    for (std::size_t h = 0; h < head.size(); ++h) g[h] = head[h];
    for (std::size_t q = 0; q < tail.size(); ++q) g[8 - tail.size() + q] = tail[q];
    for (int q = 0; q < 8; ++q) {
        out->b[2 * q] = static_cast<std::uint8_t>(g[q] >> 8);
        out->b[2 * q + 1] = static_cast<std::uint8_t>(g[q]);
    }
    return true;
}

std::string ip6_text(const Ip6& a) {
    std::uint16_t g[8];
    for (int q = 0; q < 8; ++q) g[q] = static_cast<std::uint16_t>(a.b[2 * q] << 8 | a.b[2 * q + 1]);
    int best = -1, best_len = 0;
    for (int q = 0; q < 8;) {
        if (g[q]) {
            ++q;
            continue;
        }
        int e = q;
        while (e < 8 && !g[e]) ++e;
        if (e - q > best_len && e - q > 1) {
            best = q;
            best_len = e - q;
        }
        q = e;
    }
    std::string s;
    char buf[8];
    for (int q = 0; q < 8; ++q) {
        if (q == best) {
            s += "::";
            q += best_len - 1;
            continue;
        }
        if (!s.empty() && s.back() != ':') s += ':';
        std::snprintf(buf, sizeof buf, "%x", g[q]);
        s += buf;
    }
    return s.empty() ? "::" : s;
}

bool ip6_global(const Ip6& a) { return (a.b[0] & 0xe0) == 0x20; }
bool ip6_loopback(const Ip6& a) {
    for (int i = 0; i < 15; ++i)
        if (a.b[i]) return false;
    return a.b[15] == 1;
}
bool ip6_zero(const Ip6& a) {
    for (std::uint8_t c : a.b)
        if (c) return false;
    return true;
}

std::string key_text(std::uint32_t addr_nbo, std::uint16_t port_nbo) {
    const auto* b = reinterpret_cast<const std::uint8_t*>(&addr_nbo);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%u.%u.%u.%u:%u", b[0], b[1], b[2], b[3], sw16(port_nbo));
    return buf;
}

bool is_synthetic(std::uint32_t addr_nbo) { return (reinterpret_cast<const std::uint8_t*>(&addr_nbo)[0] & 0xf8) == 0xf0; }

void synthetic_key(const Ep6& e, std::uint32_t* addr_nbo, std::uint16_t* port_nbo) {
    std::uint32_t h = 2166136261u;  // FNV-1a over the address
    for (std::uint8_t c : e.addr.b) h = (h ^ c) * 16777619u;
    const std::uint32_t host = 0xf0000000u | (h & 0x07ffffffu);
    const std::uint8_t b[4] = {static_cast<std::uint8_t>(host >> 24), static_cast<std::uint8_t>(host >> 16),
                               static_cast<std::uint8_t>(host >> 8), static_cast<std::uint8_t>(host)};
    std::memcpy(addr_nbo, b, 4);
    *port_nbo = e.port;
}

// ---- Router ----

bool Router::Bucket::take(double n, RClock::time_point now) {
    if (at == RClock::time_point{}) {
        tokens = burst;
        at = now;
    }
    const double dt = std::chrono::duration<double>(now - at).count();
    at = now;
    tokens = std::min(burst, tokens + dt * rate);
    if (tokens < n) return false;
    tokens -= n;
    return true;
}

void Router::note(std::string s) {
    if (notes_.size() < 256) notes_.push_back(std::move(s));
}

std::vector<std::string> Router::take_notes() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::string> v;
    v.swap(notes_);
    return v;
}

void Router::set_ipv6(bool on) {
    std::lock_guard<std::mutex> lk(mu_);
    ipv6_ = on;
}
bool Router::ipv6() const {
    std::lock_guard<std::mutex> lk(mu_);
    return ipv6_;
}

void Router::bind_v6(std::uint32_t ka, std::uint16_t kp, const Ep6& e6, bool route) {
    std::lock_guard<std::mutex> lk(mu_);
    const std::uint64_t key = k(ka, kp);
    if (auto old = v6_by_key_.find(key); old != v6_by_key_.end() && !(old->second.e6 == e6))
        key_by_v6_.erase(old->second.e6);
    // The endpoint presented as another key before (its synthetic one): this binding wins.
    if (auto prev = key_by_v6_.find(e6); prev != key_by_v6_.end() && prev->second != key) v6_by_key_.erase(prev->second);
    V6& v = v6_by_key_[key];
    const bool changed = !(v.e6 == e6) || v.route != route || v.synthetic;
    v.e6 = e6;
    v.route = route;
    v.synthetic = false;
    v.seen = RClock::now();
    key_by_v6_[e6] = key;
    if (changed)
        note("route: " + key_text(ka, kp) + " is [" + ip6_text(e6.addr) + "]:" + std::to_string(sw16(e6.port)) +
             (route ? " (IPv6, in use)" : " (IPv6 candidate)"));
}

void Router::route_v6(std::uint32_t ka, std::uint16_t kp, bool route) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = v6_by_key_.find(k(ka, kp));
    if (it == v6_by_key_.end() || it->second.synthetic || it->second.route == route) return;
    it->second.route = route;
    note("route: " + key_text(ka, kp) + (route ? " over IPv6 now" : " over IPv4 again"));
}

void Router::unbind_v6(std::uint32_t ka, std::uint16_t kp) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = v6_by_key_.find(k(ka, kp));
    if (it == v6_by_key_.end()) return;
    key_by_v6_.erase(it->second.e6);
    v6_by_key_.erase(it);
}

bool Router::v6_of(std::uint32_t ka, std::uint16_t kp, Ep6* e6, bool* route) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = v6_by_key_.find(k(ka, kp));
    if (it == v6_by_key_.end()) return false;
    if (e6) *e6 = it->second.e6;
    if (route) *route = it->second.route || it->second.synthetic;
    return true;
}

void Router::key_of(const Ep6& e6, std::uint32_t* addr, std::uint16_t* port) {
    std::lock_guard<std::mutex> lk(mu_);
    key_of_locked(e6, addr, port);
}

void Router::key_of_locked(const Ep6& e6, std::uint32_t* addr, std::uint16_t* port) {
    auto it = key_by_v6_.find(e6);
    if (it != key_by_v6_.end()) {
        *addr = static_cast<std::uint32_t>(it->second);
        *port = static_cast<std::uint16_t>(it->second >> 32);
        if (auto v = v6_by_key_.find(it->second); v != v6_by_key_.end()) v->second.seen = RClock::now();
        return;
    }
    synthetic_key(e6, addr, port);
    const std::uint64_t key = k(*addr, *port);
    if (v6_by_key_.size() >= kMaxV6) {  // the stalest synthetic binding goes
        auto old = v6_by_key_.end();
        for (auto i = v6_by_key_.begin(); i != v6_by_key_.end(); ++i)
            if (i->second.synthetic && (old == v6_by_key_.end() || i->second.seen < old->second.seen)) old = i;
        if (old != v6_by_key_.end()) {
            key_by_v6_.erase(old->second.e6);
            v6_by_key_.erase(old);
        }
    }
    if (auto clash = v6_by_key_.find(key); clash != v6_by_key_.end()) key_by_v6_.erase(clash->second.e6);
    V6& v = v6_by_key_[key];
    v.e6 = e6;
    v.synthetic = true;
    v.route = true;
    v.seen = RClock::now();
    key_by_v6_[e6] = key;
}

void Router::wire_to(std::uint32_t addr, std::uint16_t port, std::vector<std::uint8_t> data, std::vector<Wire>* out) const {
    auto it = v6_by_key_.find(k(addr, port));
    Wire w;
    if (it != v6_by_key_.end() && ipv6_ && (it->second.route || it->second.synthetic)) {
        w.v6 = true;
        w.addr6 = it->second.e6.addr;
        w.port = it->second.e6.port;
    } else if (is_synthetic(addr)) {
        return;  // an IPv6 peer we have no route to (or IPv6 is off): never put on the IPv4 wire
    } else {
        w.addr = addr;
        w.port = port;
    }
    w.data = std::move(data);
    out->push_back(std::move(w));
}

void Router::outgoing(std::uint32_t to_addr, std::uint16_t to_port, const std::uint8_t* d, std::size_t n,
                      RClock::time_point now, std::vector<Wire>* out) {
    std::lock_guard<std::mutex> lk(mu_);
    if (cl_.st != ClientState::Off && to_addr == cl_.h_addr && to_port == cl_.h_port) {
        if (cl_.st != ClientState::Ready) return;  // the handshake first; the link dials after it
        std::vector<std::uint8_t> f{kRouteMagic, 'T'};
        put32(f, cl_.session);
        f.insert(f.end(), d, d + n);
        cl_.last_sent = now;
        wire_to(cl_.m_addr, cl_.m_port, std::move(f), out);
        return;
    }
    if (accept_) {
        if (auto it = host_routes_.find(k(to_addr, to_port)); it != host_routes_.end()) {
            std::vector<std::uint8_t> f{kRouteMagic, 'v'};
            put_key(f, to_addr, to_port);
            f.insert(f.end(), d, d + n);
            wire_to(it->second.r.member_addr, it->second.r.member_port, std::move(f), out);
            return;
        }
    }
    wire_to(to_addr, to_port, std::vector<std::uint8_t>(d, d + n), out);
}

bool Router::outgoing_v6(std::uint32_t to_addr, std::uint16_t to_port, const std::uint8_t* d, std::size_t n,
                         std::vector<Wire>* out) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = v6_by_key_.find(k(to_addr, to_port));
    if (!ipv6_ || it == v6_by_key_.end()) return false;
    Wire w;
    w.v6 = true;
    w.addr6 = it->second.e6.addr;
    w.port = it->second.e6.port;
    w.data.assign(d, d + n);
    out->push_back(std::move(w));
    return true;
}

bool Router::incoming(const Wire& from, RClock::time_point now, std::vector<std::uint8_t>* data, std::uint32_t* src_addr,
                      std::uint16_t* src_port, std::vector<Wire>* out, bool* via_v6, bool* via_relay) {
    if (via_v6) *via_v6 = from.v6;
    if (via_relay) *via_relay = false;
    std::uint32_t sa = from.addr;
    std::uint16_t sp = from.port;
    // A V frame asks whether its sender is a member: that callback runs without our lock (it
    // reads the party link, whose IO thread sends through us while holding its own lock).
    std::function<bool(std::uint32_t, std::uint16_t)> member_check;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (from.v6) {
            if (!ipv6_) return false;
            key_of_locked(Ep6{from.addr6, from.port}, &sa, &sp);
        }
        const std::vector<std::uint8_t>& d = from.data;
        if (!is_route_frame(d.data(), d.size())) {
            if (accept_) {
                direct_[k(sa, sp)] = now;
                if (direct_.size() > 4096 || now - direct_pruned_ > std::chrono::seconds(30)) {
                    direct_pruned_ = now;
                    for (auto i = direct_.begin(); i != direct_.end();)
                        i = now - i->second > kDirectGuard ? direct_.erase(i) : std::next(i);
                    if (direct_.size() > 4096) direct_.clear();
                }
            }
            *data = d;
            *src_addr = sa;
            *src_port = sp;
            return true;
        }
        if (d[1] != 'V') {
            *data = d;
            return on_frame(sa, sp, *data, now, src_addr, src_port, out, via_relay);
        }
        if (!accept_ || d.size() < 8) return false;
        member_check = is_member_;
    }
    if (!member_check || !member_check(sa, sp)) return false;
    std::lock_guard<std::mutex> lk(mu_);
    if (!accept_) return false;
    *data = from.data;
    return on_frame(sa, sp, *data, now, src_addr, src_port, out, via_relay);
}

void Router::server_nonce(std::uint32_t a, std::uint16_t p, const std::uint8_t* cn, std::int64_t minute,
                          std::uint8_t out[16]) const {
    std::vector<std::uint8_t> m;
    append(m, kLabelNonce);
    m.insert(m.end(), sv_salt_.begin(), sv_salt_.end());
    put_key(m, a, p);
    m.insert(m.end(), cn, cn + 16);
    put32(m, static_cast<std::uint32_t>(minute));
    sv_mac_(m.data(), m.size(), out);
}

// mu_ held. `d` holds the frame; on true it holds the inner datagram for the layers above.
bool Router::on_frame(std::uint32_t sa, std::uint16_t sp, std::vector<std::uint8_t>& d, RClock::time_point now,
                      std::uint32_t* src_addr, std::uint16_t* src_port, std::vector<Wire>* out, bool* via_relay) {
    const std::size_t n = d.size();
    const std::uint8_t type = d[1];
    if (type == 'Q' && n >= 3 + 16) {
        const std::uint8_t sub = d[2];
        const std::uint8_t* cn = d.data() + 3;
        const std::int64_t minute = std::chrono::duration_cast<std::chrono::minutes>(now.time_since_epoch()).count();
        auto refuse = [&](std::uint8_t code, int wait_s) {
            std::vector<std::uint8_t> f{kRouteMagic, 'Q', 5};
            f.insert(f.end(), cn, cn + 16);
            f.push_back(code);
            const int w = std::clamp(wait_s, 0, 65535);
            f.push_back(static_cast<std::uint8_t>(w >> 8));
            f.push_back(static_cast<std::uint8_t>(w));
            wire_to(sa, sp, std::move(f), out);
        };
        if (sub == 1 && n >= kQ1) {  // a guest's hello at the member
            if (!serving_) {
                refuse(kNotServing, 0);
                return false;
            }
            std::uint8_t sn[16];
            server_nonce(sa, sp, cn, minute, sn);
            std::vector<std::uint8_t> f{kRouteMagic, 'Q', 2};
            f.insert(f.end(), cn, cn + 16);
            f.insert(f.end(), sn, sn + 16);
            wire_to(sa, sp, std::move(f), out);
            return false;
        }
        if (sub == 3 && n == kQ3) {  // its proof
            if (!serving_) {
                refuse(kNotServing, 0);
                return false;
            }
            const std::uint8_t* sn = d.data() + 19;
            std::uint8_t want[16];
            bool fresh = false;
            for (std::int64_t m = minute; m >= minute - 1 && !fresh; --m) {
                server_nonce(sa, sp, cn, m, want);
                fresh = equal16(want, sn);
            }
            if (!fresh) return false;  // stale or not ours: the guest says hello again
            std::int64_t remaining = 0;
            if (sv_throttle_.locked && sv_throttle_.locked(sa, &remaining)) {
                refuse(kLockedOut, static_cast<int>((remaining + 999) / 1000));
                return false;
            }
            std::vector<std::uint8_t> m;
            append(m, kLabelAuth);
            m.insert(m.end(), cn, cn + 16);
            m.insert(m.end(), sn, sn + 16);
            std::uint8_t proof[16];
            sv_mac_(m.data(), m.size(), proof);
            if (!equal16(proof, d.data() + 35)) {
                if (sv_throttle_.fail) sv_throttle_.fail(sa);
                note("member relay: a wrong party key from " + key_text(sa, sp) + " (refused)");
                refuse(kWrongKey, 0);
                return false;
            }
            if (sv_throttle_.success) sv_throttle_.success(sa);
            auto it = sessions_.find(k(sa, sp));
            if (it == sessions_.end() || std::memcmp(it->second.cn.data(), cn, 16) != 0) {
                if (it == sessions_.end()) {
                    for (auto i = sessions_.begin(); i != sessions_.end();)
                        i = now - i->second.last > kSessionIdle ? sessions_.erase(i) : std::next(i);
                    if (sessions_.size() >= kMaxSessions) {
                        refuse(kFull, 0);
                        note("member relay: refused " + key_text(sa, sp) + " (already relaying for " +
                             std::to_string(kMaxSessions) + ")");
                        return false;
                    }
                }
                Session s;
                s.addr = sa;
                s.port = sp;
                while (!s.id) random_fill(reinterpret_cast<std::uint8_t*>(&s.id), 4);
                std::memcpy(s.cn.data(), cn, 16);
                s.frames.rate = kSessionFrameRate;
                s.frames.burst = 2 * kSessionFrameRate;
                s.bytes.rate = kSessionByteRate;
                s.bytes.burst = 2 * kSessionByteRate;
                const bool again = it != sessions_.end();
                sessions_[k(sa, sp)] = s;
                it = sessions_.find(k(sa, sp));
                note("member relay: relaying for a guest at " + key_text(sa, sp) +
                     (again ? " (a new session)" : "") + ": it cannot reach the host directly");
            }
            it->second.last = now;
            std::vector<std::uint8_t> f{kRouteMagic, 'Q', 4};
            f.insert(f.end(), cn, cn + 16);
            put32(f, it->second.id);
            const std::string name = sv_name_.substr(0, 16);
            f.push_back(static_cast<std::uint8_t>(name.size()));
            f.insert(f.end(), name.begin(), name.end());
            std::vector<std::uint8_t> ok;
            append(ok, kLabelOk);
            ok.insert(ok.end(), cn, cn + 16);
            ok.insert(ok.end(), sn, sn + 16);
            put32(ok, it->second.id);
            ok.insert(ok.end(), name.begin(), name.end());
            std::uint8_t tag[16];
            sv_mac_(ok.data(), ok.size(), tag);
            f.insert(f.end(), tag, tag + 16);
            wire_to(sa, sp, std::move(f), out);
            return false;
        }
        // The guest's side: answers from our member only, for our hello.
        if (cl_.st != ClientState::Handshake || sa != cl_.m_addr || sp != cl_.m_port ||
            std::memcmp(cn, cl_.cn.data(), 16) != 0)
            return false;
        if (sub == 2 && n == kQ2) {
            std::memcpy(cl_.sn.data(), d.data() + 19, 16);
            cl_.have_sn = true;
            client_send_auth(now, out);
        } else if (sub == 4 && n >= 3 + 16 + 4 + 1 + 16 && cl_.have_sn) {
            const std::uint32_t session = get32(d.data() + 19);
            const std::size_t nl = d[23];
            if (n != 24 + nl + 16 || nl > 16) return false;
            std::vector<std::uint8_t> ok;
            append(ok, kLabelOk);
            ok.insert(ok.end(), cl_.cn.begin(), cl_.cn.end());
            ok.insert(ok.end(), cl_.sn.begin(), cl_.sn.end());
            put32(ok, session);
            ok.insert(ok.end(), d.begin() + 24, d.begin() + 24 + static_cast<std::ptrdiff_t>(nl));
            std::uint8_t tag[16];
            cl_.mac(ok.data(), ok.size(), tag);
            if (!equal16(tag, d.data() + 24 + nl)) return false;  // not a party member: ignored
            cl_.session = session;
            cl_.name = clean_name(d.data() + 24, nl);
            cl_.st = ClientState::Ready;
            cl_.last_sent = now;
            note("member relay: " + (cl_.name.empty() ? std::string("the member") : cl_.name) + " at " +
                 key_text(cl_.m_addr, cl_.m_port) + " relays for us to the host " + key_text(cl_.h_addr, cl_.h_port));
        } else if (sub == 5 && n == kQ5) {
            cl_.code = d[19];
            cl_.wait_s = d[20] << 8 | d[21];
            cl_.st = ClientState::Refused;
            static const char* why[] = {"", "it has another party key (a different code or password)",
                                        "too many wrong attempts from here", "it does not relay now",
                                        "it relays for others already"};
            note("member relay: the member at " + key_text(cl_.m_addr, cl_.m_port) + " refused: " +
                 (cl_.code >= 1 && cl_.code <= 4 ? why[cl_.code] : "?") +
                 (cl_.wait_s ? " (" + std::to_string(cl_.wait_s) + " s)" : ""));
        }
        return false;
    }
    if (type == 'T' && n >= 6 && serving_) {  // a guest's datagram for the host
        auto it = sessions_.find(k(sa, sp));
        if (it == sessions_.end() || it->second.id != get32(d.data() + 2)) return false;
        Session& s = it->second;
        s.last = now;
        if (n == 6) return false;  // keepalive
        if (is_route_frame(d.data() + 6, n - 6)) return false;  // no relay inside a relay
        if (!s.frames.take(1, now) || !s.bytes.take(static_cast<double>(n), now)) {
            if (s.dropped++ % 1000 == 0)
                note("member relay: " + key_text(sa, sp) + " is over its rate; datagrams dropped");
            return false;
        }
        std::vector<std::uint8_t> f{kRouteMagic, 'V'};
        put_key(f, sa, sp);
        f.insert(f.end(), d.begin() + 6, d.end());
        ++s.up;
        wire_to(sv_h_addr_, sv_h_port_, std::move(f), out);
        return false;
    }
    if (type == 'v' && n >= 8 && serving_) {  // the host's datagram for a guest we relay for
        if (sa != sv_h_addr_ || sp != sv_h_port_) return false;
        std::uint32_t ga;
        std::uint16_t gp;
        get_key(d.data() + 2, &ga, &gp);
        auto it = sessions_.find(k(ga, gp));
        if (it == sessions_.end() || is_route_frame(d.data() + 8, n - 8)) return false;
        std::vector<std::uint8_t> f{kRouteMagic, 't'};
        put32(f, it->second.id);
        f.insert(f.end(), d.begin() + 8, d.end());
        ++it->second.down;
        wire_to(ga, gp, std::move(f), out);
        return false;
    }
    if (type == 't' && n >= 6) {  // the host's datagram, through our member
        if (cl_.st != ClientState::Ready || sa != cl_.m_addr || sp != cl_.m_port || get32(d.data() + 2) != cl_.session)
            return false;
        if (is_route_frame(d.data() + 6, n - 6) || n == 6) return false;
        d.erase(d.begin(), d.begin() + 6);
        *src_addr = cl_.h_addr;
        *src_port = cl_.h_port;
        if (via_relay) *via_relay = true;
        return true;
    }
    if (type == 'V' && n >= 8 && accept_) {  // a member's frame from a guest it relays (checked: a member)
        std::uint32_t ga;
        std::uint16_t gp;
        get_key(d.data() + 2, &ga, &gp);
        if ((ga == sa && gp == sp) || !ga || !gp || is_route_frame(d.data() + 8, n - 8) || n == 8) return false;
        if (auto dir = direct_.find(k(ga, gp)); dir != direct_.end() && now - dir->second < kDirectGuard)
            return false;  // heard directly: a member cannot pose as it
        auto it = host_routes_.find(k(ga, gp));
        if (it == host_routes_.end()) {
            if (host_routes_.size() >= kMaxHostRoutes) {
                auto old = host_routes_.begin();
                for (auto i = host_routes_.begin(); i != host_routes_.end(); ++i)
                    if (i->second.last < old->second.last) old = i;
                if (now - old->second.last < kHostRouteIdle) return false;
                host_routes_.erase(old);
            }
            HostRouteState st;
            st.r = {ga, sa, gp, sp};
            it = host_routes_.emplace(k(ga, gp), st).first;
            note("member relay: a guest at " + key_text(ga, gp) + " reaches us through the member at " +
                 key_text(sa, sp));
        } else if (it->second.r.member_addr != sa || it->second.r.member_port != sp) {
            note("member relay: the guest at " + key_text(ga, gp) + " now comes through the member at " +
                 key_text(sa, sp));
            it->second.r.member_addr = sa;
            it->second.r.member_port = sp;
        }
        it->second.last = now;
        d.erase(d.begin(), d.begin() + 8);
        *src_addr = ga;
        *src_port = gp;
        if (via_relay) *via_relay = true;
        return true;
    }
    return false;
}

void Router::client_send_hello(RClock::time_point now, std::vector<Wire>* out) {
    std::vector<std::uint8_t> f{kRouteMagic, 'Q', 1};
    f.insert(f.end(), cl_.cn.begin(), cl_.cn.end());
    f.resize(kQ1, 0);
    cl_.next_send = now + std::chrono::milliseconds(500);
    wire_to(cl_.m_addr, cl_.m_port, std::move(f), out);
}

void Router::client_send_auth(RClock::time_point now, std::vector<Wire>* out) {
    std::vector<std::uint8_t> m;
    append(m, kLabelAuth);
    m.insert(m.end(), cl_.cn.begin(), cl_.cn.end());
    m.insert(m.end(), cl_.sn.begin(), cl_.sn.end());
    std::uint8_t proof[16];
    cl_.mac(m.data(), m.size(), proof);
    std::vector<std::uint8_t> f{kRouteMagic, 'Q', 3};
    f.insert(f.end(), cl_.cn.begin(), cl_.cn.end());
    f.insert(f.end(), cl_.sn.begin(), cl_.sn.end());
    f.insert(f.end(), proof, proof + 16);
    cl_.next_send = now + std::chrono::milliseconds(500);
    wire_to(cl_.m_addr, cl_.m_port, std::move(f), out);
}

void Router::relay_use(std::uint32_t ma, std::uint16_t mp, std::uint32_t ha, std::uint16_t hp, Mac mac,
                       RClock::time_point now) {
    std::lock_guard<std::mutex> lk(mu_);
    cl_ = Client{};
    cl_.st = ClientState::Handshake;
    cl_.m_addr = ma;
    cl_.m_port = mp;
    cl_.h_addr = ha;
    cl_.h_port = hp;
    cl_.mac = std::move(mac);
    random_fill(cl_.cn.data(), 16);
    cl_.started = now;
    cl_.next_send = now;
    note("member relay: asking the member at " + key_text(ma, mp) + " to relay for us to the host " + key_text(ha, hp));
}

void Router::relay_stop() {
    std::lock_guard<std::mutex> lk(mu_);
    if (cl_.st == ClientState::Off) return;
    note("member relay: no longer through the member at " + key_text(cl_.m_addr, cl_.m_port));
    cl_ = Client{};
}

Router::ClientState Router::relay_state(std::string* name, int* code, int* wait_s) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (name) *name = cl_.name;
    if (code) *code = cl_.code;
    if (wait_s) *wait_s = cl_.wait_s;
    return cl_.st;
}

bool Router::relay_member(std::uint32_t* addr, std::uint16_t* port) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (cl_.st == ClientState::Off) return false;
    *addr = cl_.m_addr;
    *port = cl_.m_port;
    return true;
}

void Router::serve(bool on, std::uint32_t ha, std::uint16_t hp, Mac mac, std::string own_name, Throttle throttle) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!on) {
        if (serving_) note("member relay: not relaying for others any more");
        serving_ = false;
        sessions_.clear();
        return;
    }
    if (!serving_ || ha != sv_h_addr_ || hp != sv_h_port_) {
        sessions_.clear();
        random_fill(sv_salt_.data(), 16);
        note("member relay: ready to relay to the host " + key_text(ha, hp) +
             " for party members that cannot reach it directly");
    }
    serving_ = true;
    sv_h_addr_ = ha;
    sv_h_port_ = hp;
    sv_mac_ = std::move(mac);
    sv_name_ = std::move(own_name);
    sv_throttle_ = std::move(throttle);
}

bool Router::serving() const {
    std::lock_guard<std::mutex> lk(mu_);
    return serving_;
}

std::vector<Router::Served> Router::served() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<Served> v;
    for (const auto& [key, s] : sessions_) {
        (void)key;
        v.push_back({s.addr, s.port, s.up, s.down});
    }
    return v;
}

void Router::accept_relayed(bool on, std::function<bool(std::uint32_t, std::uint16_t)> is_member) {
    std::lock_guard<std::mutex> lk(mu_);
    accept_ = on;
    is_member_ = std::move(is_member);
    if (!on) host_routes_.clear();
}

std::vector<Router::HostRoute> Router::host_routes() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<HostRoute> v;
    for (const auto& [key, r] : host_routes_) {
        (void)key;
        v.push_back(r.r);
    }
    return v;
}

bool Router::relayed_guest(std::uint32_t addr, std::uint16_t port, std::uint32_t* ma, std::uint16_t* mp) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = host_routes_.find(k(addr, port));
    if (it == host_routes_.end()) return false;
    if (ma) *ma = it->second.r.member_addr;
    if (mp) *mp = it->second.r.member_port;
    return true;
}

void Router::poll(RClock::time_point now, std::vector<Wire>* out) {
    std::lock_guard<std::mutex> lk(mu_);
    if (cl_.st == ClientState::Handshake) {
        if (now - cl_.started > kHandshakeTimeout) {
            cl_.st = ClientState::Refused;
            cl_.code = kTimeout;
            note("member relay: no answer from the member at " + key_text(cl_.m_addr, cl_.m_port));
        } else if (now >= cl_.next_send) {
            if (cl_.have_sn) client_send_auth(now, out);
            else client_send_hello(now, out);
        }
    } else if (cl_.st == ClientState::Ready && now - cl_.last_sent > kKeepalive) {
        std::vector<std::uint8_t> f{kRouteMagic, 'T'};
        put32(f, cl_.session);
        cl_.last_sent = now;
        wire_to(cl_.m_addr, cl_.m_port, std::move(f), out);
    }
    for (auto i = sessions_.begin(); i != sessions_.end();) {
        if (now - i->second.last > kSessionIdle) {
            note("member relay: the guest at " + key_text(i->second.addr, i->second.port) + " went quiet (" +
                 std::to_string(i->second.up) + " datagrams to the host, " + std::to_string(i->second.down) +
                 " back): session closed");
            i = sessions_.erase(i);
        } else {
            ++i;
        }
    }
    for (auto i = host_routes_.begin(); i != host_routes_.end();) {
        if (now - i->second.last > kHostRouteIdle) {
            note("member relay: the guest at " + key_text(i->second.r.guest_addr, i->second.r.guest_port) +
                 " is no longer relayed");
            i = host_routes_.erase(i);
        } else {
            ++i;
        }
    }
}

}  // namespace bbnet::udp

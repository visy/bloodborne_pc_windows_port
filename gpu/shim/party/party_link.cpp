// SPDX-License-Identifier: GPL-3.0-or-later
#include "party_link.h"

#include "party_crypto.h"
#include "party_sock.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace party {

namespace {

using Clock = std::chrono::steady_clock;
using crypto::Key;
using crypto::Nonce;

enum MsgType : std::uint8_t {
    kHello = 1,
    kChallenge = 2,
    kAuth = 3,
    kWelcome = 4,
    kReject = 5,
    kPing = 10,
    kPong = 11,
    kRoster = 12,
    kRpcReq = 13,
    kRpcResp = 14,
    kEvent = 15,
    kEventAck = 16,
    kPartyCmd = 17,
    kProgress = 18,
    kBye = 19,
    kEncrypted = 0xE0,
};

constexpr std::uint32_t kMaxFrame = 16u << 20;
constexpr std::uint8_t kMagic[4] = {'B', 'B', 'P', 'L'};

std::int64_t ms_between(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count();
}

struct Writer {
    std::vector<std::uint8_t> b;
    Writer& u8(std::uint8_t v) {
        b.push_back(v);
        return *this;
    }
    Writer& u16(std::uint16_t v) { return u8(static_cast<std::uint8_t>(v)).u8(static_cast<std::uint8_t>(v >> 8)); }
    Writer& u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) u8(static_cast<std::uint8_t>(v >> (8 * i)));
        return *this;
    }
    Writer& u64(std::uint64_t v) {
        for (int i = 0; i < 8; ++i) u8(static_cast<std::uint8_t>(v >> (8 * i)));
        return *this;
    }
    Writer& bytes(const void* p, std::size_t n) {
        auto* c = static_cast<const std::uint8_t*>(p);
        b.insert(b.end(), c, c + n);
        return *this;
    }
    Writer& str16(const std::string& s) {
        std::size_t n = std::min<std::size_t>(s.size(), 0xFFFF);
        u16(static_cast<std::uint16_t>(n));
        return bytes(s.data(), n);
    }
    Writer& str32(const std::string& s) {
        u32(static_cast<std::uint32_t>(s.size()));
        return bytes(s.data(), s.size());
    }
};

struct Reader {
    const std::uint8_t* p;
    std::size_t n;
    bool ok = true;
    Reader(const std::uint8_t* d, std::size_t len) : p(d), n(len) {}
    bool need(std::size_t k) {
        if (!ok || n < k) ok = false;
        return ok;
    }
    std::uint8_t u8() {
        if (!need(1)) return 0;
        std::uint8_t v = *p;
        ++p;
        --n;
        return v;
    }
    std::uint16_t u16() {
        std::uint16_t lo = u8();
        return static_cast<std::uint16_t>(lo | (u8() << 8));
    }
    std::uint32_t u32() {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(u8()) << (8 * i);
        return v;
    }
    std::uint64_t u64() {
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(u8()) << (8 * i);
        return v;
    }
    void bytes(void* out, std::size_t k) {
        if (!need(k)) {
            std::memset(out, 0, k);
            return;
        }
        std::memcpy(out, p, k);
        p += k;
        n -= k;
    }
    std::string str(std::size_t len) {
        if (!need(len)) return {};
        std::string s(reinterpret_cast<const char*>(p), len);
        p += len;
        n -= len;
        return s;
    }
    std::string str16() { return str(u16()); }
    std::string str32() { return str(u32()); }
};

void write_roster(Writer& w, const std::vector<RosterEntry>& r) {
    w.u8(static_cast<std::uint8_t>(r.size()));
    for (const auto& e : r) {
        w.u8(static_cast<std::uint8_t>(e.slot)).str16(e.name).u8(static_cast<std::uint8_t>(e.state));
        w.u8(e.connected ? 1 : 0).u32(e.map_id).u32(e.ping_ms);
    }
}

std::vector<RosterEntry> read_roster(Reader& r) {
    std::vector<RosterEntry> out;
    unsigned n = r.u8();
    for (unsigned i = 0; i < n && r.ok; ++i) {
        RosterEntry e;
        e.slot = r.u8();
        e.name = r.str16();
        std::uint8_t st = r.u8();
        e.state = st <= 6 ? static_cast<MemberState>(st) : MemberState::Title;
        e.connected = r.u8() != 0;
        e.map_id = r.u32();
        e.ping_ms = r.u32();
        out.push_back(std::move(e));
    }
    return out;
}

using Token = std::array<std::uint8_t, 16>;

bool is_zero(const Token& t) {
    for (auto b : t)
        if (b) return false;
    return true;
}

// Reliable events in one direction: the sender's unacked queue and the receiver's last cursor.
struct EventStream {
    struct Ev {
        std::uint64_t cursor;
        std::string name, json;
    };
    std::deque<Ev> unacked;
    std::uint64_t next_cursor = 1;
    std::uint64_t rx_last = 0;
    void reset() {
        unacked.clear();
        next_cursor = 1;
        rx_last = 0;
    }
};

}  // namespace

const char* member_state_name(MemberState s) {
    switch (s) {
    case MemberState::Title: return "title";
    case MemberState::Home: return "home";
    case MemberState::Joining: return "joining";
    case MemberState::InHostWorld: return "in_host_world";
    case MemberState::Dead: return "dead";
    case MemberState::Loading: return "loading";
    case MemberState::Prologue: return "prologue";
    }
    return "?";
}

const char* link_state_name(LinkState s) {
    switch (s) {
    case LinkState::Idle: return "idle";
    case LinkState::Hosting: return "hosting";
    case LinkState::Connecting: return "connecting";
    case LinkState::Connected: return "connected";
    case LinkState::Reconnecting: return "reconnecting";
    case LinkState::Rejected: return "rejected";
    case LinkState::Stopped: return "stopped";
    }
    return "?";
}

bool valid_member_name(const std::string& name) {
    if (name.empty() || name.size() > 16) return false;
    for (char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return false;
    return true;
}

void apply_link_env(LinkConfig& cfg) {
    if (const char* v = std::getenv("BB_PARTY_PORT"); v && *v) {
        long p = std::strtol(v, nullptr, 10);
        if (p > 0 && p <= 65535) cfg.port = static_cast<std::uint16_t>(p);
    }
    if (const char* v = std::getenv("BB_PARTY_MAX"); v && *v) {
        long m = std::strtol(v, nullptr, 10);
        cfg.max_players = static_cast<int>(std::clamp<long>(m, 2, 4));
    }
    if (const char* v = std::getenv("BB_PARTY_PASSWORD"); v && *v) cfg.password = v;
}

struct PartyLink::Impl {
    enum class Phase { Connecting, WaitHello, WaitAuth, WaitChallenge, WaitWelcome, Open };

    struct Conn {
        std::uint64_t id = 0;
        sock::Socket s = sock::kInvalid;
        Phase phase = Phase::WaitHello;
        std::vector<std::uint8_t> in, out;
        std::size_t out_off = 0;
        crypto::Aead tx, rx;
        Nonce hn{}, gn{};
        Key pending_tx{}, pending_rx{};  // guest: keys derived at AUTH, armed at WELCOME
        Clock::time_point created, last_rx, last_ping;
        bool dead = false;
        bool close_after_flush = false;
        int slot = -1;  // host side, after WELCOME
        std::array<std::uint8_t, 4> peer_ip{};
        std::uint16_t peer_port = 0;
        // Host: from HELLO.
        std::string name;
        Token token{};
    };

    struct Member {
        int slot = 0;
        std::string name;
        Token token{};
        Conn* conn = nullptr;
        Clock::time_point lost_at;
        MemberState state = MemberState::Joining;
        std::uint32_t map_id = 0;
        std::uint32_t ping_ms = 0;
        EventStream ev;
    };

    struct Pending {
        Clock::time_point deadline;
        bool sync = false;
        bool done = false;
        bool ok = false;
        std::string reply;
        std::function<void(bool, const std::string&)> cb;
    };

    LinkConfig cfg;
    LinkCallbacks cb;
    Key party_key{};
    bool host = false;

    mutable std::mutex mu;
    mutable std::condition_variable state_cv;
    LinkState state = LinkState::Idle;
    bool stopping = false;
    std::thread io_thread;
    Clock::time_point start_time = Clock::now();
    Clock::time_point freeze_until{};
    std::uint64_t next_conn_id = 1;
    std::vector<std::unique_ptr<Conn>> conns;

    // Local roster entry.
    MemberState local_state = MemberState::Title;
    std::uint32_t local_map = 0;

    // Host.
    sock::Socket listen_s = sock::kInvalid;
    std::uint16_t port = 0;
    std::map<int, Member> members;
    Clock::time_point last_roster_bcast{};

    // Guest.
    std::string host_name;
    std::uint16_t host_port = 0;
    Conn* gconn = nullptr;
    int my_slot = -1;
    int host_max_players = 0;
    Token my_token{};
    EventStream gev;
    std::vector<RosterEntry> groster;
    std::map<std::uint32_t, Pending> pending;
    std::uint32_t next_rpc = 1;
    int backoff_ms = 0;
    Clock::time_point next_attempt{};
    std::int64_t clock_offset = 0;
    std::uint32_t rtt = 0;
    std::string observed;
    RejectCode reject = RejectCode::None;
    std::string reject_text;

    // Callback thread.
    std::mutex dmu;
    std::condition_variable dcv;
    std::deque<std::function<void()>> dq;
    bool dstop = false;
    std::thread dthread;

    Impl(LinkConfig c, LinkCallbacks k) : cfg(std::move(c)), cb(std::move(k)) {
        cfg.max_players = std::clamp(cfg.max_players, 2, 4);
    }

    // ---------- callback thread ----------
    void post(std::function<void()> f) {
        {
            std::lock_guard<std::mutex> lk(dmu);
            dq.push_back(std::move(f));
        }
        dcv.notify_one();
    }
    void dispatch_loop() {
        for (;;) {
            std::function<void()> f;
            {
                std::unique_lock<std::mutex> lk(dmu);
                dcv.wait(lk, [&] { return dstop || !dq.empty(); });
                if (dq.empty()) return;
                f = std::move(dq.front());
                dq.pop_front();
            }
            f();
        }
    }
    void log(std::string line) {
        if (cb.on_log) post([this, line = std::move(line)] { cb.on_log(line); });
    }
    void set_state(LinkState s, std::string detail) {  // mu held
        if (state == s) return;
        state = s;
        state_cv.notify_all();
        log(std::string("party link: ") + link_state_name(s) + (detail.empty() ? "" : ": " + detail));
        if (cb.on_state) post([this, s, detail = std::move(detail)] { cb.on_state(s, detail); });
    }
    std::int64_t now_ms() const { return ms_between(start_time, Clock::now()); }

    // ---------- framing ----------
    void queue_frame(Conn* c, std::uint8_t type, const std::vector<std::uint8_t>& body) {
        if (c->dead) return;
        if (c->tx.ready()) {
            std::uint32_t len = static_cast<std::uint32_t>(1 + crypto::kMacSize + 1 + body.size());
            std::uint8_t hdr[5] = {static_cast<std::uint8_t>(len), static_cast<std::uint8_t>(len >> 8),
                                   static_cast<std::uint8_t>(len >> 16), static_cast<std::uint8_t>(len >> 24),
                                   kEncrypted};
            std::vector<std::uint8_t> pt;
            pt.reserve(1 + body.size());
            pt.push_back(type);
            pt.insert(pt.end(), body.begin(), body.end());
            std::size_t base = c->out.size();
            c->out.resize(base + 5 + crypto::kMacSize + pt.size());
            std::memcpy(c->out.data() + base, hdr, 5);
            c->tx.seal(c->out.data() + base + 5 + crypto::kMacSize, c->out.data() + base + 5, hdr, 5, pt.data(),
                       pt.size());
            crypto::wipe(pt.data(), pt.size());
        } else {
            std::uint32_t len = static_cast<std::uint32_t>(1 + body.size());
            Writer w;
            w.u32(len).u8(type).bytes(body.data(), body.size());
            c->out.insert(c->out.end(), w.b.begin(), w.b.end());
        }
        flush(c);
    }
    void send(Conn* c, std::uint8_t type, const Writer& w) { queue_frame(c, type, w.b); }

    void flush(Conn* c) {
        if (c->dead || c->phase == Phase::Connecting) return;
        if (freeze_until > Clock::now()) return;
        while (c->out_off < c->out.size()) {
            long r = sock::send_some(c->s, c->out.data() + c->out_off, c->out.size() - c->out_off);
            if (r < 0) {
                if (sock::would_block(sock::last_error())) break;
                lose(c, "send failed");
                return;
            }
            c->out_off += static_cast<std::size_t>(r);
        }
        if (c->out_off == c->out.size()) {
            c->out.clear();
            c->out_off = 0;
            if (c->close_after_flush) close_conn(c);
        } else if (c->out_off > (1u << 20)) {
            c->out.erase(c->out.begin(), c->out.begin() + static_cast<std::ptrdiff_t>(c->out_off));
            c->out_off = 0;
        }
    }

    void close_conn(Conn* c) {
        if (c->dead) return;
        c->dead = true;
        if (c->s != sock::kInvalid) {
            sock::close(c->s);
            c->s = sock::kInvalid;
        }
        c->tx.reset();
        c->rx.reset();
    }

    void reject_conn(Conn* c, RejectCode code, const std::string& reason) {
        log("party link: rejecting " + (c->name.empty() ? std::string("connection") : c->name) + ": " + reason);
        Writer w;
        w.u8(static_cast<std::uint8_t>(code)).str16(reason);
        c->tx.reset();  // REJECT is always plaintext
        send(c, kReject, w);
        c->close_after_flush = true;
        c->phase = Phase::WaitHello;
        flush(c);
    }

    // A connection is gone (EOF, error, silence). mu held.
    void lose(Conn* c, const std::string& why) {
        if (c->dead) return;
        bool was_open = c->phase == Phase::Open;
        close_conn(c);
        if (host) {
            if (c->slot >= 0) {
                auto it = members.find(c->slot);
                if (it != members.end() && it->second.conn == c) {
                    Member& m = it->second;
                    m.conn = nullptr;
                    m.lost_at = Clock::now();
                    log("party link: lost " + m.name + " (slot " + std::to_string(m.slot) + "): " + why +
                        "; keeping the slot " + std::to_string(cfg.slot_keep_ms / 1000) + " s");
                    RosterEntry e = entry_of(m);
                    if (cb.on_member_left) post([this, e] { cb.on_member_left(e, true); });
                    broadcast_roster();
                }
            }
            return;
        }
        if (c != gconn) return;
        gconn = nullptr;
        fail_pending("connection to the host lost");
        if (stopping || state == LinkState::Rejected || state == LinkState::Stopped) return;
        if (was_open || backoff_ms == 0) backoff_ms = cfg.backoff_initial_ms;
        next_attempt = Clock::now() + std::chrono::milliseconds(backoff_ms);
        set_state(LinkState::Reconnecting, why + "; retrying in " + std::to_string(backoff_ms) + " ms");
        backoff_ms = std::min(backoff_ms * 2, cfg.backoff_max_ms);
    }

    void fail_pending(const std::string& why) {
        for (auto it = pending.begin(); it != pending.end();) {
            Pending& p = it->second;
            if (p.sync) {
                p.done = true;
                p.ok = false;
                p.reply = why;
                ++it;
            } else {
                auto f = std::move(p.cb);
                if (f) post([f, why] { f(false, why); });
                it = pending.erase(it);
            }
        }
        state_cv.notify_all();
    }

    // ---------- roster ----------
    RosterEntry entry_of(const Member& m) const {
        RosterEntry e;
        e.slot = m.slot;
        e.name = m.name;
        e.state = m.state;
        e.connected = m.conn != nullptr;
        e.map_id = m.map_id;
        e.ping_ms = m.ping_ms;
        return e;
    }
    std::vector<RosterEntry> host_roster() const {
        std::vector<RosterEntry> r;
        RosterEntry me;
        me.slot = kHostSlot;
        me.name = cfg.name;
        me.state = local_state;
        me.connected = true;
        me.map_id = local_map;
        r.push_back(me);
        for (const auto& [slot, m] : members) r.push_back(entry_of(m));
        return r;
    }
    void broadcast_roster() {  // host, mu held
        last_roster_bcast = Clock::now();
        auto r = host_roster();
        Writer w;
        write_roster(w, r);
        for (auto& [slot, m] : members)
            if (m.conn && m.conn->phase == Phase::Open) send(m.conn, kRoster, w);
        if (cb.on_roster) post([this, r] { cb.on_roster(r); });
    }

    // ---------- events ----------
    void send_event_frame(Conn* c, const EventStream::Ev& e) {
        Writer w;
        w.u64(e.cursor).str16(e.name).str32(e.json);
        send(c, kEvent, w);
    }
    void on_event_frame(Conn* c, EventStream& es, int from_slot, Reader& r) {
        std::uint64_t cursor = r.u64();
        std::string name = r.str16();
        std::string json = r.str32();
        if (!r.ok) return;
        if (cursor > es.rx_last) {
            es.rx_last = cursor;
            if (cb.on_event)
                post([this, from_slot, cursor, name = std::move(name), json = std::move(json)] {
                    cb.on_event(from_slot, cursor, name, json);
                });
        }
        Writer w;
        w.u64(cursor);
        send(c, kEventAck, w);
    }
    static void on_event_ack(EventStream& es, Reader& r) {
        std::uint64_t cursor = r.u64();
        while (!es.unacked.empty() && es.unacked.front().cursor <= cursor) es.unacked.pop_front();
    }

    // ---------- frame processing ----------
    void process_input(Conn* c) {
        std::size_t off = 0;
        while (!c->dead && c->in.size() - off >= 5) {
            const std::uint8_t* h = c->in.data() + off;
            std::uint32_t len = static_cast<std::uint32_t>(h[0]) | (static_cast<std::uint32_t>(h[1]) << 8) |
                                (static_cast<std::uint32_t>(h[2]) << 16) | (static_cast<std::uint32_t>(h[3]) << 24);
            if (len == 0 || len > kMaxFrame) {
                lose(c, "bad frame length");
                return;
            }
            if (c->in.size() - off < 4 + static_cast<std::size_t>(len)) break;
            std::uint8_t type = h[4];
            const std::uint8_t* body = h + 5;
            std::size_t body_len = len - 1;
            if (type == kEncrypted) {
                if (!c->rx.ready() || body_len < crypto::kMacSize + 1) {
                    lose(c, "unexpected encrypted frame");
                    return;
                }
                std::vector<std::uint8_t> pt(body_len - crypto::kMacSize);
                if (!c->rx.open(pt.data(), body, h, 5, body + crypto::kMacSize, pt.size())) {
                    lose(c, "frame failed authentication");
                    return;
                }
                handle(c, pt[0], pt.data() + 1, pt.size() - 1, true);
            } else {
                handle(c, type, body, body_len, false);
            }
            off += 4 + len;
        }
        if (!c->dead && off) c->in.erase(c->in.begin(), c->in.begin() + static_cast<std::ptrdiff_t>(off));
    }

    void handle(Conn* c, std::uint8_t type, const std::uint8_t* d, std::size_t n, bool enc) {
        if (c->close_after_flush) return;  // rejected / saying BYE: ignore the rest
        c->last_rx = Clock::now();
        Reader r(d, n);
        if (host)
            handle_host(c, type, r, enc);
        else
            handle_guest(c, type, r, enc);
    }

    void handle_common(Conn* c, std::uint8_t type, Reader& r) {
        if (type == kPing) {
            std::uint64_t t = r.u64();
            Writer w;
            w.u64(t).u64(static_cast<std::uint64_t>(host ? now_ms() : 0));
            send(c, kPong, w);
        }
    }

    // ---------- host ----------
    void handle_host(Conn* c, std::uint8_t type, Reader& r, bool enc) {
        if (c->phase == Phase::WaitHello) {
            if (type != kHello || enc) return lose(c, "expected HELLO");
            std::uint8_t magic[4];
            r.bytes(magic, 4);
            std::uint16_t ver = r.u16();
            c->name = r.str16();
            std::uint8_t eboot[32], mods[32];
            r.bytes(eboot, 32);
            r.bytes(mods, 32);
            r.bytes(c->token.data(), 16);
            if (!r.ok || std::memcmp(magic, kMagic, 4) != 0) return lose(c, "not a PartyLink client");
            if (ver != kLinkProtocolVersion)
                return reject_conn(c, RejectCode::Version,
                                   "different party protocol version (host " + std::to_string(kLinkProtocolVersion) +
                                       ", yours " + std::to_string(ver) + "): update the port");
            if (std::memcmp(eboot, cfg.eboot_sha256.data(), 32) != 0 || std::memcmp(mods, cfg.mods_hash.data(), 32) != 0)
                return reject_conn(c, RejectCode::Mismatch, "different game version/patches/mods");
            if (!valid_member_name(c->name))
                return reject_conn(c, RejectCode::Name, "invalid name (1-16 of A-Z a-z 0-9 _ -)");
            crypto::random_bytes(c->hn.data(), c->hn.size());
            Writer w;
            w.bytes(c->hn.data(), c->hn.size());
            send(c, kChallenge, w);
            c->phase = Phase::WaitAuth;
            return;
        }
        if (c->phase == Phase::WaitAuth) {
            if (type != kAuth || enc) return lose(c, "expected AUTH");
            std::uint8_t proof[32];
            r.bytes(c->gn.data(), c->gn.size());
            r.bytes(proof, 32);
            if (!r.ok) return lose(c, "short AUTH");
            Key expect = crypto::auth_proof(party_key, c->hn, c->gn);
            if (!crypto::equal32(expect.data(), proof))
                return reject_conn(c, RejectCode::Auth, "wrong password or party code");
            admit(c);
            return;
        }
        if (!enc) return lose(c, "plaintext frame after WELCOME");
        auto it = members.find(c->slot);
        if (it == members.end() || it->second.conn != c) return lose(c, "stale connection");
        Member& m = it->second;
        switch (type) {
        case kPing: handle_common(c, type, r); break;
        case kPong: {
            std::uint64_t t = r.u64();
            std::int64_t rtt_ms = now_ms() - static_cast<std::int64_t>(t);
            if (r.ok && rtt_ms >= 0) m.ping_ms = static_cast<std::uint32_t>(rtt_ms);
            break;
        }
        case kRoster: {  // the guest's own entry
            auto entries = read_roster(r);
            if (!r.ok || entries.empty()) break;
            if (entries[0].state != m.state || entries[0].map_id != m.map_id) {
                m.state = entries[0].state;
                m.map_id = entries[0].map_id;
                broadcast_roster();
            }
            break;
        }
        case kRpcReq: {
            std::uint32_t id = r.u32();
            std::string kind = r.str16();
            std::string json = r.str32();
            if (!r.ok) break;
            int slot = m.slot;
            std::uint64_t conn_id = c->id;
            post([this, slot, conn_id, id, kind = std::move(kind), json = std::move(json)] {
                bool ok = static_cast<bool>(cb.on_rpc);
                std::string reply = ok ? cb.on_rpc(slot, kind, json) : std::string("{\"error\":\"no rpc handler\"}");
                std::lock_guard<std::mutex> lk(mu);
                auto mit = members.find(slot);
                if (mit == members.end() || !mit->second.conn || mit->second.conn->id != conn_id) return;
                Writer w;
                w.u32(id).u8(ok ? 1 : 0).str32(reply);
                send(mit->second.conn, kRpcResp, w);
            });
            break;
        }
        case kEvent: on_event_frame(c, m.ev, m.slot, r); break;
        case kEventAck: on_event_ack(m.ev, r); break;
        case kPartyCmd: {
            std::string cmd = r.str16();
            std::string json = r.str32();
            int slot = m.slot;
            if (r.ok && cb.on_party_cmd)
                post([this, slot, cmd = std::move(cmd), json = std::move(json)] { cb.on_party_cmd(slot, cmd, json); });
            break;
        }
        case kProgress: {
            std::string blob = r.str32();
            int slot = m.slot;
            if (r.ok && cb.on_progress)
                post([this, slot, b = std::vector<std::uint8_t>(blob.begin(), blob.end())] { cb.on_progress(slot, b); });
            break;
        }
        case kBye: {
            log("party link: " + m.name + " left the party");
            release_member(m.slot, false);
            break;
        }
        default: break;
        }
    }

    void admit(Conn* c) {
        // Resume: the same token (or, for a restarted guest, the same name) on a kept slot.
        Member* m = nullptr;
        bool resumed = false;
        for (auto& [slot, mm] : members) {
            if (!is_zero(c->token) && mm.token == c->token) {
                m = &mm;
                break;
            }
        }
        // By name also while the old connection is not yet declared lost, when it comes from the
        // same address and has been silent for two ping intervals (a crashed guest restarted
        // before lost_timeout); a live player of the same name still gets RejectCode::Name.
        const auto silent = std::chrono::milliseconds(2 * cfg.ping_interval_ms);
        if (!m)
            for (auto& [slot, mm] : members)
                if (mm.name == c->name &&
                    (!mm.conn || (mm.conn->peer_ip == c->peer_ip && Clock::now() - mm.conn->last_rx > silent))) {
                    m = &mm;
                    break;
                }
        if (m) {
            if (m->conn) {  // the old connection has not timed out yet: replace it
                Conn* old = m->conn;
                m->conn = nullptr;
                close_conn(old);
            }
            resumed = !is_zero(c->token) && m->token == c->token;
            if (!resumed) m->ev.reset();
        } else {
            for (auto& [slot, mm] : members)
                if (mm.name == c->name)
                    return reject_conn(c, RejectCode::Name, "the name " + c->name + " is already in the party");
            if (c->name == cfg.name)
                return reject_conn(c, RejectCode::Name, "the name " + c->name + " is already in the party");
            int slot = -1;
            for (int s = 1; s < cfg.max_players; ++s)
                if (!members.count(s)) {
                    slot = s;
                    break;
                }
            if (slot < 0)
                return reject_conn(c, RejectCode::Full,
                                   "the party is full (" + std::to_string(cfg.max_players) + " players)");
            Member nm;
            nm.slot = slot;
            nm.name = c->name;
            crypto::random_bytes(nm.token.data(), nm.token.size());
            m = &members.emplace(slot, std::move(nm)).first->second;
        }
        bool rejoined = m->conn == nullptr && (resumed || m->lost_at != Clock::time_point{});
        m->conn = c;
        m->state = MemberState::Joining;
        c->slot = m->slot;
        c->tx.init(crypto::session_key(party_key, "bbp-h2g", c->hn, c->gn));
        c->rx.init(crypto::session_key(party_key, "bbp-g2h", c->hn, c->gn));
        c->phase = Phase::Open;
        c->last_ping = Clock::now();

        Writer w;
        w.u8(static_cast<std::uint8_t>(m->slot)).u8(static_cast<std::uint8_t>(cfg.max_players)).u8(resumed ? 1 : 0);
        w.u64(static_cast<std::uint64_t>(now_ms()));
        w.bytes(c->peer_ip.data(), 4).u16(c->peer_port);
        w.bytes(m->token.data(), 16);
        write_roster(w, host_roster());
        send(c, kWelcome, w);
        for (const auto& e : m->ev.unacked) send_event_frame(c, e);
        log("party link: " + m->name + (rejoined ? " rejoined" : " joined") + " (slot " + std::to_string(m->slot) +
            ", from " + format_peer(c) + ")");
        RosterEntry e = entry_of(*m);
        if (cb.on_member_joined) post([this, e, rejoined] { cb.on_member_joined(e, rejoined); });
        broadcast_roster();
    }

    void release_member(int slot, bool notify_bye, RejectCode code = RejectCode::Kicked, const std::string& why = {}) {
        auto it = members.find(slot);
        if (it == members.end()) return;
        Member& m = it->second;
        if (m.conn) {
            if (notify_bye) {
                Writer w;
                w.u8(static_cast<std::uint8_t>(code)).str16(why);
                send(m.conn, kBye, w);
                m.conn->close_after_flush = true;
                flush(m.conn);
            } else {
                close_conn(m.conn);
            }
            m.conn->slot = -1;
        }
        RosterEntry e = entry_of(m);
        e.connected = false;
        members.erase(it);
        if (cb.on_member_left) post([this, e] { cb.on_member_left(e, false); });
        broadcast_roster();
    }

    static std::string format_peer(const Conn* c) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%u.%u.%u.%u:%u", c->peer_ip[0], c->peer_ip[1], c->peer_ip[2], c->peer_ip[3],
                      c->peer_port);
        return buf;
    }

    // ---------- guest ----------
    void handle_guest(Conn* c, std::uint8_t type, Reader& r, bool enc) {
        if (type == kReject && !enc) {
            RejectCode code = static_cast<RejectCode>(r.u8());
            std::string why = r.str16();
            reject = code;
            reject_text = why;
            close_conn(c);
            gconn = nullptr;
            fail_pending("rejected: " + why);
            set_state(LinkState::Rejected, why);
            return;
        }
        if (c->phase == Phase::WaitChallenge) {
            if (type != kChallenge || enc) return lose(c, "expected CHALLENGE");
            r.bytes(c->hn.data(), c->hn.size());
            if (!r.ok) return lose(c, "short CHALLENGE");
            crypto::random_bytes(c->gn.data(), c->gn.size());
            Key proof = crypto::auth_proof(party_key, c->hn, c->gn);
            Writer w;
            w.bytes(c->gn.data(), c->gn.size()).bytes(proof.data(), proof.size());
            send(c, kAuth, w);
            // The host answers with the first encrypted frame: arm the receive key now.
            c->rx.init(crypto::session_key(party_key, "bbp-h2g", c->hn, c->gn));
            c->pending_tx = crypto::session_key(party_key, "bbp-g2h", c->hn, c->gn);
            c->phase = Phase::WaitWelcome;
            return;
        }
        if (!enc) return lose(c, "unexpected plaintext frame");
        if (c->phase == Phase::WaitWelcome) {
            if (type != kWelcome) return lose(c, "expected WELCOME");
            int slot = r.u8();
            int maxp = r.u8();
            bool resumed = r.u8() != 0;
            std::uint64_t hclock = r.u64();
            std::array<std::uint8_t, 4> ip{};
            r.bytes(ip.data(), 4);
            std::uint16_t oport = r.u16();
            Token tok{};
            r.bytes(tok.data(), 16);
            auto ros = read_roster(r);
            if (!r.ok) return lose(c, "short WELCOME");
            c->tx.init(c->pending_tx);
            crypto::wipe(c->pending_tx.data(), 32);
            c->phase = Phase::Open;
            c->last_ping = Clock::now();
            my_slot = slot;
            host_max_players = maxp;
            my_token = tok;
            clock_offset = static_cast<std::int64_t>(hclock) - now_ms();
            char buf[32];
            std::snprintf(buf, sizeof buf, "%u.%u.%u.%u:%u", ip[0], ip[1], ip[2], ip[3], oport);
            observed = buf;
            if (!resumed) gev.reset();
            backoff_ms = 0;
            groster = ros;
            // Our own state, then any events the host has not acknowledged.
            send_local_state(c);
            for (const auto& e : gev.unacked) send_event_frame(c, e);
            set_state(LinkState::Connected, "slot " + std::to_string(slot) + (resumed ? " (resumed)" : ""));
            if (cb.on_roster) post([this, ros] { cb.on_roster(ros); });
            return;
        }
        switch (type) {
        case kPing: handle_common(c, type, r); break;
        case kPong: {
            std::uint64_t t = r.u64();
            std::uint64_t hclock = r.u64();
            if (!r.ok) break;
            std::int64_t now = now_ms();
            std::int64_t rt = now - static_cast<std::int64_t>(t);
            if (rt >= 0) {
                rtt = static_cast<std::uint32_t>(rt);
                clock_offset = static_cast<std::int64_t>(hclock) + rt / 2 - now;
            }
            break;
        }
        case kRoster: {
            auto ros = read_roster(r);
            if (!r.ok) break;
            groster = ros;
            if (cb.on_roster) post([this, ros] { cb.on_roster(ros); });
            break;
        }
        case kRpcResp: {
            std::uint32_t id = r.u32();
            bool ok = r.u8() != 0;
            std::string reply = r.str32();
            if (!r.ok) break;
            auto it = pending.find(id);
            if (it == pending.end()) break;
            if (it->second.sync) {
                it->second.done = true;
                it->second.ok = ok;
                it->second.reply = std::move(reply);
                state_cv.notify_all();
            } else {
                auto f = std::move(it->second.cb);
                pending.erase(it);
                if (f) post([f, ok, reply = std::move(reply)] { f(ok, reply); });
            }
            break;
        }
        case kEvent: on_event_frame(c, gev, kHostSlot, r); break;
        case kEventAck: on_event_ack(gev, r); break;
        case kPartyCmd: {
            std::string cmd = r.str16();
            std::string json = r.str32();
            if (r.ok && cb.on_party_cmd)
                post([this, cmd = std::move(cmd), json = std::move(json)] { cb.on_party_cmd(kHostSlot, cmd, json); });
            break;
        }
        case kProgress: {
            std::string blob = r.str32();
            if (r.ok && cb.on_progress)
                post([this, b = std::vector<std::uint8_t>(blob.begin(), blob.end())] { cb.on_progress(kHostSlot, b); });
            break;
        }
        case kBye: {
            RejectCode code = static_cast<RejectCode>(r.u8());
            std::string why = r.str16();
            reject = code == RejectCode::None ? RejectCode::Shutdown : code;
            reject_text = why.empty() ? "the host ended the party" : why;
            close_conn(c);
            gconn = nullptr;
            fail_pending(reject_text);
            set_state(LinkState::Rejected, reject_text);
            break;
        }
        default: break;
        }
    }

    void send_local_state(Conn* c) {
        RosterEntry me;
        me.slot = my_slot;
        me.name = cfg.name;
        me.state = local_state;
        me.connected = true;
        me.map_id = local_map;
        Writer w;
        write_roster(w, {me});
        send(c, kRoster, w);
    }

    void start_connect() {  // guest, mu held
        std::array<std::uint8_t, 4> ip{};
        bool resolved = false;
        {
            addrinfo hints{};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            addrinfo* res = nullptr;
            if (getaddrinfo(host_name.c_str(), nullptr, &hints, &res) == 0 && res) {
                for (addrinfo* a = res; a; a = a->ai_next)
                    if (a->ai_family == AF_INET) {
                        std::memcpy(ip.data(), &reinterpret_cast<sockaddr_in*>(a->ai_addr)->sin_addr, 4);
                        resolved = true;
                        break;
                    }
                freeaddrinfo(res);
            }
        }
        auto c = std::make_unique<Conn>();
        c->id = next_conn_id++;
        c->created = c->last_rx = Clock::now();
        c->phase = Phase::Connecting;
        Conn* raw = c.get();
        conns.push_back(std::move(c));
        gconn = raw;
        set_state(LinkState::Connecting, host_name + ":" + std::to_string(host_port));
        if (!resolved) return lose(raw, "cannot resolve " + host_name);
        raw->s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (raw->s == sock::kInvalid) return lose(raw, "socket() failed");
        sock::set_nonblocking(raw->s);
        int one = 1;
        setsockopt(raw->s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(host_port);
        std::memcpy(&a.sin_addr, ip.data(), 4);
        raw->peer_ip = ip;
        raw->peer_port = host_port;
        int rc = ::connect(raw->s, reinterpret_cast<sockaddr*>(&a), sizeof a);
        if (rc != 0 && !sock::would_block(sock::last_error())) return lose(raw, "connect failed");
        if (rc == 0) on_connected(raw);
    }

    void on_connected(Conn* c) {
        c->phase = Phase::WaitChallenge;
        c->last_rx = Clock::now();
        Writer w;
        w.bytes(kMagic, 4).u16(kLinkProtocolVersion).str16(cfg.name);
        w.bytes(cfg.eboot_sha256.data(), 32).bytes(cfg.mods_hash.data(), 32).bytes(my_token.data(), 16);
        send(c, kHello, w);
    }

    // ---------- IO loop ----------
    void accept_all() {
        for (;;) {
            sockaddr_in a{};
            sock::socklen al = sizeof a;
            sock::Socket s = ::accept(listen_s, reinterpret_cast<sockaddr*>(&a), &al);
            if (s == sock::kInvalid) return;
            sock::set_nonblocking(s);
            int one = 1;
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
            auto c = std::make_unique<Conn>();
            c->id = next_conn_id++;
            c->s = s;
            c->phase = Phase::WaitHello;
            c->created = c->last_rx = Clock::now();
            std::memcpy(c->peer_ip.data(), &a.sin_addr, 4);
            c->peer_port = ntohs(a.sin_port);
            conns.push_back(std::move(c));
        }
    }

    void read_conn(Conn* c) {
        std::uint8_t buf[65536];
        for (;;) {
            long r = sock::recv_some(c->s, buf, sizeof buf);
            if (r > 0) {
                c->in.insert(c->in.end(), buf, buf + r);
                if (r < static_cast<long>(sizeof buf)) break;
                continue;
            }
            if (r == 0) return lose(c, "connection closed by peer");
            if (sock::would_block(sock::last_error())) break;
            return lose(c, "connection reset");
        }
        process_input(c);
    }

    void timers() {
        auto now = Clock::now();
        for (auto& cp : conns) {
            Conn* c = cp.get();
            if (c->dead) continue;
            if (c->phase == Phase::Connecting) {
                if (ms_between(c->created, now) > cfg.connect_timeout_ms) lose(c, "connect timed out");
                continue;
            }
            if (ms_between(c->last_rx, now) > cfg.lost_timeout_ms) {
                lose(c, "no traffic for " + std::to_string(cfg.lost_timeout_ms / 1000) + " s");
                continue;
            }
            if (c->phase == Phase::Open && ms_between(c->last_ping, now) >= cfg.ping_interval_ms) {
                c->last_ping = now;
                Writer w;
                w.u64(static_cast<std::uint64_t>(now_ms()));
                send(c, kPing, w);
            }
            if (!c->out.empty()) flush(c);
        }
        if (host) {
            std::vector<int> expired;
            for (auto& [slot, m] : members)
                if (!m.conn && ms_between(m.lost_at, now) > cfg.slot_keep_ms) expired.push_back(slot);
            for (int s : expired) {
                log("party link: slot " + std::to_string(s) + " released (" + members[s].name + " did not come back)");
                release_member(s, false);
            }
            if (ms_between(last_roster_bcast, now) >= cfg.roster_refresh_ms) broadcast_roster();
        } else {
            if (!gconn && state == LinkState::Reconnecting && now >= next_attempt) start_connect();
            for (auto it = pending.begin(); it != pending.end();) {
                if (!it->second.sync && now >= it->second.deadline) {
                    auto f = std::move(it->second.cb);
                    if (f) post([f] { f(false, "rpc timed out"); });
                    it = pending.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }

    void io_loop() {
        std::vector<sock::PollFd> fds;
        std::vector<Conn*> owners;
        for (;;) {
            fds.clear();
            owners.clear();
            {
                std::lock_guard<std::mutex> lk(mu);
                if (stopping) break;
                if (freeze_until <= Clock::now()) {
                    if (listen_s != sock::kInvalid) {
                        sock::PollFd p{};
                        p.fd = listen_s;
                        p.events = POLLIN;
                        fds.push_back(p);
                        owners.push_back(nullptr);
                    }
                    for (auto& c : conns) {
                        if (c->dead || c->s == sock::kInvalid) continue;
                        sock::PollFd p{};
                        p.fd = c->s;
                        p.events = POLLIN;
                        if (c->phase == Phase::Connecting || c->out_off < c->out.size()) p.events |= POLLOUT;
                        fds.push_back(p);
                        owners.push_back(c.get());
                    }
                }
            }
            int n = 0;
            if (fds.empty())
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            else
                n = sock::poll(fds.data(), static_cast<unsigned long>(fds.size()), 10);
            std::lock_guard<std::mutex> lk(mu);
            if (stopping) break;
            if (freeze_until > Clock::now()) continue;
            if (n > 0) {
                for (std::size_t i = 0; i < fds.size(); ++i) {
                    short re = fds[i].revents;
                    if (!re) continue;
                    Conn* c = owners[i];
                    if (!c) {
                        accept_all();
                        continue;
                    }
                    if (c->dead) continue;
                    if (c->phase == Phase::Connecting) {
                        if (re & (POLLOUT | POLLERR | POLLHUP)) {
                            int err = 0;
                            sock::socklen el = sizeof err;
                            getsockopt(c->s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &el);
                            if (err != 0 || (re & (POLLERR | POLLHUP) && !(re & POLLOUT)))
                                lose(c, "cannot connect to the host (" + std::to_string(err) + ")");
                            else
                                on_connected(c);
                        }
                        continue;
                    }
                    if (re & (POLLIN | POLLERR | POLLHUP)) read_conn(c);
                    if (!c->dead && (re & POLLOUT)) flush(c);
                }
            }
            timers();
            conns.erase(std::remove_if(conns.begin(), conns.end(), [](const std::unique_ptr<Conn>& c) { return c->dead; }),
                        conns.end());
        }
    }

    void start_threads() {
        dstop = false;
        dthread = std::thread([this] { dispatch_loop(); });
        io_thread = std::thread([this] { io_loop(); });
    }
};

// ---------------- public API ----------------

PartyLink::PartyLink(LinkConfig cfg, LinkCallbacks cb) : impl_(std::make_unique<Impl>(std::move(cfg), std::move(cb))) {}

PartyLink::~PartyLink() { stop(); }

bool PartyLink::start_host(std::string* error) {
    Impl& I = *impl_;
    auto fail = [&](std::string m) {
        if (error) *error = std::move(m);
        return false;
    };
    if (I.state != LinkState::Idle) return fail("already started");
    if (!sock::startup()) return fail("winsock unavailable");
    if (!valid_member_name(I.cfg.name)) return fail("invalid name (1-16 of A-Z a-z 0-9 _ -)");
    I.host = true;
    I.party_key = crypto::derive_party_key(I.cfg.password, I.cfg.secret.data());
    sock::Socket s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == sock::kInvalid) return fail("socket() failed");
#if defined(_WIN32)
    BOOL excl = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof excl);
#else
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#endif
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(I.cfg.port);
    if (inet_pton(AF_INET, I.cfg.bind_addr.c_str(), &a.sin_addr) != 1) a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
        sock::close(s);
        return fail("party port " + std::to_string(I.cfg.port) + " is in use (another game instance?)");
    }
    if (::listen(s, 8) != 0) {
        sock::close(s);
        return fail("listen() failed");
    }
    sock::set_nonblocking(s);
    sockaddr_in b{};
    sock::socklen bl = sizeof b;
    getsockname(s, reinterpret_cast<sockaddr*>(&b), &bl);
    {
        std::lock_guard<std::mutex> lk(I.mu);
        I.listen_s = s;
        I.port = ntohs(b.sin_port);
        I.start_time = Clock::now();
        I.set_state(LinkState::Hosting, "port " + std::to_string(I.port) + ", up to " +
                                            std::to_string(I.cfg.max_players) + " players");
    }
    I.start_threads();
    return true;
}

bool PartyLink::start_guest(const std::string& host, std::uint16_t port, std::string* error) {
    Impl& I = *impl_;
    auto fail = [&](std::string m) {
        if (error) *error = std::move(m);
        return false;
    };
    if (I.state != LinkState::Idle) return fail("already started");
    if (!sock::startup()) return fail("winsock unavailable");
    if (!valid_member_name(I.cfg.name)) return fail("invalid name (1-16 of A-Z a-z 0-9 _ -)");
    I.host = false;
    I.party_key = crypto::derive_party_key(I.cfg.password, I.cfg.secret.data());
    {
        std::lock_guard<std::mutex> lk(I.mu);
        I.host_name = host;
        I.host_port = port;
        I.start_time = Clock::now();
        I.start_connect();
    }
    I.start_threads();
    return true;
}

void PartyLink::stop(bool graceful) {
    Impl& I = *impl_;
    {
        std::lock_guard<std::mutex> lk(I.mu);
        if (I.state == LinkState::Idle) return;
        if (I.stopping) goto join;
        if (graceful) {
            I.freeze_until = {};
            Writer w;
            w.u8(static_cast<std::uint8_t>(I.host ? RejectCode::Shutdown : RejectCode::None)).str16(
                I.host ? "the host ended the party" : "left");
            for (auto& c : I.conns)
                if (!c->dead && c->phase == Impl::Phase::Open) I.send(c.get(), kBye, w);
        }
        I.stopping = true;
        for (auto& c : I.conns) I.close_conn(c.get());
        if (I.listen_s != sock::kInvalid) {
            sock::close(I.listen_s);
            I.listen_s = sock::kInvalid;
        }
        I.gconn = nullptr;
        I.fail_pending("party link stopped");
        I.set_state(LinkState::Stopped, graceful ? "" : "dropped");
    }
join:
    if (I.io_thread.joinable()) I.io_thread.join();
    {
        std::lock_guard<std::mutex> lk(I.dmu);
        I.dstop = true;
    }
    I.dcv.notify_all();
    if (I.dthread.joinable()) I.dthread.join();
    std::lock_guard<std::mutex> lk(I.mu);
    I.conns.clear();
    I.members.clear();
    crypto::wipe(I.party_key.data(), I.party_key.size());
}

bool PartyLink::is_host() const { return impl_->host; }

LinkState PartyLink::state() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->state;
}

bool PartyLink::wait_state(LinkState s, int timeout_ms) const {
    std::unique_lock<std::mutex> lk(impl_->mu);
    return impl_->state_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] { return impl_->state == s; });
}

int PartyLink::local_slot() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->host ? kHostSlot : impl_->my_slot;
}

std::uint16_t PartyLink::bound_port() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->host ? impl_->port : impl_->host_port;
}

int PartyLink::max_players() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->host ? impl_->cfg.max_players : impl_->host_max_players;
}

std::vector<RosterEntry> PartyLink::roster() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->host ? impl_->host_roster() : impl_->groster;
}

std::int64_t PartyLink::host_clock_ms() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->now_ms() + (impl_->host ? 0 : impl_->clock_offset);
}

std::uint32_t PartyLink::rtt_ms() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->rtt;
}

std::string PartyLink::observed_address() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->observed;
}

std::uint32_t PartyLink::member_ip(int slot) const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->host) return 0;
    auto it = impl_->members.find(slot);
    if (it == impl_->members.end() || !it->second.conn) return 0;
    std::uint32_t ip = 0;
    std::memcpy(&ip, it->second.conn->peer_ip.data(), 4);
    return ip;
}

RejectCode PartyLink::reject_code() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->reject;
}

std::string PartyLink::reject_reason() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->reject_text;
}

bool PartyLink::rpc_call(const std::string& kind, const std::string& json, std::string* reply, int timeout_ms) {
    Impl& I = *impl_;
    std::unique_lock<std::mutex> lk(I.mu);
    if (I.host || !I.gconn || I.gconn->phase != Impl::Phase::Open) {
        if (reply) *reply = I.host ? "rpc_call is guest-only" : "not connected to the host";
        return false;
    }
    std::uint32_t id = I.next_rpc++;
    if (id == 0) id = I.next_rpc++;
    Impl::Pending& p = I.pending[id];
    p.sync = true;
    p.deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    Writer w;
    w.u32(id).str16(kind).str32(json);
    I.send(I.gconn, kRpcReq, w);
    I.state_cv.wait_until(lk, p.deadline, [&] { return I.pending[id].done; });
    Impl::Pending done = std::move(I.pending[id]);
    I.pending.erase(id);
    if (!done.done) {
        if (reply) *reply = "rpc timed out";
        return false;
    }
    if (reply) *reply = std::move(done.reply);
    return done.ok;
}

std::uint32_t PartyLink::rpc_call_async(const std::string& kind, const std::string& json,
                                        std::function<void(bool, const std::string&)> done, int timeout_ms) {
    Impl& I = *impl_;
    std::lock_guard<std::mutex> lk(I.mu);
    if (I.host || !I.gconn || I.gconn->phase != Impl::Phase::Open) {
        std::string why = I.host ? "rpc_call is guest-only" : "not connected to the host";
        if (done) I.post([done, why] { done(false, why); });
        return 0;
    }
    std::uint32_t id = I.next_rpc++;
    if (id == 0) id = I.next_rpc++;
    Impl::Pending& p = I.pending[id];
    p.sync = false;
    p.deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    p.cb = std::move(done);
    Writer w;
    w.u32(id).str16(kind).str32(json);
    I.send(I.gconn, kRpcReq, w);
    return id;
}

std::uint64_t PartyLink::send_event(int slot, const std::string& name, const std::string& json) {
    Impl& I = *impl_;
    std::lock_guard<std::mutex> lk(I.mu);
    if (I.state == LinkState::Stopped || I.state == LinkState::Idle) return 0;
    auto push = [&](EventStream& es, Impl::Conn* c) {
        EventStream::Ev e{es.next_cursor++, name, json};
        es.unacked.push_back(e);
        if (c && c->phase == Impl::Phase::Open) I.send_event_frame(c, e);
        return e.cursor;
    };
    if (!I.host) {
        if (I.state == LinkState::Rejected) return 0;
        return push(I.gev, I.gconn);
    }
    std::uint64_t last = 0;
    for (auto& [s, m] : I.members)
        if (slot == kBroadcast || slot == s) last = push(m.ev, m.conn);
    return last;
}

bool PartyLink::send_party_cmd(int slot, const std::string& cmd, const std::string& json) {
    Impl& I = *impl_;
    std::lock_guard<std::mutex> lk(I.mu);
    Writer w;
    w.str16(cmd).str32(json);
    if (!I.host) {
        if (!I.gconn || I.gconn->phase != Impl::Phase::Open) return false;
        I.send(I.gconn, kPartyCmd, w);
        return true;
    }
    bool any = false;
    for (auto& [s, m] : I.members)
        if ((slot == kBroadcast || slot == s) && m.conn && m.conn->phase == Impl::Phase::Open) {
            I.send(m.conn, kPartyCmd, w);
            any = true;
        }
    return any;
}

bool PartyLink::send_progress(int slot, const std::vector<std::uint8_t>& blob) {
    Impl& I = *impl_;
    std::lock_guard<std::mutex> lk(I.mu);
    Writer w;
    w.u32(static_cast<std::uint32_t>(blob.size())).bytes(blob.data(), blob.size());
    if (!I.host) {
        if (!I.gconn || I.gconn->phase != Impl::Phase::Open) return false;
        I.send(I.gconn, kProgress, w);
        return true;
    }
    bool any = false;
    for (auto& [s, m] : I.members)
        if ((slot == kBroadcast || slot == s) && m.conn && m.conn->phase == Impl::Phase::Open) {
            I.send(m.conn, kProgress, w);
            any = true;
        }
    return any;
}

void PartyLink::set_local_state(MemberState state, std::uint32_t map_id) {
    Impl& I = *impl_;
    std::lock_guard<std::mutex> lk(I.mu);
    if (I.local_state == state && I.local_map == map_id) return;
    I.local_state = state;
    I.local_map = map_id;
    if (I.host) {
        if (I.state == LinkState::Hosting) I.broadcast_roster();
    } else if (I.gconn && I.gconn->phase == Impl::Phase::Open) {
        I.send_local_state(I.gconn);
    }
}

bool PartyLink::kick(int slot, const std::string& reason) {
    Impl& I = *impl_;
    std::lock_guard<std::mutex> lk(I.mu);
    if (!I.host || !I.members.count(slot)) return false;
    I.release_member(slot, true, RejectCode::Kicked, reason.empty() ? "kicked by the host" : reason);
    return true;
}

void PartyLink::debug_freeze(int ms) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->freeze_until = Clock::now() + std::chrono::milliseconds(ms);
}

void PartyLink::debug_drop_connections() {
    Impl& I = *impl_;
    std::lock_guard<std::mutex> lk(I.mu);
    for (auto& c : I.conns)
        if (!c->dead) I.lose(c.get(), "dropped (debug)");
}

}  // namespace party

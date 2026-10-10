// SPDX-License-Identifier: GPL-3.0-or-later
// PartyLink (gpu/shim/party): party codes, crypto, STUN against a local responder, and a
// loopback party: host + guests over 127.0.0.1 -- handshake, wrong password / mismatched hash
// rejected, encrypted RPC, events, disconnect detection, reconnect with the slot kept, slot
// expiry, max players, host shutdown.
// Build and run: ninja -C out/gpu party-link-test && out/gpu/party-link-test.exe
#include "party/party_addr.h"
#include "party/party_code.h"
#include "party/party_crypto.h"
#include "party/party_link.h"
#include "party/party_sock.h"
#include "party/upnp_win.h"
#include "net/net_stun.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace party;

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        ++g_checks;                                                                    \
        if (!(cond)) {                                                                 \
            ++g_failures;                                                              \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                              \
    } while (0)

static bool wait_for(const std::function<bool()>& pred, int timeout_ms) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < end) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

static void test_codes() {
    std::array<std::uint8_t, 8> secret = {0xde, 0xad, 0xbe, 0xef, 0x01, 0x23, 0x45, 0x67};
    std::string code = make_party_code({203, 0, 113, 7}, 9307, secret, kCodeFlagPasswordRequired);
    std::printf("  code: %s\n", code.c_str());
    CHECK(code.rfind("BBP1-", 0) == 0);
    PartyCode pc;
    std::string err;
    CHECK(decode_party_code(code, &pc, &err));
    CHECK(!pc.plain && pc.port == 9307 && pc.secret == secret && pc.flags == kCodeFlagPasswordRequired);
    CHECK(pc.address() == "203.0.113.7:9307");

    // Case-insensitive, blanks, I/L -> 1 and O -> 0.
    std::string sloppy;
    for (char c : code) sloppy += static_cast<char>(c == '1' ? 'l' : c == '0' ? 'O' : std::tolower(c));
    sloppy = "  " + sloppy + " ";
    PartyCode pc2;
    CHECK(decode_party_code(sloppy, &pc2, &err));
    CHECK(pc2.secret == secret && pc2.ipv4 == pc.ipv4);
    std::string spaced = code;
    for (auto& c : spaced)
        if (c == '-') c = ' ';
    CHECK(decode_party_code(spaced, &pc2, &err));

    // Every single-character substitution is caught (CRC-16) or still decodes to the same code.
    int caught = 0, total = 0;
    const char* alpha = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    for (std::size_t i = 5; i < code.size(); ++i) {
        if (code[i] == '-') continue;
        for (const char* a = alpha; *a; ++a) {
            if (*a == code[i]) continue;
            std::string t = code;
            t[i] = *a;
            ++total;
            PartyCode x;
            if (!decode_party_code(t, &x, &err)) ++caught;
        }
    }
    CHECK(caught == total);
    std::printf("  %d/%d single-character typos rejected\n", caught, total);

    std::string bad = code;
    bad[7] = 'U';  // not in Crockford's alphabet
    CHECK(!decode_party_code(bad, &pc2, &err) && err.find("invalid character") != std::string::npos);
    CHECK(!decode_party_code(code.substr(0, code.size() - 2), &pc2, &err));
    std::printf("  short code: %s\n", err.c_str());

    CHECK(decode_party_code("192.168.1.20:9400", &pc2, &err) && pc2.plain && pc2.port == 9400 &&
          pc2.host == "192.168.1.20" && pc2.ipv4[3] == 20);
    CHECK(decode_party_code("myhost.example", &pc2, &err) && pc2.plain && pc2.port == kDefaultPartyPort);
    CHECK(!decode_party_code("1.2.3.4:70000", &pc2, &err));
    CHECK(!decode_party_code("", &pc2, &err));
    CHECK(crc16_ccitt(reinterpret_cast<const std::uint8_t*>("123456789"), 9) == 0x29B1);

    std::array<std::uint8_t, 4> lan{};
    if (best_local_ipv4(&lan)) {
        std::string lc;
        CHECK(make_lan_code(9307, secret, 0, &lc));
        CHECK(decode_party_code(lc, &pc2, &err) && (pc2.flags & kCodeFlagLan));
        std::printf("  LAN address %s -> %s\n", format_ipv4(lan).c_str(), lc.c_str());
    } else {
        std::printf("  (no LAN adapter)\n");
    }
}

static void test_crypto() {
    std::uint8_t secret[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    auto t0 = std::chrono::steady_clock::now();
    crypto::Key k1 = crypto::derive_party_key("pw", secret);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  argon2i: %lld ms\n", static_cast<long long>(ms));
    crypto::Key k2 = crypto::derive_party_key("pw", secret);
    crypto::Key k3 = crypto::derive_party_key("pX", secret);
    CHECK(k1 == k2 && k1 != k3);
    crypto::Nonce a{}, b{};
    crypto::random_bytes(a.data(), a.size());
    crypto::random_bytes(b.data(), b.size());
    CHECK(a != b);
    crypto::Aead tx, rx;
    tx.init(crypto::session_key(k1, "bbp-h2g", a, b));
    rx.init(crypto::session_key(k1, "bbp-h2g", a, b));
    const char msg[] = "hello party";
    std::uint8_t ct[sizeof msg], mac[16], pt[sizeof msg];
    std::uint8_t ad[5] = {1, 2, 3, 4, 5};
    tx.seal(ct, mac, ad, 5, reinterpret_cast<const std::uint8_t*>(msg), sizeof msg);
    std::uint8_t ct2[sizeof msg], mac2[16];
    std::memcpy(ct2, ct, sizeof ct);
    std::memcpy(mac2, mac, 16);
    ct2[0] ^= 1;
    CHECK(!rx.open(pt, mac2, ad, 5, ct2, sizeof ct2));  // tampered
    CHECK(rx.open(pt, mac, ad, 5, ct, sizeof ct) && std::memcmp(pt, msg, sizeof msg) == 0);
    CHECK(!rx.open(pt, mac, ad, 5, ct, sizeof ct));  // replay: the counter moved on
}

static void test_stun_local() {
    sock::startup();
    sock::Socket srv = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(srv, reinterpret_cast<sockaddr*>(&a), sizeof a);
    sock::socklen al = sizeof a;
    getsockname(srv, reinterpret_cast<sockaddr*>(&a), &al);
    std::uint16_t srv_port = ntohs(a.sin_port);
    std::thread responder([srv] {
        std::uint8_t buf[1500];
        sockaddr_in from{};
        sock::socklen fl = sizeof from;
        long n = ::recvfrom(srv, reinterpret_cast<char*>(buf), sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl);
        if (n <= 0) return;
        std::uint8_t txid[net::stun::kTxid], tok[net::stun::kTokenLen];
        bool hello = false;
        if (!net::stun::parse_binding_request(buf, static_cast<std::size_t>(n), txid, &hello, tok)) return;
        std::uint8_t out[net::stun::kMaxResponse];
        std::uint32_t addr;
        std::memcpy(&addr, &from.sin_addr, 4);
        std::size_t len = net::stun::build_binding_response(out, txid, addr, ntohs(from.sin_port));
        ::sendto(srv, reinterpret_cast<const char*>(out), static_cast<int>(len), 0, reinterpret_cast<sockaddr*>(&from),
                 fl);
    });
    std::array<std::uint8_t, 4> ip{};
    std::uint16_t port = 0;
    std::string err;
    bool ok = stun_query(-1, "127.0.0.1:" + std::to_string(srv_port), 0, &ip, &port, &err, 1000, 2);
    responder.join();
    sock::close(srv);
    CHECK(ok);
    CHECK(ip[0] == 127 && ip[3] == 1 && port != 0);
    std::printf("  STUN (local responder): %s:%u %s\n", format_ipv4(ip).c_str(), port, err.c_str());

#if defined(_WIN32)
    _putenv("BB_PARTY_PUBLIC_ADDR=198.51.100.9:9999");
#else
    setenv("BB_PARTY_PUBLIC_ADDR", "198.51.100.9:9999", 1);
#endif
    PublicAddress pa = resolve_public_address(9307);
    CHECK(pa.ok && pa.source == "override" && pa.port == 9999 && pa.ip[0] == 198);
#if defined(_WIN32)
    _putenv("BB_PARTY_PUBLIC_ADDR=");
    _putenv("BB_PARTY_UPNP=0");
#else
    unsetenv("BB_PARTY_PUBLIC_ADDR");
    setenv("BB_PARTY_UPNP", "0", 1);
#endif
    UpnpMapper upnp;
    upnp.start(9307);
    UpnpResult ur = upnp.wait(100);
    CHECK(ur.done && ur.disabled && !ur.ok);
    std::printf("  %s\n", ur.message.c_str());
}

// ---------------- loopback party ----------------

struct Recorder {
    std::mutex mu;
    std::vector<std::string> log;
    std::atomic<int> joined{0}, rejoined{0}, left_kept{0}, left_released{0}, events{0}, cmds{0}, progress{0},
        rosters{0};
    std::vector<std::uint64_t> cursors;
    std::vector<std::string> event_names;
    void add(std::string s) {
        std::lock_guard<std::mutex> lk(mu);
        log.push_back(std::move(s));
    }
    bool has_event(const std::string& n) {
        std::lock_guard<std::mutex> lk(mu);
        for (auto& e : event_names)
            if (e == n) return true;
        return false;
    }
    int count_event(const std::string& n) {
        std::lock_guard<std::mutex> lk(mu);
        int k = 0;
        for (auto& e : event_names)
            if (e == n) ++k;
        return k;
    }
};

static LinkConfig base_cfg(const char* name, const char* password) {
    LinkConfig c;
    c.name = name;
    c.password = password;
    c.secret = {9, 8, 7, 6, 5, 4, 3, 2};
    c.eboot_sha256.fill(0xAB);
    c.mods_hash.fill(0x11);
    c.port = 0;  // ephemeral: tests never collide with a running game
    c.bind_addr = "127.0.0.1";
    c.max_players = 3;
    c.ping_interval_ms = 100;
    c.lost_timeout_ms = 700;
    c.slot_keep_ms = 2500;
    c.backoff_initial_ms = 100;
    c.backoff_max_ms = 400;
    c.connect_timeout_ms = 1000;
    c.roster_refresh_ms = 300;
    return c;
}

static LinkCallbacks recorder_callbacks(Recorder& r, const char* who) {
    LinkCallbacks cb;
    std::string w = who;
    cb.on_member_joined = [&r, w](const RosterEntry& m, bool rejoined) {
        (rejoined ? r.rejoined : r.joined)++;
        r.add(w + " joined " + m.name + " slot " + std::to_string(m.slot) + (rejoined ? " (rejoined)" : ""));
    };
    cb.on_member_left = [&r, w](const RosterEntry& m, bool kept) {
        (kept ? r.left_kept : r.left_released)++;
        r.add(w + " left " + m.name + (kept ? " (slot kept)" : " (released)"));
    };
    cb.on_event = [&r](int, std::uint64_t cursor, const std::string& name, const std::string&) {
        std::lock_guard<std::mutex> lk(r.mu);
        r.cursors.push_back(cursor);
        r.event_names.push_back(name);
        r.events++;
    };
    cb.on_party_cmd = [&r](int, const std::string&, const std::string&) { r.cmds++; };
    cb.on_progress = [&r](int, const std::vector<std::uint8_t>& b) {
        if (b.size() == 100000 && b[99999] == 0x5a) r.progress++;
    };
    cb.on_roster = [&r](const std::vector<RosterEntry>&) { r.rosters++; };
    if (std::getenv("PARTY_TEST_VERBOSE"))
        cb.on_log = [w](const std::string& l) { std::printf("    [%s] %s\n", w.c_str(), l.c_str()); };
    return cb;
}

static void test_party() {
    Recorder hr, ar, br, cr;
    LinkCallbacks hcb = recorder_callbacks(hr, "host");
    hcb.on_rpc = [](int slot, const std::string& kind, const std::string& json) {
        return "{\"slot\":" + std::to_string(slot) + ",\"kind\":\"" + kind + "\",\"echo\":" + json + "}";
    };
    PartyLink host(base_cfg("Host", "hunter2"), hcb);
    std::string err;
    CHECK(host.start_host(&err));
    std::uint16_t port = host.bound_port();
    std::printf("  host listening on 127.0.0.1:%u\n", port);
    CHECK(port != 0 && host.state() == LinkState::Hosting);

    PartyLink a(base_cfg("Alice", "hunter2"), recorder_callbacks(ar, "alice"));
    PartyLink b(base_cfg("Bob_2", "hunter2"), recorder_callbacks(br, "bob"));
    CHECK(a.start_guest("127.0.0.1", port, &err));
    CHECK(b.start_guest("127.0.0.1", port, &err));
    CHECK(a.wait_connected(5000));
    CHECK(b.wait_connected(5000));
    CHECK(a.local_slot() != b.local_slot() && a.local_slot() >= 1 && b.local_slot() >= 1);
    CHECK(a.max_players() == 3);
    CHECK(wait_for([&] { return hr.joined.load() == 2; }, 2000));
    CHECK(wait_for([&] { return a.roster().size() == 3 && b.roster().size() == 3; }, 2000));
    std::printf("  alice slot %d (seen at %s), bob slot %d\n", a.local_slot(), a.observed_address().c_str(),
                b.local_slot());
    CHECK(a.observed_address().rfind("127.0.0.1:", 0) == 0);

    // Wrong password, mismatched hash, duplicate name, party full.
    {
        Recorder xr;
        PartyLink bad(base_cfg("Mallory", "wrong"), recorder_callbacks(xr, "mallory"));
        CHECK(bad.start_guest("127.0.0.1", port, &err));
        CHECK(bad.wait_state(LinkState::Rejected, 5000));
        CHECK(bad.reject_code() == RejectCode::Auth);
        std::printf("  wrong password: %s\n", bad.reject_reason().c_str());
    }
    {
        Recorder xr;
        LinkConfig c = base_cfg("Modder", "hunter2");
        c.mods_hash[3] ^= 1;
        PartyLink bad(c, recorder_callbacks(xr, "modder"));
        CHECK(bad.start_guest("127.0.0.1", port, &err));
        CHECK(bad.wait_state(LinkState::Rejected, 5000));
        CHECK(bad.reject_code() == RejectCode::Mismatch);
        std::printf("  mismatched mods: %s\n", bad.reject_reason().c_str());
    }
    {
        Recorder xr;
        PartyLink bad(base_cfg("Alice", "hunter2"), recorder_callbacks(xr, "alice2"));
        CHECK(bad.start_guest("127.0.0.1", port, &err));
        CHECK(bad.wait_state(LinkState::Rejected, 5000));
        CHECK(bad.reject_code() == RejectCode::Name);
    }
    {
        Recorder xr;
        PartyLink extra(base_cfg("Carol", "hunter2"), recorder_callbacks(xr, "carol"));
        CHECK(extra.start_guest("127.0.0.1", port, &err));
        CHECK(extra.wait_state(LinkState::Rejected, 5000));
        CHECK(extra.reject_code() == RejectCode::Full);
        std::printf("  third guest: %s\n", extra.reject_reason().c_str());
    }
    CHECK(hr.joined.load() == 2);

    // Encrypted RPC, sync and async.
    std::string reply;
    CHECK(a.rpc_call("ss.info", "{\"x\":1}", &reply, 2000));
    std::printf("  rpc reply: %s\n", reply.c_str());
    CHECK(reply == "{\"slot\":" + std::to_string(a.local_slot()) + ",\"kind\":\"ss.info\",\"echo\":{\"x\":1}}");
    std::atomic<int> async_ok{0};
    std::string big(200000, 'j');
    b.rpc_call_async("big", "\"" + big + "\"", [&](bool ok, const std::string& r) {
        if (ok && r.size() > 200000) async_ok++;
    }, 2000);
    CHECK(wait_for([&] { return async_ok.load() == 1; }, 3000));
    CHECK(!host.rpc_call("x", "{}", &reply, 100));  // host side has no rpc_call

    // Events: targeted, broadcast, guest -> host; party commands, progress blobs.
    CHECK(host.send_event(a.local_slot(), "only_alice", "{}") == 1);
    CHECK(host.send_event(kBroadcast, "everyone", "{\"n\":1}") != 0);
    CHECK(wait_for([&] { return ar.has_event("only_alice") && ar.has_event("everyone") && br.has_event("everyone"); },
                   2000));
    CHECK(!br.has_event("only_alice"));
    CHECK(a.send_event(0, "guest_said", "{}") == 1);
    CHECK(wait_for([&] { return hr.has_event("guest_said"); }, 2000));
    CHECK(host.send_party_cmd(kBroadcast, "warp", "{\"map\":1}"));
    CHECK(b.send_party_cmd(0, "ready", "{}"));
    std::vector<std::uint8_t> blob(100000, 1);
    blob[99999] = 0x5a;
    CHECK(host.send_progress(b.local_slot(), blob));
    CHECK(wait_for([&] { return ar.cmds.load() == 1 && br.cmds.load() == 1 && hr.cmds.load() == 1; }, 2000));
    CHECK(wait_for([&] { return br.progress.load() == 1; }, 2000));

    // Roster state from a guest reaches everyone.
    a.set_local_state(MemberState::InHostWorld, 0x0A000000);
    CHECK(wait_for([&] {
        for (auto& e : b.roster())
            if (e.name == "Alice" && e.state == MemberState::InHostWorld && e.map_id == 0x0A000000) return true;
        return false;
    }, 2000));
    CHECK(wait_for([&] { return a.rtt_ms() < 1000 && std::llabs(a.host_clock_ms() - host.host_clock_ms()) < 100; },
                   2000));

    // Disconnect detection: Alice's network freezes past the lost timeout. The host notices the
    // silence and keeps the slot; an event queued meanwhile arrives after Alice reconnects.
    int alice_slot = a.local_slot();
    a.debug_freeze(1200);
    CHECK(wait_for([&] { return hr.left_kept.load() == 1; }, 3000));
    CHECK(host.send_event(alice_slot, "while_away", "{}") != 0);
    bool kept = false;
    for (auto& e : host.roster())
        if (e.slot == alice_slot && !e.connected) kept = true;
    CHECK(kept);
    CHECK(wait_for([&] { return hr.rejoined.load() == 1; }, 5000));
    CHECK(a.wait_connected(3000));
    CHECK(a.local_slot() == alice_slot);
    CHECK(wait_for([&] { return ar.has_event("while_away"); }, 2000));
    CHECK(ar.count_event("everyone") == 1);  // no duplicate delivery after the replay
    CHECK(a.rpc_call("after", "{}", &reply, 2000));

    // A hard drop (no BYE) of Alice's socket: the guest reconnects by itself.
    a.debug_drop_connections();
    CHECK(wait_for([&] { return hr.rejoined.load() == 2; }, 5000));
    CHECK(a.wait_connected(3000) && a.local_slot() == alice_slot);

    // Bob crashes: slot kept, then released after slot_keep; Carol can then join.
    int lost_before = hr.left_kept.load();
    b.stop(false);
    CHECK(wait_for([&] { return hr.left_kept.load() == lost_before + 1; }, 3000));
    {
        Recorder xr;
        PartyLink early(base_cfg("Carol", "hunter2"), recorder_callbacks(xr, "carol"));
        CHECK(early.start_guest("127.0.0.1", port, &err));
        CHECK(early.wait_state(LinkState::Rejected, 5000) && early.reject_code() == RejectCode::Full);
    }
    CHECK(wait_for([&] { return hr.left_released.load() == 1; }, 5000));
    PartyLink c(base_cfg("Carol", "hunter2"), recorder_callbacks(cr, "carol"));
    CHECK(c.start_guest("127.0.0.1", port, &err));
    CHECK(c.wait_connected(5000));
    CHECK(c.rpc_call("hello", "{}", &reply, 2000));

    // Graceful leave frees the slot immediately; host shutdown ends the party for the rest.
    c.stop();
    CHECK(wait_for([&] { return hr.left_released.load() == 2; }, 2000));
    host.stop();
    CHECK(a.wait_state(LinkState::Rejected, 3000));
    CHECK(a.reject_code() == RejectCode::Shutdown);
    std::printf("  after host stop: alice %s (%s)\n", link_state_name(a.state()), a.reject_reason().c_str());
    a.stop();

    std::printf("  host saw: joined %d, rejoined %d, lost %d, released %d, rosters %d\n", hr.joined.load(),
                hr.rejoined.load(), hr.left_kept.load(), hr.left_released.load(), hr.rosters.load());
}

static void test_max_players_two() {
    LinkCallbacks none;
    LinkConfig hc = base_cfg("Host", "");
    hc.max_players = 2;
    PartyLink host(hc, none);
    std::string err;
    CHECK(host.start_host(&err));
    PartyLink a(base_cfg("A", ""), none), b(base_cfg("B", ""), none);
    CHECK(a.start_guest("127.0.0.1", host.bound_port(), &err));
    CHECK(a.wait_connected(5000));
    CHECK(b.start_guest("127.0.0.1", host.bound_port(), &err));
    CHECK(b.wait_state(LinkState::Rejected, 5000) && b.reject_code() == RejectCode::Full);
    // Code secret alone (no password): a guest with another secret is rejected.
    LinkConfig other = base_cfg("D", "");
    other.secret[0] ^= 0xFF;
    PartyLink d(other, none);
    CHECK(d.start_guest("127.0.0.1", host.bound_port(), &err));
    CHECK(d.wait_state(LinkState::Rejected, 5000) && d.reject_code() == RejectCode::Auth);
}

// A crashed host restarted on the same port with the same secret and the member table it kept:
// every guest gets its old slot back (a new session: not resumed), nobody is rejected.
static void test_host_restart() {
    Recorder hr, ar, br, h2r;
    PartyLink* host = new PartyLink(base_cfg("Host", ""), recorder_callbacks(hr, "host"));
    std::string err;
    CHECK(host->start_host(&err));
    const std::uint16_t port = host->bound_port();
    PartyLink a(base_cfg("Alice", ""), recorder_callbacks(ar, "alice"));
    PartyLink b(base_cfg("Bob", ""), recorder_callbacks(br, "bob"));
    CHECK(a.start_guest("127.0.0.1", port, &err));
    CHECK(a.wait_connected(5000));
    CHECK(b.start_guest("127.0.0.1", port, &err));
    CHECK(b.wait_connected(5000));
    const int sa = a.local_slot(), sb = b.local_slot();
    const std::vector<KeptMember> kept = host->kept_members();
    CHECK(kept.size() == 2);
    host->stop(false);  // the crash: no BYE
    CHECK(a.wait_state(LinkState::Reconnecting, 3000));
    CHECK(b.wait_state(LinkState::Reconnecting, 3000));
    // Reversed join order on purpose: Bob first. Slots come from the kept table, not the order.
    LinkConfig hc = base_cfg("Host", "");
    hc.port = port;
    PartyLink host2(hc, recorder_callbacks(h2r, "host2"));
    host2.restore_members(kept);
    bool up = false;
    for (int i = 0; i < 50 && !(up = host2.start_host(&err)); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(up);
    CHECK(wait_for([&] { return a.state() == LinkState::Connected && b.state() == LinkState::Connected; }, 5000));
    CHECK(a.local_slot() == sa && b.local_slot() == sb);
    CHECK(!a.last_welcome_resumed() && !b.last_welcome_resumed());
    CHECK(wait_for([&] {
        int conn = 0;
        for (const RosterEntry& e : host2.roster()) conn += e.connected;
        return conn == 3;
    }, 3000));
    CHECK(h2r.rejoined.load() == 2 && h2r.joined.load() == 0);
    // Events flow on the new streams.
    host2.send_event(kBroadcast, "after_restart", "{}");
    CHECK(wait_for([&] { return ar.events.load() >= 1 && br.events.load() >= 1; }, 3000));
    std::printf("  restarted host on :%u: alice slot %d, bob slot %d back\n", port, a.local_slot(), b.local_slot());
    a.stop();
    b.stop();
    host2.stop();
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("party codes\n");
    test_codes();
    std::printf("crypto\n");
    test_crypto();
    std::printf("stun / env\n");
    test_stun_local();
    std::printf("loopback party\n");
    test_party();
    std::printf("max players 2, secret-only key\n");
    test_max_players_two();
    std::printf("host restart: slots restored\n");
    test_host_restart();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    std::printf(g_failures ? "FAILED\n" : "OK\n");
    return g_failures ? 1 : 0;
}

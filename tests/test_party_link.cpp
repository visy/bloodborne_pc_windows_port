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
#include "net/netsim.h"
#include "party/party_sock.h"
#include "party/upnp_win.h"
#include "net/net_stun.h"

#include <atomic>
#include <chrono>
#include <cmath>
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
    c.patches_hash.fill(0x22);
    c.patch_names = {"Party: Bells anywhere", "Party: Skip Online/Offline Choice (Online)"};
    c.mod_names = {"Better Params (12 files)"};
    c.build_id = "0.5 (v0.5-42-gabcdef1234)";
    c.params_sha256.fill(0x33);
    c.hook_names = {"4p H1 cooperator slots (SessionSlotSummary) @+0x186fe40", "travel lamp warp (0x13CDF30) @+0x13cdf30"};
    c.hooks_hash = hook_set_hash(c.hook_names);
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
        CHECK(bad.reject_reason().find("gameplay mods differ") != std::string::npos);
        CHECK(bad.reject_reason().find("patches") == std::string::npos);
        CHECK(bad.reject_reason().find("eboot") == std::string::npos);
        std::printf("  mismatched mods: %s\n", bad.reject_reason().c_str());
    }
    {
        // Another gameplay patch set and another eboot: the guest learns both, by name.
        Recorder xr;
        LinkConfig c = base_cfg("Cheater", "hunter2");
        c.patches_hash[0] ^= 1;
        c.patch_names = {"Party: Skip Online/Offline Choice (Online)", "Player No Dead (Read note)"};
        c.eboot_sha256[0] = 0x01;
        PartyLink bad(c, recorder_callbacks(xr, "cheater"));
        CHECK(bad.start_guest("127.0.0.1", port, &err));
        CHECK(bad.wait_state(LinkState::Rejected, 5000));
        CHECK(bad.reject_code() == RejectCode::Mismatch);
        const std::string why = bad.reject_reason();
        CHECK(why.find("eboot.bin differs") != std::string::npos);
        CHECK(why.find("host only: Party: Bells anywhere") != std::string::npos);
        CHECK(why.find("yours only: Player No Dead (Read note)") != std::string::npos);
        CHECK(why.find("mods") == std::string::npos);
        std::printf("  mismatched eboot + patches: %s\n", why.c_str());
    }
    {
        // A9 over the wire: another bbport build and another party hook set.
        Recorder xr;
        LinkConfig c = base_cfg("Oldbuild", "hunter2");
        c.build_id = "0.5 (v0.5-7-g1111111111-dirty)";
        c.hook_names.pop_back();
        c.hooks_hash = hook_set_hash(c.hook_names);
        PartyLink bad(c, recorder_callbacks(xr, "oldbuild"));
        CHECK(bad.start_guest("127.0.0.1", port, &err));
        CHECK(bad.wait_state(LinkState::Rejected, 5000));
        CHECK(bad.reject_code() == RejectCode::Mismatch);
        const std::string why = bad.reject_reason();
        CHECK(why.find("bbport build differs") != std::string::npos && why.find("v0.5-7-g1111111111-dirty") != std::string::npos);
        CHECK(why.find("party patches differ (host only: travel lamp warp") != std::string::npos);
        CHECK(why.find("eboot") == std::string::npos && why.find("game params") == std::string::npos);
        std::printf("  other build + hooks: %s\n", why.c_str());
    }
    {
        // Cosmetic patches never reach the hash: only the gameplay set is compared, so a guest
        // with the same gameplay set (whatever its graphics patches) joins. Here: same everything.
        CHECK(identity_mismatch(base_cfg("A", ""), base_cfg("B", "")).empty());
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
    // post_event (the game's main thread): never waits for the IO lock; same delivery.
    for (int k = 0; k < 50; ++k) host.post_event(kBroadcast, "posted_" + std::to_string(k), "{}");
    a.post_event(0, "guest_posted", "{}");
    CHECK(wait_for([&] { return ar.has_event("posted_49") && br.has_event("posted_49") && hr.has_event("guest_posted"); },
                   2000));
    CHECK(ar.has_event("posted_0") && br.has_event("posted_0"));
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

// A plain host:port (no party code, so no secret) with the right password joins a host that has
// a password (party_code.h; the in-game party flow saves such addresses); without the password
// (or when the host has none) it is refused.
static void test_plain_address_password() {
    LinkCallbacks none;
    PartyLink host(base_cfg("Host", "pw"), none);
    std::string err;
    CHECK(host.start_host(&err));
    LinkConfig plain = base_cfg("Plain", "pw");
    plain.secret = {};
    PartyLink p(plain, none);
    CHECK(p.start_guest("127.0.0.1", host.bound_port(), &err));
    CHECK(p.wait_connected(5000));
    CHECK(wait_for([&] { return host.roster().size() == 2; }, 2000));
    LinkConfig wrong = base_cfg("Wrong", "nope");
    wrong.secret = {};
    PartyLink w(wrong, none);
    CHECK(w.start_guest("127.0.0.1", host.bound_port(), &err));
    CHECK(w.wait_state(LinkState::Rejected, 5000) && w.reject_code() == RejectCode::Auth);
    PartyLink open(base_cfg("Host2", ""), none);
    CHECK(open.start_host(&err));
    LinkConfig nopw = base_cfg("NoPw", "");
    nopw.secret = {};
    PartyLink n(nopw, none);
    CHECK(n.start_guest("127.0.0.1", open.bound_port(), &err));
    CHECK(n.wait_state(LinkState::Rejected, 5000) && n.reject_code() == RejectCode::Auth);
    std::printf("  plain address + password: joined; wrong / no password: refused\n");
}

// Four players: host + 3 guests (slots 1..3, roster of 4, events to all), a 4th guest is
// refused, a guest's own max is overridden by the host's WELCOME, a guest with other game
// rules (party_fourp.h tag) is rejected with a reason naming both.
static void test_four_players() {
    Recorder hr, ar, br, cr;
    LinkConfig hc = base_cfg("Host", "");
    hc.max_players = 9;  // clamped to kMaxPartyPlayers (A7: host + 3)
    hc.rules = "max 4 players, 4p:v1:H1,H2,H3,H4,E6";
    PartyLink host(hc, recorder_callbacks(hr, "host"));
    std::string err;
    CHECK(host.start_host(&err));
    CHECK(host.max_players() == kMaxPartyPlayers && kMaxPartyPlayers == 4);
    const std::uint16_t port = host.bound_port();
    auto guest_cfg = [&](const char* name) {
        LinkConfig c = base_cfg(name, "");
        c.max_players = 3;  // the guest's own setting: the host's counts
        c.rules = hc.rules;
        return c;
    };
    PartyLink a(guest_cfg("A"), recorder_callbacks(ar, "a")), b(guest_cfg("B"), recorder_callbacks(br, "b")),
        c(guest_cfg("C"), recorder_callbacks(cr, "c"));
    CHECK(a.start_guest("127.0.0.1", port, &err));
    CHECK(b.start_guest("127.0.0.1", port, &err));
    CHECK(c.start_guest("127.0.0.1", port, &err));
    CHECK(a.wait_connected(5000) && b.wait_connected(5000) && c.wait_connected(5000));
    CHECK(a.max_players() == 4 && b.max_players() == 4 && c.max_players() == 4);
    const int sa = a.local_slot(), sb = b.local_slot(), sc = c.local_slot();
    CHECK(sa >= 1 && sa <= 3 && sb >= 1 && sb <= 3 && sc >= 1 && sc <= 3 && sa != sb && sb != sc && sa != sc);
    CHECK(wait_for([&] { return hr.joined.load() == 3; }, 3000));
    CHECK(wait_for([&] { return a.roster().size() == 4 && b.roster().size() == 4 && c.roster().size() == 4; }, 3000));
    std::printf("  slots %d %d %d, roster %zu\n", sa, sb, sc, host.roster().size());
    host.send_event(kBroadcast, "four", "{}");
    CHECK(wait_for([&] { return ar.count_event("four") == 1 && br.count_event("four") == 1 && cr.count_event("four") == 1; }, 3000));
    LinkCallbacks none;
    PartyLink d(guest_cfg("D"), none);
    CHECK(d.start_guest("127.0.0.1", port, &err));
    CHECK(d.wait_state(LinkState::Rejected, 5000) && d.reject_code() == RejectCode::Full);
    LinkConfig ec = guest_cfg("E");
    ec.rules = "max 3 players, 4p:v1:H1,H2,H3,H4,E6";
    PartyLink e(ec, none);
    c.stop();  // a free slot: the rejection below is about the rules, not the size
    CHECK(wait_for([&] { return host.roster().size() == 3; }, 3000));
    CHECK(e.start_guest("127.0.0.1", port, &err));
    CHECK(e.wait_state(LinkState::Rejected, 5000) && e.reject_code() == RejectCode::Mismatch);
    std::printf("  other rules: %s\n", e.reject_reason().c_str());
    CHECK(e.reject_reason().find("party rules differ") != std::string::npos &&
          e.reject_reason().find("max 3 players") != std::string::npos &&
          e.reject_reason().find("max 4 players") != std::string::npos);
}

// Party HUD vitals: the sanitizer, then host + 2 guests exchanging VITALS (each sees the others).
static void test_vitals() {
    MemberVitals v;
    v.slot = 2;
    v.flags = 0xff;
    v.hp = 500000;
    v.max_hp = 4000000;
    v.stamina = 60000;
    v.max_stamina = 60000;
    v.pos[0] = 1e9f;
    CHECK(sanitize_vitals(v));
    CHECK(v.flags == (kVitalsValid | kVitalsAlive) && v.max_hp == kMaxVitalsHp && v.hp == kMaxVitalsHp);
    CHECK(v.max_stamina == kMaxVitalsStamina && v.stamina == kMaxVitalsStamina && v.pos[0] == 0.0f);
    MemberVitals w;
    w.slot = 4;
    CHECK(!sanitize_vitals(w));
    w.slot = 1;
    w.flags = kVitalsValid | kVitalsAlive | kVitalsHasPos;
    w.hp = 0;
    w.max_hp = 100;
    w.pos[1] = std::nanf("");
    w.yaw = 7.0f;
    CHECK(sanitize_vitals(w));
    CHECK(w.flags == kVitalsValid);  // dead, no position
    w.pos[1] = 3.0f;
    w.flags = kVitalsHasPos;
    w.hp = 50;
    w.yaw = 7.0f;
    CHECK(sanitize_vitals(w) && w.hp == 0 && w.max_hp == 0 && w.flags == kVitalsHasPos && w.yaw < 3.1416f &&
          w.yaw > 0.7f);

    PartyLink host(base_cfg("Host", ""), LinkCallbacks{});
    std::string err;
    CHECK(host.start_host(&err));
    const std::uint16_t port = host.bound_port();
    PartyLink a(base_cfg("Alice", ""), LinkCallbacks{}), b(base_cfg("Bob", ""), LinkCallbacks{});
    CHECK(a.start_guest("127.0.0.1", port, &err) && b.start_guest("127.0.0.1", port, &err));
    CHECK(a.wait_connected(5000) && b.wait_connected(5000));
    auto mk = [](std::uint32_t hp, float x) {
        MemberVitals m;
        m.slot = 3;  // ignored: the link fills in its own slot
        m.flags = kVitalsValid | kVitalsAlive | kVitalsHasPos;
        m.hp = hp;
        m.max_hp = 1000;
        m.stamina = 80;
        m.max_stamina = 100;
        m.map_id = 0x18000000;
        m.pos[0] = x;
        m.pos[1] = 1;
        m.pos[2] = -x;
        m.yaw = 0.5f;
        return m;
    };
    host.set_local_vitals(mk(900, 1));
    a.set_local_vitals(mk(800, 2));
    b.set_local_vitals(mk(700, 3));
    auto find = [](const PartyLink& l, int slot, MemberVitals* out) {
        MemberVitals all[kMaxVitals];
        const int n = l.vitals(all, kMaxVitals);
        for (int i = 0; i < n; ++i)
            if (all[i].slot == slot) {
                *out = all[i];
                return true;
            }
        return false;
    };
    MemberVitals got;
    const int sa = a.local_slot(), sb = b.local_slot();
    CHECK(wait_for([&] { return find(host, sa, &got) && got.hp == 800; }, 3000));
    CHECK(got.pos[0] == 2.0f && got.map_id == 0x18000000 && got.stamina == 80 && got.yaw == 0.5f);
    CHECK(wait_for([&] { return find(a, 0, &got) && got.hp == 900; }, 3000));
    CHECK(wait_for([&] { return find(a, sb, &got) && got.hp == 700 && got.pos[2] == -3.0f; }, 3000));
    CHECK(wait_for([&] { return find(b, sa, &got) && got.hp == 800; }, 3000));
    CHECK(find(a, sa, &got) && got.hp == 800 && got.age_ms == 0);  // its own
    // A change gets through within a few send intervals.
    a.set_local_vitals(mk(10, 2));
    CHECK(wait_for([&] { return find(b, sa, &got) && got.hp == 10; }, 3000));
    // The overhead display extras (the VITALS trailer): guest -> host -> the other guest.
    MemberVitals ext = mk(10, 2);
    ext.level = 42;
    ext.deaths = 7;
    ext.rot = 3;
    a.set_local_vitals(ext);
    CHECK(wait_for([&] { return find(b, sa, &got) && got.level == 42; }, 3000));
    CHECK(got.deaths == 7 && got.rot == 3 && got.hp == 10);
    CHECK(wait_for([&] { return find(host, sa, &got) && got.level == 42 && got.rot == 3; }, 3000));
    // A guest that leaves goes stale on the others.
    b.stop();
    CHECK(wait_for([&] { return !find(a, sb, &got); }, kVitalsStaleMs + 3000));
    std::printf("  vitals relayed (slots %d, %d)\n", sa, sb);
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
    CHECK(wait_for([&] { return h2r.rejoined.load() == 2; }, 2000) && h2r.joined.load() == 0);  // callbacks lag
    // Events flow on the new streams.
    host2.send_event(kBroadcast, "after_restart", "{}");
    CHECK(wait_for([&] { return ar.events.load() >= 1 && br.events.load() >= 1; }, 3000));
    std::printf("  restarted host on :%u: alice slot %d, bob slot %d back\n", port, a.local_slot(), b.local_slot());
    a.stop();
    b.stop();
    host2.stop();
}

// The version check's explanation and the identity files patches.py / mods.py write.
static void test_identity() {
    LinkConfig host, guest;
    host.eboot_sha256.fill(1);
    guest.eboot_sha256.fill(1);
    CHECK(identity_mismatch(host, guest).empty());
    guest.eboot_sha256[31] = 2;
    std::string why = identity_mismatch(host, guest);
    CHECK(why.find("eboot.bin differs") != std::string::npos);
    CHECK(why.find("patches") == std::string::npos && why.find("mods") == std::string::npos);
    guest.eboot_sha256 = host.eboot_sha256;
    // Patches: names only on one side, then the same names with other bytes.
    host.patch_names = {"A", "B", "C"};
    host.patches_hash.fill(3);
    guest.patch_names = {"B", "D"};
    guest.patches_hash.fill(4);
    why = identity_mismatch(host, guest);
    CHECK(why.find("gameplay patches differ (host only: A, C; yours only: D)") != std::string::npos);
    guest.patch_names = host.patch_names;
    why = identity_mismatch(host, guest);
    CHECK(why.find("same names, different contents") != std::string::npos);
    guest.patches_hash = host.patches_hash;
    // Mods: the host has none (zero hash), the guest one.
    guest.mod_names = {"Texture-free param mod (3 files)"};
    guest.mods_hash.fill(5);
    why = identity_mismatch(host, guest);
    CHECK(why.find("gameplay mods differ (host only: -; yours only: Texture-free param mod (3 files))") !=
          std::string::npos);
    CHECK(why.find("patches") == std::string::npos);
    // Many names are cut.
    guest.mod_names.clear();
    for (int i = 0; i < 10; ++i) guest.mod_names.push_back("m" + std::to_string(i));
    CHECK(identity_mismatch(host, guest).find("+4 more") != std::string::npos);
    std::printf("  %s\n", identity_mismatch(host, guest).c_str());

    std::array<std::uint8_t, 32> h{};
    std::vector<std::string> items;
    const std::string text =
        "# comment\r\nhash 00112233445566778899aabbccddeeff00112233445566778899AABBCCDDEEFF\r\n"
        "patch Party: Bells anywhere\r\npatch Player No Dead (Read note)\r\n# cosmetic 60 FPS++\r\nmod x\r\n";
    CHECK(parse_identity_file(text, "patch", &h, &items));
    CHECK(h[0] == 0x00 && h[1] == 0x11 && h[15] == 0xff && h[31] == 0xff);
    CHECK(items.size() == 2 && items[0] == "Party: Bells anywhere" && items[1] == "Player No Dead (Read note)");
    CHECK(parse_identity_file(text, "mod", &h, &items) && items.size() == 1 && items[0] == "x");
    CHECK(!parse_identity_file("patch A\n", "patch", &h, &items));
    CHECK(!parse_identity_file("hash 0011\n", "patch", &h, &items));
    CHECK(!parse_identity_file("hash " + std::string(64, 'g') + "\n", "patch", &h, &items));
    const std::array<std::uint8_t, 32> zero{};
    CHECK(parse_identity_file("hash " + std::string(64, '0') + "\n", "mod", &h, &items) && items.empty() && h == zero);

    // A9: build, params, party hooks; each clause says what to do.
    {
        LinkConfig hb = base_cfg("H", ""), gb = base_cfg("G", "");
        CHECK(identity_mismatch(hb, gb).empty());
        gb.build_id = "0.5 (v0.5-40-g0123456789)";
        std::string w = identity_mismatch(hb, gb);
        CHECK(w.find("bbport build differs (host 0.5 (v0.5-42-gabcdef1234), yours 0.5 (v0.5-40-g0123456789))") !=
              std::string::npos);
        CHECK(w.find("install the same bbport release as the host") != std::string::npos);
        CHECK(w.find("params") == std::string::npos && w.find("party patches") == std::string::npos);
        hb.check_build = false;  // BB_PARTY_CHECK_BUILD=0 on the host
        CHECK(identity_mismatch(hb, gb).empty());
        hb.check_build = true;
        gb.build_id.clear();  // an older build sends none
        CHECK(identity_mismatch(hb, gb).find("unknown: an older build") != std::string::npos);
        gb.build_id = hb.build_id;
        gb.params_sha256[7] ^= 0x40;
        w = identity_mismatch(hb, gb);
        CHECK(w.find("game params differ (gameparam.parambnd.dcx") != std::string::npos);
        CHECK(w.find("use the same game dump (1.09) as the host") != std::string::npos);
        gb.params_sha256 = hb.params_sha256;
        gb.hook_names = {"4p H1 cooperator slots (SessionSlotSummary) @+0x186fe40",
                         "talk IsClientPlayer (guest services) @+0x1234"};
        gb.hooks_hash = hook_set_hash(gb.hook_names);
        w = identity_mismatch(hb, gb);
        CHECK(w.find("party patches differ (host only: travel lamp warp (0x13CDF30) @+0x13cdf30; yours only: talk "
                     "IsClientPlayer (guest services) @+0x1234)") != std::string::npos);
        CHECK(w.find("same bbport build and party options") != std::string::npos);
        std::printf("  A9: %s\n", w.c_str());
        // Order and duplicates do not matter; the empty set hashes to zeros.
        CHECK(hook_set_hash({"b", "a", "a"}) == hook_set_hash({"a", "b"}));
        CHECK(hook_set_hash({"a"}) != hook_set_hash({"a", "b"}));
        CHECK(hook_set_hash({}) == zero);
        // Diagnostics stay out of the hook set.
        for (const char* d : {"ss.info parse log @+0x1", "level-up menu factory (log) @+0x2", "B2 leave trace (0x1ED07A0)",
                              "HUD tick (item-get probe)", "NPC fixture: SOS filter observer (0x1874710)",
                              "action-button registration (test probe)", "Lua event log (LuaEvent_DispatchByName)",
                              "FROM client drop-offline log (0x1ea5ce0)", "item give log (0x17D89F0)"})
            CHECK(diagnostic_hook_name(d));
        for (const char* g : {"4p H1 cooperator slots (SessionSlotSummary)", "travel funnel (SessionWorldTransition)",
                              "main-thread tick (SprjFlipper::Update)", "progress: SetEventFlag",
                              "lamp multiplayer gate +0x49 (party lamps)", "EMEVD instruction dispatch filter",
                              "chalice: carry the slot record into load mode 1"})
            CHECK(!diagnostic_hook_name(g));
    }
}

// Kick by name: BYE Kicked "kicked by the host" to that guest, its slot freed, and the name refused
// for the rest of the session (no auto-rejoin, not even with a fresh link after a restart).
static void test_kick() {
    Recorder hr;
    PartyLink host(base_cfg("Host", ""), recorder_callbacks(hr, "host"));
    std::string err;
    CHECK(host.start_host(&err));
    LinkCallbacks none;
    PartyLink a(base_cfg("Alice", ""), none), b(base_cfg("Bob", ""), none);
    CHECK(a.start_guest("127.0.0.1", host.bound_port(), &err));
    CHECK(b.start_guest("127.0.0.1", host.bound_port(), &err));
    CHECK(a.wait_connected(5000) && b.wait_connected(5000));
    CHECK(!host.kick_name("Nobody"));
    CHECK(!host.kick_name("Host"));
    CHECK(!a.kick_name("Bob"));  // guests cannot kick
    CHECK(host.kick_name("Alice"));
    CHECK(a.wait_state(LinkState::Rejected, 3000));
    CHECK(a.reject_code() == RejectCode::Kicked && a.reject_reason() == "kicked by the host");
    CHECK(wait_for([&] { return hr.left_released.load() == 1; }, 2000));
    CHECK(wait_for([&] { return host.roster().size() == 2; }, 2000));
    CHECK(host.is_banned("Alice") && !host.is_banned("Bob"));
    CHECK(b.state() == LinkState::Connected);
    // A restarted Alice (new link, no token) is refused.
    PartyLink a2(base_cfg("Alice", ""), none);
    CHECK(a2.start_guest("127.0.0.1", host.bound_port(), &err));
    CHECK(a2.wait_state(LinkState::Rejected, 5000) && a2.reject_code() == RejectCode::Kicked);
    std::printf("  kicked Alice: %s; restarted Alice: %s\n", a.reject_reason().c_str(), a2.reject_reason().c_str());
    // reconnect_now: only while Reconnecting.
    CHECK(!b.reconnect_now());
    b.debug_drop_connections();
    CHECK(b.wait_state(LinkState::Reconnecting, 2000));
    b.reconnect_now();
    CHECK(b.wait_connected(3000));
    CHECK(!host.reconnect_now());
}

// The link under the network simulator (BB_NET_SIM, netsim.h; configured process-wide, so this
// runs last): 60 ms one way + 20 jitter + 5 % segment loss (retransmission delays) + a 1.5 s
// total outage mid-stream. Every event arrives once, in order; the link never drops; the ping
// shows the latency.
static void test_netsim_link() {
    bbnet::netsim::Config sc;
    std::string err;
    CHECK(bbnet::netsim::parse("lat=60,jit=20,loss=5,seed=3,outage=1500,at=2500", &sc, &err));
    bbnet::netsim::configure(sc);
    CHECK(bbnet::netsim::enabled());
    Recorder hr, ar;
    // The game's timeouts are 10 s; the tests' 0.7 s would call the 1.5 s outage a lost link.
    LinkConfig hc = base_cfg("Host", "hunter2"), gc = base_cfg("Alice", "hunter2");
    hc.lost_timeout_ms = gc.lost_timeout_ms = 4000;
    hc.connect_timeout_ms = gc.connect_timeout_ms = 4000;
    PartyLink host(hc, recorder_callbacks(hr, "host"));
    CHECK(host.start_host(&err));
    PartyLink a(gc, recorder_callbacks(ar, "alice"));
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(a.start_guest("127.0.0.1", host.bound_port(), &err));
    CHECK(a.wait_connected(8000));
    const auto joined_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  joined in %lld ms under the simulator\n", static_cast<long long>(joined_ms));
    CHECK(joined_ms >= 2 * 60);  // the handshake's round trips pay the latency
    const int kEvents = 300;
    for (int k = 0; k < kEvents; ++k) {
        host.post_event(kBroadcast, "e_" + std::to_string(k), "{}");
        std::this_thread::sleep_for(std::chrono::milliseconds(15));  // ~4.5 s: across the outage
    }
    CHECK(wait_for([&] { return ar.count_event("e_" + std::to_string(kEvents - 1)) >= 1; }, 10000));
    {
        std::lock_guard<std::mutex> lk(ar.mu);
        int next = 0, bad = 0;
        for (const std::string& n : ar.event_names) {
            if (n.rfind("e_", 0) != 0) continue;
            if (n != "e_" + std::to_string(next)) ++bad;
            ++next;
        }
        CHECK(next == kEvents && bad == 0);
        std::printf("  %d events in order through 5 %% loss and a 1.5 s outage (%d out of place)\n", next, bad);
    }
    CHECK(a.state() == LinkState::Connected);
    CHECK(hr.left_kept.load() == 0 && hr.left_released.load() == 0);
    std::uint32_t ping = 0;
    CHECK(wait_for([&] {
        for (const RosterEntry& e : a.roster())
            if (e.slot != kHostSlot && e.ping_ms > 0) ping = e.ping_ms;
        return ping >= 100;
    }, 5000));
    std::printf("  ping %u ms (2 x 60 ms simulated)\n", ping);
    a.stop();
    host.stop();
}

int main() {
    // Loopback-only sockets: no Windows Firewall prompt for a test program (party_sock.h).
    if (!std::getenv("BB_PARTY_LOOPBACK")) {
#if defined(_WIN32)
        _putenv("BB_PARTY_LOOPBACK=1");
#else
        setenv("BB_PARTY_LOOPBACK", "1", 1);
#endif
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("party codes\n");
    test_codes();
    std::printf("crypto\n");
    test_crypto();
    std::printf("stun / env\n");
    test_stun_local();
    std::printf("version check\n");
    test_identity();
    std::printf("loopback party\n");
    test_party();
    std::printf("max players 2, secret-only key\n");
    test_max_players_two();
    test_plain_address_password();
    std::printf("kick by name\n");
    test_kick();
    std::printf("four players (host + 3 guests), game rules\n");
    test_four_players();
    std::printf("party HUD vitals\n");
    test_vitals();
    std::printf("host restart: slots restored\n");
    test_host_restart();
    std::printf("network simulator on the link (last: process-wide)\n");
    test_netsim_link();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    std::printf(g_failures ? "FAILED\n" : "OK\n");
    return g_failures ? 1 : 0;
}

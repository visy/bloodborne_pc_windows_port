// SPDX-License-Identifier: GPL-3.0-or-later
// Party network library (gpu/shim/net): STUN encode/decode and the host's answer, vport framing,
// the network simulator's delay line, loopback UDP through the guest-facing socket functions,
// the host relay, NP signed-in answers, import routing and the guest-callback dispatcher.
// Build and run: ninja -C out/gpu party-net-test && out/gpu/party-net-test.exe
#include "bbnet_internal.h"
#include "net_stun.h"
#include "netsim.h"
#include "json.h"
#include "gpu/bbnet.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

// --- Runtime stand-ins (src/probe.c, runtime_thread.c, runtime.c) ---
static std::atomic<int> g_fs_restores{0}, g_attached{0};
extern "C" {
BBNET_ABI void restore_guest_fs(void) { g_fs_restores.fetch_add(1); }
void runtime_thread_attach_host(const char*) { g_attached.fetch_add(1); }
const char* runtime_symbol(const char* nid) {
    if (!std::strcmp(nid, "TEST1#A#B")) return "sceNetSocket";
    if (!std::strcmp(nid, "TEST2#A#B")) return "sceNpScoreCreateRequest";
    return nullptr;
}
}

static int g_failures = 0;
#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            ++g_failures;                                                          \
        }                                                                          \
    } while (0)

static void set_env(const char* k, const char* v) {
#if defined(_WIN32)
    _putenv_s(k, v);
#else
    setenv(k, v, 1);
#endif
}

static void* fn(const char* name) {
    void* f = reinterpret_cast<void*>(bbnet_resolve(name));
    if (!f) std::fprintf(stderr, "no party function %s\n", name);
    return f;
}

struct SceAddr {
    std::uint8_t len, family;
    std::uint16_t port;
    std::uint32_t addr;
    std::uint16_t vport;
    std::uint8_t zero[6];
};
static SceAddr sce_addr(const char* ip, std::uint16_t port, std::uint16_t vport) {
    SceAddr a{};
    a.len = 16;
    a.family = 2;
    a.port = htons(port);
    inet_pton(AF_INET, ip, &a.addr);
    a.vport = htons(vport);
    return a;
}

using SocketFn = int(BBNET_ABI*)(const char*, int, int, int);
using BindFn = int(BBNET_ABI*)(int, const void*, int);
using SendtoFn = int(BBNET_ABI*)(int, const void*, std::uint64_t, int, const void*, int);
using RecvfromFn = int(BBNET_ABI*)(int, void*, std::uint64_t, int, void*, unsigned*);
using CloseFn = int(BBNET_ABI*)(int);
using SetsockoptFn = int(BBNET_ABI*)(int, int, int, const void*, int);
using GetsocknameFn = int(BBNET_ABI*)(int, void*, unsigned*);
using EpollCreateFn = int(BBNET_ABI*)(const char*, int);
using EpollControlFn = int(BBNET_ABI*)(int, int, int, void*);
using EpollWaitFn = int(BBNET_ABI*)(int, void*, int, int);
using ErrnoFn = int*(BBNET_ABI*)();
using InitFn = int(BBNET_ABI*)();

static void test_stun() {
    using namespace net::stun;
    std::uint8_t req[kMaxRequest], txid[kTxid];
    const std::uint8_t token[kTokenLen] = {1, 2, 3, 4, 5, 6, 7, 8};
    const std::size_t n = build_binding_request(req, txid, true, token);
    CHECK(n == kHeader + 16);
    CHECK(req[0] == 0x00 && req[1] == 0x01);
    CHECK(req[4] == 0x21 && req[5] == 0x12 && req[6] == 0xA4 && req[7] == 0x42);  // the cookie leads the id
    std::uint8_t got_txid[kTxid], got_token[kTokenLen];
    bool hello = false;
    CHECK(parse_binding_request(req, n, got_txid, &hello, got_token));
    CHECK(hello && std::memcmp(got_token, token, kTokenLen) == 0 && std::memcmp(got_txid, txid, kTxid) == 0);
    // A plain request: no HELLO.
    const std::size_t n2 = build_binding_request(req, txid);
    CHECK(n2 == kHeader);
    CHECK(parse_binding_request(req, n2, got_txid, &hello, got_token) && !hello);
    CHECK(!parse_binding_request(req, 10, got_txid, &hello, got_token));

    // The server's answer, with the relay attribute, read back by the client parser.
    std::uint32_t addr = 0;
    inet_pton(AF_INET, "203.0.113.7", &addr);
    Relay relay;
    relay.present = true;
    std::memcpy(relay.token, token, kTokenLen);
    relay.vport = 50001;
    relay.observed_addr = addr;
    relay.observed_port = 41000;
    std::uint8_t resp[kMaxResponse];
    const std::size_t rn = build_binding_response(resp, txid, addr, 41000, &relay);
    CHECK(rn == kMaxResponse);
    std::uint32_t got_addr = 0;
    std::uint16_t got_port = 0;
    Relay got_relay;
    CHECK(parse_binding_response(resp, rn, txid, &got_addr, &got_port, &got_relay));
    CHECK(got_addr == addr && got_port == 41000);
    CHECK(got_relay.present && got_relay.vport == 50001 && got_relay.observed_addr == addr &&
          got_relay.observed_port == 41000 && std::memcmp(got_relay.token, token, kTokenLen) == 0);
    // XOR-MAPPED-ADDRESS alone: hide MAPPED-ADDRESS behind an unknown type.
    resp[kHeader] = 0x80;
    resp[kHeader + 1] = 0x22;
    got_addr = 0;
    got_port = 0;
    CHECK(parse_binding_response(resp, rn, txid, &got_addr, &got_port, nullptr));
    CHECK(got_addr == addr && got_port == 41000);
    // Another transaction's answer is not ours.
    std::uint8_t other[kTxid];
    std::memcpy(other, txid, kTxid);
    other[kTxid - 1] ^= 1;
    CHECK(!parse_binding_response(resp, rn, other, &got_addr, &got_port, nullptr));
    std::printf("stun: ok\n");
}

static void test_netsim() {
    using namespace bbnet::netsim;
    Config c;
    std::string err;
    CHECK(parse("lat=80,jitter=20,loss=2,dup=0.5,reorder=1,seed=7", &c, &err));
    CHECK(c.enabled && c.lat_ms == 80 && c.jitter_ms == 20 && c.loss_pct == 2 && c.dup_pct == 0.5 &&
          c.reorder_pct == 1 && c.seed == 7);
    CHECK(!parse("lat=abc", &c, &err));
    CHECK(!parse("speed=3", &c, &err));
    CHECK(parse("off", &c, &err) && !c.enabled);
    CHECK(parse("dsl", &c, &err) && c.enabled && c.lat_ms == 25);

    // The statistics over many datagrams.
    CHECK(parse("lat=50,jitter=10,loss=10,dup=5,reorder=5,seed=7", &c, &err));
    Queue q(c);
    const auto t0 = Clock::now();
    const int kN = 20000;
    for (int i = 0; i < kN; ++i) {
        std::uint32_t v = static_cast<std::uint32_t>(i);
        q.submit(t0 + std::chrono::milliseconds(i), 1, 0x0100007f, htons(9), &v, sizeof(v));
    }
    std::vector<Packet> out;
    q.take_due(t0 + std::chrono::milliseconds(kN - 1), out);
    CHECK(!out.empty());
    q.take_due(t0 + std::chrono::hours(1), out);
    CHECK(q.size() == 0);
    const Stats& s = q.stats();
    const double loss = 100.0 * s.dropped / kN, dup = 100.0 * s.duplicated / (kN - s.dropped);
    CHECK(loss > 8.5 && loss < 11.5);
    CHECK(dup > 4.0 && dup < 6.0);
    CHECK(out.size() == static_cast<std::size_t>(kN - s.dropped + s.duplicated));
    // Delivered earliest first; every datagram at least lat - jitter late; out of order only
    // where reordered (each reordered datagram can be overtaken by a handful).
    int inversions = 0;
    std::uint32_t last = 0;
    for (std::size_t i = 0; i < out.size(); ++i) {
        std::uint32_t v;
        std::memcpy(&v, out[i].data.data(), 4);
        const auto sent = t0 + std::chrono::milliseconds(v);
        CHECK(out[i].due - sent >= std::chrono::milliseconds(40) - std::chrono::microseconds(1));
        if (i && out[i].due < out[i - 1].due) CHECK(false);
        if (i && v < last) ++inversions;
        last = v;
    }
    CHECK(inversions > 0);
    CHECK(inversions < static_cast<int>(s.reordered) * 2 + 10);

    // Without reorder, jitter alone keeps a destination's order.
    Config c2;
    CHECK(parse("lat=30,jitter=25,seed=3", &c2, &err));
    Queue q2(c2);
    for (int i = 0; i < 2000; ++i) {
        std::uint32_t v = static_cast<std::uint32_t>(i);
        q2.submit(t0 + std::chrono::microseconds(i * 300), 1, 0x0100007f, htons(9), &v, sizeof(v));
    }
    std::vector<Packet> out2;
    q2.take_due(t0 + std::chrono::hours(1), out2);
    CHECK(out2.size() == 2000);
    bool ordered = true;
    for (std::size_t i = 0; i < out2.size(); ++i) {
        std::uint32_t v;
        std::memcpy(&v, out2[i].data.data(), 4);
        if (v != i) ordered = false;
    }
    CHECK(ordered);
    std::printf("netsim: ok (loss %.2f%%, dup %.2f%%, reordered %llu, inversions %d)\n", loss, dup,
                static_cast<unsigned long long>(s.reordered), inversions);
}

// A raw host socket on loopback, for looking at the wire.
static int raw_udp(std::uint16_t* port) {
    const auto fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    ::bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
    socklen_t sl = sizeof(sa);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&sa), &sl);
    *port = ntohs(sa.sin_port);
    return static_cast<int>(fd);
}
static int raw_recv(int fd, std::uint8_t* buf, int len, int timeout_ms) {
#if defined(_WIN32)
    WSAPOLLFD p{static_cast<SOCKET>(fd), POLLIN, 0};
    if (WSAPoll(&p, 1, timeout_ms) <= 0) return -1;
#else
    pollfd p{fd, POLLIN, 0};
    if (poll(&p, 1, timeout_ms) <= 0) return -1;
#endif
    return static_cast<int>(::recv(fd, reinterpret_cast<char*>(buf), len, 0));
}
static void raw_send(int fd, const void* buf, int len, std::uint16_t port) {
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    sa.sin_port = htons(port);
    ::sendto(fd, static_cast<const char*>(buf), len, 0, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
}

static void test_sockets(std::uint16_t party_port) {
    auto init = reinterpret_cast<InitFn>(fn("sceNetInit"));
    auto sock = reinterpret_cast<SocketFn>(fn("sceNetSocket"));
    auto bind = reinterpret_cast<BindFn>(fn("sceNetBind"));
    auto sendto = reinterpret_cast<SendtoFn>(fn("sceNetSendto"));
    auto recvfrom = reinterpret_cast<RecvfromFn>(fn("sceNetRecvfrom"));
    auto close = reinterpret_cast<CloseFn>(fn("sceNetSocketClose"));
    auto setsockopt = reinterpret_cast<SetsockoptFn>(fn("sceNetSetsockopt"));
    auto getsockname = reinterpret_cast<GetsocknameFn>(fn("sceNetGetsockname"));
    auto ep_create = reinterpret_cast<EpollCreateFn>(fn("sceNetEpollCreate"));
    auto ep_ctl = reinterpret_cast<EpollControlFn>(fn("sceNetEpollControl"));
    auto ep_wait = reinterpret_cast<EpollWaitFn>(fn("sceNetEpollWait"));
    auto errno_loc = reinterpret_cast<ErrnoFn>(fn("sceNetErrnoLoc"));
    if (!init || !sock || !bind || !sendto || !recvfrom || !close || !setsockopt || !getsockname || !ep_create ||
        !ep_ctl || !ep_wait || !errno_loc) {
        CHECK(false);
        return;
    }
    CHECK(init() == 0);

    // --- Plain UDP over loopback ---
    const int a = sock("a", 2, 2, 0), b = sock("b", 2, 2, 0);
    CHECK(a > 0 && b > 0);
    SceAddr any = sce_addr("127.0.0.1", 0, 0);
    CHECK(bind(a, &any, 16) == 0);
    CHECK(bind(b, &any, 16) == 0);
    SceAddr bname{};
    unsigned blen = 16;
    CHECK(getsockname(b, &bname, &blen) == 0 && bname.port != 0);
    const int one = 1;
    CHECK(setsockopt(b, 0xffff, 0x1200, &one, 4) == 0);  // non-blocking
    char buf[256];
    SceAddr from{};
    unsigned fromlen = 16;
    CHECK(recvfrom(b, buf, sizeof(buf), 0, &from, &fromlen) == static_cast<int>(0x80410123));  // EAGAIN
    CHECK(*errno_loc() == 35);
    const int ep = ep_create("test", 0);
    struct {
        std::uint32_t events, pad;
        std::uint64_t ident, data;
    } ev{1, 0, 0, 0x1234}, evs[4];
    CHECK(ep_ctl(ep, 1, b, &ev) == 0);
    CHECK(ep_wait(ep, evs, 4, 20000) == 0);  // nothing yet: 20 ms timeout
    CHECK(sendto(a, "hello", 5, 0, &bname, 16) == 5);
    CHECK(ep_wait(ep, evs, 4, 2000000) == 1 && evs[0].data == 0x1234 && (evs[0].events & 1));
    std::memset(buf, 0, sizeof(buf));
    CHECK(recvfrom(b, buf, sizeof(buf), 0, &from, &fromlen) == 5 && std::memcmp(buf, "hello", 5) == 0);
    CHECK(from.addr == htonl(0x7f000001));
    // Blocking receive, woken by a sender thread.
    const int zero = 0;
    CHECK(setsockopt(b, 0xffff, 0x1200, &zero, 4) == 0);
    std::thread t([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        sendto(a, "later", 5, 0, &bname, 16);
    });
    CHECK(recvfrom(b, buf, sizeof(buf), 0, &from, &fromlen) == 5 && std::memcmp(buf, "later", 5) == 0);
    t.join();
    CHECK(close(a) == 0 && close(b) == 0);
    std::printf("udp loopback: ok\n");

    // --- P2P sockets on the party port: vport framing ---
    const int p40 = sock("p2p40", 2, 6, 0), p30 = sock("p2p30", 2, 6, 0);
    SceAddr game40 = sce_addr("0.0.0.0", 3658, 40), game30 = sce_addr("0.0.0.0", 3658, 30);
    CHECK(bind(p40, &game40, 16) == 0);
    CHECK(bind(p30, &game30, 16) == 0);
    CHECK(bbnet::p2p_bound_port() == party_port);
    SceAddr name40{};
    unsigned n40 = 16;
    CHECK(getsockname(p40, &name40, &n40) == 0 && ntohs(name40.port) == party_port && ntohs(name40.vport) == 40);
    // 40 -> ourselves, vport 30: through the wire and the reader.
    SceAddr to30 = sce_addr("127.0.0.1", party_port, 30);
    CHECK(sendto(p40, "abc", 3, 0, &to30, 16) == 3);
    std::memset(buf, 0, sizeof(buf));
    from = SceAddr{};
    CHECK(recvfrom(p30, buf, sizeof(buf), 0, &from, &fromlen) == 3 && std::memcmp(buf, "abc", 3) == 0);
    CHECK(ntohs(from.vport) == 40 && ntohs(from.port) == party_port);
    // The wire: a P2P datagram to a raw socket carries [ff][c3][src][dst].
    std::uint16_t raw_port = 0;
    const int raw = raw_udp(&raw_port);
    SceAddr to_raw = sce_addr("127.0.0.1", raw_port, 30);
    CHECK(sendto(p40, "xyz", 3, 0, &to_raw, 16) == 3);
    std::uint8_t wire[64];
    CHECK(raw_recv(raw, wire, sizeof(wire), 2000) == 7);
    CHECK(wire[0] == 0xff && wire[1] == 0xc3 && wire[2] == 40 && wire[3] == 30 && std::memcmp(wire + 4, "xyz", 3) == 0);
    // Two-byte vports from a peer (flag 0x40 clear) reach vport 30 too.
    const std::uint8_t wide[] = {0xff, 0x83, 0x01, 0x2c, 0x00, 0x1e, 'w', 'd'};
    raw_send(raw, wide, sizeof(wide), party_port);
    CHECK(recvfrom(p30, buf, sizeof(buf), 0, &from, &fromlen) == 2 && std::memcmp(buf, "wd", 2) == 0);
    CHECK(ntohs(from.vport) == 300 && ntohs(from.port) == raw_port);
    // A hole-punch probe never reaches a game socket; the datagram after it does.
    const std::uint8_t probe[8] = {0xfe, 'b', 'b', 'h', 'p', 0, 0, 0};
    raw_send(raw, probe, sizeof(probe), party_port);
    const std::uint8_t after[] = {0xff, 0xc3, 40, 30, 'o', 'k'};
    raw_send(raw, after, sizeof(after), party_port);
    CHECK(recvfrom(p30, buf, sizeof(buf), 0, &from, &fromlen) == 2 && std::memcmp(buf, "ok", 2) == 0);
    // The host answers a STUN Binding Request on the party port.
    std::uint8_t req[net::stun::kMaxRequest], txid[net::stun::kTxid];
    const std::size_t rq = net::stun::build_binding_request(req, txid);
    raw_send(raw, req, static_cast<int>(rq), party_port);
    const int rn = raw_recv(raw, wire, sizeof(wire), 2000);
    std::uint32_t mapped = 0;
    std::uint16_t mapped_port = 0;
    CHECK(rn > 0 && net::stun::parse_binding_response(wire, static_cast<std::size_t>(rn), txid, &mapped, &mapped_port));
    CHECK(mapped == htonl(0x7f000001) && mapped_port == raw_port);
    std::printf("p2p vport framing + stun server: ok\n");

    // --- The session layer's STUN query (to ourselves) with the relay HELLO ---
    std::uint32_t self_addr = 0;
    std::uint16_t self_port = 0;
    bbnet::RelayInfo relay;
    CHECK(bbnet::p2p_stun("127.0.0.1", party_port, 2000, &self_addr, &self_port, true, &relay));
    CHECK(self_addr == htonl(0x7f000001) && self_port == party_port);
    CHECK(relay.present && relay.vport >= 50001);
    std::uint32_t server = 0;
    std::uint16_t relay_vport = 0;
    CHECK(bbnet::p2p_relay(&server, &relay_vport) && relay_vport == relay.vport);
    // Relay round trip: a datagram for our own relay port goes framed to the host (ourselves),
    // is forwarded back as [fb]['r'][port] and reaches vport 30 from 127.0.0.1:<relay port>.
    SceAddr via_relay = sce_addr("127.0.0.1", relay.vport, 30);
    CHECK(sendto(p40, "rly", 3, 0, &via_relay, 16) == 3);
    CHECK(recvfrom(p30, buf, sizeof(buf), 0, &from, &fromlen) == 3 && std::memcmp(buf, "rly", 3) == 0);
    CHECK(ntohs(from.port) == relay.vport && ntohs(from.vport) == 40);
    std::printf("relay: ok (port %u)\n", relay.vport);
    std::printf("status: %s\n", bbnet_status_line());

    CHECK(close(p40) == 0 && close(p30) == 0);
#if defined(_WIN32)
    closesocket(static_cast<SOCKET>(raw));
#else
    ::close(raw);
#endif
}

static void test_np() {
    using GetStateFn = int(BBNET_ABI*)(int, int*);
    using OnlineIdFn = int(BBNET_ABI*)(int, void*);
    using AuthCodeFn = int(BBNET_ABI*)(int, const void*, char*, int*);
    using CheckPlusFn = int(BBNET_ABI*)(int, const void*, std::uint8_t*);
    using CreateReqFn = int(BBNET_ABI*)(int, const char*, const char*, int, const void*, std::int64_t*);
    using ReadFn = int(BBNET_ABI*)(std::int64_t, void*, std::uint64_t);
    using RegStateFn = int(BBNET_ABI*)(void*, void*);
    using CheckCbFn = int(BBNET_ABI*)();
    int state = 0;
    CHECK(reinterpret_cast<GetStateFn>(fn("sceNpGetState"))(1, &state) == 0 && state == 2);
    char id[20];
    CHECK(reinterpret_cast<OnlineIdFn>(fn("sceNpGetOnlineId"))(1, id) == 0 && std::strcmp(id, "Test_Hunter-1") == 0);
    std::uint8_t npid[36];
    CHECK(reinterpret_cast<OnlineIdFn>(fn("sceNpGetNpId"))(1, npid) == 0 &&
          std::strcmp(reinterpret_cast<char*>(npid), "Test_Hunter-1") == 0);
    char code[128];
    int issuer = 0;
    CHECK(reinterpret_cast<AuthCodeFn>(fn("sceNpAuthGetAuthorizationCode"))(1, nullptr, code, &issuer) == 0 &&
          std::strcmp(code, "DUMMY") == 0 && issuer == 10);
    std::uint8_t plus = 0;
    CHECK(reinterpret_cast<CheckPlusFn>(fn("sceNpCheckPlus"))(1, nullptr, &plus) == 0 && plus == 1);
    std::int64_t req = 0;
    CHECK(reinterpret_cast<CreateReqFn>(fn("sceNpWebApiCreateRequest"))(1, "userProfile", "/v1/users/me/friendList", 0,
                                                                         nullptr, &req) == 0);
    char body[128] = {};
    const int got = reinterpret_cast<ReadFn>(fn("sceNpWebApiReadData"))(req, body, sizeof(body) - 1);
    CHECK(got > 0);
    json::Value v;
    std::string err;
    CHECK(json::parse(body, v, err) && v.find("totalResults") && v.find("totalResults")->number == 0 &&
          v.find("friendList") && v.find("friendList")->array.empty());
    // The state callback fires once, on the caller's thread.
    static std::atomic<int> fired{0};
    struct Cb {
        static BBNET_ABI void state(int user, int st, void* np, void* ud) {
            if (user == 1 && st == 2 && np && ud == reinterpret_cast<void*>(0x55)) fired.fetch_add(1);
        }
    };
    CHECK(reinterpret_cast<RegStateFn>(fn("sceNpRegisterStateCallback"))(reinterpret_cast<void*>(&Cb::state),
                                                                         reinterpret_cast<void*>(0x55)) == 0);
    auto check_cb = reinterpret_cast<CheckCbFn>(fn("sceNpCheckCallback"));
    CHECK(check_cb() == 0 && check_cb() == 0 && fired.load() == 1);
    std::printf("np: ok\n");
}

static void test_routing() {
    CHECK(bbnet_resolve("sceNetSocket") != 0);
    CHECK(bbnet_resolve("TEST1#A#B") == bbnet_resolve("sceNetSocket"));  // scoped NID via runtime_symbol
    CHECK(bbnet_resolve("TEST2#A#B") == 0);                              // NpScore stays on the stubs
    CHECK(bbnet_resolve("sceNpScoreCreateRequest") == 0);
    CHECK(bbnet_resolve("sceNpTrophyCreateContext") == 0);
    CHECK(bbnet_resolve("sceNpCommerceDialogOpen") == 0);
    CHECK(bbnet_resolve("sceNpProfileDialogOpen") == 0);
    CHECK(bbnet_resolve("sceNpMatching2Initialize") != 0);  // A3: np_matching2.cpp
    CHECK(bbnet_resolve("sceNpSignalingInitialize") != 0);  // A3: np_signaling.cpp
    CHECK(bbnet_resolve("sceHttpInit") != 0);               // A3: http_hle.cpp
    CHECK(bbnet_resolve("sceSslInit") != 0);
    CHECK(bbnet_resolve("sceNpCmpNpId") == 0);              // the stub's NOT_MATCH contract is kept
    CHECK(bbnet_resolve("sceNpGetState") != 0);
    CHECK(bbnet_resolve("sceNetCtlGetInfo") != 0);
    CHECK(bbnet_resolve("sceKernelUsleep") == 0);
    CHECK(bbnet_resolve("UNKNOWN#A#B") == 0);
    std::printf("routing: ok\n");
}

static std::atomic<std::uint64_t> g_cb_sum{0};
static BBNET_ABI std::uint64_t guest_cb(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d,
                                        std::uint64_t e, std::uint64_t f) {
    g_cb_sum.fetch_add(a + b + c + d + e + f);
    return 0;
}
static void test_dispatcher() {
    const int before = g_fs_restores.load();
    for (int i = 0; i < 10; ++i) bbnet_post_guest_call(reinterpret_cast<uintptr_t>(&guest_cb), 1, 2, 3, 4, 5, 6);
    bbnet::drain_guest_calls();
    CHECK(g_cb_sum.load() == 210);
    CHECK(g_fs_restores.load() - before == 10);
    CHECK(g_attached.load() == 1);
    std::printf("dispatcher: ok\n");
}

static void test_port_remap(std::uint16_t party_port) {
    // With the P2P sockets closed the party port is free again: a plain UDP bind of the game's
    // 3658 lands on it.
    auto sock = reinterpret_cast<SocketFn>(fn("sceNetSocket"));
    auto bind = reinterpret_cast<BindFn>(fn("sceNetBind"));
    auto getsockname = reinterpret_cast<GetsocknameFn>(fn("sceNetGetsockname"));
    auto close = reinterpret_cast<CloseFn>(fn("sceNetSocketClose"));
    if (bbnet::p2p_bound_port()) {
        std::printf("port remap: skipped (party port held by the session layer)\n");
        return;
    }
    const int s = sock("udp3658", 2, 2, 0);
    SceAddr game = sce_addr("0.0.0.0", 3658, 0);
    CHECK(bind(s, &game, 16) == 0);
    SceAddr name{};
    unsigned len = 16;
    CHECK(getsockname(s, &name, &len) == 0 && ntohs(name.port) == party_port);
    CHECK(close(s) == 0);
    std::printf("port remap: ok\n");
}

// The LAN address choice: BB_PARTY_LOCAL_IP / BB_PARTY_LOOPBACK win over every adapter (the
// loopback one included); real adapters with the default route beat Hyper-V / WSL / VPN ones.
static std::uint32_t ipv4(const char* text) {
    std::uint32_t a = 0;
    inet_pton(AF_INET, text, &a);
    return a;
}
static void test_local_address() {
    using bbnet::LanAdapter;
    auto adapter = [](const char* name, const char* ip, bool gw, bool def, bool phys) {
        LanAdapter a;
        a.name = name;
        a.ip = ipv4(ip);
        a.gateway = gw;
        a.default_route = def;
        a.physical = phys;
        return a;
    };
    const LanAdapter lan = adapter("Ethernet / Realtek PCIe 2.5GbE Family Controller", "192.168.1.20", true, true, true);
    const LanAdapter wifi = adapter("Wi-Fi / Intel(R) Wi-Fi 6E AX211", "192.168.1.21", true, false, true);
    const LanAdapter second = adapter("Ethernet 2 / Intel(R) Ethernet I219-V", "10.0.0.5", false, false, true);
    const LanAdapter hyperv =
        adapter("vEthernet (Default Switch) / Hyper-V Virtual Ethernet Adapter", "172.23.16.1", false, false, true);
    const LanAdapter wsl = adapter("vEthernet (WSL (Hyper-V firewall)) / Hyper-V Virtual Ethernet Adapter #2",
                                   "172.23.32.1", true, false, true);
    const LanAdapter vbox =
        adapter("Ethernet 3 / VirtualBox Host-Only Ethernet Adapter", "192.168.56.1", false, false, true);
    const LanAdapter vpn = adapter("OpenVPN TAP / TAP-Windows Adapter V9", "10.8.0.6", true, true, true);
    CHECK(!bbnet::adapter_is_virtual(lan.name) && !bbnet::adapter_is_virtual(wifi.name));
    CHECK(bbnet::adapter_is_virtual(hyperv.name) && bbnet::adapter_is_virtual(wsl.name));
    CHECK(bbnet::adapter_is_virtual(vbox.name) && bbnet::adapter_is_virtual(vpn.name));
    CHECK(bbnet::adapter_is_virtual("Tailscale / Tailscale Tunnel"));
    CHECK(bbnet::lan_adapter_score(lan) > bbnet::lan_adapter_score(wifi));
    CHECK(bbnet::lan_adapter_score(wifi) > bbnet::lan_adapter_score(second));
    for (const LanAdapter* v : {&hyperv, &wsl, &vbox, &vpn}) {
        CHECK(bbnet::lan_adapter_score(*v) >= 0);
        CHECK(bbnet::lan_adapter_score(second) > bbnet::lan_adapter_score(*v));  // even a VPN with the default route
    }
    LanAdapter lo = adapter("Loopback Pseudo-Interface 1", "127.0.0.1", false, false, false);
    lo.loopback = true;
    CHECK(bbnet::lan_adapter_score(lo) < 0);
    CHECK(bbnet::lan_adapter_score(adapter("Ethernet", "169.254.3.4", false, false, true)) < 0);
    LanAdapter teredo = adapter("Teredo", "10.1.1.1", false, false, false);
    teredo.tunnel = true;
    CHECK(bbnet::lan_adapter_score(teredo) < 0);

    // The environment overrides, read at every query (party_runtime sets them late).
    std::string how;
    set_env("BB_PARTY_LOOPBACK", "");
    set_env("BB_MP_LOCAL_TEST", "");
    set_env("BB_PARTY_LOCAL_IP", "127.0.0.1");
    CHECK(bbnet::forced_local_ipv4() == ipv4("127.0.0.1"));
    CHECK(bbnet::query_local_ipv4(&how) == ipv4("127.0.0.1"));
    CHECK(how.find("BB_PARTY_LOCAL_IP") != std::string::npos);
    std::printf("local address (forced): %s\n", how.c_str());
    set_env("BB_PARTY_LOCAL_IP", "10.200.201.202");
    CHECK(bbnet::query_local_ipv4(&how) == ipv4("10.200.201.202"));
    CHECK(how.find("no adapter") != std::string::npos);
    set_env("BB_PARTY_LOCAL_IP", "");
    set_env("BB_PARTY_LOOPBACK", "1");
    std::string source;
    CHECK(bbnet::forced_local_ipv4(&source) == ipv4("127.0.0.1") && source == "BB_PARTY_LOOPBACK=1");
    CHECK(bbnet::query_local_ipv4(&how) == ipv4("127.0.0.1"));
    set_env("BB_PARTY_LOOPBACK", "0");
    set_env("BB_PARTY_LOCAL_IP", "not-an-ip");
    CHECK(bbnet::forced_local_ipv4() == 0);
    set_env("BB_PARTY_LOCAL_IP", "");
    CHECK(bbnet::forced_local_ipv4() == 0);
    const std::uint32_t chosen = bbnet::query_local_ipv4(&how);
    CHECK((ntohl(chosen) >> 24) != 127);  // never the loopback adapter unless forced
    std::printf("local address (auto): %s\n", how.c_str());
    std::printf("local address: ok\n");
}

int main() {
    const std::uint16_t party_port = static_cast<std::uint16_t>(39000 + (std::rand() % 500));
    char port_text[16];
    std::snprintf(port_text, sizeof(port_text), "%u", party_port);
    set_env("BB_PARTY", "host");
    set_env("BB_PARTY_PORT", port_text);
    set_env("BB_PARTY_NAME", "Test_Hunter-1");
    setvbuf(stdout, nullptr, _IONBF, 0);

    test_local_address();
    test_stun();
    test_netsim();
    test_routing();
    test_np();
    test_dispatcher();
    test_port_remap(party_port);
    test_sockets(party_port);
    // json round trip (the host service's bodies).
    json::Value o = json::Value::make_object();
    o.set("totalResults", 0);
    o.set("name", "a\"b");
    json::Value back;
    std::string err;
    CHECK(json::parse(json::dump(o, 0), back, err) && back.find("name")->string == "a\"b");

    if (g_failures) {
        std::fprintf(stderr, "party-net-test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("party-net-test: all passed\n");
    return 0;
}

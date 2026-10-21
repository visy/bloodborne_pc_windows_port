// SPDX-License-Identifier: GPL-3.0-or-later
// NAT traversal (F2): the NAT model (party_nat: classification from two STUN answers, the path
// prediction, the advice text) checked against simulated NATs of every type with the real
// reliable-UDP streams; the network simulator's NAT kinds; the DHT rendezvous (party_rdv:
// bencode, KRPC, an iterative lookup through a simulated DHT, the socket driver against fake DHT
// nodes on loopback) and the whole automatic path: a guest behind the simulated NAT, no guest
// code, connects because the host finds its endpoint in the (fake) DHT and punches it.
// F2 0.7.1: the party port's route table (net/party_route.h: member relay, IPv6) over simulated NATs
// for every host x member x guest triple, its security rules, IPv6-only pairs through stateful
// firewalls, the path chooser (party_netpath.h) and PeerPaths' IPv6 step.
// Build and run: ninja -C out/gpu party-nat-test && out/gpu/party-nat-test.exe
#include "net/netsim.h"
#include "net/party_route.h"
#include "net/party_udp.h"
#include "party/party_code.h"
#include "party/party_crypto.h"
#include "party/party_link.h"
#include "party/party_nat.h"
#include "party/party_netpath.h"
#include "party/party_rdv.h"
#include "party/party_rudp.h"
#include "party/party_sock.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace party;
using rudp::Endpoint;

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

static bool wait_for(const std::function<bool()>& pred, int timeout_ms) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < end) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

static std::uint32_t ip(int a, int b, int c, int d) {
    const std::uint8_t v[4] = {static_cast<std::uint8_t>(a), static_cast<std::uint8_t>(b), static_cast<std::uint8_t>(c),
                               static_cast<std::uint8_t>(d)};
    std::uint32_t r;
    std::memcpy(&r, v, 4);
    return r;
}
static std::uint16_t nbo(std::uint16_t p) { return htons(p); }
static const std::uint8_t* udp_probe() { return bbnet::udp::kProbe; }

// ---- the model against simulated NATs ----

static void test_classify() {
    using namespace nat;
    const std::uint32_t lan = ip(192, 168, 1, 20);
    const Endpoint s1{ip(192, 0, 2, 3), nbo(3478)}, s2{ip(192, 0, 2, 4), nbo(19302)};
    for (Type t : {Type::Open, Type::FullCone, Type::Restricted, Type::PortRestricted, Type::Symmetric}) {
        Box b(t, t == Type::Open ? lan : ip(203, 0, 113, 4));
        const Endpoint a = b.stun(9307, s1), c = b.stun(9307, s2);
        StunSample sa{true, a.addr, ntohs(a.port)}, sb{true, c.addr, ntohs(c.port)};
        const Type got = classify({lan}, 9307, sa, sb);
        const Type want = t == Type::Open ? Type::Open : t == Type::Symmetric ? Type::Symmetric : Type::Cone;
        CHECK(got == want);
        std::printf("  %-22s -> classified %s\n", type_name(t), type_name(got));
    }
    CHECK(classify({lan}, 9307, StunSample{}, StunSample{}) == Type::Unknown);
    CHECK(classify({lan}, 9307, StunSample{true, ip(203, 0, 113, 4), 9307}, StunSample{}) == Type::Cone);
    CHECK(parse_type("sym") == Type::Symmetric && parse_type("full") == Type::FullCone &&
          parse_type("addr") == Type::Restricted && parse_type("port") == Type::PortRestricted &&
          parse_type("bogus") == Type::Unknown);
    // Advice names the remedy.
    const Rendezvous on = Rendezvous::On, off = Rendezvous::Off, na = Rendezvous::NotApplicable;
    CHECK(advice(Type::Symmetric, false, false, on, 9307).find("VPN") != std::string::npos);
    CHECK(advice(Type::Symmetric, true, false, on, 9307).find("Forward UDP+TCP 9307") != std::string::npos);
    CHECK(advice(Type::Cone, true, false, on, 9307).find("automatically") != std::string::npos);
    CHECK(advice(Type::Cone, true, true, on, 9307).find("open") != std::string::npos);
    // The DHT rendezvous is opt-in: off, the host's and the guest's lines say to turn it on on both PCs.
    CHECK(advice(Type::Cone, true, false, off, 9307).find("DHT option on both PCs") != std::string::npos);
    CHECK(advice(Type::Cone, true, false, off, 9307).find("automatically") == std::string::npos);
    CHECK(advice(Type::Restricted, false, false, off, 9307).find(kRendezvousOffHint) != std::string::npos);
    CHECK(advice(Type::Restricted, false, false, na, 9307).find("DHT") == std::string::npos);
    CHECK(advice(Type::Restricted, false, false, on, 9307).find("automatically") != std::string::npos);
}

// One guest connection attempt through two simulated NATs, with the real rudp streams: the host
// (party port 9307 behind `ht`) and the guest (port 5000 behind `gt`). The host's party code
// carries its STUN-mapped endpoint; `known`: the host punches the guest's STUN endpoint (its
// guest code / the rendezvous). Returns whether the stream opened.
static bool nat_attempt(nat::Type ht, nat::Type gt, bool known) {
    using namespace nat;
    const Endpoint s1{ip(192, 0, 2, 3), nbo(3478)};
    Box hb(ht, ht == Type::Open ? ip(203, 0, 113, 1) : ip(203, 0, 113, 1));
    Box gb(gt, ip(198, 51, 100, 2));
    const Endpoint host_pub = hb.stun(9307, s1);   // the party code
    const Endpoint guest_pub = gb.stun(5000, s1);  // the guest code / the rendezvous announce
    rudp::Mux host(11), guest(13);
    host.accepting = true;
    const auto t0 = rudp::Clock::now();
    if (known) host.punch(guest_pub, t0, 30000, 200);
    auto gs = guest.connect(host_pub, t0, 6000);
    std::shared_ptr<rudp::Stream> hs;
    struct Flight {
        int at;
        bool to_host;
        Endpoint from, to;
        std::vector<std::uint8_t> data;
    };
    std::vector<Flight> wire;
    for (int ms = 0; ms < 8000; ms += 5) {
        const auto now = t0 + std::chrono::milliseconds(ms);
        std::vector<rudp::Datagram> out;
        host.poll(now, out);
        for (auto& d : out) wire.push_back({ms + 20, d.to.addr == gb.public_addr(), hb.out(9307, d.to), d.to, d.data});
        out.clear();
        guest.poll(now, out);
        for (auto& d : out) wire.push_back({ms + 20, d.to.addr == hb.public_addr(), gb.out(5000, d.to), d.to, d.data});
        for (auto it = wire.begin(); it != wire.end();) {
            if (it->at > ms) {
                ++it;
                continue;
            }
            // to_host here means "addressed to the other side's public address".
            const bool from_host = it->from.addr == hb.public_addr();
            if (it->to_host) {
                Box& dst = from_host ? gb : hb;
                if (dst.in(it->to.port, it->from) != 0)
                    (from_host ? guest : host).on_datagram(it->from, it->data.data(), it->data.size(), now);
            }
            it = wire.erase(it);
        }
        if (!hs) hs = host.accept();
        if (gs->state() == rudp::Stream::State::Open && hs) return true;
        if (gs->state() == rudp::Stream::State::Failed) return false;
    }
    return false;
}

static void test_matrix() {
    using namespace nat;
    const Type types[] = {Type::Open, Type::FullCone, Type::Restricted, Type::PortRestricted, Type::Symmetric};
    int agree = 0, total = 0, punched_ok = 0, need_code = 0;
    std::printf("  host \\ guest          open  full  addr  port  sym   (o: connects with the endpoint known, "
                "x: never, d: direct)\n");
    for (Type h : types) {
        std::printf("  %-20s", type_name(h));
        for (Type g : types) {
            const bool with = nat_attempt(h, g, true), without = nat_attempt(h, g, false);
            const Side hs{h, false}, gs{g, false};
            const Path pw = predict(hs, gs, true), pn = predict(hs, gs, false);
            const bool ok = with == (pw != Path::None) && without == (pn != Path::None);
            CHECK(ok);
            if (!ok)
                std::fprintf(stderr, "  mismatch host %s guest %s: with %d (%s), without %d (%s)\n", type_name(h),
                             type_name(g), with, path_name(pw), without, path_name(pn));
            agree += ok;
            ++total;
            if (with && !without) ++need_code;
            if (with) ++punched_ok;
            std::printf("  %s   ", without ? "d" : with ? "o" : "x");
        }
        std::printf("\n");
    }
    std::printf("  model agrees with the simulation in %d/%d pairs; %d pairs connect, %d of them only once the "
                "host knows the guest's endpoint\n",
                agree, total, punched_ok, need_code);
    // An open port (UPnP / forward) always gives TCP.
    CHECK(predict({Type::Symmetric, true}, {Type::Symmetric, false}, false) == Path::Tcp);
}

static void test_netsim_kinds() {
    using namespace bbnet::netsim;
    Config c;
    std::string err;
    CHECK(parse("lat=0,nat=sym", &c, &err) && c.nat && c.nat_kind == NatKind::Symmetric);
    CHECK(describe(c).find("nat=sym") != std::string::npos);
    CHECK(parse("lat=0,nat=full", &c, &err) && c.nat_kind == NatKind::FullCone);
    CHECK(parse("lat=0,nat=addr", &c, &err) && c.nat_kind == NatKind::AddrRestricted);
    CHECK(parse("lat=0,nat=1", &c, &err) && c.nat && c.nat_kind == NatKind::PortRestricted);
    CHECK(!parse("lat=0,nat=weird", &c, &err));
    const std::uint32_t a = ip(127, 0, 0, 9), b = ip(127, 0, 0, 10);
    nat_note_out(a, nbo(1111));
    CHECK(nat_allow_in_kind(NatKind::PortRestricted, a, nbo(1111)));
    CHECK(!nat_allow_in_kind(NatKind::PortRestricted, a, nbo(2222)));
    CHECK(nat_allow_in_kind(NatKind::AddrRestricted, a, nbo(2222)));
    CHECK(!nat_allow_in_kind(NatKind::AddrRestricted, b, nbo(1111)));
    CHECK(nat_allow_in_kind(NatKind::FullCone, b, nbo(3333)));
    CHECK(!nat_allow_in_kind(NatKind::Symmetric, a, nbo(2222)));
    Config s;
    CHECK(parse("lat=0,nat=sym", &s, &err));
    reconfigure_for_tests(s);
    CHECK(nat_advertised_port(nbo(9307), 0) == nbo(10307) && nat_advertised_port(nbo(9307), 1) == nbo(11307));
    Config p;
    CHECK(parse("lat=0,nat=1", &p, &err));
    reconfigure_for_tests(p);
    CHECK(nat_advertised_port(nbo(9307), 1) == nbo(9307));
}

// ---- the DHT ----

// A DHT node enough for get_peers / announce_peer (BEP 5).
struct FakeNode {
    rdv::Id id{};
    Endpoint ep;
    std::vector<std::pair<rdv::Id, Endpoint>> known;
    std::map<rdv::Id, std::vector<Endpoint>> store;
    int announces = 0;
    std::string token_for(const Endpoint& from) const {
        return "tk" + std::string(reinterpret_cast<const char*>(&from.addr), 4);
    }
    std::string handle(const Endpoint& from, const std::uint8_t* d, std::size_t n) {
        rdv::BNode m;
        if (!rdv::bdecode(d, n, &m)) return {};
        const rdv::BNode* t = m.get("t");
        const rdv::BNode* q = m.get("q");
        const rdv::BNode* a = m.get("a");
        if (!t || !q || !a) return {};
        rdv::BNode r;
        r.kind = rdv::BNode::Dict;
        r.dict.emplace_back("id", rdv::BNode::str(std::string(reinterpret_cast<const char*>(id.data()), 20)));
        if (q->s == "get_peers") {
            const rdv::BNode* ih = a->get("info_hash");
            if (!ih || ih->s.size() != 20) return {};
            rdv::Id target;
            std::memcpy(target.data(), ih->s.data(), 20);
            r.dict.emplace_back("token", rdv::BNode::str(token_for(from)));
            auto k = known;
            std::sort(k.begin(), k.end(), [&](auto& x, auto& y) { return rdv::closer(target, x.first, y.first); });
            std::string nodes;
            for (std::size_t i = 0; i < k.size() && i < 8; ++i) {
                nodes.append(reinterpret_cast<const char*>(k[i].first.data()), 20);
                nodes.append(reinterpret_cast<const char*>(&k[i].second.addr), 4);
                nodes.append(reinterpret_cast<const char*>(&k[i].second.port), 2);
            }
            r.dict.emplace_back("nodes", rdv::BNode::str(nodes));
            if (auto it = store.find(target); it != store.end()) {
                rdv::BNode vals;
                vals.kind = rdv::BNode::List;
                for (const Endpoint& e : it->second)
                    vals.list.push_back(rdv::BNode::str(std::string(reinterpret_cast<const char*>(&e.addr), 4) +
                                                        std::string(reinterpret_cast<const char*>(&e.port), 2)));
                r.dict.emplace_back("values", vals);
            }
        } else if (q->s == "announce_peer") {
            const rdv::BNode* ih = a->get("info_hash");
            const rdv::BNode* tok = a->get("token");
            const rdv::BNode* port = a->get("port");
            if (!ih || ih->s.size() != 20 || !tok || tok->s != token_for(from) || !port) {
                rdv::BNode e;
                e.kind = rdv::BNode::Dict;
                e.dict.emplace_back("t", *t);
                e.dict.emplace_back("y", rdv::BNode::str("e"));
                return rdv::bencode(e);
            }
            rdv::Id target;
            std::memcpy(target.data(), ih->s.data(), 20);
            Endpoint e{from.addr, htons(static_cast<std::uint16_t>(port->i))};
            auto& v = store[target];
            if (std::find(v.begin(), v.end(), e) == v.end()) v.push_back(e);
            ++announces;
        }
        rdv::BNode msg;
        msg.kind = rdv::BNode::Dict;
        msg.dict.emplace_back("t", *t);
        msg.dict.emplace_back("y", rdv::BNode::str("r"));
        msg.dict.emplace_back("r", r);
        return rdv::bencode(msg);
    }
};

static void test_bencode() {
    using namespace rdv;
    const char* s = "d1:ad2:id20:abcdefghij01234567899:info_hash20:mnopqrstuvwxyz123456e1:q9:get_peers1:t2:aa1:y1:qe";
    BNode m;
    CHECK(bdecode(reinterpret_cast<const std::uint8_t*>(s), std::strlen(s), &m));
    CHECK(m.get("q") && m.get("q")->s == "get_peers" && m.get("a")->get("id")->s.size() == 20);
    CHECK(bencode(m) == s);  // canonical (sorted keys) round trip
    for (const char* bad : {"d1:a", "i12", "l", "5:abc", "d1:ai1ee1", "i-e", "di1ei1ee", "d1:ai1e1:b"}) {
        BNode x;
        CHECK(!bdecode(reinterpret_cast<const std::uint8_t*>(bad), std::strlen(bad), &x));
    }
    // A deep nest is refused, not a stack overflow.
    std::string deep(200, 'l');
    deep += std::string(200, 'e');
    BNode x;
    CHECK(!bdecode(reinterpret_cast<const std::uint8_t*>(deep.data()), deep.size(), &x));
    Id self{}, target{};
    self[0] = 1;
    target[0] = 2;
    const std::string gp = make_get_peers("xy", self, target);
    CHECK(gp.find("9:get_peers") != std::string::npos && gp.find("2:roi1e") != std::string::npos);
    const std::string an = make_announce("xz", self, target, 5555, "tok");
    CHECK(an.find("4:porti5555e") != std::string::npos && an.find("12:implied_porti0e") != std::string::npos);
    // The hash depends on the key only.
    std::uint8_t k1[32] = {1}, k2[32] = {2};
    CHECK(info_hash(k1, 32) == info_hash(k1, 32) && !(info_hash(k1, 32) == info_hash(k2, 32)));
    CHECK(!usable_node({ip(192, 168, 0, 1), nbo(6881)}, false) && !usable_node({ip(127, 0, 0, 1), nbo(1)}, false) &&
          usable_node({ip(127, 0, 0, 1), nbo(1)}, true) && usable_node({ip(203, 0, 113, 10), nbo(6881)}, false) &&
          !usable_node({ip(100, 64, 1, 1), nbo(6881)}, false));
}

// 300 simulated DHT nodes, each knowing 12 others: the guest's announce lands on the nodes
// closest to the hash and the host's lookup (a fresh node id, the same bootstrap) finds it.
static void test_lookup_sim() {
    using namespace rdv;
    std::mt19937 rng(42);
    std::vector<FakeNode> net(300);
    for (std::size_t i = 0; i < net.size(); ++i) {
        for (auto& b : net[i].id) b = static_cast<std::uint8_t>(rng());
        net[i].ep = {ip(192, 0, 2, static_cast<int>(i % 250 + 1)), nbo(static_cast<std::uint16_t>(6881 + i / 250))};
    }
    // Kademlia-like routing tables: up to 3 nodes per shared-prefix length (bucket).
    auto prefix = [](const rdv::Id& a, const rdv::Id& b) {
        for (int i = 0; i < 160; ++i)
            if (((a[i / 8] ^ b[i / 8]) >> (7 - i % 8)) & 1) return i;
        return 160;
    };
    for (std::size_t i = 0; i < net.size(); ++i) {
        std::vector<std::size_t> order(net.size());
        for (std::size_t j = 0; j < order.size(); ++j) order[j] = j;
        std::shuffle(order.begin(), order.end(), rng);
        std::map<int, int> per_bucket;
        for (std::size_t j : order)
            if (j != i && per_bucket[prefix(net[i].id, net[j].id)]++ < 3) net[i].known.emplace_back(net[j].id, net[j].ep);
    }
    std::uint8_t key[32];
    for (auto& b : key) b = static_cast<std::uint8_t>(rng());
    const Id target = info_hash(key, 32);
    auto run = [&](Lookup& lk, const Endpoint& me, double loss) {
        auto t = Clock::now();
        std::uniform_real_distribution<double> u(0, 1);
        for (int step = 0; step < 4000 && !lk.done(); ++step, t += std::chrono::milliseconds(10)) {
            std::vector<rudp::Datagram> out;
            lk.poll(t, out);
            for (auto& d : out) {
                if (u(rng) < loss) continue;
                for (FakeNode& n : net)
                    if (n.ep == d.to) {
                        const std::string r = n.handle(me, d.data.data(), d.data.size());
                        if (!r.empty() && u(rng) >= loss)
                            lk.on_datagram(n.ep, reinterpret_cast<const std::uint8_t*>(r.data()), r.size(), t);
                    }
            }
        }
    };
    Id gid{}, hid{};
    for (auto& b : gid) b = static_cast<std::uint8_t>(rng());
    for (auto& b : hid) b = static_cast<std::uint8_t>(rng());
    Lookup::Options go;
    go.announce = true;
    go.announce_port = 47961;
    const Endpoint guest_pub{ip(198, 51, 100, 2), nbo(5000)};
    Lookup g(target, gid, go, Clock::now());
    g.seed(net[0].ep);
    run(g, guest_pub, 0.05);
    CHECK(g.done() && g.announced() >= 4);
    // The announce reached the true closest nodes.
    auto sorted = net;
    std::sort(sorted.begin(), sorted.end(), [&](auto& a, auto& b) { return closer(target, a.id, b.id); });
    int top_hit = 0;
    for (int i = 0; i < 8; ++i) top_hit += sorted[i].announces > 0;
    Lookup h(target, hid, Lookup::Options{}, Clock::now());
    h.seed(net[1].ep);
    run(h, {ip(203, 0, 113, 1), nbo(9307)}, 0.05);
    const Endpoint want{guest_pub.addr, nbo(47961)};
    CHECK(h.done() && std::find(h.peers().begin(), h.peers().end(), want) != h.peers().end());
    std::printf("  simulated DHT (300 nodes, 5%% loss): guest asked %d nodes, stored at %d (%d of the 8 closest); "
                "host asked %d, found %zu endpoint(s)\n",
                g.queried(), g.announced(), top_hit, h.queried(), h.peers().size());
    CHECK(top_hit >= 5);
    // A second lookup seeded with the first one's closest nodes needs one hop.
    Lookup h2(target, hid, Lookup::Options{}, Clock::now());
    for (const Endpoint& e : h.closest()) h2.seed(e);
    run(h2, {ip(203, 0, 113, 1), nbo(9307)}, 0);
    CHECK(h2.done() && !h2.peers().empty() && h2.queried() <= 12);
    std::printf("  re-poll seeded with the closest nodes: %d queries\n", h2.queried());
}

// Fake DHT nodes on loopback sockets, served by one thread.
struct LoopDht {
    std::vector<FakeNode> nodes;
    std::vector<sock::Socket> socks;
    std::atomic<bool> stop{false};
    std::thread th;
    explicit LoopDht(int n) {
        sock::startup();
        std::mt19937 rng(9);
        nodes.resize(n);
        for (FakeNode& f : nodes) {
            for (auto& b : f.id) b = static_cast<std::uint8_t>(rng());
            sock::Socket s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            ::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a);
            sock::socklen l = sizeof a;
            getsockname(s, reinterpret_cast<sockaddr*>(&a), &l);
            sock::set_nonblocking(s);
            f.ep = {a.sin_addr.s_addr, a.sin_port};
            socks.push_back(s);
        }
        for (FakeNode& f : nodes)
            for (FakeNode& o : nodes)
                if (&f != &o) f.known.emplace_back(o.id, o.ep);
        th = std::thread([this] {
            while (!stop) {
                for (std::size_t i = 0; i < socks.size(); ++i) {
                    for (;;) {
                        std::uint8_t buf[2048];
                        sockaddr_in from{};
                        sock::socklen fl = sizeof from;
                        const long r = ::recvfrom(socks[i], reinterpret_cast<char*>(buf), sizeof buf, 0,
                                                  reinterpret_cast<sockaddr*>(&from), &fl);
                        if (r <= 0) break;
                        const std::string out =
                            nodes[i].handle({from.sin_addr.s_addr, from.sin_port}, buf, static_cast<std::size_t>(r));
                        if (!out.empty())
                            ::sendto(socks[i], out.data(), static_cast<int>(out.size()), 0,
                                     reinterpret_cast<sockaddr*>(&from), sizeof from);
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        });
    }
    ~LoopDht() {
        stop = true;
        th.join();
        for (auto s : socks) sock::close(s);
    }
    std::string bootstrap() const { return "127.0.0.1:" + std::to_string(ntohs(nodes[0].ep.port)); }
};

static void test_rendezvous_sockets() {
    LoopDht dht(5);
    std::uint8_t key[32] = {7, 7, 7};
    const rdv::Id target = rdv::info_hash(key, 32);
    std::mutex mu;
    std::vector<Endpoint> found;
    std::vector<std::string> logs;
    auto log = [&](const std::string& s) {
        std::lock_guard<std::mutex> lk(mu);
        logs.push_back(s);
    };
    rdv::Rendezvous host(target, {dht.bootstrap()}, true, log), guest(target, {dht.bootstrap()}, true, log);
    CHECK(host.start_host([&](const Endpoint& e) {
        std::lock_guard<std::mutex> lk(mu);
        if (std::find(found.begin(), found.end(), e) == found.end()) found.push_back(e);
    }, 200));
    std::atomic<bool> want{true};
    CHECK(guest.start_guest(4321, [&] { return want.load(); }, 300));
    const Endpoint expect{htonl(INADDR_LOOPBACK), htons(4321)};
    CHECK(wait_for([&] {
        std::lock_guard<std::mutex> lk(mu);
        return std::find(found.begin(), found.end(), expect) != found.end();
    }, 10000));
    want = false;
    host.stop();
    guest.stop();
    CHECK(guest.announces() > 0 && host.lookups() > 0);
    std::printf("  loopback DHT (5 nodes): host found the guest's 127.0.0.1:4321 after %d lookup(s); %d announce answer(s)\n",
                host.lookups(), guest.announces());
}

// The whole automatic path: the host behind the simulated NAT (netsim nat=1: no TCP listener,
// unsolicited UDP dropped), a guest with no guest code. The guest's link fails over to UDP and
// announces its endpoint; the host's rendezvous finds it and punches; the link comes up.
static LinkConfig cfg_for(const char* name) {
    LinkConfig c;
    c.name = name;
    c.password = "hunter2";
    c.bind_addr = "127.0.0.1";
    c.port = 0;
    c.secret = {1, 2, 3, 4, 5, 6, 7, 8};
    c.lost_timeout_ms = 15000;
    c.connect_timeout_ms = 15000;
    c.tcp_fallback_ms = 500;
    return c;
}

static void test_auto_punch_link() {
    bbnet::netsim::Config sc;
    std::string err;
    CHECK(bbnet::netsim::parse("lat=0,nat=1", &sc, &err));
    bbnet::netsim::reconfigure_for_tests(sc);
    LoopDht dht(4);
    LinkCallbacks cb;
    if (std::getenv("PARTY_TEST_VERBOSE"))
        cb.on_log = [](const std::string& l) {
            std::printf("    [%lld] %s\n",
                        static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                   std::chrono::steady_clock::now().time_since_epoch())
                                                   .count() %
                                               100000),
                        l.c_str());
        };
    PartyLink host(cfg_for("Host"), cb);
    CHECK(host.start_host(&err));
    PartyLink g(cfg_for("Carol"), cb);
    CHECK(g.start_guest("127.0.0.1", host.bound_port(), &err));
    CHECK(wait_for([&] { return g.udp_local_port() != 0; }, 5000));
    const crypto::Key k = crypto::derive_party_key("hunter2", cfg_for("x").secret.data());
    const rdv::Id target = rdv::info_hash(k.data(), k.size());
    std::atomic<int> punched{0};
    rdv::Rendezvous hr(target, {dht.bootstrap()}, true, nullptr), gr(target, {dht.bootstrap()}, true, nullptr);
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(hr.start_host([&](const Endpoint& e) {
        host.punch_guest(e.addr, e.port);
        ++punched;
    }, 300));
    CHECK(gr.start_guest(g.udp_local_port(), [&] { return g.state() != LinkState::Connected; }, 1000));
    CHECK(g.wait_connected(20000));
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(g.connected_over_udp() && punched > 0);
    std::printf("  guest without a code behind the simulated NAT: connected over UDP %lld ms after the "
                "rendezvous started (host punched %d time(s))\n",
                static_cast<long long>(ms), punched.load());
    hr.stop();
    gr.stop();
    g.stop();
    host.stop();
}

// ---- F2 0.7.1: the route table (member relay, IPv6) and the path chooser ----

using bbnet::udp::Ep6;
using bbnet::udp::Ip6;
using bbnet::udp::Router;
using bbnet::udp::Wire;

static Ip6 ip6(const char* text) {
    Ip6 a;
    const bool ok = bbnet::udp::parse_ip6(text, &a);
    CHECK(ok);
    return a;
}

static void test_route_basics() {
    using namespace bbnet::udp;
    // IPv6 text both ways.
    for (const char* t : {"2001:db8::1", "::1", "2a02:8108:1c0:5e00:1d2:3e4:5f6:789", "fe80::1", "2001:db8:0:1::"}) {
        Ip6 a;
        CHECK(parse_ip6(t, &a));
        CHECK(ip6_text(a) == t);
    }
    Ip6 a;
    CHECK(parse_ip6("[2001:DB8::A]", &a) && ip6_text(a) == "2001:db8::a");
    CHECK(parse_ip6("fe80::1%12", &a) && ip6_text(a) == "fe80::1");
    for (const char* bad : {"", ":", "1:2:3", "2001:db8::1::2", "12345::", "1:2:3:4:5:6:7:8:9", "g::1", ":1::"}) CHECK(!parse_ip6(bad, &a));
    CHECK(ip6_global(ip6("2001:db8::1")) && !ip6_global(ip6("fe80::1")) && !ip6_global(ip6("fd7a::1")) &&
          ip6_loopback(ip6("::1")));
    // Synthetic keys: 240/4 (never 255.255.255.255), the same on every PC, the port kept.
    std::uint32_t k1 = 0, k2 = 0;
    std::uint16_t p1 = 0, p2 = 0;
    synthetic_key(Ep6{ip6("2001:db8::7"), nbo(9307)}, &k1, &p1);
    synthetic_key(Ep6{ip6("2001:db8::7"), nbo(9307)}, &k2, &p2);
    CHECK(k1 == k2 && p1 == nbo(9307) && is_synthetic(k1) && !is_synthetic(ip(203, 0, 113, 1)));
    CHECK(reinterpret_cast<const std::uint8_t*>(&k1)[0] >= 240 && reinterpret_cast<const std::uint8_t*>(&k1)[0] <= 247);
    // A synthetic key without an IPv6 route is never put on the IPv4 wire.
    Router r;
    std::vector<Wire> out;
    const std::uint8_t d[4] = {1, 2, 3, 4};
    r.outgoing(k1, p1, d, 4, rudp::Clock::now(), &out);
    CHECK(out.empty());
    r.set_ipv6(true);
    Wire in;
    in.v6 = true;
    in.addr6 = ip6("2001:db8::7");
    in.port = nbo(9307);
    in.data.assign(d, d + 4);
    std::vector<std::uint8_t> got;
    std::uint32_t sa = 0;
    std::uint16_t sp = 0;
    bool v6 = false;
    CHECK(r.incoming(in, rudp::Clock::now(), &got, &sa, &sp, &out, &v6) && sa == k1 && sp == p1 && v6 && got.size() == 4);
    r.outgoing(k1, p1, d, 4, rudp::Clock::now(), &out);  // the reply goes back over IPv6
    CHECK(out.size() == 1 && out[0].v6 && out[0].addr6 == in.addr6 && out[0].port == nbo(9307));
    // A bound key wins over the synthetic one, both ways.
    const std::uint32_t host = ip(203, 0, 113, 9);
    r.bind_v6(host, nbo(9307), Ep6{ip6("2001:db8::7"), nbo(9307)}, true);
    CHECK(r.incoming(in, rudp::Clock::now(), &got, &sa, &sp, &out) && sa == host && sp == nbo(9307));
    out.clear();
    r.route_v6(host, nbo(9307), false);  // a candidate only: IPv4 out, IPv6 still recognised in
    r.outgoing(host, nbo(9307), d, 4, rudp::Clock::now(), &out);
    CHECK(out.size() == 1 && !out[0].v6 && out[0].addr == host);
    r.set_ipv6(false);  // IPv6 off: nothing from the IPv6 socket gets in
    CHECK(!r.incoming(in, rudp::Clock::now(), &got, &sa, &sp, &out));
    for (auto& n : r.take_notes()) (void)n;

    // The party code's IPv6 tail and the BBG6 guest code.
    std::array<std::uint8_t, 8> secret{1, 2, 3, 4, 5, 6, 0, 0};
    std::array<std::uint8_t, 16> h6{};
    std::memcpy(h6.data(), ip6("2001:db8:1:2::abcd").b.data(), 16);
    const std::string c6 = make_party_code({203, 0, 113, 9}, 9307, secret, 0, &h6);
    const std::string c4 = make_party_code({203, 0, 113, 9}, 9307, secret, 0);
    CHECK(c6.rfind(c4 + "+6-", 0) == 0);
    PartyCode pc;
    std::string err;
    CHECK(decode_party_code(c6, &pc, &err) && pc.has_ipv6 && pc.ipv6 == h6 && pc.host == "203.0.113.9" &&
          pc.secret == secret);
    CHECK(decode_party_code(c4, &pc, &err) && !pc.has_ipv6);
    std::string typo = c6;
    typo[typo.size() - 3] = typo[typo.size() - 3] == 'A' ? 'B' : 'A';
    CHECK(!decode_party_code(typo, &pc, &err) && err.find("IPv6") != std::string::npos);
    CHECK(!decode_party_code("203.0.113.9:9307+6-AAAA", &pc, &err));
    std::printf("  party code with IPv6: %s\n", c6.c_str());
    std::uint8_t g6[16], back[16];
    std::memcpy(g6, ip6("2001:db8::42").b.data(), 16);
    const std::string gc6 = rudp::encode_guest_code6(g6, nbo(9307));
    std::uint16_t gp = 0;
    CHECK(rudp::decode_guest_code6(gc6, back, &gp) && std::memcmp(back, g6, 16) == 0 && gp == nbo(9307));
    CHECK(rudp::decode_guest_code6(" " + gc6 + " ", back, &gp));
    std::string gbad = gc6;
    gbad[6] = gbad[6] == '0' ? '1' : '0';
    CHECK(!rudp::decode_guest_code6(gbad, back, &gp) && !rudp::decode_guest_code6(rudp::encode_guest_code(1, 2), back, &gp));
    std::printf("  guest code with IPv6: %s\n", gc6.c_str());
}

static void test_chooser() {
    using namespace netpath;
    Chooser::Config cfg;
    cfg.relay_after_ms = 20000;
    cfg.stick_failures = 2;
    Chooser c(cfg);
    const auto t0 = Clock::now();
    auto at = [&](int ms) { return t0 + std::chrono::milliseconds(ms); };
    c.start(t0);
    CHECK(c.current().route == Route::Direct);  // no IPv6: direct only
    c.set_ipv6(true);
    CHECK(c.current().route == Route::Ipv6);  // not tried yet: IPv6 first (TCP, then UDP over IPv6)
    CHECK(c.add_relay(ip(198, 51, 100, 7), nbo(9307), "guest code") && !c.add_relay(ip(198, 51, 100, 7), nbo(9307), "x"));
    CHECK(c.failed(at(7000)).route == Route::Direct);
    CHECK(c.failed(at(14000)).route == Route::Ipv6);  // relays wait for 20 s
    CHECK(c.failed(at(21000)).route == Route::Direct);
    const Step r = c.failed(at(28000));
    CHECK(r.route == Route::Relay && r.relay == 0);
    CHECK(c.describe(r).find("198.51.100.7:9307") != std::string::npos);
    CHECK(c.failed(at(35000)).route == Route::Ipv6);  // round again: direct steps keep their turns
    CHECK(c.failed(at(42000)).route == Route::Direct);
    CHECK(c.failed(at(49000)).route == Route::Relay);
    c.connected();  // the relay worked: reconnects stay on it
    c.lost();
    CHECK(c.failed(at(60000)).route == Route::Relay);
    CHECK(c.failed(at(65000)).route == Route::Ipv6);  // two failed reconnects: the rounds from the top
    CHECK(c.relays()[0].failures == 2);
    // Without IPv6 or relays it stays direct.
    Chooser d;
    d.start(t0);
    CHECK(d.failed(at(30000)).route == Route::Direct && d.failed(at(60000)).route == Route::Direct);
}

// Three (or two) players behind simulated NATs, each with the real route table and reliable-UDP
// streams, on one simulated wire (15 ms one way). IPv6: an address and a stateful firewall per player.
namespace sim3 {

struct Party {
    std::string name;
    nat::Box box;
    Router router;
    rudp::Mux mux;
    std::shared_ptr<rudp::Stream> stream;  // guest: its link; host: the accepted one
    rudp::Endpoint accepted_from;
    std::uint64_t game_in = 0;  // non-link datagrams that reached the layers above
    rudp::Endpoint last_game_src;  // where they came from (a game answers its peers there)
    Ip6 a6;
    bool has6 = false, fw_open6 = false;
    std::set<std::pair<Ip6, std::uint16_t>> sent6;  // the stateful IPv6 firewall's flows
    Party(std::string n, nat::Type t, std::uint32_t pub, std::uint32_t seed)
        : name(std::move(n)), box(t, pub, 40000), mux(seed) {}
};

struct Net {
    struct Flight {
        int at;
        bool v6;
        rudp::Endpoint from, to;  // IPv4 (public, after the sender's NAT)
        Ip6 from6, to6;
        std::uint16_t from6_port = 0, to6_port = 0;
        std::vector<std::uint8_t> data;
    };
    std::vector<Party*> parties;
    std::vector<Flight> wire;
    int now = 0;
    rudp::Clock::time_point t0 = rudp::Clock::now();
    rudp::Clock::time_point t() const { return t0 + std::chrono::milliseconds(now); }

    void put(Party& p, const std::vector<Wire>& out) {
        for (const Wire& w : out) {
            Flight f;
            f.at = now + 15;
            f.v6 = w.v6;
            f.data = w.data;
            if (!w.v6) {
                f.to = {w.addr, w.port};
                f.from = p.box.out(9307, f.to);
            } else {
                if (!p.has6) continue;
                f.from6 = p.a6;
                f.from6_port = nbo(9307);
                f.to6 = w.addr6;
                f.to6_port = w.port;
                p.sent6.insert({w.addr6, w.port});
            }
            wire.push_back(std::move(f));
        }
    }
    void send(Party& p, std::uint32_t addr, std::uint16_t port, const std::uint8_t* d, std::size_t n) {
        std::vector<Wire> out;
        p.router.outgoing(addr, port, d, n, t(), &out);
        put(p, out);
    }
    void deliver(Party& p, const Wire& in) {
        std::vector<std::uint8_t> d;
        std::uint32_t sa = 0;
        std::uint16_t sp = 0;
        std::vector<Wire> rep;
        const bool up = p.router.incoming(in, t(), &d, &sa, &sp, &rep);
        put(p, rep);
        if (!up) return;
        if (rudp::is_rudp(d.data(), d.size())) p.mux.on_datagram({sa, sp}, d.data(), d.size(), t());
        else {
            ++p.game_in;
            p.last_game_src = {sa, sp};
        }
    }
    void step() {
        for (std::size_t i = 0; i < wire.size();) {
            if (wire[i].at > now) {
                ++i;
                continue;
            }
            Flight f = std::move(wire[i]);
            wire.erase(wire.begin() + static_cast<std::ptrdiff_t>(i));
            for (Party* p : parties) {
                Wire in;
                if (!f.v6) {
                    if (f.to.addr != p->box.public_addr()) continue;
                    if (!p->box.in(f.to.port, f.from)) break;  // its NAT drops it
                    in.addr = f.from.addr;
                    in.port = f.from.port;
                } else {
                    if (!p->has6 || !(f.to6 == p->a6)) continue;
                    if (!p->fw_open6 && !p->sent6.count({f.from6, f.from6_port})) break;  // its firewall drops it
                    in.v6 = true;
                    in.addr6 = f.from6;
                    in.port = f.from6_port;
                }
                in.data = std::move(f.data);
                deliver(*p, in);
                break;
            }
        }
        for (Party* p : parties) {
            std::vector<Wire> out;
            p->router.poll(t(), &out);
            put(*p, out);
            std::vector<rudp::Datagram> dg;
            p->mux.poll(t(), dg);
            for (const rudp::Datagram& d : dg) send(*p, d.to.addr, d.to.port, d.data.data(), d.data.size());
            if (p->mux.accepting && !p->stream) {
                if (auto s = p->mux.accept()) {
                    p->stream = s;
                    p->accepted_from = s->peer();
                }
            }
        }
        now += 5;
    }
};

crypto::Key key_of_pw(const char* pw) {
    std::uint8_t secret[8] = {9, 8, 7, 6, 5, 4, 0, 0};
    return crypto::relay_key(crypto::derive_party_key(pw, secret));
}
Router::Mac mac(const crypto::Key& k) {
    return [k](const std::uint8_t* d, std::size_t n, std::uint8_t out[16]) { crypto::mac16(k, d, n, out); };
}

struct RelayResult {
    bool link = false, data = false;
    rudp::Endpoint host_saw;
};

// The guest reaches the host only through the member: the host and the member already play
// (they exchange game datagrams; the host knows the member's endpoint), the member relays. With
// `punch`, the member opens its NAT towards the guest (the host had its guest code / DHT entry:
// relay_punch). `member_key` != the guest's: a party member with another key.
RelayResult relay_run(nat::Type ht, nat::Type mt, nat::Type gt, bool punch, const crypto::Key& gkey,
                      const crypto::Key& mkey) {
    const rudp::Endpoint s1{ip(192, 0, 2, 3), nbo(3478)};
    Party H("host", ht, ip(203, 0, 113, 1), 21), M("member", mt, ip(192, 0, 2, 50), 23), G("guest", gt, ip(198, 51, 100, 2), 29);
    Net net;
    net.parties = {&H, &M, &G};
    const rudp::Endpoint hp = H.box.stun(9307, s1), mp = M.box.stun(9307, s1), gp = G.box.stun(9307, s1);
    H.mux.accepting = true;
    H.router.accept_relayed(true, [&](std::uint32_t a, std::uint16_t) { return a == M.box.public_addr(); });
    const std::uint8_t game[8] = {0xff, 0xc3, 40, 40, 1, 2, 3, 4};
    bool m_heard_host = false;
    RelayResult res;
    for (; net.now < 12000;) {
        // The host and the member play together (game datagrams both ways, the host punching the
        // member as for its guest code); the member opens its NAT towards the guest.
        if (net.now % 100 == 0) {
            // The host's game addresses the member where its STUN answer saw it (its room record).
            const rudp::Endpoint to_m = H.last_game_src.addr ? H.last_game_src : mp;
            net.send(H, to_m.addr, to_m.port, game, sizeof game);
            net.send(M, hp.addr, hp.port, game, sizeof game);
            if (punch) net.send(M, gp.addr, gp.port, udp_probe(), 8);
        }
        if (!m_heard_host && M.game_in > 0) {
            m_heard_host = true;  // its link is up: it serves
            M.router.serve(true, hp.addr, hp.port, mac(mkey), "Member", {});
        }
        if (net.now == 1500) {
            G.router.relay_use(mp.addr, mp.port, hp.addr, hp.port, mac(gkey), net.t());
            G.stream = G.mux.connect(hp, net.t(), 8000);
        }
        net.step();
        if (G.stream && G.stream->state() == rudp::Stream::State::Open && H.stream && !res.link) {
            res.link = true;
            res.host_saw = H.accepted_from;
            std::vector<std::uint8_t> blob(40000);
            for (std::size_t i = 0; i < blob.size(); ++i) blob[i] = static_cast<std::uint8_t>(i * 7);
            G.stream->write(blob.data(), blob.size());
            H.stream->write(blob.data(), blob.size());
        }
        if (res.link && G.stream->readable() == 40000 && H.stream->readable() == 40000) {
            res.data = true;
            break;
        }
    }
    return res;
}

}  // namespace sim3

static void test_member_relay_matrix() {
    using namespace nat;
    const Type types[] = {Type::Open, Type::FullCone, Type::Restricted, Type::PortRestricted, Type::Symmetric};
    const crypto::Key k = sim3::key_of_pw("hunter");
    int agree = 0, total = 0, only_relay = 0, only_relay_nocode = 0;
    std::printf("  host / member -> guest (o: through the member relay only, r: relay works, d: direct works too, x: neither)\n");
    for (bool punch : {true, false}) {
        std::printf("  %s\n", punch ? "the host has the guest's endpoint (guest code / DHT): the member punches it too"
                                    : "no guest code, no DHT: nobody knows the guest's endpoint");
        for (Type h : types) {
            for (Type m : types) {
                std::printf("    %-16s %-16s", type_name(h), type_name(m));
                for (Type g : types) {
                    // The model: the member is in the party (the host reached it, punching its code), and
                    // the guest reaches the member as it would a host that punches it (or one open to all).
                    const bool m_in = nat_attempt(h, m, true);
                    const bool g_to_m = nat_attempt(m, g, punch);
                    const bool want = m_in && g_to_m;
                    const sim3::RelayResult r = sim3::relay_run(h, m, g, punch, k, k);
                    const bool ok = r.link && r.data;
                    CHECK(ok == want);
                    if (ok != want)
                        std::fprintf(stderr, "  mismatch host %s member %s guest %s punch %d: relay %d, model %d\n",
                                     type_name(h), type_name(m), type_name(g), punch, ok, want);
                    agree += ok == want;
                    ++total;
                    const bool direct = nat_attempt(h, g, punch);
                    if (ok && !direct) (punch ? only_relay : only_relay_nocode)++;
                    std::printf(" %s", ok ? (direct ? "d" : "o") : "x");
                }
                std::printf("\n");
            }
        }
    }
    std::printf("  relay agrees with the model in %d/%d triples; only the member relay connects in %d (with the "
                "guest's endpoint known) + %d (without) triples\n",
                agree, total, only_relay, only_relay_nocode);
    CHECK(only_relay > 0 && only_relay_nocode > 0);
    // The relayed guest is at its endpoint as the member sees it (its own address: its password checks
    // and lockout count against it at the host, not against the member).
    const sim3::RelayResult r = sim3::relay_run(Type::PortRestricted, Type::FullCone, Type::Symmetric, true, k, k);
    CHECK(r.link && r.host_saw.addr == ip(198, 51, 100, 2));
}

static void test_member_relay_security() {
    using namespace nat;
    const crypto::Key good = sim3::key_of_pw("hunter"), other = sim3::key_of_pw("not the password");
    // Another party key: the member refuses (Q5), nothing reaches the host.
    {
        const sim3::RelayResult r = sim3::relay_run(Type::PortRestricted, Type::Open, Type::PortRestricted, true, other, good);
        CHECK(!r.link);
    }
    // The handshake and its refusals on a direct wire (no NATs).
    const std::uint32_t M = ip(192, 0, 2, 50), G = ip(198, 51, 100, 2), H = ip(203, 0, 113, 1), X = ip(198, 51, 100, 66);
    const std::uint16_t P = nbo(9307);
    auto t0 = rudp::Clock::now();
    Router m, g, h;
    std::mutex tmu;
    AuthThrottle at;
    std::int64_t clock_ms = 0;
    Router::Throttle th;
    th.locked = [&](std::uint32_t a, std::int64_t* rem) { return at.Locked(a, clock_ms, rem); };
    th.fail = [&](std::uint32_t a) { at.Fail(a, clock_ms); };
    th.success = [&](std::uint32_t a) { at.Success(a); };
    // Exchange: deliver everything `from` sent to `to` (keys = the routers' own addresses here).
    auto pump = [&](Router& from, std::uint32_t from_addr, std::vector<Wire>& out, Router& to, std::uint32_t to_addr,
                    std::vector<Wire>* replies, std::vector<std::vector<std::uint8_t>>* delivered = nullptr) {
        for (Wire& w : out) {
            if (w.addr != to_addr) continue;
            Wire in;
            in.addr = from_addr;
            in.port = P;
            in.data = w.data;
            std::vector<std::uint8_t> d;
            std::uint32_t sa;
            std::uint16_t sp;
            if (to.incoming(in, t0, &d, &sa, &sp, replies) && delivered) delivered->push_back(d);
        }
        out.clear();
        (void)from;
    };
    std::vector<Wire> gout, mout, hout;
    // Not serving: a refusal (smaller than the hello: no amplification).
    g.relay_use(M, P, H, P, sim3::mac(good), t0);
    g.poll(t0, &gout);
    CHECK(gout.size() == 1 && gout[0].data.size() == 40);
    pump(g, G, gout, m, M, &mout);
    CHECK(mout.size() == 1 && mout[0].data.size() < 40);
    pump(m, M, mout, g, G, &gout);
    int code = 0;
    CHECK(g.relay_state(nullptr, &code) == Router::ClientState::Refused && code == Router::kNotServing);
    // Serving: the handshake, the session, the name.
    m.serve(true, H, P, sim3::mac(good), "Hunter1", th);
    g.relay_use(M, P, H, P, sim3::mac(good), t0);
    for (int i = 0; i < 3; ++i) {
        g.poll(t0, &gout);
        pump(g, G, gout, m, M, &mout);
        pump(m, M, mout, g, G, &gout);
    }
    std::string name;
    CHECK(g.relay_state(&name) == Router::ClientState::Ready && name == "Hunter1" && m.served().size() == 1);
    // The guest's datagram for the host: T to the member, V (with the guest's endpoint) to the host.
    const std::uint8_t dgram[5] = {0xfa, 'L', 1, 2, 3};
    g.outgoing(H, P, dgram, 5, t0, &gout);
    CHECK(gout.size() == 1 && gout[0].addr == M && gout[0].data[1] == 'T');
    pump(g, G, gout, m, M, &mout);
    CHECK(mout.size() == 1 && mout[0].addr == H && mout[0].data[1] == 'V');
    // The host: not from a member -> dropped; from the member -> delivered from the guest's endpoint.
    std::vector<std::vector<std::uint8_t>> got;
    h.accept_relayed(true, [&](std::uint32_t a, std::uint16_t) { return a == M; });
    std::vector<Wire> stray = mout;
    pump(m, X, stray, h, H, &hout, &got);  // the same frame from a stranger's address
    CHECK(got.empty() && h.host_routes().empty());
    pump(m, M, mout, h, H, &hout, &got);
    CHECK(got.size() == 1 && got[0].size() == 5 && h.relayed_guest(G, P));
    // The host's answer to the guest goes back through the member, and only from the host's endpoint.
    h.outgoing(G, P, dgram, 5, t0, &hout);
    CHECK(hout.size() == 1 && hout[0].addr == M && hout[0].data[1] == 'v');
    std::vector<Wire> forged = hout;
    pump(h, X, forged, m, M, &mout);  // a 'v' from someone else: dropped
    CHECK(mout.empty());
    pump(h, H, hout, m, M, &mout);
    CHECK(mout.size() == 1 && mout[0].addr == G && mout[0].data[1] == 't');
    got.clear();
    pump(m, M, mout, g, G, &gout, &got);
    CHECK(got.size() == 1 && got[0].size() == 5);
    // A T frame from a stranger (no session): nothing goes to the host.
    {
        std::vector<Wire> t{Wire{}};
        t[0].addr = M;
        t[0].data = {0xfc, 'T', 0, 0, 0, 1, 0xfa, 'L', 1};
        pump(g, X, t, m, M, &mout);
        CHECK(mout.empty());
    }
    // A member cannot pose as a guest the host hears directly.
    {
        Router h2;
        h2.accept_relayed(true, [&](std::uint32_t a, std::uint16_t) { return a == M; });
        Wire direct;
        direct.addr = G;
        direct.port = P;
        direct.data = {0xff, 0xc3, 40, 40};
        std::vector<std::uint8_t> d;
        std::uint32_t sa;
        std::uint16_t sp;
        std::vector<Wire> rr;
        CHECK(h2.incoming(direct, t0, &d, &sa, &sp, &rr));
        Wire v;
        v.addr = M;
        v.port = P;
        v.data = {0xfc, 'V'};
        v.data.insert(v.data.end(), reinterpret_cast<const std::uint8_t*>(&G), reinterpret_cast<const std::uint8_t*>(&G) + 4);
        v.data.insert(v.data.end(), reinterpret_cast<const std::uint8_t*>(&P), reinterpret_cast<const std::uint8_t*>(&P) + 2);
        v.data.push_back(0xff);
        CHECK(!h2.incoming(v, t0 + std::chrono::seconds(2), &d, &sa, &sp, &rr));
        CHECK(h2.incoming(v, t0 + std::chrono::seconds(12), &d, &sa, &sp, &rr) && sa == G);  // quiet for 10 s: allowed
        // A running route keeps working when the guest is heard directly too (live relay2: the host's
        // punch opened its NAT, the guest's probe answer came straight in, the relay must not stop).
        CHECK(h2.incoming(direct, t0 + std::chrono::seconds(13), &d, &sa, &sp, &rr));
        CHECK(h2.incoming(v, t0 + std::chrono::seconds(14), &d, &sa, &sp, &rr) && sa == G);
        // ... but another member cannot take it over meanwhile.
        Wire v2 = v;
        v2.addr = ip(192, 0, 2, 51);
        h2.accept_relayed(true, [&](std::uint32_t a, std::uint16_t) { return a == M || a == ip(192, 0, 2, 51); });
        CHECK(!h2.incoming(v2, t0 + std::chrono::seconds(15), &d, &sa, &sp, &rr));
    }
    // Wrong keys: refused, counted per address, then a lockout (the F3 rule) that holds the right key too.
    {
        Router bad;
        int refusals = 0, lockouts = 0;
        for (int i = 0; i < 7; ++i) {
            bad.relay_use(M, P, H, P, sim3::mac(sim3::key_of_pw("guess")), t0);
            std::vector<Wire> bo, mo;
            for (int k = 0; k < 3; ++k) {
                bad.poll(t0, &bo);
                pump(bad, X, bo, m, M, &mo);
                pump(m, M, mo, bad, X, &bo);
            }
            int c = 0;
            if (bad.relay_state(nullptr, &c) == Router::ClientState::Refused) {
                if (c == Router::kWrongKey) ++refusals;
                if (c == Router::kLockedOut) ++lockouts;
            }
        }
        CHECK(refusals == 6 && lockouts == 1);
        Router right;
        right.relay_use(M, P, H, P, sim3::mac(good), t0);
        std::vector<Wire> ro, mo;
        for (int k = 0; k < 3; ++k) {
            right.poll(t0, &ro);
            pump(right, X, ro, m, M, &mo);
            pump(m, M, mo, right, X, &ro);
        }
        int c = 0;
        CHECK(right.relay_state(nullptr, &c) == Router::ClientState::Refused && c == Router::kLockedOut);
        clock_ms += 31000;  // the lockout ends
        right.relay_use(M, P, H, P, sim3::mac(good), t0);
        for (int k = 0; k < 3; ++k) {
            right.poll(t0, &ro);
            pump(right, X, ro, m, M, &mo);
            pump(m, M, mo, right, X, &ro);
        }
        CHECK(right.relay_state() == Router::ClientState::Ready);
    }
    // At most kMaxSessions guests per member.
    {
        Router mm;
        mm.serve(true, H, P, sim3::mac(good), "M", {});
        int ready = 0, full = 0;
        for (int i = 0; i < 6; ++i) {
            Router gg;
            const std::uint32_t ga = ip(198, 51, 100, 100 + i);
            gg.relay_use(M, P, H, P, sim3::mac(good), t0);
            std::vector<Wire> go, mo;
            for (int k = 0; k < 3; ++k) {
                gg.poll(t0, &go);
                pump(gg, ga, go, mm, M, &mo);
                pump(mm, M, mo, gg, ga, &go);
            }
            int c = 0;
            const auto st = gg.relay_state(nullptr, &c);
            ready += st == Router::ClientState::Ready;
            full += st == Router::ClientState::Refused && c == Router::kFull;
        }
        CHECK(ready == static_cast<int>(Router::kMaxSessions) && full == 2);
    }
    // A member that is not serving any more forwards nothing.
    m.serve(false, 0, 0, nullptr, {}, {});
    g.outgoing(H, P, dgram, 5, t0, &gout);
    pump(g, G, gout, m, M, &mout);
    CHECK(mout.empty());
    (void)tmu;
}

// IPv6 when IPv4 cannot connect at all (the host behind a symmetric NAT / CGNAT): the guest's
// link to the host's IPv6 address, through stateful IPv6 firewalls. It opens when the host's
// firewall lets it in, or the host first sent to the guest's IPv6 endpoint (its BBG6 code).
static void test_ipv6_pairs() {
    using namespace nat;
    int agree = 0, total = 0;
    std::printf("  host firewall / guest firewall / host has the guest's BBG6 code -> link over IPv6\n");
    for (bool hopen : {true, false})
        for (bool gopen : {true, false})
            for (bool code6 : {true, false}) {
                sim3::Party H("host", Type::Symmetric, ip(203, 0, 113, 1), 31), G("guest", Type::Symmetric, ip(198, 51, 100, 2), 37);
                H.has6 = G.has6 = true;
                H.a6 = ip6("2001:db8:1::1");
                G.a6 = ip6("2001:db8:2::2");
                H.fw_open6 = hopen;
                G.fw_open6 = gopen;
                H.router.set_ipv6(true);
                G.router.set_ipv6(true);
                H.mux.accepting = true;
                sim3::Net net;
                net.parties = {&H, &G};
                const rudp::Endpoint hp = H.box.stun(9307, {ip(192, 0, 2, 3), nbo(3478)});
                // The guest: the host's code address is reached at its IPv6 address (the code's tail).
                G.router.bind_v6(hp.addr, hp.port, Ep6{H.a6, nbo(9307)}, true);
                G.stream = G.mux.connect(hp, net.t(), 6000);
                bool ok = false;
                for (; net.now < 8000 && !ok; ) {
                    if (code6 && net.now % 250 == 0) {  // the host punches the guest's BBG6 endpoint
                        Wire w;
                        w.v6 = true;
                        w.addr6 = G.a6;
                        w.port = nbo(9307);
                        w.data.assign(udp_probe(), udp_probe() + 8);
                        net.put(H, {w});
                    }
                    net.step();
                    ok = G.stream->state() == rudp::Stream::State::Open && H.stream;
                }
                const bool want = hopen || code6;
                CHECK(ok == want);
                agree += ok == want;
                ++total;
                std::printf("    %-6s %-6s %-3s -> %s\n", hopen ? "open" : "closed", gopen ? "open" : "closed",
                            code6 ? "yes" : "no", ok ? "IPv6" : "no");
                if (ok) {
                    // The host sees the guest at its synthetic key, the same one every PC derives.
                    std::uint32_t k = 0;
                    std::uint16_t p = 0;
                    bbnet::udp::synthetic_key(Ep6{G.a6, nbo(9307)}, &k, &p);
                    CHECK(H.accepted_from.addr == k && H.accepted_from.port == p);
                    // IPv4 between these two cannot work at all (both symmetric).
                    CHECK(!nat_attempt(Type::Symmetric, Type::Symmetric, true));
                }
            }
    std::printf("  IPv6 pairs agree with the firewall model in %d/%d\n", agree, total);
}

// Guest to guest: IPv4 probes unanswered, IPv6 answered -> over IPv6 before the host relay; back to
// IPv4 as soon as it answers.
static void test_peerpaths_v6() {
    using bbnet::udp::PeerPaths;
    PeerPaths paths(PeerPaths::Config{500, 2500, 2500, false});
    const auto t0 = std::chrono::steady_clock::now();
    auto at = [&](int ms) { return t0 + std::chrono::milliseconds(ms); };
    const std::uint32_t a = ip(198, 51, 100, 9);
    const std::uint16_t p = nbo(9307);
    paths.add(a, p, 50123, at(0));
    paths.set_v6(a, p, true, at(0));
    std::vector<std::pair<std::uint32_t, std::uint16_t>> probes, probes6;
    std::vector<std::string> notes;
    for (int ms = 0; ms <= 3000; ms += 100) {
        paths.tick(at(ms), &probes, &notes, &probes6);
        if (ms % 500 == 0) paths.on_ack6(a, p, at(ms));  // only IPv6 answers
    }
    CHECK(!probes6.empty());
    auto v = paths.peers();
    CHECK(v.size() == 1 && v[0].via6 && !v[0].relayed);  // IPv6, not the relay
    std::uint16_t rp = 0;
    CHECK(!paths.relayed(a, p, &rp));
    paths.on_ack(a, p, at(3100));  // IPv4 answers again
    paths.tick(at(3150), &probes, &notes, &probes6);
    v = paths.peers();
    CHECK(!v[0].via6 && !v[0].relayed);
    // Neither answers: the relay.
    for (int ms = 3200; ms <= 9000; ms += 100) paths.tick(at(ms), &probes, &notes, &probes6);
    v = paths.peers();
    CHECK(v[0].relayed && !v[0].via6);
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
    std::printf("classify\n");
    test_classify();
    std::printf("NAT matrix (model vs simulated NATs, real rudp streams)\n");
    test_matrix();
    std::printf("netsim NAT kinds\n");
    test_netsim_kinds();
    std::printf("bencode / KRPC\n");
    test_bencode();
    std::printf("DHT lookup (simulated)\n");
    test_lookup_sim();
    std::printf("rendezvous over loopback sockets\n");
    test_rendezvous_sockets();
    std::printf("automatic hole punch, no guest code\n");
    test_auto_punch_link();
    std::printf("route table basics (IPv6 keys, the party code's IPv6 tail, BBG6)\n");
    test_route_basics();
    std::printf("path chooser (IPv6, direct, member relays)\n");
    test_chooser();
    std::printf("member relay: host x member x guest NAT triples (model vs simulation, real rudp streams)\n");
    test_member_relay_matrix();
    std::printf("member relay: security\n");
    test_member_relay_security();
    std::printf("IPv6 pairs (no IPv4 path at all)\n");
    test_ipv6_pairs();
    std::printf("guest to guest: IPv6 before the host relay\n");
    test_peerpaths_v6();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

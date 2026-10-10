// SPDX-License-Identifier: GPL-3.0-or-later
// Party host service (gpu/shim/net, A3): room lifecycle with three members, event cursors,
// heartbeat timeout and the hold while loading, signaling resolve, kicks; FromApi through the
// guest-facing sceHttp functions (ss.info, login, the party sign board, nonblocking epoll);
// sceNpMatching2 / sceNpSignaling create -> member joined -> left, and JoinRoom, over LocalHost.
// Build and run: ninja -C out/gpu party-host-test && out/gpu/party-host-test.exe
#include "bbnet_internal.h"
#include "from_api.h"
#include "json.h"
#include "np_session.h"
#include "party_host_service.h"
#include "party_transport.h"
#include "party_util.h"
#include "gpu/bbnet.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// --- Runtime stand-ins (src/probe.c, runtime_thread.c, runtime.c) ---
extern "C" {
BBNET_ABI void restore_guest_fs(void) {}
void runtime_thread_attach_host(const char*) {}
const char* runtime_symbol(const char*) { return nullptr; }
}

using bbnet::party::Caller;
using bbnet::party::int_of;
using bbnet::party::PartyHostService;
using bbnet::party::str_of;

static int g_failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

static void set_env(const char* k, const char* v) {
#if defined(_WIN32)
    _putenv_s(k, v);
#else
    setenv(k, v, 1);
#endif
}

static json::Value obj(std::initializer_list<std::pair<const char*, json::Value>> kv) {
    json::Value o = json::Value::make_object();
    for (const auto& [k, v] : kv) o.set(k, v);
    return o;
}

static json::Value call(PartyHostService& s, const char* who, const char* kind, const json::Value& rq) {
    json::Value reply;
    s.handle(Caller{who, 0}, kind, rq, reply);
    return reply;
}

static std::vector<json::Value> events(PartyHostService& s, const char* who, std::uint64_t cursor = 0) {
    return s.wait_events(who, cursor, 0, 64);
}
static bool has_event(const std::vector<json::Value>& evs, const char* name, long long member = -1) {
    for (const json::Value& e : evs) {
        if (str_of(e, "Name") == name && (member < 0 || int_of(e, "MemberId", -2) == member)) return true;
    }
    return false;
}

// --- 1. the service on its own -----------------------------------------------------------------

static void test_service() {
    PartyHostService s;
    std::int64_t now = 1000000;
    s.set_clock([&] { return now; });
    for (const char* id : {"Host", "GuestB", "GuestC", "GuestD"}) {
        json::Value r = call(s, id, "context_start",
                             obj({{"OnlineId", id}, {"SignalingAddr", "192.168.1.10"}, {"SignalingPort", 9307}}));
        CHECK(int_of(r, "ResKind", -1) == 0);
    }
    // GuestB is behind a NAT (mapped differs), GuestC is not.
    call(s, "GuestB", "context_start",
         obj({{"OnlineId", "GuestB"}, {"SignalingAddr", "192.168.1.20"}, {"SignalingPort", 9307},
              {"MappedAddr", "203.0.113.5"}, {"MappedPort", 40000}}));
    call(s, "GuestC", "context_start",
         obj({{"OnlineId", "GuestC"}, {"SignalingAddr", "192.168.1.30"}, {"SignalingPort", 9308},
              {"MappedAddr", "192.168.1.30"}, {"MappedPort", 9308}}));

    json::Value made = call(s, "Host", "create_room",
                            obj({{"OnlineId", "Host"}, {"MaxMembers", 5}, {"HostArea", 0x18010000}, {"HostLevel", 50}}));
    CHECK(int_of(made, "ResKind", -1) == 0);
    CHECK(int_of(made, "MemberId", 0) == 1 && int_of(made, "OwnerMemberId", 0) == 1);
    CHECK(int_of(made, "MaxMembers", 0) == 5);
    const long long room = int_of(made, "RoomId", 0);
    const std::string sid = str_of(made, "SessionId");
    CHECK(room > 0 && !sid.empty());

    json::Value jb = call(s, "GuestB", "join_room", obj({{"OnlineId", "GuestB"}, {"RoomId", room}}));
    CHECK(int_of(jb, "ResKind", -1) == 0 && int_of(jb, "MemberId", 0) == 2);
    CHECK(int_of(jb, "OwnerMemberId", 0) == 1 && int_of(jb, "MaxMembers", 0) == 5);
    const json::Value* mb = jb.find("Members");
    CHECK(mb && mb->array.size() == 1 && str_of(mb->array[0], "OnlineId") == "Host");
    json::Value jc = call(s, "GuestC", "join_room", obj({{"OnlineId", "GuestC"}, {"RoomId", room}}));
    CHECK(int_of(jc, "MemberId", 0) == 3 && jc.find("Members")->array.size() == 2);
    // BB_PARTY_MAX (3) caps the joins whatever the game asked.
    json::Value jd = call(s, "GuestD", "join_room", obj({{"OnlineId", "GuestD"}, {"RoomId", room}}));
    CHECK(int_of(jd, "ResKind", 0) != 0 && str_of(jd, "Error") == "Room full");
    CHECK(int_of(call(s, "GuestD", "join_room", obj({{"RoomId", 99999}})), "ResKind", 0) != 0);

    // Event cursors: Host heard 2 joins, GuestB 1.
    auto he = events(s, "Host");
    CHECK(he.size() == 2 && has_event(he, "room_member_joined", 2) && has_event(he, "room_member_joined", 3));
    CHECK(int_of(he[0], "EventId", 0) == 1 && int_of(he[1], "EventId", 0) == 2);
    CHECK(str_of(he[0], "Addr") == "203.0.113.5" && int_of(he[0], "Port", 0) == 40000);  // mapped wins
    CHECK(events(s, "Host", 1).size() == 1);
    s.ack_events("Host", 1);
    CHECK(events(s, "Host").size() == 1 && int_of(events(s, "Host")[0], "EventId", 0) == 2);
    s.ack_events("Host", 2);
    CHECK(events(s, "GuestB").size() == 1 && has_event(events(s, "GuestB"), "room_member_joined", 3));
    s.ack_events("GuestB", 99);
    s.ack_events("GuestC", 99);
    // A waiter wakes on a push.
    std::thread pusher([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        s.push_event("GuestC", obj({{"Name", "peer_deactivated"}, {"OnlineId", "X"}}));
    });
    auto waited = s.wait_events("GuestC", 0, 2000);
    pusher.join();
    CHECK(waited.size() == 1 && str_of(waited[0], "Name") == "peer_deactivated");
    s.ack_events("GuestC", 99);

    // Signaling resolve: what each registered at context_start.
    json::Value rb = call(s, "Host", "signaling_resolve", obj({{"OnlineId", "GuestB"}}));
    CHECK(int_of(rb, "ResKind", -1) == 0 && str_of(rb, "Addr") == "203.0.113.5" && int_of(rb, "Port", 0) == 40000);
    CHECK(str_of(rb, "LocalAddr") == "192.168.1.20" && int_of(rb, "LocalPort", 0) == 9307);
    json::Value rc = call(s, "Host", "signaling_resolve", obj({{"OnlineId", "GuestC"}}));
    CHECK(str_of(rc, "Addr") == "192.168.1.30" && int_of(rc, "Port", 0) == 9308);
    CHECK(int_of(call(s, "Host", "signaling_resolve", obj({{"OnlineId", "Nobody"}})), "ResKind", 0) != 0);
    // An update moves GuestB and its room hears it.
    call(s, "GuestB", "signaling_update",
         obj({{"OnlineId", "GuestB"}, {"MappedAddr", "203.0.113.9"}, {"MappedPort", 40001}}));
    CHECK(str_of(call(s, "Host", "signaling_resolve", obj({{"OnlineId", "GuestB"}})), "Addr") == "203.0.113.9");
    auto upd = events(s, "Host", 2);
    CHECK(upd.size() == 1 && has_event(upd, "room_member_joined", 2) && str_of(upd[0], "Addr") == "203.0.113.9");
    s.ack_events("Host", 99);
    s.ack_events("GuestC", 99);

    // Heartbeats: 15 s of silence is a timeout; a loading member is held.
    now += 10000;
    CHECK(int_of(call(s, "Host", "heartbeat", obj({{"SessionId", sid}, {"MemberId", 1}})), "InRoom", 0) == 1);
    CHECK(int_of(call(s, "GuestB", "heartbeat", obj({{"SessionId", sid}, {"MemberId", 2}})), "InRoom", 0) == 1);
    now += 6000;  // GuestC: 16 s since its join
    s.set_member_loading("GuestC", true);
    s.tick();
    PartyHostService::RoomView rv;
    CHECK(s.room(static_cast<std::uint64_t>(room), &rv) && rv.members.size() == 3);
    CHECK(events(s, "Host").empty());
    s.set_member_loading("GuestC", false);  // back: its clock starts over
    s.tick();
    CHECK(s.room(static_cast<std::uint64_t>(room), &rv) && rv.members.size() == 3);
    // The roster's query holds too.
    bool c_loading = true;
    s.set_loading_query([&](const std::string& id) { return id == "GuestC" && c_loading; });
    now += 16000;
    call(s, "Host", "heartbeat", obj({{"SessionId", sid}, {"MemberId", 1}}));
    call(s, "GuestB", "heartbeat", obj({{"SessionId", sid}, {"MemberId", 2}}));
    s.tick();
    CHECK(s.room(static_cast<std::uint64_t>(room), &rv) && rv.members.size() == 3);
    c_loading = false;
    s.tick();
    CHECK(s.room(static_cast<std::uint64_t>(room), &rv) && rv.members.size() == 2);
    auto left = events(s, "Host");
    CHECK(left.size() == 1 && has_event(left, "room_member_left", 3) && str_of(left[0], "Reason") == "timeout");
    CHECK(has_event(events(s, "GuestB"), "room_member_left", 3));
    CHECK(int_of(call(s, "GuestC", "heartbeat", obj({{"SessionId", sid}, {"MemberId", 3}})), "InRoom", 1) == 0);
    s.ack_events("Host", 99);
    s.ack_events("GuestB", 99);

    // A kick: the kicked hears room_member_kicked, the rest room_member_left "kicked".
    CHECK(int_of(call(s, "GuestB", "kick_member",
                      obj({{"SessionId", sid}, {"MemberId", 1}, {"KickerMemberId", 2}, {"OptData", "AAAA"}})),
                 "ResKind", 0) != 0);  // only the owner kicks
    json::Value k = call(s, "Host", "kick_member",
                         obj({{"SessionId", sid}, {"MemberId", 2}, {"KickerMemberId", 1}, {"OptData", "GQAA/w=="}}));
    CHECK(int_of(k, "ResKind", -1) == 0);
    auto kb = events(s, "GuestB");
    CHECK(kb.size() == 1 && str_of(kb[0], "Name") == "room_member_kicked" && str_of(kb[0], "OptData") == "GQAA/w==");
    auto kh = events(s, "Host");
    CHECK(kh.size() == 1 && has_event(kh, "room_member_left", 2) && str_of(kh[0], "Reason") == "kicked");
    s.ack_events("Host", 99);
    s.ack_events("GuestB", 99);

    // The owner leaving closes the room for the others.
    json::Value j2 = call(s, "GuestC", "join_room", obj({{"OnlineId", "GuestC"}, {"RoomId", room}}));
    CHECK(int_of(j2, "MemberId", 0) == 4);  // ids are not reused
    call(s, "Host", "leave_room", obj({{"SessionId", sid}, {"MemberId", 1}}));
    CHECK(!s.room(static_cast<std::uint64_t>(room), nullptr));
    auto closed = events(s, "GuestC");
    CHECK(closed.size() == 1 && str_of(closed[0], "Name") == "room_closed" && str_of(closed[0], "Reason") == "host_left");
    // A link gone: the others hear peer_deactivated.
    s.ack_events("GuestB", 99);
    s.context_gone("GuestC");
    CHECK(has_event(events(s, "GuestB"), "peer_deactivated"));
    CHECK(int_of(call(s, "Host", "signaling_resolve", obj({{"OnlineId", "GuestC"}})), "ResKind", 0) != 0);
    std::printf("service: ok\n");
}

// --- guest callbacks --------------------------------------------------------------------------

struct Cb {
    const char* kind;
    std::uint64_t a[6];
    std::uint64_t room_id = 0;    // copied out of the callback's data
    std::uint64_t members = 0;
    std::uint16_t member = 0;     // member id in the data
    std::uint16_t owner = 0;
};
static std::mutex g_cb_mu;
static std::condition_variable g_cb_cv;
static std::vector<Cb> g_cbs;

static void record(Cb c) {
    std::lock_guard<std::mutex> lk(g_cb_mu);
    g_cbs.push_back(c);
    g_cb_cv.notify_all();
}
static BBNET_ABI std::uint64_t cb_context(std::uint64_t a0, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                                          std::uint64_t a4, std::uint64_t a5) {
    record(Cb{"context", {a0, a1, a2, a3, a4, a5}});
    return 0;
}
// (ctxId, reqId, event, errorCode, data, arg)
static BBNET_ABI std::uint64_t cb_request(std::uint64_t a0, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                                          std::uint64_t a4, std::uint64_t a5) {
    Cb c{"request", {a0, a1, a2, a3, a4, a5}};
    const auto event = static_cast<std::uint16_t>(a2);
    if ((event == 0x0101 || event == 0x0102) && a4 && static_cast<std::uint32_t>(a3) == 0) {
        const auto* resp = reinterpret_cast<const std::uint8_t*>(a4);
        const std::uint8_t* room = nullptr;
        std::memcpy(&room, resp, 8);
        std::memcpy(&c.room_id, room + 0x18, 8);
        std::memcpy(&c.members, resp + 0x10, 8);
        const std::uint8_t* me = nullptr;
        const std::uint8_t* owner = nullptr;
        std::memcpy(&me, resp + 0x18, 8);
        std::memcpy(&owner, resp + 0x20, 8);
        if (me) std::memcpy(&c.member, me + 0x38, 2);
        if (owner) std::memcpy(&c.owner, owner + 0x38, 2);
    }
    record(c);
    return 0;
}
// (ctxId, roomId, event, data, arg)
static BBNET_ABI std::uint64_t cb_room(std::uint64_t a0, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                                       std::uint64_t a4, std::uint64_t a5) {
    Cb c{"room", {a0, a1, a2, a3, a4, a5}};
    const auto event = static_cast<std::uint16_t>(a2);
    if ((event == 0x1101 || event == 0x1102) && a3) {
        const std::uint8_t* m = nullptr;
        std::memcpy(&m, reinterpret_cast<const void*>(a3), 8);
        if (m) std::memcpy(&c.member, m + 0x38, 2);
    }
    record(c);
    return 0;
}
// (ctxId, roomId, member, event, error, arg)
static BBNET_ABI std::uint64_t cb_m2sig(std::uint64_t a0, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                                        std::uint64_t a4, std::uint64_t a5) {
    record(Cb{"m2sig", {a0, a1, a2, a3, a4, a5}});
    return 0;
}
// (ctxId, connId, event, error, arg)
static BBNET_ABI std::uint64_t cb_sig(std::uint64_t a0, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                                      std::uint64_t a4, std::uint64_t a5) {
    record(Cb{"sig", {a0, a1, a2, a3, a4, a5}});
    return 0;
}

// Waits for a callback matching `pred` (anywhere in the record), returns it.
static bool wait_cb(std::function<bool(const Cb&)> pred, Cb* out = nullptr, int timeout_ms = 3000) {
    std::unique_lock<std::mutex> lk(g_cb_mu);
    const bool ok = g_cb_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] {
        for (const Cb& c : g_cbs) {
            if (pred(c)) return true;
        }
        return false;
    });
    if (ok && out) {
        for (const Cb& c : g_cbs) {
            if (pred(c)) {
                *out = c;
                break;
            }
        }
    }
    return ok;
}
static bool is(const Cb& c, const char* kind, std::uint64_t event, int event_arg) {
    return std::strcmp(c.kind, kind) == 0 && (c.a[event_arg] & 0xffff) == event;
}

template <typename F>
static F fn(const char* name) {
    void* f = reinterpret_cast<void*>(bbnet_resolve(name));
    if (!f) {
        std::fprintf(stderr, "no party function %s\n", name);
        ++g_failures;
        std::exit(1);
    }
    return reinterpret_cast<F>(f);
}

// --- 2. Matching2 + Signaling over LocalHost ----------------------------------------------------

struct RequestOptParam {
    void* cbFunc;
    void* cbFuncArg;
    std::uint32_t timeout;
    std::uint16_t appReqId;
    std::uint16_t pad;
};

using M2Init = int(BBNET_ABI*)(const void*);
using M2CreateCtx = int(BBNET_ABI*)(const void*, unsigned*);
using M2RegCtx = int(BBNET_ABI*)(void*, void*);
using M2Reg = int(BBNET_ABI*)(unsigned, void*, void*);
using M2Start = int(BBNET_ABI*)(unsigned, unsigned);
using M2Req = int(BBNET_ABI*)(unsigned, const void*, const RequestOptParam*, unsigned*);
using M2Status = int(BBNET_ABI*)(unsigned, std::uint64_t, std::uint16_t, int*, std::uint32_t*, std::uint16_t*);
using SigCreate = int(BBNET_ABI*)(const void*, void*, void*, unsigned*);
using SigActivate = int(BBNET_ABI*)(unsigned, const void*, unsigned*);
using SigStatus = int(BBNET_ABI*)(unsigned, unsigned, int*, std::uint32_t*, std::uint16_t*);

static unsigned g_ctx = 0;
static std::uint64_t g_host_room = 0;
static std::vector<std::uint8_t>* g_image = nullptr;

static void test_matching2_host() {
    PartyHostService& s = PartyHostService::instance();
    std::uint8_t npid[36] = {};
    std::snprintf(reinterpret_cast<char*>(npid), 17, "HostHunter");
    const void* param[1] = {npid};
    CHECK(fn<M2Init>("sceNpMatching2Initialize")(nullptr) == 0);
    CHECK(fn<M2CreateCtx>("sceNpMatching2CreateContext")(param, &g_ctx) == 0 && g_ctx == 1);
    CHECK(fn<M2RegCtx>("sceNpMatching2RegisterContextCallback")(reinterpret_cast<void*>(cb_context),
                                                                  reinterpret_cast<void*>(0xc0)) == 0);
    CHECK(fn<M2Reg>("sceNpMatching2RegisterRoomEventCallback")(g_ctx, reinterpret_cast<void*>(cb_room),
                                                                 reinterpret_cast<void*>(0xd0)) == 0);
    CHECK(fn<M2Reg>("sceNpMatching2RegisterSignalingCallback")(g_ctx, reinterpret_cast<void*>(cb_m2sig),
                                                                 reinterpret_cast<void*>(0xe0)) == 0);
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(fn<M2Start>("sceNpMatching2ContextStart")(g_ctx, 0) == 0);
    Cb c;
    CHECK(wait_cb([](const Cb& x) { return is(x, "context", 0x6F02, 1); }, &c));
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(ms >= 150);  // CONTEXT_STARTED at +200 ms
    CHECK(c.a[4] == 0xc0);
    CHECK((*g_image)[0x546cc09] == 1);  // the signaling gate
    // The host's context reached the service.
    json::Value rec;
    CHECK(s.resolve("HostHunter", &rec));

    // CreateJoinRoom: 0x0101 at +300 ms with the room, us as member 1 and owner.
    std::uint8_t create[0x40] = {};
    const std::uint16_t max_slot = 5;
    std::memcpy(create, &max_slot, 2);
    RequestOptParam opt{reinterpret_cast<void*>(cb_request), reinterpret_cast<void*>(0xa0), 0, 0, 0};
    unsigned req = 0;
    CHECK(fn<M2Req>("sceNpMatching2CreateJoinRoom")(g_ctx, create, &opt, &req) == 0 && req > 0);
    CHECK(wait_cb([&](const Cb& x) { return is(x, "request", 0x0101, 2) && x.a[1] == req; }, &c));
    CHECK(static_cast<std::uint32_t>(c.a[3]) == 0 && c.room_id != 0 && c.members == 1 && c.member == 1 && c.owner == 1);
    g_host_room = c.room_id;
    PartyHostService::RoomView rv;
    CHECK(s.room_of("HostHunter", &rv, true) && rv.room_id == g_host_room && rv.max_members == 5);
    // The extra came from FrpgNetMan in the fake image (area/level/pos).
    CHECK(int_of(rv.extra, "HostArea", 0) == 0x18010000 && int_of(rv.extra, "HostLevel", 0) == 77);
    CHECK(wait_cb([](const Cb& x) { return is(x, "m2sig", 0x5102, 3) && x.a[2] == 1; }));  // ESTABLISHED, self

    // A guest joins through the service (as its PartyLink RPC would): MEMBER_JOINED +300 ms,
    // ESTABLISHED +800 ms, and the connection status gives its address.
    call(s, "GuestA", "context_start",
         obj({{"OnlineId", "GuestA"}, {"SignalingAddr", "10.0.0.7"}, {"SignalingPort", 9400}}));
    json::Value j = call(s, "GuestA", "join_room",
                         obj({{"OnlineId", "GuestA"}, {"RoomId", static_cast<long long>(g_host_room)},
                              {"LocalAddr", "10.0.0.7"}, {"LocalPort", 9400}}));
    CHECK(int_of(j, "ResKind", -1) == 0 && int_of(j, "MemberId", 0) == 2);
    CHECK(wait_cb([](const Cb& x) { return is(x, "room", 0x1101, 2) && x.member == 2; }));
    CHECK(wait_cb([](const Cb& x) { return is(x, "m2sig", 0x5102, 3) && x.a[2] == 2; }));
    int st = -1;
    std::uint32_t addr = 0;
    std::uint16_t port = 0;
    CHECK(fn<M2Status>("sceNpMatching2SignalingGetConnectionStatus")(g_ctx, g_host_room, 2, &st, &addr, &port) == 0);
    CHECK(st == 2 && addr == bbnet::party::ip_parse("10.0.0.7") && port == static_cast<std::uint16_t>((9400 >> 8) | ((9400 & 0xff) << 8)));

    // NpSignaling to the guest: ESTABLISHED (+200) then ACTIVE (+400), status 2.
    unsigned sctx = 0, conn = 0;
    CHECK(fn<SigCreate>("sceNpSignalingCreateContext")(npid, reinterpret_cast<void*>(cb_sig),
                                                        reinterpret_cast<void*>(0xf0), &sctx) == 0);
    std::uint8_t guest_npid[36] = {};
    std::snprintf(reinterpret_cast<char*>(guest_npid), 17, "GuestA");
    CHECK(fn<SigActivate>("sceNpSignalingActivateConnection")(sctx, guest_npid, &conn) == 0 && conn > 0);
    unsigned conn2 = 0;
    CHECK(fn<SigActivate>("sceNpSignalingActivateConnection")(sctx, guest_npid, &conn2) == 0 && conn2 == conn);
    CHECK(wait_cb([&](const Cb& x) { return is(x, "sig", 0x01, 2) && x.a[1] == conn; }));
    CHECK(wait_cb([&](const Cb& x) { return is(x, "sig", 0x0c, 2) && x.a[1] == conn; }));
    CHECK(fn<SigStatus>("sceNpSignalingGetConnectionStatus")(sctx, conn, &st, &addr, &port) == 0 && st == 2);
    // A peer not in the room yet is resolved by the host (the summoned guest before its join).
    call(s, "GuestE", "context_start", obj({{"OnlineId", "GuestE"}, {"SignalingAddr", "10.0.0.9"}, {"SignalingPort", 9401}}));
    std::uint8_t e_npid[36] = {};
    std::snprintf(reinterpret_cast<char*>(e_npid), 17, "GuestE");
    unsigned conn_e = 0;
    CHECK(fn<SigActivate>("sceNpSignalingActivateConnection")(sctx, e_npid, &conn_e) == 0);
    CHECK(wait_cb([&](const Cb& x) { return is(x, "sig", 0x0c, 2) && x.a[1] == conn_e; }));
    CHECK(fn<SigStatus>("sceNpSignalingGetConnectionStatus")(sctx, conn_e, &st, &addr, &port) == 0 && st == 2 &&
          addr == bbnet::party::ip_parse("10.0.0.9"));
    std::printf("matching2 host: ok (room %llu)\n", static_cast<unsigned long long>(g_host_room));
}

// --- 3. FromApi through sceHttp -----------------------------------------------------------------

using HttpInit = int(BBNET_ABI*)(int, int, std::uint64_t);
using HttpTmpl = int(BBNET_ABI*)(int, const char*, int, int);
using HttpConn = int(BBNET_ABI*)(int, const char*, int);
using HttpReqUrl = int(BBNET_ABI*)(int, int, const char*, std::uint64_t);
using HttpSend = int(BBNET_ABI*)(int, const void*, std::uint64_t);
using HttpStatus = int(BBNET_ABI*)(int, int*);
using HttpLen = int(BBNET_ABI*)(int, int*, std::uint64_t*);
using HttpRead = int(BBNET_ABI*)(int, void*, std::uint64_t);
using HttpDel = int(BBNET_ABI*)(int);
using HttpNonblock = int(BBNET_ABI*)(int, int);
using HttpCreateEpoll = int(BBNET_ABI*)(int, void**);
using HttpSetEpoll = int(BBNET_ABI*)(int, void*, void*);
using HttpWait = int(BBNET_ABI*)(void*, void*, int, int);
using HttpHeader = int(BBNET_ABI*)(int, const char*, const char*, unsigned);

struct Http {
    int ctx = 0, tmpl = 0;
};

// One blocking request as the game makes it; returns the status (0 on error) and the body.
static int http_request(Http& h, int method, const std::string& url, const std::string& post, std::string* body) {
    const int conn = fn<HttpConn>("sceHttpCreateConnectionWithURL")(h.tmpl, url.c_str(), 1);
    CHECK(conn > 0);
    const int rq = fn<HttpReqUrl>("sceHttpCreateRequestWithURL")(conn, method, url.c_str(), post.size());
    CHECK(rq > 0);
    CHECK(fn<HttpHeader>("sceHttpAddRequestHeader")(rq, "Content-Type", "application/json", 0) == 0);
    const int sent = fn<HttpSend>("sceHttpSendRequest")(rq, post.data(), post.size());
    int code = 0;
    if (sent == 0) {
        CHECK(fn<HttpStatus>("sceHttpGetStatusCode")(rq, &code) == 0);
        int result = -1;
        std::uint64_t len = 0;
        CHECK(fn<HttpLen>("sceHttpGetResponseContentLength")(rq, &result, &len) == 0 && result == 0);
        std::string b(static_cast<std::size_t>(len), '\0');
        std::size_t got = 0;
        while (got < len) {
            const int n = fn<HttpRead>("sceHttpReadData")(rq, &b[got], std::min<std::uint64_t>(len - got, 100));
            if (n <= 0) break;
            got += static_cast<std::size_t>(n);
        }
        CHECK(got == len);
        CHECK(fn<HttpRead>("sceHttpReadData")(rq, &b[0], 1) == 0 || len == 0);  // the end
        if (body) *body = b;
    }
    fn<HttpDel>("sceHttpDeleteRequest")(rq);
    fn<HttpDel>("sceHttpDeleteConnection")(conn);
    return code;
}

static json::Value parse(const std::string& s) {
    json::Value v;
    std::string err;
    CHECK(json::parse(s, v, err));
    return v;
}

// A guest's FROM API request, as the host gets it over PartyLink ("http" call).
static json::Value guest_http(const char* guest, const char* method, const std::string& url, const std::string& body) {
    json::Value rq = obj({{"Method", method}, {"Url", url}, {"Body", bbnet::party::b64_encode(body)}});
    json::Value reply;
    bbnet::party::host_handle(Caller{guest, 0}, "http", rq, reply);
    CHECK(int_of(reply, "ResKind", -1) == 0 && int_of(reply, "Status", 0) == 200);
    const std::vector<std::uint8_t> b = bbnet::party::b64_decode(str_of(reply, "Body"));
    return parse(std::string(b.begin(), b.end()));
}

static void test_from_api() {
    Http h;
    h.ctx = fn<HttpInit>("sceHttpInit")(1, 1, 0x10000);
    CHECK(h.ctx > 0);
    h.tmpl = fn<HttpTmpl>("sceHttpCreateTemplate")(h.ctx, "PS4Application FROMhttp/1.0 (test)", 2, 0);
    CHECK(h.tmpl > 0);

    // ss.info, with N = FrpgNetMan+0x9e8 = 7 from the fake image.
    std::string ss;
    CHECK(http_request(h, 0, "https://ss4.scej-network.jp:20443/bb/ss.info", "", &ss) == 200);
    CHECK(ss.find("<ss>0</ss>") != std::string::npos);
    CHECK(ss.find("<gameurl7>") != std::string::npos && ss.find("</gameurl7>") != std::string::npos);
    CHECK(ss.find("<gameurl0>") == std::string::npos);
    CHECK(ss.find("<api_SummonDataSummon>http://bbparty.invalid:18671</api_SummonDataSummon>") != std::string::npos);
    CHECK(ss.find("<SummonDataCreateInterval7>") != std::string::npos);
    std::size_t apis = 0;
    for (std::size_t at = 0; (at = ss.find("<api_", at)) != std::string::npos; ++at) ++apis;
    CHECK(apis == 37);

    // Login and the basics.
    std::string body;
    CHECK(http_request(h, 1, "http://bbparty.invalid:18671/basic_utils/login",
                       R"({"MessageId":"LoginRequest","AuthorizationCode":"DUMMY","ApplicationVersion":109})",
                       &body) == 200);
    json::Value login = parse(body);
    CHECK(!str_of(login, "SessionId").empty() && int_of(login, "UserId", 0) > 0);
    const long long host_uid = int_of(login, "UserId", 0);
    CHECK(http_request(h, 1, "http://bbparty.invalid:18671/basic_utils/get_datetime", "{}", &body) == 200);
    CHECK(int_of(parse(body), "ResKind", -1) == 0);
    // sync_chara_id: the PublishCharacterIdList array the game requires (A0 4.2).
    CHECK(http_request(h, 1, "http://bbparty.invalid:18671/basic_utils/sync_chara_id", R"({"CharaIdNum":1})", &body) == 200);
    {
        const json::Value sync = parse(body);
        const json::Value* l = sync.find("PublishCharacterIdList");
        CHECK(l && l->type == json::Value::Type::Array && l->array.size() == 1 &&
              int_of(l->array[0], "PublishCharaId", 0) > 0);
    }
    for (const char* p : {"get_normal_notice", "get_emergency_notice", "get_user_agreement", "sync_chara_id"}) {
        CHECK(http_request(h, 1, std::string("http://bbparty.invalid:18671/basic_utils/") + p, "{}", &body) == 200);
        CHECK(int_of(parse(body), "ResKind", -1) == 0);
    }
    // Another API under the gameurl: an empty success. Play logs: 200. Elsewhere: no network.
    CHECK(http_request(h, 1, "http://bbparty.invalid:18671/bloodmessage/get_list", "{}", &body) == 200);
    CHECK(http_request(h, 4, "https://bb-playlog-prod.s3.amazonaws.com/20261010/x.log", "zz", &body) == 200 && body.empty());
    {
        const int conn = fn<HttpConn>("sceHttpCreateConnectionWithURL")(h.tmpl, "https://example.com/", 1);
        const int rq = fn<HttpReqUrl>("sceHttpCreateRequestWithURL")(conn, 0, "https://example.com/", 0);
        CHECK(fn<HttpSend>("sceHttpSendRequest")(rq, nullptr, 0) == static_cast<int>(0x80431063u));
    }

    // Nonblocking with an epoll, the game's way.
    {
        void* ep = nullptr;
        CHECK(fn<HttpCreateEpoll>("sceHttpCreateEpoll")(h.ctx, &ep) == 0 && ep);
        CHECK(fn<HttpNonblock>("sceHttpSetNonblock")(h.tmpl, 1) == 0);
        const std::string url = "http://bbparty.invalid:18671/basic_utils/get_datetime";
        const int conn = fn<HttpConn>("sceHttpCreateConnectionWithURL")(h.tmpl, url.c_str(), 1);
        const int rq = fn<HttpReqUrl>("sceHttpCreateRequestWithURL")(conn, 1, url.c_str(), 2);
        CHECK(fn<HttpSetEpoll>("sceHttpSetEpoll")(rq, ep, reinterpret_cast<void*>(0x1234)) == 0);
        CHECK(fn<HttpSend>("sceHttpSendRequest")(rq, "{}", 2) == 0);
        struct NbEvent {
            std::uint32_t events, detail;
            int id;
            void* user;
        } evs[4] = {};
        int n = 0;
        for (int i = 0; i < 50 && n == 0; ++i) n = fn<HttpWait>("sceHttpWaitRequest")(ep, evs, 4, 100000);
        CHECK(n == 1 && evs[0].id == rq && evs[0].user == reinterpret_cast<void*>(0x1234) && (evs[0].events & 0x1));
        int code = 0;
        CHECK(fn<HttpStatus>("sceHttpGetStatusCode")(rq, &code) == 0 && code == 200);
        CHECK(fn<HttpNonblock>("sceHttpSetNonblock")(h.tmpl, 0) == 0);
    }

    // The party sign board: GuestA (over the link) puts a sign up, the host lists it and
    // summons; GuestA's queue gets the guest_invite np_matching2 reads.
    PartyHostService& s = PartyHostService::instance();
    json::Value gl = guest_http("GuestA", "POST", "http://bbparty.invalid:18671/basic_utils/login",
                                R"({"MessageId":"LoginRequest"})");
    const long long guest_uid = int_of(gl, "UserId", 0);
    CHECK(guest_uid > 0 && guest_uid != host_uid);
    std::string blob(0xE0, '\0');
    blob[0x76] = 7;
    json::Value cr = guest_http(
        "GuestA", "POST", "http://bbparty.invalid:18671/summon_messenger/create?user_id=" + std::to_string(guest_uid),
        R"({"MessageId":"SummonDataCreateRequest","CharaId":1,"AreaId":402718720,"AreaRegionId":1,"SummonType":0,"SummonData":")" +
            bbnet::party::b64_encode(blob) + R"(","SummonDataVersion":3})");
    CHECK(int_of(cr, "SummonDataId", 0) > 0);
    // The guest does not see its own sign; the host does.
    CHECK(guest_http("GuestA", "POST", "http://bbparty.invalid:18671/summon_messenger/get", "{}")
              .find("SummonDataList")->array.empty());
    CHECK(http_request(h, 1, "http://bbparty.invalid:18671/summon_messenger/get",
                       R"({"MessageId":"SummonDataGetListRequest","AreaId":402718720,"GetMaxCount":20,"SummonTypeList":[{"SummonType":0,"GetLimitCount":5}]})",
                       &body) == 200);
    json::Value list = parse(body);
    const json::Value* signs = list.find("SummonDataList");
    CHECK(signs && signs->array.size() == 1 && int_of(signs->array[0], "UserId", 0) == guest_uid);
    CHECK(str_of(signs->array[0], "SummonData") == bbnet::party::b64_encode(blob));
    CHECK(int_of(signs->array[0], "SummonDataVersion", -1) == 3 && int_of(signs->array[0], "CharaId", 0) == 1);
    // A type filter that matches nothing.
    CHECK(http_request(h, 1, "http://bbparty.invalid:18671/summon_messenger/get",
                       R"({"SummonTypeList":[{"SummonType":2}]})", &body) == 200);
    CHECK(parse(body).find("SummonDataList")->array.empty());

    s.ack_events("GuestA", 1u << 30);
    CHECK(http_request(h, 1, "http://bbparty.invalid:18671/summon_messenger/request",
                       R"({"MessageId":"SummonDataSummonRequest","TargetUserId":)" + std::to_string(guest_uid) + "}",
                       &body) == 200);
    CHECK(int_of(parse(body), "Result", 0) == 1);
    auto inv = s.wait_events("GuestA", 0, 0);
    CHECK(inv.size() == 1);
    if (!inv.empty()) {
        const json::Value& e = inv[0];
        CHECK(str_of(e, "Name") == "guest_invite");
        CHECK(static_cast<std::uint64_t>(int_of(e, "RoomId", 0)) == g_host_room);
        CHECK(str_of(e, "HostOnlineId") == "HostHunter");
        CHECK(!str_of(e, "HostAddr").empty() && int_of(e, "HostPort", 0) > 0);
        CHECK(e.find("HostLocalAddr") && e.find("HostLocalPort") && e.find("HostMappedAddr") && e.find("HostMappedPort"));
        CHECK(int_of(e, "MemberTag", 0) == 1 && int_of(e, "HostArea", 0) == 0x18010000 && int_of(e, "HostLevel", 0) == 77);
        const json::Value* pos = e.find("HostPos");
        CHECK(pos && pos->array.size() == 3 && pos->array[0].number == 1.5);
    }
    s.ack_events("GuestA", 1u << 30);
    // delete takes the sign down.
    guest_http("GuestA", "POST", "http://bbparty.invalid:18671/summon_messenger/delete", "{}");
    CHECK(bbnet::party::FromApi::instance().signs().empty());
    std::printf("from api: ok\n");
}

// --- 4. members leave; LeaveRoom; JoinRoom into another's room -------------------------------------

static void test_matching2_leave_and_join() {
    PartyHostService& s = PartyHostService::instance();
    // GuestA leaves: DEAD (0x5101) for member 2, then MEMBER_LEFT (0x1102); its NpSignaling
    // connection dies (0x00).
    PartyHostService::RoomView rv;
    CHECK(s.room(g_host_room, &rv));
    call(s, "GuestA", "leave_room", obj({{"SessionId", rv.session_id}, {"MemberId", 2}}));
    CHECK(wait_cb([](const Cb& x) { return is(x, "m2sig", 0x5101, 3) && x.a[2] == 2; }));
    CHECK(wait_cb([](const Cb& x) { return is(x, "room", 0x1102, 2) && x.member == 2; }));
    CHECK(wait_cb([](const Cb& x) { return is(x, "sig", 0x00, 2); }));
    // DEAD came before MEMBER_LEFT.
    {
        std::lock_guard<std::mutex> lk(g_cb_mu);
        int dead = -1, left = -1;
        for (int i = 0; i < static_cast<int>(g_cbs.size()); ++i) {
            if (dead < 0 && is(g_cbs[i], "m2sig", 0x5101, 3) && g_cbs[i].a[2] == 2) dead = i;
            if (left < 0 && is(g_cbs[i], "room", 0x1102, 2)) left = i;
        }
        CHECK(dead >= 0 && left > dead);
    }
    // No auto-leave: the room stays ours with nobody else in it.
    CHECK(s.room_of("HostHunter", &rv, true) && rv.members.size() == 1);

    // LeaveRoom: 0x103 at +300 ms; the service closes the room.
    RequestOptParam opt{reinterpret_cast<void*>(cb_request), reinterpret_cast<void*>(0xa1), 0, 0, 0};
    unsigned req = 0;
    CHECK(fn<M2Req>("sceNpMatching2LeaveRoom")(g_ctx, nullptr, &opt, &req) == 0);
    CHECK(wait_cb([&](const Cb& x) { return is(x, "request", 0x0103, 2) && x.a[1] == req; }));
    bbnet::session::drain(0);
    CHECK(!s.room(g_host_room, nullptr));

    // Another party member's room; our game joins it: 0x0102 with both members, owner 1.
    call(s, "OtherHost", "context_start",
         obj({{"OnlineId", "OtherHost"}, {"SignalingAddr", "10.0.0.20"}, {"SignalingPort", 9500}}));
    json::Value made = call(s, "OtherHost", "create_room",
                            obj({{"OnlineId", "OtherHost"}, {"MaxMembers", 5}, {"LocalAddr", "10.0.0.20"}, {"LocalPort", 9500}}));
    const auto room = static_cast<std::uint64_t>(int_of(made, "RoomId", 0));
    CHECK(room != 0);
    std::uint8_t join[0x20] = {};
    std::memcpy(join, &room, 8);
    CHECK(fn<M2Req>("sceNpMatching2JoinRoom")(g_ctx, join, &opt, &req) == 0);
    Cb c;
    CHECK(wait_cb([&](const Cb& x) { return is(x, "request", 0x0102, 2) && x.a[1] == req; }, &c));
    CHECK(static_cast<std::uint32_t>(c.a[3]) == 0 && c.room_id == room && c.members == 2 && c.member == 2 && c.owner == 1);
    CHECK(wait_cb([&](const Cb& x) { return is(x, "m2sig", 0x5102, 3) && x.a[1] == room && x.a[2] == 1; }));  // +500 ms
    int st = 0;
    std::uint32_t addr = 0;
    std::uint16_t port = 0;
    CHECK(fn<M2Status>("sceNpMatching2SignalingGetConnectionStatus")(g_ctx, room, 1, &st, &addr, &port) == 0 && st == 2 &&
          addr == bbnet::party::ip_parse("10.0.0.20"));
    // The owner leaves: the room is destroyed for us (0x1104 after DEAD for member 1).
    call(s, "OtherHost", "leave_room", obj({{"SessionId", str_of(made, "SessionId")}, {"MemberId", 1}}));
    CHECK(wait_cb([&](const Cb& x) { return is(x, "room", 0x1104, 2) && x.a[1] == room; }));
    CHECK(wait_cb([&](const Cb& x) { return is(x, "m2sig", 0x5101, 3) && x.a[1] == room && x.a[2] == 1; }));
    std::printf("matching2 leave/join: ok\n");
}

// --- 5. RemoteGuest JSON plumbing (the A4 seam) -------------------------------------------------

static void test_remote_guest() {
    bbnet::party::RemoteGuest rg;
    json::Value reply;
    std::string err;
    CHECK(!rg.rpc("heartbeat", obj({}), reply, err, 100) && err == "no party link");
    // A fake link that answers like the host would.
    PartyHostService svc;
    rg.set_rpc([&](const char* kind, const std::string& rq, std::string& out, std::string& e, int) {
        json::Value v, r;
        if (!json::parse(rq, v, e)) return false;
        svc.handle(Caller{"Remote1", 0}, kind, v, r);
        out = json::dump(r, 0);
        return true;
    });
    rg.set_host_endpoint(bbnet::party::ip_parse("198.51.100.4"), 9307, false);
    call(svc, "Boss", "context_start", obj({{"OnlineId", "Boss"}, {"SignalingAddr", "127.0.0.1"}, {"SignalingPort", 9307}}));
    json::Value made = call(svc, "Boss", "create_room", obj({{"OnlineId", "Boss"}}));
    CHECK(rg.ok_call("context_start", obj({{"OnlineId", "Remote1"}, {"SignalingAddr", "10.1.1.1"}, {"SignalingPort", 9307}}),
                     reply, err));
    CHECK(rg.ok_call("join_room", obj({{"RoomId", int_of(made, "RoomId", 0)}}), reply, err));
    // The host's loopback record became the address the link reached it at.
    const json::Value* m = reply.find("Members");
    CHECK(m && m->array.size() == 1 && str_of(m->array[0], "Addr") == "198.51.100.4");
    // Events: in order, each once, invite addresses fixed up the same way.
    std::vector<std::string> got;
    rg.start_events("Remote1", [&](const json::Value& ev) { got.push_back(str_of(ev, "Name") + ":" + str_of(ev, "HostAddr")); });
    CHECK(rg.on_link_event(R"({"EventId":1,"Name":"guest_invite","HostAddr":"127.0.0.1","HostPort":9307})") == 1);
    CHECK(rg.on_link_event(R"({"EventId":1,"Name":"guest_invite","HostAddr":"127.0.0.1"})") == 1);  // resend
    CHECK(rg.on_link_event(R"({"EventId":2,"Name":"room_closed"})") == 2);
    CHECK(got.size() == 2 && got[0] == "guest_invite:198.51.100.4" && got[1] == "room_closed:");
    std::printf("remote guest seam: ok\n");
}

int main() {
    const std::uint16_t party_port = static_cast<std::uint16_t>(41000 + (std::rand() % 500));
    char port_text[16];
    std::snprintf(port_text, sizeof(port_text), "%u", party_port);
    set_env("BB_PARTY", "host");
    set_env("BB_PARTY_PORT", port_text);
    set_env("BB_PARTY_NAME", "HostHunter");
    set_env("BB_PARTY_LOCAL_IP", "127.0.0.1");
    setvbuf(stdout, nullptr, _IONBF, 0);

    // A fake game image: the signaling gate and FrpgNetMan (area, level, position, N for ss.info).
    static std::vector<std::uint8_t> image(0x5540000, 0);
    g_image = &image;
    static std::vector<std::uint8_t> netman(0x1000, 0);
    const std::uint64_t man = reinterpret_cast<std::uint64_t>(netman.data());
    std::memcpy(&image[0x553b120], &man, 8);
    const std::uint32_t n = 7, area = 0x18010000, level = 77;
    const float pos[3] = {1.5f, 2.5f, -3.0f};
    std::memcpy(&netman[0x9e8], &n, 4);
    std::memcpy(&netman[0xa7c], &area, 4);
    std::memcpy(&netman[0xaa8], &level, 4);
    std::memcpy(&netman[0xaac], pos, sizeof(pos));
    bbnet_set_image(image.data(), image.size());

    test_service();
    test_remote_guest();
    test_matching2_host();
    test_from_api();
    test_matching2_leave_and_join();

    if (g_failures) {
        std::fprintf(stderr, "party-host-test: %d failure(s)\n", g_failures);
        std::fflush(stderr);
        std::_Exit(1);
    }
    std::printf("party-host-test: all passed\n");
    std::fflush(stdout);
    std::_Exit(0);  // detached service threads keep running; do not run static destructors under them
}

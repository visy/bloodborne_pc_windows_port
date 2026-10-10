// SPDX-License-Identifier: GPL-3.0-or-later
// derived from droogie/bbhost src/hle/np_signaling.cpp @8f2746c
//
// sceNpSignaling over the session layer's peer table.
//
// The game's SocketState FSM is the only caller: state 4 activates a connection to a peer's
// NpId and expects ESTABLISHED, state 5 reads the status and expects 2 with the peer's
// address and port. The events are the ones the game's handler acts on (the co-op contract):
// 0x01 established, 0x0c active, 0x00 dead; ESTABLISHED +200 ms, ACTIVE +400 ms once the peer
// is known, DEAD +100 ms. bbport: callbacks through the A1 dispatcher; a peer not yet in the
// room is resolved by the party host (signaling_resolve), from what it registered at its
// context_start.
#include "bbnet_internal.h"
#include "np_hle.h"
#include "np_session.h"

#include <cstring>
#include <map>
#include <mutex>
#include <vector>

namespace bbnet::np {

namespace {

constexpr int kErrInvalidArg = static_cast<int>(0x80550003);
constexpr std::uint16_t kEvDead = 0x00;
constexpr std::uint16_t kEvEstablished = 0x01;
constexpr std::uint16_t kEvActive = 0x0c;

struct Ctx {
    void* cb = nullptr;
    void* arg = nullptr;
};
struct Conn {
    unsigned ctx = 0;
    std::string online_id;
    std::uint16_t member_id = 0;  // 0 until the peer is known
    bool announced = false;       // ESTABLISHED/ACTIVE scheduled
    bool dead = false;
};

std::mutex g_mu;
std::map<unsigned, Ctx> g_ctx;
std::map<unsigned, Conn> g_conn;  // by connection id
unsigned g_next_ctx = 1;
unsigned g_next_conn = 1;

// (ctxId, connId, event, errorCode, arg)
void fire(unsigned ctx, unsigned conn, std::uint16_t event, int error) {
    Ctx c;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto it = g_ctx.find(ctx);
        if (it == g_ctx.end() || !it->second.cb) return;
        c = it->second;
    }
    if (party_trace()) log("np: NpSignaling event 0x%x conn %u", event, conn);
    post_guest_call(reinterpret_cast<std::uintptr_t>(c.cb), ctx, conn, event,
                    static_cast<std::uint64_t>(static_cast<std::int64_t>(error)), reinterpret_cast<std::uint64_t>(c.arg));
}

// Once a connection has a known peer: ESTABLISHED at +200 ms, ACTIVE at +400.
void announce_locked(unsigned conn_id, Conn& c) {
    if (c.announced || !c.member_id) return;
    c.announced = true;
    session::peers_set_conn(c.member_id, conn_id, 2);
    const unsigned ctx = c.ctx;
    session::dispatch_after(200, session::Prio::Signaling, [ctx, conn_id] { fire(ctx, conn_id, kEvEstablished, 0); });
    session::dispatch_after(400, session::Prio::Signaling, [ctx, conn_id] { fire(ctx, conn_id, kEvActive, 0); });
}

BBNET_ABI int sig_init(std::uint64_t, int, int, std::uint64_t) {
    BBNET_GUEST_RETURN();
    log("sceNpSignalingInitialize");
    return 0;
}
BBNET_ABI int sig_term() {
    BBNET_GUEST_RETURN();
    std::lock_guard<std::mutex> lk(g_mu);
    g_ctx.clear();
    g_conn.clear();
    return 0;
}
// (const SceNpId*, callback, arg, ctxId*)
BBNET_ABI int sig_create_ctx(const void*, void* cb, void* arg, unsigned* ctx) {
    BBNET_GUEST_RETURN();
    if (!ctx) return kErrInvalidArg;
    std::lock_guard<std::mutex> lk(g_mu);
    const unsigned id = g_next_ctx++;
    g_ctx[id] = Ctx{cb, arg};
    *ctx = id;
    log("sceNpSignalingCreateContext -> %u", id);
    return 0;
}
BBNET_ABI int sig_delete_ctx(unsigned ctx) {
    BBNET_GUEST_RETURN();
    std::lock_guard<std::mutex> lk(g_mu);
    g_ctx.erase(ctx);
    for (auto it = g_conn.begin(); it != g_conn.end();) {
        it = it->second.ctx == ctx ? g_conn.erase(it) : std::next(it);
    }
    return 0;
}
// (ctxId, const SceNpId* peer, connId*): idempotent per peer (the game re-activates).
BBNET_ABI int sig_activate(unsigned ctx, const void* npid, unsigned* conn_out) {
    BBNET_GUEST_RETURN();
    if (!conn_out) return kErrInvalidArg;
    const std::string peer = npid_text(npid);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        for (auto& [id, c] : g_conn) {
            if (c.ctx == ctx && c.online_id == peer && !c.dead) {
                *conn_out = id;
                if (!c.member_id) {
                    session::Peer p;
                    if (session::peers_find_online(peer, &p)) c.member_id = p.member_id;
                }
                announce_locked(id, c);
                return 0;
            }
        }
    }
    Conn c;
    c.ctx = ctx;
    c.online_id = peer;
    session::Peer p;
    if (session::peers_find_online(peer, &p)) c.member_id = p.member_id;
    std::string how = c.member_id ? "" : " (peer not known yet)";
    if (!c.member_id) {
        // The host activates its connection to the guest it summons before the guest is a room
        // member; on PSN the signaling server resolved the address, here the party host does -
        // on the session thread: the game's thread never waits for the party link (a guest's
        // resolve is an RPC that can take its full timeout on a bad network). The connection
        // stays pending (status 1) and is announced once the peer is known.
        how = " (resolving)";
    }
    std::lock_guard<std::mutex> lk(g_mu);
    const unsigned id = g_next_conn++;
    g_conn[id] = c;
    *conn_out = id;
    log("sceNpSignalingActivateConnection %s -> conn %u%s", peer.c_str(), id, how.c_str());
    announce_locked(id, g_conn[id]);
    if (!c.member_id) {
        session::dispatch_after(0, session::Prio::Signaling, [peer] {
            std::uint32_t addr = 0;
            std::uint16_t port = 0;
            std::string error;
            if (!session::server_signaling_resolve(peer, &addr, &port, error)) {
                log("np: signaling resolve %s: %s", peer.c_str(), error.c_str());
                return;
            }
            const session::Peer prov = session::peers_provisional(peer, addr, port);
            log("np: signaling resolved %s at %u.%u.%u.%u:%u", peer.c_str(), addr & 0xff, (addr >> 8) & 0xff,
                (addr >> 16) & 0xff, addr >> 24, port);
            std::lock_guard<std::mutex> lk(g_mu);
            for (auto& [cid, cc] : g_conn) {
                if (!cc.dead && cc.online_id == peer && !cc.member_id) {  // a room member meanwhile keeps its id
                    cc.member_id = prov.member_id;
                    announce_locked(cid, cc);
                }
            }
        });
    }
    return 0;
}
BBNET_ABI int sig_deactivate(unsigned ctx, unsigned conn) {
    BBNET_GUEST_RETURN();
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_conn.find(conn);
    if (it != g_conn.end() && it->second.ctx == ctx) {
        it->second.dead = true;
        if (it->second.member_id) session::peers_set_conn(it->second.member_id, conn, 0);
    }
    return 0;
}
// (ctxId, connId, status*, SceNetInAddr*, SceInPort_t*)
BBNET_ABI int sig_status(unsigned ctx, unsigned conn, int* st, std::uint32_t* addr, std::uint16_t* port) {
    BBNET_GUEST_RETURN();
    int status = 0;
    std::uint32_t a = 0;
    std::uint16_t pt = 0;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto it = g_conn.find(conn);
        if (it != g_conn.end() && it->second.ctx == ctx && !it->second.dead) {
            session::Peer peer;
            if (it->second.member_id && session::peers_get(it->second.member_id, &peer) && peer.addr) {
                status = it->second.announced ? 2 : 1;
                a = peer.addr;
                pt = static_cast<std::uint16_t>((peer.port << 8) | (peer.port >> 8));
            } else {
                status = 1;
            }
        }
    }
    if (st) *st = status;
    if (addr) *addr = a;
    if (port) *port = pt;
    if (party_trace()) log("sceNpSignalingGetConnectionStatus conn %u -> %d", conn, status);
    return 0;
}

}  // namespace

void signaling_peer_known(std::uint16_t member_id) {
    session::Peer p;
    if (!session::peers_get(member_id, &p)) return;
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto& [id, c] : g_conn) {
        if (!c.dead && c.online_id == p.online_id) {
            c.member_id = member_id;
            announce_locked(id, c);
        }
    }
}

void signaling_peer_dead(const std::string& online_id) {
    std::vector<std::pair<unsigned, unsigned>> dead;  // ctx, conn
    {
        std::lock_guard<std::mutex> lk(g_mu);
        for (auto& [id, c] : g_conn) {
            if (!c.dead && c.online_id == online_id) {
                c.dead = true;
                dead.emplace_back(c.ctx, id);
            }
        }
    }
    for (const auto& [ctx, conn] : dead) {
        const unsigned cx = ctx, cn = conn;
        session::dispatch_after(100, session::Prio::Signaling, [cx, cn] { fire(cx, cn, kEvDead, 0); });
    }
}

void signaling_reset() {
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto& [id, c] : g_conn) {
        (void)id;
        c.dead = true;
    }
}

}  // namespace bbnet::np

namespace bbnet {

const Export kSignalingExports[] = {
    {"sceNpSignalingInitialize", reinterpret_cast<void*>(np::sig_init)},
    {"sceNpSignalingTerminate", reinterpret_cast<void*>(np::sig_term)},
    {"sceNpSignalingCreateContext", reinterpret_cast<void*>(np::sig_create_ctx)},
    {"sceNpSignalingDeleteContext", reinterpret_cast<void*>(np::sig_delete_ctx)},
    {"sceNpSignalingActivateConnection", reinterpret_cast<void*>(np::sig_activate)},
    {"sceNpSignalingDeactivateConnection", reinterpret_cast<void*>(np::sig_deactivate)},
    {"sceNpSignalingGetConnectionStatus", reinterpret_cast<void*>(np::sig_status)},
    {nullptr, nullptr},
};

}  // namespace bbnet

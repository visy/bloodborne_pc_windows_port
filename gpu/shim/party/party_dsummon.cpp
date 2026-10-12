// SPDX-License-Identifier: GPL-3.0-or-later
// Direct summon delivery: wire format, payload and decisions (party_dsummon.h). Payload layout
// ported from droogie/bbhost (GPL-3.0+) src/engine/summon_invite.cpp.
#include "party/party_dsummon.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace coop::dsummon {

bool SwitchOn(const char* v, bool default_on) {
    if (!v || !v[0]) return default_on;
    return !(std::strcmp(v, "0") == 0 || std::strcmp(v, "off") == 0 || std::strcmp(v, "false") == 0);
}

Kind KindOf(const std::string& b) {
    if (b.find("\"op\":\"dsum_req\"") != std::string::npos) return Kind::Req;
    if (b.find("\"op\":\"dsum_room\"") != std::string::npos) return Kind::Room;
    if (b.find("\"op\":\"dsum_done\"") != std::string::npos) return Kind::Done;
    return Kind::None;
}

std::string EncodeReq() { return "{\"op\":\"dsum_req\"}"; }
std::string EncodeRefuse() { return "{\"op\":\"dsum_room\",\"room\":\"\"}"; }
std::string EncodeDone(bool ok) { return ok ? "{\"op\":\"dsum_done\",\"ok\":1}" : "{\"op\":\"dsum_done\",\"ok\":0}"; }

std::string EncodeRoom(const Room& r) {
    std::string s = "{\"op\":\"dsum_room\",\"room\":\"";
    char h[3];
    for (std::uint8_t b : r.data) {
        std::snprintf(h, sizeof h, "%02x", b);
        s += h;
    }
    char tail[160];
    std::snprintf(tail, sizeof tail, "\",\"area\":%u,\"pos\":[%.4f,%.4f,%.4f],\"level\":%u}", r.area, double(r.pos[0]),
                  double(r.pos[1]), double(r.pos[2]), r.level);
    return s + tail;
}

namespace {
int Nib(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}
}  // namespace

bool DecodeRoom(const std::string& body, Room* out, bool* refused, std::string* err) {
    *refused = false;
    const char* b = body.c_str();
    const char* p = std::strstr(b, "\"room\":\"");
    if (KindOf(body) != Kind::Room || !p) {
        *err = "not a dsum_room";
        return false;
    }
    p += 8;
    out->data.clear();
    while (*p && *p != '"') {
        const int a = Nib(p[0]), c = p[1] ? Nib(p[1]) : -1;
        if (a < 0 || c < 0 || out->data.size() >= kMaxRoomData) {
            *err = "bad room data";
            return false;
        }
        out->data.push_back(std::uint8_t(a << 4 | c));
        p += 2;
    }
    if (out->data.empty()) {
        *refused = true;
        *err = "refused by the host";
        return false;
    }
    if (out->data.size() <= 8) {
        *err = "room data too short";
        return false;
    }
    const char* pa = std::strstr(b, "\"area\":");
    const char* pp = std::strstr(b, "\"pos\":[");
    const char* pl = std::strstr(b, "\"level\":");
    unsigned area = 0, level = 0;
    float x = 0, y = 0, z = 0;
    if (!pa || !pp || !pl || std::sscanf(pa, "\"area\":%u", &area) != 1 ||
        std::sscanf(pp, "\"pos\":[%f,%f,%f]", &x, &y, &z) != 3 || std::sscanf(pl, "\"level\":%u", &level) != 1) {
        *err = "missing area / pos / level";
        return false;
    }
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || std::fabs(x) >= 1e5f || std::fabs(y) >= 1e5f ||
        std::fabs(z) >= 1e5f || level > 544) {
        *err = "position or level out of range";
        return false;
    }
    out->area = area;
    out->pos[0] = x;
    out->pos[1] = y;
    out->pos[2] = z;
    out->level = level;
    return true;
}

bool DecodeDone(const std::string& body, bool* ok) {
    if (KindOf(body) != Kind::Done) return false;
    *ok = body.find("\"ok\":1") != std::string::npos;
    return true;
}

std::vector<std::uint8_t> BuildPayload(const std::uint8_t entry_id[8], const Room& r) {
    std::vector<std::uint8_t> p(kHeadSize + r.data.size(), 0);
    std::memcpy(&p[0x00], entry_id, 8);
    std::memcpy(&p[0x0c], &r.area, 4);
    std::memcpy(&p[0x10], r.pos, 12);
    std::memcpy(&p[0x1c], &r.level, 4);
    const std::uint64_t no_channel = 0x8000000000000000ull;
    std::memcpy(&p[0x28], &no_channel, 8);
    const std::uint32_t n = std::uint32_t(r.data.size());
    std::memcpy(&p[0x30], &n, 4);
    if (n) std::memcpy(&p[kHeadSize], r.data.data(), n);
    return p;
}

const char* HostRefusal(const HostView& v) {
    if (!v.world_up || v.loading) return "the host is not in its world";
    if (v.cooperators >= v.max_cooperators) return "the host's world is full";
    if (v.ms_since_sign_summon < 20000) return "the host's game is summoning a sign";
    if (v.other_hold_ms > 0) return "a crashed member's connection is still held";
    return nullptr;
}

const char* PhaseName(Phase p) {
    switch (p) {
    case Phase::Idle: return "idle";
    case Phase::Asked: return "asked";
    case Phase::HaveRoom: return "have room";
    case Phase::Delivered: return "delivered";
    case Phase::Done: return "done";
    case Phase::Failed: return "failed";
    }
    return "?";
}

void OnAsked(Guest& g, double now) {
    g.phase = Phase::Asked;
    g.since = now;
    g.deliver_tries = 0;
}

void OnRoom(Guest& g, double now, bool refused) {
    if (g.phase != Phase::Asked) return;  // late or unasked: ignored
    g.phase = refused ? Phase::Failed : Phase::HaveRoom;
    g.since = now;
}

void OnDelivered(Guest& g, double now, bool ok) {
    if (ok) {
        g.phase = Phase::Delivered;
        g.since = now;
    } else {
        ++g.deliver_tries;
    }
}

Action Step(Guest& g, double now, bool entry_waiting, bool joined) {
    switch (g.phase) {
    case Phase::Asked:
        if (now - g.since > kAnswerWait) {
            g.phase = Phase::Failed;
            return Action::None;  // the host never answered: it holds nothing
        }
        return Action::None;
    case Phase::HaveRoom:
        if (joined) {  // the sign path won the race
            g.phase = Phase::Done;
            return Action::ReportOk;
        }
        if (now - g.since > kEntryWait || g.deliver_tries >= 3) {
            g.phase = Phase::Failed;
            return Action::ReportFail;
        }
        return entry_waiting ? Action::Deliver : Action::None;
    case Phase::Delivered:
        if (joined) {
            g.phase = Phase::Done;
            return Action::ReportOk;
        }
        if (now - g.since > kJoinWait) {
            g.phase = Phase::Failed;
            return Action::ReportFail;
        }
        return Action::None;
    default:
        return Action::None;
    }
}

}  // namespace coop::dsummon

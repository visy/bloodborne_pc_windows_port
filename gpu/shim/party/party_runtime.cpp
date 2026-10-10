// SPDX-License-Identifier: GPL-3.0-or-later
// The party runtime (party_runtime.h): wires PartyLink (A4) to the host service / transport (A3)
// and the network library (A1/A2) inside the game process.
#include "party_runtime.h"

#include "party_addr.h"
#include "party_code.h"
#include "party_crypto.h"
#include "party_director.h"
#include "party_story.h"
#include "party_phantom.h"
#include "party_fourp.h"
#include "party_travel.h"
#include "upnp_win.h"

#include "../net/bbnet_internal.h"
#include "../net/np_hle.h"
#include "../net/np_session.h"
#include "../net/party_host_service.h"
#include "../net/party_transport.h"
#include "../net/party_util.h"
#include "../../bbnet.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace party {

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

void plog(const char* fmt, ...) __attribute__((format(gnu_printf, 1, 2)));
void plog(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::printf("Party: %s\n", buf);
    std::fflush(stdout);
}

const char* env(const char* name) {
    const char* v = std::getenv(name);
    return v && *v ? v : nullptr;
}

bool env_on(const char* name) {
    const char* v = env(name);
    return v && v[0] != '0';
}

bool loopback_mode() { return env_on("BB_PARTY_LOOPBACK") || env_on("BB_MP_LOCAL_TEST"); }

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// ---- SHA-256 (FIPS 180-4), for the eboot hash in HELLO -------------------------------------

class Sha256 {
public:
    Sha256() { reset(); }
    void reset() {
        static const std::uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                              0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        std::memcpy(h_, init, sizeof h_);
        len_ = 0;
        fill_ = 0;
    }
    void update(const void* data, std::size_t n) {
        const auto* p = static_cast<const std::uint8_t*>(data);
        len_ += n;
        if (fill_) {
            const std::size_t take = std::min<std::size_t>(64 - fill_, n);
            std::memcpy(buf_ + fill_, p, take);
            fill_ += take;
            p += take;
            n -= take;
            if (fill_ == 64) {
                block(buf_);
                fill_ = 0;
            }
        }
        while (n >= 64) {
            block(p);
            p += 64;
            n -= 64;
        }
        if (n) {
            std::memcpy(buf_, p, n);
            fill_ = n;
        }
    }
    std::array<std::uint8_t, 32> finish() {
        const std::uint64_t bits = len_ * 8;
        const std::uint8_t pad = 0x80;
        update(&pad, 1);
        const std::uint8_t zero = 0;
        while (fill_ != 56) update(&zero, 1);
        std::uint8_t lenb[8];
        for (int i = 0; i < 8; ++i) lenb[i] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
        update(lenb, 8);
        std::array<std::uint8_t, 32> out{};
        for (int i = 0; i < 8; ++i) {
            out[4 * i] = static_cast<std::uint8_t>(h_[i] >> 24);
            out[4 * i + 1] = static_cast<std::uint8_t>(h_[i] >> 16);
            out[4 * i + 2] = static_cast<std::uint8_t>(h_[i] >> 8);
            out[4 * i + 3] = static_cast<std::uint8_t>(h_[i]);
        }
        return out;
    }

private:
    static std::uint32_t ror(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void block(const std::uint8_t* p) {
        static const std::uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (std::uint32_t(p[4 * i]) << 24) | (std::uint32_t(p[4 * i + 1]) << 16) |
                   (std::uint32_t(p[4 * i + 2]) << 8) | std::uint32_t(p[4 * i + 3]);
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t t1 = h + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const std::uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d;
        h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
    }
    std::uint32_t h_[8];
    std::uint64_t len_;
    std::uint8_t buf_[64];
    std::size_t fill_;
};

std::string hex_prefix(const std::array<std::uint8_t, 32>& h, int bytes = 6) {
    std::string s;
    char b[3];
    for (int i = 0; i < bytes; ++i) {
        std::snprintf(b, sizeof b, "%02x", h[i]);
        s += b;
    }
    return s;
}

bool sha256_file(const fs::path& path, std::array<std::uint8_t, 32>* out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    Sha256 sha;
    std::vector<char> buf(1 << 20);
    while (f) {
        f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const std::streamsize got = f.gcount();
        if (got > 0) sha.update(buf.data(), static_cast<std::size_t>(got));
    }
    *out = sha.finish();
    return true;
}

bool read_file(const fs::path& path, std::string* out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    *out = ss.str();
    return true;
}

std::string data_dir() { return env("BB_DATA_DIR") ? env("BB_DATA_DIR") : "."; }

std::string user_dir() {
    const std::string& configured = bbnet::settings().user_dir;
    if (!configured.empty()) return configured;
    if (const char* u = env("BB_GPU_USER_DIR")) return u;
    if (const char* u = env("BB_USER_DIR")) return u;
    return data_dir() + "/user";
}

// The game's eboot.bin: app0 (bbnet_configure) or BB_GAME_DIR. Zeros (with a warning) when it
// cannot be read: then only players who also could not read theirs match.
std::array<std::uint8_t, 32> eboot_hash() {
    std::array<std::uint8_t, 32> h{};
    std::string dir = bbnet::settings().app0;
    if (dir.empty() && env("BB_GAME_DIR")) dir = env("BB_GAME_DIR");
    if (dir.empty()) {
        plog("warning: no game directory (BB_GAME_DIR) to hash eboot.bin from");
        return h;
    }
    const auto t0 = Clock::now();
    const fs::path p = fs::path(dir) / "eboot.bin";
    if (!sha256_file(p, &h)) {
        plog("warning: cannot read %s for the game version check", p.string().c_str());
        return h;
    }
    plog("eboot.bin sha256 %s... (%.0f ms)", hex_prefix(h).c_str(),
         std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
    return h;
}

// The gameplay patches (BB_PARTY_HASH_PATCHES=0: not compared): out/party_patch_hash.txt, which
// scripts/patches.py writes next to patches.bin with the hash and names of the patches that are
// not cosmetic (graphics, FPS, resolution ... do not count). Without it (an older patches.py) the
// whole patches.bin is hashed, as before. Fills cfg.patches_hash / patch_names.
void patch_identity(LinkConfig& cfg, std::string* what) {
    if (env("BB_PARTY_HASH_PATCHES") && !env_on("BB_PARTY_HASH_PATCHES")) {
        *what += "patches not compared (BB_PARTY_HASH_PATCHES=0)";
        return;
    }
    const fs::path out = fs::path(data_dir()) / "out";
    std::string data;
    if (read_file(out / "party_patch_hash.txt", &data) &&
        parse_identity_file(data, "patch", &cfg.patches_hash, &cfg.patch_names)) {
        *what += std::to_string(cfg.patch_names.size()) + " gameplay patches";
        return;
    }
    if (read_file(out / "patches.bin", &data)) {
        const std::string blob = std::string("bbparty-patches-bin\0", 20) + data;
        cfg.patches_hash = crypto::blake2b256(blob.data(), blob.size());
        cfg.patch_names = {"patches.bin (all patches: no party_patch_hash.txt)"};
        *what += "patches.bin " + std::to_string(data.size()) + " B (no party_patch_hash.txt)";
        return;
    }
    *what += "no patches";
}

// The gameplay mods: out/party_mods.txt from scripts/mods.py (the enabled mods' winning files that
// are not cosmetic - textures, shaders, sound, fonts ... do not count; ReShade-type files outside
// dvdroot_ps4 never do). No file (mods.py did not run or failed: no mods applied) or mods off:
// none. Fills cfg.mods_hash / mod_names.
void mod_identity(LinkConfig& cfg, std::string* what) {
    if (env("BB_MODS_ENABLED") && !env_on("BB_MODS_ENABLED")) {
        *what += ", mods off";
        return;
    }
    std::string data;
    const fs::path f = fs::path(data_dir()) / "out" / "party_mods.txt";
    if (read_file(f, &data) && parse_identity_file(data, "mod", &cfg.mods_hash, &cfg.mod_names)) {
        *what += cfg.mod_names.empty() ? ", no gameplay mods" : ", " + std::to_string(cfg.mod_names.size()) + " gameplay mods";
        return;
    }
    *what += ", no party_mods.txt (no mods applied)";
}

std::string joined(const std::vector<std::string>& v) {
    std::string s;
    for (const std::string& x : v) s += (s.empty() ? "" : ", ") + x;
    return s.empty() ? "none" : s;
}

// ---- state --------------------------------------------------------------------------------

struct Pump {
    std::string name;
    int slot = 0;
    std::atomic<bool> stop{false};
};

struct Runtime {
    std::mutex mu;
    std::condition_variable cv;
    RuntimeStatus st;
    bool stopping = false;
    bool shut = false;
    PartyLink* link = nullptr;  // never deleted (the director may hold it)
    std::unique_ptr<UpnpMapper> upnp;
    std::map<std::string, std::shared_ptr<Pump>> pumps;  // host: per guest online id
    bool rpc_wired = false;
    bool have_local_state = false;
    MemberState local_state = MemberState::Title;
    std::uint32_t local_map = 0;
    std::string roster_sig;
    Clock::time_point last_roster_log{};
    std::string marker;  // <user>/party_state.json while this run is up
    std::string marker_head;  // its pid / role / name / started members
    bool marker_secret = false;
    std::array<std::uint8_t, 8> secret{};  // host: the party code's secret (kept across a crash)
    // guest
    std::uint32_t host_ip_nbo = 0;
    std::uint16_t host_port = 0;
    bool was_connected = false;
    std::uint64_t loss_gen = 0;  // bumps on every link state change (host-lost grace check)
    Clock::time_point lost_at{};
};

std::mutex g_marker_mu;  // serializes marker writes (startup thread, link callback thread)

Runtime& R() {
    static Runtime* r = new Runtime;  // lives as long as the process (exit paths use it)
    return *r;
}

void set_error(const std::string& e) {
    std::lock_guard<std::mutex> lk(R().mu);
    R().st.last_error = e;
}

std::string roster_text(const std::vector<RosterEntry>& roster, bool with_ping) {
    std::string s;
    for (const RosterEntry& e : roster) {
        if (!s.empty()) s += ", ";
        char b[160];
        if (with_ping) {
            std::snprintf(b, sizeof b, "%s(slot %d, %s%s, map %u, %u ms)", e.name.c_str(), e.slot,
                          member_state_name(e.state), e.connected ? "" : ", lost", e.map_id, e.ping_ms);
        } else {
            std::snprintf(b, sizeof b, "%s(slot %d, %s%s, map %u)", e.name.c_str(), e.slot, member_state_name(e.state),
                          e.connected ? "" : ", lost", e.map_id);
        }
        s += b;
    }
    return s.empty() ? "(empty)" : s;
}

void write_text_file(const fs::path& path, const std::string& text);

std::string hex_of(const std::uint8_t* p, std::size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (std::size_t i = 0; i < n; ++i) {
        s += d[p[i] >> 4];
        s += d[p[i] & 15];
    }
    return s;
}

bool unhex(const std::string& t, std::uint8_t* out, std::size_t n) {
    if (t.size() != 2 * n) return false;
    auto v = [](char c) {
        return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    };
    for (std::size_t i = 0; i < n; ++i) {
        const int hi = v(t[2 * i]), lo = v(t[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<std::uint8_t>(hi << 4 | lo);
    }
    return true;
}

// The crash marker <user>/party_state.json: pid, role, name, start time and, on the host, the
// party secret and the member table (slot, name, resume token), so a restarted host keeps its
// party code and gives every member its slot back. Rewritten on every member change.
void write_marker() {
    Runtime& r = R();
    std::lock_guard<std::mutex> mk(g_marker_mu);
    std::string path, head;
    bool with_secret;
    std::array<std::uint8_t, 8> secret;
    PartyLink* link;
    {
        std::lock_guard<std::mutex> lk(r.mu);
        if (r.shut || r.marker.empty()) return;
        path = r.marker;
        head = r.marker_head;
        with_secret = r.marker_secret;
        secret = r.secret;
        link = r.link;
    }
    std::string body = "{" + head;
    if (with_secret) body += ",\"secret\":\"" + hex_of(secret.data(), secret.size()) + "\"";
    if (link && link->is_host()) {
        body += ",\"members\":[";
        bool first = true;
        for (const KeptMember& k : link->kept_members()) {
            body += std::string(first ? "" : ",") + "{\"slot\":" + std::to_string(k.slot) + ",\"name\":\"" + k.name +
                    "\",\"token\":\"" + hex_of(k.token.data(), k.token.size()) + "\"}";
            first = false;
        }
        body += "]";
    }
    body += "}\n";
    write_text_file(path, body);
}

// ---- host: event pumps --------------------------------------------------------------------

void pump_main(std::shared_ptr<Pump> p, PartyLink* link) {
    bbnet::party::PartyHostService& svc = bbnet::party::PartyHostService::instance();
    std::uint64_t cursor = 0;
    while (!p->stop.load()) {
        std::vector<json::Value> evs = svc.wait_events(p->name, cursor, 1000);
        if (p->stop.load()) break;
        std::uint64_t last = cursor;
        for (const json::Value& ev : evs) {
            const auto id = static_cast<std::uint64_t>(bbnet::party::int_of(ev, "EventId", 0));
            if (id && id <= cursor) continue;
            // PartyLink owns delivery from here (queued and replayed for a lost member).
            link->send_event(p->slot, bbnet::party::str_of(ev, "Name"), json::dump(ev, 0));
            if (id > last) last = id;
        }
        if (last > cursor) {
            cursor = last;
            svc.ack_events(p->name, cursor);
        }
    }
}

void pump_start(const RosterEntry& m) {
    Runtime& r = R();
    std::lock_guard<std::mutex> lk(r.mu);
    auto it = r.pumps.find(m.name);
    if (it != r.pumps.end()) {
        if (it->second->slot == m.slot && !it->second->stop.load()) return;  // resumed: keep its cursor
        it->second->stop = true;
    }
    auto p = std::make_shared<Pump>();
    p->name = m.name;
    p->slot = m.slot;
    r.pumps[m.name] = p;
    PartyLink* link = r.link;
    std::thread(pump_main, p, link).detach();
}

void pump_stop(const std::string& name) {
    Runtime& r = R();
    {
        std::lock_guard<std::mutex> lk(r.mu);
        auto it = r.pumps.find(name);
        if (it == r.pumps.end()) return;
        it->second->stop = true;
        r.pumps.erase(it);
    }
    bbnet::party::PartyHostService::instance().wake_all();
}

// ---- callbacks ----------------------------------------------------------------------------

// Guest: the host is gone - its game crashed or quit without BYE (the link dropped and did not
// come back within the grace), or it restarted and does not know us (WELCOME not resumed). The
// game's room with it ends the way a host leaving ends it (np party_host_lost: ROOM_DESTROYED),
// so the game goes home on its own; the link keeps reconnecting and the director rings again.
int host_lost_grace_ms() {
    const char* v = env("BB_PARTY_HOST_LOST_MS");
    return v ? std::max(0, std::atoi(v)) : 2000;
}

void host_lost(const std::string& why) {
    if (bbnet::np::party_host_lost(why)) plog("host lost (%s): the game leaves the host's world", why.c_str());
    else plog("host lost (%s): not in the host's world", why.c_str());
}

void on_state(LinkState s, const std::string& detail) {
    Runtime& r = R();
    PartyLink* link;
    bool host;
    std::uint64_t gen;
    {
        std::lock_guard<std::mutex> lk(r.mu);
        r.st.state = s;
        link = r.link;
        host = r.st.role == RuntimeRole::Host;
        gen = ++r.loss_gen;
    }
    if (!link) return;
    if (!host && (s == LinkState::Reconnecting || s == LinkState::Rejected)) {
        bool first = false;
        {
            std::lock_guard<std::mutex> lk(r.mu);
            if (r.was_connected) {
                first = true;
                r.was_connected = false;
                r.lost_at = Clock::now();
            }
        }
        if (first) {
            const int grace = s == LinkState::Rejected ? 0 : host_lost_grace_ms();
            std::thread([gen, grace, detail] {
                std::this_thread::sleep_for(std::chrono::milliseconds(grace));
                {
                    std::lock_guard<std::mutex> lk(R().mu);
                    if (R().loss_gen != gen && R().st.state == LinkState::Connected) return;  // back already
                }
                host_lost("party link down: " + detail);
            }).detach();
        }
    }
    switch (s) {
    case LinkState::Connected: {
        const int slot = link->local_slot();
        {
            std::lock_guard<std::mutex> lk(r.mu);
            r.st.local_slot = slot;
            r.st.last_error.clear();
        }
        plog("connected to the host as slot %d (we are seen at %s)%s%s", slot, link->observed_address().c_str(),
             detail.empty() ? "" : ": ", detail.c_str());
        const bool resumed = link->last_welcome_resumed();
        bool again;
        Clock::time_point lost_at;
        {
            std::lock_guard<std::mutex> lk(r.mu);
            again = r.rpc_wired;
            lost_at = r.lost_at;
            r.was_connected = true;
        }
        if (again) {
            plog("back in the party after %.1f s%s", std::chrono::duration<double>(Clock::now() - lost_at).count(),
                 resumed ? " (session resumed)" : " (the host restarted: a new session)");
            if (!resumed) {
                if (bbnet::party::RemoteGuest* rg = bbnet::party::remote_guest()) rg->reset_event_cursor();
                host_lost("the host does not know our session any more");
            }
        }
        bbnet::party::RemoteGuest* rg = bbnet::party::remote_guest();
        bool wire = false;
        {
            std::lock_guard<std::mutex> lk(r.mu);
            wire = !r.rpc_wired;
            r.rpc_wired = true;
        }
        if (rg && wire) {
            rg->set_rpc([link](const char* kind, const std::string& req, std::string& reply, std::string& error,
                               int timeout_ms) {
                std::string out;
                if (!link->rpc_call(kind, req, &out, timeout_ms)) {
                    error = out.empty() ? std::string("party link: ") + kind + " failed" : out;
                    return false;
                }
                reply = std::move(out);
                return true;
            });
            const bool lb = bbnet::party::ip_is_loopback(r.host_ip_nbo);
            rg->set_host_endpoint(r.host_ip_nbo, r.host_port, lb);
            bbnet::session::set_stun_server(bbnet::party::ip_text(r.host_ip_nbo), r.host_port);
            plog("transport: RPC and events over the party link; STUN at %s:%u",
                 bbnet::party::ip_text(r.host_ip_nbo).c_str(), r.host_port);
        } else if (!rg) {
            plog("warning: this process has no RemoteGuest transport (BB_PARTY_HOST set on a guest?)");
        }
        break;
    }
    case LinkState::Connecting: break;  // PartyLink logs it (on_log)
    case LinkState::Reconnecting: plog("connection lost; reconnecting%s%s", detail.empty() ? "" : ": ", detail.c_str()); break;
    case LinkState::Rejected: {
        const RejectCode code = link->reject_code();
        if (code == RejectCode::Mismatch) {
            // The host names what differs (identity_mismatch): eboot.bin, gameplay patches, mods.
            set_error("host refused: " + link->reject_reason());
            plog("the host refused this game: %s", link->reject_reason().c_str());
            plog("make the listed patches / mods / game version the same as the host's (the launcher's Patches "
                 "and Mods tabs), or BB_PARTY_HASH_PATCHES=0 on every player to skip the patch check");
            break;
        }
        const std::string why = std::string("rejected (") + std::to_string(static_cast<int>(code)) +
                                "): " + link->reject_reason();
        set_error(why);
        plog("%s", why.c_str());
        break;
    }
    default: break;  // hosting / stopped: PartyLink logs it (on_log)
    }
}

void on_roster(const std::vector<RosterEntry>& roster) {
    Runtime& r = R();
    const std::string sig = roster_text(roster, false);
    bool log_it = false;
    {
        std::lock_guard<std::mutex> lk(r.mu);
        r.st.roster = roster;
        const auto now = Clock::now();
        if (sig != r.roster_sig || now - r.last_roster_log >= std::chrono::seconds(15)) {
            r.roster_sig = sig;
            r.last_roster_log = now;
            log_it = true;
        }
    }
    if (log_it) plog("roster: %s", roster_text(roster, true).c_str());
}

LinkCallbacks make_callbacks(bool host) {
    LinkCallbacks cb;
    cb.on_state = on_state;
    cb.on_roster = on_roster;
    cb.on_log = [](const std::string& line) { plog("link: %s", line.c_str()); };
    if (host) {
        cb.on_member_joined = [](const RosterEntry& m, bool rejoined) {
            plog("%s %s (slot %d)", m.name.c_str(), rejoined ? "is back" : "joined", m.slot);
            pump_start(m);
            write_marker();
        };
        cb.on_member_left = [](const RosterEntry& m, bool slot_kept) {
            if (slot_kept) {
                plog("%s (slot %d) lost; slot kept", m.name.c_str(), m.slot);
                return;
            }
            plog("%s (slot %d) left", m.name.c_str(), m.slot);
            write_marker();
            pump_stop(m.name);
            bbnet::party::PartyHostService::instance().context_gone(m.name);
        };
        cb.on_rpc = [](int slot, const std::string& kind, const std::string& body) -> std::string {
            PartyLink* link = R().link;
            bbnet::party::Caller caller;
            if (link) {
                for (const RosterEntry& e : link->roster())
                    if (e.slot == slot) caller.online_id = e.name;
                caller.link_addr = link->member_ip(slot);
            }
            json::Value req, reply;
            std::string err;
            if (caller.online_id.empty()) {  // no authenticated roster name: it cannot act as anyone
                reply = json::Value::make_object();
                reply.set("ResKind", 7);
                reply.set("Error", "caller not authenticated");
                return json::dump(reply, 0);
            }
            if (!json::parse(body, req, err)) {
                reply = json::Value::make_object();
                reply.set("ResKind", 1);
                reply.set("Error", "bad request: " + err);
                return json::dump(reply, 0);
            }
            if (bbnet::party_trace()) plog("rpc from %s: %s %s", caller.online_id.c_str(), kind.c_str(), body.c_str());
            bbnet::party::host_handle(caller, kind, req, reply);
            return json::dump(reply, 0);
        };
    } else {
        cb.on_event = [](int, std::uint64_t, const std::string& name, const std::string& body) {
            if (name == coop::kTravelEventName) {  // B1: follow the host's warp
                coop::TravelIntent t;
                std::string err;
                if (coop::TravelFromJsonText(body, &t, &err)) coop::RequestGuestTravel(t);
                else plog("travel event: %s", err.c_str());
                return;
            }
            if (name == coop::kStoryEventName) {  // C4: the host's cutscene / ending
                coop::StoryIntent s;
                std::string err;
                if (coop::StoryFromJsonText(body, &s, &err)) coop::RequestGuestStory(s);
                else plog("story event: %s", err.c_str());
                return;
            }
            if (name == coop::kPhantomEventName) {  // host lamp / rested / boss Insight
                coop::PhantomOnEvent(body);
                return;
            }
            bbnet::party::RemoteGuest* rg = bbnet::party::remote_guest();
            const std::uint64_t id = rg ? rg->on_link_event(body) : 0;
            if (bbnet::party_trace()) plog("event %s id %llu", name.c_str(), static_cast<unsigned long long>(id));
        };
    }
    return cb;
}

// ---- crash recovery -----------------------------------------------------------------------

fs::path marker_path() { return fs::path(user_dir()) / "party_state.json"; }

struct RestartInfo {
    bool restarted = false;
    std::string why;
    std::string marker_body;  // the previous run's marker (host: secret, members)
};

const RestartInfo& restart_info() {
    static RestartInfo info;
    static std::once_flag once;
    std::call_once(once, [] {
        if (env("BB_PARTY_RESTARTED") && env_on("BB_PARTY_RESTARTED")) {
            info.restarted = true;
            info.why = "BB_PARTY_RESTARTED=1";
        }
        std::error_code ec;
        const fs::path m = marker_path();
        if (fs::exists(m, ec)) {
            const auto age = fs::file_time_type::clock::now() - fs::last_write_time(m, ec);
            if (!ec && age < std::chrono::minutes(30)) {
                std::string body;
                read_file(m, &body);
                info.marker_body = body;
                while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) body.pop_back();
                info.restarted = true;
                info.why += std::string(info.why.empty() ? "" : "; ") + "unclean exit marker " + m.string() + " " + body;
            }
        }
    });
    return info;
}

// ---- startup ------------------------------------------------------------------------------

bool stopping() {
    std::lock_guard<std::mutex> lk(R().mu);
    return R().stopping;
}

void write_text_file(const fs::path& path, const std::string& text) {
    std::error_code ec;
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            plog("warning: cannot write %s", path.string().c_str());
            return;
        }
        f << text;
    }
    fs::rename(tmp, path, ec);  // replaces on Windows too (MoveFileEx REPLACE_EXISTING)
    if (ec) {
        fs::remove(path, ec);
        fs::rename(tmp, path, ec);
    }
}

// A restarted host's party secret and members: the crash marker's, or (an older marker) the
// secret of the code it wrote to <user>/party_code.txt. The party code stays the same.
bool previous_party(std::array<std::uint8_t, 8>* secret, std::vector<KeptMember>* members, std::string* from) {
    const RestartInfo& ri = restart_info();
    json::Value v;
    std::string err;
    if (!ri.marker_body.empty() && json::parse(ri.marker_body, v, err) && v.type == json::Value::Type::Object &&
        unhex(bbnet::party::str_of(v, "secret"), secret->data(), secret->size())) {
        *from = "the crash marker";
        if (const json::Value* ms = v.find("members"); ms && ms->type == json::Value::Type::Array) {
            for (const json::Value& m : ms->array) {
                KeptMember k;
                k.slot = static_cast<int>(bbnet::party::int_of(m, "slot", 0));
                k.name = bbnet::party::str_of(m, "name");
                if (!unhex(bbnet::party::str_of(m, "token"), k.token.data(), k.token.size())) k.token = {};
                if (k.slot > 0 && !k.name.empty()) members->push_back(std::move(k));
            }
        }
        return true;
    }
    std::string body;
    if (read_file(fs::path(user_dir()) / "party_secret.txt", &body)) {
        while (!body.empty() && std::isspace(static_cast<unsigned char>(body.back()))) body.pop_back();
        if (unhex(body, secret->data(), secret->size())) {
            *from = "party_secret.txt";
            return true;
        }
    }
    if (read_file(fs::path(user_dir()) / "party_code.txt", &body)) {
        std::istringstream in(body);
        std::string line;
        while (std::getline(in, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            const auto eq = line.find('=');
            const std::string c = eq == std::string::npos ? line : line.substr(eq + 1);
            PartyCode code;
            std::string e;
            if (!c.empty() && decode_party_code(c, &code, &e)) {
                *secret = code.secret;
                *from = "party_code.txt";
                return true;
            }
        }
    }
    return false;
}

void start_host(LinkConfig cfg) {
    Runtime& r = R();
    const bool lb = loopback_mode();
    std::vector<KeptMember> kept;
    std::string from;
    const bool restarted = restart_info().restarted;
    if (restarted && previous_party(&cfg.secret, &kept, &from)) {
        plog("restarted host: keeping the party code (secret from %s) and %zu member slot(s)", from.c_str(), kept.size());
    } else {
        crypto::random_bytes(cfg.secret.data(), cfg.secret.size());
    }
    // <user>/party_secret.txt: the restart's second source after the crash marker.
    write_text_file(fs::path(user_dir()) / "party_secret.txt", hex_of(cfg.secret.data(), cfg.secret.size()) + "\n");
    {
        std::lock_guard<std::mutex> lk(r.mu);
        r.secret = cfg.secret;
        r.marker_secret = true;
    }
    if (lb) cfg.bind_addr = "0.0.0.0";
    const std::string name = cfg.name;
    const std::uint16_t port = cfg.port;

    // The game's party UDP port first: STUN below goes out from it (so the mapping is that
    // port's) and the host's STUN responder / relay is up before any guest asks. The TCP link
    // shares the number, which is fine (different protocol).
    if (!bbnet::p2p_open()) plog("warning: cannot open the party UDP port %u (another program?)", port);

    auto* link = new PartyLink(cfg, make_callbacks(true));
    {
        std::lock_guard<std::mutex> lk(r.mu);
        r.link = link;
    }
    if (!kept.empty()) link->restore_members(kept);
    std::string err;
    // A restarted host's port can be held a little longer by the dead process's connections:
    // retry for up to 60 s (the guests keep reconnecting meanwhile).
    const auto bind_deadline = Clock::now() + std::chrono::seconds(restarted ? 60 : 0);
    bool up = link->start_host(&err);
    while (!up && Clock::now() < bind_deadline && !stopping()) {
        plog("cannot host on TCP port %u yet (%s); retrying", port, err.c_str());
        std::this_thread::sleep_for(std::chrono::seconds(1));
        up = link->start_host(&err);
    }
    if (!up) {
        set_error("cannot host: " + err);
        plog("cannot host on TCP port %u: %s", port, err.c_str());
        return;
    }
    coop::PartyDirector::Get().SetLink(link);
    write_marker();

    bbnet::party::PartyHostService& svc = bbnet::party::PartyHostService::instance();
    svc.set_loading_query([](const std::string& id) {
        PartyLink* l = R().link;
        if (!l) return false;
        for (const RosterEntry& e : l->roster())
            if (e.name == id) return e.state == MemberState::Loading || !e.connected;
        return false;
    });
    bbnet::party::transport();  // LocalHost over the service

    std::string upnp_ip, upnp_msg;
    if (lb) {
        upnp_msg = "off (loopback test)";
    } else if (!UpnpMapper::enabled_by_env()) {
        upnp_msg = "off (BB_PARTY_UPNP=0)";
    } else {
        auto m = std::make_unique<UpnpMapper>();
        m->start(port);
        const UpnpResult u = m->wait(3000);
        upnp_ip = u.external_ip;
        upnp_msg = u.done ? u.message : "still trying: " + u.message;
        std::lock_guard<std::mutex> lk(r.mu);
        r.upnp = std::move(m);
    }
    plog("UPnP: %s", upnp_msg.c_str());

    // The public address: loopback test / BB_PARTY_PUBLIC_ADDR / STUN from the game's port / UPnP.
    std::array<std::uint8_t, 4> pub{};
    std::uint16_t pub_port = port;
    std::string source;
    if (lb) {
        pub = {127, 0, 0, 1};
        source = "loopback";
    } else if (const char* ov = env("BB_PARTY_PUBLIC_ADDR"); ov && resolve_ipv4(ov, port, &pub, &pub_port)) {
        source = "override";
    } else {
        const char* stun_env = env("BB_PARTY_STUN");
        const std::string server = stun_env ? stun_env : "stun.l.google.com:19302";
        if (server != "off" && server != "0") {
            std::array<std::uint8_t, 4> sip{};
            std::uint16_t sport = 19302;
            std::string host_part = server;
            if (const auto c = server.rfind(':'); c != std::string::npos) {
                host_part = server.substr(0, c);
                sport = static_cast<std::uint16_t>(std::strtoul(server.c_str() + c + 1, nullptr, 10));
            }
            std::uint32_t ma = 0;
            std::uint16_t mp = 0;
            bool ok = false;
            for (int i = 0; i < 3 && !ok && !stopping(); ++i)
                ok = bbnet::p2p_stun(host_part.c_str(), sport, 2000, &ma, &mp);
            (void)sip;
            if (ok) {
                std::memcpy(pub.data(), &ma, 4);
                pub_port = mp;
                source = "stun " + server;
                if (mp != port) plog("note: the NAT maps UDP %u to %u: guests may need the host relay", port, mp);
            } else {
                plog("STUN %s: no answer", server.c_str());
            }
        }
        if (source.empty() && !upnp_ip.empty() && parse_ipv4(upnp_ip, &pub)) source = "upnp";
    }

    std::uint8_t flags = 0;
    if (!cfg.password.empty()) flags |= kCodeFlagPasswordRequired;
    std::string internet, lan;
    if (!source.empty()) internet = make_party_code(pub, port, cfg.secret, flags);
    if (lb) {
        lan = make_party_code({127, 0, 0, 1}, port, cfg.secret, static_cast<std::uint8_t>(flags | kCodeFlagLan));
    } else if (!make_lan_code(port, cfg.secret, flags, &lan)) {
        lan.clear();
    }
    const std::string pub_text = source.empty() ? std::string() : format_ipv4(pub) + ":" + std::to_string(pub_port);
    {
        std::lock_guard<std::mutex> lk(r.mu);
        r.st.internet_code = internet;
        r.st.lan_code = lan;
        r.st.public_address = source.empty() ? "unknown" : pub_text + " (" + source + ")";
        r.st.upnp = upnp_msg;
        r.st.local_slot = 0;
    }
    plog("hosting on port %u as %s; code (Internet): %s; code (LAN): %s", port, name.c_str(),
         internet.empty() ? "(no public address: set BB_PARTY_PUBLIC_ADDR)" : internet.c_str(),
         lan.empty() ? "(no LAN address)" : lan.c_str());
    if (!source.empty()) plog("public address %s (%s)", pub_text.c_str(), source.c_str());

    // The host's own endpoint for guests' records and our MappedAddr.
    const std::string ep_addr = source.empty() ? bbnet::party::ip_text(bbnet::local_ipv4()) : format_ipv4(pub);
    if (!ep_addr.empty()) {
        svc.set_host_endpoint(name, ep_addr, pub_port);
        bbnet::session::set_mapped_override(ep_addr, pub_port);
    }

    std::string text = "internet=" + internet + "\nlan=" + lan + "\n";
    write_text_file(fs::path(user_dir()) / "party_code.txt", text);
    if (const char* cf = env("BB_PARTY_CODE_FILE")) write_text_file(cf, text);
}

// The code a guest uses: BB_PARTY_CODE, BB_PARTY=<code>, or BB_PARTY_CODE_FILE (waits <= 60 s).
bool guest_code(PartyCode* code, std::string* error) {
    std::string text;
    if (const char* c = env("BB_PARTY_CODE")) {
        text = c;
    } else if (const char* p = env("BB_PARTY"); p && lower(p) != "join" && lower(p) != "guest" && lower(p) != "1") {
        text = p;
    }
    if (!text.empty()) return decode_party_code(text, code, error);
    const char* cf = env("BB_PARTY_CODE_FILE");
    if (!cf) {
        *error = "no party code: set BB_PARTY_CODE (or BB_PARTY_CODE_FILE)";
        return false;
    }
    const bool prefer_lan = env_on("BB_PARTY_PREFER_LAN");
    plog("waiting for the party code in %s", cf);
    const auto deadline = Clock::now() + std::chrono::seconds(60);
    std::string last_err = "the file did not appear";
    while (Clock::now() < deadline && !stopping()) {
        std::string body;
        if (read_file(cf, &body)) {
            std::string internet, lan, bare;
            std::istringstream in(body);
            std::string line;
            while (std::getline(in, line)) {
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                if (line.rfind("internet=", 0) == 0) internet = line.substr(9);
                else if (line.rfind("lan=", 0) == 0) lan = line.substr(4);
                else if (!line.empty() && bare.empty()) bare = line;
            }
            for (const std::string& c : prefer_lan ? std::vector<std::string>{lan, internet, bare}
                                                   : std::vector<std::string>{internet, lan, bare}) {
                if (!c.empty() && decode_party_code(c, code, &last_err)) return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    *error = std::string("no usable party code in ") + cf + ": " + last_err;
    return false;
}

void start_guest(LinkConfig cfg) {
    Runtime& r = R();
    PartyCode code;
    std::string err;
    if (!guest_code(&code, &err)) {
        set_error(err);
        plog("%s", err.c_str());
        return;
    }
    std::array<std::uint8_t, 4> ip{};
    std::uint16_t port = code.port;
    if (!parse_ipv4(code.host, &ip) && !resolve_ipv4(code.host, code.port, &ip, &port)) {
        set_error("cannot resolve the host " + code.host);
        plog("cannot resolve the host %s", code.host.c_str());
        return;
    }
    cfg.secret = code.secret;
    if ((code.flags & kCodeFlagPasswordRequired) && cfg.password.empty())
        plog("warning: the host requires a password (BB_PARTY_PASSWORD)");
    std::uint32_t nbo = 0;
    std::memcpy(&nbo, ip.data(), 4);
    auto* link = new PartyLink(cfg, make_callbacks(false));
    {
        std::lock_guard<std::mutex> lk(r.mu);
        r.link = link;
        r.host_ip_nbo = nbo;
        r.host_port = port;
        r.st.host_address = format_ipv4(ip) + ":" + std::to_string(port);
    }
    plog("joining %s as %s%s", r.st.host_address.c_str(), cfg.name.c_str(),
         (code.flags & kCodeFlagLan) ? " (LAN code)" : "");
    bbnet::party::remote_guest();  // the RemoteGuest transport exists before the game asks
    if (!link->start_guest(format_ipv4(ip), port, &err)) {
        set_error("cannot join: " + err);
        plog("cannot join: %s", err.c_str());
        return;
    }
    coop::PartyDirector::Get().SetLink(link);
}

void startup_main() {
    Runtime& r = R();
    const bbnet::Settings& s = bbnet::settings();
    const bool host = s.host;
    {
        std::lock_guard<std::mutex> lk(r.mu);
        r.st.role = host ? RuntimeRole::Host : RuntimeRole::Guest;
        r.st.name = s.online_id;
        r.st.port = s.party_port;
    }
    {
        const RestartInfo& ri = restart_info();  // before the marker below is rewritten
        {
            std::lock_guard<std::mutex> lk(r.mu);
            r.st.restarted = ri.restarted;
        }
        if (ri.restarted) plog("restarted after an unclean exit (%s): rejoining", ri.why.c_str());
        const fs::path m = marker_path();
        char body[256];
        std::snprintf(body, sizeof body, "\"pid\":%lu,\"role\":\"%s\",\"name\":\"%s\",\"started\":%lld",
                      static_cast<unsigned long>(
#if defined(_WIN32)
                          GetCurrentProcessId()
#else
                          0
#endif
                              ),
                      host ? "host" : "guest", s.online_id.c_str(),
                      static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(
                                                 std::chrono::system_clock::now().time_since_epoch())
                                                 .count()));
        {
            std::lock_guard<std::mutex> lk(r.mu);
            r.marker = m.string();
            r.marker_head = body;
        }
        // restart_info() above kept the old marker's text (the host's previous secret and
        // members) before this first write.
        write_marker();
    }
    if (loopback_mode()) {
#if defined(_WIN32)
        if (!env("BB_PARTY_LOCAL_IP")) _putenv_s("BB_PARTY_LOCAL_IP", "127.0.0.1");
#endif
    }
    LinkConfig cfg;
    apply_link_env(cfg);
    cfg.port = s.party_port;  // the network library's (validated) value
    cfg.name = s.online_id;
    if (!valid_member_name(cfg.name)) {
        set_error("invalid party name " + cfg.name);
        plog("invalid party name '%s'", cfg.name.c_str());
        return;
    }
    cfg.eboot_sha256 = eboot_hash();
    std::string what;
    patch_identity(cfg, &what);
    mod_identity(cfg, &what);
    // Max players and the 4-player rule set (party_fourp.h): the same for every player.
    cfg.rules = "max " + std::to_string(cfg.max_players) + " players, " + coop::fourp::FourpRulesTag();
    plog("%s %s, port %u, up to %d players; version check: eboot %s..., patches %s..., mods %s... (%s)%s",
         host ? "host" : "guest", cfg.name.c_str(), cfg.port, cfg.max_players, hex_prefix(cfg.eboot_sha256).c_str(),
         hex_prefix(cfg.patches_hash).c_str(), hex_prefix(cfg.mods_hash).c_str(), what.c_str(),
         loopback_mode() ? "; loopback test" : "");
    plog("gameplay patches: %s", joined(cfg.patch_names).c_str());
    plog("gameplay mods: %s", joined(cfg.mod_names).c_str());
    plog("party rules: %s", cfg.rules.c_str());
    {
        std::lock_guard<std::mutex> lk(r.mu);
        if (r.stopping) return;
    }
    if (host) start_host(std::move(cfg));
    else start_guest(std::move(cfg));
    bool have;
    MemberState ms;
    std::uint32_t map;
    PartyLink* link;
    {
        std::lock_guard<std::mutex> lk(r.mu);
        have = r.have_local_state;
        ms = r.local_state;
        map = r.local_map;
        link = r.link;
    }
    if (have && link) link->set_local_state(ms, map);
}

void do_shutdown() {
    Runtime& r = R();
    PartyLink* link;
    std::unique_ptr<UpnpMapper> upnp;
    std::vector<std::shared_ptr<Pump>> pumps;
    {
        std::lock_guard<std::mutex> lk(r.mu);
        r.stopping = true;
        link = r.link;
        upnp = std::move(r.upnp);
        for (auto& [k, p] : r.pumps) pumps.push_back(p);
        r.pumps.clear();
    }
    for (auto& p : pumps) p->stop = true;
    if (!pumps.empty()) bbnet::party::PartyHostService::instance().wake_all();
    if (link) {
        coop::PartyDirector::Get().SetLink(nullptr);
        link->stop(true);  // BYE; the object stays (never deleted)
    }
    if (upnp) upnp->stop(2000);
}

// Registered with the network library (bbnet_internal.h) at static initialization.
struct Registrar {
    Registrar() {
        bbnet::RuntimeHooks h;
        h.start = &runtime_start;
        h.shutdown = &runtime_shutdown;
        h.status = &party_status_line;
        bbnet::set_runtime_hooks(h);
    }
} g_registrar;

}  // namespace

const char* runtime_role_name(RuntimeRole r) {
    switch (r) {
    case RuntimeRole::Host: return "host";
    case RuntimeRole::Guest: return "guest";
    default: return "none";
    }
}

void runtime_start() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (!bbnet::settings().party) return;
        std::thread([] {
            startup_main();
            Runtime& r = R();
            std::lock_guard<std::mutex> lk(r.mu);
            r.st.started = true;
            r.cv.notify_all();
        }).detach();
    });
}

void runtime_shutdown() {
    Runtime& r = R();
    {
        std::lock_guard<std::mutex> lk(r.mu);
        if (r.shut) return;
        r.shut = true;
        if (!r.marker.empty()) {  // a clean exit: the next start is not a crash recovery
            std::error_code ec;
            fs::remove(r.marker, ec);
        }
        if (!r.link && !r.upnp) {
            r.stopping = true;
            return;
        }
    }
    plog("shutting down (BYE%s)", r.upnp ? ", UPnP unmap" : "");
    // Bounded: the exit path must not hang on a stuck router or socket.
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::thread([done] {
        do_shutdown();
        done->store(true);
    }).detach();
    const auto deadline = Clock::now() + std::chrono::milliseconds(2500);
    while (!done->load() && Clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    plog("shutdown %s", done->load() ? "done" : "timed out");
}

RuntimeStatus runtime_status() {
    Runtime& r = R();
    PartyLink* link;
    RuntimeStatus st;
    {
        std::lock_guard<std::mutex> lk(r.mu);
        st = r.st;
        link = r.link;
    }
    if (link) {
        st.state = link->state();
        st.local_slot = link->local_slot();
        st.roster = link->roster();
    }
    return st;
}

std::string party_status_line() {
    const RuntimeStatus st = runtime_status();
    if (st.role == RuntimeRole::None) return "party off";
    std::string s = std::string("party ") + runtime_role_name(st.role) + " " + st.name + ": " + link_state_name(st.state);
    if (st.role == RuntimeRole::Host) {
        s += " :" + std::to_string(st.port);
    } else if (!st.host_address.empty()) {
        s += " " + st.host_address;
        if (st.local_slot > 0) s += " slot " + std::to_string(st.local_slot);
    }
    int connected = 0;
    std::string others;
    for (const RosterEntry& e : st.roster) {
        if (e.connected) ++connected;
        if (e.name == st.name) continue;
        if (!others.empty()) others += ", ";
        others += e.name + " " + member_state_name(e.state) + (e.connected ? "" : " lost") + " " +
                  std::to_string(e.ping_ms) + " ms";
    }
    if (!st.roster.empty()) s += ", " + std::to_string(connected) + " in party";
    if (!others.empty()) s += " (" + others + ")";
    if (!st.last_error.empty()) s += "; " + st.last_error;
    return s;
}

bool runtime_restarted() { return bbnet::settings().party && restart_info().restarted; }

PartyLink* RuntimeApi::link() const { return runtime_link(); }
RuntimeStatus RuntimeApi::status() const { return runtime_status(); }
std::string RuntimeApi::status_line() const { return party_status_line(); }
void RuntimeApi::set_local_state(MemberState state, std::uint32_t map_id) const { party::set_local_state(state, map_id); }

const RuntimeApi& runtime() {
    static const RuntimeApi api;
    return api;
}

PartyLink* runtime_link() {
    std::lock_guard<std::mutex> lk(R().mu);
    return R().link;
}

void set_local_state(MemberState state, std::uint32_t map_id) {
    Runtime& r = R();
    PartyLink* link;
    {
        std::lock_guard<std::mutex> lk(r.mu);
        r.have_local_state = true;
        r.local_state = state;
        r.local_map = map_id;
        link = r.link;
    }
    if (link) link->set_local_state(state, map_id);
}

}  // namespace party

extern "C" void bbnet_shutdown(void) { party::runtime_shutdown(); }

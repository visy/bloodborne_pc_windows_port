// SPDX-License-Identifier: GPL-3.0-or-later
// Guest services in the host's Hunter's Dream (see party_guestsvc.h; the RE is
// docs/party/guest_services.md).
//
// Offsets are ours (raw ELF VA of eboot.elf 1.09), each read from the disassembly:
//   0x203AD60 talk environment test-function switch (rdi result {value, type}, rsi = r15 talk
//     environment, rdx = r14 call {vfunc +0x10 id, +0x18 argument count});
//   0x203BA66 case 19 IsClientPlayer: argument count 1, then
//   0x203BA78 41 0f b6 47 25   movzx eax, byte [r15+0x25]
//   0x203BA7D e9 4d 34 00 00   jmp 0x203EECF   ([r12+8] = 2 int, [r12] = eax)
#include "party_guestsvc.h"

#include "net/json.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace coop {

bool IsDreamMap(std::uint32_t map_id) {
    return map_id != 0xffffffffu && (map_id >> 24) == 21;
}

bool GuestServicesGate(const GuestServicesInput& in) {
    return in.enabled && in.world_up && !in.loading && in.session_role == kSessionRoleClient &&
           IsDreamMap(in.map_id);
}

std::string DescribeRecordChange(const PlayerRecord& a, const PlayerRecord& b) {
    std::string out;
    auto field = [&](const char* name, int x, int y) {
        if (x < 0 || y < 0 || x == y) {
            return;
        }
        char buf[96];
        std::snprintf(buf, sizeof buf, "%s%s %d -> %d", out.empty() ? "" : ", ", name, x, y);
        out += buf;
    };
    field("level", a.level, b.level);
    field("echoes", a.echoes, b.echoes);
    field("Insight", a.insight, b.insight);
    return out;
}

bool GuestServiceTalk(std::uint32_t talk) {
    switch (talk) {
    case 210304: // Doll: level up (MSB entity 2100700)
    case 210691: // Bath Messenger: shop, sell (2100211)
    case 210692: // workbench: upgrade, Blood Gems, repair, crush (2100201)
    case 210693: // storage (2100200)
    case 210694: // Bath Messenger shop (2100215)
    case 210695: // Bath Messenger shop (2100216)
    case 210696: // mirror: appearance (2100202)
    case 210697: // dress-up messenger (2100218)
    case 210698: // Insight messenger: Insight shop (2100219)
    case 210699: // Bath Messenger shop (2100203)
        return true;
    default:
        return false;
    }
}

bool GuestServiceTalkAllowed(std::uint32_t talk, const std::string& extra) {
    if (GuestServiceTalk(talk) || extra == "all") {
        return true;
    }
    std::size_t at = 0;
    while (at < extra.size()) {
        std::size_t end = extra.find(',', at);
        if (end == std::string::npos) {
            end = extra.size();
        }
        const std::string item = extra.substr(at, end - at);
        char* stop = nullptr;
        const unsigned long v = std::strtoul(item.c_str(), &stop, 10);
        if (!item.empty() && stop && !*stop && v == talk) {
            return true;
        }
        at = end + 1;
    }
    return false;
}

bool HeadstoneTalk(std::uint32_t talk) {
    return (talk >= 210000 && talk <= 210003) || talk == 210721;
}

std::string LampRequestToJsonText(const LampRequest& r) {
    json::Value v = json::Value::make_object();
    v.set("id", r.id);
    v.set("seq", json::hex(r.seq));
    return json::dump(v, 0);
}

bool LampRequestFromJsonText(const std::string& text, LampRequest* out, std::string* error) {
    json::Value v;
    std::string err;
    if (!json::parse(text, v, err)) {
        if (error) {
            *error = err;
        }
        return false;
    }
    try {
        if (v.type != json::Value::Type::Object) {
            throw std::runtime_error("not an object");
        }
        LampRequest r;
        r.id = json::u32(v, "id");
        r.seq = json::u64(v, "seq");
        *out = r;
        return true;
    } catch (const std::exception& e) {
        if (error) {
            *error = e.what();
        }
        return false;
    }
}

bool SanitizePeerLampRequest(const LampRequest& r, std::string* why) {
    if (!TravelLampKnown(r.id)) {
        if (why) {
            *why = "id " + std::to_string(r.id) + " is no ReturnPointParam row";
        }
        return false;
    }
    return true;
}

bool LampRequestGate::Accept(bool host_ready, double now, std::string* why) {
    if (!host_ready) {
        if (why) {
            *why = "the host is not in the world";
        }
        return false;
    }
    if (now - last_ < kMinInterval) {
        if (why) {
            *why = "another request was accepted less than 10 s ago";
        }
        return false;
    }
    last_ = now;
    return true;
}

std::vector<EmevdSkipRule> LampSkipRules() {
    std::vector<EmevdSkipRule> out;
    ParseEmevdSkipRules("7200:1003:6@0", &out);
    return out;
}

std::vector<EmevdSkipRule> DollPlacementRules() {
    std::vector<EmevdSkipRule> out;
    ParseEmevdSkipRules("12100110:2003:22@25,12100110:2003:17@26", &out);
    for (EmevdSkipRule& r : out) {
        r.client_only = true;
    }
    return out;
}

bool DollLevelUpCharTypeTest(int event_id, int bank, int id, int entity, int type) {
    return event_id >= 12100112 && event_id <= 12100114 && bank == 4 && id == 3 && entity == 10000 &&
           (type == 0 || type == 1);
}

bool GuestLampsEnabledFromEnv(const char* v) {
    return v && v[0] == '1' && !v[1];
}

bool GuestServicesEnabledFromEnv(const char* v) {
    return !(v && v[0] == '0' && !v[1]);
}

} // namespace coop

#ifndef BB_PARTY_GUESTSVC_NO_GAME
#include "coop_hooks.h"
#include "game_state.h"
#include "seamless_rules.h"
#include "party_director.h"
#include "party_link.h"
#include "party_progress.h"
#include "bbport_game_menu.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <deque>
#include <mutex>

extern "C" void runtime_pad_director_press(std::uint32_t buttons); // src/runtime_pad.c

namespace coop {
namespace {

constexpr u64 kIsClientLoad = 0x203BA78;   // movzx eax, byte [r15+0x25]
constexpr u64 kIsClientJmp = 0x203BA7D;    // jmp 0x203EECF
constexpr u64 kTestResultInt = 0x203EECF;  // [r12+8] = 2; [r12] = eax
// Case 52 IsMultiplayerInProgress: ... setg cl; or cl, al; movzx eax, cl; jmp 0x203EECF.
constexpr u64 kMultiOr = 0x203C564;        // 08 c1 0f b6 c1
constexpr u64 kMultiJmp = 0x203C569;       // e9 61 29 00 00 -> 0x203EECF
// Lamp object update 0x12F5C40: +0x49 = peer count > 0 || presentation count > 1.
constexpr u64 kLampMultiGate = 0x12F831C;  // test ecx,ecx; setg cl; or cl,al (then mov [r13+0x49], cl)

std::atomic<bool> g_lamps{false};
// The host's headstones in a party (default on; BB_PARTY_HEADSTONES=0 off): IsMultiplayerInProgress
// answers 0 for the headstone scripts, so the host can travel from the Dream with guests present
// (the party follows by B2 / B1 travel). A guest's headstones stay behind IsClientPlayer unless
// BB_PARTY_GUEST_LAMPS=1.
std::atomic<bool> g_headstones{false};
std::atomic<int> g_session_role{-1};       // the tick's last read (the interceptor's thread)
std::atomic<std::uint32_t> g_multi_overrides{0};
std::mutex g_lamp_mu;
std::deque<LampRequest> g_lamp_out;        // guest: to send
std::deque<std::pair<int, LampRequest>> g_lamp_in; // host: from guests (slot, request)
std::uint64_t g_lamp_seq = 0;
LampRequestGate g_lamp_gate;
std::atomic<int> g_lamp_requests_made{0}; // guest: lamp warps turned into requests (tests)

std::atomic<bool> g_gate{false};
std::atomic<std::uint32_t> g_overrides{0};
// Talk environments seen asking while the gate was open (discovery log, tick side).
constexpr int kEnvSlots = 32;
std::atomic<std::uint64_t> g_envs[kEnvSlots];

struct State {
    bool init = false, enabled = false, installed = false;
    std::string extra; // BB_PARTY_GUEST_SERVICES_TALK
    bool open = false;
    PlayerRecord rec;
    std::uint32_t overrides_logged = 0;
    double last_check = 0;
    std::uint64_t logged_envs[kEnvSlots] = {};
    // BB_PARTY_GUESTSVC_TEST (live tests only): "echoes=N;warp=x,y,z,yaw[/x,y,z,yaw...]": the echoes
    // and the first warp 15 s after the gate opened (the summoned reload is over by then), each
    // further warp 8 s later.
    std::string test;
    bool test_done = false;
    int test_step = 0;
    double test_next = 0;
    double open_at = 0;
    double changed_at = 0; // the last record change (0: none pending)
} g;

void NoteEnv(std::uint64_t env) {
    for (auto& slot : g_envs) {
        std::uint64_t cur = slot.load(std::memory_order_relaxed);
        if (cur == env) {
            return;
        }
        if (cur == 0 && slot.compare_exchange_strong(cur, env)) {
            return;
        }
    }
}

// Main thread (talk scripts): the game's answer `value` of IsClientPlayer for talk environment `env`.
std::uint64_t BB_COOP_SYSV IsClientFilter(std::uint64_t env, std::uint64_t value) {
    if (!g_gate.load(std::memory_order_relaxed) || (value & 0xff) == 0) {
        return value;
    }
    NoteEnv(env);
    // The talk script id (t210304 -> 210304; the case-19 environment is valid here: the game
    // just read +0x25).
    const std::uint32_t talk = *reinterpret_cast<const std::uint32_t*>(env + 0x10);
    // Headstones stay in their client wait on a guest (live: choosing an area in the headstone menu
    // crashed the guest at once - the warp list is built from lamp flags the guest's overlay lacks);
    // the host's headstones work in a party (MultiFilter) and the party follows the host.
    if (!GuestServiceTalkAllowed(talk, g.extra)) {
        return value;
    }
    g_overrides.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

// EMEVD dispatch thread: the Doll's level-up kneel event 12100112 for a client (see the header).
std::atomic<std::uint32_t> g_doll_rewrites{0};
void DollRewriter(int event_id, int bank, int id, std::uint64_t event, std::uint64_t instr) {
    if (event_id < 12100112 || event_id > 12100114 || bank != 4 || id != 3) {
        return;
    }
    std::uint32_t size = 0;
    const std::uint64_t args = EmevdInstructionArgs(event, instr, &size);
    std::int32_t entity = 0;
    std::int8_t type = -1;
    if (!args || size < 9 || !SafeGet(args + 4, &entity) || !SafeGet(args + 8, &type) ||
        !DollLevelUpCharTypeTest(event_id, bank, id, entity, type)) {
        return;
    }
    const std::int8_t want = LocalSessionRole() == kSessionRoleClient ? 1 : 0;
    if (type != want && SafeWrite(args + 8, &want, 1)) {
        g_doll_rewrites.fetch_add(1, std::memory_order_relaxed);
    }
}

// Main thread (talk scripts): IsMultiplayerInProgress for talk environment `env` (both roles):
// the headstones work in a party.
std::uint64_t BB_COOP_SYSV MultiFilter(std::uint64_t env, std::uint64_t value) {
    if (!(g_lamps.load(std::memory_order_relaxed) || g_headstones.load(std::memory_order_relaxed)) ||
        (value & 0xff) == 0) {
        return value;
    }
    const std::uint32_t talk = *reinterpret_cast<const std::uint32_t*>(env + 0x10);
    if (!HeadstoneTalk(talk)) {
        return value;
    }
    g_multi_overrides.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    std::printf("Party guest services: %s\n", line);
    std::fflush(stdout);
}

double Now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

PlayerRecord RecordOf(const GameSnapshot& s) {
    PlayerRecord r;
    r.level = s.level;
    r.echoes = s.echoes;
    r.insight = s.insight;
    return r;
}

// Discovery: which fields of a talk environment name its script (t210140 = 210140 ...).
void LogEnv(std::uint64_t env) {
    std::string found;
    auto scan = [&](std::uint64_t base, const char* path, std::size_t n) {
        for (std::size_t off = 0; off + 4 <= n; off += 4) {
            std::int32_t v = 0;
            if (!SafeGet(base + off, &v)) {
                return;
            }
            if (v >= 100000 && v <= 999999) {
                char buf[96];
                std::snprintf(buf, sizeof buf, " %s+0x%zx=%d", path, off, v);
                found += buf;
            }
        }
    };
    scan(env, "", 0x200);
    for (std::size_t off = 0; off < 0x100; off += 8) {
        std::uint64_t p = 0;
        if (!SafeGet(env + off, &p) || p < 0x10000 || (p & 7)) {
            continue;
        }
        char path[32];
        std::snprintf(path, sizeof path, "[+0x%zx]", off);
        scan(p, path, 0x100);
    }
    std::string hex;
    for (std::size_t off = 0; off < 0x80; off += 8) {
        std::uint64_t q = 0;
        SafeGet(env + off, &q);
        char buf[24];
        std::snprintf(buf, sizeof buf, " %016llx", static_cast<unsigned long long>(q));
        hex += buf;
    }
    std::uint32_t talk = 0;
    SafeGet(env + 0x10, &talk);
    Log("talk script t%u (environment %p) asks IsClientPlayer: %s; ids:%s; head:%s", talk, reinterpret_cast<void*>(env),
        GuestServiceTalkAllowed(talk, g.extra) ? "allowed" : "kept vanilla", found.empty() ? " none" : found.c_str(),
        hex.c_str());
}

// Game thread (0x13CDF30, not our replays): a guest in the host's world used a lamp / headstone.
bool GuestLampIntercept(std::uint32_t id) {
    if (!g_lamps.load() || PartyDirector::Get().Role() != PartyRole::Guest ||
        g_session_role.load() != kSessionRoleClient) {
        return false;
    }
    LampRequest r;
    {
        std::lock_guard<std::mutex> lk(g_lamp_mu);
        r.id = id;
        r.seq = ++g_lamp_seq;
        if (g_lamp_out.size() < 8) {
            g_lamp_out.push_back(r);
        }
    }
    Log("lamp: the guest's warp to %u becomes a request to the host (the party travels together)", id);
    g_lamp_requests_made.fetch_add(1);
    return true;
}

// Diagnostics (BB_PARTY_GUESTSVC_TEST "probe"): the character with `entity` the way the talk
// environment finds it (0x203AC3D..0x203AD09): WorldChrMan +0x90 count of chr sets, +0x98[i] a set
// with +0xF0 count and +0xF8 sorted {entity, handle} pairs; handle bits 14..19 = slot block,
// bits 0..13 = index; block < WorldChrMan+0x20: entry = [WorldChrMan+0x28] + block * 0x160 ->
// +0xC0 array (stride 0x38), else (block == [[WorldChrMan+8]+0x18]) WorldChrMan+0x38 array;
// [entry] = ChrIns*.
std::uint64_t FindChr(std::uint32_t entity, std::string* diag = nullptr) {
    const std::uint64_t wcm = ReadGameState(false).world_chr_man;
    std::int32_t sets = 0;
    const bool got = wcm && SafeGet(wcm + 0x90, &sets);
    if (diag) {
        char b[96];
        std::snprintf(b, sizeof b, "wcm %p sets %d", reinterpret_cast<void*>(wcm), got ? sets : -1);
        *diag = b;
    }
    if (!got || sets <= 0 || sets > 4096) {
        return 0;
    }
    for (std::int32_t i = 0; i < sets; ++i) {
        std::uint64_t set = 0, arr = 0;
        std::int32_t n = 0;
        if (!SafeGet(wcm + 0x98 + 8 * std::uint64_t(i), &set) || !set || !SafeGet(set + 0xf0, &n) ||
            !SafeGet(set + 0xf8, &arr) || n <= 0 || n > 100000) {
            continue;
        }
        if (diag && i < 8) {
            std::int32_t first = 0, last = 0;
            SafeGet(arr, &first);
            SafeGet(arr + 8 * std::uint64_t(n - 1), &last);
            char b[96];
            std::snprintf(b, sizeof b, "; set %d: %d ids %d..%d", i, n, first, last);
            *diag += b;
        }
        for (std::int32_t k = 0; k < n; ++k) {
            std::int32_t id = 0, handle = 0;
            if (!SafeGet(arr + 8 * std::uint64_t(k), &id) || id != std::int32_t(entity)) {
                continue;
            }
            SafeGet(arr + 8 * std::uint64_t(k) + 4, &handle);
            if (handle < 0) {
                return 0;
            }
            const std::uint32_t block = (std::uint32_t(handle) >> 14) & 0x3f, index = std::uint32_t(handle) & 0x3fff;
            std::int32_t blocks = 0;
            std::uint64_t entry = 0;
            SafeGet(wcm + 0x20, &blocks);
            if (std::int32_t(block) < blocks) {
                std::uint64_t base = 0, list = 0;
                SafeGet(wcm + 0x28, &base);
                SafeGet(base + block * 0x160 + 0xc0, &list);
                entry = list ? list + index * 0x38 : 0;
            } else {
                std::uint64_t list = 0;
                SafeGet(wcm + 0x38, &list);
                entry = list ? list + index * 0x38 : 0;
            }
            std::uint64_t chr = 0;
            if (entry) {
                SafeGet(entry, &chr);
            }
            return chr;
        }
    }
    return 0;
}

// Live tests: the save's Doll progress (campaign_start.md 1.3): 12100105 awake, 9401 first Dream,
// Insight >= 1, 12101024 / 12101025 (Beckoning Bell messenger). Logged once per world-up.
bool CheckDollProgress(const GameSnapshot& s, const char* who) {
    bool awake = false, first = false, m24 = false, m25 = false;
    progress::ReadFlag(12100105, &awake);
    progress::ReadFlag(9401, &first);
    progress::ReadFlag(12101024, &m24);
    progress::ReadFlag(12101025, &m25);
    const bool ok = awake && first && s.insight >= 1;
    Log("test: %s progress: 12100105=%d 9401=%d Insight=%d 12101024=%d 12101025=%d (%s, map %s)%s", who, awake, first,
        s.insight, m24, m25, SessionRoleName(s.session_role), MapName(s.map_id).c_str(),
        ok ? "" : " -- PROGRESS MISSING: the Doll cannot be awake on this save");
    return ok;
}

void ProbeDoll() {
    std::string diag;
    const std::uint64_t chr = FindChr(210304, &diag); // the table is keyed by talk script id
    {
        const GameSnapshot me = ReadGameState(false);
        std::uint64_t phys = 0;
        float p[4] = {};
        if (me.player) {
            SafeRead(me.player + 0xc0, p, sizeof p);
        }
        (void)phys;
        Log("probe: lookup %s; local player %p at +0xc0 (%.2f, %.2f, %.2f)", diag.c_str(),
            reinterpret_cast<void*>(me.player), p[0], p[1], p[2]);
    }
    std::int32_t type = -1, st10 = -1;
    std::uint8_t b20 = 0, draw[8] = {};
    std::uint64_t sub = 0, phys = 0;
    float pos[4] = {};
    if (chr) {
        SafeGet(chr + 0x78, &type);
        SafeGet(chr + 0x18, &sub);
        if (sub) {
            SafeGet(sub + 0x10, &st10);
            SafeGet(sub + 0x20, &b20);
        }
        SafeRead(chr + 0x1e0, draw, sizeof draw);
        SafeRead(chr + 0xc0, pos, sizeof pos);
    }
    // Discovery: where does the Doll keep her position? Float triples near one of her MSB spots
    // (2102301 / 2102302 / 2102303 / 2102304), in the chr and one pointer deep.
    if (chr) {
        static const float spots[4][3] = {{-5.8f, -5.62f, -13.6f}, {-13.27f, -1.05f, 1.73f}, {-7.4f, -6.25f, -25.09f},
                                          {-5.8f, -4.83f, -11.6f}};
        std::string hits;
        auto scan = [&](std::uint64_t base, const char* path, std::size_t n) {
            for (std::size_t off = 0; off + 12 <= n && hits.size() < 600; off += 4) {
                float v[3];
                if (!SafeRead(base + off, v, sizeof v)) {
                    return;
                }
                for (const auto& sp : spots) {
                    if (std::fabs(v[0] - sp[0]) < 1.5f && std::fabs(v[1] - sp[1]) < 1.5f && std::fabs(v[2] - sp[2]) < 1.5f) {
                        char b[96];
                        std::snprintf(b, sizeof b, " %s+0x%zx=(%.2f,%.2f,%.2f)", path, off, v[0], v[1], v[2]);
                        hits += b;
                        break;
                    }
                }
            }
        };
        scan(chr, "", 0x800);
        for (std::size_t off = 0; off < 0x200; off += 8) {
            std::uint64_t q = 0;
            if (!SafeGet(chr + off, &q) || q < 0x10000 || (q & 7)) {
                continue;
            }
            char path[24];
            std::snprintf(path, sizeof path, "[+0x%zx]", off);
            scan(q, path, 0x400);
        }
        Log("probe: Doll position candidates:%s", hits.empty() ? " none" : hits.c_str());
        float f[32] = {};
        SafeRead(chr + 0xa0, f, sizeof f);
        std::string fl;
        for (int k = 0; k < 32; ++k) {
            char b[24];
            std::snprintf(b, sizeof b, " %.3f", f[k]);
            fl += b;
        }
        Log("probe: Doll chr +0xa0..+0x120 floats:%s", fl.c_str());
    }
    std::string flags;
    for (const std::uint32_t f : {9401u, 100u, 1000u, 1001u, 1002u, 1003u, 12100105u, 12100500u, 12100501u, 12100502u,
                                  12100510u, 12105100u}) {
        bool on = false;
        const bool ok = progress::ReadFlag(f, &on);
        char b[32];
        std::snprintf(b, sizeof b, " %u=%s", f, ok ? (on ? "1" : "0") : "?");
        flags += b;
    }
    {
        const GameSnapshot me = ReadGameState(false);
        std::uint8_t own = 0xff;
        std::uint64_t wts = 0;
        if (SafeGet(Guest(0x5556678), &wts) && wts) {
            SafeGet(wts + 0x1592, &own);
        }
        float p[4] = {}, pr[4] = {}, dr[4] = {};
        if (me.player) {
            SafeRead(me.player + 0xc0, p, sizeof p);
            SafeRead(me.player + 0xd0, pr, sizeof pr);
        }
        if (chr) {
            SafeRead(chr + 0xd0, dr, sizeof dr);
        }
        const float dx = pos[0] - p[0], dz = pos[2] - p[2];
        const float dist = std::sqrt(dx * dx + dz * dz);
        // Angle between the player's facing (-sin, -cos) and the direction to the Doll.
        const float fx = -std::sin(pr[1]), fz = -std::cos(pr[1]);
        const float cosang = dist > 1e-3f ? (fx * dx + fz * dz) / dist : 1.0f;
        Log("probe: role %s, own world byte %u, Insight %d; player (%.2f, %.2f, %.2f) yaw %.3f; Doll yaw %.3f; "
            "distance %.2f m, facing her within %.0f degrees",
            SessionRoleName(me.session_role), own, me.insight, p[0], p[1], p[2], pr[1], dr[1], dist,
            std::acos(std::fmax(-1.0f, std::fmin(1.0f, cosang))) * 57.2958f);
    }
    Log("probe: Doll chr %p type %d, +0x18->+0x10 %d +0x20 0x%02x, +0x1e0 %02x %02x %02x %02x %02x %02x %02x %02x, "
        "pos (%.2f, %.2f, %.2f); flags%s",
        reinterpret_cast<void*>(chr), type, st10, b20, draw[0], draw[1], draw[2], draw[3], draw[4], draw[5], draw[6],
        draw[7], pos[0], pos[1], pos[2], flags.c_str());
}

// Live test (BB_PARTY_GUESTSVC_TEST "levelup"): the Doll's level up on the guest, event-driven
// through the game's own pad path (runtime_pad_director_press, no OS input). Each stage waits for
// what the game shows: message lookups (BbGameMenu::LastLookupMs / TextActivity: a text on screen
// is looked up every frame) and the character record. Talk menu texts (talk ESD t210304):
// 15000100 "Channel the Blood Echoes", 15000007 "Talk", 15000005 "Leave".
// The level-up menu: talk command OpenSoul (command switch 0x203FFD0 case 31 at 0x2040B4E) queues a
// menu request (0x1C4A520) with the factory 0x1BB7F60, which builds the menu named "LevelUp"
// (0x49B46F6, through 0x1C1CDF0). A pre-hook on the factory tells the test (and the log) that the
// game really built the level-up menu on this game.
std::atomic<std::uint32_t> g_levelup_menus{0};
BB_COOP_SYSV void LevelUpMenuFactoryEntry(u64, u64, u64, u64, u64, u64) {
    g_levelup_menus.fetch_add(1, std::memory_order_relaxed);
}

// Menu request 0x1C4A520(out, factory std::function*, ...): the OpenSoul command (0x2040BB2) passes
// a functor whose call pointer ([[rsi+0x20]+8]) is the LevelUp factory 0x1BB7F60. Its first test is
// 0x176BFE0 (called at 0x1C4A549; false = no menu). Logged for the live tests.
std::atomic<std::uint32_t> g_menu_requests{0}, g_levelup_requests{0};
std::atomic<bool> g_menu_req_active{false};
BB_COOP_SYSV void MenuRequestEntry(u64, u64 functor, u64, u64, u64, u64) {
    g_menu_requests.fetch_add(1, std::memory_order_relaxed);
    std::uint64_t impl = 0, fn = 0;
    if (functor && SafeGet(functor + 0x20, &impl) && impl && SafeGet(impl + 8, &fn) && fn == Guest(0x1BB7F60)) {
        g_levelup_requests.fetch_add(1, std::memory_order_relaxed);
    }
}

// Talk commands (switch 0x203FFD0: rsi = the call, vfunc +0x18 = command id 0..87): a count per id
// while the gate is open (the live tests: did OpenSoul, id 31, run on the guest?).
std::atomic<std::uint32_t> g_talk_cmds[96];
BB_COOP_SYSV void TalkCommandEntry(u64, u64 call, u64, u64, u64, u64) {
    if (!g_gate.load(std::memory_order_relaxed) || !call) {
        return;
    }
    std::uint64_t vtbl = 0, fn = 0;
    if (!SafeGet(call, &vtbl) || !SafeGet(vtbl + 0x18, &fn) || !fn) {
        return;
    }
    const std::uint32_t id = reinterpret_cast<std::uint32_t(BB_COOP_SYSV*)(u64)>(fn)(call);
    if (id < 96) {
        g_talk_cmds[id].fetch_add(1, std::memory_order_relaxed);
    }
}

struct LevelUpScript {
    enum Stage { Off, WaitPlaced, Greeting, MenuUp, WaitScreen, Raise, Confirm, Accept, WaitLevel, BackOut, Done, Keys };
    std::uint32_t menus0 = 0;
    bool shop = false;                 ///< "buy": a messenger's Purchase instead of the Doll's level up
    bool open = false;                 ///< "open[=TEXT]": open a menu, then press "keys=" one by one
    std::string keys;                  ///< "cross,left,..." pressed 1.5 s apart once the menu is open
    std::size_t key_at = 0;
    std::uint32_t target = 0;          ///< talk id of the character / object to use
    int approach = 0;                  ///< WaitPlaced: placement attempts (distance x side)
    std::uint32_t menu_text = 15000100; ///< the talk menu row to choose
    std::uint32_t requests0 = 0;
    int insight0 = -1;
    Stage stage = Off;
    double at = 0, press_until = 0;
    int tries = 0, level0 = -1, echoes0 = -1;
    std::uint64_t seq0 = 0;
} g_lvl;

constexpr std::uint32_t kPadCross = 0x4000, kPadCircle = 0x2000, kPadRight = 0x20, kPadLeft = 0x80, kPadUp = 0x10,
                        kPadDown = 0x40;
constexpr std::uint32_t kTextChannel = 15000100, kTextLeave = 15000005, kTextPurchase = 15000140;

bool OnScreen(std::uint32_t id, std::int64_t within_ms = 400) {
    const std::int64_t ms = BbGameMenu::LastLookupMs(id);
    const std::int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count();
    return ms && now - ms < within_ms;
}

// The action prompt: ActionButtonParam textId of the Dream's talk / examine regions (6003 / 6004 /
// 6019-6026 / 6030: 10010100, 10010200, 10010220, 70000009) - looked up every frame while shown.
// Any action-button text on screen: ActionButtonParam text ids are 1001xxxx (talk / examine /
// "Workshop"-style object prompts) or 7000xxxx; *id receives the first one seen.
bool PromptUp(std::uint32_t* id = nullptr) {
    std::uint32_t ids[256];
    const int n = BbGameMenu::RecentLookups(400, ids, 256);
    for (int i = 0; i < n; ++i) {
        if ((ids[i] >= 10010000 && ids[i] < 10020000) || (ids[i] >= 70000000 && ids[i] < 70010000)) {
            if (id) {
                *id = ids[i];
            }
            return true;
        }
    }
    return false;
}

// Stand the local player at `dist` m from `chr` on the side `side` (0 its front, 1 back, 2 / 3
// its sides; forward(yaw) = (-sin, -cos)), facing it (PlayerIns vfunc +0x5F8).
bool StandAt(const GameSnapshot& s, std::uint64_t chr, float dist, int side) {
    float d[4] = {}, r[4] = {};
    if (!chr || !s.player || !SafeRead(chr + 0xc0, d, sizeof d) || !SafeRead(chr + 0xd0, r, sizeof r)) {
        return false;
    }
    const float a = r[1] + (side == 1 ? 3.14159265f : side == 2 ? 1.5707963f : side == 3 ? -1.5707963f : 0.0f);
    alignas(16) float pos[4] = {d[0] - dist * std::sin(a), d[1], d[2] - dist * std::cos(a), 1.0f};
    alignas(16) float rot[4] = {0, a + 3.14159265f, 0, 0};
    std::uint64_t vtbl = 0, fn = 0;
    if (!SafeGet(s.player, &vtbl) || !SafeGet(vtbl + 0x5f8, &fn) || !fn) {
        return false;
    }
    using WarpFn = void(BB_COOP_SYSV*)(std::uint64_t, const float*, const float*, std::uint64_t);
    reinterpret_cast<WarpFn>(fn)(s.player, pos, rot, 0);
    return true;
}

void Tap(std::uint32_t buttons, double now) {
    runtime_pad_director_press(buttons);
    g_lvl.press_until = now + 0.12;
}

void LevelUpTick(const GameSnapshot& s, double now) {
    LevelUpScript& L = g_lvl;
    if (L.stage == LevelUpScript::Off || L.stage == LevelUpScript::Done) {
        return;
    }
    if (L.press_until > 0) {
        if (now < L.press_until) {
            return;
        }
        runtime_pad_director_press(0);
        L.press_until = 0;
    }
    auto go = [&](LevelUpScript::Stage st, const char* what) {
        L.stage = st;
        L.at = now;
        L.tries = 0;
        Log("test: levelup: %s", what);
    };
    const double since = now - L.at;
    switch (L.stage) {
    case LevelUpScript::WaitPlaced: {
        // Step in along the target's front (1.2 m, then closer), 1 s at each spot: log the
        // action-button texts seen, press Cross, and let Greeting look for the talk menu.
        static const float dists[] = {1.2f, 1.0f, 0.85f, 0.7f, 0.55f, 0.4f};
        if (since < 1.0) {
            break;
        }
        if (L.approach > 0) {
            std::uint32_t prompt = 0;
            const bool up = PromptUp(&prompt);
            L.requests0 = g_menu_requests.load();
            Tap(kPadCross, now);
            Log("test: levelup: at %.2f m: action prompt %s%s; Cross", dists[L.approach - 1], up ? "text " : "not seen",
                up ? std::to_string(prompt).c_str() : "");
            go(LevelUpScript::Greeting, "advancing the lines until the menu is up");
            break;
        }
        const bool ok = StandAt(s, FindChr(L.target), dists[0], 0);
        Log("test: levelup: standing %.2f m in front of t%u%s", dists[0], L.target, ok ? "" : " - warp failed");
        L.approach = 1;
        L.at = now;
        break;
    }
    case LevelUpScript::Greeting:
        if (L.open && L.menu_text == 0 && g_menu_requests.load() != L.requests0) {
            L.level0 = s.level;
            L.echoes0 = s.echoes;
            L.insight0 = s.insight;
            L.key_at = 0;
            go(LevelUpScript::Keys, "no talk list: the menu was requested (0x1C4A520): menu open");
        } else if (L.menu_text != 0 && OnScreen(L.menu_text) && OnScreen(kTextLeave)) {
            go(LevelUpScript::MenuUp, L.open ? "talk menu up: the chosen row is on screen"
                                    : L.shop ? "talk menu up: 'Purchase' (15000140) is on screen"
                                             : "talk menu up: 'Channel the Blood Echoes' (15000100) is on screen");
        } else if (since >= 1.5 * (L.tries + 1)) {
            const int limit = L.target == 210304 ? 12 : 3;
            if (++L.tries > limit) {
                static const float dists[] = {1.2f, 1.0f, 0.85f, 0.7f, 0.55f, 0.4f};
                if (L.approach < 6) {
                    const float d = dists[L.approach++];
                    StandAt(s, FindChr(L.target), d, 0);
                    Log("test: levelup: no menu; one step closer: %.2f m", d);
                    L.stage = LevelUpScript::WaitPlaced;
                    L.at = now;
                    L.tries = 0;
                    break;
                }
                go(LevelUpScript::Done, "FAILED: no talk menu at any distance");
                break;
            }
            Tap(kPadCross, now);
            Log("test: levelup: line %d: Cross", L.tries);
        }
        break;
    case LevelUpScript::MenuUp:
        if (since >= 0.8) {
            // The menu's cursor starts on its first row, "Channel the Blood Echoes" (AddTalkListData
            // index 1). Make sure: Up wraps would move it, so press nothing but Cross.
            L.menus0 = g_levelup_menus.load();
            L.requests0 = g_menu_requests.load();
            Tap(kPadCross, now);
            go(LevelUpScript::WaitScreen, L.open ? "Cross on the chosen row"
                                          : L.shop ? "Cross on 'Purchase'" : "Cross on 'Channel the Blood Echoes'; the Doll's lines follow");
        }
        break;
    case LevelUpScript::WaitScreen:
        // The guest kneels (event 12100112), the Doll speaks; then OpenSoul builds the menu.
        if (L.open && g_menu_requests.load() != L.requests0) {
            L.level0 = s.level;
            L.echoes0 = s.echoes;
            L.insight0 = s.insight;
            L.key_at = 0;
            go(LevelUpScript::Keys, "the menu was requested (0x1C4A520): menu open");
        } else if (L.shop && g_menu_requests.load() != L.requests0) {
            go(LevelUpScript::Raise, "the shop menu was requested (OpenRegularShop -> 0x1C4A520): shop open");
        } else if (g_levelup_menus.load() != L.menus0) {
            go(LevelUpScript::Raise, "the game built the LevelUp menu (factory 0x1BB7F60): level-up screen open");
        } else if (since >= 2.0 * (L.tries + 1)) {
            if (++L.tries > 10) {
                go(LevelUpScript::Done, "FAILED: no LevelUp menu after the Doll's lines (10 presses)");
                break;
            }
            Tap(kPadCross, now);
            Log("test: levelup: Doll line %d: Cross", L.tries);
        }
        break;
    case LevelUpScript::Raise:
        if (since >= 1.5) {
            L.level0 = s.level;
            L.echoes0 = s.echoes;
            L.insight0 = s.insight;
            if (L.shop) {
                Tap(kPadCross, now);
                go(LevelUpScript::Confirm, "Cross on the shop's first item");
                break;
            }
            Tap(L.tries == 0 ? kPadDown : kPadRight, now);
            if (L.tries++ == 0) {
                Log("test: levelup: Down (first attribute)");
                L.at = now - 0.5;
                break;
            }
            go(LevelUpScript::Confirm, "Right: +1 on the selected attribute");
        }
        break;
    case LevelUpScript::Confirm:
        if (since >= 1.0) {
            Tap(kPadCross, now);
            go(LevelUpScript::Accept, "Cross: confirm the level up");
        }
        break;
    case LevelUpScript::Accept:
        if (L.shop && L.echoes0 >= 0 && (s.echoes != L.echoes0 || s.insight != L.insight0)) {
            go(LevelUpScript::WaitLevel, "the purchase went through");
        } else if (s.level > L.level0 && L.level0 >= 0) {
            go(LevelUpScript::WaitLevel, "level changed already");
        } else if (since >= 1.2) {
            if (++L.tries > 2) {
                go(LevelUpScript::WaitLevel, "no level change after the confirm presses");
                break;
            }
            // The confirmation dialog ("Spend N Blood Echoes and level up?"): the cursor starts on
            // NO (live: Cross there cancelled the raise), so Left to YES, then Cross.
            Tap(L.tries % 2 ? kPadLeft : kPadCross, now);
            Log("test: levelup: dialog: %s (echoes %d)", L.tries % 2 ? "Left (to YES)" : "Cross", s.echoes);
            L.at = now; // space the presses (1.2 s)
        }
        break;
    case LevelUpScript::WaitLevel:
        if (L.shop && L.echoes0 >= 0 && (s.echoes != L.echoes0 || s.insight != L.insight0)) {
            Log("test: levelup: PURCHASE on the guest: echoes %d -> %d, Insight %d -> %d", L.echoes0, s.echoes, L.insight0,
                s.insight);
            go(LevelUpScript::BackOut, "backing out (Circle)");
        } else if (L.shop && since >= 4.0) {
            Log("test: levelup: FAILED: no purchase (echoes %d, Insight %d)", s.echoes, s.insight);
            go(LevelUpScript::BackOut, "backing out (Circle)");
        } else if (L.shop) {
        } else if (s.level > L.level0 && L.level0 >= 0) {
            Log("test: levelup: LEVEL UP on the guest: level %d -> %d, echoes %d -> %d", L.level0, s.level, L.echoes0,
                s.echoes);
            go(LevelUpScript::BackOut, "backing out (Circle)");
        } else if (since >= 4.0) {
            Log("test: levelup: FAILED: level still %d (echoes %d)", s.level, s.echoes);
            go(LevelUpScript::BackOut, "backing out (Circle)");
        }
        break;
    case LevelUpScript::Keys:
        if (since >= 1.5) {
            if (L.key_at >= L.keys.size()) {
                Log("test: levelup: keys done: level %d -> %d, echoes %d -> %d, Insight %d -> %d, Blood Stone Shards now %d, Twin %d",
                    L.level0, s.level, L.echoes0, s.echoes, L.insight0, s.insight, GoodsCount(3000), GoodsCount(3010));
                go(LevelUpScript::BackOut, "backing out (Circle)");
                break;
            }
            std::size_t end = L.keys.find(',', L.key_at);
            if (end == std::string::npos) {
                end = L.keys.size();
            }
            const std::string k = L.keys.substr(L.key_at, end - L.key_at);
            L.key_at = end + 1;
            const std::uint32_t b = k == "cross" ? kPadCross : k == "circle" ? kPadCircle : k == "left" ? kPadLeft
                                    : k == "right" ? kPadRight : k == "up" ? kPadUp : k == "down" ? kPadDown : 0;
            if (b) {
                Tap(b, now);
            }
            Log("test: levelup: key %s (level %d, echoes %d, Insight %d, Blood Stone Shards %d, Twin %d, Blood Vials %d)",
                k.c_str(), s.level, s.echoes, s.insight, GoodsCount(3000), GoodsCount(3010), GoodsCount(1000));
            L.at = now;
        }
        break;
    case LevelUpScript::BackOut:
        if (since >= 1.0 * (L.tries + 1)) {
            if (++L.tries > 4) {
                go(LevelUpScript::Done, "done");
                break;
            }
            Tap(kPadCircle, now);
        }
        break;
    default:
        break;
    }
}

// Test fixture: stand the guest somewhere (PlayerIns vfunc +0x5F8 warp 0x18CD600: rsi float4
// position, rdx float4 rotation (0, yaw, 0, 0), ecx 0; the convention of 0x13AEADD) and/or set its
// echoes, so a scripted run can open a menu and spend them. Never on by default.
// Step `step` (0: the echoes and warp 0; k: warp k). False when there is no such step.
bool RunTestFixture(const GameSnapshot& s, int step) {
    const std::size_t e = g.test.find("echoes=");
    if (step == 0 && e != std::string::npos && s.player_rec) {
        const std::int32_t v = std::atoi(g.test.c_str() + e + 7);
        if (SafeWrite(s.player_rec + 0x94, &v, 4)) {
            Log("test: guest echoes set to %d", v);
        }
    }
    const std::size_t sh = g.test.find("shards=");
    if (step == 0 && sh != std::string::npos) {
        // Blood Stone Shards (goods 3000) and Twin Blood Stone Shards (3010: +3 -> +4 and up) for the
        // workbench check.
        const int n = std::atoi(g.test.c_str() + sh + 7);
        const int b0 = GoodsCount(3000), b1 = GoodsCount(3010);
        const bool ok = GiveGoods(3000, n) && GiveGoods(3010, n);
        Log("test: Blood Stone Shards %d -> %d, Twin Blood Stone Shards %d -> %d%s", b0, GoodsCount(3000), b1,
            GoodsCount(3010), ok ? "" : " (give failed)");
    }
    const std::size_t w = g.test.find("warp=");
    if (w == std::string::npos) {
        return step == 0;
    }
    const char* at = g.test.c_str() + w + 5;
    for (int k = 0; k < step; ++k) {
        at = std::strchr(at, '/');
        if (!at) {
            return false;
        }
        ++at;
    }
    alignas(16) float pos[4] = {0, 0, 0, 1};
    alignas(16) float rot[4] = {0, 0, 0, 0};
    std::uint32_t talk_target = 0;
    if (std::strncmp(at, "doll", 4) == 0) {
        talk_target = 210304;
    } else if (std::strncmp(at, "talk:", 5) == 0) {
        talk_target = std::uint32_t(std::strtoul(at + 5, nullptr, 10));
    }
    if (talk_target && talk_target != 210304 && FindChr(talk_target)) {
        const std::uint64_t chr = FindChr(talk_target);
        float d[4] = {}, r[4] = {};
        SafeRead(chr + 0xc0, d, sizeof d);
        SafeRead(chr + 0xd0, r, sizeof r);
        const float yaw = r[1];
        pos[0] = d[0] - 1.2f * std::sin(yaw);
        pos[1] = d[1];
        pos[2] = d[2] - 1.2f * std::cos(yaw);
        rot[1] = yaw + 3.14159265f;
        Log("test: t%u's character stands at (%.2f, %.2f, %.2f) yaw %.3f; guest goes 1.2 m in front, yaw %.3f",
            talk_target, d[0], d[1], d[2], yaw, rot[1]);
    } else if (std::strncmp(at, "doll", 4) == 0 && FindChr(210304)) {
        // In front of the Doll as this game placed her: ChrIns +0xC0 position, +0xD0 rotation
        // (x, yaw, z, 0; 0.524 = her MSB 30 degrees). Live evidence: a character with yaw t faces
        // (-sin t, 0, -cos t) (a guest put at +(sin, cos) * 1.2 ended up behind her). So: 1.2 m along
        // her forward, facing back at her (yaw + pi).
        const std::uint64_t doll = FindChr(210304);
        float d[4] = {}, r[4] = {};
        SafeRead(doll + 0xc0, d, sizeof d);
        SafeRead(doll + 0xd0, r, sizeof r);
        const float yaw = r[1];
        pos[0] = d[0] - 1.2f * std::sin(yaw);
        pos[1] = d[1];
        pos[2] = d[2] - 1.2f * std::cos(yaw);
        rot[1] = yaw + 3.14159265f;
        Log("test: the Doll stands at (%.2f, %.2f, %.2f) yaw %.3f on this game; guest goes 1.2 m in front, yaw %.3f",
            d[0], d[1], d[2], yaw, rot[1]);
    } else if (std::strncmp(at, "doll", 4) == 0) {
        // In front of the Doll's stand spot on this game (12100500-12100502, event 12100110):
        // MSB regions 2102301 / 2102302 / 2102303; 1.2 m off, facing her.
        static const float spots[3][3] = {{-5.8f, -5.62f, -13.6f}, {-13.27f, -1.05f, 1.73f}, {-7.4f, -6.25f, -25.09f}};
        int spot = 0;
        for (int k = 0; k < 3; ++k) {
            bool on = false;
            if (progress::ReadFlag(12100500 + k, &on) && on) {
                spot = k;
            }
        }
        pos[0] = spots[spot][0] + 0.6f;
        pos[1] = spots[spot][1];
        pos[2] = spots[spot][2] + 1.04f;
        rot[1] = std::atan2(-0.6f, -1.04f);
        Log("test: the Doll's spot on this game is #%d (12100500 + %d)", spot + 1, spot);
    } else if (talk_target && talk_target != 210304) {
        return false;
    } else if (std::sscanf(at, "%f,%f,%f,%f", &pos[0], &pos[1], &pos[2], &rot[1]) != 4) {
        return false;
    }
    if (!s.player) {
        return false;
    }
    std::uint64_t vtbl = 0, fn = 0;
    if (SafeGet(s.player, &vtbl) && SafeGet(vtbl + 0x5f8, &fn) && fn) {
        using WarpFn = void(BB_COOP_SYSV*)(std::uint64_t, const float*, const float*, std::uint64_t);
        reinterpret_cast<WarpFn>(fn)(s.player, pos, rot, 0);
        float now_at[3] = {};
        SafeRead(s.player + 0xc0, now_at, sizeof now_at);
        Log("test: warp #%d: the guest to (%.2f, %.2f, %.2f) yaw %.2f (now at %.2f, %.2f, %.2f)", step, pos[0], pos[1],
            pos[2], rot[1], now_at[0], now_at[1], now_at[2]);
    }
    return true;
}

} // namespace

BB_COOP_SYSV void LampObjEntry(u64 obj, u64, u64, u64, u64, u64); // test probe (below)

void GuestServicesInit() {
    if (g.init) {
        return;
    }
    g.init = true;
    if (const char* t = std::getenv("BB_PARTY_GUESTSVC_TEST")) {
        g.test = t;
    }
    const char* party = std::getenv("BB_PARTY");
    g.enabled = party && party[0] && GuestServicesEnabledFromEnv(std::getenv("BB_PARTY_GUEST_SERVICES"));
    if (!g.enabled) {
        if (party && party[0]) {
            Log("off (BB_PARTY_GUEST_SERVICES=0): guests cannot use the Doll, shops or workshop in the host's Dream");
        }
        return;
    }
    if (const char* t = std::getenv("BB_PARTY_GUESTSVC_TEST")) {
        g.test = t;
    }
    if (const char* e = std::getenv("BB_PARTY_GUEST_SERVICES_TALK")) {
        g.extra = e;
    }
    g_lamps = GuestLampsEnabledFromEnv(std::getenv("BB_PARTY_GUEST_LAMPS"));
    const char* hs = std::getenv("BB_PARTY_HEADSTONES");
    const bool headstones = g_lamps.load() || !(hs && hs[0] == '0' && !hs[1]);
    bool multi_ok = false;
    if (headstones) {
        multi_ok = Matches(kMultiJmp, {0xe9, 0x61, 0x29, 0x00, 0x00}) &&
                   HookValueFilter(kMultiOr, {0x08, 0xc1, 0x0f, 0xb6, 0xc1}, {0x4c, 0x89, 0xff, 0x89, 0xc6}, &MultiFilter,
                                   kTestResultInt, "talk IsMultiplayerInProgress (party headstones)");
        g_headstones = multi_ok;
        Log("headstones %s: the host travels from the Hunter's Dream with guests present (the party follows)",
            multi_ok ? "on" : "NOT installed");
    } else {
        Log("headstones off (BB_PARTY_HEADSTONES=0): vanilla, no headstone travel while in a party");
    }
    if (g_lamps.load()) {
        bool ok = multi_ok;
        ok = PatchBytes(kLampMultiGate, {0x85, 0xc9, 0x0f, 0x9f, 0xc1, 0x08, 0xc1}, {0x31, 0xc9, 0x90, 0x90, 0x90, 0x90, 0x90},
                        "lamp multiplayer gate +0x49 (party lamps)") && ok;
        SetLampWarpInterceptor(&GuestLampIntercept);
        Log("lamps %s: lamps and headstones work in a party; a guest's lamp use moves the whole party (host-led)",
            ok ? "on" : "partly installed");
    } else {
        Log("lamps off (opt-in until tested live: BB_PARTY_GUEST_LAMPS=1)");
    }
    if (!Matches(kIsClientJmp, {0xe9, 0x4d, 0x34, 0x00, 0x00})) {
        Log("guest +0x%llx is not the expected jmp; not installed", static_cast<unsigned long long>(kIsClientJmp));
        g.enabled = false;
        return;
    }
    SetEmevdRewriter2(&DollRewriter);
    if (g.test.find("lampprobe") != std::string::npos) {
        // push rbp; mov rbp, rsp; push r15..r12, rbx; sub rsp, 0x2848 (20 bytes; then mov [rbp-0x2820], rdx).
        HookPrologue(0x12F5C40, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81,
                                 0xec, 0x48, 0x28, 0x00, 0x00},
                     &LampObjEntry, "lamp object update (test probe)");
    }
    if (!g.test.empty()) {
        // Trace hooks for the live tests only (BB_PARTY_GUESTSVC_TEST): the LevelUp menu factory,
        // the talk command ids, the menu requests.
        // push rbp; mov rbp, rsp; push r15, r14, rbx; sub rsp, 0x48 (13 bytes, then mov rbx, rdi).
        HookPrologue(0x1BB7F60, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x48},
                     &LevelUpMenuFactoryEntry, "level-up menu factory (log)");
        // push rbp; mov rbp, rsp; push r15..r12, rbx; sub rsp, 0x3c8 (20 bytes; then mov rbx, rsi).
        HookPrologue(0x203FFD0, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81,
                                 0xec, 0xc8, 0x03, 0x00, 0x00},
                     &TalkCommandEntry, "talk command switch (log)");
        // push rbp; mov rbp, rsp; push r15, r14, r13, r12, rbx; sub rsp, 0x48 (17 bytes; then mov rbx, rdx).
        HookPrologue(0x1C4A520, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83,
                                 0xec, 0x48},
                     &MenuRequestEntry, "menu request (log)");
    }
    // (0x176BFE0, the request's first test, is not hooked: replacing it stopped the guest from
    // joining in a live run.)
    EnsureEmevdFilter();
    // rdi = r15 (the talk environment), rsi = eax (the game's answer).
    g.installed = HookValueFilter(kIsClientLoad, {0x41, 0x0f, 0xb6, 0x47, 0x25}, {0x4c, 0x89, 0xff, 0x89, 0xc6},
                                  &IsClientFilter, kTestResultInt, "talk IsClientPlayer (guest services)");
    Log("%s: a guest in the host's Hunter's Dream can use the Doll, the messengers' shops and the workshop",
        g.installed ? "on" : "not installed");
}

std::vector<EmevdSkipRule> GuestServicesSkipRules() {
    const char* party = std::getenv("BB_PARTY");
    if (!party || !party[0] || !GuestServicesEnabledFromEnv(std::getenv("BB_PARTY_GUEST_SERVICES"))) {
        return {};
    }
    std::vector<EmevdSkipRule> out = DollPlacementRules();
    if (GuestLampsEnabledFromEnv(std::getenv("BB_PARTY_GUEST_LAMPS"))) {
        for (const EmevdSkipRule& r : LampSkipRules()) {
            out.push_back(r);
        }
    }
    return out;
}

bool GuestServicesOnLampRequest(int slot, const std::string& body, std::string* why) {
    if (!g_lamps.load()) {
        if (why) {
            *why = "lamps are off on the host (BB_PARTY_GUEST_LAMPS)";
        }
        return false;
    }
    LampRequest r;
    if (!LampRequestFromJsonText(body, &r, why) || !SanitizePeerLampRequest(r, why)) {
        return false;
    }
    std::lock_guard<std::mutex> lk(g_lamp_mu);
    if (g_lamp_in.size() >= 8) {
        if (why) {
            *why = "too many pending requests";
        }
        return false;
    }
    g_lamp_in.emplace_back(slot, r);
    return true;
}

// Live test (BB_PARTY_GUESTSVC_TEST "lamppress"): a guest in the host's world outside the Dream
// presses Cross whenever an action prompt is up (it arrives at the host's lamp after a host warp),
// at most 8 times, 1.5 s apart, until its lamp warp became a request.
// Test-only lamp object probe ("lampprobe"): the lamp action-object update 0x12F5C40(obj) records
// its objects; the tick logs each one's entity (+0x20), type (+0x28), current row (+0x2C) and the
// four "unavailable" bytes +0x48..+0x4B (travel.md 1.1: 0x48 lamp state, 0x49 multiplayer,
// 0x4A no lamp record, 0x4B someone else at it).
std::atomic<std::uint64_t> g_lamp_objs[16];
BB_COOP_SYSV void LampObjEntry(u64 obj, u64, u64, u64, u64, u64) {
    for (auto& slot : g_lamp_objs) {
        std::uint64_t cur = slot.load(std::memory_order_relaxed);
        if (cur == obj) {
            return;
        }
        if (cur == 0 && slot.compare_exchange_strong(cur, obj)) {
            return;
        }
    }
}
void LogLampObjs() {
    for (auto& slot : g_lamp_objs) {
        const std::uint64_t o = slot.load(std::memory_order_relaxed);
        if (!o) {
            continue;
        }
        std::int32_t ent = 0, row = 0;
        std::uint8_t type = 0, b[4] = {};
        SafeGet(o + 0x20, &ent);
        SafeGet(o + 0x28, &type);
        SafeGet(o + 0x2c, &row);
        SafeRead(o + 0x48, b, 4);
        Log("test: lamp object %p entity %d type %u row %d: +0x48 %u +0x49 %u +0x4A %u +0x4B %u",
            reinterpret_cast<void*>(o), ent, type, row, b[0], b[1], b[2], b[3]);
    }
}

struct LampPress {
    double next = 0, release = 0;
    int presses = 0;
    bool done = false;
} g_lamp_press;

void LampPressTick(const GameSnapshot& s, double now) {
    LampPress& P = g_lamp_press;
    if (P.release > 0 && now >= P.release) {
        runtime_pad_director_press(0);
        P.release = 0;
    }
    if (P.done || g.test.find("lamppress") == std::string::npos || !s.world_up || s.loading ||
        s.session_role != kSessionRoleClient || IsDreamMap(s.map_id) || now < P.next) {
        return;
    }
    if (g_lamp_requests_made.load() > 0) {
        P.done = true;
        Log("test: lamppress: the guest's lamp use became a request after %d press(es)", P.presses);
        return;
    }
    P.next = now + 1.5;
    if (g.test.find("lampprobe") != std::string::npos) {
        LogLampObjs();
    }
    if (const std::size_t f = g.test.find("lampflag="); f != std::string::npos) {
        // The lamp's own "used" flag (the one its common event 7200 instance waits for), as the
        // lamp interaction would set it on this game (load mode 1: the host-world overlay).
        if (P.presses++ == 0) {
            const std::uint32_t flag = std::uint32_t(std::strtoul(g.test.c_str() + f + 9, nullptr, 10));
            const bool ok = WriteEventFlag(flag, true);
            Log("test: lamppress: lamp flag %u set on the guest%s (map %s); its 7200 runs the warp", flag,
                ok ? "" : " - FAILED", MapName(s.map_id).c_str());
        } else if (P.presses > 10) {
            P.done = true;
            Log("test: lamppress: FAILED: no lamp request 15 s after the lamp flag");
        }
        return;
    }
    std::uint32_t prompt = 0;
    const bool up = PromptUp(&prompt);
    if (!up && P.presses == 0 && P.next - now < 1.6) {
        static int waits = 0;
        if (++waits < 6) {
            Log("test: lamppress: in %s, no action prompt yet (%d)", MapName(s.map_id).c_str(), waits);
            return;
        }
    }
    if (++P.presses > 8) {
        P.done = true;
        Log("test: lamppress: FAILED: no lamp request after 8 presses");
        return;
    }
    runtime_pad_director_press(kPadCross);
    P.release = now + 0.12;
    Log("test: lamppress: Cross #%d (prompt %s %u, map %s)", P.presses, up ? "up" : "not seen", prompt,
        MapName(s.map_id).c_str());
}

void LampTick(const GameSnapshot& s, party::PartyLink* link) {
    g_session_role = s.session_role;
    if (!g.test.empty()) {
        LampPressTick(s, Now());
    }
    if (!g_lamps.load()) {
        return;
    }
    const PartyRole role = PartyDirector::Get().Role();
    std::deque<LampRequest> out;
    std::deque<std::pair<int, LampRequest>> in;
    {
        std::lock_guard<std::mutex> lk(g_lamp_mu);
        if (role == PartyRole::Guest && link) {
            out.swap(g_lamp_out);
        }
        if (role == PartyRole::Host) {
            in.swap(g_lamp_in);
        }
    }
    for (const LampRequest& r : out) {
        link->send_event(0, kLampRequestEventName, LampRequestToJsonText(r));
        Log("lamp: request %u sent to the host", r.id);
    }
    for (const auto& [slot, r] : in) {
        std::string why;
        if (!g_lamp_gate.Accept(s.world_up && !s.loading, Now(), &why)) {
            Log("lamp: request %u from slot %d not run: %s", r.id, slot, why.c_str());
            continue;
        }
        Log("lamp: slot %d used a lamp / headstone: the host warps to %u, the party follows", slot, r.id);
        RunLampWarp(r.id);
    }
}

void GuestServicesTick(const GameSnapshot& s, party::PartyLink* link) {
    LampTick(s, link);
    // Live test "hostheadstone[;keys=...]": the HOST uses a Dream headstone (t210000) with a guest
    // present (IsMultiplayerInProgress lift), 20 s after the first cooperator: the party follows.
    {
        static bool started = false;
        static double coop_at = 0;
        if (!started && g.test.find("hostheadstone") != std::string::npos && s.session_role == 3 &&
            IsDreamMap(s.map_id) && s.world_up && !s.loading && s.cooperators >= 1) {
            if (coop_at == 0) {
                coop_at = Now();
            } else if (Now() - coop_at >= 20.0 && FindChr(210000)) {
                started = true;
                g_lvl = LevelUpScript{};
                g_lvl.open = true;
                g_lvl.target = 210000;
                g_lvl.menu_text = 0;
                if (const std::size_t k = g.test.find("keys="); k != std::string::npos) {
                    const std::size_t e = g.test.find(';', k);
                    g_lvl.keys = g.test.substr(k + 5, e == std::string::npos ? std::string::npos : e - k - 5);
                }
                g_lvl.stage = LevelUpScript::WaitPlaced;
                g_lvl.at = Now();
                Log("test: host headstone: starting (cooperators %d)", s.cooperators);
            }
        }
        if (started && s.session_role == 3) {
            LevelUpTick(s, Now());
        }
    }
    static bool progress_logged = false;
    if (!g.test.empty() && s.world_up && !s.loading && IsDreamMap(s.map_id) && !progress_logged) {
        progress_logged = true;
        CheckDollProgress(s, s.session_role == kSessionRoleClient ? "guest (host world)" : "own world");
    } else if (!s.world_up || s.loading) {
        progress_logged = false;
    }
    static double last_probe = 0;
    if (g.test.find("probe") != std::string::npos && s.world_up && !s.loading && IsDreamMap(s.map_id) &&
        Now() - last_probe >= 10.0) {
        last_probe = Now();
        ProbeDoll();
    }
    if (!g.installed) {
        return;
    }
    GuestServicesInput in;
    in.enabled = g.enabled;
    in.world_up = s.world_up;
    in.loading = s.loading;
    in.session_role = s.session_role;
    in.map_id = s.map_id;
    const bool open = GuestServicesGate(in);
    if (open != g.open) {
        g.open = open;
        g_gate.store(open, std::memory_order_relaxed);
        if (open) {
            g.open_at = Now();
            g.rec = RecordOf(s);
            Log("open: guest in the host's %s; level %d, echoes %d, Insight %d", MapName(s.map_id).c_str(), g.rec.level,
                g.rec.echoes, g.rec.insight);
        } else {
            Log("closed (role %s, map %s%s)", SessionRoleName(s.session_role), MapName(s.map_id).c_str(),
                s.loading ? ", loading" : "");
        }
    }
    const double now = Now();
    bool doll_ready = true;
    std::uint32_t target = 0;
    if (g.test.find("warp=doll") != std::string::npos) {
        target = 210304;
    } else if (const std::size_t t = g.test.find("warp=talk:"); t != std::string::npos) {
        target = std::uint32_t(std::strtoul(g.test.c_str() + t + 10, nullptr, 10));
    }
    if (target && g.test_step == 0) {
        // Wait until the character exists and stands somewhere (polled every tick).
        float d[3] = {};
        const std::uint64_t chr = FindChr(target);
        doll_ready = chr && SafeRead(chr + 0xc0, d, sizeof d) && (d[0] != 0.0f || d[2] != 0.0f);
    }
    if (g.open && !g.test.empty() && !g.test_done && doll_ready && now - g.open_at >= 5.0 && now >= g.test_next) {
        if (!RunTestFixture(s, g.test_step)) {
            g.test_done = true;
            Log("test: fixture done");
        } else if (g.test_step == 0 && g.test.find("levelup") != std::string::npos) {
            g_lvl.stage = LevelUpScript::WaitPlaced;
            g_lvl.at = now;
            g_lvl.shop = false;
            g_lvl.menu_text = kTextChannel;
            g_lvl.target = target ? target : 210304;
        } else if (const std::size_t o = g.test.find("open"); g.test_step == 0 && o != std::string::npos) {
            g_lvl.stage = LevelUpScript::WaitPlaced;
            g_lvl.at = now;
            g_lvl.open = true;
            g_lvl.target = target;
            g_lvl.menu_text = g.test.compare(o, 5, "open=") == 0 ? std::uint32_t(std::strtoul(g.test.c_str() + o + 5, nullptr, 10)) : 0;
            if (const std::size_t k = g.test.find("keys="); k != std::string::npos) {
                const std::size_t e = g.test.find(';', k);
                g_lvl.keys = g.test.substr(k + 5, e == std::string::npos ? std::string::npos : e - k - 5);
            }
        } else if (g.test_step == 0 && g.test.find("buy") != std::string::npos) {
            g_lvl.stage = LevelUpScript::WaitPlaced;
            g_lvl.at = now;
            g_lvl.shop = true;
            g_lvl.menu_text = kTextPurchase;
            g_lvl.target = target;
        }
        ++g.test_step;
        g.test_next = now + 8.0;
        g.rec = RecordOf(ReadGameState(false)); // the fixture's echoes are not a purchase
    }
    if (g.open) {
        LevelUpTick(s, now);
    }
    if (!g.open || now - g.last_check < 1.0) {
        return;
    }
    g.last_check = now;
    for (int i = 0; i < kEnvSlots; ++i) {
        const std::uint64_t env = g_envs[i].load(std::memory_order_relaxed);
        if (!env || g.logged_envs[i] == env) {
            continue;
        }
        g.logged_envs[i] = env;
        LogEnv(env);
    }
    {
        std::string cmds;
        for (int i = 0; i < 96; ++i) {
            if (const std::uint32_t n = g_talk_cmds[i].exchange(0, std::memory_order_relaxed)) {
                cmds += " " + std::to_string(i) + "x" + std::to_string(n);
            }
        }
        if (!cmds.empty()) {
            Log("talk commands run:%s (31 = OpenSoul)", cmds.c_str());
        }
    }
    static std::uint32_t reqs_logged = 0;
    if (const std::uint32_t q = g_menu_requests.load(std::memory_order_relaxed); q != reqs_logged) {
        Log("menu requests %u (LevelUp %u)", q, g_levelup_requests.load());
        reqs_logged = q;
    }
    static std::uint32_t menus_logged = 0;
    if (const std::uint32_t m = g_levelup_menus.load(std::memory_order_relaxed); m != menus_logged) {
        Log("the game built the LevelUp menu (%u so far)", m);
        menus_logged = m;
    }
    if (const std::uint32_t d = g_doll_rewrites.exchange(0, std::memory_order_relaxed)) {
        Log("Doll level-up events (12100112-114): %u chr type test(s) set to the guest's own type", d);
    }
    if (const std::uint32_t m = g_multi_overrides.exchange(0, std::memory_order_relaxed)) {
        Log("headstones asked IsMultiplayerInProgress %u times (answered 0)", m);
    }
    const std::uint32_t n = g_overrides.load(std::memory_order_relaxed);
    if (n != g.overrides_logged) {
        Log("talk scripts asked IsClientPlayer %u times (answered 0)", n - g.overrides_logged);
        g.overrides_logged = n;
    }
    const PlayerRecord r = RecordOf(s);
    if (r != g.rec) {
        const std::string d = DescribeRecordChange(g.rec, r);
        if (!d.empty()) {
            Log("own character: %s", d.c_str());
            g.changed_at = now;
        }
        g.rec = r;
    } else if (g.changed_at > 0 && now - g.changed_at >= 15.0) {
        g.changed_at = 0;
        Log("own character steady for 15 s: level %d, echoes %d, Insight %d", r.level, r.echoes, r.insight);
    }
}

std::uint64_t GuestSvcFindChr(std::uint32_t id) {
    return FindChr(id);
}

bool GuestServicesOpen() {
    return g.open;
}

} // namespace coop
#endif

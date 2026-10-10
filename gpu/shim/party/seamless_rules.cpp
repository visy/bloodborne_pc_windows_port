// SPDX-License-Identifier: GPL-3.0-or-later
// Seamless party rules (see seamless_rules.h).
//
// Sources: the bell / boss / map-reload byte patches follow Wozzardman/shadp2p
// (GPL-2.0-or-later; documents/bloodborne-seamless-re.md, src/core/bloodborne_re.cpp,
// InstallSeamlessCoopPatches). The param repository walk follows droogie/bbhost
// (GPL-3.0-or-later) src/engine/params.cpp @8f2746c; row layouts from its
// include/bbhost/params.hpp, the EMEVD event layout from include/bbhost/engine/sprj/emk_system.hpp.
// Every offset was checked against our 1.09 eboot (docs/party/seamless_rules.md).
#include "seamless_rules.h"

#include "coop_hooks.h"
#include "game_state.h"
#include "party_phantom.h"
#include "party_travel.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace coop {
namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using Clock = std::chrono::steady_clock;
using ull = unsigned long long;

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[768];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    std::printf("Party seamless: %s\n", line);
    std::fflush(stdout);
}

bool EnvOff(const char* name) {
    const char* v = std::getenv(name);
    return v && v[0] == '0' && !v[1];
}

bool EnvOn(const char* name) {
    const char* v = std::getenv(name);
    return v && v[0] && !(v[0] == '0' && !v[1]);
}

// ---- The XML party patches: what the image holds at each site ----

struct Site {
    const char* patch;
    u64 off;
    const char* original;
    const char* patched;
};

// Our offsets (XML Address - 0x400000), 1.09 original and patched bytes (patches/Bloodborne.xml).
constexpr Site kSites[] = {
    {"Party: Bells anywhere", 0x157f8c8, "0f8790000000", "0f8f90000000"},
    {"Party: Bells anywhere", 0x157f960, "3401", "31c0"},
    {"Party: Bells anywhere", 0x157f6d1, "88c3", "b101"},
    {"Party: Bells anywhere", 0x157f6d3, "4c89f7e8f59f3400", "e9e9090000909090"},
    {"Party: Bells anywhere", 0x15068bb, "774a", "7f4a"},
    {"Party: Bells anywhere", 0x15068fc, "88c380f301", "b301909090"},
    {"Party: Bells anywhere", 0x191a8c3, "18c920c1eb02", "7d0488c1eb02"},
    {"Party: Bells anywhere", 0x18700d3, "84c041bdffffffff440f44eb41c1ed1f", "4531ed90909090909090909090909090"},
    {"Party: Bells after boss defeated", 0x18749e8, "0f8548010000", "909090909090"},
    {"Party: Bells after boss defeated", 0x18749f0, "0f8840010000", "909090909090"},
    {"Party: Bells after boss defeated", 0x14b714a, "0f85c0030000", "909090909090"},
    {"Party: Bells after boss defeated", 0x1874b79, "740d", "eb0d"},
    {"Party: Keep session on map reload", 0x19471b1, "e8ea955800", "e8ea8e5800"},
    {"Party: SOS sign timeout 30s", 0x4927a5c, "00003443", "0000f041"},
    {"Party: Bells without Insight", 0x157fa5c, "0f9fc0", "30c090"},
    {"Party: Bells without Insight", 0x18c92bb, "410fb64738f7d8", "31c00f1f440000"},
};

std::vector<u8> Hex(const char* h) {
    std::vector<u8> out;
    for (std::size_t i = 0; h[i] && h[i + 1]; i += 2) {
        char b[3] = {h[i], h[i + 1], 0};
        out.push_back(static_cast<u8>(std::strtoul(b, nullptr, 16)));
    }
    return out;
}

void ReportSites() {
    const char* current = nullptr;
    int applied = 0, off = 0, bad = 0;
    auto flush = [&] {
        if (current) {
            Log("%s: %s (%d sites applied, %d original, %d mismatched)", current,
                bad ? "MISMATCH" : (applied && !off ? "applied" : (!applied ? "off" : "PARTLY applied")), applied, off,
                bad);
        }
    };
    for (const Site& s : kSites) {
        if (!current || std::strcmp(current, s.patch) != 0) {
            flush();
            current = s.patch;
            applied = off = bad = 0;
        }
        const std::vector<u8> o = Hex(s.original), p = Hex(s.patched);
        if (Matches(s.off, p.data(), p.size())) {
            ++applied;
        } else if (Matches(s.off, o.data(), o.size())) {
            ++off;
        } else {
            ++bad;
            Log("%s: +0x%llx holds neither the 1.09 bytes nor the patch", s.patch, ull(s.off));
        }
    }
    flush();
}

// ---- Params (SoloParamRepository) ----
// Slot 0x5540340 (bbhost 0x5940340, SoloParamRepository_ptr): holders at +0x70, stride 0x48
// ({u32 count, ParamResCap* caps[8]}); cap +0x70 FD4ParamResCap, whose +0x70 is the param file.
// File: +0x0a u16 rows, +0x0c type name, +0x2d format, +0x2e flags; records at +0x40 (format
// >= 4 with flags & 2: 0x18 bytes {u32 id, pad, u64 data offset}; else 0xc {u32 id, u32 offset}).
// The 1.09 gameparam files are format 4 / flags 7 (checked offline in gameparam.parambnd.dcx).
constexpr u64 kParamRepository = 0x5540340;
constexpr unsigned kHolders = 63, kCapsPerHolder = 8;

struct ParamTable {
    u64 file = 0;
    u32 rows = 0;
    u8 format = 0, flags = 0;
};

std::string CapName(u64 cap) {
    u64 capacity = 0;
    if (!SafeGet(cap + 0x30, &capacity)) {
        return "";
    }
    u64 at = cap + 0x18;
    if (capacity >= 8 && !SafeGet(cap + 0x18, &at)) {
        return "";
    }
    std::string s;
    for (int i = 0; i < 64; ++i) {
        u16 c = 0;
        if (!SafeGet(at + 2u * unsigned(i), &c) || !c) {
            break;
        }
        s.push_back(c < 128 ? char(c) : '?');
    }
    return s;
}

bool FindParam(const char* name, const char* type, ParamTable* out) {
    u64 repo = 0;
    const u64 slot = Guest(kParamRepository);
    if (!slot || !SafeGet(slot, &repo) || !repo) {
        return false;
    }
    for (unsigned h = 0; h < kHolders; ++h) {
        const u64 holder = repo + 0x70 + u64(h) * 0x48;
        i32 count = 0;
        if (!SafeGet(holder, &count) || count <= 0) {
            continue;
        }
        for (unsigned k = 0; k < unsigned(count) && k < kCapsPerHolder; ++k) {
            u64 cap = 0, res = 0, file = 0;
            if (!SafeGet(holder + 8 + 8 * k, &cap) || !cap || !SafeGet(cap + 0x70, &res) || !res ||
                !SafeGet(res + 0x70, &file) || !file) {
                continue;
            }
            if (CapName(cap) != name) {
                continue;
            }
            char t[33] = {};
            u16 rows = 0;
            ParamTable p;
            if (!SafeRead(file + 0xc, t, 32) || std::strncmp(t, type, 32) != 0 || !SafeGet(file + 0xa, &rows) ||
                !SafeGet(file + 0x2d, &p.format) || !SafeGet(file + 0x2e, &p.flags)) {
                continue;
            }
            p.file = file;
            p.rows = rows;
            *out = p;
            return true;
        }
    }
    return false;
}

bool Record(const ParamTable& t, u32 i, u32* id, u64* data) {
    if (i >= t.rows) {
        return false;
    }
    if (t.format == 2) {
        u32 off = 0;
        if (!SafeGet(t.file + 0x34 + 0xc * u64(i), id) || !SafeGet(t.file + 0x34 + 0xc * u64(i) + 4, &off)) {
            return false;
        }
        *data = t.file + off;
    } else if (t.format <= 3 || !(t.flags & 2)) {
        u32 off = 0;
        if (!SafeGet(t.file + 0x40 + 0xc * u64(i), id) || !SafeGet(t.file + 0x40 + 0xc * u64(i) + 4, &off)) {
            return false;
        }
        *data = t.file + off;
    } else {
        u64 off = 0;
        if (!SafeGet(t.file + 0x40 + 0x18 * u64(i), id) || !SafeGet(t.file + 0x40 + 0x18 * u64(i) + 8, &off)) {
            return false;
        }
        *data = t.file + off;
    }
    return true;
}

std::size_t RowSize(const ParamTable& t) {
    u32 a = 0, b = 0;
    u64 da = 0, db = 0;
    if (t.rows < 2 || !Record(t, 0, &a, &da) || !Record(t, 1, &b, &db) || db <= da) {
        return 0;
    }
    return std::size_t(db - da);
}

/// The row with `id` (binary search; ids are sorted), or 0.
u64 Row(const ParamTable& t, u32 id) {
    u32 lo = 0, hi = t.rows;
    while (lo < hi) {
        const u32 mid = lo + (hi - lo) / 2;
        u32 rid = 0;
        u64 data = 0;
        if (!Record(t, mid, &rid, &data)) {
            return 0;
        }
        if (rid == id) {
            return data;
        }
        if (rid < id) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return 0;
}

// One field write: the game's value must be `from` (or already `to`).
struct ParamRule {
    const char* what;
    const char* param;
    const char* type;
    std::size_t row_size;
    u32 row;
    u32 field;
    std::size_t width; // 1 (u8) or 4 (f32)
    float from, to;
    const char* env_off; // the env switch that turns this rule off
    bool opt_in = false; // off unless env_off is set to a non-"0" value
    bool enabled = true;
    int state = 0; // 0 waiting, 1 written / holds, 2 refused (unexpected value or layout)
    std::uint64_t writes = 0;
};

ParamRule g_param_rules[] = {
    {"full HP for cooperators (SpEffect 9006 maxHpRate)", "SpEffectParam", "SP_EFFECT_PARAM_ST", 0x210, 9006, 0x10, 4,
     0.7f, 1.0f, "BB_PARTY_FULL_HP"},
    {"full HP for invader phantoms (SpEffect 9026 maxHpRate)", "SpEffectParam", "SP_EFFECT_PARAM_ST", 0x210, 9026,
     0x10, 4, 0.7f, 1.0f, "BB_PARTY_FULL_HP"},
    {"Beckoning Bell without Insight (EquipParamGoods 200 consumeHeroPoint)", "EquipParamGoods",
     "EQUIP_PARAM_GOODS_ST", 0x6c, 200, 0x38, 1, 1.0f, 0.0f, "BB_PARTY_BELL_NO_INSIGHT"},
    // Hunter's Mark for guests (phantom_limits.md 1.1 / 3.2): byte 0x44 |= enable_white (bit 2) |
    // enable_multi (bit 4); 1.09 data 0x43 / 0xC3. Opt-in: a guest's Mark then goes to the host's
    // last lamp (party_travel's OnReviveMagic_1 redirect), the host's is a party travel.
    {"Hunter's Mark for phantoms (EquipParamGoods 100 enable_white / enable_multi)", "EquipParamGoods",
     "EQUIP_PARAM_GOODS_ST", 0x6c, 100, 0x44, 1, 67.0f, 87.0f, "BB_PARTY_GUEST_MARK", true},
    {"Bold Hunter's Mark for phantoms (EquipParamGoods 1400 enable_white / enable_multi)", "EquipParamGoods",
     "EQUIP_PARAM_GOODS_ST", 0x6c, 1400, 0x44, 1, 195.0f, 215.0f, "BB_PARTY_GUEST_MARK", true},
};

bool ReadField(u64 at, std::size_t width, float* v) {
    if (width == 1) {
        u8 b = 0;
        if (!SafeGet(at, &b)) {
            return false;
        }
        *v = float(b);
        return true;
    }
    return SafeGet(at, v);
}

bool WriteField(u64 at, std::size_t width, float v) {
    if (width == 1) {
        const u8 b = u8(v);
        return SafeWrite(at, &b, 1);
    }
    return SafeWrite(at, &v, 4);
}

/// True once every enabled rule holds (written or found written).
bool ApplyParamRules() {
    bool all = true;
    for (ParamRule& r : g_param_rules) {
        if (!r.enabled || r.state == 2) {
            continue;
        }
        ParamTable t;
        if (!FindParam(r.param, r.type, &t)) {
            all = false;
            continue;
        }
        const std::size_t size = RowSize(t);
        const u64 row = Row(t, r.row);
        if ((size && size != r.row_size) || !row) {
            Log("%s: %s %s (row size %zu, expected %zu); left alone", r.what, r.param,
                row ? "has another layout" : "has no such row", size, r.row_size);
            r.state = 2;
            continue;
        }
        float v = 0;
        if (!ReadField(row + r.field, r.width, &v)) {
            all = false;
            continue;
        }
        if (v == r.to) {
            if (r.state == 0) {
                Log("%s: already %g", r.what, double(r.to));
            }
            r.state = 1;
            continue;
        }
        if (v != r.from) {
            Log("%s: the game's value is %g, not %g; left alone", r.what, double(v), double(r.from));
            r.state = 2;
            continue;
        }
        if (!WriteField(row + r.field, r.width, r.to)) {
            all = false;
            continue;
        }
        ++r.writes;
        Log("%s: %g -> %g (row at 0x%llx%s)", r.what, double(v), double(r.to), ull(row),
            r.writes > 1 ? ", again: the params were reloaded" : "");
        r.state = 1;
    }
    return all;
}

// ---- EMEVD instruction dispatch filter (0x17b93a0) ----
// SprjEmkInstructionDispatcher::Dispatch(this, SprjEmkEventIns* event, float dt) -> bool. Its one
// caller is SprjEmkEventIns::Update (0x12ecdc0), which ignores the result and then advances the
// event (0x12ef2e0, instruction_delta defaults to 1): a skipped instruction is simply passed.
// Event: +0x28 i32 event id, +0x68 map id, +0xa0 instruction index, +0xa8 EVD runtime data,
// +0xb0 the instruction record {i32 bank, i32 id, u32 argument size, pad, i64 argument offset},
// +0xb8 the copied arguments (else EVD base + *(base + 0x78) + argument offset).
constexpr u64 kEmevdDispatch = 0x17b93a0;

using DispatchFn = u8(BB_COOP_SYSV*)(u64 self, u64 event, float dt);
DispatchFn g_dispatch_original = nullptr;
bool g_emevd_trace = false;
std::atomic<EmevdRewriter> g_rewriter{nullptr};

// Rules "event:bank:id[@index]" (party_phantom.h). Built in: the confinement walls of common
// event 7600 (index 6 / 7, only while B1 travel is on; BB_PARTY_OPEN_WORLD=0 drops them). The A6
// "send phantoms home" instructions are still to be identified with BB_PARTY_EMEVD_TRACE=1.
std::vector<EmevdSkipRule> g_skips;
bool g_emevd_observe = false; // party_phantom's boss-Insight observer

std::mutex g_emevd_mu;
std::unordered_map<u64, u64> g_emevd_seen; // key: event << 32 ^ bank << 16 ^ id
std::atomic<u64> g_emevd_calls{0}, g_emevd_skipped{0};

void ParseSkips() {
    for (const EmevdSkipRule& r : PhantomSkipRules()) {
        g_skips.push_back(r);
        Log("EMEVD rule (built in, confinement walls): skip %s", DescribeSkipRule(r).c_str());
    }
    const char* e = std::getenv("BB_PARTY_EMEVD_SKIP");
    if (!e) {
        return;
    }
    std::vector<EmevdSkipRule> rules;
    std::vector<std::string> errors;
    ParseEmevdSkipRules(e, &rules, &errors);
    for (const std::string& err : errors) {
        Log("BB_PARTY_EMEVD_SKIP: %s; ignored", err.c_str());
    }
    for (const EmevdSkipRule& r : rules) {
        g_skips.push_back(r);
        Log("EMEVD rule: skip %s", DescribeSkipRule(r).c_str());
    }
}

void TraceInstruction(u64 event, i32 event_id, i32 bank, i32 id, u64 instr, bool skipped) {
    const u64 key = (u64(u32(event_id)) << 32) ^ (u64(u16(bank)) << 16) ^ u64(u16(id));
    u64 n;
    {
        std::lock_guard<std::mutex> lk(g_emevd_mu);
        n = ++g_emevd_seen[key];
    }
    if (n != 1 && n != 1000 && n != 100000) {
        return;
    }
    u32 map = 0, arg_size = 0;
    i32 index = -1;
    SafeGet(event + 0x68, &map);
    SafeGet(event + 0xa0, &index);
    const u64 args = EmevdInstructionArgs(event, instr, &arg_size);
    char hex[3 * 24 + 1] = "";
    u8 bytes[24] = {};
    const std::size_t shown = arg_size < sizeof bytes ? arg_size : sizeof bytes;
    if (args && shown && SafeRead(args, bytes, shown)) {
        for (std::size_t i = 0; i < shown; ++i) {
            std::snprintf(hex + 3 * i, 4, "%02x ", bytes[i]);
        }
    }
    Log("EMEVD event %d [%d] %d[%02d] map %08x args(%u) %s(call #%llu)%s", event_id, index, bank, id, map, arg_size,
        hex, ull(n), skipped ? " SKIPPED (rule)" : "");
}

BB_COOP_SYSV u8 EmevdDispatch(u64 self, u64 event, float dt) {
    g_emevd_calls.fetch_add(1, std::memory_order_relaxed);
    u64 instr = 0;
    i32 event_id = 0, bank = -1, id = -1;
    if (event && SafeGet(event + 0xb0, &instr) && instr) {
        SafeGet(event + 0x28, &event_id);
        SafeGet(instr, &bank);
        SafeGet(instr + 4, &id);
    }
    bool skip = false;
    if (instr && !g_skips.empty()) {
        i32 index = -1;
        SafeGet(event + 0xa0, &index);
        for (const EmevdSkipRule& r : g_skips) {
            if (EmevdSkipMatches(r, event_id, bank, id, index) && (!r.needs_travel || TravelEnabled())) {
                skip = true;
                break;
            }
        }
    }
    if (g_emevd_observe && instr && !skip &&
        ((bank == 2003 && (id == 12 || id == 15 || id == 53)) || (bank == 2000 && id == 0))) {
        u8 args[12] = {};
        u32 arg_size = 0;
        const u64 at = EmevdInstructionArgs(event, instr, &arg_size);
        const std::size_t n = arg_size < sizeof args ? arg_size : sizeof args;
        const bool have = at && n && SafeRead(at, args, n);
        PhantomEmevdInstruction(event_id, bank, id, have ? args : nullptr, have ? n : 0);
    }
    if (g_emevd_trace && instr) {
        TraceInstruction(event, event_id, bank, id, instr, skip);
    }
    if (skip) {
        g_emevd_skipped.fetch_add(1, std::memory_order_relaxed);
        return 1;
    }
    if (instr) {
        if (const EmevdRewriter rw = g_rewriter.load(std::memory_order_relaxed)) {
            rw(event_id, bank, id, event, instr);
        }
    }
    return g_dispatch_original(self, event, dt);
}

struct State {
    bool init = false;
    bool params_done = false;
    Clock::time_point last_param{};
    bool param_logged_wait = false;
    Clock::time_point start{};
    Clock::time_point last_emevd_log{};
};

State& S() {
    static State s;
    return s;
}

double Seconds(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double>(b - a).count();
}

} // namespace

bool SeamlessRequested() {
    const char* p = std::getenv("BB_PARTY");
    return p && p[0] && !EnvOff("BB_PARTY_SEAMLESS");
}

void SeamlessRulesInit() {
    State& st = S();
    if (st.init) {
        return;
    }
    if (!SeamlessRequested()) {
        Log("off (%s)", EnvOff("BB_PARTY_SEAMLESS") ? "BB_PARTY_SEAMLESS=0" : "BB_PARTY not set");
        ReportSites(); // shows the XML patches are off too
        return;
    }
    if (!Image()) {
        Log("no image; off");
        return;
    }
    st.init = true;
    st.start = Clock::now();
    ReportSites();
    for (ParamRule& r : g_param_rules) {
        r.enabled = r.opt_in ? EnvOn(r.env_off) : !EnvOff(r.env_off);
        if (!r.enabled) {
            Log("%s: off (%s%s)", r.what, r.env_off, r.opt_in ? " not set; opt-in" : "=0");
        }
    }
    g_emevd_trace = EnvOn("BB_PARTY_EMEVD_TRACE");
    ParseSkips();
    g_emevd_observe = PhantomWantsEmevd();
    if (g_emevd_trace || !g_skips.empty() || g_emevd_observe) {
        if (EnsureEmevdFilter()) {
            Log("EMEVD filter: %s, %zu skip rules, boss-Insight observer %s", g_emevd_trace ? "trace on" : "trace off",
                g_skips.size(), g_emevd_observe ? "on" : "off");
        } else {
            g_emevd_trace = false;
            g_emevd_observe = false;
            g_skips.clear();
        }
    }
}

void SetEmevdRewriter(EmevdRewriter fn) {
    g_rewriter.store(fn);
}

bool EnsureEmevdFilter() {
    static std::mutex mu;
    std::lock_guard<std::mutex> lk(mu);
    if (g_dispatch_original) {
        return true;
    }
    // push rbp; mov rbp, rsp; push rbx; push rax - 6 bytes, no relative operands.
    void* original = nullptr;
    if (!ReplacePrologue(kEmevdDispatch, {0x55, 0x48, 0x89, 0xe5, 0x53, 0x50},
                         reinterpret_cast<const void*>(&EmevdDispatch), &original, "EMEVD instruction dispatch filter")) {
        return false;
    }
    g_dispatch_original = reinterpret_cast<DispatchFn>(original);
    return true;
}

std::uint64_t EmevdInstructionArgs(std::uint64_t event, std::uint64_t instr, std::uint32_t* size) {
    u32 arg_size = 0;
    i64 arg_off = 0;
    if (size) {
        *size = 0;
    }
    if (!event || !instr || !SafeGet(instr + 8, &arg_size) || !SafeGet(instr + 0x10, &arg_off)) {
        return 0;
    }
    u64 args = 0;
    SafeGet(event + 0xb8, &args);
    if (!args) {
        u64 runtime = 0, base = 0, section = 0;
        if (SafeGet(event + 0xa8, &runtime) && runtime && SafeGet(runtime + 8, &base) && base &&
            SafeGet(base + 0x78, &section)) {
            args = base + section + u64(arg_off);
        }
    }
    if (args && size) {
        *size = arg_size;
    }
    return args;
}

void SeamlessRulesTick() {
    State& st = S();
    if (!st.init) {
        return;
    }
    const auto now = Clock::now();
    // Params: every 0.5 s until they are written, then every 2 s (a reload restores them).
    if (Seconds(st.last_param, now) >= (st.params_done ? 2.0 : 0.5)) {
        st.last_param = now;
        const bool done = ApplyParamRules();
        if (done && !st.params_done) {
            Log("param rules hold (%.1f s after start)", Seconds(st.start, now));
        }
        if (!done && !st.param_logged_wait && Seconds(st.start, now) > 60.0) {
            st.param_logged_wait = true;
            Log("params not found 60 s after start; still trying");
        }
        st.params_done = done;
    }
    if (g_dispatch_original && Seconds(st.last_emevd_log, now) >= 60.0) {
        st.last_emevd_log = now;
        std::size_t kinds;
        {
            std::lock_guard<std::mutex> lk(g_emevd_mu);
            kinds = g_emevd_seen.size();
        }
        Log("EMEVD: %llu instructions dispatched, %llu skipped, %zu distinct (event, instruction)",
            ull(g_emevd_calls.load()), ull(g_emevd_skipped.load()), kinds);
    }
}

} // namespace coop

// SPDX-License-Identifier: GPL-3.0-or-later
// Party phantoms as full players (see party_phantom.h; the RE is docs/party/phantom_limits.md).
//
// Offsets are ours (raw ELF VA of eboot.elf 1.09), each read from the disassembly:
//   0x14DACE0 auto-replenish (no arguments; GameDataMan slot 0x553B130 -> +8 PlayerGameData,
//     +0x5B0 inventory list, +0x328 storage) and 0x14DBB70 (no arguments; +0x390 / +0x3A0), the
//     pair MoveMapStep calls at 0x19398D9 / 0x19398DE.
//   SpEffect apply: PlayerIns vfunc +0x3F0 (chr, id, source chr, 0, 0, 0, [rsp] 0, xmm0..4 1.0f),
//     the convention of BlockClear2 0x13852B1 (SpEffect 4680) and MultiDoping 0x138BD48.
//   WorldTransitionState slot 0x5556678: +0x08 stage request, +0x1528 last lamp {id, aux},
//     +0x1592 own world.
#include "party_phantom.h"

#include "net/json.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <stdexcept>

#ifndef BB_PARTY_PHANTOM_NO_GAME
#include "coop_hooks.h"
#include "game_state.h"
#include "party_director.h"
#include "party_link.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <deque>
#include <mutex>
#endif

namespace coop {

// ---------------------------------------------------------------------------------------------
// Pure part (unit-tested)
// ---------------------------------------------------------------------------------------------

namespace {

std::string Trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) {
        ++a;
    }
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) {
        --b;
    }
    return s.substr(a, b - a);
}

/// A whole decimal / 0x-hex integer in [lo, hi].
bool ParseInt(const std::string& s, long lo, long hi, std::int32_t* out) {
    if (s.empty()) {
        return false;
    }
    char* end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 0);
    if (!end || *end || v < lo || v > hi) {
        return false;
    }
    *out = static_cast<std::int32_t>(v);
    return true;
}

} // namespace

std::size_t ParseEmevdSkipRules(const std::string& text, std::vector<EmevdSkipRule>* out,
                                std::vector<std::string>* errors) {
    std::size_t added = 0, at = 0;
    while (at <= text.size()) {
        std::size_t comma = text.find(',', at);
        if (comma == std::string::npos) {
            comma = text.size();
        }
        const std::string item = Trim(text.substr(at, comma - at));
        at = comma + 1;
        if (item.empty()) {
            continue;
        }
        EmevdSkipRule r;
        std::string body = item;
        bool ok = true;
        if (const std::size_t amp = body.find('@'); amp != std::string::npos) {
            ok = ParseInt(Trim(body.substr(amp + 1)), 0, 0xffff, &r.index);
            body = body.substr(0, amp);
        }
        const std::size_t c1 = body.find(':');
        const std::size_t c2 = c1 == std::string::npos ? c1 : body.find(':', c1 + 1);
        if (ok && c2 != std::string::npos && body.find(':', c2 + 1) == std::string::npos) {
            const std::string ev = Trim(body.substr(0, c1));
            if (ev == "*") {
                r.event = -1;
            } else {
                ok = ParseInt(ev, 0, 0x7fffffff, &r.event);
            }
            ok = ok && ParseInt(Trim(body.substr(c1 + 1, c2 - c1 - 1)), 0, 0xffff, &r.bank) &&
                 ParseInt(Trim(body.substr(c2 + 1)), 0, 0xffff, &r.id);
        } else {
            ok = false;
        }
        if (!ok) {
            if (errors) {
                errors->push_back("'" + item + "' is not event:bank:id[@index]");
            }
            continue;
        }
        out->push_back(r);
        ++added;
    }
    return added;
}

bool EmevdSkipMatches(const EmevdSkipRule& r, std::int32_t event, std::int32_t bank, std::int32_t id,
                      std::int32_t index) {
    return (r.event < 0 || r.event == event) && r.bank == bank && r.id == id && (r.index < 0 || r.index == index);
}

std::string DescribeSkipRule(const EmevdSkipRule& r) {
    char text[128];
    char ev[16] = "*", idx[16] = "";
    if (r.event >= 0) {
        std::snprintf(ev, sizeof ev, "%d", r.event);
    }
    if (r.index >= 0) {
        std::snprintf(idx, sizeof idx, "@%d", r.index);
    }
    std::snprintf(text, sizeof text, "%s:%d:%d%s%s", ev, r.bank, r.id, idx, r.needs_travel ? " (with B1 travel)" : "");
    return text;
}

std::vector<EmevdSkipRule> ConfinementWallRules() {
    // common.emevd event 7600 (args X0 object, X1 SFX): 1 2005[3](X0, 0) off ... 5 wait for
    // multiplayer ... 6 2005[3](X0, 1) on, 7 2006[2](X1) SFX on ... 11 restart.
    EmevdSkipRule on{7600, 2005, 3, 6, true};
    EmevdSkipRule sfx{7600, 2006, 2, 7, true};
    return {on, sfx};
}

const char* PhantomEventKindName(PhantomEventKind k) {
    switch (k) {
    case PhantomEventKind::Lamp: return "lamp";
    case PhantomEventKind::Rested: return "rested";
    case PhantomEventKind::Insight: return "insight";
    case PhantomEventKind::Unknown: break;
    }
    return "unknown";
}

std::string PhantomEventToJsonText(const PhantomEvent& e) {
    json::Value v = json::Value::make_object();
    v.set("kind", PhantomEventKindName(e.kind));
    v.set("seq", json::hex(e.seq));
    if (e.kind == PhantomEventKind::Lamp) {
        v.set("lamp", e.lamp);
    }
    if (e.kind == PhantomEventKind::Insight) {
        v.set("n", e.insight);
        v.set("event", e.source_event);
    }
    return json::dump(v, 0);
}

bool PhantomEventFromJsonText(const std::string& text, PhantomEvent* out, std::string* error) {
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
        PhantomEvent e;
        const std::string& kind = json::str(v, "kind");
        for (PhantomEventKind k : {PhantomEventKind::Lamp, PhantomEventKind::Rested, PhantomEventKind::Insight}) {
            if (kind == PhantomEventKindName(k)) {
                e.kind = k;
            }
        }
        if (e.kind == PhantomEventKind::Unknown) {
            throw std::runtime_error("unknown kind '" + kind + "'");
        }
        e.seq = json::u64(v, "seq");
        if (e.kind == PhantomEventKind::Lamp) {
            e.lamp = json::u32(v, "lamp");
        }
        if (e.kind == PhantomEventKind::Insight) {
            const double n = json::num(v, "n");
            if (n < 0 || n > 99 || n != static_cast<double>(static_cast<int>(n))) {
                throw std::runtime_error("n out of range");
            }
            e.insight = static_cast<int>(n);
            e.source_event = json::u32(v, "event");
        }
        *out = e;
        return true;
    } catch (const std::exception& ex) {
        if (error) {
            *error = ex.what();
        }
        return false;
    }
}

bool SanitizePeerPhantom(const PhantomEvent& e, std::string* why) {
    if (e.kind == PhantomEventKind::Lamp && e.lamp != kTravelNone && !TravelLampKnown(e.lamp)) {
        if (why) {
            *why = "lamp " + std::to_string(e.lamp) + " is not a ReturnPointParam row";
        }
        return false;
    }
    return true;
}

bool RestedFromTravel(const TravelIntent& t) {
    switch (t.kind) {
    case TravelKind::Lamp:
        return t.respawn_mode == 2; // a world lamp into the Hunter's Dream (0x13CDF30 mode 2)
    case TravelKind::HostDeath:
    case TravelKind::HuntersMark:
        return true; // respawn-type loads at the last lamp
    default:
        return false;
    }
}

InsightMode InsightModeFromString(const char* s) {
    if (!s || !s[0]) {
        return InsightMode::Parity;
    }
    const std::string v(s);
    if (v == "0" || v == "off") {
        return InsightMode::Off;
    }
    if (v == "full") {
        return InsightMode::Full;
    }
    return InsightMode::Parity;
}

const char* InsightModeName(InsightMode m) {
    switch (m) {
    case InsightMode::Off: return "off";
    case InsightMode::Parity: return "parity";
    case InsightMode::Full: return "full";
    }
    return "?";
}

int GuestInsightGrant(int host_n, InsightMode mode) {
    if (host_n <= 0) {
        return 0;
    }
    switch (mode) {
    case InsightMode::Off: return 0;
    case InsightMode::Parity: return host_n - 1; // the native BlockClear2 +1 makes it N
    case InsightMode::Full: return host_n;
    }
    return 0;
}

void BossInsightTracker::OnBossDefeat(std::int32_t event, double now) {
    defeats_[event] = now;
    // Keep it small: drop stale entries.
    for (auto it = defeats_.begin(); it != defeats_.end();) {
        it = now - it->second > kWindow ? defeats_.erase(it) : std::next(it);
    }
}

std::optional<int> BossInsightTracker::OnInitializeEvent(std::int32_t event, std::int32_t target, std::int32_t n,
                                                         double now) {
    if (target != kInsightEvent) {
        return std::nullopt;
    }
    const auto it = defeats_.find(event);
    if (it == defeats_.end()) {
        return std::nullopt; // first encounter, story Insight, ...: not a boss kill
    }
    const bool fresh = now - it->second <= kWindow;
    defeats_.erase(it); // once per defeat
    if (!fresh || n <= 0 || n > 99) {
        return std::nullopt;
    }
    return n;
}

void InsightDripper::Add(int n) {
    if (n <= 0) {
        return;
    }
    pending_ = pending_ + n > kMaxPending ? kMaxPending : pending_ + n;
}

bool InsightDripper::Step(bool allowed) {
    if (wait_ > 0) {
        --wait_;
    }
    if (pending_ <= 0 || !allowed || wait_ > 0) {
        return false;
    }
    --pending_;
    wait_ = kInterval;
    return true;
}

const char* RefillReasonName(RefillReason r) {
    switch (r) {
    case RefillReason::HostRested: return "host rested";
    case RefillReason::GuestRespawn: return "guest respawn";
    case RefillReason::Probe: return "probe";
    }
    return "?";
}

void RefillScheduler::Request(RefillReason why, bool expect_load, double now) {
    // A newer request restarts the wait (a second warp: refill after the last load).
    pending_ = true;
    why_ = why;
    expect_load_ = expect_load;
    load_seen_ = false;
    since_ = now;
    stable_ = 0;
}

bool RefillScheduler::Step(const ReplayState& s, double now) {
    if (!pending_) {
        return false;
    }
    if (s.loading || !s.world_up) {
        load_seen_ = true;
    }
    stable_ = ReplayAllowedNow(s) ? stable_ + 1 : 0;
    if (now - since_ > kTimeout) {
        pending_ = false;
        return false;
    }
    const bool load_done = !expect_load_ || load_seen_ || now - since_ > kLoadWait;
    if (load_done && stable_ >= kStableTicks) {
        pending_ = false;
        return true;
    }
    return false;
}

bool LampAnnouncer::ShouldSend(std::uint32_t lamp, std::uint64_t members_key) {
    if (lamp == kTravelNone || lamp == 0) {
        return false;
    }
    if (sent_ && lamp == lamp_ && members_key == members_) {
        return false;
    }
    sent_ = true;
    lamp_ = lamp;
    members_ = members_key;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Game part
// ---------------------------------------------------------------------------------------------
#ifndef BB_PARTY_PHANTOM_NO_GAME

namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using ull = unsigned long long;
using Clock = std::chrono::steady_clock;

constexpr u64 kWts = 0x5556678;
constexpr u64 kWtsRequested = 0x08, kWtsLastLamp = 0x1528, kWtsOwnWorld = 0x1592;
constexpr u64 kWorldChrMan = 0x553e878; // +0x60 PlayerIns
constexpr u64 kGameDataMan = 0x553b130; // +8 PlayerGameData
constexpr u64 kReplenish = 0x14dace0;   // void(): vials / bullets from storage
constexpr u64 kBloodBullets = 0x14dbb70; // void(): Blood Bullets reset (MoveMapStep calls it first)
constexpr u64 kSpEffectVfunc = 0x3f0;
constexpr u32 kInsightSpEffect = 4680; // heroPointDamage -1: +1 Insight
constexpr double kParticipation = 90.0; // s: a guest that was a client this recently was in the fight

using VoidFn = void(BB_COOP_SYSV*)();
using SpEffectFn = u64(BB_COOP_SYSV*)(u64 chr, u64 id, u64 source, u64 c, u64 r8, u64 r9, u64 stack0, float, float,
                                      float, float, float);

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[768];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    std::printf("Party phantom: %s\n", line);
    std::fflush(stdout);
}

bool EnvOff(const char* name) {
    const char* v = std::getenv(name);
    return v && v[0] == '0' && !v[1];
}

bool PartyMode() {
    const char* p = std::getenv("BB_PARTY");
    return p && p[0];
}

double Now() {
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

struct Config {
    bool init = false;
    bool respawn = true, refill = true, open_world = true;
    InsightMode insight = InsightMode::Parity;
    bool probe_insight = false, probe_refill = false;
};
Config g_cfg;

std::atomic<u64> g_seq{0};
u64 NextSeq() {
    u64 cur = g_seq.load();
    if (cur == 0) {
        const u64 seed = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count());
        g_seq.compare_exchange_strong(cur, seed);
    }
    return g_seq.fetch_add(1) + 1;
}

// Host.
std::mutex g_host_mu;
BossInsightTracker g_tracker;       // under g_host_mu
std::deque<PhantomEvent> g_outbox;  // under g_host_mu
LampAnnouncer g_lamp_announcer;     // main thread
double g_last_lamp_read = 0;        // main thread

// Guest.
std::mutex g_guest_mu;
RefillScheduler g_refill;           // under g_guest_mu
InsightDripper g_drip;              // under g_guest_mu
u64 g_last_rested = 0, g_last_insight = 0; // under g_guest_mu
double g_last_insight_grant = -1e9;        // under g_guest_mu
constexpr double kInsightMinInterval = 30.0;
constexpr int kMaxInsightPerEvent = 10;
std::atomic<double> g_last_client{-1e9};

// Probe (main thread).
bool g_world_seen = false;
double g_world_at = 0;
bool g_probe_done = false;

void Queue(const PhantomEvent& e) {
    std::lock_guard<std::mutex> lk(g_host_mu);
    if (g_outbox.size() >= 32) {
        g_outbox.pop_front();
    }
    g_outbox.push_back(e);
}

u64 Wts() {
    u64 p = 0;
    return SafeGet(Guest(kWts), &p) ? p : 0;
}

/// The host's own last lamp id (own world only), else kTravelNone.
u32 OwnLastLamp() {
    const u64 wts = Wts();
    u8 own = 0;
    u64 rec = ~0ull;
    if (!wts || !SafeGet(wts + kWtsOwnWorld, &own) || !own || !SafeGet(wts + kWtsLastLamp, &rec)) {
        return kTravelNone;
    }
    const u32 id = static_cast<u32>(rec);
    return id == 0 ? kTravelNone : id;
}

u64 PlayerGameData() {
    u64 gdm = 0, rec = 0;
    return SafeGet(Guest(kGameDataMan), &gdm) && gdm && SafeGet(gdm + 8, &rec) ? rec : 0;
}

u64 LocalPlayer() {
    u64 wcm = 0, player = 0;
    return SafeGet(Guest(kWorldChrMan), &wcm) && wcm && SafeGet(wcm + 0x60, &player) ? player : 0;
}

bool InImage(u64 address) {
    const u64 base = Guest(0);
    return base && address >= base && address < base + ImageSize();
}

/// The game's own auto-replenish, as MoveMapStep runs it after a respawn-type load.
bool Refill(RefillReason why) {
    const u64 rec = PlayerGameData();
    u64 inv = 0, a = 0, b = 0;
    if (!rec || !SafeGet(rec + 0x5b0, &inv) || !inv || !SafeGet(rec + 0x390, &a) || !a || !SafeGet(rec + 0x3a0, &b) ||
        !b) {
        Log("refill (%s): no player inventory; skipped", RefillReasonName(why));
        return false;
    }
    reinterpret_cast<VoidFn>(Guest(kBloodBullets))();
    reinterpret_cast<VoidFn>(Guest(kReplenish))();
    Log("refill (%s): vials and bullets topped up from storage (0x14DBB70, 0x14DACE0)", RefillReasonName(why));
    return true;
}

/// SpEffect 4680 on the local player (BlockClear2's call at 0x13852B1).
bool ApplyInsightSpEffect() {
    const u64 player = LocalPlayer();
    u64 vt = 0, fn = 0;
    if (!player || !SafeGet(player, &vt) || !vt || !SafeGet(vt + kSpEffectVfunc, &fn) || !InImage(fn)) {
        return false;
    }
    reinterpret_cast<SpEffectFn>(fn)(player, kInsightSpEffect, player, 0, 0, 0, 0, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
    return true;
}

void OnGuestRedirect(TravelKind kind, GuestRedirect how, std::uint32_t lamp) {
    Log("guest %s redirected to %s (lamp %d); the bell brings it back", TravelKindName(kind), GuestRedirectName(how),
        static_cast<int>(lamp));
    if (g_cfg.refill) {
        std::lock_guard<std::mutex> lk(g_guest_mu);
        g_refill.Request(RefillReason::GuestRespawn, true, Now());
    }
}

std::uint64_t MembersKey(party::PartyLink* link) {
    std::uint64_t h = 1469598103934665603ull;
    for (const party::RosterEntry& e : link->roster()) {
        if (!e.connected) {
            continue;
        }
        h = (h ^ static_cast<std::uint64_t>(e.slot + 1)) * 1099511628211ull;
        for (char c : e.name) {
            h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ull;
        }
    }
    return h;
}

void HostTick(party::PartyLink* link, double now) {
    if (g_cfg.respawn && link && now - g_last_lamp_read >= 1.0) {
        g_last_lamp_read = now;
        const u32 lamp = OwnLastLamp();
        if (g_lamp_announcer.ShouldSend(lamp, MembersKey(link))) {
            PhantomEvent e;
            e.kind = PhantomEventKind::Lamp;
            e.seq = NextSeq();
            e.lamp = lamp;
            Queue(e);
        }
    }
    std::deque<PhantomEvent> out;
    {
        std::lock_guard<std::mutex> lk(g_host_mu);
        out.swap(g_outbox);
    }
    if (!link) {
        return; // nobody to tell
    }
    for (const PhantomEvent& e : out) {
        link->send_event(party::kBroadcast, kPhantomEventName, PhantomEventToJsonText(e));
        Log("sent %s #%llu (lamp %d, Insight %d, event %u)", PhantomEventKindName(e.kind), ull(e.seq),
            static_cast<int>(e.lamp), e.insight, e.source_event);
    }
}

void GuestTick(const GameSnapshot& g, double now) {
    if (g.session_role == RoleClient) {
        g_last_client = now;
    }
    ReplayState s;
    s.world_up = g.world_up;
    s.loading = g.loading;
    s.session_role = g.session_role;
    u8 requested = 1;
    const u64 wts = Wts();
    s.transition_requested = !wts || !SafeGet(wts + kWtsRequested, &requested) || requested != 0;
    bool refill_now, insight_now;
    RefillReason why;
    {
        std::lock_guard<std::mutex> lk(g_guest_mu);
        refill_now = g_refill.Step(s, now);
        why = g_refill.Reason();
        insight_now = g_drip.Step(ReplayAllowedNow(s));
    }
    if (refill_now) {
        Refill(why);
    }
    if (insight_now) {
        const int before = g.insight;
        const bool ok = ApplyInsightSpEffect();
        Log("boss Insight: SpEffect %u %s (Insight was %d)", kInsightSpEffect, ok ? "applied" : "NOT applied (no player)",
            before);
    }
}

void ProbeTick(const GameSnapshot& g, double now) {
    if (!g.world_up || g.loading) {
        g_world_seen = false;
        return;
    }
    if (!g_world_seen) {
        g_world_seen = true;
        g_world_at = now;
    }
    if (g_probe_done || now - g_world_at < 5.0) {
        return;
    }
    g_probe_done = true;
    if (g_cfg.probe_insight) {
        const int before = g.insight;
        const bool ok = ApplyInsightSpEffect();
        Log("probe R5: SpEffect 4680 %s; Insight before %d (read again in the next state lines)", ok ? "applied" : "NOT applied",
            before);
    }
    if (g_cfg.probe_refill) {
        Refill(RefillReason::Probe);
    }
}

} // namespace

void PhantomInit() {
    if (g_cfg.init) {
        return;
    }
    g_cfg.init = true;
    g_cfg.respawn = !EnvOff("BB_PARTY_GUEST_RESPAWN");
    g_cfg.refill = !EnvOff("BB_PARTY_GUEST_REFILL");
    g_cfg.open_world = !EnvOff("BB_PARTY_OPEN_WORLD");
    g_cfg.insight = InsightModeFromString(std::getenv("BB_PARTY_GUEST_INSIGHT"));
    if (const char* p = std::getenv("BB_PARTY_PHANTOM_PROBE")) {
        g_cfg.probe_insight = std::strstr(p, "insight") != nullptr;
        g_cfg.probe_refill = std::strstr(p, "refill") != nullptr;
    }
    SetGuestDeathRedirectEnabled(g_cfg.respawn);
    SetGuestRedirectCallback(&OnGuestRedirect);
    Log("guest death -> host lamp %s, refill %s, open world %s, boss Insight %s%s%s", g_cfg.respawn ? "on" : "off",
        g_cfg.refill ? "on" : "off", g_cfg.open_world ? (TravelEnabled() ? "on" : "on (inactive: no B1 travel)") : "off",
        InsightModeName(g_cfg.insight), g_cfg.probe_insight ? ", probe insight" : "",
        g_cfg.probe_refill ? ", probe refill" : "");
}

bool PhantomWantsEmevd() {
    return PartyMode() && InsightModeFromString(std::getenv("BB_PARTY_GUEST_INSIGHT")) != InsightMode::Off;
}

std::vector<EmevdSkipRule> PhantomSkipRules() {
    if (!PartyMode() || EnvOff("BB_PARTY_OPEN_WORLD") || EnvOff("BB_PARTY_TRAVEL")) {
        return {};
    }
    return ConfinementWallRules();
}

void PhantomEmevdInstruction(std::int32_t event, std::int32_t bank, std::int32_t id, const std::uint8_t* args,
                             std::size_t size) {
    if (PartyDirector::Get().Role() != PartyRole::Host || g_cfg.insight == InsightMode::Off) {
        return;
    }
    const double now = Now();
    if (bank == 2003 && (id == 12 || id == 15 || id == 53)) {
        std::lock_guard<std::mutex> lk(g_host_mu);
        g_tracker.OnBossDefeat(event, now);
        return;
    }
    if (bank != 2000 || id != 0 || !args || size < 12) {
        return;
    }
    std::int32_t target = 0, n = 0;
    std::memcpy(&target, args + 4, 4);
    std::memcpy(&n, args + 8, 4);
    std::optional<int> grant;
    {
        std::lock_guard<std::mutex> lk(g_host_mu);
        grant = g_tracker.OnInitializeEvent(event, target, n, now);
    }
    if (!grant) {
        return;
    }
    PhantomEvent e;
    e.kind = PhantomEventKind::Insight;
    e.seq = NextSeq();
    e.insight = *grant;
    e.source_event = static_cast<u32>(event);
    Queue(e);
    Log("host boss kill: event %d starts 9350 with %d Insight; telling the guests", event, *grant);
}

void PhantomNoteHostTravel(const TravelIntent& t) {
    if (!g_cfg.init || !g_cfg.refill || !RestedFromTravel(t)) {
        return;
    }
    PhantomEvent e;
    e.kind = PhantomEventKind::Rested;
    e.seq = NextSeq();
    Queue(e);
}

void PhantomTick(party::PartyLink* link) {
    if (!g_cfg.init) {
        return;
    }
    const PartyRole role = PartyDirector::Get().Role();
    const double now = Now();
    const GameSnapshot g = ReadGameState(false);
    if (role == PartyRole::Host) {
        HostTick(link, now);
    } else if (role == PartyRole::Guest) {
        GuestTick(g, now);
    }
    if (g_cfg.probe_insight || g_cfg.probe_refill) {
        ProbeTick(g, now);
    }
}

void PhantomOnEvent(const std::string& body) {
    PhantomEvent e;
    std::string err;
    if (!PhantomEventFromJsonText(body, &e, &err)) {
        Log("bad phantom event: %s", err.c_str());
        return;
    }
    const double now = Now();
    switch (e.kind) {
    case PhantomEventKind::Lamp:
        // A lamp id the guest's game will warp to on death (0x13CDF30): a ReturnPointParam row.
        if (!SanitizePeerPhantom(e, &err)) {
            Log("host %s; ignored", err.c_str());
            break;
        }
        if (g_cfg.respawn) {
            SetGuestDeathRedirect(e.lamp);
        }
        break;
    case PhantomEventKind::Rested: {
        if (!g_cfg.refill) {
            break;
        }
        std::lock_guard<std::mutex> lk(g_guest_mu);
        if (e.seq <= g_last_rested) {
            break;
        }
        g_last_rested = e.seq;
        // The guest follows the host's warp (B1): refill after that load.
        g_refill.Request(RefillReason::HostRested, TravelEnabled(), now);
        Log("host rested #%llu: refill after %s", ull(e.seq), TravelEnabled() ? "the follow load" : "a steady moment");
        break;
    }
    case PhantomEventKind::Insight: {
        const bool present = now - g_last_client.load() <= kParticipation;
        const int grant = GuestInsightGrant(e.insight, g_cfg.insight);
        std::lock_guard<std::mutex> lk(g_guest_mu);
        if (e.seq <= g_last_insight) {
            break;
        }
        g_last_insight = e.seq;
        // A boss gives a few Insight, and bosses do not fall twice a minute: a host's stream of
        // insight events is capped (bbport security pass).
        if (now - g_last_insight_grant < kInsightMinInterval) {
            Log("host boss kill (event %u): another Insight event %.0f s after the last; ignored", e.source_event,
                now - g_last_insight_grant);
            break;
        }
        if (!present) {
            Log("host boss kill (event %u, %d Insight): this guest was not in the host's world; nothing", e.source_event,
                e.insight);
            break;
        }
        g_last_insight_grant = now;
        g_drip.Add(grant > kMaxInsightPerEvent ? kMaxInsightPerEvent : grant);
        Log("host boss kill (event %u, %d Insight): +%d here, %d with the native cooperator +1 (mode %s)",
            e.source_event, e.insight, grant, grant + 1, InsightModeName(g_cfg.insight));
        break;
    }
    case PhantomEventKind::Unknown:
        break;
    }
}

#endif // BB_PARTY_PHANTOM_NO_GAME

} // namespace coop

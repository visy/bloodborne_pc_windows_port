// SPDX-License-Identifier: GPL-3.0-or-later
// The world warper (see world_warper.h; route, options and progress file in world_warper_rules.h).
#include "world_warper.h"

#include "bbport_compile_progress.h"
#include "coop_hooks.h"
#include "game_state.h"
#include "lamp_travel.h"
#include "party_async_io.h"
#include "party_balance.h"
#include "party_director.h"
#include "party_enemy_census.h"
#include "party_ids.h"
#include "world_warper_rules.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

extern "C" void runtime_pad_director_press(std::uint32_t buttons);        // src/runtime_pad.c
extern "C" void runtime_pad_director_sticks(int lx, int ly, int rx, int ry); // src/runtime_pad.c
[[noreturn]] void BbHardExit(int code);                                       // shim/bbgpu.cpp

namespace coop::warper {

namespace {

using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i32 = std::int32_t;
namespace cp = BbCompileProgress;

constexpr u64 kLampWarp = 0x13cdf30;      // void (u32 ReturnPointParam id): lamp / headstone warp
constexpr u64 kFlipperUpdate = 0x2034770; // main-thread tick (as party_director / lamp travel)
constexpr u64 kForceDead = 0x13320c0;     // Lua ForceDead(ctx unused, entity id)
constexpr u64 kChrDeath = 0x18bf9a0;      // the death path on a chr (what ForceDead calls)
constexpr u32 kPadCross = 0x4000;

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[1400];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    coop::asyncio::Print(std::string("World warper: ") + line + "\n");
}

double Now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

template <class T>
T Field(u64 base, u64 off, T fallback = T{}) {
    T v{};
    return base && SafeGet(base + off, &v) ? v : fallback;
}

/// The counters an area / action is measured with (all monotonic).
struct Counters {
    u64 created = 0, hitches = 0, hitch_us = 0, skipped = 0, waits = 0, compile_us = 0, flips = 0;
    u64 shader_hitches = 0, shader_hitch_us = 0;
    static Counters Now() {
        Counters c;
        c.created = cp::created_total.load();
        c.hitches = cp::hitch_frames.load();
        c.hitch_us = cp::hitch_us_total.load();
        c.skipped = cp::skipped_total.load();
        c.waits = cp::waits_total.load();
        c.compile_us = cp::gpu_thread_compile_us.load();
        c.flips = cp::flips_total.load();
        c.shader_hitches = cp::compile_hitch_frames.load();
        c.shader_hitch_us = cp::compile_hitch_us_total.load();
        return c;
    }
    Counters operator-(const Counters& o) const {
        Counters d;
        d.created = created - o.created, d.hitches = hitches - o.hitches, d.hitch_us = hitch_us - o.hitch_us;
        d.skipped = skipped - o.skipped, d.waits = waits - o.waits, d.compile_us = compile_us - o.compile_us;
        d.flips = flips - o.flips, d.shader_hitches = shader_hitches - o.shader_hitches;
        d.shader_hitch_us = shader_hitch_us - o.shader_hitch_us;
        return d;
    }
    Counters& operator+=(const Counters& o) {
        created += o.created, hitches += o.hitches, hitch_us += o.hitch_us, skipped += o.skipped;
        waits += o.waits, compile_us += o.compile_us, flips += o.flips;
        shader_hitches += o.shader_hitches, shader_hitch_us += o.shader_hitch_us;
        return *this;
    }
};

enum class Phase : std::uint8_t { ReplayBefore, Tour, ReplayAfter };
const char* PhaseName(Phase p) {
    return p == Phase::Tour ? "tour" : p == Phase::ReplayBefore ? "replay (before)" : "replay (after)";
}

enum class Stage : std::uint8_t { Title, Settle, Next, WaitLoad, Arrive, Routine, Finish, Done };

struct EnemyTarget {
    u32 entity = 0;
    u64 chr = 0;
};

struct State {
    Options opt;
    bool on = false;
    std::string state_path;
    std::vector<Phase> phases;
    std::size_t phase_i = 0;
    std::vector<u32> route;       // the current phase's rows
    std::size_t idx = 0;          // next row to look at
    Progress progress;            // the tour's (from the file)
    Stage stage = Stage::Title;
    double at = 0, stage_at = 0, steady_since = 0, title_press_at = 0, start_at = 0;
    int title_presses = 0;
    u32 rp = 0;                   // the row being visited
    std::size_t visited = 0;      // rows done in this phase (this session)
    bool load_seen = false;
    bool interrupted = false;
    double arrive_at = 0, routine_at = 0, warp_at = 0;
    Counters c_warp, c_arrive, c_step, c_phase, c_session0;
    std::map<std::string, u64> action_new;  // the area's new pipelines per action
    std::map<std::string, u64> tour_action_new;
    std::vector<PadStep> steps;
    std::size_t step_i = 0;
    double step_end = 0;
    std::vector<u32> maps_seen;
    bool loadout_done = false;
    // enemies
    int enemy_round = 0;
    std::vector<u32> enemies_used;
    EnemyTarget target;
    double enemy_kill_at = 0, enemy_next_at = 0;
    bool enemy_pending = false;
    // replay results: row -> counters
    std::map<u32, Counters> replay_before, replay_after;
    std::size_t done_rows = 0, skipped_rows = 0;
    u64 tour_created = 0;
} g;

std::mutex g_status_mu;
std::string g_status;  // under g_status_mu
std::atomic<bool> g_active{false};

void SetStatus(const std::string& s) {
    std::lock_guard<std::mutex> lk(g_status_mu);
    g_status = s;
}

void AppendState(const std::string& line) {
    if (g.state_path.empty()) return;
    if (FILE* f = std::fopen(g.state_path.c_str(), "a")) {
        std::fprintf(f, "%s\n", line.c_str());
        std::fflush(f);
        std::fclose(f);
    }
}

std::string ReadFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string RowName(u32 rp) {
    const char* area = AreaName(rp);
    char b[128];
    std::snprintf(b, sizeof b, "%s%s%u", area, area[0] ? ", row " : "row ", rp);
    return b;
}

void Release() {
    runtime_pad_director_press(0);
    runtime_pad_director_sticks(-1, -1, -1, -1);
}

void Apply(const PadStep& s) {
    runtime_pad_director_press(s.buttons);
    runtime_pad_director_sticks(s.lx, s.ly, s.rx, s.ry);
}

bool PlayerPos(u64 player, float out[3]) {
    float p[4] = {};
    if (!player || !SafeRead(player + 0xc0, p, sizeof p)) return false;
    out[0] = p[0], out[1] = p[1], out[2] = p[2];
    return true;
}

/// Places the player (the chr's own warp, vtable +0x5F8, as the lamp travel test's WalkTo).
bool PlacePlayer(u64 player, const float pos3[3], float yaw) {
    alignas(16) float pos[4] = {pos3[0], pos3[1], pos3[2], 1.0f};
    alignas(16) float rot[4] = {0, yaw, 0, 0};
    u64 vtbl = 0, fn = 0;
    if (!player || !SafeGet(player, &vtbl) || !SafeGet(vtbl + 0x5f8, &fn) || !fn) return false;
    reinterpret_cast<void(BB_COOP_SYSV*)(u64, const float*, const float*, u64)>(fn)(player, pos, rot, 0);
    return true;
}

/// Back to the row's spot (its revive point), facing its yaw.
void BackToSpot(u64 player, u32 rp) {
    float sp[4];
    if (ids::ReturnSpot(rp, sp)) PlacePlayer(player, sp, sp[3]);
}

/// Keeps the player alive: HP back to its maximum (PlayerIns +0x3B0 -> +0x20: HP +0xF8, max +0xFC).
void KeepAlive(u64 player) {
    const u64 dm = Field<u64>(Field<u64>(player, 0x3b0), 0x20);
    if (!dm) return;
    const int hp = Field<int>(dm, 0xf8, -1), max = Field<int>(dm, 0xfc, -1);
    if (hp > 0 && max > 0 && hp < max) SafeWrite(dm + 0xf8, &max, 4);
}

std::string Counts(const Counters& c) {
    char b[300];
    std::snprintf(b, sizeof b, "%llu new pipelines, %llu hitches over 50 ms (%.0f ms; %llu of them, %.0f ms, with "
                  "a pipeline made in the frame), %llu draws skipped, %llu GPU-thread waits, %.0f ms GPU-thread compiles",
                  static_cast<unsigned long long>(c.created), static_cast<unsigned long long>(c.hitches),
                  c.hitch_us / 1e3, static_cast<unsigned long long>(c.shader_hitches), c.shader_hitch_us / 1e3,
                  static_cast<unsigned long long>(c.skipped),
                  static_cast<unsigned long long>(c.waits), c.compile_us / 1e3);
    return b;
}

void UpdateStatus(const char* doing) {
    const Phase ph = g.phases.empty() ? Phase::Tour : g.phases[g.phase_i];
    const std::size_t total = g.route.size();
    std::size_t n = ph == Phase::Tour ? HandledCount(g.route, g.progress) : g.visited;
    if (g.rp && n < total) ++n;  // the row in progress
    const Counters here = Counters::Now() - g.c_warp;
    char b[600];
    std::snprintf(b, sizeof b,
                  "World warper: %s %zu / %zu\n%s\n%s - %llu new pipelines here, %llu this session",
                  PhaseName(ph), n, total, g.rp ? RowName(g.rp).c_str() : "-", doing,
                  static_cast<unsigned long long>(g.rp ? here.created : 0),
                  static_cast<unsigned long long>(cp::created_total.load() - g.c_session0.created));
    SetStatus(b);
}

bool Steady(const GameSnapshot& s) {
    return s.world_up && !s.loading;
}

std::vector<u32> AllRows() {
    return std::vector<u32>(std::begin(ids::kPartyReturnPoints), std::end(ids::kPartyReturnPoints));
}

void BeginPhase(double now) {
    const Phase ph = g.phases[g.phase_i];
    g.idx = 0;
    g.visited = 0;
    g.c_phase = Counters::Now();
    auto spot = [](u32 rp) {
        float sp[4];
        return ids::ReturnSpot(rp, sp);
    };
    if (ph == Phase::Tour) {
        g.route = BuildRoute(AllRows(), spot, g.opt);
        g.progress = ParseProgress(ReadFile(g.state_path));
        const std::size_t handled = HandledCount(g.route, g.progress);
        Log("tour: %zu rows (every ReturnPointParam row with a spot, Chalice Dungeons left out)%s; %zu already "
            "handled (progress file %s)", g.route.size(), g.opt.lamps.empty() ? "" : " from lamps=", handled,
            g.state_path.empty() ? "(none)" : g.state_path.c_str());
        for (u32 rp : g.route) {
            if (const char* why = SkipReason(g.progress, rp)) {
                if (std::strcmp(why, "done") != 0) Log("tour: row %u not visited again (%s)", rp, why);
            }
        }
    } else {
        Options ro;
        ro.lamps = g.opt.replay.empty() ? std::vector<u32>(std::begin(kDefaultReplay), std::end(kDefaultReplay))
                                        : g.opt.replay;
        g.route = BuildRoute(AllRows(), spot, ro);
        Log("%s: %zu rows (look + walk each, the stutter measurement)", PhaseName(ph), g.route.size());
    }
    g.stage = Stage::Next;
    g.at = now;
}

void StartSteps(std::vector<PadStep> steps, double now) {
    g.steps = std::move(steps);
    g.step_i = 0;
    g.step_end = 0;
    g.c_step = Counters::Now();
    (void)now;
}

/// Runs the scripted steps; true once all ran.
bool RunSteps(double now) {
    if (g.step_i >= g.steps.size()) return true;
    if (g.step_end == 0) {
        Apply(g.steps[g.step_i]);
        g.step_end = now + g.steps[g.step_i].seconds;
        return false;
    }
    if (now < g.step_end) return false;
    const Counters c = Counters::Now();
    g.action_new[g.steps[g.step_i].what] += c.created - g.c_step.created;
    g.c_step = c;
    ++g.step_i;
    g.step_end = 0;
    if (g.step_i >= g.steps.size()) {
        Release();
        return true;
    }
    Apply(g.steps[g.step_i]);
    g.step_end = now + g.steps[g.step_i].seconds;
    return false;
}

/// The next enemy near the row's spot: alive, not a boss, within 60 m, not used yet.
bool PickEnemy(u32 rp, EnemyTarget* out, float* dist) {
    float sp[4];
    if (!ids::ReturnSpot(rp, sp)) return false;
    const auto list = balance::LoadedEnemies();
    float best = 60.0f * 60.0f;
    const EnemyKillCandidate* pick = nullptr;
    for (const auto& e : list) {
        if (!e.alive || !e.have_pos || !e.chr || EnemyIsBossEntity(e.entity)) continue;
        if (std::find(g.enemies_used.begin(), g.enemies_used.end(), e.id) != g.enemies_used.end()) continue;
        const float dx = e.pos[0] - sp[0], dy = e.pos[1] - sp[1], dz = e.pos[2] - sp[2];
        const float d = dx * dx + dy * dy + dz * dz;
        if (d < best) {
            best = d;
            pick = &e;
        }
    }
    if (!pick) return false;
    g.enemies_used.push_back(pick->id);
    out->entity = pick->entity;
    out->chr = pick->chr;
    *dist = std::sqrt(best);
    return true;
}

/// Stands the player 2.5 m from the enemy, facing it.
bool GoToEnemy(u64 player, const EnemyTarget& t) {
    float e[3], me[3];
    if (!balance::ChrPosition(t.chr, e) || !PlayerPos(player, me)) return false;
    float dx = me[0] - e[0], dz = me[2] - e[2];
    const float h = std::sqrt(dx * dx + dz * dz);
    if (h < 0.01f) dx = 0, dz = 1;
    else dx /= h, dz /= h;
    const float pos[3] = {e[0] + dx * 2.5f, e[1] + 0.2f, e[2] + dz * 2.5f};
    const float yaw = std::atan2(e[0] - pos[0], e[2] - pos[2]);
    return PlacePlayer(player, pos, yaw);
}

/// The kill of the director's host_kill test step: ForceDead by id, the chr's death path, HP 0.
void KillEnemy(const EnemyTarget& t) {
    if (balance::EnemyHp(t.chr) <= 0) return;
    if (t.entity) reinterpret_cast<void(BB_COOP_SYSV*)(u64, u32)>(Guest(kForceDead))(0, t.entity);
    if (balance::EnemyHp(t.chr) > 0) {
        std::uint8_t b = 0;
        if (SafeGet(t.chr + 0x1e3, &b)) {
            b = std::uint8_t(b & 0xdf);
            SafeWrite(t.chr + 0x1e3, &b, 1);
        }
        reinterpret_cast<void(BB_COOP_SYSV*)(u64)>(Guest(kChrDeath))(t.chr);
    }
    if (balance::EnemyHp(t.chr) > 0) {
        const int zero = 0;
        const u64 dm = Field<u64>(Field<u64>(t.chr, 0x3b0), 0x20);
        if (dm) SafeWrite(dm + 0xf8, &zero, 4);
    }
}

std::string ActionList(const std::map<std::string, u64>& m) {
    std::vector<std::pair<std::string, u64>> v(m.begin(), m.end());
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::string out;
    for (const auto& [k, n] : v) {
        if (!n) continue;
        char b[96];
        std::snprintf(b, sizeof b, "%s%s %llu", out.empty() ? "" : ", ", k.c_str(), static_cast<unsigned long long>(n));
        out += b;
    }
    return out.empty() ? "none" : out;
}

void FinishRow(const GameSnapshot& s, double now, const char* note) {
    Release();
    const Phase ph = g.phases[g.phase_i];
    const Counters all = Counters::Now() - g.c_warp;
    const Counters here = Counters::Now() - g.c_arrive;  // in the world (no loading screen)
    const double worst = cp::TakeWorstFrameUs() / 1e3;
    ++g.visited;
    if (ph == Phase::Tour) {
        g.tour_created += all.created;
        for (const auto& [k, n] : g.action_new) g.tour_action_new[k] += n;
        AppendState(DoneLine(g.rp, all.created, here.hitches, worst, now - g.warp_at, MapName(s.map_id)));
        g.progress.done.insert(g.rp);
        ++g.done_rows;
        const std::size_t n = HandledCount(g.route, g.progress);
        Log("[%zu/%zu] %s (%s)%s: +%llu new pipelines (in the world %llu: %s); %llu hitches (%llu with a pipeline made), worst frame %.0f ms; "
            "%.0f s; %llu new this session", n, g.route.size(), RowName(g.rp).c_str(), MapName(s.map_id).c_str(), note,
            static_cast<unsigned long long>(all.created), static_cast<unsigned long long>(here.created),
            ActionList(g.action_new).c_str(), static_cast<unsigned long long>(here.hitches),
            static_cast<unsigned long long>(here.shader_hitches), worst, now - g.arrive_at,
            static_cast<unsigned long long>(cp::created_total.load() - g.c_session0.created));
    } else {
        (ph == Phase::ReplayBefore ? g.replay_before : g.replay_after)[g.rp] = here;
        Log("%s %zu/%zu %s: %s; worst frame %.0f ms%s", PhaseName(ph), g.visited, g.route.size(),
            RowName(g.rp).c_str(), Counts(here).c_str(), worst, note);
    }
    g.rp = 0;
    g.stage = Stage::Next;
    g.at = now + 0.5;
}

std::vector<PadStep> RoutineSteps(const GameSnapshot& s) {
    const Phase ph = g.phases[g.phase_i];
    std::vector<PadStep> v = LookSteps();
    const auto walk = WalkSteps();
    v.insert(v.end(), walk.begin(), walk.end());
    if (ph != Phase::Tour || !g.opt.act) return v;
    const u32 map = s.map_id & 0xffff0000u;
    const bool first_in_map = std::find(g.maps_seen.begin(), g.maps_seen.end(), map) == g.maps_seen.end();
    if (first_in_map) {
        g.maps_seen.push_back(map);
        const auto act = ActionSteps();
        v.insert(v.end(), act.begin(), act.end());
    }
    if (!g.loadout_done) {
        g.loadout_done = true;
        const auto lo = LoadoutSteps();
        v.insert(v.end(), lo.begin(), lo.end());
    }
    return v;
}

void Finish(double now) {
    Release();
    const Counters total = Counters::Now() - g.c_session0;
    Log("done: %zu areas visited, %zu skipped this session; %s; %.1f min", g.done_rows, g.skipped_rows,
        Counts(total).c_str(), (now - g.start_at) / 60.0);
    if (!g.tour_action_new.empty()) Log("new pipelines per action (tour): %s", ActionList(g.tour_action_new).c_str());
    if (!g.replay_before.empty() && !g.replay_after.empty()) {
        Counters b, a;
        for (const auto& [rp, c] : g.replay_before) b += c;
        for (const auto& [rp, c] : g.replay_after) a += c;
        Log("stutter check: replay before the tour: %s", Counts(b).c_str());
        Log("stutter check: replay after the tour:  %s", Counts(a).c_str());
    }
    const bool tour = std::find(g.phases.begin(), g.phases.end(), Phase::Tour) != g.phases.end();
    if (tour) AppendState("finished");
    SetStatus("World warper: finished\nSaving the shader cache...\n");
    g.stage = Stage::Done;
    g.at = now + 2.0;
}

void TickImpl(double now) {
    const GameSnapshot s = ReadGameState(false);
    if (s.player && Steady(s) && g.stage >= Stage::Next && g.stage <= Stage::Routine) KeepAlive(s.player);
    switch (g.stage) {
    case Stage::Title:
        if (Steady(s)) {
            g.stage = Stage::Settle;
            g.steady_since = now;
            Log("in the world; starting in a few seconds");
            SetStatus("World warper\nIn the world\nStarting...");
            return;
        }
        if (g.title_press_at > 0 && now - g.title_press_at >= 0.12) {
            runtime_pad_director_press(0);
            g.title_press_at = 0;
        }
        if (!s.world_up && now - g.at >= 2.5 && now - g.start_at > 4.0) {
            // The title: Cross until the save is continued (as the lamp travel test).
            g.at = now;
            runtime_pad_director_press(kPadCross);
            g.title_press_at = now;
            if (++g.title_presses <= 3 || g.title_presses % 20 == 0) Log("title: Cross #%d", g.title_presses);
            SetStatus("World warper\nContinuing the save...\n");
        }
        if (now - g.start_at > 300.0) {
            Log("FAILED: no world within 300 s of the start (no save to continue?)");
            SetStatus("World warper: FAILED\nNo world: is there a save to continue?\n");
            g.stage = Stage::Done;
            g.at = now + 3.0;
        }
        return;
    case Stage::Settle:
        if (!Steady(s)) {
            g.steady_since = now;
            return;
        }
        if (now - g.steady_since < 4.0) return;
        BeginPhase(now);
        return;
    case Stage::Next: {
        if (now < g.at) return;
        if (!Steady(s)) return;  // a load (a death) first
        const Phase ph = g.phases[g.phase_i];
        g.idx = ph == Phase::Tour ? NextIndex(g.route, g.progress, g.idx) : g.idx;
        if (g.idx >= g.route.size()) {
            Log("%s finished: %s", PhaseName(ph), Counts(Counters::Now() - g.c_phase).c_str());
            if (++g.phase_i < g.phases.size()) {
                BeginPhase(now);
            } else {
                Finish(now);
            }
            return;
        }
        g.rp = g.route[g.idx++];
        if (ph == Phase::Tour) {
            AppendState("start " + std::to_string(g.rp));
            ++g.progress.starts[g.rp];
        }
        g.c_warp = Counters::Now();
        g.warp_at = now;
        cp::TakeWorstFrameUs();
        g.action_new.clear();
        g.enemies_used.clear();
        g.enemy_round = 0;
        g.load_seen = false;
        g.interrupted = false;
        Log("%s: warping to %s (%s)", PhaseName(ph), RowName(g.rp).c_str(), MapName(MapOfRow(g.rp)).c_str());
        reinterpret_cast<void(BB_COOP_SYSV*)(u32)>(Guest(kLampWarp))(g.rp);
        g.stage = Stage::WaitLoad;
        g.stage_at = now;
        UpdateStatus("loading");
        return;
    }
    case Stage::WaitLoad:
        if (!Steady(s)) {
            g.load_seen = true;
            g.stage = Stage::Arrive;
            g.stage_at = now;
            g.steady_since = 0;
            return;
        }
        if (now - g.stage_at > 25.0) {
            Log("row %u: no loading screen within 25 s of the warp: skipped", g.rp);
            if (g.phases[g.phase_i] == Phase::Tour) {
                AppendState("skip " + std::to_string(g.rp) + " no load");
                g.progress.skipped.insert(g.rp);
            }
            ++g.skipped_rows;
            g.rp = 0;
            g.stage = Stage::Next;
            g.at = now;
        }
        return;
    case Stage::Arrive:
        if (!Steady(s)) {
            g.steady_since = 0;
            if (now - g.stage_at > 240.0) {
                // Stuck in the load: end the game; the launcher starts it again and the progress
                // file's "start" lines skip this row after kMaxAttempts.
                Log("FAILED: row %u: still loading 240 s after the warp; ending the game (the next start resumes)",
                    g.rp);
                Release();
                BbHardExit(3);
            }
            return;
        }
        if (g.steady_since == 0) {
            g.steady_since = now;
            g.arrive_at = now;
            g.c_arrive = Counters::Now();
            UpdateStatus("arrived");
        }
        if (now - g.steady_since < 2.5) return;
        if (g.interrupted) {
            // Back after a load in the middle of the routine (a death, a fall, an event warp): the
            // row is done; running it again from the respawn could repeat the same fall.
            FinishRow(s, now, " (interrupted by a load)");
            return;
        }
        StartSteps(RoutineSteps(s), now);
        g.stage = Stage::Routine;
        g.routine_at = now;
        g.enemy_pending = false;
        g.enemy_next_at = 0;
        UpdateStatus("looking round");
        return;
    case Stage::Routine: {
        if (!Steady(s)) {
            // A load in the middle (the player died, or an event moved it): wait, then go on.
            Release();
            g.interrupted = true;
            g.stage = Stage::Arrive;
            g.stage_at = now;
            g.steady_since = 0;
            Log("row %u: a load during the routine (step %zu of %zu); finishing the row after it", g.rp, g.step_i,
                g.steps.size());
            g.steps.clear();
            return;
        }
        if (g.interrupted && g.steps.empty()) {
            FinishRow(s, now, " (interrupted by a load)");
            return;
        }
        if (!RunSteps(now)) {
            static double last = 0;
            if (now - last > 1.0 && g.step_i < g.steps.size()) {
                last = now;
                UpdateStatus(g.steps[g.step_i].what);
            }
            return;
        }
        // Enemies (the tour only): the nearest two, fought briefly, then killed.
        const Phase ph = g.phases[g.phase_i];
        if (ph == Phase::Tour && g.opt.enemies && g.enemy_round < 2) {
            if (!g.enemy_pending) {
                float d = 0;
                EnemyTarget t;
                if (!PickEnemy(g.rp, &t, &d)) {
                    if (g.enemy_round == 0) Log("row %u: no living enemy within 60 m", g.rp);
                    g.enemy_round = 2;
                } else {
                    g.target = t;
                    g.enemy_pending = true;
                    ++g.enemy_round;
                    GoToEnemy(s.player, t);
                    Log("row %u: enemy %u, %.0f m from the spot (HP %d)", g.rp, t.entity, d, balance::EnemyHp(t.chr));
                    UpdateStatus("fighting");
                    std::vector<PadStep> fight = {
                        {0.6f, 0, -1, -1, -1, -1, "enemy"},     {0.15f, kR1, -1, -1, -1, -1, "enemy"},
                        {0.9f, 0, -1, -1, -1, -1, "enemy"},     {0.15f, kR1, -1, -1, -1, -1, "enemy"},
                        {1.2f, 0, -1, -1, -1, -1, "enemy"},     {0.15f, kL2, -1, -1, -1, -1, "enemy"},
                        {1.0f, 0, -1, -1, -1, -1, "enemy"},
                    };
                    StartSteps(fight, now);
                    g.enemy_kill_at = 0;
                }
                return;
            }
            if (g.enemy_kill_at == 0) {
                if (g.opt.kill) KillEnemy(g.target);
                g.enemy_kill_at = now;
                UpdateStatus("enemy killed");
                return;
            }
            if (now - g.enemy_kill_at < 3.0) return;  // the death animation and its effects
            const Counters c = Counters::Now();
            g.action_new["enemy"] += c.created - g.c_step.created;
            g.c_step = c;
            g.enemy_pending = false;
            return;
        }
        BackToSpot(s.player, g.rp);
        FinishRow(s, now, g.interrupted ? " (interrupted by a load)" : "");
        return;
    }
    case Stage::Finish:
        return;
    case Stage::Done:
        if (now < g.at) return;
        g.on = false;
        g_active = false;
        Log("ending the game (shader cache saved)");
        BbHardExit(0);
    }
}

BB_COOP_SYSV void OwnTickEntry(u64, u64, u64, u64, u64, u64) {
    Tick();
}

} // namespace

void Install(unsigned char* image, std::uint64_t size) {
    const char* env = std::getenv("BB_WORLD_WARPER");
    g.opt = ParseOptions(env);
    if (g.opt.mode == Mode::Off) return;
    if (PartyRequested()) {
        std::printf("World warper: off: party mode is on (the warper runs solo only)\n");
        return;
    }
    if (!HooksInit(image, size)) return;
    if (!MatchesOriginal(kLampWarp, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53}) ||
        !MatchesOriginal(kForceDead, {0x55, 0x48, 0x89, 0xe5})) {
        std::printf("World warper: NOT installed: the code differs from 1.09\n");
        return;
    }
    if (const char* p = std::getenv("BB_WORLD_WARPER_STATE"); p && p[0]) {
        g.state_path = p;
    } else if (const char* u = std::getenv("BB_GPU_USER_DIR"); u && u[0]) {
        g.state_path = std::string(u) + "/world_warper_state.txt";
    }
    switch (g.opt.mode) {
    case Mode::Replay: g.phases = {Phase::ReplayBefore}; break;
    case Mode::Measure: g.phases = {Phase::ReplayBefore, Phase::Tour, Phase::ReplayAfter}; break;
    default: g.phases = {Phase::Tour}; break;
    }
    if (!lamptravel::OwnsTick()) {
        HookPrologue(kFlipperUpdate,
                     {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec,
                      0x38},
                     &OwnTickEntry, "world warper main-thread tick");
    }
    g.on = true;
    g_active = true;
    SetStatus("World warper\nStarting...\n");
    std::printf("World warper: on (%s; actions %s, enemies %s; progress file %s)\n", env,
                g.opt.act ? "on" : "off", g.opt.enemies ? "on" : "off",
                g.state_path.empty() ? "(none)" : g.state_path.c_str());
}

void Tick() {
    if (!g.on) return;
    const double now = Now();
    if (g.start_at == 0) {
        g.start_at = now;
        g.c_session0 = Counters::Now();
    }
    TickImpl(now);
}

bool Active() {
    return g_active.load(std::memory_order_relaxed);
}

std::string StatusText() {
    std::lock_guard<std::mutex> lk(g_status_mu);
    return g_status;
}

} // namespace coop::warper

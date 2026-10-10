// SPDX-License-Identifier: GPL-3.0-or-later
// Party story, phase C4 (see party_story.h; the RE is docs/party/cutscenes_endings.md).
//
// Every site below was disassembled from smoketest/out/eboot.elf (1.09) and is compared byte for
// byte before anything is written. Offsets are ours (raw ELF VA). The game's code is System V:
// every call into it goes through a BB_COOP_SYSV pointer at Guest(off).
#include "party_story.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#ifndef BB_PARTY_STORY_NO_GAME
#include "coop_hooks.h"
#include "game_state.h"
#include "party_director.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#endif

namespace coop {

// ---------------------------------------------------------------------------------------------
// Pure part (unit-tested)
// ---------------------------------------------------------------------------------------------

namespace {

struct KindName {
    StoryKind kind;
    const char* name;
};
constexpr KindName kKindNames[] = {
    {StoryKind::Unknown, "unknown"},
    {StoryKind::Cutscene, "cutscene"},
    {StoryKind::Ending, "ending"},
    {StoryKind::TimeOfDay, "time_of_day"},
};

// remo/sAA_BB_1NNN.remobnd.dcx siblings (cutscenes_endings.md 4.2).
constexpr std::uint32_t kSexPairs[][2] = {
    {21000010, 21001010}, // A ending
    {21000030, 21001030}, // C ending
    {24010005, 24011005}, // character creation
    {28000040, 28001040}, // Lecture Building grab
    {33000000, 33001000}, // Patches push
};

struct StoryScene {
    std::uint32_t remo;
    std::uint32_t flags[2]; // 0 = none
};
// Scenes a guest replays in its own world when it missed them (4.3 / 4.6 / 3.1). Flags [data],
// set ON after the remo exactly like the event bodies that follow the 2002 instruction.
constexpr StoryScene kStoryScenes[] = {
    {24000020, {0, 0}},        // m24_00 12400750 day -> evening (tod 1)
    {24000030, {0, 0}},        // m24_00 12401803 Willem's memory, evening -> night (tod 2)
    {32000000, {70002802, 0}}, // m32 13201803 Rom -> Blood Moon (tod 3)
    {21000000, {12417810, 0}}, // m21 9401 first death -> Hunter's Dream
    {28000010, {9423, 0}},     // common 9422 first kidnap
    {22000030, {0, 0}},        // m22 carriage
    {22000040, {0, 0}},
    {26000000, {0, 0}},        // m26 12601854 Micolash post-fight
    {26000005, {0, 0}},
    {26000040, {0, 0}},        // m26 12600026 brain shutdown
    {28000040, {12800434, 0}}, // m28 12800431 Lecture Building grab
    {33000000, {0, 0}},        // m33 13300200 Patches push
    {34000040, {0, 0}},        // m34 13401800 Ludwig
    {36000010, {0, 0}},        // m36 13601803 Orphan / Kos
    {24000000, {12401000, 0}}, // m24_00 12405263 DLC entry grab (m34 13401000 consumes it)
};

constexpr int kTodTable[4] = {0, 4, 6, 7}; // 0x4925010

std::uint32_t BaseRemo(std::uint32_t id) {
    for (const auto& p : kSexPairs) {
        if (id == p[1]) {
            return p[0];
        }
    }
    return id;
}

} // namespace

const char* StoryKindName(StoryKind k) {
    for (const KindName& n : kKindNames) {
        if (n.kind == k) {
            return n.name;
        }
    }
    return "unknown";
}

StoryKind StoryKindFromName(const std::string& name) {
    for (const KindName& n : kKindNames) {
        if (name == n.name) {
            return n.kind;
        }
    }
    return StoryKind::Unknown;
}

std::uint32_t RemoForSex(std::uint32_t remo_id, bool sex_variant) {
    for (const auto& p : kSexPairs) {
        if (remo_id == p[0] || remo_id == p[1]) {
            return sex_variant ? p[1] : p[0];
        }
    }
    return remo_id;
}

int EndingTypeOfEvent(std::uint32_t event_id) {
    switch (event_id) {
    case 12100180: return 1;
    case 12100000: return 2;
    case 12100002: return 3;
    default: return 0;
    }
}

int EndingTypeOfRemo(std::uint32_t remo_id) {
    switch (BaseRemo(remo_id)) {
    case 21000010: return 1;
    case 21000020: return 2;
    case 21000030: return 3;
    default: return 0;
    }
}

std::vector<std::uint32_t> StoryFlagsForRemo(std::uint32_t remo_id) {
    const std::uint32_t base = BaseRemo(remo_id);
    std::vector<std::uint32_t> out;
    for (const StoryScene& s : kStoryScenes) {
        if (s.remo == base) {
            for (std::uint32_t f : s.flags) {
                if (f) {
                    out.push_back(f);
                }
            }
        }
    }
    return out;
}

bool ReplayWorthy(const StoryIntent& s) {
    if (s.force) {
        return true;
    }
    switch (s.kind) {
    case StoryKind::Ending:
    case StoryKind::TimeOfDay:
        return true;
    case StoryKind::Cutscene: {
        if (s.tod >= 0 || !s.flags.empty()) {
            return true;
        }
        const std::uint32_t base = BaseRemo(s.id);
        for (const StoryScene& sc : kStoryScenes) {
            if (sc.remo == base) {
                return true;
            }
        }
        return false;
    }
    default:
        return false;
    }
}

int TodFlagValue(int tod) {
    return tod >= 0 && tod < 4 ? kTodTable[tod] : -1;
}

int TodFromFlagValue(int value) {
    int tod = 0;
    for (int i = 0; i < 4; ++i) {
        if ((value & kTodTable[i]) == kTodTable[i]) {
            tod = i;
        }
    }
    return tod;
}

std::string DescribeStory(const StoryIntent& s) {
    char text[320];
    std::string flags;
    for (std::uint32_t f : s.flags) {
        flags += (flags.empty() ? "" : ",") + std::to_string(f);
    }
    std::snprintf(text, sizeof text, "#%llu %s %u (mode %u, 2002[%u], event %u, map %08x, tod %d, warp %d @%08x%s%s)%s",
                  static_cast<unsigned long long>(s.seq), StoryKindName(s.kind), s.id, s.mode, s.instr, s.event_id,
                  s.map, s.tod, static_cast<int>(s.warp_point), s.warp_map, flags.empty() ? "" : ", flags ",
                  flags.c_str(), s.force ? " [forced]" : "");
    return text;
}

json::Value StoryToJson(const StoryIntent& s) {
    json::Value v = json::Value::make_object();
    v.set("seq", json::hex(s.seq));
    v.set("kind", StoryKindName(s.kind));
    v.set("id", s.id);
    v.set("mode", s.mode);
    v.set("instr", s.instr);
    v.set("event", s.event_id);
    v.set("map", s.map);
    v.set("tod", s.tod);
    v.set("warp_point", s.warp_point);
    v.set("warp_map", s.warp_map);
    json::Value f = json::Value::make_array();
    for (std::uint32_t id : s.flags) {
        f.push(id);
    }
    v.set("flags", f);
    if (s.force) {
        v.set("force", true);
    }
    return v;
}

bool StoryFromJson(const json::Value& v, StoryIntent* out, std::string* error) {
    try {
        if (v.type != json::Value::Type::Object) {
            throw std::runtime_error("not an object");
        }
        StoryIntent s;
        s.seq = json::u64(v, "seq");
        s.kind = StoryKindFromName(json::str(v, "kind"));
        if (s.kind == StoryKind::Unknown) {
            throw std::runtime_error("kind: unknown");
        }
        s.id = json::u32(v, "id");
        s.mode = json::u32(v, "mode");
        s.instr = json::u32(v, "instr");
        s.event_id = json::u32(v, "event");
        s.map = json::u32(v, "map");
        const double tod = json::num(v, "tod");
        if (tod < -1 || tod > 3 || tod != static_cast<int>(tod)) {
            throw std::runtime_error("tod: out of range");
        }
        s.tod = static_cast<int>(tod);
        s.warp_point = json::u32(v, "warp_point");
        s.warp_map = json::u32(v, "warp_map");
        for (const json::Value& f : json::arr(v, "flags")) {
            s.flags.push_back(static_cast<std::uint32_t>(json::as_u64(f, "flags")));
        }
        if (const json::Value* f = v.find("force")) {
            s.force = f->type == json::Value::Type::Bool && f->boolean;
        }
        if (s.seq == 0) {
            throw std::runtime_error("seq 0");
        }
        if (s.kind == StoryKind::Ending && (s.id < 1 || s.id > 3)) {
            throw std::runtime_error("ending: type not 1..3");
        }
        if (s.kind == StoryKind::TimeOfDay && s.id > 3) {
            throw std::runtime_error("time_of_day: not 0..3");
        }
        *out = s;
        return true;
    } catch (const std::exception& e) {
        if (error) {
            *error = e.what();
        }
        return false;
    }
}

std::string StoryToJsonText(const StoryIntent& s) {
    return json::dump(StoryToJson(s), 0);
}

bool StoryFromJsonText(const std::string& text, StoryIntent* out, std::string* error) {
    json::Value v;
    std::string err;
    if (!json::parse(text, v, err)) {
        if (error) {
            *error = err;
        }
        return false;
    }
    return StoryFromJson(v, out, error);
}

const char* StoryActionName(StoryAction::Kind k) {
    switch (k) {
    case StoryAction::None: return "none";
    case StoryAction::PlayRemo: return "play remo 0x131A9F0";
    case StoryAction::SetFlags: return "set flags";
    case StoryAction::ApplyTod: return "time of day (0x1CECEB0 body)";
    case StoryAction::NgCycle: return "NG cycle +1 (2003[21])";
    case StoryAction::TravelDream: return "Hunter's Dream warp 0x13CDF30(2102961)";
    case StoryAction::ForceEnding: return "ending request (GSM+0x1550)";
    }
    return "?";
}

bool HostStoryQueue::Push(const StoryIntent& s) {
    if (s.seq <= last_seq_) {
        return false;
    }
    last_seq_ = s.seq;
    if (q_.size() >= kMax) {
        q_.pop_front();
    }
    q_.push_back(s);
    return true;
}

bool HostStoryQueue::Pop(StoryIntent* out) {
    if (q_.empty()) {
        return false;
    }
    *out = q_.front();
    q_.pop_front();
    return true;
}

// ---- GuestStory ----

bool GuestStory::Offer(const StoryIntent& s, bool in_host_world, double now) {
    if (s.seq <= last_seq_) {
        return false;
    }
    last_seq_ = s.seq;
    dirty_ = true;
    if (s.kind == StoryKind::Cutscene && in_host_world && mirror_enabled && EndingTypeOfRemo(s.id) == 0) {
        mirror_ = s;
        mirror_since_ = now;
    }
    if (s.kind == StoryKind::Ending) {
        mirror_.reset(); // the ending is replayed as a whole in the own world
        for (const StoryIntent& q : queue_) {
            if (q.kind == StoryKind::Ending && q.id == s.id) {
                return true; // the event and the flag poll both report it
            }
        }
    }
    if (ReplayWorthy(s) && !(s.kind == StoryKind::Cutscene && EndingTypeOfRemo(s.id) != 0)) {
        if (queue_.size() >= kMaxQueue) {
            queue_.pop_front();
        }
        queue_.push_back(s);
    }
    return true;
}

bool GuestStory::Busy() const {
    return mirror_.has_value() || !queue_.empty() || running_;
}

bool GuestStory::Seen(std::uint32_t remo_id) const {
    return seen_.count(BaseRemo(remo_id)) != 0;
}

bool GuestStory::TakeDirty() {
    const bool d = dirty_;
    dirty_ = false;
    return d;
}

void GuestStory::Build(const StoryIntent& s, bool mirror) {
    ops_.clear();
    cur_ = s;
    running_ = true;
    running_mirror_ = mirror;
    op_started_ = false;
    auto push = [this](Op op) { ops_.push_back(std::move(op)); };
    if (mirror) {
        push({Op::Play, s.id, 2});
        push({Op::WaitRemoStart});
        push({Op::WaitRemoEnd});
        return;
    }
    switch (s.kind) {
    case StoryKind::Cutscene: {
        if (!Seen(s.id) || s.force) {
            push({Op::Play, s.id, 0});
            push({Op::WaitRemoStart});
            push({Op::WaitRemoEnd});
        }
        if (s.tod >= 0) {
            Op t{Op::Tod};
            t.tod = s.tod;
            push(t);
        }
        std::vector<std::uint32_t> flags = s.flags;
        for (std::uint32_t f : StoryFlagsForRemo(s.id)) {
            if (std::find(flags.begin(), flags.end(), f) == flags.end()) {
                flags.push_back(f);
            }
        }
        if (!flags.empty()) {
            Op f{Op::Flags};
            f.flags = flags;
            push(f);
        }
        break;
    }
    case StoryKind::TimeOfDay: {
        Op t{Op::Tod};
        t.tod = static_cast<int>(s.id);
        push(t);
        break;
    }
    case StoryKind::Ending: {
        push({Op::TravelDream});
        push({Op::WaitArrive});
        auto flags = [&](std::vector<std::uint32_t> f, bool on) {
            Op op{Op::Flags};
            op.flags = std::move(f);
            op.on = on;
            push(op);
        };
        if (s.id == 1) {
            // m21 12100180 waits for 72100130 and does NG+1, the remo by sex, 6604, 21, 6600, 6603.
            flags({72100130}, true);
            Op e{Op::EndingCheck};
            e.ending = 1;
            push(e);
            break;
        }
        const bool c = s.id == 3;
        flags(c ? std::vector<std::uint32_t>{12101800, 12101850, 9180} : std::vector<std::uint32_t>{12101800, 9180},
              true);
        push({Op::Play, c ? 21000030u : 21000020u, 0});
        push({Op::WaitRemoStart});
        push({Op::WaitRemoEnd});
        flags({9180}, false);
        push({Op::NgCycle});
        flags({6604, c ? 6602u : 6601u, 6603}, true);
        flags({c ? 23u : 22u}, true);
        Op e{Op::EndingCheck};
        e.ending = static_cast<int>(s.id);
        push(e);
        break;
    }
    default:
        break;
    }
}

void GuestStory::Finish(bool ok) {
    (void)ok;
    running_ = false;
    running_mirror_ = false;
    ops_.clear();
    dirty_ = true;
}

std::optional<StoryAction> GuestStory::RunOps(const StoryState& st, double now) {
    const bool gate = st.world_up && !st.loading && !st.transition_requested;
    if (running_mirror_ && st.session_role != 6) {
        Finish(false); // left the host world: the replay queue takes over
        return std::nullopt;
    }
    while (!ops_.empty()) {
        Op& op = ops_.front();
        if (!op_started_) {
            op_started_ = true;
            op_since_ = now;
            load_seen_ = false;
        }
        switch (op.kind) {
        case Op::Play: {
            if (!gate || st.remo_playing) {
                return std::nullopt;
            }
            StoryAction a;
            a.kind = StoryAction::PlayRemo;
            a.id = RemoForSex(op.id, st.sex_variant);
            a.mode = op.mode;
            a.mirror = running_mirror_;
            a.why = running_mirror_ ? "live mirror" : "replay";
            ops_.pop_front();
            op_started_ = false;
            return a;
        }
        case Op::WaitRemoStart:
            if (st.remo_playing) {
                ops_.pop_front();
                op_started_ = false;
                continue;
            }
            if (now - op_since_ > kRemoStartTimeout) {
                // Never started (queue refused or a one-frame scene): skip the end wait.
                ops_.pop_front();
                if (!ops_.empty() && ops_.front().kind == Op::WaitRemoEnd) {
                    ops_.pop_front();
                }
                op_started_ = false;
                continue;
            }
            return std::nullopt;
        case Op::WaitRemoEnd:
            if (!st.remo_playing || now - op_since_ > kRemoTimeout) {
                if (cur_.kind == StoryKind::Cutscene) {
                    seen_.insert(BaseRemo(cur_.id));
                    dirty_ = true;
                }
                ops_.pop_front();
                op_started_ = false;
                continue;
            }
            return std::nullopt;
        case Op::Flags: {
            if (!gate) {
                return std::nullopt;
            }
            StoryAction a;
            a.kind = StoryAction::SetFlags;
            a.flags = op.flags;
            a.on = op.on;
            a.why = StoryKindName(cur_.kind);
            ops_.pop_front();
            op_started_ = false;
            return a;
        }
        case Op::Tod: {
            if (!gate) {
                return std::nullopt;
            }
            const int want = TodFlagValue(op.tod);
            ops_.pop_front();
            op_started_ = false;
            if (want <= st.tod_value) {
                continue; // never turn the guest's clock back
            }
            StoryAction a;
            a.kind = StoryAction::ApplyTod;
            a.tod = op.tod;
            a.why = "time of day";
            return a;
        }
        case Op::NgCycle: {
            if (!gate) {
                return std::nullopt;
            }
            ops_.pop_front();
            op_started_ = false;
            StoryAction a;
            a.kind = StoryAction::NgCycle;
            a.why = "ending";
            return a;
        }
        case Op::TravelDream: {
            if (!gate) {
                return std::nullopt;
            }
            ops_.pop_front();
            op_started_ = false;
            if ((st.map_id >> 16) == 0x1500) { // m21_00: already in the Dream
                if (!ops_.empty() && ops_.front().kind == Op::WaitArrive) {
                    ops_.pop_front();
                }
                continue;
            }
            StoryAction a;
            a.kind = StoryAction::TravelDream;
            a.id = kDreamLamp;
            a.why = "ending";
            return a;
        }
        case Op::WaitArrive:
            load_seen_ = load_seen_ || st.loading || !st.world_up;
            if (load_seen_ && gate && (st.map_id >> 16) == 0x1500 && stable_own_ >= kStableTicks) {
                ops_.pop_front();
                op_started_ = false;
                continue;
            }
            if (now - op_since_ > kTravelTimeout) {
                Finish(false); // never arrived: drop the ending (logged by the caller)
                return std::nullopt;
            }
            return std::nullopt;
        case Op::EndingCheck:
            // The flag (21/22/23) is set; 0x193AC10 should request the ending step itself (a
            // loading screen follows). Probe C7: if not, request it like RequestEnding.
            load_seen_ = load_seen_ || st.loading || !st.world_up;
            if (load_seen_) {
                ops_.pop_front();
                op_started_ = false;
                continue;
            }
            if (now - op_since_ > (op.ending == 1 ? kRemoTimeout : kEndingWait)) {
                const int ending = op.ending;
                ops_.pop_front();
                op_started_ = false;
                if (ending == 1) {
                    continue; // A is the game's own event: never forced (it does NG+ itself)
                }
                StoryAction a;
                a.kind = StoryAction::ForceEnding;
                a.ending = ending;
                a.why = "no ending step after the flag";
                return a;
            }
            return std::nullopt;
        }
    }
    Finish(true);
    return std::nullopt;
}

std::optional<StoryAction> GuestStory::Run(const StoryState& st, double now) {
    std::optional<StoryAction> a = RunOps(st, now);
    if (a && running_ && ops_.empty()) {
        Finish(true); // that was the last step
    }
    return a;
}

std::optional<StoryAction> GuestStory::Step(const StoryState& st, double now) {
    const bool base = st.world_up && !st.loading && !st.transition_requested && !st.remo_playing;
    stable_mirror_ = base && st.session_role == 6 ? stable_mirror_ + 1 : 0;
    const bool own = st.own_world && st.session_role != 4 && st.session_role != 6 && st.session_role != 7;
    stable_own_ = st.world_up && !st.loading && !st.transition_requested && own ? stable_own_ + 1 : 0;

    if (running_) {
        return Run(st, now);
    }
    if (mirror_) {
        if (now - mirror_since_ > kMirrorTimeout || st.session_role != 6) {
            mirror_.reset();
        } else if (stable_mirror_ >= kStableTicks) {
            const StoryIntent s = *mirror_;
            mirror_.reset();
            Build(s, true);
            return Run(st, now);
        } else {
            return std::nullopt;
        }
    }
    if (!queue_.empty() && stable_own_ >= kStableTicks && !st.remo_playing) {
        const StoryIntent s = queue_.front();
        queue_.pop_front();
        dirty_ = true;
        Build(s, false);
        return Run(st, now);
    }
    return std::nullopt;
}

json::Value GuestStory::ToJson() const {
    json::Value v = json::Value::make_object();
    v.set("last_seq", json::hex(last_seq_));
    json::Value q = json::Value::make_array();
    // An item under way is kept (replayed again after a restart: its steps are idempotent).
    if (running_ && !running_mirror_) {
        q.push(StoryToJson(cur_));
    }
    for (const StoryIntent& s : queue_) {
        q.push(StoryToJson(s));
    }
    v.set("queue", q);
    json::Value seen = json::Value::make_array();
    for (std::uint32_t id : seen_) {
        seen.push(id);
    }
    v.set("seen", seen);
    return v;
}

bool GuestStory::FromJson(const json::Value& v, std::string* error) {
    try {
        if (v.type != json::Value::Type::Object) {
            throw std::runtime_error("not an object");
        }
        std::deque<StoryIntent> q;
        for (const json::Value& item : json::arr(v, "queue")) {
            StoryIntent s;
            std::string err;
            if (!StoryFromJson(item, &s, &err)) {
                throw std::runtime_error("queue: " + err);
            }
            q.push_back(s);
        }
        std::set<std::uint32_t> seen;
        for (const json::Value& id : json::arr(v, "seen")) {
            seen.insert(static_cast<std::uint32_t>(json::as_u64(id, "seen")));
        }
        last_seq_ = std::max(last_seq_, json::u64(v, "last_seq"));
        queue_ = std::move(q);
        seen_ = std::move(seen);
        return true;
    } catch (const std::exception& e) {
        if (error) {
            *error = e.what();
        }
        return false;
    }
}

// ---------------------------------------------------------------------------------------------
// Game part
// ---------------------------------------------------------------------------------------------
#ifndef BB_PARTY_STORY_NO_GAME

namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using ull = unsigned long long;
using Clock = std::chrono::steady_clock;

constexpr u64 kRemoHandler = 0x17c53b0;  // bank-2002 EMEVD handler (this, SprjEmkEventIns*)
constexpr u64 kPlayRemo = 0x131a9f0;     // (unused, remo id, mode, a4, a5, player) -> al
constexpr u64 kSprjRemo = 0x5540058;     // singleton slot; *(+8) = remo player; +0x168 bit 0 playing
constexpr u64 kEventFlagMan = 0x553b100; // singleton slot
constexpr u64 kSetEventFlag = 0x13cfcc0; // (man, id, on)
constexpr u64 kGetFlagValue = 0x13cfd80; // (man, first, bits) -> value
constexpr u64 kSetFlagValue = 0x13d0060; // (man, first, bits, value)
constexpr u64 kTodWorld = 0x553b148;     // *(slot)+0x81 = 1: re-evaluate time of day (0x1CECEB0)
constexpr u64 kGameDataMan = 0x553b130;  // +0x68 NG cycle, +0x6C ending type
constexpr u64 kWts = 0x5556678;          // +0x08 transition requested, +0x1550 ending request, +0x1592 own world
constexpr u64 kWorldChrMan = 0x553e878;  // +0x60 local PlayerIns (vtable +0x1C8 -> chr data, +0xCA sex)
constexpr u64 kLampWarp = 0x13cdf30;     // void (u32 id)

using PlayRemoFn = u64(BB_COOP_SYSV*)(u64, u64, u64, u64, u64, u64);
using SetFlagFn = void(BB_COOP_SYSV*)(u64, u32, u32);
using GetFlagValueFn = u32(BB_COOP_SYSV*)(u64, u32, u32);
using SetFlagValueFn = void(BB_COOP_SYSV*)(u64, u32, u32, u32);
using LampWarpFn = void(BB_COOP_SYSV*)(u32);
using ChrDataFn = u64(BB_COOP_SYSV*)(u64);

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[768];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    std::printf("Party story: %s\n", line);
    std::fflush(stdout);
}

bool EnvOff(const char* name) {
    const char* v = std::getenv(name);
    return v && v[0] == '0' && !v[1];
}

double Now() {
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

std::atomic<bool> g_story_on{false};
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

std::mutex g_host_mu;
HostStoryQueue g_host_queue; // under g_host_mu
std::mutex g_guest_mu;
GuestStory g_guest;          // under g_guest_mu
i32 g_player_arg = 10000;

// Main-thread state.
struct Tick {
    bool host_baseline = false;
    bool ending_sent[4] = {};
    int last_tod_value = -1;
    double world_since = 0;
    bool world_was_up = false;
    // test
    bool test_on = false, test_done = false, test_mirror = false;
    u32 test_remo = 0;
    double test_delay = 10.0;
    // remo watch (logs)
    bool remo_was_playing = false;
    double remo_since = 0;
    std::string sidecar;
    double last_save = 0;
};
Tick g_tick;

u64 Slot(u64 off) {
    u64 p = 0;
    return SafeGet(Guest(off), &p) ? p : 0;
}

template <class T>
T Field(u64 base, u64 off, T fallback) {
    T v{};
    return base && SafeGet(base + off, &v) ? v : fallback;
}

bool RemoPlaying() {
    const u64 remo = Slot(kSprjRemo);
    const u64 player = Field<u64>(remo, 8, 0);
    return (Field<u8>(player, 0x168, 0) & 1) != 0;
}

bool OwnWorld() {
    return Field<u8>(Slot(kWts), 0x1592, 0) == 1;
}

int FlagValue(u32 first, u32 bits) {
    const u64 man = Slot(kEventFlagMan);
    if (!man) {
        return -1;
    }
    return static_cast<int>(reinterpret_cast<GetFlagValueFn>(Guest(kGetFlagValue))(man, first, bits));
}

/// EMEVD sex 1 (the +1000 remo files) = chr data +0xCA == 0 (1003[12] at 0x17bedd0 case 0xc).
bool SexVariant() {
    const u64 wcm = Slot(kWorldChrMan);
    const u64 player = Field<u64>(wcm, 0x60, 0);
    const u64 vt = Field<u64>(player, 0, 0);
    const u64 fn = Field<u64>(vt, 0x1c8, 0);
    if (!fn) {
        return false;
    }
    const u64 data = reinterpret_cast<ChrDataFn>(fn)(player);
    return data && Field<u8>(data, 0xca, 1) == 0;
}

PartyRole Role() {
    return PartyDirector::Get().Role();
}

void PushHost(const StoryIntent& s) {
    {
        std::lock_guard<std::mutex> lk(g_host_mu);
        g_host_queue.Push(s);
    }
    Log("host %s", DescribeStory(s).c_str());
}

// ---- Host capture: the bank-2002 handler entry ----

BB_COOP_SYSV void RemoHandlerEntry(u64, u64 event, u64, u64, u64, u64) {
    if (!g_story_on.load() || Role() != PartyRole::Host || !event) {
        return;
    }
    u64 instr = 0;
    if (!SafeGet(event + 0xb0, &instr) || !instr) {
        return;
    }
    i32 bank = 0, id = 0;
    SafeGet(instr, &bank);
    SafeGet(instr + 4, &id);
    if (bank != 2002 || id < 1 || id > 7 || id == 5) {
        return; // 8 = dummy remo + warp (B1 travel covers it), 5 unused in 1.09
    }
    if (!OwnWorld()) {
        return; // a summoned host? only plays of our own world are the party's
    }
    // The arguments exactly as 0x17C53B0 finds them.
    u64 args = 0;
    SafeGet(event + 0xb8, &args);
    if (!args) {
        u64 runtime = 0, base = 0, section = 0;
        i64 arg_off = 0;
        if (SafeGet(event + 0xa8, &runtime) && runtime && SafeGet(runtime + 8, &base) && base &&
            SafeGet(base + 0x78, &section) && SafeGet(instr + 0x10, &arg_off)) {
            args = base + section + u64(arg_off);
        }
    }
    u8 raw[0x18] = {};
    if (!args || !SafeRead(args, raw, sizeof raw)) {
        return;
    }
    auto rd32 = [&](int off) {
        u32 v;
        std::memcpy(&v, raw + off, 4);
        return v;
    };
    StoryIntent s;
    s.kind = StoryKind::Cutscene;
    s.instr = static_cast<u32>(id);
    s.id = rd32(0);
    s.mode = rd32(4);
    SafeGet(event + 0x28, &s.event_id);
    SafeGet(event + 0x68, &s.map);
    switch (id) {
    case 2:
    case 4:
        s.warp_point = rd32(8);
        s.warp_map = u32(raw[0xc]) << 24 | u32(raw[0xd]) << 16;
        break;
    case 6:
        s.warp_point = rd32(8);
        s.warp_map = u32(raw[0xc]) << 24 | u32(raw[0xd]) << 16;
        s.tod = raw[0x14] < 4 ? raw[0x14] : -1;
        break;
    case 7:
        s.tod = raw[0xc] < 4 ? raw[0xc] : -1;
        break;
    default:
        break;
    }
    const int ending = EndingTypeOfEvent(s.event_id);
    if (ending) {
        // Before the room closes (the flag 21/22/23 comes after the remo).
        StoryIntent e;
        e.seq = NextSeq();
        e.kind = StoryKind::Ending;
        e.id = static_cast<u32>(ending);
        e.event_id = s.event_id;
        e.map = s.map;
        if (!g_tick.ending_sent[ending]) { // a benign race with the tick: worst case a duplicate the guest drops
            g_tick.ending_sent[ending] = true;
            PushHost(e);
        }
        return;
    }
    s.flags = StoryFlagsForRemo(s.id);
    s.seq = NextSeq();
    PushHost(s);
}

// ---- Guest actions ----

void SetFlags(const std::vector<u32>& flags, bool on) {
    const u64 man = Slot(kEventFlagMan);
    if (!man) {
        Log("no EventFlagMan: flags not set");
        return;
    }
    for (u32 f : flags) {
        reinterpret_cast<SetFlagFn>(Guest(kSetEventFlag))(man, f, on ? 1 : 0);
    }
}

void Run(const StoryAction& a) {
    switch (a.kind) {
    case StoryAction::PlayRemo: {
        const u64 ok = reinterpret_cast<PlayRemoFn>(Guest(kPlayRemo))(0, a.id, a.mode, 0xffffffffu, 0xffffffffu,
                                                                       static_cast<u32>(g_player_arg));
        Log("%s: remo %u mode %u player %d -> %s", a.why.c_str(), a.id, a.mode, g_player_arg,
            (ok & 0xff) ? "queued" : "NOT queued (no remo player)");
        break;
    }
    case StoryAction::SetFlags: {
        SetFlags(a.flags, a.on);
        std::string list;
        for (u32 f : a.flags) {
            list += (list.empty() ? "" : ",") + std::to_string(f) + "=" + std::to_string(FlagValue(f, 1));
        }
        Log("%s: flags set %s (read back: %s)", a.why.c_str(), a.on ? "ON" : "OFF", list.c_str());
        break;
    }
    case StoryAction::ApplyTod: {
        const u64 man = Slot(kEventFlagMan);
        const int v = TodFlagValue(a.tod);
        if (man && v >= 0) {
            const int before = FlagValue(9800, 3);
            reinterpret_cast<SetFlagValueFn>(Guest(kSetFlagValue))(man, 9800, 3, static_cast<u32>(v));
            if (const u64 w = Slot(kTodWorld)) {
                const u8 one = 1;
                SafeWrite(w + 0x81, &one, 1);
            }
            Log("time of day %d: 9800..9802 %d -> %d", a.tod, before, FlagValue(9800, 3));
        }
        break;
    }
    case StoryAction::NgCycle: {
        const u64 gdm = Slot(kGameDataMan);
        u32 ng = Field<u32>(gdm, 0x68, 0xffffffffu);
        if (ng != 0xffffffffu) {
            const u32 next = ng + 1 > 7 ? 7 : ng + 1;
            SafeWrite(gdm + 0x68, &next, 4);
            Log("NG cycle %u -> %u", ng, next);
        }
        break;
    }
    case StoryAction::TravelDream:
        Log("ending: warping to the own Hunter's Dream (%u)", a.id);
        reinterpret_cast<LampWarpFn>(Guest(kLampWarp))(a.id);
        break;
    case StoryAction::ForceEnding: {
        const u64 gdm = Slot(kGameDataMan), wts = Slot(kWts);
        const u32 type = static_cast<u32>(a.ending);
        const u8 req = Field<u8>(wts, 0x1550, 0);
        Log("ending %d: no ending step yet (GSM+0x1550 %u, GameDataMan+0x6C %u); requesting it", a.ending, req,
            Field<u32>(gdm, 0x6c, 0));
        if (gdm && wts) {
            const u8 one = 1;
            SafeWrite(gdm + 0x6c, &type, 4);
            SafeWrite(wts + 0x1550, &one, 1);
        }
        break;
    }
    case StoryAction::None:
        break;
    }
}

// ---- Sidecar ----

void LoadSidecar() {
    const char* dir = std::getenv("BB_GPU_USER_DIR");
    if (!dir || !dir[0]) {
        return;
    }
    g_tick.sidecar = std::string(dir) + "/party_story.json";
    std::ifstream in(g_tick.sidecar, std::ios::binary);
    if (!in) {
        return;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    json::Value v;
    std::string err;
    std::lock_guard<std::mutex> lk(g_guest_mu);
    if (!json::parse(ss.str(), v, err) || !g_guest.FromJson(v, &err)) {
        Log("%s: %s; ignored", g_tick.sidecar.c_str(), err.c_str());
        return;
    }
    g_guest.TakeDirty();
    Log("%s: %zu queued, last seq %llu", g_tick.sidecar.c_str(), g_guest.QueueSize(), ull(g_guest.LastSeq()));
}

void SaveSidecar() {
    if (g_tick.sidecar.empty()) {
        return;
    }
    std::string text;
    {
        std::lock_guard<std::mutex> lk(g_guest_mu);
        if (!g_guest.TakeDirty()) {
            return;
        }
        text = json::dump(g_guest.ToJson(), 1);
    }
    const std::string tmp = g_tick.sidecar + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << text;
    }
    std::remove(g_tick.sidecar.c_str());
    std::rename(tmp.c_str(), g_tick.sidecar.c_str());
}

// ---- Host polls: endings (flags 21/22/23) and time of day ----

void HostPoll(const GameSnapshot& g) {
    if (!g.world_up || g.loading || !OwnWorld()) {
        g_tick.host_baseline = false;
        return;
    }
    const int tod_value = FlagValue(9800, 3);
    int endings[4] = {0, FlagValue(21, 1), FlagValue(22, 1), FlagValue(23, 1)};
    if (!g_tick.host_baseline) {
        // A loaded save may already hold them: only changes from here on are news.
        g_tick.host_baseline = true;
        g_tick.last_tod_value = tod_value;
        for (int i = 1; i <= 3; ++i) {
            g_tick.ending_sent[i] = g_tick.ending_sent[i] || endings[i] == 1;
        }
        return;
    }
    for (int i = 1; i <= 3; ++i) {
        if (endings[i] == 1 && !g_tick.ending_sent[i]) {
            g_tick.ending_sent[i] = true;
            StoryIntent e;
            e.seq = NextSeq();
            e.kind = StoryKind::Ending;
            e.id = static_cast<u32>(i);
            PushHost(e);
        }
    }
    if (tod_value > g_tick.last_tod_value && tod_value >= 0) {
        StoryIntent t;
        t.seq = NextSeq();
        t.kind = StoryKind::TimeOfDay;
        t.id = static_cast<u32>(TodFromFlagValue(tod_value));
        PushHost(t);
    }
    if (tod_value >= 0) {
        g_tick.last_tod_value = tod_value;
    }
}

void ParseTest() {
    const char* t = std::getenv("BB_PARTY_STORY_TEST");
    if (!t || !t[0]) {
        return;
    }
    std::string s(t);
    if (s.rfind("cutscene:", 0) == 0) {
        g_tick.test_on = true;
        g_tick.test_remo = static_cast<u32>(std::strtoul(s.c_str() + 9, nullptr, 0));
        g_tick.test_mirror = s.find(",mirror") != std::string::npos;
        if (const char* d = std::getenv("BB_PARTY_STORY_TEST_DELAY"); d && d[0]) {
            g_tick.test_delay = std::atof(d);
        }
        Log("test: replay remo %u %.0f s after the world is up%s", g_tick.test_remo, g_tick.test_delay,
            g_tick.test_mirror ? " (mirror path, mode 2)" : "");
    } else {
        Log("BB_PARTY_STORY_TEST: '%s' is not cutscene:<id>[,mirror]; ignored", t);
    }
}

} // namespace

bool StoryTestRequested() {
    const char* t = std::getenv("BB_PARTY_STORY_TEST");
    return t && t[0];
}

void InstallStoryHooks() {
    static std::atomic<bool> done{false};
    if (done.exchange(true)) {
        return;
    }
    if (EnvOff("BB_PARTY_STORY")) {
        Log("BB_PARTY_STORY=0: no cutscene / ending sync");
        return;
    }
    if (!Image()) {
        Log("no image; story off");
        return;
    }
    if (const char* p = std::getenv("BB_PARTY_STORY_PLAYER"); p && p[0]) {
        g_player_arg = static_cast<i32>(std::strtol(p, nullptr, 0));
    }
    g_guest.mirror_enabled = !EnvOff("BB_PARTY_STORY_MIRROR");
    // Reference sites (cutscenes_endings.md 5.1): play remo, time-of-day apply; the calls need them.
    const bool play_ok = Matches(kPlayRemo, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54});
    const bool tod_ok =
        Matches(0x1ceceb0, {0x55, 0x48, 0x89, 0xe5, 0x48, 0x89, 0xf8, 0x83, 0x78, 0x0c, 0x01, 0x75, 0x41});
    if (!play_ok || !tod_ok) {
        Log("0x131A9F0 / 0x1CECEB0 are not the 1.09 bytes (%s / %s); story off", play_ok ? "ok" : "MISMATCH",
            tod_ok ? "ok" : "MISMATCH");
        return;
    }
    // push rbp; mov rbp, rsp; push r15, r14, r13, r12, rbx; sub rsp, 0x68 (17 bytes; next is the
    // rip-relative stack-guard load).
    HookPrologue(kRemoHandler,
                 {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x68},
                 &RemoHandlerEntry, "story capture (EMEVD bank 2002 handler)");
    ParseTest();
    LoadSidecar();
    g_story_on = true;
    Log("on: mirror %s, player argument %d", g_guest.mirror_enabled ? "on" : "off", g_player_arg);
}

bool PopHostStory(StoryIntent* out) {
    std::lock_guard<std::mutex> lk(g_host_mu);
    return g_host_queue.Pop(out);
}

void RequestGuestStory(const StoryIntent& s) {
    if (!g_story_on.load()) {
        return;
    }
    // Role from SprjSessionManager without calling the game (any thread).
    const GameSnapshot g = ReadGameState(false);
    const bool phantom = g.session_role == RoleClient;
    bool taken;
    {
        std::lock_guard<std::mutex> lk(g_guest_mu);
        taken = g_guest.Offer(s, phantom, Now());
    }
    Log("guest %s %s%s", DescribeStory(s).c_str(), taken ? "taken" : "ignored (not newer than the last)",
        taken && phantom ? " (phantom: live mirror)" : "");
}

bool StoryBusy() {
    if (!g_story_on.load()) {
        return false;
    }
    std::lock_guard<std::mutex> lk(g_guest_mu);
    return g_guest.ReplayBusy();
}

void StoryTick() {
    if (!g_story_on.load()) {
        return;
    }
    const double now = Now();
    const GameSnapshot g = ReadGameState(false);
    if (g.world_up && !g.loading) {
        if (!g_tick.world_was_up) {
            g_tick.world_was_up = true;
            g_tick.world_since = now;
        }
    } else {
        g_tick.world_was_up = false;
    }
    const PartyRole role = Role();
    if (role == PartyRole::Host) {
        HostPoll(g);
    }
    StoryState st;
    st.world_up = g.world_up;
    st.loading = g.loading;
    st.session_role = g.session_role;
    st.transition_requested = Field<u8>(Slot(kWts), 0x08, 1) != 0;
    st.own_world = OwnWorld();
    st.remo_playing = g.world_up && RemoPlaying();
    st.map_id = g.map_id;
    bool busy;
    {
        std::lock_guard<std::mutex> lk(g_guest_mu);
        busy = g_guest.Busy();
    }
    if (g.world_up && !g.loading && (busy || (g_tick.test_on && !g_tick.test_done))) {
        st.sex_variant = SexVariant(); // a virtual call into the game: only when a play may follow
    }
    if (g.world_up && !g.loading) {
        const int tv = FlagValue(9800, 3);
        st.tod_value = tv < 0 ? 0 : tv;
    }
    // Remo watch: the playing bit's edges (both roles; the evidence for tests).
    if (st.remo_playing != g_tick.remo_was_playing) {
        if (st.remo_playing) {
            g_tick.remo_since = now;
            Log("remo playing (map %s)", MapName(g.map_id).c_str());
        } else {
            Log("remo ended after %.1f s; control back (%s)", now - g_tick.remo_since, Describe(g).c_str());
        }
        g_tick.remo_was_playing = st.remo_playing;
    }
    // Test: one forced replay (or mirror) item.
    if (g_tick.test_on && !g_tick.test_done && g_tick.world_was_up && now - g_tick.world_since >= g_tick.test_delay) {
        g_tick.test_done = true;
        StoryIntent s;
        s.seq = NextSeq();
        s.kind = StoryKind::Cutscene;
        s.id = g_tick.test_remo;
        s.instr = 3;
        s.force = true;
        bool taken = true;
        if (g_tick.test_mirror) {
            // The mirror program needs session role 6 (a phantom); run its play call directly.
            StoryAction a;
            a.kind = StoryAction::PlayRemo;
            a.id = RemoForSex(s.id, st.sex_variant);
            a.mode = 2;
            a.mirror = true;
            a.why = "test mirror";
            Run(a);
        } else {
            std::lock_guard<std::mutex> lk(g_guest_mu);
            taken = g_guest.Offer(s, false, now);
        }
        Log("test: %s (%s; sex variant %d, tod value %d)", DescribeStory(s).c_str(), taken ? "queued" : "refused",
            st.sex_variant ? 1 : 0, st.tod_value);
    }
    std::optional<StoryAction> a;
    {
        std::lock_guard<std::mutex> lk(g_guest_mu);
        a = g_guest.Step(st, now);
    }
    if (a) {
        Run(*a);
    }
    if (now - g_tick.last_save >= 2.0) {
        g_tick.last_save = now;
        SaveSidecar();
    }
}

#endif // BB_PARTY_STORY_NO_GAME

} // namespace coop

// SPDX-License-Identifier: GPL-3.0-or-later
// Party campaign start logic (see party_start.h). No game access here.
#include "party_start.h"

#include <cctype>
#include <cstdio>

namespace coop {

StartMode ParseStartMode(const char* text, bool* known) {
    if (known) {
        *known = true;
    }
    if (!text || !text[0]) {
        return StartMode::PrologueSolo;
    }
    std::string v;
    for (const char* p = text; *p; ++p) {
        v += char(std::tolower(static_cast<unsigned char>(*p)));
    }
    if (v == "prologue_solo" || v == "prologue" || v == "solo") {
        return StartMode::PrologueSolo;
    }
    if (v == "immediate" || v == "clinic") {
        return StartMode::Immediate;
    }
    if (known) {
        *known = false;
    }
    return StartMode::PrologueSolo;
}

const char* StartModeName(StartMode m) {
    return m == StartMode::Immediate ? "immediate" : "prologue_solo";
}

const char* StartStepName(StartStep s) {
    switch (s) {
    case StartStep::NoWorld: return "no-world";
    case StartStep::Unknown: return "unknown";
    case StartStep::Opening: return "opening";
    case StartStep::Clinic: return "clinic";
    case StartStep::FirstDream: return "first-dream";
    case StartStep::Ready: return "ready";
    }
    return "?";
}

StartStep ClassifyStart(const StartFlags& f, bool in_world) {
    if (!in_world) {
        return StartStep::NoWorld;
    }
    if (!f.flags_ok) {
        return StartStep::Unknown;
    }
    if (!f.opening_done) {
        return StartStep::Opening;
    }
    if (!f.first_dream) {
        return StartStep::Clinic;
    }
    if (!f.weapon_right || !f.weapon_left) {
        return StartStep::FirstDream;
    }
    return StartStep::Ready;
}

bool StartReady(StartMode mode, const StartFlags& f, bool in_world) {
    if (!in_world || !f.flags_ok || f.cutscene) {
        return false;
    }
    if (mode == StartMode::Immediate) {
        return f.opening_done;
    }
    return ClassifyStart(f, in_world) == StartStep::Ready;
}

std::string DescribeStart(const StartFlags& f, bool in_world) {
    char text[320];
    if (!f.flags_ok) {
        std::snprintf(text, sizeof text, "step %s (flags unreadable)", StartStepName(ClassifyStart(f, in_world)));
        return text;
    }
    std::snprintf(text, sizeof text,
                  "step %s: 12410000 %d, 9180 %d, 9402 %d, 9401 %d, 12101020 %d, 12101021 %d, 12100105 %d, "
                  "6622 %d, 6610 %d, goods 200 x%d, 205 x%d",
                  StartStepName(ClassifyStart(f, in_world)), f.opening_done, f.cutscene, f.first_death, f.first_dream,
                  f.weapon_right, f.weapon_left, f.doll_awake, f.beckoning_lot, f.resonant_shop, f.beckoning_count,
                  f.resonant_count);
    return text;
}

BellGrant PlanBellGrant(const StartFlags& f) {
    BellGrant g;
    if (!f.flags_ok) {
        return g;
    }
    if (f.beckoning_count == 0) {
        (f.beckoning_lot ? g.beckoning_goods : g.beckoning_lot) = true;
    }
    if (f.resonant_count == 0) {
        g.resonant = true;
    }
    return g;
}

party::MemberState OwnWorldState(bool ready) {
    return ready ? party::MemberState::Home : party::MemberState::Prologue;
}

bool MemberWaits(const std::vector<party::RosterEntry>& roster) {
    for (const party::RosterEntry& e : roster) {
        if (e.slot != party::kHostSlot && e.connected &&
            (e.state == party::MemberState::Home || e.state == party::MemberState::Joining)) {
            return true;
        }
    }
    return false;
}

bool HostMayRing(bool host_ready, int cooperators, int max_players, const std::vector<party::RosterEntry>& roster) {
    return host_ready && cooperators >= 0 && cooperators < max_players - 1 && MemberWaits(roster);
}

bool GuestMayRing(bool guest_ready, bool connected) {
    return guest_ready && connected;
}

bool SessionMayRing(bool host, int session_role) {
    constexpr int kIdle = 0, kHost = 3; // RoleIdle, RoleHost (game_state.h)
    return session_role == kIdle || (host && session_role == kHost);
}

const char* CatchupPhaseName(Catchup::Phase p) {
    switch (p) {
    case Catchup::Phase::Idle: return "idle";
    case Catchup::Phase::Opening: return "opening";
    case Catchup::Phase::Apply: return "apply";
    case Catchup::Phase::Warp: return "warp";
    case Catchup::Phase::WaitWarp: return "wait-warp";
    case Catchup::Phase::Done: return "done";
    }
    return "?";
}

CatchupAction Catchup::Step(const CatchupInput& in) {
    CatchupAction a;
    if (in.in_world && in.own_idle) {
        if (world_at_ < 0) {
            world_at_ = in.t;
        }
    } else {
        world_at_ = -1;
    }
    const bool steady = world_at_ >= 0 && in.t - world_at_ >= kSteady && in.flags.flags_ok;
    switch (phase_) {
    case Phase::Idle:
    case Phase::Opening: {
        if (!in.guest || !in.host_known || !in.host_ready) {
            return a; // nothing to compare against (or the host itself is in its prologue)
        }
        const StartStep step = ClassifyStart(in.flags, in.in_world);
        // A scene playing in the own world while behind the host (the opening; its 12410000 may
        // already be set by C2's snapshot while the remo still runs): the game's skip request,
        // retried until the remo is skippable and gone.
        if (step != StartStep::Ready && in.remo_playing && in.in_world && in.own_idle) {
            if (step == StartStep::Opening) {
                phase_ = Phase::Opening;
            }
            if (skips_ < kSkipTries && in.t - skip_at_ >= kSkipEvery) {
                skip_at_ = in.t;
                ++skips_;
                a.kind = CatchupAction::SkipOpening;
                a.why = "behind the host and a cutscene plays in the own world: skip " + std::to_string(skips_);
            }
            return a;
        }
        if (step == StartStep::Ready) {
            phase_ = Phase::Done;
            a.kind = CatchupAction::Finished;
            a.why = "the own save is past the prologue: nothing to catch up";
            return a;
        }
        if (step == StartStep::NoWorld || step == StartStep::Unknown) {
            return a;
        }
        if (step == StartStep::Opening) {
            // The opening cutscene (or the frames before it): skip it, as Options does.
            phase_ = Phase::Opening;
            if ((in.remo_playing || in.flags.cutscene) && in.in_world && skips_ < kSkipTries &&
                in.t - skip_at_ >= kSkipEvery) {
                skip_at_ = in.t;
                ++skips_;
                a.kind = CatchupAction::SkipOpening;
                a.why = "behind the host and in the opening cutscene: skip " + std::to_string(skips_);
            }
            return a;
        }
        if (!steady || in.remo_playing || in.flags.cutscene) {
            return a; // a cutscene (first death, ...) or a load: wait for a steady own world
        }
        phase_ = Phase::Apply;
        a.kind = CatchupAction::ApplyStart;
        a.why = std::string("behind the host (step ") + StartStepName(step) + ")";
        applied_ = true;
        phase_at_ = in.t;
        return a;
    }
    case Phase::Apply:
        if (in.t - phase_at_ >= 1.0 && in.in_world) {
            phase_ = Phase::WaitWarp;
            phase_at_ = in.t;
            load_seen_ = false;
            a.kind = CatchupAction::Warp;
            a.why = "gather at the host's lamp";
        }
        return a;
    case Phase::Warp:
    case Phase::WaitWarp:
        if (!in.in_world) {
            load_seen_ = true;
        }
        if ((load_seen_ && steady) || in.t - phase_at_ >= kWarpTimeout) {
            phase_ = Phase::Done;
            a.kind = CatchupAction::Finished;
            a.why = load_seen_ ? "arrived after the gather warp" : "no load after the gather warp (timeout)";
        }
        return a;
    case Phase::Done:
        return a;
    }
    return a;
}

} // namespace coop

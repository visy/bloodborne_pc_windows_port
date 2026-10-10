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

} // namespace coop

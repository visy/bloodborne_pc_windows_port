// SPDX-License-Identifier: GPL-3.0-or-later
// C2 progress sync <-> PartyDirector / PartyLink glue (party_progress.h, DirectorTick). Kept out
// of party_director.cpp so the director only calls one function per frame.
//
// Host:  every frame, the captured flag changes go to every member (EVENT "flags", reliable:
//        replayed to a member that resumes); each member that is newly connected (a new slot, a
//        resumed slot: connected false -> true, or another name in the slot) gets the full
//        snapshot (EVENT "flag_snapshot") once the host has a baseline (retried every frame until
//        then). Session boundaries: hosting started / first member connected -> "host",
//        last member gone -> ended.
// Guest: entering the host's world (session role Client) -> "guest", leaving it -> ended. The
//        EVENTs themselves arrive in party_runtime's on_event (progress::OnLinkEvent).
#include "party_progress.h"

#include "game_state.h"
#include "party_link.h"

#include <cstdarg>
#include <cstdio>
#include <iterator>
#include <map>
#include <set>
#include <string>

namespace coop::progress {
namespace {

void BLog(const char* fmt, ...) __attribute__((format(gnu_printf, 1, 2)));
void BLog(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    std::printf("Party progress: %s\n", line);
    std::fflush(stdout);
}

struct Bridge {  // main thread only
    party::PartyLink* link = nullptr;
    bool hosting_seen = false;
    bool host_session = false;           // at least one member connected
    bool guest_in_world = false;
    std::map<int, std::string> connected;  // slot -> name, members connected at the last tick
    std::set<int> want_snapshot;
    bool waiting_logged = false;
};

Bridge& B() {
    static Bridge b;
    return b;
}

void HostSide(Bridge& b, party::PartyLink* link) {
    std::vector<FlagChange> changes = PopHostFlagChanges();  // always drained (no backlog)
    if (!link || !link->is_host()) {
        return;
    }
    if (!b.hosting_seen && link->state() == party::LinkState::Hosting) {
        b.hosting_seen = true;
        SessionStarted("host");
    }
    const std::vector<party::RosterEntry> roster = link->roster();
    std::map<int, std::string> now;
    bool any_member = false;  // connected or slot kept (broadcasts queue for kept slots)
    for (const party::RosterEntry& e : roster) {
        if (e.slot == party::kHostSlot) {
            continue;
        }
        any_member = true;
        if (e.connected) {
            now[e.slot] = e.name;
        }
    }
    for (const auto& [slot, name] : now) {
        const auto it = b.connected.find(slot);
        if (it == b.connected.end() || it->second != name) {
            b.want_snapshot.insert(slot);
            BLog("member %s (slot %d) connected: flag snapshot due", name.c_str(), slot);
        }
    }
    for (auto it = b.want_snapshot.begin(); it != b.want_snapshot.end();) {
        it = now.count(*it) ? std::next(it) : b.want_snapshot.erase(it);
    }
    b.connected = std::move(now);

    if (!b.host_session && !b.connected.empty()) {
        b.host_session = true;
        SessionStarted("host");
    } else if (b.host_session && b.connected.empty()) {
        b.host_session = false;
        SessionEnded();
    }

    if (!changes.empty() && any_member) {
        link->send_event(party::kBroadcast, kEventFlags, ChangesToJson(HostEpoch(), changes));
        BLog("%zu flag changes (seq %llu..%llu) sent to the party", changes.size(),
             static_cast<unsigned long long>(changes.front().seq), static_cast<unsigned long long>(changes.back().seq));
    }
    if (!b.want_snapshot.empty()) {
        FlagSnapshot snap;
        if (!FullSnapshot(&snap)) {
            if (!b.waiting_logged) {
                b.waiting_logged = true;
                BLog("flag snapshot waits for the host's baseline (world not steady yet)");
            }
            return;
        }
        b.waiting_logged = false;
        const std::string body = SnapshotToJson(snap);
        for (int slot : b.want_snapshot) {
            link->send_event(slot, kEventFlagSnapshot, body);
            BLog("flag snapshot (%zu blocks, seq %llu, %zu bytes) sent to slot %d", snap.blocks.size(),
                 static_cast<unsigned long long>(snap.seq), body.size(), slot);
        }
        b.want_snapshot.clear();
    }
}

void GuestSide(Bridge& b, const GameSnapshot& s) {
    const bool in_world = s.session_role == RoleClient;
    if (in_world != b.guest_in_world) {
        b.guest_in_world = in_world;
        BLog("guest %s the host's world", in_world ? "entered" : "left");
        if (in_world) {
            SessionStarted("guest");
        } else {
            SessionEnded();
        }
    }
}

} // namespace

void DirectorTick(const GameSnapshot& s, party::PartyLink* link) {
    if (!Enabled()) {
        return;
    }
    Tick(s);
    Bridge& b = B();
    if (link != b.link) {  // a new link (or none): its roster starts over
        b.link = link;
        b.hosting_seen = false;
        b.connected.clear();
        b.want_snapshot.clear();
        if (b.host_session) {
            b.host_session = false;
            SessionEnded();
        }
    }
    switch (CurrentRole()) {
    case Role::Host:
        HostSide(b, link);
        break;
    case Role::Guest:
        GuestSide(b, s);
        break;
    default:
        break;
    }
}

} // namespace coop::progress

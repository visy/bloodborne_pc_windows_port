// SPDX-License-Identifier: GPL-3.0-or-later
// A guest the party keeps where it stands although its game was sending it home (split travel,
// boss clear), and what such a guest's lamp / headstone answer means while the host is in another
// area. Pure logic, no game access (unit test tests/test_party_stay_rules.cpp); the game side is
// party_b2.cpp (SendHomeVeto, BossClearLeaveHook, StayActionHoldTick) and party_split.cpp (Carry).
//
// Player report (0.7.2): the host left the Hunter's Dream alone (a headstone, "travel alone"); the
// guest stayed in the host's Dream and could not use any headstone, so it could not leave.
//
// The guest's state after the host's alone travel (live logs pard4 A3 / parab3 b3t1, RE of the
// eboot): it is still a summoned client in the host's world image of the map it stood in (B2
// `the host travels alone ...; staying here (no load)`), the host's chr is removed, the session
// and the party link stay up. Its game sees the host's member / room go away and runs the Lua
// state OnRoomDisappeared (0x138A590) or OnIrregularLeaveSession_1 (0x13840E0). Both set flag 4047
// (0x13325A0(0xFCF, 1): the log's `4047(never_sync)`), SprjEventState = 2, the Lua event context's
// +0x8D "load wait" byte (0x138A714 `mov byte [r12+0x8d], 1`, 0x138412C `mov byte [rbx+0x8d], 1`)
// and start Lua timer 4046 (0x132F1C0(0xFCE)), whose end is OnLeave_Limit (0x138A4A0): the
// send-home load at 0x138A51C. Split travel drops that send-home (the host is apart and connected,
// `send-home at +0x138a51c (OnLeave_Limit) dropped: split travel`), so nothing ends the load wait:
// only the Lua context's constructor clears +0x8D, and a load is what rebuilds it. The Lua event
// update (0x1339B50) copies +0x8D into SprjActionButtonMan +0x52 every frame, the summoned
// player's action-button hold (issue #4 item 1, the same lock after a boss clear): no action button
// registers - no headstone, Doll, workbench, lamp or item prompt - until the next load. In the
// Dream the headstones are the only way out, so the guest was stuck there.
//
// The headstone talk scripts themselves were never the gate: t210000-t210003 ask
// IsMultiplayerInProgress (MultiFilter answers 0 for them in a party, both roles) and
// IsClientPlayer (IsClientFilter answers 0 for them on a guest with guest lamps on, also with the
// host away: the gate is "a client in the Dream", not "the host is here"). Their talk chrs
// 2100950.. are not in the NPC mirror's allow-list (kNpcMirrorIds), so the host's world never
// turns them off on a guest (0.7.2 lamprule).
#pragma once

#include <cstdint>

namespace coop::stayrules {

/// Why the party kept a guest whose game was sending it home.
enum class Keep : std::uint8_t {
    None,
    BossClear,  ///< K10: the boss-clear leave dropped (the party stays after a boss)
    HostApart,  ///< OnLeave_Limit's send-home dropped: the host travelled alone / is in another area
};

inline const char* KeepName(Keep k) {
    switch (k) {
    case Keep::BossClear: return "boss clear";
    case Keep::HostApart: return "the host travelled alone";
    default: return "none";
    }
}

/// Whether a load ends the lift. After a host's alone travel the guest's next load (a re-join, an
/// alone trip, a death respawn) builds a new Lua context with no load wait, so the lift ends there.
/// The boss-clear lift stays for the session (issue #4: unchanged).
inline bool KeepEndsAtLoad(Keep k) {
    return k == Keep::HostApart;
}

/// What the stay tick sees on a guest.
struct HoldView {
    bool party_guest = false;  ///< party on and this game is a summoned client
    Keep kept = Keep::None;    ///< why the party kept it (None: the game's own hold, left alone)
    double since_kept = 0;     ///< s since the party kept it
    double delay = 0;          ///< test aid (BB_PARTY_K10_HOLD_DELAY, boss clear only): lift after this
    bool keep_env = false;     ///< BB_PARTY_K10_HOLD=keep (boss clear only): the game's hold stays
    bool world_up = false;
    bool loading = false;
    bool player = false;
    int load_wait = 0;  ///< Lua event context +0x8D (-1 unreadable)
    int hold = 0;       ///< SprjActionButtonMan +0x52 (-1 unreadable)
};

enum class HoldAction : std::uint8_t {
    None,  ///< nothing to do (not held, not kept, a load running, ...)
    Lift,  ///< write 0 to the load wait and the hold: the guest's action buttons work again
    End,   ///< the session is over: forget the keep (a new session starts without the lift)
};

inline HoldAction PlanActionHold(const HoldView& v) {
    if (v.kept == Keep::None) {
        return HoldAction::None;
    }
    if (!v.party_guest) {
        return HoldAction::End;
    }
    if (v.loading && KeepEndsAtLoad(v.kept)) {
        return HoldAction::End;  // the load builds a new Lua context: nothing left to lift
    }
    if (v.kept == Keep::BossClear && (v.keep_env || v.since_kept < v.delay)) {
        return HoldAction::None;
    }
    if (!v.world_up || v.loading || !v.player) {
        return HoldAction::None;
    }
    return v.load_wait == 1 || v.hold == 1 ? HoldAction::Lift : HoldAction::None;
}

/// A guest's "Travel with the party?" YES at a lamp / headstone. Normally a request to the host,
/// whose warp moves the party. The host ignores a request for a lamp of the block it has loaded
/// (playtest 1 bug 10: it would only be yanked across its own map), so for a guest in another area
/// (left in the Dream) that YES did nothing at all. Such a guest goes there itself: a re-join into
/// the host's area at that lamp (one loading screen, the host does not move).
enum class ApartYes : std::uint8_t {
    PartyRequest,  ///< the host's party travel (as before)
    OwnTrip,       ///< this guest's own trip into the host's block (TravelAlone: a re-join)
};

inline ApartYes PlanApartGuestYes(bool host_apart, std::uint32_t lamp_map, std::uint32_t host_map) {
    const bool known = lamp_map != 0 && host_map != 0 && host_map != 0xffffffffu;
    return host_apart && known && (lamp_map >> 16) == (host_map >> 16) ? ApartYes::OwnTrip : ApartYes::PartyRequest;
}

}  // namespace coop::stayrules

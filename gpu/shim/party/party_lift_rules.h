// SPDX-License-Identifier: GPL-3.0-or-later
// Lifts in a party (D3, docs/party/world_actions.md 10.1): the table of every lift of the main game
// and the DLC, and the decisions the party makes about them. Pure logic, no game access (unit test:
// tests/test_party_lift_rules.cpp); the game side is party_worldact.cpp (LiftReplayTick, the guest's
// ride report, the host's lift_set / lift_clr) and party_b2.cpp (the fall recovery's ride test).
//
// How a lift moves in a party (live, EMEVD traces of every game, lifts071 / liftfix): every game runs
// its own copy of the lift's events, and the game's own network sync starts the copies of the other
// games that have the lift's block loaded when any player starts a ride (a step into the car, a call
// lever). Each copy ends its ride by itself: the newer lifts once their busy flag clears, the
// Nightmare of Mensis' library lift (12601352) once its OWN player is out of the car's regions; that
// copy then reads the position flag to pick the end it plays and writes the flag (1: the car at the
// top). So the copies of a ride ridden together end at different moments, and the world mirror can
// bring the host's flip to a guest while the guest's copy has not read the flag yet.
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace coop::liftrules {

/// A lift. `pos` is where the car is (the up event sets it 1, the down event 0, or the other way
/// round), `busy` is on while a ride runs (and after it until every player left the car), `host_state`
/// the newer lifts' "host's state" copy of `pos` (event `...ホスト状態管理`, 0 = none), `car` the car's
/// map object. A game with the lift's block streamed out never runs its copy: its world keeps the car
/// at the old end, and the block's init event puts the car there when the block streams in again.
/// Lifts whose ride ends with the flag alone (busy 0, no other "ride over" state) replay: `gate` and
/// the end animations are what their own copy plays right before it writes `pos` (the car and its gate
/// standing at that end); `ride_from_on` / `ride_from_off` the car's ride animations from that end (the
/// copy has not read the flag for its end yet while the car shows one of them), `init_on` / `init_off`
/// the poses the block's init event (2005[7]) puts the car in.
struct Lift {
    std::uint32_t pos, busy, host_state;
    std::int32_t car;
    std::int32_t gate = 0;
    std::int16_t car_end_on = -1, car_end_off = -1, gate_end_on = -1, gate_end_off = -1;
    std::int16_t ride_from_on[2] = {-1, -1}, ride_from_off[2] = {-1, -1};
    std::int16_t init_on = -1, init_off = -1;
    bool Replays() const { return car_end_on >= 0 && car_end_off >= 0; }
};

/// Every lift of the main game and the DLC. Checked against the game data (tools/party/lift_tool.py
/// check GAME: every event named エレベ.. with a ride into a region and a waited car animation, and a
/// scan of every event of every map for an unnamed one - none): 21 lifts. The Chalice Dungeons'
/// lifts are generated (thousands of layout slots of m29's 12900240.. / 12900315.. / 12900347..
/// events, cars 2901001..); their copies are network-synced like the main game's, and the fall
/// recovery's rate rule (deathrules::FallRecoveryDue) covers them without a table.
inline const std::vector<Lift>& Lifts() {
    // flag_tool emevd + an instruction dump of every elevator event (lifts071): {pos, busy, host
    // state, car}. Old lifts (down / up events only): busy is set at the ride's start and cleared
    // with pos at its end. Newer ones (init / host state / up / down): pos flips at the start.
    static const std::vector<Lift> lifts = {
        {12200125u, 12200126u, 0u, 2201210},         // Hemwick
        {12400150u, 12400151u, 0u, 2401101},         // Cathedral Ward (church lift)
        {12400827u, 12400828u, 0u, 2401102},         // Cathedral Ward <-> Old Yharnam
        {12410350u, 12415350u, 12410351u, 2411320},  // Central Yharnam
        {12420154u, 12420155u, 0u, 2421250},         // Upper Cathedral Ward (to Ebrietas)
        {12500074u, 12505074u, 0u, 2501010},         // Cainhurst Castle (liftfall)
        // Nightmare of Mensis (liftfall: its common up / down events 12601320 / 12601330, slots 0-5)
        {12601250u, 12605250u, 0u, 2601210},
        {12601251u, 12605251u, 0u, 2601220},         // roof lift (start state reversed, 12601340)
        {12601252u, 12605252u, 0u, 2601230},
        {12601254u, 12605253u, 0u, 2601280},         // roof lift
        {12601255u, 12605254u, 0u, 2601290},         // roof lift
        {12601256u, 12605256u, 0u, 2601240},         // first shortcut lift (28 m, lamp 2602953)
        // the library lift (12601351 / 12601352, after Micolash: the 60 m ride down of the player's
        // report): no busy flag; regions 2602241 / 2602242 in the car (bottom / top), 2602243 / 2602244
        // the call spots. From the top (pos 1): car 6, 3; from the bottom: 8, 5. Its end once the rider
        // is out: pos 1 -> gate 7 / car 9, pos 0; pos 0 -> gate 9 / car 7, pos 1. Init: pos 1 car 2,
        // pos 0 car 4.
        {12601253u, 0u, 0u, 2601260, 2601261, 7, 9, 9, 7, {6, 3}, {8, 5}, 2, 4},
        {12700134u, 12700135u, 0u, 2701032},         // Forbidden Woods
        {12700144u, 12700145u, 0u, 2701042},         // Forbidden Woods
        {12800610u, 12804700u, 12800611u, 2801400},  // Yahar'gul
        {13300115u, 13300116u, 0u, 3301000},         // Nightmare Frontier (liftfall)
        {13501106u, 13504100u, 13501107u, 3501100},  // m35 altar lift (DLC)
        {13501116u, 13504110u, 13501117u, 3501105},  // m35 1F <-> 3F (DLC)
        {13501126u, 13504120u, 13501127u, 3501110},  // m35 3F <-> 5F (DLC)
        {13601106u, 13604100u, 13601107u, 3601100},  // Fishing Hamlet (DLC)
    };
    return lifts;
}

/// liftfix: the lift events whose ride ends once the LOCAL player has left the car (3[2] on entity
/// 10000, "outside"): Cainhurst's (12500076 down / 12500077 up) and the Nightmare of Mensis' library
/// lift (12601352). Every other lift waits until ALL players are out (3[7]). In a party every game
/// runs its copy and the game's own sync passes a condition for every copy once one copy passes it
/// (live, EMEVD traces of both games: the guest's copy went past its "outside" wait 0.1 s after the
/// host's while the guest stood in the car, then its start wait at once): the copy of a player who is
/// not in the car ends every game's ride the moment the car arrives, with the rider still in it, and
/// the restarted copy sees the rider in its start region and sends the car straight back - the rider
/// rides back and forth until it steps out at the right moment (the 0.7.1 full smoke: 5-6 rides for 3;
/// liftfix run 2: 15 for 3). Their end checks become the all-players check of the other lifts.
constexpr std::int32_t kLocalEndRideEvents[] = {12500076, 12500077, 12601352};
inline bool LocalEndRideEvent(std::int32_t event_id) {
    for (const std::int32_t e : kLocalEndRideEvents) {
        if (e == event_id) {
            return true;
        }
    }
    return false;
}

/// liftfix: instruction (bank, id) with `args` (`size` bytes) of event `event_id` is such an end check
/// (3[2]: s8 group, u8 state 0 = outside, s32 entity 10000 = the local player, s32 region): it becomes
/// 3[7] (s8 group, u8 state, s32 region): the region moves to +4. True when `args` was changed (the
/// caller then writes id 7). Start checks (state 1, inside) stay the local player's.
inline bool LocalEndToAllPlayers(std::int32_t event_id, std::int32_t bank, std::int32_t id, std::uint8_t* args,
                                 std::uint32_t size) {
    if (bank != 3 || id != 2 || size < 12 || !LocalEndRideEvent(event_id) || args[1] != 0) {
        return false;
    }
    std::int32_t entity = 0, region = 0;
    std::memcpy(&entity, args + 4, 4);
    std::memcpy(&region, args + 8, 4);
    if (entity != 10000 || region <= 0) {
        return false;
    }
    std::memcpy(args + 4, &region, 4);
    std::memset(args + 8, 0, 4);
    return true;
}

/// The lift whose position flag is `pos`, or nullptr.
inline const Lift* LiftByPosFlag(std::uint32_t pos) {
    for (const Lift& l : Lifts()) {
        if (l.pos == pos) {
            return &l;
        }
    }
    return nullptr;
}

/// The lift whose car is map object `car`, or nullptr.
inline const Lift* LiftByCar(std::int32_t car) {
    for (const Lift& l : Lifts()) {
        if (l.car == car) {
            return &l;
        }
    }
    return nullptr;
}

/// What a replaying lift's car animation says: `riding` (one of its ride animations: this game's copy
/// rides, or its car arrived and the copy waits for its player to leave the car; it has not read the
/// flag for its end yet), `at_on` / `at_off` (an end or init pose: the car stands at the end of pos 1 /
/// pos 0), `unknown` (no animation yet, or a lift that does not replay).
enum class CarPhase { unknown, riding, at_on, at_off };
inline CarPhase PhaseOf(const Lift& l, std::int32_t anim) {
    if (!l.Replays() || anim < 0) {
        return CarPhase::unknown;
    }
    for (const std::int16_t a : {l.ride_from_on[0], l.ride_from_on[1], l.ride_from_off[0], l.ride_from_off[1]}) {
        if (a >= 0 && anim == a) {
            return CarPhase::riding;
        }
    }
    if (anim == l.car_end_on || anim == l.init_on) {
        return CarPhase::at_on;
    }
    if (anim == l.car_end_off || anim == l.init_off) {
        return CarPhase::at_off;
    }
    return CarPhase::unknown;
}

/// The car of a replaying lift stands at the end its position flag names (both games after a ride:
/// the same flag and the car at that end; the smoke checks it after every ride).
inline bool CarAgreesWithFlag(const Lift& l, bool pos, std::int32_t anim) {
    return PhaseOf(l, anim) == (pos ? CarPhase::at_on : CarPhase::at_off);
}

/// liftfix: the position flag of a replaying lift changed on this game without its own write (the
/// mirror brought the host's flip, the host took a guest's ride): put back until this game's copy has
/// played its own end? Only while its car shows a ride animation: the copy then still has to read the
/// flag to pick its end (12601352: "goto if 12601253 on" once its player left the car), and a flip
/// would make it play the other end's animation and write the old value back. Once its car shows an end
/// pose the copy has read the flag (or never rode): a flip is the other game's ride (replayed) or the
/// same value its copy writes. (0.7.1 held it while the player stood within a few m of the car: the
/// copy's own end, in the same tick as the host's flip, was put back too, and the guest kept the old
/// end until its next ride: full smoke 0.7.1, "kept at 1 until its end" after the car's anim 9 ended.)
inline bool HoldMirrorFlip(const Lift& l, std::int32_t car_anim) {
    return PhaseOf(l, car_anim) == CarPhase::riding;
}

/// liftfix: a guest's world mirror holds a replaying lift's position flag back (progress::SetMirrorHold:
/// the host's flip waits) while this game's copy of a ride has not written its own end yet: its car
/// shows a ride animation (the copy reads the flag once its player left the car) or an end animation
/// still plays (the copy writes the flag when it stops). Released, the copy's own end stands (the
/// mirror's image may still hold the host's end of the ride before - live run 2 wrote that stale end
/// back over the guest's own); a later change of the host's goes in as usual, and a guest's end that
/// differs from the host's world goes up as its ride report (GuestLiftReport).
inline bool MirrorHold(const Lift& l, std::int32_t car_anim, bool playing) {
    const CarPhase p = PhaseOf(l, car_anim);
    return p == CarPhase::riding || ((p == CarPhase::at_on || p == CarPhase::at_off) && playing);
}

/// What LiftReplayTick does when `pos` changed on this game (and the flip is not held): nothing when
/// the car stands at the new end already (its own copy rode there) or the lift does not replay; else
/// the end animations of that end are played (the car and its gate to the other game's end).
inline bool LiftNeedsReplay(const Lift& l, bool pos_now, std::int32_t car_anim) {
    return l.Replays() && !CarAgreesWithFlag(l, pos_now, car_anim);
}

/// The host's answer to a guest's finished ride (lift_set / lift_clr): `same` the host's world has
/// that end already (its own copy ran: the usual case), `busy` a ride runs in the host's world (its
/// own copy decides), `loaded` the car is loaded in the host's game but its copy did not follow
/// (left alone: setting the flag would not move the car), `apply` the lift's block is not loaded
/// here: the flags are written and the block's init event puts the car at that end when it
/// streams in.
/// liftfall: a lift that `replays` (Lift::Replays) is applied with its car loaded too (the host's
/// LiftReplayTick then puts the car at that end), unless the host stands at it (`occupied`: within
/// a few m of the car, or the car rides: its own copy decides).
enum class LiftSync { apply, same, busy, loaded, occupied };
inline LiftSync HostLiftSync(bool host_pos, bool host_busy, bool car_loaded, bool want, bool replays = false,
                             bool occupied = false) {
    if (host_pos == want) {
        return LiftSync::same;
    }
    if (host_busy) {
        return LiftSync::busy;
    }
    if (car_loaded && replays) {
        return occupied ? LiftSync::occupied : LiftSync::apply;
    }
    return car_loaded ? LiftSync::loaded : LiftSync::apply;
}

/// Guest: a lift position change of its own copy goes to the host once the ride is over (`busy`
/// off) and the flag still reads the value it changed to; false while it runs.
inline bool GuestLiftRideDone(bool pos_now, bool pos_changed_to, bool busy) {
    return !busy && pos_now == pos_changed_to;
}

/// liftfix: the guest's report of a finished ride, per lift: `send` it to the host (the host's world
/// has the other end and this end was not sent yet), `same` (the host's world has it: nothing to
/// send; forget what was sent), `sent` (sent already: the host answered, or will; never again until
/// the host's world agrees or this end changes). `last_sent`: the end sent last (-1 none). 0.7.1 kept a
/// "seen" list of (flag, value) for the whole stay in a map, so a second ride to an end the guest had
/// reported once never went up again (a host with the lift's block streamed out kept the old end).
enum class GuestReport { wait, same, send, sent };
inline GuestReport GuestLiftReport(bool pos_now, bool host_pos, bool busy, int last_sent) {
    if (busy) {
        return GuestReport::wait;
    }
    if (pos_now == host_pos) {
        return GuestReport::same;
    }
    return last_sent == (pos_now ? 1 : 0) ? GuestReport::sent : GuestReport::send;
}

}  // namespace coop::liftrules

// SPDX-License-Identifier: GPL-3.0-or-later
// Seamless join / warp rules (party_seam.cpp; design research/reseam081.md 3).
// Header only, no game access: unit test tests/test_seam_rules.cpp (ninja -C out/gpu seam-rules-test).
//
//  A "parallel follow" (party_parallel_follow): at the host's party lamp travel a guest starts its own
//    load of the same lamp at once instead of waiting in flow state 1 for the host's load and snapshot;
//    ParallelFollowRefusal says when it may not (today's serial path runs then).
//  B "settled drop" (party_settled_drop): the loading screen stays up after the game's InGameStart
//    until two frames in a row came in under 50 ms (the first frames after a load: 150-790 ms in
//    run 1), at most 2 s; SettleWatch decides from the frame-end timestamps.
#pragma once

#include <cmath>
#include <cstdint>

namespace coop::seam {

constexpr double kSettledFrameMs = 50.0;   // a frame at or above this is not "settled"
constexpr int kSettledFrames = 2;          // in a row
constexpr double kSettleCapMs = 2000.0;    // the screen is dropped at the latest then
constexpr double kParallelDestWaitS = 30.0;  // after its own load, at most this long for the host's spot
constexpr double kParallelLoadCapS = 90.0;   // the guest's own load

/// B: fed once per frame end while a dropped screen is held. Start() at the InGameStart clear.
struct SettleWatch {
    bool active = false;
    double start_ms = 0;
    int good = 0;        // frames under kSettledFrameMs in a row
    int frames = 0;      // frames seen since Start
    double worst_ms = 0; // the longest of them
    bool capped = false; // ended by the cap, not by settled frames

    void Start(double now_ms) {
        active = true;
        start_ms = now_ms;
        good = 0;
        frames = 0;
        worst_ms = 0;
        capped = false;
    }
    /// One frame of `frame_ms` ended at `now_ms`. True: drop the screen now (the watch ends).
    bool Frame(double frame_ms, double now_ms) {
        if (!active) return false;
        ++frames;
        if (frame_ms > worst_ms) worst_ms = frame_ms;
        good = (frame_ms >= 0 && frame_ms < kSettledFrameMs) ? good + 1 : 0;
        if (good >= kSettledFrames) {
            active = false;
            return true;
        }
        if (now_ms - start_ms >= kSettleCapMs) {
            active = false;
            capped = true;
            return true;
        }
        return false;
    }
    double HeldMs(double now_ms) const { return now_ms - start_ms; }
};

/// A: what the guest knows at the host's TRAVEL event. Every field false/0 = "fine".
struct FollowCase {
    bool lamp_kind = false;      // TravelKind::Lamp (lamp, Hunter's Dream, headstone) or HuntersMark
    bool alone = false;          // split travel (the guest stays)
    bool host_death = false;     // TravelKind::HostDeath (the vanilla respawn load for everyone)
    bool dead_follow = false;    // this guest follows dead (a wipe)
    bool boss_fight = false;     // a boss fight runs (C7: a real load)
    std::uint32_t lamp = 0;      // the lamp id the load replays (0 / ~0: none)
    bool spot_known = false;     // party_ids knows that lamp's spot
    bool in_world = false;       // world up, no load, own chr, flow state 4
    bool idle = false;           // no re-join / own travel running
    bool hp_left = false;        // own HP > 0
    bool hold_hook = false;      // the InGameStart clear hook is in (the screen can be held)
};

/// Null: the parallel follow may run. Else why not (a log reason; today's path runs).
inline const char* ParallelFollowRefusal(const FollowCase& c) {
    if (!c.hold_hook) return "the loading screen cannot be held (InGameStart hook not installed)";
    if (c.alone) return "the host travels alone";
    if (c.host_death) return "a host death (the vanilla respawn load)";
    if (!c.lamp_kind) return "not a lamp / Dream / headstone / Hunter's Mark travel";
    if (c.dead_follow || !c.hp_left) return "this guest is dead";
    if (c.boss_fight) return "a boss fight runs";
    if (c.lamp == 0 || c.lamp == 0xffffffffu) return "no lamp id";
    if (c.lamp / 100000 == 29) return "a Chalice Dungeon lamp";
    if (!c.spot_known) return "the lamp's spot is not known";
    if (!c.idle) return "a re-join or own travel runs";
    if (!c.in_world) return "not in the world (loading, or not in flow state 4)";
    return nullptr;
}

/// A: after the guest's own load, the host's spot arrives: same block (area + block of the map id)?
inline bool SameBlock(std::uint32_t a, std::uint32_t b) { return (a >> 16) == (b >> 16) && a != 0 && b != 0 &&
                                                                 a != 0xffffffffu && b != 0xffffffffu; }

}  // namespace coop::seam

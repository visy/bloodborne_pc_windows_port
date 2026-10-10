// SPDX-License-Identifier: GPL-3.0-or-later
// Party travel, phase B2 "true seamless" PROTOTYPE (docs/party/b2_seamless.md): the session and
// the remote players survive a host map load. Off unless BB_PARTY_B2=1.
//
// What it does (all sites byte-verified before anything is written; our offsets):
//   K1  0x19471B1 call BeginLeaveSession (stage payload dtor 0x19470F0): no leave while a session
//       is up (role 3/6). Skipped when the XML "Keep session on map reload" retarget is applied.
//   K2  0x1947207 call 0x1E54830 (CSMultiPlayMan reset, same dtor): the human
//       CSMultiPlayerInsTasks are kept (the NPC tasks are still destroyed, as the reset does).
//   K3  0x193C657 call BeginLeaveSession / K4 0x193C689 call 0x15BED10 (in-game stage update,
//       stage-record UID changed): no leave, no presentation reset while a session is up.
//   K5  0x13809B7 call BeginLeaveSession (Lua OnMatchingCheck: a member's chr is missing):
//       suppressed while a B2 re-join is in flight.
//   K6  0x178524F call 0x1317780 (Lua OnBeJoinStart_* after the host's flag snapshot): suppressed
//       while a guest B2 re-join is in flight (the vanilla SummonReloadStart would overwrite the
//       guest's own-world return snapshot 0x16 90/B0/D0 and start its own warp).
//   K7  0x13CDEAE `jne` -> `jmp` in SessionWorldTransition 0x13CDE30, only around the guest's own
//       B2 warp (main thread, written and restored around the call): the guest keeps the room.
//   Driver (main thread, the coop tick):
//     host  after each own load with a session up: per member of the network flow's member list
//           (flow 0x553D6D0 +0x68) re-sends the join snapshot: packet 0x19, 0x10 flags
//           (0x1786050), 0x11 world snapshot (0x1786360), bullets (0x1A04190), the WorldChrMan
//           net-sync queues (0x18F0DB0, 0x18F0E80), NPC summons (0x15BD0E0), packet 0x42.
//           On a guest's "arrived" event it rewinds every remote CSMultiPlayerInsTask whose
//           PlayerIns is gone to STEP_Create (+0x54 = 4) with the create bit (+0xED |= 1).
//     guest on the host's travel event: flow +0x84 = 1 (awaiting the snapshot); when the game
//           applied the host's flags (+0x84 = 2) it sets +0x84 = 3 (host-world load) and replays
//           the host's destination (party_travel ReplayTravelNow) under K7; after the load
//           (+0x84 = 4, set by 0x178D9C0) it rewinds its remote tasks and sends "arrived".
//   Probe (always with BB_PARTY_B2=1): one "B2 load report" line per load: local PlayerIns
//           pointer before / after, CSMultiPlayMan human / NPC tasks (step, mode, flags), flow
//           state, session role - what single-instance runs can check.
//
// Env: default ON in party mode (BB_PARTY); BB_PARTY_B2=0 off (B1 only), =1 on without BB_PARTY;
//      any failed / timed-out guest step falls back to B1 (FallbackToB1); BB_PARTY_B2_FORCE=1 treats "no session" as a session for K2 (to
//      exercise the keep path offline with NPC tasks, single-instance test only).
#pragma once

#include <cstdint>
#include <string>

namespace coop {
struct TravelIntent;
struct GameSnapshot;
} // namespace coop

namespace coop::b2 {

constexpr const char* kEventName = "b2";

bool Requested();
/// PartyInit, after InstallTravelPatches (bbgpu_patch_image, game not running yet).
void Init();
bool Enabled();
/// Host: a member task (mode 1) exists - a guest is still in the session (main thread).
bool HostHasMemberTasks();
/// Main thread, once a frame (the director tick).
void Tick(const GameSnapshot& s);
/// Guest: the host's travel event. True when B2 takes it (B1's replay must not run it).
bool OnHostTravel(const TravelIntent& t);
/// A PartyLink EVENT "b2" from `from_slot` (any thread).
void OnLinkEvent(int from_slot, const std::string& body);
/// An event to send (the director's tick drains it): slot -1 = to the host / broadcast.
bool PopOutgoing(int* slot, std::string* body);

} // namespace coop::b2

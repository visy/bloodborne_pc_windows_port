// SPDX-License-Identifier: GPL-3.0-or-later
// derived from droogie/bbhost src/hle/np.h @8f2746c
// Shared between np_matching2.cpp and np_signaling.cpp.
#pragma once

#include <cstdint>
#include <string>

namespace bbnet::np {

// The SceNpId's online id text (its first 16 bytes).
std::string npid_text(const void* npid);
bool matching2_context_started();
std::string matching2_status();
// Guest: the party host is gone (its game crashed or quit without a word: the party link is
// down, or a restarted host does not know our room). A room we are in as a guest ends for the
// game the way a host leaving ends it: DEAD per member, then ROOM_DESTROYED (0x1104, cause
// LEAVE) - the game returns to its own world. False when there was no such room.
bool party_host_lost(const std::string& why);

// NpSignaling, told by NpMatching2 about room members:
// a member whose address is now in the peer table: connections to it can announce;
void signaling_peer_known(std::uint16_t member_id);
// every connection to this online id is dead (DEAD at +100 ms);
void signaling_peer_dead(const std::string& online_id);
// our room is over: every connection is dead without an event.
void signaling_reset();

}  // namespace bbnet::np

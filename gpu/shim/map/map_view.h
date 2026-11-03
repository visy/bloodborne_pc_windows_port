// SPDX-License-Identifier: GPL-2.0-or-later
// The in-game map: areas with their floors, the lit lamps, this player and the party members, and
// travel to a lit lamp from the map. After Hunter's Atlas by MikeTat1988 (github.com/MikeTat1988/
// HuntersAtlas, commit 06d4021, GPL-2.0-or-later): its geometry model and floor choice (map_core.h) and
// its controller shortcut (bbport_map_controller.*, unchanged); the drawing is ImGui's draw list in the
// overlay (bbport_overlay.cpp), not Atlas's compiled vector renderer.
//
// Off by default: System > Map "Hunter's Atlas map" (bbport.ini map_enabled, launcher "In-game map",
// BB_MAP=0/1). Then M opens and closes it (keyboard), or L1 + R1 + Start held for 0.65 s (pad). The data
// is made from the player's own game files by the launcher (tools/map, scripts/map_prepare.py) into
// <user>\map\*.bbm (BB_MAP_DIR overrides); without it the key shows a notice.
//
// Threads: HandleEvent / PollWindowThread run on the window thread, Draw on the present thread; the
// overlay calls both with its ImGui mutex held, so the state needs no lock of its own. IsOpen / Notice
// are atomics.
#pragma once

#include <cstdint>

union SDL_Event;

namespace BbMapView {

/// The map feature is switched on (the setting; BB_MAP=0/1 over it).
bool Enabled();
/// The map is open: the overlay draws it and holds the game's input.
bool IsOpen();
/// A short notice (no map data, nothing to show) is up: the overlay must draw this frame.
bool NoticeShown();

/// Window thread, every SDL event. `density`: the window's pixel density (mouse positions to the
/// overlay's pixels). True: consumed. Opens on the key / chord, handles everything while open.
bool HandleEvent(const SDL_Event& event, float density);
/// Window thread, once per poll: the controller chord's hold timer.
void PollWindowThread();

/// Present thread, inside an ImGui frame (after NewFrame): the map (when open) or its notice.
void Draw(float display_w, float display_h);

/// Opens / closes (menu rows, tests). Open fails with a notice when the world is not up.
void Open();
void Close();

/// Test hook (BB_MAP_TEST): scripted steps from the smoke harness; see map_view.cpp.
void TestTick();

}  // namespace BbMapView

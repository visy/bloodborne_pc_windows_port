// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the port's settings as pages of the game's own System menu ("Display", "Game effects",
// "Game patches" after "Screen/Sound"), built and drawn by the game's menu code: rows are the
// game's list, toggle and slider rows, texts come from a hook on its message lookup, the values live
// in the port and are applied as the player changes them. BB_GAME_MENU=0 leaves the menu as it is.
#pragma once

#include <cstdint>

namespace BbGameMenu {
/// Installs the hooks (the loader calls it once, before any game code runs, while the image is
/// still writable). Each hooked function is checked byte for byte against the 1.09 eboot.
void PatchImage(unsigned char* image, std::uint64_t size);

/// Screen changes, for the test aids' event-driven menus (test_newgame.cpp): `seq` counts the
/// message lookups of ids the game had not looked up in the last second (a new screen or dialog
/// building its texts); `last_ms` is that last lookup's steady-clock time in ms. Both 0 while the
/// hooks are off.
void TextActivity(std::uint64_t* seq, std::int64_t* last_ms);

/// Applies what the player changed on the port's pages (each presented frame).
void Poll();
} // namespace BbGameMenu

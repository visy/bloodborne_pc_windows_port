// SPDX-License-Identifier: GPL-2.0-or-later
// Mouse & keyboard mode: the mouse buttons the game pad sees (window.cpp -> runtime_pad.c).
// Pure rules, unit tested (tests/test_mouse_buttons.cpp). Bits: 0 left (R1), 1 right, 2 middle,
// 3 X1, 4 X2 (SDL's SDL_BUTTON_MASK layout).
//  - A press and release inside one sample interval (a Steam Deck / Steam Input tap is a single
//    short pulse) still reaches the game for one sample: `tapped` is read once and cleared.
//  - A button the OS no longer reports down is dropped (a release lost while the mouse capture
//    changed hands, e.g. across a menu or a loading screen, would keep R1 held for ever).
#pragma once
#include <cstdint>

namespace BbMouseButtons {

struct Latch {
    std::uint32_t held{};
    std::uint32_t tapped{};
};

inline void Press(Latch& l, std::uint32_t bit) {
    l.held |= bit;
    l.tapped |= bit;
}
inline void Release(Latch& l, std::uint32_t bit) {
    l.held &= ~bit;
}
inline void Clear(Latch& l) {
    l = Latch{};
}
/// The buttons for one sample. A consuming read ends the taps; a second read in the same frame
/// (consume false) leaves them for the next sample.
inline std::uint32_t Read(Latch& l, bool consume) {
    const std::uint32_t out = l.held | l.tapped;
    if (consume) {
        l.tapped = 0;
    }
    return out;
}
/// `os_down`: the buttons the OS reports down now. A held bit the OS does not have is stale.
/// Returns the bits dropped.
inline std::uint32_t Resync(Latch& l, std::uint32_t os_down) {
    const std::uint32_t stale = l.held & ~os_down;
    l.held &= os_down;
    return stale;
}

} // namespace BbMouseButtons

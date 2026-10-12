// SPDX-License-Identifier: GPL-2.0-or-later
// Headless-lite test mode (BB_HEADLESS=1, tools/mp/instances.py --headless N): a game instance
// that keeps its game logic but draws and shows nothing, so a 4-player party test can run two
// rendering and two cheap instances. Off by default; test-only.
//  - the window starts minimised and never takes focus; nothing is presented (Presenter::Present);
//  - draw calls are not recorded (Rasterizer::Draw / DrawIndirect return before the pipeline
//    selection, so no graphics pipeline is compiled either). Compute dispatches, DMA / copies,
//    the PM4 stream itself, fences, labels and memory writes the game waits on run as usual;
//  - instances.py also sets the lowest effects, the upscaler off and BB_DLSS=0 for it.
// What is lost: the pictures (render targets keep whatever they held) and anything the game reads
// back from a draw's output (occlusion results: everything counts as hidden). Game logic,
// networking and the party code are unchanged.
#pragma once

#include <cstdlib>

namespace BbHeadless {

inline bool On() {
    static const bool on = [] {
        const char* v = std::getenv("BB_HEADLESS");
        return v && v[0] == '1';
    }();
    return on;
}

}  // namespace BbHeadless

// SPDX-License-Identifier: GPL-2.0-or-later
// Mouse button rules of mouse & keyboard mode (gpu/shim/mouse_buttons.h): a short tap still reaches
// one sample, a stale held button (lost release) is dropped.
#include <cassert>
#include <cstdio>
#include "gpu/shim/mouse_buttons.h"

int main() {
    using namespace BbMouseButtons;
    constexpr std::uint32_t kLeft = 1u << 0;
    {   // a tap inside one sample interval was lost before: the release cleared the bit unseen
        Latch l;
        Press(l, kLeft);
        Release(l, kLeft);
        assert(Read(l, false) == kLeft);   // a non consuming read keeps it
        assert(Read(l, true) == kLeft);    // the sample sees the tap
        assert(Read(l, true) == 0);        // and only once
    }
    {   // a held button stays held over samples
        Latch l;
        Press(l, kLeft);
        assert(Read(l, true) == kLeft);
        assert(Read(l, true) == kLeft);
        Release(l, kLeft);
        assert(Read(l, true) == 0);
    }
    {   // a release that never came: the OS says up, the button must not stay held
        Latch l;
        Press(l, kLeft);
        assert(Read(l, true) == kLeft);
        assert(Resync(l, 0) == kLeft);
        assert(Read(l, true) == 0);
    }
    {   // a button the OS still holds is kept
        Latch l;
        Press(l, kLeft | 2u);
        (void)Read(l, true);
        assert(Resync(l, 2u) == kLeft);
        assert(Read(l, true) == 2u);
    }
    {   // a tap that is already up in the OS is still delivered once
        Latch l;
        Press(l, kLeft);
        Release(l, kLeft);
        Resync(l, 0);
        assert(Read(l, true) == kLeft);
    }
    std::puts("mouse buttons: ok");
    return 0;
}

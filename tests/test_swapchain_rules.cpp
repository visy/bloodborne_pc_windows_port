// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: swapchain acquire / recreation rules (vk_swapchain_rules.h): a SUBOPTIMAL acquire keeps
// its image, the frame index restarts with new semaphores and never runs past them.
#include <cassert>
#include <cstdio>
#include "gpu/shadps4/video_core/renderer_vulkan/vk_swapchain_rules.h"

using namespace BbSwapchain;

int main() {
    // Safe rules (default).
    assert(ClassifyAcquire(kSuccess, true) == AcquireAction::Use);
    assert(ClassifyAcquire(kSuboptimal, true) == AcquireAction::UseThenRecreate);
    assert(ClassifyAcquire(kErrorOutOfDate, true) == AcquireAction::Recreate);
    assert(ClassifyAcquire(kErrorSurfaceLost, true) == AcquireAction::Recreate);
    assert(ClassifyAcquire(kErrorUnknown, true) == AcquireAction::Recreate);
    assert(ClassifyAcquire(kErrorDeviceLost, true) == AcquireAction::Fatal);
    assert(ClassifyAcquire(kTimeout, true) == AcquireAction::Fatal);
    assert(ClassifyAcquire(kNotReady, true) == AcquireAction::Fatal);
    // BB_SWAPCHAIN_SAFE=0: the 0.7.2 handling (suboptimal: recreate at once).
    assert(ClassifyAcquire(kSuboptimal, false) == AcquireAction::Recreate);
    assert(ClassifyAcquire(kSuccess, false) == AcquireAction::Use);

    // A recreation with fewer images: index 2 of 3 must not index a 2-image array.
    for (unsigned old_index = 0; old_index < 8; ++old_index) {
        for (unsigned count = 1; count < 6; ++count) {
            assert(FrameIndexAfterCreate(old_index, count) < count);
        }
    }
    unsigned index = 0;
    for (int i = 0; i < 10; ++i) {
        index = NextFrameIndex(index, 3);
        assert(index < 3);
    }
    assert(NextFrameIndex(2, 3) == 0);
    assert(NextFrameIndex(0, 3) == 1);
    assert(NextFrameIndex(5, 0) == 0); // no swapchain: no division by zero

    assert(SafeFromEnv(nullptr));
    assert(SafeFromEnv("1"));
    assert(SafeFromEnv(""));
    assert(!SafeFromEnv("0"));
    std::puts("swapchain-rules-test: ok");
    return 0;
}

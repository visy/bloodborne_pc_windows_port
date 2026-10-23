// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: swapchain acquire / recreation rules, kept free of Vulkan types so a unit test can
// check them (tests/test_swapchain_rules.cpp).
//
// Why (GPU device lost on a hybrid-GPU laptop in exclusive fullscreen, 0.7.2 reports): the
// display path changes when exclusive fullscreen starts or ends (alt-tab), and the swapchain is
// then suboptimal or out of date. Three things the recreation did there were outside the Vulkan
// rules:
//  - a SUBOPTIMAL acquire was treated as a failure: the swapchain was recreated at once and the
//    acquired image's semaphore, whose signal was still pending, destroyed;
//  - the frame index was kept across a recreation: with fewer images than before it indexed
//    past the new semaphore array;
//  - vkDeviceWaitIdle ran from the presenter thread while the recording threads submitted to
//    the same queue (BB_ASYNC_SUBMIT), without the queue lock (Scheduler::submit_mutex).
// BB_SWAPCHAIN_SAFE=0 goes back to the earlier acquire handling and unlocked device wait.
#pragma once

#include <cstdint>
#include <cstdlib>

namespace BbSwapchain {

/// VkResult values (vulkan_core.h; vk_swapchain.cpp checks them with static_assert).
enum : int {
    kSuccess = 0,
    kNotReady = 1,
    kTimeout = 2,
    kSuboptimal = 1000001003,
    kErrorOutOfDate = -1000001004,
    kErrorSurfaceLost = -1000000000,
    kErrorUnknown = -13,
    kErrorDeviceLost = -4,
};

enum class AcquireAction {
    Use,             ///< the image is acquired: render and present it
    UseThenRecreate, ///< acquired, but recreate after presenting it (its semaphore will signal)
    Recreate,        ///< nothing acquired: recreate the swapchain, then try again
    Fatal,           ///< anything else (device lost, out of memory): an error
};

inline AcquireAction ClassifyAcquire(int result, bool safe) {
    switch (result) {
    case kSuccess:
        return AcquireAction::Use;
    case kSuboptimal:
        return safe ? AcquireAction::UseThenRecreate : AcquireAction::Recreate;
    case kErrorOutOfDate:
    case kErrorSurfaceLost:
    case kErrorUnknown:
        return AcquireAction::Recreate;
    default:
        return AcquireAction::Fatal;
    }
}

/// The frame index after the swapchain was (re)created with `image_count` images: the
/// semaphores are new, so the count starts again; never past the arrays.
inline std::uint32_t FrameIndexAfterCreate(std::uint32_t /*old_index*/, std::uint32_t /*image_count*/) {
    return 0;
}

/// The next frame index; a zero count (no swapchain) stays at 0 instead of dividing by zero.
inline std::uint32_t NextFrameIndex(std::uint32_t index, std::uint32_t image_count) {
    return image_count ? (index + 1) % image_count : 0;
}

/// BB_SWAPCHAIN_SAFE (default on): the rules above; 0 restores the 0.7.2 behaviour.
inline bool SafeFromEnv(const char* value) {
    return !(value && value[0] == '0');
}

inline bool Safe() {
    static const bool safe = SafeFromEnv(std::getenv("BB_SWAPCHAIN_SAFE"));
    return safe;
}

} // namespace BbSwapchain

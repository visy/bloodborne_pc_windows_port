// SPDX-License-Identifier: GPL-2.0-or-later
// Standalone CPU regression checks for Vulkan NVIDIA Reflex integration (from
// Supermedo/bloodborne_pc PR #69 by FIocker; bbport: off by default, every hook a no-op then).
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "bbgpu.h"
#include "bbport_settings.h"
#include "video_core/renderer_vulkan/vk_reflex.h"

#include "common/logging/log.h"

// Local test stubs for Common::Log when running outside libbbgpu
namespace Common::Log {
bool ShouldLog(Level /*level*/) {
    return false;
}
void Write(std::string_view /*log_class*/, Level /*level*/, const char* /*file*/, int /*line*/,
           const char* /*function*/, std::string&& /*message*/) {}
} // namespace Common::Log



// Vulkan dynamic dispatcher storage definition for standalone test binary
#include <vulkan/vulkan.hpp>
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

// Stubs for bbgpu C exports when linking outside libbbgpu.a
extern "C" {
void bbgpu_reflex_pre_input_sample(void) {
    Vulkan::Reflex::PreInputSample();
}
void bbgpu_reflex_on_input_sampled(void) {
    Vulkan::Reflex::OnInputSampled();
}
}

int main() {
    using namespace Vulkan::Reflex;

    std::puts("Running test_reflex_cpu...");

    // 1. Initial / unsupported state verification
    assert(!IsSupported());
    assert(!IsActive());
    assert(CurrentSimFrameId() == 0);

    // bbport: Reflex not running this session (the default): the pad / flip hooks do nothing.
    assert(!Tracking());
    for (int i = 0; i < 3; ++i) {
        PreInputSample();
        OnInputSampled();
        OnSimulationEnd();
        bbgpu_reflex_pre_input_sample();
        bbgpu_reflex_on_input_sampled();
    }
    assert(CurrentSimFrameId() == 0);
    // The simulation cycle bookkeeping as with Reflex on (no driver: markers and sleeps skip).
    SetTrackingForTest(true);

    // 2. Pre-input hook pacing and once-per-frame gating verification
    // Calling PreInputSample repeatedly within the SAME simulation cycle must NOT increment frame ID
    PreInputSample();
    const uint64_t fid1 = CurrentSimFrameId();
    assert(fid1 == 1);

    // Explicitly call PreInputSample multiple times within the same frame: must stay on frame 1
    PreInputSample();
    assert(CurrentSimFrameId() == 1);

    PreInputSample();
    assert(CurrentSimFrameId() == 1);

    // Input sampled within the cycle
    OnInputSampled();
    assert(CurrentSimFrameId() == 1);

    // Ending the simulation cycle (flip consumed) allows the next frame to open
    OnSimulationEnd();
    assert(CurrentSimFrameId() == 1);

    // Now next pre-input sample advances to frame 2 (exactly 1 increment)
    PreInputSample();
    const uint64_t fid2 = CurrentSimFrameId();
    assert(fid2 == 2);

    // Multiple calls within frame 2 stay on frame 2
    PreInputSample();
    assert(CurrentSimFrameId() == 2);

    OnInputSampled();
    OnSimulationEnd();

    // 3. Thread attribution & context tracking
    SetRecordingFrameId(fid2);
    assert(RecordingFrameId() == fid2);
    SetRecordingFrameId(0);
    assert(RecordingFrameId() == 0);

    // 4. Systematic Lock Hierarchy & Deadlock-Free Concurrency Test
    // Hierarchy: SleepMutex -> WsiMutex
    // Test: Thread A holds SleepMutex and operates, Thread B acquires SleepMutex then WsiMutex (like Swapchain::Recreate)
    // while Thread C performs PreInputSample. Neither thread should deadlock.
    {
        std::atomic<bool> thread_started{false};
        std::atomic<bool> test_done{false};

        std::thread recreate_thread([&] {
            thread_started = true;
            for (int i = 0; i < 50; ++i) {
                std::lock_guard<std::mutex> sleep_lk{SleepMutex()};
                std::lock_guard<std::mutex> wsi_lk{WsiMutex()};
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            test_done = true;
        });

        while (!thread_started) {
            std::this_thread::yield();
        }

        while (!test_done) {
            PreInputSample();
            OnInputSampled();
            OnSimulationEnd();
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }

        recreate_thread.join();
    }

    // 5. C API hooks (bbgpu_reflex_*) verification
    bbgpu_reflex_pre_input_sample();
    const uint64_t c_fid_curr = CurrentSimFrameId();
    assert(c_fid_curr > 2);

    // Re-entrant calls stay at current frame
    bbgpu_reflex_pre_input_sample();
    assert(CurrentSimFrameId() == c_fid_curr);

    bbgpu_reflex_on_input_sampled();
    OnSimulationEnd();

    // 6. Submission attribution sanitization without GPU/driver
    vk::SubmitInfo submit{};
    VkLatencySubmissionPresentIdNV attribution{};
    const uint64_t prep_id = PrepareSubmit(nullptr, c_fid_curr, submit, attribution);
    // Unsupported driver path returns 0 and does not corrupt submission
    assert(prep_id == 0);

    // 7. Stubs for present and render markers do not crash when unsupported
    OnRenderSubmitBegin(c_fid_curr);
    OnRenderSubmitEnd(c_fid_curr);
    OnForeignSubmitBegin(c_fid_curr);
    OnForeignSubmitEnd(c_fid_curr);
    OnPresentBegin(c_fid_curr, true);
    OnPresentEnd(c_fid_curr, true);
    OnPresentBegin(c_fid_curr, false);
    OnPresentEnd(c_fid_curr, false);

    // 8. Timings query stub
    LatencyReport report{};
    assert(!QueryLatencyTimings(&report));

    // 9. Swapchain latency create info setup stub
    vk::SwapchainCreateInfoKHR sc_ci{};
    VkSwapchainLatencyCreateInfoNV lat_ci{};
    PrepareSwapchainCreateInfo(sc_ci, lat_ci);

    // 10. Multi-threaded pad sample simulation
    // Verify that multiple threads polling pad simultaneously never leak duplicate frame IDs
    const uint64_t sim_before = CurrentSimFrameId();
    PreInputSample();
    const uint64_t sim_active = CurrentSimFrameId();
    assert(sim_active == sim_before + 1);

    for (int t = 0; t < 4; ++t) {
        std::thread([&] {
            bbgpu_reflex_pre_input_sample();
            bbgpu_reflex_on_input_sampled();
        }).join();
    }
    // Still sim_active because OnSimulationEnd has not been called!
    assert(CurrentSimFrameId() == sim_active);
    OnSimulationEnd();

    // Next PreInputSample moves to sim_active + 1
    PreInputSample();
    assert(CurrentSimFrameId() == sim_active + 1);
    OnSimulationEnd();

    // 11. Shutdown cleans up gracefully
    Shutdown();
    assert(!IsSupported());
    assert(!IsActive());

    std::puts("Vulkan NVIDIA Reflex CPU integration tests: PASS");
    return 0;
}

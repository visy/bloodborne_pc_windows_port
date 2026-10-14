// SPDX-License-Identifier: GPL-2.0-or-later
// NVIDIA Reflex (VK_NV_low_latency2 rev3) integration for bbport.
// From Supermedo/bloodborne_pc PR #69 by FIocker; off by default (bbport.ini reflex, BB_REFLEX).

#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan::Reflex {

enum class Mode : int {
    Off = 0,
    On = 1,
    Boost = 2
};

struct LatencyReport {
    uint64_t present_id = 0;
    uint64_t input_sample_us = 0;
    uint64_t sim_start_us = 0;
    uint64_t sim_end_us = 0;
    uint64_t render_submit_start_us = 0;
    uint64_t render_submit_end_us = 0;
    uint64_t present_start_us = 0;
    uint64_t present_end_us = 0;
    uint64_t gpu_render_start_us = 0;
    uint64_t gpu_render_end_us = 0;
};

/// Outer lifecycle mutex protecting sleep semaphore and swapchain handle lifetime.
/// Must be acquired BEFORE WsiMutex whenever both are taken.
std::mutex& SleepMutex();

/// Mutex to serialize WSI operations (acquire, present, swapchain recreation) with driver sleep.
/// Never held across CPU sleeps or marker operations.
std::mutex& WsiMutex();

/// Device extension and feature configuration before vkCreateDevice.
bool ConfigureDevice(vk::PhysicalDevice phys, std::vector<const char*>& exts,
                     vk::PhysicalDeviceVulkan12Features& features12,
                     vk::PhysicalDevicePresentIdFeaturesKHR& present_id);

/// Lifecycle on Vulkan device.
void Initialize(vk::Instance instance, vk::PhysicalDevice phys, vk::Device device, bool extensions_enabled);
void Shutdown();

/// Reflex runs this session (supported, chosen for this start): every hook returns at once
/// otherwise.
bool Tracking();
/// CPU tests without a Vulkan device: the simulation cycle bookkeeping on or off.
void SetTrackingForTest(bool on);

bool IsSupported();
bool IsActive();
Mode CurrentMode();
void SetMode(Mode mode, uint32_t minimum_interval_us = 0);

/// Swapchain creation pNext chain setup (attaches VkSwapchainLatencyCreateInfoNV when active/supported)
void PrepareSwapchainCreateInfo(vk::SwapchainCreateInfoKHR& create_info,
                                VkSwapchainLatencyCreateInfoNV& latency_info);

/// Swapchain and present queue lifecycle
void OnSwapchainCreated(vk::SwapchainKHR swapchain);
void OnSwapchainDestroying(vk::SwapchainKHR old_swapchain);
void OnPresentQueueCreated(vk::Queue present_queue);

/// Attaches Reflex present ID attribution to a submission.
/// Safe to call on any queue; returns the effective frame ID (or 0 if stripped/unsupported).
uint64_t PrepareSubmit(vk::Queue queue, uint64_t frame_id, vk::SubmitInfo& submit,
                       VkLatencySubmissionPresentIdNV& attribution);

/// Thread Attribution & Context Tracking
uint64_t CurrentSimFrameId();
void SetRecordingFrameId(uint64_t frame_id);
uint64_t RecordingFrameId();

/// Simulation & Input Tracing
uint64_t OnFrameBegin();
void PreInputSample();
void OnInputSampled();
/// Diagnostic CPU pad-sample to present-call interval for an explicitly owned
/// guest flip. Excludes driver queuing, scanout and physical input devices.
void TraceCpuPresent(uint64_t content_frame_id);
void OnSimulationEnd();

/// Render Queue Attribution
void OnRenderSubmitBegin(uint64_t frame_id);
void OnRenderSubmitEnd(uint64_t frame_id);
void OnForeignSubmitBegin(uint64_t frame_id);
void OnForeignSubmitEnd(uint64_t frame_id);

/// Present Tracking
void OnPresentBegin(uint64_t frame_id, bool is_real);
void OnPresentEnd(uint64_t frame_id, bool is_real);

/// Latency diagnostic query
bool QueryLatencyTimings(LatencyReport* out_report);

} // namespace Vulkan::Reflex

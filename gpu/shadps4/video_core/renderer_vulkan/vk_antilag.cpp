// SPDX-License-Identifier: GPL-2.0-or-later
// AMD Anti-Lag 2 (VK_AMD_anti_lag). Compile-checked only on the development PC (NVIDIA GPU): the
// calls follow the extension's specification and AMD's Anti-Lag 2 SDK sequence (an INPUT update
// before the game samples input, a PRESENT update with the same frame index at present).
#include "video_core/renderer_vulkan/vk_antilag.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "bbport_settings.h"
#include "common/logging/log.h"

namespace Vulkan::AntiLag {

namespace {
std::atomic<bool> g_active{false};
VkDevice g_device = VK_NULL_HANDLE;
PFN_vkAntiLagUpdateAMD g_update = nullptr;
std::mutex g_mu;
std::uint64_t g_index = 0;        // the frame being simulated
std::uint64_t g_last_closed = 0;  // the newest frame whose flip was accepted
bool g_open = false;

void Update(VkAntiLagStageAMD stage, std::uint64_t index) {
    VkAntiLagPresentationInfoAMD pres{};
    pres.sType = VK_STRUCTURE_TYPE_ANTI_LAG_PRESENTATION_INFO_AMD;
    pres.stage = stage;
    pres.frameIndex = index;
    VkAntiLagDataAMD data{};
    data.sType = VK_STRUCTURE_TYPE_ANTI_LAG_DATA_AMD;
    data.mode = VK_ANTI_LAG_MODE_ON_AMD;
    data.maxFPS = 0; // the presenter / pacer hold the frame limit
    data.pPresentationInfo = &pres;
    g_update(g_device, &data);
}
} // namespace

bool ConfigureDevice(vk::PhysicalDevice phys, std::vector<const char*>& exts) {
    auto [res, available] = phys.enumerateDeviceExtensionProperties();
    if (res != vk::Result::eSuccess) return false;
    bool found = false;
    for (const auto& prop : available) {
        found |= std::strcmp(prop.extensionName.data(), VK_AMD_ANTI_LAG_EXTENSION_NAME) == 0;
    }
    if (!found) return false;
    auto chain = phys.getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceAntiLagFeaturesAMD>();
    if (!chain.get<vk::PhysicalDeviceAntiLagFeaturesAMD>().antiLag) return false;
    exts.push_back(VK_AMD_ANTI_LAG_EXTENSION_NAME);
    return true;
}

void Initialize(vk::Device device, bool enabled) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_active.store(false);
    if (!enabled) return;
    auto get_proc = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;
    g_device = static_cast<VkDevice>(device);
    g_update = get_proc ? reinterpret_cast<PFN_vkAntiLagUpdateAMD>(get_proc(g_device, "vkAntiLagUpdateAMD")) : nullptr;
    if (!g_update) {
        std::printf("Anti-Lag 2: vkAntiLagUpdateAMD missing; off\n");
        return;
    }
    g_active.store(true, std::memory_order_release);
    std::printf("Anti-Lag 2: VK_AMD_anti_lag enabled\n");
}

void Shutdown() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_active.store(false);
    g_update = nullptr;
    g_device = VK_NULL_HANDLE;
}

bool Active() {
    return g_active.load(std::memory_order_relaxed);
}

void OnInput() {
    if (!Active()) return;
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_open || !g_update) return;
    g_open = true;
    Update(VK_ANTI_LAG_STAGE_INPUT_AMD, ++g_index);
}

void OnFlip() {
    if (!Active()) return;
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_open) g_last_closed = g_index;
    g_open = false;
}

void OnPresent() {
    if (!Active()) return;
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_update && g_last_closed) Update(VK_ANTI_LAG_STAGE_PRESENT_AMD, g_last_closed);
}

} // namespace Vulkan::AntiLag

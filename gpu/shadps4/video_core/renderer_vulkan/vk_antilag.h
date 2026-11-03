// SPDX-License-Identifier: GPL-2.0-or-later
// AMD Anti-Lag 2 (VK_AMD_anti_lag) for bbport (0.7.8). Same shape as Reflex: the driver paces the CPU
// so the game reads input as late as the GPU allows. Off unless the GPU is AMD (auto) or the player
// chose it (bbport.ini anti_lag, BB_ANTI_LAG), and the driver has the extension and its feature.
#pragma once

#include <cstdint>
#include <vector>

#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan::AntiLag {

/// Device extension and feature, before vkCreateDevice: true when the driver offers both.
bool ConfigureDevice(vk::PhysicalDevice phys, std::vector<const char*>& exts);
/// After vkCreateDevice. `enabled`: the extension and feature were enabled on the device.
void Initialize(vk::Device device, bool enabled);
void Shutdown();
/// Runs this session: every hook returns at once otherwise.
bool Active();

/// Before the game reads the pad: opens a frame once per simulation frame (INPUT stage).
void OnInput();
/// The game's flip was accepted: the frame's input phase is over.
void OnFlip();
/// A real present is about to be queued (PRESENT stage of the newest finished frame).
void OnPresent();

} // namespace Vulkan::AntiLag

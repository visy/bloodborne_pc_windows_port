// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include "common/assert.h"
#include "common/logging/log.h"
#include "core/emulator_settings.h"
#include "bbport_hdr.h"
#include "bbport_settings.h"
#include "imgui/renderer/imgui_core.h"
#include "sdl_window.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/renderer_vulkan/vk_reflex.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_swapchain_rules.h"

#include <mutex>

namespace Vulkan {

static_assert(BbSwapchain::kSuccess == VK_SUCCESS && BbSwapchain::kSuboptimal == VK_SUBOPTIMAL_KHR &&
              BbSwapchain::kErrorOutOfDate == VK_ERROR_OUT_OF_DATE_KHR &&
              BbSwapchain::kErrorSurfaceLost == VK_ERROR_SURFACE_LOST_KHR &&
              BbSwapchain::kErrorUnknown == VK_ERROR_UNKNOWN &&
              BbSwapchain::kErrorDeviceLost == VK_ERROR_DEVICE_LOST);

namespace {
// bbport: NVIDIA Reflex (from Supermedo/bloodborne_pc PR #69 by FIocker): its driver sleep,
// markers and swapchain lifetime are serialized with acquire / present / recreation. Without
// Reflex this session nothing is locked.
struct ReflexWsiLock {
    std::unique_lock<std::mutex> sleep, wsi;
    ReflexWsiLock(bool reflex, bool lifetime) {
        if (!reflex) return;
        if (lifetime) sleep = std::unique_lock{Reflex::SleepMutex()};
        wsi = std::unique_lock{Reflex::WsiMutex()};
    }
};
} // namespace

static constexpr vk::SurfaceFormatKHR SURFACE_FORMAT_HDR = {
    .format = vk::Format::eA2B10G10R10UnormPack32,
    .colorSpace = vk::ColorSpaceKHR::eHdr10St2084EXT,
};

Swapchain::Swapchain(const Instance& instance_, const Frontend::WindowSDL& window_)
    : instance{instance_}, window{window_}, surface{CreateSurface(instance.GetInstance(), window)} {
    FindPresentFormat();
    FindPresentMode();

    Create(window.GetWidth(), window.GetHeight());
    ImGui::Core::Initialize(instance, window, image_count, surface_format.format);
}

Swapchain::~Swapchain() {
    {
        std::unique_lock<std::mutex> queue_lock;
        if (BbSwapchain::Safe()) queue_lock = std::unique_lock{Scheduler::submit_mutex};
        ReflexWsiLock lock{instance.IsReflexSupported(), true};
        Destroy();
    }
    instance.GetInstance().destroySurfaceKHR(surface);
}

void Swapchain::Create(u32 width_, u32 height_) {
    ReflexWsiLock lock{instance.IsReflexSupported(), true};
    width = width_;
    height = height_;
    needs_recreation = false;

    // bbport: a minimised window has a 0x0 surface (issue #31); no swapchain can have that extent.
    // The old one stays (presents are skipped) until the window is back.
    SetSurfaceProperties();
    if (extent.width == 0 || extent.height == 0) {
        zero_extent = true;
        needs_recreation = true;
        return;
    }
    zero_extent = false;

    // bbport: the old swapchain is retired through oldSwapchain instead of destroyed first.
    // NVIDIA's Wayland WSI keeps a wp_tearing_control per swapchain; a fresh swapchain on the
    // same surface was rejected ("Surface already has a tearing controller", SurfaceLost).
    const vk::SwapchainKHR old_swapchain = swapchain;
    swapchain = nullptr;
    Destroy();

    const std::array queue_family_indices = {
        instance.GetGraphicsQueueFamilyIndex(),
        instance.GetPresentQueueFamilyIndex(),
    };

    const bool exclusive = queue_family_indices[0] == queue_family_indices[1];
    const u32 queue_family_indices_count = exclusive ? 1u : 2u;
    const vk::SharingMode sharing_mode =
        exclusive ? vk::SharingMode::eExclusive : vk::SharingMode::eConcurrent;
    const auto format = needs_hdr ? SURFACE_FORMAT_HDR : surface_format;
    vk::SwapchainCreateInfoKHR swapchain_info = {
        .surface = surface,
        .minImageCount = image_count,
        .imageFormat = format.format,
        .imageColorSpace = format.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = vk::ImageUsageFlagBits::eColorAttachment |
                      vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst,
        .imageSharingMode = sharing_mode,
        .queueFamilyIndexCount = queue_family_indices_count,
        .pQueueFamilyIndices = queue_family_indices.data(),
        .preTransform = transform,
        .compositeAlpha = composite_alpha,
        .presentMode = present_mode,
        .clipped = true,
        .oldSwapchain = old_swapchain,
    };
    VkSwapchainLatencyCreateInfoNV latency_info{};
    if (instance.IsReflexSupported()) {
        Reflex::PrepareSwapchainCreateInfo(swapchain_info, latency_info);
    }

    auto [swapchain_result, chain] = instance.GetDevice().createSwapchainKHR(swapchain_info);
    ASSERT_MSG(swapchain_result == vk::Result::eSuccess, "Failed to create swapchain: {}",
               vk::to_string(swapchain_result));
    swapchain = chain;
    if (old_swapchain) {
        if (instance.IsReflexSupported()) Reflex::OnSwapchainDestroying(old_swapchain);
        instance.GetDevice().destroySwapchainKHR(old_swapchain);
    }
    if (instance.IsReflexSupported()) Reflex::OnSwapchainCreated(swapchain);

    SetupImages();
    RefreshSemaphores();
    // bbport: new semaphores, maybe fewer images: the old index could point past the arrays.
    frame_index = BbSwapchain::FrameIndexAfterCreate(frame_index, image_count);
    if (hdr_output) {
        // A new swapchain has no metadata yet.
        const float peak = metadata_peak, paper = metadata_paper;
        metadata_peak = metadata_paper = 0.0f;
        if (peak > 0.0f) SetHdrMetadata(peak, paper);
    }
}

void Swapchain::SetHdrMetadata(float peak_nits, float paper_white_nits) {
    if (!hdr_output || !swapchain || !instance.IsHdrMetadataSupported() ||
        (peak_nits == metadata_peak && paper_white_nits == metadata_paper)) {
        return;
    }
    metadata_peak = peak_nits;
    metadata_paper = paper_white_nits;
    // The content: Rec.2020 primaries, D65 white; its light level is what the curve can reach.
    const vk::HdrMetadataEXT metadata{
        .displayPrimaryRed = {0.708f, 0.292f},
        .displayPrimaryGreen = {0.170f, 0.797f},
        .displayPrimaryBlue = {0.131f, 0.046f},
        .whitePoint = {0.3127f, 0.3290f},
        .maxLuminance = peak_nits,
        .minLuminance = 0.0f,
        .maxContentLightLevel = peak_nits,
        .maxFrameAverageLightLevel = std::min(peak_nits, paper_white_nits),
    };
    instance.GetDevice().setHdrMetadataEXT(swapchain, metadata);
    std::printf("HDR: metadata peak %.0f nits, frame average %.0f nits\n", peak_nits,
                std::min(peak_nits, paper_white_nits));
}

void Swapchain::Recreate(u32 width_, u32 height_) {
    LOG_DEBUG(Render_Vulkan, "Recreate the swapchain: width={} height={} HDR={}", width_, height_,
              needs_hdr);
    // bbport: Destroy waits for the queues, which the recording threads submit to: under their
    // lock (vkQueueWaitIdle / vkDeviceWaitIdle need the queues externally synchronized). The
    // callers must not hold it (vk_swapchain_rules.h).
    std::unique_lock<std::mutex> queue_lock;
    if (BbSwapchain::Safe()) queue_lock = std::unique_lock{Scheduler::submit_mutex};
    Create(width_, height_);
}

void Swapchain::SetHDR(bool hdr) {
    if (needs_hdr == hdr) {
        return;
    }

    if (!BbSwapchain::Safe()) { // Recreate waits for the queues (under their lock) itself
        auto result = instance.GetDevice().waitIdle();
        if (result != vk::Result::eSuccess) {
            LOG_WARNING(ImGui, "Failed to wait for Vulkan device idle on mode change: {}",
                        vk::to_string(result));
        }
    }

    needs_hdr = hdr;
    Recreate(width, height);
    ImGui::Core::OnSurfaceFormatChange(needs_hdr ? SURFACE_FORMAT_HDR.format
                                                 : surface_format.format);
}

bool Swapchain::AcquireNextImage() {
    ReflexWsiLock lock{instance.IsReflexSupported(), false};
    vk::Device device = instance.GetDevice();
    vk::Result result =
        device.acquireNextImageKHR(swapchain, std::numeric_limits<u64>::max(),
                                   image_acquired[frame_index], VK_NULL_HANDLE, &image_index);

    // bbport: a SUBOPTIMAL acquire has the image (its semaphore will be signalled): it is
    // presented and the swapchain recreated after that (Present returns false), instead of
    // destroying the semaphore with its signal pending (vk_swapchain_rules.h).
    switch (BbSwapchain::ClassifyAcquire(static_cast<int>(result), BbSwapchain::Safe())) {
    case BbSwapchain::AcquireAction::Use:
        return true;
    case BbSwapchain::AcquireAction::UseThenRecreate:
        needs_recreation = true;
        return true;
    case BbSwapchain::AcquireAction::Recreate:
        needs_recreation = true;
        return false;
    case BbSwapchain::AcquireAction::Fatal:
        break;
    }
    LOG_CRITICAL(Render_Vulkan, "Swapchain acquire returned unknown result {}",
                 vk::to_string(result));
    UNREACHABLE();
    return false;
}

bool Swapchain::Present() {
    ReflexWsiLock lock{instance.IsReflexSupported(), false};
    const vk::PresentInfoKHR present_info = {
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &present_ready[image_index],
        .swapchainCount = 1,
        .pSwapchains = &swapchain,
        .pImageIndices = &image_index,
    };

    auto result = instance.GetPresentQueue().presentKHR(present_info);
    if (result == vk::Result::eErrorOutOfDateKHR || result == vk::Result::eSuboptimalKHR) {
        needs_recreation = true;
    } else {
        ASSERT_MSG(result == vk::Result::eSuccess, "Swapchain presentation failed: {}",
                   vk::to_string(result));
    }

    frame_index = BbSwapchain::NextFrameIndex(frame_index, image_count);

    return !needs_recreation;
}

void Swapchain::FindPresentFormat() {
    const auto [formats_result, formats] =
        instance.GetPhysicalDevice().getSurfaceFormatsKHR(surface);
    ASSERT_MSG(formats_result == vk::Result::eSuccess, "Failed to query surface formats: {}",
               vk::to_string(formats_result));

    // Check if the device supports HDR formats. Here we care of Rec.2020 PQ only as it is expected
    // game output. Other variants as e.g. linear Rec.2020 will require additional color space
    // rotation
    supports_hdr =
        std::find_if(formats.begin(), formats.end(), [](const vk::SurfaceFormatKHR& format) {
            return format == SURFACE_FORMAT_HDR;
        }) != formats.end();
    // Also make sure that user allowed us to use HDR
    supports_hdr &= EmulatorSettings.IsHdrAllowed();

    // bbport: what the display offers (with VK_EXT_swapchain_colorspace, HDR colour spaces too).
    {
        std::string offered;
        for (const auto& f : formats) {
            offered += " " + vk::to_string(f.format) + "/" + vk::to_string(f.colorSpace);
        }
        std::printf("Swapchain: formats offered:%s\n", offered.c_str());
    }
    // bbport: native HDR output (bbport_hdr.h): chosen in the settings, HDR on in Windows (or
    // BB_HDR_FORCE=1 for tests: Windows then maps the HDR picture down), and an HDR10 or scRGB
    // format offered. Otherwise the SDR path below, with the reason in the log and the menu.
    auto& settings = BbSettings::Get();
    if (settings.startup_hdr) {
        const char* force = std::getenv("BB_HDR_FORCE");
        const char* want = std::getenv("BB_HDR_FORMAT");
        const bool scrgb_first = want && !std::strcmp(want, "scrgb");
        const auto find = [&](vk::Format format, vk::ColorSpaceKHR space) {
            return std::ranges::find(formats, vk::SurfaceFormatKHR{format, space});
        };
        const std::array pq_formats{vk::Format::eA2B10G10R10UnormPack32,
                                    vk::Format::eA2R10G10B10UnormPack32,
                                    vk::Format::eR16G16B16A16Sfloat, vk::Format::eR16G16B16A16Unorm};
        auto chosen = formats.end();
        int encoding = BbHdr::Sdr;
        const auto try_scrgb = [&] {
            if (chosen != formats.end()) return;
            chosen = find(vk::Format::eR16G16B16A16Sfloat, vk::ColorSpaceKHR::eExtendedSrgbLinearEXT);
            if (chosen != formats.end()) encoding = BbHdr::ScRgb;
        };
        if (scrgb_first) try_scrgb();
        for (const vk::Format format : pq_formats) {
            if (chosen != formats.end()) break;
            chosen = find(format, vk::ColorSpaceKHR::eHdr10St2084EXT);
            if (chosen != formats.end()) encoding = BbHdr::Pq;
        }
        try_scrgb();
        const bool display_hdr = window.IsOnHdrDisplay() || (force && force[0] == '1');
        if (!display_hdr) {
            settings.hdr_problem = "HDR is off in Windows for this display (Settings > Display > Use HDR)";
        } else if (chosen == formats.end()) {
            settings.hdr_problem = "the driver offers no HDR10 or scRGB swapchain for this display";
        } else {
            surface_format = *chosen;
            hdr_output = encoding;
            settings.hdr_active = encoding;
            settings.hdr_problem = nullptr;
            std::printf("HDR: output on, %s %s%s\n", vk::to_string(chosen->format).c_str(),
                        vk::to_string(chosen->colorSpace).c_str(),
                        window.IsOnHdrDisplay() ? "" : " (forced: HDR is off in Windows)");
            return;
        }
        std::printf("HDR: output asked for but not used: %s; presenting SDR\n",
                    settings.hdr_problem.load());
    }

    // If there is a single undefined surface format, the device doesn't care, so we'll just use
    // RGBA sRGB.
    if (formats[0].format == vk::Format::eUndefined) {
        surface_format.format = vk::Format::eR8G8B8A8Unorm;
        surface_format.colorSpace = vk::ColorSpaceKHR::eSrgbNonlinear;
        return;
    }

    // bbport: on a display Windows runs in HDR the picture is re-encoded for the desktop's sRGB
    // decode (post_process.frag), which leaves the game's 12 darkest 8-bit levels on 4 codes of
    // an 8-bit swapchain. A 10-bit one keeps them apart.
    if (window.IsOnHdrDisplay() && EmulatorSettings.IsHdrGammaEnabled()) {
        const auto it = std::ranges::find(
            formats, vk::SurfaceFormatKHR{vk::Format::eA2B10G10R10UnormPack32,
                                          vk::ColorSpaceKHR::eSrgbNonlinear});
        if (it != formats.end()) {
            surface_format = *it;
            return;
        }
    }

    // Try to find a suitable format.
    for (const vk::SurfaceFormatKHR& sformat : formats) {
        vk::Format format = sformat.format;
        if (format != vk::Format::eR8G8B8A8Unorm && format != vk::Format::eB8G8R8A8Unorm) {
            continue;
        }

        surface_format.format = format;
        surface_format.colorSpace = sformat.colorSpace;
        return;
    }

    UNREACHABLE_MSG("Unable to find required swapchain format!");
}

void Swapchain::FindPresentMode() {
    const auto [modes_result, modes] =
        instance.GetPhysicalDevice().getSurfacePresentModesKHR(surface);
    if (modes_result != vk::Result::eSuccess) {
        LOG_ERROR(Render, "Failed to query available present modes, falling back to Fifo as "
                          "guaranteed supported option.");
        present_mode = vk::PresentModeKHR::eFifo;
        return;
    }

    const auto requested_mode = EmulatorSettings.GetPresentMode();
    if (requested_mode == "Mailbox") {
        present_mode = vk::PresentModeKHR::eMailbox;
    } else if (requested_mode == "Fifo") {
        present_mode = vk::PresentModeKHR::eFifo;
    } else if (requested_mode == "Immediate") {
        present_mode = vk::PresentModeKHR::eImmediate;
    } else {
        LOG_ERROR(Render_Vulkan, "Unknown present mode {}, defaulting to Mailbox.",
                  EmulatorSettings.GetPresentMode());
        present_mode = vk::PresentModeKHR::eMailbox;
    }

    if (std::ranges::find(modes, present_mode) == modes.cend()) {
        // FIFO is guaranteed to be supported by the Vulkan spec.
        constexpr auto fallback = vk::PresentModeKHR::eFifo;
        LOG_WARNING(Render, "Requested present mode {} is not supported, falling back to {}.",
                    vk::to_string(present_mode), vk::to_string(fallback));
        present_mode = fallback;
    }
}

void Swapchain::SetSurfaceProperties() {
    const auto [capabilities_result, capabilities] =
        instance.GetPhysicalDevice().getSurfaceCapabilitiesKHR(surface);
    ASSERT_MSG(capabilities_result == vk::Result::eSuccess,
               "Failed to query surface capabilities: {}", vk::to_string(capabilities_result));

    extent = capabilities.currentExtent;
    if (capabilities.currentExtent.width == std::numeric_limits<u32>::max()) {
        extent.width = std::max(capabilities.minImageExtent.width,
                                std::min(capabilities.maxImageExtent.width, width));
        extent.height = std::max(capabilities.minImageExtent.height,
                                 std::min(capabilities.maxImageExtent.height, height));
    }

    // Select number of images in swap chain, we prefer one buffer in the background to work on
    image_count = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0) {
        image_count = std::min(image_count, capabilities.maxImageCount);
    }

    // Prefer identity transform if possible
    transform = vk::SurfaceTransformFlagBitsKHR::eIdentity;
    if (!(capabilities.supportedTransforms & transform)) {
        transform = capabilities.currentTransform;
    }

    // Opaque is not supported everywhere.
    composite_alpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
    if (!(capabilities.supportedCompositeAlpha & vk::CompositeAlphaFlagBitsKHR::eOpaque)) {
        composite_alpha = vk::CompositeAlphaFlagBitsKHR::eInherit;
    }
}

void Swapchain::Destroy() {
    vk::Device device = instance.GetDevice();
    // bbport: only the graphics and present queues use the swapchain's images and semaphores;
    // the readback queue is submitted to without the queue lock, so the device-wide wait raced
    // with it (vk_swapchain_rules.h). The caller holds Scheduler::submit_mutex (Recreate).
    vk::Result wait_result = vk::Result::eSuccess;
    if (BbSwapchain::Safe()) {
        wait_result = instance.GetGraphicsQueue().waitIdle();
        if (wait_result == vk::Result::eSuccess &&
            instance.GetPresentQueue() != instance.GetGraphicsQueue()) {
            wait_result = instance.GetPresentQueue().waitIdle();
        }
    } else {
        wait_result = device.waitIdle();
    }
    if (wait_result != vk::Result::eSuccess) {
        LOG_WARNING(Render_Vulkan, "Failed to wait for device to become idle: {}",
                    vk::to_string(wait_result));
    }

    for (auto& image_view : images_view) {
        device.destroyImageView(image_view);
    }
    images_view.clear();

    if (swapchain) {
        if (instance.IsReflexSupported()) Reflex::OnSwapchainDestroying(swapchain);
        device.destroySwapchainKHR(swapchain);
    }

    for (const auto& sem : image_acquired) {
        device.destroySemaphore(sem);
    }
    for (const auto& sem : present_ready) {
        device.destroySemaphore(sem);
    }

    image_acquired.clear();
    present_ready.clear();
}

void Swapchain::RefreshSemaphores() {
    const vk::Device device = instance.GetDevice();
    image_acquired.resize(image_count);
    present_ready.resize(image_count);

    for (vk::Semaphore& semaphore : image_acquired) {
        auto [semaphore_result, sem] = device.createSemaphore({});
        ASSERT_MSG(semaphore_result == vk::Result::eSuccess,
                   "Failed to create image acquired semaphore: {}",
                   vk::to_string(semaphore_result));
        semaphore = sem;
    }
    for (vk::Semaphore& semaphore : present_ready) {
        auto [semaphore_result, sem] = device.createSemaphore({});
        ASSERT_MSG(semaphore_result == vk::Result::eSuccess,
                   "Failed to create present ready semaphore: {}", vk::to_string(semaphore_result));
        semaphore = sem;
    }

    for (u32 i = 0; i < image_count; ++i) {
        SetObjectName(device, image_acquired[i], "Swapchain Semaphore: image_acquired {}", i);
        SetObjectName(device, present_ready[i], "Swapchain Semaphore: present_ready {}", i);
    }
}

void Swapchain::SetupImages() {
    vk::Device device = instance.GetDevice();
    auto [images_result, imgs] = device.getSwapchainImagesKHR(swapchain);
    ASSERT_MSG(images_result == vk::Result::eSuccess, "Failed to create swapchain images: {}",
               vk::to_string(images_result));
    images = std::move(imgs);
    image_count = static_cast<u32>(images.size());
    images_view.resize(image_count);
    for (u32 i = 0; i < image_count; ++i) {
        if (images_view[i]) {
            device.destroyImageView(images_view[i]);
        }
        auto [im_view_result, im_view] = device.createImageView(vk::ImageViewCreateInfo{
            .image = images[i],
            .viewType = vk::ImageViewType::e2D,
            .format = needs_hdr ? SURFACE_FORMAT_HDR.format : surface_format.format,
            .subresourceRange =
                {
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .levelCount = 1,
                    .layerCount = 1,
                },
        });
        ASSERT_MSG(im_view_result == vk::Result::eSuccess, "Failed to create image view: {}",
                   vk::to_string(im_view_result));
        images_view[i] = im_view;
    }

    for (u32 i = 0; i < image_count; ++i) {
        SetObjectName(device, images[i], "Swapchain Image {}", i);
        SetObjectName(device, images_view[i], "Swapchain ImageView {}", i);
    }
}

} // namespace Vulkan

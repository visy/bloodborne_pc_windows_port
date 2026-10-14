// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: native HDR output (I4). With an HDR swapchain the frame is no longer blitted into it:
// this pass draws the swapchain image whole from the SDR frame (letterboxed), the inverse tone
// curve and the port's overlay (drawn first into an RGBA8 image of the swapchain's size). See
// gpu/shim/bbport_hdr.h for the mapping and host_shaders/hdr_compose.frag for the shader.
#pragma once

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_fg_images.h"

namespace Vulkan {
class Instance;
}

namespace Vulkan::HostPasses {

class HdrComposePass {
public:
    /// hdr_compose.frag's push constants.
    struct Constants {
        float fit[4]{};          ///< the frame's rectangle in the swapchain (x, y, w, h), pixels
        float pattern[4]{};      ///< the calibration band (left, top, right, bottom), pixels
        float peak = 1000.0f;
        float paper_white = 200.0f;
        float strength = 1.0f;
        float saturation = 1.0f;
        s32 encoding = 1;        ///< BbHdr::Pq or BbHdr::ScRgb
        s32 flags = 0;           ///< FlagFrame | FlagOverlay | FlagPattern
        float background = 0.0f; ///< encoded SDR level where no frame is drawn
        float pad = 0.0f;
    };
    static constexpr s32 FlagFrame = 1, FlagOverlay = 2, FlagPattern = 4;
    /// The overlay's target format (ImGui's pipeline is made for it).
    static constexpr vk::Format OverlayFormat = vk::Format::eR8G8B8A8Unorm;

    void Create(const Instance& instance, vk::Format swapchain_format);
    bool Ready() const {
        return bool(pipeline);
    }

    /// The overlay target at the swapchain's size (made or remade here); layout
    /// ColorAttachmentOptimal after the call, contents undefined (the overlay clears it).
    FgImage* BeginOverlay(vk::CommandBuffer cmdbuf, u32 width, u32 height);

    /// Draws `target` (the swapchain image's view, layout ColorAttachmentOptimal) whole.
    /// `frame_view`: the SDR frame (layout General), or null. `overlay`: whether BeginOverlay's
    /// image holds the overlay this frame (it is moved to ShaderReadOnlyOptimal for the read).
    void Render(vk::CommandBuffer cmdbuf, vk::ImageView frame_view, bool overlay,
                vk::ImageView target, vk::Extent2D extent, Constants constants);

private:
    const Instance* instance{};
    vk::UniquePipeline pipeline{};
    vk::UniquePipelineLayout pipeline_layout{};
    vk::UniqueDescriptorSetLayout desc_set_layout{};
    vk::UniqueSampler sampler{};
    FgImage overlay_image;
    bool overlay_begun = false;
};

} // namespace Vulkan::HostPasses

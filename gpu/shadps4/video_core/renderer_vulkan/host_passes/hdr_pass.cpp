// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: native HDR output (I4), see hdr_pass.h.

#include "video_core/renderer_vulkan/host_passes/hdr_pass.h"

#include "common/assert.h"
#include "video_core/host_shaders/fs_tri_vert.h"
#include "video_core/host_shaders/hdr_compose_frag.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan::HostPasses {

void HdrComposePass::Create(const Instance& instance_, vk::Format swapchain_format) {
    instance = &instance_;
    const vk::Device device = instance->GetDevice();
    const std::array bindings{
        vk::DescriptorSetLayoutBinding{.binding = 0,
                                       .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                                       .descriptorCount = 1,
                                       .stageFlags = vk::ShaderStageFlagBits::eFragment},
        vk::DescriptorSetLayoutBinding{.binding = 1,
                                       .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                                       .descriptorCount = 1,
                                       .stageFlags = vk::ShaderStageFlagBits::eFragment},
    };
    desc_set_layout = Check<"create HDR compose descriptor set layout">(
        device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = u32(bindings.size()),
            .pBindings = bindings.data(),
        }));
    const vk::PushConstantRange push_constants{
        .stageFlags = vk::ShaderStageFlagBits::eFragment,
        .offset = 0,
        .size = sizeof(Constants),
    };
    pipeline_layout = Check<"create HDR compose pipeline layout">(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &*desc_set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_constants,
    }));

    const auto vs_module = CompileSPV(FS_TRI_VERT, device);
    const auto fs_module = CompileSPV(HDR_COMPOSE_FRAG, device);
    ASSERT(vs_module && fs_module);
    SetObjectName(device, fs_module, "hdr_compose.frag");
    const std::array stages{
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eVertex, .module = vs_module, .pName = "main"},
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eFragment, .module = fs_module, .pName = "main"},
    };
    const vk::PipelineRenderingCreateInfo rendering{
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &swapchain_format,
    };
    const vk::PipelineVertexInputStateCreateInfo vertex_input{};
    const vk::PipelineInputAssemblyStateCreateInfo input_assembly{
        .topology = vk::PrimitiveTopology::eTriangleList};
    const vk::Viewport viewport{.width = 1.0f, .height = 1.0f, .maxDepth = 1.0f};
    const vk::Rect2D scissor{.extent = {1, 1}};
    const vk::PipelineViewportStateCreateInfo viewport_state{
        .viewportCount = 1, .pViewports = &viewport, .scissorCount = 1, .pScissors = &scissor};
    const vk::PipelineRasterizationStateCreateInfo raster{
        .polygonMode = vk::PolygonMode::eFill,
        .cullMode = vk::CullModeFlagBits::eNone,
        .frontFace = vk::FrontFace::eClockwise,
        .lineWidth = 1.0f,
    };
    const vk::PipelineMultisampleStateCreateInfo multisample{
        .rasterizationSamples = vk::SampleCountFlagBits::e1};
    const vk::PipelineColorBlendAttachmentState attachment{
        .blendEnable = false,
        .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
    };
    const vk::PipelineColorBlendStateCreateInfo blend{.attachmentCount = 1, .pAttachments = &attachment};
    const std::array dynamic_states{vk::DynamicState::eViewport, vk::DynamicState::eScissor};
    const vk::PipelineDynamicStateCreateInfo dynamic{
        .dynamicStateCount = u32(dynamic_states.size()), .pDynamicStates = dynamic_states.data()};
    pipeline = Check<"create HDR compose pipeline">(device.createGraphicsPipelineUnique(
        {}, vk::GraphicsPipelineCreateInfo{
                .pNext = &rendering,
                .stageCount = u32(stages.size()),
                .pStages = stages.data(),
                .pVertexInputState = &vertex_input,
                .pInputAssemblyState = &input_assembly,
                .pViewportState = &viewport_state,
                .pRasterizationState = &raster,
                .pMultisampleState = &multisample,
                .pColorBlendState = &blend,
                .pDynamicState = &dynamic,
                .layout = *pipeline_layout,
            }));
    device.destroyShaderModule(vs_module);
    device.destroyShaderModule(fs_module);

    sampler = Check<"create HDR compose sampler">(device.createSamplerUnique({
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
    }));
}

FgImage* HdrComposePass::BeginOverlay(vk::CommandBuffer cmdbuf, u32 width, u32 height) {
    overlay_begun = false;
    if (!instance || !overlay_image.Ensure(*instance, width, height, OverlayFormat, false)) {
        return nullptr;
    }
    // The previous present's read (same queue) before this frame's clear and draw.
    const vk::ImageMemoryBarrier2 to_attachment{
        .srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .srcAccessMask = vk::AccessFlagBits2::eNone,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentRead |
                         vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = overlay_image.image,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_attachment});
    overlay_begun = true;
    return &overlay_image;
}

void HdrComposePass::Render(vk::CommandBuffer cmdbuf, vk::ImageView frame_view, bool overlay,
                            vk::ImageView target, vk::Extent2D extent, Constants constants) {
    if (!overlay_begun) {
        // No overlay target this frame: one is still bound (made and cleared here).
        if (!BeginOverlay(cmdbuf, extent.width, extent.height)) {
            return;
        }
        const vk::RenderingAttachmentInfo clear{
            .imageView = overlay_image.view,
            .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .loadOp = vk::AttachmentLoadOp::eClear,
            .storeOp = vk::AttachmentStoreOp::eStore,
        };
        cmdbuf.beginRendering({.renderArea = {{0, 0}, {overlay_image.width, overlay_image.height}},
                               .layerCount = 1,
                               .colorAttachmentCount = 1,
                               .pColorAttachments = &clear});
        cmdbuf.endRendering();
        overlay = false;
    }
    overlay_begun = false;
    const vk::ImageMemoryBarrier2 to_read{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        .image = overlay_image.image,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_read});

    if (overlay) constants.flags |= FlagOverlay;
    if (frame_view) constants.flags |= FlagFrame;
    const std::array infos{
        vk::DescriptorImageInfo{
            .sampler = *sampler,
            .imageView = frame_view ? frame_view : overlay_image.view,
            .imageLayout = frame_view ? vk::ImageLayout::eGeneral
                                      : vk::ImageLayout::eShaderReadOnlyOptimal,
        },
        vk::DescriptorImageInfo{
            .sampler = *sampler,
            .imageView = overlay_image.view,
            .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        },
    };
    const std::array writes{
        vk::WriteDescriptorSet{.dstBinding = 0,
                               .descriptorCount = 1,
                               .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                               .pImageInfo = &infos[0]},
        vk::WriteDescriptorSet{.dstBinding = 1,
                               .descriptorCount = 1,
                               .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                               .pImageInfo = &infos[1]},
    };
    const vk::RenderingAttachmentInfo attachment{
        .imageView = target,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eDontCare,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    cmdbuf.beginRendering({.renderArea = {{0, 0}, extent},
                           .layerCount = 1,
                           .colorAttachmentCount = 1,
                           .pColorAttachments = &attachment});
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline);
    cmdbuf.setViewport(0, vk::Viewport{.width = float(extent.width),
                                       .height = float(extent.height),
                                       .maxDepth = 1.0f});
    cmdbuf.setScissor(0, vk::Rect2D{{0, 0}, extent});
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, *pipeline_layout, 0, writes);
    cmdbuf.pushConstants(*pipeline_layout, vk::ShaderStageFlagBits::eFragment, 0, sizeof(Constants),
                         &constants);
    cmdbuf.draw(3, 1, 0, 0);
    cmdbuf.endRendering();
}

} // namespace Vulkan::HostPasses

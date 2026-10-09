// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <boost/container/small_vector.hpp>

#include "common/serdes.h"
#include "common/assert.h"
#include "shader_recompiler/backend/spirv/emit_spirv_discard_frag.h"
#include "shader_recompiler/backend/spirv/emit_spirv_quad_rect.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

using Shader::Backend::SPIRV::AuxShaderType;

static constexpr std::array LogicalStageToStageBit = {
    vk::ShaderStageFlagBits::eFragment,
    vk::ShaderStageFlagBits::eTessellationControl,
    vk::ShaderStageFlagBits::eTessellationEvaluation,
    vk::ShaderStageFlagBits::eVertex,
    vk::ShaderStageFlagBits::eGeometry,
    vk::ShaderStageFlagBits::eCompute,
};

/// bbport: the create-info structures of a graphics pipeline, built once (BuildState) and used
/// for a monolithic pipeline or split into pipeline libraries. Holds pointers into itself: built
/// in place, never copied or moved.
struct GraphicsPipelineState {
    GraphicsPipelineState() = default;
    GraphicsPipelineState(const GraphicsPipelineState&) = delete;
    GraphicsPipelineState& operator=(const GraphicsPipelineState&) = delete;

    struct Stage {
        vk::ShaderStageFlagBits stage;
        vk::ShaderModule module;          ///< a program's module, or
        const std::vector<u32>* aux_spv;  ///< SPIR-V of an auxiliary shader (module made per use)
    };

    vk::PipelineVertexInputDivisorStateCreateInfo divisor_state{};
    vk::PipelineVertexInputStateCreateInfo vertex_input_info{};
    vk::PipelineInputAssemblyStateCreateInfo input_assembly{};
    vk::PipelineTessellationStateCreateInfo tessellation_state{};
    vk::StructureChain<vk::PipelineRasterizationStateCreateInfo,
                       vk::PipelineRasterizationProvokingVertexStateCreateInfoEXT,
                       vk::PipelineRasterizationDepthClipStateCreateInfoEXT>
        raster_chain{};
    vk::PipelineViewportDepthClipControlCreateInfoEXT clip_control{};
    vk::PipelineViewportStateCreateInfo viewport_info{};
    boost::container::static_vector<vk::DynamicState, 32> dynamic_states;
    vk::PipelineDynamicStateCreateInfo dynamic_info{};
    boost::container::static_vector<Stage, MaxShaderStages> stages;
    vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup_size_ci{
        .requiredSubgroupSize = 64,
    };
    vk::Format depth_format{};
    std::array<vk::Format, Shader::IR::NumRenderTargets> color_formats{};
    std::array<vk::SampleCountFlagBits, AmdGpu::NUM_COLOR_BUFFERS> color_samples{};
    vk::AttachmentSampleCountInfoAMD mixed_samples{};
    vk::PipelineRenderingCreateInfo pipeline_rendering_ci{};
    std::array<vk::PipelineColorBlendAttachmentState, AmdGpu::NUM_COLOR_BUFFERS> attachments{};
    vk::PipelineColorBlendStateCreateInfo color_blending{};
    // Required by spec unless VK_EXT_extended_dynamic_state3 is supported.
    // In practice, we use dynamic state for all of it.
    vk::PipelineDepthStencilStateCreateInfo depth_stencil_info{};
    vk::PipelineMultisampleStateCreateInfo multisampling{};
};

namespace {

/// Fills `st` for the pipeline of `key` (the body of the former GraphicsPipeline constructor,
/// with the serialization support already filled).
void BuildState(const Instance& instance, const GraphicsPipelineKey& key,
                std::span<const Shader::Info*, MaxShaderStages> infos,
                std::span<const vk::ShaderModule> modules,
                const GraphicsPipeline::SerializationSupport& sdata, GraphicsPipelineState& st,
                bool quiet) {
    st.divisor_state = vk::PipelineVertexInputDivisorStateCreateInfo{
        .vertexBindingDivisorCount = static_cast<u32>(sdata.divisors.size()),
        .pVertexBindingDivisors = sdata.divisors.data(),
    };
    st.vertex_input_info = vk::PipelineVertexInputStateCreateInfo{
        .pNext = sdata.divisors.empty() ? nullptr : &st.divisor_state,
        .vertexBindingDescriptionCount = static_cast<u32>(sdata.vertex_bindings.size()),
        .pVertexBindingDescriptions = sdata.vertex_bindings.data(),
        .vertexAttributeDescriptionCount = static_cast<u32>(sdata.vertex_attributes.size()),
        .pVertexAttributeDescriptions = sdata.vertex_attributes.data(),
    };

    const auto topology = LiverpoolToVK::PrimitiveType(key.prim_type);
    st.input_assembly = vk::PipelineInputAssemblyStateCreateInfo{
        .topology = topology,
    };

    const bool is_rect_list = key.prim_type == AmdGpu::PrimitiveType::RectList;
    const bool is_quad_list = key.prim_type == AmdGpu::PrimitiveType::QuadList;
    st.tessellation_state = vk::PipelineTessellationStateCreateInfo{
        .patchControlPoints = is_rect_list ? 3U : (is_quad_list ? 4U : key.patch_control_points),
    };

    // Fields set one by one: assigning whole structures would clear the chain's pNext links.
    auto& raster = st.raster_chain.get<vk::PipelineRasterizationStateCreateInfo>();
    raster.depthClampEnable = key.depth_clamp_enable &&
                              (!key.depth_clip_enable || instance.IsDepthClipEnableSupported());
    raster.rasterizerDiscardEnable = false;
    raster.polygonMode = LiverpoolToVK::PolygonMode(key.polygon_mode);
    raster.lineWidth = 1.0f;
    st.raster_chain.get<vk::PipelineRasterizationProvokingVertexStateCreateInfoEXT>()
        .provokingVertexMode = key.provoking_vtx_last == AmdGpu::ProvokingVtxLast::First
                                   ? vk::ProvokingVertexModeEXT::eFirstVertex
                                   : vk::ProvokingVertexModeEXT::eLastVertex;
    st.raster_chain.get<vk::PipelineRasterizationDepthClipStateCreateInfoEXT>().depthClipEnable =
        key.depth_clip_enable;
    if (!instance.IsProvokingVertexSupported()) {
        st.raster_chain.unlink<vk::PipelineRasterizationProvokingVertexStateCreateInfoEXT>();
    }
    if (!instance.IsDepthClipEnableSupported()) {
        st.raster_chain.unlink<vk::PipelineRasterizationDepthClipStateCreateInfoEXT>();
    }

    st.multisampling = sdata.multisampling;

    st.clip_control = vk::PipelineViewportDepthClipControlCreateInfoEXT{
        .negativeOneToOne = key.clip_space == AmdGpu::ClipSpace::MinusWToW,
    };
    st.viewport_info = vk::PipelineViewportStateCreateInfo{
        .pNext = instance.IsDepthClipControlSupported() ? &st.clip_control : nullptr,
    };

    st.dynamic_states = {
        vk::DynamicState::eViewportWithCount,  vk::DynamicState::eScissorWithCount,
        vk::DynamicState::eBlendConstants,     vk::DynamicState::eDepthTestEnable,
        vk::DynamicState::eDepthWriteEnable,   vk::DynamicState::eDepthCompareOp,
        vk::DynamicState::eDepthBiasEnable,    vk::DynamicState::eDepthBias,
        vk::DynamicState::eStencilTestEnable,  vk::DynamicState::eStencilReference,
        vk::DynamicState::eStencilCompareMask, vk::DynamicState::eStencilWriteMask,
        vk::DynamicState::eStencilOp,          vk::DynamicState::eCullMode,
        vk::DynamicState::eFrontFace,          vk::DynamicState::eRasterizerDiscardEnable,
        vk::DynamicState::eLineWidth,          vk::DynamicState::ePrimitiveRestartEnable,
    };
    if (instance.IsDepthBoundsSupported()) {
        st.dynamic_states.push_back(vk::DynamicState::eDepthBoundsTestEnable);
        st.dynamic_states.push_back(vk::DynamicState::eDepthBounds);
    }
    if (instance.IsDynamicColorWriteMaskSupported()) {
        st.dynamic_states.push_back(vk::DynamicState::eColorWriteMaskEXT);
    }
    if (instance.IsVertexInputDynamicState()) {
        st.dynamic_states.push_back(vk::DynamicState::eVertexInputEXT);
    } else if (!sdata.vertex_bindings.empty()) {
        st.dynamic_states.push_back(vk::DynamicState::eVertexInputBindingStride);
    }
    st.dynamic_info = vk::PipelineDynamicStateCreateInfo{
        .dynamicStateCount = static_cast<u32>(st.dynamic_states.size()),
        .pDynamicStates = st.dynamic_states.data(),
    };

    auto stage = u32(Shader::SwStage::Vertex);
    if (infos[stage]) {
        st.stages.push_back({vk::ShaderStageFlagBits::eVertex, modules[stage], nullptr});
    }
    stage = u32(Shader::SwStage::Geometry);
    if (infos[stage]) {
        st.stages.push_back({vk::ShaderStageFlagBits::eGeometry, modules[stage], nullptr});
    }
    stage = u32(Shader::SwStage::TessellationControl);
    if (infos[stage]) {
        st.stages.push_back({vk::ShaderStageFlagBits::eTessellationControl, modules[stage], nullptr});
    } else if (is_rect_list || is_quad_list) {
        st.stages.push_back({vk::ShaderStageFlagBits::eTessellationControl, {}, &sdata.tcs});
    }
    stage = u32(Shader::SwStage::TessellationEval);
    if (infos[stage]) {
        st.stages.push_back(
            {vk::ShaderStageFlagBits::eTessellationEvaluation, modules[stage], nullptr});
    } else if (is_rect_list || is_quad_list) {
        st.stages.push_back({vk::ShaderStageFlagBits::eTessellationEvaluation, {}, &sdata.tes});
    }
    stage = u32(Shader::SwStage::Fragment);
    if (infos[stage]) {
        st.stages.push_back({vk::ShaderStageFlagBits::eFragment, modules[stage], nullptr});
    } else if (!sdata.fragment.empty()) {
        // bbport: the clip distance discard shader (FillSerializationSupport decided it).
        st.stages.push_back({vk::ShaderStageFlagBits::eFragment, {}, &sdata.fragment});
    }

    st.depth_format =
        instance.GetSupportedFormat(LiverpoolToVK::DepthFormat(key.z_format, key.stencil_format),
                                    vk::FormatFeatureFlagBits2::eDepthStencilAttachment);
    for (s32 i = 0; i < key.num_color_attachments; ++i) {
        const auto& col_buf = key.color_buffers[i];
        const auto format = LiverpoolToVK::SurfaceFormat(col_buf.data_format, col_buf.num_format);
        const auto color_format =
            instance.GetSupportedFormat(format, vk::FormatFeatureFlagBits2::eColorAttachment);
        if (!quiet && !instance.IsFormatSupported(color_format,
                                                  vk::FormatFeatureFlagBits2::eColorAttachment)) {
            LOG_WARNING(Render_Vulkan,
                        "color buffer format {} does not support COLOR_ATTACHMENT_BIT",
                        vk::to_string(color_format));
        }
        st.color_formats[i] = color_format;
    }

    std::ranges::transform(key.color_samples, st.color_samples.begin(), [&instance](u8 num_samples) {
        return num_samples ? LiverpoolToVK::NumSamples(num_samples, instance.GetColorSampleCounts())
                           : vk::SampleCountFlagBits::e1;
    });
    st.mixed_samples = vk::AttachmentSampleCountInfoAMD{
        .colorAttachmentCount = key.num_color_attachments,
        .pColorAttachmentSamples = st.color_samples.data(),
        .depthStencilAttachmentSamples =
            LiverpoolToVK::NumSamples(key.depth_samples, instance.GetDepthSampleCounts()),
    };

    st.pipeline_rendering_ci = vk::PipelineRenderingCreateInfo{
        .pNext = instance.IsMixedDepthSamplesSupported() ? &st.mixed_samples : nullptr,
        .colorAttachmentCount = key.num_color_attachments,
        .pColorAttachmentFormats = st.color_formats.data(),
        .depthAttachmentFormat = key.z_format != AmdGpu::DepthBuffer::ZFormat::Invalid
                                     ? st.depth_format
                                     : vk::Format::eUndefined,
        .stencilAttachmentFormat = key.stencil_format != AmdGpu::DepthBuffer::StencilFormat::Invalid
                                       ? st.depth_format
                                       : vk::Format::eUndefined,
    };

    auto& attachments = st.attachments;
    for (u32 i = 0; i < key.num_color_attachments; i++) {
        const auto& control = key.blend_controls[i];

        const auto src_color = LiverpoolToVK::BlendFactor(control.color_src_factor);
        const auto dst_color = LiverpoolToVK::BlendFactor(control.color_dst_factor);
        const auto color_blend = LiverpoolToVK::BlendOp(control.color_func);

        const auto src_alpha = control.separate_alpha_blend
                                   ? LiverpoolToVK::BlendFactor(control.alpha_src_factor)
                                   : src_color;
        const auto dst_alpha = control.separate_alpha_blend
                                   ? LiverpoolToVK::BlendFactor(control.alpha_dst_factor)
                                   : dst_color;
        const auto alpha_blend =
            control.separate_alpha_blend ? LiverpoolToVK::BlendOp(control.alpha_func) : color_blend;

        // Vulkan ignores blend factors for min/max, but a factor that zeroes one operand
        // makes the operation collapse to a plain selection: min(s, 0) is 0 and max(s, 0)
        // is s for normalized alpha. Rewrite those to the equivalent add so the result is
        // exact instead of leaving the other operand to survive.
        auto eff_src_alpha = src_alpha;
        auto eff_dst_alpha = dst_alpha;
        auto eff_alpha_blend = alpha_blend;
        if (alpha_blend == vk::BlendOp::eMin || alpha_blend == vk::BlendOp::eMax) {
            const bool takes_max = alpha_blend == vk::BlendOp::eMax;
            if (src_alpha == vk::BlendFactor::eOne && dst_alpha == vk::BlendFactor::eZero) {
                eff_alpha_blend = vk::BlendOp::eAdd;
                eff_src_alpha = takes_max ? vk::BlendFactor::eOne : vk::BlendFactor::eZero;
                eff_dst_alpha = vk::BlendFactor::eZero;
            } else if (src_alpha == vk::BlendFactor::eZero && dst_alpha == vk::BlendFactor::eOne) {
                eff_alpha_blend = vk::BlendOp::eAdd;
                eff_src_alpha = vk::BlendFactor::eZero;
                eff_dst_alpha = takes_max ? vk::BlendFactor::eOne : vk::BlendFactor::eZero;
            }
        }

        const auto color_scaled_min_max =
            (color_blend == vk::BlendOp::eMin || color_blend == vk::BlendOp::eMax) &&
            (src_color != vk::BlendFactor::eOne || dst_color != vk::BlendFactor::eOne) &&
            !key.color_buffers[i].blend_self_scale;
        const auto alpha_scaled_min_max =
            (eff_alpha_blend == vk::BlendOp::eMin || eff_alpha_blend == vk::BlendOp::eMax) &&
            (eff_src_alpha != vk::BlendFactor::eOne || eff_dst_alpha != vk::BlendFactor::eOne);
        if (!quiet && (color_scaled_min_max || alpha_scaled_min_max)) {
            LOG_WARNING(
                Render_Vulkan,
                "Unimplemented use of min/max blend op with blend factor not equal to one.");
        }

        attachments[i] = vk::PipelineColorBlendAttachmentState{
            .blendEnable = control.enable,
            .srcColorBlendFactor = src_color,
            .dstColorBlendFactor = dst_color,
            .colorBlendOp = color_blend,
            .srcAlphaBlendFactor = eff_src_alpha,
            .dstAlphaBlendFactor = eff_dst_alpha,
            .alphaBlendOp = eff_alpha_blend,
            .colorWriteMask =
                instance.IsDynamicColorWriteMaskSupported()
                    ? vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA
                    : key.write_masks[i],
        };

        // The shader squares its color output for this attachment (see PsColorBuffer), so the
        // factors must not scale the operands again.
        if (key.color_buffers[i].blend_self_scale) {
            if (!quiet) {
                LOG_WARNING(
                    Render_Vulkan,
                    "Emulating scaled min/max blend with squared shader output on attachment {}",
                    i);
            }
            attachments[i].srcColorBlendFactor = vk::BlendFactor::eOne;
            attachments[i].dstColorBlendFactor = vk::BlendFactor::eOne;
        }

        // On GCN GPU there is an additional mask which allows to control color components exported
        // from a pixel shader. A situation possible, when the game may mask out the alpha channel,
        // while it is still need to be used in blending ops. For such cases, HW will default alpha
        // to 1 and perform the blending, while shader normally outputs 0 in the last component.
        // Unfortunatelly, Vulkan doesn't provide any control on blend inputs, so below we detecting
        // such cases and override alpha value in order to emulate HW behaviour.
        const auto has_alpha_masked_out =
            (key.cb_shader_mask.GetMask(i) & AmdGpu::ColorBufferMask::ComponentA) == 0;
        const auto has_src_alpha_in_src_blend = src_color == vk::BlendFactor::eSrcAlpha ||
                                                src_color == vk::BlendFactor::eOneMinusSrcAlpha;
        const auto has_src_alpha_in_dst_blend = dst_color == vk::BlendFactor::eSrcAlpha ||
                                                dst_color == vk::BlendFactor::eOneMinusSrcAlpha;
        if (has_alpha_masked_out && has_src_alpha_in_src_blend) {
            attachments[i].srcColorBlendFactor = src_color == vk::BlendFactor::eSrcAlpha
                                                     ? vk::BlendFactor::eOne
                                                     : vk::BlendFactor::eZero; // 1-A
        }
        if (has_alpha_masked_out && has_src_alpha_in_dst_blend) {
            attachments[i].dstColorBlendFactor = dst_color == vk::BlendFactor::eSrcAlpha
                                                     ? vk::BlendFactor::eOne
                                                     : vk::BlendFactor::eZero; // 1-A
        }
    }

    st.color_blending = vk::PipelineColorBlendStateCreateInfo{
        .logicOpEnable =
            instance.IsLogicOpSupported() && key.logic_op != AmdGpu::ColorControl::LogicOp::Copy,
        .logicOp = LiverpoolToVK::LogicOp(key.logic_op),
        .attachmentCount = key.num_color_attachments,
        .pAttachments = st.attachments.data(),
        .blendConstants = std::array{1.0f, 1.0f, 1.0f, 1.0f},
    };
}

/// Descriptor bindings of the stages (one set), as BuildDescSetLayout lays them out.
void CollectBindings(std::span<const Shader::Info* const> stages,
                     boost::container::small_vector<vk::DescriptorSetLayoutBinding, 32>& bindings,
                     u32& binding) {
    binding = 0;
    for (const auto* stage : stages) {
        if (!stage) {
            continue;
        }
        const auto stage_bit = LogicalStageToStageBit[u32(stage->sw_stage)];
        for (size_t i = 0; i < stage->buffers.size(); ++i) {
            bindings.push_back({
                .binding = binding++,
                .descriptorType = vk::DescriptorType::eStorageBuffer,
                .descriptorCount = 1,
                .stageFlags = stage_bit,
            });
        }
        for (const auto& image : stage->images) {
            const u32 num_bindings = image.NumBindings(*stage);
            bindings.push_back({
                .binding = binding,
                .descriptorType = image.is_written ? vk::DescriptorType::eStorageImage
                                                   : vk::DescriptorType::eSampledImage,
                .descriptorCount = num_bindings,
                .stageFlags = stage_bit,
            });
            binding += num_bindings;
        }
        for (size_t i = 0; i < stage->samplers.size(); ++i) {
            bindings.push_back({
                .binding = binding++,
                .descriptorType = vk::DescriptorType::eSampler,
                .descriptorCount = 1,
                .stageFlags = stage_bit,
            });
        }
    }
}

/// Byte description of a library's state: the GplLibraryCache key.
struct KeyWriter {
    std::string out;
    template <typename T>
    void Value(const T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        out.append(reinterpret_cast<const char*>(&value), sizeof(value));
    }
    void Bytes(const void* data, size_t size) {
        Value(u64(size));
        out.append(static_cast<const char*>(data), size);
    }
};

/// Keys of the four libraries of a pipeline. `layout_sig` describes its descriptor set layout.
std::array<std::string, GplLibraryCache::NumKinds> LibraryKeys(const Instance& instance,
                                                               const GraphicsPipelineState& st,
                                                               const std::string& layout_sig) {
    std::array<std::string, GplLibraryCache::NumKinds> keys;
    const auto common = [&](KeyWriter& w, u32 kind) {
        w.Value(kind);
        w.Bytes(st.dynamic_states.data(), st.dynamic_states.size() * sizeof(vk::DynamicState));
    };
    const auto multisampling = [&](KeyWriter& w) {
        w.Value(st.multisampling.rasterizationSamples);
        w.Value(st.multisampling.sampleShadingEnable);
        w.Value(st.multisampling.minSampleShading);
        w.Value(st.multisampling.alphaToCoverageEnable);
        w.Value(st.multisampling.alphaToOneEnable);
    };
    const auto shader = [&](KeyWriter& w, const GraphicsPipelineState::Stage& stage) {
        w.Value(stage.stage);
        if (stage.aux_spv) {
            w.Value(u8(1));
            w.Bytes(stage.aux_spv->data(), stage.aux_spv->size() * sizeof(u32));
        } else {
            w.Value(u8(0));
            w.Value(u64(reinterpret_cast<uintptr_t>(VkShaderModule(stage.module))));
        }
    };
    {
        KeyWriter w;
        common(w, GplLibraryCache::VertexInput);
        w.Value(st.input_assembly.topology);
        if (!instance.IsVertexInputDynamicState()) {
            const auto& vi = st.vertex_input_info;
            w.Bytes(vi.pVertexBindingDescriptions,
                    vi.vertexBindingDescriptionCount * sizeof(vk::VertexInputBindingDescription));
            w.Bytes(vi.pVertexAttributeDescriptions, vi.vertexAttributeDescriptionCount *
                                                         sizeof(vk::VertexInputAttributeDescription));
            w.Bytes(st.divisor_state.pVertexBindingDivisors,
                    st.divisor_state.vertexBindingDivisorCount *
                        sizeof(vk::VertexInputBindingDivisorDescriptionEXT));
        }
        keys[GplLibraryCache::VertexInput] = std::move(w.out);
    }
    {
        KeyWriter w;
        common(w, GplLibraryCache::PreRasterization);
        w.out += layout_sig;
        w.Value(st.input_assembly.topology);
        for (const auto& stage : st.stages) {
            if (stage.stage != vk::ShaderStageFlagBits::eFragment) {
                shader(w, stage);
            }
        }
        w.Value(st.tessellation_state.patchControlPoints);
        const auto& raster = st.raster_chain.get<vk::PipelineRasterizationStateCreateInfo>();
        w.Value(raster.depthClampEnable);
        w.Value(raster.polygonMode);
        w.Value(st.raster_chain.get<vk::PipelineRasterizationProvokingVertexStateCreateInfoEXT>()
                    .provokingVertexMode);
        w.Value(st.raster_chain.get<vk::PipelineRasterizationDepthClipStateCreateInfoEXT>()
                    .depthClipEnable);
        w.Value(st.clip_control.negativeOneToOne);
        keys[GplLibraryCache::PreRasterization] = std::move(w.out);
    }
    {
        KeyWriter w;
        common(w, GplLibraryCache::FragmentShader);
        w.out += layout_sig;
        for (const auto& stage : st.stages) {
            if (stage.stage == vk::ShaderStageFlagBits::eFragment) {
                shader(w, stage);
            }
        }
        multisampling(w);
        keys[GplLibraryCache::FragmentShader] = std::move(w.out);
    }
    {
        KeyWriter w;
        common(w, GplLibraryCache::FragmentOutput);
        const auto& rendering = st.pipeline_rendering_ci;
        w.Value(rendering.colorAttachmentCount);
        w.Bytes(st.color_formats.data(), rendering.colorAttachmentCount * sizeof(vk::Format));
        w.Value(rendering.depthAttachmentFormat);
        w.Value(rendering.stencilAttachmentFormat);
        w.Value(u8(rendering.pNext != nullptr));
        w.Bytes(st.color_samples.data(),
                st.mixed_samples.colorAttachmentCount * sizeof(vk::SampleCountFlagBits));
        w.Value(st.mixed_samples.depthStencilAttachmentSamples);
        w.Bytes(st.attachments.data(), st.color_blending.attachmentCount *
                                           sizeof(vk::PipelineColorBlendAttachmentState));
        w.Value(st.color_blending.logicOpEnable);
        w.Value(st.color_blending.logicOp);
        multisampling(w);
        keys[GplLibraryCache::FragmentOutput] = std::move(w.out);
    }
    return keys;
}

std::string LayoutSignature(
    const boost::container::small_vector<vk::DescriptorSetLayoutBinding, 32>& bindings,
    bool push) {
    KeyWriter w;
    w.Value(u8(push));
    for (const auto& b : bindings) {
        w.Value(b.binding);
        w.Value(b.descriptorType);
        w.Value(b.descriptorCount);
        w.Value(b.stageFlags);
    }
    w.Value(u32(bindings.size()));
    return std::move(w.out);
}

/// Descriptor set layout and pipeline layout for `bindings` (the graphics pipeline layout).
void MakeLayouts(const Instance& instance,
                 const boost::container::small_vector<vk::DescriptorSetLayoutBinding, 32>& bindings,
                 bool push, vk::UniqueDescriptorSetLayout& set_layout,
                 vk::UniquePipelineLayout& layout) {
    const auto flags = push ? vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR
                            : vk::DescriptorSetLayoutCreateFlagBits{};
    const vk::DescriptorSetLayoutCreateInfo desc_layout_ci = {
        .flags = flags,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    };
    auto [set_result, set] = instance.GetDevice().createDescriptorSetLayoutUnique(desc_layout_ci);
    ASSERT_MSG(set_result == vk::Result::eSuccess,
               "Failed to create graphics descriptor set layout: {}", vk::to_string(set_result));
    set_layout = std::move(set);

    const vk::PushConstantRange push_constants = {
        .stageFlags = AllGraphicsStageBits,
        .offset = 0,
        .size = sizeof(Shader::PushData),
    };
    const vk::DescriptorSetLayout set_handle = *set_layout;
    const vk::PipelineLayoutCreateInfo layout_info = {
        .setLayoutCount = 1U,
        .pSetLayouts = &set_handle,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_constants,
    };
    auto [layout_result, created] = instance.GetDevice().createPipelineLayoutUnique(layout_info);
    ASSERT_MSG(layout_result == vk::Result::eSuccess,
               "Failed to create graphics pipeline layout: {}", vk::to_string(layout_result));
    layout = std::move(created);
}

/// Shader stage create infos of `st` for the stages in `mask` (auxiliary modules compiled here;
/// like the original code, they are not destroyed).
boost::container::static_vector<vk::PipelineShaderStageCreateInfo, MaxShaderStages> StageInfos(
    const Instance& instance, const GraphicsPipelineState& st, vk::ShaderStageFlags mask) {
    boost::container::static_vector<vk::PipelineShaderStageCreateInfo, MaxShaderStages> out;
    for (const auto& stage : st.stages) {
        if (!(mask & stage.stage)) {
            continue;
        }
        out.push_back(vk::PipelineShaderStageCreateInfo{
            .pNext = instance.IsSubgroupSize64Supported() ? &st.subgroup_size_ci : nullptr,
            .stage = stage.stage,
            .module = stage.aux_spv ? CompileSPV(*stage.aux_spv, instance.GetDevice())
                                    : stage.module,
            .pName = "main",
        });
    }
    return out;
}

constexpr vk::ShaderStageFlags PreRasterStages =
    vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eGeometry |
    vk::ShaderStageFlagBits::eTessellationControl |
    vk::ShaderStageFlagBits::eTessellationEvaluation;

} // namespace

GraphicsPipeline::GraphicsPipeline(
    const Instance& instance, Scheduler& scheduler, DescriptorHeap& desc_heap,
    const Shader::Profile& profile, const GraphicsPipelineKey& key_,
    vk::PipelineCache pipeline_cache, std::span<const Shader::Info*, MaxShaderStages> infos,
    std::span<const Shader::RuntimeInfo, MaxShaderStages> runtime_infos,
    std::optional<const Shader::Gcn::FetchShaderData> fetch_shader_,
    std::span<const vk::ShaderModule> modules, SerializationSupport& sdata, BuildMode mode,
    GplLibraryCache* gpl)
    : Pipeline{instance, scheduler, desc_heap, profile, pipeline_cache}, key{key_},
      fetch_shader{std::move(fetch_shader_)}, vk_pipeline_cache{pipeline_cache} {
    const vk::Device device = instance.GetDevice();
    std::ranges::copy(infos, stages.begin());
    if (mode == BuildMode::Live) {
        FillSerializationSupport(instance, key, infos, runtime_infos, fetch_shader, sdata);
    }
    BuildDescSetLayout();
    const auto debug_str = GetDebugString();
    SetObjectName(device, *pipeline_layout, "Graphics PipelineLayout {}", debug_str);

    GraphicsPipelineState state{};
    BuildState(instance, key, infos, modules, sdata, state, false);

    vk::UniquePipeline pipe;
    vk::Result result = vk::Result::eErrorUnknown;
    if (gpl) {
        result = CreateFromLibraries(state, *gpl, pipe);
        if (result != vk::Result::eSuccess) {
            ++gpl->num_failures;
            LOG_WARNING(Render_Vulkan, "Pipeline library link failed ({}), monolithic pipeline",
                        vk::to_string(result));
            libraries = {};
            fast_linked = false;
        }
    }
    if (!pipe) {
        result = CreateMonolithic(state, pipe);
    }
    if (mode != BuildMode::Live && result != vk::Result::eSuccess) {
        // bbport: a pipeline from the cache the driver rejects is a damaged cache entry: the
        // cache is rebuilt (PipelineCache::WarmUp) instead of stopping the game at every start.
        // Async builds fail the same way and are built again on the GPU thread.
        throw Serialization::CorruptData{"graphics pipeline rejected by the driver: " +
                                         vk::to_string(result)};
    }
    ASSERT_MSG(result == vk::Result::eSuccess, "Failed to create graphics pipeline: {}",
               vk::to_string(result));
    pipeline = std::move(pipe);
    handle.store(*pipeline, std::memory_order_release);
    SetObjectName(device, *pipeline, "Graphics Pipeline {}", debug_str);
}

GraphicsPipeline::~GraphicsPipeline() = default;

void GraphicsPipeline::FillSerializationSupport(
    const Instance& instance, const GraphicsPipelineKey& key,
    std::span<const Shader::Info*, MaxShaderStages> infos,
    std::span<const Shader::RuntimeInfo, MaxShaderStages> runtime_infos,
    const std::optional<const Shader::Gcn::FetchShaderData>& fetch_shader,
    SerializationSupport& sdata) {
    if (!instance.IsVertexInputDynamicState()) {
        const auto& vs_rt = runtime_infos[u32(Shader::SwStage::Vertex)].sw.vs;
        const auto* vs_info = infos[u32(Shader::SwStage::Vertex)];
        if (fetch_shader && !fetch_shader->attributes.empty() && vs_info) {
            // As GetVertexInputs with the stage's live Info.
            using InstanceIdType = Shader::Gcn::VertexAttribute::InstanceIdType;
            for (const auto& attrib : fetch_shader->attributes) {
                const auto step_rate = attrib.GetStepRate();
                const auto buffer = attrib.GetSharp(*vs_info);
                sdata.vertex_attributes.push_back(vk::VertexInputAttributeDescription{
                    .location = attrib.semantic,
                    .binding = attrib.semantic,
                    .format =
                        LiverpoolToVK::SurfaceFormat(buffer.GetDataFmt(), buffer.GetNumberFmt()),
                    .offset = 0,
                });
                sdata.vertex_bindings.push_back(vk::VertexInputBindingDescription{
                    .binding = attrib.semantic,
                    .stride = buffer.GetStride(),
                    .inputRate = step_rate == InstanceIdType::None
                                     ? vk::VertexInputRate::eVertex
                                     : vk::VertexInputRate::eInstance,
                });
                const u32 divisor =
                    step_rate == InstanceIdType::OverStepRate0
                        ? vs_rt.step_rate_0
                        : (step_rate == InstanceIdType::OverStepRate1 ? vs_rt.step_rate_1 : 1);
                if (step_rate != InstanceIdType::None) {
                    sdata.divisors.push_back(vk::VertexInputBindingDivisorDescriptionEXT{
                        .binding = attrib.semantic,
                        .divisor = divisor,
                    });
                }
            }
        }
    }

    const auto& fs_info = runtime_infos[u32(Shader::SwStage::Fragment)].hw.fs;
    sdata.multisampling = {
        .rasterizationSamples = LiverpoolToVK::NumSamples(
            key.num_samples, instance.GetColorSampleCounts() & instance.GetDepthSampleCounts()),
        .sampleShadingEnable =
            fs_info.addr_flags.persp_sample_ena || fs_info.addr_flags.linear_sample_ena,
    };

    const bool is_rect_list = key.prim_type == AmdGpu::PrimitiveType::RectList;
    const bool is_quad_list = key.prim_type == AmdGpu::PrimitiveType::QuadList;
    if (!infos[u32(Shader::SwStage::TessellationControl)] && (is_rect_list || is_quad_list)) {
        const auto type = is_quad_list ? AuxShaderType::QuadListTCS : AuxShaderType::RectListTCS;
        sdata.tcs = Shader::Backend::SPIRV::EmitAuxilaryTessShader(type, fs_info);
    }
    if (!infos[u32(Shader::SwStage::TessellationEval)] && (is_rect_list || is_quad_list)) {
        sdata.tes =
            Shader::Backend::SPIRV::EmitAuxilaryTessShader(AuxShaderType::PassthroughTES, fs_info);
    }
    if (!infos[u32(Shader::SwStage::Fragment)] && fs_info.clip_distance_emulation) {
        const auto& vs = runtime_infos[static_cast<u32>(Shader::SwStage::Vertex)].hw.vs;
        sdata.fragment = Shader::Backend::SPIRV::EmitDiscardFragmentShader(vs.outputs);
    }
}

vk::Result GraphicsPipeline::CreateMonolithic(GraphicsPipelineState& st, vk::UniquePipeline& out) {
    const auto shader_stages = StageInfos(instance, st, vk::ShaderStageFlagBits::eAll);
    const vk::GraphicsPipelineCreateInfo pipeline_info = {
        .pNext = &st.pipeline_rendering_ci,
        .stageCount = static_cast<u32>(shader_stages.size()),
        .pStages = shader_stages.data(),
        .pVertexInputState =
            !instance.IsVertexInputDynamicState() ? &st.vertex_input_info : nullptr,
        .pInputAssemblyState = &st.input_assembly,
        .pTessellationState = &st.tessellation_state,
        .pViewportState = &st.viewport_info,
        .pRasterizationState = &st.raster_chain.get(),
        .pMultisampleState = &st.multisampling,
        .pDepthStencilState =
            !instance.IsExtendedDynamicState3Supported() ? &st.depth_stencil_info : nullptr,
        .pColorBlendState = &st.color_blending,
        .pDynamicState = &st.dynamic_info,
        .layout = *pipeline_layout,
    };
    auto [result, pipe] =
        instance.GetDevice().createGraphicsPipelineUnique(vk_pipeline_cache, pipeline_info);
    if (result == vk::Result::eSuccess) {
        out = std::move(pipe);
    }
    return result;
}

vk::Result GraphicsPipeline::CreateFromLibraries(GraphicsPipelineState& st, GplLibraryCache& gpl,
                                                 vk::UniquePipeline& out) {
    const vk::Device device = instance.GetDevice();
    boost::container::small_vector<vk::DescriptorSetLayoutBinding, 32> bindings;
    u32 num_bindings = 0;
    CollectBindings(stages, bindings, num_bindings);
    const auto layout_sig = LayoutSignature(bindings, uses_push_descriptors);
    const auto keys = LibraryKeys(instance, st, layout_sig);
    constexpr auto library_flags = vk::PipelineCreateFlagBits::eLibraryKHR |
                                   vk::PipelineCreateFlagBits::eRetainLinkTimeOptimizationInfoEXT;
    // Only the view mask matters for the shader parts (dynamic rendering, no multiview).
    const vk::PipelineRenderingCreateInfo no_formats{};

    for (u32 kind = 0; kind < GplLibraryCache::NumKinds; ++kind) {
        const auto entry = gpl.Get(keys[kind]);
        std::scoped_lock lk{entry->mutex};
        if (!entry->done.load(std::memory_order_acquire)) {
            vk::GraphicsPipelineLibraryCreateInfoEXT library_info{};
            vk::GraphicsPipelineCreateInfo ci{
                .flags = library_flags,
                .pDynamicState = &st.dynamic_info,
            };
            boost::container::static_vector<vk::PipelineShaderStageCreateInfo, MaxShaderStages>
                shader_stages;
            const bool needs_layout = kind == GplLibraryCache::PreRasterization ||
                                      kind == GplLibraryCache::FragmentShader;
            if (needs_layout) {
                MakeLayouts(instance, bindings, uses_push_descriptors, entry->set_layout,
                            entry->layout);
                ci.layout = *entry->layout;
            }
            switch (kind) {
            case GplLibraryCache::VertexInput:
                library_info.flags = vk::GraphicsPipelineLibraryFlagBitsEXT::eVertexInputInterface;
                ci.pNext = &library_info;
                ci.pVertexInputState =
                    !instance.IsVertexInputDynamicState() ? &st.vertex_input_info : nullptr;
                ci.pInputAssemblyState = &st.input_assembly;
                break;
            case GplLibraryCache::PreRasterization:
                library_info.flags = vk::GraphicsPipelineLibraryFlagBitsEXT::ePreRasterizationShaders;
                library_info.pNext = &no_formats;
                ci.pNext = &library_info;
                shader_stages = StageInfos(instance, st, PreRasterStages);
                ci.stageCount = u32(shader_stages.size());
                ci.pStages = shader_stages.data();
                ci.pTessellationState = &st.tessellation_state;
                ci.pViewportState = &st.viewport_info;
                ci.pRasterizationState = &st.raster_chain.get();
                break;
            case GplLibraryCache::FragmentShader:
                library_info.flags = vk::GraphicsPipelineLibraryFlagBitsEXT::eFragmentShader;
                library_info.pNext = &no_formats;
                ci.pNext = &library_info;
                shader_stages = StageInfos(instance, st, vk::ShaderStageFlagBits::eFragment);
                ci.stageCount = u32(shader_stages.size());
                ci.pStages = shader_stages.data();
                ci.pMultisampleState = &st.multisampling;
                ci.pDepthStencilState = &st.depth_stencil_info;
                break;
            default:
                library_info.flags = vk::GraphicsPipelineLibraryFlagBitsEXT::eFragmentOutputInterface;
                library_info.pNext = &st.pipeline_rendering_ci;
                ci.pNext = &library_info;
                ci.pMultisampleState = &st.multisampling;
                ci.pColorBlendState = &st.color_blending;
                break;
            }
            auto [result, library] = device.createGraphicsPipeline(vk_pipeline_cache, ci);
            entry->library = result == vk::Result::eSuccess ? library : vk::Pipeline{};
            entry->done.store(true, std::memory_order_release);
            if (result == vk::Result::eSuccess) {
                ++gpl.num_libraries;
            } else {
                LOG_WARNING(Render_Vulkan, "Pipeline library (kind {}) failed: {}", kind,
                            vk::to_string(result));
            }
        }
        if (!entry->library) {
            return vk::Result::eErrorUnknown;
        }
        libraries[kind] = entry->library;
    }

    const vk::PipelineLibraryCreateInfoKHR link_info{
        .libraryCount = u32(libraries.size()),
        .pLibraries = libraries.data(),
    };
    const vk::GraphicsPipelineCreateInfo ci{
        .pNext = &link_info,
        .layout = *pipeline_layout,
    };
    auto [result, pipe] = device.createGraphicsPipelineUnique(vk_pipeline_cache, ci);
    if (result == vk::Result::eSuccess) {
        out = std::move(pipe);
        fast_linked = true;
        ++gpl.num_fast_links;
    }
    return result;
}

bool GraphicsPipeline::LinkOptimized() {
    if (!fast_linked || optimized) {
        return false;
    }
    const vk::PipelineLibraryCreateInfoKHR link_info{
        .libraryCount = u32(libraries.size()),
        .pLibraries = libraries.data(),
    };
    const vk::GraphicsPipelineCreateInfo ci{
        .pNext = &link_info,
        .flags = vk::PipelineCreateFlagBits::eLinkTimeOptimizationEXT,
        .layout = *pipeline_layout,
    };
    auto [result, pipe] = instance.GetDevice().createGraphicsPipelineUnique(vk_pipeline_cache, ci);
    if (result != vk::Result::eSuccess) {
        return false;
    }
    optimized = std::move(pipe);
    // The fast-linked handle stays alive (recorded commands may still bind it).
    handle.store(*optimized, std::memory_order_release);
    return true;
}

bool GraphicsPipeline::LibrariesReady(const Instance& instance, GplLibraryCache& gpl,
                                      const GraphicsPipelineKey& key,
                                      std::span<const Shader::Info*, MaxShaderStages> infos,
                                      std::span<const vk::ShaderModule> modules,
                                      const SerializationSupport& sdata) {
    GraphicsPipelineState state{};
    BuildState(instance, key, infos, modules, sdata, state, true);
    boost::container::small_vector<vk::DescriptorSetLayoutBinding, 32> bindings;
    u32 num_bindings = 0;
    CollectBindings(std::span<const Shader::Info* const>{infos.data(), infos.size()}, bindings,
                    num_bindings);
    const bool push = num_bindings < instance.MaxPushDescriptors();
    const auto keys = LibraryKeys(instance, state, LayoutSignature(bindings, push));
    return std::ranges::all_of(keys, [&](const std::string& k) { return gpl.Contains(k); });
}

GplLibraryCache::GplLibraryCache(vk::Device device_) : device{device_} {}

GplLibraryCache::~GplLibraryCache() {
    std::scoped_lock lk{mutex};
    for (auto& [_, entry] : entries) {
        if (entry->library) {
            device.destroyPipeline(entry->library);
        }
    }
    LOG_INFO(Render_Vulkan, "Pipeline libraries: {} built, {} fast links, {} optimized, {} failed",
             num_libraries.load(), num_fast_links.load(), num_optimized.load(),
             num_failures.load());
}

std::shared_ptr<GplLibraryCache::Entry> GplLibraryCache::Get(const std::string& key) {
    std::scoped_lock lk{mutex};
    auto& entry = entries[key];
    if (!entry) {
        entry = std::make_shared<Entry>();
    }
    return entry;
}

bool GplLibraryCache::Contains(const std::string& key) const {
    std::scoped_lock lk{mutex};
    const auto it = entries.find(key);
    return it != entries.end() && it->second->done.load(std::memory_order_acquire) &&
           it->second->library;
}

template <typename Attribute, typename Binding>
void GraphicsPipeline::GetVertexInputs(
    VertexInputs<Attribute>& attributes, VertexInputs<Binding>& bindings,
    VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT>& divisors,
    VertexInputs<AmdGpu::Buffer>& guest_buffers, u32 step_rate_0, u32 step_rate_1,
    std::span<const AmdGpu::Buffer> sharps) const {
    using InstanceIdType = Shader::Gcn::VertexAttribute::InstanceIdType;
    if (!fetch_shader || fetch_shader->attributes.empty()) {
        return;
    }
    const auto& vs_info = GetStage(Shader::SwStage::Vertex);
    const bool given = sharps.size() == fetch_shader->attributes.size();
    for (u32 index = 0; const auto& attrib : fetch_shader->attributes) {
        const auto step_rate = attrib.GetStepRate();
        const auto buffer = given ? sharps[index] : attrib.GetSharp(vs_info);
        ++index;
        attributes.push_back(Attribute{
            .location = attrib.semantic,
            .binding = attrib.semantic,
            .format = LiverpoolToVK::SurfaceFormat(buffer.GetDataFmt(), buffer.GetNumberFmt()),
            .offset = 0,
        });
        bindings.push_back(Binding{
            .binding = attrib.semantic,
            .stride = buffer.GetStride(),
            .inputRate = step_rate == InstanceIdType::None ? vk::VertexInputRate::eVertex
                                                           : vk::VertexInputRate::eInstance,
        });
        const u32 divisor = step_rate == InstanceIdType::OverStepRate0
                                ? step_rate_0
                                : (step_rate == InstanceIdType::OverStepRate1 ? step_rate_1 : 1);
        if constexpr (std::is_same_v<Binding, vk::VertexInputBindingDescription2EXT>) {
            bindings.back().divisor = divisor;
        } else if (step_rate != InstanceIdType::None) {
            divisors.push_back(vk::VertexInputBindingDivisorDescriptionEXT{
                .binding = attrib.semantic,
                .divisor = divisor,
            });
        }
        guest_buffers.emplace_back(buffer);
    }
}

// Declare templated GetVertexInputs for necessary types.
template void GraphicsPipeline::GetVertexInputs(
    VertexInputs<vk::VertexInputAttributeDescription>& attributes,
    VertexInputs<vk::VertexInputBindingDescription>& bindings,
    VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT>& divisors,
    VertexInputs<AmdGpu::Buffer>& guest_buffers, u32 step_rate_0, u32 step_rate_1,
    std::span<const AmdGpu::Buffer> sharps) const;
template void GraphicsPipeline::GetVertexInputs(
    VertexInputs<vk::VertexInputAttributeDescription2EXT>& attributes,
    VertexInputs<vk::VertexInputBindingDescription2EXT>& bindings,
    VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT>& divisors,
    VertexInputs<AmdGpu::Buffer>& guest_buffers, u32 step_rate_0, u32 step_rate_1,
    std::span<const AmdGpu::Buffer> sharps) const;

void GraphicsPipeline::BuildDescSetLayout() {
    boost::container::small_vector<vk::DescriptorSetLayoutBinding, 32> bindings;
    u32 binding{};
    CollectBindings(stages, bindings, binding);
    uses_push_descriptors = binding < instance.MaxPushDescriptors();
    MakeLayouts(instance, bindings, uses_push_descriptors, desc_layout, pipeline_layout);
}

} // namespace Vulkan

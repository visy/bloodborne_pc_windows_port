// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <boost/container/static_vector.hpp>
#include <xxhash.h>

#include "shader_recompiler/frontend/fetch_shader.h"
#include "video_core/amdgpu/regs_color.h"
#include "video_core/amdgpu/regs_depth.h"
#include "video_core/amdgpu/regs_primitive.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"

namespace VideoCore {
class BufferCache;
class TextureCache;
} // namespace VideoCore

namespace Vulkan {

static constexpr u32 MaxShaderStages = static_cast<u32>(Shader::SwStage::NumLogicalStages);
static constexpr u32 MaxVertexBufferCount = 32;

class Instance;
class Scheduler;
class DescriptorHeap;

template <typename T>
using VertexInputs = boost::container::static_vector<T, MaxVertexBufferCount>;

struct GraphicsPipelineKey {
    std::array<size_t, MaxShaderStages> stage_hashes;
    std::array<vk::Format, MaxVertexBufferCount> vertex_buffer_formats;
    u32 patch_control_points;
    u32 num_color_attachments;
    std::array<Shader::PsColorBuffer, AmdGpu::NUM_COLOR_BUFFERS> color_buffers;
    std::array<AmdGpu::BlendControl, AmdGpu::NUM_COLOR_BUFFERS> blend_controls;
    std::array<vk::ColorComponentFlags, AmdGpu::NUM_COLOR_BUFFERS> write_masks;
    AmdGpu::ColorBufferMask cb_shader_mask;
    AmdGpu::ColorControl::LogicOp logic_op;
    u8 num_samples;
    u8 depth_samples;
    std::array<u8, AmdGpu::NUM_COLOR_BUFFERS> color_samples;
    u32 mrt_mask;
    u32 motion_vectors = 0;
    struct {
        AmdGpu::DepthBuffer::ZFormat z_format : 2;
        AmdGpu::DepthBuffer::StencilFormat stencil_format : 1;
        u32 depth_clamp_enable : 1;
    };
    struct {
        AmdGpu::PrimitiveType prim_type : 5;
        AmdGpu::PolygonMode polygon_mode : 2;
        AmdGpu::ClipSpace clip_space : 1;
        AmdGpu::ProvokingVtxLast provoking_vtx_last : 1;
        u32 depth_clip_enable : 1;
    };

    GraphicsPipelineKey() {
        std::memset(this, 0, sizeof(*this));
    }

    bool operator==(const GraphicsPipelineKey& key) const noexcept {
        return std::memcmp(this, &key, sizeof(key)) == 0;
    }

    void Serialize(Serialization::Archive& ar) const;
    bool Deserialize(Serialization::Archive& ar);
};

/// bbport: how a GraphicsPipeline is built. Live: on the GPU thread for a draw (fills the
/// serialization support from the live state, asserts on driver failure). Preload: from the cache
/// warm-up (serialization support as stored). Async: on a worker from a live miss whose
/// serialization support the GPU thread filled (FillSerializationSupport). Preload and Async throw
/// Serialization::CorruptData when the driver rejects the pipeline.
enum class BuildMode { Live, Preload, Async };

class GplLibraryCache;
struct GraphicsPipelineState;

class GraphicsPipeline : public Pipeline {
public:
    struct SerializationSupport {
        VertexInputs<vk::VertexInputAttributeDescription> vertex_attributes{};
        VertexInputs<vk::VertexInputBindingDescription> vertex_bindings{};
        VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT> divisors{};
        vk::PipelineMultisampleStateCreateInfo multisampling{};
        std::vector<u32> tcs{};
        std::vector<u32> tes{};
        std::vector<u32> fragment{};

        void Serialize(Serialization::Archive& ar) const;
        bool Deserialize(Serialization::Archive& ar);
    };

    GraphicsPipeline(const Instance& instance, Scheduler& scheduler, DescriptorHeap& desc_heap,
                     const Shader::Profile& profile, const GraphicsPipelineKey& key,
                     vk::PipelineCache pipeline_cache,
                     std::span<const Shader::Info*, MaxShaderStages> stages,
                     std::span<const Shader::RuntimeInfo, MaxShaderStages> runtime_infos,
                     std::optional<const Shader::Gcn::FetchShaderData> fetch_shader,
                     std::span<const vk::ShaderModule> modules, SerializationSupport& sdata,
                     BuildMode mode, GplLibraryCache* gpl = nullptr);
    ~GraphicsPipeline();

    /// bbport: the parts of `sdata` a live build derives from the draw's state (vertex inputs,
    /// multisampling, auxiliary tessellation and discard shaders). GPU thread (reads user data).
    static void FillSerializationSupport(
        const Instance& instance, const GraphicsPipelineKey& key,
        std::span<const Shader::Info*, MaxShaderStages> infos,
        std::span<const Shader::RuntimeInfo, MaxShaderStages> runtime_infos,
        const std::optional<const Shader::Gcn::FetchShaderData>& fetch_shader,
        SerializationSupport& sdata);

    /// bbport: a pipeline built off the GPU thread from Info copies reads the programs' own
    /// Infos once published (as a pipeline built on the GPU thread does).
    void RebindStages(std::span<const Shader::Info*, MaxShaderStages> infos) {
        std::ranges::copy(infos, stages.begin());
    }

    /// bbport (GPL): linked from pipeline libraries without link-time optimization.
    [[nodiscard]] bool IsFastLinked() const {
        return fast_linked;
    }
    /// bbport (GPL): links the libraries again with link-time optimization and swaps the bound
    /// handle (the fast-linked one stays alive). Any thread; false when the driver fails.
    bool LinkOptimized();
    /// bbport (GPL): whether every library of this pipeline already exists (a miss then only
    /// needs a fast link). GPU thread, `sdata` as FillSerializationSupport made it.
    static bool LibrariesReady(const Instance& instance, GplLibraryCache& gpl,
                               const GraphicsPipelineKey& key,
                               std::span<const Shader::Info*, MaxShaderStages> infos,
                               std::span<const vk::ShaderModule> modules,
                               const SerializationSupport& sdata);

    const std::optional<const Shader::Gcn::FetchShaderData>& GetFetchShader() const noexcept {
        return fetch_shader;
    }

    const GraphicsPipelineKey& GetGraphicsKey() const {
        return key;
    }

    /// Gets the attributes and bindings for vertex inputs.
    /// bbport: `sharps`, when given, are the V#s of the fetch shader's attributes as the GPU
    /// command thread read them (in the draw packet): the V# tables they come from may have been
    /// rewritten by a later constant engine dump by the time the recording thread gets here.
    template <typename Attribute, typename Binding>
    void GetVertexInputs(VertexInputs<Attribute>& attributes, VertexInputs<Binding>& bindings,
                         VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT>& divisors,
                         VertexInputs<AmdGpu::Buffer>& guest_buffers, u32 step_rate_0,
                         u32 step_rate_1, std::span<const AmdGpu::Buffer> sharps = {}) const;

private:
    void BuildDescSetLayout();
    vk::Result CreateMonolithic(GraphicsPipelineState& state, vk::UniquePipeline& out);
    vk::Result CreateFromLibraries(GraphicsPipelineState& state, GplLibraryCache& gpl,
                                   vk::UniquePipeline& out);

private:
    GraphicsPipelineKey key;
    std::optional<const Shader::Gcn::FetchShaderData> fetch_shader{};
    vk::PipelineCache vk_pipeline_cache{};
    /// bbport (GPL): the libraries this pipeline was linked from (owned by GplLibraryCache).
    std::array<vk::Pipeline, 4> libraries{};
    vk::UniquePipeline optimized;
    bool fast_linked = false;
};

/// bbport: VK_EXT_graphics_pipeline_library parts shared between pipelines: vertex input
/// interface, pre-rasterization shaders, fragment shader, fragment output interface. Keyed by
/// the complete description of the state each part holds. Thread-safe: a part requested while
/// another thread builds it waits for that build.
class GplLibraryCache {
public:
    enum Kind : u32 { VertexInput, PreRasterization, FragmentShader, FragmentOutput, NumKinds };

    struct Entry {
        std::mutex mutex;
        std::atomic<bool> done{false};
        vk::Pipeline library{};
        /// Layout the part was created with (identically defined to its pipelines' layouts).
        vk::UniqueDescriptorSetLayout set_layout;
        vk::UniquePipelineLayout layout;
    };

    explicit GplLibraryCache(vk::Device device);
    ~GplLibraryCache();

    std::shared_ptr<Entry> Get(const std::string& key);
    [[nodiscard]] bool Contains(const std::string& key) const;

    std::atomic<u32> num_libraries{0}, num_fast_links{0}, num_optimized{0}, num_failures{0};

private:
    vk::Device device;
    mutable std::mutex mutex;
    std::unordered_map<std::string, std::shared_ptr<Entry>> entries;
};

struct ClipDistanceShaderKey {
    std::array<std::tuple<u8, u8>, 8> clip_locations;

    bool operator==(const ClipDistanceShaderKey& key) const noexcept {
        return std::memcmp(this, &key, sizeof(key)) == 0;
    }
};

} // namespace Vulkan

template <>
struct std::hash<Vulkan::GraphicsPipelineKey> {
    std::size_t operator()(const Vulkan::GraphicsPipelineKey& key) const noexcept {
        return XXH3_64bits(&key, sizeof(key));
    }
};

template <>
struct std::hash<Vulkan::ClipDistanceShaderKey> {
    std::size_t operator()(const Vulkan::ClipDistanceShaderKey& key) const noexcept {
        return XXH3_64bits(&key, sizeof(key));
    }
};

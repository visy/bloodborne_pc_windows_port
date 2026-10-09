// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: pipeline jobs of the PipelineCache (vk_pipeline_compiler.h), shared by its sources.

#pragma once

#include <optional>
#include "shader_recompiler/info.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"

namespace Vulkan {

struct PipelineCache::GraphicsJob : PipelineCompiler::Job {
    PipelineCache* cache{};
    GraphicsPipelineKey key{};
    BuildMode mode = BuildMode::Async;
    bool register_data = false;   ///< a new pipeline: written to the cache storage when published
    bool optimize_inline = false; ///< startup: the optimized library link in the job as well
    /// Copies of the stages' Infos the build reads; the pipeline is rebound to `bound_infos`
    /// (the programs' own Infos) when published.
    std::array<std::optional<Shader::Info>, MaxShaderStages> owned{};
    std::array<const Shader::Info*, MaxShaderStages> build_infos{};
    std::array<const Shader::Info*, MaxShaderStages> bound_infos{};
    std::array<Shader::RuntimeInfo, MaxShaderStages> runtime_infos{};
    std::optional<Shader::Gcn::FetchShaderData> fetch_shader{};
    std::array<vk::ShaderModule, MaxShaderStages> modules{};
    GraphicsPipeline::SerializationSupport sdata{};
    u32 first_skip_frame = ~0u;
    std::unique_ptr<GraphicsPipeline> result;

    void Run() override {
        result = std::make_unique<GraphicsPipeline>(
            cache->instance, cache->scheduler, cache->desc_heap, cache->profile, key,
            *cache->pipeline_cache, build_infos, runtime_infos, fetch_shader, modules, sdata, mode,
            cache->gpl.get());
        if (optimize_inline && result->IsFastLinked() && result->LinkOptimized()) {
            ++cache->gpl->num_optimized;
        }
    }
};

struct PipelineCache::ComputeJob : PipelineCompiler::Job {
    PipelineCache* cache{};
    ComputePipelineKey key{};
    std::optional<Shader::Info> owned;
    const Shader::Info* bound_info{};
    vk::ShaderModule module{};
    std::unique_ptr<ComputePipeline> result;

    void Run() override {
        ComputePipeline::SerializationSupport sdata{};
        result = std::make_unique<ComputePipeline>(cache->instance, cache->scheduler,
                                                   cache->desc_heap, cache->profile,
                                                   *cache->pipeline_cache, key, *owned, module,
                                                   sdata, true);
    }
};

struct PipelineCache::OptimizeJob : PipelineCompiler::Job {
    GraphicsPipeline* pipeline{};
    GplLibraryCache* gpl{};

    void Run() override {
        if (pipeline->LinkOptimized()) {
            ++gpl->num_optimized;
        } else {
            failed = true;
        }
    }
};

} // namespace Vulkan

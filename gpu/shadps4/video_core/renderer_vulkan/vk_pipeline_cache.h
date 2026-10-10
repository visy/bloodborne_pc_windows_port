// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <unordered_map>
#include <shared_mutex>
#include <variant>
#include <boost/container/static_vector.hpp>
#include <tsl/robin_map.h>
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/specialization.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_pipeline_compiler.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"

template <>
struct std::hash<vk::ShaderModule> {
    std::size_t operator()(const vk::ShaderModule& module) const noexcept {
        return std::hash<size_t>{}(reinterpret_cast<size_t>((VkShaderModule)module));
    }
};

namespace AmdGpu {
class Liverpool;
}

namespace Serialization {
struct Archive;
}

namespace Shader {
struct Info;
}

namespace Vulkan {

class Instance;
class Scheduler;
class ShaderCache;
struct ShaderSource;

struct Program {
    struct Module {
        vk::ShaderModule module;
        Shader::StageSpecialization spec;
    };
    static constexpr size_t MaxPermutations = 8;
    using ModuleList = boost::container::small_vector<Module, MaxPermutations>;

    Shader::Info info;
    ModuleList modules{};
    size_t last_used = 0; ///< bbport: permutation of the previous lookup, compared first
    /// bbport: `info` as translated, for draw-preparation workers (they must not read `info`,
    /// whose user data the GPU thread rewrites every draw). Guarded by programs_mutex.
    std::unique_ptr<Shader::Info> info_template;

    Program() = default;
    Program(Shader::HwStage stage, Shader::SwStage l_stage, Shader::ShaderParams params)
        : info{stage, l_stage, params} {}

    void AddPermut(vk::ShaderModule module, Shader::StageSpecialization&& spec) {
        modules.emplace_back(module, std::move(spec));
    }

    void InsertPermut(vk::ShaderModule module, Shader::StageSpecialization&& spec,
                      size_t perm_idx) {
        modules.resize(std::max(modules.size(), perm_idx + 1)); // <-- beware of realloc
        modules[perm_idx] = {module, std::move(spec)};
    }
};

struct DrawIndirectParams {
    u16 vertex_sgpr_offset;
    u32 instance_sgpr_offset;
};

} // namespace Vulkan

namespace AmdGpu {
union Regs;
}

namespace Vulkan {

/// bbport: state of one graphics/compute pipeline selection. The GPU thread owns one
/// (PipelineCache::sel); draw-preparation workers use their own with their register copies.
struct PipelineSelection {
    const AmdGpu::Regs* regs{};
    std::array<Shader::RuntimeInfo, MaxShaderStages> runtime_infos{};
    std::array<const Shader::Info*, MaxShaderStages> infos{};
    std::array<vk::ShaderModule, MaxShaderStages> modules{};
    std::optional<Shader::Gcn::FetchShaderData> fetch_shader{};
    GraphicsPipelineKey graphics_key{};
    bool motion = false;
    DrawIndirectParams draw_indirect_params{};
    struct PrepWorker* worker{}; ///< set: read-only selection for a draw-preparation worker
};

/// bbport: a draw-preparation worker's own program state (see vk_draw_prep.h).
struct PrepWorker {
    struct Stage {
        const Program* program;
        u64 hash;
        Shader::HwStage hw_stage;
        VAddr pgm_base;
        const std::vector<u32>* flat;
    };
    // Node-based: stages keep pointers to these Infos while later stages are inserted.
    std::unordered_map<const Program*, Shader::Info> infos;
    boost::container::static_vector<Stage, MaxShaderStages> stages;
    bool failed = false;
};

struct PreparedDraw;

class PipelineCache {
public:
    explicit PipelineCache(const Instance& instance, Scheduler& scheduler,
                           AmdGpu::Liverpool* liverpool, u32 sparse_page_shift);
    ~PipelineCache();

    /// Preloads the pipeline cache. `parallel` (the startup precompile, PrecompileEnabled):
    /// pipelines are built on all cores with progress (BbCompileProgress); otherwise one by one
    /// as before (BB_SHADER_PRECOMPILE=0, called from the constructor). Nothing else may use
    /// the cache meanwhile (the game has not started).
    void WarmUp(bool parallel = false);
    /// Another thread: stops a running parallel WarmUp soon (Esc on the precompile screen).
    void AbortWarmUp();
    /// Stops the pipeline compiler (queued jobs dropped), then closes the cache storage.
    void Sync();
    /// bbport: the startup precompile runs with a progress screen (BB_SHADER_PRECOMPILE, on).
    static bool PrecompileEnabled();

    bool LoadComputePipeline(Serialization::Archive& ar);
    bool LoadGraphicsPipeline(Serialization::Archive& ar);
    bool LoadPipelineStage(Serialization::Archive& ar, size_t stage);

    /// `indirect`: the draw's arguments are in memory (bbport: never skipped while compiling).
    const GraphicsPipeline* GetGraphicsPipeline(const DrawIndirectParams params = {},
                                                const PreparedDraw* prepared = nullptr,
                                                bool indirect = false);

    /// bbport: worker side of draw preparation: selects the pipeline key for `sel.regs` without
    /// creating anything. False when a program or permutation does not exist yet.
    bool PrepareGraphicsPipeline(PipelineSelection& sel);

    /// bbport: GPU-thread side: the pipeline for a prepared draw after checking that registers
    /// and flattened user data match; null to take the regular path.
    const GraphicsPipeline* TryPreparedPipeline(const PreparedDraw& prepared);

    /// bbport: the prepared draw the last GetGraphicsPipeline used, or null (regular path).
    [[nodiscard]] const PreparedDraw* UsedPrepared() const noexcept {
        return used_prepared;
    }

    const ComputePipeline* GetComputePipeline();

    using Result = std::tuple<const Shader::Info*, vk::ShaderModule,
                              std::optional<Shader::Gcn::FetchShaderData>, u64>;
    Result GetProgram(PipelineSelection& sel, Shader::HwStage stage, Shader::SwStage l_stage,
                      const Shader::ShaderParams& params, Shader::Backend::Bindings& binding);

    std::optional<vk::ShaderModule> ReplaceShader(vk::ShaderModule module,
                                                  std::span<const u32> spv_code);

    static std::string GetShaderName(Shader::HwStage stage, u64 hash,
                                     std::optional<size_t> perm = {});

    auto& GetProfile() const {
        return profile;
    }

private:
    bool RefreshGraphicsKey(PipelineSelection& sel);
    bool RefreshGraphicsStages(PipelineSelection& sel);
    bool RefreshComputeKey();

    void DumpShader(std::span<const u32> code, u64 hash, Shader::HwStage stage, size_t perm_idx,
                    std::string_view ext);
    std::optional<std::vector<u32>> GetShaderPatch(u64 hash, Shader::HwStage stage, size_t perm_idx,
                                                   std::string_view ext);
    vk::ShaderModule CompileModule(Shader::Info& info, Shader::RuntimeInfo& runtime_info,
                                   const std::span<const u32>& code, size_t perm_idx,
                                   Shader::Backend::Bindings& binding,
                                   std::vector<u32>* spv_out = nullptr);

    /// bbport: shader sources (vk_pipeline_serialization.h). Inputs of a compilation, taken
    /// before it; null when nothing is recorded (no cache and no self-test).
    std::unique_ptr<ShaderSource> BeginShaderSource(const PipelineSelection& sel,
                                                    Shader::HwStage hw_stage,
                                                    Shader::SwStage sw_stage,
                                                    const Shader::ShaderParams& params,
                                                    const Shader::RuntimeInfo& runtime_info,
                                                    const Shader::Backend::Bindings& start,
                                                    size_t perm_idx);
    /// Stores the source with the guest reads of `log`; BB_SHADER_CACHE_SELFTEST=1 translates
    /// it again right away and compares with the in-game result.
    void FinishShaderSource(std::unique_ptr<ShaderSource> source, Shader::GuestReadLog& log,
                            const std::vector<u32>& spv, const Shader::StageSpecialization& spec,
                            const Shader::Info* base_info);
    /// Translates the shader sources again for this GPU (all, or only programs whose meta or
    /// SPIR-V is missing). Returns the number of rebuilt shaders.
    u32 RebuildShaderCache(bool full);
    const Shader::RuntimeInfo& BuildRuntimeInfo(PipelineSelection& sel, Shader::HwStage stage,
                                                Shader::SwStage l_stage);

    [[nodiscard]] bool IsPipelineCacheDirty() const {
        return num_new_pipelines > 0;
    }

    // bbport: pipelines built on the compiler's workers (startup precompile, async graphics
    // pipelines BB_ASYNC_SHADERS, optimized links of pipeline libraries).
    struct GraphicsJob;
    struct ComputeJob;
    struct OptimizeJob;
    std::shared_ptr<GraphicsJob> MakeGraphicsJob(const GraphicsPipeline::SerializationSupport& sdata,
                                                 BuildMode mode);
    void PublishCompleted();
    /// Inserts a finished job's pipeline (null when it failed).
    const GraphicsPipeline* PublishGraphics(GraphicsJob& job);
    void QueueOptimize(GraphicsPipeline* pipeline);
    void EnsureRuntimeWorkers();
    /// GPU thread: waits for a job (runs it here when no worker took it), logs long waits.
    void WaitJob(const std::shared_ptr<GraphicsJob>& job);
    /// Async graphics pipelines: frame and render target history, the skip decision.
    void TrackTargets();
    bool DrawSkippable(bool indirect) const;
    struct TargetSlot {
        VAddr addr;
        u32 last_frame;
        u32 mask; ///< bit i: a target in frame (last_frame - i)
    };

private:
    const Instance& instance;
    Scheduler& scheduler;
    /// bbport: VK_EXT_graphics_pipeline_library parts (BB_GPL), destroyed after the pipelines.
    std::unique_ptr<GplLibraryCache> gpl;
    AmdGpu::Liverpool* liverpool;
    DescriptorHeap desc_heap;
    vk::UniquePipelineCache pipeline_cache;
    vk::UniquePipelineLayout pipeline_layout;
    Shader::Profile profile{};
    Shader::Pools pools;
    tsl::robin_map<size_t, std::unique_ptr<Program>> program_cache;
    /// bbport: exclusive for program/permutation insertions, shared for worker lookups.
    std::shared_mutex programs_mutex;
    u64 prepared_hits = 0, prepared_misses = 0;
    const PreparedDraw* used_prepared = nullptr;
    tsl::robin_map<ComputePipelineKey, std::unique_ptr<ComputePipeline>> compute_pipelines;
    tsl::robin_map<GraphicsPipelineKey, std::unique_ptr<GraphicsPipeline>> graphics_pipelines;
    PipelineSelection sel{}; ///< GPU thread selection state
    ComputePipelineKey compute_key{};
    u32 num_new_pipelines{}; // new pipelines added to the cache since the game start
    bool preload_profile_changed{}; ///< bbport: WarmUp rebuilt the cache for another profile
    u32 selftest_runs{}, selftest_failures{};

    // Only if Config::collectShadersForDebug()
    tsl::robin_map<vk::ShaderModule,
                   std::vector<std::variant<GraphicsPipelineKey, ComputePipelineKey>>>
        module_related_pipelines;

    // bbport: startup precompile.
    bool parallel_warmup = false;
    std::atomic<bool> warmup_abort{false};
    std::vector<std::shared_ptr<PipelineCompiler::Job>> warmup_jobs;
    // bbport: async graphics pipelines (GPU thread).
    tsl::robin_map<GraphicsPipelineKey, std::shared_ptr<GraphicsJob>> pending_graphics;
    std::vector<TargetSlot> target_table;
    u32 async_frame = 0;
    bool display_pass = false;
    bool targets_stable = false;
    u64 last_target_sig = 0;
    u32 last_target_frame = ~0u;
    u32 frame_draws = 0, last_frame_draws = 0; ///< draws per frame (light frames: no skips)
    /// Last: stopped (workers joined) before everything the jobs use is destroyed.
    PipelineCompiler compiler;
};

} // namespace Vulkan

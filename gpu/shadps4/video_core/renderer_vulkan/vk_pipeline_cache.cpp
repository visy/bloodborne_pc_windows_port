// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <ranges>
#include <string>
#include <unordered_set>

#include "bbport_compile_progress.h"
#include "bbport_settings.h"
#include "bbport_threads.h"

#include "common/hash.h"
#include "common/io_file.h"
#include "common/path_util.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/motion_history.h"
#include "video_core/renderer_vulkan/vk_draw_prep.h"
#include "video_core/renderer_vulkan/vk_frame_capture.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_jobs.h"
#include "video_core/renderer_vulkan/vk_pipeline_serialization.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "bbport_guest_memory.h"

namespace Vulkan {

using Shader::HwStage;
using Shader::Output;
using Shader::SwStage;

constexpr static auto SpirvVersion1_6 = 0x00010600U;

constexpr static std::array DescriptorHeapSizes = {
    vk::DescriptorPoolSize{vk::DescriptorType::eUniformBuffer, 512},
    vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, 8192},
    vk::DescriptorPoolSize{vk::DescriptorType::eSampledImage, 8192},
    vk::DescriptorPoolSize{vk::DescriptorType::eStorageImage, 1024},
    vk::DescriptorPoolSize{vk::DescriptorType::eSampler, 1024},
};

static u32 MapOutputs(std::span<Shader::OutputMap, 3> outputs, const AmdGpu::VsOutputControl& ctl) {
    u32 num_outputs = 0;

    if (ctl.vs_out_misc_enable) {
        auto& misc_vec = outputs[num_outputs++];
        misc_vec[0] = ctl.use_vtx_point_size ? Output::PointSize : Output::None;
        misc_vec[1] = ctl.use_vtx_edge_flag
                          ? Output::EdgeFlag
                          : (ctl.use_vtx_gs_cut_flag ? Output::GsCutFlag : Output::None);
        misc_vec[2] =
            ctl.use_vtx_kill_flag
                ? Output::KillFlag
                : (ctl.use_vtx_render_target_idx ? Output::RenderTargetIndex : Output::None);
        misc_vec[3] = ctl.use_vtx_viewport_idx ? Output::ViewportIndex : Output::None;
    }

    if (ctl.vs_out_ccdist0_enable) {
        auto& ccdist0 = outputs[num_outputs++];
        ccdist0[0] = ctl.IsClipDistEnabled(0)
                         ? Output::ClipDist0
                         : (ctl.IsCullDistEnabled(0) ? Output::CullDist0 : Output::None);
        ccdist0[1] = ctl.IsClipDistEnabled(1)
                         ? Output::ClipDist1
                         : (ctl.IsCullDistEnabled(1) ? Output::CullDist1 : Output::None);
        ccdist0[2] = ctl.IsClipDistEnabled(2)
                         ? Output::ClipDist2
                         : (ctl.IsCullDistEnabled(2) ? Output::CullDist2 : Output::None);
        ccdist0[3] = ctl.IsClipDistEnabled(3)
                         ? Output::ClipDist3
                         : (ctl.IsCullDistEnabled(3) ? Output::CullDist3 : Output::None);
    }

    if (ctl.vs_out_ccdist1_enable) {
        auto& ccdist1 = outputs[num_outputs++];
        ccdist1[0] = ctl.IsClipDistEnabled(4)
                         ? Output::ClipDist4
                         : (ctl.IsCullDistEnabled(4) ? Output::CullDist4 : Output::None);
        ccdist1[1] = ctl.IsClipDistEnabled(5)
                         ? Output::ClipDist5
                         : (ctl.IsCullDistEnabled(5) ? Output::CullDist5 : Output::None);
        ccdist1[2] = ctl.IsClipDistEnabled(6)
                         ? Output::ClipDist6
                         : (ctl.IsCullDistEnabled(6) ? Output::CullDist6 : Output::None);
        ccdist1[3] = ctl.IsClipDistEnabled(7)
                         ? Output::ClipDist7
                         : (ctl.IsCullDistEnabled(7) ? Output::CullDist7 : Output::None);
    }

    return num_outputs;
}

/// bbport: register state behind the GPU-dependent runtime info bits (ShaderSource::host_flags).
static u32 SourceHostFlags(const AmdGpu::Regs& regs, HwStage stage) {
    u32 flags = 0;
    if (stage == HwStage::Vertex && regs.clipper_control.clip_space == AmdGpu::ClipSpace::MinusWToW) {
        flags |= ShaderSource::FlagClipSpaceMinusW;
    }
    if (stage == HwStage::Fragment) {
        // Lowered user clip planes ride the same emulation path as guest-exported distances, so
        // the fragment side arms whenever the hardware vertex stage lowers them, keeping its
        // input locations in sync with the shifted vertex outputs.
        const bool lowers_user_clip_planes =
            regs.clipper_control.user_clip_plane_enable &&
            !regs.stage_enable.IsStageEnabled(static_cast<u32>(HwStage::Geometry));
        if ((regs.vs_output_control.clip_distance_enable &&
             !regs.stage_enable.IsStageEnabled(static_cast<u32>(HwStage::Local))) ||
            lowers_user_clip_planes) {
            flags |= ShaderSource::FlagClipDistanceEmulation;
        }
    }
    return flags;
}

const Shader::RuntimeInfo& PipelineCache::BuildRuntimeInfo(PipelineSelection& sel, HwStage stage,
                                                             SwStage l_stage) {
    auto& info = sel.runtime_infos[u32(l_stage)];
    const auto& regs = (*sel.regs);
    const auto BuildCommon = [&](const auto& program) {
        info.props.num_user_data = program.settings.num_user_regs;
        info.props.num_input_vgprs = program.settings.vgpr_comp_cnt;
        info.props.num_allocated_vgprs = program.NumVgprs();
        info.props.fp_denorm_mode32 = program.settings.fp_denorm_mode32;
        info.props.fp_denorm_mode16_64 = program.settings.fp_denorm_mode64;
        info.props.fp_round_mode32 = program.settings.fp_round_mode32;
        info.props.fp_round_mode16_64 = program.settings.fp_round_mode64;
    };
    info.Initialize(stage, l_stage);
    switch (stage) {
    case HwStage::Local: {
        BuildCommon(regs.ls_program);
        Shader::TessellationDataConstantBuffer tess_constants{};
        const auto* hull_info = sel.infos[u32(SwStage::TessellationControl)];
        hull_info->ReadTessConstantBuffer(tess_constants);
        info.hw.ls.ls_stride = tess_constants.ls_stride;
        break;
    }
    case HwStage::Hull:
        BuildCommon(regs.hs_program);
        break;
    case HwStage::Export:
        BuildCommon(regs.es_program);
        info.hw.es.vertex_data_size = regs.vgt_esgs_ring_itemsize;
        break;
    case HwStage::Geometry: {
        BuildCommon(regs.gs_program);
        info.hw.gs.num_outputs = MapOutputs(info.hw.gs.outputs, regs.vs_output_control);
        info.hw.gs.output_vertices = regs.vgt_gs_max_vert_out;
        info.hw.gs.num_invocations =
            regs.vgt_gs_instance_cnt.IsEnabled() ? regs.vgt_gs_instance_cnt.count : 1;
        if (regs.stage_enable.raw == AmdGpu::ShaderStageEnable::LsHsEsGs) {
            info.hw.gs.in_primitive = [&]() {
                switch (regs.tess_config.topology) {
                case AmdGpu::TessellationTopology::Point:
                    return AmdGpu::PrimitiveType::PointList;
                case AmdGpu::TessellationTopology::Line:
                    return AmdGpu::PrimitiveType::LineList;
                case AmdGpu::TessellationTopology::TriangleCw:
                case AmdGpu::TessellationTopology::TriangleCcw:
                    return AmdGpu::PrimitiveType::TriangleList;
                default:
                    UNREACHABLE();
                }
            }();
        } else {
            info.hw.gs.in_primitive = regs.primitive_type;
        }
        for (u32 stream_id = 0; stream_id < Shader::GsMaxOutputStreams; ++stream_id) {
            info.hw.gs.out_primitive[stream_id] =
                regs.vgt_gs_out_prim_type.GetPrimitiveType(stream_id);
        }
        info.hw.gs.in_vertex_data_size = regs.vgt_esgs_ring_itemsize;
        info.hw.gs.out_vertex_data_size = regs.vgt_gs_vert_itemsize[0];
        info.hw.gs.mode = regs.vgt_gs_mode.mode;
        const auto params_vc = AmdGpu::GetParams(regs.vs_program);
        info.hw.gs.vs_copy = params_vc.code;
        info.hw.gs.vs_copy_hash = params_vc.hash;
        DumpShader(info.hw.gs.vs_copy, info.hw.gs.vs_copy_hash, Shader::HwStage::Vertex, 0,
                   "copy.bin");
        break;
    }
    case HwStage::Vertex: {
        BuildCommon(regs.vs_program);
        info.hw.vs.user_clip_plane_mask = regs.clipper_control.user_clip_plane_enable;
        info.hw.vs.num_outputs = MapOutputs(info.hw.vs.outputs, regs.vs_output_control);
        ApplyHostRuntimeInfo(info, SourceHostFlags(regs, stage), profile);
        info.hw.vs.clip_disable = regs.IsClipDisabled();
        info.hw.vs.motion_vectors = sel.motion;
        break;
    }
    case HwStage::Fragment: {
        BuildCommon(regs.ps_program);
        info.hw.fs.en_flags = regs.ps_input_ena;
        info.hw.fs.addr_flags = regs.ps_input_addr;
        info.hw.fs.num_inputs = regs.num_interp;
        info.hw.fs.front_face_all_bits = regs.barycentric_control.front_face_all_bits;
        info.hw.fs.num_samples =
            regs.ps_input_addr.sample_coverage_ena && regs.ps_input_ena.sample_coverage_ena
                ? regs.aa_config.NumSamples()
                : 1;
        info.hw.fs.z_export_format = regs.z_export_format;
        info.hw.fs.motion_vectors = sel.motion;
        u8 stencil_ref_export_enable = regs.depth_shader_control.stencil_op_val_export_enable |
                                       regs.depth_shader_control.stencil_test_val_export_enable;
        info.hw.fs.mrtz_mask = regs.depth_shader_control.z_export_enable |
                               (stencil_ref_export_enable << 1) |
                               (regs.depth_shader_control.mask_export_enable << 2) |
                               (regs.depth_shader_control.coverage_to_mask_enable << 3);
        const auto& cb0_blend = regs.blend_control[0];
        if (cb0_blend.enable) {
            info.hw.fs.dual_source_blending =
                LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.color_dst_factor) ||
                LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.color_src_factor);
            if (cb0_blend.separate_alpha_blend) {
                info.hw.fs.dual_source_blending |=
                    LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.alpha_dst_factor) ||
                    LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.alpha_src_factor);
            }
        } else {
            info.hw.fs.dual_source_blending = false;
        }
        const auto& ps_inputs = regs.ps_inputs;
        for (u32 i = 0; i < regs.num_interp; i++) {
            info.hw.fs.inputs[i] = {
                .param_index = u8(ps_inputs[i].input_offset),
                .is_default = bool(ps_inputs[i].use_default),
                .is_flat = bool(ps_inputs[i].flat_shade),
                .default_value = u8(ps_inputs[i].default_value),
            };
        }
        for (u32 i = 0; i < Shader::MaxColorBuffers; i++) {
            info.hw.fs.color_buffers[i] = sel.graphics_key.color_buffers[i];
        }
        ApplyHostRuntimeInfo(info, SourceHostFlags(regs, stage), profile);
        break;
    }
    case HwStage::Compute: {
        const auto& cs_pgm = liverpool->GetCsRegs();
        info.props.num_user_data = cs_pgm.settings.num_user_regs;
        info.props.num_allocated_vgprs = cs_pgm.settings.num_vgprs * 4;
        info.props.fp_denorm_mode32 = cs_pgm.settings.fp_denorm_mode32;
        info.props.fp_denorm_mode16_64 = cs_pgm.settings.fp_denorm_mode64;
        info.props.fp_round_mode32 = cs_pgm.settings.fp_round_mode32;
        info.props.fp_round_mode16_64 = cs_pgm.settings.fp_round_mode64;
        info.hw.cs.workgroup_size = {cs_pgm.num_thread_x.full, cs_pgm.num_thread_y.full,
                                     cs_pgm.num_thread_z.full};
        info.hw.cs.tgid_enable = {cs_pgm.IsTgidEnabled(0), cs_pgm.IsTgidEnabled(1),
                                  cs_pgm.IsTgidEnabled(2)};
        info.hw.cs.shared_memory_size = cs_pgm.SharedMemSize();
        break;
    }
    default:
        break;
    }
    switch (l_stage) {
    case SwStage::Vertex:
        info.sw.vs.step_rate_0 = regs.vgt_instance_step_rate_0;
        info.sw.vs.step_rate_1 = regs.vgt_instance_step_rate_1;
        info.sw.vs.vertex_sgpr_offset = sel.draw_indirect_params.vertex_sgpr_offset;
        info.sw.vs.instance_sgpr_offset = sel.draw_indirect_params.instance_sgpr_offset;
        info.sw.vs.tess_emulated_primitive =
            regs.primitive_type == AmdGpu::PrimitiveType::RectList ||
            regs.primitive_type == AmdGpu::PrimitiveType::QuadList;
        break;
    case SwStage::TessellationControl: {
        info.sw.tcs.num_input_control_points = regs.ls_hs_config.hs_input_control_points;
        info.sw.tcs.num_threads = regs.ls_hs_config.hs_output_control_points;
        info.sw.tcs.tess_type = regs.tess_config.type;
        info.sw.tcs.offchip_lds_enable = regs.hs_program.settings.oc_lds_en;
        break;
    }
    case SwStage::TessellationEval: {
        info.sw.tes.tess_type = regs.tess_config.type;
        info.sw.tes.tess_topology = regs.tess_config.topology;
        info.sw.tes.tess_partitioning = regs.tess_config.partitioning;
        break;
    }
    default:
        break;
    }
    return info;
}

PipelineCache::PipelineCache(const Instance& instance_, Scheduler& scheduler_,
                             AmdGpu::Liverpool* liverpool_, u32 sparse_page_shift)
    : instance{instance_}, scheduler{scheduler_}, liverpool{liverpool_},
      desc_heap{instance, scheduler.GetWorkSemaphore(), DescriptorHeapSizes} {
    sel.regs = &liverpool->regs;
    const auto& vk12_props = instance.GetVk12Properties();
    profile = Shader::Profile{
        .max_viewport_width = instance.GetMaxViewportWidth(),
        .max_viewport_height = instance.GetMaxViewportHeight(),
        .max_shared_memory_size = instance.MaxComputeSharedMemorySize(),
        .supported_spirv = SpirvVersion1_6,
        .subgroup_size = instance.SubgroupSize(),
        .sparse_page_shift = sparse_page_shift,
        .support_int8 = instance.IsShaderInt8Supported(),
        .support_int16 = instance.IsShaderInt16Supported(),
        .support_int64 = instance.IsShaderInt64Supported(),
        .support_float16 = instance.IsShaderFloat16Supported(),
        .support_float64 = instance.IsShaderFloat64Supported(),
        .supports_denorm_behavior_independence =
            vk12_props.denormBehaviorIndependence != vk::ShaderFloatControlsIndependence::eNone,
        .supports_rounding_mode_independence =
            vk12_props.roundingModeIndependence != vk::ShaderFloatControlsIndependence::eNone,
        .support_fp16_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat16),
        .support_fp16_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat16),
        .support_fp16_round_to_zero = bool(vk12_props.shaderRoundingModeRTZFloat16),
        .support_fp32_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat32),
        .support_fp32_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat32),
        .support_fp32_round_to_zero = bool(vk12_props.shaderRoundingModeRTZFloat32),
        .support_fp64_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat64),
        .support_fp64_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat64),
        .support_fp64_round_to_zero = bool(vk12_props.shaderRoundingModeRTZFloat64),
        .support_fp16_signed_zero_inf_nan_preserve =
            bool(vk12_props.shaderSignedZeroInfNanPreserveFloat16),
        .support_fp32_signed_zero_inf_nan_preserve =
            bool(vk12_props.shaderSignedZeroInfNanPreserveFloat32),
        .support_fp64_signed_zero_inf_nan_preserve =
            bool(vk12_props.shaderSignedZeroInfNanPreserveFloat64),
        .supports_image_load_store_lod = instance_.IsImageLoadStoreLodSupported(),
        .supports_native_cube_calc = instance_.IsAmdGcnShaderSupported(),
        .supports_trinary_minmax = instance_.IsAmdShaderTrinaryMinMaxSupported(),
        .supports_buffer_fp32_atomic_min_max =
            instance_.IsShaderAtomicFloatBuffer32MinMaxSupported(),
        .supports_image_fp32_atomic_min_max = instance_.IsShaderAtomicFloatImage32MinMaxSupported(),
        .supports_buffer_int64_atomics = instance_.IsBufferInt64AtomicsSupported(),
        .supports_shared_int64_atomics = instance_.IsSharedInt64AtomicsSupported(),
        .supports_workgroup_explicit_memory_layout =
            instance_.IsWorkgroupMemoryExplicitLayoutSupported(),
        .supports_amd_shader_explicit_vertex_parameter =
            instance_.IsAmdShaderExplicitVertexParameterSupported(),
        .supports_fragment_shader_barycentric = instance_.IsFragmentShaderBarycentricSupported(),
        .supports_shader_subgroup_clock = instance_.IsShaderSubgroupClockSupported(),
        .needs_manual_interpolation = instance.IsFragmentShaderBarycentricSupported() &&
                                      instance.GetDriverID() == vk::DriverId::eNvidiaProprietary,
        // bbport: older NVIDIA (Pascal) has no barycentrics; BB_INTERP_INT_FIX=0/1 overrides.
        .needs_integer_interpolation_fix = [&] {
            if (const char* env = std::getenv("BB_INTERP_INT_FIX")) {
                return env[0] == '1';
            }
            return !instance.IsFragmentShaderBarycentricSupported() &&
                   instance.GetDriverID() == vk::DriverId::eNvidiaProprietary;
        }(),
        .needs_lds_barriers = instance.GetDriverID() == vk::DriverId::eNvidiaProprietary ||
                              instance.GetDriverID() == vk::DriverId::eMesaKosmickrisp,
        .needs_buffer_offsets = instance.StorageMinAlignment() > 4,
        .needs_unorm_fixup = instance.GetDriverID() == vk::DriverId::eMesaKosmickrisp,
        .needs_clip_distance_emulation = instance.GetDriverID() == vk::DriverId::eNvidiaProprietary,
        .supports_shader_stencil_export = instance_.IsShaderStencilExportSupported(),
        .supports_depth_clip_control = instance_.IsDepthClipControlSupported(),
        // bbport BB_LAYER_MEMORY: buffers over nearly all memory go through the page table.
        .paged_buffers = VideoCore::BufferCache::LayerPagedActive(),
    };
    // bbport: created before the warm-up, whose preloaded pipelines are built with it.
    auto [cache_result, cache] = instance.GetDevice().createPipelineCacheUnique({});
    ASSERT_MSG(cache_result == vk::Result::eSuccess, "Failed to create pipeline cache: {}",
               vk::to_string(cache_result));
    pipeline_cache = std::move(cache);

    if (instance.IsGraphicsPipelineLibrarySupported()) {
        gpl = std::make_unique<GplLibraryCache>(instance.GetDevice());
    }
    // bbport: with the startup precompile, the presenter runs the warm-up with its screen.
    if (!PrecompileEnabled()) {
        WarmUp();
    }
}

PipelineCache::~PipelineCache() {
    compiler.Stop();
}

bool PipelineCache::PrecompileEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_SHADER_PRECOMPILE");
        return !(env && env[0] == '0');
    }();
    return enabled;
}

// bbport: shader/pipeline compile time on the GPU thread, reported by BB_FRAME_STATS.
std::atomic<u64> g_bb_compile_ns;
std::atomic<u32> g_bb_compiles;
namespace {
struct CompileTimer {
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~CompileTimer() {
        g_bb_compile_ns += u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now() - start)
                                   .count());
        ++g_bb_compiles;
    }
};
} // namespace

// ---- bbport: pipeline jobs -------------------------------------------------------------------

std::shared_ptr<PipelineCache::GraphicsJob> PipelineCache::MakeGraphicsJob(
    const GraphicsPipeline::SerializationSupport& sdata, BuildMode mode) {
    auto job = std::make_shared<GraphicsJob>();
    job->cache = this;
    job->key = sel.graphics_key;
    job->mode = mode;
    for (u32 i = 0; i < MaxShaderStages; ++i) {
        if (sel.infos[i]) {
            job->owned[i].emplace(*sel.infos[i]);
            job->build_infos[i] = &*job->owned[i];
        }
    }
    job->bound_infos = sel.infos;
    job->runtime_infos = sel.runtime_infos;
    job->fetch_shader = sel.fetch_shader;
    job->modules = sel.modules;
    job->sdata = sdata;
    return job;
}

const GraphicsPipeline* PipelineCache::PublishGraphics(GraphicsJob& job) {
    if (job.published) {
        const auto it = graphics_pipelines.find(job.key);
        return it != graphics_pipelines.end() ? it->second.get() : nullptr;
    }
    job.published = true;
    if (pending_graphics.erase(job.key) != 0) {
        --BbCompileProgress::async_pending;
    }
    if (job.failed || !job.result || !job.IsDone()) {
        return nullptr;
    }
    job.result->RebindStages(job.bound_infos);
    const auto [it, is_new] = graphics_pipelines.try_emplace(job.key);
    if (!is_new) {
        return it->second.get();
    }
    it.value() = std::move(job.result);
    GraphicsPipeline* pipeline = it.value().get();
    if (job.register_data) {
        RegisterPipelineData(job.key, std::hash<GraphicsPipelineKey>{}(job.key), job.sdata);
        ++num_new_pipelines;
        ++BbCompileProgress::async_done;
        if (EmulatorSettings.IsShaderCollect()) {
            for (u32 stage = 0; stage < MaxShaderStages; ++stage) {
                if (job.bound_infos[stage]) {
                    module_related_pipelines[job.modules[stage]].emplace_back(job.key);
                }
            }
        }
    }
    if (gpl && pipeline->IsFastLinked() && !job.optimize_inline) {
        QueueOptimize(pipeline);
    }
    return pipeline;
}

void PipelineCache::QueueOptimize(GraphicsPipeline* pipeline) {
    auto job = std::make_shared<OptimizeJob>();
    job->pipeline = pipeline;
    job->gpl = gpl.get();
    EnsureRuntimeWorkers();
    compiler.Enqueue(std::move(job), PipelineCompiler::PriorityOptimize);
}

void PipelineCache::EnsureRuntimeWorkers() {
    if (compiler.NumWorkers() == 0 && !compiler.Stopped()) {
        compiler.Start(PipelineCompiler::GameplayWorkers(), true);
    }
}

void PipelineCache::PublishCompleted() {
    for (const auto& done : compiler.DrainCompleted()) {
        if (auto* job = dynamic_cast<GraphicsJob*>(done.get())) {
            if (!job->published && job->mode == BuildMode::Async) {
                PublishGraphics(*job);
            }
        }
    }
}

void PipelineCache::WaitJob(const std::shared_ptr<GraphicsJob>& job) {
    const u64 us = compiler.Wait(job);
    ++BbCompileProgress::sync_waits;
    static const bool stats = [] {
        const char* env = std::getenv("BB_FRAME_STATS");
        return env && env[0] == '1';
    }();
    if (stats && us > 50'000) {
        std::printf("Shaders: GPU thread waited %.1f ms for pipeline %#llx\n", us / 1e3,
                    static_cast<unsigned long long>(std::hash<GraphicsPipelineKey>{}(job->key)));
    }
}

namespace {
struct AsyncPolicy {
    bool aggressive = false;
    u32 max_skip_frames = 8;
    u32 queue = 256;
};
const AsyncPolicy& GetAsyncPolicy() {
    static const AsyncPolicy policy = [] {
        AsyncPolicy p;
        if (const char* env = std::getenv("BB_ASYNC_SHADERS_POLICY")) {
            p.aggressive = std::strcmp(env, "aggressive") == 0;
        }
        if (const char* env = std::getenv("BB_ASYNC_SHADERS_MAX_SKIP_FRAMES")) {
            p.max_skip_frames = u32(std::max(0, std::atoi(env)));
        }
        if (const char* env = std::getenv("BB_ASYNC_SHADERS_QUEUE")) {
            p.queue = u32(std::max(1, std::atoi(env)));
        }
        return p;
    }();
    return policy;
}
} // namespace

void PipelineCache::TrackTargets() {
    const auto& regs = liverpool->regs;
    const VAddr cb0 = regs.color_buffers[0] ? VAddr(regs.color_buffers[0].Address()) : 0;
    const bool display = cb0 != 0 && FrameCapture::IsDisplayBuffer(cb0);
    if (display && !display_pass) {
        ++async_frame; // the pass copying a finished frame to a display buffer
    }
    display_pass = display;

    std::array<VAddr, AmdGpu::NUM_COLOR_BUFFERS + 2> targets;
    u32 count = 0;
    if (regs.color_control.mode != AmdGpu::ColorControl::OperationMode::Disable) {
        for (u32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS; ++cb) {
            if (regs.color_buffers[cb] && regs.color_target_mask.GetMask(cb)) {
                targets[count++] = VAddr(regs.color_buffers[cb].Address());
            }
        }
    }
    if (regs.depth_buffer.DepthValid() && regs.depth_control.depth_enable &&
        regs.depth_control.depth_write_enable) {
        targets[count++] = VAddr(regs.depth_buffer.DepthAddress()) | 1;
    }
    if (regs.depth_buffer.StencilValid() && regs.depth_control.stencil_enable) {
        targets[count++] = VAddr(regs.depth_buffer.StencilAddress()) | 2;
    }
    u64 sig = count;
    for (u32 i = 0; i < count; ++i) {
        sig = (sig ^ targets[i]) * 0x100000001b3ull;
    }
    if (sig == last_target_sig && async_frame == last_target_frame) {
        return; // same pass in the same frame
    }
    last_target_sig = sig;
    last_target_frame = async_frame;

    // Every target of the draw was a target in 2 of the 3 previous frames: not a one-off
    // render-to-texture (those are never skipped).
    constexpr size_t TableSize = 8192;
    if (target_table.empty()) {
        target_table.resize(TableSize);
    }
    bool stable = count > 0;
    for (u32 i = 0; i < count; ++i) {
        const VAddr addr = targets[i];
        size_t slot = size_t((u64(addr) * 0x9E3779B97F4A7C15ull) >> 51) & (TableSize - 1);
        TargetSlot* found = nullptr;
        for (u32 probe = 0; probe < 32; ++probe, slot = (slot + 1) & (TableSize - 1)) {
            auto& s = target_table[slot];
            if (s.addr == addr || s.addr == 0) {
                found = &s;
                break;
            }
        }
        if (!found) {
            std::fill(target_table.begin(), target_table.end(), TargetSlot{});
            stable = false;
            continue;
        }
        if (found->addr == 0) {
            *found = {addr, async_frame, 1};
            stable = false;
            continue;
        }
        const u32 shift = async_frame - found->last_frame;
        if (shift != 0) {
            found->mask = shift >= 32 ? 0u : found->mask << shift;
            found->last_frame = async_frame;
        }
        found->mask |= 1;
        stable &= std::popcount(found->mask & 0xEu) >= 2;
    }
    targets_stable = stable;
}

bool PipelineCache::DrawSkippable() const {
    if (FrameCapture::Active() || display_pass || !targets_stable) {
        return false;
    }
    // Screen-space passes (upscaler input, camera motion, UI) keep the frame's structure.
    if (!GetAsyncPolicy().aggressive && liverpool->regs.IsClipDisabled()) {
        return false;
    }
    // Draws that write memory besides their targets feed later work.
    for (const auto* info : sel.infos) {
        if (!info) {
            continue;
        }
        for (const auto& buffer : info->buffers) {
            if (buffer.is_written) {
                return false;
            }
        }
        for (const auto& image : info->images) {
            if (image.is_written) {
                return false;
            }
        }
    }
    return true;
}

bool PipelineCache::PrepareGraphicsPipeline(PipelineSelection& worker_sel) {
    // Tessellation stages read constant buffers from memory at selection time: not prepared.
    if (worker_sel.regs->stage_enable.hs_en) {
        return false;
    }
    return RefreshGraphicsKey(worker_sel) && !worker_sel.worker->failed;
}

const GraphicsPipeline* PipelineCache::TryPreparedPipeline(const PreparedDraw& prepared) {
    if (prepared.state.load(std::memory_order_acquire) != PreparedDraw::Ready ||
        prepared.reg_checksum != liverpool->gfx_reg_checksum) {
        return nullptr;
    }
    // Same registers; the stage programs and their flattened user data (which also covers
    // everything read from guest memory for the specialization) must match as well. This is
    // the per-stage work GetProgram does on the regular path.
    const auto& regs = liverpool->regs;
    for (u32 i = 0; i < prepared.num_stages; ++i) {
        const auto& stage = prepared.stages[i];
        const auto* pgm = regs.ProgramForStage(static_cast<u32>(stage.hw_stage));
        if (!pgm || !pgm->Address<u32*>()) {
            return nullptr;
        }
        const auto params = AmdGpu::GetParams(*pgm);
        if (params.hash != stage.hash) {
            return nullptr;
        }
        auto& info = const_cast<Program*>(stage.program)->info;
        info.pgm_base = params.Base();
        info.user_data = params.user_data;
        info.RefreshFlatBuf();
        if (info.pgm_base != stage.pgm_base || info.flattened_ud_buf.size() != stage.flat_size ||
            std::memcmp(info.flattened_ud_buf.data(), stage.flat,
                        stage.flat_size * sizeof(u32)) != 0) {
            return nullptr;
        }
    }
    const auto it = graphics_pipelines.find(prepared.key);
    return it != graphics_pipelines.end() ? it->second.get() : nullptr;
}

const GraphicsPipeline* PipelineCache::GetGraphicsPipeline(const DrawIndirectParams params,
                                                           const PreparedDraw* prepared) {
    // bbport: results of the pipeline compiler (async pipelines, optimized library links).
    if (compiler.HasCompleted()) {
        PublishCompleted();
    }
    const bool async = BbSettings::Get().async_shaders.load(std::memory_order_relaxed);
    if (async) {
        TrackTargets();
    }
    used_prepared = nullptr;
    if (prepared) {
        if (const auto* pipeline = TryPreparedPipeline(*prepared)) {
            used_prepared = prepared;
            return pipeline;
        }
    }
    sel.draw_indirect_params = params;
    if (!RefreshGraphicsKey(sel)) {
        return nullptr;
    }
    if (!pending_graphics.empty() || async) {
        if (const auto found = graphics_pipelines.find(sel.graphics_key);
            found != graphics_pipelines.end()) {
            return found->second.get();
        }
        // bbport: a pipeline a worker builds (BB_ASYNC_SHADERS): never compiled twice.
        if (const auto pending = pending_graphics.find(sel.graphics_key);
            pending != pending_graphics.end()) {
            const auto job = pending->second;
            if (!job->IsFinished()) {
                bool skip = async && DrawSkippable();
                if (skip) {
                    if (job->first_skip_frame == ~0u) {
                        job->first_skip_frame = async_frame;
                    } else if (async_frame - job->first_skip_frame >
                               GetAsyncPolicy().max_skip_frames) {
                        ++BbCompileProgress::timeouts;
                        skip = false;
                    }
                }
                if (skip) {
                    ++BbCompileProgress::skipped_draws;
                    sel.fetch_shader.reset();
                    return nullptr;
                }
                WaitJob(job);
            }
            if (const auto* pipeline = PublishGraphics(*job)) {
                sel.fetch_shader.reset();
                return pipeline;
            }
            // Failed off the GPU thread: built here below (asserts on a driver failure).
        } else if (async && BbCompileProgress::async_pending.load() < GetAsyncPolicy().queue &&
                   DrawSkippable()) {
            GraphicsPipeline::SerializationSupport sdata{};
            GraphicsPipeline::FillSerializationSupport(instance, sel.graphics_key, sel.infos,
                                                       sel.runtime_infos, sel.fetch_shader, sdata);
            auto job = MakeGraphicsJob(sdata, BuildMode::Async);
            job->register_data = true;
            if (gpl && GraphicsPipeline::LibrariesReady(instance, *gpl, job->key, job->build_infos,
                                                        job->modules, job->sdata)) {
                // Every part exists: a fast link, no need to skip the draw.
                CompileTimer timer;
                try {
                    job->Run();
                    job->state.store(PipelineCompiler::Job::Done, std::memory_order_release);
                } catch (const std::exception&) {
                    job->failed = true;
                    job->state.store(PipelineCompiler::Job::Done, std::memory_order_release);
                }
                if (const auto* pipeline = PublishGraphics(*job)) {
                    sel.fetch_shader.reset();
                    return pipeline;
                }
            } else {
                job->progress = true;
                job->first_skip_frame = async_frame;
                EnsureRuntimeWorkers();
                if (compiler.Enqueue(job, PipelineCompiler::PriorityLive)) {
                    pending_graphics.emplace(job->key, job);
                    ++BbCompileProgress::async_pending;
                    BbCompileProgress::Add(BbCompileProgress::Phase::Runtime);
                    ++BbCompileProgress::skipped_draws;
                    sel.fetch_shader.reset();
                    return nullptr;
                }
            }
            // Not queued (shutting down) or failed: built here as usual.
        }
    }
    const auto [it, is_new] = graphics_pipelines.try_emplace(sel.graphics_key);
    if (is_new) {
        const auto pipeline_hash = std::hash<GraphicsPipelineKey>{}(sel.graphics_key);
        LOG_INFO(Render_Vulkan, "Compiling graphics pipeline {:#x}", pipeline_hash);
        CompileTimer timer;

        GraphicsPipeline::SerializationSupport sdata{};
        it.value() = std::make_unique<GraphicsPipeline>(
            instance, scheduler, desc_heap, profile, sel.graphics_key, *pipeline_cache, sel.infos,
            sel.runtime_infos, sel.fetch_shader, sel.modules, sdata, BuildMode::Live, gpl.get());

        RegisterPipelineData(sel.graphics_key, pipeline_hash, sdata);
        ++num_new_pipelines;
        if (gpl && it.value()->IsFastLinked()) {
            QueueOptimize(it.value().get());
        }

        if (EmulatorSettings.IsShaderCollect()) {
            for (auto stage = 0; stage < MaxShaderStages; ++stage) {
                if (sel.infos[stage]) {
                    auto& m = sel.modules[stage];
                    module_related_pipelines[m].emplace_back(sel.graphics_key);
                }
            }
        }
        sel.fetch_shader.reset();
    }
    return it->second.get();
}

const ComputePipeline* PipelineCache::GetComputePipeline() {
    if (!RefreshComputeKey()) {
        return nullptr;
    }
    const auto [it, is_new] = compute_pipelines.try_emplace(compute_key);
    if (is_new) {
        const auto pipeline_hash = std::hash<ComputePipelineKey>{}(compute_key);
        LOG_INFO(Render_Vulkan, "Compiling compute pipeline {:#x}", pipeline_hash);
        CompileTimer timer;

        ComputePipeline::SerializationSupport sdata{};
        it.value() = std::make_unique<ComputePipeline>(instance, scheduler, desc_heap, profile,
                                                       *pipeline_cache, compute_key, *sel.infos[0],
                                                       sel.modules[0], sdata, false);
        RegisterPipelineData(compute_key, sdata);
        ++num_new_pipelines;

        if (EmulatorSettings.IsShaderCollect()) {
            auto& m = sel.modules[0];
            module_related_pipelines[m].emplace_back(compute_key);
        }
    }
    return it->second.get();
}

bool PipelineCache::RefreshGraphicsKey(PipelineSelection& sel) {
    std::memset(&sel.graphics_key, 0, sizeof(GraphicsPipelineKey));
    const auto& regs = (*sel.regs);
    auto& key = sel.graphics_key;

    const bool db_enabled = regs.depth_buffer.DepthValid() || regs.depth_buffer.StencilValid();

    key.z_format = regs.depth_buffer.DepthValid() ? regs.depth_buffer.z_info.format
                                                  : AmdGpu::DepthBuffer::ZFormat::Invalid;
    key.stencil_format = regs.depth_buffer.StencilValid()
                             ? regs.depth_buffer.stencil_info.format
                             : AmdGpu::DepthBuffer::StencilFormat::Invalid;
    key.depth_clamp_enable = !regs.depth_render_override.disable_viewport_clamp;
    key.depth_clip_enable = regs.clipper_control.ZclipEnable();
    key.clip_space = regs.clipper_control.clip_space;
    key.provoking_vtx_last = regs.polygon_control.provoking_vtx_last;
    key.prim_type = regs.primitive_type;
    key.polygon_mode = regs.polygon_control.PolyMode();
    key.patch_control_points =
        regs.stage_enable.hs_en ? regs.ls_hs_config.hs_input_control_points : 0;
    key.logic_op = regs.color_control.rop3;
    key.depth_samples = db_enabled ? regs.depth_buffer.NumSamples() : 1;
    key.num_samples = key.depth_samples;
    key.cb_shader_mask = regs.color_shader_mask;

    const bool skip_cb_binding =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;

    // Only potentially animated G-buffer draws may use the extra motion attachment. The
    // ordinary stage variant is needed first to inspect the vertex shader's resources.
    bool motion_possible = false;
    {
        u32 bound = 0;
        for (s32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS && !skip_cb_binding; ++cb) {
            bound += (regs.color_buffers[cb] && regs.color_target_mask.GetMask(cb)) ? 1 : 0;
        }
        motion_possible = Shader::MotionVectors::positions_address != 0 && bound >= 5 &&
                          !regs.color_buffers[Shader::MotionVectors::Output] &&
                          regs.depth_buffer.DepthValid() &&
                          regs.depth_buffer.NumSamples() == 1 && !regs.IsClipDisabled() &&
                          regs.stage_enable.raw == AmdGpu::ShaderStageEnable::VgtStages::Vs;
    }
    sel.motion = false;

    // First pass to fill render target information needed by shader recompiler
    for (s32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS && !skip_cb_binding; ++cb) {
        const auto& col_buf = regs.color_buffers[cb];
        if (!col_buf || !regs.color_target_mask.GetMask(cb)) {
            // No attachment bound or writing to it is disabled.
            continue;
        }

        // Fill color target information
        auto& color_buffer = key.color_buffers[cb];
        color_buffer.data_format = col_buf.GetDataFmt();
        color_buffer.num_format = col_buf.GetNumberFmt();
        color_buffer.num_conversion = col_buf.GetNumberConversion();
        color_buffer.export_format = regs.color_export_format.GetFormat(cb);
        color_buffer.swizzle = col_buf.Swizzle();

        const auto& bc = regs.blend_control[cb];
        color_buffer.blend_self_scale =
            bc.enable && !col_buf.info.blend_bypass &&
            (bc.color_func == AmdGpu::BlendControl::BlendFunc::Min ||
             bc.color_func == AmdGpu::BlendControl::BlendFunc::Max) &&
            bc.color_src_factor == AmdGpu::BlendControl::BlendFactor::SrcColor &&
            bc.color_dst_factor == AmdGpu::BlendControl::BlendFactor::DstColor;
    }

    // Compile and bind shader stages
    if (!RefreshGraphicsStages(sel)) {
        return false;
    }
    if (motion_possible) {
        const auto* vs = sel.infos[static_cast<u32>(Shader::SwStage::Vertex)];
        if (vs) {
            // Shaders with a bone palette (motion_history.h). Small skeletons (weapons,
            // props) also include static world pieces: the rasterizer gives those
            // history only while their constants change.
            for (const auto& resource : vs->buffers) {
                if (resource.IsSpecial()) continue;
                const auto buffer = resource.GetSharp(*vs);
                if (buffer.Valid() && buffer.GetStride() == 16 &&
                    Motion::ClassifyBuffer(buffer.GetSize()) != Motion::BufferRole::Other) {
                    sel.motion = true;
                    break;
                }
            }
        }
        // BB_MOTION_SELECT_LOG=1: each G-buffer vertex shader once, with its buffer sizes
        // and the selection, to find animated models that the size rule leaves out.
        static const bool select_log = [] {
            const char* value = std::getenv("BB_MOTION_SELECT_LOG");
            return value && value[0] == '1';
        }();
        if (select_log && vs) {
            std::string sizes;
            for (const auto& resource : vs->buffers) {
                if (resource.IsSpecial()) continue;
                const auto buffer = resource.GetSharp(*vs);
                sizes += fmt::format(" {}/{}", buffer.GetSize(), buffer.GetStride());
            }
            static std::mutex log_mutex;
            static std::unordered_set<u64> logged;
            std::scoped_lock lock{log_mutex};
            if (logged.insert(vs->pgm_hash ^ std::hash<std::string>{}(sizes)).second) {
                std::printf("Motion select: vs %016llx %s, buffers (size/stride):%s\n",
                            static_cast<unsigned long long>(vs->pgm_hash),
                            sel.motion ? "ON " : "off", sizes.c_str());
            }
        }
        // Keep the old broad path available for visual A/B tests.
        static const bool all_motion = [] {
            const char* value = std::getenv("BB_OBJECT_MOTION_ALL");
            return value && value[0] == '1';
        }();
        if (all_motion) {
            sel.motion = true;
        }
        if (sel.motion && !RefreshGraphicsStages(sel)) {
            return false;
        }
    }

    // Second pass to mask out render targets not written by shader and fill remaining info
    u8 color_samples = 0;
    bool all_color_samples_same = true;
    for (s32 cb = 0; cb < key.num_color_attachments && !skip_cb_binding; ++cb) {
        const auto& col_buf = regs.color_buffers[cb];
        const u32 target_mask = regs.color_target_mask.GetMask(cb);
        if (!col_buf || !target_mask) {
            continue;
        }
        if ((key.mrt_mask & (1u << cb)) == 0) {
            std::memset(&key.color_buffers[cb], 0, sizeof(Shader::PsColorBuffer));
            continue;
        }

        // Fill color blending information
        if (regs.blend_control[cb].enable && !col_buf.info.blend_bypass) {
            key.blend_controls[cb] = regs.blend_control[cb];
        }

        // Apply swizzle to target mask
        key.write_masks[cb] =
            vk::ColorComponentFlags{key.color_buffers[cb].swizzle.ApplyMask(target_mask)};

        // Fill color samples
        const u8 prev_color_samples = std::exchange(color_samples, col_buf.NumSamples());
        all_color_samples_same &= color_samples == prev_color_samples || prev_color_samples == 0;
        key.color_samples[cb] = color_samples;
        key.num_samples = std::max(key.num_samples, color_samples);
    }

    if (sel.motion) {
        constexpr u32 mv = Shader::MotionVectors::Output;
        key.motion_vectors = 1;
        key.mrt_mask |= 1u << mv;
        key.num_color_attachments = mv + 1;
        auto& color_buffer = key.color_buffers[mv];
        color_buffer.data_format = AmdGpu::DataFormat::Format32_32_32_32;
        color_buffer.num_format = AmdGpu::NumberFormat::Float;
        color_buffer.swizzle = AmdGpu::IdentityMapping;
        key.write_masks[mv] = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                              vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
        key.color_samples[mv] = 1;
    }

    // Force all color samples to match depth samples to avoid unsupported MSAA configuration
    if (color_samples != 0) {
        const bool depth_mismatch = db_enabled && color_samples != key.depth_samples;
        if (!all_color_samples_same && !instance.IsMixedAnySamplesSupported() ||
            all_color_samples_same && depth_mismatch && !instance.IsMixedDepthSamplesSupported()) {
            key.color_samples.fill(key.depth_samples);
            key.num_samples = key.depth_samples;
        }
    }

    return true;
}

bool PipelineCache::RefreshGraphicsStages(PipelineSelection& sel) {
    const auto& regs = (*sel.regs);
    auto& key = sel.graphics_key;
    sel.fetch_shader = std::nullopt;

    Shader::Backend::Bindings binding{};
    const auto bind_stage = [&](HwStage stage_in, SwStage stage_out) -> bool {
        const auto stage_in_idx = static_cast<u32>(stage_in);
        const auto stage_out_idx = static_cast<u32>(stage_out);
        if (!regs.stage_enable.IsStageEnabled(stage_in_idx)) {
            key.stage_hashes[stage_out_idx] = 0;
            sel.infos[stage_out_idx] = nullptr;
            return false;
        }

        const auto* pgm = regs.ProgramForStage(stage_in_idx);
        if (!pgm || !pgm->Address<u32*>()) {
            key.stage_hashes[stage_out_idx] = 0;
            sel.infos[stage_out_idx] = nullptr;
            return false;
        }

        const auto params = AmdGpu::GetParams(*pgm);
        std::optional<Shader::Gcn::FetchShaderData> fetch_shader_;
        std::tie(sel.infos[stage_out_idx], sel.modules[stage_out_idx], fetch_shader_,
                 key.stage_hashes[stage_out_idx]) =
            GetProgram(sel, stage_in, stage_out, params, binding);
        if (fetch_shader_) {
            sel.fetch_shader = fetch_shader_;
        }
        return true;
    };

    sel.infos.fill(nullptr);
    sel.modules.fill(nullptr);

    bind_stage(HwStage::Fragment, SwStage::Fragment);

    const auto* fs_info = sel.infos[static_cast<u32>(SwStage::Fragment)];
    key.mrt_mask = fs_info ? fs_info->mrt_mask : 0u;
    key.num_color_attachments = std::bit_width(key.mrt_mask);

    switch (regs.stage_enable.raw) {
    case AmdGpu::ShaderStageEnable::VgtStages::EsGs:
        if (!instance.IsGeometryStageSupported()) {
            LOG_WARNING(Render_Vulkan, "Geometry shader stage unsupported, skipping");
            return false;
        }
        if (regs.vgt_gs_mode.onchip || regs.vgt_strmout_config.raw) {
            LOG_WARNING(Render_Vulkan, "Geometry shader features unsupported, skipping");
            return false;
        }
        if (!bind_stage(HwStage::Export, SwStage::Vertex)) {
            return false;
        }
        if (!bind_stage(HwStage::Geometry, SwStage::Geometry)) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::LsHs:
        if (!instance.IsTessellationSupported()) {
            return false;
        }
        if (!bind_stage(HwStage::Hull, SwStage::TessellationControl)) {
            return false;
        }
        if (!bind_stage(HwStage::Vertex, SwStage::TessellationEval)) {
            return false;
        }
        if (!bind_stage(HwStage::Local, SwStage::Vertex)) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::LsHsEsGs:
        if (!instance.IsTessellationSupported()) {
            return false;
        }
        if (!instance.IsGeometryStageSupported()) {
            LOG_WARNING(Render_Vulkan, "Geometry shader stage unsupported, skipping");
            return false;
        }
        if (regs.vgt_gs_mode.onchip || regs.vgt_strmout_config.raw) {
            LOG_WARNING(Render_Vulkan, "Geometry shader features unsupported, skipping");
            return false;
        }
        if (!bind_stage(HwStage::Hull, SwStage::TessellationControl)) {
            return false;
        }
        if (!bind_stage(HwStage::Export, SwStage::TessellationEval)) {
            return false;
        }
        if (!bind_stage(HwStage::Local, SwStage::Vertex)) {
            return false;
        }
        if (!bind_stage(HwStage::Geometry, SwStage::Geometry)) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::Vs:
        bind_stage(HwStage::Vertex, SwStage::Vertex);
        break;
    default:
        LOG_WARNING(Render_Vulkan, "unimplemented shader stage {}", (u32)regs.stage_enable.raw);
        return false;
    }

    const auto* vs_info = sel.infos[static_cast<u32>(SwStage::Vertex)];
    if (vs_info && sel.fetch_shader && !instance.IsVertexInputDynamicState()) {
        // Without vertex input dynamic state, the pipeline needs to specialize on format.
        // Stride will still be handled outside the pipeline using dynamic state.
        u32 vertex_binding = 0;
        for (const auto& attrib : sel.fetch_shader->attributes) {
            const auto& buffer = attrib.GetSharp(*vs_info);
            ASSERT_MSG(vertex_binding < MaxVertexBufferCount,
                       "Vertex attribute binding count exceeded limit: {} >= {}", vertex_binding,
                       MaxVertexBufferCount);
            key.vertex_buffer_formats[vertex_binding++] =
                Vulkan::LiverpoolToVK::SurfaceFormat(buffer.GetDataFmt(), buffer.GetNumberFmt());
        }
    }

    return true;
}

bool PipelineCache::RefreshComputeKey() {
    Shader::Backend::Bindings binding{};
    const auto& cs_pgm = liverpool->GetCsRegs();
    const auto cs_params = AmdGpu::GetParams(cs_pgm);
    std::tie(sel.infos[0], sel.modules[0], sel.fetch_shader, compute_key.value) =
        GetProgram(sel, HwStage::Compute, SwStage::Compute, cs_params, binding);
    return true;
}

vk::ShaderModule PipelineCache::CompileModule(Shader::Info& info, Shader::RuntimeInfo& runtime_info,
                                              const std::span<const u32>& code, size_t perm_idx,
                                              Shader::Backend::Bindings& binding,
                                              std::vector<u32>* spv_out) {
    LOG_INFO(Render_Vulkan, "Compiling {} shader {:#x} {}", info.hw_stage, info.pgm_hash,
             perm_idx != 0 ? "(permutation)" : "");
    DumpShader(code, info.pgm_hash, info.hw_stage, perm_idx, "bin");
    CompileTimer timer;

    const auto ir_program = Shader::TranslateProgram(code, pools, info, runtime_info, profile);
    auto spv = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, ir_program, binding);
    DumpShader(spv, info.pgm_hash, info.hw_stage, perm_idx, "spv");

    vk::ShaderModule module;

    auto patch = GetShaderPatch(info.pgm_hash, info.hw_stage, perm_idx, "spv");
    const bool is_patched = patch && EmulatorSettings.IsPatchShaders();
    if (is_patched) {
        LOG_INFO(Loader, "Loaded patch for {} shader {:#x}", info.hw_stage, info.pgm_hash);
        module = CompileSPV(*patch, instance.GetDevice());
    } else {
        module = CompileSPV(spv, instance.GetDevice());
    }

    if (spv_out) {
        *spv_out = spv;
    }
    RegisterShaderBinary(std::move(spv), info.pgm_hash, perm_idx);

    const auto name = GetShaderName(info.hw_stage, info.pgm_hash, perm_idx);
    Vulkan::SetObjectName(instance.GetDevice(), module, name);
    if (EmulatorSettings.IsShaderCollect()) {
        DebugState.CollectShader(name, info.sw_stage, module, spv, code,
                                 patch ? *patch : std::span<const u32>{}, is_patched);
    }
    return module;
}

PipelineCache::Result PipelineCache::GetProgram(PipelineSelection& sel, HwStage hw_stage,
                                                SwStage sw_stage,
                                                const Shader::ShaderParams& params,
                                                Shader::Backend::Bindings& binding) {
    auto runtime_info = BuildRuntimeInfo(sel, hw_stage, sw_stage);
    if (sel.worker) {
        // bbport: draw-preparation worker: look up only, with the worker's own Info copy.
        auto& worker = *sel.worker;
        std::shared_lock lk{programs_mutex};
        const auto found_program = program_cache.find(params.hash);
        if (found_program == program_cache.end() || !found_program->second->info_template) {
            worker.failed = true;
            return {};
        }
        const Program* program = found_program->second.get();
        auto [it_info, new_info] = worker.infos.try_emplace(program, *program->info_template);
        auto& info = it_info->second;
        info.pgm_base = params.Base();
        info.user_data = params.user_data;
        // The walk of resource tables and the fetch shader parse read guest memory through
        // pointers in the registers; ahead of the GPU thread that memory may already be
        // reused. A fault returns here (runtime_fault_recover) and the draw is left to the GPU
        // thread. A jump out of the specialization leaks its partial allocations (rare).
        BbRecoverBuf recover;
        if (BB_RECOVER_SET(recover)) {
            worker.failed = true;
            return {};
        }
        runtime_fault_recover = &recover;
        info.RefreshFlatBuf();
        auto spec = Shader::StageSpecialization(info, runtime_info, profile, binding);
        runtime_fault_recover = nullptr;
        const auto it = std::ranges::find(program->modules, spec, &Program::Module::spec);
        if (it == program->modules.end()) {
            worker.failed = true;
            return {};
        }
        info.AddBindings(binding);
        const size_t perm_idx = std::distance(program->modules.begin(), it);
        worker.stages.push_back(
            {program, params.hash, hw_stage, info.pgm_base, &info.flattened_ud_buf});
        return std::make_tuple(&info, it->module, it->spec.fetch_shader_data,
                               HashCombine(params.hash, perm_idx));
    }

    auto it_pgm = program_cache.find(params.hash); // this thread is the only writer
    if (it_pgm == program_cache.end()) {
        auto new_program = std::make_unique<Program>(hw_stage, sw_stage, params);
        auto start = binding;
        // bbport: the inputs and guest memory reads of the compilation are recorded as its
        // shader source (the cache is translated again from them for another GPU).
        auto source = BeginShaderSource(sel, hw_stage, sw_stage, params, runtime_info, start, 0);
        Shader::GuestReadLog log{Shader::GuestReadLog::Mode::Record};
        std::vector<u32> spv;
        vk::ShaderModule module{};
        Shader::StageSpecialization spec{};
        {
            Shader::GuestReadLog::Scope scope{source ? &log : nullptr};
            module = CompileModule(new_program->info, runtime_info, params.code, 0, binding,
                                   source ? &spv : nullptr);
            spec = Shader::StageSpecialization(new_program->info, runtime_info, profile, start);
        }
        if (source) {
            FinishShaderSource(std::move(source), log, spv, spec, nullptr);
        }
        const auto perm_hash = HashCombine(params.hash, 0);

        RegisterShaderMeta(new_program->info, spec.fetch_shader_data, spec, perm_hash, 0);
        new_program->AddPermut(module, std::move(spec));
        new_program->info_template = std::make_unique<Shader::Info>(new_program->info);
        Program* program = new_program.get();
        {
            std::unique_lock lk{programs_mutex};
            program_cache.emplace(params.hash, std::move(new_program));
        }
        return std::make_tuple(&program->info, module, program->modules[0].spec.fetch_shader_data,
                               perm_hash);
    }

    auto& program = it_pgm.value();
    if (!program->info_template) {
        // Programs loaded by the pipeline cache warm-up get their template on first use.
        std::unique_lock lk{programs_mutex};
        program->info_template = std::make_unique<Shader::Info>(program->info);
    }
    auto& info = program->info;
    info.pgm_base = params.Base(); // Needs to be actualized for inline cbuffer address fixup
    info.user_data = params.user_data;
    info.RefreshFlatBuf();
    auto spec = Shader::StageSpecialization(info, runtime_info, profile, binding);

    size_t perm_idx = program->modules.size();
    u64 perm_hash = HashCombine(params.hash, perm_idx);

    vk::ShaderModule module{};

    // bbport: consecutive draws of a program almost always use the same permutation.
    auto it = program->last_used < program->modules.size() &&
                      program->modules[program->last_used].spec == spec
                  ? program->modules.begin() + program->last_used
                  : std::ranges::find(program->modules, spec, &Program::Module::spec);
    if (it != program->modules.end()) {
        program->last_used = std::distance(program->modules.begin(), it);
    }
    if (it == program->modules.end()) {
        auto source =
            BeginShaderSource(sel, hw_stage, sw_stage, params, runtime_info, binding, perm_idx);
        Shader::GuestReadLog log{Shader::GuestReadLog::Mode::Record};
        std::vector<u32> spv;
        {
            Shader::GuestReadLog::Scope scope{source ? &log : nullptr};
            if (source) {
                // bbport: the specialization is made again under the log, so that its guest
                // reads (resource tables, fetch shader) are part of the shader source.
                info.RefreshFlatBuf();
                spec = Shader::StageSpecialization(info, runtime_info, profile, binding);
            }
            auto new_info = Shader::Info(hw_stage, sw_stage, params);
            module = CompileModule(new_info, runtime_info, params.code, perm_idx, binding,
                                   source ? &spv : nullptr);
        }
        if (source) {
            FinishShaderSource(std::move(source), log, spv, spec, program->info_template.get());
        }

        RegisterShaderMeta(info, spec.fetch_shader_data, spec, perm_hash, perm_idx);
        std::unique_lock lk{programs_mutex};
        program->AddPermut(module, std::move(spec));
    } else {
        info.AddBindings(binding);
        module = it->module;
        perm_idx = std::distance(program->modules.begin(), it);
        perm_hash = HashCombine(params.hash, perm_idx);
    }
    return std::make_tuple(&program->info, module,
                           program->modules[perm_idx].spec.fetch_shader_data, perm_hash);
}

static bool ShaderCacheSelfTest() {
    static const bool enabled = [] {
        const char* value = std::getenv("BB_SHADER_CACHE_SELFTEST");
        return value && value[0] == '1';
    }();
    return enabled;
}

std::unique_ptr<ShaderSource> PipelineCache::BeginShaderSource(
    const PipelineSelection& sel, HwStage hw_stage, SwStage sw_stage,
    const Shader::ShaderParams& params, const Shader::RuntimeInfo& runtime_info,
    const Shader::Backend::Bindings& start, size_t perm_idx) {
    if (!ShaderCacheSelfTest() && !Storage::DataBase::Instance().IsOpened()) {
        return nullptr;
    }
    // Motion vertex shaders embed session-local buffer addresses: never preloaded or rebuilt.
    if (hw_stage == HwStage::Vertex && runtime_info.hw.vs.motion_vectors) {
        return nullptr;
    }
    auto source = std::make_unique<ShaderSource>();
    source->pgm_hash = params.hash;
    source->perm_idx = u32(perm_idx);
    source->hw_stage = hw_stage;
    source->sw_stage = sw_stage;
    source->host_flags = hw_stage == HwStage::Compute ? 0u : SourceHostFlags(*sel.regs, hw_stage);
    source->pgm_base = params.Base();
    std::ranges::copy(params.user_data, source->user_data.begin());
    source->start = start;
    source->runtime_info = runtime_info;
    if (hw_stage == HwStage::Geometry) {
        const auto vs_copy = runtime_info.hw.gs.vs_copy;
        source->vs_copy.assign(vs_copy.begin(), vs_copy.end());
        source->runtime_info.hw.gs.vs_copy = {};
    }
    source->code.assign(params.code.begin(), params.code.end());
    return source;
}

void PipelineCache::FinishShaderSource(std::unique_ptr<ShaderSource> source,
                                       Shader::GuestReadLog& log, const std::vector<u32>& spv,
                                       const Shader::StageSpecialization& spec,
                                       const Shader::Info* base_info) {
    if (log.inconsistent) {
        // Guest memory changed while the shader was compiled: no exact copy of the inputs.
        LOG_DEBUG(Render_Vulkan, "Shader source of {:#x} not recorded (inputs changed)",
                  source->pgm_hash);
        return;
    }
    source->ranges = std::move(log.ranges);
    source->flat_bufs = std::move(log.flat_bufs);
    auto blob = SerializeShaderSource(*source);

    if (ShaderCacheSelfTest()) {
        // Translate again from the serialized source, as the rebuild for another GPU does, and
        // compare: proves that the source holds every input of the translation.
        std::string failure;
        ShaderSource copy{};
        RebuiltShader out{};
        Shader::Info base{};
        if (base_info) {
            base = *base_info;
        }
        if (source->perm_idx != 0 && !base_info) {
            failure = "no program Info";
        } else if (!DeserializeShaderSource(std::vector<u8>(blob), copy)) {
            failure = "source blob does not read back";
        } else if (!RebuildShader(copy, profile, pools, base, out)) {
            failure = "a guest memory read is missing from the source";
        } else if (out.spv != spv) {
            failure = fmt::format("SPIR-V differs ({} / {} words)", out.spv.size(), spv.size());
        } else if (!(spec == out.spec)) {
            failure = "specialization differs";
        }
        ++selftest_runs;
        if (!failure.empty()) {
            ++selftest_failures;
            LOG_ERROR(Render_Vulkan, "Shader cache self-test: {} shader {:#x} permutation {}: {}",
                      source->hw_stage, source->pgm_hash, source->perm_idx, failure);
        }
        if (!failure.empty() || selftest_runs % 100 == 0) {
            LOG_INFO(Render_Vulkan, "Shader cache self-test: {} shaders checked, {} mismatches",
                     selftest_runs, selftest_failures);
        }
    }

    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderSource,
                                       ShaderSourceName(source->pgm_hash, source->perm_idx),
                                       std::move(blob));
}

std::optional<vk::ShaderModule> PipelineCache::ReplaceShader(vk::ShaderModule module,
                                                             std::span<const u32> spv_code) {
    std::optional<vk::ShaderModule> new_module{};
    for (const auto& [_, program] : program_cache) {
        for (auto& m : program->modules) {
            if (m.module == module) {
                const auto& d = instance.GetDevice();
                d.destroyShaderModule(m.module);
                m.module = CompileSPV(spv_code, d);
                new_module = m.module;
            }
        }
    }
    if (module_related_pipelines.contains(module)) {
        auto& pipeline_keys = module_related_pipelines[module];
        for (auto& key : pipeline_keys) {
            if (std::holds_alternative<GraphicsPipelineKey>(key)) {
                auto& graphics_key = std::get<GraphicsPipelineKey>(key);
                graphics_pipelines.erase(graphics_key);
            } else if (std::holds_alternative<ComputePipelineKey>(key)) {
                auto& compute_key = std::get<ComputePipelineKey>(key);
                compute_pipelines.erase(compute_key);
            }
        }
    }
    return new_module;
}

std::string PipelineCache::GetShaderName(Shader::HwStage stage, u64 hash,
                                         std::optional<size_t> perm) {
    if (perm) {
        return fmt::format("{}_{:#018x}_{}", stage, hash, *perm);
    }
    return fmt::format("{}_{:#018x}", stage, hash);
}

void PipelineCache::DumpShader(std::span<const u32> code, u64 hash, Shader::HwStage stage,
                               size_t perm_idx, std::string_view ext) {
    if (!EmulatorSettings.IsDumpShaders()) {
        return;
    }

    using namespace Common::FS;
    const auto dump_dir = GetUserPath(PathType::ShaderDir) / "dumps";
    if (!std::filesystem::exists(dump_dir)) {
        std::filesystem::create_directories(dump_dir);
    }
    const auto filename = fmt::format("{}.{}", GetShaderName(stage, hash, perm_idx), ext);
    const auto file = IOFile{dump_dir / filename, FileAccessMode::Create};
    file.WriteSpan(code);
}

std::optional<std::vector<u32>> PipelineCache::GetShaderPatch(u64 hash, Shader::HwStage stage,
                                                              size_t perm_idx,
                                                              std::string_view ext) {

    using namespace Common::FS;
    const auto patch_dir = GetUserPath(PathType::ShaderDir) / "patch";
    if (!std::filesystem::exists(patch_dir)) {
        std::filesystem::create_directories(patch_dir);
    }
    const auto filename = fmt::format("{}.{}", GetShaderName(stage, hash, perm_idx), ext);
    const auto filepath = patch_dir / filename;
    if (!std::filesystem::exists(filepath)) {
        return {};
    }
    const auto file = IOFile{patch_dir / filename, FileAccessMode::Read};
    std::vector<u32> code(file.GetSize() / sizeof(u32));
    file.Read(code);
    return code;
}
} // namespace Vulkan

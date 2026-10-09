// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <span>
#include <string>
#include <vector>
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/guest_read_log.h"
#include "shader_recompiler/params.h"
#include "shader_recompiler/runtime_info.h"
#include "shader_recompiler/specialization.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

void RegisterPipelineData(const ComputePipelineKey& key,
                          ComputePipeline::SerializationSupport& sdata);
void RegisterPipelineData(const GraphicsPipelineKey& key, u64 hash,
                          GraphicsPipeline::SerializationSupport& sdata);
void RegisterShaderMeta(const Shader::Info& info,
                        const std::optional<Shader::Gcn::FetchShaderData>& fetch_shader_data,
                        const Shader::StageSpecialization& spec, size_t perm_hash, size_t perm_idx);
void RegisterShaderBinary(std::vector<u32>&& spv, u64 pgm_hash, size_t perm_idx);

/// bbport: meta blob contents (RegisterShaderMeta).
std::vector<u8> SerializeShaderMeta(const Shader::Info& info, const Shader::StageSpecialization& spec,
                                    size_t perm_hash, size_t perm_idx);

/**
 * bbport: everything one CompileModule call (plus the StageSpecialization and meta built around
 * it in PipelineCache::GetProgram) reads, so that the shader can be translated again for another
 * GPU (Shader::Profile) without the game: the guest code, the user data, runtime info, start
 * bindings and the guest memory read through user data pointers (Shader::GuestReadLog). Holds no
 * host pointers: the cache folder can be copied to another PC.
 */
struct ShaderSource {
    /// Runtime info bits that depend on the GPU, kept as the register state they come from and
    /// applied again for the current GPU (ApplyHostRuntimeInfo).
    static constexpr u32 FlagClipSpaceMinusW = 1u << 0;      ///< VS: clip space [-w, w]
    static constexpr u32 FlagClipDistanceEmulation = 1u << 1; ///< FS: wants clip distance emulation

    u64 pgm_hash{};
    u32 perm_idx{};
    Shader::HwStage hw_stage{};
    Shader::SwStage sw_stage{};
    u32 host_flags{};
    VAddr pgm_base{};
    std::array<u32, Shader::ShaderParams::NumShaderUserData> user_data{};
    Shader::Backend::Bindings start{};
    Shader::RuntimeInfo runtime_info{}; ///< before translation; gs.vs_copy is in vs_copy
    std::vector<u32> code;
    std::vector<u32> vs_copy;
    std::vector<Shader::GuestReadLog::Range> ranges;
    std::vector<Shader::GuestReadLog::FlatBuf> flat_bufs;
};

/// Sets the GPU-dependent runtime info bits for `profile` (see ShaderSource::host_flags).
void ApplyHostRuntimeInfo(Shader::RuntimeInfo& runtime_info, u32 host_flags,
                          const Shader::Profile& profile);

std::string ShaderSourceName(u64 pgm_hash, size_t perm_idx);
std::vector<u8> SerializeShaderSource(const ShaderSource& source);
/// False for a damaged blob or one of another version or build layout.
bool DeserializeShaderSource(std::vector<u8>&& data, ShaderSource& source);

struct RebuiltShader {
    std::vector<u32> spv;
    Shader::StageSpecialization spec; ///< spec.info is cleared
    std::vector<u8> meta;
};

/// Translates a source again for `profile`, as GetProgram did in game. `base_info` is the Info of
/// permutation 0 of the program: written for perm_idx 0, read for the other permutations.
/// False when the source does not replay (a guest read missing from it).
bool RebuildShader(const ShaderSource& source, const Shader::Profile& profile,
                   Shader::Pools& pools, Shader::Info& base_info, RebuiltShader& out);

/// Progress of the shader cache rebuild, for an on-screen indicator (any thread may read it).
struct ShaderCacheRebuildProgress {
    std::atomic<u32> total{0};
    std::atomic<u32> done{0};
    std::atomic<bool> active{false};

    static ShaderCacheRebuildProgress& Instance() {
        static ShaderCacheRebuildProgress progress;
        return progress;
    }
};

/// Rebuilds the sources of one program (permutations ascending, 0 first) for `profile` and
/// queues their meta/SPIR-V writes. Thread-safe (one `pools` per thread); touches no
/// PipelineCache state, so it can run as a job on a worker pool. Counts progress.
u32 RebuildProgramSources(u64 pgm_hash, std::span<const u32> perms, const Shader::Profile& profile,
                          Shader::Pools& pools, u32& num_skipped);

} // namespace Vulkan

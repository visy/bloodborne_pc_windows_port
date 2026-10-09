// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <map>
#include <thread>
#include <array>
#include <unordered_set>
#include <xxhash.h>
#include "common/hash.h"
#include "common/serdes.h"
#include "core/emulator_settings.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/recompiler.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_pipeline_jobs.h"
#include "video_core/renderer_vulkan/vk_pipeline_serialization.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "bbport_compile_progress.h"

namespace Serialization {
/* You should increment versions below once corresponding serialization scheme is changed. */
static constexpr u32 ShaderBinaryVersion = 9u; // layer page-table pairs / guarded write-through stores
static constexpr u32 ShaderMetaVersion = 7u; // bbport: ImageResource::needs_native
static constexpr u32 PipelineKeyVersion = 6u; // bbport: discard fragment shader stored
/// Keys of version 5 are the same without the discard fragment shader: still loaded (caches
/// recorded before version 6 keep working), except pipelines that would need that shader.
static constexpr u32 LegacyPipelineKeyVersion = 5u;
/// bbport: ShaderSource blobs. Independent of the versions above: a translator change bumps
/// ShaderBinaryVersion and the cache is translated again from its sources. Bump this one only
/// when the source format or what it must capture changes (a new guest memory read).
static constexpr u32 SourceVersion = 1u;
} // namespace Serialization

namespace Vulkan {

void RegisterPipelineData(const ComputePipelineKey& key,
                          ComputePipeline::SerializationSupport& sdata) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar{};
    Serialization::Writer pldata{ar};

    pldata.Write(Serialization::PipelineKeyVersion);
    pldata.Write(u32{1}); // compute

    key.Serialize(ar);
    sdata.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::PipelineKey,
                                       fmt::format("c_{:#018x}", key.value), ar.TakeOff());
}

void RegisterPipelineData(const GraphicsPipelineKey& key, u64 hash,
                          GraphicsPipeline::SerializationSupport& sdata) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar{};
    Serialization::Writer pldata{ar};

    pldata.Write(Serialization::PipelineKeyVersion);
    pldata.Write(u32{0}); // graphics

    key.Serialize(ar);
    sdata.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::PipelineKey,
                                       fmt::format("g_{:#018x}", hash), ar.TakeOff());
}

void RegisterShaderMeta(const Shader::Info& info,
                        const std::optional<Shader::Gcn::FetchShaderData>& fetch_shader_data,
                        const Shader::StageSpecialization& spec, size_t perm_hash,
                        size_t perm_idx) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderMeta,
                                       fmt::format("{:#018x}", perm_hash),
                                       SerializeShaderMeta(info, spec, perm_hash, perm_idx));
}

std::vector<u8> SerializeShaderMeta(const Shader::Info& info, const Shader::StageSpecialization& spec,
                                    size_t perm_hash, size_t perm_idx) {
    Serialization::Archive ar;
    Serialization::Writer meta{ar};

    meta.Write(Serialization::ShaderMetaVersion);
    meta.Write(Serialization::ShaderBinaryVersion);

    meta.Write(perm_hash);
    meta.Write(perm_idx);

    spec.Serialize(ar);
    info.Serialize(ar);

    return ar.TakeOff();
}

void RegisterShaderBinary(std::vector<u32>&& spv, u64 pgm_hash, size_t perm_idx) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderBinary,
                                       fmt::format("{:#018x}_{}", pgm_hash, perm_idx),
                                       std::move(spv));
}

bool LoadShaderMeta(Serialization::Archive& ar, Shader::Info& info,
                    std::optional<Shader::Gcn::FetchShaderData>& fetch_shader_data,
                    Shader::StageSpecialization& spec, size_t& perm_idx) {
    Serialization::Reader meta{ar};

    u32 meta_version{};
    meta.Read(meta_version);
    if (meta_version != Serialization::ShaderMetaVersion) {
        return false;
    }

    u32 binary_version{};
    meta.Read(binary_version);
    if (binary_version != Serialization::ShaderBinaryVersion) {
        return false;
    }

    u64 perm_hash_ar{};
    meta.Read(perm_hash_ar);
    meta.Read(perm_idx);

    spec.Deserialize(ar);
    info.Deserialize(ar);

    // Motion vertex shaders embed session-local buffer device addresses. They must be
    // recompiled for the current allocation, never loaded from a previous process.
    if (info.hw_stage == Shader::HwStage::Vertex && spec.runtime_info.hw.vs.motion_vectors) {
        return false;
    }

    fetch_shader_data = spec.fetch_shader_data;
    return true;
}

void ComputePipelineKey::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer key{ar};
    key.Write(value);
}

bool ComputePipelineKey::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader key{ar};
    key.Read(value);
    return true;
}

void ComputePipeline::SerializationSupport::Serialize(Serialization::Archive& ar) const {
    // Nothing here yet
    return;
}

bool ComputePipeline::SerializationSupport::Deserialize(Serialization::Archive& ar) {
    // Nothing here yet
    return true;
}

bool PipelineCache::LoadComputePipeline(Serialization::Archive& ar) {
    compute_key.Deserialize(ar);

    ComputePipeline::SerializationSupport sdata{};
    sdata.Deserialize(ar);

    std::vector<u8> meta_blob;
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta,
                                       fmt::format("{:#018x}", compute_key.value), meta_blob);
    if (meta_blob.empty()) {
        return false;
    }

    Serialization::Archive meta_ar{std::move(meta_blob)};

    if (!LoadPipelineStage(meta_ar, 0)) {
        return false;
    }

    if (parallel_warmup) {
        // bbport: the startup precompile builds it on a worker, from a copy of the Info.
        auto job = std::make_shared<ComputeJob>();
        job->cache = this;
        job->key = compute_key;
        job->owned.emplace(*sel.infos[0]);
        job->bound_info = sel.infos[0];
        job->module = sel.modules[0];
        job->progress = true;
        warmup_jobs.push_back(std::move(job));
        sel.infos.fill(nullptr);
        sel.modules.fill(nullptr);
        return true;
    }

    // bbport: built before it is inserted: a pipeline the driver rejects while preloading throws
    // (Serialization::CorruptData) and leaves no empty entry behind.
    auto pipeline =
        std::make_unique<ComputePipeline>(instance, scheduler, desc_heap, profile, *pipeline_cache,
                                          compute_key, *sel.infos[0], sel.modules[0], sdata, true);
    const auto [it, is_new] = compute_pipelines.try_emplace(compute_key);
    ASSERT(is_new);
    it.value() = std::move(pipeline);

    sel.infos.fill(nullptr);
    sel.modules.fill(nullptr);

    return true;
}

void GraphicsPipelineKey::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer key{ar};

    key.Write(this, sizeof(*this));
}

bool GraphicsPipelineKey::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader key{ar};

    key.Read(this, sizeof(*this));
    return true;
}

void GraphicsPipeline::SerializationSupport::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer sdata{ar};

    sdata.Write(&vertex_attributes, sizeof(vertex_attributes));
    sdata.Write(&vertex_bindings, sizeof(vertex_bindings));
    sdata.Write(&divisors, sizeof(divisors));
    sdata.Write(multisampling);
    sdata.Write(tcs);
    sdata.Write(tes);
    sdata.Write(fragment);
}

namespace {
/// Set while WarmUp reads a version 5 key (no `fragment` field).
bool reading_legacy_key = false;
} // namespace

bool GraphicsPipeline::SerializationSupport::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader sdata{ar};

    sdata.Read(&vertex_attributes, sizeof(vertex_attributes));
    sdata.Read(&vertex_bindings, sizeof(vertex_bindings));
    sdata.Read(&divisors, sizeof(divisors));
    sdata.Read(multisampling);
    sdata.Read(tcs);
    sdata.Read(tes);
    if (!reading_legacy_key) {
        sdata.Read(fragment);
    }
    return true;
}

bool PipelineCache::LoadGraphicsPipeline(Serialization::Archive& ar) {
    sel.graphics_key.Deserialize(ar);

    GraphicsPipeline::SerializationSupport sdata{};
    sdata.Deserialize(ar);

    // bbport: without a fragment shader, the pipeline carries the clip distance discard shader
    // only on GPUs that need the emulation (and the vertex shader then exports the distances).
    // Whether it does was decided for the profile the key was made with: after a rebuild for
    // another profile such a pipeline is compiled again when the game uses it.
    if (preload_profile_changed && !sel.graphics_key.stage_hashes[u32(Shader::SwStage::Fragment)] &&
        (profile.needs_clip_distance_emulation || !sdata.fragment.empty())) {
        return false;
    }
    // A version 5 key never stored the discard shader: compile such a pipeline in game.
    if (reading_legacy_key && !sel.graphics_key.stage_hashes[u32(Shader::SwStage::Fragment)] &&
        profile.needs_clip_distance_emulation) {
        return false;
    }

    for (int stage_idx = 0; stage_idx < MaxShaderStages; ++stage_idx) {
        const auto& hash = sel.graphics_key.stage_hashes[stage_idx];
        if (!hash) {
            continue;
        }

        std::vector<u8> meta_blob;
        Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta,
                                           fmt::format("{:#018x}", hash), meta_blob);
        if (meta_blob.empty()) {
            return false;
        }

        Serialization::Archive meta_ar{std::move(meta_blob)};

        if (!LoadPipelineStage(meta_ar, stage_idx)) {
            return false;
        }
    }

    if (parallel_warmup) {
        // bbport: the startup precompile builds it on a worker, from copies of the Infos.
        auto job = MakeGraphicsJob(sdata, BuildMode::Preload);
        job->progress = true;
        job->optimize_inline = gpl != nullptr;
        warmup_jobs.push_back(std::move(job));
        sel.infos.fill(nullptr);
        sel.modules.fill(nullptr);
        sel.fetch_shader.reset();
        return true;
    }

    auto pipeline = std::make_unique<GraphicsPipeline>(
        instance, scheduler, desc_heap, profile, sel.graphics_key, *pipeline_cache, sel.infos,
        sel.runtime_infos, sel.fetch_shader, sel.modules, sdata, BuildMode::Preload, gpl.get());
    if (gpl && pipeline->IsFastLinked() && pipeline->LinkOptimized()) {
        ++gpl->num_optimized;
    }
    const auto [it, is_new] = graphics_pipelines.try_emplace(sel.graphics_key);
    ASSERT(is_new);
    it.value() = std::move(pipeline);

    sel.infos.fill(nullptr);
    sel.modules.fill(nullptr);
    sel.fetch_shader.reset();

    return true;
}

bool PipelineCache::LoadPipelineStage(Serialization::Archive& ar, size_t stage) {
    auto program = std::make_unique<Program>();
    Shader::StageSpecialization spec{};
    spec.info = &program->info;
    size_t perm_idx{};
    if (!LoadShaderMeta(ar, program->info, sel.fetch_shader, spec, perm_idx)) {
        return false;
    }

    std::vector<u32> spv{};
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderBinary,
                                       fmt::format("{:#018x}_{}", program->info.pgm_hash, perm_idx),
                                       spv);
    // bbport: a SPIR-V binary starts with its magic number and a 5-word header; anything else is
    // a damaged file (crash or power loss while it was written) the driver must not see.
    if (spv.size() < 5 || spv[0] != 0x07230203u) {
        if (!spv.empty()) {
            throw Serialization::CorruptData{"damaged SPIR-V in the shader cache"};
        }
        return false;
    }

    // Permutation hash depends on shader variation index. To prevent collisions, we need insert it
    // at the exact position rather than append

    vk::ShaderModule module{};
    // bbport: a module the driver rejects is a damaged entry (WarmUp rebuilds the cache), not a
    // fatal error as in CompileSPV.
    const auto compile = [&] {
        auto [result, created] = instance.GetDevice().createShaderModule(
            {.codeSize = spv.size() * sizeof(u32), .pCode = spv.data()});
        if (result != vk::Result::eSuccess) {
            throw Serialization::CorruptData{"cached SPIR-V rejected by the driver"};
        }
        return created;
    };

    auto [it_pgm, new_program] = program_cache.try_emplace(program->info.pgm_hash);
    if (new_program) {
        module = compile();
        it_pgm.value() = std::move(program);
    } else {
        const auto& it = std::ranges::find(it_pgm.value()->modules, spec, &Program::Module::spec);
        if (it != it_pgm.value()->modules.end()) {
            // A matching permutation is valid only at its original index. A different index means
            // the store holds entries from more than one cache generation, so this pipeline is
            // left to compile at runtime.
            const auto idx = std::distance(it_pgm.value()->modules.begin(), it);
            if (perm_idx != idx) {
                LOG_WARNING(Render_Vulkan,
                            "Cached permutation {} of {}_{:x} conflicts with index {}, skipping "
                            "preload",
                            perm_idx, program->info.hw_stage, program->info.pgm_hash, idx);
                return false;
            }
            module = it->module;
        } else {
            module = compile();
        }
    }
    it_pgm.value()->InsertPermut(module, std::move(spec), perm_idx);

    sel.infos[stage] = &it_pgm.value()->info;
    sel.modules[stage] = module;

    return true;
}

// ---- bbport: shader sources and the cache rebuild ------------------------------------------

namespace {

constexpr u32 SourceMagic = 0x53534242u; // "BBSS"

/// Layout of the raw structures in a source: a build whose layout differs (another compiler or
/// OS) skips the source instead of misreading it.
constexpr u32 SourceLayout() {
    return u32(sizeof(Shader::RuntimeInfo)) | (u32(sizeof(Shader::Backend::Bindings)) << 16) |
#ifdef _WIN32
           (1u << 31);
#else
           0u;
#endif
}

struct SourceWriter {
    std::vector<u8> out;

    void Raw(const void* data, size_t size) {
        const auto* bytes = static_cast<const u8*>(data);
        out.insert(out.end(), bytes, bytes + size);
    }
    template <typename T>
    void Value(const T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        Raw(&value, sizeof(value));
    }
    template <typename T>
    void Vector(const std::vector<T>& v) {
        Value(u64(v.size()));
        Raw(v.data(), v.size() * sizeof(T));
    }
};

struct SourceReader {
    const u8* data;
    size_t size;
    size_t offset = 0;
    bool ok = true;

    void Raw(void* dst, size_t count) {
        if (!ok || count > size - offset) {
            ok = false;
            return;
        }
        std::memcpy(dst, data + offset, count);
        offset += count;
    }
    template <typename T>
    void Value(T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        Raw(&value, sizeof(value));
    }
    template <typename T>
    void Vector(std::vector<T>& v) {
        u64 count{};
        Value(count);
        if (!ok || count > (size - offset) / sizeof(T)) {
            ok = false;
            return;
        }
        v.resize(count);
        Raw(v.data(), count * sizeof(T));
    }
};

std::string MetaName(u64 pgm_hash, size_t perm_idx) {
    return fmt::format("{:#018x}", HashCombine(pgm_hash, u64(perm_idx)));
}

std::string BinaryName(u64 pgm_hash, size_t perm_idx) {
    return fmt::format("{:#018x}_{}", pgm_hash, perm_idx);
}

bool ParseSourceName(const std::string& name, u64& pgm_hash, u32& perm_idx) {
    const auto sep = name.find('_');
    if (sep == std::string::npos || !name.starts_with("0x")) {
        return false;
    }
    char* end{};
    pgm_hash = std::strtoull(name.c_str() + 2, &end, 16);
    if (end != name.c_str() + sep) {
        return false;
    }
    const auto idx = std::strtoul(name.c_str() + sep + 1, &end, 10);
    if (*end != '\0' || idx >= 4096) {
        return false;
    }
    perm_idx = u32(idx);
    return true;
}

} // namespace

std::string ShaderSourceName(u64 pgm_hash, size_t perm_idx) {
    return BinaryName(pgm_hash, perm_idx);
}

void ApplyHostRuntimeInfo(Shader::RuntimeInfo& runtime_info, u32 host_flags,
                          const Shader::Profile& profile) {
    if (runtime_info.hw_stage == Shader::HwStage::Vertex) {
        runtime_info.hw.vs.emulate_depth_negative_one_to_one =
            (host_flags & ShaderSource::FlagClipSpaceMinusW) && !profile.supports_depth_clip_control;
    } else if (runtime_info.hw_stage == Shader::HwStage::Fragment) {
        runtime_info.hw.fs.clip_distance_emulation =
            (host_flags & ShaderSource::FlagClipDistanceEmulation) &&
            profile.needs_clip_distance_emulation;
    }
}

std::vector<u8> SerializeShaderSource(const ShaderSource& source) {
    SourceWriter payload;
    payload.Value(source.pgm_hash);
    payload.Value(source.perm_idx);
    payload.Value(u32(source.hw_stage));
    payload.Value(u32(source.sw_stage));
    payload.Value(source.host_flags);
    payload.Value(u64(source.pgm_base));
    payload.Value(source.user_data);
    payload.Value(source.start);
    auto runtime_info = source.runtime_info;
    if (runtime_info.hw_stage == Shader::HwStage::Geometry) {
        runtime_info.hw.gs.vs_copy = {};
    }
    payload.Value(runtime_info);
    payload.Vector(source.code);
    payload.Vector(source.vs_copy);
    payload.Value(u64(source.ranges.size()));
    for (const auto& range : source.ranges) {
        payload.Value(u64(range.addr));
        payload.Vector(range.bytes);
    }
    payload.Value(u64(source.flat_bufs.size()));
    for (const auto& flat : source.flat_bufs) {
        payload.Vector(flat.walker);
        payload.Vector(flat.data);
    }

    SourceWriter blob;
    blob.Value(SourceMagic);
    blob.Value(Serialization::SourceVersion);
    blob.Value(SourceLayout());
    blob.Value(u32{0});
    blob.Value(u64(payload.out.size()));
    blob.Value(u64(XXH3_64bits(payload.out.data(), payload.out.size())));
    blob.Raw(payload.out.data(), payload.out.size());
    return std::move(blob.out);
}

bool DeserializeShaderSource(std::vector<u8>&& data, ShaderSource& source) {
    SourceReader header{data.data(), data.size()};
    u32 magic{}, version{}, layout{}, reserved{};
    u64 payload_size{}, checksum{};
    header.Value(magic);
    header.Value(version);
    header.Value(layout);
    header.Value(reserved);
    header.Value(payload_size);
    header.Value(checksum);
    if (!header.ok || magic != SourceMagic || version != Serialization::SourceVersion ||
        layout != SourceLayout() || payload_size != data.size() - header.offset) {
        return false;
    }
    const u8* payload = data.data() + header.offset;
    if (XXH3_64bits(payload, payload_size) != checksum) {
        return false;
    }

    SourceReader in{payload, payload_size};
    u32 hw_stage{}, sw_stage{};
    u64 pgm_base{};
    in.Value(source.pgm_hash);
    in.Value(source.perm_idx);
    in.Value(hw_stage);
    in.Value(sw_stage);
    in.Value(source.host_flags);
    in.Value(pgm_base);
    in.Value(source.user_data);
    in.Value(source.start);
    in.Value(source.runtime_info);
    in.Vector(source.code);
    in.Vector(source.vs_copy);
    u64 num_ranges{};
    in.Value(num_ranges);
    if (!in.ok || num_ranges > payload_size) {
        return false;
    }
    source.ranges.resize(num_ranges);
    for (auto& range : source.ranges) {
        u64 addr{};
        in.Value(addr);
        range.addr = VAddr(addr);
        in.Vector(range.bytes);
    }
    u64 num_flat{};
    in.Value(num_flat);
    if (!in.ok || num_flat > payload_size) {
        return false;
    }
    source.flat_bufs.resize(num_flat);
    for (auto& flat : source.flat_bufs) {
        in.Vector(flat.walker);
        in.Vector(flat.data);
    }
    source.hw_stage = Shader::HwStage(hw_stage);
    source.sw_stage = Shader::SwStage(sw_stage);
    source.pgm_base = VAddr(pgm_base);
    return in.ok && in.offset == payload_size && !source.code.empty() &&
           hw_stage <= u32(Shader::HwStage::Compute) && sw_stage <= u32(Shader::SwStage::Compute) &&
           source.runtime_info.hw_stage == source.hw_stage &&
           source.runtime_info.sw_stage == source.sw_stage;
}

bool RebuildShader(const ShaderSource& source, const Shader::Profile& profile,
                   Shader::Pools& pools, Shader::Info& base_info, RebuiltShader& out) {
    Shader::GuestReadLog log{Shader::GuestReadLog::Mode::Replay};
    log.ranges = source.ranges;
    log.flat_bufs = source.flat_bufs;

    const Shader::ShaderParams params{
        .user_data = std::span<const u32, Shader::ShaderParams::NumShaderUserData>{
            source.user_data},
        .code = source.code,
        .hash = source.pgm_hash,
    };
    auto runtime_info = source.runtime_info;
    ApplyHostRuntimeInfo(runtime_info, source.host_flags, profile);
    if (runtime_info.hw_stage == Shader::HwStage::Geometry) {
        runtime_info.hw.gs.vs_copy = source.vs_copy;
    }
    const std::span<const u32> code{source.code};

    Shader::GuestReadLog::Scope scope{&log};
    if (source.perm_idx == 0) {
        // GetProgram, new program: translate, then specialize on the translated Info.
        base_info = Shader::Info(source.hw_stage, source.sw_stage, params);
        base_info.pgm_base = source.pgm_base;
        auto binding = source.start;
        const auto program =
            Shader::TranslateProgram(code, pools, base_info, runtime_info, profile);
        out.spv = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, program, binding);
        out.spec = Shader::StageSpecialization(base_info, runtime_info, profile, source.start);
        if (log.failed) {
            return false;
        }
        out.meta = SerializeShaderMeta(base_info, out.spec, HashCombine(source.pgm_hash, u64{0}), 0);
    } else {
        // GetProgram, new permutation: specialize on the program's Info with this draw's user
        // data, then translate a fresh Info.
        if (base_info.pgm_hash != source.pgm_hash || base_info.hw_stage != source.hw_stage) {
            return false;
        }
        Shader::Info info = base_info;
        info.pgm_base = source.pgm_base;
        info.user_data = params.user_data;
        info.RefreshFlatBuf();
        out.spec = Shader::StageSpecialization(info, runtime_info, profile, source.start);

        Shader::Info new_info(source.hw_stage, source.sw_stage, params);
        new_info.pgm_base = source.pgm_base;
        auto binding = source.start;
        const auto program = Shader::TranslateProgram(code, pools, new_info, runtime_info, profile);
        out.spv = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, program, binding);
        if (log.failed) {
            return false;
        }
        out.meta = SerializeShaderMeta(info, out.spec, HashCombine(source.pgm_hash, u64(source.perm_idx)),
                                       source.perm_idx);
    }
    out.spec.info = nullptr;
    return true;
}

u32 RebuildProgramSources(u64 pgm_hash, std::span<const u32> perms, const Shader::Profile& profile,
                          Shader::Pools& pools, u32& num_skipped) {
    auto& db = Storage::DataBase::Instance();
    auto& progress = ShaderCacheRebuildProgress::Instance();
    Shader::Info base_info{};
    bool have_base = false;
    u32 num_built = 0;
    for (const u32 perm_idx : perms) {
        const auto name = ShaderSourceName(pgm_hash, perm_idx);
        std::vector<u8> blob;
        db.Load(Storage::BlobType::ShaderSource, name, blob);
        ShaderSource source{};
        RebuiltShader out{};
        const bool ok = (perm_idx == 0 || have_base) &&
                        DeserializeShaderSource(std::move(blob), source) &&
                        source.pgm_hash == pgm_hash && source.perm_idx == perm_idx &&
                        RebuildShader(source, profile, pools, base_info, out);
        if (!ok) {
            // Useless now (damaged, another version, no permutation 0, or it does not replay):
            // the shader is compiled again when the game uses it, which writes a new source.
            db.Remove(Storage::BlobType::ShaderSource, name);
            ++num_skipped;
        } else {
            have_base |= perm_idx == 0;
            db.Save(Storage::BlobType::ShaderMeta, MetaName(pgm_hash, perm_idx),
                    std::move(out.meta));
            db.Save(Storage::BlobType::ShaderBinary, BinaryName(pgm_hash, perm_idx),
                    std::move(out.spv));
            ++num_built;
        }
        const u32 done = ++progress.done;
        BbCompileProgress::Done(ok);
        if (done % 500 == 0) {
            LOG_INFO(Render, "Shader cache: {}/{} shaders", done, progress.total.load());
        }
    }
    return num_built;
}

u32 PipelineCache::RebuildShaderCache(bool full) {
    auto& db = Storage::DataBase::Instance();
    if (!db.SupportsFiles()) {
        return 0;
    }

    // Sources by program, permutations in order (permutation 0 first: the others need its Info).
    std::map<u64, std::vector<u32>> programs;
    for (const auto& name : db.ListNames(Storage::BlobType::ShaderSource)) {
        u64 pgm_hash{};
        u32 perm_idx{};
        if (ParseSourceName(name, pgm_hash, perm_idx)) {
            programs[pgm_hash].push_back(perm_idx);
        } else {
            db.Remove(Storage::BlobType::ShaderSource, name);
        }
    }

    if (full) {
        // Meta and SPIR-V were made for another profile; keys and sources are GPU-independent.
        db.Clear({Storage::BlobType::PipelineKey, Storage::BlobType::ShaderSource,
                  Storage::BlobType::ShaderProfile});
    } else {
        // Only programs with a permutation whose meta or SPIR-V is missing.
        std::erase_if(programs, [&](const auto& program) {
            return std::ranges::all_of(program.second, [&](u32 perm_idx) {
                return db.Exists(Storage::BlobType::ShaderMeta, MetaName(program.first, perm_idx)) &&
                       db.Exists(Storage::BlobType::ShaderBinary,
                                 BinaryName(program.first, perm_idx));
            });
        });
    }

    u32 num_sources = 0;
    std::vector<std::pair<u64, std::vector<u32>>> work;
    work.reserve(programs.size());
    for (auto& [pgm_hash, perms] : programs) {
        std::ranges::sort(perms);
        num_sources += u32(perms.size());
        work.emplace_back(pgm_hash, std::move(perms));
    }
    if (work.empty()) {
        return 0;
    }

    u32 num_threads = std::max(1u, std::thread::hardware_concurrency());
    num_threads = num_threads > 2 ? num_threads - 1 : num_threads;
    if (const char* env = std::getenv("BB_SHADER_CACHE_REBUILD_THREADS")) {
        num_threads = std::max(1, std::atoi(env));
    }
    num_threads = std::min<u32>(num_threads, u32(work.size()));

    LOG_INFO(Render, "Shader cache: rebuilding {} shaders ({} programs) for this GPU on {} threads...",
             num_sources, work.size(), num_threads);
    const auto start_time = std::chrono::steady_clock::now();

    auto& progress = ShaderCacheRebuildProgress::Instance();
    progress.total = num_sources;
    progress.done = 0;
    progress.active = true;
    BbCompileProgress::BeginBatch(BbCompileProgress::Phase::Rebuild, num_sources);
    BbCompileProgress::first_launch = true;
    std::atomic<size_t> next{0};
    std::atomic<u32> num_built{0}, num_skipped{0};
    const auto worker = [&] {
        Shader::Pools pools;
        for (size_t i = next++; i < work.size() && !warmup_abort.load(); i = next++) {
            u32 skipped = 0;
            num_built += RebuildProgramSources(work[i].first, work[i].second, profile, pools,
                                               skipped);
            num_skipped += skipped;
        }
    };
    {
        std::vector<std::jthread> threads;
        for (u32 i = 1; i < num_threads; ++i) {
            threads.emplace_back(worker);
        }
        worker();
    }
    db.Flush();
    progress.active = false;

    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                       start_time)
                             .count();
    LOG_INFO(Render, "Shader cache: rebuilt {} of {} shaders in {:.1f} s ({} skipped)",
             num_built.load(), num_sources, seconds, num_skipped.load());
    return num_built.load();
}

namespace {
/// bbport: stored after the Shader::Profile in profile.bin: meta or SPIR-V of another version
/// is translated again from the sources like a cache of another GPU.
struct CacheVersions {
    u32 meta = Serialization::ShaderMetaVersion;
    u32 binary = Serialization::ShaderBinaryVersion;
    bool operator==(const CacheVersions&) const = default;
};

bool EnvFlag(const char* name) {
    const char* value = std::getenv(name);
    return value && value[0] == '1';
}
} // namespace

void PipelineCache::AbortWarmUp() {
    warmup_abort = true;
    compiler.Cancel();
}

void PipelineCache::WarmUp(bool parallel) {
    if (!EmulatorSettings.IsPipelineCacheEnabled()) {
        return;
    }
    const auto start_time = std::chrono::steady_clock::now();
    parallel_warmup = parallel;
    struct ParallelReset {
        PipelineCache& cache;
        ~ParallelReset() {
            cache.parallel_warmup = false;
            cache.warmup_jobs.clear();
        }
    } parallel_reset{*this};

    auto& db = Storage::DataBase::Instance();
    db.Open();

    const auto save_profile = [&] {
        std::vector<u8> data(sizeof(Shader::Profile) + sizeof(CacheVersions));
        const CacheVersions versions{};
        std::memcpy(data.data(), &profile, sizeof(profile));
        std::memcpy(data.data() + sizeof(profile), &versions, sizeof(versions));
        db.Save(Storage::BlobType::ShaderProfile, "profile", std::move(data));
    };

    // Shader metadata and SPIR-V filenames share permutation indices. After a backend version
    // change, retaining old blobs alongside new ones can associate a valid metadata entry with
    // a different binary at the same index (upstream 0.5-pre2). The versions are checked together
    // with the device profile before any module is loaded; a mismatch migrates the whole cache:
    // with files, meta and SPIR-V are translated again from the GPU-independent sources (keys
    // and sources kept); an archived cache cannot be rebuilt and is cleared.
    std::vector<u8> profile_data{};
    db.Load(Storage::BlobType::ShaderProfile, "profile", profile_data);
    bool compatible = false;
    if (profile_data.size() == sizeof(Shader::Profile) + sizeof(CacheVersions)) {
        Shader::Profile cached_profile{};
        CacheVersions cached_versions{};
        std::memcpy(&cached_profile, profile_data.data(), sizeof(cached_profile));
        std::memcpy(&cached_versions, profile_data.data() + sizeof(cached_profile),
                    sizeof(cached_versions));
        compatible = cached_profile == profile && cached_versions == CacheVersions{};
    } else if (constexpr size_t known = offsetof(Shader::Profile, supports_depth_clip_control);
               profile_data.size() >= known && profile_data.size() <= sizeof(Shader::Profile)) {
        // Caches written before the versions were appended (and before
        // supports_depth_clip_control): the bare Profile of that build, whose SPIR-V is binary
        // version 7 (before upstream's version 9, layer page-table pairs). Recognized, but its
        // meta and SPIR-V are migrated: rebuilt from the sources (keys and sources are kept).
        LOG_INFO(Render, "Pipeline cache: header of an earlier build{}: shaders are rebuilt",
                 std::memcmp(profile_data.data(), &profile, known) == 0 ? ", same GPU" : "");
        compatible = false;
    }

    preload_profile_changed = false;
    if (!db.SupportsFiles()) {
        // Archived cache: no rebuild. As upstream, blobs of unknown or other versions are
        // cleared rather than mixed with new ones.
        if (profile_data.empty()) {
            db.Clear();
            db.FinishPreload();
            save_profile();
            return;
        }
        if (!compatible) {
            LOG_WARNING(Render, "Pipeline cache isn't compatible with current compiler/system: "
                                "clearing it");
            db.Clear();
            db.FinishPreload();
            save_profile();
            return;
        }
    } else if (!compatible || EnvFlag("BB_SHADER_CACHE_REBUILD")) {
        // bbport: a cache made on another GPU (or by another build) keeps its pipeline keys and
        // shader sources; its shaders are translated again for this GPU. Upstream closed the
        // cache, later builds cleared it.
        if (!profile_data.empty()) {
            LOG_WARNING(Render, "Pipeline cache was made for another GPU or build{}: rebuilding it",
                        compatible ? " (BB_SHADER_CACHE_REBUILD)" : "");
        }
        RebuildShaderCache(true);
        save_profile();
        preload_profile_changed = !compatible;
    } else {
        // Sources whose meta or SPIR-V is missing (for instance a cache shared without them).
        RebuildShaderCache(false);
    }

    u32 num_pipelines{};
    u32 num_total_pipelines{};
    u32 num_damaged{};
    std::vector<std::string> stale_keys;
    if (warmup_abort) {
        return;
    }
    if (parallel) {
        // Progress: each key read, then each pipeline built (the total is corrected below).
        const u32 num_keys = u32(db.ListNames(Storage::BlobType::PipelineKey).size());
        BbCompileProgress::BeginBatch(BbCompileProgress::Phase::Startup, num_keys * 2);
    }

    db.ForEachNamedBlob(
        Storage::BlobType::PipelineKey, [&](const std::string& name, std::vector<u8>&& data) {
            if (warmup_abort) {
                return;
            }
            if (parallel) {
                BbCompileProgress::Done(true);
            }
            ++num_total_pipelines;
            // bbport: a damaged entry (cut short by a crash or a power loss, or rejected by the
            // driver) used to stop the game at every start until the cache was deleted by hand
            // (issues #28, #38, #39). It is counted here and the cache is rebuilt below.
            try {
                Serialization::Archive ar{std::move(data)};
                Serialization::Reader pldata{ar};

                u32 version{};
                pldata.Read(version);
                if (version != Serialization::PipelineKeyVersion &&
                    version != Serialization::LegacyPipelineKeyVersion) {
                    stale_keys.push_back(name);
                    return;
                }
                reading_legacy_key = version == Serialization::LegacyPipelineKeyVersion;

                u32 is_compute{};
                pldata.Read(is_compute);

                bool result{};
                if (is_compute) {
                    result = LoadComputePipeline(ar);
                } else {
                    result = LoadGraphicsPipeline(ar);
                }

                if (result) {
                    ++num_pipelines;
                } else {
                    stale_keys.push_back(name);
                    sel.infos.fill(nullptr);
                    sel.modules.fill(nullptr);
                    sel.fetch_shader.reset();
                }
            } catch (const std::exception& e) {
                if (num_damaged++ == 0) {
                    LOG_WARNING(Render, "Pipeline cache: damaged entry ({})", e.what());
                }
                sel.infos.fill(nullptr);
                sel.modules.fill(nullptr);
                sel.fetch_shader.reset();
            }
        });
    preload_profile_changed = false;
    reading_legacy_key = false;

    // bbport: BB_GPL_STATS=1: how much pipeline libraries could share (keys per shader tuple).
    if (EnvFlag("BB_GPL_STATS")) {
        std::unordered_set<u64> pre_raster, fragment, stages;
        u32 count = 0;
        const auto add = [&](const GraphicsPipelineKey& key) {
            ++count;
            const auto& h = key.stage_hashes;
            pre_raster.insert(XXH3_64bits(&h[1], sizeof(h[0]) * (MaxShaderStages - 2)));
            fragment.insert(h[0]);
            stages.insert(XXH3_64bits(h.data(), sizeof(h[0]) * (MaxShaderStages - 1)));
        };
        for (const auto& job : warmup_jobs) {
            if (const auto* graphics = dynamic_cast<const GraphicsJob*>(job.get())) {
                add(graphics->key);
            }
        }
        for (const auto& [key, _] : graphics_pipelines) {
            add(key);
        }
        std::printf("GPL stats: %u graphics pipelines; %zu pre-rasterization shader tuples, %zu "
                    "fragment shaders, %zu full shader tuples (%.1f pipelines per tuple)\n",
                    count, pre_raster.size(), fragment.size(), stages.size(),
                    stages.empty() ? 0.0 : double(count) / double(stages.size()));
    }

    // bbport: the startup precompile: the pipelines the keys describe, built on all cores.
    if (parallel && !warmup_jobs.empty() && num_damaged == 0 && !warmup_abort) {
        BbCompileProgress::total = BbCompileProgress::done.load() + u32(warmup_jobs.size());
        const auto compile_start = std::chrono::steady_clock::now();
        compiler.Start(PipelineCompiler::StartupWorkers(), false);
        for (const auto& job : warmup_jobs) {
            compiler.Enqueue(job, PipelineCompiler::PriorityWarmUp);
        }
        compiler.WaitIdle();
        u32 failed = 0;
        for (const auto& job : warmup_jobs) {
            if (auto* graphics = dynamic_cast<GraphicsJob*>(job.get())) {
                if (!graphics->IsDone() || graphics->failed || !graphics->result) {
                    ++failed;
                    continue;
                }
                graphics->published = true;
                graphics->result->RebindStages(graphics->bound_infos);
                const auto [it, is_new] = graphics_pipelines.try_emplace(graphics->key);
                if (is_new) {
                    it.value() = std::move(graphics->result);
                }
            } else if (auto* compute = dynamic_cast<ComputeJob*>(job.get())) {
                if (!compute->IsDone() || compute->failed || !compute->result) {
                    ++failed;
                    continue;
                }
                compute->published = true;
                compute->result->RebindStage(compute->bound_info);
                const auto [it, is_new] = compute_pipelines.try_emplace(compute->key);
                if (is_new) {
                    it.value() = std::move(compute->result);
                }
            }
        }
        (void)compiler.DrainCompleted();
        // No worker stays: gameplay starts them again only for async pipelines or library links.
        compiler.StopWorkers();
        if (warmup_abort) {
            return;
        }
        LOG_INFO(Render, "Shader precompile: {} pipelines on {} threads in {:.1f} s",
                 warmup_jobs.size() - failed, PipelineCompiler::StartupWorkers(),
                 std::chrono::duration<double>(std::chrono::steady_clock::now() - compile_start)
                     .count());
        // A pipeline the driver rejects is a damaged entry, as in the sequential warm-up.
        num_damaged += failed;
    }
    warmup_jobs.clear();
    if (warmup_abort) {
        return;
    }

    if (num_damaged) {
        // Nothing preloaded is trusted: modules of a damaged entry may sit in programs that later
        // lookups would reuse. Start as with no cache and write a fresh one (the shader sources
        // are checked on their own and kept).
        LOG_WARNING(Render, "Pipeline cache: {} damaged entries, rebuilding it", num_damaged);
        graphics_pipelines.clear();
        compute_pipelines.clear();
        std::unordered_set<VkShaderModule> modules;
        for (const auto& [_, program] : program_cache) {
            if (!program) {
                continue;
            }
            for (const auto& permutation : program->modules) {
                if (permutation.module) {
                    modules.insert(VkShaderModule(permutation.module));
                }
            }
        }
        for (const VkShaderModule module : modules) {
            instance.GetDevice().destroyShaderModule(vk::ShaderModule{module});
        }
        program_cache.clear();
        db.Clear({Storage::BlobType::ShaderSource});
        db.FinishPreload();
        save_profile();
        return;
    }

    LOG_INFO(Render, "Preloaded {} pipelines", num_pipelines);
    if (num_total_pipelines > num_pipelines) {
        // bbport: only the entries that did not load are removed (a stage of another cache
        // version, a shader that could not be rebuilt for this GPU, ...); upstream cleared the
        // whole cache, and with it the shader sources.
        LOG_WARNING(Render, "{} stale pipelines were found: removing them",
                    num_total_pipelines - num_pipelines);
        if (db.SupportsFiles()) {
            for (const auto& name : stale_keys) {
                db.Remove(Storage::BlobType::PipelineKey, name);
            }
        } else {
            db.Clear();
        }
    }

    db.FinishPreload();
    std::printf("Pipeline cache: %u of %u pipelines preloaded in %.1f s%s\n", num_pipelines,
                num_total_pipelines,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count(),
                parallel ? " (parallel precompile)" : "");
}

void PipelineCache::Sync() {
    // bbport: no job may write to the storage after it is closed (results are published on
    // the GPU thread; queued jobs are dropped, running ones finish first).
    compiler.Stop();
    Storage::DataBase::Instance().Close();
}

} // namespace Vulkan

namespace Shader {

void Info::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer info{ar};

    info.Write(this, sizeof(InfoPersistent));
    info.Write(flattened_ud_buf);
    srt_info.Serialize(ar);
}

bool Info::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader info{ar};

    info.Read(this, sizeof(Shader::InfoPersistent));
    info.Read(flattened_ud_buf);

    return srt_info.Deserialize(ar);
}

void Gcn::FetchShaderData::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer fetch{ar};
    ar.Grow(6 + attributes.size() * sizeof(VertexAttribute));

    fetch.Write(size);
    fetch.Write(vertex_offset_sgpr);
    fetch.Write(instance_offset_sgpr);
    fetch.Write(attributes);
}

bool Gcn::FetchShaderData::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader fetch{ar};

    fetch.Read(size);
    fetch.Read(vertex_offset_sgpr);
    fetch.Read(instance_offset_sgpr);
    fetch.Read(attributes);

    return true;
}

void PersistentSrtInfo::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer srt{ar};

    // bbport: without the host address of the walker (registered again when loaded), so the
    // cache holds no host pointers.
    auto persistent = *this;
    persistent.walker_func = nullptr;
    srt.Write(&persistent, sizeof(persistent));
    if (walker_func_size) {
        srt.Write(reinterpret_cast<void*>(walker_func), walker_func_size);
    }
}

bool PersistentSrtInfo::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader srt{ar};

    srt.Read(this, sizeof(*this));

    if (walker_func_size) {
        // bbport: the size is checked before the code is registered (it becomes executable):
        // a cut-short file must not have bytes past its end run as the walker.
        const auto code = ar.CurrPtr();
        ar.Advance(walker_func_size);
        walker_func = RegisterWalkerCode(code, walker_func_size);
    }

    return true;
}

void StageSpecialization::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer spec{ar};

    spec.Write(start);
    // bbport: without the host address of the copy shader code (only its hash is compared).
    auto stored_runtime_info = runtime_info;
    if (stored_runtime_info.hw_stage == HwStage::Geometry) {
        stored_runtime_info.hw.gs.vs_copy = {};
    }
    spec.Write(stored_runtime_info);

    spec.Write(bitset.to_string());

    if (fetch_shader_data) {
        spec.Write(sizeof(*fetch_shader_data));
        fetch_shader_data->Serialize(ar);
    } else {
        spec.Write(size_t{0});
    }

    spec.Write(vs_attribs);
    spec.Write(buffers);
    spec.Write(images);
    spec.Write(fmasks);
    spec.Write(samplers);
}

bool StageSpecialization::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader spec{ar};

    spec.Read(start);
    spec.Read(runtime_info);

    std::string bits{};
    spec.Read(bits);
    bitset = std::bitset<MaxStageResources>(bits);

    u64 fetch_data_size{};
    spec.Read(fetch_data_size);

    if (fetch_data_size) {
        Gcn::FetchShaderData fetch_data;
        fetch_data.Deserialize(ar);
        fetch_shader_data = fetch_data;
    }

    spec.Read(vs_attribs);
    spec.Read(buffers);
    spec.Read(images);
    spec.Read(fmasks);
    spec.Read(samplers);

    return true;
}

} // namespace Shader

// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the shader cache's profile.bin, the gate in front of every cached shader. It holds the
// GPU's Shader::Profile and the versions the meta and SPIR-V were written with; a cache that does
// not match is translated again from its GPU-independent sources (PipelineCache::WarmUp) instead
// of loading meta and SPIR-V of another GPU or another build.
//
// Layouts, newest first (each build reads all of them):
//   Profile + Versions {meta, binary, translator}  since 0.7.2: `translator` is a hash of the
//       shader translator's sources (gpu/cmake/shader_translator_id.cmake), so a translator change
//       retranslates the cache even when nobody bumped ShaderBinaryVersion (upstream 80b631ef:
//       two builds sharing one cache folder crashed on each other's shaders).
//   Profile + LegacyVersions {meta, binary}        0.4 .. 0.7.1
//   shorter Profile (before paged_buffers) + LegacyVersions   v0.3
//   bare Profile (binary version 7)                before the versions were appended
// A header without the translator id is trusted only while the translator still hashes to
// kLegacyTranslatorId, the sources of the last build that wrote such headers (0.7.1); after any
// translator change those caches are retranslated once.
// Header-only: tests/test_shader_cache_header.cpp.
#pragma once

#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

#include "common/types.h"
#include "shader_recompiler/profile.h"

namespace Vulkan::CacheHeader {

/// bbport_shader_translator_id() of v0.7.1 (unchanged up to the build that added the id).
inline constexpr u64 kLegacyTranslatorId = 0x9e51728ea7bfbd05ull;

struct LegacyVersions {
    u32 meta = 0;
    u32 binary = 0;
};
static_assert(sizeof(LegacyVersions) == 8);

struct Versions {
    u32 meta = 0;
    u32 binary = 0;
    u64 translator = 0;
};
static_assert(sizeof(Versions) == 16);

/// What this build writes and accepts.
struct Current {
    u32 meta = 0;
    u32 binary = 0;
    u64 translator = 0;
    u64 legacy_translator = kLegacyTranslatorId;
};

struct Verdict {
    bool compatible = false;     // meta and SPIR-V may be loaded
    bool current_format = false; // the bytes are exactly what Make() writes (no rewrite needed)
};

inline std::vector<u8> Make(const Shader::Profile& profile, const Current& cur) {
    std::vector<u8> data(sizeof(Shader::Profile) + sizeof(Versions));
    const Versions v{cur.meta, cur.binary, cur.translator};
    std::memcpy(data.data(), &profile, sizeof(profile));
    std::memcpy(data.data() + sizeof(profile), &v, sizeof(v));
    return data;
}

inline Verdict Check(std::span<const u8> data, const Shader::Profile& profile, const Current& cur) {
    // Binary version 9 changed only shaders built for the layer memory model (paged buffers:
    // page-table pairs, guarded write-through stores). Without it the SPIR-V is the same as
    // version 7's, so caches of version 7 stay valid then instead of being thrown away (most
    // entries of those caches have no source to rebuild them from).
    const auto versions_ok = [&](u32 meta, u32 binary) {
        return meta == cur.meta && (binary == cur.binary || (binary == 7u && !profile.paged_buffers));
    };
    // Headers of binary version 7 predate paged_buffers, which took a padding byte of the struct
    // (any value there): compare only what existed then.
    constexpr size_t before_paged = offsetof(Shader::Profile, paged_buffers);
    constexpr size_t before_clip = offsetof(Shader::Profile, supports_depth_clip_control);
    const auto profile_ok = [&](u32 binary) {
        if (binary == 7u) {
            return std::memcmp(data.data(), &profile, before_paged) == 0;
        }
        Shader::Profile cached{};
        std::memcpy(&cached, data.data(), sizeof(cached));
        return cached == profile;
    };
    const bool legacy_ok = cur.translator == cur.legacy_translator;
    const size_t size = data.size();
    Verdict v;
    if (size == sizeof(Shader::Profile) + sizeof(Versions)) {
        Versions cached{};
        std::memcpy(&cached, data.data() + sizeof(Shader::Profile), sizeof(cached));
        v.compatible = cached.translator == cur.translator && versions_ok(cached.meta, cached.binary) &&
                       profile_ok(cached.binary);
        v.current_format = v.compatible && cached.meta == cur.meta && cached.binary == cur.binary;
    } else if (size == sizeof(Shader::Profile) + sizeof(LegacyVersions)) {
        // 0.4 .. 0.7.1: no translator id.
        LegacyVersions cached{};
        std::memcpy(&cached, data.data() + sizeof(Shader::Profile), sizeof(cached));
        v.compatible = legacy_ok && versions_ok(cached.meta, cached.binary) && profile_ok(cached.binary);
    } else if (size >= before_paged + sizeof(LegacyVersions) &&
               size < sizeof(Shader::Profile) + sizeof(LegacyVersions)) {
        // Header of a build before paged_buffers (v0.3): Profile of that size, then versions.
        LegacyVersions cached{};
        std::memcpy(&cached, data.data() + size - sizeof(LegacyVersions), sizeof(cached));
        v.compatible = legacy_ok && !profile.paged_buffers &&
                       std::memcmp(data.data(), &profile, before_paged) == 0 &&
                       versions_ok(cached.meta, cached.binary);
    } else if (size >= before_clip && size <= sizeof(Shader::Profile)) {
        // Bare Profile of a build before the versions were appended: binary version 7.
        v.compatible = legacy_ok && !profile.paged_buffers &&
                       std::memcmp(data.data(), &profile, before_clip) == 0;
    }
    return v;
}

} // namespace Vulkan::CacheHeader

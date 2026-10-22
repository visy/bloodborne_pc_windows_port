// Shader cache header (gpu/shadps4/video_core/renderer_vulkan/vk_cache_header.h): which caches a
// build may load. ninja -C out/gpu shader-cache-header-test
#include "video_core/renderer_vulkan/vk_cache_header.h"

#include <cassert>
#include <cstdio>
#include <cstring>

using namespace Vulkan::CacheHeader;

namespace {

Shader::Profile Gpu(bool paged) {
    Shader::Profile p{};
    p.max_viewport_width = 16384;
    p.max_viewport_height = 16384;
    p.max_shared_memory_size = 49152;
    p.supports_depth_clip_control = true;
    p.paged_buffers = paged;
    return p;
}

std::vector<u8> Legacy(const Shader::Profile& p, u32 meta, u32 binary) {
    std::vector<u8> d(sizeof(p) + sizeof(LegacyVersions));
    const LegacyVersions v{meta, binary};
    std::memcpy(d.data(), &p, sizeof(p));
    std::memcpy(d.data() + sizeof(p), &v, sizeof(v));
    return d;
}

} // namespace

int main() {
    const u64 kNewTranslator = 0x1234567890abcdefull;
    const Shader::Profile gpu = Gpu(true);
    const Current now{7, 9, kLegacyTranslatorId};      // a build whose translator is 0.7.1's
    const Current changed{7, 9, kNewTranslator};       // the translator changed, versions not bumped

    // Its own header: loaded, nothing to rewrite.
    const std::vector<u8> own = Make(gpu, now);
    assert(own.size() == sizeof(Shader::Profile) + 16);
    Verdict v = Check(own, gpu, now);
    assert(v.compatible && v.current_format);

    // The point of the translator id: same hand-bumped versions, other translator -> rebuilt.
    assert(!Check(own, gpu, changed).compatible);
    assert(!Check(Make(gpu, changed), gpu, now).compatible);
    assert(Check(Make(gpu, changed), gpu, changed).compatible);

    // Other versions or another GPU: rebuilt.
    assert(!Check(Make(gpu, Current{8, 9, kLegacyTranslatorId}), gpu, now).compatible);
    assert(!Check(Make(gpu, Current{7, 10, kLegacyTranslatorId}), gpu, now).compatible);
    Shader::Profile other = gpu;
    other.max_shared_memory_size = 65536;
    assert(!Check(Make(other, now), gpu, now).compatible);

    // A 0.7.1 header (no translator id): kept while the translator is still 0.7.1's, and then
    // rewritten in the new format; after a translator change it is rebuilt once.
    const std::vector<u8> v071 = Legacy(gpu, 7, 9);
    v = Check(v071, gpu, now);
    assert(v.compatible && !v.current_format);
    assert(!Check(v071, gpu, changed).compatible);
    assert(!Check(Legacy(gpu, 7, 8), gpu, now).compatible);
    // The old 8-byte versions are never read as the new layout (or the reverse).
    assert(v071.size() != own.size());

    // Binary version 7 without paged buffers (same SPIR-V): still accepted, legacy only.
    const Shader::Profile flat = Gpu(false);
    assert(Check(Legacy(flat, 7, 7), flat, now).compatible);
    assert(!Check(Legacy(flat, 7, 7), flat, changed).compatible);
    assert(!Check(Legacy(gpu, 7, 7), gpu, now).compatible); // paged buffers changed those shaders

    // A bare Profile of an old build (before the versions were appended; a shorter struct,
    // up to supports_depth_clip_control): binary 7, no paged buffers only.
    {
        const size_t old_size = offsetof(Shader::Profile, supports_depth_clip_control);
        std::vector<u8> bare(old_size);
        std::memcpy(bare.data(), &flat, old_size);
        assert(Check(bare, flat, now).compatible);
        assert(!Check(bare, flat, changed).compatible);
        std::memcpy(bare.data(), &gpu, old_size);
        assert(!Check(bare, gpu, now).compatible);
    }

    // v0.3: a Profile cut before paged_buffers, then the 8-byte versions.
    {
        const size_t cut = offsetof(Shader::Profile, paged_buffers);
        std::vector<u8> v03(cut + sizeof(LegacyVersions));
        const LegacyVersions lv{7, 7};
        std::memcpy(v03.data(), &flat, cut);
        std::memcpy(v03.data() + cut, &lv, sizeof(lv));
        assert(Check(v03, flat, now).compatible);
        assert(!Check(v03, flat, changed).compatible);
    }

    // Empty, truncated or oversized files: rebuilt.
    assert(!Check({}, gpu, now).compatible);
    std::vector<u8> cut(own.begin(), own.begin() + 5);
    assert(!Check(cut, gpu, now).compatible);
    std::vector<u8> big(own.size() + 1, 0);
    std::memcpy(big.data(), own.data(), own.size());
    assert(!Check(big, gpu, now).compatible);

    std::puts("shader-cache-header-test: ok");
    return 0;
}

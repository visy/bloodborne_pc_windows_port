// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <memory>
#include <optional>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include "video_core/renderer_vulkan/scene_resolution.h"
#include "video_core/texture_cache/image.h"

namespace VideoCore { class TextureCache; }
namespace Vulkan {
class Instance;
class Runtime;
class Scheduler;

// Reduced raster targets, with native-size images retained for unmodified guest compute,
// integer texture loads, copies and CPU readbacks. All native access passes Runtime::Transit.
class SceneTargets {
public:
    SceneTargets(const Instance&, Scheduler&, Runtime&, VideoCore::TextureCache&);
    using Lookup = std::function<VideoCore::Image*(VideoCore::ImageId, u64)>;
    SceneTargets(const Instance&, Scheduler&, Runtime&, Lookup);
    ~SceneTargets();
    bool SetSize(SceneResolution::Size);
    SceneResolution::Size Size() const { return size; }
    bool Reduced() const { return size != SceneResolution::Size{}; }
    /// A reduced proxy can stand in for the image: a 1920x1080 scene target or, scaled by
    /// the same factor, a half-resolution (960x540) one (toggle SceneHalfRes).
    bool Eligible(const VideoCore::Image&) const;
    /// Eligible at the full scene size (1920x1080): the upscaler's inputs, the scene start.
    bool EligibleScene(const VideoCore::Image&) const;
    /// The proxy size of an eligible image: the scene size divided like the native size.
    SceneResolution::Size ProxySize(const VideoCore::Image&, u32 level = 0) const;
    struct Target {
        vk::Image image;
        vk::ImageView view;
        vk::ImageLayout layout;
        vk::ImageUsageFlags usage;
    };
    Target Attachment(VideoCore::ImageId, const VideoCore::ImageViewInfo&);
    Target Read(VideoCore::ImageId, const VideoCore::ImageViewInfo&,
                vk::PipelineStageFlags2 = vk::PipelineStageFlagBits2::eComputeShader,
                vk::AccessFlags2 = vk::AccessFlagBits2::eShaderRead);
    void NativeAccess(VideoCore::Image&, vk::AccessFlags2);
    /// The proxy of `image` for sampling when it holds the current content (else nullopt: the
    /// native image is current). Transitions only after other use (attachment, copy).
    /// Mipmapped targets have one proxy per level: views of one level only.
    std::optional<Target> SampleProxy(const VideoCore::Image& image,
                                      const VideoCore::ImageViewInfo& info);
    /// Whether SampleProxy would return the proxy of `image` for these levels.
    [[nodiscard]] bool ProxyCurrent(const VideoCore::Image& image, u32 level = 0,
                                    u32 levels = 1) const;
    /// SampleProxy for a view obtained before (valid while Generation() is unchanged): the
    /// layout to sample it in.
    vk::ImageLayout PrepareSample(const VideoCore::Image& image, u32 level = 0);
    /// Changes when proxies and their views are destroyed (SetSize).
    [[nodiscard]] u64 Generation() const noexcept {
        return generation;
    }
    /// Whether NativeAccess has work for this image (a reduced-size proxy exists).
    [[nodiscard]] bool Tracks(u64 image_uid) const {
        return !copying && tracked.contains(image_uid);
    }
    void ResolveAll();
    bool debug = false; ///< BB_SCENE_DEBUG frame: print resolves and fills

    // ---- Volumetric effects resolution (volumetric_resolution.h) ----
    /// The level (VolumetricResolution::Level) from the next frame on; a change resolves and
    /// drops every proxy (like SetSize). Returns whether it changed.
    bool SetVolumetricLevel(int level);
    [[nodiscard]] int VolumetricLevel() const noexcept {
        return volumetric_level;
    }
    [[nodiscard]] bool VolumetricActive() const noexcept {
        return volumetric_level != 0;
    }
    /// The G-buffer pass's depth size (rule 1's reference).
    void NoteSceneDepth(SceneResolution::Size size) {
        scene_depth = size;
    }
    [[nodiscard]] SceneResolution::Size SceneDepth() const noexcept {
        return scene_depth;
    }
    /// The volumetric depth's size (rule 3's reference; 0x0 until seen).
    [[nodiscard]] SceneResolution::Size VolumetricHalf() const noexcept {
        return volumetric_half;
    }
    /// Queues `id` as volumetric from the next frame on (a pass keeps one size within a frame).
    void QueueVolumetric(VideoCore::ImageId id, const VideoCore::Image& image);
    /// Frame start: applies the queued classifications. Returns whether any image changed.
    bool CommitVolumetric();
    /// Changes when the volumetric classification or level changes (render state memo key).
    [[nodiscard]] u64 VolumetricGeneration() const noexcept {
        return volumetric_generation;
    }
    /// The volumetric images classified so far (diagnostics, the log line).
    [[nodiscard]] std::size_t VolumetricCount() const noexcept {
        return volumetric_count;
    }
    /// Test aid (BB_VOLUMETRIC_DUMP_TRIGGER): writes every classified volumetric color image as
    /// it is drawn (its proxy when that holds the content, else the game's image) to
    /// <dir>/vol<frame>_<address>_<w>x<h>_<format>.raw once the GPU is done.
    void DumpVolumetric(const char* dir, u32 frame);
    /// The size `image` would have without the volumetric scale (its proxy's base).
    SceneResolution::Size BaseSize(const VideoCore::Image&, u32 level = 0) const;
private:
    struct Entry {
        VideoCore::ImageId source{};
        u64 uid = 0;
        u32 level = 0; ///< mip level of the native image this proxy stands for
        VideoCore::UniqueImage image;
        std::vector<std::pair<VideoCore::ImageViewInfo, vk::UniqueImageView>> views;
        vk::ImageLayout layout = vk::ImageLayout::eUndefined;
        SceneResolution::Coherence state;
    };
    static constexpr u64 Key(u64 uid, u32 level) {
        return uid << 4 | level;
    }
    Entry& Get(VideoCore::ImageId, u32 level = 0);
    vk::ImageView View(Entry&, const VideoCore::Image&, const VideoCore::ImageViewInfo&);
    void Copy(Entry&, VideoCore::Image&, bool to_native);
    // Depth/stencil formats without blit support (D32S8 on RADV) are resampled by a
    // fullscreen draw writing gl_FragDepth and, with stencil export, the stencil value.
    vk::FormatFeatureFlags Features(vk::Format) const;
    bool Blittable(vk::Format) const;
    bool ShaderResampled(const VideoCore::Image&) const;
    void Resample(vk::Image src, vk::Image dst, const VideoCore::Image& original,
                  vk::Extent2D dst_size);
    vk::Pipeline ResamplePipeline(vk::Format, bool stencil);
    void CreateResampleResources();
    void Transition(Entry&, vk::ImageAspectFlags, vk::ImageLayout,
                    vk::PipelineStageFlags2, vk::AccessFlags2);
    const Instance& instance;
    Scheduler& scheduler;
    Runtime& runtime;
    Lookup lookup;
    /// bbport: frees unused textures, up to the bytes given (TextureCache::ReclaimMemory).
    std::function<u64(u64)> reclaim;
    SceneResolution::Size size;
    std::unordered_map<u64, std::unique_ptr<Entry>> entries; ///< by Key(uid, level)
    std::unordered_set<u64> tracked; ///< uids with proxies
    bool copying = false;
    bool force_stencil_bits = false; ///< test the portable resampler on any driver
    u64 generation = 0;
    int volumetric_level = 0;
    u64 volumetric_generation = 0;
    std::size_t volumetric_count = 0;
    SceneResolution::Size scene_depth{0, 0};
    SceneResolution::Size volumetric_half{0, 0};
    std::vector<std::pair<VideoCore::ImageId, u64>> volumetric_queue;
    std::vector<std::pair<VideoCore::ImageId, u64>> volumetric_images; ///< committed (diagnostics)
    mutable std::unordered_map<vk::Format, vk::FormatFeatureFlags> format_features;
    mutable std::array<vk::FormatFeatureFlags, 256> format_table{};
    mutable std::array<bool, 256> format_known{};
    std::array<std::pair<u64, Entry*>, 8> recent{}; ///< last entries by Key
    u32 recent_next = 0;
    vk::UniqueShaderModule fs_tri_vert, depth_frag, depth_stencil_frag, stencil_bits_frag;
    vk::UniqueDescriptorSetLayout resample_set_layout;
    vk::UniquePipelineLayout resample_layout;
    std::vector<std::pair<std::pair<vk::Format, bool>, vk::UniquePipeline>> resample_pipelines;
};
} // namespace Vulkan

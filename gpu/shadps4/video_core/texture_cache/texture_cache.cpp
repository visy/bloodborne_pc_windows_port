// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <xxhash.h>

#include <array>
#include <bit>
#include <cstdio>
#include "bbport_button_prompts.h"
#include "bbport_toggles.h"
#include "bbport_free_check.h"
#include "bbport_settings.h"
#include "common/assert.h"
#include "common/debug.h"
#include "common/div_ceil.h"
#include "common/hash.h"
#include "common/scope_exit.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/texture_cache/reclaim_rounds.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/host_compatibility.h"
#include "video_core/texture_cache/texture_cache.h"
#include "video_core/texture_cache/tile_manager.h"

namespace VideoCore {

static constexpr u64 PageShift = 12;

// bbport (Mrsuss60/bloodborne_pc_windows_port d81d63be, shadPS4 issue #184 "GC part 2"): the
// collector checks what it frees. BB_GC_VERIFY=0 returns to the earlier rules.
static bool GcVerify() {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_GC_VERIFY");
        return !(env && env[0] == '0');
    }();
    return enabled;
}

// Counts of the verification's decisions, printed with the collector's report.
static struct GcVerifyStats {
    std::atomic<u64> gated{0};         // write-back refused: CanBeDownloaded false
    std::atomic<u64> unverifiable{0};  // kept: buffer-written, no hash
    std::atomic<u64> overlap_checked{0};
    std::atomic<u64> overlap_freed{0}; // buffer data newer: freed without write-back
    std::atomic<u64> edge_flushes{0};
    std::atomic<u64> edge_dirty{0};    // edge pages rewritten by the guest
    std::atomic<u64> marked{0};        // images marked maybe-GPU-dirty
    std::atomic<u64> verified_write_backs{0};
} g_vfy;

static u64 HashBackingMemory(VAddr virtual_addr, u64 size) {
    if (virtual_addr == 0 || size == 0) {
        return 0;
    }
    // Through the backing view in chunks: a page the GPU's read protection covers must not fault
    // on the thread that would have to serve the fault (Memory::ReadBacking).
    XXH3_state_t* state = XXH3_createState();
    XXH3_64bits_reset(state);
    std::array<u8, 64_KB> chunk;
    auto* memory = Core::Memory::Instance();
    for (u64 done = 0; done < size;) {
        const u64 n = std::min<u64>(chunk.size(), size - done);
        memory->ReadBacking(virtual_addr + done, chunk.data(), n);
        XXH3_64bits_update(state, chunk.data(), n);
        done += n;
    }
    const u64 hash = XXH3_64bits_digest(state);
    XXH3_freeState(state);
    return hash | 1; // never 0: 0 means "no hash"
}
static constexpr u64 NumFramesBeforeRemoval = 32;

// Mrsuss60 6b2cc0e0 (shadPS4 ca36c7e3): SubresourceExtent's defaulted compare orders by levels
// first and layers only on a tie, so an image with more mips but fewer layers counted as "bigger".
// Contains() asks both dimensions. BB_TEX_EXTENT_CONTAINS=0 restores the lexicographic compares.
static bool ExtentContainsOn() {
    static const bool on = [] {
        const char* env = std::getenv("BB_TEX_EXTENT_CONTAINS");
        return !(env && env[0] == '0');
    }();
    return on;
}
/// The cached image cannot serve the requested extent.
static bool ResourcesTooSmall(const SubresourceExtent& cached, const SubresourceExtent& requested) {
    return ExtentContainsOn() ? !cached.Contains(requested) : cached < requested;
}
/// The requested extent is bigger than the cached image: expand it.
static bool ExpandNeeded(const SubresourceExtent& cached, const SubresourceExtent& requested) {
    return ExtentContainsOn() ? !cached.Contains(requested) : requested > cached;
}

TextureCache::TextureCache(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                           Vulkan::Runtime& runtime_, AmdGpu::Liverpool* liverpool_,
                           BufferCache& buffer_cache_, PageManager& tracker_)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_}, liverpool{liverpool_},
      buffer_cache{buffer_cache_}, tracker{tracker_}, blit_helper{instance, scheduler},
      tile_manager{instance, scheduler, runtime, buffer_cache.GetStreamBuffer()},
      readback_linear_images{EmulatorSettings.IsReadbackLinearImagesEnabled()} {

    u32 max_samplers = instance.GetMaxSamplerAllocationCount();
    trigger_gc_samplers = max_samplers * 3 / 4;
    pressure_gc_samplers = max_samplers * 7 / 8;
    critical_gc_samplers = max_samplers * 15 / 16;

    // texture_quality (Mrsuss60/bloodborne_pc_windows_port 8841db5b, after deadinside28/bloodborne_pc):
    // fixed for the session. Auto = half on cards with 3 GB or less. The size decided on is the
    // card's own device-local memory (not the budget left at start, which shrinks while other
    // programs hold VRAM, so a 4 GB card is never made half by a browser); BB_VRAM_LIMIT_MB,
    // the test cap, lowers it.
    {
        const int quality = BbSettings::Get().texture_quality.load(std::memory_order_relaxed);
        u64 decide_mem = instance.GetDeviceLocalMemory() ? instance.GetDeviceLocalMemory()
                                                         : instance.GetTotalMemoryBudget();
        if (std::getenv("BB_VRAM_LIMIT_MB")) {
            decide_mem = std::min(decide_mem, instance.GetTotalMemoryBudget());
        }
        texture_skip_wanted = TextureQualitySkip(quality, decide_mem);
        std::printf("[Texture Quality] setting='%s', effective='%s' (device memory %.2f GB)\n",
                    BbSettings::TextureQualityName(quality),
                    texture_skip_wanted == 0 ? "full" : texture_skip_wanted == 1 ? "half" : "quarter",
                    static_cast<double>(decide_mem) / (1024.0 * 1024.0 * 1024.0));
        std::fflush(stdout);
    }

    // Set up garbage collection parameters.
    if (!instance.CanReportMemoryUsage()) {
        trigger_gc_memory = 0;
        pressure_gc_memory = DEFAULT_PRESSURE_GC_MEMORY;
        critical_gc_memory = DEFAULT_CRITICAL_GC_MEMORY;
        return;
    }

    u64 budget = instance.GetDeviceMemoryBudgetNow() ? instance.GetDeviceMemoryBudgetNow()
                                                    : instance.GetTotalMemoryBudget();
    const u64 local_mem = instance.GetDeviceLocalMemory();
    const bool is_low_vram = (local_mem > 0 && local_mem <= 4096ULL * 1024 * 1024) ||
                             (budget > 0 && budget <= 4096ULL * 1024 * 1024);
    const u64 base_mem = is_low_vram && local_mem > 0 ? local_mem : budget;
    if (base_mem > 0) {
        trigger_gc_memory = base_mem / 100 * (is_low_vram ? 70 : 70);
        pressure_gc_memory = base_mem / 100 * (is_low_vram ? 90 : 85);
        critical_gc_memory = base_mem / 100 * (is_low_vram ? 95 : 95);
    } else {
        pressure_gc_memory = DEFAULT_PRESSURE_GC_MEMORY;
        critical_gc_memory = DEFAULT_CRITICAL_GC_MEMORY;
        trigger_gc_memory = 0;
    }
}

TextureCache::~TextureCache() = default;

void TextureCache::AdjustView(const Image& image, ImageViewInfo& view) {
    const u32 skip = image.info.skip_levels;
    if (skip == 0) {
        return;
    }
    const u32 first = view.range.base.level;
    const u32 last = first + std::max(view.range.extent.levels, 1u) - 1;
    const u32 levels = image.info.resources.levels;
    const u32 new_first = std::min(first > skip ? first - skip : 0u, levels - 1);
    const u32 new_last = std::min(last > skip ? last - skip : 0u, levels - 1);
    view.range.base.level = new_first;
    view.range.extent.levels = std::max(new_last, new_first) - new_first + 1;
    view.min_lod = view.min_lod > skip * 256 ? view.min_lod - skip * 256 : 0;
}

std::string TextureCache::DescribeImagesIn(VAddr addr, u64 size) {
    std::unique_lock lock{mutex, std::try_to_lock}; // the caller may hold it already
    if (!lock.owns_lock()) {
        return " [images: not looked up]";
    }
    std::string out;
    int shown = 0;
    ForEachImageInRegion(addr, size, [&](ImageId, Image& image) {
        if (shown++ < 3) {
            out += fmt::format(" [image {:#x}+{:#x} {}x{} fmt {} tile {}{}{}]", image.info.guest_address,
                               image.info.guest_size, image.info.size.width,
                               image.info.size.height, u32(image.info.pixel_format),
                               u32(image.info.tile_mode), image.usage.render_target ? " RT" : "",
                               True(image.flags & ImageFlagBits::GpuModified) ? " gpu" : "");
        }
    });
    if (shown > 3) {
        out += fmt::format(" +{} more", shown - 3);
    }
    return out;
}

void TextureCache::ProcessDownloadImages() {
    // bbport: called for every fence (~450 a frame): the lock only when something is queued.
    if (!downloads_queued.load(std::memory_order_acquire)) {
        return;
    }
    std::unique_lock lk{download_images_mutex};
    downloads_queued.store(false, std::memory_order_relaxed);
    // Mrsuss60/bloodborne_pc_windows_port f8f450b1 (#176): one flush and one wait for all the
    // images queued, not one wait per image (multi-second stalls opening menus / item icons).
    std::vector<BatchedDownload> batch;
    batch.reserve(download_images.size());
    for (const ImageId image_id : download_images) {
        DownloadImageMemory(image_id, true, &batch);
    }
    download_images.clear();
    if (!batch.empty()) {
        runtime.FlushBarriers(); // pending barriers go out before the synchronous finish (#129)
        scheduler.Finish();
        for (const auto& item : batch) {
            FinishDownload(item);
        }
    }
}

void TextureCache::FinishDownload(const BatchedDownload& item) {
    item.download.Invalidate();
    BbFreeCheck::Check(item.guest_address, item.size, item.download.mapped,
                       BbFreeCheck::ImageDownload);
    Core::Memory::Instance()->TryWriteBacking(std::bit_cast<u8*>(item.guest_address),
                                              item.download.mapped, item.size);
    if (!WriteTracking()) {
        buffer_cache.InvalidateMemory(item.guest_address, item.size);
    }
}

void TextureCache::DownloadImageMemory(ImageId image_id, bool sync,
                                       std::vector<BatchedDownload>* batch) {
    if (!slot_images.is_allocated(image_id)) {
        return; // Mrsuss60 f8f450b1: a freed slot is not read
    }
    Image& image = slot_images[image_id];
    if (False(image.flags & ImageFlagBits::GpuModified)) {
        return;
    }
    const u32 download_size = image.info.pitch * image.info.size.height * image.info.size.depth *
                              image.info.resources.layers * (image.info.num_bits / 8);
    ASSERT(download_size <= image.info.guest_size);
    vk::BufferImageCopy image_copy = {
        .bufferOffset = 0,
        .bufferRowLength = image.info.pitch,
        .bufferImageHeight = image.info.size.height,
        .imageSubresource =
            {
                .aspectMask = image.info.props.is_depth ? vk::ImageAspectFlagBits::eDepth
                                                        : vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
        .imageOffset = {0, 0, 0},
        .imageExtent = {image.info.size.width, image.info.size.height, image.info.size.depth},
    };
    // bbport BB_GUEST_IN_PLACE: the GPU copies the image into the game's memory itself, in stream
    // order: before the fences that follow, no wait here and no CPU copy afterwards.
    // Where the memory has a VRAM copy the GPU copies into that copy as well: buffers bound over
    // it read the image's data in stream order (issue #122: motion blur read a render target as a
    // buffer, 0x104ff00000, from a VRAM copy refreshed only once the CPU copy below landed, frames
    // later: the frame smeared while the camera turned). The game's memory still gets it below.
    if (GuestInPlace() && image.info.guest_address != 0) {
        const auto [arena, offset] =
            buffer_cache.ObtainBuffer(image.info.guest_address, download_size, true);
        image_copy.bufferOffset = offset;
        runtime.DownloadImage(&image, arena, std::span{&image_copy, 1});
        if (buffer_cache.IsInPlace(image.info.guest_address, download_size)) {
            return;
        }
    }
    const auto download =
        runtime.GetStagingPool().Request(download_size, MemoryType::HostCached, 16, !sync);
    const vk::BufferImageCopy image_download = {
        .bufferOffset = download.offset,
        .bufferRowLength = image.info.pitch,
        .bufferImageHeight = image.info.size.height,
        .imageSubresource =
            {
                .aspectMask = image.info.props.is_depth ? vk::ImageAspectFlagBits::eDepth
                                                        : vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
        .imageOffset = {0, 0, 0},
        .imageExtent = {image.info.size.width, image.info.size.height, image.info.size.depth},
    };
    runtime.DownloadImage(&image, download.buffer, std::span{&image_download, 1});
    if (sync && batch) {
        batch->push_back({download, image.info.guest_address, download_size});
    } else if (sync) {
        runtime.FlushBarriers(); // pending barriers go out before the synchronous finish (#129)
        scheduler.Finish();
        FinishDownload({download, image.info.guest_address, download_size});
    } else {
        scheduler.DeferPriorityOperation(
            [this, device_addr = image.info.guest_address, download, download_size] {
                download.Invalidate();
                BbFreeCheck::Check(device_addr, download_size, download.mapped,
                                   BbFreeCheck::ImageDownload);
                Core::Memory::Instance()->TryWriteBacking(std::bit_cast<u8*>(device_addr),
                                                          download.mapped, download_size);
                if (!WriteTracking()) {
                    buffer_cache.InvalidateMemory(device_addr, download_size);
                }
                runtime.GetStagingPool().FreeDeferred(download);
            });
    }
}

static inline u64 MaybeDirtyHash(const Image& image) {
    return XXH3_64bits(std::bit_cast<const u8*>(image.info.guest_address), image.info.guest_size);
}

void TextureCache::MarkAsMaybeDirty(ImageId image_id, Image& image) {
    if (image.hash == 0) {
        // Initialize hash
        image.hash = MaybeDirtyHash(image);
    }
    image.flags |= ImageFlagBits::MaybeCpuDirty;
    UntrackImage(image_id);
}

void TextureCache::InvalidateMemory(VAddr addr, size_t size) {
    std::scoped_lock lock{mutex};
    const auto pages_start = PageManager::GetPageAddr(addr);
    const auto pages_end = PageManager::GetNextPageAddr(addr + size - 1);
    ForEachImageInRegion(pages_start, pages_end - pages_start, [&](ImageId image_id, Image& image) {
        const auto image_begin = image.info.guest_address;
        const auto image_end = image.info.guest_address + image.info.guest_size;
        if (image.Overlaps(addr, size)) {
            // Modified region overlaps image, so the image was definitely accessed by this fault.
            // Untrack the image, so that the range is unprotected and the guest can write freely.
            image.flags |= ImageFlagBits::CpuDirty;
            PageManager::NoteImageFault(image.info.guest_address, image.info.guest_size,
                                        image.info.size.width, image.info.size.height,
                                        u32(image.info.pixel_format), u32(image.info.tile_mode));
            UntrackImage(image_id);
        } else if (pages_end < image_end) {
            // This page access may or may not modify the image.
            // We should not mark it as dirty now. If it really was modified
            // it will receive more invalidations on its other pages.
            // Remove tracking from this page only.
            UntrackImageHead(image_id);
        } else if (image_begin < pages_start) {
            // This page access does not modify the image but the page should be untracked.
            // We should not mark this image as dirty now. If it really was modified
            // it will receive more invalidations on its other pages.
            UntrackImageTail(image_id);
        } else {
            // Image begins and ends on this page so it can not receive any more invalidations.
            // We will check it's hash later to see if it really was modified.
            MarkAsMaybeDirty(image_id, image);
        }
    });
}

// bbport BB_IMAGE_WATCH=addr (diagnostics): how the image at that address is used (first 300).
void TextureCache::WatchImage(const Image& image, const char* what) {
    static const u64 watched = [] {
        const char* env = std::getenv("BB_IMAGE_WATCH");
        return env ? std::strtoull(env, nullptr, 0) : 0ull;
    }();
    if (!watched || image.info.guest_address != watched) {
        return;
    }
    static std::atomic<u32> printed{0};
    if (printed.fetch_add(1) < 300) {
        std::printf("Image watch %#llx: %s, flags %#x\n", (unsigned long long)watched, what,
                    u32(image.flags));
    }
}

void TextureCache::InvalidateMemoryFromGPU(VAddr address, size_t max_size) {
    std::scoped_lock lock{mutex};
    // Images the write only overlaps (not at their start) are looked at by the collector before
    // it writes them back (d81d63be); small ranges only: this runs for every written binding.
    if (GcVerify() && max_size <= 1_MB) {
        MarkAsMaybeGpuDirty(address, max_size);
    }
    // bbport: only an image starting at `address` is affected, so the images over its first byte
    // are all there is to look at (the whole range was walked for every GPU write binding).
    ForEachImageInRegion(address, 1, [&](ImageId image_id, Image& image) {
        static const bool log = std::getenv("BB_IMAGE_SYNC_LOG") != nullptr;
        if (log) {
            static std::atomic<u32> printed{0};
            if (printed.fetch_add(1) < 200) {
                std::printf("Image sync: GPU write %#llx+%#llx over image %#llx %ux%u fmt %u flags %#x%s\n",
                            (unsigned long long)address, (unsigned long long)max_size,
                            (unsigned long long)image.info.guest_address, image.info.size.width,
                            image.info.size.height, u32(image.info.pixel_format), u32(image.flags),
                            image.info.guest_address == address ? "" : " (not at its start: kept)");
            }
        }
        // Only consider images that match base address.
        // TODO: Maybe also consider subresources
        if (image.info.guest_address != address) {
            return;
        }
        // Ensure image is reuploaded when accessed again.
        WatchImage(image, "GPU write (marked for refresh)");
        image.flags |= ImageFlagBits::GpuDirty;
    });
}

void TextureCache::MarkAsMaybeGpuDirty(VAddr addr, size_t size) {
    // Collector candidates only (GPU-written, no CPU changes pending), and bounded: the walk and
    // the hash of a big range would cost more than a write-back it guards.
    constexpr u64 MaxHashBytes = 16_MB;
    ForEachImageInRegion(addr, size, [&](ImageId, Image& image) {
        if (!image.HasGpuValidData()) {
            return;
        }
        if (True(image.flags & ImageFlagBits::MaybeGpuDirty)) {
            return;
        }
        if (image.info.guest_size <= MaxHashBytes) {
            image.gpu_hash = HashBackingMemory(image.info.guest_address, image.info.guest_size);
        }
        image.flags |= ImageFlagBits::MaybeGpuDirty;
        g_vfy.marked.fetch_add(1, std::memory_order_relaxed);
    });
}

void TextureCache::UnmapMemory(VAddr cpu_addr, size_t size) {
    std::scoped_lock lk{mutex};

    ImageIds deleted_images;
    ForEachImageInRegion(cpu_addr, size, [&](ImageId id, Image&) { deleted_images.push_back(id); });
    for (const ImageId id : deleted_images) {
        // TODO: Download image data back to host.
        FreeImage(id);
    }
}

ImageId TextureCache::ResolveDepthOverlap(const ImageInfo& requested_info, BindingType binding,
                                          ImageId cache_image_id) {
    auto& cache_image = slot_images[cache_image_id];

    if (!cache_image.info.props.is_depth && !requested_info.props.is_depth) {
        return {};
    }

    const bool stencil_match =
        requested_info.props.has_stencil == cache_image.info.props.has_stencil;
    const bool bpp_match = requested_info.num_bits == cache_image.info.num_bits;

    // If an image in the cache has less slices we need to expand it
    bool recreate = ResourcesTooSmall(cache_image.info.resources, requested_info.resources);

    switch (binding) {
    case BindingType::Texture:
        // The guest requires a depth sampled texture, but cache can offer only Rxf. Need to
        // recreate the image.
        recreate |= requested_info.props.is_depth && !cache_image.info.props.is_depth;
        break;
    case BindingType::Storage:
        // If the guest is going to use previously created depth as storage, the image needs to be
        // recreated. (TODO: Probably a case with linear rgba8 aliasing is legit)
        recreate |= cache_image.info.props.is_depth;
        break;
    case BindingType::RenderTarget:
        // Render target can have only Rxf format. If the cache contains only Dx[S8] we need to
        // re-create the image.
        ASSERT(!requested_info.props.is_depth);
        recreate |= cache_image.info.props.is_depth;
        break;
    case BindingType::DepthTarget:
        // The guest has requested previously allocated texture to be bound as a depth target.
        // In this case we need to convert Rx float to a Dx[S8] as requested
        recreate |= !cache_image.info.props.is_depth;

        // The guest is trying to bind a depth target and cache has it. Need to be sure that aspects
        // and bpp match
        recreate |= cache_image.info.props.is_depth && !(stencil_match && bpp_match);
        break;
    default:
        break;
    }

    if (recreate) {
        auto new_info = requested_info;
        new_info.resources = std::max(requested_info.resources, cache_image.info.resources);
        new_info.UpdateSize();
        const auto new_image_id = InsertImage(new_info);
        RegisterImage(new_image_id);

        // insert() may have grown slot_images and moved every image: `cache_image` would then
        // point at freed storage, so look it up again.
        auto& old_image = slot_images[cache_image_id];

        // Inherit image usage
        auto& new_image = slot_images[new_image_id];
        new_image.usage = old_image.usage;
        new_image.flags &= ~ImageFlagBits::Dirty;
        // When creating a depth buffer through overlap resolution don't clear it on first use.
        new_image.info.meta_info.htile_clear_mask = 0;
        runtime.CopyColorAndDepth(&old_image, &new_image);

        // Free the cache image.
        FreeImage(cache_image_id);
        return new_image_id;
    }

    // Will be handled by view
    return cache_image_id;
}

std::tuple<ImageId, int, int> TextureCache::ResolveOverlap(const ImageInfo& image_info,
                                                           BindingType binding,
                                                           ImageId cache_image_id,
                                                           ImageId merged_image_id) {
    auto& cache_image = slot_images[cache_image_id];
    const bool safe_to_delete =
        scheduler.CurrentTick() - cache_image.tick_accessed_last > NumFramesBeforeRemoval;

    // Equal address
    if (image_info.guest_address == cache_image.info.guest_address) {
        const u32 lhs_block_size = image_info.num_bits * image_info.num_samples;
        const u32 rhs_block_size = cache_image.info.num_bits * cache_image.info.num_samples;
        if (image_info.BlockDim() != cache_image.info.BlockDim() ||
            lhs_block_size != rhs_block_size) {
            // Very likely this kind of overlap is caused by allocation from a pool.
            if (safe_to_delete) {
                FreeImage(cache_image_id);
            }
            return {merged_image_id, -1, -1};
        }

        if (const auto depth_image_id = ResolveDepthOverlap(image_info, binding, cache_image_id)) {
            return {depth_image_id, -1, -1};
        }

        // Compressed view of uncompressed image with same block size.
        if (image_info.props.is_block && !cache_image.info.props.is_block) {
            return {ExpandImage(image_info, cache_image_id), -1, -1};
        }

        if (image_info.guest_size == cache_image.info.guest_size &&
            (image_info.type == AmdGpu::ImageType::Color3D ||
             cache_image.info.type == AmdGpu::ImageType::Color3D)) {
            return {ExpandImage(image_info, cache_image_id), -1, -1};
        }

        const bool pow2_padding_only =
            image_info.props.is_pow2 != cache_image.info.props.is_pow2 &&
            image_info.tile_mode == cache_image.info.tile_mode &&
            image_info.size == cache_image.info.size &&
            image_info.pitch == cache_image.info.pitch && image_info.resources.levels == 1 &&
            cache_image.info.resources.levels == 1 && image_info.resources.layers == 1 &&
            cache_image.info.resources.layers == 1;

        // Size and resources are less than or equal, use image view.
        if (image_info.pixel_format != cache_image.info.pixel_format ||
            image_info.guest_size <= cache_image.info.guest_size || pow2_padding_only) {
            auto result_id = merged_image_id ? merged_image_id : cache_image_id;
            const auto& result_image = slot_images[result_id];
            const bool is_compatible =
                IsVulkanFormatCompatible(result_image.info.pixel_format, image_info.pixel_format);
            return {is_compatible ? result_id : ImageId{}, -1, -1};
        }

        // Size and resources are greater, expand the image.
        if (image_info.type == cache_image.info.type &&
            ExpandNeeded(cache_image.info.resources, image_info.resources)) {
            return {ExpandImage(image_info, cache_image_id), -1, -1};
        }

        // Size is greater but resources are not, because the tiling mode is different.
        // Likely the address is reused for a image with a different tiling mode.
        if (image_info.tile_mode != cache_image.info.tile_mode) {
            if (safe_to_delete) {
                FreeImage(cache_image_id);
            }
            return {merged_image_id, -1, -1};
        }

        // Enhanced debug logging for unreachable case
        // Calculate expected size based on format and dimensions
        u64 expected_size =
            (static_cast<u64>(image_info.size.width) * static_cast<u64>(image_info.size.height) *
             static_cast<u64>(image_info.size.depth) * static_cast<u64>(image_info.num_bits) / 8);
        LOG_ERROR(Render_Vulkan,
                  "Unresolvable image overlap with equal memory address:\n"
                  "=== OLD IMAGE (cached) ===\n"
                  "  Address:        {:#x}\n"
                  "  Size:           {:#x} bytes\n"
                  "  Format:         {}\n"
                  "  Type:           {}\n"
                  "  Width:          {}\n"
                  "  Height:         {}\n"
                  "  Depth:          {}\n"
                  "  Pitch:          {}\n"
                  "  Mip levels:     {}\n"
                  "  Array layers:   {}\n"
                  "  Samples:        {}\n"
                  "  Tile mode:      {:#x}\n"
                  "  Block size:     {} bits\n"
                  "  Is block-comp:  {}\n"
                  "  Guest size:     {:#x}\n"
                  "  Last accessed:  tick {}\n"
                  "  Safe to delete: {}\n"
                  "  isPow2:         {}\n"
                  "  Alt tile:       {}\n"
                  "\n"
                  "=== NEW IMAGE (requested) ===\n"
                  "  Address:        {:#x}\n"
                  "  Size:           {:#x} bytes\n"
                  "  Format:         {}\n"
                  "  Type:           {}\n"
                  "  Width:          {}\n"
                  "  Height:         {}\n"
                  "  Depth:          {}\n"
                  "  Pitch:          {}\n"
                  "  Mip levels:     {}\n"
                  "  Array layers:   {}\n"
                  "  Samples:        {}\n"
                  "  Tile mode:      {:#x}\n"
                  "  Block size:     {} bits\n"
                  "  Is block-comp:  {}\n"
                  "  Guest size:     {:#x}\n"
                  "  isPow2:         {}\n"
                  "  Alt tile:       {}\n"
                  "\n"
                  "=== COMPARISON ===\n"
                  "  Same format:           {}\n"
                  "  Same type:             {}\n"
                  "  Same tile mode:        {}\n"
                  "  Same block size:       {}\n"
                  "  Same BlockDim:         {}\n"
                  "  Same pitch:            {}\n"
                  "  Same pow2:             {}\n"
                  "  Same alt tile:         {}\n"
                  "  Old resources <= new:  {} (old: {}, new: {})\n"
                  "  Old size <= new size:  {}\n"
                  "  Expected size (calc):  {} bytes\n"
                  "  Size ratio (new/expected): {:.2f}x\n"
                  "  Size ratio (new/old):  {:.2f}x\n"
                  "  Old vs expected diff:  {} bytes ({:+.2f}%)\n"
                  "  New vs expected diff:  {} bytes ({:+.2f}%)\n"
                  "  Merged image ID:       {}\n"
                  "  Binding type:          {}\n"
                  "  Current tick:          {}\n"
                  "  Age (ticks since last access): {}",

                  // Old image details
                  cache_image.info.guest_address, cache_image.info.guest_size,
                  vk::to_string(cache_image.info.pixel_format),
                  static_cast<int>(cache_image.info.type), cache_image.info.size.width,
                  cache_image.info.size.height, cache_image.info.size.depth, cache_image.info.pitch,
                  cache_image.info.resources.levels, cache_image.info.resources.layers,
                  cache_image.info.num_samples, static_cast<u32>(cache_image.info.tile_mode),
                  cache_image.info.num_bits, +cache_image.info.props.is_block,
                  cache_image.info.guest_size, cache_image.tick_accessed_last, safe_to_delete,
                  bool(cache_image.info.props.is_pow2), cache_image.info.alt_tile,

                  // New image details
                  image_info.guest_address, image_info.guest_size,
                  vk::to_string(image_info.pixel_format), static_cast<int>(image_info.type),
                  image_info.size.width, image_info.size.height, image_info.size.depth,
                  image_info.pitch, image_info.resources.levels, image_info.resources.layers,
                  image_info.num_samples, static_cast<u32>(image_info.tile_mode),
                  image_info.num_bits, image_info.props.is_block, image_info.guest_size,
                  bool(image_info.props.is_pow2), image_info.alt_tile,

                  // Comparison
                  (image_info.pixel_format == cache_image.info.pixel_format),
                  (image_info.type == cache_image.info.type),
                  (image_info.tile_mode == cache_image.info.tile_mode),
                  (image_info.num_bits == cache_image.info.num_bits),
                  (image_info.BlockDim() == cache_image.info.BlockDim()),
                  (image_info.pitch == cache_image.info.pitch),
                  (image_info.props.is_pow2 == cache_image.info.props.is_pow2),
                  (image_info.alt_tile == cache_image.info.alt_tile),
                  (cache_image.info.resources <= image_info.resources),
                  cache_image.info.resources.levels, image_info.resources.levels,
                  (cache_image.info.guest_size <= image_info.guest_size), expected_size,

                  // Size ratios
                  static_cast<double>(image_info.guest_size) / expected_size,
                  static_cast<double>(image_info.guest_size) / cache_image.info.guest_size,

                  // Difference between actual and expected sizes with percentages
                  static_cast<s64>(cache_image.info.guest_size) - static_cast<s64>(expected_size),
                  (static_cast<double>(cache_image.info.guest_size) / expected_size - 1.0) * 100.0,

                  static_cast<s64>(image_info.guest_size) - static_cast<s64>(expected_size),
                  (static_cast<double>(image_info.guest_size) / expected_size - 1.0) * 100.0,

                  merged_image_id.index, static_cast<int>(binding), scheduler.CurrentTick(),
                  scheduler.CurrentTick() - cache_image.tick_accessed_last);

        UNREACHABLE_MSG("Encountered unresolvable image overlap with equal memory address.");
    }

    // Right overlap, the image requested is a possible subresource of the image from cache.
    if (image_info.guest_address > cache_image.info.guest_address) {
        if (auto mip = image_info.MipOf(cache_image.info); mip >= 0) {
            if (auto slice = image_info.SliceOf(cache_image.info, mip); slice >= 0) {
                return {cache_image_id, mip, slice};
            }
        }

        // Image isn't a subresource but a chance overlap.
        if (safe_to_delete) {
            FreeImage(cache_image_id);
        }

        return {{}, -1, -1};
    } else {
        // Left overlap, the image from cache is a possible subresource of the image requested
        if (auto mip = cache_image.info.MipOf(image_info); mip >= 0) {
            if (auto slice = cache_image.info.SliceOf(image_info, mip); slice >= 0) {
                // We have a larger image created and a separate one, representing a subres of it
                // bound as render target. In this case we need to rebind render target.
                if (cache_image.binding.is_target) {
                    cache_image.binding.needs_rebind = 1u;
                    if (merged_image_id) {
                        GetImage(merged_image_id).binding.is_target = 1u;
                    }

                    FreeImage(cache_image_id);
                    return {merged_image_id, -1, -1};
                }

                // We need to have a larger, already allocated image to copy this one into
                if (merged_image_id) {
                    auto& merged_image = slot_images[merged_image_id];
                    runtime.CopyMip(&cache_image, &merged_image, mip, slice);
                    FreeImage(cache_image_id);
                }
            }
        }
    }

    return {merged_image_id, -1, -1};
}

ImageId TextureCache::ExpandImage(const ImageInfo& info, ImageId image_id) {
    const auto new_image_id = InsertImage(info);
    RegisterImage(new_image_id);

    auto& src_image = slot_images[image_id];
    auto& new_image = slot_images[new_image_id];

    RefreshImage(new_image);
    // A CPU-dirty source holds stale contents: copying them after RefreshImage would overwrite
    // the fresh upload. Copy (and inherit GpuModified) only when it holds GPU-written data.
    // (Mrsuss60 6b2cc0e0 / shadPS4 699009c9; BB_EXPAND_GPU_VALID=0: always copy)
    static const bool gpu_valid_only = [] {
        const char* env = std::getenv("BB_EXPAND_GPU_VALID");
        return !(env && env[0] == '0');
    }();
    if (!gpu_valid_only) {
        runtime.CopyImage(&src_image, &new_image);
    } else if (src_image.HasGpuValidData()) {
        runtime.CopyImage(&src_image, &new_image);
        new_image.flags |= (src_image.flags & ImageFlagBits::GpuModified);
    }

    if (src_image.binding.is_bound || src_image.binding.is_target) {
        src_image.binding.needs_rebind = 1u;
    }

    FreeImage(image_id);
    TrackImage(new_image_id);
    return new_image_id;
}

ImageId TextureCache::FindImage(ImageDesc& desc, bool exact_fmt) {
    const auto& info = desc.info;
    ASSERT(info.guest_address != 0);

    std::scoped_lock lock{mutex};

    u64 key_hash = info.guest_address ^ info.guest_size << 17 ^ u64(info.pixel_format) << 40 ^
                   u64(info.size.width) << 24 ^ info.size.height ^ u64(desc.type) << 58 ^
                   u64(exact_fmt) << 63;
    key_hash = (key_hash ^ key_hash >> 33) * 0xFF51AFD7ED558CCDull;
    key_hash = (key_hash ^ key_hash >> 33) * 0xC4CEB9FE1A85EC53ull;
    key_hash ^= key_hash >> 33;
    auto& cached = find_image_cache[key_hash % find_image_cache.size()];
    // An exact match stays valid while that image is registered with the same description;
    // resolved overlaps (views into other images) only until any image registration changes.
    // A texture made without its largest levels (texture_quality) keeps the whole texture's description.
    const auto whole_info = [](const Image& image) -> const ImageInfo& {
        return image.guest_info.guest_address != 0 ? image.guest_info : image.info;
    };
    const auto still_exact = [&] {
        if (cached.view_mip >= 0 || cached.view_slice >= 0 || !cached.image_id) {
            return false;
        }
        if (!slot_images.is_allocated(cached.image_id)) {
            return false; // Mrsuss60 7c0e5f0d: a freed slot is no use-after-free
        }
        const Image& image = slot_images[cached.image_id];
        return True(image.flags & ImageFlagBits::Registered) &&
               whole_info(image).guest_address == info.guest_address &&
               whole_info(image).guest_size == info.guest_size && whole_info(image).size == info.size &&
               whole_info(image).pixel_format == info.pixel_format;
    };
    if (cached.image_id && !slot_images.is_allocated(cached.image_id)) {
        cached = {};
    }
    if (cached.image_id &&
        (cached.generation == registry_generation.load(std::memory_order_relaxed) || still_exact()) &&
        cached.address == info.guest_address &&
        cached.size == info.guest_size && cached.extent == info.size &&
        cached.format == info.pixel_format && cached.type == info.type &&
        cached.exact_fmt == exact_fmt && cached.binding == desc.type &&
        cached.levels == info.resources.levels && cached.layers == info.resources.layers &&
        !BbToggle::Disabled(BbToggle::FindImageCache)) {
        Image& image = slot_images[cached.image_id];
        image.tick_accessed_last = scheduler.CurrentTick();
        TouchImage(image);
        if (cached.view_mip > 0) {
            desc.view_info.range.base.level = cached.view_mip;
        }
        if (cached.view_slice > 0) {
            desc.view_info.range.base.layer = cached.view_slice;
        }
        AdjustView(image, desc.view_info);
        return cached.image_id;
    }

    ImageIds image_ids;
    ForEachImageInRegion(info.guest_address, info.guest_size,
                         [&](ImageId image_id, Image& image) { image_ids.push_back(image_id); });

    // A texture without its largest levels (texture_quality) serves only the sampled binding of
    // that very texture: bound otherwise, or met by another image over it, it goes (made again
    // whole) and its address keeps its textures whole from then on.
    for (auto it = image_ids.begin(); it != image_ids.end();) {
        const Image& cached_img = slot_images[*it];
        if (cached_img.info.skip_levels == 0) {
            ++it;
            continue;
        }
        const ImageInfo& whole = cached_img.guest_info;
        const bool same = desc.type == BindingType::Texture &&
                          whole.guest_address == info.guest_address &&
                          whole.guest_size == info.guest_size && whole.size == info.size &&
                          whole.type == info.type && whole.resources == info.resources &&
                          IsVulkanFormatCompatible(whole.pixel_format, info.pixel_format);
        if (same) {
            ++it;
            continue;
        }
        whole_textures.insert(whole.guest_address);
        const ImageId id = *it;
        it = image_ids.erase(it);
        FreeImage(id);
    }

    ImageId image_id{};

    // Check for a perfect match first
    for (const auto& cache_id : image_ids) {
        auto& cache_image = slot_images[cache_id];
        const ImageInfo& match_info = whole_info(cache_image);
        if (match_info.guest_address != info.guest_address) {
            continue;
        }
        if (match_info.guest_size != info.guest_size) {
            continue;
        }
        if (match_info.size != info.size) {
            continue;
        }
        if (!IsVulkanFormatCompatible(match_info.pixel_format, info.pixel_format) ||
            (match_info.type != info.type && info.size != Extent3D{1, 1, 1})) {
            continue;
        }
        if (exact_fmt && info.pixel_format != match_info.pixel_format) {
            continue;
        }
        image_id = cache_id;
    }

    // Try to resolve overlaps (if any)
    int view_mip{-1};
    int view_slice{-1};
    if (!image_id) {
        for (const auto& cache_id : image_ids) {
            view_mip = -1;
            view_slice = -1;

            const auto& merged_info = image_id ? slot_images[image_id].info : info;
            auto [overlap_image_id, overlap_view_mip, overlap_view_slice] =
                ResolveOverlap(merged_info, desc.type, cache_id, image_id);
            if (overlap_image_id) {
                image_id = overlap_image_id;
                view_mip = overlap_view_mip;
                view_slice = overlap_view_slice;
            }
        }
    }

    if (image_id) {
        Image& image_resolved = slot_images[image_id];
        const ImageInfo& check_info = whole_info(image_resolved);
        if (exact_fmt && info.pixel_format != check_info.pixel_format) {
            // Cannot reuse this image as we need the exact requested format.
            image_id = {};
        } else if (ResourcesTooSmall(check_info.resources, info.resources)) {
            // The image was clearly picked up wrong.
            FreeImage(image_id);
            image_id = {};
            LOG_WARNING(Render_Vulkan, "Image overlap resolve failed");
        }
    }
    // Create and register a new image
    if (!image_id) {
        // texture_quality: only a sampled, plain, block-compressed texture nothing has written.
        u32 skip = 0;
        if (texture_skip_wanted != 0 && !whole_textures.contains(info.guest_address)) {
            skip = SkipLevelsFor(info, desc.type, desc.view_info.is_storage, texture_skip_wanted,
                                 buffer_cache.IsRegionGpuModified(info.guest_address, info.guest_size));
        }
        if (skip > 0) {
            image_id = InsertImage(info.Reduced(skip));
            slot_images[image_id].guest_info = info;
        } else {
            image_id = InsertImage(info);
        }
        RegisterImage(image_id);
    }

    Image& image = slot_images[image_id];
    image.tick_accessed_last = scheduler.CurrentTick();
    TouchImage(image);

    // If the image requested is a subresource of the image from cache record its location.
    if (view_mip > 0) {
        desc.view_info.range.base.level = view_mip;
    }
    if (view_slice > 0) {
        desc.view_info.range.base.layer = view_slice;
    }
    AdjustView(image, desc.view_info);

    cached = FindImageCacheEntry{
        .address = info.guest_address,
        .size = info.guest_size,
        .extent = info.size,
        .format = info.pixel_format,
        .type = info.type,
        .exact_fmt = exact_fmt,
        .binding = desc.type,
        .levels = info.resources.levels,
        .layers = info.resources.layers,
        .generation = registry_generation.load(std::memory_order_relaxed),
        .image_id = image_id,
        .view_mip = view_mip,
        .view_slice = view_slice,
    };
    return image_id;
}

ImageId TextureCache::FindImageFromRange(VAddr address, size_t size, bool ensure_valid) {
    ImageIds image_ids;
    ForEachImageWithAddress(address, size, [&](ImageId image_id, Image& image) {
        if (ensure_valid && !image.SafeToDownload()) {
            return;
        }
        if (image.info.skip_levels != 0) {
            return; // texture_quality: a sampled texture without its largest levels; its bytes are in guest memory
        }
        image_ids.push_back(image_id);
    });
    if (image_ids.size() == 1) {
        // Sometimes image size might not exactly match with requested buffer size
        // If we only found 1 candidate image use it without too many questions.
        return image_ids.back();
    }
    if (!image_ids.empty()) {
        for (s32 i = 0; i < image_ids.size(); ++i) {
            Image& image = slot_images[image_ids[i]];
            if (image.info.guest_size == size) {
                return image_ids[i];
            }
        }
        LOG_WARNING(Render_Vulkan,
                    "Failed to find exact image match for copy addr={:#x}, size={:#x}", address,
                    size);
    }
    return {};
}

ImageView& TextureCache::FindTexture(ImageId image_id, const ImageDesc& desc, ViewMemo* memo,
                                     bool refresh) {
    Image& image = slot_images[image_id];
    WatchImage(image, refresh ? "texture (refresh)" : "texture (no refresh)");
    if (desc.type == BindingType::Storage) {
        image.flags |= ImageFlagBits::GpuModified;
        image.gpu_write_tick = scheduler.CurrentTick();
        if (readback_linear_images && (!image.info.props.is_tiled || image.info.size.width <= 8) &&
            image.info.guest_address != 0) {
            std::unique_lock lk{download_images_mutex};
            download_images.emplace(image_id);
            downloads_queued.store(true, std::memory_order_release);
        }
    }
    if (refresh) {
        UpdateImage(image_id);
    }
    if (memo && !BbToggle::Disabled(BbToggle::TextureViewMemo)) {
        if (memo->image_id == image_id && memo->backing == image.backing && memo->view_id) {
            return slot_image_views[memo->view_id];
        }
        ImageView& view = image.FindView(desc.view_info);
        const auto& ids = image.backing->image_view_ids;
        const u32 last = image.backing->last_view;
        memo->image_id = image_id;
        memo->backing = image.backing;
        memo->view_id = last < ids.size() && &slot_image_views[ids[last]] == &view ? ids[last]
                                                                                    : ids.back();
        return view;
    }
    return image.FindView(desc.view_info);
}

ImageView& TextureCache::FindRenderTarget(ImageId image_id, const ImageDesc& desc) {
    Image& image = slot_images[image_id];
    WatchImage(image, "render target");
    image.flags |= ImageFlagBits::GpuModified;
    image.gpu_write_tick = scheduler.CurrentTick();
    if (readback_linear_images && (!image.info.props.is_tiled || image.info.size.width <= 8)) {
        std::unique_lock lk{download_images_mutex};
        download_images.emplace(image_id);
        downloads_queued.store(true, std::memory_order_release);
    }
    image.usage.render_target = 1u;
    UpdateImage(image_id);

    // Register meta data for this color buffer
    if (desc.info.meta_info.cmask_addr) {
        surface_metas.emplace(desc.info.meta_info.cmask_addr,
                              MetaDataInfo{.type = MetaType::CMask});
        image.info.meta_info.cmask_addr = desc.info.meta_info.cmask_addr;
    }

    if (desc.info.meta_info.fmask_addr) {
        surface_metas.emplace(desc.info.meta_info.fmask_addr,
                              MetaDataInfo{.type = MetaType::FMask});
        image.info.meta_info.fmask_addr = desc.info.meta_info.fmask_addr;
    }

    return image.FindView(desc.view_info, false);
}

ImageView& TextureCache::FindDepthTarget(ImageId image_id, const ImageDesc& desc) {
    Image& image = slot_images[image_id];
    WatchImage(image, "depth target");
    image.flags |= ImageFlagBits::GpuModified;
    image.gpu_write_tick = scheduler.CurrentTick();
    image.usage.depth_target = 1u;
    UpdateImage(image_id);

    // Register meta data for this depth buffer
    if (desc.info.meta_info.htile_addr) {
        surface_metas.emplace(desc.info.meta_info.htile_addr,
                              MetaDataInfo{.type = MetaType::HTile,
                                           .clear_mask = image.info.meta_info.htile_clear_mask});
        image.info.meta_info.htile_addr = desc.info.meta_info.htile_addr;
    }

    // If there is a stencil attachment, link depth and stencil.
    if (desc.info.stencil_addr != 0 && desc.info.stencil_addr != desc.info.guest_address) {
        ImageId stencil_id{};
        ForEachImageInRegion(desc.info.stencil_addr, desc.info.stencil_size,
                             [&](ImageId image_id, Image& image) {
                                 if (image.info.guest_address == desc.info.stencil_addr) {
                                     stencil_id = image_id;
                                 }
                             });
        if (!stencil_id) {
            ImageInfo info{};
            info.guest_address = desc.info.stencil_addr;
            info.guest_size = desc.info.stencil_size;
            info.size = desc.info.size;
            stencil_id = InsertImage(info);
            RegisterImage(stencil_id);
        }
        Image& stencil_image = slot_images[stencil_id];
        TouchImage(stencil_image);
        // insert() above may have grown slot_images and moved every image, leaving `image`
        // pointing at freed storage: look the depth image up again.
        stencil_image.AssociateDepth(image_id, slot_images[image_id].image_uid);
    }

    return slot_images[image_id].FindView(desc.view_info, false);
}

void TextureCache::RefreshImage(Image& image) {
    WatchImage(image, "refresh entered");
    if (False(image.flags & ImageFlagBits::Dirty) || image.info.num_samples > 1) {
        return;
    }
    BbStats::Timer timer{BbStats::t_refresh};

    RENDERER_TRACE;
    TRACE_HINT(fmt::format("{:x}:{:x}", image.info.guest_address, image.info.guest_size));

    if (True(image.flags & ImageFlagBits::MaybeCpuDirty) &&
        False(image.flags & ImageFlagBits::CpuDirty)) {
        const u64 hash = MaybeDirtyHash(image);
        if (image.hash == hash) {
            image.flags &= ~ImageFlagBits::MaybeCpuDirty;
            return;
        }
        image.hash = hash;
    }

    const u32 num_layers = image.info.resources.layers;
    const u32 num_mips = image.info.resources.levels;
    // bbport: the game's key guide icons (32x32, one level, known by their bytes) and the HUD's
    // atlases with icons packed in: the rasterizer may sample the port's for the device in use
    // (vk_button_prompts.h; from deadinside28/bloodborne_pc 72e5c8ce).
    if (num_mips == 1 && num_layers == 1 && image.info.props.is_block) {
        const auto* bytes = std::bit_cast<const u8*>(image.info.guest_address);
        if (image.info.size.width == 32 && image.info.size.height == 32 && image.info.guest_size >= 1024) {
            image.prompt_icon = u8(BbPrompts::IconOf(bytes, 1024));
        }
        static std::atomic<u32> prompt_uploads{0};
        image.prompt_atlas = u8(BbPrompts::AtlasOf(bytes, image.info.guest_size, image.info.size.width,
                                                   image.info.size.height));
        image.prompt_upload = image.prompt_atlas ? ++prompt_uploads : 0;
    }
    const bool is_gpu_modified = True(image.flags & ImageFlagBits::GpuModified);
    const bool is_gpu_dirty = True(image.flags & ImageFlagBits::GpuDirty);
    static const bool sync_log = std::getenv("BB_IMAGE_SYNC_LOG") != nullptr;
    if (sync_log && is_gpu_dirty) {
        static std::atomic<u32> printed{0};
        if (printed.fetch_add(1) < 100) {
            std::printf("Image sync: refresh of %#llx (%ux%u) from memory the GPU wrote, flags %#x\n",
                        (unsigned long long)image.info.guest_address, image.info.size.width,
                        image.info.size.height, u32(image.flags));
        }
    }

    if (sync_log) {
        // BB_IMAGE_SYNC_LOG: the images refreshed most per 5 s (any reason), with their flags.
        struct Refreshes {
            u64 count = 0, bytes = 0;
            u32 width = 0, height = 0, format = 0, tiling = 0, flags = 0;
        };
        static std::mutex refresh_mutex;
        static std::unordered_map<VAddr, Refreshes> refreshes;
        static auto window_start = std::chrono::steady_clock::now();
        std::scoped_lock lk{refresh_mutex};
        auto& r = refreshes[image.info.guest_address];
        r = {r.count + 1,
             r.bytes + image.info.guest_size,
             image.info.size.width,
             image.info.size.height,
             u32(image.info.pixel_format),
             u32(image.info.tile_mode),
             r.flags | u32(image.flags)};
        if (const auto now = std::chrono::steady_clock::now();
            now - window_start >= std::chrono::seconds(5)) {
            std::vector<std::pair<VAddr, Refreshes>> top(refreshes.begin(), refreshes.end());
            std::ranges::sort(top, std::greater{}, [](const auto& e) { return e.second.bytes; });
            std::string text;
            for (size_t i = 0; i < std::min<size_t>(top.size(), 5); ++i) {
                const auto& [address, e] = top[i];
                text += fmt::format(" {:#x} {}x{} fmt {} tile {}: {}x {:.0f} MB flags {:#x};",
                                    address, e.width, e.height, e.format, e.tiling, e.count,
                                    e.bytes / 1e6, e.flags);
            }
            std::printf("Image refreshes (5 s, %zu images):%s\n", refreshes.size(), text.c_str());
            refreshes.clear();
            window_start = now;
        }
    }
    // One copy per level (texture_quality: only the levels kept, offsets in the buffer
    // DetileImage / the guest layout gives); levels a hash shows unchanged are dropped below.
    boost::container::small_vector<vk::BufferImageCopy, 14> all_copies;
    blit_helper.BuildUploadCopies(image, num_mips, num_layers, all_copies);
    boost::container::small_vector<vk::BufferImageCopy, 14> image_copies;
    for (u32 m = 0; m < num_mips; m++) {
        const auto [mip_size, mip_pitch, mip_height, mip_offset] = image.info.mips_layout[m];

        // Protect GPU modified resources from accidental CPU reuploads.
        // Only for images the GPU writes or may write (small ones, targets, storage images): a
        // streamed texture hashed here cost the GPU thread its whole size again on every upload.
        // (From Supermedo/bloodborne_pc 136a72d.)
        const bool track = (is_gpu_modified && !is_gpu_dirty) || mip_size <= 64_KB ||
                           image.usage.storage || image.usage.render_target ||
                           image.usage.depth_target;
        if (track) {
            const u8* mip_addr = std::bit_cast<u8*>(image.info.guest_address) + mip_offset;
            const u64 mip_hash = XXH3_64bits(mip_addr, mip_size);
            if (is_gpu_modified && !is_gpu_dirty && image.mip_hashes[m] == mip_hash) {
                continue;
            }
            image.mip_hashes[m] = mip_hash;
        }

        image_copies.push_back(all_copies[m]);
    }

    WatchImage(image, image_copies.empty() ? "refresh: nothing to copy" : "refresh: uploading");
    if (image_copies.empty()) {
        image.flags &= ~ImageFlagBits::Dirty;
        return;
    }

    // Counted only when something is copied (the hash skip above used to count a whole image
    // that went nowhere), and apart when the source is the GPU's own buffer-cache copy: that is a
    // GPU-side copy, not an upload from guest memory.
    (is_gpu_dirty ? BbStats::image_gpu_copy_bytes : BbStats::image_upload_bytes)
        .fetch_add(image.info.guest_size, std::memory_order_relaxed);

    scheduler.EndRendering();

    const ImageInfo& upload_info = image.guest_info.guest_address != 0 ? image.guest_info : image.info;
    const auto [in_buffer, in_offset] =
        buffer_cache.ObtainBufferForImage(upload_info.guest_address, upload_info.guest_size);
    const auto [buffer, offset] = tile_manager.DetileImage(in_buffer, in_offset, upload_info, image.info.skip_levels);
    for (auto& copy : image_copies) {
        copy.bufferOffset += offset;
    }

    runtime.UploadImage(&image, buffer, image_copies);
}

vk::Sampler TextureCache::GetSampler(const AmdGpu::Sampler& sampler,
                                     AmdGpu::BorderColorBuffer border_color_base,
                                     const bool is_depth, float extra_lod_bias) {
    // Compare and plain uses of one S# need separate samplers; so do extra LOD biases.
    const u64 hash = HashCombine(HashCombine(XXH3_64bits(&sampler, sizeof(sampler)), is_depth),
                                 u64(std::bit_cast<u32>(extra_lod_bias)));

    std::scoped_lock lock{samplers_mutex};
    const auto [it, new_sampler] = samplers.try_emplace(hash, instance, sampler, border_color_base,
                                                        is_depth, extra_lod_bias);
    if (new_sampler) {
        samplers.at(hash).lru_id = sampler_lru_cache.Insert(hash, gc_tick);
    } else {
        sampler_lru_cache.Touch(it->second.lru_id, gc_tick);
    }

    return it->second.Handle();
}

void TextureCache::RegisterImage(ImageId image_id) {
    BbStats::images_registered.fetch_add(1, std::memory_order_relaxed);
    Image& image = slot_images[image_id];
    ASSERT_MSG(False(image.flags & ImageFlagBits::Registered),
               "Trying to register an already registered image");
    image.flags |= ImageFlagBits::Registered;
    ++registry_generation;
    total_used_memory += Common::AlignUp(image.info.guest_size, 1024);
    BbStats::live_image_bytes.fetch_add(Common::AlignUp(image.info.guest_size, 1024),
                                        std::memory_order_relaxed);
    BbStats::live_images.fetch_add(1, std::memory_order_relaxed);
    image.lru_id = lru_cache.Insert(image_id, gc_tick);
    image.lru_touched_tick = gc_tick;
    ForEachPage(image.info.guest_address, image.info.guest_size,
                [this, image_id](u64 page) { page_table[page].push_back(image_id); });
}

void TextureCache::UnregisterImage(ImageId image_id) {
    // Mrsuss60 7c0e5f0d: no memoized lookup keeps the id of an image that leaves the registry.
    for (auto& entry : find_image_cache) {
        if (entry.image_id == image_id) {
            entry = {};
        }
    }
    Image& image = slot_images[image_id];
    ASSERT_MSG(True(image.flags & ImageFlagBits::Registered),
               "Trying to unregister an already unregistered image");
    image.flags &= ~ImageFlagBits::Registered;
    ++registry_generation;
    lru_cache.Free(image.lru_id);
    total_used_memory -= Common::AlignUp(image.info.guest_size, 1024);
    BbStats::live_image_bytes.fetch_sub(Common::AlignUp(image.info.guest_size, 1024),
                                        std::memory_order_relaxed);
    BbStats::live_images.fetch_sub(1, std::memory_order_relaxed);
    ForEachPage(image.info.guest_address, image.info.guest_size, [this, image_id](u64 page) {
        const auto page_it = page_table.find(page);
        if (page_it == nullptr) {
            UNREACHABLE_MSG("Unregistering unregistered page=0x{:x}", page << PageShift);
            return;
        }
        auto& image_ids = *page_it;
        const auto vector_it = std::ranges::find(image_ids, image_id);
        if (vector_it == image_ids.end()) {
            ASSERT_MSG(false, "Unregistering unregistered image in page=0x{:x}", page << PageShift);
            return;
        }
        image_ids.erase(vector_it);
    });
}

void TextureCache::TrackImage(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!(image.flags & ImageFlagBits::Registered)) {
        return;
    }
    const auto image_begin = image.info.guest_address;
    const auto image_end = image.info.guest_address + image.info.guest_size;
    if (image_begin == image.track_addr && image_end == image.track_addr_end) {
        return;
    }

    if (!image.IsTracked()) {
        // Re-track the whole image
        image.track_addr = image_begin;
        image.track_addr_end = image_end;
        tracker.UpdatePageWatchers<1>(image_begin, image.info.guest_size);
    } else {
        if (image_begin < image.track_addr) {
            TrackImageHead(image_id);
        }
        if (image.track_addr_end < image_end) {
            TrackImageTail(image_id);
        }
    }
    MarkDirtyIfEdgesModified(image);
    image.head_hash = 0;
    image.tail_hash = 0;
}

void TextureCache::TrackImageHead(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!(image.flags & ImageFlagBits::Registered)) {
        return;
    }
    const auto image_begin = image.info.guest_address;
    if (image_begin == image.track_addr) {
        return;
    }
    ASSERT(image.track_addr != 0 && image_begin < image.track_addr);
    const auto size = image.track_addr - image_begin;
    image.track_addr = image_begin;
    tracker.UpdatePageWatchers<1>(image_begin, size);
}

void TextureCache::TrackImageTail(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!(image.flags & ImageFlagBits::Registered)) {
        return;
    }
    const auto image_end = image.info.guest_address + image.info.guest_size;
    if (image_end == image.track_addr_end) {
        return;
    }
    ASSERT(image.track_addr_end != 0 && image.track_addr_end < image_end);
    const auto addr = image.track_addr_end;
    const auto size = image_end - image.track_addr_end;
    image.track_addr_end = image_end;
    tracker.UpdatePageWatchers<1>(addr, size);
}

void TextureCache::UntrackImage(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!image.IsTracked()) {
        return;
    }
    const auto addr = image.track_addr;
    const auto size = image.track_addr_end - image.track_addr;
    image.track_addr = 0;
    image.track_addr_end = 0;
    if (size != 0) {
        tracker.UpdatePageWatchers<false>(addr, size);
    }
}

void TextureCache::UntrackImageHead(ImageId image_id) {
    auto& image = slot_images[image_id];
    const auto image_begin = image.info.guest_address;
    if (!image.IsTracked() || image_begin < image.track_addr) {
        return;
    }
    const auto addr = tracker.GetNextPageAddr(image_begin);
    const auto size = addr - image_begin;
    if (GcVerify()) {
        image.head_hash = HashBackingMemory(image_begin, size);
    }
    image.track_addr = addr;
    if (image.track_addr == image.track_addr_end) {
        // This image spans only 2 pages and both are modified,
        // but the image itself was not directly affected.
        // Cehck its hash later.
        MarkAsMaybeDirty(image_id, image);
    }
    tracker.UpdatePageWatchers<false>(image_begin, size);
}

void TextureCache::UntrackImageTail(ImageId image_id) {
    auto& image = slot_images[image_id];
    const auto image_end = image.info.guest_address + image.info.guest_size;
    if (!image.IsTracked() || image.track_addr_end < image_end) {
        return;
    }
    ASSERT(image.track_addr_end != 0);
    const auto addr = tracker.GetPageAddr(image_end);
    const auto size = image_end - addr;
    if (GcVerify()) {
        image.tail_hash = HashBackingMemory(addr, size);
    }
    image.track_addr_end = addr;
    if (image.track_addr == image.track_addr_end) {
        // This image spans only 2 pages and both are modified,
        // but the image itself was not directly affected.
        // Cehck its hash later.
        MarkAsMaybeDirty(image_id, image);
    }
    tracker.UpdatePageWatchers<false>(addr, size);
}

void TextureCache::MarkDirtyIfEdgesModified(Image& image) {
    const auto image_begin = image.info.guest_address;
    const auto image_end = image.info.guest_address + image.info.guest_size;
    if (image.head_hash != 0) {
        const auto head_end = tracker.GetNextPageAddr(image_begin);
        if (HashBackingMemory(image_begin, std::min(head_end, image_end) - image_begin) !=
            image.head_hash) {
            image.flags |= ImageFlagBits::CpuDirty;
            g_vfy.edge_dirty.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (image.tail_hash != 0) {
        const auto tail_begin = std::max<VAddr>(tracker.GetPageAddr(image_end), image_begin);
        if (HashBackingMemory(tail_begin, image_end - tail_begin) != image.tail_hash) {
            image.flags |= ImageFlagBits::CpuDirty;
            g_vfy.edge_dirty.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void TextureCache::GarbageCollectImages() {
    const auto t0 = std::chrono::steady_clock::now();
    SCOPE_EXIT {
        BbStats::gc_ns_total.fetch_add(u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                               std::chrono::steady_clock::now() - t0)
                                               .count()),
                                       std::memory_order_relaxed);
    };
    const u32 frame = BbStats::frame_number.load(std::memory_order_relaxed);
    gc_frame_ticks.Note(frame, gc_tick);
    const u64 second = u64(std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count());
    if (second != gc_second) {
        gc_second = second;
        // The driver's budget, once a second: near it VRAM is short (as NoteVramShort), and the
        // collector goes on at its hard rule for a moment.
        if (instance.CanReportMemoryUsage() && !instance.IsIntegrated()) {
            const u64 budget = instance.GetDeviceMemoryBudgetNow();
            if (budget != 0 && instance.GetDeviceMemoryUsage() >= budget / 100 * 97 &&
                vram_short_until.load(std::memory_order_relaxed) < second + 2) {
                vram_short_until.store(second + 2, std::memory_order_relaxed);
            }
        }
        BbStats::coarse_second.store(u32(second), std::memory_order_relaxed);
        gc_tick_at_second[second % gc_tick_at_second.size()] = gc_tick;
    }
    // bbport: the usage once a frame (and every 64 submissions while no frame is shown): two
    // driver queries at each of the tens of submissions a frame cost more than they told. In
    // between, images registered and unregistered move it by their size.
    if (instance.CanReportMemoryUsage() &&
        (frame != gc_measured_frame || gc_tick - gc_measured_tick >= 64)) {
        gc_measured_frame = frame;
        gc_measured_tick = gc_tick;
        // BB_GC_BUDGET_MB=N: the marks from a fixed budget on any GPU (tests: the pressure of a
        // smaller card on a desktop one).
        static const u64 forced_budget = [] {
            const char* env = std::getenv("BB_GC_BUDGET_MB");
            const u64 mib = env ? std::strtoull(env, nullptr, 10) : 0;
            if (mib != 0) {
                std::printf("Texture cache: collector budget %llu MiB (BB_GC_BUDGET_MB)\n",
                            (unsigned long long)mib);
            }
            return mib << 20;
        }();
        const u64 forced = forced_budget;
        const u64 usage = instance.GetDeviceMemoryUsage();
        const u64 driver_budget = forced ? forced : instance.GetDeviceMemoryBudgetNow();
        const u64 local_mem = instance.GetDeviceLocalMemory();
        if (driver_budget != 0 || local_mem != 0) {
            // Cards up to 4 GB: from the VRAM heap as before (their budget leaves out what the
            // system holds already; 0.6 tuning). On integrated GPUs the usage covers system-memory
            // heaps holding much more than images: the driver's current budget (Steam Deck).
            const bool is_low_vram =
                (local_mem > 0 && local_mem <= 4096ULL * 1024 * 1024) ||
                (driver_budget > 0 && driver_budget <= 4096ULL * 1024 * 1024);
            const u64 base =
                !forced && is_low_vram && local_mem > 0 && !instance.IsIntegrated()
                    ? local_mem
                    : driver_budget;
            gc_marks = GcPolicy::MarksFor(GcPolicy::BudgetFor(
                base, forced || instance.IsIntegrated() ? 0 : local_mem));
            // BB_GC_TRIGGER / BB_GC_PRESSURE / BB_GC_CRITICAL (percent of the budget): the
            // trigger, soft and hard marks fixed instead (tests).
            static const auto percent = [](const char* name) -> u64 {
                const char* env = std::getenv(name);
                return env && *env ? std::strtoull(env, nullptr, 10) : 0;
            };
            static const u64 trig_pct = percent("BB_GC_TRIGGER");
            static const u64 soft_pct = percent("BB_GC_PRESSURE");
            static const u64 hard_pct = percent("BB_GC_CRITICAL");
            if (trig_pct != 0) {
                gc_marks.trigger = gc_marks.budget / 100 * trig_pct;
            }
            if (soft_pct != 0) {
                gc_marks.soft = gc_marks.budget / 100 * soft_pct;
            }
            if (hard_pct != 0) {
                gc_marks.hard = gc_marks.budget / 100 * hard_pct;
            }
            trigger_gc_memory = gc_marks.trigger;
            pressure_gc_memory = gc_marks.soft;
            critical_gc_memory = gc_marks.hard;
        }
        total_used_memory = GcPolicy::EffectiveUsage(
            usage, instance.GetDeviceBlockSlack(),
            gc_pending_free_bytes.load(std::memory_order_relaxed));
    } else if (!instance.CanReportMemoryUsage() && gc_marks.budget == 0) {
        // No usage from the driver: the image bytes against the fixed marks, as before.
        gc_marks.soft = pressure_gc_memory;
        gc_marks.hard = critical_gc_memory;
        gc_marks.budget = critical_gc_memory + critical_gc_memory / 16;
    }
    BbStats::gc_used_bytes.store(total_used_memory, std::memory_order_relaxed);
    BbStats::gc_trigger_bytes.store(gc_marks.trigger, std::memory_order_relaxed);
    BbStats::gc_pressure_bytes.store(gc_marks.soft, std::memory_order_relaxed);
    BbStats::gc_critical_bytes.store(gc_marks.hard, std::memory_order_relaxed);
    if (BbToggle::Disabled(BbToggle::TextureCollector)) {
        return;
    }
    GcPolicy::Level level = GcPolicy::LevelFor(total_used_memory, gc_marks);
    // VRAM ran out lately (a guest memory copy found none, or the usage near the driver's
    // budget): at least the hard rule for a while.
    if (level >= GcPolicy::Level::Soft && level < GcPolicy::Level::Hard && VramShort()) {
        level = GcPolicy::Level::Hard;
    }
    BbStats::gc_level.store(u32(level), std::memory_order_relaxed);
    if (level == GcPolicy::Level::None) {
        return;
    }
    static const u32 idle_seconds = [] {
        const char* env = std::getenv("BB_GC_IDLE_SECONDS");
        return u32(std::clamp<u64>(env ? std::strtoull(env, nullptr, 10) : 20, 1, 63));
    }();
    static const u32 pressure_seconds = [] {
        const char* env = std::getenv("BB_GC_PRESSURE_IDLE_SECONDS");
        return u32(std::clamp<u64>(env ? std::strtoull(env, nullptr, 10) : 5, 1, 63));
    }();
    const GcPolicy::Rule rule = GcPolicy::RuleFor(level, idle_seconds, pressure_seconds);
    gc_quota.Begin(frame, gc_tick);
    if (!gc_quota.Left(rule)) {
        return;
    }
    // Unused for the rule's seconds (the tick at the start of that second; 0 while the game has
    // not run that long: nothing that old) and not used in its last frames, whichever is older.
    const auto tick_seconds_ago = [&](u64 seconds) {
        return seconds == 0 ? gc_tick
                            : gc_tick_at_second[(second - seconds) % gc_tick_at_second.size()];
    };
    const u64 below_tick = std::min(tick_seconds_ago(rule.idle_seconds),
                                    gc_frame_ticks.TickFramesAgo(rule.protect_frames));
    if (below_tick == 0) {
        return;
    }
    // Last used a minute ago or more (lru_touched_tick: real use, not the collector's Touch).
    const u64 minute_tick = tick_seconds_ago(60);

    std::scoped_lock lock{mutex};
    u32 visited = 0;
    lru_cache.ForEachItemBelow(below_tick, [&](ImageId image_id) {
        // Images that cannot go now (bound, GPU-written) are passed over but still count here.
        if (!gc_quota.Left(rule) || ++visited > 512) {
            return true;
        }
        auto& image = slot_images[image_id];
        // Bound to the draw being recorded, a render/depth target of it, or a presented surface:
        // never evicted (from Mrsuss60/bloodborne_pc_windows_port 89f05218 by Mrsuss60). Not
        // marked as used (lru_touched_tick): only real use counts.
        if (image.binding.is_bound || image.binding.is_target || image.usage.vo_surface) {
            lru_cache.Touch(image.lru_id, gc_tick);
            return false;
        }
        // An edge write means the guest reused the memory since: nothing of the GPU's to keep.
        if (GcVerify()) {
            MarkDirtyIfEdgesModified(image);
        }
        const bool download = image.SafeToDownload();
        // Depth/stencil targets and shadow maps are GPU-only and never read back by the CPU: they
        // go without a write-back (from Mrsuss60/bloodborne_pc_windows_port PR #89 by ALuaNegra).
        if (download && !image.info.props.is_depth) {
            // GPU-written: their contents exist only here. A write-back waits for the GPU (the
            // frame stalls), and a tiled one cannot be written back at all: they stay. Only past
            // the budget itself does one linear image a frame go, written back, and a tiled one
            // unused for a minute, unwritten (render targets of an area left behind; out of VRAM
            // the game would crash instead, from Supermedo/bloodborne_pc 44e7644).
            const bool tiled = image.info.IsTiled();
            const bool drop =
                tiled && rule.drop_old_tiled && image.lru_touched_tick < minute_tick;
            const bool write_back = !tiled && rule.write_back && gc_quota.write_backs == 0;
            // d81d63be (shadPS4 GC part 2): a write-back only of what reads back as the guest's
            // data (single sample, no stencil, compatible format) and that the buffer cache did
            // not write over since; else the image stays.
            bool verified_drop = false;
            if (write_back && GcVerify()) {
                if (!image.CanBeDownloaded()) {
                    g_vfy.gated.fetch_add(1, std::memory_order_relaxed);
                    lru_cache.Touch(image.lru_id, gc_tick);
                    ++gc_kept_gpu;
                    BbStats::gc_kept_images.fetch_add(1, std::memory_order_relaxed);
                    return false;
                }
                if (True(image.flags & ImageFlagBits::MaybeGpuDirty)) {
                    if (image.gpu_hash == 0) {
                        // Written over by a buffer too big to hash: cannot tell whose data is newer.
                        g_vfy.unverifiable.fetch_add(1, std::memory_order_relaxed);
                        lru_cache.Touch(image.lru_id, gc_tick);
                        ++gc_kept_gpu;
                        BbStats::gc_kept_images.fetch_add(1, std::memory_order_relaxed);
                        return false;
                    }
                    // The buffer cache's GPU-written bytes over the image go to guest memory first;
                    // if they changed it the image's contents are older than the buffer's.
                    buffer_cache.ReadMemory(image.info.guest_address, image.info.guest_size,
                                            false, true);
                    verified_drop = HashBackingMemory(image.info.guest_address,
                                                      image.info.guest_size) != image.gpu_hash;
                    g_vfy.overlap_checked.fetch_add(1, std::memory_order_relaxed);
                    if (verified_drop) {
                        g_vfy.overlap_freed.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
            if (drop || verified_drop) {
                ++gc_dropped;
            } else {
                // Synchronously, while the image still protects its pages. A deferred write-back
                // landed after FreeImage had unprotected them, over whatever the game had mapped
                // there since (0x28ce9b5).
                if (GcVerify()) {
                    // The GPU-written bytes of the pages the image shares with others.
                    g_vfy.edge_flushes.fetch_add(buffer_cache.ReadEdgeImagePages(image),
                                                 std::memory_order_relaxed);
                    g_vfy.verified_write_backs.fetch_add(1, std::memory_order_relaxed);
                }
                DownloadImageMemory(image_id, true);
                if (GcVerify()) {
                    // The image's data is the newest in the range now: older buffer-cache copies of
                    // it must not be read back over it later.
                    buffer_cache.UnmarkRegionAsGpuModified(image.info.guest_address,
                                                           image.info.guest_size);
                }
                ++gc_downloads;
                ++gc_quota.write_backs;
                BbStats::gc_write_backs_total.fetch_add(1, std::memory_order_relaxed);
            }
        }
        const u64 bytes = image.backing ? image.GetHostImageSize() : 0;
        gc_quota.Take(bytes);
        ++gc_evictions;
        gc_freed_bytes += bytes;
        BbStats::gc_freed_images.fetch_add(1, std::memory_order_relaxed);
        BbStats::gc_freed_total.fetch_add(1, std::memory_order_relaxed);
        BbStats::gc_freed_bytes_total.fetch_add(bytes, std::memory_order_relaxed);
        FreeImage(image_id);
        // Below the mark this rule started at: the rest waits for the next pass and its rule.
        return level >= GcPolicy::Level::Soft &&
               GcPolicy::LevelFor(total_used_memory, gc_marks) < level;
    });
    // bbport: evictions under memory pressure, at most every 5 s (BB_FRAME_STATS or not).
    if (level >= GcPolicy::Level::Soft || gc_downloads != 0) {
        const auto now = std::chrono::steady_clock::now();
        if (now - gc_report_time >= std::chrono::seconds(5)) {
            std::printf("Texture cache: memory pressure, %llu of %llu MiB budget (gradual from "
                        "%llu, hard %llu): %llu images evicted (%llu MiB), %llu written back, "
                        "%llu old GPU-written tiled dropped, %llu GPU-written kept since the last "
                        "report\n",
                        (unsigned long long)(total_used_memory >> 20),
                        (unsigned long long)(gc_marks.budget >> 20),
                        (unsigned long long)(gc_marks.soft >> 20),
                        (unsigned long long)(gc_marks.hard >> 20),
                        (unsigned long long)gc_evictions,
                        (unsigned long long)(gc_freed_bytes >> 20),
                        (unsigned long long)gc_downloads, (unsigned long long)gc_dropped,
                        (unsigned long long)gc_kept_gpu);
            std::printf("Texture cache: verify (BB_GC_VERIFY) totals: %llu marked maybe-GPU-dirty, "
                        "%llu write-backs refused (not downloadable), %llu kept (no hash), %llu "
                        "buffer-overlap checks (%llu freed without write-back), %llu verified "
                        "write-backs (%llu edge-page flushes), %llu edge-page rewrites seen\n",
                        (unsigned long long)g_vfy.marked.load(),
                        (unsigned long long)g_vfy.gated.load(),
                        (unsigned long long)g_vfy.unverifiable.load(),
                        (unsigned long long)g_vfy.overlap_checked.load(),
                        (unsigned long long)g_vfy.overlap_freed.load(),
                        (unsigned long long)g_vfy.verified_write_backs.load(),
                        (unsigned long long)g_vfy.edge_flushes.load(),
                        (unsigned long long)g_vfy.edge_dirty.load());
            gc_report_time = now;
            gc_evictions = gc_downloads = gc_dropped = gc_kept_gpu = gc_freed_bytes = 0;
        }
    }
}

void TextureCache::GarbageCollectSamplers() {
    total_used_samplers = samplers.size();
    if (total_used_samplers < trigger_gc_samplers) {
        return;
    }
    std::scoped_lock lock{samplers_mutex};
    bool pressured = false;
    bool aggresive = false;
    u64 ticks_to_destroy = 0;
    size_t num_deletions = 0;

    const auto configure = [&](bool allow_aggressive) {
        pressured = total_used_samplers >= pressure_gc_samplers;
        aggresive = allow_aggressive && total_used_samplers >= critical_gc_samplers;
        ticks_to_destroy = aggresive ? 160 : pressured ? 80 : 16;
        ticks_to_destroy = std::min(ticks_to_destroy, gc_tick);
        num_deletions = aggresive ? 40 : pressured ? 20 : 10;
    };
    const auto clean_up = [&](u64 hash) {
        if (num_deletions == 0) {
            return true;
        }
        --num_deletions;
        const size_t lru_id = samplers.at(hash).lru_id;
        samplers.erase(hash);
        sampler_lru_cache.Free(lru_id);
        return false;
    };

    // Try to remove anything old enough and not high priority.
    configure(false);
    sampler_lru_cache.ForEachItemBelow(gc_tick - ticks_to_destroy, clean_up);

    if (total_used_samplers >= critical_gc_samplers) {
        // If we are still over the critical limit, run an aggressive GC
        configure(true);
        sampler_lru_cache.ForEachItemBelow(gc_tick - ticks_to_destroy, clean_up);
    }
}

void TextureCache::NoteVramShort() {
    const u64 now = u64(std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count());
    vram_short_until.store(now + 30, std::memory_order_relaxed);
}

bool TextureCache::VramShort() {
    const u64 now = u64(std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count());
    return now < vram_short_until.load(std::memory_order_relaxed);
}

void TextureCache::RunGarbageCollector() {
    SCOPE_EXIT {
        ++gc_tick;
    };

    GarbageCollectImages();
    GarbageCollectSamplers();
}

void TextureCache::TouchImage(Image& image) {
    if (image.lru_touched_tick == gc_tick) {
        return;
    }
    image.lru_touched_tick = gc_tick;
    lru_cache.Touch(image.lru_id, gc_tick);
    // In use again: its contents are what the game sees.
    image.flags &= ~ImageFlagBits::MaybeGpuDirty;
    image.gpu_hash = 0;
}

ImageId TextureCache::InsertImage(const ImageInfo& info) {
    const ImageId image_id = slot_images.insert(instance, runtime, slot_image_views, info);
    if (!slot_images[image_id].allocation_failed) [[likely]] {
        return image_id;
    }
    // bbport: the GPU has no memory left for it, past the driver's budget too (4 GB cards, on
    // entering a big area or the Hunter's Dream). Unused textures go at once, more each round,
    // before the image is given up on: a moment's stutter instead of the game ending (from
    // Supermedo/bloodborne_pc 5f56ed6a).
    u64 freed = 0;
    const bool created = ReclaimAndRetry(
        u64(info.guest_size) * 2,
        [&](u64 want) { return ReclaimMemoryFor(info.guest_address, info.guest_size, want); },
        [&] { return slot_images[image_id].RetryAllocation(); }, freed);
    if (created) {
        static int reported = 0;
        if (reported < 8) {
            ++reported;
            std::printf("GPU: video memory full: %llu MiB of unused textures freed for a new "
                        "%ux%u image\n",
                        (unsigned long long)(freed >> 20), info.size.width, info.size.height);
            std::fflush(stdout);
        }
        return image_id;
    }
    std::printf("GPU: video memory full: no memory for a %ux%u image after freeing %llu MiB of "
                "unused textures\n",
                info.size.width, info.size.height, (unsigned long long)(freed >> 20));
    std::fflush(stdout);
    UNREACHABLE_MSG("Failed allocating image with error {}",
                    vk::to_string(vk::Result::eErrorOutOfDeviceMemory));
    return image_id;
}

u64 TextureCache::ReclaimMemoryFor(VAddr addr, size_t size, u64 bytes) {
    // Unused for 2 s: in no command buffer still being recorded, and after WaitSubmittedWork in
    // none the GPU is still running, so destroying it at once is safe. The collector's rules
    // hold: images bound to the draw being recorded, its render/depth targets, presented
    // surfaces and scene proxies stay, and so do images holding GPU results (render targets),
    // as writing them back here would stall in the middle of a draw. Textures are uploaded
    // again from guest memory when next used.
    const u64 second = u64(std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count());
    const u64 unused_tick =
        std::min(gc_tick, gc_tick_at_second[(second - 2) % gc_tick_at_second.size()]);
    boost::container::small_vector<ImageId, 64> victims;
    u64 freed = 0;
    lru_cache.ForEachItemBelow(unused_tick, [&](ImageId image_id) {
        if (freed >= bytes) {
            return true;
        }
        const Image& image = slot_images[image_id];
        // No backing (an undefined format): no GPU memory to give back.
        if (!image.backing || image.lru_touched_tick >= unused_tick || image.binding.is_bound ||
            image.binding.is_target || image.usage.vo_surface || image.scene_proxy ||
            True(image.flags & ImageFlagBits::GpuModified) || image.allocation_failed ||
            image.Overlaps(addr, size)) {
            return false;
        }
        victims.push_back(image_id);
        freed += image.GetHostImageSize();
        return false;
    });
    if (victims.empty()) {
        return 0;
    }
    scheduler.WaitSubmittedWork();
    for (const ImageId image_id : victims) {
        UntrackImage(image_id);
        UnregisterImage(image_id);
        DeleteImage(image_id, true);
        ++gc_evictions;
        BbStats::gc_freed_images.fetch_add(1, std::memory_order_relaxed);
        BbStats::gc_freed_total.fetch_add(1, std::memory_order_relaxed);
    }
    static int reported = 0;
    if (reported < 32) {
        ++reported;
        std::printf("GPU: video memory full: a round of up to %llu MiB freed %llu MiB (%zu "
                    "unused textures)\n",
                    (unsigned long long)(bytes >> 20), (unsigned long long)(freed >> 20),
                    victims.size());
        std::fflush(stdout);
    }
    return freed;
}

void TextureCache::DeleteImage(ImageId image_id, bool now) {
    Image& image = slot_images[image_id];
    ASSERT_MSG(!image.IsTracked(), "Image was not untracked");
    ASSERT_MSG(False(image.flags & ImageFlagBits::Registered), "Image was not unregistered");

    // Remove any registered meta areas.
    const auto& meta_info = image.info.meta_info;
    if (meta_info.cmask_addr) {
        surface_metas.erase(meta_info.cmask_addr);
    }
    if (meta_info.fmask_addr) {
        surface_metas.erase(meta_info.fmask_addr);
    }
    if (meta_info.htile_addr) {
        surface_metas.erase(meta_info.htile_addr);
    }

    {
        std::unique_lock lk{download_images_mutex};
        if (download_images.contains(image_id)) {
            download_images.erase(image_id);
        }
    }

    // Reclaim image and any image views it references.
    if (now) {
        EraseImage(image_id);
        return;
    }
    // bbport: its memory counts as free for the collector until it is destroyed (a frame later).
    u64 bytes = 0;
    for (const auto& backing : image.backing_images) {
        bytes += backing.image.size_bytes;
    }
    gc_pending_free_bytes.fetch_add(bytes, std::memory_order_relaxed);
    scheduler.DeferOperation([this, image_id, bytes] {
        EraseImage(image_id);
        gc_pending_free_bytes.fetch_sub(bytes, std::memory_order_relaxed);
    });
}

void TextureCache::EraseImage(ImageId image_id) {
    Image& image = slot_images[image_id];
    for (auto& backing : image.backing_images) {
        for (const ImageViewId image_view_id : backing.image_view_ids) {
            slot_image_views.erase(image_view_id);
        }
    }
    slot_images.erase(image_id);
}

} // namespace VideoCore

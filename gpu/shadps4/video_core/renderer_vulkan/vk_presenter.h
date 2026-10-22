// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>

#include <condition_variable>
#include <mutex>

#include "core/libraries/videoout/buffer.h"
#include "video_core/renderer_vulkan/host_passes/fsr_pass.h"
#include "video_core/renderer_vulkan/host_passes/hdr_pass.h"
#include "video_core/renderer_vulkan/host_passes/pp_pass.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/renderer_vulkan/fg_timing.h"
#include "video_core/renderer_vulkan/vk_frame_generation.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Frontend {
class WindowSDL;
}

namespace AmdGpu {
struct Liverpool;
}

namespace Vulkan {

struct FgFrameResources;

struct Frame {
    u32 width;
    u32 height;
    VmaAllocation allocation;
    vk::Image image;
    vk::ImageView image_view;
    vk::Fence present_done;
    vk::Semaphore ready_semaphore;
    u64 ready_tick;
    bool is_hdr{false};
    u8 id{};
    /// bbport: frame generation's images of this frame (generated frames, fitted for the
    /// swapchain) and the display buffer it shows (from Supermedo/bloodborne_pc PR #69 by FIocker).
    std::shared_ptr<FgFrameResources> generation;
    VAddr guest_address{};
    /// bbport: NVIDIA Reflex: the simulation frame of this frame's commands, and of the guest's
    /// flip request (CPU latency diagnostics); 0 without Reflex.
    u64 reflex_frame_id{};
    u64 content_frame_id{};
};

enum SchedulerType {
    Draw,
    Present,
    CpuFlip,
};

class Rasterizer;

class Presenter {
public:
    Presenter(Frontend::WindowSDL& window, AmdGpu::Liverpool* liverpool);
    ~Presenter();

    HostPasses::PostProcessingPass::Settings& GetPPSettingsRef() {
        return pp_settings;
    }

    HostPasses::FsrPass::Settings& GetFsrSettingsRef() {
        return fsr_settings;
    }

    Frontend::WindowSDL& GetWindow() const {
        return window;
    }

    Rasterizer& GetRasterizer() const {
        return *rasterizer.get();
    }

    bool IsHDRSupported() const {
        return swapchain.HasHDR();
    }

    void SetHDR(bool enable) {
        if (!IsHDRSupported()) {
            return;
        }
        swapchain.SetHDR(enable);
        pp_settings.hdr = enable ? 1 : 0;
    }

    VideoCore::Image& RegisterVideoOutSurface(
        const Libraries::VideoOut::BufferAttributeGroup& attribute, VAddr cpu_address) {
        vo_buffers_addr.emplace_back(cpu_address);
        auto desc = VideoCore::TextureCache::ImageDesc{attribute, cpu_address};
        const auto image_id = texture_cache.FindImage(desc);
        auto& image = texture_cache.GetImage(image_id);
        image.usage.vo_surface = 1u;
        return image;
    }

    bool IsVideoOutSurface(const AmdGpu::ColorBuffer& color_buffer) const;

    Frame* PrepareFrame(const Libraries::VideoOut::BufferAttributeGroup& attribute,
                        VAddr cpu_address);

    Frame* PrepareBlankFrame(bool present_thread);

    void Present(Frame* frame, bool is_reusing_frame = false, bool is_game_frame = true);
    /// bbport: waits until the GPU has finished the commands that render `frame` (any thread).
    void WaitRendered(const Frame* frame);
    Frame* PrepareLastFrame();

private:
    Frame* GetRenderFrame();

    void RecreateFrame(Frame* frame, u32 width, u32 height);

    void SetExpectedGameSize(s32 width, s32 height);

    /// bbport: the startup shader precompile (PipelineCache::WarmUp on a worker thread) with a
    /// progress screen this thread presents; Esc aborts it and exits.
    void RunShaderPrecompile();
    /// bbport: one frame of the precompile screen (cleared swapchain image + overlay).
    void PresentPrecompileFrame();

    /// bbport: frame generation (from Supermedo/bloodborne_pc PR #69 by FIocker). Records the
    /// generated frames of `frame` from the display buffer's guides (GPU command thread).
    void PrepareGeneration(Frame* frame, vk::Image display_image, u32 width, u32 height,
                           vk::Format sample_format, vk::Format source_format);
    /// One swapchain present of `image` (the frame's own or a generated one); `real`: the game's
    /// frame (menu polling, dumps). `deadline` (steady clock ns, 0: none): presented no earlier.
    bool PresentOne(Frame* frame, vk::Image image, vk::ImageView view, bool real,
                    std::int64_t deadline = 0);
    /// bbport: Swapchain::Present under Scheduler::submit_mutex (a recreation after it locks itself).
    bool PresentLocked();
    /// bbport: native HDR output (I4): the swapchain image drawn whole by the HDR compose pass
    /// from `view` (the SDR frame in General, or null: `background` only) and the overlay.
    void RecordHdrPresent(vk::CommandBuffer cmdbuf, Frame* frame, vk::Image image,
                          vk::ImageView view, bool real, float background, bool precompile);
    void CountPresentation(bool real);

private:
    float expected_ratio{1920.0 / 1080.0f};
    u32 expected_frame_width{1920};
    u32 expected_frame_height{1080};

    Frontend::WindowSDL& window;
    Instance instance;
    HostPasses::FsrPass fsr_pass;
    HostPasses::FsrPass::Settings fsr_settings{};
    HostPasses::PostProcessingPass::Settings pp_settings{};
    HostPasses::PostProcessingPass pp_pass;
    /// bbport: native HDR output (I4): the compose pass, and the format of the presentation
    /// frames (the SDR picture: the swapchain's format, or 10-bit UNORM under HDR output).
    HostPasses::HdrComposePass hdr_pass;
    vk::Format frame_format{};
    float hdr_scene_scale = 1.0f; ///< the highlights' fade while the game's menus are up
    std::mutex passes_mutex; ///< bbport: the passes are recorded on the recording threads
    AmdGpu::Liverpool* liverpool;
    Scheduler draw_scheduler;
    std::deque<u64> recent_frame_ticks; ///< bbport: BB_FRAMES_AHEAD bound (PrepareFrame)
    Scheduler present_scheduler;
    Scheduler flip_scheduler;
    Swapchain swapchain;
    Runtime runtime;
    std::unique_ptr<Rasterizer> rasterizer;
    VideoCore::TextureCache& texture_cache;
    vk::UniqueCommandPool command_pool;
    std::vector<Frame> present_frames;
    std::queue<Frame*> free_queue;
    Frame* last_submit_frame;
    Frame* held_frame = nullptr;  // bbport: the frame a present hold shows (bbport_present_hold.h)
    std::mutex free_mutex;
    std::condition_variable free_cv;
    std::condition_variable_any frame_cv;
    std::vector<VAddr> vo_buffers_addr;
    // Frame generation: null unless chosen for this start and its provider is available.
    std::unique_ptr<FrameGeneration> generation;
    std::mutex generation_mutex;
    FgHistory generation_history;
    FgSchedule generation_schedule{true, true};
    vk::UniqueDescriptorSetLayout hud_desc;
    vk::UniquePipelineLayout hud_layout;
    vk::UniquePipeline hud_pipeline;
    int generation_factor{}, generation_backend{};
    u64 presentation_count{}, source_count{}, generated_count{}, skipped_count{};
    std::int64_t last_ready_ns{};
    u64 last_reflex_report{};
    u64 last_ready_serial{};
    std::chrono::steady_clock::time_point counter_start{std::chrono::steady_clock::now()};
};

} // namespace Vulkan

// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "bbport_headless.h"
#include "bbport_present_hold.h"
#include "common/debug.h"
#include "common/elf_info.h"
#include "common/io_file.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "sdl_window.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderdoc.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "bbport_game_menu.h"
#include <thread>
#include <SDL3/SDL_keyboard.h>
#include "bbport_compile_progress.h"
#include "bbport_frame_state.h"
#include "bbport_overlay.h"
#include "common/thread.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "bbport_timeline.h"
#include "video_core/renderer_vulkan/vk_temporal_upscaler.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/texture_cache/image.h"
#include "bbport_settings.h"
#include "bbport_hdr.h"
#include "party/party_hud.h"
#include "video_core/renderer_vulkan/vk_dlss.h"
#include "video_core/renderer_vulkan/vk_fg_images.h"
#include "video_core/renderer_vulkan/vk_reflex.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/host_shaders/fg_hud_comp.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csetjmp>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <system_error>
#include <vector>
#include <vk_mem_alloc.h>

[[noreturn]] void BbHardExit(int code); // gpu/shim/bbgpu.cpp

namespace Vulkan {

// bbport: frame generation's per-frame images (from Supermedo/bloodborne_pc PR #69 by FIocker):
// the guides copied for this frame, the generated frames (output size) and the same fitted to
// the frame size as the swapchain blit takes it. Reused only after the frame's presents finish.
struct FgFrameResources {
    FgImage color, full, depth, motion, ui;
    std::array<FgImage, 3> outputs, presented;
    std::array<vk::ImageView, 3> srgb_views{};
    vk::Device device{};
    vk::QueryPool timestamps{};
    u64 serial{};
    u32 count{};
    float frame_ms{16.6667f};
    bool timestamp_valid{};
    vk::Format sample_format{};
    ~FgFrameResources() {
        for (auto view : srgb_views) {
            if (view) device.destroyImageView(view);
        }
        if (timestamps) device.destroyQueryPool(timestamps);
    }
};

static FrameGeneration::Resource FgResource(const FgImage& image) {
    return {image.image, image.view, image.format, vk::ImageAspectFlagBits::eColor, image.width,
            image.height};
}

static std::int64_t FgNow() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool CanBlitToSwapchain(const vk::PhysicalDevice physical_device, vk::Format format) {
    const vk::FormatProperties props{physical_device.getFormatProperties(format)};
    return static_cast<bool>(props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eBlitDst);
}

[[nodiscard]] vk::ImageSubresourceLayers MakeImageSubresourceLayers() {
    return vk::ImageSubresourceLayers{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .mipLevel = 0,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
}

[[nodiscard]] vk::ImageBlit MakeImageBlit(s32 frame_width, s32 frame_height, s32 dst_width,
                                          s32 dst_height, s32 offset_x, s32 offset_y) {
    return vk::ImageBlit{
        .srcSubresource = MakeImageSubresourceLayers(),
        .srcOffsets =
            std::array{
                vk::Offset3D{
                    .x = 0,
                    .y = 0,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = frame_width,
                    .y = frame_height,
                    .z = 1,
                },
            },
        .dstSubresource = MakeImageSubresourceLayers(),
        .dstOffsets =
            std::array{
                vk::Offset3D{
                    .x = offset_x,
                    .y = offset_y,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = offset_x + dst_width,
                    .y = offset_y + dst_height,
                    .z = 1,
                },
            },
    };
}

[[nodiscard]] vk::ImageBlit MakeImageBlitStretch(s32 frame_width, s32 frame_height,
                                                 s32 swapchain_width, s32 swapchain_height) {
    return MakeImageBlit(frame_width, frame_height, swapchain_width, swapchain_height, 0, 0);
}

static vk::Rect2D FitImage(s32 frame_width, s32 frame_height, s32 swapchain_width,
                           s32 swapchain_height) {
    float frame_aspect = static_cast<float>(frame_width) / frame_height;
    float swapchain_aspect = static_cast<float>(swapchain_width) / swapchain_height;

    u32 dst_width = swapchain_width;
    u32 dst_height = swapchain_height;

    if (frame_aspect > swapchain_aspect) {
        dst_height = static_cast<s32>(swapchain_width / frame_aspect);
    } else {
        dst_width = static_cast<s32>(swapchain_height * frame_aspect);
    }

    const s32 offset_x = (swapchain_width - dst_width) / 2;
    const s32 offset_y = (swapchain_height - dst_height) / 2;

    return vk::Rect2D{{offset_x, offset_y}, {dst_width, dst_height}};
}

[[nodiscard]] vk::ImageBlit MakeImageBlitFit(s32 frame_width, s32 frame_height, s32 swapchain_width,
                                             s32 swapchain_height) {
    const auto& dst_rect = FitImage(frame_width, frame_height, swapchain_width, swapchain_height);

    return MakeImageBlit(frame_width, frame_height, dst_rect.extent.width, dst_rect.extent.height,
                         dst_rect.offset.x, dst_rect.offset.y);
}

// bbport: screenshot capture and ImGui overlays removed; the port presents directly.

Presenter::Presenter(Frontend::WindowSDL& window_, AmdGpu::Liverpool* liverpool_)
    : window{window_}, liverpool{liverpool_},
      instance{window, EmulatorSettings.GetGpuId(), EmulatorSettings.IsVkValidationEnabled(),
               EmulatorSettings.IsVkCrashDiagnosticEnabled()},
      draw_scheduler{instance, true}, present_scheduler{instance}, flip_scheduler{instance},
      swapchain{instance, window}, runtime{instance, draw_scheduler},
      rasterizer{std::make_unique<Rasterizer>(instance, draw_scheduler, runtime, liverpool)},
      texture_cache{rasterizer->GetTextureCache()} {
    const u32 num_images = swapchain.GetImageCount();
    const vk::Device device = instance.GetDevice();

    // Create presentation frames.
    present_frames.resize(num_images);
    for (u32 i = 0; i < num_images; i++) {
        Frame& frame = present_frames[i];
        frame.id = i;
        auto fence = Check<"create present done fence">(
            device.createFence({.flags = vk::FenceCreateFlagBits::eSignaled}));
        frame.present_done = fence;
        free_queue.push(&frame);
    }

    fsr_settings.enable = EmulatorSettings.IsFsrEnabled();
    fsr_settings.use_rcas = EmulatorSettings.IsRcasEnabled();
    fsr_settings.rcas_attenuation =
        static_cast<float>(EmulatorSettings.GetRcasAttenuation() / 1000.f);

    fsr_pass.Create(device, instance.GetAllocator(), num_images);
    // bbport: native HDR output (I4): the frames keep the SDR picture (10-bit) and the HDR
    // compose pass draws the swapchain; the overlay draws into its own RGBA8 image first.
    const bool hdr_output = swapchain.HdrOutput() != BbHdr::Sdr;
    frame_format = hdr_output ? vk::Format::eA2B10G10R10UnormPack32 : swapchain.GetSurfaceFormat().format;
    pp_pass.Create(device, frame_format);
    if (hdr_output) {
        hdr_pass.Create(instance, swapchain.GetSurfaceFormat().format);
    }
    BbOverlay::Init(instance,
                    hdr_output ? HostPasses::HdrComposePass::OverlayFormat
                               : swapchain.GetSurfaceFormat().format,
                    num_images);

    // bbport: NVIDIA Reflex: presentation's submissions are out of band (no-op without it).
    present_scheduler.SetOutOfBand(true);
    flip_scheduler.SetOutOfBand(true);

    // bbport: frame generation (from Supermedo/bloodborne_pc PR #69 by FIocker), when chosen
    // for this start and its provider runs here; otherwise nothing of it exists.
    auto& settings = BbSettings::Get();
    if (settings.fg_active) { // the provider is available (Instance::CreateDevice)
        generation_factor = settings.startup_frame_generation;
        generation_backend = settings.startup_fg_backend;
        if (generation_backend == BbSettings::FgFsr) generation_factor = 2;
        auto candidate = std::make_unique<FrameGeneration>(instance, draw_scheduler);
        const char* name = generation_backend == BbSettings::FgFsr ? "FSR" : "DLSS";
        if (candidate->Available(generation_backend) &&
            candidate->MaxGenerated(generation_backend) > 0) {
            const u32 max_generated = candidate->MaxGenerated(generation_backend);
            if (u32(generation_factor - 1) > max_generated) {
                std::printf("Frame generation: %s %dx requested, the runtime allows %ux\n", name,
                            generation_factor, max_generated + 1);
                generation_factor = int(max_generated) + 1;
            }
            generation = std::move(candidate);
            settings.fg_problem = nullptr;
            std::printf("Frame generation: %s %dx on (presented frames per rendered frame; "
                        "presentation queue %s)\n",
                        name, generation_factor,
                        instance.GetPresentQueue() != instance.GetGraphicsQueue() ? "independent"
                                                                                  : "shared");
            present_scheduler.SetPresentQueue(instance.GetPresentQueue() !=
                                              instance.GetGraphicsQueue());
        } else {
            static std::string problem;
            if (generation_backend == BbSettings::FgFsr) {
                problem = "FSR frame generation needs VK_KHR_compute_shader_derivatives (linear "
                          "groups), which this GPU or driver lacks";
            } else {
                const Dlss* dlss = Dlss::Get();
                problem = dlss ? dlss->FgProblem()
                               : std::string("the DLSS bridge (bbport_dlss.dll) and NVIDIA's "
                                             "libraries are not installed");
            }
            settings.fg_problem = problem.c_str();
            settings.fg_active = false;
            std::printf("Frame generation: %s unavailable (%s); presenting normally\n", name,
                        problem.c_str());
        }
    }

    // bbport: the pipeline cache is compiled before the game starts (BB_SHADER_PRECOMPILE=0:
    // as before, inside the PipelineCache constructor, without a screen).
    if (PipelineCache::PrecompileEnabled()) {
        RunShaderPrecompile();
    }
    BbCompileProgress::StartTestIfRequested();
}

void Presenter::RunShaderPrecompile() {
    auto& cache = rasterizer->GetPipelineCache();
    std::atomic<bool> finished{false};
    std::thread worker([&] {
        Common::SetCurrentThreadName("bb:Precompile");
        cache.WarmUp(true);
        finished.store(true, std::memory_order_release);
    });
    const auto start = std::chrono::steady_clock::now();
    bool aborted = false;
    bool shown = false;
    while (!finished.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
        if (finished.load(std::memory_order_acquire)) {
            break;
        }
        // Esc (the window has focus) stops it; closing the window exits from the window thread.
        const bool* keys = SDL_GetKeyboardState(nullptr);
        if (!aborted && keys && keys[SDL_SCANCODE_ESCAPE]) {
            aborted = true;
            std::printf("Shader precompile: aborted (Esc)\n");
            cache.AbortWarmUp();
        }
        // Fast (warm) starts show nothing.
        if (!aborted && std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(300)) {
            PresentPrecompileFrame();
            shown = true;
        }
    }
    worker.join();
    if (aborted) {
        // What was written so far stays (each file is replaced whole); the next start goes on.
        cache.Sync();
        BbHardExit(0);
    }
    if (shown) {
        // Leave a cleared frame instead of the last progress screen until the game presents.
        PresentPrecompileFrame();
    }
    std::printf("Shader precompile: %.1f s\n",
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
}

void Presenter::RecordHdrPresent(vk::CommandBuffer cmdbuf, Frame* frame, vk::Image image,
                                 vk::ImageView view, bool real, float background, bool precompile) {
    const vk::Extent2D extent = swapchain.GetExtent();
    const vk::Image swapchain_image = swapchain.Image();
    const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    const std::array pre{
        vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .srcAccessMask = vk::AccessFlagBits2::eNone,
            .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .image = swapchain_image,
            .subresourceRange = range,
        },
        // The frame (post process or frame generation's fitted copy) stays in General.
        vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = image,
            .subresourceRange = range,
        },
    };
    cmdbuf.pipelineBarrier2(
        {.imageMemoryBarrierCount = image ? 2u : 1u, .pImageMemoryBarriers = pre.data()});
    if (real) {
        BbGameMenu::Poll();        // the port's pages in the game's System menu
        BbFrameState::OnPresent(); // menu/loading detection (compile indicator)
    }
    // The overlay at display resolution into its own image, composited at paper white.
    bool overlay = false;
    if (FgImage* target = hdr_pass.BeginOverlay(cmdbuf, extent.width, extent.height)) {
        if (precompile) {
            const vk::RenderingAttachmentInfo attachment{
                .imageView = target->view,
                .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .loadOp = vk::AttachmentLoadOp::eClear,
                .storeOp = vk::AttachmentStoreOp::eStore,
            };
            cmdbuf.beginRendering({.renderArea = {{0, 0}, extent},
                                   .layerCount = 1,
                                   .colorAttachmentCount = 1,
                                   .pColorAttachments = &attachment});
            BbOverlay::RenderPrecompile(cmdbuf, extent);
            cmdbuf.endRendering();
            overlay = true;
        } else if (BbOverlay::Visible()) {
            overlay = BbOverlay::Render(cmdbuf, target->view, extent, true);
        }
    }
    auto& settings = BbSettings::Get();
    // Test aid BB_HDR_TEST_FILE: when the file appears (consumed), its tokens change the curve
    // live as the menu would ("pattern" toggles the calibration pattern, peak=N, paper=N,
    // strength=F, saturation=F), for sweeps inside one game session.
    static const char* test_file = std::getenv("BB_HDR_TEST_FILE");
    static u32 test_polls = 0;
    if (test_file && real && ++test_polls % 30 == 0) {
        if (FILE* f = std::fopen(test_file, "r")) {
            char token[64];
            while (std::fscanf(f, "%63s", token) == 1) {
                float value = 0.0f;
                if (!std::strcmp(token, "pattern")) settings.hdr_calibration = !settings.hdr_calibration;
                else if (std::sscanf(token, "peak=%f", &value) == 1) settings.hdr_peak_nits = int(value);
                else if (std::sscanf(token, "paper=%f", &value) == 1) settings.hdr_paper_white = int(value);
                else if (std::sscanf(token, "strength=%f", &value) == 1) settings.hdr_strength = value;
                else if (std::sscanf(token, "saturation=%f", &value) == 1) settings.hdr_saturation = value;
            }
            std::fclose(f);
            std::remove(test_file);
            std::printf("HDR test: pattern %d peak %.0f paper white %d strength %.2f saturation %.2f\n",
                        settings.hdr_calibration ? 1 : 0, BbSettings::HdrPeakNits(),
                        settings.hdr_paper_white.load(), settings.hdr_strength.load(),
                        settings.hdr_saturation.load());
        }
    }
    HostPasses::HdrComposePass::Constants constants{};
    if (frame && view) {
        const vk::Rect2D fit = FitImage(s32(frame->width), s32(frame->height), s32(extent.width),
                                        s32(extent.height));
        constants.fit[0] = float(fit.offset.x);
        constants.fit[1] = float(fit.offset.y);
        constants.fit[2] = float(fit.extent.width);
        constants.fit[3] = float(fit.extent.height);
    }
    constants.pattern[0] = BbHdr::PatternLeft * float(extent.width);
    constants.pattern[1] = BbHdr::PatternTop * float(extent.height);
    constants.pattern[2] = BbHdr::PatternRight * float(extent.width);
    constants.pattern[3] = BbHdr::PatternBottom * float(extent.height);
    constants.peak = BbSettings::HdrPeakNits();
    constants.paper_white = std::clamp(float(settings.hdr_paper_white.load()),
                                       BbHdr::MinPaperWhite, BbHdr::MaxPaperWhite);
    constants.strength = std::clamp(settings.hdr_strength.load(), 0.0f, 1.5f);
    constants.saturation = std::clamp(settings.hdr_saturation.load(), 0.5f, 1.5f);
    constants.encoding = swapchain.HdrOutput();
    constants.flags =
        settings.hdr_calibration && !precompile ? HostPasses::HdrComposePass::FlagPattern : 0;
    constants.background = background;
    // The game's full-screen menus (pause, System, inventory, stats, the lamp's travel list) are
    // white text on a dimmed world: shown at paper white like any HDR game's UI, the highlights
    // fading out and back in over ~0.25 s (BB_HDR_MENU_SDR=0 keeps them expanded).
    static const bool menu_sdr = [] {
        const char* value = std::getenv("BB_HDR_MENU_SDR");
        return !value || value[0] != '0';
    }();
    if (real && menu_sdr) {
        bool game_menu = false, banner = false;
        coop::PerfHudGameState(&game_menu, &banner);
        const auto now = std::chrono::steady_clock::now();
        static auto last = now;
        const float dt = std::clamp(std::chrono::duration<float>(now - last).count(), 0.0f, 0.1f);
        last = now;
        const float target = game_menu ? 0.0f : 1.0f;
        hdr_scene_scale += std::clamp(target - hdr_scene_scale, -dt * 4.0f, dt * 4.0f);
    }
    constants.scene_scale = menu_sdr ? hdr_scene_scale : 1.0f;
    swapchain.SetHdrMetadata(constants.peak, constants.paper_white);
    hdr_pass.Render(cmdbuf, frame ? view : vk::ImageView{}, overlay, swapchain.ImageView(), extent,
                    constants);

    // BB_HDR_DUMP_TRIGGER: the SDR frame and the encoded output, for tools/windows/hdr_check.py.
    static const char* dump_trigger = std::getenv("BB_HDR_DUMP_TRIGGER");
    static u32 dump_polls = 0;
    std::error_code dump_error;
    const bool dump_now = dump_trigger && real && frame && ++dump_polls % 15 == 0 &&
                          std::filesystem::exists(dump_trigger, dump_error);
    vk::ImageMemoryBarrier2 done{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = dump_now ? vk::PipelineStageFlagBits2::eTransfer
                                 : vk::PipelineStageFlagBits2::eBottomOfPipe,
        .dstAccessMask = dump_now ? vk::AccessFlagBits2::eTransferRead : vk::AccessFlagBits2::eNone,
        .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .newLayout = dump_now ? vk::ImageLayout::eGeneral : vk::ImageLayout::ePresentSrcKHR,
        .image = swapchain_image,
        .subresourceRange = range,
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &done});
    if (dump_now) {
        const vk::Format format = swapchain.GetSurfaceFormat().format;
        const char* name = format == vk::Format::eA2B10G10R10UnormPack32   ? "a2b10g10r10"
                           : format == vk::Format::eA2R10G10B10UnormPack32 ? "a2r10g10b10"
                           : format == vk::Format::eR16G16B16A16Unorm      ? "rgba16"
                                                                           : "rgba16f";
        std::printf("HDR dump params: encoding=%d peak=%.1f paper_white=%.1f strength=%.3f "
                    "saturation=%.3f fit=%.0f,%.0f,%.0f,%.0f pattern=%d overlay=%d scene=%.3f\n",
                    constants.encoding, constants.peak, constants.paper_white, constants.strength,
                    constants.saturation, constants.fit[0], constants.fit[1], constants.fit[2],
                    constants.fit[3], constants.flags & HostPasses::HdrComposePass::FlagPattern ? 1 : 0,
                    overlay ? 1 : 0, constants.scene_scale);
        DumpHdrIfDue(instance, present_scheduler, cmdbuf, frame->image, frame->width,
                     frame->height, swapchain_image, extent.width, extent.height, name);
        done.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
        done.srcAccessMask = vk::AccessFlagBits2::eNone;
        done.dstStageMask = vk::PipelineStageFlagBits2::eBottomOfPipe;
        done.dstAccessMask = vk::AccessFlagBits2::eNone;
        done.oldLayout = vk::ImageLayout::eGeneral;
        done.newLayout = vk::ImageLayout::ePresentSrcKHR;
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &done});
    }
}

void Presenter::PresentPrecompileFrame() {
    if (window.GetWidth() == 0 || window.GetHeight() == 0) {
        return;
    }
    if (window.GetWidth() != swapchain.GetWidth() || window.GetHeight() != swapchain.GetHeight() ||
        !swapchain.IsPresentable()) {
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
        if (!swapchain.IsPresentable()) {
            return;
        }
    }
    if (!swapchain.AcquireNextImage()) {
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
        return;
    }
    auto& scheduler = present_scheduler;
    const auto cmdbuf = scheduler.CommandBuffer();
    const vk::Extent2D extent = swapchain.GetExtent();
    if (swapchain.HdrOutput() != BbHdr::Sdr) {
        // The same screen through the HDR compose pass (the clear colour as its background).
        RecordHdrPresent(cmdbuf, nullptr, {}, {}, false, 0.02f, true);
        SubmitInfo info{};
        info.AddWait(swapchain.GetImageAcquiredSemaphore());
        info.AddSignal(swapchain.GetPresentReadySemaphore());
        const u64 tick = scheduler.CurrentTick();
        scheduler.Flush(info);
        if (!PresentLocked()) {
            swapchain.Recreate(window.GetWidth(), window.GetHeight());
        }
        scheduler.Wait(tick);
        return;
    }
    const vk::ImageSubresourceRange color_range{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };
    const vk::ImageMemoryBarrier to_attachment{
        .srcAccessMask = vk::AccessFlagBits::eNone,
        .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = swapchain.Image(),
        .subresourceRange = color_range,
    };
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
                           vk::PipelineStageFlagBits::eColorAttachmentOutput,
                           vk::DependencyFlagBits::eByRegion, {}, {}, to_attachment);
    const vk::RenderingAttachmentInfo attachment{
        .imageView = swapchain.ImageView(),
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue = vk::ClearValue{vk::ClearColorValue{std::array{0.02f, 0.02f, 0.025f, 1.0f}}},
    };
    cmdbuf.beginRendering(vk::RenderingInfo{
        .renderArea = {{0, 0}, extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    });
    BbOverlay::RenderPrecompile(cmdbuf, extent);
    cmdbuf.endRendering();
    const vk::ImageMemoryBarrier to_present{
        .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
        .dstAccessMask = vk::AccessFlagBits::eNone,
        .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .newLayout = vk::ImageLayout::ePresentSrcKHR,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = swapchain.Image(),
        .subresourceRange = color_range,
    };
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
                           vk::PipelineStageFlagBits::eBottomOfPipe,
                           vk::DependencyFlagBits::eByRegion, {}, {}, to_present);
    SubmitInfo info{};
    info.AddWait(swapchain.GetImageAcquiredSemaphore());
    info.AddSignal(swapchain.GetPresentReadySemaphore());
    const u64 tick = scheduler.CurrentTick();
    scheduler.Flush(info);
    if (!PresentLocked()) {
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
    }
    scheduler.Wait(tick);
}

Presenter::~Presenter() {

    draw_scheduler.Finish();
    present_scheduler.Finish();
    flip_scheduler.Finish();
    for (auto& frame : present_frames) {
        frame.generation.reset();
    }
    generation.reset();
    if (rasterizer) {
        rasterizer->GetPipelineCache().Sync();
    }
    if (draw_scheduler.CommandBuffer()) {
        (void)draw_scheduler.CommandBuffer().reset();
    }
    if (present_scheduler.CommandBuffer()) {
        (void)present_scheduler.CommandBuffer().reset();
    }
    if (flip_scheduler.CommandBuffer()) {
        (void)flip_scheduler.CommandBuffer().reset();
    }

    const vk::Device device = instance.GetDevice();
    for (auto& frame : present_frames) {
        vmaDestroyImage(instance.GetAllocator(), frame.image, frame.allocation);
        device.destroyImageView(frame.image_view);
        device.destroyFence(frame.present_done);
    }
}

bool Presenter::IsVideoOutSurface(const AmdGpu::ColorBuffer& color_buffer) const {
    return std::ranges::find(vo_buffers_addr, color_buffer.Address()) != vo_buffers_addr.cend();
}

void Presenter::RecreateFrame(Frame* frame, u32 width, u32 height) {
    const vk::Device device = instance.GetDevice();
    if (frame->image_view) {
        device.destroyImageView(frame->image_view);
    }
    if (frame->image) {
        vmaDestroyImage(instance.GetAllocator(), frame->image, frame->allocation);
    }

    const vk::Format format = frame_format;
    const vk::ImageCreateInfo image_info = {
        .flags = vk::ImageCreateFlagBits::eMutableFormat,
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst |
                 vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled,
    };

    const VmaAllocationCreateInfo alloc_info = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .requiredFlags = 0,
        .preferredFlags = 0,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };

    VkImage unsafe_image{};
    VkImageCreateInfo unsafe_image_info = static_cast<VkImageCreateInfo>(image_info);

    VkResult result = vmaCreateImage(instance.GetAllocator(), &unsafe_image_info, &alloc_info,
                                     &unsafe_image, &frame->allocation, nullptr);
    if (result != VK_SUCCESS) {
        VmaAllocationCreateInfo fallback_alloc = alloc_info;
        fallback_alloc.flags = 0;
        result = vmaCreateImage(instance.GetAllocator(), &unsafe_image_info, &fallback_alloc,
                                &unsafe_image, &frame->allocation, nullptr);
    }
    if (result != VK_SUCCESS) [[unlikely]] {
        LOG_CRITICAL(Render_Vulkan, "Failed allocating frame texture with error {}",
                     vk::to_string(vk::Result{result}));
        VmaAllocationCreateInfo sys_alloc = alloc_info;
        sys_alloc.flags = 0;
        sys_alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        result = vmaCreateImage(instance.GetAllocator(), &unsafe_image_info, &sys_alloc,
                                &unsafe_image, &frame->allocation, nullptr);
        if (result != VK_SUCCESS) {
            UNREACHABLE();
        }
    }
    frame->image = vk::Image{unsafe_image};
    SetObjectName(device, frame->image, "Frame image #{}", frame->id);

    const vk::ImageViewCreateInfo view_info = {
        .image = frame->image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    auto view = Check<"create frame image view">(device.createImageView(view_info));
    frame->image_view = view;
    frame->width = width;
    frame->height = height;

    frame->is_hdr = swapchain.GetHDR();
}

Frame* Presenter::PrepareLastFrame() {
    if (last_submit_frame == nullptr) {
        return nullptr;
    }

    Frame* frame = last_submit_frame;

    while (true) {
        vk::Result result = instance.GetDevice().waitForFences(frame->present_done, false,
                                                               std::numeric_limits<u64>::max());
        if (result == vk::Result::eSuccess) {
            break;
        }
        if (result == vk::Result::eTimeout) {
            continue;
        }
        if (result == vk::Result::eErrorDeviceLost) {
            Breadcrumbs::ReportDeviceLost("waiting for a frame");
        }
        ASSERT_MSG(result != vk::Result::eErrorDeviceLost,
                   "Device lost during waiting for a frame");
    }

    auto& scheduler = flip_scheduler;
    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();

    const auto frame_subresources = vk::ImageSubresourceRange{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };

    const auto pre_barrier =
        vk::ImageMemoryBarrier2{.srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
                                .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
                                .oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                                .newLayout = vk::ImageLayout::eGeneral,
                                .image = frame->image,
                                .subresourceRange{frame_subresources}};

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    // Flush frame creation commands.
    frame->ready_semaphore = scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return frame;
}

static vk::Format GetFrameViewFormat(const Libraries::VideoOut::PixelFormat format) {
    switch (format) {
    case Libraries::VideoOut::PixelFormat::A8B8G8R8Srgb:
        return vk::Format::eR8G8B8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A8R8G8B8Srgb:
        return vk::Format::eB8G8R8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A2R10G10B10:
    case Libraries::VideoOut::PixelFormat::A2R10G10B10Srgb:
    case Libraries::VideoOut::PixelFormat::A2R10G10B10Bt2020Pq:
        return vk::Format::eA2R10G10B10UnormPack32;
    default:
        break;
    }
    UNREACHABLE_MSG("Unknown format={}", static_cast<u32>(format));
    return {};
}

Frame* Presenter::PrepareFrame(const Libraries::VideoOut::BufferAttributeGroup& attribute,
                               VAddr cpu_address) {
    BbTimeline::Note(BbTimeline::PipeTask, 4, draw_scheduler.CurrentTick());
    // bbport: scaled upscaler presets: the output-size display buffer drawn by the port.
    TemporalUpscaler::Display display{};
    const bool upscaled = rasterizer->GetUpscaler().DisplayOverride(cpu_address, display);
    VideoCore::ImageId image_id{};
    if (!upscaled) {
        auto desc = VideoCore::TextureCache::ImageDesc{attribute, cpu_address};
        image_id = texture_cache.FindImage(desc);
        texture_cache.UpdateImage(image_id);
    }

    Frame* frame = GetRenderFrame();
    frame->guest_address = cpu_address;
    if (instance.IsReflexSupported()) {
        frame->reflex_frame_id = Reflex::RecordingFrameId();
        if (!frame->reflex_frame_id) frame->reflex_frame_id = Reflex::CurrentSimFrameId();
    }
    if (frame->generation) {
        frame->generation->count = 0;
        frame->generation->serial = 0;
        frame->generation->timestamp_valid = false;
    }

    const auto frame_subresources = vk::ImageSubresourceRange{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };

    // bbport: recorded like draws, on the recording threads. CommandBuffer() would first wait
    // for them to record everything handed over before (~90 us a frame of the draw recording
    // thread) and then submit from this thread.
    draw_scheduler.EndRendering();

    VideoCore::ImageViewInfo view_info{};
    view_info.format = GetFrameViewFormat(attribute.attrib.pixel_format);
    // Exclude alpha from output frame to avoid blending with UI.
    view_info.mapping.a = vk::ComponentSwizzle::eOne;

    vk::ImageView image_view{};
    vk::Extent2D image_size{};
    if (upscaled) {
        const auto device = instance.GetDevice();
        image_view = Check(device.createImageView({
            .image = display.image,
            .viewType = vk::ImageViewType::e2D,
            .format = view_info.format,
            .components = {vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity,
                           vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eOne},
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
        draw_scheduler.DeferOperation([device, image_view] { device.destroyImageView(image_view); });
        image_size = vk::Extent2D{display.width, display.height};
        // BB_PRESENT_DUMP_TRIGGER: labelled with the format it is shown in, as below.
        if (auto& upscaler = rasterizer->GetUpscaler(); upscaler.PresentDumpDue()) {
            upscaler.DumpPresented(display.image, display.width, display.height, view_info.format);
        }
        draw_scheduler.Record([image = display.image](vk::CommandBuffer cmdbuf) {
            const vk::ImageMemoryBarrier2 to_read{
                .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                .image = image,
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
            };
            cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_read});
        });
    } else {
        auto& image = texture_cache.GetImage(image_id);
        image_view = *image.FindView(view_info).image_view;
        image_size = vk::Extent2D{image.info.size.width, image.info.size.height};
        // bbport: BB_PRESENT_DUMP_TRIGGER for the game's own display buffer too.
        if (auto& upscaler = rasterizer->GetUpscaler(); upscaler.PresentDumpDue()) {
            runtime.Transit(&image, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eTransfer,
                            vk::AccessFlagBits2::eTransferRead);
            runtime.FlushBarriers();
            upscaler.DumpPresented(image.GetImage(), image_size.width, image_size.height,
                                   view_info.format);
        }
        runtime.Transit(&image, vk::ImageLayout::eShaderReadOnlyOptimal,
                        vk::PipelineStageFlagBits2::eFragmentShader,
                        vk::AccessFlagBits2::eShaderRead);
        runtime.FlushBarriers();
    }
    expected_ratio = static_cast<float>(image_size.width) / static_cast<float>(image_size.height);

    // Vulkan has no sRGB variant of the 10-bit format, so an A2R10G10B10Srgb buffer reaches
    // the post process pass still sRGB encoded and has to be decoded there instead.
    pp_settings.srgb_input =
        attribute.attrib.pixel_format == Libraries::VideoOut::PixelFormat::A2R10G10B10Srgb;
    // bbport: Bloodborne is an SDR game, so with HDR on in Windows the desktop decodes the
    // swapchain as sRGB; the pass corrects that to gamma 2.2 (BB_HDR_GAMMA=0 turns it off; from
    // Supermedo/bloodborne_pc PR #71 by fliperama86).
    // Under the native HDR output (I4) the compose pass decodes the picture itself.
    pp_settings.sdr_on_hdr = swapchain.HdrOutput() == BbHdr::Sdr && window.IsOnHdrDisplay() &&
                             EmulatorSettings.IsHdrGammaEnabled();

    // Numbered here, in command order; marked where the commands are recorded.
    const u32 stream = draw_scheduler.CrumbStream();
    const bool crumbs = Breadcrumbs::Enabled();
    const u32 fsr_crumb = crumbs ? Breadcrumbs::Note(stream, {.name = "presenter FSR"}) : 0;
    const u32 pp_crumb = crumbs ? Breadcrumbs::Note(stream, {.name = "presenter post process"}) : 0;
    draw_scheduler.Record([this, frame, image_view, image_size, frame_subresources, stream,
                           fsr_crumb, pp_crumb, fsr = fsr_settings,
                           pp = pp_settings](vk::CommandBuffer cmdbuf) {
        // Frames of different submissions may be recorded on two threads at once.
        std::scoped_lock lock{passes_mutex};
        const auto pre_barrier = vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .image = frame->image,
            .subresourceRange{frame_subresources},
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &pre_barrier,
        });
        if (fsr_crumb) {
            Breadcrumbs::Mark(cmdbuf, stream, fsr_crumb, false);
        }
        const vk::ImageView input = fsr_pass.Render(cmdbuf, image_view, image_size,
                                                    {frame->width, frame->height}, fsr,
                                                    frame->is_hdr);
        if (fsr_crumb) {
            Breadcrumbs::Mark(cmdbuf, stream, fsr_crumb, true);
            Breadcrumbs::Mark(cmdbuf, stream, pp_crumb, false);
        }
        pp_pass.Render(cmdbuf, input, image_size, *frame, pp);
        if (pp_crumb) {
            Breadcrumbs::Mark(cmdbuf, stream, pp_crumb, true);
        }
    });
    // bbport: frame generation from the output-size display buffer and its guides.
    if (generation && upscaled && !frame->is_hdr) {
        PrepareGeneration(frame, display.image, display.width, display.height, view_info.format,
                          display.format);
    }

    // Flush frame creation commands.
    BbStats::frame_number.fetch_add(1, std::memory_order_relaxed); // bbport: game frames shown
    frame->ready_semaphore = draw_scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = draw_scheduler.CurrentTick();
    SubmitInfo info{};
    draw_scheduler.Flush(info);
    BbTimeline::Note(BbTimeline::PipeTask, 5, frame->ready_tick);

    // bbport: the GPU command thread runs at most BB_FRAMES_AHEAD (default 1) guest frames
    // ahead of the GPU: it waits here for the frame that many flips back. When the GPU is the
    // bottleneck it finishes frames at an even rate; without this bound the command thread ran
    // ahead and then blocked wherever a resource ran out, so flips (and the guest's frame
    // timing) came in bursts: 12.5/25 ms alternation at 80 FPS. 0 turns it off.
    static const u32 frames_ahead = [] {
        const char* env = std::getenv("BB_FRAMES_AHEAD");
        return env ? u32(std::max(0, std::atoi(env))) : 1u;
    }();
    if (frames_ahead) {
        recent_frame_ticks.push_back(frame->ready_tick);
        while (recent_frame_ticks.size() > frames_ahead) {
            const u64 tick = recent_frame_ticks.front();
            recent_frame_ticks.pop_front();
            if (recent_frame_ticks.size() == frames_ahead) {
                BbStats::WaitTimer timer{BbStats::present_wait_ns};
                draw_scheduler.Wait(tick);
            }
        }
    }
    return frame;
}

// bbport: BB_FG_CAPTURE_DIR=<folder>: one generated frame's guides and outputs as raw files once
// its rendered serial reaches BB_FG_CAPTURE_FRAME (default 120), for
// tools/windows/analyze_fg_guides.py (from Supermedo/bloodborne_pc PR #69 by FIocker). Written
// after the draw timeline completes; keep it off for pacing measurements.
static void CaptureFg(const Instance& instance, Scheduler& scheduler, vk::CommandBuffer cmd,
                      const FgFrameResources& resources, const BbDlssCamera& camera, bool reset,
                      const Frame& frame) {
    static const char* directory = std::getenv("BB_FG_CAPTURE_DIR");
    static const u64 first = [] {
        const char* value = std::getenv("BB_FG_CAPTURE_FRAME");
        return value ? std::max<u64>(1, std::strtoull(value, nullptr, 10)) : 120;
    }();
    static bool captured = false;
    if (!directory || !*directory || captured || resources.serial < first || !resources.count) {
        return;
    }
    struct Readback {
        VkBuffer buffer{};
        VmaAllocation allocation{};
        VmaAllocationInfo info{};
        VkDeviceSize bytes{};
        vk::Image image{};
        u32 width{}, height{};
        std::string name;
    };
    struct Capture {
        VmaAllocator allocator{};
        std::vector<Readback> buffers;
        ~Capture() {
            for (const auto& b : buffers) {
                if (b.buffer) vmaDestroyBuffer(allocator, b.buffer, b.allocation);
            }
        }
    };
    const auto capture = std::make_shared<Capture>();
    capture->allocator = instance.GetAllocator();
    const auto add_raw = [&](vk::Image image, u32 width, u32 height, const std::string& name) {
        Readback b{.bytes = VkDeviceSize(width) * height * 4,
                   .image = image,
                   .width = width,
                   .height = height,
                   .name = name};
        const VkBufferCreateInfo ci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                    .size = b.bytes,
                                    .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        const VmaAllocationCreateInfo allocation{
            .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                     VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
        if (vmaCreateBuffer(capture->allocator, &ci, &allocation, &b.buffer, &b.allocation,
                            &b.info) != VK_SUCCESS) {
            return false;
        }
        capture->buffers.push_back(std::move(b));
        return true;
    };
    const auto add = [&](const FgImage& image, const std::string& name) {
        return add_raw(image.image, image.width, image.height, name);
    };
    if (!add(resources.depth, "depth.bin") || !add(resources.motion, "motion.bin") ||
        !add(resources.color, "color_rgba8.raw") || !add(resources.full, "full_rgba8.raw") ||
        !add(resources.ui, "ui_rgba8.raw")) {
        return;
    }
    for (u32 i = 0; i < resources.count; ++i) {
        if (!add(resources.outputs[i], "generated_" + std::to_string(i + 1) + "_rgba8.raw") ||
            !add(resources.presented[i], "presented_" + std::to_string(i + 1) + ".raw")) {
            return;
        }
    }
    if (!add_raw(frame.image, frame.width, frame.height, "presented_real.raw")) return;
    const auto path = std::filesystem::path(directory) / ("frame_" + std::to_string(resources.serial));
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) return;
    for (const auto& b : capture->buffers) {
        const vk::ImageMemoryBarrier2 barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = b.image,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
        cmd.copyImageToBuffer(b.image, vk::ImageLayout::eGeneral, b.buffer,
                              vk::BufferImageCopy{.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                                  .imageExtent = {b.width, b.height, 1}});
    }
    std::ostringstream manifest;
    manifest << std::setprecision(9) << "{\n\"width\":" << resources.depth.width
             << ",\"height\":" << resources.depth.height
             << ",\"output_width\":" << resources.color.width
             << ",\"output_height\":" << resources.color.height
             << ",\"sample_format\":" << u32(resources.sample_format)
             << ",\"presentation_width\":" << frame.width
             << ",\"presentation_height\":" << frame.height
             << ",\"presentation_format\":" << u32(resources.presented[0].format)
             << ",\"depth_format\":100,\"motion_format\":83,\"render_frame_index\":" << resources.serial
             << ",\"frame_index\":" << resources.serial << ",\"reflex_frame_id\":0"
             << ",\"frame_ms\":" << resources.frame_ms << ",\"reset\":" << (reset ? "true" : "false")
             << ",\"generated_count\":" << resources.count << ",\"jitter\":[" << camera.jitter[0]
             << ',' << camera.jitter[1] << "],\"camera\":{\"near_plane\":" << camera.near_plane
             << ",\"far_plane\":" << camera.far_plane << ",\"vertical_fov\":" << camera.vertical_fov
             << "}}\n";
    scheduler.DeferOperation([capture, path, text = manifest.str()] {
        bool complete = true;
        for (const auto& b : capture->buffers) {
            if (vmaInvalidateAllocation(capture->allocator, b.allocation, 0, VK_WHOLE_SIZE) != VK_SUCCESS) {
                complete = false;
                continue;
            }
            std::ofstream file(path / b.name, std::ios::binary);
            file.write(static_cast<const char*>(b.info.pMappedData), std::streamsize(b.bytes));
            complete &= bool(file);
        }
        // The manifest only after every raw file was written.
        if (complete) std::ofstream(path / "guides_manifest.json") << text;
    });
    captured = true;
    std::printf("Frame generation: captured frame %llu into %s\n",
                (unsigned long long)resources.serial, path.string().c_str());
}

void Presenter::PrepareGeneration(Frame* frame, vk::Image display_image, u32 sw, u32 sh,
                                  vk::Format sample_format, vk::Format source_format) {
    const bool srgb = sample_format == vk::Format::eR8G8B8A8Srgb ||
                      sample_format == vk::Format::eB8G8R8A8Srgb;
    if (source_format != vk::Format::eR8G8B8A8Srgb && source_format != vk::Format::eR8G8B8A8Unorm) {
        generation_history.clear();
        return;
    }
    const auto guides = rasterizer->GetUpscaler().FrameGenerationGuides(frame->guest_address);
    if (!guides.valid || !guides.serial || !guides.ui || guides.output_width != sw ||
        guides.output_height != sh) {
        generation_history.clear();
        return;
    }
    std::scoped_lock provider_lock{generation_mutex};
    if (!generation->Configure(sw, sh, vk::Format::eR8G8B8A8Unorm, generation_factor,
                               generation_backend)) {
        generation_history.clear();
        return;
    }
    if (!frame->generation) frame->generation = std::make_shared<FgFrameResources>();
    auto& resources = *frame->generation;
    resources.count = 0;
    resources.serial = guides.serial;
    resources.frame_ms = guides.frame_ms;
    resources.device = instance.GetDevice();
    resources.timestamp_valid = false;
    if (!resources.color.Ensure(instance, sw, sh, vk::Format::eR8G8B8A8Unorm) ||
        !resources.full.Ensure(instance, sw, sh, vk::Format::eR8G8B8A8Unorm) ||
        !resources.ui.Ensure(instance, sw, sh, vk::Format::eR8G8B8A8Unorm) ||
        !resources.depth.Ensure(instance, guides.render_width, guides.render_height, vk::Format::eR32Sfloat) ||
        !resources.motion.Ensure(instance, guides.render_width, guides.render_height, vk::Format::eR16G16Sfloat)) {
        return;
    }
    const u32 count = std::min<u32>(u32(generation_factor - 1), generation->MaxGenerated(generation_backend));
    if (!count) return;
    for (u32 i = 0; i < count; ++i) {
        if (resources.outputs[i].width != sw || resources.outputs[i].height != sh ||
            resources.sample_format != sample_format) {
            if (resources.srgb_views[i]) resources.device.destroyImageView(resources.srgb_views[i]);
            resources.srgb_views[i] = nullptr;
        }
        if (!resources.outputs[i].Ensure(instance, sw, sh, vk::Format::eR8G8B8A8Unorm) ||
            !resources.presented[i].Ensure(instance, frame->width, frame->height, frame_format,
                                           false)) {
            return;
        }
        if (srgb && !resources.srgb_views[i]) {
            const vk::ImageViewUsageCreateInfo sampled_usage{.usage = vk::ImageUsageFlagBits::eSampled};
            resources.srgb_views[i] = Check(resources.device.createImageView({
                .pNext = &sampled_usage,
                .image = resources.outputs[i].image,
                .viewType = vk::ImageViewType::e2D,
                // As the ordinary display view, channel order included.
                .format = sample_format,
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}}));
        }
    }
    resources.sample_format = sample_format;
    // Direct recording on this thread (after the frame's post-process pass recorded above).
    const auto cmd = draw_scheduler.CommandBuffer();
    const auto families = instance.GetPhysicalDevice().getQueueFamilyProperties();
    if (families[instance.GetGraphicsQueueFamilyIndex()].timestampValidBits) {
        if (!resources.timestamps) {
            resources.timestamps = Check(resources.device.createQueryPool({
                .queryType = vk::QueryType::eTimestamp, .queryCount = 2}));
        }
        cmd.resetQueryPool(resources.timestamps, 0, 2);
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, resources.timestamps, 0);
        resources.timestamp_valid = true;
    }
    const auto readable = [&](vk::Image image) {
        const vk::ImageMemoryBarrier2 b{.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
            .oldLayout = vk::ImageLayout::eGeneral, .newLayout = vk::ImageLayout::eGeneral,
            .image = image, .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b});
    };
    readable(guides.color);
    readable(guides.depth);
    readable(guides.motion);
    readable(guides.ui);
    FgCopy(cmd, guides.color, vk::ImageLayout::eGeneral, sw, sh, resources.color);
    FgCopy(cmd, guides.depth, vk::ImageLayout::eGeneral, guides.render_width, guides.render_height,
           resources.depth);
    FgCopy(cmd, guides.motion, vk::ImageLayout::eGeneral, guides.render_width, guides.render_height,
           resources.motion);
    FgCopy(cmd, guides.ui, vk::ImageLayout::eGeneral, sw, sh, resources.ui);
    // The display buffer as it is (encoded bytes kept: an sRGB-to-UNORM blit would decode it
    // while the HUD-less scene stays encoded, and the HUD mask would take the whole frame).
    const vk::ImageMemoryBarrier2 to_copy{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
        .oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        .newLayout = vk::ImageLayout::eTransferSrcOptimal,
        .image = display_image,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_copy});
    resources.full.General(cmd, vk::AccessFlagBits2::eTransferWrite);
    const vk::ImageCopy full_copy{.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                  .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                  .extent = {sw, sh, 1}};
    cmd.copyImage(display_image, vk::ImageLayout::eTransferSrcOptimal, resources.full.image,
                  vk::ImageLayout::eGeneral, full_copy);
    resources.full.General(cmd);
    auto restore = to_copy;
    restore.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
    restore.srcAccessMask = vk::AccessFlagBits2::eTransferRead;
    restore.dstStageMask = vk::PipelineStageFlagBits2::eAllCommands;
    restore.dstAccessMask = vk::AccessFlagBits2::eShaderRead;
    restore.oldLayout = vk::ImageLayout::eTransferSrcOptimal;
    restore.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &restore});

    std::array<FrameGeneration::Output, 3> outputs{};
    for (u32 i = 0; i < count; ++i) {
        resources.outputs[i].General(cmd);
        outputs[i].target = FgResource(resources.outputs[i]);
    }
    const FrameGeneration::Input input{
        .color = FgResource(resources.color),
        .depth = FgResource(resources.depth),
        .motion = FgResource(resources.motion),
        .camera = guides.camera,
        .frame_ms = guides.frame_ms,
        .reset = guides.reset || generation_history.needs_reset(guides.serial),
        .render_frame_index = guides.serial,
        .reflex_frame_id = u32(frame->reflex_frame_id)};
    resources.count = generation->Record(cmd, input, std::span(outputs.data(), count), count + 1);
    generation_history.submitted(guides.serial);
    if (!hud_pipeline && resources.count) {
        const auto device = instance.GetDevice();
        const std::array bindings{
            vk::DescriptorSetLayoutBinding{.binding = 0, .descriptorType = vk::DescriptorType::eSampledImage,
                                           .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute},
            vk::DescriptorSetLayoutBinding{.binding = 1, .descriptorType = vk::DescriptorType::eSampledImage,
                                           .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute},
            vk::DescriptorSetLayoutBinding{.binding = 2, .descriptorType = vk::DescriptorType::eStorageImage,
                                           .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute},
            vk::DescriptorSetLayoutBinding{.binding = 3, .descriptorType = vk::DescriptorType::eSampledImage,
                                           .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute}};
        hud_desc = Check(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = u32(bindings.size()),
            .pBindings = bindings.data()}));
        hud_layout = Check(device.createPipelineLayoutUnique({.setLayoutCount = 1, .pSetLayouts = &*hud_desc}));
        const auto module = CompileSPV(FG_HUD_COMP, device);
        hud_pipeline = Check(device.createComputePipelineUnique({}, vk::ComputePipelineCreateInfo{
            .stage = {.stage = vk::ShaderStageFlagBits::eCompute, .module = module, .pName = "main"},
            .layout = *hud_layout}));
        device.destroyShaderModule(module);
    }
    for (u32 i = 0; i < resources.count; ++i) {
        // The real frame's HUD over the generated scene (fg_hud.comp), then the same post
        // process as the real frame, at its fitted size.
        auto& output = resources.outputs[i];
        output.General(cmd);
        const std::array infos{
            vk::DescriptorImageInfo{.imageView = resources.full.view, .imageLayout = vk::ImageLayout::eGeneral},
            vk::DescriptorImageInfo{.imageView = resources.color.view, .imageLayout = vk::ImageLayout::eGeneral},
            vk::DescriptorImageInfo{.imageView = output.view, .imageLayout = vk::ImageLayout::eGeneral},
            vk::DescriptorImageInfo{.imageView = resources.ui.view, .imageLayout = vk::ImageLayout::eGeneral}};
        const std::array writes{
            vk::WriteDescriptorSet{.dstBinding = 0, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eSampledImage, .pImageInfo = &infos[0]},
            vk::WriteDescriptorSet{.dstBinding = 1, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eSampledImage, .pImageInfo = &infos[1]},
            vk::WriteDescriptorSet{.dstBinding = 2, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &infos[2]},
            vk::WriteDescriptorSet{.dstBinding = 3, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eSampledImage, .pImageInfo = &infos[3]}};
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *hud_pipeline);
        cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *hud_layout, 0, writes);
        cmd.dispatch((sw + 7) / 8, (sh + 7) / 8, 1);
        auto& fitted = resources.presented[i];
        fitted.General(cmd);
        const std::array barriers{
            vk::ImageMemoryBarrier2{.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
                .oldLayout = vk::ImageLayout::eGeneral, .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                .image = output.image, .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}},
            vk::ImageMemoryBarrier2{.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
                .oldLayout = vk::ImageLayout::eGeneral, .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .image = fitted.image, .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}}};
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = u32(barriers.size()), .pImageMemoryBarriers = barriers.data()});
        Frame target{};
        target.width = frame->width;
        target.height = frame->height;
        target.image = fitted.image;
        target.image_view = fitted.view;
        target.is_hdr = false;
        {
            std::scoped_lock lock{passes_mutex};
            pp_pass.Render(cmd, srgb ? resources.srgb_views[i] : output.view, {sw, sh}, target,
                           pp_settings);
        }
        auto back = barriers[0];
        back.srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader;
        back.srcAccessMask = vk::AccessFlagBits2::eShaderRead;
        back.dstStageMask = vk::PipelineStageFlagBits2::eAllCommands;
        back.dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
        back.oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
        back.newLayout = vk::ImageLayout::eGeneral;
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &back});
    }
    if (resources.timestamp_valid) {
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, resources.timestamps, 1);
    }
    CaptureFg(instance, draw_scheduler, cmd, resources, guides.camera, input.reset, *frame);
}

void Presenter::CountPresentation(bool real) {
    if (!generation) return;
    ++presentation_count;
    if (real) {
        ++source_count;
    } else {
        ++generated_count;
    }
    const auto now = std::chrono::steady_clock::now();
    const float seconds = std::chrono::duration<float>(now - counter_start).count();
    if (seconds >= 1.0f) {
        BbSettings::Get().source_fps = float(source_count) / seconds;
        BbSettings::Get().display_fps = float(presentation_count) / seconds;
        static const bool timing = std::getenv("BB_FG_TIMING") != nullptr;
        if (timing) {
            std::printf("FG pacing: source %.2f presented %.2f generated %llu skipped %llu\n",
                        float(source_count) / seconds, float(presentation_count) / seconds,
                        (unsigned long long)generated_count, (unsigned long long)skipped_count);
        }
        presentation_count = source_count = generated_count = skipped_count = 0;
        counter_start = now;
    }
}

Frame* Presenter::PrepareBlankFrame(bool present_thread) {
    // Request a free presentation frame.
    Frame* frame = GetRenderFrame();
    if (frame->generation) {
        frame->generation->count = 0;
        frame->generation->serial = 0;
    }
    frame->reflex_frame_id = instance.IsReflexSupported() ? Reflex::CurrentSimFrameId() : 0;

    auto& scheduler = present_thread ? present_scheduler : draw_scheduler;
    scheduler.EndRendering();

    const auto cmdbuf = scheduler.CommandBuffer();

    constexpr vk::ImageSubresourceRange simple_subresource = {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .levelCount = 1,
        .layerCount = 1,
    };
    const auto pre_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = frame->image,
        .subresourceRange = simple_subresource,
    };

    const auto post_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = frame->image,
        .subresourceRange = simple_subresource,
    };

    const vk::RenderingAttachmentInfo attachment = {
        .imageView = frame->image_view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    const vk::RenderingInfo rendering_info = {
        .renderArea =
            {
                .extent = {frame->width, frame->height},
            },
        .layerCount = 1,
        .colorAttachmentCount = 1u,
        .pColorAttachments = &attachment,
    };

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    cmdbuf.beginRendering(rendering_info);
    cmdbuf.endRendering();

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &post_barrier,
    });

    // Flush frame creation commands.
    frame->ready_semaphore = scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return frame;
}

void Presenter::WaitRendered(const Frame* frame) {
    if (!frame || !frame->ready_semaphore) {
        return;
    }
    const vk::SemaphoreWaitInfo info{
        .semaphoreCount = 1,
        .pSemaphores = &frame->ready_semaphore,
        .pValues = &frame->ready_tick,
    };
    // A lost device ends the wait (the presenter reports it); a timeout waits again.
    while (instance.GetDevice().waitSemaphores(&info, 1'000'000'000) == vk::Result::eTimeout) {
    }
}

void Presenter::Present(Frame* frame, bool is_reusing_frame, bool is_game_frame) {
    // bbport: frame generation (from Supermedo/bloodborne_pc PR #69 by FIocker): the generated
    // frames of this one go first, each at its slot between the previous real frame and this
    // one; expired slots are skipped, never presented in a burst. FSR presents after its copy
    // completed (paced on the GPU's completion); DLSS sleeps to the slot.
    std::int64_t real_deadline = 0;
    static const bool gpu_pacing = [] {
        const char* value = std::getenv("BB_FSR_GPU_PACING");
        return !value || value[0] != '0';
    }();
    const bool pace_after_copy = generation && generation_backend == BbSettings::FgFsr && gpu_pacing;
    if (generation && frame->generation && frame->generation->serial && !is_reusing_frame &&
        !BbHeadless::On() && window.GetWidth() && window.GetHeight()) {
        auto& resources = *frame->generation;
        // Completes the evaluation (also of a failed / reset one) before its flag and
        // resources are read.
        draw_scheduler.Wait(frame->ready_tick);
        const auto now = FgNow();
        u32 count = 0;
        {
            std::scoped_lock lock{generation_mutex};
            generation->Retire(resources.serial);
            if (!generation->IsInterpolationDisabled(resources.serial)) count = resources.count;
        }
        if (count) {
            std::array<u64, 2> ticks{};
            u64 source_ns = 0, ready_ns = 0;
            if (resources.timestamp_valid &&
                instance.GetDevice().getQueryPoolResults(resources.timestamps, 0, 2, sizeof(ticks),
                                                         ticks.data(), sizeof(u64),
                                                         vk::QueryResultFlagBits::e64) ==
                    vk::Result::eSuccess) {
                const double period = instance.GetPhysicalDevice().getProperties().limits.timestampPeriod;
                source_ns = u64(double(ticks[0]) * period);
                ready_ns = u64(double(ticks[1]) * period);
            }
            float frame_ms = resources.frame_ms;
            if (!source_ns && last_ready_ns && resources.serial > last_ready_serial) {
                frame_ms = float(now - last_ready_ns) / float(resources.serial - last_ready_serial) / 1e6f;
            }
            int cap = int(EmulatorSettings.GetFrameLimit());
            if (!cap) {
                if (const char* setting = std::getenv("BB_FPS")) cap = std::atoi(setting);
            }
            generation_schedule.observe(source_ns, ready_ns, resources.serial, now, frame_ms, cap);
            static const bool trace = std::getenv("BB_FG_TRACE") != nullptr;
            static const bool timing = std::getenv("BB_FG_TIMING") != nullptr;
            if (trace) {
                std::printf("FG ready: serial=%llu host_ns=%lld source_ns=%llu ready_ns=%llu period_ns=%lld first_ns=%lld real_ns=%lld count=%u\n",
                            (unsigned long long)resources.serial, (long long)now,
                            (unsigned long long)source_ns, (unsigned long long)ready_ns,
                            (long long)generation_schedule.period(),
                            (long long)generation_schedule.deadline(1, count + 1),
                            (long long)generation_schedule.deadline(count + 1, count + 1), count);
            }
            if (timing && resources.serial % 30 == 0) {
                std::printf("FG phase: serial=%llu period_ms=%.3f evaluation_ms=%.3f first_lag_ms=%.3f real_lag_ms=%.3f\n",
                            (unsigned long long)resources.serial, generation_schedule.period() / 1e6,
                            ready_ns >= source_ns ? (ready_ns - source_ns) / 1e6 : 0.0,
                            (now - generation_schedule.deadline(1, count + 1)) / 1e6,
                            (now - generation_schedule.deadline(count + 1, count + 1)) / 1e6);
            }
            last_ready_ns = now;
            last_ready_serial = resources.serial;
            // bbport: never hold a present more than 100 ms (a bad clock mapping or a stall
            // would otherwise freeze the picture): such a frame is shown at once, the schedule
            // starts again.
            if (generation_schedule.deadline(count + 1, count + 1) - now > 100'000'000) {
                static int logged = 0;
                if (logged++ < 4) {
                    std::printf("FG pacing: slot %.1f ms ahead, presenting at once (schedule reset)\n",
                                (generation_schedule.deadline(count + 1, count + 1) - now) / 1e6);
                }
                generation_schedule = FgSchedule{true, true};
                generation_history.clear();
                count = 0;
            }
            unsigned position = 1;
            while (position <= count) {
                const auto fresh = generation_schedule.next_position(position, count + 1, FgNow());
                skipped_count += fresh - position;
                position = fresh;
                if (position > count) break;
                const auto deadline = generation_schedule.deadline(position, count + 1);
                const auto delay = deadline - FgNow();
                if (!pace_after_copy && delay > 0) {
                    std::this_thread::sleep_for(std::chrono::nanoseconds(delay));
                }
                // One fence covers every present of this frame: the previous one must be done
                // before it is reset for the next.
                Check(instance.GetDevice().waitForFences(frame->present_done, true,
                                                         std::numeric_limits<u64>::max()));
                if (!PresentOne(frame, resources.presented[position - 1].image,
                                resources.presented[position - 1].view, false,
                                pace_after_copy ? deadline : 0)) {
                    break;
                }
                ++position;
            }
            real_deadline = count ? generation_schedule.deadline(count + 1, count + 1) : 0;
            const auto delay = real_deadline - FgNow();
            if (!pace_after_copy && delay > 0) {
                std::this_thread::sleep_for(std::chrono::nanoseconds(delay));
            }
            Check(instance.GetDevice().waitForFences(frame->present_done, true,
                                                     std::numeric_limits<u64>::max()));
        }
    }
    // bbport: present hold (bbport_present_hold.h, the host's first-fight visit): the first game
    // frame of a hold is shown and kept out of the free queue; the next ones go back unseen while
    // that one is shown again (as DrawLastFrame shows the last frame); the held one is freed when
    // the hold is over. Not with frame generation (its own pacing owns the presents).
    if (!is_reusing_frame && is_game_frame && !generation) {
        if (BbPresentHold::Active()) {
            if (held_frame == nullptr) {
                PresentOne(frame, frame->image, frame->image_view, true, 0);
                held_frame = frame;
                last_submit_frame = frame;
            } else {
                last_submit_frame = held_frame;
                if (Frame* again = PrepareLastFrame()) {
                    PresentOne(again, again->image, again->image_view, true, 0);
                }
                std::scoped_lock fl{free_mutex};
                free_queue.push(frame);
                free_cv.notify_one();
            }
            DebugState.IncFlipFrameNum();
            return;
        }
        if (held_frame != nullptr) {
            std::scoped_lock fl{free_mutex};
            free_queue.push(held_frame);
            free_cv.notify_one();
            held_frame = nullptr;
        }
    }
    // Ordinary frames, loading screens, unavailable or failed generation and expired slots all
    // keep this real frame and the game's flip.
    PresentOne(frame, frame->image, frame->image_view, true, pace_after_copy ? real_deadline : 0);
    if (!is_reusing_frame) {
        last_submit_frame = frame;
        std::scoped_lock fl{free_mutex};
        free_queue.push(frame);
        free_cv.notify_one();
    }
    if (!is_reusing_frame && is_game_frame) {
        DebugState.IncFlipFrameNum();
    }
}

// bbport: presents under the queue lock; a recreation after it takes the lock itself
// (Swapchain::Recreate), so it is done by the caller once this returns.
bool Presenter::PresentLocked() {
    std::scoped_lock submit_lock{Scheduler::submit_mutex};
    return swapchain.Present();
}

bool Presenter::PresentOne(Frame* frame, vk::Image source_image, vk::ImageView source_view, bool real,
                           std::int64_t deadline) {
    // bbport: nothing to present to while the window is minimised (a 0x0 surface, issue #31), or in
    // the headless-lite test mode (bbport_headless.h).
    if (BbHeadless::On() || window.GetWidth() == 0 || window.GetHeight() == 0) {
        return false;
    }
    // Recreate the swapchain if the window was resized (or was minimised at the last try).
    if (window.GetWidth() != swapchain.GetWidth() || window.GetHeight() != swapchain.GetHeight() ||
        !swapchain.IsPresentable()) {
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
        if (!swapchain.IsPresentable()) {
            return false;
        }
    }

    if (!swapchain.AcquireNextImage()) {
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
        if (!swapchain.AcquireNextImage()) {
            // User resizes the window too fast and GPU can't keep up. Skip this frame.
            LOG_WARNING(Render_Vulkan, "Skipping frame!");
            return false;
        }
    }

    // Reset fence for queue submission. Do it here instead of GetRenderFrame() because we may
    // skip frame because of slow swapchain recreation. If a frame skip occurs, we skip signal
    // the frame's present fence and future GetRenderFrame() call will hang waiting for this frame.
    const auto reset_result = instance.GetDevice().resetFences(frame->present_done);
    ASSERT_MSG(reset_result == vk::Result::eSuccess,
               "Unexpected error resetting present done fence: {}", vk::to_string(reset_result));

    // bbport: the game frame is blitted (letterboxed) straight into the swapchain image.
    const vk::Image swapchain_image = swapchain.Image();
    auto& scheduler = present_scheduler;
    const bool reflex = instance.IsReflexSupported();
    if (reflex) {
        Reflex::SetRecordingFrameId(frame->reflex_frame_id);
        scheduler.SetFrameId(frame->reflex_frame_id);
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    if (real) {
        DumpFinalFrameIfDue(instance, scheduler, cmdbuf, frame->image, frame->width, frame->height,
                            frame_format);
    }

    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
            .pLabelName = "Present",
        });
    }

    if (swapchain.HdrOutput() != BbHdr::Sdr) {
        SetExpectedGameSize(s32(swapchain.GetExtent().width), s32(swapchain.GetExtent().height));
        RecordHdrPresent(cmdbuf, frame, source_image, source_view, real, 0.0f, false);
    } else {
        const vk::Extent2D extent = swapchain.GetExtent();
        SetExpectedGameSize(s32(extent.width), s32(extent.height));
        const vk::ImageSubresourceRange color_range{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = VK_REMAINING_ARRAY_LAYERS,
        };
        const std::array pre_barriers{
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eNone,
                .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eTransferDstOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange = color_range,
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = source_image,
                .subresourceRange = color_range,
            },
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                               vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlagBits::eByRegion,
                               {}, {}, pre_barriers);
        const vk::ClearColorValue black{std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f}};
        cmdbuf.clearColorImage(swapchain_image, vk::ImageLayout::eTransferDstOptimal, black, color_range);
        const vk::MemoryBarrier clear_done{
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer,
                               vk::DependencyFlagBits::eByRegion, clear_done, {}, {});
        cmdbuf.blitImage(source_image, vk::ImageLayout::eTransferSrcOptimal, swapchain_image,
                         vk::ImageLayout::eTransferDstOptimal,
                         MakeImageBlitFit(frame->width, frame->height, extent.width, extent.height),
                         vk::Filter::eLinear);
        // bbport: the settings menu / FPS counter over the frame, at display resolution.
        if (real) {
            BbGameMenu::Poll(); // the port's pages in the game's System menu
            BbFrameState::OnPresent(); // menu/loading detection (compile indicator)
        }
        const bool overlay = BbOverlay::Visible();
        const std::array post_barriers{
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                .dstAccessMask = overlay ? vk::AccessFlagBits::eColorAttachmentRead |
                                               vk::AccessFlagBits::eColorAttachmentWrite
                                         : vk::AccessFlagBits::eNone,
                .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                .newLayout = overlay ? vk::ImageLayout::eColorAttachmentOptimal
                                     : vk::ImageLayout::ePresentSrcKHR,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange = color_range,
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eTransferRead,
                .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = source_image,
                .subresourceRange = color_range,
            },
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                               vk::PipelineStageFlagBits::eAllCommands,
                               vk::DependencyFlagBits::eByRegion, {}, {}, post_barriers);
        if (overlay) {
            BbOverlay::Render(cmdbuf, swapchain.ImageView(), extent);
            const vk::ImageMemoryBarrier to_present{
                .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .dstAccessMask = vk::AccessFlagBits::eNone,
                .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .newLayout = vk::ImageLayout::ePresentSrcKHR,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange = color_range,
            };
            cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
                                   vk::PipelineStageFlagBits::eBottomOfPipe,
                                   vk::DependencyFlagBits::eByRegion, {}, {}, to_present);
        }
    }
    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.endDebugUtilsLabelEXT();
    }

    // Flush vulkan commands.

    // bbport: the frame's submission goes out from a recording thread (BB_ASYNC_SUBMIT); this
    // one, waiting for it on the same queue, must not get there first.
    if (frame->ready_semaphore == draw_scheduler.GetWorkSemaphore()->Handle()) {
        draw_scheduler.WaitSubmitted(frame->ready_tick);
    }
    SubmitInfo info{};
    info.AddWait(swapchain.GetImageAcquiredSemaphore());
    info.AddWait(frame->ready_semaphore, frame->ready_tick);
    info.AddSignal(swapchain.GetPresentReadySemaphore());
    info.AddSignal(frame->present_done);
    scheduler.Flush(info);

    // bbport: FSR frame generation's paced present: its composition copy completes before the
    // deadline (a copy queued behind the next frame's work would reach the display late).
    if (deadline) {
        Check(instance.GetDevice().waitForFences(frame->present_done, true,
                                                 std::numeric_limits<u64>::max()));
        Common::PreciseSleepUntil(
            std::chrono::steady_clock::time_point(std::chrono::nanoseconds(deadline)));
    }

    // Present to swapchain.
    if (reflex) {
        if (real) Reflex::TraceCpuPresent(frame->content_frame_id);
        Reflex::OnPresentBegin(frame->reflex_frame_id, real);
    }
    if (!PresentLocked()) {
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
    }
    if (reflex) {
        Reflex::OnPresentEnd(frame->reflex_frame_id, real);
        // BB_REFLEX_TIMING=1: the driver's latency report of the newest finished frame.
        static const bool timing = std::getenv("BB_REFLEX_TIMING") != nullptr;
        Reflex::LatencyReport r{};
        if (real && timing && Reflex::QueryLatencyTimings(&r) && r.present_id > last_reflex_report) {
            last_reflex_report = r.present_id;
            std::printf("reflex timing: id=%llu input=%llu sim=%llu..%llu render=%llu..%llu "
                        "present=%llu..%llu gpu=%llu..%llu\n",
                        (unsigned long long)r.present_id, (unsigned long long)r.input_sample_us,
                        (unsigned long long)r.sim_start_us, (unsigned long long)r.sim_end_us,
                        (unsigned long long)r.render_submit_start_us,
                        (unsigned long long)r.render_submit_end_us,
                        (unsigned long long)r.present_start_us, (unsigned long long)r.present_end_us,
                        (unsigned long long)r.gpu_render_start_us, (unsigned long long)r.gpu_render_end_us);
        }
    }
    CountPresentation(real);
    return true;
}

Frame* Presenter::GetRenderFrame() {
    // Wait for free presentation frames
    Frame* frame;
    {
        std::unique_lock lock{free_mutex};
        free_cv.wait(lock, [this] { return !free_queue.empty(); });
        LOG_DEBUG(Render_Vulkan, "Got render frame, remaining {}", free_queue.size() - 1);

        // Take the frame from the queue
        frame = free_queue.front();
        free_queue.pop();
    }

    const vk::Device device = instance.GetDevice();
    vk::Result result{};

    const auto wait = [&]() {
        result = device.waitForFences(frame->present_done, false, std::numeric_limits<u64>::max());
        return result;
    };

    // Wait for the presentation to be finished so all frame resources are free
    while (wait() != vk::Result::eSuccess) {
        if (result == vk::Result::eErrorDeviceLost) {
            Breadcrumbs::ReportDeviceLost("waiting for a frame");
        }
        ASSERT_MSG(result != vk::Result::eErrorDeviceLost,
                   "Device lost during waiting for a frame");
        // Retry if the waiting times out
        if (result == vk::Result::eTimeout) {
            continue;
        }
    }

    if (frame->width != expected_frame_width || frame->height != expected_frame_height ||
        frame->is_hdr != swapchain.GetHDR()) {
        RecreateFrame(frame, expected_frame_width, expected_frame_height);
    }

    return frame;
}

void Presenter::SetExpectedGameSize(s32 width, s32 height) {
    if (width <= 0 || height <= 0) {
        return; // minimised: keep the last size (a 0x0 frame image cannot be made, issue #31)
    }
    const float ratio = (float)width / (float)height;

    expected_frame_height = height;
    expected_frame_width = width;
    if (ratio > expected_ratio) {
        expected_frame_width = static_cast<s32>(height * expected_ratio);
    } else {
        expected_frame_height = static_cast<s32>(width / expected_ratio);
    }
}

} // namespace Vulkan

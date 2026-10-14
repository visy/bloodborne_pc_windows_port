// SPDX-FileCopyrightText: Copyright 2026 IFreemz, bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// bbport: NVIDIA DLSS Super Resolution and Frame Generation through the optional bridge
// (gpu/dlss_bridge, MIT; libbbport_dlss.so / bbport_dlss.dll), loaded at run time from next to
// bb-probe together with NVIDIA's libnvidia-ngx-dlss.so.<version> / nvngx_dlss.dll (frame
// generation: nvngx_dlssg.dll, ABI 2 bridge). The port itself contains no NVIDIA code; without
// the libraries, on other GPUs or
// with BB_DLSS=0 nothing changes. Adapted from IFreemz/shadPS4-Bloodborne-DLSS-FSR
// (vk_dlss_ngx).

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "../../../dlss_bridge/bbport_dlss_bridge.h"

namespace Vulkan {

class Dlss {
public:
    /// The process-wide instance, or null when the DLLs are absent or BB_DLSS=0.
    static Dlss* Get();
    ~Dlss();

    /// Instance and device creation: adds what NGX needs (or disables DLSS when it is missing).
    void AppendInstanceExtensions(std::vector<const char*>& enabled);
    void AppendDeviceExtensions(vk::Instance instance, vk::PhysicalDevice physical,
                                std::vector<const char*>& enabled);
    /// After the device exists.
    void Initialize(vk::Instance instance, vk::PhysicalDevice physical, vk::Device device);
    void Shutdown();

    [[nodiscard]] bool Available() const;
    /// Why DLSS cannot run on this system (empty when it can, or before device creation).
    [[nodiscard]] std::string Problem() const;

    struct Resource {
        vk::Image image;
        vk::ImageView view;
        vk::Format format;
        vk::ImageAspectFlags aspect;
        u32 width, height;
    };
    struct FeatureDesc {
        u32 input_width, input_height, output_width, output_height;
        int quality;
        bool hdr;
        bool operator==(const FeatureDesc&) const = default;
    };
    struct Frame {
        Resource color, depth, motion, output;
        float jitter_x, jitter_y;
        bool reset;
        float frame_ms;
        float sharpness;
    };

    /// DLSS quality mode for an output/render size ratio (0 DLAA .. 4 Ultra Performance).
    static int QualityForScale(float scale);
    [[nodiscard]] bool HasFeature(const FeatureDesc& desc) const;
    /// Records the feature creation. The GPU must be done with the previous one.
    bool CreateFeature(vk::CommandBuffer command, const FeatureDesc& desc);
    bool Evaluate(vk::CommandBuffer command, const Frame& frame);
    void ReleaseFeature();

    /// DLSS frame generation (DLSS-G, ABI 2 bridge; from Supermedo/bloodborne_pc PR #69 by
    /// FIocker). Wanted only when the player chose it for this start (set before the instance is
    /// created): otherwise no frame generation extensions are enabled and nothing changes.
    static void WantFrameGeneration(bool wanted) { fg_wanted = wanted; }
    [[nodiscard]] bool FgAvailable() const;
    /// Why DLSS frame generation cannot run (empty when it can).
    [[nodiscard]] std::string FgProblem() const;
    [[nodiscard]] u32 FgMaxGenerated() const;
    bool CreateFgFeature(vk::CommandBuffer command, u32 width, u32 height, vk::Format format);
    bool EvaluateFg(vk::CommandBuffer command, const BbDlssFgEvaluate& evaluate,
                    u32 multi_frame_count, u32 multi_frame_index, u64 render_frame);
    void ReleaseFgFeature();

private:
    Dlss();
    static bool fg_wanted;
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Vulkan

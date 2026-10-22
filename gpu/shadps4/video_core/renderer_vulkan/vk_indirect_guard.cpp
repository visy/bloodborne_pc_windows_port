// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_indirect_guard.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "video_core/buffer_cache/buffer.h"
#include "video_core/host_shaders/indirect_draw_guard_comp.h"
#include "video_core/host_shaders/indirect_guard_comp.h"
#include "video_core/renderer_vulkan/vk_breadcrumbs.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

namespace {
struct Push {
    u32 max_count[3];
    u32 first;
    u32 max_total;
    u32 slot;
};

struct Skipped {
    u32 count;
    u32 pad[3];
    u32 last[3];
};

struct DrawPush {
    u32 first;
    u32 stride;
    u32 count;
    u32 max_total;
    u32 slot;
};

struct DrawSkipped {
    u32 count;
    u32 last_count;
    u32 last_instances;
    u32 largest; ///< vertices times instances of the largest command run (statistics)
};

bool StatsOn() {
    static const bool on = [] {
        const char* v = std::getenv("BB_INDIRECT_GUARD_STATS");
        return v && v[0] == '1';
    }();
    return on;
}

std::atomic<u64> draws_checked{0};
std::atomic<u64> draws_unchecked{0};
std::atomic<u64> draws_split{0};

s64 NowNs() {
    return std::chrono::steady_clock::now().time_since_epoch().count();
}
} // namespace

bool IndirectGuard::Enabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("BB_INDIRECT_GUARD");
        return !(v && v[0] == '0');
    }();
    return enabled;
}

IndirectGuard::IndirectGuard(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {
    const auto device = instance.GetDevice();
    checked = std::make_unique<VideoCore::Buffer>(instance, 0, NumSlots * 16,
                                                  VideoCore::MemoryType::DeviceLocal,
                                                  "Indirect dispatch counts");
    skipped = std::make_unique<VideoCore::Buffer>(instance, 0, sizeof(Skipped),
                                                  VideoCore::MemoryType::HostCached,
                                                  "Indirect dispatches skipped");
    std::memset(skipped->mapped_data.data(), 0, sizeof(Skipped));
    skipped->Flush(0, sizeof(Skipped));
    draw_checked = std::make_unique<VideoCore::Buffer>(instance, 0, DrawRingDwords * 4,
                                                       VideoCore::MemoryType::DeviceLocal,
                                                       "Indirect draw commands");
    draw_skipped = std::make_unique<VideoCore::Buffer>(instance, 0, sizeof(DrawSkipped),
                                                       VideoCore::MemoryType::HostCached,
                                                       "Indirect draws skipped");
    std::memset(draw_skipped->mapped_data.data(), 0, sizeof(DrawSkipped));
    draw_skipped->Flush(0, sizeof(DrawSkipped));

    const auto limits = instance.GetPhysicalDevice().getProperties().limits;
    for (u32 i = 0; i < 3; ++i) {
        max_count[i] = limits.maxComputeWorkGroupCount[i];
    }
    const char* max = std::getenv("BB_INDIRECT_GUARD_MAX");
    max_total = max && *max ? static_cast<u32>(std::strtoul(max, nullptr, 0)) : 1u << 22;
    const char* max_vertices = std::getenv("BB_INDIRECT_GUARD_MAX_VERTICES");
    max_draw_total = max_vertices && *max_vertices
                         ? static_cast<u32>(std::strtoul(max_vertices, nullptr, 0))
                         : 1u << 24;

    std::array<vk::DescriptorSetLayoutBinding, 3> bindings{};
    for (u32 i = 0; i < bindings.size(); ++i) {
        bindings[i] = {.binding = i,
                       .descriptorType = vk::DescriptorType::eStorageBuffer,
                       .descriptorCount = 1,
                       .stageFlags = vk::ShaderStageFlagBits::eCompute};
    }
    set_layout = Check(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    }));
    const vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                      .offset = 0,
                                      .size = sizeof(Push)};
    pipeline_layout = Check(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &*set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &range,
    }));
    const auto module = CompileSPV(INDIRECT_GUARD_COMP, device);
    pipeline = Check(device.createComputePipelineUnique(
        {}, vk::ComputePipelineCreateInfo{
                .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                          .module = module,
                          .pName = "main"},
                .layout = *pipeline_layout,
            }));
    device.destroyShaderModule(module);
    static_assert(sizeof(DrawPush) <= sizeof(Push)); // one pipeline layout for both checks
    const auto draw_module = CompileSPV(INDIRECT_DRAW_GUARD_COMP, device);
    draw_pipeline = Check(device.createComputePipelineUnique(
        {}, vk::ComputePipelineCreateInfo{
                .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                          .module = draw_module,
                          .pName = "main"},
                .layout = *pipeline_layout,
            }));
    device.destroyShaderModule(draw_module);
    last_report_ns = NowNs();
}

IndirectGuard::~IndirectGuard() = default;

std::pair<vk::Buffer, u64> IndirectGuard::CheckArgs(const VideoCore::Buffer& args, u64 offset) {
    // A storage binding starts at a multiple of the device's alignment: the arguments are
    // `first` dwords into it.
    const u64 alignment = instance.StorageMinAlignment();
    const u64 bind_offset = offset / alignment * alignment;
    const u32 first = static_cast<u32>((offset - bind_offset) / sizeof(u32));
    const u64 bind_size = (offset - bind_offset) + 3 * sizeof(u32);
    const u32 slot = next_slot.fetch_add(1, std::memory_order_relaxed) % NumSlots;
    const Push push{{max_count[0], max_count[1], max_count[2]}, first, max_total, slot * 4};
    scheduler.Record([this, source = args.Handle(), bind_offset, bind_size,
                      push](vk::CommandBuffer cmdbuf) {
        const vk::MemoryBarrier2 before{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead |
                             vk::AccessFlagBits2::eShaderStorageWrite,
        };
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &before});
        const std::array<vk::DescriptorBufferInfo, 3> infos{{
            {source, bind_offset, bind_size},
            {checked->Handle(), 0, NumSlots * 16},
            {skipped->Handle(), 0, sizeof(Skipped)},
        }};
        std::array<vk::WriteDescriptorSet, 3> writes{};
        for (u32 i = 0; i < writes.size(); ++i) {
            writes[i] = {.dstBinding = i,
                         .descriptorCount = 1,
                         .descriptorType = vk::DescriptorType::eStorageBuffer,
                         .pBufferInfo = &infos[i]};
        }
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *pipeline_layout, 0, writes);
        cmdbuf.pushConstants(*pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push),
                             &push);
        cmdbuf.dispatch(1, 1, 1);
        const vk::MemoryBarrier2 after{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eDrawIndirect |
                            vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eIndirectCommandRead |
                             vk::AccessFlagBits2::eShaderStorageRead,
        };
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &after});
    });
    Report();
    return {checked->Handle(), u64(slot) * 16};
}

std::pair<vk::Buffer, u64> IndirectGuard::CheckDrawArgs(const VideoCore::Buffer& args, u64 offset,
                                                         u32 stride, u32 count, u32 args_slot) {
    const u32 stride_dwords = stride / sizeof(u32);
    if (count == 0 || count > MaxDrawCommands || stride % sizeof(u32) != 0 || stride_dwords < 4 ||
        stride_dwords > 5) {
        draws_unchecked.fetch_add(1, std::memory_order_relaxed);
        return {args.Handle(), offset};
    }
    draws_checked.fetch_add(1, std::memory_order_relaxed);
    const u64 alignment = instance.StorageMinAlignment();
    const u64 bind_offset = offset / alignment * alignment;
    const u32 first = static_cast<u32>((offset - bind_offset) / sizeof(u32));
    const u64 bind_size = (offset - bind_offset) + u64(count) * stride;
    // A ring: the barrier before the check waits for every earlier command, the draws that read
    // a slot before among them.
    const u32 dwords = count * stride_dwords;
    if (draw_next + dwords > DrawRingDwords) {
        draw_next = 0;
    }
    const u32 slot = draw_next;
    draw_next += dwords;
    const DrawPush push{first, stride_dwords, count, max_draw_total, slot};
    const u32 groups = (count + 63) / 64;
    scheduler.Record([this, source = args.Handle(), offset, bind_offset, bind_size, push, groups,
                      args_slot](vk::CommandBuffer cmdbuf) {
        const vk::MemoryBarrier2 before{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask =
                vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eCopy,
            .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead |
                             vk::AccessFlagBits2::eShaderStorageWrite |
                             vk::AccessFlagBits2::eTransferRead,
        };
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &before});
        Breadcrumbs::CopyArgs(cmdbuf, source, offset, args_slot); // what the game wrote
        const std::array<vk::DescriptorBufferInfo, 3> infos{{
            {source, bind_offset, bind_size},
            {draw_checked->Handle(), 0, DrawRingDwords * 4},
            {draw_skipped->Handle(), 0, sizeof(DrawSkipped)},
        }};
        std::array<vk::WriteDescriptorSet, 3> writes{};
        for (u32 i = 0; i < writes.size(); ++i) {
            writes[i] = {.dstBinding = i,
                         .descriptorCount = 1,
                         .descriptorType = vk::DescriptorType::eStorageBuffer,
                         .pBufferInfo = &infos[i]};
        }
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *draw_pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *pipeline_layout, 0, writes);
        cmdbuf.pushConstants(*pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push),
                             &push);
        cmdbuf.dispatch(groups, 1, 1);
        const vk::MemoryBarrier2 after{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eDrawIndirect,
            .dstAccessMask = vk::AccessFlagBits2::eIndirectCommandRead,
        };
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &after});
    });
    Report();
    return {draw_checked->Handle(), u64(slot) * sizeof(u32)};
}

void IndirectGuard::NoteRenderPassSplit() {
    draws_split.fetch_add(1, std::memory_order_relaxed);
}

void IndirectGuard::Report() {
    const s64 now = NowNs();
    s64 last = last_report_ns.load(std::memory_order_relaxed);
    if (now - last < 5'000'000'000 ||
        !last_report_ns.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
        return;
    }
    skipped->Invalidate(0, sizeof(Skipped));
    const auto* s = reinterpret_cast<const volatile Skipped*>(skipped->mapped_data.data());
    const u32 count = s->count;
    const u32 before = reported.exchange(count, std::memory_order_relaxed);
    if (count != before) {
        std::printf("Indirect dispatches: %u skipped in all (garbage group counts, the last "
                    "%ux%ux%u; BB_INDIRECT_GUARD)\n",
                    count, s->last[0], s->last[1], s->last[2]);
    }
    draw_skipped->Invalidate(0, sizeof(DrawSkipped));
    const auto* d = reinterpret_cast<const volatile DrawSkipped*>(draw_skipped->mapped_data.data());
    const u32 draw_count = d->count;
    const u32 draw_before = draw_reported.exchange(draw_count, std::memory_order_relaxed);
    if (draw_count != draw_before) {
        std::printf("Indirect draws: %u skipped in all (garbage counts, the last %u vertices x %u "
                    "instances; BB_INDIRECT_GUARD)\n",
                    draw_count, d->last_count, d->last_instances);
    }
    if (StatsOn()) {
        std::printf("Indirect draws: %llu checked, %llu unchecked, %llu render passes resumed for "
                    "a check; largest command %u vertices x instances (BB_INDIRECT_GUARD_STATS)\n",
                    (unsigned long long)draws_checked.load(std::memory_order_relaxed),
                    (unsigned long long)draws_unchecked.load(std::memory_order_relaxed),
                    (unsigned long long)draws_split.load(std::memory_order_relaxed), d->largest);
    }
}

} // namespace Vulkan

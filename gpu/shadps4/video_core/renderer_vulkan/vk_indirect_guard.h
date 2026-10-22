// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: indirect dispatches run from checked group counts.
//
// The game's indirect dispatches take their group counts from memory a shader wrote. Twice
// (issues #34 and #32, the PC memory model) those counts were garbage — 1.2 and 3.8 billion
// groups of the particle simulations — and the GPU, still running them after the kernel's
// timeout, was reset: device lost, the game gone. Bloodborne's real counts are at most a few
// hundred. Before each indirect dispatch a one-thread shader (indirect_guard.comp) copies the
// counts to a slot of our own, zero if they are implausible (past the device's limits or
// BB_INDIRECT_GUARD_MAX groups in all, default 4M), and the dispatch reads that slot. Skipped
// dispatches are counted and reported. BB_INDIRECT_GUARD=0: the game's counts as they are.
//
// Indirect draws the same way (0.7.3: a guest's device lost on its arrival in Cathedral Ward,
// stuck in a particle draw with no loop in its shaders: most likely garbage counts again): their commands are copied to a
// ring of our own, a command with more than BB_INDIRECT_GUARD_MAX_VERTICES vertices (indices)
// times instances (default 16M) drawing nothing, and
// the draw reads the copy. The check runs outside the render pass, which is closed and
// resumed without clearing around it. BB_INDIRECT_GUARD_STATS=1 (diagnostics): every 5 s
// the draws checked and the largest command seen.
#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <utility>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace VideoCore {
struct Buffer;
}

namespace Vulkan {

class Instance;
class Scheduler;

class IndirectGuard {
public:
    IndirectGuard(const Instance& instance, Scheduler& scheduler);
    ~IndirectGuard();

    static bool Enabled();

    /// Records the check of the arguments at `offset` in `args` (outside a render pass); returns
    /// the buffer and offset to dispatch from.
    std::pair<vk::Buffer, u64> CheckArgs(const VideoCore::Buffer& args, u64 offset);

    /// The same for the `count` draw commands `stride` bytes apart at `offset` in `args`
    /// (outside a render pass). `args_slot`: the breadcrumbs' copy of the first command
    /// (Breadcrumbs::ArgsSlot), ~0u for none. Returns the buffer and offset to draw from; `args`
    /// itself when there are too many commands to copy.
    std::pair<vk::Buffer, u64> CheckDrawArgs(const VideoCore::Buffer& args, u64 offset, u32 stride,
                                             u32 count, u32 args_slot);

private:
    static constexpr u32 NumSlots = 1024;
    static constexpr u32 DrawRingDwords = 1u << 16; ///< 256 KiB of checked draw commands
    static constexpr u32 MaxDrawCommands = 1024;    ///< more: drawn unchecked

    void Report();

public:
    /// A render pass closed for a check (statistics).
    static void NoteRenderPassSplit();

private:

    const Instance& instance;
    Scheduler& scheduler;
    std::unique_ptr<VideoCore::Buffer> checked; ///< NumSlots x 16 bytes of group counts
    std::unique_ptr<VideoCore::Buffer> skipped; ///< count and the last skipped counts (host)
    vk::UniqueDescriptorSetLayout set_layout;
    vk::UniquePipelineLayout pipeline_layout;
    vk::UniquePipeline pipeline;
    std::unique_ptr<VideoCore::Buffer> draw_checked; ///< DrawRingDwords of draw commands
    std::unique_ptr<VideoCore::Buffer> draw_skipped; ///< count and the last skipped command
    vk::UniquePipeline draw_pipeline;
    u32 draw_next = 0; ///< next free dword of draw_checked (stage B only)
    u32 max_draw_total = 0;
    std::atomic<u32> draw_reported{0};
    u32 max_count[3]{};
    u32 max_total = 0;
    std::atomic<u32> next_slot{0};
    std::atomic<u32> reported{0};
    std::atomic<s64> last_report_ns{0};
};

} // namespace Vulkan

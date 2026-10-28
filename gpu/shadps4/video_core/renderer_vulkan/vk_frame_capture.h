// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: frame analyzer. Creating the file named by BB_CAPTURE_TRIGGER records the next full
// frame: every render pass (targets, depth, draw count, shaders, sampled textures) and compute
// dispatch, in order, plus the buffer that was presented. Input for placing a temporal
// upscaler (scene color, depth, motion vectors, UI) in Bloodborne's frame.

#pragma once

#include <array>
#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

#include "common/types.h"

namespace VideoCore {
struct ImageInfo;
}

namespace Vulkan {

class FrameCapture {
public:
    /// VideoOut thread, per flip: frame boundary, presented buffer, trigger file check.
    static void OnFlip(VAddr presented_address);

    /// VideoOut: a display buffer the game registered. A pass drawing into one starts a frame
    /// in the GPU command stream (the flip itself runs a frame behind).
    static void AddDisplayBuffer(VAddr address);
    static bool IsDisplayBuffer(VAddr address);

    /// GPU thread: whether passes are being looked at (armed or recording; cheap).
    static bool Active() {
        return state.load(std::memory_order_relaxed) != Idle;
    }
    /// GPU thread, at every draw/dispatch: starts or ends a recording at frame boundaries.
    static void Poll();

    static void BeginPass(const VideoCore::ImageInfo* const* colors, u32 num_colors,
                          const VideoCore::ImageInfo* depth);
    static void Draw(u64 vs_hash, u64 ps_hash, u32 num_indices, u32 num_instances);
    static void Dispatch(u64 cs_hash, u32 x, u32 y, u32 z);
    static void Sampled(const VideoCore::ImageInfo& info, bool storage);
    /// Contents of a bound buffer (first 1 KiB), kept for small passes and a pass's first draw.
    static void Buffer(u64 stage_hash, u32 slot, VAddr address, const void* data, u64 size);
    static void Note(const char* text);
    /// BB_CAPTURE_MESH_COUNTS=N[,N...] (diagnostic): draws of these index counts (a chr mesh's face
    /// sets, tools/mp/flver_meshes.py) are listed at the end of a record with the statistics of every
    /// buffer they read: their vertex streams (the first three floats of each element: the position's
    /// box, NaN count) and their bound constants / storage (NaN, Inf, largest magnitude, zero floats).
    /// Says whether a mesh that is not seen was drawn with collapsed or broken data.
    static bool WantsMesh(u32 num_indices);
    static void Vertex(u32 stream, VAddr address, const void* data, u64 size, u32 stride);

    /// BB_DRAW_SKIP_FILE (diagnostic, off by default): draws whose vertex or pixel shader hash
    /// is listed in the file (hex, whitespace-separated; re-read at a flip when it changes) are
    /// not recorded. Finds which draw puts a surface on screen, inside one game session.
    static bool SkipDraw(u64 vs_hash, u64 ps_hash) {
        const u32 count = skip_count.load(std::memory_order_acquire);
        for (u32 i = 0; i < count; ++i) {
            const u64 h = skip_hashes[i].load(std::memory_order_relaxed);
            if (h == vs_hash || h == ps_hash) {
                return true;
            }
        }
        return false;
    }

private:
    enum : u32 { Idle, Armed, Recording };
    static inline std::atomic<u32> state{Idle};
    static inline std::atomic<u64> flips{0};
    static inline std::atomic<VAddr> last_presented{0};
    static inline std::array<std::atomic<u64>, 64> skip_hashes{};
    static inline std::atomic<u32> skip_count{0};
};

} // namespace Vulkan

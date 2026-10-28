// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_frame_capture.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <format>
#include <functional>
#include <mutex>

#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image_info.h"

namespace Vulkan {

namespace {

struct Target {
    VAddr address{};
    vk::Format format{};
    u32 width{}, height{};
    u32 tile{}, levels{}, layers{};
    bool operator==(const Target&) const = default;
};

/// The draws of one vs/ps pair in a pass, with the textures its first draw sampled.
struct Group {
    u64 vs{}, ps{};
    u32 draws{};
    u64 indices{};
    std::vector<std::pair<Target, bool>> sampled;
    /// Draws per index count (count, draws): one model's mesh is one count at each LOD, so a
    /// model that went missing between two captures shows up as a missing count.
    std::vector<std::pair<u32, u32>> sizes;
};

struct Entry {
    bool compute{};
    std::vector<Target> colors;
    Target depth;
    u32 draws{};
    u64 indices{};
    std::vector<u64> shaders; ///< vs/ps pairs (graphics) or cs (compute), first few
    std::vector<std::pair<Target, bool>> sampled; ///< textures (false) and storage images (true)
    std::string note;
    std::vector<std::string> buffers; ///< dumped constants, per draw
    std::vector<Group> groups;
};

constexpr size_t kMaxSizes = 256; ///< distinct index counts listed per group

// GPU thread only.
std::vector<Entry> entries;
std::vector<std::pair<Target, bool>> pending_sampled;
std::vector<std::string> pending_buffers;
// BB_CAPTURE_MESH_COUNTS: the buffer statistics of the draw being prepared, and the listed draws.
std::vector<std::string> pending_stats;
std::vector<std::string> mesh_lines;
constexpr size_t kMaxMeshLines = 512;

std::string FloatStats(const void* data, u64 size) {
    const u64 n = size / 4;
    const auto* f = static_cast<const float*>(data);
    u64 nan = 0, inf = 0, zero = 0;
    float big = 0;
    for (u64 i = 0; i < n; ++i) {
        const float v = f[i];
        if (std::isnan(v)) {
            ++nan;
        } else if (std::isinf(v)) {
            ++inf;
        } else {
            zero += v == 0.0f;
            big = std::max(big, std::fabs(v));
        }
    }
    return std::format("{} floats, NaN {}, Inf {}, zero {}, max |v| {:.4g}", n, nan, inf, zero, big);
}

void AddBuffers(Entry& entry) {
    // Constants of full-screen passes (post-processing: camera matrices) and of the first draw
    // of each geometry pass.
    if (entry.draws <= 4) {
        for (auto& b : pending_buffers) {
            entry.buffers.push_back(std::move(b));
        }
    }
    pending_buffers.clear();
}
bool pass_open = false;
std::mutex display_mutex;
std::vector<VAddr> display_buffers;
// bbport: read on every draw; the game registers a handful of display buffers once.
std::array<std::atomic<VAddr>, 16> display_list{};
std::atomic<u32> display_count{0};

Target ToTarget(const VideoCore::ImageInfo& info) {
    return {info.guest_address, info.pixel_format, info.size.width, info.size.height,
            u32(info.tile_mode), info.resources.levels, info.resources.layers};
}

std::string Describe(const Target& t) {
    if (!t.address) {
        return "-";
    }
    return std::format("{:#x} {} {}x{} tile {} mips {} layers {}", t.address,
                       vk::to_string(t.format), t.width, t.height, t.tile, t.levels, t.layers);
}

void AddSampled(Entry& entry) {
    for (const auto& s : pending_sampled) {
        if (entry.sampled.size() < 32 &&
            std::ranges::find(entry.sampled, s) == entry.sampled.end()) {
            entry.sampled.push_back(s);
        }
    }
    pending_sampled.clear();
}

void AddShader(Entry& entry, u64 hash) {
    if (entry.shaders.size() < 8 && std::ranges::find(entry.shaders, hash) == entry.shaders.end()) {
        entry.shaders.push_back(hash);
    }
}

void Write(VAddr presented) {
    const char* dir = std::getenv("BB_CAPTURE_DIR");
    // A sequence number too: two records in one second (a scripted sweep) kept apart.
    static u32 written = 0;
    const std::string path = std::format("{}/frame_{}_{:03}.txt", dir ? dir : ".",
                                         static_cast<long long>(std::time(nullptr)), written++);
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) {
        std::printf("Frame capture: cannot write %s\n", path.c_str());
        return;
    }
    std::fprintf(f, "presented buffer %#llx\n", static_cast<unsigned long long>(presented));
    if (!mesh_lines.empty()) {
        std::fprintf(f, "\nmesh draws (BB_CAPTURE_MESH_COUNTS): %zu\n", mesh_lines.size());
        for (const auto& l : mesh_lines) {
            std::fprintf(f, "%s\n", l.c_str());
        }
    }
    mesh_lines.clear();
    u32 index = 0;
    for (const auto& e : entries) {
        if (e.compute) {
            std::fprintf(f, "\n#%u COMPUTE dispatches %u, cs", index++, e.draws);
        } else {
            std::fprintf(f, "\n#%u PASS draws %u (indices %llu), vs/ps", index++, e.draws,
                         static_cast<unsigned long long>(e.indices));
        }
        for (const u64 h : e.shaders) {
            std::fprintf(f, " %016llx", static_cast<unsigned long long>(h));
        }
        std::fprintf(f, "\n");
        for (size_t i = 0; i < e.colors.size(); ++i) {
            std::fprintf(f, "  color%zu %s\n", i, Describe(e.colors[i]).c_str());
        }
        if (!e.compute) {
            std::fprintf(f, "  depth  %s\n", Describe(e.depth).c_str());
        }
        for (const auto& [t, storage] : e.sampled) {
            std::fprintf(f, "  %s %s\n", storage ? "writes " : "samples", Describe(t).c_str());
        }
        if (!e.note.empty()) {
            std::fprintf(f, "  note: %s\n", e.note.c_str());
        }
        for (const auto& b : e.buffers) {
            std::fprintf(f, "%s\n", b.c_str());
        }
        for (const auto& g : e.groups) {
            std::fprintf(f, "  group vs %016llx ps %016llx: draws %u (indices %llu)\n",
                         static_cast<unsigned long long>(g.vs),
                         static_cast<unsigned long long>(g.ps), g.draws,
                         static_cast<unsigned long long>(g.indices));
            for (const auto& [t, storage] : g.sampled) {
                std::fprintf(f, "    %s %s\n", storage ? "writes " : "samples", Describe(t).c_str());
            }
            std::string sizes;
            auto sorted = g.sizes;
            std::ranges::sort(sorted, std::greater{});
            for (const auto& [count, draws] : sorted) {
                sizes += draws > 1 ? std::format(" {}x{}", count, draws) : std::format(" {}", count);
            }
            if (!sizes.empty()) {
                std::fprintf(f, "    index counts%s%s\n", sizes.c_str(),
                             g.sizes.size() >= kMaxSizes ? " ..." : "");
            }
        }
    }
    std::fclose(f);
    std::printf("Frame capture: %zu passes written to %s\n", entries.size(), path.c_str());
}

} // namespace

void FrameCapture::OnFlip(VAddr presented_address) {
    last_presented.store(presented_address, std::memory_order_relaxed);
    flips.fetch_add(1, std::memory_order_release);
    static const char* trigger = std::getenv("BB_CAPTURE_TRIGGER");
    if (trigger && state.load(std::memory_order_relaxed) == Idle &&
        std::filesystem::exists(trigger)) {
        std::error_code ec;
        std::filesystem::remove(trigger, ec);
        state.store(Armed, std::memory_order_release);
        std::printf("Frame capture: armed\n");
    }
    // BB_DRAW_SKIP_FILE: the list of shaders whose draws are skipped, re-read when it changes.
    static const char* skip_file = std::getenv("BB_DRAW_SKIP_FILE");
    static std::filesystem::file_time_type skip_time{};
    if (skip_file) {
        std::error_code ec;
        const auto time = std::filesystem::last_write_time(skip_file, ec);
        if (ec) {
            if (skip_count.exchange(0) != 0) {
                skip_time = {};
                std::printf("Draw skip: none\n");
            }
        } else if (time != skip_time) {
            skip_time = time;
            u32 count = 0;
            std::string list;
            if (FILE* f = std::fopen(skip_file, "r")) {
                unsigned long long h = 0;
                while (count < skip_hashes.size() && std::fscanf(f, "%llx", &h) == 1) {
                    skip_hashes[count++].store(h, std::memory_order_relaxed);
                    list += std::format(" {:016x}", h);
                }
                std::fclose(f);
            }
            skip_count.store(count, std::memory_order_release);
            std::printf("Draw skip: %u shader(s)%s\n", count, list.c_str());
            std::fflush(stdout);
        }
    }
}

void FrameCapture::Poll() {}

bool FrameCapture::IsDisplayBuffer(VAddr address) {
    const u32 count = display_count.load(std::memory_order_acquire);
    for (u32 i = 0; i < count; ++i) {
        if (display_list[i].load(std::memory_order_relaxed) == address) {
            return true;
        }
    }
    if (count < display_list.size()) {
        return false;
    }
    std::scoped_lock lk{display_mutex};
    return std::ranges::find(display_buffers, address) != display_buffers.end();
}

void FrameCapture::AddDisplayBuffer(VAddr address) {
    std::scoped_lock lk{display_mutex};
    if (std::ranges::find(display_buffers, address) == display_buffers.end()) {
        display_buffers.push_back(address);
        const u32 count = display_count.load(std::memory_order_relaxed);
        if (count < display_list.size()) {
            display_list[count].store(address, std::memory_order_relaxed);
            display_count.store(count + 1, std::memory_order_release);
        }
    }
}

void FrameCapture::BeginPass(const VideoCore::ImageInfo* const* colors, u32 num_colors,
                             const VideoCore::ImageInfo* depth) {
    std::vector<Target> targets;
    bool display = false;
    {
        std::scoped_lock lk{display_mutex};
        for (u32 i = 0; i < num_colors; ++i) {
            targets.push_back(colors[i] ? ToTarget(*colors[i]) : Target{});
            display |= colors[i] && std::ranges::find(display_buffers, colors[i]->guest_address) !=
                                        display_buffers.end();
        }
    }
    // Frame boundaries in the command stream: the pass that writes a display buffer.
    const bool new_pass = !(pass_open && !entries.empty() && !entries.back().compute &&
                            entries.back().colors == targets);
    if (display && new_pass) {
        if (state.load(std::memory_order_relaxed) == Recording) {
            Write(last_presented.load(std::memory_order_relaxed));
            entries.clear();
            pending_sampled.clear();
            pending_buffers.clear();
            state.store(Idle, std::memory_order_release);
            return;
        }
        entries.clear();
        pending_sampled.clear();
        pending_buffers.clear();
        pass_open = false;
        state.store(Recording, std::memory_order_release);
    }
    if (state.load(std::memory_order_relaxed) != Recording) {
        pending_sampled.clear();
        pending_buffers.clear();
        return;
    }
    const Target d = depth ? ToTarget(*depth) : Target{};
    if (pass_open && !entries.empty() && !entries.back().compute &&
        entries.back().colors == targets && entries.back().depth == d) {
        return;
    }
    entries.push_back({.compute = false, .colors = std::move(targets), .depth = d});
    pass_open = true;
}

void FrameCapture::Draw(u64 vs_hash, u64 ps_hash, u32 num_indices, u32 num_instances) {
    if (state.load(std::memory_order_relaxed) != Recording || entries.empty() ||
        entries.back().compute) {
        return;
    }
    auto& e = entries.back();
    if (WantsMesh(num_indices) && mesh_lines.size() < kMaxMeshLines) {
        std::string line = std::format("  mesh pass #{} draw {}: indices {} instances {} vs {:016x} ps {:016x}",
                                       entries.size() - 1, e.draws, num_indices, num_instances, vs_hash, ps_hash);
        for (const auto& st : pending_stats) {
            line += "\n" + st;
        }
        mesh_lines.push_back(std::move(line));
    }
    pending_stats.clear();
    ++e.draws;
    e.indices += u64(num_indices) * std::max(num_instances, 1u);
    AddShader(e, vs_hash);
    AddShader(e, ps_hash);
    auto group = std::ranges::find_if(
        e.groups, [&](const Group& g) { return g.vs == vs_hash && g.ps == ps_hash; });
    if (group == e.groups.end()) {
        e.groups.push_back({.vs = vs_hash, .ps = ps_hash, .sampled = pending_sampled});
        group = e.groups.end() - 1;
    }
    ++group->draws;
    group->indices += u64(num_indices) * std::max(num_instances, 1u);
    auto size = std::ranges::find_if(group->sizes, [&](const auto& c) { return c.first == num_indices; });
    if (size != group->sizes.end()) {
        ++size->second;
    } else if (group->sizes.size() < kMaxSizes) {
        group->sizes.emplace_back(num_indices, 1u);
    }
    AddSampled(e);
    AddBuffers(e);
}

void FrameCapture::Dispatch(u64 cs_hash, u32 x, u32 y, u32 z) {
    if (state.load(std::memory_order_relaxed) != Recording) {
        pending_sampled.clear();
        pending_buffers.clear();
        return;
    }
    if (!entries.empty() && entries.back().compute && !entries.back().shaders.empty() &&
        entries.back().shaders[0] == cs_hash) {
        ++entries.back().draws;
    } else {
        entries.push_back({.compute = true, .draws = 1, .shaders = {cs_hash}});
        entries.back().note = std::format("groups {}x{}x{}", x, y, z);
    }
    AddSampled(entries.back());
    AddBuffers(entries.back());
    pass_open = false;
}

bool FrameCapture::WantsMesh(u32 num_indices) {
    static const std::vector<u32> counts = [] {
        std::vector<u32> out;
        if (const char* env = std::getenv("BB_CAPTURE_MESH_COUNTS")) {
            for (const char* p = env; *p;) {
                char* end = nullptr;
                const unsigned long v = std::strtoul(p, &end, 10);
                if (end == p) {
                    ++p;
                    continue;
                }
                out.push_back(u32(v));
                p = end;
            }
        }
        return out;
    }();
    return !counts.empty() && std::ranges::find(counts, num_indices) != counts.end();
}

void FrameCapture::Vertex(u32 stream, VAddr address, const void* data, u64 size, u32 stride) {
    if (state.load(std::memory_order_relaxed) != Recording || pending_stats.size() >= 32 || !data ||
        stride < 12) {
        return;
    }
    const auto* bytes = static_cast<const u8*>(data);
    float lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
    u64 nan = 0, count = size / stride;
    for (u64 i = 0; i < count; ++i) {
        float p[3];
        std::memcpy(p, bytes + i * stride, sizeof p);
        if (std::isnan(p[0]) || std::isnan(p[1]) || std::isnan(p[2])) {
            ++nan;
            continue;
        }
        for (int k = 0; k < 3; ++k) {
            lo[k] = std::min(lo[k], p[k]);
            hi[k] = std::max(hi[k], p[k]);
        }
    }
    pending_stats.push_back(std::format(
        "    vertex stream {} at {:#x} size {} stride {}: {} elements, NaN {}, box ({:.3f} {:.3f} {:.3f}) .. "
        "({:.3f} {:.3f} {:.3f})",
        stream, address, size, stride, count, nan, lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]));
}

void FrameCapture::Buffer(u64 stage_hash, u32 slot, VAddr address, const void* data, u64 size) {
    if (state.load(std::memory_order_relaxed) == Recording && pending_stats.size() < 32 && data) {
        pending_stats.push_back(std::format("    stage {:08x} slot {} at {:#x} size {}: {}", u32(stage_hash), slot,
                                            address, size, FloatStats(data, size & ~u64(3))));
    }
    const u64 bytes = std::min<u64>(size, 1024) & ~u64(3);
    std::string out = std::format("  buffer stage {:016x} slot {} at {:#x} size {}:", stage_hash,
                                  slot, address, size);
    const auto* words = static_cast<const float*>(data);
    for (u64 i = 0; i < bytes / 4; ++i) {
        if (i % 8 == 0) {
            out += std::format("\n    [{:3}]", i);
        }
        out += std::format(" {:12.6g}", words[i]);
    }
    pending_buffers.push_back(std::move(out));
}

void FrameCapture::Sampled(const VideoCore::ImageInfo& info, bool storage) {
    if (pending_sampled.size() < 64) {
        pending_sampled.emplace_back(ToTarget(info), storage);
    }
}

void FrameCapture::Note(const char* text) {
    if (!entries.empty()) {
        entries.back().note += text;
    }
}

} // namespace Vulkan

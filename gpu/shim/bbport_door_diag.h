// bbport door082: diagnostics for the "doorway" frames (first-draw buffer uploads) and the page
// protection changes of every frame. Off by default; BB_DOOR_DIAG=<MiB> turns it on: a line for
// every frame that uploads at least that many MiB into the arena (by binding kind, with the
// largest bindings), and a summary of the protection changes every 600 frames (by origin,
// direction and thread, run sizes, the busiest 4 MiB regions).
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <vector>

extern "C" int runtime_memory_vma_info(uintptr_t address, int* prot, int* type, uintptr_t* end);

namespace BbDoor {

/// 0: off; else the burst threshold in MiB.
inline const std::uint64_t level = [] {
    const char* env = std::getenv("BB_DOOR_DIAG");
    return env ? std::strtoull(env, nullptr, 10) : 0ull;
}();
inline bool On() {
    return level != 0;
}

enum Kind : int { Other = 0, Vertex, Index, Shader, Image, Preupload, Fault, NumKinds };
inline const char* KindName(int kind) {
    static const char* names[] = {"other", "vertex", "index", "shader", "image", "preupload", "fault"};
    return names[kind];
}
/// What the calling thread binds (set by the rasterizer around ObtainBuffer).
inline thread_local int bind_kind = Other;
struct KindScope {
    int previous;
    explicit KindScope(int kind) : previous(bind_kind) {
        bind_kind = kind;
    }
    ~KindScope() {
        bind_kind = previous;
    }
};

/// Who changes page protection on the calling thread: 0 other, 1 texture cache, 2 buffer regions.
inline thread_local int protect_origin = 0;
struct OriginScope {
    int previous;
    explicit OriginScope(int origin) : previous(protect_origin) {
        protect_origin = origin;
    }
    ~OriginScope() {
        protect_origin = previous;
    }
};

struct Tally {
    std::uint64_t calls = 0, pages = 0, ns = 0;
    void Add(std::uint64_t p, std::uint64_t t) {
        ++calls;
        pages += p;
        ns += t;
    }
};
struct State {
    std::mutex mutex;
    // [origin][0 = made writable (untrack), 1 = protected (track)][0 = game thread, 1 = GPU side]
    Tally frame_protect[3][2][2]{}, window_protect[3][2][2]{};
    Tally window_kind_protect[NumKinds]{}; ///< buffer regions protected again on the GPU side, by binding kind
    std::array<std::uint64_t, 8> run_hist{}; // pages: 1, 2-3, 4-7, 8-15, 16-63, 64-255, 256-1023, 1024+
    struct Region {
        std::uint64_t calls[2]{}, pages[2]{};
    };
    std::map<std::uint64_t, Region> regions;
    struct KindSum {
        std::uint64_t calls = 0, bound = 0, uploaded = 0, resident = 0;
    };
    std::array<KindSum, NumKinds> kinds{};
    struct Top {
        std::uint64_t addr, size, uploaded;
        int kind;
    };
    std::vector<Top> top;
    std::uint64_t uploaded = 0, resident = 0;
    std::uint32_t frame = 0, window_frames = 0;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};
inline State& Get() {
    static State state;
    return state;
}

inline void NoteProtect(std::uint64_t address, std::uint64_t pages, bool writable, std::uint64_t ns,
                        bool gpu_side) {
    auto& s = Get();
    std::scoped_lock lk{s.mutex};
    const int origin = protect_origin;
    s.frame_protect[origin][writable ? 0 : 1][gpu_side ? 1 : 0].Add(pages, ns);
    s.window_protect[origin][writable ? 0 : 1][gpu_side ? 1 : 0].Add(pages, ns);
    if (origin == 2 && !writable && gpu_side) {
        s.window_kind_protect[bind_kind].Add(pages, ns);
    }
    const int bucket = pages < 2 ? 0 : pages < 4 ? 1 : pages < 8 ? 2 : pages < 16 ? 3 : pages < 64 ? 4
                     : pages < 256 ? 5 : pages < 1024 ? 6 : 7;
    ++s.run_hist[bucket];
    auto& region = s.regions[address >> 22];
    ++region.calls[writable ? 0 : 1];
    region.pages[writable ? 0 : 1] += pages;
}

/// An arena synchronization of [addr, addr + size) that uploaded `uploaded` bytes.
inline void NoteSync(std::uint64_t addr, std::uint64_t size, std::uint64_t uploaded) {
    auto& s = Get();
    std::scoped_lock lk{s.mutex};
    auto& kind = s.kinds[bind_kind];
    ++kind.calls;
    kind.bound += size;
    kind.uploaded += uploaded;
    s.uploaded += uploaded;
    if (uploaded >= (256u << 10)) {
        s.top.push_back({addr, size, uploaded, bind_kind});
    }
}
/// New arena memory (VRAM blocks) bound for the calling thread's binding.
inline void NoteResident(std::uint64_t bytes) {
    auto& s = Get();
    std::scoped_lock lk{s.mutex};
    s.kinds[bind_kind].resident += bytes;
    s.resident += bytes;
}

inline int VmaType(std::uint64_t address) {
    int prot = 0, type = -1;
    uintptr_t end = 0;
    return runtime_memory_vma_info(address, &prot, &type, &end) ? type : -1;
}

/// Once a frame (the binding thread): the burst line of the frame that ended, the window summary.
inline void FrameTick(std::uint32_t frame) {
    auto& s = Get();
    std::scoped_lock lk{s.mutex};
    static const char* origins[] = {"other", "texture", "region"};
    static const char* dirs[] = {"open", "protect"};
    static const char* threads[] = {"game", "gpu"};
    if (s.uploaded >= (level << 20)) {
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - s.start).count();
        std::string line;
        char text[256];
        std::snprintf(text, sizeof(text), "Door diag burst: frame %u t=%.3f upload %.1f MB, new arena memory %.1f MB;",
                      s.frame, t, s.uploaded / 1048576.0, s.resident / 1048576.0);
        line += text;
        for (int k = 0; k < NumKinds; ++k) {
            const auto& kind = s.kinds[k];
            if (kind.calls == 0) {
                continue;
            }
            std::snprintf(text, sizeof(text), " %s %llu syncs bound %.1f up %.1f new %.1f MB;", KindName(k),
                          (unsigned long long)kind.calls, kind.bound / 1048576.0, kind.uploaded / 1048576.0,
                          kind.resident / 1048576.0);
            line += text;
        }
        line += " protect:";
        for (int o = 0; o < 3; ++o) {
            for (int d = 0; d < 2; ++d) {
                for (int g = 0; g < 2; ++g) {
                    const auto& tally = s.frame_protect[o][d][g];
                    if (tally.calls == 0) {
                        continue;
                    }
                    std::snprintf(text, sizeof(text), " %s/%s/%s %llu calls %llu pages %.2f ms;", origins[o],
                                  dirs[d], threads[g], (unsigned long long)tally.calls,
                                  (unsigned long long)tally.pages, tally.ns / 1e6);
                    line += text;
                }
            }
        }
        std::ranges::sort(s.top, [](const auto& a, const auto& b) { return a.uploaded > b.uploaded; });
        line += " top:";
        for (std::size_t i = 0; i < s.top.size() && i < 10; ++i) {
            const auto& top = s.top[i];
            std::snprintf(text, sizeof(text), " %s %#llx+%#llx up %.2f MB type %d;", KindName(top.kind),
                          (unsigned long long)top.addr, (unsigned long long)top.size,
                          top.uploaded / 1048576.0, VmaType(top.addr));
            line += text;
        }
        std::snprintf(text, sizeof(text), " (%zu syncs of 256 KiB+)", s.top.size());
        line += text;
        std::printf("%s\n", line.c_str());
    }
    for (auto& a : s.frame_protect) {
        for (auto& b : a) {
            for (auto& c : b) {
                c = {};
            }
        }
    }
    s.kinds = {};
    s.top.clear();
    s.uploaded = s.resident = 0;
    s.frame = frame;
    if (++s.window_frames < 600) {
        return;
    }
    std::string line = "Door diag protect (600 frames, per frame):";
    char text[256];
    for (int o = 0; o < 3; ++o) {
        for (int d = 0; d < 2; ++d) {
            for (int g = 0; g < 2; ++g) {
                auto& tally = s.window_protect[o][d][g];
                if (tally.calls == 0) {
                    continue;
                }
                std::snprintf(text, sizeof(text), " %s/%s/%s %.1f calls %.0f pages %.3f ms;", origins[o], dirs[d],
                              threads[g], tally.calls / 600.0, tally.pages / 600.0, tally.ns / 600e6);
                line += text;
                tally = {};
            }
        }
    }
    line += " region/protect/gpu by binding:";
    for (int k = 0; k < NumKinds; ++k) {
        auto& tally = s.window_kind_protect[k];
        if (tally.calls == 0) {
            continue;
        }
        std::snprintf(text, sizeof(text), " %s %.1f calls %.0f pages %.3f ms;", KindName(k), tally.calls / 600.0,
                      tally.pages / 600.0, tally.ns / 600e6);
        line += text;
        tally = {};
    }
    line += " run pages 1/2-3/4-7/8-15/16-63/64-255/256-1023/1024+:";
    for (auto& count : s.run_hist) {
        std::snprintf(text, sizeof(text), " %llu", (unsigned long long)count);
        line += text;
        count = 0;
    }
    std::vector<std::pair<std::uint64_t, State::Region>> busiest(s.regions.begin(), s.regions.end());
    std::ranges::sort(busiest, [](const auto& a, const auto& b) {
        return a.second.calls[0] + a.second.calls[1] > b.second.calls[0] + b.second.calls[1];
    });
    std::snprintf(text, sizeof(text), " regions %zu, busiest:", busiest.size());
    line += text;
    for (std::size_t i = 0; i < busiest.size() && i < 12; ++i) {
        const auto& [key, region] = busiest[i];
        std::snprintf(text, sizeof(text), " %#llx type %d open %llu/%llu protect %llu/%llu;",
                      (unsigned long long)(key << 22), VmaType(key << 22),
                      (unsigned long long)region.calls[0], (unsigned long long)region.pages[0],
                      (unsigned long long)region.calls[1], (unsigned long long)region.pages[1]);
        line += text;
    }
    std::printf("%s\n", line.c_str());
    s.regions.clear();
    s.window_frames = 0;
}

} // namespace BbDoor

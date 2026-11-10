// SPDX-License-Identifier: GPL-3.0-or-later
// bbport: long-session stability fixes in the game's code. Most are from droogie/bbhost,
// GPL-3.0-or-later (each says where); bbhost names Binary Ninja addresses (image + 0x400000),
// this file guest offsets (image + 0): bbhost 0x2aaa860 is guest +0x26aa860 here.
//
// Each site is compared byte for byte with the 1.09 code first; a site that differs (another
// version, or a patch file that changed it) is left alone with a log line. Switches (default on):
//   BB_HEAP_TABLE=0        the SprjMemory heap size growth (Scaleform/MENU; GFX above 1080p)
//   BB_GX_REGION_MB=N      the GX range allocator (default 288; 0 or 160: the game's 160); BB_GX_REGION=0 off
//   BB_FAST_RESPAWN=0      a death respawn's loading screen keeps the game's fixed 12 s hold floor
//                          (BB_FAST_RESPAWN_SECONDS=S: the floor, default 2.0, 0.5..12)
//   BB_GFX_HEAP_MORE_MIB=N the GFX_GraphicsPrivate growth (runtime_memory.c; 0: none)
//   BB_ARENA_PROBE=0       the log when the frame arena allocator (+0xe45560) returns NULL
//   BB_ARENA_GUARD=0       the NULL checks at +0x21d5b35 and +0x2199303 (drop the render command)
//   BB_FRAME_ARENA_KIB=N   the per-frame command allocator behind the frame arenas (game: 256;
//                          default 1024; 0: the game's own)
//   BB_ARENA_FAIL_TEST=N   test aid (off by default): every N-th pass of the +0x2199303 guard
//                          drops its command as if the arena had returned NULL
//   BB_GX_RECLAIM_WAIT=0   the GPU catch-up wait in the GX resource-table block reclaim
//                          (BB_GX_RECLAIM_WAIT_MS: its bound, default 100)
//   BB_FRAME_POOL_FIX=0    the frame loop's pool borrow (+0x1067f60) is the game's own
//   BB_RIBBON_FIX=0        the effect ribbon writers (+0x28ce7b0/+0x28ceec0) are the game's own
//   BB_BLEND_MASK_FIX=0    the blend-state constructor's write mask (+0x2a51640) is not cleared
//   BB_RENDER_RESOURCE_CACHE_MB=N the render resource cache's backing (+0x216b21d; 160..512, default
//                          512 in the port's extra direct memory, BB_DMEM_EXTRA_MB; 160: the game's)
//   BB_KERNEL_CLAMP=0      the blur-kernel base sample count (+0xbbc3e0) is not raised to 3
//   BB_SF_MOVIE_LOCK=0     the game's HUD update (+0x1bf7c20) runs without its movie's lock (the game's own)
//   BB_SF_TEXT_GUARD=0     a text shown before its layout exists is not guarded (+0x12d5e0 / +0x12df80)
//   BB_FMOD_RING_GUARD=0   a sound freed while still in FMOD's sound ring is not taken out first (+0xdab3a0)
//   BB_MOVIE_DRAW_WAIT=0   the attract movie's frame draw / close (+0x28fe7c0 / +0x28fe2e0) do not wait for GXRenderThread
//   BB_HAVOK_EVENT_GUARD=0 a Havok behaviour event of a freed animation (+0x1a21100 / +0xf5bc30) crashes
//                          again instead of being skipped (crash handler: src/probe.c, src/havok_event_guard.h)
//   BB_TASK_STEAL=1        (default off, pending an A/B) render task stealing byte on; 2 = A/B test aid (experiment bit 512)
//   BB_JOB_BARRIER_STEAL=1 (default off; 2: installed, off at start) GX job barrier: the main thread keeps stealing
//   BB_WORKPOOL_NODE_CACHE=1 (default off; 2: installed, off at start) work-pool job list nodes cached per pool
#include "bbport_stability.h"
#include "bbport_toggles.h"
#include "bbport_stability_logic.h"
#include "bbport_ribbons.h"
#include "bbport_scaleform_logic.h"
#include "bbport_fmod_logic.h"
extern "C" {
#include "../../src/havok_event_guard.h"
}

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <thread>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

extern "C" std::uint64_t runtime_gfx_heap_more_mib(void);
extern "C" std::uint64_t runtime_memory_pool_size(void);
extern "C" void runtime_memory_grow_flexible(std::uint64_t bytes);
extern "C" void* runtime_low_map(size_t size, int prot); // src/runtime_memory.c
extern "C" int runtime_extra_direct_map(std::uint64_t size, std::uint64_t alignment, void** out);
extern "C" int runtime_memory_is_mapped(std::uintptr_t address, std::uint64_t size); // src/runtime_memory.c
extern "C" int runtime_guest_mutex_acquire(void** slot, std::uint64_t usec, std::uint64_t* waited_us); // runtime_mutex.c
extern "C" void runtime_guest_mutex_release(void** slot);

namespace Libraries::GnmDriver {
bool GpuCaughtUp();
}

#define BB_SYSV __attribute__((sysv_abi))

namespace BbStability {
namespace {
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using ull = unsigned long long;

u8* image = nullptr;
u64 image_size = 0;

bool On(const char* name) {
    const char* e = std::getenv(name);
    return !(e && e[0] == '0');
}

u64 Rd64(u64 address) {
    u64 v;
    std::memcpy(&v, reinterpret_cast<const void*>(address), 8);
    return v;
}
u32 Rd32(u64 address) {
    u32 v;
    std::memcpy(&v, reinterpret_cast<const void*>(address), 4);
    return v;
}

/// The image holds `bytes` at guest offset `off`.
bool Matches(u64 off, std::initializer_list<u8> bytes) {
    if (off + bytes.size() > image_size) {
        return false;
    }
    return std::memcmp(image + off, bytes.begin(), bytes.size()) == 0;
}

/// A `call rel32` at guest offset `off` to guest offset `target`.
bool CallsTo(u64 off, u64 target) {
    if (off + 5 > image_size || image[off] != 0xe8) {
        return false;
    }
    std::int32_t rel;
    std::memcpy(&rel, image + off + 1, 4);
    return off + 5 + std::int64_t(rel) == target;
}

// ---- Stubs next to the image (rel32 reach) ----
u8* stub_page = nullptr;
std::size_t stub_used = 0;
constexpr std::size_t StubPageSize = 16384;

u8* MapNear(std::size_t size) {
    const u64 base = reinterpret_cast<u64>(image);
#ifdef _WIN32
    // The image is the first block of the runtime's low guest area, which is reserved for the
    // guest as a whole: the next low block lies right after the image, within rel32 reach (as in
    // bbport_game_menu.cpp). Fixed hints around the image all fall into that reservation.
    if (void* p = runtime_low_map(size, 3 /* read | write; VirtualProtect later */)) {
        const u64 at = reinterpret_cast<u64>(p);
        if (at >= base && at + size <= base + (2ull << 30) - (1ull << 20)) {
            return static_cast<u8*>(p);
        }
    }
#endif
    for (u64 k = 1; k <= 64; ++k) {
        for (const u64 hint : {base - k * (16ull << 20), base + image_size + k * (16ull << 20)}) {
#ifdef _WIN32
            void* p = VirtualAlloc(reinterpret_cast<void*>(hint & ~u64(0xffff)), size, MEM_RESERVE | MEM_COMMIT,
                                   PAGE_READWRITE);
            if (p) {
                return static_cast<u8*>(p);
            }
#else
            void* p = mmap(reinterpret_cast<void*>(hint), size, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            if (p != MAP_FAILED) {
                return static_cast<u8*>(p);
            }
#endif
        }
    }
    return nullptr;
}

bool Rel32(u64 from_next, u64 to, std::int32_t& rel) {
    const std::int64_t d = std::int64_t(to) - std::int64_t(from_next);
    if (d < INT32_MIN || d > INT32_MAX) {
        return false;
    }
    rel = std::int32_t(d);
    return true;
}

struct Emitter {
    std::array<u8, 256> code{};
    std::size_t size = 0;
    u64 at = 0; ///< where the stub will be placed
    bool ok = true;
    void Bytes(std::initializer_list<u8> bytes) {
        for (const u8 b : bytes) {
            code[size++] = b;
        }
    }
    void U32(u32 v) {
        std::memcpy(&code[size], &v, 4);
        size += 4;
    }
    void U64(u64 v) {
        std::memcpy(&code[size], &v, 8);
        size += 8;
    }
    /// jmp rel32 / jz rel32 to an absolute address (in the image).
    void JmpTo(u64 target) {
        Bytes({0xe9});
        std::int32_t rel = 0;
        ok &= Rel32(at + size + 4, target, rel);
        U32(u32(rel));
    }
    void SkipRedZone() { Bytes({0x48, 0x8d, 0xa4, 0x24, 0x80, 0xff, 0xff, 0xff}); } // lea rsp, [rsp - 128]
    void BackRedZone() { Bytes({0x48, 0x8d, 0xa4, 0x24, 0x80, 0x00, 0x00, 0x00}); } // lea rsp, [rsp + 128]
    /// Saves the caller-saved registers (the handler may change them), aligns the stack.
    void SaveVolatile() {
        Bytes({0x50, 0x51, 0x52, 0x56, 0x57, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53});
        Bytes({0x41, 0x54});             // push r12 (holds the unaligned stack pointer)
        Bytes({0x49, 0x89, 0xe4});       // mov r12, rsp
        Bytes({0x48, 0x83, 0xe4, 0xf0}); // and rsp, -16
    }
    void CallAbs(const void* fn) {
        Bytes({0x48, 0xb8}); // movabs rax, fn
        U64(reinterpret_cast<u64>(fn));
        Bytes({0xff, 0xd0}); // call rax
    }
    void RestoreVolatile() {
        Bytes({0x4c, 0x89, 0xe4}); // mov rsp, r12
        Bytes({0x41, 0x5c});       // pop r12
        Bytes({0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5f, 0x5e, 0x5a, 0x59, 0x58});
    }
    /// jmp [rip]; dq fn - an absolute jump.
    void JmpAbs(const void* fn) {
        Bytes({0xff, 0x25, 0x00, 0x00, 0x00, 0x00});
        U64(reinterpret_cast<u64>(fn));
    }
};

/// A new emitter for the next stub in the page.
Emitter NewStub() {
    Emitter e;
    e.at = reinterpret_cast<u64>(stub_page + stub_used);
    e.ok = stub_page != nullptr;
    return e;
}

/// Copies `e` into the stub page; returns its address, or 0.
u64 PlaceStub(const Emitter& e) {
    if (!e.ok || !stub_page || stub_used + e.size > StubPageSize ||
        e.at != reinterpret_cast<u64>(stub_page + stub_used)) {
        return 0;
    }
    u8* at = stub_page + stub_used;
    std::memcpy(at, e.code.data(), e.size);
    stub_used += (e.size + 15) & ~std::size_t(15);
    return reinterpret_cast<u64>(at);
}

/// Writes `jmp stub` (or `call stub`, opcode 0xe8) at guest offset `off`, `nops` more bytes after.
bool WriteBranch(u64 off, u8 opcode, u64 stub, std::size_t pad) {
    std::int32_t rel = 0;
    const u64 address = reinterpret_cast<u64>(image + off);
    if (!stub || !Rel32(address + 5, stub, rel)) {
        return false;
    }
    image[off] = opcode;
    std::memcpy(image + off + 1, &rel, 4);
    // NOPs for the rest of the instructions replaced (never executed: the jump leaves first).
    for (std::size_t i = 0; i < pad; ++i) {
        image[off + 5 + i] = 0x90;
    }
    return true;
}

// ======================================================================================
// 2. SprjMemory heap sizes (bbhost src/engine/menu_memory.cpp and live_resolution.cpp).
// The size table at guest +0x4736d40 is [mode][16] qwords, 0x80 a row. Scaleform runs on two
// Dantelion heaps carved from MENU (row 2 entry 9, flexible memory) when it starts: the large
// block heap (mov edx at +0x1f5a465, 18 MiB) and the page heap (mov edx at +0x1f5a479, 34 MiB).
// When they ran out the allocator returned NULL and the next constructor wrote through it. Both
// grow (+32 and +64 MiB) and MENU by their sum, so what MENU has spare stays as it was.
// Render targets come from GFX_GraphicsPrivate (row 1 entry 3, 2287 MiB): above 1080p it grows
// by runtime_gfx_heap_more_mib() unless a patch (patches/Bloodborne.xml "Increased Graphics Heap
// Sizes", which patches.py applies above 1080p) already changed it.
// ======================================================================================
constexpr u64 kMiB = 1024 * 1024;
constexpr u64 kSizeTable = 0x4736d40;
constexpr u64 kMenuSize = kSizeTable + 2 * 0x80 + 9 * 8;
constexpr u64 kMenuStock = 0x5200000;
constexpr u64 kGfxPrivate = kSizeTable + 1 * 0x80 + 3 * 8;
constexpr u64 kGfxPrivateStock = 0x8ef00000ull;
constexpr u64 kBlockHeapSize = 0x1f5a465, kPageHeapSize = 0x1f5a479;
constexpr u32 kBlockHeapStock = 0x1200000, kPageHeapStock = 0x2200000;
constexpr u32 kBlockHeapMore = 32 * kMiB, kPageHeapMore = 64 * kMiB;

bool PatchMovEdx(u64 off, u32 stock, u32 want) {
    u32 imm = 0;
    std::memcpy(&imm, image + off + 1, 4);
    if (image[off] != 0xba || imm != stock) {
        std::printf("Stability: heap table: guest +0x%llx is not mov edx, 0x%x; left alone\n", ull(off), stock);
        return false;
    }
    std::memcpy(image + off + 1, &want, 4);
    return true;
}

std::string HeapTable() {
    if (!On("BB_HEAP_TABLE")) {
        return "heap-table off";
    }
    std::string result;
    // Scaleform's heaps and MENU.
    u64 menu = 0;
    std::memcpy(&menu, image + kMenuSize, 8);
    if (menu != kMenuStock) {
        std::printf("Stability: heap table: MENU's size at guest +0x%llx is 0x%llx, not 0x%llx; Scaleform heaps left "
                    "alone\n",
                    ull(kMenuSize), ull(menu), ull(kMenuStock));
        result = "menu-heap skipped";
    } else {
        const bool block = PatchMovEdx(kBlockHeapSize, kBlockHeapStock, kBlockHeapStock + kBlockHeapMore);
        const bool page = PatchMovEdx(kPageHeapSize, kPageHeapStock, kPageHeapStock + kPageHeapMore);
        const u64 more = (block ? kBlockHeapMore : 0) + (page ? kPageHeapMore : 0);
        menu = kMenuStock + more;
        std::memcpy(image + kMenuSize, &menu, 8);
        runtime_memory_grow_flexible(more); // MENU is flexible memory
        std::printf("Stability: heap table: Scaleform heaps %llu and %llu MiB, MENU %llu MiB (were 18, 34 and 82; "
                    "flexible memory +%llu MiB)\n",
                    ull((kBlockHeapStock + (block ? kBlockHeapMore : 0)) / kMiB),
                    ull((kPageHeapStock + (page ? kPageHeapMore : 0)) / kMiB), ull(menu / kMiB), ull(more / kMiB));
        result = more ? "menu-heap +" + std::to_string(more / kMiB) + "MiB" : "menu-heap skipped";
    }
    // GFX_GraphicsPrivate.
    u64 gfx = 0;
    std::memcpy(&gfx, image + kGfxPrivate, 8);
    u64 more = runtime_gfx_heap_more_mib();
    if (gfx != kGfxPrivateStock) {
        std::printf("Stability: heap table: GFX_GraphicsPrivate is already %llu MiB (a patch: Increased Graphics Heap "
                    "Sizes); left as it is\n",
                    ull(gfx / kMiB));
        return result + ", gfx-heap by patch (" + std::to_string(gfx / kMiB) + "MiB)";
    }
    if (!more) {
        return result + ", gfx-heap stock";
    }
    // The stock heaps fit in 5 GiB of direct memory with ~1 GiB spare at 6 GiB (bbhost): what
    // fits in the pool this run has (BB_DMEM_MB may set less).
    const u64 pool_mib = runtime_memory_pool_size() / kMiB;
    const u64 fits = pool_mib > 5120 ? (pool_mib - 5120) / 64 * 64 : 0;
    if (more > fits) {
        std::printf("Stability: heap table: GFX_GraphicsPrivate +%llu MiB does not fit in %llu MiB of direct memory; "
                    "+%llu MiB\n",
                    ull(more), ull(pool_mib), ull(fits));
        more = fits;
    }
    if (!more) {
        return result + ", gfx-heap stock (no room)";
    }
    gfx = kGfxPrivateStock + (more << 20);
    std::memcpy(image + kGfxPrivate, &gfx, 8);
    std::printf("Stability: heap table: GFX_GraphicsPrivate %llu -> %llu MiB (direct memory %llu MiB)\n",
                ull(kGfxPrivateStock / kMiB), ull(gfx / kMiB), ull(pool_mib));
    return result + ", gfx-heap +" + std::to_string(more) + "MiB";
}

// The GX range allocator (BB_GX_REGION_MB, default 288 MiB; <=160 or 0: the game's). Ported from
// Nightwatch c6305d7 (engine/start/patches.cpp), Lantern Engine by Awaken
// (https://github.com/Awakenchan/LanternEngine; GPL-3.0-or-later with its section 7 additional terms,
// see its NOTICE: attribution kept, marked as modified), modified: it is applied here to the loaded image,
// after HeapTable(), and GFX_GraphicsPrivate grows only when HeapTable() (or a patch) left it at the
// stock size, so the heap is never grown twice. The region is the GX manager's default 128 MiB
// (mov dword [rbx+0x88], 0x8000000 at +0x1d9933d) plus 32 MiB; Ebrietas' trail effect took it to
// 0.5 MB free before her device losses. Its +128 MiB comes from the 6 GiB direct memory (the stock
// heaps end near 4.6 GB; the direct-memory pool is not resized).
constexpr u64 kGxRegionSite = 0x1d9933d;

std::string GxRegion() {
    if (!On("BB_GX_REGION")) {
        return "gx-region off";
    }
    const unsigned mib = BbStability::GxRegionApplied(BbStability::GxRegionRequest(std::getenv("BB_GX_REGION_MB")));
    if (!mib) {
        std::printf("Stability: GX range allocator at the game's %u MiB (BB_GX_REGION_MB)\n", BbStability::kGxRegionGame);
        return "gx-region stock";
    }
    u64 gfx = 0;
    std::memcpy(&gfx, image + kGfxPrivate, 8);
    const bool heap_stock = gfx == kGfxPrivateStock;
    const u64 more = BbStability::GxRegionHeapMore(mib, heap_stock);
    // Direct memory: the stock heaps end near 4.6 GB, so a 5056 MiB pool holds the growth; a heap
    // another growth enlarged already carries it.
    if (!Matches(kGxRegionSite, {0xc7, 0x83, 0x88, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08}) ||
        (heap_stock && runtime_memory_pool_size() / kMiB < 5056 + more)) {
        std::printf("Stability: GX range allocator left at the game's size: this executable differs at guest +0x%llx, "
                    "or the direct memory is too small\n",
                    ull(kGxRegionSite));
        return "gx-region skipped";
    }
    const std::uint32_t imm = BbStability::GxRegionImmediate(mib);
    std::memcpy(image + kGxRegionSite + 6, &imm, 4);
    if (more) {
        gfx = kGfxPrivateStock + (more << 20);
        std::memcpy(image + kGfxPrivate, &gfx, 8);
    }
    std::printf("Stability: GX range allocator %u MiB (the game's %u), GFX_GraphicsPrivate %llu MiB%s\n", mib,
                BbStability::kGxRegionGame, ull(gfx / kMiB), more ? "" : " (already enlarged: not grown again)");
    return "gx-region " + std::to_string(mib) + "MiB";
}

// The death respawn hold (BB_FAST_RESPAWN, default on; BB_FAST_RESPAWN_SECONDS, default 2.0). MoveMapStep
// step 0 (+0x1938870) loads its hold timer +0x278 from the rodata float at +0x49280c8 (12.0) when the load
// request has the "player died" byte; the screen gate (step 12, +0x193a3e0) keeps the loading screen up while
// it is above zero (research/remp081.md 3.3). The float has one reader (the load at +0x1938b68), so the
// value in the loaded image is replaced; the reader is checked to be a rip-relative load of it first.
// kRespawnReader is the load's rip-relative displacement (4 bytes 5c f5 fe 02 -> +0x49280c8).
constexpr u64 kRespawnFloat = 0x49280c8, kRespawnReader = 0x1938b68;

std::string FastRespawn() {
    if (!On("BB_FAST_RESPAWN")) {
        return "fast-respawn off";
    }
    const float secs = BbStability::RespawnSeconds(std::getenv("BB_FAST_RESPAWN_SECONDS"));
    bool reader = false;
    if (kRespawnReader + 16 <= image_size) {
        for (u64 i = 0; i <= 8 && !reader; ++i) {
            std::int32_t disp;
            std::memcpy(&disp, image + kRespawnReader + i, 4);
            reader = kRespawnReader + i + 4 + std::int64_t(disp) == kRespawnFloat; // movss/mov reg,[rip+disp]
        }
    }
    if (!reader || !Matches(kRespawnFloat, {0x00, 0x00, 0x40, 0x41})) { // 12.0f
        std::printf("Stability: death respawn hold left at the game's 12 s: this executable differs at guest "
                    "+0x%llx (reader) or +0x%llx (12.0f)\n",
                    ull(kRespawnReader), ull(kRespawnFloat));
        if (kRespawnReader + 16 <= image_size && kRespawnFloat + 4 <= image_size) {
            std::printf("Stability:   reader bytes %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x, float bytes %02x %02x %02x %02x\n",
                        image[kRespawnReader], image[kRespawnReader + 1], image[kRespawnReader + 2],
                        image[kRespawnReader + 3], image[kRespawnReader + 4], image[kRespawnReader + 5],
                        image[kRespawnReader + 6], image[kRespawnReader + 7], image[kRespawnReader + 8],
                        image[kRespawnReader + 9], image[kRespawnFloat], image[kRespawnFloat + 1],
                        image[kRespawnFloat + 2], image[kRespawnFloat + 3]);
        } else {
            std::printf("Stability:   image size 0x%llx does not reach the sites\n", ull(image_size));
        }
        return "fast-respawn skipped";
    }
    std::memcpy(image + kRespawnFloat, &secs, 4);
    std::printf("Stability: death respawn loading screen hold %.2f s (the game's 12 s; BB_FAST_RESPAWN=0 restores)\n",
                double(secs));
    return "fast-respawn " + std::to_string(secs).substr(0, 4) + "s";
}

// ======================================================================================
// 3. The frame arena allocator's NULL. +0x21d5a80 (a render command's copy) takes 0x4d0 bytes
// from the frame context's arena (+0xe45560(frameCtx + 0x78, 0x4d0, 16)) and copies the command
// into them with no NULL check. +0xe45560 is a chunked bump allocator (+0 backing allocator,
// +8 min chunk, +0xc alignment, +0x10 chunks, +0x14 allocations, +0x18 chunk, +0x20 end,
// +0x28 cursor); it returns NULL when the backing allocator's vtbl[+0x58] does.
// Probe (the idea of bbhost's src/engine/sf_heap_probe.cpp): the failure path at +0xe455f7 logs
// the arena and its backing heap; for CSGraphicsPrivateAllocator (vtable guest +0x53aec90) also
// its two heaps' free and largest block (heap vtable +0x30, +0x38; bbhost live_resolution.cpp).
// Guard: after the arena lock is released (+0x21d5b35, r13 = the allocation) a NULL skips the
// copy and the command's submission (+0x21b4b00, a tail call) and returns (the epilogue at
// +0x21d5aca, which the function's early exit uses as well): the command is dropped for a frame.
// ======================================================================================
constexpr u64 kArenaFailPath = 0xe455f7;
constexpr u64 kArenaFailNext = 0xe45603;
constexpr u64 kGuardSite = 0x21d5b35;
constexpr u64 kGuardNext = 0x21d5b3a;
constexpr u64 kGuardExit = 0x21d5aca;
constexpr u64 kGraphicsPrivateAllocatorVtable = 0x53aec90;
constexpr u64 kFrameLinearVtable = 0x52e6fd0;

std::atomic<u64> arena_nulls{0}, commands_dropped{0}, frame_high_water{0};

std::string GuestName(u64 address) {
    char text[48];
    const u64 base = reinterpret_cast<u64>(image);
    if (address >= base && address < base + image_size) {
        std::snprintf(text, sizeof text, "guest +0x%llx", ull(address - base));
    } else {
        std::snprintf(text, sizeof text, "0x%llx", ull(address));
    }
    return text;
}

using HeapQuery = u64(BB_SYSV*)(u64 heap);

BB_SYSV void ArenaNull(u64 arena, u64 size, u64 chunk) {
    const u64 n = arena_nulls.fetch_add(1, std::memory_order_relaxed);
    if (n >= 8 && (n & 1023) != 0) {
        return;
    }
    const u64 backing = Rd64(arena);
    const u64 vtable = backing ? Rd64(backing) : 0;
    const u64 alloc_fn = vtable ? Rd64(vtable + 0x58) : 0;
    std::printf("Stability: arena allocator (guest +0xe45560) returned NULL #%llu: arena 0x%llx, request 0x%llx "
                "(chunk 0x%llx; min chunk 0x%x, %u chunks, %u allocations); backing allocator 0x%llx, vtable %s, "
                "alloc %s\n",
                ull(n + 1), ull(arena), ull(size), ull(chunk), Rd32(arena + 8), Rd32(arena + 0x10),
                Rd32(arena + 0x14), ull(backing), GuestName(vtable).c_str(), GuestName(alloc_fn).c_str());
    if (vtable == reinterpret_cast<u64>(image) + kFrameLinearVtable) {
        // The frame's linear allocator (+0xe453b0 -> +0x2085120): +8 base, +0x18 cursor, +0x20 end,
        // +0x30 blocks handed out (reset at the frame flip).
        const u64 base = Rd64(backing + 8), cursor = Rd64(backing + 0x18), end = Rd64(backing + 0x20);
        std::printf("Stability:   frame allocator: %llu of %llu KiB used, %u blocks; fullest frame so far %llu KiB\n",
                    ull((cursor - base) >> 10), ull((end - base) >> 10), Rd32(backing + 0x30),
                    ull(frame_high_water.load() >> 10));
    }
    if (vtable == reinterpret_cast<u64>(image) + kGraphicsPrivateAllocatorVtable) {
        const char* names[2] = {"GFX_GraphicsPrivate", "GFX_GraphicsPrivateB"};
        for (int i = 0; i < 2; ++i) {
            const u64 heap = Rd64(backing + 8 + 8 * i);
            if (!heap) {
                continue;
            }
            const u64 hv = Rd64(heap);
            const u64 free_bytes = reinterpret_cast<HeapQuery>(Rd64(hv + 0x30))(heap);
            const u64 largest = reinterpret_cast<HeapQuery>(Rd64(hv + 0x38))(heap);
            std::printf("Stability:   %s heap 0x%llx: %llu MiB free, largest block %llu KiB\n", names[i], ull(heap),
                        ull(free_bytes >> 20), ull(largest >> 10));
        }
    }
    std::fflush(stdout);
}

BB_SYSV void CommandDropped(u64 frame_ctx, u64 command, u64 site) {
    const u64 n = commands_dropped.fetch_add(1, std::memory_order_relaxed);
    if (n < 8 || (n & 1023) == 0) {
        std::printf("Stability: render command 0x%llx dropped (#%llu): frame context 0x%llx's arena returned NULL "
                    "(guest +0x%llx)\n",
                    ull(command), ull(n + 1), ull(frame_ctx), ull(site));
        std::fflush(stdout);
    }
}

bool InstallArenaProbe() {
    // je to the failure path, the path itself: add rbx, 0x20; mov qword [rbx + 8], 0; and its
    // last store (mov qword [rbx], 0), where the stub returns.
    if (!Matches(0xe455c2, {0x48, 0x89, 0x43, 0x18, 0x48, 0x85, 0xc0, 0x74, 0x2c}) ||
        !Matches(kArenaFailPath, {0x48, 0x83, 0xc3, 0x20, 0x48, 0xc7, 0x43, 0x08, 0x00, 0x00, 0x00, 0x00, 0x48, 0xc7,
                                  0x03, 0x00, 0x00, 0x00, 0x00})) {
        std::printf("Stability: arena probe: guest +0x%llx is not the expected code; left alone\n",
                    ull(kArenaFailPath));
        return false;
    }
    Emitter e = NewStub();
    e.SkipRedZone();
    e.SaveVolatile();
    e.Bytes({0x48, 0x89, 0xdf}); // mov rdi, rbx (the arena)
    e.Bytes({0x4c, 0x89, 0xf6}); // mov rsi, r14 (the size asked for)
    e.Bytes({0x4c, 0x89, 0xfa}); // mov rdx, r15 (the chunk asked of the backing allocator)
    e.CallAbs(reinterpret_cast<const void*>(&ArenaNull));
    e.RestoreVolatile();
    e.BackRedZone();
    e.Bytes({0x48, 0x83, 0xc3, 0x20});                         // add rbx, 0x20
    e.Bytes({0x48, 0xc7, 0x43, 0x08, 0x00, 0x00, 0x00, 0x00}); // mov qword [rbx + 8], 0
    e.JmpTo(reinterpret_cast<u64>(image) + kArenaFailNext);
    return WriteBranch(kArenaFailPath, 0xe9, PlaceStub(e), 12 - 5);
}

bool InstallArenaGuard() {
    // call 0xe45560; mov r13, rax; (unlock) mov rax, [r14 + 0xc8]; mov rdi, r12; call [rax + 0x28];
    // mov edx, 0x398; mov rdi, r13; mov rsi, rbx; call memcpy - and the epilogue.
    if (!Matches(0x21d5b20, {0xe8, 0x3b, 0xfa, 0xc6, 0xfe, 0x49, 0x89, 0xc5, 0x49, 0x8b, 0x86, 0xc8, 0x00, 0x00, 0x00,
                             0x4c, 0x89, 0xe7, 0xff, 0x50, 0x28, 0xba, 0x98, 0x03, 0x00, 0x00, 0x4c, 0x89, 0xef, 0x48,
                             0x89, 0xde, 0xe8, 0x33, 0x8a, 0x9e, 0x00}) ||
        !Matches(kGuardExit, {0x48, 0x83, 0xc4, 0x08, 0x5b, 0x41, 0x5c, 0x41, 0x5d, 0x41, 0x5e, 0x41, 0x5f, 0x5d, 0xc3}) ||
        !Matches(0x21d5a80, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x50})) {
        std::printf("Stability: arena guard: guest +0x%llx is not the expected code; left alone\n", ull(kGuardSite));
        return false;
    }
    Emitter e = NewStub();
    e.Bytes({0x4d, 0x85, 0xed});                   // test r13, r13
    e.Bytes({0x74, 0x0a});                         // jz .null
    e.Bytes({0xba, 0x98, 0x03, 0x00, 0x00});       // mov edx, 0x398
    e.JmpTo(reinterpret_cast<u64>(image) + kGuardNext);
    // .null: the stack is as after the prologue (push rbp .. push rax); the epilogue returns.
    e.SkipRedZone();
    e.SaveVolatile();
    e.Bytes({0x4c, 0x89, 0xf7}); // mov rdi, r14 (the frame context)
    e.Bytes({0x48, 0x89, 0xde}); // mov rsi, rbx (the command)
    e.Bytes({0xba});             // mov edx, the call site
    e.U32(0x21d5b20);
    e.CallAbs(reinterpret_cast<const void*>(&CommandDropped));
    e.RestoreVolatile();
    e.BackRedZone();
    e.JmpTo(reinterpret_cast<u64>(image) + kGuardExit);
    return WriteBranch(kGuardSite, 0xe9, PlaceStub(e), 0);
}

// ======================================================================================
// 3b. The second unchecked arena allocation, in FUN_02198fd0 (the frame flip / post-process command
// builder; playtest 1 crash at +0x2199310): +0x2199266 takes 0x20 bytes and checks them, +0x21992fb
// takes 0x10 and writes through them at once (`mov [r12 + 8], r13` after the unlock). The guard
// replaces the unlock's first two instructions (+0x2199303, 10 bytes): a NULL runs the unlock and
// skips the command's fill and submission (+0x21b4b00) to +0x2199346, the code after it.
// ======================================================================================
constexpr u64 kGuard2Site = 0x2199303;
constexpr u64 kGuard2Call = 0x219930d; // call [rax + 0x28], the unlock
constexpr u64 kGuard2Skip = 0x2199346; // mov byte [r15 + 0x3ab], 0: after the submission
std::atomic<u64> fail_test_passes{0};
u64 fail_test_every = 0;
volatile u8 fail_test_now = 0;

BB_SYSV void ArenaFailTest() {
    const u64 n = fail_test_passes.fetch_add(1, std::memory_order_relaxed);
    fail_test_now = FailTestHit(n, fail_test_every) ? 1 : 0;
}

bool InstallArenaGuard2() {
    // lea rdi, [rbx + 0x78]; mov esi, 0x10; mov edx, 0x10; call 0xe45560; mov r12, rax;
    // mov rax, [rbx + 0xc8]; mov rdi, r14; call [rax + 0x28]; mov [r12 + 8], r13 ... mov rdx, r12;
    // call 0x21b4b00; mov byte [r15 + 0x3ab], 0.
    if (!Matches(0x21992ed, {0x48, 0x8d, 0x7b, 0x78, 0xbe, 0x10, 0x00, 0x00, 0x00, 0xba, 0x10, 0x00, 0x00, 0x00}) ||
        !CallsTo(0x21992fb, 0xe45560) ||
        !Matches(0x2199300, {0x49, 0x89, 0xc4, 0x48, 0x8b, 0x83, 0xc8, 0x00, 0x00, 0x00, 0x4c, 0x89, 0xf7, 0xff, 0x50,
                             0x28, 0x4d, 0x89, 0x6c, 0x24, 0x08}) ||
        !Matches(0x219933e, {0x4c, 0x89, 0xe2}) || !CallsTo(0x2199341, 0x21b4b00) ||
        !Matches(kGuard2Skip, {0x41, 0xc6, 0x87, 0xab, 0x03, 0x00, 0x00, 0x00})) {
        std::printf("Stability: arena guard: guest +0x%llx is not the expected code; left alone\n", ull(kGuard2Site));
        return false;
    }
    if (const char* e = std::getenv("BB_ARENA_FAIL_TEST")) {
        fail_test_every = std::strtoull(e, nullptr, 10);
    }
    Emitter e = NewStub();
    std::size_t jz_null = 0, jnz_null = 0;
    if (fail_test_every) {
        e.SkipRedZone();
        e.SaveVolatile();
        e.CallAbs(reinterpret_cast<const void*>(&ArenaFailTest));
        e.RestoreVolatile();
        e.BackRedZone();
        e.Bytes({0x49, 0xbb}); // movabs r11, &fail_test_now (r11 is dead here: between two calls)
        e.U64(reinterpret_cast<u64>(&fail_test_now));
        e.Bytes({0x41, 0x80, 0x3b, 0x00}); // cmp byte [r11], 0
        e.Bytes({0x75, 0x00});             // jnz .null
        jnz_null = e.size - 1;
    }
    e.Bytes({0x4d, 0x85, 0xe4}); // test r12, r12
    e.Bytes({0x74, 0x00});       // jz .null
    jz_null = e.size - 1;
    e.Bytes({0x48, 0x8b, 0x83, 0xc8, 0x00, 0x00, 0x00}); // mov rax, [rbx + 0xc8]
    e.Bytes({0x4c, 0x89, 0xf7});                         // mov rdi, r14
    e.JmpTo(reinterpret_cast<u64>(image) + kGuard2Call);
    // .null: unlock, log, skip the fill and the submission. The stack is the function's, as at the call.
    const std::size_t null_at = e.size;
    e.code[jz_null] = u8(null_at - (jz_null + 1));
    if (jnz_null) {
        e.code[jnz_null] = u8(null_at - (jnz_null + 1));
    }
    e.Bytes({0x48, 0x8b, 0x83, 0xc8, 0x00, 0x00, 0x00}); // mov rax, [rbx + 0xc8]
    e.Bytes({0x4c, 0x89, 0xf7});                         // mov rdi, r14
    e.Bytes({0xff, 0x50, 0x28});                         // call [rax + 0x28]
    e.SkipRedZone();
    e.SaveVolatile();
    e.Bytes({0x48, 0x89, 0xdf}); // mov rdi, rbx (the frame context)
    e.Bytes({0x4c, 0x89, 0xe6}); // mov rsi, r12 (the allocation: 0, or the fail test's)
    e.Bytes({0xba});             // mov edx, the call site
    e.U32(0x21992fb);
    e.CallAbs(reinterpret_cast<const void*>(&CommandDropped));
    e.RestoreVolatile();
    e.BackRedZone();
    e.JmpTo(reinterpret_cast<u64>(image) + kGuard2Skip);
    return WriteBranch(kGuard2Site, 0xe9, PlaceStub(e), 10 - 5);
}

// ======================================================================================
// 3c. Why the arena ran dry (playtest 1): each frame context's arenas (+0x48, +0x78; 2 KiB chunks)
// take their chunks from a linear allocator (vtable +0x52e6fd0) of 256 KiB, one per frame context
// (renderer +0x370 / +0x378, made by FUN_02194580 with `mov edx, 0x40000` -> +0xe45200) and reset
// at each frame flip in FUN_02198fd0 (vtbl+0x78 at +0x2199134, +0x2199587, +0x21999a3). Once a
// frame has used it up, every arena allocation fails for the rest of that frame. The size grows
// (BB_FRAME_ARENA_KIB), and the flip notes how full the fullest frame got.
// ======================================================================================
constexpr std::array<u64, 2> kFrameArenaSizes = {0x2195c09, 0x2195c46};
constexpr std::array<u64, 3> kFrameFlips = {0x2199134, 0x2199587, 0x21999a3};
u64 frame_arena_bytes = kFrameArenaStock;
std::atomic<u64> frame_fill_logged{0};

BB_SYSV void FrameFlip(u64 linear) {
    if (!linear) {
        return;
    }
    const u64 base = Rd64(linear + 8), used = Rd64(linear + 0x18) - base, capacity = Rd64(linear + 0x20) - base;
    if (used <= frame_high_water.load(std::memory_order_relaxed)) {
        return;
    }
    frame_high_water.store(used, std::memory_order_relaxed);
    u64 logged = frame_fill_logged.load(std::memory_order_relaxed);
    if (HighWaterNews(used, logged) && frame_fill_logged.compare_exchange_strong(logged, used)) {
        std::printf("Stability: frame allocator: the fullest frame so far used %llu of %llu KiB (%u/8; %u blocks)\n",
                    ull(used >> 10), ull(capacity >> 10), FillEighths(used, capacity), Rd32(linear + 0x30));
        std::fflush(stdout);
    }
}

std::string FrameArena() {
    const u64 want = FrameArenaBytes(std::getenv("BB_FRAME_ARENA_KIB"));
    for (const u64 site : kFrameArenaSizes) {
        if (!Matches(site, {0xba, 0x00, 0x00, 0x04, 0x00}) || !CallsTo(site + 5, 0xe45200)) {
            std::printf("Stability: frame allocator: guest +0x%llx is not mov edx, 0x40000; left alone\n", ull(site));
            return "frame-arena stock";
        }
    }
    if (want != kFrameArenaStock) {
        const u32 imm = u32(want);
        for (const u64 site : kFrameArenaSizes) {
            std::memcpy(image + site + 1, &imm, 4);
        }
        frame_arena_bytes = want;
    }
    return "frame-arena " + std::to_string(frame_arena_bytes >> 10) + "KiB";
}

bool InstallFrameFlipProbe() {
    for (const u64 site : kFrameFlips) {
        // mov rdi, [r15 + rax*8 + 0x370]; mov rax, [rdi]; call [rax + 0x78]
        if (!Matches(site, {0x49, 0x8b, 0xbc, 0xc7, 0x70, 0x03, 0x00, 0x00, 0x48, 0x8b, 0x07, 0xff, 0x50, 0x78})) {
            std::printf("Stability: frame allocator probe: guest +0x%llx is not the expected code; left alone\n",
                        ull(site));
            return false;
        }
    }
    bool ok = true;
    for (const u64 site : kFrameFlips) {
        Emitter e = NewStub();
        e.Bytes({0x49, 0x8b, 0xbc, 0xc7, 0x70, 0x03, 0x00, 0x00}); // mov rdi, [r15 + rax*8 + 0x370]
        e.SkipRedZone();
        e.SaveVolatile();
        e.CallAbs(reinterpret_cast<const void*>(&FrameFlip));
        e.RestoreVolatile();
        e.BackRedZone();
        e.JmpTo(reinterpret_cast<u64>(image) + site + 8);
        ok = ok && WriteBranch(site, 0xe9, PlaceStub(e), 8 - 5);
    }
    return ok;
}

// ======================================================================================
// 4. The GX draw-resource-table block reclaim (from bbhost src/decomp/gx/block_reclaim.cpp).
// GX draws carve their T#/S#/V# tables from 64 KiB blocks; a block's status turns 4 once the GPU
// has finished the work that used it, and +0x26aa860(pool, cb) walks the used blocks and frees
// those. The two allocators call it when no block has room (+0x26ade22, +0x26d0d92) and fail if
// nothing came free (a draw with its tables at address 0, or a failed allocation). Our command
// processor and GPU run further behind than the console's: when the walk frees nothing, wait
// (bounded) for the GPU to catch up and walk again.
// ======================================================================================
constexpr u64 kReclaim = 0x26aa860;
constexpr std::array<u64, 2> kReclaimCalls = {0x26ade22, 0x26d0d92};
using ReclaimFn = void(BB_SYSV*)(u64 pool, u64 cb);
ReclaimFn game_reclaim = nullptr;
std::chrono::microseconds reclaim_budget{100000};
std::atomic<u64> reclaim_stuck{0}, reclaim_freed{0}, reclaim_failed{0}, reclaim_skipped{0}, reclaim_waited_us{0};
std::atomic<std::int64_t> reclaim_failed_at{INT64_MIN / 2};

u64 InUse(u64 pool) {
    return *reinterpret_cast<const volatile u64*>(pool + 0xc8);
}

BB_SYSV void Reclaim(u64 pool, u64 cb) {
    const u64 before = InUse(pool);
    game_reclaim(pool, cb);
    if (before == 0 || InUse(pool) < before) {
        return;
    }
    const u64 n = reclaim_stuck.fetch_add(1, std::memory_order_relaxed);
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    const std::int64_t now_us =
        std::chrono::duration_cast<std::chrono::microseconds>(t0.time_since_epoch()).count();
    // A wait that freed nothing less than 250 ms ago: the blocks wait on work the game has not
    // submitted yet; waiting again would only stall each draw.
    if (now_us - reclaim_failed_at.load(std::memory_order_relaxed) < 250000) {
        reclaim_skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    bool freed = false;
    while (true) {
        const bool caught_up = Libraries::GnmDriver::GpuCaughtUp();
        if (!caught_up) {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        game_reclaim(pool, cb);
        if (InUse(pool) < before) {
            freed = true;
            break;
        }
        if (caught_up || clock::now() - t0 >= reclaim_budget) {
            break; // all labels written (or out of time) and still nothing free
        }
    }
    const u64 us = u64(std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - t0).count());
    reclaim_waited_us.fetch_add(us, std::memory_order_relaxed);
    (freed ? reclaim_freed : reclaim_failed).fetch_add(1, std::memory_order_relaxed);
    if (!freed) {
        reclaim_failed_at.store(now_us, std::memory_order_relaxed);
    }
    if (n < 20 || (n & 255) == 0) {
        std::printf("Stability: GX resource-table blocks all waiting on the GPU (#%llu): %s after %llu us "
                    "(%llu freed, %llu not, %llu skipped so far; %llu ms waited)\n",
                    ull(n + 1), freed ? "the GPU's catching up freed some" : "still none free", ull(us),
                    ull(reclaim_freed.load()), ull(reclaim_failed.load()), ull(reclaim_skipped.load()),
                    ull(reclaim_waited_us.load() / 1000));
        std::fflush(stdout);
    }
}

bool InstallReclaimWait() {
    if (!Matches(kReclaim, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81,
                            0xe4, 0xe0, 0xff, 0xff, 0xff}) ||
        !CallsTo(kReclaimCalls[0], kReclaim) || !CallsTo(kReclaimCalls[1], kReclaim) ||
        !Matches(0x26ade1c, {0x49, 0x8b, 0x3e, 0x4c, 0x89, 0xfe}) ||
        !Matches(0x26d0d89, {0x49, 0x8b, 0x7c, 0x24, 0x08, 0x48, 0x8d, 0x75, 0xc8})) {
        std::printf("Stability: GX reclaim wait: guest +0x%llx and its callers are not the expected code; left "
                    "alone\n",
                    ull(kReclaim));
        return false;
    }
    if (const char* e = std::getenv("BB_GX_RECLAIM_WAIT_MS")) {
        reclaim_budget = std::chrono::microseconds(std::max(1, std::atoi(e)) * 1000);
    }
    game_reclaim = reinterpret_cast<ReclaimFn>(image + kReclaim);
    Emitter e = NewStub();
    e.JmpAbs(reinterpret_cast<const void*>(&Reclaim));
    const u64 stub = PlaceStub(e);
    bool ok = stub != 0;
    for (const u64 site : kReclaimCalls) {
        ok = ok && WriteBranch(site, 0xe8, stub, 0);
    }
    return ok;
}

// ======================================================================================
// 5. The frame loop's pool borrow, +0x1067f60 (from bbhost src/engine/frame_pool.cpp). The game
// keeps the entry it takes from the pool (+0x54b8788, six objects) in ctx + 0x120 and writes that
// on every try of its acquire loop: a second thread spinning on the same context writes 0 over
// the working thread's entry, whose release then frees nothing; and two error paths leave with
// the entry held. Once all six are lost, threads spin in here forever (a freeze). Ours keeps the
// entry on the stack and gives it back on every path; ctx + 0x120 is still written.
// ======================================================================================
constexpr u64 kBorrow = 0x1067f60;
constexpr u64 kPoolOf = 0x1057350;   // () -> the pool, or 0
constexpr u64 kPrepare = 0x1049890;  // (ctx->0x118, arg2)
constexpr u64 kAcquire = 0x1056920;  // (pool) -> an entry, or 0 when none is free
constexpr u64 kRelease = 0x1056990;  // (pool, entry)
constexpr u64 kKindOf = 0x216a7c0;   // (entry->8) -> a kind
constexpr u64 kCanUse = 0x2165150;   // (target, 3, kind) -> 0 when it may be used
constexpr u64 kBegin = 0x2169c30;    // (obj, obj + 0xb730, target, 0, 3, &out) -> 0 on success
constexpr u64 kHandOver = 0x1049860; // (ctx->0x118, out)
constexpr u64 kEnd = 0x2169eb0;      // (obj, obj + 0xb730, target, 0)
constexpr u64 kYield = 0x207f970;    // scePthreadYield

std::atomic<u64> pool_borrows{0}, pool_clobbered{0}, pool_error_released{0};

template <class... A>
std::int64_t Call(u64 off, A... a) {
    using Fn = std::int64_t(BB_SYSV*)(decltype(static_cast<std::int64_t>(a))...);
    return reinterpret_cast<Fn>(image + off)(static_cast<std::int64_t>(a)...);
}

void Wr64(u64 address, u64 v) {
    std::memcpy(reinterpret_cast<void*>(address), &v, 8);
}

void GiveBack(u64 pool, u64 ctx, u64 entry) {
    if (Rd64(ctx + 0x120) != entry) {
        // Another thread on this context overwrote it: the game's release would have freed nothing.
        const u64 n = pool_clobbered.fetch_add(1, std::memory_order_relaxed);
        if (n < 8) {
            std::printf("Stability: frame pool: context 0x%llx held entry 0x%llx and now reads 0x%llx (another "
                        "thread on the same context)\n",
                        ull(ctx), ull(entry), ull(Rd64(ctx + 0x120)));
            std::fflush(stdout);
        }
    }
    Call(kRelease, pool, entry);
    Wr64(ctx + 0x120, 0);
}

BB_SYSV std::int64_t FramePoolBorrow(std::int64_t ctx_a, std::int64_t arg2) {
    const u64 ctx = u64(ctx_a);
    const u64 pool = u64(Call(kPoolOf));
    if (!pool) {
        return 1;
    }
    const u64 holder = Rd64(ctx + 0x118);
    if (!holder || !Rd64(holder + 0x40)) {
        return 1;
    }
    Call(kPrepare, holder, arg2);
    u64 entry = 0;
    while (!(entry = u64(Call(kAcquire, pool)))) {
        Wr64(ctx + 0x120, 0); // as the game leaves it while it waits
        Call(kYield);
    }
    Wr64(ctx + 0x120, entry);
    pool_borrows.fetch_add(1, std::memory_order_relaxed);
    // The target is read after the wait, as the game does.
    const u64 target = Rd64(Rd64(ctx + 0x118) + 0x40);
    const u64 obj = Rd64(entry + 8);
    const std::int64_t kind = std::int64_t(u32(Call(kKindOf, obj)));
    if (Call(kCanUse, target, 3, kind) != 0) {
        pool_error_released.fetch_add(1, std::memory_order_relaxed);
        GiveBack(pool, ctx, entry);
        return 1;
    }
    // Sixteen bytes: the callee fills the whole of the game's slot (bbhost).
    alignas(16) u64 out[2] = {0, 0};
    if (Call(kBegin, obj, obj + 0xb730, target, 0, 3, reinterpret_cast<std::int64_t>(&out[0])) != 0) {
        pool_error_released.fetch_add(1, std::memory_order_relaxed);
        GiveBack(pool, ctx, entry);
        return 1;
    }
    Call(kHandOver, Rd64(ctx + 0x118), out[0]);
    const u64 obj_now = Rd64(entry + 8);
    Call(kEnd, obj_now, obj_now + 0xb730, Rd64(Rd64(ctx + 0x118) + 0x40), 0);
    GiveBack(pool, ctx, entry);
    return 0;
}

bool InstallFramePool() {
    const bool calls = CallsTo(0x1067f82, kPoolOf) && CallsTo(0x1067fb0, kPrepare) && CallsTo(0x1067fc0, kYield) &&
                       CallsTo(0x1067fc8, kAcquire) && CallsTo(0x1067feb, kKindOf) && CallsTo(0x1067ffa, kCanUse) &&
                       CallsTo(0x1068031, kBegin) && CallsTo(0x106804c, kHandOver) && CallsTo(0x1068070, kEnd) &&
                       CallsTo(0x106807f, kRelease);
    if (!calls ||
        !Matches(kBorrow, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83,
                           0xec, 0x18, 0x49, 0x89, 0xfe}) ||
        !Matches(kPoolOf, {0x48, 0x8b, 0x05, 0x31, 0x14, 0x46, 0x04, 0xc3})) {
        std::printf("Stability: frame pool: guest +0x%llx is not the expected code; left alone\n", ull(kBorrow));
        return false;
    }
    Emitter e = NewStub();
    e.JmpAbs(reinterpret_cast<const void*>(&FramePoolBorrow));
    return WriteBranch(kBorrow, 0xe9, PlaceStub(e), 0);
}

// ======================================================================================
// 6. The effect ribbon writers (bbport_ribbons.cpp, from bbhost src/decomp/sfx/ribbons.cpp).
// ======================================================================================
constexpr u64 kRibbonFacing = 0x28ce7b0, kRibbonAlong = 0x28ceec0;

bool InstallRibbons() {
    // push rbp, r15, r14, r13, r12, rbx; sub rsp, 0x30 / 0x20; then the arguments into the red zone.
    if (!Matches(kRibbonFacing, {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x30,
                                 0xc5, 0xfa, 0x11, 0x7c, 0x24, 0xfc, 0xc5, 0xfa, 0x11, 0x74, 0x24, 0xf0, 0xc5, 0xf8,
                                 0x28, 0xfd}) ||
        !Matches(kRibbonAlong, {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x20,
                                0xc5, 0xfa, 0x11, 0x64, 0x24, 0xf8, 0xc5, 0xfa, 0x11, 0x5c, 0x24, 0xe4})) {
        std::printf("Stability: ribbons: guest +0x%llx / +0x%llx are not the expected code; left alone\n",
                    ull(kRibbonFacing), ull(kRibbonAlong));
        return false;
    }
    Emitter facing = NewStub();
    facing.JmpAbs(reinterpret_cast<const void*>(&BbRibbons::Facing));
    const u64 facing_stub = PlaceStub(facing);
    Emitter along = NewStub();
    along.JmpAbs(reinterpret_cast<const void*>(&BbRibbons::Along));
    const u64 along_stub = PlaceStub(along);
    if (!facing_stub || !along_stub) {
        return false;
    }
    return WriteBranch(kRibbonFacing, 0xe9, facing_stub, 0) && WriteBranch(kRibbonAlong, 0xe9, along_stub, 0);
}

// ======================================================================================
// 7. The blend-state constructor +0x2a51640 ORs each render target's write mask into +0x164
// without clearing it first (bbhost: its zeroed guest malloc fixed a coloured veil from leftover
// bits reaching CB_TARGET_MASK). Here the constructor clears it before the loop: the loop's
// `xor ecx, ecx; xor edi, edi; jmp` and the alignment NOP after it become
// `mov [rbx + 0x164], eax` (eax is 0 there), the same two xors and a shorter jmp.
// ======================================================================================
constexpr u64 kBlendLoopSetup = 0x2a517f1;

bool InstallBlendMask() {
    // cmp [rbx+0x16c], 0; sete al; movzx edx, al; mov [rbx+0x160], edx; xor eax, eax; lea r8, ..;
    // lea r9, ..; xor ecx, ecx; xor edi, edi; jmp +0x17; nop9 - then the loop at +0x2a51800.
    if (!Matches(0x2a517ce, {0x83, 0xbb, 0x6c, 0x01, 0x00, 0x00, 0x00, 0x0f, 0x94, 0xc0, 0x0f, 0xb6, 0xd0, 0x89, 0x93,
                             0x60, 0x01, 0x00, 0x00, 0x31, 0xc0, 0x4c, 0x8d, 0x05, 0x66, 0xdb, 0xcd, 0x01, 0x4c, 0x8d,
                             0x0d, 0x7f, 0xdb, 0xcd, 0x01, 0x31, 0xc9, 0x31, 0xff, 0xeb, 0x17, 0x66, 0x0f, 0x1f, 0x84,
                             0x00, 0x00, 0x00, 0x00, 0x00}) ||
        !Matches(0x2a51821, {0x09, 0x93, 0x64, 0x01, 0x00, 0x00})) {
        std::printf("Stability: blend mask: guest +0x2a51640 is not the expected code; left alone\n");
        return false;
    }
    static constexpr u8 code[15] = {
        0x89, 0x83, 0x64, 0x01, 0x00, 0x00, // mov [rbx + 0x164], eax
        0x31, 0xc9,                         // xor ecx, ecx
        0x31, 0xff,                         // xor edi, edi
        0xeb, 0x11,                         // jmp +0x2a5180e
        0x0f, 0x1f, 0x00,                   // nop3
    };
    std::memcpy(image + kBlendLoopSetup, code, sizeof code);
    return true;
}

// ======================================================================================
// 8. The render resource cache (from deadinside28/bloodborne_pc 0.6). The cache the GX resource
// allocator +0x26d0d30 serves has its own backing, a 160 MiB range heap set up from a creation
// record (r14; its size at +0x54). Above 30 FPS it filled before the Laurence cinematic's
// geometry was prepared, and failed 20-40 KiB allocations left models missing or exploding;
// 512 MiB survived that peak. Taken from the game's own memory the larger backing starved the
// areas loaded later (upstream), so the game allocates its 160 MiB as always and the cache gets
// the port's direct memory past the game's size instead (runtime_extra_direct_map): right after
// the allocator's Allocate(size, 16) returns (+0x216b21d), its result and the record's size are
// replaced. BB_RENDER_RESOURCE_CACHE_MB=160: the game's cache as it is.
// ======================================================================================
constexpr u64 kCacheBacking = 0x216b21d, kCacheBackingNext = kCacheBacking + 7;
constexpr u32 kCacheNativeBytes = 160u << 20;

BB_SYSV u64 ResourceCacheBacking(u64 record, u64 allocated) {
    if (!allocated || Rd32(record + 0x54) != kCacheNativeBytes) {
        return allocated;
    }
    static const u32 bytes = [] {
        const char* env = std::getenv("BB_RENDER_RESOURCE_CACHE_MB");
        const long mb = env ? std::strtol(env, nullptr, 10) : 512;
        return u32(std::clamp(mb, 160l, 512l)) << 20;
    }();
    if (bytes == kCacheNativeBytes) {
        return allocated;
    }
    void* block = nullptr;
    if (const int error = runtime_extra_direct_map(bytes, 64ull << 10, &block); error || !block) {
        std::printf("Stability: render resource cache: no port memory for %u MiB (error %#x): the game's "
                    "160 MiB\n",
                    bytes >> 20, unsigned(error));
        std::fflush(stdout);
        return allocated;
    }
    // The creation record can be a stack object: not necessarily in the runtime's map.
    std::memcpy(reinterpret_cast<void*>(record + 0x54), &bytes, sizeof(bytes));
    std::printf("Stability: render resource cache: %u MiB in the port's memory at %p (the game's 160 MiB "
                "stays allocated)\n",
                bytes >> 20, block);
    std::fflush(stdout);
    return reinterpret_cast<u64>(block);
}

bool InstallResourceCache() {
    // mov rcx, rbx; mov [rcx + 0x10], rax: right after the allocator's Allocate(size, 16) returns.
    if (!Matches(kCacheBacking, {0x48, 0x89, 0xd9, 0x48, 0x89, 0x41, 0x10})) {
        std::printf("Stability: render resource cache: guest +0x%llx is not the expected code; left alone\n",
                    ull(kCacheBacking));
        return false;
    }
    Emitter e = NewStub();
    e.SkipRedZone();
    e.SaveVolatile();
    e.Bytes({0x4c, 0x89, 0xf7});             // mov rdi, r14 (the creation record)
    e.Bytes({0x48, 0x89, 0xc6});             // mov rsi, rax (the game's allocation)
    e.CallAbs(reinterpret_cast<const void*>(&ResourceCacheBacking));
    e.Bytes({0x49, 0x89, 0x44, 0x24, 0x48}); // mov [r12 + 0x48], rax (the saved rax)
    e.RestoreVolatile();
    e.BackRedZone();
    e.Bytes({0x48, 0x89, 0xd9});             // mov rcx, rbx
    e.Bytes({0x48, 0x89, 0x41, 0x10});       // mov [rcx + 0x10], rax
    e.JmpTo(reinterpret_cast<u64>(image) + kCacheBackingNext);
    return WriteBranch(kCacheBacking, 0xe9, PlaceStub(e), 7 - 5);
}

// ======================================================================================
// 9. Yebis's blur-kernel sample parameters (GPUTexUtil_GetRecursiveSampleParameters, +0xbbc3e0;
// from deadinside28/bloodborne_pc 0.6): its first argument, the base sample count, must be at
// least 3 (an assertion the retail build has compiled out). Game data now and then gives 2; the
// function then runs on in a state that does not end (the guest stops submitting: a black
// screen), so the count is raised to 3 at the entry.
// ======================================================================================
constexpr u64 kKernelParams = 0xbbc3e0;

bool InstallKernelSampleClamp() {
    // push rbp; mov rbp, rsp; push r15
    if (!Matches(kKernelParams, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57})) {
        std::printf("Stability: blur-kernel clamp: guest +0x%llx is not the expected code; left alone\n",
                    ull(kKernelParams));
        return false;
    }
    Emitter e = NewStub();
    e.Bytes({0x83, 0xff, 0x03});                         // cmp edi, 3
    e.Bytes({0x7d, 0x05});                               // jge over the mov
    e.Bytes({0xbf, 0x03, 0x00, 0x00, 0x00});             // mov edi, 3
    e.Bytes({0x55, 0x48, 0x89, 0xe5, 0x41, 0x57});       // the instructions the jump covers
    e.JmpTo(reinterpret_cast<u64>(image) + kKernelParams + 6);
    return WriteBranch(kKernelParams, 0xe9, PlaceStub(e), 6 - 5);
}

// ======================================================================================
// GX occlusion query guard (ported from deadinside28's InstallGxQueryGuard, upstream 0fc2921c,
// PR #343 by whp199, issue #336; GPL-2.0-or-later; reduced to the two skip stubs). Crash 0x2169edc on
// GXRenderThread, address 0, after hours of play: the game's query (0x30 bytes, its handle the first
// field) is made while the device heap is full and keeps a null handle; the render command then
// begins it (0xe2f130: state [this + 8] 0 -> 2, reads the handle) and ends it (0xe2f160). A query
// without a handle is neither begun nor ended: its state stays 0, so its result getter gives 0
// (nothing visible). BB_GX_QUERY_GUARD=0: off.
// BB_GX_QUERY_TEST_NULL=N (test aid): the N-th begin of a query with a handle sees a null handle (the
// real handle is put back at the next begin), to run the skip path in a short session.
// ======================================================================================
constexpr u64 kQueryBegin = 0xe2f134;  // cmp dword [rdi + 8], 0; jne 0xe2f157 (begun already)
constexpr u64 kQueryReturn = 0xe2f157; // mov al, 1; pop rbp; ret
constexpr u64 kQueryEnd = 0xe2f164;    // mov dword [rdi + 8], 1
constexpr u64 kQueryEndReturn = 0xe2f181; // mov al, 1; pop rbp; ret
std::atomic<u64> query_skipped{0};
u64 query_test_every = 0, query_test_calls = 0, query_test_query = 0, query_test_handle = 0;

/// The begin stub calls this (rdi: the query) for a query without a handle, and in test mode for
/// every begin; the stub then looks at the handle again.
BB_SYSV void QueryNoHandle(u64 query) {
    if (query_test_every) {
        if (query_test_query) { // the handle of the forced begin comes back
            *reinterpret_cast<u64*>(query_test_query) = query_test_handle;
            query_test_query = 0;
        }
        const u64 handle = *reinterpret_cast<const u64*>(query);
        if (handle && (query_test_calls == 0 || (query_test_calls + 1) % 1000 == 0)) {
            std::printf("GX query guard: test aid saw %llu begins\n", ull(query_test_calls + 1));
        }
        if (handle && ++query_test_calls == query_test_every) {
            query_test_query = query;
            query_test_handle = handle;
            *reinterpret_cast<u64*>(query) = 0;
            std::printf("GX query guard: test aid nulls the handle of query %#llx at begin %llu\n", ull(query),
                        ull(query_test_calls));
        }
    }
    if (*reinterpret_cast<const u64*>(query) != 0) {
        return;
    }
    const u64 n = query_skipped.fetch_add(1) + 1;
    if ((n & (n - 1)) == 0) { // the 1st, 2nd, 4th, ...
        std::printf("GX query guard: null handle skipped (%llu)\n", ull(n));
    }
}

bool InstallQueryGuard() {
    if (!Matches(kQueryBegin, {0x83, 0x7f, 0x08, 0x00, 0x75, 0x1d}) ||
        !Matches(kQueryEnd, {0xc7, 0x47, 0x08, 0x01, 0x00, 0x00, 0x00}) ||
        !Matches(kQueryReturn, {0xb0, 0x01}) || !Matches(kQueryEndReturn, {0xb0, 0x01})) {
        std::printf("Stability: GX query guard: guest +0x%llx / +0x%llx is not the expected code; left alone\n",
                    ull(kQueryBegin), ull(kQueryEnd));
        return false;
    }
    if (const char* test = std::getenv("BB_GX_QUERY_TEST_NULL")) {
        query_test_every = std::strtoull(test, nullptr, 10);
    }
    const u64 base = reinterpret_cast<u64>(image);
    Emitter b = NewStub();
    b.Bytes({0x83, 0x7f, 0x08, 0x00});       // cmp dword [rdi + 8], 0
    b.Bytes({0x75, 0x00});                   // jne done (begun or ended already)
    const std::size_t to_done = b.size;
    b.Bytes({0x80, 0x3d, 0, 0, 0, 0, 0x00}); // cmp byte [rip + test flag], 0
    const std::size_t flag_disp = b.size - 5;
    b.Bytes({0x75, 0x00});                   // jne none
    const std::size_t to_none_a = b.size;
    b.Bytes({0x48, 0x83, 0x3f, 0x00});       // cmp qword [rdi], 0
    b.Bytes({0x74, 0x00});                   // je none
    const std::size_t to_none_b = b.size;
    const std::size_t body = b.size;
    b.JmpTo(base + kQueryBegin + 6);         // body: state 2, the begin
    const std::size_t done = b.size;
    b.JmpTo(base + kQueryReturn);
    b.code[to_done - 1] = u8(done - to_done);
    b.code[to_none_a - 1] = u8(b.size - to_none_a);
    b.code[to_none_b - 1] = u8(b.size - to_none_b);
    b.SaveVolatile();                        // none: rdi (the query) kept
    b.CallAbs(reinterpret_cast<const void*>(&QueryNoHandle));
    b.RestoreVolatile();
    b.Bytes({0x48, 0x83, 0x3f, 0x00});       // cmp qword [rdi], 0
    b.Bytes({0x75, u8(body - (b.size + 2))}); // jne body (test mode: the handle is there)
    b.Bytes({0xeb, u8(done - (b.size + 2))}); // jmp done: not begun, the state stays 0
    const std::size_t flag_at = b.size;
    b.Bytes({u8(query_test_every ? 1 : 0)});
    const std::int32_t flag_rel = std::int32_t(flag_at) - std::int32_t(flag_disp + 5);
    std::memcpy(&b.code[flag_disp], &flag_rel, 4);
    const u64 begin_stub = PlaceStub(b);
    Emitter e = NewStub();
    e.Bytes({0x48, 0x83, 0x3f, 0x00});       // cmp qword [rdi], 0
    e.Bytes({0x74, 0x00});                   // je none
    const std::size_t to_skip = e.size;
    e.Bytes({0xc7, 0x47, 0x08, 0x01, 0x00, 0x00, 0x00}); // mov dword [rdi + 8], 1
    e.JmpTo(base + kQueryEnd + 7);
    e.code[to_skip - 1] = u8(e.size - to_skip);
    e.JmpTo(base + kQueryEndReturn);         // none: not ended, the state stays 0
    const u64 end_stub = begin_stub ? PlaceStub(e) : 0;
    if (!begin_stub || !end_stub) {
        std::printf("Stability: GX query guard: no stub space; left alone\n");
        return false;
    }
    return WriteBranch(kQueryBegin, 0xe9, begin_stub, 1) && WriteBranch(kQueryEnd, 0xe9, end_stub, 2);
}

// ======================================================================================
// 4. Scaleform HUD race and text-layout guard (bbhost src/engine/sf_text_layout.cpp, 7fea28a,
// 4b18a86, 46b2f8c, 8551763, 3e609c7; the rules are in bbport_scaleform_logic.h).
//  a. The game's HUD update (+0x1bf7c20: bars, enemy tags, boss list, talk window, map name, event
//     text) drives the HUD movie through GFx SetText / GotoAndPlay with no lock, beside the movie's
//     own jobs (Advance + Capture +0x1f58e50, NextCapture +0x1f59150), which hold the movie wrapper's
//     lock (+0x38, a recursive mutex over the scePthreadMutex at +0x40). BB_SF_MOVIE_LOCK=0: off.
//     The update now takes that mutex too: a stub in front of it finds the item's movie the way the
//     update itself does first (+0x1adb8a0: the movie handle at +0x20, whose method 0 returns the
//     movie; +0x1cfb0 lets that reference go), the movie's wrapper among the Advance jobs seen, and
//     calls the original under the mutex. A thread that holds it already runs as is; a wait past
//     100 ms runs as the game does, counted. Lock order: the game runs the update itself on the main
//     thread only while holding data_5994200, which NextCapture also takes before +0x38; Advance
//     never takes it.
//  b. A text changed after a capture's propagate pass is published with its layout-changed flag set
//     and no layout, and the text cache node's transform update (+0x12d5e0) reads +0x1c of the null
//     layout (fault at +0x12d6c0). BB_SF_TEXT_GUARD=0: off. A text node whose displayed data has no
//     layout runs the update without flag bit 0 (the part that reads the layout); when a later
//     capture brings the layout (HandleChanges +0x12df80, change 0x400), the node is marked for the
//     update with 0x10000, the flag the tree update turns into bit 0.
// Not ported from bbhost: the stranded-propagate-list repair (restrand / destrand at the capture's
// call sites) and the detector; with the lock in place the race that strands an entry no longer
// happens, and the guard keeps the game up where it does.
// ======================================================================================
constexpr u64 kHudUpdate = 0x1bf7c20;
constexpr u8 kHudUpdatePrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                     0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0x28, 0x07, 0x00, 0x00};
constexpr u64 kAdvanceJob = 0x1f58e50;
constexpr u8 kAdvanceJobPrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                      0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0x98, 0x00, 0x00, 0x00};
constexpr u64 kUpdateTransform = 0x12d5e0;
constexpr u8 kUpdateTransformPrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                           0x41, 0x54, 0x53, 0x48, 0x81, 0xe4, 0xe0, 0xff, 0xff, 0xff};
constexpr u64 kHandleChanges = 0x12df80;
constexpr u8 kHandleChangesPrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                                         0x53, 0x50, 0x89, 0xd3, 0x49, 0x89, 0xf6};
constexpr u64 kMovieRelease = 0x1cfb0;
constexpr u8 kMovieReleasePrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53};
constexpr u64 kHudItemMovie = 0x20; // the item's movie handle
constexpr u64 kWrapperMovie = 0x18, kWrapperMutex = 0x40;

BbScaleform::WrapperEntry sf_entries[BbScaleform::kWrapperTableSize];
BbScaleform::WrapperTable sf_table{sf_entries, BbScaleform::kWrapperTableSize};
BbScaleform::WaitSet sf_waiting;
std::atomic<u64> sf_runs{0}, sf_waits{0}, sf_wait_us{0}, sf_wait_max{0}, sf_timeouts{0}, sf_no_wrapper{0}, sf_already{0},
    sf_no_mutex{0}, sf_skipped{0}, sf_marked{0}, sf_unmarked{0};
bool sf_lock_on = false, sf_guard_on = false;

bool GameReads(u64 a, std::size_t) { return a >= 0x10000; }
bool Mapped(u64 a, std::size_t n) { return a >= 0x10000 && runtime_memory_is_mapped(std::uintptr_t(a), n) != 0; }
bool LogThis(u64 n) { return n <= 16 || (n & (n - 1)) == 0; }

/// The Advance job's prologue stub: the wrapper (rdi) holds a movie.
BB_SYSV void SfNoteWrapper(u64 w) {
    if (!Mapped(w + kWrapperMovie, 8)) {
        return;
    }
    const u64 movie = Rd64(w + kWrapperMovie);
    if (!movie) {
        return;
    }
    const u64 reused = sf_table.reused.load(std::memory_order_relaxed);
    const u64 left_out = sf_table.left_out.load(std::memory_order_relaxed);
    BbScaleform::TableNote(sf_table, w, movie, Mapped);
    if (sf_table.reused.load(std::memory_order_relaxed) > reused && reused == 0) {
        std::printf("Stability: sf lock: the movie table took its first entry again (movie %#llx)\n", ull(movie));
    }
    if (sf_table.left_out.load(std::memory_order_relaxed) > left_out && left_out == 0) {
        std::printf("Stability: sf lock: the movie table is full of live movies (%d): movie %#llx is left out, the HUD "
                    "lock does not see it\n", sf_table.size, ull(movie));
    }
}

void SfReport();
using GetMovieFn = void(BB_SYSV*)(u64* out, u64 handle);
using ReleaseFn = void(BB_SYSV*)(u64 movie);

/// The item's movie as its handle's method 0 gives it (a Ptr<Movie> through rdi), the reference that
/// call took let go again: the handle keeps its own.
u64 HudMovie(u64 item) {
    if (item < 0x10000) {
        return 0;
    }
    const u64 handle = Rd64(item + kHudItemMovie);
    if (handle < 0x10000) {
        return 0;
    }
    const u64 get = Rd64(Rd64(handle));
    if (get < reinterpret_cast<u64>(image) || get >= reinterpret_cast<u64>(image) + image_size) {
        return 0;
    }
    u64 movie = 0;
    reinterpret_cast<GetMovieFn>(get)(&movie, handle);
    if (movie) {
        reinterpret_cast<ReleaseFn>(reinterpret_cast<u64>(image) + kMovieRelease)(movie);
    }
    return movie;
}

/// The HUD update's stub, before the original: takes the movie wrapper's mutex when it can. Returns the
/// mutex slot to give back afterwards (SfHudUnlock), or 0.
BB_SYSV u64 SfHudLock(u64 item) {
    const u64 movie = HudMovie(item);
    BbScaleform::WrapperEntry* e = movie ? BbScaleform::TableFind(sf_table, movie) : nullptr;
    const u64 w = e ? e->wrapper.load(std::memory_order_acquire) : 0;
    if (!w || !Mapped(w + kWrapperMovie, 8) || Rd64(w + kWrapperMovie) != movie) {
        if (sf_no_wrapper.fetch_add(1, std::memory_order_relaxed) == 0) {
            std::printf("Stability: sf lock: the HUD update ran before its movie's Advance job was seen (movie %#llx): "
                        "it runs as the game has it until then\n", ull(movie));
        }
        return 0;
    }
    void** slot = reinterpret_cast<void**>(w + kWrapperMutex);
    std::uint64_t us = 0;
    const int rc = runtime_guest_mutex_acquire(slot, BbScaleform::kLockWaitUs, &us);
    if (us) {
        sf_waits.fetch_add(1, std::memory_order_relaxed);
        sf_wait_us.fetch_add(us, std::memory_order_relaxed);
        u64 cur = sf_wait_max.load(std::memory_order_relaxed);
        while (us > cur && !sf_wait_max.compare_exchange_weak(cur, us, std::memory_order_relaxed)) {
        }
    }
    switch (BbScaleform::LockResult(rc)) {
    case BbScaleform::LockResult::kLocked:
        {
            const u64 n = sf_runs.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n == 1) {
                std::printf("Stability: sf lock: the HUD update runs under the lock of movie %#llx (wrapper %#llx)\n",
                            ull(movie), ull(w));
            } else if (n == 10000 || n == 100000 || n % 1000000 == 0) {
                SfReport(); // the game is often ended without a normal exit: a running tally
            }
        }
        break;
    case BbScaleform::LockResult::kAlreadyHeld:
        sf_already.fetch_add(1, std::memory_order_relaxed);
        break;
    case BbScaleform::LockResult::kTimedOut: {
        const u64 n = sf_timeouts.fetch_add(1, std::memory_order_relaxed) + 1;
        if (LogThis(n)) {
            std::printf("Stability: sf lock: the HUD update waited %llu us for its movie's lock (wrapper %#llx) and ran "
                        "without it, as the game has it (%llu so far)\n", ull(us), ull(w), ull(n));
        }
        break;
    }
    default:
        sf_no_mutex.fetch_add(1, std::memory_order_relaxed);
        break;
    }
    return BbScaleform::MustRelease(rc) ? reinterpret_cast<u64>(slot) : 0;
}

BB_SYSV void SfHudUnlock(u64 slot) {
    if (slot) {
        runtime_guest_mutex_release(reinterpret_cast<void**>(slot));
    }
}

/// The transform update's stub (rdi: the text cache node, esi: its flags in r8d): the flags to run with.
BB_SYSV u64 SfTextFlags(u64 node, u64 flags) {
    const BbScaleform::Decision d = BbScaleform::Decide(node, u32(flags), GameReads);
    if (!d.skipped) {
        return flags;
    }
    sf_waiting.Add(node);
    const u64 n = sf_skipped.fetch_add(1, std::memory_order_relaxed) + 1;
    if (LogThis(n)) {
        std::printf("Stability: sf text: a text is shown before its layout exists (cache node %#llx, data %#llx): its "
                    "transform update runs without the layout's part, done again once the layout arrives (%llu so "
                    "far)\n", ull(node), ull(d.data), ull(n));
    }
    return d.flags;
}

/// HandleChanges' stub (rdi: the node, esi: the change flags).
BB_SYSV void SfTextChanged(u64 node, u64 changes) {
    if (!(u32(changes) & BbScaleform::kChangeTextLayout) || sf_waiting.Count() == 0) {
        return;
    }
    const u64 data = BbScaleform::EntryData(Rd64(node + BbScaleform::kNodeEntry), BbScaleform::kPageDisplay, GameReads);
    if (!data || Rd64(data + BbScaleform::kTextLayout) == 0 || !sf_waiting.Take(node)) {
        return;
    }
    // Its layout is on display now: the part the guard took out runs in this capture's tree update.
    const bool marked = BbScaleform::MarkForUpdate(node, BbScaleform::kNodeRetransform, GameReads);
    const u64 n = (marked ? sf_marked : sf_unmarked).fetch_add(1, std::memory_order_relaxed) + 1;
    if (LogThis(n)) {
        std::printf("Stability: sf text: cache node %#llx has its layout now - %s (%llu so far)\n", ull(node),
                    marked ? "marked for its transform update" : "not marked (no root, or its update under way)", ull(n));
    }
}

void SfReport() {
    if (sf_lock_on && (sf_runs || sf_timeouts || sf_no_wrapper || sf_already)) {
        std::printf("Stability: sf lock: the HUD update ran %llu time(s) under its movie's lock; %llu waited (%llu us in "
                    "all, longest %llu us), %llu waited past %llu ms and ran without it; %llu before its movie's Advance "
                    "job was seen, %llu with the lock held already, %llu without a usable mutex; movie table: %llu "
                    "entries taken again, %llu movies left out\n",
                    ull(sf_runs), ull(sf_waits), ull(sf_wait_us), ull(sf_wait_max), ull(sf_timeouts),
                    ull(BbScaleform::kLockWaitUs / 1000), ull(sf_no_wrapper), ull(sf_already), ull(sf_no_mutex),
                    ull(sf_table.reused.load()), ull(sf_table.left_out.load()));
    }
    if (sf_guard_on && (sf_skipped || sf_marked || sf_unmarked)) {
        std::printf("Stability: sf text: %llu text update(s) ran without their layout's part; %llu node(s) marked again "
                    "when it arrived, %llu not marked, %d still waiting\n",
                    ull(sf_skipped), ull(sf_marked), ull(sf_unmarked), sf_waiting.Count());
    }
    std::fflush(stdout);
}

/// The image holds `n` bytes `bytes` at guest offset `off`.
bool MatchesBytes(u64 off, const u8* bytes, std::size_t n) {
    return off + n <= image_size && std::memcmp(image + off, bytes, n) == 0;
}

/// The moved prologue of the function at `off` and a jump back past it: the original, callable.
u64 PlaceTrampoline(u64 off, const u8* prologue, std::size_t n) {
    Emitter t = NewStub();
    for (std::size_t i = 0; i < n; ++i) {
        t.Bytes({prologue[i]});
    }
    t.JmpTo(reinterpret_cast<u64>(image) + off + n);
    return PlaceStub(t);
}

bool InstallHudLock() {
    const std::size_t n = sizeof(kHudUpdatePrologue);
    if (!MatchesBytes(kHudUpdate, kHudUpdatePrologue, n) ||
        !MatchesBytes(kAdvanceJob, kAdvanceJobPrologue, sizeof(kAdvanceJobPrologue)) ||
        !MatchesBytes(kMovieRelease, kMovieReleasePrologue, sizeof(kMovieReleasePrologue))) {
        std::printf("Stability: sf lock: guest +0x%llx / +0x%llx is not the expected code; left alone\n", ull(kHudUpdate),
                    ull(kAdvanceJob));
        return false;
    }
    const u64 base = reinterpret_cast<u64>(image);
    // The Advance job notes its wrapper, then runs as it was.
    Emitter a = NewStub();
    a.SaveVolatile();
    a.CallAbs(reinterpret_cast<const void*>(&SfNoteWrapper)); // rdi: the wrapper
    a.RestoreVolatile();
    for (const u8 b : kAdvanceJobPrologue) {
        a.Bytes({b});
    }
    a.JmpTo(base + kAdvanceJob + sizeof(kAdvanceJobPrologue));
    const u64 advance_stub = PlaceStub(a);
    const u64 original = PlaceTrampoline(kHudUpdate, kHudUpdatePrologue, n);
    if (!advance_stub || !original) {
        std::printf("Stability: sf lock: no stub space; left alone\n");
        return false;
    }
    // The HUD update: item (rdi), state (rsi), dt (xmm0), run under the wrapper's mutex. Frame: 56 bytes
    // (xmm0 at 0, the mutex slot at 16, rax at 24) under the six saved argument registers.
    Emitter e = NewStub();
    e.Bytes({0x57, 0x56, 0x52, 0x51, 0x41, 0x50, 0x41, 0x51}); // push rdi, rsi, rdx, rcx, r8, r9
    e.Bytes({0x48, 0x83, 0xec, 0x38});                         // sub rsp, 56
    e.Bytes({0x0f, 0x11, 0x04, 0x24});                         // movups [rsp], xmm0
    e.CallAbs(reinterpret_cast<const void*>(&SfHudLock));      // rdi: the item
    e.Bytes({0x48, 0x89, 0x44, 0x24, 0x10});                   // mov [rsp + 16], rax
    e.Bytes({0x48, 0x8b, 0x7c, 0x24, 0x60});                   // mov rdi, [rsp + 96]
    e.Bytes({0x48, 0x8b, 0x74, 0x24, 0x58});                   // mov rsi, [rsp + 88]
    e.Bytes({0x48, 0x8b, 0x54, 0x24, 0x50});                   // mov rdx, [rsp + 80]
    e.Bytes({0x48, 0x8b, 0x4c, 0x24, 0x48});                   // mov rcx, [rsp + 72]
    e.Bytes({0x4c, 0x8b, 0x44, 0x24, 0x40});                   // mov r8, [rsp + 64]
    e.Bytes({0x4c, 0x8b, 0x4c, 0x24, 0x38});                   // mov r9, [rsp + 56]
    e.Bytes({0x0f, 0x10, 0x04, 0x24});                         // movups xmm0, [rsp]
    e.Bytes({0xe8});                                           // call the original
    std::int32_t rel = 0;
    e.ok &= Rel32(e.at + e.size + 4, original, rel);
    e.U32(u32(rel));
    e.Bytes({0x48, 0x89, 0x44, 0x24, 0x18});                   // mov [rsp + 24], rax
    e.Bytes({0x0f, 0x11, 0x04, 0x24});                         // movups [rsp], xmm0
    e.Bytes({0x48, 0x8b, 0x7c, 0x24, 0x10});                   // mov rdi, [rsp + 16]
    e.CallAbs(reinterpret_cast<const void*>(&SfHudUnlock));
    e.Bytes({0x48, 0x8b, 0x44, 0x24, 0x18});                   // mov rax, [rsp + 24]
    e.Bytes({0x0f, 0x10, 0x04, 0x24});                         // movups xmm0, [rsp]
    e.Bytes({0x48, 0x83, 0xc4, 0x68});                         // add rsp, 104 (frame and the six saved registers)
    e.Bytes({0xc3});                                           // ret
    const u64 hud_stub = PlaceStub(e);
    if (!hud_stub) {
        std::printf("Stability: sf lock: no stub space; left alone\n");
        return false;
    }
    return WriteBranch(kAdvanceJob, 0xe9, advance_stub, sizeof(kAdvanceJobPrologue) - 5) &&
           WriteBranch(kHudUpdate, 0xe9, hud_stub, n - 5);
}

bool InstallTextGuard() {
    if (!MatchesBytes(kUpdateTransform, kUpdateTransformPrologue, sizeof(kUpdateTransformPrologue)) ||
        !MatchesBytes(kHandleChanges, kHandleChangesPrologue, sizeof(kHandleChangesPrologue))) {
        std::printf("Stability: sf text: guest +0x%llx / +0x%llx is not the expected code; left alone\n",
                    ull(kUpdateTransform), ull(kHandleChanges));
        return false;
    }
    const u64 base = reinterpret_cast<u64>(image);
    // UpdateTransform(node = rdi, ..., flags = r8d): the flags come back in the saved r8's place.
    Emitter u = NewStub();
    u.SaveVolatile();
    u.Bytes({0x44, 0x89, 0xc6}); // mov esi, r8d
    u.CallAbs(reinterpret_cast<const void*>(&SfTextFlags));
    u.Bytes({0x49, 0x89, 0x44, 0x24, 0x20}); // mov [r12 + 32], rax (the saved r8)
    u.RestoreVolatile();
    for (const u8 b : kUpdateTransformPrologue) {
        u.Bytes({b});
    }
    u.JmpTo(base + kUpdateTransform + sizeof(kUpdateTransformPrologue));
    const u64 update_stub = PlaceStub(u);
    // HandleChanges(node = rdi, root, changes = edx).
    Emitter h = NewStub();
    h.SaveVolatile();
    h.Bytes({0x89, 0xd6}); // mov esi, edx
    h.CallAbs(reinterpret_cast<const void*>(&SfTextChanged));
    h.RestoreVolatile();
    for (const u8 b : kHandleChangesPrologue) {
        h.Bytes({b});
    }
    h.JmpTo(base + kHandleChanges + sizeof(kHandleChangesPrologue));
    const u64 changes_stub = PlaceStub(h);
    if (!update_stub || !changes_stub) {
        std::printf("Stability: sf text: no stub space; left alone\n");
        return false;
    }
    return WriteBranch(kUpdateTransform, 0xe9, update_stub, sizeof(kUpdateTransformPrologue) - 5) &&
           WriteBranch(kHandleChanges, 0xe9, changes_stub, sizeof(kHandleChangesPrologue) - 5);
}

// ======================================================================================
// 5. FMOD sound-ring guard (bbhost src/engine/fmod_ring.cpp, 4d9fb54, f1d4fa3; the rules are in
// bbport_fmod_logic.h). BB_FMOD_RING_GUARD=0: off. FMOD's MemPool free (+0xdab3a0) first takes a sound
// that is still linked out of the master sound group's ring, under FMOD's global critical section (the
// slot behind the pointer at +0x546d260). At the stream's free of a refusing shared subsound (the call
// at +0xdbe682) it also keeps the object, as a sound of its own whose open state is ERROR, so that the
// event or the release still holding it does not use freed memory; it is never played again.
// ======================================================================================
constexpr u64 kMemPoolFree = 0xdab3a0;
constexpr u8 kMemPoolFreePrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                                       0x41, 0x55, 0x41, 0x54, 0x53, 0x50};
constexpr u64 kSharedFreeReturn = 0xdbe687; // after the stream's call of the free
constexpr u64 kFmodCrit = 0x546d260;        // pointer to FMOD's critical section mutex slot
constexpr u64 kSoundVtables[4] = {0x52d7640, 0x52d7840, 0x52d7a30, 0x52d7c30};
BbFmod::Vtables fmod_vt{};
std::atomic<u64> fmod_unlinked{0}, fmod_kept{0}, fmod_kept_bytes{0};

/// MemPool::free(pool = rdi, ptr = rsi, ...): ret is the free's return address. Non-zero: the free is
/// not run (its caller ignores the result).
BB_SYSV u64 FmodFreeGuard(u64 /*pool*/, u64 p, u64 ret) {
    if (p < 0x10000 || !BbFmod::IsSound(fmod_vt, p)) {
        return 0;
    }
    const bool linked_now = BbFmod::Linked(p, GameReads);
    if (!linked_now) {
        return 0;
    }
    const BbFmod::FreeAction action = BbFmod::ChooseFree(
        linked_now, ret == reinterpret_cast<u64>(image) + kSharedFreeReturn, BbFmod::RefusedShared(p));
    void** slot = nullptr;
    if (const u64 c = Rd64(reinterpret_cast<u64>(image) + kFmodCrit)) {
        slot = reinterpret_cast<void**>(c);
    }
    std::uint64_t waited = 0;
    const int rc = slot ? runtime_guest_mutex_acquire(slot, 5000000, &waited) : 3;
    if (rc == int(BbScaleform::LockResult::kTimedOut) || rc == int(BbScaleform::LockResult::kNoMutex)) {
        return 0; // the critical section cannot be taken: the game's own free
    }
    const bool done = BbFmod::Unlink(p, GameReads);
    const bool keep = done && action == BbFmod::FreeAction::kUnlinkAndKeep;
    if (keep) {
        BbFmod::Orphan(p);
    }
    if (BbScaleform::MustRelease(rc)) {
        runtime_guest_mutex_release(slot);
    }
    if (!done) {
        return 0;
    }
    const u64 n = fmod_unlinked.fetch_add(1, std::memory_order_relaxed) + 1;
    if (keep) {
        fmod_kept.fetch_add(1, std::memory_order_relaxed);
        fmod_kept_bytes.fetch_add(BbFmod::OwnedBytes(p, GameReads), std::memory_order_relaxed);
    }
    if (LogThis(n)) {
        std::printf("Stability: fmod ring: a sound was freed while still in its sound group (sound %#llx, flags %#x): "
                    "%s (%llu so far, %llu kept)\n", ull(p), BbFmod::Rd32(p + BbFmod::kFlags),
                    keep ? "taken out of the ring and kept as a sound of its own that nothing plays again"
                         : "taken out of the ring first",
                    ull(n), ull(fmod_kept.load(std::memory_order_relaxed)));
    }
    return keep ? 1 : 0;
}

void FmodReport() {
    if (fmod_unlinked) {
        std::printf("Stability: fmod ring: the guard took %llu freed sound(s) out of the ring; kept %llu refusing shared "
                    "subsound(s) (about %llu bytes never freed)\n",
                    ull(fmod_unlinked), ull(fmod_kept), ull(fmod_kept_bytes));
        std::fflush(stdout);
    }
}

bool InstallFmodGuard() {
    if (!MatchesBytes(kMemPoolFree, kMemPoolFreePrologue, sizeof(kMemPoolFreePrologue)) ||
        !CallsTo(kSharedFreeReturn - 5, kMemPoolFree)) {
        std::printf("Stability: fmod ring: guest +0x%llx is not the expected code; left alone\n", ull(kMemPoolFree));
        return false;
    }
    const u64 base = reinterpret_cast<u64>(image);
    for (int i = 0; i < 4; ++i) {
        fmod_vt.vt[i] = base + kSoundVtables[i];
    }
    Emitter e = NewStub();
    e.SaveVolatile();
    e.Bytes({0x49, 0x8b, 0x54, 0x24, 0x50}); // mov rdx, [r12 + 80] (the return address)
    e.CallAbs(reinterpret_cast<const void*>(&FmodFreeGuard));
    e.Bytes({0x49, 0x89, 0x44, 0x24, 0x48}); // mov [r12 + 72], rax (the saved rax)
    e.RestoreVolatile();
    e.Bytes({0x85, 0xc0});                   // test eax, eax
    e.Bytes({0x74, 0x01});                   // jz run
    e.Bytes({0xc3});                         // ret (the free is not run)
    for (const u8 b : kMemPoolFreePrologue) {
        e.Bytes({b});
    }
    e.JmpTo(base + kMemPoolFree + sizeof(kMemPoolFreePrologue));
    return WriteBranch(kMemPoolFree, 0xe9, PlaceStub(e), sizeof(kMemPoolFreePrologue) - 5);
}

// ======================================================================================
// 19. Test aid for the Havok behaviour event guard (crashfix081, CL-078659; the guard itself is in the
// crash handler, src/probe.c with src/havok_event_guard.h). BB_HAVOK_EVENT_TEST=N (off by default):
// every N-th animation event that plays annotations (0x1a27fb0's call of 0x1a21100 at +0x1a2813a)
// gets a fake animation object whose +0x18 is 1, as in the crash dump, so 0x1a21100 faults at
// +0x1a21134 and the guard has to skip it. A game that keeps running with
// "Havok event guard: ... skipped" lines in its log shows the guard works in the real code path.
u32 havok_test_counter = 0;
alignas(16) u8 havok_test_object[0x40] = {};

bool InstallHavokEventTest(u32 every) {
    constexpr u64 kSite = 0x1a2813a;
    if (!CallsTo(kSite, HEG_PLAY) || !heg_image_matches(image, image_size)) {
        std::printf("Stability: Havok event test: guest +0x%llx is not the expected code; left alone\n", ull(kSite));
        return false;
    }
    const u64 one = 1;
    std::memcpy(havok_test_object + 0x18, &one, 8); // the stale +0x18 of the dump
    Emitter e = NewStub();
    e.Bytes({0x48, 0xb9}); // movabs rcx, &counter (rax, rcx, rdx: free at a call)
    e.U64(reinterpret_cast<u64>(&havok_test_counter));
    e.Bytes({0xb8, 0x01, 0x00, 0x00, 0x00}); // mov eax, 1
    e.Bytes({0xf0, 0x0f, 0xc1, 0x01});       // lock xadd [rcx], eax
    e.Bytes({0xff, 0xc0});                   // inc eax
    e.Bytes({0x31, 0xd2});                   // xor edx, edx
    e.Bytes({0xb9});                         // mov ecx, every
    e.U32(every);
    e.Bytes({0xf7, 0xf1});                   // div ecx
    e.Bytes({0x85, 0xd2});                   // test edx, edx
    e.Bytes({0x75, 0x0a});                   // jnz .play
    e.Bytes({0x48, 0xbe});                   // movabs rsi, &fake object
    e.U64(reinterpret_cast<u64>(havok_test_object));
    e.JmpTo(reinterpret_cast<u64>(image + HEG_PLAY)); // .play: jmp 0x1a21100
    const u64 stub = PlaceStub(e);
    return stub && WriteBranch(kSite, 0xe8, stub, 0);
}

// 17. The title's attract movie waits for GXRenderThread (bbhost 41d725b, droogie/bbhost, GPL-3.0;
// ported by intent). BB_MOVIE_DRAW_WAIT=0: the game's own calls. The movie player's frame draw
// (+0x28fe7c0, called every frame by CSMovieIns::STEP_Wait_Play on the main thread with GX's
// immediate context) and its close (+0x28fe2e0, from STEP_Finish_Play) use that context while
// GXRenderThread still works on it (its last batch's pending state, the rename map's resolve): a
// null read in GX's commit (+0x26c01c3) or a null write in the map's resolve (+0x26ae211) a minute
// or two into the attract movie. The game's other main-thread code waits for the render thread's
// idle event first; these two do not. Now they do, the way those callers do: the event's wait (vtable
// slot +0x18, 100 ms steps), bounded at 5 s (then the game's call goes ahead, logged). From the render
// thread itself nothing is waited for. GX manager +0x348 = GXRenderThread; its idle event at +0x68
// (set once a batch and its flip are done); the thread object at +8, its pthread at thread +0x18.
// ======================================================================================
constexpr u64 kMovieDraw = 0x28fe7c0;
constexpr u64 kMovieClose = 0x28fe2e0;
constexpr u8 kMovieDrawPrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                     0x41, 0x54, 0x53, 0x48, 0x81, 0xe4, 0xe0, 0xff, 0xff, 0xff};
constexpr u8 kMovieClosePrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x50, 0x48, 0x89, 0xfb};
constexpr u64 kGxManager = 0x55406c8;      // pointer to GX's manager
constexpr u64 kGxImmediateWrapper = 0x10;  // its immediate context's wrapper; the context at +8
constexpr u64 kGxRenderThread = 0x348;
constexpr u64 kRtThread = 0x8, kThreadPthread = 0x18, kRtIdleEvent = 0x68;
constexpr u64 kEventFlags = 0x18, kEventVtableWait = 0x18;
constexpr u32 kEventSet = 2;
constexpr u64 kEventVtable = 0x52b97a0;    // the Dantelion2 event's vtable
constexpr u64 kPthreadSelf = 0x208ba90;    // j_scePthreadSelf
constexpr u32 kMovieWaitStepUs = 100000;
constexpr auto kMovieWaitBound = std::chrono::seconds(5);

using MovieDrawFn = BB_SYSV u64 (*)(u64, u64, u64);
using MovieCloseFn = BB_SYSV u64 (*)(u64);
using EventWaitFn = BB_SYSV u64 (*)(u64, u64);
using SelfFn = BB_SYSV u64 (*)();
bool movie_wait_on = false;
MovieDrawFn movie_draw_orig = nullptr;
MovieCloseFn movie_close_orig = nullptr;
std::atomic<u64> movie_draws{0}, movie_immediate{0}, movie_busy{0}, movie_closes{0}, movie_closes_busy{0},
    movie_bounded{0}, movie_waited_us{0}, movie_longest_us{0};

/// Waits until GXRenderThread (object `rt`) is idle; no wait from the render thread itself. `busy` counts
/// the call when it had to wait. Bounded; the game's call goes ahead after the bound (logged).
void MovieWaitIdle(u64 rt, const char* what, u64 n, std::atomic<u64>& busy) {
    const u64 event = rt + kRtIdleEvent;
    if ((Rd32(event + kEventFlags) & kEventSet) != 0) {
        return;
    }
    const u64 base = reinterpret_cast<u64>(image);
    const u64 thread = Rd64(rt + kRtThread);
    if (thread >= 0x10000 && Rd64(thread + kThreadPthread) == reinterpret_cast<SelfFn>(image + kPthreadSelf)()) {
        return; // the render thread itself
    }
    if (Rd64(event) != base + kEventVtable) {
        return; // not the event we know
    }
    const u64 k = busy.fetch_add(1, std::memory_order_relaxed);
    const auto wait = reinterpret_cast<EventWaitFn>(Rd64(Rd64(event) + kEventVtableWait));
    const auto t0 = std::chrono::steady_clock::now();
    bool done = true;
    while ((Rd32(event + kEventFlags) & kEventSet) == 0) {
        if (std::chrono::steady_clock::now() - t0 > kMovieWaitBound) {
            done = false;
            break;
        }
        wait(event, kMovieWaitStepUs);
    }
    const u64 us = u64(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
    movie_waited_us.fetch_add(us, std::memory_order_relaxed);
    u64 longest = movie_longest_us.load(std::memory_order_relaxed);
    while (us > longest && !movie_longest_us.compare_exchange_weak(longest, us, std::memory_order_relaxed)) {
    }
    if (!done) {
        if (movie_bounded.fetch_add(1, std::memory_order_relaxed) < 8) {
            std::printf("Stability: movie: the %s %llu waited %llu ms for GXRenderThread to go idle and goes ahead without it\n",
                        what, ull(n), ull(us / 1000));
        }
    } else if (k == 0 || ((k + 1) & k) == 0) {
        // The first wait, then the 2nd, 4th, 8th ... (the exit report is lost when the game is killed).
        std::printf("Stability: movie: the %s %llu waited %llu us for GXRenderThread to finish its frame first (wait %llu, %llu ms "
                    "in all)\n", what, ull(n), ull(us), ull(k + 1), ull(movie_waited_us.load() / 1000));
    }
}

/// GX's render thread object, or 0 before GX is up; `immediate` = GX's immediate context.
u64 MovieRenderThread(u64* immediate) {
    const u64 gx = Rd64(reinterpret_cast<u64>(image) + kGxManager);
    if (gx < 0x10000) {
        return 0;
    }
    const u64 wrapper = Rd64(gx + kGxImmediateWrapper);
    *immediate = wrapper >= 0x10000 ? Rd64(wrapper + 8) : 0;
    return wrapper >= 0x10000 ? Rd64(gx + kGxRenderThread) : 0;
}

BB_SYSV u64 MovieDraw(u64 player, u64 ctx, u64 out) {
    const u64 n = movie_draws.fetch_add(1, std::memory_order_relaxed);
    if (n == 0 || (n & 1023) == 0) {
        std::printf("Stability: movie: frame draw %llu (%llu waited so far, %llu ms)\n", ull(n), ull(movie_busy.load()),
                    ull(movie_waited_us.load() / 1000));
        std::fflush(stdout);
    }
    u64 immediate = 0;
    const u64 rt = MovieRenderThread(&immediate);
    if (rt >= 0x10000 && ctx && ctx == immediate) {
        movie_immediate.fetch_add(1, std::memory_order_relaxed);
        MovieWaitIdle(rt, "draw", n, movie_busy);
    }
    return movie_draw_orig(player, ctx, out);
}

BB_SYSV u64 MovieClose(u64 player) {
    const u64 n = movie_closes.fetch_add(1, std::memory_order_relaxed);
    u64 immediate = 0;
    const u64 rt = MovieRenderThread(&immediate);
    if (rt >= 0x10000) {
        MovieWaitIdle(rt, "close", n, movie_closes_busy);
    }
    return movie_close_orig(player);
}

void MovieReport() {
    if (movie_draws || movie_closes) {
        std::printf("Stability: movie: %llu frame draws (%llu on the immediate context, %llu waited for GXRenderThread), "
                    "%llu closes (%llu waited); waited %llu ms in all, longest %llu ms, %llu went ahead after the bound\n",
                    ull(movie_draws), ull(movie_immediate), ull(movie_busy), ull(movie_closes), ull(movie_closes_busy),
                    ull(movie_waited_us / 1000), ull(movie_longest_us / 1000), ull(movie_bounded));
        std::fflush(stdout);
    }
}

bool InstallMovieWait() {
    if (!MatchesBytes(kMovieDraw, kMovieDrawPrologue, sizeof(kMovieDrawPrologue)) ||
        !MatchesBytes(kMovieClose, kMovieClosePrologue, sizeof(kMovieClosePrologue))) {
        std::printf("Stability: movie: guest +0x%llx / +0x%llx is not the expected code; left alone\n", ull(kMovieDraw),
                    ull(kMovieClose));
        return false;
    }
    const u64 draw_orig = PlaceTrampoline(kMovieDraw, kMovieDrawPrologue, sizeof(kMovieDrawPrologue));
    const u64 close_orig = PlaceTrampoline(kMovieClose, kMovieClosePrologue, sizeof(kMovieClosePrologue));
    Emitter d = NewStub();
    d.JmpAbs(reinterpret_cast<const void*>(&MovieDraw));
    const u64 draw_stub = PlaceStub(d);
    Emitter c = NewStub();
    c.JmpAbs(reinterpret_cast<const void*>(&MovieClose));
    const u64 close_stub = PlaceStub(c);
    if (!draw_orig || !close_orig || !draw_stub || !close_stub) {
        std::printf("Stability: movie: no stub space; left alone\n");
        return false;
    }
    movie_draw_orig = reinterpret_cast<MovieDrawFn>(draw_orig);
    movie_close_orig = reinterpret_cast<MovieCloseFn>(close_orig);
    return WriteBranch(kMovieDraw, 0xe9, draw_stub, sizeof(kMovieDrawPrologue) - 5) &&
           WriteBranch(kMovieClose, 0xe9, close_stub, sizeof(kMovieClosePrologue) - 5);
}

// ======================================================================================
// 18. Render task stealing (the scene's "Enable Task Stealing", byte +0x78bd of the graphics manager, which
// the game's constructor clears). The community Performance Patch v3 sets it (its site: the constructor's
// immediate at +0x2295d04); Live FPS locks that patch out, so it stays 0 here. droogie/bbhost (render_queue.cpp,
// graphics_patch.cpp, GPL-3.0) cuts the render queue's runs into GX jobs by that byte: with it every
// run the table cuts is one job that GX's job system splits as it runs (a pop takes a grain, a steal the
// upper half), which evens the load between the worker threads.
// BB_TASK_STEAL=1: the immediate is 1 (set at load); =2: A/B test aid, the manager's byte follows the
// experiment bit 512 (off at start), written once a second by LogCpuSave (BB_LIVE_FPS_LOG); 0: the game's.
// ======================================================================================
constexpr u64 kTaskStealImm = 0x2295d04;
constexpr u64 kGraphicsManagerSlot = 0x5540dd8; // pointer to the graphics manager
constexpr u64 kManagerTaskSteal = 0x78bd;
int task_steal_mode = 0;
inline bool TaskStealWanted() { return (task_steal_mode == 1) != BbToggle::Experiment(512); }

bool InstallTaskSteal(int mode) {
    // mov byte [rbx + 0x78bd], 0: the immediate is the instruction's last byte.
    if (!Matches(kTaskStealImm - 6, {0xc6, 0x83, 0xbd, 0x78, 0x00, 0x00, 0x00})) {
        std::printf("Stability: task steal: guest +0x%llx is not the expected code (%02x %02x %02x %02x %02x %02x %02x); left alone\n",
                    ull(kTaskStealImm), image[kTaskStealImm - 6], image[kTaskStealImm - 5], image[kTaskStealImm - 4],
                    image[kTaskStealImm - 3], image[kTaskStealImm - 2], image[kTaskStealImm - 1], image[kTaskStealImm]);
        return false;
    }
    task_steal_mode = mode;
    if (mode == 1) {
        image[kTaskStealImm] = 1;
    }
    return true;
}

/// Mode 2: the manager's byte follows the experiment bit (once a second).
void TaskStealTick() {
    if (task_steal_mode != 2) {
        return;
    }
    const u64 gm = Rd64(reinterpret_cast<u64>(image) + kGraphicsManagerSlot);
    if (gm >= 0x10000) {
        *reinterpret_cast<volatile u8*>(gm + kManagerTaskSteal) = TaskStealWanted() ? 1 : 0;
    }
}

// ======================================================================================
// 16. Guest CPU savings (default off; BB_JOB_BARRIER_STEAL / BB_WORKPOOL_NODE_CACHE =1 on, =2 installed
// but off at start, so BB_TOGGLE_FILE experiment bits 128 / 256 turn them on while the game runs:
// A/B in one session).
//
// Ported idea: droogie/bbhost eaacf93 (GX job barrier steal) and 58af1c8 (work-pool node cache), GPL-3.
// Reimplemented here as a wrapper around the game's code instead of a rewrite of it.
//
// a) The main thread waits in GX's job barrier (+0x21b8710) after each parallel-for of GX jobs. The
// barrier calls its step (+0x21b9980, the only caller) while the main thread's own deque has jobs,
// then spins until every worker is idle; the step steals from the other participants only when the
// main thread comes in with an empty deque. So once its own jobs are done the main thread never
// steals again, and whatever is queued on the workers runs there while it spins. The wrapper calls
// the game's step and then, when the main thread's deque is empty, makes the same sweep once more
// without taking a job (a deque it stole into keeps the barrier calling the step).
// GX's job system (the object at +0): participant array at [+8]->[0], 0x530 bytes each, count at
// [[+8]+8]; a participant's deque at +0x18 (count at +0x38), its index at +0xa0.
//
// b) CSEzWork::ProcessOne (+0x2032370): every push of a job allocates a 0x18-byte list node and every
// pop frees one, both under the pool's queue lock and through the engine's general allocator, whose
// heap has a lock of its own; the worker threads convoy on the queue lock. The first time a worker
// pops from a pool whose job list still allocates from the general allocator, a stand-in allocator
// (a copy of that object with its aligned alloc +0x58 and free +0x70 ours) goes into the list's
// allocator slot (pool +0x28): a free keeps the node (up to 1,024 per pool), an alloc of a node
// takes one back, anything else goes to the general allocator. Every alloc and free on a pool's list
// runs under the pool's queue lock (a DLAdaptiveMutex at +0x30; lock = vtable +0x18, unlock +0x28)
// or in the destructor, so the cache takes no lock of its own. ~CSEzWork frees the list's sentinel
// last, through the slot: that free ends the pool's cache.
// ======================================================================================
constexpr u64 kBarrierCall = 0x21b8793;  // call +0x21b9980 in the barrier
constexpr u64 kBarrierStep = 0x21b9980;
constexpr u64 kDequeSteal = 0xe47d20;    // (victim deque, thief deque): al true when it moved jobs
constexpr u8 kBarrierStepPrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55};
constexpr u64 kParticipantSize = 0x530;

constexpr u64 kProcessOne = 0x2032370;
constexpr u8 kProcessOnePrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56};
constexpr u64 kGeneralAllocator = 0x5540420;  // pointer to the engine's general allocator object
constexpr u64 kAllocatorVtable = 0x53aefa0;
constexpr int kVtableSlots = 28;
constexpr u64 kAllocSlot = 0x58, kFreeSlot = 0x70;
constexpr u64 kNodeSize = 0x18;
constexpr u32 kCacheCap = 1024;
constexpr int kCaches = 32;

bool barrier_start_on = false, cache_start_on = false, cpusave_installed = false;
/// Switched at run time by BB_TOGGLE_FILE experiment bit 128 / 256 (flips the start state).
inline bool BarrierActive() { return barrier_start_on != BbToggle::Experiment(128); }
inline bool CacheActive() { return cache_start_on != BbToggle::Experiment(256); }

struct BarrierStats {
    std::atomic<u64> calls{0}, sweeps{0}, steals{0};
} barrier_stats;

using StepFn = BB_SYSV void (*)(u64);
using StealFn = BB_SYSV u64 (*)(u64, u64);
using LockFn = BB_SYSV u64 (*)(u64, u64);
using UnlockFn = BB_SYSV u64 (*)(u64);
using AllocFn = BB_SYSV u64 (*)(u64, u64, u64);
using FreeFn = BB_SYSV u64 (*)(u64, u64);

/// Replaces the barrier's `call +0x21b9980`: the game's step, then one more sweep.
BB_SYSV void BarrierStep(u64 self) {
    reinterpret_cast<StepFn>(image + kBarrierStep)(self);
    barrier_stats.calls.fetch_add(1, std::memory_order_relaxed);
    if (!BarrierActive() || Rd64(self + 0x38) != 0) {
        return;
    }
    barrier_stats.sweeps.fetch_add(1, std::memory_order_relaxed);
    u64 system = Rd64(self + 8);
    u32 n = Rd32(system + 8);
    if (n == 0) {
        return;
    }
    const u32 start = u32((u64(Rd32(self + 0xa0)) + __builtin_ia32_rdtsc()) % n);
    const auto steal = reinterpret_cast<StealFn>(image + kDequeSteal);
    for (u32 i = 0; i < n;) {
        const u64 p = Rd64(system) + u64((start + i) % n) * kParticipantSize;
        if (p != self) {
            if ((steal(p + 0x18, self + 0x18) & 0xff) != 0 || Rd64(self + 0x38) != 0) {
                barrier_stats.steals.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            system = Rd64(self + 8);
        }
        ++i;
        n = Rd32(system + 8);
    }
}

bool InstallBarrierSteal() {
    if (!MatchesBytes(kBarrierStep, kBarrierStepPrologue, sizeof(kBarrierStepPrologue)) ||
        !CallsTo(kBarrierCall, kBarrierStep)) {
        std::printf("Stability: job barrier: guest +0x%llx is not the expected code; left alone\n", ull(kBarrierStep));
        return false;
    }
    Emitter e = NewStub();
    e.JmpAbs(reinterpret_cast<const void*>(&BarrierStep));
    return WriteBranch(kBarrierCall, 0xe8, PlaceStub(e), 0);
}

enum : u32 { kCacheFree, kCacheClaimed, kCacheInUse };
struct alignas(64) NodeCache {
    u64 vtable;    // -> slots
    u64 heap;      // the general allocator's +0x8, which the methods not replaced read
    u64 original;  // the general allocator object it stands in for
    u64 sentinel;  // the pool's job list sentinel: its free is the pool's end
    u64 top;       // nodes kept, linked through their first word
    u32 count;
    std::atomic<u32> state;
    u64 hits, misses, kept, passed, ends;  // written under the pool's lock
    u64 slots[kVtableSlots];
};
NodeCache node_caches[kCaches];
std::atomic<u32> cache_full{0};

template <class F>
inline F Method(u64 object, u64 offset) {
    return reinterpret_cast<F>(Rd64(Rd64(object) + offset));
}

inline NodeCache& CacheOf(u64 self) { return *reinterpret_cast<NodeCache*>(self); }

BB_SYSV u64 NodeCacheAlloc(u64 self, u64 size, u64 align) {
    NodeCache& c = CacheOf(self);
    if (size == kNodeSize && align <= 8 && c.top && CacheActive()) {
        const u64 node = c.top;
        c.top = Rd64(node);
        --c.count;
        ++c.hits;
        return node;
    }
    ++c.misses;
    return Method<AllocFn>(c.original, kAllocSlot)(c.original, size, align);
}

BB_SYSV u64 NodeCacheFree(u64 self, u64 p) {
    NodeCache& c = CacheOf(self);
    const u64 original = c.original;
    if (p != 0 && p == c.sentinel) {
        // ~CSEzWork's last free: the pool is gone, and so is its cache.
        for (u32 n = c.count < kCacheCap ? c.count : kCacheCap; n != 0 && c.top; --n) {
            const u64 node = c.top;
            c.top = Rd64(node);
            Method<FreeFn>(original, kFreeSlot)(original, node);
        }
        c.top = 0;
        c.count = 0;
        ++c.ends;
        c.sentinel = 0;
        c.state.store(kCacheFree, std::memory_order_release);
        return Method<FreeFn>(original, kFreeSlot)(original, p);
    }
    if (p != 0 && c.count < kCacheCap && CacheActive()) {
        std::memcpy(reinterpret_cast<void*>(p), &c.top, 8);
        c.top = p;
        ++c.count;
        ++c.kept;
        return 0;
    }
    ++c.passed;
    return Method<FreeFn>(original, kFreeSlot)(original, p);
}

/// Under the pool's queue lock: the stand-in goes into the pool's allocator slot.
void InstallNodeCache(u64 pool, u64 general) {
    for (NodeCache& c : node_caches) {
        u32 expect = kCacheFree;
        if (!c.state.compare_exchange_strong(expect, kCacheClaimed, std::memory_order_acquire)) {
            continue;
        }
        const u64 vt = Rd64(general);
        for (int i = 0; i < kVtableSlots; ++i) {
            c.slots[i] = Rd64(vt + 8ull * u64(i));
        }
        c.slots[kAllocSlot / 8] = reinterpret_cast<u64>(&NodeCacheAlloc);
        c.slots[kFreeSlot / 8] = reinterpret_cast<u64>(&NodeCacheFree);
        c.vtable = reinterpret_cast<u64>(&c.slots[0]);
        c.heap = Rd64(general + 8);
        c.original = general;
        c.sentinel = Rd64(pool + 0x18);
        c.top = 0;
        c.count = 0;
        c.state.store(kCacheInUse, std::memory_order_release);
        const u64 stand_in = reinterpret_cast<u64>(&c);
        std::memcpy(reinterpret_cast<void*>(pool + 0x28), &stand_in, 8);
        return;
    }
    cache_full.fetch_add(1, std::memory_order_relaxed);
}

/// Called at the entry of ProcessOne (rdi: the pool).
BB_SYSV void WorkPoolSeen(u64 pool) {
    const u64 general = Rd64(reinterpret_cast<u64>(image) + kGeneralAllocator);
    if (general == 0 || Rd64(pool + 0x28) != general || !CacheActive()) {
        return;
    }
    const u64 lock = pool + 0x30;
    Method<LockFn>(lock, 0x18)(lock, 0xffffffffu);
    if (Rd64(pool + 0x28) == general && Rd64(general) == reinterpret_cast<u64>(image) + kAllocatorVtable) {
        InstallNodeCache(pool, general);
    }
    Method<UnlockFn>(lock, 0x28)(lock);
}

bool InstallWorkPoolCache() {
    if (!MatchesBytes(kProcessOne, kProcessOnePrologue, sizeof(kProcessOnePrologue))) {
        std::printf("Stability: work pool: guest +0x%llx is not the expected code; left alone\n", ull(kProcessOne));
        return false;
    }
    Emitter e = NewStub();
    e.SaveVolatile();
    e.CallAbs(reinterpret_cast<const void*>(&WorkPoolSeen));
    e.RestoreVolatile();
    for (const u8 b : kProcessOnePrologue) {
        e.Bytes({b});
    }
    e.JmpTo(reinterpret_cast<u64>(image) + kProcessOne + sizeof(kProcessOnePrologue));
    return WriteBranch(kProcessOne, 0xe9, PlaceStub(e), sizeof(kProcessOnePrologue) - 5);
}

/// 0: off (default), 1: on, 2: installed but off at start (experiment bit turns it on).
int CpuSaveMode(const char* name) {
    const char* e = std::getenv(name);
    return e && (e[0] == '1' || e[0] == '2') ? e[0] - '0' : 0;
}

void CpuSaveReport() {
    u64 hits = 0, misses = 0, kept = 0, passed = 0, ends = 0, live = 0;
    for (const NodeCache& c : node_caches) {
        hits += c.hits, misses += c.misses, kept += c.kept, passed += c.passed, ends += c.ends;
        live += c.state.load() == kCacheInUse;
    }
    std::printf("CPU save: barrier %s calls %llu sweeps %llu steals %llu | node cache %s pools %llu ended %llu hits %llu "
                "misses %llu kept %llu passed %llu full %u | task steal %s\n",
                BarrierActive() ? "on" : "off", ull(barrier_stats.calls.load()), ull(barrier_stats.sweeps.load()),
                ull(barrier_stats.steals.load()), CacheActive() ? "on" : "off", ull(live), ull(ends), ull(hits),
                ull(misses), ull(kept), ull(passed), cache_full.load(),
                task_steal_mode == 0 ? "off" : TaskStealWanted() ? "on" : "off");
    std::fflush(stdout);
}

} // namespace

void LogCpuSave() {
    TaskStealTick();
    if (cpusave_installed) {
        CpuSaveReport();
    }
}

void PatchImage(unsigned char* image_, std::uint64_t size) {
    image = image_;
    image_size = size;
    std::string active;
    auto note = [&](const std::string& what) {
        active += active.empty() ? what : "; " + what;
    };
    note("dmem " + std::to_string(runtime_memory_pool_size() / kMiB) + "MiB");
    note(HeapTable());
    note(GxRegion());
    note(FastRespawn());
    stub_page = MapNear(StubPageSize);
    if (!stub_page) {
        std::printf("Stability: no memory near the image for stubs; code fixes off\n");
    }
    const bool probe = On("BB_ARENA_PROBE") && stub_page && InstallArenaProbe();
    note(std::string("arena-probe ") + (probe ? "on" : "off"));
    const bool guard = On("BB_ARENA_GUARD") && stub_page && InstallArenaGuard();
    const bool guard2 = On("BB_ARENA_GUARD") && stub_page && InstallArenaGuard2();
    note(std::string("arena-guard ") + (guard && guard2 ? "on" : guard || guard2 ? "partly" : "off") +
         (fail_test_every ? " (fail test 1/" + std::to_string(fail_test_every) + ")" : ""));
    note(FrameArena());
    const bool flip_probe = On("BB_ARENA_PROBE") && stub_page && InstallFrameFlipProbe();
    note(std::string("frame-arena-probe ") + (flip_probe ? "on" : "off"));
    const bool reclaim = On("BB_GX_RECLAIM_WAIT") && stub_page && InstallReclaimWait();
    note(reclaim ? "gx-reclaim-wait " + std::to_string(reclaim_budget.count() / 1000) + "ms" : "gx-reclaim-wait off");
    const bool pool = On("BB_FRAME_POOL_FIX") && stub_page && InstallFramePool();
    note(std::string("frame-pool-fix ") + (pool ? "on" : "off"));
    const bool ribbons = On("BB_RIBBON_FIX") && stub_page && InstallRibbons();
    note(std::string("ribbon-fix ") + (ribbons ? "on" : "off"));
    const bool blend = On("BB_BLEND_MASK_FIX") && InstallBlendMask();
    note(std::string("blend-mask-fix ") + (blend ? "on" : "off"));
    const bool cache = stub_page && InstallResourceCache();
    note(std::string("resource-cache ") + (cache ? "on" : "off"));
    const bool kernel = On("BB_KERNEL_CLAMP") && stub_page && InstallKernelSampleClamp();
    note(std::string("kernel-clamp ") + (kernel ? "on" : "off"));
    const bool query = On("BB_GX_QUERY_GUARD") && stub_page && InstallQueryGuard();
    note(std::string("gx-query-guard ") + (query ? "on" : "off") + (query && query_test_every ? " (test aid)" : ""));
    sf_lock_on = On("BB_SF_MOVIE_LOCK") && stub_page && InstallHudLock();
    note(std::string("sf-movie-lock ") + (sf_lock_on ? "on" : "off"));
    sf_guard_on = On("BB_SF_TEXT_GUARD") && stub_page && InstallTextGuard();
    note(std::string("sf-text-guard ") + (sf_guard_on ? "on" : "off"));
    const bool fmod_ring = On("BB_FMOD_RING_GUARD") && stub_page && InstallFmodGuard();
    note(std::string("fmod-ring-guard ") + (fmod_ring ? "on" : "off"));
    movie_wait_on = On("BB_MOVIE_DRAW_WAIT") && stub_page && InstallMovieWait();
    note(std::string("movie-wait ") + (movie_wait_on ? "on" : "off"));
    // The Havok behaviour event guard is in the crash handler (src/probe.c, src/havok_event_guard.h):
    // only reported here, with the same byte check it makes on the first fault.
    note(std::string("havok-event-guard ") +
         (On("BB_HAVOK_EVENT_GUARD") && heg_image_matches(image, image_size) ? "on" : "off"));
    if (const char* e = std::getenv("BB_HAVOK_EVENT_TEST")) {
        const long every = std::strtol(e, nullptr, 10);
        if (every > 0 && stub_page && InstallHavokEventTest(u32(every))) {
            note("havok-event-test 1/" + std::to_string(every));
        }
    }
    const int barrier_mode = CpuSaveMode("BB_JOB_BARRIER_STEAL");
    const int cache_mode = CpuSaveMode("BB_WORKPOOL_NODE_CACHE");
    barrier_start_on = barrier_mode == 1;
    cache_start_on = cache_mode == 1;
    const bool barrier = barrier_mode && stub_page && InstallBarrierSteal();
    note(std::string("job-barrier-steal ") + (barrier ? (barrier_mode == 1 ? "on" : "installed, off") : "off"));
    const bool pool_cache = cache_mode && stub_page && InstallWorkPoolCache();
    note(std::string("workpool-node-cache ") + (pool_cache ? (cache_mode == 1 ? "on" : "installed, off") : "off"));
    const int steal_mode = CpuSaveMode("BB_TASK_STEAL");
    const bool steal = steal_mode && InstallTaskSteal(steal_mode);
    note(std::string("task-steal ") + (steal ? (steal_mode == 1 ? "on" : "installed, off") : "off"));
    cpusave_installed = barrier || pool_cache || steal;
    if (sf_lock_on || sf_guard_on || fmod_ring || movie_wait_on || cpusave_installed) {
        std::atexit([] {
            SfReport();
            FmodReport();
            MovieReport();
            if (cpusave_installed) {
                CpuSaveReport();
            }
        });
    }
    if (stub_page) {
#ifdef _WIN32
        DWORD unused = 0;
        VirtualProtect(stub_page, StubPageSize, PAGE_EXECUTE_READ, &unused);
        FlushInstructionCache(GetCurrentProcess(), stub_page, StubPageSize);
#else
        mprotect(stub_page, StubPageSize, PROT_READ | PROT_EXEC);
#endif
    }
    std::printf("Stability fixes: %s\n", active.c_str());
    std::fflush(stdout);
}

} // namespace BbStability

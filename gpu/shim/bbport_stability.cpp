// SPDX-License-Identifier: GPL-3.0-or-later
// bbport: long-session stability fixes in the game's code. Most are from droogie/bbhost,
// GPL-3.0-or-later (each says where); bbhost names Binary Ninja addresses (image + 0x400000),
// this file guest offsets (image + 0): bbhost 0x2aaa860 is guest +0x26aa860 here.
//
// Each site is compared byte for byte with the 1.09 code first; a site that differs (another
// version, or a patch file that changed it) is left alone with a log line. Switches (default on):
//   BB_HEAP_TABLE=0        the SprjMemory heap size growth (Scaleform/MENU; GFX above 1080p)
//   BB_GFX_HEAP_MORE_MIB=N the GFX_GraphicsPrivate growth (runtime_memory.c; 0: none)
//   BB_ARENA_PROBE=0       the log when the frame arena allocator (+0xe45560) returns NULL
//   BB_ARENA_GUARD=0       the NULL check at +0x21d5b35 (drops the render command instead)
//   BB_GX_RECLAIM_WAIT=0   the GPU catch-up wait in the GX resource-table block reclaim
//                          (BB_GX_RECLAIM_WAIT_MS: its bound, default 100)
//   BB_FRAME_POOL_FIX=0    the frame loop's pool borrow (+0x1067f60) is the game's own
//   BB_RIBBON_FIX=0        the effect ribbon writers (+0x28ce7b0/+0x28ceec0) are the game's own
//   BB_BLEND_MASK_FIX=0    the blend-state constructor's write mask (+0x2a51640) is not cleared
#include "bbport_stability.h"
#include "bbport_ribbons.h"

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
constexpr std::size_t StubPageSize = 4096;

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

std::atomic<u64> arena_nulls{0}, commands_dropped{0};

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

BB_SYSV void CommandDropped(u64 frame_ctx, u64 command) {
    const u64 n = commands_dropped.fetch_add(1, std::memory_order_relaxed);
    if (n < 8 || (n & 1023) == 0) {
        std::printf("Stability: render command 0x%llx dropped (#%llu): frame context 0x%llx's arena returned NULL "
                    "(guest +0x21d5b20)\n",
                    ull(command), ull(n + 1), ull(frame_ctx));
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
    e.CallAbs(reinterpret_cast<const void*>(&CommandDropped));
    e.RestoreVolatile();
    e.BackRedZone();
    e.JmpTo(reinterpret_cast<u64>(image) + kGuardExit);
    return WriteBranch(kGuardSite, 0xe9, PlaceStub(e), 0);
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

} // namespace

void PatchImage(unsigned char* image_, std::uint64_t size) {
    image = image_;
    image_size = size;
    std::string active;
    auto note = [&](const std::string& what) {
        active += active.empty() ? what : "; " + what;
    };
    note("dmem " + std::to_string(runtime_memory_pool_size() / kMiB) + "MiB");
    note(HeapTable());
    stub_page = MapNear(StubPageSize);
    if (!stub_page) {
        std::printf("Stability: no memory near the image for stubs; code fixes off\n");
    }
    const bool probe = On("BB_ARENA_PROBE") && stub_page && InstallArenaProbe();
    note(std::string("arena-probe ") + (probe ? "on" : "off"));
    const bool guard = On("BB_ARENA_GUARD") && stub_page && InstallArenaGuard();
    note(std::string("arena-guard ") + (guard ? "on" : "off"));
    const bool reclaim = On("BB_GX_RECLAIM_WAIT") && stub_page && InstallReclaimWait();
    note(reclaim ? "gx-reclaim-wait " + std::to_string(reclaim_budget.count() / 1000) + "ms" : "gx-reclaim-wait off");
    const bool pool = On("BB_FRAME_POOL_FIX") && stub_page && InstallFramePool();
    note(std::string("frame-pool-fix ") + (pool ? "on" : "off"));
    const bool ribbons = On("BB_RIBBON_FIX") && stub_page && InstallRibbons();
    note(std::string("ribbon-fix ") + (ribbons ? "on" : "off"));
    const bool blend = On("BB_BLEND_MASK_FIX") && InstallBlendMask();
    note(std::string("blend-mask-fix ") + (blend ? "on" : "off"));
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

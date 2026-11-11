// SPDX-License-Identifier: GPL-3.0-or-later
// Load speed: see bbport_load_speed.h. Offsets are ours (bbhost - 0x400000).
#include "bbport_load_speed.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "party/coop_hooks.h"

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef BB_HAVE_ZLIB_NG
#include "bbport_fast_inflate.h"
#ifdef BB_HAVE_ZSTD
#include "bbport_inflate_bench.h"
#include "bbport_zstd_inflate.h"
#include "bbport_zstd_files.h"
#endif
#endif

namespace BbLoadSpeed {
namespace {

using u32 = std::uint32_t;
using u64 = std::uint64_t;

// ---- more post-processor threads -------------------------------------------------------------------------------
// The resource repository (FUN_020c4240) reads "Core.Resource.PostProcessorNum" (default 1; the game's registry holds
// 3) and makes that many PostProcessor objects of 0x4c8 bytes, each with its own thread "Core.Res.PostProcessor_%d"
// (FUN_020d8eb0); the thread affinity lookups past the third fall back to "none". The call is the one at +0x20c4761.
constexpr u64 kNumCall = 0x20c4761, kRegistryGetInt = 0x20ebb50;
using GetIntFn = u32(BB_COOP_SYSV*)(u64 registry, u64 name, u32 fallback);
std::atomic<u32> g_wanted{0};

BB_COOP_SYSV u32 PostProcessorNum(u64 registry, u64 name, u32 fallback) {
    const u32 game = reinterpret_cast<GetIntFn>(coop::Guest(kRegistryGetInt))(registry, name, fallback);
    const u32 n = std::max(game, g_wanted.load(std::memory_order_relaxed));
    std::printf("Load speed: post-processor threads %u (the game's %u; BB_LOAD_WORKERS=0 keeps the game's)\n", n, game);
    std::fflush(stdout);
    return n;
}

// Default: the game's own count. More post-processor threads shorten loads (about 0.4 s of a 4 s warp with 4-6) but
// the engine loses a job now and then with more than 3 (a hung load in 2 of 6 sessions with 6), so this is opt-in.
u32 DefaultWorkers(u32) { return 0; }

// "0" / "off": no change. Unset: the default. A number: that many (1..8).
u32 ParseWorkers(const char* v, u32 hw) {
    if (!v || !*v) return DefaultWorkers(hw);
    if (!std::strcmp(v, "off") || !std::strcmp(v, "false")) return 0;
    const long n = std::strtol(v, nullptr, 10);
    return n <= 0 ? 0 : u32(std::min<long>(n, 8));
}

// Logical CPUs the game may run on: the machine's, less what an affinity mask takes away (BB_AFFINITY_MASK is applied
// later by the audio start, so it is read here too).
u32 UsableCpus() {
    u32 n = std::max(1u, std::thread::hardware_concurrency());
#ifdef _WIN32
    DWORD_PTR process = 0, system = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &process, &system) && process) {
        n = std::min<u32>(n, u32(__builtin_popcountll(u64(process))));
    }
#endif
    if (const char* m = std::getenv("BB_AFFINITY_MASK")) {
        const u64 mask = std::strtoull(m, nullptr, 16);
        if (mask) n = std::min<u32>(n, u32(__builtin_popcountll(mask)));
    }
    return std::max(1u, n);
}

void InstallWorkers() {
    const u32 hw = UsableCpus();
    const u32 n = ParseWorkers(std::getenv("BB_LOAD_WORKERS"), hw);
    if (n == 0) {
        std::printf("Load speed: post-processor threads are the game's own (BB_LOAD_WORKERS=0)\n");
        return;
    }
    g_wanted.store(n);
    const bool ok = coop::HookCallSite(kNumCall, kRegistryGetInt, reinterpret_cast<const void*>(&PostProcessorNum),
                                       "load speed: post-processor count");
    std::printf("Load speed: BB_LOAD_WORKERS %u (%u hardware threads): %s\n", n, hw, ok ? "hooked" : "NOT hooked");
}

// ---- zlib-ng behind the engine's inflate -----------------------------------------------------------------------
#ifdef BB_HAVE_ZLIB_NG
// zlib 1.2.3 inside the executable: inflateReset +0x2067850, inflateInit2_ +0x2067900, inflate +0x2067ac0,
// inflateEnd +0x20697c0 (the stream readers at +0x2065cf0 and +0x2066b70 call init2_ / inflate; their ends and
// resets run through the same functions).
constexpr u64 kReset = 0x2067850, kInit2 = 0x2067900, kInflate = 0x2067ac0, kEnd = 0x20697c0;
using Z = BbFastInflate::GuestZ;
BbFastInflate::Pool g_pool_zlib{BbFastInflate::kZlibWindowBits}, g_pool_raw{BbFastInflate::kRawWindowBits};
// z->reserved: the zlib-ng stream's address, bit 0 set for a raw (windowBits -15) one (stream objects are 8-aligned).
// Bit 1 set (zstd081): a zstd stream; the address is its BbZstdInflate::State, which holds the zlib-ng stream.
BbFastInflate::Pool& PoolOf(std::uint64_t tagged) { return tagged & 1 ? g_pool_raw : g_pool_zlib; }
zng_stream* StreamOf(std::uint64_t tagged) { return reinterpret_cast<zng_stream*>(tagged & ~std::uint64_t(3)); }
std::atomic<std::uint64_t> g_streams{0}, g_fast_calls{0}, g_in_bytes{0}, g_out_bytes{0};

#ifdef BB_HAVE_ZSTD
BbZstdInflate::Pool g_zstd_pool;
std::atomic<std::uint64_t> g_zstd_streams{0}, g_zstd_in{0}, g_zstd_out{0}, g_zstd_ns{0};
BbZstdInflate::State* ZStateOf(std::uint64_t tagged) { return BbZstdInflate::StateOf(tagged); }
bool ZstdInflate(Z* z, std::int32_t flush, std::int64_t* rc) {
    BbZstdInflate::Stats st;
    const auto t0 = std::chrono::steady_clock::now();
    if (!BbZstdInflate::Route(z, flush, g_zstd_pool, rc, &st)) return false;
    g_zstd_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count(),
                        std::memory_order_relaxed);
    g_zstd_streams.fetch_add(st.streams, std::memory_order_relaxed);
    g_zstd_in.fetch_add(st.in_bytes, std::memory_order_relaxed);
    g_zstd_out.fetch_add(st.out_bytes, std::memory_order_relaxed);
    static std::atomic<std::uint64_t> calls{0};
    if (calls.fetch_add(1, std::memory_order_relaxed) % 4000 == 3999) {
        std::printf("Load speed: zstd inflate: %llu streams, %llu MB in, %llu MB out, %.2f s in the decoder\n",
                    static_cast<unsigned long long>(g_zstd_streams.load()),
                    static_cast<unsigned long long>(g_zstd_in.load() >> 20),
                    static_cast<unsigned long long>(g_zstd_out.load() >> 20), g_zstd_ns.load() * 1e-9);
        std::fflush(stdout);
    }
    return true;
}
#endif

using InitFn = std::int64_t(BB_COOP_SYSV*)(Z*, std::int32_t, const char*, std::int32_t);
using InflateFn = std::int64_t(BB_COOP_SYSV*)(Z*, std::int32_t);
using OneFn = std::int64_t(BB_COOP_SYSV*)(Z*);
InitFn g_init = nullptr;
InflateFn g_inflate = nullptr;
OneFn g_reset = nullptr, g_end = nullptr;

BB_COOP_SYSV std::int64_t InitHook(Z* z, std::int32_t window_bits, const char* version, std::int32_t size) {
    if (z) z->reserved = 0;
    const std::int64_t rc = g_init(z, window_bits, version, size);
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 6) {
        std::printf("Load speed: inflateInit2_ window_bits %d rc %lld size %d\n", window_bits, static_cast<long long>(rc), size);
    }
    const bool raw = window_bits == BbFastInflate::kRawWindowBits;
    if (rc == 0 && z && (raw || window_bits == BbFastInflate::kZlibWindowBits)) {
        if (zng_stream* s = (raw ? g_pool_raw : g_pool_zlib).Acquire()) {
            z->reserved = reinterpret_cast<std::uint64_t>(s) | (raw ? 1 : 0);
            g_streams.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return rc;
}

BB_COOP_SYSV std::int64_t InflateHook(Z* z, std::int32_t flush) {
    if (z && z->reserved && z->state) {
#ifdef BB_HAVE_ZSTD
        if (std::int64_t zrc; ZstdInflate(z, flush, &zrc)) return zrc;
#endif
        const std::uint64_t n = g_fast_calls.fetch_add(1, std::memory_order_relaxed) + 1;
        const std::uint64_t in0 = z->total_in, out0 = z->total_out;
#ifdef BB_HAVE_ZSTD
        const std::int64_t rc = BbInflateBench::g_on
                                    ? BbInflateBench::Call(z, [&] { return BbFastInflate::Step(z, StreamOf(z->reserved), flush); })
                                    : BbFastInflate::Step(z, StreamOf(z->reserved), flush);
#else
        const std::int64_t rc = BbFastInflate::Step(z, StreamOf(z->reserved), flush);
#endif
        g_in_bytes.fetch_add(z->total_in - in0, std::memory_order_relaxed);
        g_out_bytes.fetch_add(z->total_out - out0, std::memory_order_relaxed);
        if (n % 1000 == 0) {
            std::printf("Load speed: fast inflate: %llu streams, %llu calls, %llu MB in, %llu MB out\n",
                        static_cast<unsigned long long>(g_streams.load()), static_cast<unsigned long long>(n),
                        static_cast<unsigned long long>(g_in_bytes.load() >> 20),
                        static_cast<unsigned long long>(g_out_bytes.load() >> 20));
            std::fflush(stdout);
        }
        return rc;
    }
    return g_inflate(z, flush);
}

BB_COOP_SYSV std::int64_t ResetHook(Z* z) {
#ifdef BB_HAVE_ZSTD
    if (BbInflateBench::g_on) BbInflateBench::Drop(z);
#endif
    const std::int64_t rc = g_reset(z);
#ifdef BB_HAVE_ZSTD
    if (z && (z->reserved & 2) && rc == 0) {
        BbZstdInflate::State* st = ZStateOf(z->reserved);
        BbZstdInflate::Reset(st);
        if (st->zng) zng_inflateReset(reinterpret_cast<zng_stream*>(st->zng));
        return rc;
    }
#endif
    if (z && z->reserved && rc == 0) zng_inflateReset(StreamOf(z->reserved));
    return rc;
}

BB_COOP_SYSV std::int64_t EndHook(Z* z) {
#ifdef BB_HAVE_ZSTD
    if (BbInflateBench::g_on) BbInflateBench::Drop(z);
#endif
    if (z && z->reserved && z->state) {
#ifdef BB_HAVE_ZSTD
        BbZstdInflate::Unroute(z, g_zstd_pool);
#endif
        PoolOf(z->reserved).Release(StreamOf(z->reserved));
        z->reserved = 0;
    }
    return g_end(z);
}

bool g_inflate_installed = false;
void InstallInflate() {
    const char* e = std::getenv("BB_FAST_INFLATE");
    if (e && (!std::strcmp(e, "0") || !std::strcmp(e, "off") || !std::strcmp(e, "false"))) {
        std::printf("Load speed: the engine's inflate is the game's own (BB_FAST_INFLATE=0)\n");
        return;
    }
    // All four sites or none: a half-installed set would leak or step a stream the engine did not make.
    const bool match =
        coop::Matches(kReset, {0xb8, 0xfe, 0xff, 0xff, 0xff, 0x48, 0x85, 0xff}) &&
        coop::Matches(kInit2, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x41, 0x89, 0xf6, 0x48, 0x89, 0xfb}) &&
        coop::Matches(kInflate, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48,
                                 0x81, 0xec, 0x88, 0x00, 0x00, 0x00}) &&
        coop::Matches(kEnd, {0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0x48, 0x89, 0xfb});
    if (!match) {
        std::printf("Load speed: fast inflate not installed: this executable's zlib differs\n");
        return;
    }
    const bool ok =
        coop::ReplacePrologue(kInit2, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x41, 0x89, 0xf6, 0x48, 0x89, 0xfb},
                              reinterpret_cast<const void*>(&InitHook), reinterpret_cast<void**>(&g_init),
                              "load speed: inflateInit2_") &&
        coop::ReplacePrologue(kInflate,
                              {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48,
                               0x81, 0xec, 0x88, 0x00, 0x00, 0x00},
                              reinterpret_cast<const void*>(&InflateHook), reinterpret_cast<void**>(&g_inflate),
                              "load speed: inflate") &&
        coop::ReplacePrologue(kReset, {0xb8, 0xfe, 0xff, 0xff, 0xff, 0x48, 0x85, 0xff},
                              reinterpret_cast<const void*>(&ResetHook), reinterpret_cast<void**>(&g_reset),
                              "load speed: inflateReset") &&
        coop::ReplacePrologue(kEnd, {0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0x48, 0x89, 0xfb},
                              reinterpret_cast<const void*>(&EndHook), reinterpret_cast<void**>(&g_end),
                              "load speed: inflateEnd");
    std::printf("Load speed: zlib-ng %s behind the engine's inflate: %s\n", zlibng_version(),
                ok ? "installed" : "NOT (partly) installed");
    g_inflate_installed = ok;
}
#else
void InstallInflate() { std::printf("Load speed: this build has no zlib-ng; the engine's inflate is the game's own\n"); }
#endif

} // namespace

void Install() {
    if (!coop::Image()) return;
    InstallWorkers();
#if defined(BB_HAVE_ZLIB_NG) && defined(BB_HAVE_ZSTD)
    BbInflateBench::Init();
#endif
    InstallInflate();
#if defined(BB_HAVE_ZLIB_NG) && defined(BB_HAVE_ZSTD)
    // zstd081: converted game files, only where the inflate hooks that decode them are installed.
    if (const char* dir = std::getenv("BB_ZSTD_DIR"); dir && *dir) {
        if (g_inflate_installed) BbZstdFiles::Enable(dir);
        else std::printf("Zstd files: not used: the inflate hooks are not installed\n");
    }
#endif
}

} // namespace BbLoadSpeed

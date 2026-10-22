#include "bbport_write_log.h"
#include "bbport_boot_watch.h"
#include "bbport_game_menu.h"
#include "bbport_gnm_hooks.h"
#include "bbport_frame_rate.h"
#include "bbport_stability.h"
#include "party/party_director.h"
#include "party/lamp_travel.h"
#include "party/mirror_frame.h"
#include "party/world_warper.h"
#include "party/coop_hooks.h"
#include "party/game_state.h"
#include "net/bbnet_internal.h"
// bbport: glue between the C loader and the vendored shadPS4 video core.
#include "bbport_overlay.h"
#include "bbport_settings.h"
#include "game_profile.h"
#include "bbport_copy.h"
#ifndef _WIN32
#include <sys/resource.h>
#endif
#include "bbport_free_check.h"
#include "bbport_toggles.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <boost/asio/io_context.hpp>
#include "common/polyfill_thread.h"
#include "video_core/renderdoc.h"
#include <cstring>
#include <mutex>
#include <string>
#include <chrono>
#include <thread>
#include <vector>
#include <SDL3/SDL.h>
#include "../bbgpu.h"
#include "video_core/renderer_vulkan/vk_reflex.h"
#include "../bbnet.h"
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/rdtsc.h"
#include "video_core/renderer_vulkan/vk_breadcrumbs.h"
#include "video_core/cache_storage.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/libs.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#ifdef _WIN32
#include <windows.h>
#endif
#include "core/signals.h"
#include "sdl_window.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

extern "C" {
// runtime_memory.c
int runtime_memory_is_mapped(uintptr_t address, uint64_t size);
int runtime_memory_write_backing(uintptr_t address, const void* data, uint64_t size);
void runtime_memory_read_backing(uintptr_t address, void* data, uint64_t size);
uint64_t runtime_memory_clamp(uintptr_t address, uint64_t size);
int runtime_memory_region(uintptr_t address, uintptr_t* start, uintptr_t* end, int* mapped);
const uint64_t* runtime_memory_generation(void);
void runtime_memory_gpu_protect(uintptr_t address, uint64_t size, int read, int write);
typedef void (*RuntimeGpuRange)(uintptr_t address, uint64_t size);
// runtime_kernel.c: one clock for guest and GPU timestamps
uint64_t runtime_process_time_us(void);
uint64_t runtime_process_time_counter(void);
uint64_t runtime_tsc_frequency(void);
int32_t* runtime_errno(void);
void runtime_memory_set_gpu_hooks(RuntimeGpuRange map, RuntimeGpuRange unmap, RuntimeGpuRange invalidate);
void runtime_memory_set_note_write_hook(RuntimeGpuRange note);
void runtime_memory_set_cpu_write_hook(RuntimeGpuRange hook);
}

Frontend::WindowSDL* g_window = nullptr;

namespace Libraries::GnmDriver { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::AvPlayer { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::VideoOut { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::Kernel { void RegisterEventQueue(Core::Loader::SymbolsResolver* sym); }

namespace {
struct Symbol {
    std::string nid, library, module;
    u64 address;
};
std::vector<Symbol> g_symbols;
u32 g_sdk_version;
} // namespace

namespace Core::Loader {
void SymbolsResolver::AddSymbol(const char* nid, const char* library, const char* module, SymbolType,
                                u64 address) {
    g_symbols.push_back({nid, library, module, address});
}
} // namespace Core::Loader

// ElfInfo's fields are private to Core::Emulator; the port fills them here.
namespace Core {
class Emulator {
public:
    static void FillElfInfo(const BbGpuConfig& config) {
        auto& info = Common::ElfInfo::Instance();
        info.initialized = true;
        info.game_serial = config.serial ? config.serial : "UNKNOWN";
        info.title = config.title ? config.title : "";
        info.sdk_ver = config.sdk_version;
        info.psf_attributes.raw = config.psf_attributes;
    }
};

void MemoryManager::SetRasterizer(Vulkan::Rasterizer* rasterizer_) {
    rasterizer = rasterizer_;
    // Existing guest mappings are replayed by the runtime when hooks are installed.
    runtime_memory_set_gpu_hooks(
        [](uintptr_t address, uint64_t size) {
            auto* r = Memory::Instance()->GetRasterizer();
            r->RegisterMemory(address, size);
            r->MapMemory(address, size);
        },
        [](uintptr_t address, uint64_t size) { Memory::Instance()->GetRasterizer()->UnmapMemory(address, size); },
        [](uintptr_t address, uint64_t size) {
            Memory::Instance()->GetRasterizer()->InvalidateMemory(address, size);
        });
    // bbport: data written into GPU memory by a path the GPU side hears of (file reads, the game's
    // resource loaders): invalidated, and an asset (it may keep a VRAM copy).
    runtime_memory_set_note_write_hook([](uintptr_t address, uint64_t size) {
        Memory::Instance()->GetRasterizer()->NoteAssetWrite(address, size);
    });
    // bbport: the game's libc copies (memcpy, memset, memmove) over pages the caches watch.
    runtime_memory_set_cpu_write_hook([](uintptr_t address, uint64_t size) {
        Memory::Instance()->GetRasterizer()->OnCpuWrite(address, size);
    });
}
void MemoryManager::InvalidateMemory(VAddr address, u64 size) {
    if (rasterizer) rasterizer->InvalidateMemory(address, size);
}
namespace {
// bbport: the mapped region of the last lookup, per thread, valid for the mapping table generation
// it was read at. The GPU command thread clamps and copies thousands of constant ranges a frame,
// nearly all within the region of the one before: two calls into the runtime each (~5% of it).
struct RegionCache {
    u64 generation = 1; // odd: invalid
    uintptr_t start = 0, end = 0;
};
thread_local RegionCache region_cache;

bool CachedMapped(VAddr address, u64 size) {
    static const uint64_t* const generation_ptr = runtime_memory_generation();
    const u64 generation = __atomic_load_n(generation_ptr, __ATOMIC_ACQUIRE);
    if (generation & 1) {
        return false; // being changed
    }
    auto& cache = region_cache;
    if (cache.generation == generation && address >= cache.start && address + size <= cache.end) {
        return true;
    }
    uintptr_t start = 0, end = 0;
    int mapped = 0;
    if (!runtime_memory_region(address, &start, &end, &mapped) || !mapped) {
        return false;
    }
    cache = {generation, start, end};
    return address + size <= end;
}
} // namespace

u64 MemoryManager::ClampRangeSize(VAddr virtual_addr, u64 size) {
    if (size && CachedMapped(virtual_addr, size)) {
        return size;
    }
    return runtime_memory_clamp(virtual_addr, size);
}
static void CopySparseSerial(VAddr source, u8* dest, u64 size) {
    // BB_READBACKS=2 protects GPU-written pages against reads: the copy threads and recorders
    // read through the backing view, as a read fault on them would wait for the GPU thread,
    // which waits for them.
    static const bool precise =
        EmulatorSettings.GetReadbacksMode() == GpuReadbacksMode::Precise;
    if (precise) {
        runtime_memory_read_backing(source, dest, size);
        return;
    }
    if (size && CachedMapped(source, size)) {
        std::memcpy(dest, reinterpret_cast<const void*>(source), size);
        return;
    }
    while (size) {
        uintptr_t start = 0, end = 0;
        int mapped = 0;
        if (!runtime_memory_region(source, &start, &end, &mapped)) {
            end = source + size;
        }
        const u64 n = std::min<u64>(size, end - source);
        if (mapped) std::memcpy(dest, reinterpret_cast<const void*>(source), n);
        else std::memset(dest, 0, n);
        source += n; dest += n; size -= n;
    }
}
void MemoryManager::CopySparseMemory(VAddr source, u8* dest, u64 size) {
    // bbport: large uploads (streaming) are split across the copy threads.
    constexpr u64 Chunk = 512 * 1024;
    if (size < 4 * Chunk || !BbCopy::Enabled()) {
        return CopySparseSerial(source, dest, size);
    }
    BbCopy::ParallelFor((size + Chunk - 1) / Chunk, [&](std::size_t i) {
        const u64 offset = i * Chunk;
        CopySparseSerial(source + offset, dest + offset, std::min(Chunk, size - offset));
    });
}
void MemoryManager::ReadBacking(VAddr address, void* data, u64 size) {
    runtime_memory_read_backing(address, data, size);
}
bool MemoryManager::TryWriteBacking(void* address, const void* data, u64 size) {
    BbWriteLog::Note(reinterpret_cast<uintptr_t>(address), data, size, BbWriteLog::Backing);
    return runtime_memory_write_backing(reinterpret_cast<uintptr_t>(address), data, size) != 0;
}
void AddressSpace::Protect(VAddr virtual_addr, u64 size, MemoryPermission perms) {
    BbStats::Timer timer{BbStats::t_protect};
    const u64 pages = (size + 4095) / 4096;
    BbStats::protect_calls.fetch_add(1, std::memory_order_relaxed);
    BbStats::protect_pages.fetch_add(pages, std::memory_order_relaxed);
    if (!True(perms & MemoryPermission::Write)) {
        BbStats::protect_revoke_calls.fetch_add(1, std::memory_order_relaxed);
        BbStats::protect_revoke_pages.fetch_add(pages, std::memory_order_relaxed);
    }
    runtime_memory_gpu_protect(virtual_addr, size, True(perms & MemoryPermission::Read),
                               True(perms & MemoryPermission::Write));
}
boost::icl::interval_set<VAddr> AddressSpace::GetUsableRegions() {
    boost::icl::interval_set<VAddr> set;
    set += boost::icl::interval<VAddr>::right_open(0x10'0000'0000ULL, 0xfc'0000'0000ULL);
    return set;
}
} // namespace Core

// Kernel services the vendored libraries call directly.
namespace Libraries::Kernel {
u64 PS4_SYSV_ABI sceKernelGetTscFrequency() { return runtime_tsc_frequency(); }
u64 PS4_SYSV_ABI sceKernelReadTsc() { return Common::FencedRDTSC(); }
u64 PS4_SYSV_ABI sceKernelGetProcessTime() { return runtime_process_time_us(); }
u64 PS4_SYSV_ABI sceKernelGetProcessTimeCounter() { return runtime_process_time_counter(); }
u64 PS4_SYSV_ABI sceKernelGetProcessTimeCounterFrequency() { return 1'000'000'000; }
s32 PS4_SYSV_ABI sceKernelIsNeoMode() { return 0; } // base PS4
s32 PS4_SYSV_ABI sceKernelGetCompiledSdkVersion(s32* ver) {
    if (!ver) return ORBIS_KERNEL_ERROR_EINVAL;
    *ver = s32(g_sdk_version);
    return ORBIS_OK;
}
s32 PS4_SYSV_ABI sceKernelUsleep(u32 microseconds) {
    std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
    return ORBIS_OK;
}
} // namespace Libraries::Kernel

namespace {
// SDL video must be driven from one thread: the window lives on its own host thread.
std::thread g_window_thread;
std::mutex g_window_mutex;
std::condition_variable g_window_cv;
bool g_window_ready;
} // namespace

#ifdef BB_PGO_GENERATE
extern "C" void __gcov_dump(void);
extern "C" void __gcov_reset(void);
// Instrumented build (BB_PGO=generate): the game often ends through _exit (watchdog, guest
// exit), which skips the profile write at exit. Write every 30 s and reset: the files sum the
// intervals.
static void StartProfileWriter() {
    std::thread([] {
        Common::SetCurrentThreadName("bb:pgo");
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            __gcov_dump();
            __gcov_reset();
            std::printf("PGO: profile written\n");
        }
    }).detach();
}
#endif

extern "C" void bbgpu_save_caches(void) {
    static std::atomic<bool> saved{false};
    if (saved.exchange(true)) {
        return;
    }
    // On a helper thread with a time limit: a GPU hang at exit must not keep the process alive.
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::thread([done] {
        if (auto* cache = Vulkan::PipelineCache::driver_cache_owner.load()) {
            cache->SaveDriverPipelineCache(true);
        }
        // The shader cache's queued writes (keys and sources of this session's new pipelines).
        Storage::DataBase::Instance().Flush();
        done->store(true);
    }).detach();
    for (int i = 0; i < 500 && !done->load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

/// Ends the process without unloading DLLs (see the window close path); stdio is flushed first.
[[noreturn]] void BbHardExit(int code) {
    bbgpu_save_caches();
    bbnet_shutdown();  // party: BYE + UPnP unmap (no-op without BB_PARTY)
    std::fflush(stdout);
    std::fflush(stderr);
#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
#endif
    std::_Exit(code);
}

extern "C" int bbgpu_init(const BbGpuConfig* config) {
    BbSettings::Load();
    // What the translator knows about this game (games/), before anything asks for it.
    Game::Select(config->serial);
#ifdef BB_PGO_GENERATE
    StartProfileWriter();
#endif
    g_sdk_version = config->sdk_version;
#ifdef _WIN32
    if (config->user_dir) _putenv_s("BB_GPU_USER_DIR", config->user_dir);
#else
    if (config->user_dir) setenv("BB_GPU_USER_DIR", config->user_dir, 0);
#endif
    Core::Emulator::FillElfInfo(*config);
    const std::string title = config->title ? config->title : "Bloodborne";
    const s32 width = config->width, height = config->height;
    g_window_thread = std::thread([title, width, height] {
        Common::SetCurrentThreadName("bb:window");
        auto* window = new Frontend::WindowSDL(width, height, title.c_str());
        {
            std::scoped_lock lock{g_window_mutex};
            g_window = window;
            g_window_ready = true;
        }
        g_window_cv.notify_all();
        while (window->PollEvents()) {
            SDL_Delay(2);
        }
        LOG_INFO(Frontend, "Window closed by user");
        bbgpu_save_caches();
        bbnet_shutdown();  // party: BYE + UPnP unmap
        std::fflush(stdout);
        std::fflush(stderr);
#ifdef _WIN32
        // Not _Exit/ExitProcess: unloading the Vulkan driver while GPU threads are inside it can
        // hang the process for good.
        TerminateProcess(GetCurrentProcess(), 0);
#endif
        std::_Exit(0);
    });
    g_window_thread.detach();
    {
        std::unique_lock lock{g_window_mutex};
        g_window_cv.wait(lock, [] { return g_window_ready; });
    }
    Core::Loader::SymbolsResolver resolver;
    // GnmDriver creates the presenter that the VideoOut present thread uses.
    Libraries::GnmDriver::RegisterLib(&resolver);
    Libraries::VideoOut::RegisterLib(&resolver);
    return 0;
}

namespace Libraries::Kernel { void StartKernelService(); }
extern "C" void bbgpu_register_kernel(void) {
    Libraries::Kernel::StartKernelService();
    Core::Loader::SymbolsResolver resolver;
    Libraries::Kernel::RegisterEventQueue(&resolver);
    Libraries::AvPlayer::RegisterLib(&resolver);
}

extern "C" uintptr_t bbgpu_resolve(const char* scoped_nid) {
    const char* hash = std::strchr(scoped_nid, '#');
    const size_t length = hash ? size_t(hash - scoped_nid) : std::strlen(scoped_nid);
    for (const auto& symbol : g_symbols) {
        if (symbol.nid.size() == length && !std::memcmp(symbol.nid.data(), scoped_nid, length)) {
            return uintptr_t(symbol.address);
        }
    }
    return 0;
}

extern "C" int bbgpu_handle_fault(void* ucontext, void* address) {
    // BB_LABEL_TRAP (diagnostic): its read-only label pages first. Not passed on to GPU page
    // tracking (a write fault drains the draw pipe: thousands a second would change the timing
    // under test); label pages are not expected to be GPU-tracked.
    if (BbFreeCheck::OnTrapFault(ucontext, reinterpret_cast<std::uint64_t>(address))) {
        return 1;
    }
    if (Core::Signals::Instance()->DispatchAccessViolation(ucontext, address)) {
        return 1;
    }
    return BbFreeCheck::OnStaleTrapFault(reinterpret_cast<std::uint64_t>(address)) ? 1 : 0;
}

extern "C" void bbgpu_patch_image(unsigned char* image, uint64_t size) {
    BbGnmHooks::PatchImage(image, size);
    BbGameMenu::PatchImage(image, size);
    BbStability::PatchImage(image, size);
    BbFrameRate::PatchImage(image, size);  // frame rate by hooks (deadinside 0.6-pre1; BB_FRAME_HOOKS=0: FPS++ patches)
    bbnet_set_image(image, size);  // the party library's view of game data (gpu/shim/net)
    // Its guest_read through the fault-recovering SafeRead, not a VirtualQuery check per call:
    // VirtualQuery took ~4-12 ms per call in a running game, and the party's per-frame save slot
    // reads took 112 FPS to 40 (GitHub issue #3).
    bbnet::set_guest_reader(&coop::SafeRead);
    coop::HooksInit(image, size);  // the image for game-state reads (the performance HUD's menu / banner check, solo too)
    coop::lamptravel::Install(image, size);  // direct lamp travel (solo and party; BB_LAMP_TRAVEL=0 off)
    coop::mirror::Install();  // the Dream mirror's editor in character creation's frame (H5)
    coop::warper::Install(image, size);  // the world warper (BB_WORLD_WARPER, solo shader-cache builder)
    if (coop::PartyRequested()) {
        coop::PartyInit(image, size);  // party co-op: main-thread tick, Lua events, director
    }
    coop::MarkImageHooksDone();  // the party version check hashes the installed hooks after this
}

extern "C" unsigned bbgpu_symbol_count(void) {
    return unsigned(g_symbols.size());
}

extern "C" uint64_t bbgpu_get_flip_count(void) {
    return BbStats::flips.load(std::memory_order_relaxed);
}

extern "C" uint64_t bbgpu_boot_ticks(void) {
    return BbBootWatchState::ticks.load(std::memory_order_relaxed);
}

extern "C" int bbgpu_boot_done(void) {
    return BbBootWatchState::done.load(std::memory_order_relaxed) ? 1 : 0;
}

extern "C" int bbgpu_boot_take_request(char* why, unsigned size) {
    if (!BbBootWatchState::requested.exchange(false, std::memory_order_acquire)) {
        return 0;
    }
    std::lock_guard<std::mutex> lk(BbBootWatchState::why_mu);
    if (why && size) {
        std::snprintf(why, size, "%s", BbBootWatchState::why.c_str());
    }
    return 1;
}

extern "C" uint64_t bbgpu_get_submit_count(void) {
    return BbStats::submissions.load(std::memory_order_relaxed);
}

namespace Libraries::VideoOut {
void GetFrametimeStats(double* avg_fps, double* p95_ms, double* p99_ms);
}

extern "C" void bbgpu_get_frametime_percentiles(double* avg_fps, double* p95_ms, double* p99_ms) {
    Libraries::VideoOut::GetFrametimeStats(avg_fps, p95_ms, p99_ms);
}

extern "C" void bbgpu_dump_host_threads_hang(void* file_handle) {
    FILE* f = static_cast<FILE*>(file_handle);
    if (!f) return;
    std::fprintf(f, "=== HOST GPU / PRESENTER THREADS ===\n");
    std::fprintf(f, "GPU Stats:\n");
    std::fprintf(f, "  Submissions: %llu, Flips: %llu, Draws: %llu, Dispatches: %llu\n",
                 (unsigned long long)BbStats::submissions.load(std::memory_order_relaxed),
                 (unsigned long long)BbStats::flips.load(std::memory_order_relaxed),
                 (unsigned long long)BbStats::draws.load(std::memory_order_relaxed),
                 (unsigned long long)BbStats::dispatches.load(std::memory_order_relaxed));
    std::fprintf(f, "  GPU Idle Time: %.2f ms\n",
                 (double)BbStats::gpu_idle_ns.load(std::memory_order_relaxed) / 1000000.0);
    std::fprintf(f, "  Scheduler Sync Wait: %.2f ms\n",
                 (double)BbStats::sync_recording_ns.load(std::memory_order_relaxed) / 1000000.0);
    std::fprintf(f, "  Host Copies Wait: %.2f ms\n",
                 (double)BbStats::host_copies_wait_ns.load(std::memory_order_relaxed) / 1000000.0);
    std::fprintf(f, "  GPU Tick Wait: %.2f ms\n",
                 (double)BbStats::tick_wait_ns.load(std::memory_order_relaxed) / 1000000.0);
    std::fprintf(f, "  Host Wait (fences/queue): %.2f ms\n",
                 (double)BbStats::t_host_wait.load(std::memory_order_relaxed) / 1000000.0);
    std::fprintf(f, "  Texture Creation Time: %.2f ms, Staging Time: %.2f ms\n",
                 (double)BbStats::t_image_create.load(std::memory_order_relaxed) / 1000000.0,
                 (double)BbStats::t_staging.load(std::memory_order_relaxed) / 1000000.0);
    std::fprintf(f, "  Page Protection Revoke Calls: %llu (%llu pages)\n",
                 (unsigned long long)BbStats::protect_revoke_calls.load(std::memory_order_relaxed),
                 (unsigned long long)BbStats::protect_revoke_pages.load(std::memory_order_relaxed));
    std::fprintf(f, "====================================\n\n");
    std::fflush(f);
}

extern "C" void bbgpu_dump_breadcrumbs(void* file_handle) {
    if (!file_handle) return;
    FILE* f = static_cast<FILE*>(file_handle);
    std::fprintf(f, "\n=== GPU BREADCRUMBS STATE ===\n");
    Vulkan::Breadcrumbs::ReportStuck("diagnostic dump");
    std::fprintf(f, "=============================\n\n");
    std::fflush(f);
}

// Kernel service thread: runs boost::asio timers for equeue timer events.
namespace Libraries::Kernel {
boost::asio::io_context io_context;
static std::mutex m_asio_req;
static std::condition_variable_any cv_asio_req;
static std::atomic<u32> asio_requests;
static std::jthread service_thread;

void KernelSignalRequest() {
    std::unique_lock lock{m_asio_req};
    ++asio_requests;
    cv_asio_req.notify_one();
}

static void KernelServiceThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("bb:kernel_service");
    while (!stoken.stop_requested()) {
        {
            std::unique_lock lock{m_asio_req};
            cv_asio_req.wait(lock, stoken, [] { return asio_requests != 0; });
        }
        if (stoken.stop_requested()) break;
        io_context.run();
        io_context.restart();
        asio_requests = 0;
    }
}

void StartKernelService() {
    service_thread = std::jthread{KernelServiceThread};
}

int* PS4_SYSV_ABI __Error() {
    return runtime_errno();
}
} // namespace Libraries::Kernel

// RenderDoc capture hooks: not used by the port.
namespace VideoCore {
void LoadRenderDoc() {}
void StartCapture() {}
void EndCapture() {}
void TriggerCapture() {}
void SetOutputDir(const std::filesystem::path&, const std::string&) {}
bool IsRenderDocLoaded() { return false; }
void RequestScreenshot(ScreenshotRequest) {}
u32 ConsumeGameOnlyScreenshotRequests() { return 0; }
u32 ConsumeWithOverlaysScreenshotRequests() { return 0; }
ScreenshotRequests ConsumeScreenshotRequests() { return {}; }
} // namespace VideoCore

extern "C" void bbgpu_reflex_pre_input_sample(void) {
    Vulkan::Reflex::PreInputSample();
}

extern "C" void bbgpu_reflex_on_input_sampled(void) {
    Vulkan::Reflex::OnInputSampled();
}

extern "C" int bbgpu_overlay_captures_input(void) {
    return BbOverlay::CapturesInput() ? 1 : 0;
}

extern "C" const char* bbgpu_ime_preset(uint32_t max_length) {
    return BbGameMenu::ImePreset(max_length);
}

extern "C" int bbgpu_text_input_begin(const char* initial, const char* prompt) {
    if (!g_window) return 0;
    g_window->BeginTextInput(initial ? initial : "", prompt ? prompt : "Text");
    return 1;
}

extern "C" int bbgpu_text_input_poll(char* out, uint64_t size) {
    if (!g_window) return 2;
    std::string text;
    const int state = g_window->PollTextInput(text);
    if (size) {
        const size_t n = std::min<size_t>(text.size(), size - 1);
        std::memcpy(out, text.data(), n);
        out[n] = 0;
    }
    return state;
}

extern "C" int bbgpu_audio_audible(void) {
    return g_window ? g_window->IsAudible() : 1;
}

extern "C" int bbgpu_text_input_test_type(const char* keys) {
    if (!g_window || !keys) return 0;
    g_window->TestType(keys);
    return 1;
}

extern "C" int bbgpu_text_input_is_active(void) {
    return (g_window && g_window->IsTextInputActive()) ? 1 : 0;
}

extern "C" int bbgpu_test_key(const char* name) {
    if (!g_window || !name) return 0;
    const SDL_Keycode key = SDL_GetKeyFromName(name);
    if (key == SDLK_UNKNOWN) return 0;
    g_window->TestKey(u32(key));
    return 1;
}

extern "C" void bbgpu_request_quit(void) {
    // Like the window's close button: the window thread ends the process (party BYE first).
    SDL_Event quit;
    SDL_zero(quit);
    quit.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&quit);
}

// Mouse & keyboard controls (after Mrsuss60/bloodborne_pc_windows_port's
// bbgpu_consume_mouse_delta / bbgpu_get_mk_config, as one call).
extern "C" float bbgpu_audio_boost() {
    static const float env = [] {
        const char* e = std::getenv("BB_AUDIO_BOOST");
        return e && *e ? std::clamp(float(std::atof(e)), 0.1f, 4.0f) : -1.0f;
    }();
    return env > 0.0f ? env : BbSettings::Get().audio_boost.load();
}

extern "C" int bbgpu_prompt_style() {
    return BbSettings::Get().prompt_style.load();
}

extern "C" int bbgpu_mouse_keyboard(BbMouseState* state, int consume_motion) {
    const auto& s = BbSettings::Get();
    const bool on = s.mk_enabled.load();
    if (state) {
        *state = BbMouseState{};
        if (on && g_window) {
            g_window->ReadMouse(state->dx, state->dy, state->buttons, consume_motion != 0);
            if (consume_motion) {
                g_window->TakeWheel(state->wheel_up, state->wheel_down);
            }
        }
        state->wheel = s.mk_wheel.load() ? 1 : 0;
        state->sens_x = s.mk_sens_x.load();
        state->sens_y = s.mk_sens_y.load();
        state->deadzone = s.mk_deadzone.load();
        state->smoothing = s.mk_smoothing.load();
        state->invert_x = s.mk_invert_x.load() ? 1 : 0;
        state->invert_y = s.mk_invert_y.load() ? 1 : 0;
        state->bindings_generation = BbSettings::BindingsGeneration();
    }
    return on ? 1 : 0;
}

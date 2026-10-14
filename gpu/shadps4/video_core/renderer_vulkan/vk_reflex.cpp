// SPDX-License-Identifier: GPL-2.0-or-later
// NVIDIA Reflex (VK_NV_low_latency2 rev3) integration for bbport.
// From Supermedo/bloodborne_pc PR #69 by FIocker; off by default in bbport: nothing here runs
// (no extension, no locks or markers on the pad / flip / submit paths) unless the player chose
// Reflex for this start and the driver supports it (g_tracking).

#include "video_core/renderer_vulkan/vk_reflex.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "common/logging/log.h"
#include "bbport_settings.h"

namespace Vulkan::Reflex {

namespace {

typedef VkResult (VKAPI_PTR *PFN_vkSetLatencySleepModeNV)(VkDevice device, VkSwapchainKHR swapchain, const VkLatencySleepModeInfoNV* pSleepModeInfo);
typedef VkResult (VKAPI_PTR *PFN_vkLatencySleepNV)(VkDevice device, VkSwapchainKHR swapchain, const VkLatencySleepInfoNV* pSleepInfo);
typedef void (VKAPI_PTR *PFN_vkSetLatencyMarkerNV)(VkDevice device, VkSwapchainKHR swapchain, const VkSetLatencyMarkerInfoNV* pLatencyMarkerInfo);
typedef void (VKAPI_PTR *PFN_vkGetLatencyTimingsNV)(VkDevice device, VkSwapchainKHR swapchain, VkGetLatencyMarkerInfoNV* pLatencyMarkerInfo);
typedef void (VKAPI_PTR *PFN_vkQueueNotifyOutOfBandNV)(VkQueue queue, const VkOutOfBandQueueTypeInfoNV* pQueueTypeInfo);

PFN_vkSetLatencySleepModeNV g_pfn_set_sleep_mode = nullptr;
PFN_vkLatencySleepNV g_pfn_latency_sleep = nullptr;
PFN_vkSetLatencyMarkerNV g_pfn_set_marker = nullptr;
PFN_vkGetLatencyTimingsNV g_pfn_get_timings = nullptr;
PFN_vkQueueNotifyOutOfBandNV g_pfn_queue_notify_oob = nullptr;

VkInstance g_instance = VK_NULL_HANDLE;
VkPhysicalDevice g_phys = VK_NULL_HANDLE;
VkDevice g_device = VK_NULL_HANDLE;
VkSwapchainKHR g_swapchain = VK_NULL_HANDLE;
VkQueue g_present_queue = VK_NULL_HANDLE;

VkSemaphore g_sleep_semaphore = VK_NULL_HANDLE;
uint64_t g_sleep_value = 0;
std::unordered_map<VkQueue, uint64_t> g_queue_ids;
uint64_t g_render_started = 0;
uint64_t g_report_logged = 0;
uint64_t g_input_sampled = 0;
struct CpuSample { uint64_t id{}; int64_t ns{}; };
std::array<CpuSample,512> g_cpu_samples{};

std::mutex g_mu;
std::mutex g_sleep_mu; // serializes sleeps and swapchain/semaphore destruction
std::mutex g_wsi_mu;   // WSI serialization: acquire/recreate/present
bool g_supported = false;
Mode g_mode = Mode::Off;
// Reflex runs this session (supported and not Off): the hooks' fast check.
std::atomic<bool> g_tracking{false};
bool TimingLog() {
    static const bool on = std::getenv("BB_REFLEX_TIMING") != nullptr;
    return on;
}
uint32_t g_min_interval_us = 0;

std::atomic<uint64_t> g_sim_frame_id{0};
std::atomic<bool> g_sim_cycle_active{false};
std::mutex g_cycle_mu; // gates simulation cycle transitions and pacing sleeps
thread_local uint64_t t_recording_frame_id = 0;

Mode parse_settings_or_env_mode() {
    // bbport.ini reflex / BB_REFLEX (BbSettings::Load); this start's choice.
    const int s_val = BbSettings::Get().startup_reflex;
    if (s_val == BbSettings::ReflexBoost) return Mode::Boost;
    if (s_val == BbSettings::ReflexOn) return Mode::On;
    return Mode::Off;
}

void disable_locked(const char* operation, VkResult result) {
    LOG_ERROR(Render_Vulkan, "Reflex: {} failed ({}); driver pacing and markers disabled", operation, static_cast<int>(result));
    g_supported = false;
}

// Internal helper called under g_sleep_mu + WsiMutex + g_mu
void apply_mode_under_wsi_locked() {
    if (!g_supported || !g_device || !g_swapchain || !g_pfn_set_sleep_mode) return;
    VkLatencySleepModeInfoNV info{};
    info.sType = VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV;
    info.pNext = nullptr;
    info.lowLatencyMode = (g_mode != Mode::Off) ? VK_TRUE : VK_FALSE;
    info.lowLatencyBoost = (g_mode == Mode::Boost) ? VK_TRUE : VK_FALSE;
    info.minimumIntervalUs = g_min_interval_us;
    const VkResult r = g_pfn_set_sleep_mode(g_device, g_swapchain, &info);
    if (r != VK_SUCCESS) disable_locked("vkSetLatencySleepModeNV", r);
    LOG_INFO(Render_Vulkan, "Reflex: set latency sleep mode: mode={} boost={} min_interval={} us (result={})",
             static_cast<int>(info.lowLatencyMode), static_cast<int>(info.lowLatencyBoost),
             info.minimumIntervalUs, static_cast<int>(r));
}

// The specification permits markers concurrently with acquire/present. g_mu serializes
// our marker state with OnSwapchainDestroying, which clears g_swapchain before destruction.
// Do not make simulation/render markers wait behind WSI acquisition.
// https://docs.vulkan.org/refpages/latest/refpages/source/vkSetLatencyMarkerNV.html
void emit_marker_locked(uint64_t frame_id, VkLatencyMarkerNV marker) {
    if (!g_supported || !g_device || !g_swapchain || !g_pfn_set_marker) return;
    VkSetLatencyMarkerInfoNV info{};
    info.sType = VK_STRUCTURE_TYPE_SET_LATENCY_MARKER_INFO_NV;
    info.pNext = nullptr;
    info.presentID = frame_id;
    info.marker = marker;
    g_pfn_set_marker(g_device, g_swapchain, &info);
}

} // namespace

std::mutex& SleepMutex() {
    return g_sleep_mu;
}

std::mutex& WsiMutex() {
    return g_wsi_mu;
}

bool ConfigureDevice(vk::PhysicalDevice phys, std::vector<const char*>& exts,
                     vk::PhysicalDeviceVulkan12Features& features12,
                     vk::PhysicalDevicePresentIdFeaturesKHR& present_id) {
    auto [ext_res, available] = phys.enumerateDeviceExtensionProperties();
    if (ext_res != vk::Result::eSuccess) return false;

    bool latency = false, present = false;
    for (const auto& prop : available) {
        const char* name = prop.extensionName.data();
        latency |= (std::strcmp(name, VK_NV_LOW_LATENCY_2_EXTENSION_NAME) == 0 && prop.specVersion >= 3);
        present |= (std::strcmp(name, VK_KHR_PRESENT_ID_EXTENSION_NAME) == 0);
    }
    if (!latency || !present) return false;

    auto chain = phys.getFeatures2<vk::PhysicalDeviceFeatures2,
                                   vk::PhysicalDeviceVulkan12Features,
                                   vk::PhysicalDevicePresentIdFeaturesKHR>();
    const auto& q12 = chain.get<vk::PhysicalDeviceVulkan12Features>();
    const auto& qp = chain.get<vk::PhysicalDevicePresentIdFeaturesKHR>();

    if (!q12.timelineSemaphore || !qp.presentId) return false;

    features12.timelineSemaphore = true;
    present_id.presentId = true;

    for (auto name : {VK_NV_LOW_LATENCY_2_EXTENSION_NAME, VK_KHR_PRESENT_ID_EXTENSION_NAME}) {
        if (std::none_of(exts.begin(), exts.end(), [&](const char* x) { return std::strcmp(x, name) == 0; })) {
            exts.push_back(name);
        }
    }

    LOG_INFO(Render_Vulkan, "Reflex: low_latency2 revision 3, present IDs and timeline semaphores enabled");
    return true;
}

void Initialize(vk::Instance instance, vk::PhysicalDevice phys, vk::Device device, bool extensions_enabled) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_instance = static_cast<VkInstance>(instance);
    g_phys = static_cast<VkPhysicalDevice>(phys);
    g_device = static_cast<VkDevice>(device);

    g_mode = parse_settings_or_env_mode();
    if (const char* interval = std::getenv("BB_REFLEX_INTERVAL_US")) {
        g_min_interval_us = static_cast<uint32_t>(std::clamp(std::atoi(interval), 0, 100000));
    }

    if (!extensions_enabled) {
        g_supported = false;
        LOG_INFO(Render_Vulkan, "Reflex: extensions not enabled on device; low latency disabled");
        return;
    }

    auto get_proc = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;
    if (!get_proc) {
        g_supported = false;
        return;
    }

    g_pfn_set_sleep_mode = reinterpret_cast<PFN_vkSetLatencySleepModeNV>(get_proc(g_device, "vkSetLatencySleepModeNV"));
    g_pfn_latency_sleep = reinterpret_cast<PFN_vkLatencySleepNV>(get_proc(g_device, "vkLatencySleepNV"));
    g_pfn_set_marker = reinterpret_cast<PFN_vkSetLatencyMarkerNV>(get_proc(g_device, "vkSetLatencyMarkerNV"));
    g_pfn_get_timings = reinterpret_cast<PFN_vkGetLatencyTimingsNV>(get_proc(g_device, "vkGetLatencyTimingsNV"));
    g_pfn_queue_notify_oob = reinterpret_cast<PFN_vkQueueNotifyOutOfBandNV>(get_proc(g_device, "vkQueueNotifyOutOfBandNV"));

    g_supported = g_pfn_set_sleep_mode && g_pfn_latency_sleep && g_pfn_set_marker && g_pfn_get_timings && g_pfn_queue_notify_oob;
    if (g_supported) {
        VkSemaphoreTypeCreateInfo type{};
        type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        ci.pNext = &type;
        auto create_sem = VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateSemaphore;
        VkResult r = create_sem ? create_sem(g_device, &ci, nullptr, &g_sleep_semaphore) : VK_ERROR_INITIALIZATION_FAILED;
        if (r != VK_SUCCESS) {
            disable_locked("timeline semaphore creation", r);
        }
    }
    g_tracking.store(g_supported && g_mode != Mode::Off, std::memory_order_release);
    LOG_INFO(Render_Vulkan, "Reflex: device initialized: supported={} (mode={})",
             g_supported, g_mode == Mode::Off ? "off" : (g_mode == Mode::Boost ? "on+boost" : "on"));
}

void Shutdown() {
    std::lock_guard<std::mutex> sleep(g_sleep_mu);
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_sleep_semaphore && g_device) {
        VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroySemaphore(g_device, g_sleep_semaphore, nullptr);
        g_sleep_semaphore = VK_NULL_HANDLE;
    }
    g_swapchain = VK_NULL_HANDLE;
    g_device = VK_NULL_HANDLE;
    g_phys = VK_NULL_HANDLE;
    g_instance = VK_NULL_HANDLE;
    g_supported = false;
    g_tracking.store(false, std::memory_order_release);
}

bool Tracking() {
    return g_tracking.load(std::memory_order_relaxed);
}

void SetTrackingForTest(bool on) {
    g_tracking.store(on, std::memory_order_release);
}

bool IsSupported() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_supported;
}

bool IsActive() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_supported && g_mode != Mode::Off;
}

Mode CurrentMode() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_mode;
}

void SetMode(Mode mode, uint32_t minimum_interval_us) {
    // Systematic lock hierarchy: SleepMutex -> WsiMutex -> g_mu
    std::lock_guard<std::mutex> sleep_lk(g_sleep_mu);
    std::lock_guard<std::mutex> wsi_lk(g_wsi_mu);
    std::lock_guard<std::mutex> lk(g_mu);
    g_mode = mode;
    if (minimum_interval_us) g_min_interval_us = minimum_interval_us;
    apply_mode_under_wsi_locked();
}

void PrepareSwapchainCreateInfo(vk::SwapchainCreateInfoKHR& create_info,
                                VkSwapchainLatencyCreateInfoNV& latency_info) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_supported) return;
    latency_info = {};
    latency_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_LATENCY_CREATE_INFO_NV;
    latency_info.pNext = create_info.pNext;
    latency_info.latencyModeEnable = (g_mode != Mode::Off) ? VK_TRUE : VK_FALSE;
    create_info.pNext = &latency_info;
}

void OnSwapchainCreated(vk::SwapchainKHR swapchain) {
    // Caller (Swapchain::Create) holds SleepMutex and WsiMutex
    std::lock_guard<std::mutex> lk(g_mu);
    g_swapchain = static_cast<VkSwapchainKHR>(swapchain);
    apply_mode_under_wsi_locked();
}

void OnSwapchainDestroying(vk::SwapchainKHR old_swapchain) {
    // Caller (Swapchain::Destroy) holds SleepMutex and WsiMutex
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_swapchain == static_cast<VkSwapchainKHR>(old_swapchain)) {
        g_swapchain = VK_NULL_HANDLE;
    }
}

void OnPresentQueueCreated(vk::Queue present_queue) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_present_queue = static_cast<VkQueue>(present_queue);
    if (g_supported && g_present_queue && g_pfn_queue_notify_oob) {
        VkOutOfBandQueueTypeInfoNV info{};
        info.sType = VK_STRUCTURE_TYPE_OUT_OF_BAND_QUEUE_TYPE_INFO_NV;
        info.pNext = nullptr;
        info.queueType = VK_OUT_OF_BAND_QUEUE_TYPE_PRESENT_NV;
        g_pfn_queue_notify_oob(g_present_queue, &info);
        LOG_INFO(Render_Vulkan, "Reflex: present queue marked OUT_OF_BAND (present)");
    }
}

uint64_t PrepareSubmit(vk::Queue queue, uint64_t frame_id, vk::SubmitInfo& submit,
                       VkLatencySubmissionPresentIdNV& attribution) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_supported) return 0;
    auto raw_queue = static_cast<VkQueue>(queue);
    auto& last = g_queue_ids[raw_queue];
    if (frame_id && frame_id < last) {
        static unsigned logs = 0;
        if (logs++ < 8) {
            LOG_WARNING(Render_Vulkan, "Reflex: stale submission ID {} after {}; clearing queue attribution",
                        frame_id, last);
        }
        frame_id = 0;
    }
    if (frame_id) last = frame_id;
    attribution = {};
    attribution.sType = VK_STRUCTURE_TYPE_LATENCY_SUBMISSION_PRESENT_ID_NV;
    attribution.presentID = frame_id;
    attribution.pNext = submit.pNext;
    submit.pNext = &attribution;
    return frame_id;
}

uint64_t CurrentSimFrameId() {
    return g_sim_frame_id.load(std::memory_order_relaxed);
}

void SetRecordingFrameId(uint64_t frame_id) {
    t_recording_frame_id = frame_id;
}

uint64_t RecordingFrameId() {
    return t_recording_frame_id;
}

uint64_t OnFrameBegin() {
    if (!Tracking()) return 0;
    std::lock_guard<std::mutex> cycle_lk(g_cycle_mu);
    if (!g_sim_cycle_active.load(std::memory_order_relaxed)) {
        g_sim_cycle_active.store(true, std::memory_order_release);
        const uint64_t fid = g_sim_frame_id.fetch_add(1, std::memory_order_relaxed) + 1;
        std::lock_guard<std::mutex> lk(g_mu);
        emit_marker_locked(fid, VK_LATENCY_MARKER_SIMULATION_START_NV);
        return fid;
    }
    return g_sim_frame_id.load(std::memory_order_relaxed);
}

void PreInputSample() {
    if (!Tracking()) return;
    // Only open and pace a simulation cycle if one is not already open.
    // Multiple pad reads across different threads/calls within the same cycle reuse the open ID.
    if (g_sim_cycle_active.load(std::memory_order_acquire)) {
        return;
    }

    std::unique_lock<std::mutex> cycle_lk(g_cycle_mu);
    if (g_sim_cycle_active.load(std::memory_order_relaxed)) {
        return;
    }

    // Driver pacing sleep before starting simulation / sampling input.
    // Ensure g_sleep_mu protects Vulkan device/swapchain destruction.
    std::unique_lock<std::mutex> sleep_lk(g_sleep_mu);
    std::unique_lock<std::mutex> lk(g_mu);

    if (g_supported && g_mode != Mode::Off && g_device && g_swapchain && g_pfn_latency_sleep && g_sleep_semaphore) {
        VkLatencySleepInfoNV sleep_info{};
        sleep_info.sType = VK_STRUCTURE_TYPE_LATENCY_SLEEP_INFO_NV;
        sleep_info.pNext = nullptr;
        sleep_info.signalSemaphore = g_sleep_semaphore;
        sleep_info.value = ++g_sleep_value;
        const auto device = g_device;
        const auto swapchain = g_swapchain;
        const auto started = std::chrono::steady_clock::now();

        lk.unlock();

        VkResult r;
        {
            // Driver latency sleep requires WSI serialization
            std::lock_guard<std::mutex> wsi(g_wsi_mu);
            r = g_pfn_latency_sleep(device, swapchain, &sleep_info);
        }
        if (r == VK_SUCCESS) {
            // CPU timeline wait: never hold wsi lock or g_mu or input pad lock here
            VkSemaphoreWaitInfo wait{};
            wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
            wait.semaphoreCount = 1;
            wait.pSemaphores = &g_sleep_semaphore;
            wait.pValues = &sleep_info.value;
            auto wait_fn = VULKAN_HPP_DEFAULT_DISPATCHER.vkWaitSemaphores;
            if (wait_fn) {
                r = wait_fn(device, &wait, 100000000ull); // 100 ms timeout
            } else {
                r = VK_ERROR_INITIALIZATION_FAILED;
            }
        }

        lk.lock();
        const auto sleep_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started).count();
        static auto last_slow_sleep = std::chrono::steady_clock::time_point{};
        const auto finished = std::chrono::steady_clock::now();
        if (sleep_us > 50000 && finished - last_slow_sleep >= std::chrono::seconds(1)) {
            last_slow_sleep = finished;
            LOG_INFO(Render_Vulkan, "Reflex: latency sleep took {} us", static_cast<long long>(sleep_us));
        }
        if (r != VK_SUCCESS) {
            disable_locked("latency sleep/wait", r);
        }
    }

    // Now open the new simulation cycle with a newly incremented frame ID
    const uint64_t fid = g_sim_frame_id.fetch_add(1, std::memory_order_relaxed) + 1;
    g_sim_cycle_active.store(true, std::memory_order_release);
    emit_marker_locked(fid, VK_LATENCY_MARKER_SIMULATION_START_NV);
}

void OnInputSampled() {
    if (!Tracking()) return;
    const uint64_t fid = CurrentSimFrameId();
    if (!fid) return;
    // Timestamp the sample before any marker-state lock contention.
    const bool timing = TimingLog();
    const auto sampled_ns = timing
        ? std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now().time_since_epoch()).count()
        : 0;
    std::lock_guard<std::mutex> lk(g_mu);
    if (fid <= g_input_sampled) return;
    g_input_sampled = fid;
    if (timing) {
        g_cpu_samples[fid % g_cpu_samples.size()] = {fid, sampled_ns};
    }
    emit_marker_locked(fid, VK_LATENCY_MARKER_INPUT_SAMPLE_NV);
}

void TraceCpuPresent(uint64_t fid) {
    if (!fid || !TimingLog()) return;
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    CpuSample sample;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        sample = g_cpu_samples[fid % g_cpu_samples.size()];
    }
    if (sample.id == fid && now >= sample.ns) {
        std::printf("reflex cpu: id=%llu sampled_ns=%lld present_ns=%lld latency_ms=%.3f\n",
            (unsigned long long)fid,(long long)sample.ns,(long long)now,(now-sample.ns)/1e6);
    }
}

void OnSimulationEnd() {
    if (!Tracking()) return;
    const uint64_t fid = CurrentSimFrameId();
    if (!fid) return;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        emit_marker_locked(fid, VK_LATENCY_MARKER_SIMULATION_END_NV);
    }
    // Close the simulation cycle on CPU flip consumption so the next PreInputSample can advance to the next frame.
    std::lock_guard<std::mutex> cycle_lk(g_cycle_mu);
    g_sim_cycle_active.store(false, std::memory_order_release);
}

void OnRenderSubmitBegin(uint64_t frame_id) {
    if (!frame_id || !Tracking()) return;
    std::lock_guard<std::mutex> lk(g_mu);
    if (frame_id > g_render_started) {
        g_render_started = frame_id;
        emit_marker_locked(frame_id, VK_LATENCY_MARKER_RENDERSUBMIT_START_NV);
    }
}

void OnRenderSubmitEnd(uint64_t frame_id) {
    if (!frame_id || !Tracking()) return;
    std::lock_guard<std::mutex> lk(g_mu);
    emit_marker_locked(frame_id, VK_LATENCY_MARKER_RENDERSUBMIT_END_NV);
}

void OnForeignSubmitBegin(uint64_t frame_id) {
    if (!frame_id || !Tracking()) return;
    std::lock_guard<std::mutex> lk(g_mu);
    emit_marker_locked(frame_id, VK_LATENCY_MARKER_OUT_OF_BAND_RENDERSUBMIT_START_NV);
}

void OnForeignSubmitEnd(uint64_t frame_id) {
    if (!frame_id || !Tracking()) return;
    std::lock_guard<std::mutex> lk(g_mu);
    emit_marker_locked(frame_id, VK_LATENCY_MARKER_OUT_OF_BAND_RENDERSUBMIT_END_NV);
}

void OnPresentBegin(uint64_t frame_id, bool is_real) {
    if (!frame_id || !Tracking()) return;
    std::lock_guard<std::mutex> lk(g_mu);
    emit_marker_locked(frame_id, is_real ? VK_LATENCY_MARKER_PRESENT_START_NV
                                         : VK_LATENCY_MARKER_OUT_OF_BAND_PRESENT_START_NV);
}

void OnPresentEnd(uint64_t frame_id, bool is_real) {
    if (!frame_id || !Tracking()) return;
    std::lock_guard<std::mutex> lk(g_mu);
    emit_marker_locked(frame_id, is_real ? VK_LATENCY_MARKER_PRESENT_END_NV
                                         : VK_LATENCY_MARKER_OUT_OF_BAND_PRESENT_END_NV);
}

bool QueryLatencyTimings(LatencyReport* out_report) {
    if (!out_report) return false;
    // Capture state under SleepMutex -> WsiMutex -> g_mu hierarchy
    std::lock_guard<std::mutex> sleep_lk(g_sleep_mu);
    std::lock_guard<std::mutex> wsi_lk(g_wsi_mu);
    VkDevice dev = VK_NULL_HANDLE;
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    PFN_vkGetLatencyTimingsNV pfn_timings = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_supported || !g_device || !g_swapchain || !g_pfn_get_timings) return false;
        dev = g_device;
        sc = g_swapchain;
        pfn_timings = g_pfn_get_timings;
    }

    VkGetLatencyMarkerInfoNV info{};
    info.sType = VK_STRUCTURE_TYPE_GET_LATENCY_MARKER_INFO_NV;
    pfn_timings(dev, sc, &info);
    if (!info.timingCount || info.timingCount > 4096) return false;

    std::vector<VkLatencyTimingsFrameReportNV> reports(info.timingCount);
    for (auto& report : reports) report.sType = VK_STRUCTURE_TYPE_LATENCY_TIMINGS_FRAME_REPORT_NV;
    info.pTimings = reports.data();
    pfn_timings(dev, sc, &info);

    const VkLatencyTimingsFrameReportNV* latest = nullptr;
    for (uint32_t i = 0; i < std::min(info.timingCount, uint32_t(reports.size())); ++i) {
        const auto& r = reports[i];
        if (r.presentID && r.simStartTimeUs && r.simEndTimeUs && r.renderSubmitStartTimeUs &&
            r.renderSubmitEndTimeUs && r.presentStartTimeUs && r.presentEndTimeUs &&
            r.gpuRenderStartTimeUs && r.gpuRenderEndTimeUs && (!latest || r.presentID > latest->presentID)) {
            latest = &r;
        }
    }
    if (!latest) return false;
    const auto& report = *latest;
    out_report->present_id = report.presentID;
    out_report->input_sample_us = report.inputSampleTimeUs;
    out_report->sim_start_us = report.simStartTimeUs;
    out_report->sim_end_us = report.simEndTimeUs;
    out_report->render_submit_start_us = report.renderSubmitStartTimeUs;
    out_report->render_submit_end_us = report.renderSubmitEndTimeUs;
    out_report->present_start_us = report.presentStartTimeUs;
    out_report->present_end_us = report.presentEndTimeUs;
    out_report->gpu_render_start_us = report.gpuRenderStartTimeUs;
    out_report->gpu_render_end_us = report.gpuRenderEndTimeUs;
    return true;
}

} // namespace Vulkan::Reflex

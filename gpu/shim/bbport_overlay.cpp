// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_overlay.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <SDL3/SDL.h>
#include "bbport_compile_progress.h"
#include "bbport_frame_state.h"
#include "bbport_hdr.h"
#include "bbport_settings.h"
#include "../../src/pad_bindings.h"
#include "party/party_hud.h"
#include "party/party_balance_rules.h"
#include "party/party_gather.h"
#include "party/party_npctalk.h"
#include "party/party_ping.h"
#include "party/party_status.h"
#include "party/party_log.h"
#include "party/seamless_rules.h"
#include "party/world_warper.h"
#include "bbport_camera.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_vulkan.h"
#include "video_core/renderer_vulkan/ui_composition.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include <map>
#include <thread>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <pdh.h>
#endif

// DejaVu Sans (Cyrillic), embedded (third_party/fonts, Bitstream Vera license).
#ifdef _WIN32
asm(".section .rdata,\"dr\"\n"
    ".balign 16\n"
    ".global bb_font_ttf\n"
    "bb_font_ttf:\n"
    ".incbin \"" BB_FONT_PATH "\"\n"
    ".global bb_font_ttf_end\n"
    "bb_font_ttf_end:\n"
    ".text\n");
#else
asm(".section .rodata\n"
    ".balign 16\n"
    ".hidden bb_font_ttf\n"
    ".global bb_font_ttf\n"
    "bb_font_ttf:\n"
    ".incbin \"" BB_FONT_PATH "\"\n"
    ".hidden bb_font_ttf_end\n"
    ".global bb_font_ttf_end\n"
    "bb_font_ttf_end:\n"
    ".previous\n");
#endif
extern "C" const unsigned char bb_font_ttf[];
extern "C" const unsigned char bb_font_ttf_end[];
// DejaVu Serif (same license): the settings menu, styled like the game's own menus.
#ifdef _WIN32
asm(".section .rdata,\"dr\"\n"
    ".balign 16\n"
    ".global bb_serif_ttf\n"
    "bb_serif_ttf:\n"
    ".incbin \"" BB_SERIF_FONT_PATH "\"\n"
    ".global bb_serif_ttf_end\n"
    "bb_serif_ttf_end:\n"
    ".text\n");
#else
asm(".section .rodata\n"
    ".balign 16\n"
    ".hidden bb_serif_ttf\n"
    ".global bb_serif_ttf\n"
    "bb_serif_ttf:\n"
    ".incbin \"" BB_SERIF_FONT_PATH "\"\n"
    ".hidden bb_serif_ttf_end\n"
    ".global bb_serif_ttf_end\n"
    "bb_serif_ttf_end:\n"
    ".previous\n");
#endif
extern "C" const unsigned char bb_serif_ttf[];
extern "C" const unsigned char bb_serif_ttf_end[];
// Libre Baskerville (SIL OFL 1.1, gpu/third_party/fonts/OFL-LibreBaskerville.txt): the game's
// Baskerville look for the in-game party text (name tags, party HUD, notices) and menu labels.
#ifdef _WIN32
asm(".section .rdata,\"dr\"\n"
    ".balign 16\n"
    ".global bb_game_ttf\n"
    "bb_game_ttf:\n"
    ".incbin \"" BB_GAME_FONT_PATH "\"\n"
    ".global bb_game_ttf_end\n"
    "bb_game_ttf_end:\n"
    ".text\n");
#else
asm(".section .rodata\n"
    ".balign 16\n"
    ".hidden bb_game_ttf\n"
    ".global bb_game_ttf\n"
    "bb_game_ttf:\n"
    ".incbin \"" BB_GAME_FONT_PATH "\"\n"
    ".hidden bb_game_ttf_end\n"
    ".global bb_game_ttf_end\n"
    "bb_game_ttf_end:\n"
    ".previous\n");
#endif
extern "C" const unsigned char bb_game_ttf[];
extern "C" const unsigned char bb_game_ttf_end[];

extern "C" void runtime_restart(void); // bb-probe (probe.c)
extern "C" void bbgpu_request_quit(void); // bbgpu.cpp: as the window's close button

namespace BbOverlay {

namespace {

std::mutex imgui_mutex; // the ImGui context: window thread (input) and present thread
bool initialized = false;
std::atomic<bool> menu_open{false};
bool l3_down = false, r3_down = false;
bool pad_toggle = false; // L3+R3 opens the menu only with BB_OVERLAY_PAD=1 (launcher option)
bool dirty = false; // settings changed while open: saved on close
float base_scale = 1.0f;
ImFont* sans_font = nullptr;
ImFont* serif_font = nullptr;
ImFont* game_font = nullptr; // Libre Baskerville, DejaVu Serif merged in for Cyrillic
bool focus_request = true; // the menu was opened: its first row takes the focus

// Party mode notices (join / leave / travel, party_status SetLastEvent): a toast in a bottom corner
// for a few seconds, never taking input. Atomics: Visible() reads them without imgui_mutex.
std::atomic<std::uint64_t> toast_seen{0};  // the party event already shown
std::atomic<std::int64_t> toast_until_ms{0}; // steady clock
std::string toast_text;                      // present thread (imgui_mutex)

std::int64_t SteadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Party HUD (party_hud.h): companions' names and health in the bottom left corner, stacked above
// the performance HUD when it has that corner; the toast goes above the panel. Present thread.
party::hud::Board hud_board;          // the overlay's copy (Read each frame: fixed size, no allocation)
bool perf_hud_banner = false;         // this frame: the game's area-name banner may be up
bool perf_hud_bottom_left = false;    // this frame: the performance HUD's window ...
float perf_hud_top = 0.0f;            // ... and its top edge
float party_hud_top = -1.0f;          // this frame: the panel's top edge (-1: not drawn)
ImVec2 party_hud_size{0.0f, 0.0f};    // the panel's size last frame (its placement clear of the boss bar)

/// While a boss health bar is up: the screen area of the game's boss bars (name and bar, every
/// stacked one), which the party panel and the notices keep clear of (party_hud.h BossBarArea).
/// False while no bar is up.
bool BossBarArea(const ImGuiViewport* viewport, party::hud::HudRect* out) {
    const int rows = coop::BossBarRows();
    if (rows <= 0) return false;
    const auto canvas = Vulkan::UiComposition::Canvas();
    party::hud::HudRect stage = party::hud::HudStage(viewport->Size.x, viewport->Size.y,
                                                     float(canvas[0]) / float(canvas[1]));
    stage.x0 += viewport->Pos.x;
    stage.x1 += viewport->Pos.x;
    stage.y0 += viewport->Pos.y;
    stage.y1 += viewport->Pos.y;
    *out = party::hud::BossBarArea(stage, rows);
    return true;
}
const bool party_tags_on = [] {
    const char* v = std::getenv("BB_PARTY_HUD_TAGS");
    return !(v && v[0] == '0');
}();

bool PartyHudWanted() {
    return party::status::Enabled() && BbSettings::Get().party_hud && party::hud::Shown();
}

/// The overhead display (E2, bbport.ini party_overhead; independent of the panel).
bool PartyTagsWanted() {
    return party_tags_on && party::status::Enabled() && BbSettings::Get().party_name_tags &&
           BbSettings::Get().party_overhead.load() != 0 &&
           party::hud::Shown() && !party::hud::GameDialog();  // not over the game's dimmed dialog box
}

/// I2: "Show party through walls" (bbport.ini party_through_walls; env BB_PARTY_THROUGH_WALLS=0/1
/// over it, for tests).
bool PartyThroughWalls() {
    static const char* const env = std::getenv("BB_PARTY_THROUGH_WALLS");
    return env && *env ? env[0] != '0' : BbSettings::Get().party_through_walls.load();
}

/// I2 pings and the party through walls (party_ping.h): a party with a companion, nothing hiding it.
bool PartyPingsWanted() {
    return party::status::Enabled() && (coop::ping::Enabled() || PartyThroughWalls()) &&
           party::hud::Shown() && !party::hud::GameDialog();
}

// Party alert (party_status SetAlert: the host refused us): a message box in the middle of the
// screen for AlertMs, whatever the notices switch says. Never takes input.
std::atomic<std::uint64_t> alert_seen{0};
std::atomic<std::int64_t> alert_until_ms{0};
std::string alert_text; // present thread (imgui_mutex)
constexpr std::int64_t AlertMs = 15000;

bool AlertWanted() {
    return party::status::Enabled() &&
           (party::status::AlertSeq() != alert_seen.load() || SteadyMs() < alert_until_ms.load());
}

// Shared NPC dialogue (party_npctalk.h): the caption under a companion's conversation.
// The game's full-screen menu (party::hud::GameMenu): the notices and the caption are held - not
// drawn and their time stopped - so they come back for their remaining time once it closes.
// caption_hold: the caption's until_ms it holds for and the ms added to it (present thread).
std::atomic<std::int64_t> caption_hold_key{0}, caption_hold_ms{0};
std::int64_t menu_hold_last_ms = 0;  // present thread: the previous frame's time while held

std::int64_t CaptionUntilMs(std::int64_t until_ms) {
    return until_ms + (caption_hold_key.load() == until_ms ? caption_hold_ms.load() : 0);
}

bool NpcCaptionWanted() {
    return party::status::Enabled() && CaptionUntilMs(coop::npctalk::CurrentCaption().until_ms) > SteadyMs();
}

bool ToastWanted() {
    return party::status::Enabled() && BbSettings::Get().party_toasts &&
           (party::status::EventSeq() != toast_seen.load() || SteadyMs() < toast_until_ms.load());
}

// The game's text dialog (ImeDialog, the character name), typed on the keyboard: drawn while it
// is open. In fullscreen the window title that showed it is not visible (issues #17, #19).
std::mutex prompt_mutex;
std::atomic<bool> prompt_active{false};
std::string prompt_title, prompt_text;

// Present rate for the FPS counter and the HUD.
std::chrono::steady_clock::time_point last_present{};
float frame_ms_avg = 0.0f;
// The HUD's frame-time graph: the last present intervals (present thread only).
constexpr int FrameHistory = 120;
float frame_history[FrameHistory] = {};
int frame_history_next = 0;

// Performance HUD telemetry (from Mrsuss60/bloodborne_pc_windows_port: NVML, the Windows PDH
// "GPU Engine" counters, GetSystemTimes, GlobalMemoryStatusEx). Sampled every 0.5 s on a thread of
// its own while the HUD is shown, so a slow PDH query never delays a present. -1: unknown.
namespace Telemetry {

std::atomic<bool> wanted{false};
std::atomic<float> gpu{-1.0f}, cpu{-1.0f}, ram_used_gb{-1.0f}, ram_total_gb{-1.0f};
std::atomic<float> vram_used_gb{-1.0f}, vram_budget_gb{-1.0f};
const Vulkan::Instance* instance = nullptr;
u32 vendor_id = 0;
u8 device_luid[8] = {};
bool device_luid_valid = false;
std::once_flag started;

#ifdef _WIN32
// NVML (NVIDIA driver), loaded at run time; used only when the game's GPU is NVIDIA's.
struct NvmlUtilization {
    unsigned int gpu, memory;
};
using NvmlInit = int (*)();
using NvmlHandleByIndex = int (*)(unsigned int, void**);
using NvmlUtilizationRates = int (*)(void*, NvmlUtilization*);
NvmlUtilizationRates nvml_utilization = nullptr;
void* nvml_device = nullptr;

// PDH (any vendor): the 3D engines' utilization, summed over processes per engine of the game's
// adapter (its LUID in the instance name), the busiest engine as Task Manager shows it.
using PdhOpen = decltype(&PdhOpenQueryW);
using PdhAdd = decltype(&PdhAddEnglishCounterW);
using PdhCollect = decltype(&PdhCollectQueryData);
using PdhArray = decltype(&PdhGetFormattedCounterArrayW);
PdhCollect pdh_collect = nullptr;
PdhArray pdh_array = nullptr;
PDH_HQUERY pdh_query = nullptr;
PDH_HCOUNTER pdh_counter = nullptr;
std::vector<unsigned char> pdh_buffer;
wchar_t luid_tag[48] = {};

void InitGpu() {
    if (vendor_id == 0x10DE) {
        if (HMODULE nvml = LoadLibraryA("nvml.dll")) {
            auto init = reinterpret_cast<NvmlInit>(GetProcAddress(nvml, "nvmlInit_v2"));
            if (!init) init = reinterpret_cast<NvmlInit>(GetProcAddress(nvml, "nvmlInit"));
            auto by_index = reinterpret_cast<NvmlHandleByIndex>(
                GetProcAddress(nvml, "nvmlDeviceGetHandleByIndex_v2"));
            if (!by_index) {
                by_index = reinterpret_cast<NvmlHandleByIndex>(
                    GetProcAddress(nvml, "nvmlDeviceGetHandleByIndex"));
            }
            nvml_utilization = reinterpret_cast<NvmlUtilizationRates>(
                GetProcAddress(nvml, "nvmlDeviceGetUtilizationRates"));
            if (init && by_index && nvml_utilization && init() == 0 && by_index(0, &nvml_device) == 0 &&
                nvml_device) {
                return;
            }
            nvml_device = nullptr;
        }
    }
    HMODULE pdh = LoadLibraryA("pdh.dll");
    if (!pdh) {
        return;
    }
    const auto open = reinterpret_cast<PdhOpen>(GetProcAddress(pdh, "PdhOpenQueryW"));
    const auto add = reinterpret_cast<PdhAdd>(GetProcAddress(pdh, "PdhAddEnglishCounterW"));
    pdh_collect = reinterpret_cast<PdhCollect>(GetProcAddress(pdh, "PdhCollectQueryData"));
    pdh_array = reinterpret_cast<PdhArray>(GetProcAddress(pdh, "PdhGetFormattedCounterArrayW"));
    if (!open || !add || !pdh_collect || !pdh_array || open(nullptr, 0, &pdh_query) != ERROR_SUCCESS) {
        pdh_query = nullptr;
        return;
    }
    if (add(pdh_query, L"\\GPU Engine(*engtype_3D)\\Utilization Percentage", 0, &pdh_counter) !=
        ERROR_SUCCESS) {
        pdh_counter = nullptr;
        return;
    }
    pdh_collect(pdh_query); // a rate: the first value comes from the second collection
    if (device_luid_valid) {
        LUID luid;
        std::memcpy(&luid, device_luid, sizeof(luid));
        std::swprintf(luid_tag, std::size(luid_tag), L"luid_0x%08lX_0x%08lX",
                      static_cast<unsigned long>(luid.HighPart), static_cast<unsigned long>(luid.LowPart));
    }
}

float SampleGpu() {
    if (nvml_device) {
        NvmlUtilization rates{};
        return nvml_utilization(nvml_device, &rates) == 0 ? float(rates.gpu) : -1.0f;
    }
    if (!pdh_counter || pdh_collect(pdh_query) != ERROR_SUCCESS) {
        return -1.0f;
    }
    DWORD size = 0, count = 0;
    pdh_array(pdh_counter, PDH_FMT_DOUBLE, &size, &count, nullptr);
    if (size == 0) {
        return -1.0f;
    }
    pdh_buffer.resize(size);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(pdh_buffer.data());
    if (pdh_array(pdh_counter, PDH_FMT_DOUBLE, &size, &count, items) != ERROR_SUCCESS) {
        return -1.0f;
    }
    std::map<std::wstring, double> engines; // "<luid>_phys_N_eng_M": summed over processes
    for (DWORD i = 0; i < count; ++i) {
        // 0 valid, 1 new data; a new instance has no rate before its second collection.
        if (items[i].FmtValue.CStatus > 1 || !items[i].szName) {
            continue;
        }
        const std::wstring_view name{items[i].szName};
        if (luid_tag[0] && name.find(luid_tag) == std::wstring_view::npos) {
            continue; // another adapter
        }
        // "pid_<n>_luid_<a>_<b>_phys_<p>_eng_<e>_engtype_3D": the engine without the process.
        const auto luid_at = name.find(L"luid_");
        const size_t start = luid_at == std::wstring_view::npos ? 0 : luid_at;
        const auto type_at = name.find(L"_engtype_", start);
        const size_t length = type_at == std::wstring_view::npos ? std::wstring_view::npos : type_at - start;
        engines[std::wstring(name.substr(start, length))] += items[i].FmtValue.doubleValue;
    }
    double busiest = 0.0;
    for (const auto& [engine, value] : engines) {
        busiest = std::max(busiest, value);
    }
    return std::clamp(float(busiest), 0.0f, 100.0f);
}

void SampleSystem() {
    static ULONGLONG last_idle = 0, last_total = 0;
    FILETIME idle_time, kernel_time, user_time;
    if (GetSystemTimes(&idle_time, &kernel_time, &user_time)) {
        const auto to64 = [](const FILETIME& t) {
            return (ULONGLONG(t.dwHighDateTime) << 32) | t.dwLowDateTime;
        };
        const ULONGLONG idle = to64(idle_time);
        const ULONGLONG total = to64(kernel_time) + to64(user_time); // kernel includes idle
        if (last_total && total > last_total) {
            const double busy = 1.0 - double(idle - last_idle) / double(total - last_total);
            cpu = std::clamp(float(busy * 100.0), 0.0f, 100.0f);
        }
        last_idle = idle;
        last_total = total;
    }
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) {
        constexpr float gb = 1024.0f * 1024.0f * 1024.0f;
        ram_total_gb = float(memory.ullTotalPhys) / gb;
        ram_used_gb = float(memory.ullTotalPhys - memory.ullAvailPhys) / gb;
    }
}
#else
void InitGpu() {}
float SampleGpu() {
    return -1.0f;
}
void SampleSystem() {}
#endif

void SampleVram() {
    if (!instance) {
        return;
    }
    constexpr double gb = 1024.0 * 1024.0 * 1024.0;
    if (instance->CanReportMemoryUsage()) {
        vram_used_gb = float(double(instance->GetDeviceMemoryUsage()) / gb);
        vram_budget_gb = float(double(instance->GetDeviceMemoryBudgetNow()) / gb);
    } else {
        u64 blocks = 0, allocations = 0;
        Vulkan::VmaDeviceUsage(blocks, allocations);
        vram_used_gb = float(double(blocks) / gb);
        vram_budget_gb = float(double(instance->GetDeviceLocalMemory()) / gb);
    }
}

void Start() {
    std::call_once(started, [] {
        std::thread([] {
            InitGpu();
            while (true) {
                if (wanted.load(std::memory_order_relaxed)) {
                    gpu = SampleGpu();
                    SampleSystem();
                    SampleVram();
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                }
            }
        }).detach();
    });
}

} // namespace Telemetry

float PixelDensity(SDL_WindowID id);

void SetOpen(bool value) {
    if (menu_open.exchange(value) == value) {
        return;
    }
    std::printf("Overlay: menu %s\n", value ? "opened" : "closed");
    // The system cursor shows over the menu (window.cpp); ImGui learns where it is now, not at
    // the next motion: mouse motion is not passed on while the menu is closed.
    if (value) {
        focus_request = true;
        if (SDL_Window* window = SDL_GetMouseFocus()) {
            float x = 0.0f, y = 0.0f;
            SDL_GetMouseState(&x, &y);
            const float density = PixelDensity(SDL_GetWindowID(window));
            ImGui::GetIO().AddMousePosEvent(x * density, y * density);
        }
    }
    if (!value && dirty) {
        dirty = false;
        BbSettings::Save();
    }
}

ImGuiKey KeyFromSdl(SDL_Keycode key) {
    switch (key) {
    case SDLK_TAB: return ImGuiKey_Tab;
    case SDLK_LEFT: return ImGuiKey_LeftArrow;
    case SDLK_RIGHT: return ImGuiKey_RightArrow;
    case SDLK_UP: return ImGuiKey_UpArrow;
    case SDLK_DOWN: return ImGuiKey_DownArrow;
    case SDLK_PAGEUP: return ImGuiKey_PageUp;
    case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
    case SDLK_HOME: return ImGuiKey_Home;
    case SDLK_END: return ImGuiKey_End;
    case SDLK_DELETE: return ImGuiKey_Delete;
    case SDLK_BACKSPACE: return ImGuiKey_Backspace;
    case SDLK_SPACE: return ImGuiKey_Space;
    case SDLK_RETURN: return ImGuiKey_Enter;
    case SDLK_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDLK_ESCAPE: return ImGuiKey_Escape;
    case SDLK_LCTRL: return ImGuiKey_LeftCtrl;
    case SDLK_RCTRL: return ImGuiKey_RightCtrl;
    case SDLK_LSHIFT: return ImGuiKey_LeftShift;
    case SDLK_RSHIFT: return ImGuiKey_RightShift;
    case SDLK_LALT: return ImGuiKey_LeftAlt;
    case SDLK_RALT: return ImGuiKey_RightAlt;
    case SDLK_Q: return ImGuiKey_Q;
    case SDLK_E: return ImGuiKey_E;
    default: return ImGuiKey_None;
    }
}

ImGuiKey KeyFromGamepad(u8 button) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return ImGuiKey_GamepadFaceDown;
    case SDL_GAMEPAD_BUTTON_EAST: return ImGuiKey_GamepadFaceRight;
    case SDL_GAMEPAD_BUTTON_WEST: return ImGuiKey_GamepadFaceLeft;
    case SDL_GAMEPAD_BUTTON_NORTH: return ImGuiKey_GamepadFaceUp;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ImGuiKey_GamepadL1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ImGuiKey_GamepadR1;
    case SDL_GAMEPAD_BUTTON_START: return ImGuiKey_GamepadStart;
    case SDL_GAMEPAD_BUTTON_BACK: return ImGuiKey_GamepadBack;
    default: return ImGuiKey_None;
    }
}

float PixelDensity(SDL_WindowID id) {
    SDL_Window* window = SDL_GetWindowFromID(id);
    const float density = window ? SDL_GetWindowPixelDensity(window) : 1.0f;
    return density > 0.0f ? density : 1.0f;
}

// Marks the settings dirty when a widget changed them.
template <typename T>
void Store(std::atomic<T>& target, T value, bool changed) {
    if (changed) {
        target = value;
        dirty = true;
    }
}

void Checkbox(const char* label, std::atomic<bool>& value) {
    bool v = value;
    const bool changed = ImGui::Checkbox(label, &v);
    Store(value, v, changed);
}

void Slider(const char* label, std::atomic<float>& value, float lo, float hi) {
    float v = value;
    const bool changed = ImGui::SliderFloat(label, &v, lo, hi, "%.2f");
    Store(value, v, changed);
}

void Hint(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// The settings menu, laid out like a PC game's options screen in the game's own style: the game
// dimmed behind, a centred panel framed by ornament lines, tabs (L1/R1, Q/E, mouse), one setting
// per row ("name  < value >": left/right change it, Cross/Enter steps it, the mouse clicks either
// side), the focused row's description at the bottom and the controls in the footer.
namespace Ui {

float S(float v) {
    return v * base_scale;
}
const char* T(const char* english, const char* russian) {
    return BbSettings::MenuText(english, russian);
}
ImU32 Gold(float alpha = 1.0f) {
    return ImGui::GetColorU32(ImVec4(0.80f, 0.68f, 0.46f, alpha));
}
ImU32 Bright(float alpha = 1.0f) {
    return ImGui::GetColorU32(ImVec4(0.96f, 0.88f, 0.70f, alpha));
}
ImU32 Plain(float alpha = 1.0f) {
    return ImGui::GetColorU32(ImVec4(0.80f, 0.77f, 0.70f, alpha));
}
ImU32 Dim(float alpha = 1.0f) {
    return ImGui::GetColorU32(ImVec4(0.48f, 0.46f, 0.42f, alpha));
}

/// The game's dialog border: a line fading out to both ends, a small diamond in the middle.
void Ornament(ImDrawList* draw, float x0, float x1, float y) {
    const float mid = (x0 + x1) * 0.5f, t = S(1.2f), d = S(5.0f);
    draw->AddRectFilledMultiColor(ImVec2(x0, y - t * 0.5f), ImVec2(mid, y + t * 0.5f), Gold(0.0f),
                                  Gold(0.85f), Gold(0.85f), Gold(0.0f));
    draw->AddRectFilledMultiColor(ImVec2(mid, y - t * 0.5f), ImVec2(x1, y + t * 0.5f), Gold(0.85f),
                                  Gold(0.0f), Gold(0.0f), Gold(0.85f));
    draw->AddQuadFilled(ImVec2(mid, y - d), ImVec2(mid + d, y), ImVec2(mid, y + d),
                        ImVec2(mid - d, y), Gold(0.95f));
    draw->AddQuadFilled(ImVec2(mid - d * 3.0f, y - d * 0.5f), ImVec2(mid - d * 2.5f, y),
                        ImVec2(mid - d * 3.0f, y + d * 0.5f), ImVec2(mid - d * 3.5f, y), Gold(0.7f));
    draw->AddQuadFilled(ImVec2(mid + d * 3.0f, y - d * 0.5f), ImVec2(mid + d * 3.5f, y),
                        ImVec2(mid + d * 3.0f, y + d * 0.5f), ImVec2(mid + d * 2.5f, y), Gold(0.7f));
}

// Focus is the menu's own, as in a game: a row index moved by up/down to the next enabled row
// (ImGui's navigation did not reach these rows), set by the mouse, scrolled into view.
struct State {
    int tab = 0;
    bool focus_first = true;
    int focus = 0;                   ///< the focused row of the current tab
    int move = 0;                    ///< -1 / +1: up / down pressed this frame
    bool activate = false;           ///< Cross / Enter pressed this frame
    bool scroll = false;             ///< bring the focused row into view
    int rows = 0;                    ///< rows drawn this frame
    std::vector<bool> enabled;       ///< per row drawn this frame
    const char* hint = nullptr;      ///< the focused row's description
    const char* next_hint = nullptr; ///< collected while drawing this frame
};
State state;

/// One setting row. Returns -1 / +1 when the value is to step back / forward (keys, Cross, a
/// click on either side of the value), 0 otherwise. A slider row also takes a drag: `drag`
/// receives the fraction under the mouse.
int Row(const char* id, const char* label, const char* value, const char* hint, bool enabled,
        float slider = -1.0f, float* drag = nullptr) {
    ImGui::PushID(id);
    const int index = state.rows++;
    state.enabled.push_back(enabled);
    const float width = ImGui::GetContentRegionAvail().x, height = S(44.0f);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::InvisibleButton("##row", ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    ImGui::EndDisabled();
    if (enabled && hovered && (ImGui::GetIO().MouseDelta.x != 0.0f || ImGui::GetIO().MouseDelta.y != 0.0f ||
                               pressed)) {
        state.focus = index;
    }
    const bool focused = enabled && index == state.focus;
    if (focused && state.scroll) {
        ImGui::SetScrollHereY(0.5f);
        state.scroll = false;
    }
    if (focused && hint) {
        state.next_hint = hint;
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float mid_y = at.y + height * 0.5f;
    if (focused || hovered) {
        // The game's highlight: a glow strongest under the text, fading to the sides.
        const float a = focused ? 0.22f : 0.10f;
        const float x0 = at.x, x1 = at.x + width, xm = at.x + width * 0.35f;
        draw->AddRectFilledMultiColor(ImVec2(x0, at.y), ImVec2(xm, at.y + height), Gold(0.0f),
                                      Gold(a), Gold(a), Gold(0.0f));
        draw->AddRectFilledMultiColor(ImVec2(xm, at.y), ImVec2(x1, at.y + height), Gold(a),
                                      Gold(0.0f), Gold(0.0f), Gold(a));
        if (focused) {
            draw->AddLine(ImVec2(x0 + width * 0.1f, at.y + height - S(1.0f)),
                          ImVec2(x1 - width * 0.1f, at.y + height - S(1.0f)), Gold(0.45f), S(1.0f));
        }
    }
    const ImU32 label_color = !enabled ? Dim() : focused ? Bright() : Plain();
    ImGui::PushFont(game_font, 0.0f);  // labels in the game's Baskerville at the current size (GetFontSize() would scale twice)
    const ImVec2 label_size = ImGui::CalcTextSize(label);
    draw->AddText(ImVec2(at.x + S(28.0f), mid_y - label_size.y * 0.5f), label_color, label);
    ImGui::PopFont();

    // The value column: "<  value  >" centred, or a bar for sliders.
    const float col0 = at.x + width * 0.56f, col1 = at.x + width - S(28.0f);
    const float col_mid = (col0 + col1) * 0.5f;
    int step = 0;
    if (enabled && value) {
        const ImU32 arrow = focused ? Gold(1.0f) : Gold(0.55f);
        const float h = S(7.0f);
        draw->AddTriangleFilled(ImVec2(col0, mid_y), ImVec2(col0 + h, mid_y - h),
                                ImVec2(col0 + h, mid_y + h), arrow);
        draw->AddTriangleFilled(ImVec2(col1, mid_y), ImVec2(col1 - h, mid_y - h),
                                ImVec2(col1 - h, mid_y + h), arrow);
    }
    if (slider >= 0.0f) {
        const float b0 = col0 + S(24.0f), b1 = col1 - S(24.0f) - S(60.0f);
        const float bh = S(3.0f);
        draw->AddRectFilled(ImVec2(b0, mid_y - bh), ImVec2(b1, mid_y + bh), Dim(0.6f), bh);
        draw->AddRectFilled(ImVec2(b0, mid_y - bh),
                            ImVec2(b0 + (b1 - b0) * std::clamp(slider, 0.0f, 1.0f), mid_y + bh),
                            enabled ? Gold(0.9f) : Dim(), bh);
        const float knob = b0 + (b1 - b0) * std::clamp(slider, 0.0f, 1.0f);
        draw->AddCircleFilled(ImVec2(knob, mid_y), S(7.0f), enabled ? Bright() : Dim());
        if (value) {
            const ImVec2 size = ImGui::CalcTextSize(value);
            draw->AddText(ImVec2(col1 - S(24.0f) - size.x, mid_y - size.y * 0.5f),
                          enabled ? Plain() : Dim(), value);
        }
        if (enabled && drag && active && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const float x = ImGui::GetIO().MousePos.x;
            if (x >= b0 - S(12.0f) && x <= b1 + S(12.0f)) {
                *drag = std::clamp((x - b0) / (b1 - b0), 0.0f, 1.0f);
            }
        }
    } else if (value) {
        const ImVec2 size = ImGui::CalcTextSize(value);
        draw->AddText(ImVec2(col_mid - size.x * 0.5f, mid_y - size.y * 0.5f),
                      !enabled ? Dim() : focused ? Bright() : Plain(), value);
    }
    if (enabled && focused) {
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadLStickLeft)) {
            step = -1;
        } else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) ||
                   ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight) ||
                   ImGui::IsKeyPressed(ImGuiKey_GamepadLStickRight)) {
            step = 1;
        }
    }
    if (enabled && pressed && slider < 0.0f) {
        step = ImGui::GetIO().MousePos.x < col_mid && ImGui::GetIO().MousePos.x >= col0 - S(12.0f) ? -1 : 1;
    } else if (focused && state.activate) {
        step = 1;
        state.activate = false;
    }
    ImGui::PopID();
    return step;
}

/// A non-interactive line under the rows (state, warnings).
void Note(const char* text, ImU32 color) {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::Indent(S(28.0f));
    ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x - S(28.0f));
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::Unindent(S(28.0f));
    ImGui::PopStyleColor();
}

const char* OnOff(bool on) {
    return on ? T("On", "Вкл") : T("Off", "Выкл");
}

void Toggle(const char* id, const char* label, std::atomic<bool>& value, const char* hint,
            bool enabled = true) {
    if (Row(id, label, OnOff(value), hint, enabled) != 0) {
        Store(value, !value.load(), true);
    }
}

/// A choice among `count` values (wrapping); `allowed(i)` false skips a value.
template <typename Label, typename Allowed>
void Choice(const char* id, const char* label, int current, int count, Label&& name,
            Allowed&& allowed, const char* hint, bool enabled, const std::function<void(int)>& set) {
    const int step = Row(id, label, name(current), hint, enabled);
    if (step == 0) {
        return;
    }
    for (int n = 1; n <= count; ++n) {
        const int next = ((current + step * n) % count + count) % count;
        if (allowed(next)) {
            if (next != current) {
                set(next);
            }
            return;
        }
    }
}

void SliderRow(const char* id, const char* label, std::atomic<float>& value, float lo, float hi,
               float step_size, const char* hint, bool enabled = true) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.2f", value.load());
    float drag = -1.0f;
    const int step = Row(id, label, text, hint, enabled, (value - lo) / (hi - lo), &drag);
    if (step != 0) {
        const float v = std::clamp(std::round((value + step * step_size) / step_size) * step_size, lo, hi);
        Store(value, v, true);
    } else if (drag >= 0.0f) {
        const float v = std::round((lo + drag * (hi - lo)) / 0.01f) * 0.01f;
        Store(value, v, v != value);
    }
}

bool RestartNeeded() {
    return BbSettings::RestartNeeded();
}

void DisplayTab() {
    auto& s = BbSettings::Get();
    static const char* outputs[] = {"1280 x 720", "1920 x 1080", "2560 x 1440", "3840 x 2160",
                                    "2560 x 1080 (21:9)", "3440 x 1440 (21:9)",
                                    "3840 x 1600 (21:9)", "5120 x 2160 (21:9)",
                                    "3840 x 1080 (32:9)", "5120 x 1440 (32:9)"};
    static_assert(sizeof(outputs) / sizeof(*outputs) == BbSettings::OutputCount);
    Choice("output", T("Output resolution", "Разрешение вывода"), s.output_res, BbSettings::OutputCount,
           [](int i) { return outputs[i]; }, [](int) { return true; },
           BbSettings::FixedRenderSession()
               ? T("Size of the final frame and UI. The preset sets the scene size relative to "
                   "the output: 4K Performance = 1920x1080. Applies after restarting the game.",
                   "Размер готового кадра и интерфейса. Пресет задаёт размер сцены относительно "
                   "вывода: 4K Performance = 1920x1080. Применяется после перезапуска игры.")
               : T("The final frame and UI size changes at the next frame. The preset sets the "
                   "scene size relative to the output: 4K Performance = 1920x1080. Changing it "
                   "resets the upscaler's history (a brief pause).",
                   "Размер готового кадра и интерфейса меняется со следующего кадра. Пресет задаёт "
                   "размер сцены относительно вывода: 4K Performance = 1920x1080. Смена сбрасывает "
                   "историю апскейлера (короткая пауза)."),
           true, [&](int i) { Store(s.output_res, i, true); });
    const char* live_modes[] = {T("Auto (by GPU)", "Авто (по видеокарте)"), T("Off (faster)", "Выкл (быстрее)"),
                                T("On", "Вкл")};
    Choice("live", T("Live resolution changes", "Смена разрешения на лету"), s.live_resolution + 1, 3,
           [&](int i) { return live_modes[i]; }, [](int) { return true; },
           T("On: output resolution and preset change without restarting, but the game's "
             "post-processing stays at 1080p (slower on the Steam Deck and older GPUs). Off: "
             "everything renders at the preset's size, changes need a restart. Auto turns it on "
             "for strong discrete GPUs. Applies after restarting the game.",
             "Вкл: разрешение вывода и пресет меняются без перезапуска, но постобработка игры "
             "остаётся в 1080p (медленнее на Steam Deck и старых видеокартах). Выкл: всё рисуется "
             "в размере пресета, смена — через перезапуск. Авто включает её на мощных дискретных "
             "видеокартах. Применяется после перезапуска игры."),
           true, [&](int i) { Store(s.live_resolution, i - 1, true); });
    const char* volumetric_levels[] = {T("Game default (half)", "Как в игре (половина)"),
                                       T("2x pixels", "2x пикселей"),
                                       T("Full resolution", "Полное разрешение")};
    Choice("volumetric", T("Motion blur resolution", "Разрешение размытия в движении"),
           std::clamp(s.volumetric_res.load(), 0, 2), 3, [&](int i) { return volumetric_levels[i]; },
           [](int) { return true; },
           T("The game draws the motion blur of moving characters and objects from "
             "half-resolution buffers; 2x pixels or full resolution give it finer edges at a GPU "
             "cost. Smoke and mist are drawn at full resolution already. Live.",
             "Игра строит размытие в движении персонажей и объектов по буферам половинного "
             "разрешения; 2x пикселей или полное разрешение дают более точные края ценой нагрузки "
             "на видеокарту. Дым и туман и так рисуются в полном разрешении. Сразу."),
           true, [&](int i) { Store(s.volumetric_res, i, true); });
    Toggle("fps",T("FPS counter", "Счётчик FPS"), s.show_fps,
           s.show_hud ? T("The performance HUD is on and shows the frame rate itself.",
                          "Включён оверлей производительности: частота кадров показана в нём.")
                      : T("Frames per second and frame time in the top right corner. The "
                          "performance HUD below shows more.",
                          "Кадры в секунду и время кадра в правом верхнем углу. Оверлей "
                          "производительности ниже показывает больше."),
           !s.show_hud);
    // Performance HUD (from Mrsuss60/bloodborne_pc_windows_port).
    const bool hud = s.show_hud;
    Toggle("hud", T("Performance HUD (F11)", "Оверлей производительности (F11)"), s.show_hud,
           T("Frame rate and frame time, with a frame-time graph, video memory, GPU, CPU and RAM "
             "load (each can be hidden below). F11 shows or hides it at any time. Replaces the FPS "
             "counter while on.",
             "Частота и время кадра, график времени кадра, видеопамять, загрузка видеокарты, "
             "процессора и памяти (каждое можно скрыть ниже). F11 включает и выключает его в любой "
             "момент. Пока он включён, счётчик FPS не показывается."));
    const char* positions[] = {T("Top left", "Сверху слева"), T("Top right", "Сверху справа"),
                               T("Bottom left", "Снизу слева"), T("Bottom right", "Снизу справа")};
    Choice("hud_position", T("HUD position", "Положение оверлея"), s.hud_position,
           BbSettings::HudPositionCount, [&](int i) { return positions[i]; }, [](int) { return true; },
           T("Corner of the screen. In the bottom right corner the shader compilation indicator "
             "moves to the bottom left.",
             "Угол экрана. В правом нижнем углу индикатор компиляции шейдеров переносится в левый "
             "нижний."),
           hud, [&](int i) { Store(s.hud_position, i, true); });
    SliderRow("hud_opacity", T("HUD background opacity", "Непрозрачность фона оверлея"), s.hud_opacity,
              0.1f, 1.0f, 0.05f, nullptr, hud);
    SliderRow("hud_scale", T("HUD size", "Размер оверлея"), s.hud_scale, 0.5f, 2.0f, 0.1f, nullptr, hud);
    Toggle("hud_graph", T("HUD: frame-time graph", "Оверлей: график времени кадра"), s.hud_graph,
           T("The last 120 frame times; spikes are stutters.",
             "Время последних 120 кадров; пики — это подтормаживания."),
           hud);
    Toggle("hud_vram", T("HUD: video memory", "Оверлей: видеопамять"), s.hud_vram,
           T("Video memory used by the game and the driver's budget for it.",
             "Видеопамять, занятая игрой, и доступный ей бюджет драйвера."),
           hud);
    Toggle("hud_gpu", T("HUD: GPU load", "Оверлей: загрузка видеокарты"), s.hud_gpu,
           T("NVIDIA: from the driver (NVML). AMD / Intel: Windows' GPU counters, as in Task "
             "Manager.",
             "NVIDIA: от драйвера (NVML). AMD / Intel: счётчики Windows, как в диспетчере задач."),
           hud);
    Toggle("hud_cpu", T("HUD: CPU load", "Оверлей: загрузка процессора"), s.hud_cpu,
           T("Load of all CPU cores together.", "Общая загрузка всех ядер процессора."), hud);
    Toggle("hud_ram", T("HUD: RAM", "Оверлей: оперативная память"), s.hud_ram,
           T("System memory in use / installed.", "Занятая / установленная оперативная память."), hud);
    static const char* languages[] = {"English", "Русский"};
    Choice("language", "Language", s.menu_language == BbSettings::MenuLanguage::Russian ? 1 : 0, 2,
           [](int i) { return languages[i]; }, [](int) { return true; },
           T("Language of this menu.", "Язык этого меню."), true, [&](int i) {
               s.menu_language = i == 1 ? BbSettings::MenuLanguage::Russian
                                        : BbSettings::MenuLanguage::English;
               BbSettings::Save();
           });
}

/// Native HDR output (bbport_hdr.h, I4): on/off after a restart, the curve live.
void HdrRows() {
    auto& s = BbSettings::Get();
    char line[256];
    Toggle("hdr", T("HDR output (HDR10)", "Вывод HDR (HDR10)"), s.hdr,
           T("An HDR10 picture (PQ, Rec.2020) for an HDR display: the game's SDR picture keeps its "
             "mid-tones at paper white and only its highlights are expanded towards the peak. Needs "
             "HDR on in Windows. Applies after restarting the game.",
             "Картинка HDR10 (PQ, Rec.2020) для HDR-дисплея: средние тона SDR-картинки игры остаются "
             "на уровне белого листа, к пику расширяются только блики. Нужен HDR в Windows. "
             "Применяется после перезапуска игры."));
    static const int peaks[] = {0, 400, 600, 800, 1000, 1200, 1500, 2000, 2500, 3000, 4000};
    static const int papers[] = {80, 100, 120, 150, 180, 200, 250, 300, 350, 400, 500};
    const auto index_of = [](const int* table, int value) {
        int best = 0;
        for (int i = 1; i < 11; ++i) {
            if (std::abs(table[i] - value) < std::abs(table[best] - value)) best = i;
        }
        return best;
    };
    static char peak_names[11][32];
    const int display = int(s.hdr_display_peak.load() + 0.5f);
    for (int i = 0; i < 11; ++i) {
        if (i == 0) {
            std::snprintf(peak_names[i], sizeof(peak_names[i]), display > 0 ? T("Auto (%d nits)", "Авто (%d нит)")
                                                                             : T("Auto (1000 nits)", "Авто (1000 нит)"),
                          display);
        } else {
            std::snprintf(peak_names[i], sizeof(peak_names[i]), T("%d nits", "%d нит"), peaks[i]);
        }
    }
    Choice("hdr_peak", T("HDR peak brightness", "HDR: пиковая яркость"), index_of(peaks, s.hdr_peak_nits), 11,
           [](int i) { return peak_names[i]; }, [](int) { return true; },
           T("The brightest the picture gets. Auto: what the display reports to Windows. Use the "
             "calibration pattern: raise it until the inner square just disappears.",
             "Самое яркое, что будет на экране. Авто: что дисплей сообщает Windows. По калибровочной "
             "таблице: поднимайте, пока внутренний квадрат не исчезнет."),
           true, [&](int i) { Store(s.hdr_peak_nits, peaks[i], true); });
    static char paper_names[11][32];
    for (int i = 0; i < 11; ++i) {
        std::snprintf(paper_names[i], sizeof(paper_names[i]), T("%d nits", "%d нит"), papers[i]);
    }
    Choice("hdr_paper", T("HDR paper white", "HDR: белый лист"), index_of(papers, s.hdr_paper_white), 11,
           [](int i) { return paper_names[i]; }, [](int) { return true; },
           T("Brightness of the picture's mid-tones and of these menus. 200 nits by default; "
             "lower in a dark room.",
             "Яркость средних тонов картинки и этих меню. 200 нит по умолчанию; в тёмной комнате "
             "меньше."),
           true, [&](int i) { Store(s.hdr_paper_white, papers[i], true); });
    SliderRow("hdr_strength", T("HDR highlights", "HDR: блики"), s.hdr_strength, 0.0f, 1.5f, 0.1f,
              T("0: the SDR picture at paper white; 1: the game's white reaches the peak (default); "
                "above 1 more highlights reach the peak.",
                "0: SDR-картинка на уровне белого листа; 1: белый игры достигает пика (по умолчанию); "
                "больше 1: больше бликов на пике."));
    SliderRow("hdr_saturation", T("HDR saturation", "HDR: насыщенность"), s.hdr_saturation, 0.5f, 1.5f, 0.05f,
              T("1: the game's colours; above 1 uses the wider Rec.2020 gamut.",
                "1: цвета игры; больше 1 использует более широкий охват Rec.2020."));
    Toggle("hdr_calibration", T("HDR calibration pattern", "HDR: калибровочная таблица"), s.hdr_calibration,
           T("Patches at the top of the screen: a 10000-nit field with a square at the peak setting "
             "(raise the peak until the square just disappears), paper white, a 0..peak ramp and the "
             "game's grey levels through the curve.",
             "Поля сверху экрана: поле 10000 нит с квадратом на уровне пика (поднимайте пик, пока "
             "квадрат не исчезнет), белый лист, шкала 0..пик и уровни серого игры после кривой."));
    if (s.hdr != s.startup_hdr) {
        Note(T("HDR output: applies after restarting the game", "Вывод HDR: после перезапуска игры"), Dim());
    } else if (s.hdr_active) {
        std::snprintf(line, sizeof(line), T("HDR output: %s, peak %.0f nits, paper white %d nits",
                                            "Вывод HDR: %s, пик %.0f нит, белый лист %d нит"),
                      s.hdr_active == 1 ? "HDR10 (PQ, Rec.2020)" : "scRGB", BbSettings::HdrPeakNits(),
                      s.hdr_paper_white.load());
        Note(line, Dim());
    } else if (const char* problem = s.hdr_problem.load(); problem && s.startup_hdr) {
        std::snprintf(line, sizeof(line), T("HDR output not used: %s", "Вывод HDR не используется: %s"), problem);
        Note(line, ImGui::GetColorU32(ImVec4(1.0f, 0.55f, 0.35f, 1.0f)));
    }
}

void UpscalerTab() {
    auto& s = BbSettings::Get();
    const char* upscalers[] = {T("Off", "Выкл"), "FSR 3.1", "FSR 4 (INT8)", "FSR 4.1.1",
                               T("TAA (native)", "TAA (нативное)"), "DLSS (NVIDIA RTX)"};
    const auto supported = [&](int i) {
        return i == BbSettings::UpscalerFsr4     ? s.fsr4_supported.load()
               : i == BbSettings::UpscalerFsr411 ? s.fsr411_supported.load()
               : i == BbSettings::UpscalerDlss   ? s.dlss_supported.load()
                                                 : true;
    };
    const char* upscaler_hint =
        s.upscaler == BbSettings::UpscalerFsr411
            ? T("FSR 4.1.1: the model from AMD's 4.1.1 DLL, reproduced in Vulkan (output matches "
                "the DLL). Assets are built from your DLL in the launcher.",
                "FSR 4.1.1: модель из DLL AMD 4.1.1, воспроизведённая в Vulkan (результат совпадает "
                "с DLL). Ассеты собираются из вашей DLL в лаунчере.")
        : s.upscaler == BbSettings::UpscalerFsr4
            ? T("FSR 4 in INT8 mode (v07 model from AMD's FidelityFX SDK sources): higher quality "
                "than FSR 3.1, heavier.",
                "FSR 4 в режиме INT8 (модель v07 из исходников AMD FidelityFX SDK): качество выше, "
                "чем у FSR 3.1, проход тяжелее.")
        : s.upscaler == BbSettings::UpscalerTaa
            ? T("TAA anti-aliases the scene at the output resolution, without an FSR model.",
                "TAA сглаживает сцену в разрешении вывода, без модели FSR.")
        : s.upscaler == BbSettings::UpscalerDlss
            ? T("NVIDIA DLSS Super Resolution (RTX GPUs; Native AA is DLAA). Needs the DLSS bridge "
                "and NVIDIA's library next to the game; otherwise FSR 3.1 is used.",
                "NVIDIA DLSS Super Resolution (видеокарты RTX; Native AA — это DLAA). Нужны мост DLSS "
                "и библиотека NVIDIA рядом с игрой; без них используется FSR 3.1.")
            : T("Temporal upscaler: anti-aliasing and rendering below the output resolution. FSR 4 "
                "needs a GPU with the required features (unavailable values are skipped).",
                "Временной апскейлер: сглаживание и отрисовка ниже разрешения вывода. FSR 4 нужна "
                "видеокарта с нужными возможностями (недоступные значения пропускаются).");
    Choice("upscaler", T("Upscaler", "Апскейлер"), s.upscaler, BbSettings::UpscalerCount,
           [&](int i) { return upscalers[i]; }, supported, upscaler_hint, true,
           [&](int i) { Store(s.upscaler, i, true); });
    const bool upscaler_on = s.upscaler != BbSettings::UpscalerOff;
    const bool taa = s.upscaler == BbSettings::UpscalerTaa;
    static char preset_names[BbSettings::PresetCount][64];
    for (int i = 0; i < BbSettings::PresetCount; ++i) {
        const float scale = BbSettings::PresetScale(i);
        std::snprintf(preset_names[i], sizeof(preset_names[i]), "%s  %dx%d", BbSettings::PresetName(i),
                      int(std::lround(BbSettings::OutputWidths[s.output_res] / scale / 2) * 2),
                      int(std::lround(BbSettings::OutputHeights[s.output_res] / scale / 2) * 2));
    }
    Choice("preset", T("Quality preset", "Пресет качества"), taa ? BbSettings::NativeAA : s.preset.load(),
           BbSettings::PresetCount, [](int i) { return preset_names[i]; }, [](int) { return true; },
           T("Native AA: the upscaler only anti-aliases. The other presets render the scene below "
             "the output resolution (the size shown); the UI stays at the output resolution.",
             "Native AA: апскейлер только сглаживает. Остальные пресеты рисуют сцену ниже "
             "разрешения вывода (показан размер); интерфейс — в разрешении вывода."),
           upscaler_on && !taa, [&](int i) { Store(s.preset, i, true); });
    Toggle("sharpen", T("Sharpening", "Резкость"), s.sharpen,
           T("RCAS sharpening after the upscaler.", "Повышение резкости RCAS после апскейлера."),
           upscaler_on);
    SliderRow("sharpness", T("Sharpness", "Сила резкости"), s.sharpness, 0.0f, 2.0f, 0.05f,
              T("Up to 1: the upscaler's own RCAS. Above 1 another RCAS pass is added.",
                "До 1 — RCAS самого апскейлера. Выше 1 добавляется ещё один проход RCAS."),
              upscaler_on && s.sharpen);
    Toggle("jitter", T("Subpixel jitter", "Субпиксельный сдвиг"), s.jitter,
           T("Each frame shifts the scene by a fraction of a pixel so the upscaler reconstructs "
             "more detail from several frames.",
             "Каждый кадр сцена сдвигается на долю пикселя, и апскейлер собирает больше деталей из "
             "нескольких кадров."),
           upscaler_on);
    Toggle("motion", T("Character motion vectors", "Векторы движения персонажей"), s.object_motion,
           T("Accurate motion for animated objects: clothes and weapons break up less in motion. "
             "Applies after restarting the game.",
             "Точное движение анимированных объектов: одежда и оружие меньше рассыпаются в "
             "движении. Применяется после перезапуска игры."),
           upscaler_on);
    if (BbSettings::IsFsr4(s.upscaler)) {
        Toggle("fsr4_exposure", T("FSR 4: auto exposure", "FSR 4: автоэкспозиция"), s.fsr4_auto_exposure,
               T("The FSR 4 network normalises colour by exposure and uses it to decide when to "
                 "drop older frames.",
                 "Сеть FSR 4 нормирует цвет по экспозиции и по ней решает, когда отбросить прошлые "
                 "кадры."));
        Toggle("fsr4_jitter", T("FSR 4: invert jitter sign", "FSR 4: обратный знак сдвига"),
               s.fsr4_invert_jitter, T("For diagnosing ghosting.", "Для проверки шлейфов."));
    }
    char line[160];
    std::snprintf(line, sizeof(line), T("Scene renders at %d x %d", "Сцена рисуется в %d x %d"),
                  s.active_render_width.load(), s.active_render_height.load());
    ImGui::Spacing();
    Note(line, Dim());
    if (s.upscaler_scene_above_output.load() && s.upscaler != BbSettings::UpscalerOff) {
        Note(T("The scene is larger than the output resolution: the upscaler does not run (it only "
               "upscales). Choose a render resolution at or below the output to use it.",
               "Сцена больше выходного разрешения: апскейлер не работает (он только увеличивает). "
               "Выберите разрешение рендера не выше выходного, чтобы его использовать."),
             Dim());
    }
    if (BbSettings::FixedRenderSession()) {
        std::snprintf(line, sizeof(line), T("Preset at startup: %s (changes after a restart)",
                                            "Пресет при запуске: %s (смена — после перезапуска)"),
                      BbSettings::PresetName(s.startup_preset));
        Note(line, Dim());
    }
    if (const char* problem = s.dlss_problem.load(); problem && s.upscaler == BbSettings::UpscalerDlss) {
        std::snprintf(line, sizeof(line), "DLSS: %s", problem);
        Note(line, ImGui::GetColorU32(ImVec4(1.0f, 0.55f, 0.35f, 1.0f)));
    }
    if (const char* problem = s.fsr4_problem.load()) {
        std::snprintf(line, sizeof(line), T("FSR 4 unavailable: %s", "FSR 4 недоступен: %s"), problem);
        Note(line, ImGui::GetColorU32(ImVec4(1.0f, 0.55f, 0.35f, 1.0f)));
    }
    // Frame generation (from Supermedo/bloodborne_pc PR #69 by FIocker): one choice for the
    // provider and the factor; applies after a restart.
    static const char* fg_modes[] = {"Off", "DLSS 2x", "DLSS 3x", "DLSS 4x", "FSR 2x"};
    const int fg_current = BbSettings::FrameGenerationChoice(s.frame_generation, s.fg_backend);
    Choice("frame_generation", T("Frame generation", "Генерация кадров"), fg_current,
           BbSettings::FrameGenerationChoices, [](int i) { return i == 0 ? T("Off", "Выкл") : fg_modes[i]; },
           [](int) { return true; },
           T("Generated frames between rendered ones: smoother motion on a high refresh display, "
             "slightly more input lag. DLSS needs an RTX 40/50 GPU and nvngx_dlssg.dll; FSR runs "
             "on most GPUs (2x). The HUD is copied from the rendered frame. Applies after "
             "restarting the game.",
             "Сгенерированные кадры между отрисованными: плавнее на мониторе с высокой частотой, "
             "чуть больше задержка ввода. DLSS нужна видеокарта RTX 40/50 и nvngx_dlssg.dll; FSR "
             "работает на большинстве видеокарт (2x). Интерфейс берётся из отрисованного кадра. "
             "Применяется после перезапуска игры."),
           true, [&](int i) {
               int factor = 0, backend = 0;
               BbSettings::FrameGenerationFromChoice(i, factor, backend);
               s.fg_backend = backend;
               Store(s.frame_generation, factor, true);
           });
    // NVIDIA Reflex (from Supermedo/bloodborne_pc PR #69 by FIocker): after a restart.
    static const char* reflex_modes[] = {"Off", "On", "On + Boost"};
    Choice("reflex", T("NVIDIA Reflex", "NVIDIA Reflex"), s.reflex, BbSettings::ReflexModeCount,
           [](int i) { return i == 0 ? T("Off", "Выкл") : reflex_modes[i]; }, [](int) { return true; },
           T("NVIDIA's low-latency mode (VK_NV_low_latency2): the driver paces the game so frames "
             "queue less. Boost also keeps GPU clocks up. NVIDIA GPUs with a current driver; "
             "elsewhere it stays off. Applies after restarting the game.",
             "Режим низкой задержки NVIDIA (VK_NV_low_latency2): драйвер выравнивает темп игры, "
             "кадры меньше копятся в очереди. Boost также держит частоты видеокарты. Видеокарты "
             "NVIDIA со свежим драйвером; на других не включается. Применяется после перезапуска игры."),
           true, [&](int i) { Store(s.reflex, i, true); });
    if (s.startup_reflex != BbSettings::ReflexOff && !s.reflex_active) {
        Note(T("NVIDIA Reflex: not supported by this GPU or driver (off)",
               "NVIDIA Reflex: не поддерживается видеокартой или драйвером (выкл)"),
             ImGui::GetColorU32(ImVec4(1.0f, 0.55f, 0.35f, 1.0f)));
    }
    if (const char* problem = s.fg_problem.load(); problem && BbSettings::FrameGenerationStarted()) {
        std::snprintf(line, sizeof(line), T("Frame generation unavailable: %s",
                                            "Генерация кадров недоступна: %s"), problem);
        Note(line, ImGui::GetColorU32(ImVec4(1.0f, 0.55f, 0.35f, 1.0f)));
    } else if (s.fg_active) {
        std::snprintf(line, sizeof(line), T("Frame generation: %.0f presented / %.0f rendered per second",
                                            "Генерация кадров: %.0f показано / %.0f отрисовано в секунду"),
                      s.display_fps.load(), s.source_fps.load());
        Note(line, Dim());
    }
    HdrRows();
}

void EffectsTab() {
    auto& s = BbSettings::Get();
    // No "Lowest (2)": the game then draws nothing for models without a third detail level
    // (the Lecture Building's floor). No "Highest (-2)": the game then draws nothing for many
    // objects past their nearest detail distance (Central Yharnam's corpses popped in and out).
    const char* lods[] = {T("Game default", "Как в игре"), T("Lower (1)", "Ниже (1)")};
    static constexpr int lod_values[] = {0, 1};
    int lod_index = 0;
    for (int i = 0; i < 2; ++i) {
        if (lod_values[i] == s.model_lod) lod_index = i;
    }
    Choice("lod", T("Model detail", "Детализация моделей"), lod_index, 2, [&](int i) { return lods[i]; },
           [](int) { return true; },
           T("Level of detail of models (game patch). Applies after restarting the game.",
             "Детализация моделей (патч игры). Применяется после перезапуска игры."),
           true, [&](int i) { Store(s.model_lod, lod_values[i], true); });
    for (int e = 0; e < BbSettings::EffectCount; ++e) {
        const auto& effect = BbSettings::Effects[e];
        const char* hint =
            std::string_view(effect.key) == "debug_camera"
                ? T("Hold Cross and press L3 (keyboard: Space + Z). Applies after restarting.",
                    "Удерживайте Cross и нажмите L3 (клавиатура: Space + Z). После перезапуска.")
            : std::string_view(effect.key) == "debug_menu"
                ? T("Left side of the touchpad (Tab). Needs the adhoc folder from Nexus mod #253. "
                    "Applies after restarting.",
                    "Левая сторона тачпада (Tab). Нужна папка adhoc из мода Nexus #253. После "
                    "перезапуска.")
                : T("A game patch, applied when the game starts: after a restart. Motion blur and "
                    "shadows from dynamic lights cost the most GPU time.",
                    "Патч игры, применяется при запуске: после перезапуска. Размытие в движении и тени "
                    "от динамических источников нагружают видеочип сильнее всего.");
        Toggle(effect.key, T(effect.label, effect.label_ru), s.effects[e], hint);
    }
}

// Key binding rows (from Mrsuss60/bloodborne_pc_windows_port 90008b39 by Mrsuss60: select a row,
// then press a key or a mouse button; a key another input had moves here in exchange). They
// write the layout's bbport.ini lines (mkkey.<input>= in mouse & keyboard mode, key.<input>= in
// the classic one; src/pad_bindings.h) and runtime_pad.c reads them again at once. In mouse &
// keyboard mode the mouse buttons' built-in roles are listed too: a mouse button named in a line
// leaves the built-in scheme (Shift + that click no longer gives R2 / L1).
struct KeyRows {
    int layout = -1;     ///< 1 mouse & keyboard (mkkey.*), 0 classic (key.*); -1 not read
    int generation = -1; ///< BbSettings::BindingsGeneration of the read
    std::vector<int> keys[IN_COUNT]; ///< the lines' keys (or the defaults)
    int capturing = -1;  ///< the input waiting for a key or mouse button
    std::string message; ///< the last exchange ("X taken from ...")
};
KeyRows key_rows;

const char* KeyPrefix(int layout) {
    return layout ? "mkkey." : "key.";
}

void ReadKeyRows(int layout) {
    key_rows.layout = layout;
    key_rows.generation = BbSettings::BindingsGeneration();
    for (auto& k : key_rows.keys) {
        k.clear();
    }
    const KeyDefault* list = layout ? mk_keys : classic_keys;
    const size_t count = layout ? MK_KEY_COUNT : CLASSIC_KEY_COUNT;
    for (size_t i = 0; i < count; ++i) {
        key_rows.keys[list[i].input].push_back(list[i].key);
    }
    const std::string prefix = KeyPrefix(layout);
    for (const auto& [key, value] : BbSettings::ReadLines(prefix)) {
        const std::string name = key.substr(prefix.size());
        for (int input = 0; input < IN_COUNT; ++input) {
            if (name != input_names[input]) {
                continue;
            }
            auto& keys = key_rows.keys[input];
            keys.clear();
            size_t at = 0;
            while (at <= value.size()) {
                size_t end = value.find(',', at);
                if (end == std::string::npos) {
                    end = value.size();
                }
                std::string part = value.substr(at, end - at);
                part.erase(part.find_last_not_of(' ') + 1);
                part.erase(0, part.find_first_not_of(' '));
                const int code = part.empty() ? -1 : key_from_name(part.c_str());
                if (code >= 0 && keys.size() < 4) {
                    keys.push_back(code);
                }
                at = end + 1;
            }
        }
    }
}

/// Mouse & keyboard mode: the mouse buttons' built-in roles (runtime_pad.c sample_mouse) while no
/// line names that button.
std::vector<int> BuiltInMouse(int input) {
    std::vector<int> out;
    if (key_rows.layout != 1) {
        return out;
    }
    static constexpr struct {
        int input, button;
    } roles[] = {{IN_R1, SDL_BUTTON_LEFT}, {IN_L2, SDL_BUTTON_RIGHT}, {IN_R3, SDL_BUTTON_MIDDLE},
                 {IN_L1, SDL_BUTTON_X1}, {IN_R2, SDL_BUTTON_X2}};
    for (const auto& role : roles) {
        if (role.input != input) {
            continue;
        }
        const int code = MOUSE_KEY + role.button;
        bool named = false;
        for (const auto& keys : key_rows.keys) {
            named |= std::find(keys.begin(), keys.end(), code) != keys.end();
        }
        if (!named) {
            out.push_back(code);
        }
    }
    return out;
}

std::string KeyList(int input) {
    std::string text;
    for (const int code : key_rows.keys[input]) {
        text += (text.empty() ? "" : " / ") + std::string(key_name(code));
    }
    for (const int code : BuiltInMouse(input)) {
        text += (text.empty() ? "" : " / ") + std::string(key_name(code));
    }
    return text.empty() ? T("(none)", "(нет)") : text;
}

const char* InputLabel(int input) {
    return T(input_labels[input][0], input_labels[input][1]);
}

/// Binds `code` alone to the capturing input (code < 0: unbinds it). An input that had the code
/// gets the capturing input's first key instead when nothing else is left on it.
void AssignKey(int code) {
    const int input = key_rows.capturing;
    key_rows.capturing = -1;
    if (input < 0 || key_rows.layout < 0) {
        return;
    }
    std::vector<int> old = key_rows.keys[input];
    for (const int builtin : BuiltInMouse(input)) {
        old.push_back(builtin);
    }
    std::vector<int> changed{input};
    key_rows.message.clear();
    if (code >= 0) {
        for (int other = 0; other < IN_COUNT; ++other) {
            if (other == input) {
                continue;
            }
            auto& keys = key_rows.keys[other];
            const std::vector<int> builtin = BuiltInMouse(other);
            const bool listed = std::find(keys.begin(), keys.end(), code) != keys.end();
            if (!listed && std::find(builtin.begin(), builtin.end(), code) == builtin.end()) {
                continue;
            }
            keys.erase(std::remove(keys.begin(), keys.end(), code), keys.end());
            bool other_mouse_left = false;
            for (const int b : builtin) {
                other_mouse_left |= b != code;
            }
            if (keys.empty() && !other_mouse_left && !old.empty() && old[0] != code) {
                keys.push_back(old[0]);
                key_rows.message = std::string(key_name(code)) + T(" taken from ", " взята у: ") +
                                   InputLabel(other) + T("; it gets ", "; ей назначена ") +
                                   key_name(old[0]);
            } else {
                key_rows.message =
                    std::string(key_name(code)) + T(" taken from ", " взята у: ") + InputLabel(other);
            }
            changed.push_back(other);
        }
    }
    key_rows.keys[input].clear();
    if (code >= 0) {
        key_rows.keys[input].push_back(code);
    }
    std::vector<std::pair<std::string, std::optional<std::string>>> lines;
    for (const int i : changed) {
        std::string value;
        for (const int k : key_rows.keys[i]) {
            value += (value.empty() ? "" : ",") + std::string(key_name(k));
        }
        lines.emplace_back(KeyPrefix(key_rows.layout) + std::string(input_names[i]), value);
    }
    BbSettings::WriteBindingLines(lines);
    key_rows.generation = BbSettings::BindingsGeneration();
    std::printf("Overlay: %s%s = %s%s%s\n", KeyPrefix(key_rows.layout), input_names[input],
                code >= 0 ? key_name(code) : "(none)", key_rows.message.empty() ? "" : "; ",
                key_rows.message.c_str());
}

/// Back to the layout's defaults: its lines are removed.
void ResetKeys() {
    std::vector<std::pair<std::string, std::optional<std::string>>> lines;
    for (int i = 0; i < IN_COUNT; ++i) {
        lines.emplace_back(KeyPrefix(key_rows.layout) + std::string(input_names[i]), std::nullopt);
    }
    BbSettings::WriteBindingLines(lines);
    key_rows.capturing = -1;
    key_rows.message.clear();
    ReadKeyRows(key_rows.layout);
    std::printf("Overlay: %s* lines removed (default keys)\n", KeyPrefix(key_rows.layout));
}

/// Window thread (HandleEvent, imgui_mutex held): a key or button for the row waiting for one.
/// Esc (gamepad Circle) cancels, Delete unbinds; Insert, F9 and F11 stay the menu's.
bool KeyCaptureEvent(const SDL_Event& event) {
    if (key_rows.capturing < 0) {
        return false;
    }
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
        if (event.key.repeat) {
            return true;
        }
        if (event.key.key == SDLK_ESCAPE || event.key.key == SDLK_INSERT || event.key.key == SDLK_F9 ||
            event.key.key == SDLK_F11) {
            key_rows.capturing = -1;
        } else if (event.key.key == SDLK_DELETE) {
            AssignKey(-1);
        } else if (event.key.scancode > SDL_SCANCODE_UNKNOWN && event.key.scancode < SDL_SCANCODE_COUNT) {
            AssignKey(int(event.key.scancode));
        }
        return true;
    case SDL_EVENT_KEY_UP:
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (key_rows.layout == 1 && event.button.button >= SDL_BUTTON_LEFT &&
            event.button.button <= SDL_BUTTON_X2) {
            AssignKey(MOUSE_KEY + event.button.button);
            return true;
        }
        return false;
    case SDL_EVENT_MOUSE_WHEEL:
        if (key_rows.layout == 1 && event.wheel.y != 0.0f) {
            const float y = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y : event.wheel.y;
            AssignKey(y > 0.0f ? MOUSE_WHEEL_UP : MOUSE_WHEEL_DOWN);
            return true;
        }
        return false;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        if (event.gbutton.button == SDL_GAMEPAD_BUTTON_EAST) {
            key_rows.capturing = -1;
            return true;
        }
        return false;
    default:
        return false;
    }
}

void KeyBindingRows(bool mk) {
    const int layout = mk ? 1 : 0;
    if (key_rows.layout != layout || key_rows.generation != BbSettings::BindingsGeneration()) {
        key_rows.capturing = -1;
        ReadKeyRows(layout);
    }
    ImGui::Spacing();
    Note(mk ? T("Key bindings (mouse & keyboard): select a row, then press a key or a mouse button. "
                "Esc cancels, Delete unbinds.",
                "Клавиши (мышь и клавиатура): выберите строку и нажмите клавишу или кнопку мыши. "
                "Esc - отмена, Delete - снять назначение.")
            : T("Key bindings (classic layout): select a row, then press a key. Esc cancels, Delete "
                "unbinds.",
                "Клавиши (классическая раскладка): выберите строку и нажмите клавишу. Esc - отмена, "
                "Delete - снять назначение."),
         Gold(0.9f));
    if (!key_rows.message.empty()) {
        Note(key_rows.message.c_str(), Bright());
    }
    for (int input = 0; input < IN_COUNT; ++input) {
        char id[32];
        std::snprintf(id, sizeof(id), "bind_%s", input_names[input]);
        const std::string keys = key_rows.capturing == input
                                     ? std::string(T("< press a key >", "< нажмите клавишу >"))
                                     : KeyList(input);
        if (Row(id, InputLabel(input), keys.c_str(),
                T("Select, then press the new key or mouse button: it replaces this input's keys. A "
                  "key another input had moves here; that input gets this one's first key.",
                  "Выберите и нажмите новую клавишу или кнопку мыши: она заменит клавиши этого "
                  "действия. Клавиша другого действия переходит сюда, а оно получает первую "
                  "клавишу этого."),
                true) != 0) {
            key_rows.capturing = key_rows.capturing == input ? -1 : input;
        }
    }
    if (Row("bind_reset", T("Default keys", "Клавиши по умолчанию"), T("Reset", "Сбросить"),
            T("Removes this layout's key lines from bbport.ini: every input gets its default keys.",
              "Удаляет строки клавиш этой раскладки из bbport.ini: все действия получают клавиши по "
              "умолчанию."),
            true) != 0) {
        ResetKeys();
    }
}

// Mouse & keyboard controls (scheme from Mrsuss60/bloodborne_pc_windows_port; runtime_pad.c).
void ControlsTab() {
    auto& s = BbSettings::Get();
    Toggle("mk", T("Mouse & keyboard controls", "Управление мышью и клавиатурой"), s.mk_enabled,
           T("A PC layout: the mouse turns the camera and attacks, the keyboard moves, dodges and "
             "uses items (the list below). Off: the classic keyboard layout (WASD / IJKL), the mouse "
             "is not used. Applies at once; the launcher's setting is used at the next start.",
             "Раскладка для ПК: мышь поворачивает камеру и атакует, клавиатура — движение, "
             "уклонение и предметы (список ниже). Выкл: классическая раскладка (WASD / IJKL), мышь "
             "не используется. Действует сразу; при следующем запуске берётся настройка лаунчера."));
    const bool mk = s.mk_enabled;
    SliderRow("mk_sens_x", T("Mouse sensitivity (horizontal)", "Чувствительность мыши (по горизонтали)"),
              s.mk_sens_x, 0.1f, 5.0f, 0.1f,
              T("How fast the camera turns for a mouse movement.",
                "Скорость поворота камеры при движении мыши."),
              mk);
    SliderRow("mk_sens_y", T("Mouse sensitivity (vertical)", "Чувствительность мыши (по вертикали)"),
              s.mk_sens_y, 0.1f, 5.0f, 0.1f, nullptr, mk);
    Toggle("mk_invert_x", T("Invert horizontal look", "Инверсия по горизонтали"), s.mk_invert_x, nullptr, mk);
    Toggle("mk_invert_y", T("Invert vertical look", "Инверсия по вертикали"), s.mk_invert_y, nullptr, mk);
    SliderRow("mk_smoothing", T("Mouse smoothing", "Сглаживание мыши"), s.mk_smoothing, 0.0f, 0.8f, 0.05f,
              T("Averages the mouse movement over a few frames: smoother, slightly delayed.",
                "Усредняет движение мыши за несколько кадров: плавнее, с небольшой задержкой."),
              mk);
    SliderRow("mk_deadzone", T("Mouse dead zone", "Мёртвая зона мыши"), s.mk_deadzone, 0.0f, 0.2f, 0.01f,
              T("Mouse movement slower than this share of a full stick push is ignored (hand "
                "tremor).",
                "Движение мыши медленнее этой доли полного наклона стика не учитывается (дрожание "
                "руки)."),
              mk);
    Toggle("mk_wheel", T("Mouse wheel: quick items", "Колесо мыши: быстрые предметы"), s.mk_wheel,
           T("The wheel steps the quick items and tools (d-pad up / down), one press per notch.",
             "Колесо листает быстрые предметы и инструменты (крестовина вверх / вниз), одно нажатие "
             "на щелчок."),
           mk);
    Toggle("mk_direct_camera", T("Direct mouse camera", "Прямая камера мышью"), s.mk_direct_camera,
           T("The mouse turns the camera itself, 1:1 like a PC game, instead of through the right "
             "stick (no dead zone or turn curve). Imedved's Bloodborne mouse camera; the stick look "
             "is kept while locked on.",
             "Мышь поворачивает камеру напрямую, 1:1, а не через правый стик (без мёртвой зоны и "
             "кривой). Камера мыши imedved; при захвате цели работает стик."),
           mk);
    // Button prompts (bbport_button_prompts.h, from deadinside28/bloodborne_pc 72e5c8ce): live.
    Choice("prompt_style", T("Button prompts", "Подсказки кнопок"), s.prompt_style, BbSettings::PromptStyleCount,
           [](int i) {
               static const char* names[] = {"Auto", "PlayStation", "Xbox", "Nintendo", "Keyboard"};
               return i == 0 ? T("Auto", "Авто") : i == 4 ? T("Keyboard", "Клавиатура") : names[i];
           },
           [](int) { return true; },
           T("The icons in the game's prompts and key guides. Auto: keys after keyboard or mouse "
             "input, the gamepad's own buttons after gamepad input. Nintendo shows the letters of a "
             "Nintendo pad (the bottom button is B); what each button does stays the same.",
             "Значки в подсказках игры. Авто: клавиши после ввода с клавиатуры или мыши, кнопки "
             "геймпада после ввода с геймпада. Nintendo показывает буквы геймпада Nintendo (нижняя "
             "кнопка B); действия кнопок не меняются."),
           true, [&](int i) { Store(s.prompt_style, i, true); });
    ImGui::Spacing();
    Note(mk ? T("Default mouse: move = camera, left = R1, Shift + left = R2, right = L2, Shift + right = L1, "
                "middle = R3 (lock-on), side buttons = L1 / R2, wheel = d-pad up / down.  Keys: WASD move, Space = Circle "
                "(dodge), E / Enter = Cross, R = Square, X = Triangle, Q = R3, C / Z = L3, Esc / F1 = "
                "Options, Tab / G = touchpad, Backspace = right touchpad, arrows or 1 2 3 4 = d-pad, "
                "IJKL = camera.",
                "Мышь по умолчанию: движение — камера, левая — R1, Shift + левая — R2, правая — L2, Shift + правая "
                "— L1, средняя — R3 (захват цели), боковые — L1 / R2, колесо — крестовина вверх / вниз.  Клавиши: WASD — движение, "
                "Пробел — Круг (уклонение), E / Enter — Крест, R — Квадрат, X — Треугольник, Q — R3, "
                "C / Z — L3, Esc / F1 — Options, Tab / G — тачпад, Backspace — правая сторона тачпада, "
                "стрелки или 1 2 3 4 — крестовина, IJKL — камера.")
            : T("Classic layout (default keys): WASD move, IJKL camera, Space / Enter = Cross, Left Shift / Esc = "
                "Circle, E = Square, Q = Triangle, 1 / 3 = L1 / R1, R / F = L2 / R2, Z / C = L3 / R3, "
                "F1 / O = Options, Tab / G = touchpad, Backspace = right touchpad, arrows = d-pad.",
                "Классическая раскладка (по умолчанию): WASD — движение, IJKL — камера, Пробел / Enter — Крест, "
                "Левый Shift / Esc — Круг, E — Квадрат, Q — Треугольник, 1 / 3 — L1 / R1, R / F — "
                "L2 / R2, Z / C — L3 / R3, F1 / O — Options, Tab / G — тачпад, Backspace — правая сторона тачпада, стрелки — "
                "крестовина."),
         Dim());
    KeyBindingRows(mk);
}

void AdvancedTab() {
    auto& s = BbSettings::Get();
    const bool upscaler_on = s.upscaler != BbSettings::UpscalerOff;
    const bool taa = s.upscaler == BbSettings::UpscalerTaa;
    Toggle("reactive", T("Reactive mask", "Маска реактивности"), s.reactive,
           BbSettings::IsFsr4(s.upscaler)
               ? T("Marks transparent effects (haze, light, water, particles): there the FSR 4 "
                   "output is blended with the current frame so effects do not drag older frames.",
                   "Помечает прозрачные эффекты (дымку, свет, воду, частицы): там результат FSR 4 "
                   "смешивается с текущим кадром, и эффекты не тянут за собой прошлые кадры.")
               : T("Marks transparent effects so the upscaler relies less on older frames: fewer "
                   "trails, more shimmer under them.",
                   "Помечает прозрачные эффекты, чтобы апскейлер меньше опирался на прошлые кадры: "
                   "меньше шлейфов, больше дрожания под ними."),
           upscaler_on && !taa);
    const bool mask = upscaler_on && !taa && s.reactive;
    SliderRow("reactive_scale", T("Mask scale", "Масштаб маски"), s.reactive_scale, 0.0f, 4.0f, 0.1f,
              nullptr, mask);
    SliderRow("reactive_threshold", T("Mask threshold", "Порог маски"), s.reactive_threshold, 0.0f,
              1.0f, 0.05f, nullptr, mask);
    SliderRow("reactive_max", T("Mask maximum", "Максимум маски"), s.reactive_max, 0.0f, 1.0f, 0.05f,
              nullptr, mask);
    const char* views[] = {T("Off", "Выкл"), T("Reactive mask", "Маска реактивности"),
                           T("Motion vectors", "Векторы движения")};
    Choice("debug_view", T("Debug view", "Отладочный вид"), s.debug_view, BbSettings::DebugViewCount,
           [&](int i) { return views[i]; }, [](int) { return true; },
           T("Motion vectors: red/green = horizontal/vertical motion (8 px = full), blue = the pixel "
             "has an accurate object vector. A moving object with neither is a trail.",
             "Векторы движения: красный/зелёный — движение по горизонтали/вертикали (8 пикселей = "
             "полная яркость), синий — у пикселя точный вектор объекта. Движущийся предмет без того "
             "и другого даёт шлейф."),
           upscaler_on, [&](int i) { s.debug_view = i; });
    Toggle("async_shaders", T("Asynchronous shaders", "Асинхронные шейдеры"), s.async_shaders,
           T("On by default. A new pipeline of a pass drawn in each of the last 8 frames is "
             "compiled in the background instead of pausing the game: objects using it may be "
             "missing for a frame or two. Never applies to the final frame, the UI, loading "
             "screens or compute work.",
             "Новый шейдер компилируется в фоне, а не останавливает игру: объекты с ним могут "
             "пропасть на несколько кадров. Не касается итогового кадра, интерфейса и "
             "вычислительных шейдеров."));
    Toggle("gpl", T("Pipeline libraries (GPL)", "Библиотеки конвейеров (GPL)"), s.gpl,
           T("Builds pipelines from reusable parts (VK_EXT_graphics_pipeline_library): new "
             "combinations of known shaders link almost instantly, an optimized version replaces "
             "them in the background. Needs driver support. Applies after restarting the game.",
             "Собирает конвейеры из готовых частей (VK_EXT_graphics_pipeline_library): новые "
             "сочетания известных шейдеров связываются почти мгновенно, оптимизированная версия "
             "заменяет их в фоне. Нужна поддержка драйвером. Применяется после перезапуска игры."));
    Toggle("compile_indicator", T("Shader compilation indicator", "Индикатор компиляции шейдеров"),
           s.compile_indicator,
           T("Shows the progress of shader compilation in the bottom right corner.",
             "Показывает ход компиляции шейдеров в правом нижнем углу."));
}

// Serverless party co-op (docs/PARTY_COOP_PLAN.md, A7): what party_status.h's board holds, and
// the commands back to the director. Read-only lines are not rows (focus skips them).

/// A read-only "label   value" line in the row layout; the value is clipped to its column.
void Info(const char* label, const char* value, ImU32 value_color) {
    const float width = ImGui::GetContentRegionAvail().x, height = S(34.0f);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(width, height));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float mid_y = at.y + height * 0.5f;
    ImGui::PushFont(game_font, 0.0f);
    const ImVec2 label_size = ImGui::CalcTextSize(label);
    draw->AddText(ImVec2(at.x + S(28.0f), mid_y - label_size.y * 0.5f), Plain(0.8f), label);
    ImGui::PopFont();
    if (value && value[0]) {
        const float col0 = at.x + width * 0.56f, col1 = at.x + width - S(28.0f);
        const ImVec2 size = ImGui::CalcTextSize(value);
        const float x = size.x < col1 - col0 ? (col0 + col1 - size.x) * 0.5f : col0;
        draw->PushClipRect(ImVec2(col0, at.y), ImVec2(col1, at.y + height), true);
        draw->AddText(ImVec2(x, mid_y - size.y * 0.5f), value_color, value);
        draw->PopClipRect();
    }
}

/// An action row (Cross / Enter / click) with an optional text in the value column, no arrows.
bool ActionRow(const char* id, const char* label, const char* value, const char* hint, bool enabled) {
    const bool act = Row(id, label, nullptr, hint, enabled) != 0;
    if (value && value[0]) {
        const bool focused = enabled && state.focus == state.rows - 1;
        const ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
        const float col0 = r0.x + (r1.x - r0.x) * 0.56f, col1 = r1.x - S(28.0f);
        const ImVec2 size = ImGui::CalcTextSize(value);
        const float x = size.x < col1 - col0 ? (col0 + col1 - size.x) * 0.5f : col0;
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->PushClipRect(ImVec2(col0, r0.y), ImVec2(col1, r1.y), true);
        draw->AddText(ImVec2(x, (r0.y + r1.y - size.y) * 0.5f), !enabled ? Dim() : focused ? Bright() : Gold(0.9f),
                      value);
        draw->PopClipRect();
    }
    return enabled && act;
}

const char* PartyStateText(party::status::State st, party::status::Role role) {
    using party::status::State;
    switch (st) {
    case State::Off: return T("Not running", "Не запущена");
    case State::Starting: return T("Starting…", "Запуск…");
    case State::Hosting: return T("Hosting: waiting for hunters", "Хост: ожидание охотников");
    case State::Connecting: return T("Connecting to the host…", "Подключение к хосту…");
    case State::WaitingForWorld: return T("Waiting for the world to load", "Ожидание загрузки мира");
    case State::RingingBell:
        return role == party::status::Role::Host ? T("Ringing the Beckoning Bell", "Звон манящего колокола")
                                                 : T("Ringing the Small Resonant Bell",
                                                     "Звон малого резонирующего колокола");
    case State::Joined: return T("Together", "Вместе");
    case State::Travelling: return T("Travelling with the party…", "Переход вместе с группой…");
    case State::Reconnecting: return T("Connection lost: reconnecting…", "Связь потеряна: переподключение…");
    case State::Error: return T("Error", "Ошибка");
    }
    return "";
}

/// A user mark (F9 / the Party tab): the log line, the peers' marks, a toast with its number.
void MarkIssue(const char* origin) {
    const int n = coop::plog::UserMark(origin);
    char text[96];
    std::snprintf(text, sizeof(text), T("Issue mark %d written to the logs", "Метка %d записана в логи"), n);
    party::status::SetLastEvent(text);
}

struct PartyUiState {
    double copied_until = 0.0;       ///< "Copied" shown in the code row until then (ImGui time)
    double lan_copied_until = 0.0;   ///< the same for the host's LAN code row
    double leave_confirm_until = 0.0;
    double kick_confirm_until = 0.0;
    std::string kick_confirm;        ///< the member a second press kicks
};
PartyUiState party_ui;

void PartyTab() {
    namespace ps = party::status;
    const ps::Board b = ps::Snapshot();
    const bool on = b.enabled;
    const double now = ImGui::GetTime();
    const ImU32 warn = ImGui::GetColorU32(ImVec4(1.0f, 0.55f, 0.35f, 1.0f));
    char line[256];
    if (!on) {
        Note(T("Party mode is off — enable it in the launcher.",
               "Режим группы выключен — включите его в лаунчере."),
             warn);
        ImGui::Spacing();
    }

    // Status, role.
    std::snprintf(line, sizeof(line), b.detail.empty() ? "%s" : "%s  (%s)", PartyStateText(b.state, b.role),
                  b.detail.c_str());
    Info(T("Status", "Состояние"), line,
         b.state == ps::State::Error          ? warn
         : b.state == ps::State::Reconnecting ? ImGui::GetColorU32(ImVec4(0.95f, 0.72f, 0.35f, 1.0f))
         : on                                 ? Bright()
                                              : Dim());
    Info(T("Role", "Роль"),
         b.role == ps::Role::Host    ? T("Host (your world)", "Хост (ваш мир)")
         : b.role == ps::Role::Guest ? T("Guest", "Гость")
                                     : T("None", "Нет"),
         on ? Plain() : Dim());

    // Party code + copy. A host has two when it knows its public address: the Internet code
    // (friends elsewhere) and the LAN code (same network: most routers cannot loop back to their
    // own public address).
    const bool two_codes = b.role == ps::Role::Host && !b.lan_code.empty();
    const bool copied = now < party_ui.copied_until;
    const char* code_value = copied          ? T("Copied to clipboard", "Скопирован в буфер обмена")
                             : b.code.empty() ? T("not yet known", "ещё неизвестен")
                                              : b.code.c_str();
    if (ActionRow("party_copy",
                  two_codes ? T("Internet code (friends elsewhere): copy", "Код для интернета (друзья не рядом): копировать")
                            : T("Party code: copy", "Код группы: копировать"),
                  code_value,
                  b.role == ps::Role::Host
                      ? T("The code friends paste into the launcher or the title screen's Party page to join "
                          "you: your address, port and the party's secret. Share it only with them.",
                          "Код, который друзья вставляют в лаунчер или на странице Party титульного экрана, "
                          "чтобы присоединиться: ваш адрес, порт и секрет группы. Давайте его только им.")
                      : T("The code of the party you joined. Copies it to the clipboard.",
                          "Код группы, к которой вы присоединились. Копирует его в буфер обмена."),
                  on && !b.code.empty())) {
        if (ps::RequestCopyCode()) {
            party_ui.copied_until = now + 2.0;
        }
    }
    if (two_codes) {
        const char* lan_value = now < party_ui.lan_copied_until ? T("Copied to clipboard", "Скопирован в буфер обмена")
                                                                : b.lan_code.c_str();
        if (ActionRow("party_copy_lan", T("LAN code (same network): copy", "Код для локальной сети: копировать"),
                      lan_value,
                      T("For players on your own network (same router). The Internet code also finds you "
                        "there, a few seconds slower.",
                        "Для игроков в вашей сети (тот же роутер). Код для интернета тоже найдёт вас, "
                        "на несколько секунд дольше."),
                      on)) {
            if (ps::RequestCopyLanCode()) party_ui.lan_copied_until = now + 2.0;
        }
    }

    // F2: what the NAT means here and which path the link took (English only: built in party_nat).
    if (on && !b.network.empty()) {
        std::snprintf(line, sizeof(line), "%s: %s", T("Network", "Сеть"), b.network.c_str());
        Note(line, Dim());
    }

    // NAT path (party_rudp.h): a guest's own code for a host that cannot open its port, and the
    // host's way to add one (the clipboard: no text input needed).
    if (b.role == ps::Role::Guest && !b.guest_code.empty()) {
        if (ActionRow("party_guest_code", T("Your guest code: copy", "Ваш гостевой код: копировать"),
                      b.guest_code.c_str(),
                      T("Only needed when the host cannot open its port (no UPnP / port forward): send this "
                        "code to the host; it adds it and the game connects through both routers.",
                        "Нужен, только если хост не может открыть порт (нет UPnP / проброса): отправьте код "
                        "хосту, он добавит его, и игра соединится через оба роутера."),
                      on)) {
            ps::RequestCopyGuestCode();
        }
    }
    if (b.role == ps::Role::Guest) {
        // F2 (0.7.1, party_netroute.h): a member that may relay for us when the host is out of reach.
        if (ActionRow("party_relay_via", T("Join through a member (paste their guest code)",
                                           "Войти через участника (вставить его гостевой код)"),
                      nullptr,
                      T("When the host cannot be reached directly but another member can: copy that member's "
                        "guest code (BBG1-...) and press. The party then goes through that member (party "
                        "members only; it uses some of their upload).",
                        "Если до хоста не достучаться напрямую, а до другого участника можно: скопируйте его "
                        "гостевой код (BBG1-...) и нажмите. Связь пойдёт через этого участника (только для "
                        "группы; использует часть его исходящего канала)."),
                      on)) {
            ps::RequestAddRelayCodeFromClipboard();
        }
    }
    if (b.role == ps::Role::Host) {
        if (ActionRow("party_add_guest", T("Add a guest code (paste)", "Добавить гостевой код (вставить)"), nullptr,
                      T("Copy a guest's code (BBG1-...) and press: without an open port, the game then "
                        "reaches that guest through both routers (UDP hole punching).",
                        "Скопируйте код гостя (BBG1-...) и нажмите: без открытого порта игра свяжется с "
                        "гостем через оба роутера (UDP hole punching)."),
                      on)) {
            ps::RequestAddGuestCodeFromClipboard();
        }
    }

    // Members.
    ImGui::Spacing();
    Note(T("Hunters", "Охотники"), Gold(0.8f));
    if (b.members.empty()) {
        Info(on ? T("No one yet", "Пока никого") : "—", nullptr, Dim());
    }
    for (const ps::Member& m : b.members) {
        char who[96];
        std::snprintf(who, sizeof(who), "%s%s%s", m.name.c_str(), m.slot == 0 ? T("  (host)", "  (хост)") : "",
                      m.local ? T("  (you)", "  (вы)") : "");
        const char* where = !m.connected  ? T("Connection lost, slot kept", "Связь потеряна, место сохранено")
                            : m.slot == 0 ? T("Host's world", "Мир хоста")
                            : m.in_world  ? T("In the host's world", "В мире хоста")
                                          : T("In their own world", "В своём мире");
        int n = std::snprintf(line, sizeof(line), "%s", where);
        if (m.connected && !m.local && m.ping_ms >= 0 && n < int(sizeof(line))) {
            n += std::snprintf(line + n, sizeof(line) - n, T("  ·  %d ms", "  ·  %d мс"), m.ping_ms);
        }
        if (!m.area.empty() && n < int(sizeof(line))) {
            std::snprintf(line + n, sizeof(line) - n, "  ·  %s", m.area.c_str());
        }
        Info(who, line, !m.connected ? warn : m.in_world || m.slot == 0 ? Bright() : Plain());
    }

    // Actions.
    ImGui::Spacing();
    const bool active = on && b.role != ps::Role::Off;
    if (ActionRow("party_rejoin",
                  b.role == ps::Role::Host ? T("Summon the party again", "Призвать группу снова")
                                           : T("Rejoin the host", "Вернуться к хосту"),
                  nullptr,
                  b.role == ps::Role::Host
                      ? T("Rings the Beckoning Bell again for hunters who are waiting.",
                          "Снова звонит в манящий колокол для ожидающих охотников.")
                      : T("Reconnects to the host now and rings the bell to be summoned again.",
                          "Сейчас же переподключается к хосту и звонит в колокол, чтобы вас снова "
                          "призвали."),
                  active)) {
        ps::RequestRejoin();
    }
    const bool leave_armed = now < party_ui.leave_confirm_until;
    if (ActionRow("party_leave", T("Leave the party", "Покинуть группу"),
                  leave_armed ? T("Press again to leave", "Нажмите ещё раз") : nullptr,
                  b.role == ps::Role::Host
                      ? T("Ends the party: every guest returns to their own world.",
                          "Завершает группу: все гости возвращаются в свои миры.")
                      : T("Returns to your own world and stops rejoining the host.",
                          "Возвращает в ваш мир и прекращает возвращения к хосту."),
                  active)) {
        if (leave_armed) {
            party_ui.leave_confirm_until = 0.0;
            ps::RequestLeave();
        } else {
            party_ui.leave_confirm_until = now + 3.0;
        }
    }
    // Party logging (party_log.h): a marked line in this log and the peers' logs (also F9).
    if (ActionRow("party_mark", T("Mark an issue in the logs (F9)", "Отметить проблему в логах (F9)"), nullptr,
                  T("Writes \"=== USER MARK n ===\" into this game's log and asks the other players' games to do the "
                    "same, so a bug report can point at the moment it happened.",
                    "Записывает \"=== USER MARK n ===\" в лог этой игры и просит игры других игроков сделать то же, "
                    "чтобы в отчёте об ошибке был виден момент, когда она случилась."),
                  true)) {
        MarkIssue("Party tab");
    }
    if (b.role == ps::Role::Host) {
        for (const ps::Member& m : b.members) {
            if (m.local || m.slot == 0) {
                continue;
            }
            char id[48], label[96];
            std::snprintf(id, sizeof(id), "party_kick_%d", m.slot);
            std::snprintf(label, sizeof(label), T("Kick %s", "Исключить %s"), m.name.c_str());
            const bool armed = now < party_ui.kick_confirm_until && party_ui.kick_confirm == m.name;
            if (ActionRow(id, label, armed ? T("Press again to kick", "Нажмите ещё раз") : nullptr,
                          T("Sends this hunter back to their own world and frees the slot.",
                            "Отправляет охотника в его мир и освобождает место."),
                          on)) {
                if (armed) {
                    party_ui.kick_confirm.clear();
                    ps::KickMember(m.name);
                } else {
                    party_ui.kick_confirm = m.name;
                    party_ui.kick_confirm_until = now + 3.0;
                }
            }
        }
    }
    auto& s = BbSettings::Get();
    Toggle("party_toasts", T("Join / leave notices", "Уведомления о входе и выходе"), s.party_toasts,
           T("A short notice in a top corner when a hunter joins or leaves, or the party travels. "
             "Never takes input.",
             "Короткое уведомление в верхнем углу, когда охотник входит или выходит или группа "
             "перемещается. Не перехватывает ввод."),
           on);
    Toggle("party_hud", T("Party HUD", "Панель группы"), s.party_hud,
           T("Your companions' names and health in the bottom left corner, greyed while they are not "
             "in your world; on the same map the distance, a direction arrow and a name tag above them.",
             "Имена и здоровье спутников в левом нижнем углу (серым, пока они не в вашем мире); на той "
             "же карте - расстояние, стрелка направления и имя над спутником."),
           on);
    Toggle("party_name_tags", T("Party name tags", "Имена над спутниками"), s.party_name_tags,
           T("On: our name plate above companions replaces the game's (which hides behind any tree or "
             "corner). Off: the game's own plate, as in the original game.",
             "Вкл.: наша табличка с именем над спутниками вместо игровой (та прячется за любым деревом "
             "или углом). Выкл.: игровая табличка, как в оригинальной игре."),
           on);
    Toggle("party_glow_off", T("Companions without glow", "Спутники без свечения"), s.party_glow_off,
           T("On: the other hunters of the party look like normal hunters on your screen. Off: the game's "
             "blue, glowing phantom look. Only how you see them; they stay cooperators.",
             "Вкл.: другие охотники группы выглядят на вашем экране как обычные охотники. Выкл.: синее "
             "свечение фантомов, как в игре. Только вид у вас; они остаются союзниками."),
           on);
    Toggle("party_npc_talk", T("Share NPC dialogue", "Общие диалоги NPC"), s.party_npc_talk,
           T("See and hear the NPC conversations of companions near you: the game's subtitles, the NPC's "
             "voice, their choices and gifts. Range: System > My party: Story > NPC dialogue range.",
             "Видеть и слышать разговоры спутников с NPC рядом с вами: субтитры, голос NPC, их выбор и "
             "подарки. Дальность: Система > Правила группы > Дальность диалогов NPC."),
           on);
    Toggle("party_pings", T("Pings", "Метки"), s.party_pings,
           T("Hold the touchpad (Tab) and press R3 (Q), or press V: a marker where you look. Every party "
             "member sees it through walls with its distance, in your colour, for 10 seconds.",
             "Зажмите тачпад (Tab) и нажмите R3 (Q) или нажмите V: метка там, куда вы смотрите. Вся "
             "группа видит её сквозь стены с расстоянием, вашим цветом, 10 секунд."),
           on);
    Toggle("party_through_walls", T("Show party through walls", "Группа сквозь стены"), s.party_through_walls,
           T("A small diamond with the distance on every companion on your map, drawn over walls and "
             "trees, in the companion's colour.",
             "Маленький ромб с расстоянием на каждом спутнике на вашей карте, поверх стен и деревьев, "
             "цветом спутника."),
           on);
    Toggle("party_stain_marker", T("Bloodstain marker", "Метка пятна крови"), s.party_stain_marker,
           T("A red drop with \"Blood Echoes\" and the distance on your lost echoes' bloodstain in the "
             "host's world, drawn over walls. The bloodstain is there either way.",
             "Красная капля с \"Отголосками крови\" и расстоянием на пятне крови с вашими потерянными "
             "отголосками в мире хоста, поверх стен. Пятно есть в любом случае."),
           on);
    Toggle("party_rendezvous", T("Find each other via the public DHT", "Поиск друг друга через публичный DHT"),
           s.party_rendezvous,
           T("When a direct connection fails, your address is posted on the public BitTorrent DHT under a "
             "key only your party can compute, so the host finds you and punches through both routers. DHT "
             "nodes can see your IP, as with any torrent client. Both host and guest need it on. Off: send "
             "the host your guest code (Add a guest code), or use port forwarding / a VPN. From the next "
             "party start; the launcher's Party tab sets it at launch.",
             "Если прямое соединение не удалось, ваш адрес публикуется в публичном DHT BitTorrent под ключом, "
             "который может вычислить только ваша группа, и хост находит вас и пробивает оба роутера. Узлы "
             "DHT видят ваш IP, как у любого торрент-клиента. Нужно включить и хосту, и гостю. Выкл.: "
             "отправьте хосту гостевой код или используйте проброс порта / VPN. Со следующего запуска группы; "
             "при запуске значение берётся из вкладки Party лаунчера."),
           on);
    Toggle("party_member_relay", T("Relay for party members", "Ретранслятор для участников"), s.party_member_relay,
           T("A member that cannot reach the host joins through you (and you through a member when you cannot). "
             "Only party members (the party code and password are checked), at most 4, capped; it uses some "
             "upload (~3 KB/s per relayed player in play, at most 128 KB/s). Relaying follows this switch at once.",
             "Участник, который не достаёт до хоста, входит через вас (и вы через участника, если не достаёте). "
             "Только участники группы (проверяются код и пароль), не более 4, с ограничением; расходует часть "
             "исходящего канала (~3 КБ/с на игрока в игре, не более 128 КБ/с). Ретрансляция следует переключателю сразу."),
           on);
    Toggle("party_ipv6", T("IPv6 for the party", "IPv6 для группы"), s.party_ipv6,
           T("Lets players reach you over IPv6 (both PCs need it on; tried before any relay, helps behind "
             "carrier-grade NAT). A host without an open IPv4 port then puts its IPv6 address into the party "
             "code (a temporary privacy address where Windows has one: the code shows it and changes when "
             "Windows rotates it). Off by default. From the next party start.",
             "Если у обоих ПК есть глобальный адрес IPv6, группа пробует IPv6 до ретрансляции (без NAT; "
             "помогает за CGNAT провайдера). Хост без открытого порта IPv4 добавляет свой адрес IPv6 в код "
             "группы. Со следующего запуска группы."),
           on);
    // 0.7.4: the host's rule (a guest starts with the host's value: the join check).
    if (b.role == ps::Role::Guest) {
        Note(ps::HostRuleOn(ps::HostRule::GuestLamps)
                 ? T("Guests use lamps: on (the host's rule)", "Фонари гостям: вкл. (правило хоста)")
                 : T("Guests use lamps: off (the host decides)", "Фонари гостям: выкл. (решает хост)"),
             Dim());
    } else {
    Toggle("party_guest_lamps", T("Guests use lamps", "Фонари гостям"), s.party_guest_lamps,
           T("Guests may use lamps and the Dream's headstones; the host leads and the whole party "
             "travels. The host's rule: every player starts with the host's. After a restart.",
             "Гости могут пользоваться фонарями и надгробиями Сна; ведёт хост, группа перемещается "
             "вместе. Правило хоста: у всех как у хоста. После перезапуска."),
           on);
    }
    // Party rules (party_balance.h): the host's values count for the whole party.
    ImGui::Spacing();
    Note(T("Party rules (the host's count for everyone)", "Правила группы (действуют правила хоста)"), Dim());
    // guestrules (0.7.3): a guest sees the host's rot and scaling, read-only (its own bbport.ini
    // values stay for when it hosts).
    const bool rules_guest = b.role == ps::Role::Guest;
    if (rules_guest) {
        const auto rule_note = [&](const char* en, const char* ru, ps::HostRule rule) {
            std::snprintf(line, sizeof(line), "%s: %s", T(en, ru),
                          ps::HostRuleOn(rule) ? T("on (the host's rule)", "вкл. (правило хоста)")
                                               : T("off (the host decides)", "выкл. (решает хост)"));
            Note(line, Dim());
        };
        rule_note("Rot on death", "Гниль при смерти", ps::HostRule::Rot);
        rule_note("  Rot: a boss kill cleanses it", "  Гниль: смерть босса снимает", ps::HostRule::RotCleanseBoss);
        rule_note("  Rot: kill your killer", "  Гниль: убить своего убийцу", ps::HostRule::RotCleanseKiller);
        rule_note("  Rot: retrieve your Blood Echoes", "  Гниль: вернуть отголоски крови", ps::HostRule::RotCleanseEchoes);
    } else {
    Toggle("party_rot", T("Rot on death", "Гниль при смерти"), s.party_rot,
           T("Each death stacks one penalty (max HP or stamina -10 %, stamina costs +10 %, rally -15 % or "
             "item discovery -10; up to 5); resting at a lamp or levelling up cleanses it. Default: off.",
             "Каждая смерть добавляет штраф (здоровье или выносливость -10 %, траты выносливости +10 %, "
             "отыгрыш -15 % или находки -10; до 5 раз); отдых у фонаря или новый уровень снимает. По умолчанию: выкл."),
           on);
    // 0.7.2: what else cleanses rot (only while rot is on; the host's rules).
    Toggle("party_rot_cleanse_boss", T("  Rot: a boss kill cleanses it", "  Гниль: смерть босса снимает"),
           s.party_rot_cleanse_boss,
           T("A boss's death cleanses all the rot of every party member in the host's world, alive or dead. "
             "Default: on.",
             "Смерть босса снимает всю гниль у всех участников в мире хоста, живых и мёртвых. По умолчанию: вкл."),
           on);
    Toggle("party_rot_cleanse_killer", T("  Rot: kill your killer", "  Гниль: убить своего убийцу"),
           s.party_rot_cleanse_killer,
           T("When the enemy that killed you dies (anyone in the party may kill it), you lose the rot of that "
             "death. Not after a fall or a trap; a rest respawns the killer. Default: on.",
             "Когда враг, убивший вас, погибает (от руки любого участника), гниль той смерти уходит. Не после "
             "падения или ловушки; отдых возрождает убийцу. По умолчанию: вкл."),
           on);
    Toggle("party_rot_cleanse_echoes", T("  Rot: retrieve your Blood Echoes", "  Гниль: вернуть отголоски крови"),
           s.party_rot_cleanse_echoes,
           T("Picking up your bloodstain removes the rot of the death that dropped it (not twice if your killer "
             "already died). Default: on.",
             "Подобранное пятно крови снимает гниль смерти, оставившей его (не дважды, если убийца уже погиб). "
             "По умолчанию: вкл."),
           on);
    }
    // I1 friendly fire / PvP (party_pvp.h): the host's switch; a guest sees the host's rule.
    if (b.role == ps::Role::Guest) {
        Note(ps::FriendlyFire() ? T("Friendly fire / PvP: ON (the host's rule) - party members can hurt each other",
                                    "Огонь по своим / PvP: ВКЛ (правило хоста) - члены группы могут ранить друг друга")
                                : T("Friendly fire / PvP: off (the host decides)",
                                    "Огонь по своим / PvP: выкл. (решает хост)"),
             Dim());
    } else {
        Toggle("party_friendly_fire", T("Friendly fire / PvP", "Огонь по своим / PvP"), s.party_friendly_fire,
               T("Party members can hurt each other, both ways, like invaders (ERSC's Judicator's Rulebook). "
                 "A party kill is a normal party death; no Echoes or Insight for the killer. The host's "
                 "setting counts and every member gets a notice when it changes. Default: off.",
                 "Члены группы могут ранить друг друга, как захватчики. Убийство в группе - обычная смерть; "
                 "убийца не получает отголосков и озарения. Действует настройка хоста, все получают "
                 "уведомление при изменении. По умолчанию: выкл."),
               on);
    }
    // Statsaudit finding 5: shared lamps, the host's rule; a guest sees it.
    if (b.role == ps::Role::Guest) {
        Note(ps::ShareLamps() ? T("Shared lamps: on (the host's rule) - every lamp lit in the party is lit for you too",
                                  "Общие фонари: вкл. (правило хоста) - каждый фонарь группы горит и у вас")
                              : T("Shared lamps: off (the host decides) - your own lamps stay yours",
                                  "Общие фонари: выкл. (решает хост) - ваши фонари остаются вашими"),
             Dim());
    } else {
        Toggle("party_share_lamps", T("Shared lamps", "Общие фонари"), s.party_share_lamps,
               T("A lamp any party member lights in your world is lit for everyone, in each member's own save "
                 "too, and your lamps reach the guests' saves. The host's setting counts. Default: on.",
                 "Фонарь, зажжённый любым членом группы в вашем мире, горит у всех и в каждом сохранении, а "
                 "ваши фонари попадают в сохранения гостей. Действует настройка хоста. По умолчанию: вкл."),
               on);
    }
    // 0.7.2 (user decision 2026-10-21): a guest's own lamps in the host's world, the host's rule.
    if (b.role == ps::Role::Guest) {
        Note(ps::HostRuleOn(ps::HostRule::GuestsBringLamps)
                 ? T("Guests bring their lamps: on (the host's rule) - your lamps light up in the host's world",
                     "Фонари гостей: вкл. (правило хоста) - ваши фонари горят и в мире хоста")
                 : T("Guests bring their lamps: off (the host decides) - the host's world keeps its own lamps",
                     "Фонари гостей: выкл. (решает хост) - мир хоста хранит свои фонари"),
             Dim());
    } else {
        Toggle("party_guests_bring_lamps", T("Guests bring their lamps", "Фонари гостей"), s.party_guests_bring_lamps,
               T("A joining guest's lamps from its own world light up in yours too (needs Shared lamps). Off: "
                 "your world keeps its own lamps; any lamp a party member lights in it is saved for everyone. "
                 "The host's setting counts. Default: off.",
                 "Фонари из мира входящего гостя загораются и в вашем (нужны общие фонари). Выкл.: ваш мир "
                 "хранит свои фонари; фонарь, зажжённый в нём любым членом группы, сохраняется у всех. "
                 "Действует настройка хоста. По умолчанию: выкл."),
               on);
    }
    static const char* overhead_en[] = {"Nothing", "Name", "Name and level", "Name and health", "Name and deaths", "Name and ping"};
    static const char* overhead_ru[] = {"Ничего", "Имя", "Имя и уровень", "Имя и здоровье", "Имя и смерти", "Имя и пинг"};
    Choice("party_overhead", T("Above companions", "Над спутниками"), std::clamp(s.party_overhead.load(), 0, 5), 6,
           [](int i) { return T(overhead_en[i], overhead_ru[i]); }, [](int) { return true; },
           T("What shows above party members in view (only on your screen).",
             "Что показывать над спутниками в поле зрения (только у вас)."),
           on, [&](int i) { Store(s.party_overhead, i, true); });
    struct Scale {
        const char* id;
        const char* en;
        const char* ru;
        std::atomic<int>* value;
        const int* table;
        int kind;  // 0 HP, 1 damage, 2 poise
    };
    const Scale scales[] = {
        {"party_boss_hp", "Boss health", "Здоровье боссов", &s.party_boss_hp, coop::balance::kHpTable, 0},
        {"party_boss_damage", "Boss damage", "Урон боссов", &s.party_boss_damage, coop::balance::kDamageTable, 1},
        {"party_boss_poise", "Boss poise", "Стойкость боссов", &s.party_boss_poise, coop::balance::kPoiseTable, 2},
        {"party_enemy_hp", "Enemy health", "Здоровье врагов", &s.party_enemy_hp, coop::balance::kHpTable, 0},
        {"party_enemy_damage", "Enemy damage", "Урон врагов", &s.party_enemy_damage, coop::balance::kDamageTable, 1},
        {"party_enemy_poise", "Enemy poise", "Стойкость врагов", &s.party_enemy_poise, coop::balance::kPoiseTable, 2},
    };
    const bool ru = BbSettings::MenuText("en", "ru")[0] == 'r';
    static std::string scale_text[11];
    if (rules_guest) {
        int host[ps::kScalingValues];
        ps::HostScaling(host);
        for (int k = 0; k < ps::kScalingValues; ++k) {
            const Scale& sc = scales[k];
            const std::string v = sc.kind == 0   ? coop::balance::DescribeHp(host[k], ru)
                                  : sc.kind == 1 ? coop::balance::DescribeDamage(host[k], ru)
                                                 : coop::balance::DescribePoise(host[k], ru);
            std::snprintf(line, sizeof(line), "%s: %s %s", T(sc.en, sc.ru), v.c_str(),
                          T("(the host's)", "(хоста)"));
            Note(line, Dim());
        }
    }
    for (const Scale& sc : scales) {
        if (rules_guest) break;
        const int pos = coop::balance::Position(sc.table, sc.value->load());
        for (int i = 0; i < 11; ++i) {
            const int v = sc.table[i];
            scale_text[i] = sc.kind == 0   ? coop::balance::DescribeHp(v, ru)
                            : sc.kind == 1 ? coop::balance::DescribeDamage(v, ru)
                                           : coop::balance::DescribePoise(v, ru);
        }
        Choice(sc.id, T(sc.en, sc.ru), pos, 11, [](int i) { return scale_text[i].c_str(); }, [](int) { return true; },
               sc.kind == 1 ? T("Per extra hunter in the party; Bloodborne itself adds none.",
                                "За каждого охотника в группе; в самой игре прибавки нет.")
                            : T("Per extra hunter, in % of Bloodborne's own boss step (100 % = the game's; normal "
                                "enemies get none in the game). Poise starts at 0 %: the game's own.",
                                "За каждого охотника, в % от шага самой игры для боссов (100 % = как в игре; "
                                "обычные враги в игре не усиливаются). Стойкость от 0 %: как в игре."),
               on, [&](int i) { Store(*sc.value, sc.table[std::clamp(i, 0, 10)], true); });
    }
    if (!b.last_event.empty()) {
        ImGui::Spacing();
        std::snprintf(line, sizeof(line), T("Last: %s", "Последнее: %s"), b.last_event.c_str());
        Note(line, Dim());
    }
}

} // namespace Ui

void Menu() {
    using namespace Ui;
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 display = io.DisplaySize;
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), display, IM_COL32(0, 0, 0, 165));

    static const char* tab_en[] = {"Display", "Upscaler", "Game effects", "Controls", "Advanced", "Party"};
    static const char* tab_ru[] = {"Изображение", "Апскейлер", "Эффекты игры", "Управление", "Дополнительно",
                                   "Группа"};
    constexpr int tabs = 6;
    if (focus_request) {
        focus_request = false;
        state.focus_first = true;
    }
    const bool prev_tab = ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) ||
                          ImGui::IsKeyPressed(ImGuiKey_Q, false) ||
                          ImGui::IsKeyPressed(ImGuiKey_PageUp, false);
    const bool next_tab = ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) ||
                          ImGui::IsKeyPressed(ImGuiKey_E, false) ||
                          ImGui::IsKeyPressed(ImGuiKey_PageDown, false);
    if (prev_tab || next_tab) {
        state.tab = (state.tab + (next_tab ? 1 : tabs - 1)) % tabs;
        state.focus_first = true;
    }
    if (state.focus_first) {
        state.focus_first = false;
        state.focus = 0;
        state.scroll = true;
    }
    const auto pressed_any = [](std::initializer_list<ImGuiKey> keys) {
        for (const ImGuiKey key : keys) {
            if (ImGui::IsKeyPressed(key, true)) {
                return true;
            }
        }
        return false;
    };
    state.move = pressed_any({ImGuiKey_UpArrow, ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadLStickUp})       ? -1
                 : pressed_any({ImGuiKey_DownArrow, ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadLStickDown}) ? 1
                                                                                                          : 0;
    state.activate = ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
                     ImGui::IsKeyPressed(ImGuiKey_Space, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false);
    state.rows = 0;
    state.enabled.clear();

    const ImVec2 size(std::min(S(1180.0f), display.x - S(40.0f)), std::min(S(800.0f), display.y - S(40.0f)));
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.035f, 0.032f, 0.028f, 0.94f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(36.0f), S(24.0f)));
    ImGui::PushFont(serif_font, 21.0f);
    ImGui::Begin("##bbport_settings", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 win = ImGui::GetWindowPos();
    const float x0 = win.x + S(24.0f), x1 = win.x + size.x - S(24.0f);
    Ornament(draw, x0, x1, win.y + S(10.0f));
    Ornament(draw, x0, x1, win.y + size.y - S(10.0f));
    // The port's version, small in the top left corner (VERSION, gpu/CMakeLists.txt); the close
    // button is top right, the controls line bottom centre.
    {
#ifndef BBPORT_VERSION
#define BBPORT_VERSION "dev"
#endif
        const char* version = "bbport " BBPORT_VERSION;
        const ImVec2 text = ImGui::CalcTextSize(version);
        draw->AddText(ImVec2(x0, win.y + S(39.0f) - text.y * 0.5f), Dim(0.8f), version);
        // Every setting here is also a page of the game's own menu (bbport_game_menu_pages.cpp):
        // this overlay is the fallback and the debugging aid.
        ImGui::PushFont(sans_font, 14.0f);
        const char* where = T("All settings are also pages of the game's own System menu",
                              "Все настройки есть и в меню игры: раздел «Система»");
        draw->AddText(ImVec2(x0, win.y + S(39.0f) + text.y * 0.5f + S(4.0f)), Gold(0.55f), where);
        ImGui::PopFont();
    }

    // Title.
    ImGui::PushFont(game_font, 34.0f);
    const char* title = T("Settings", "Настройки");
    const float title_w = ImGui::CalcTextSize(title).x;
    ImGui::SetCursorPosX((size.x - title_w) * 0.5f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Bright()));
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    // Close (mouse), top right.
    {
        const char* close = "×";
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
        ImGui::SetCursorPos(ImVec2(size.x - S(64.0f), S(22.0f)));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Gold()));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::ColorConvertU32ToFloat4(Gold(0.2f)));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::ColorConvertU32ToFloat4(Gold(0.35f)));
        if (ImGui::Button(close, ImVec2(S(34.0f), S(34.0f)))) {
            SetOpen(false);
        }
        ImGui::PopStyleColor(4);
        ImGui::PopItemFlag();
    }

    // Tabs: L1 ... R1, the current one gold and underlined.
    ImGui::SetCursorPosY(S(76.0f));
    {
        const float row_y = ImGui::GetCursorScreenPos().y;
        float widths[tabs], total = 0.0f;
        const float gap = S(46.0f);
        for (int i = 0; i < tabs; ++i) {
            widths[i] = ImGui::CalcTextSize(T(tab_en[i], tab_ru[i])).x;
            total += widths[i] + (i ? gap : 0.0f);
        }
        float x = win.x + (size.x - total) * 0.5f;
        ImGui::PushFont(sans_font, 15.0f);
        const ImVec2 l1 = ImGui::CalcTextSize("L1 / Q");
        draw->AddText(ImVec2(x - gap - l1.x, row_y + S(4.0f)), Gold(0.6f), "L1 / Q");
        draw->AddText(ImVec2(x + total + gap, row_y + S(4.0f)), Gold(0.6f), "E / R1");
        ImGui::PopFont();
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
        for (int i = 0; i < tabs; ++i) {
            const char* name = T(tab_en[i], tab_ru[i]);
            ImGui::SetCursorScreenPos(ImVec2(x, row_y));
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("##tab", ImVec2(widths[i], S(32.0f)))) {
                state.tab = i;
                state.focus_first = true;
            }
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();
            const bool current = i == state.tab;
            draw->AddText(ImVec2(x, row_y), current ? Bright() : hovered ? Plain() : Dim(), name);
            if (current) {
                draw->AddLine(ImVec2(x - S(6.0f), row_y + S(31.0f)), ImVec2(x + widths[i] + S(6.0f), row_y + S(31.0f)),
                              Gold(0.9f), S(1.5f));
            }
            x += widths[i] + gap;
        }
        ImGui::PopItemFlag();
        ImGui::SetCursorScreenPos(ImVec2(win.x + S(36.0f), row_y + S(44.0f)));
    }
    draw->AddLine(ImVec2(x0 + S(20.0f), ImGui::GetCursorScreenPos().y),
                  ImVec2(x1 - S(20.0f), ImGui::GetCursorScreenPos().y), Gold(0.25f), S(1.0f));
    ImGui::Dummy(ImVec2(0, S(6.0f)));

    // Rows (scrolling when they do not fit), then the description and the controls.
    const float footer = S(150.0f);
    state.next_hint = nullptr;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, S(2.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, S(6.0f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, ImGui::ColorConvertU32ToFloat4(Gold(0.30f)));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, ImGui::ColorConvertU32ToFloat4(Gold(0.55f)));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, ImGui::ColorConvertU32ToFloat4(Gold(0.75f)));
    ImGui::BeginChild("##rows", ImVec2(0.0f, ImGui::GetContentRegionAvail().y - footer), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    switch (state.tab) {
    case 0: DisplayTab(); break;
    case 1: UpscalerTab(); break;
    case 2: EffectsTab(); break;
    case 3: ControlsTab(); break;
    case 4: AdvancedTab(); break;
    default: PartyTab(); break;
    }
    if (RestartNeeded()) {
        ImGui::Spacing();
        if (Row("restart", T("Apply and restart the game", "Применить и перезапустить игру"), nullptr,
                T("Some changes apply only when the game starts (game patches, resolution at "
                  "startup, motion vectors). The game restarts from the title screen; your "
                  "progress is in the save.",
                  "Часть изменений применяется только при запуске игры (патчи игры, разрешение при "
                  "запуске, векторы движения). Игра перезапустится с титульного экрана; прогресс — в "
                  "сохранении."),
                true) != 0) {
            BbSettings::Save();
            runtime_restart();
        }
    }
    // Upstream 0.51 (#74, #116): leaving the game from the pad or the keyboard. The second
    // press within 5 s quits, as the window's close button does (party: goodbye to the others).
    {
        static double quit_armed_at = -1.0;
        const double now = ImGui::GetTime();
        const bool armed = quit_armed_at >= 0.0 && now - quit_armed_at <= 5.0;
        ImGui::Spacing();
        if (Row("quit", T("Quit the game", "Выйти из игры"),
                armed ? T("Press again", "Нажмите ещё раз") : T("Quit", "Выйти"),
                T("Closes the game like the window's close button. The game saves as you play; "
                  "in a party the others are told you left.",
                  "Закрывает игру, как кнопка закрытия окна. Игра сохраняется по ходу; в группе "
                  "остальные узнают, что вы вышли."),
                true) != 0) {
            std::printf("Overlay: %s\n", armed ? "quit" : "quit armed (press again)");
            if (armed) {
                BbSettings::Save();
                bbgpu_request_quit();
            }
            quit_armed_at = armed ? -1.0 : now;
        }
    }
    // Up / down: the next enabled row (also off a row that became disabled).
    if (state.rows != 0) {
        int focus = std::clamp(state.focus, 0, state.rows - 1);
        const int dir = state.move != 0 ? state.move : 1;
        if (state.move != 0 || !state.enabled[focus]) {
            for (int n = state.move != 0 ? 1 : 0; n < state.rows; ++n) {
                const int i = ((focus + dir * n) % state.rows + state.rows) % state.rows;
                if (state.enabled[i]) {
                    if (i != state.focus) {
                        state.scroll = true;
                    }
                    focus = i;
                    break;
                }
            }
        }
        state.focus = focus;
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(2);
    state.hint = state.next_hint;

    // Description of the focused setting.
    const float desc_y = win.y + size.y - footer;
    draw->AddLine(ImVec2(x0 + S(20.0f), desc_y + S(6.0f)), ImVec2(x1 - S(20.0f), desc_y + S(6.0f)), Gold(0.25f),
                  S(1.0f));
    ImGui::SetCursorScreenPos(ImVec2(win.x + S(64.0f), desc_y + S(18.0f)));
    ImGui::PushFont(serif_font, 17.0f);
    ImGui::BeginGroup(); // the lines start where the first one does
    ImGui::PushTextWrapPos(size.x - S(64.0f));
    if (RestartNeeded()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.72f, 0.35f, 1.0f));
        ImGui::TextUnformatted(T("Some changes apply after restarting the game.",
                                 "Часть изменений применится после перезапуска игры."));
        ImGui::PopStyleColor();
    }
    if (state.hint) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Plain(0.85f)));
        ImGui::TextUnformatted(state.hint);
        ImGui::PopStyleColor();
    }
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    ImGui::PopFont();

    // Controls.
    ImGui::PushFont(sans_font, 15.0f);
    // bbport (Windows fork): L3+R3 opens the menu only with BB_OVERLAY_PAD=1 (pad_toggle).
    const char* controls =
        pad_toggle ? T("Enter / Cross: change     Left / Right: value     Q / E, L1 / R1: tabs     "
                       "Esc / Circle: close     Insert, L3 + R3: menu",
                       "Enter / Крест: изменить     Влево / Вправо: значение     Q / E, L1 / R1: "
                       "вкладки     Esc / Круг: закрыть     Insert, L3 + R3: меню")
                   : T("Enter / Cross: change     Left / Right: value     Q / E, L1 / R1: tabs     "
                       "Esc / Circle: close     Insert: menu",
                       "Enter / Крест: изменить     Влево / Вправо: значение     Q / E, L1 / R1: "
                       "вкладки     Esc / Круг: закрыть     Insert: меню");
    const ImVec2 cs = ImGui::CalcTextSize(controls);
    draw->AddText(ImVec2(win.x + (size.x - cs.x) * 0.5f, win.y + size.y - S(40.0f)), Gold(0.65f), controls);
    ImGui::PopFont();

    ImGui::End();
    ImGui::PopFont();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void FpsCounter() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - pad, viewport->WorkPos.y + pad),
        ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.5f);
    ImGui::Begin("##fps", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    const auto& s = BbSettings::Get();
    ImGui::Text(BbSettings::MenuText("%.0f FPS  %.1f ms  %s", "%.0f FPS  %.1f мс  %s"),
                frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f, frame_ms_avg,
                s.upscaler == BbSettings::UpscalerFsr3     ? "FSR 3.1"
                : s.upscaler == BbSettings::UpscalerFsr4   ? "FSR 4"
                : s.upscaler == BbSettings::UpscalerFsr411 ? "FSR 4.1.1"
                : s.upscaler == BbSettings::UpscalerTaa    ? "TAA"
                : s.upscaler == BbSettings::UpscalerDlss   ? "DLSS"
                                                           : "");
    if (s.fg_active) {
        // Frame generation: presented and rendered (source) frames per second.
        ImGui::Text(BbSettings::MenuText("FG %.0f / %.0f rendered", "FG %.0f / %.0f отрисовано"),
                    s.display_fps.load(), s.source_fps.load());
    }
    ImGui::End();
}

const char* UpscalerLabel() {
    const int upscaler = BbSettings::Get().upscaler;
    return upscaler == BbSettings::UpscalerFsr3     ? "FSR 3.1"
           : upscaler == BbSettings::UpscalerFsr4   ? "FSR 4"
           : upscaler == BbSettings::UpscalerFsr411 ? "FSR 4.1.1"
           : upscaler == BbSettings::UpscalerTaa    ? "TAA"
           : upscaler == BbSettings::UpscalerDlss   ? "DLSS"
                                                    : "";
}

/// The performance HUD (after Mrsuss60/bloodborne_pc_windows_port's HudOverlay): not
/// interactive, never "menu open" (CapturesInput stays false), in the chosen corner.
void PerformanceHud() {
    const auto& s = BbSettings::Get();
    Telemetry::Start();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    const float hud_scale = std::clamp(s.hud_scale.load(), 0.5f, 2.0f);
    const int position = std::clamp(s.hud_position.load(), 0, BbSettings::HudPositionCount - 1);
    const bool right = position == BbSettings::HudTopRight || position == BbSettings::HudBottomRight;
    const bool bottom = position == BbSettings::HudBottomLeft || position == BbSettings::HudBottomRight;
    // 0.6.2: bottom right, it lifts above the game's area-name banner while that may be up.
    float y = bottom ? viewport->WorkSize.y - pad : pad;
    if (bottom && right) {
        y = party::hud::PerfHudBottom(y, viewport->WorkSize.y, perf_hud_banner);
    }
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + (right ? viewport->WorkSize.x - pad : pad),
                                   viewport->WorkPos.y + y),
                            ImGuiCond_Always, ImVec2(right ? 1.0f : 0.0f, bottom ? 1.0f : 0.0f));
    ImGui::SetNextWindowBgAlpha(std::clamp(s.hud_opacity.load(), 0.1f, 1.0f));
    ImGui::PushFont(sans_font, 16.0f * hud_scale);
    ImGui::Begin("##hud", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
    const float fps = frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f;
    ImGui::TextColored(ImVec4(0.45f, 0.95f, 0.55f, 1.0f), "%.0f FPS", fps);
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.75f, 0.85f, 1.0f, 1.0f), BbSettings::MenuText("%.1f ms  %s", "%.1f мс  %s"),
                       frame_ms_avg, UpscalerLabel());
    if (s.fg_active) {
        ImGui::TextColored(ImVec4(0.75f, 0.85f, 1.0f, 1.0f),
                           BbSettings::MenuText("Frame generation: %.0f shown, %.0f rendered",
                                                "Генерация кадров: %.0f показано, %.0f отрисовано"),
                           s.display_fps.load(), s.source_fps.load());
    }
    if (s.hud_graph) {
        float peak = 0.0f;
        for (const float ms : frame_history) {
            peak = std::max(peak, ms);
        }
        // At least 33 ms high, so a steady 60 FPS line sits low and a spike stands out.
        const float top = std::max(33.4f, std::ceil(peak / 8.0f) * 8.0f);
        char overlay[32];
        std::snprintf(overlay, sizeof(overlay), "%.0f ms", top);
        ImGui::PushStyleColor(ImGuiCol_PlotLines, ImVec4(0.80f, 0.68f, 0.46f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 0.35f));
        ImGui::PlotLines("##frametimes", frame_history, FrameHistory, frame_history_next, overlay, 0.0f,
                         top, ImVec2(220.0f * base_scale * hud_scale, 40.0f * base_scale * hud_scale));
        ImGui::PopStyleColor(2);
    }
    if (s.hud_vram && Telemetry::vram_used_gb >= 0.0f) {
        if (Telemetry::vram_budget_gb > 0.0f) {
            ImGui::Text(BbSettings::MenuText("VRAM %.1f / %.1f GB", "Видеопамять %.1f / %.1f ГБ"),
                        Telemetry::vram_used_gb.load(), Telemetry::vram_budget_gb.load());
        } else {
            ImGui::Text(BbSettings::MenuText("VRAM %.1f GB", "Видеопамять %.1f ГБ"),
                        Telemetry::vram_used_gb.load());
        }
    }
    if (s.hud_gpu && Telemetry::gpu >= 0.0f) {
        ImGui::Text(BbSettings::MenuText("GPU %.0f%%", "Видеокарта %.0f%%"), Telemetry::gpu.load());
    }
    if (s.hud_cpu && Telemetry::cpu >= 0.0f) {
        ImGui::Text(BbSettings::MenuText("CPU %.0f%%", "Процессор %.0f%%"), Telemetry::cpu.load());
    }
    if (s.hud_ram && Telemetry::ram_total_gb > 0.0f) {
        ImGui::Text(BbSettings::MenuText("RAM %.1f / %.1f GB", "Память %.1f / %.1f ГБ"),
                    Telemetry::ram_used_gb.load(), Telemetry::ram_total_gb.load());
    }
    perf_hud_bottom_left = bottom && !right; // the party HUD stacks above it
    perf_hud_top = ImGui::GetWindowPos().y;
    ImGui::End();
    ImGui::PopFont();
}

bool CompileIndicatorShown() {
    BbCompileProgress::Phase phase{};
    int percent = 0;
    // Never over gameplay: only on the title/menus, loading screens and logos (no 3D scene).
    return BbSettings::Get().compile_indicator && BbFrameState::IsMenuOrLoading() &&
           BbCompileProgress::Shown(phase, percent);
}

void CompileIndicator() {
    BbCompileProgress::Phase phase{};
    int percent = 0;
    if (!BbSettings::Get().compile_indicator || !BbFrameState::IsMenuOrLoading() ||
        !BbCompileProgress::Shown(phase, percent)) {
        return;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    // Bottom right, or bottom left while the performance HUD has that corner.
    const auto& s = BbSettings::Get();
    const bool left = s.show_hud && s.hud_position == BbSettings::HudBottomRight;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + (left ? pad : viewport->WorkSize.x - pad),
                                   viewport->WorkPos.y + viewport->WorkSize.y - pad),
                            ImGuiCond_Always, ImVec2(left ? 0.0f : 1.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.5f);
    ImGui::Begin("##compile", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    if (phase == BbCompileProgress::Phase::Rebuild) {
        ImGui::Text(BbSettings::MenuText("Rebuilding shader cache for this GPU: %d%%",
                                         "Пересборка кэша шейдеров для этой видеокарты: %d%%"),
                    percent);
    } else {
        ImGui::Text(BbSettings::MenuText("Compiling shaders: %d%%", "Компиляция шейдеров: %d%%"),
                    percent);
    }
    ImGui::End();
}

namespace PartyHudDraw {

constexpr float Pi = 3.14159265f;

ImU32 Rgba(float r, float g, float b, float a) {
    return ImGui::GetColorU32(ImVec4(r, g, b, a));
}

/// The direction axes on the ground (x, z): the game camera's when it is fresh and stands near
/// the player (the same world space), else the player's facing.
struct Axes {
    float fx = 0, fz = 1, rx = 1, rz = 0;
    bool camera = false; // the camera's: name tags can be projected with it
};

Axes DirectionAxes(const party::hud::Board& b, const BbCamera::State& cam) {
    Axes a;
    if (cam.valid && b.me_have_pos) {
        const auto at = BbCamera::Position(cam);
        const float dx = at[0] - b.me_pos[0], dy = at[1] - b.me_pos[1], dz = at[2] - b.me_pos[2];
        // A third-person camera is a few metres from the player; far off = another space (a
        // cutscene camera, a map with its own origin): not trusted.
        if (dx * dx + dy * dy + dz * dz < 15.0f * 15.0f) {
            const auto& v = cam.view;
            const float sx = cam.proj[0] < 0.0f ? -1.0f : 1.0f; // screen right is view +x times sx
            float fx = v[8], fz = v[10], rx = v[0] * sx, rz = v[2] * sx;
            const float fl = std::sqrt(fx * fx + fz * fz), rl = std::sqrt(rx * rx + rz * rz);
            if (fl > 1e-3f && rl > 1e-3f) {
                a = {fx / fl, fz / fl, rx / rl, rz / rl, true};
                return a;
            }
        }
    }
    a.fx = std::sin(b.me_yaw);
    a.fz = std::cos(b.me_yaw);
    a.rx = std::cos(b.me_yaw);
    a.rz = -std::sin(b.me_yaw);
    return a;
}

/// An arrow (pointing up = straight ahead) turned by `angle` (clockwise = right).
void Arrow(ImDrawList* draw, ImVec2 c, float r, float angle, ImU32 fill, ImU32 edge) {
    const float s = std::sin(angle), co = std::cos(angle);
    const auto at = [&](float x, float y) { // (x right, y up) -> screen
        return ImVec2(c.x + (x * co + y * s) * r, c.y - (-x * s + y * co) * r);
    };
    const ImVec2 tip = at(0.0f, 1.0f), left = at(-0.72f, -0.8f), notch = at(0.0f, -0.35f),
                 right = at(0.72f, -0.8f);
    draw->AddTriangleFilled(tip, left, notch, fill);
    draw->AddTriangleFilled(tip, notch, right, fill);
    draw->AddLine(tip, left, edge, 1.0f);
    draw->AddLine(tip, right, edge, 1.0f);
}

/// A thin bar in the game's style: dark well, gold hairline frame, `fill` up to `frac`.
void Bar(ImDrawList* draw, ImVec2 p0, ImVec2 p1, float frac, ImU32 fill, float alpha) {
    draw->AddRectFilled(p0, p1, Rgba(0.04f, 0.03f, 0.03f, 0.80f * alpha));
    if (frac > 0.0f) {
        const float x = p0.x + (p1.x - p0.x) * std::clamp(frac, 0.0f, 1.0f);
        draw->AddRectFilled(p0, ImVec2(x, p1.y), fill);
    }
    draw->AddRect(ImVec2(p0.x - 1.0f, p0.y - 1.0f), ImVec2(p1.x + 1.0f, p1.y + 1.0f),
                  Rgba(0.80f, 0.68f, 0.46f, 0.45f * alpha));
}

const char* StateText(const party::hud::Companion& c) {
    using party::MemberState;
    if (!c.connected) return BbSettings::MenuText("Connection lost", "Связь потеряна");
    if (c.have_hp && !c.alive) return BbSettings::MenuText("Dead", "Мёртв");
    switch (c.state) {
    case MemberState::Title: return BbSettings::MenuText("Title screen", "Главное меню");
    case MemberState::Loading: return BbSettings::MenuText("Loading", "Загрузка");
    case MemberState::Prologue: return BbSettings::MenuText("Prologue", "Пролог");
    case MemberState::Joining: return BbSettings::MenuText("Joining", "Входит");
    case MemberState::Home: return BbSettings::MenuText("Own world", "В своём мире");
    case MemberState::InHostWorld: return BbSettings::MenuText("Host's world", "В мире хоста");
    case MemberState::Dead: return BbSettings::MenuText("Dead", "Мёртв");
    }
    return "";
}

} // namespace PartyHudDraw

/// BB_PARTY_HUD_LOG=1: why the panel shows or not, every 5 s (present thread, before Visible).
void PartyHudDiag() {
    static const bool hud_log = [] {
        const char* v = std::getenv("BB_PARTY_HUD_LOG");
        return v && v[0] && v[0] != '0';
    }();
    if (hud_log) {
        static std::int64_t next_log = 0;
        if (const std::int64_t now = SteadyMs(); now >= next_log) {
            next_log = now + 5000;
            std::printf("Party HUD: overlay: party %d, setting %d, shown %d, menu/loading %d, camera %d, fps %d\n",
                        int(party::status::Enabled()), int(BbSettings::Get().party_hud.load()),
                        int(party::hud::Shown()), int(BbFrameState::IsMenuOrLoading()),
                        int(BbCamera::Latest().valid), int(BbSettings::Get().show_fps.load()));
            // Where each companion's head projects (u, v across the scene; depth in metres).
            const BbCamera::State cam = BbCamera::Latest();
            party::hud::Read(&hud_board);
            for (int i = 0; cam.valid && i < hud_board.count; ++i) {
                const party::hud::Companion& c = hud_board.c[i];
                const float head[3] = {c.pos[0], c.pos[1] + 2.05f, c.pos[2]};
                float u = -1, v = -1, depth = -1;
                const bool in_front = c.have_pos &&
                                      party::hud::Project(cam.view.data(), cam.proj.data(), head, &u, &v, &depth);
                std::printf("Party HUD: overlay: %s %s u %.2f v %.2f depth %.1f\n", c.name,
                            in_front ? "in front" : "behind", u, v, depth);
            }
        }
    }
}

/// Party HUD (party_hud.h): one compact row per companion - name, a thin HP bar (stamina below
/// it), distance and a direction arrow on the same map - in the bottom left corner (above the
/// performance HUD when that has the corner). Companions outside this player's world are greyed.
/// Name tags above companions in view (the game camera, BB_PARTY_HUD_TAGS=0: off). Not
/// interactive; no allocation (the board is a fixed-size copy, texts go to stack buffers).
void PartyHud() {
    using namespace PartyHudDraw;
    party_hud_top = -1.0f;
    const bool panel = PartyHudWanted(), tags = PartyTagsWanted();
    if ((!panel && !tags) || BbFrameState::IsMenuOrLoading()) {
        return;
    }
    party::hud::Read(&hud_board);
    const party::hud::Board& b = hud_board;
    if (!b.active || b.count <= 0) {
        return;
    }
    const BbCamera::State cam = BbCamera::Latest();
    const Axes axes = DirectionAxes(b, cam);
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float s = base_scale;
    if (panel) {
    const float pad = 12.0f * s;
    float bottom = perf_hud_bottom_left ? perf_hud_top - 6.0f * s
                                        : viewport->WorkPos.y + viewport->WorkSize.y - pad;
    const float width = 210.0f * s;
    // In a boss fight above the game's boss bar when it would cover the boss's name or bar (small
    // windows, 16:10 letterboxing, a tall panel; 21:9 keeps the corner: the bar is in the 16:9 middle).
    party::hud::HudRect boss;
    if (BossBarArea(viewport, &boss)) {
        const float x0 = viewport->WorkPos.x + pad;
        const float w = std::max(party_hud_size.x, width + 20.0f * s);
        const float h = party_hud_size.y > 0.0f ? party_hud_size.y : float(b.count) * 30.0f * s + 14.0f * s;
        bottom = party::hud::BottomClearOf(x0, x0 + w, bottom, h, boss, 6.0f * s);
    }
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + pad, bottom), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.42f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f * s, 7.0f * s));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f * s, 2.0f * s));
    ImGui::Begin("##party_hud", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    char right_text[48];
    for (int i = 0; i < b.count; ++i) {
        const party::hud::Companion& c = b.c[i];
        const bool here = c.same_world && c.connected;
        const float alpha = here ? 1.0f : 0.55f;
        if (i > 0) {
            ImGui::Dummy(ImVec2(0.0f, 5.0f * s));
        }
        // Name (serif, the game's cream; grey outside our world), state / distance on the right.
        const ImVec2 row = ImGui::GetCursorScreenPos();
        ImGui::PushFont(game_font, 17.0f);
        float mc[3];
        party::hud::MemberColour(c.slot, mc);  // the same colour as the member's tag (and I2's pings)
        ImGui::TextColored(here ? ImVec4(mc[0], mc[1], mc[2], 1.0f) : ImVec4(0.62f, 0.60f, 0.57f, alpha), "%s",
                           c.name);
        const float name_h = ImGui::GetItemRectSize().y;
        ImGui::PopFont();
        float right_edge = row.x + width;
        if (c.spectating && here) {
            // C5: dead in the boss fight, its camera following a member (its chr is carried along:
            // a distance would say "0.2 m" from that member).
            std::snprintf(right_text, sizeof right_text, "%s", BbSettings::MenuText("Spectating", "Наблюдает"));
        } else if (c.same_map) {
            const float d = party::hud::Distance(b.me_pos, c.pos);
            const float r = 5.5f * s;
            const ImVec2 centre(right_edge - r, row.y + name_h * 0.5f);
            Arrow(draw, centre,
                  r, party::hud::Bearing(b.me_pos, c.pos, axes.fx, axes.fz, axes.rx, axes.rz),
                  Rgba(0.93f, 0.84f, 0.62f, 0.95f * alpha), Rgba(0.10f, 0.07f, 0.04f, 0.9f * alpha));
            right_edge -= 2.0f * r + 5.0f * s;
            std::snprintf(right_text, sizeof right_text, d < 10.0f ? "%.1f m" : "%.0f m", d);
        } else {
            std::snprintf(right_text, sizeof right_text, "%s", here ? "" : StateText(c));
        }
        if (right_text[0]) {
            ImGui::PushFont(game_font, 14.0f);
            const ImVec2 ts = ImGui::CalcTextSize(right_text);
            draw->AddText(ImVec2(right_edge - ts.x, row.y + (name_h - ts.y) * 0.5f),
                          Rgba(0.80f, 0.76f, 0.68f, 0.9f * alpha), right_text);
            ImGui::PopFont();
        }
        // HP (the game's dark red), stamina (green) under it.
        const ImVec2 bar0 = ImGui::GetCursorScreenPos();
        const float hp_h = 4.0f * s, st_h = 2.0f * s;
        const float hp_frac = c.have_hp && c.max_hp > 0 ? float(c.hp) / float(c.max_hp) : 0.0f;
        Bar(draw, bar0, ImVec2(bar0.x + width, bar0.y + hp_h), hp_frac,
            here ? Rgba(0.62f, 0.07f, 0.06f, 1.0f) : Rgba(0.42f, 0.22f, 0.20f, alpha), alpha);
        float used = hp_h;
        if (c.have_stamina && c.max_stamina > 0) {
            const float y = bar0.y + hp_h + 2.0f * s;
            Bar(draw, ImVec2(bar0.x, y), ImVec2(bar0.x + width, y + st_h), float(c.stamina) / float(c.max_stamina),
                here ? Rgba(0.30f, 0.55f, 0.26f, 0.95f) : Rgba(0.30f, 0.40f, 0.28f, alpha), alpha);
            used += 2.0f * s + st_h;
        }
        ImGui::Dummy(ImVec2(width, used + 1.0f * s));
    }
    {   // C4: this player's own rot under the companions ("Rot 2/5: max HP -20 %"), while it has any.
        int kind[party::status::kRotKinds];
        party::status::Rot(kind);
        const bool ru = BbSettings::MenuText("en", "ru")[0] == 'r';
        const std::string head = coop::balance::DescribeRotHead(kind, ru);
        if (!head.empty()) {
            // Each effect in its own colour: max HP red as the HP bar, max stamina green as the
            // stamina bar, stamina costs amber, rally orange, item discovery pale blue.
            static const ImVec4 kEffect[party::status::kRotKinds] = {
                ImVec4(0.86f, 0.30f, 0.27f, 0.95f), ImVec4(0.46f, 0.76f, 0.40f, 0.95f),
                ImVec4(0.88f, 0.74f, 0.34f, 0.95f), ImVec4(0.96f, 0.56f, 0.24f, 0.95f),
                ImVec4(0.58f, 0.72f, 0.92f, 0.95f)};
            ImGui::Dummy(ImVec2(0.0f, 3.0f * s));
            ImGui::PushFont(game_font, 14.0f);
            const float x0 = ImGui::GetCursorPosX();
            // While a boss bar is up one short line ("Rot 2/5: HP, rally"): the full line can wrap to
            // three lines and the taller panel would cover the boss's name and the left end of its bar.
            const bool compact = coop::BossBarShown();
            ImGui::TextColored(ImVec4(0.74f, 0.52f, 0.40f, 0.95f), "%s",
                               compact ? coop::balance::DescribeRotShort(kind, ru).c_str() : head.c_str());
            bool first = true;
            for (int k = 0; k < party::status::kRotKinds && !compact; ++k) {
                const std::string e = coop::balance::DescribeRotEffect(k, kind[k], ru);
                if (e.empty()) continue;
                // The next effect on the same line while it fits the panel (after ", "), else at the
                // start of a new line without the separator.
                ImGui::SameLine(0.0f, 0.0f);
                std::string part = first ? e : ", " + e;
                if (ImGui::GetCursorPosX() + ImGui::CalcTextSize(part.c_str()).x > x0 + width) {
                    ImGui::NewLine();
                    part = e;
                }
                first = false;
                ImGui::TextColored(kEffect[k], "%s", part.c_str());
            }
            ImGui::PopFont();
        }
    }
    party_hud_top = ImGui::GetWindowPos().y;
    party_hud_size = ImGui::GetWindowSize();
    ImGui::End();
    ImGui::PopStyleVar(3);
    }

    // Name tags above the companions in view (only with the game camera in the player's space):
    // the overhead display (E2): name, plus level / health bar / deaths (party_overhead).
    if (!tags || !axes.camera) {
        return;
    }
    const int overhead = BbSettings::Get().party_overhead.load();
    const ImVec2 display = viewport->Size;
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    // Our own body on screen (its physics position now, the board's as the fallback): a member
    // behind it gets no tag (party_hud.h OwnBodyTagAlpha; walls still do not hide tags).
    party::hud::ScreenBox me_box;
    if (b.me_have_pos) {
        float me[3] = {b.me_pos[0], b.me_pos[1], b.me_pos[2]};
        coop::PartyHudChrPos(b.me_chr, b.me_pos, me);
        me_box = party::hud::BodyBox(cam.view.data(), cam.proj.data(), me, display.x, display.y,
                                     party::hud::kOwnBodyHalfWidth);
    }
    for (int i = 0; i < b.count; ++i) {
        const party::hud::Companion& c = b.c[i];
        // Only a live chr here, alive, on our map, from a fresh board (a dead / re-spawned / removed
        // member or a stalled game tick: no tag; user report "health bars floating in midair").
        const char* why = "";
        const bool show = party::hud::TagVisible(c, party::hud::NowMs() - b.published_ms, &why);
        {
            static char last_why[party::kMaxVitals][24];
            const int slot = c.slot >= 0 && c.slot < party::kMaxVitals ? c.slot : 0;
            if (std::strncmp(last_why[slot], why, sizeof last_why[slot]) != 0) {
                std::snprintf(last_why[slot], sizeof last_why[slot], "%s", why);
                if (why[0]) std::printf("Party HUD: tag %s hidden: %s\n", c.name, why);
                else std::printf("Party HUD: tag %s shown\n", c.name);
            }
        }
        if (!show) {
            continue;
        }
        // The chr's position now (its physics position, read this frame), not the board's: the
        // game tick publishes every 100 ms, so a tag placed from the board stepped at 10 Hz
        // behind a moving companion (user report 2026-10-14). The board's is the fallback.
        float pos[3] = {c.pos[0], c.pos[1], c.pos[2]};
        const bool live = coop::PartyHudChrPos(c.chr, c.pos, pos);
        const float d = party::hud::Distance(b.me_pos, pos);
        if (d > 80.0f) {
            continue;
        }
        const float head[3] = {pos[0], pos[1] + party::hud::kTagBarHeightM, pos[2]};
        // The reusable world-space marker projection (party_hud.h WorldToScreen; I2 pings reuse it).
        const party::hud::ScreenPoint sp =
            party::hud::WorldToScreen(cam.view.data(), cam.proj.data(), head, display.x, display.y, 0.02f);
        const float u = sp.x / display.x, v = sp.y / display.y;
        if (!sp.visible || v < 0.04f || v > 0.96f) {
            continue;
        }
        // Clear of the game's own HUD (bars and item slots top left, echoes bottom right).
        if ((u < 0.34f && v < 0.26f) || (u > 0.80f && v > 0.86f)) {
            continue;
        }
        const party::hud::ScreenBox member_box = party::hud::BodyBox(
            cam.view.data(), cam.proj.data(), pos, display.x, display.y, party::hud::kMemberBodyHalfWidth);
        const float cover = party::hud::OwnBodyTagAlpha(member_box, me_box);
        const int slot = c.slot >= 0 && c.slot < party::kMaxVitals ? c.slot : 0;
        {
            static bool behind_logged[party::kMaxVitals];
            if (behind_logged[slot] != (cover <= 0.0f)) {
                behind_logged[slot] = cover <= 0.0f;
                std::printf("Party HUD: tag %s %s (its box x %.0f..%.0f depth %.1f, ours x %.0f..%.0f depth %.1f)\n",
                            c.name,
                            cover <= 0.0f ? "hidden: behind this player's own character" : "out from behind this player",
                            member_box.x0, member_box.x1, member_box.depth, me_box.x0, me_box.x1, me_box.depth);
            }
        }
        {   // Test aids: BB_PARTY_HUD_TAGLOG=1 logs each tag's distance / camera depth / anchor once a
            // second (plate measurements; the own-body cover with both boxes), BB_PARTY_HUD_TAGLOG=frame
            // every frame with the chr position used and the camera's age; BB_PARTY_HUD_TAG_HIDE=1 draws
            // no tag (the game's plate alone).
            static const char* const taglog_env = std::getenv("BB_PARTY_HUD_TAGLOG");
            static const bool taglog = taglog_env != nullptr, every_frame = taglog && taglog_env[0] == 'f';
            static std::int64_t last_log[party::kMaxVitals] = {};
            if (taglog && (every_frame || party::hud::NowMs() - last_log[slot] >= 1000)) {
                last_log[slot] = party::hud::NowMs();
                std::printf("Party HUD: tagdiag %s d=%.2f depth=%.2f x=%.1f y=%.1f display=%.0fx%.0f t=%lld "
                            "chr=(%.3f %.3f %.3f) %s board=(%.3f %.3f %.3f) cam_age=%lld cover_alpha=%.2f "
                            "box=(%.0f..%.0f, %.0f..%.0f) own=(%.0f..%.0f, %.0f..%.0f)\n",
                            c.name, d, sp.depth, sp.x, sp.y, display.x, display.y,
                            static_cast<long long>(last_log[slot]), pos[0], pos[1], pos[2], live ? "live" : "board",
                            c.pos[0], c.pos[1], c.pos[2], static_cast<long long>(BbCamera::NowMs() - cam.at_ms), cover,
                            member_box.x0, member_box.x1, member_box.y0, member_box.y1, me_box.x0, me_box.x1,
                            me_box.y0, me_box.y1);
            }
            static const bool hide = std::getenv("BB_PARTY_HUD_TAG_HIDE") != nullptr;
            if (hide) {
                continue;
            }
        }
        if (cover <= 0.0f) {
            continue;
        }
        const float alpha = std::clamp((80.0f - d) / 30.0f, 0.0f, 1.0f) * 0.95f * cover;
        // Name, with the level ("Lv 42") or the deaths ("3 deaths, rot 2") after it.
        char label[96];
        const bool ru = BbSettings::MenuText("en", "ru")[0] == 'r';
        if (overhead == 2 && c.level > 0) {
            if (ru) std::snprintf(label, sizeof label, "%s  ур. %d", c.name, c.level);
            else std::snprintf(label, sizeof label, "%s  Lv %d", c.name, c.level);
        } else if (overhead == 4 && c.rot > 0) {
            if (ru) std::snprintf(label, sizeof label, "%s  смертей %d, гниль %d", c.name, c.deaths, c.rot);
            else std::snprintf(label, sizeof label, "%s  deaths %d, rot %d", c.name, c.deaths, c.rot);
        } else if (overhead == 5 && c.ping_ms > 0) {
            std::snprintf(label, sizeof label, ru ? "%s  %u мс" : "%s  %u ms", c.name, c.ping_ms);
        } else if (overhead == 4) {
            if (ru) std::snprintf(label, sizeof label, "%s  смертей %d", c.name, c.deaths);
            else std::snprintf(label, sizeof label, "%s  deaths %d", c.name, c.deaths);
        } else {
            std::snprintf(label, sizeof label, "%s", c.name);
        }
        // Laid out as the game's phantom plate (party_hud.h LayoutPlate: measured live; the game's
        // own plate is off in party mode, party_hud_game.cpp): the bar's top on the anchor with
        // light end caps, the name left-aligned above it, Baskerville in the plate's blue with a
        // dark outline and shadow. Bigger near, the game's size from 10 m camera depth on.
        static const float shift = [] {  // test: BB_PARTY_HUD_TAG_SHIFT=F moves ours F of the width right
            const char* e = std::getenv("BB_PARTY_HUD_TAG_SHIFT");
            return e ? static_cast<float>(std::atof(e)) : 0.0f;
        }();
        party::hud::PlateTunePoll();  // test: BB_PARTY_HUD_TUNE_FILE live parameters
        const party::hud::PlateParams& pp = party::hud::CurrentPlateParams();
        const party::hud::PlateLayout pl =
            party::hud::LayoutPlate(sp.x + shift * display.x, sp.y, display.x, display.y, sp.depth, pp);
        const bool with_bar = overhead == 3 && c.have_hp && c.max_hp > 0;
        // pl.font is the em size (as the plate was measured); ImGui sizes a font by its line
        // height (ascent - descent), smaller per em: convert through the cap height of 'H' (live
        // 2026-10-12: without this our name came out at 0.6 of the game's).
        static float size_per_em = 0.0f;
        if (size_per_em <= 0.0f) {
            ImGui::PushFont(game_font, 100.0f);
            const ImFontGlyph* g = ImGui::GetFontBaked()->FindGlyph('H');
            const float cap = g ? g->Y1 - g->Y0 : 0.0f;  // cap height in px at ImGui size 100
            ImGui::PopFont();
            size_per_em = cap > 1.0f ? 100.0f * party::hud::kPlateCapToFont / cap : 1.0f;
            std::printf("Party HUD: plate font size per em %.3f\n", size_per_em);
        }
        ImGui::PushFont(game_font, pl.font * size_per_em);
        const float ascent = ImGui::GetFontBaked()->Ascent;
        const ImVec2 at(std::round(pl.name_x), std::round(pl.name_baseline - ascent));
        const ImU32 dark = Rgba(0.02f, 0.03f, 0.03f, 0.55f * alpha);
        const float o = pl.outline;
        for (const ImVec2 d4 : {ImVec2(-o, 0), ImVec2(o, 0), ImVec2(0, -o), ImVec2(0, o)}) {
            bg->AddText(ImVec2(at.x + d4.x, at.y + d4.y), dark, label);
        }
        bg->AddText(ImVec2(at.x + o, at.y + o), Rgba(0.02f, 0.03f, 0.03f, 0.9f * alpha), label);  // drop shadow
        bg->AddText(at, Rgba(pp.name_rgb[0], pp.name_rgb[1], pp.name_rgb[2], alpha), label);  // the plate's blue
        ImGui::PopFont();
        if (with_bar) {
            // The plate's thin bar: red over burnt orange, the empty part dark, a dark rim, light end caps.
            const ImVec2 p0(pl.bar_x0, pl.bar_y0), p1(pl.bar_x1, pl.bar_y1);
            const float frac = std::clamp(float(c.hp) / float(c.max_hp), 0.0f, 1.0f);
            if (pp.rim > 0.0f) {
                bg->AddRectFilled(ImVec2(p0.x, p0.y - pp.rim), ImVec2(p1.x, p1.y + pp.rim),
                                  Rgba(0.05f, 0.04f, 0.04f, 0.6f * alpha));
            }
            bg->AddRectFilled(p0, p1, Rgba(0.16f, 0.09f, 0.09f, 0.85f * alpha));
            if (frac > 0.0f) {
                const ImU32 top = Rgba(pp.bar_top[0], pp.bar_top[1], pp.bar_top[2], alpha),
                            bottom = Rgba(pp.bar_bottom[0], pp.bar_bottom[1], pp.bar_bottom[2], alpha);
                bg->AddRectFilledMultiColor(p0, ImVec2(p0.x + (p1.x - p0.x) * frac, p1.y), top, top, bottom, bottom);
            }
            const ImU32 cap = Rgba(pp.cap_rgb[0], pp.cap_rgb[1], pp.cap_rgb[2], alpha),
                        glow = Rgba(0.55f, 0.62f, 0.70f, pp.glow * alpha);
            for (const float cx : {pl.cap_l, pl.cap_r}) {
                if (pp.glow > 0.0f) {
                    bg->AddRectFilled(ImVec2(cx - 1.0f, pl.cap_y0 - 1.0f), ImVec2(cx + pl.cap_w + 1.0f, pl.cap_y1 + 1.0f), glow);
                }
                bg->AddRectFilled(ImVec2(cx, pl.cap_y0), ImVec2(cx + pl.cap_w, pl.cap_y1), cap);
            }
        }
    }
}

/// Statsaudit finding 1: the guest's own bloodstain in the host's world (party_ping.h StainMark),
/// a blood-red drop through walls with "Blood Echoes N" and the distance (clamped to the screen
/// edge when off screen). Hidden under the game's menus, dialogs, cutscenes and loading screens.
/// An option, off by default (bbport.ini party_stain_marker; env BB_PARTY_STAIN_MARKER=0/1 over it).
bool PartyStainWanted() {
    static const char* const env = std::getenv("BB_PARTY_STAIN_MARKER");
    if (!(env && *env ? env[0] != '0' : BbSettings::Get().party_stain_marker.load())) {
        return false;
    }
    party::ping::StainMark m;
    return party::status::Enabled() && party::ping::GetStain(&m) && party::hud::Shown() && !party::hud::GameDialog();
}

void PartyStainMarker() {
    using namespace PartyHudDraw;
    if (!PartyStainWanted() || BbFrameState::IsMenuOrLoading()) {
        return;
    }
    party::ping::StainMark m;
    if (!party::ping::GetStain(&m)) {
        return;
    }
    party::hud::Board& b = hud_board;
    party::hud::Read(&b);
    if (!b.me_have_pos || party::hud::NowMs() - b.published_ms > 1000 || (m.map & 0xffff0000u) != (b.me_map & 0xffff0000u)) {
        return;
    }
    const BbCamera::State cam = BbCamera::Latest();
    if (!DirectionAxes(b, cam).camera) {
        return;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 display = viewport->Size;
    const float s = base_scale;
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    const float at3[3] = {m.pos[0], m.pos[1] + 0.4f, m.pos[2]};
    const party::ping::EdgePoint e =
        party::ping::ToScreenOrEdge(cam.view.data(), cam.proj.data(), at3, display.x, display.y, 28.0f * s);
    char dist[32], label[96];
    party::ping::FormatDistance(party::hud::Distance(b.me_pos, m.pos), dist, sizeof dist);
    std::snprintf(label, sizeof label, "%s %d  %s", BbSettings::MenuText("Blood Echoes", "Отголоски крови"), m.echoes,
                  dist);
    const ImU32 col = Rgba(0.78f, 0.10f, 0.10f, 0.95f), dark = Rgba(0.02f, 0.02f, 0.02f, 0.8f),
                text = Rgba(0.93f, 0.86f, 0.80f, 0.95f);
    ImGui::PushFont(game_font, 16.0f);
    const ImVec2 ts = ImGui::CalcTextSize(label);
    const auto outlined = [&](ImVec2 p, const char* str) {
        for (const ImVec2 d : {ImVec2(-1, 0), ImVec2(1, 0), ImVec2(0, -1), ImVec2(0, 1), ImVec2(1, 1)}) {
            bg->AddText(ImVec2(p.x + d.x * s, p.y + d.y * s), dark, str);
        }
        bg->AddText(p, text, str);
    };
    if (e.on_screen) {
        // A drop: a circle with a point on top, the label above it.
        const float r = 7.0f * s;
        const ImVec2 c(e.x, e.y);
        const ImVec2 tip(c.x, c.y - r * 2.2f), l(c.x - r * 0.75f, c.y - r * 0.6f), rr(c.x + r * 0.75f, c.y - r * 0.6f);
        bg->AddCircleFilled(c, r + 1.5f * s, dark, 24);
        bg->AddTriangleFilled(ImVec2(tip.x, tip.y - 1.5f * s), ImVec2(l.x - 1.5f * s, l.y), ImVec2(rr.x + 1.5f * s, rr.y), dark);
        bg->AddCircleFilled(c, r, col, 24);
        bg->AddTriangleFilled(tip, l, rr, col);
        outlined(ImVec2(std::round(c.x - ts.x * 0.5f), std::round(tip.y - ts.y - 4.0f * s)), label);
    } else {
        const float r = 11.0f * s, ca = std::cos(e.angle), sa = std::sin(e.angle);
        const ImVec2 tip(e.x + ca * r, e.y + sa * r);
        const ImVec2 lp(e.x - ca * r * 0.6f - sa * r * 0.8f, e.y - sa * r * 0.6f + ca * r * 0.8f);
        const ImVec2 rp(e.x - ca * r * 0.6f + sa * r * 0.8f, e.y - sa * r * 0.6f - ca * r * 0.8f);
        bg->AddTriangleFilled(tip, lp, rp, col);
        bg->AddTriangle(tip, lp, rp, dark, 1.5f * s);
        float lx = e.x - ca * (r + 6.0f * s) - ts.x * 0.5f - ca * ts.x * 0.5f;
        float ly = e.y - sa * (r + 6.0f * s) - ts.y * 0.5f - sa * ts.y * 0.5f;
        lx = std::clamp(lx, 4.0f * s, display.x - ts.x - 4.0f * s);
        ly = std::clamp(ly, 4.0f * s, display.y - ts.y - 4.0f * s);
        outlined(ImVec2(std::round(lx), std::round(ly)), label);
    }
    ImGui::PopFont();
}

/// I2 (party_ping.h): every live ping of a member in this player's world on this map, through
/// walls, in the pinger's colour with its name and distance (clamped to the screen edge with an
/// arrow when off screen), and with "Show party through walls" every companion on this map as a
/// small diamond with its distance. Under the game's menus, dialogs, cutscenes and loading screens
/// nothing is drawn (PartyPingsWanted, the caller's GameMenu check). No allocation.
void PartyPings() {
    using namespace PartyHudDraw;
    if (!PartyPingsWanted() || BbFrameState::IsMenuOrLoading()) {
        return;
    }
    party::hud::Board& b = hud_board;
    party::hud::Read(&b);
    if (!b.active || !b.me_have_pos || party::hud::NowMs() - b.published_ms > 1000) {
        return;
    }
    const BbCamera::State cam = BbCamera::Latest();
    if (!DirectionAxes(b, cam).camera) {
        return;  // no game camera in the player's space (a cutscene camera, a load)
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 display = viewport->Size;
    const float s = base_scale;
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    char text[64];
    const auto outlined = [&](ImVec2 at, ImU32 colour, float alpha, const char* str) {
        const ImU32 dark = Rgba(0.02f, 0.02f, 0.02f, 0.75f * alpha);
        for (const ImVec2 d : {ImVec2(-1, 0), ImVec2(1, 0), ImVec2(0, -1), ImVec2(0, 1), ImVec2(1, 1)}) {
            bg->AddText(ImVec2(at.x + d.x * s, at.y + d.y * s), dark, str);
        }
        bg->AddText(at, colour, str);
    };
    const party::hud::Companion* member_of[party::ping::kMaxSlots] = {};
    for (int i = 0; i < b.count; ++i) {
        if (b.c[i].slot >= 0 && b.c[i].slot < party::ping::kMaxSlots) member_of[b.c[i].slot] = &b.c[i];
    }

    // Companions through walls: a diamond at the chest with the distance under it.
    if (PartyThroughWalls()) {
        ImGui::PushFont(game_font, 14.0f);  // FontScaleMain scales it
        for (int i = 0; i < b.count; ++i) {
            const party::hud::Companion& c = b.c[i];
            const char* why = "";
            if (!party::hud::TagVisible(c, party::hud::NowMs() - b.published_ms, &why)) continue;
            float pos[3] = {c.pos[0], c.pos[1], c.pos[2]};
            coop::PartyHudChrPos(c.chr, c.pos, pos);
            const float d = party::hud::Distance(b.me_pos, pos);
            if (d < 2.0f || d > 400.0f) continue;
            const float chest[3] = {pos[0], pos[1] + 1.0f, pos[2]};
            const party::hud::ScreenPoint sp =
                party::hud::WorldToScreen(cam.view.data(), cam.proj.data(), chest, display.x, display.y, 0.02f);
            if (!sp.visible) continue;
            float rgb[3];
            party::hud::MemberColour(c.slot, rgb);
            const float a = 0.85f, r = 6.0f * s;
            const ImVec2 c0(sp.x, sp.y);
            const ImVec2 q[4] = {ImVec2(c0.x, c0.y - r), ImVec2(c0.x + r, c0.y), ImVec2(c0.x, c0.y + r),
                                 ImVec2(c0.x - r, c0.y)};
            bg->AddQuadFilled(q[0], q[1], q[2], q[3], Rgba(rgb[0], rgb[1], rgb[2], 0.35f * a));
            bg->AddQuad(q[0], q[1], q[2], q[3], Rgba(0.02f, 0.02f, 0.02f, 0.8f * a), 3.0f * s);
            bg->AddQuad(q[0], q[1], q[2], q[3], Rgba(rgb[0], rgb[1], rgb[2], a), 1.5f * s);
            party::ping::FormatDistance(d, text, sizeof text);
            const ImVec2 ts = ImGui::CalcTextSize(text);
            outlined(ImVec2(std::round(c0.x - ts.x * 0.5f), std::round(c0.y + r + 2.0f * s)),
                     Rgba(rgb[0], rgb[1], rgb[2], a), a, text);
        }
        ImGui::PopFont();
    }

    if (!coop::ping::Enabled()) {
        return;
    }
    party::ping::Marker marks[party::ping::kMaxSlots];
    const std::int64_t now = party::hud::NowMs();
    const int n = party::ping::Read(marks, now);
    for (int k = 0; k < n; ++k) {
        const party::ping::Marker& m = marks[k];
        if (m.map != b.me_map) continue;  // another map (or our own ping before a load)
        if (!m.mine) {
            const party::hud::Companion* c = member_of[m.slot];
            if (!c || !c->connected || !c->same_world) continue;  // not in this player's world
        }
        float rgb[3];
        party::hud::MemberColour(m.slot, rgb);
        const std::int64_t left = m.until_ms - now, age = party::ping::kLifeMs - left;
        const float a = party::ping::Alpha(left);
        const ImU32 col = Rgba(rgb[0], rgb[1], rgb[2], a), dark = Rgba(0.02f, 0.02f, 0.02f, 0.8f * a);
        const float inset = 28.0f * s;
        const party::ping::EdgePoint e =
            party::ping::ToScreenOrEdge(cam.view.data(), cam.proj.data(), m.pos, display.x, display.y, inset);
        const float d = party::hud::Distance(b.me_pos, m.pos);
        party::ping::FormatDistance(d, text, sizeof text);
        char label[64];
        std::snprintf(label, sizeof label, "%s  %s", m.name, text);
        ImGui::PushFont(game_font, 16.0f);
        const ImVec2 ts = ImGui::CalcTextSize(label);
        if (e.on_screen) {
            // A pin: a ring with a dot on the spot, a stem up to the label (name, distance).
            const float r = 8.0f * s;
            const ImVec2 at(e.x, e.y);
            if (age < 1200) {  // an attention pulse for the first second
                const float t = float(age) / 1200.0f;
                bg->AddCircle(at, r * (1.0f + 2.5f * t), Rgba(rgb[0], rgb[1], rgb[2], 0.8f * (1.0f - t) * a), 32,
                              2.0f * s);
            }
            bg->AddCircle(at, r, dark, 32, 4.0f * s);
            bg->AddCircle(at, r, col, 32, 2.0f * s);
            bg->AddCircleFilled(at, 2.5f * s, col, 12);
            const float stem = 22.0f * s;
            bg->AddLine(ImVec2(at.x, at.y - r), ImVec2(at.x, at.y - r - stem), dark, 3.0f * s);
            bg->AddLine(ImVec2(at.x, at.y - r), ImVec2(at.x, at.y - r - stem), col, 1.5f * s);
            outlined(ImVec2(std::round(at.x - ts.x * 0.5f), std::round(at.y - r - stem - ts.y - 2.0f * s)), col, a,
                     label);
        } else {
            // Off screen: an arrow on the edge pointing at it, the name and distance beside it.
            const float r = 11.0f * s, ca = std::cos(e.angle), sa = std::sin(e.angle);
            const ImVec2 tip(e.x + ca * r, e.y + sa * r);
            const ImVec2 left_pt(e.x - ca * r * 0.6f - sa * r * 0.8f, e.y - sa * r * 0.6f + ca * r * 0.8f);
            const ImVec2 right_pt(e.x - ca * r * 0.6f + sa * r * 0.8f, e.y - sa * r * 0.6f - ca * r * 0.8f);
            bg->AddTriangleFilled(tip, left_pt, right_pt, col);
            bg->AddTriangle(tip, left_pt, right_pt, dark, 1.5f * s);
            // The label on the screen side of the arrow.
            float lx = e.x - ca * (r + 6.0f * s) - ts.x * 0.5f - ca * ts.x * 0.5f;
            float ly = e.y - sa * (r + 6.0f * s) - ts.y * 0.5f - sa * ts.y * 0.5f;
            lx = std::clamp(lx, 4.0f * s, display.x - ts.x - 4.0f * s);
            ly = std::clamp(ly, 4.0f * s, display.y - ts.y - 4.0f * s);
            outlined(ImVec2(std::round(lx), std::round(ly)), col, a, label);
        }
        ImGui::PopFont();
    }
}

/// Party notices (join / leave / travel): the latest party_status event for ToastMs (by its length) in a bottom
/// corner, clear of the game's HUD (bottom left; bottom right while the performance HUD has the
/// bottom left), fading out. Not
/// interactive, never "menu open"; a newer event replaces the one shown.
void PartyToast() {
    if (!party::status::Enabled() || !BbSettings::Get().party_toasts) {
        return;
    }
    const std::int64_t now = SteadyMs();
    if (party::status::EventSeq() != toast_seen.load()) {
        const party::status::Board b = party::status::Snapshot();
        toast_seen = b.event_seq;
        toast_text = b.last_event;
        toast_until_ms = toast_text.empty() ? 0 : now + party::status::ToastMs(toast_text);
    }
    const std::int64_t left = toast_until_ms.load() - now;
    if (left <= 0 || toast_text.empty()) {
        return;
    }
    const float alpha = std::clamp(float(left) / 500.0f, 0.0f, 1.0f); // fades over the last 0.5 s
    const auto& s = BbSettings::Get();
    const bool right = s.show_hud && s.hud_position == BbSettings::HudBottomLeft;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    // Bottom left above the party HUD panel when that is drawn (the panel stacks above the
    // performance HUD instead, and the toast then keeps the bottom right).
    float bottom = !right && party_hud_top > 0.0f ? party_hud_top - 6.0f * base_scale
                                                   : viewport->WorkPos.y + viewport->WorkSize.y - pad;
    ImGui::PushFont(game_font, 19.0f);
    // A notice of several lines: each line centred in the box (the box is as wide as the widest).
    const auto measure = [](const std::string& s) { return ImGui::CalcTextSize(s.c_str()).x; };
    const std::vector<std::string> lines = party::hud::WrapNoticeLines(toast_text, 1e9f, measure);
    float w = 0.0f;
    for (const std::string& line : lines) w = std::max(w, measure(line));
    // In a boss fight above the game's boss bar when it would cover the boss's name or bar.
    party::hud::HudRect boss;
    if (BossBarArea(viewport, &boss)) {
        const float box_w = w + 32.0f * base_scale;
        const float box_h = float(lines.size()) * ImGui::GetTextLineHeightWithSpacing() + 24.0f * base_scale;
        const float x0 = right ? viewport->WorkPos.x + viewport->WorkSize.x - pad - box_w : viewport->WorkPos.x + pad;
        bottom = party::hud::BottomClearOf(x0, x0 + box_w, bottom, box_h, boss, 6.0f * base_scale);
    }
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + (right ? viewport->WorkSize.x - pad : pad), bottom),
                            ImGuiCond_Always, ImVec2(right ? 1.0f : 0.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.6f * alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f * base_scale, 8.0f * base_scale));
    ImGui::Begin("##party_toast", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float x0 = ImGui::GetCursorPosX();
    for (const std::string& line : lines) {
        ImGui::SetCursorPosX(party::hud::CenteredLineX(x0, w, measure(line)));
        ImGui::TextColored(ImVec4(0.96f, 0.88f, 0.70f, alpha), "%s", line.c_str());
    }
    // The game's gold underline, fading to both ends.
    const float y = ImGui::GetItemRectMax().y + 3.0f * base_scale;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 gold = ImGui::GetColorU32(ImVec4(0.80f, 0.68f, 0.46f, 0.85f * alpha));
    const ImU32 clear = ImGui::GetColorU32(ImVec4(0.80f, 0.68f, 0.46f, 0.0f));
    const float t = 1.2f * base_scale, mid = at.x + w * 0.5f;
    draw->AddRectFilledMultiColor(ImVec2(at.x, y), ImVec2(mid, y + t), clear, gold, gold, clear);
    draw->AddRectFilledMultiColor(ImVec2(mid, y), ImVec2(at.x + w, y + t), gold, clear, clear, gold);
    ImGui::Dummy(ImVec2(0.0f, 4.0f * base_scale));
    ImGui::End();
    ImGui::PopFont();
    ImGui::PopStyleVar(2);
}

/// The world warper's progress (world_warper.h): a small panel at the top middle, the warper's
/// own lines (phase and count, the area, what it does and the new pipelines). Not interactive.
void WarperPanel() {
    if (!coop::warper::Active()) {
        return;
    }
    const std::string text = coop::warper::StatusText();
    if (text.empty()) {
        return;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + 10.0f * base_scale),
                            ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f * base_scale, 6.0f * base_scale));
    ImGui::PushFont(game_font, 17.0f);
    ImGui::Begin("##world_warper", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
    std::size_t at = 0;
    bool first = true;
    while (at <= text.size()) {
        std::size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(at, end - at);
        at = end + 1;
        if (line.empty()) continue;
        ImGui::TextColored(first ? ImVec4(0.96f, 0.88f, 0.70f, 1.0f) : ImVec4(0.85f, 0.85f, 0.85f, 1.0f), "%s",
                           line.c_str());
        first = false;
    }
    ImGui::End();
    ImGui::PopFont();
    ImGui::PopStyleVar(2);
}

/// The party alert: a bordered box in the upper middle (clear of the game's own dialogs), its
/// first line as the title, then the text; fades out over its last second.
void PartyAlert() {
    if (!party::status::Enabled()) {
        return;
    }
    const std::int64_t now = SteadyMs();
    if (party::status::AlertSeq() != alert_seen.load()) {
        const party::status::Board b = party::status::Snapshot();
        alert_seen = b.alert_seq;
        alert_text = b.alert;
        alert_until_ms = alert_text.empty() ? 0 : now + AlertMs;
    }
    const std::int64_t left = alert_until_ms.load() - now;
    if (left <= 0 || alert_text.empty()) {
        return;
    }
    const float alpha = std::clamp(float(left) / 1000.0f, 0.0f, 1.0f);
    const std::size_t nl = alert_text.find('\n');
    const std::string title = alert_text.substr(0, nl);
    const std::string body = nl == std::string::npos ? std::string() : alert_text.substr(nl + 1);
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float width = std::min(viewport->WorkSize.x - 32.0f * base_scale, 760.0f * base_scale);
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.22f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.85f * alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24.0f * base_scale, 16.0f * base_scale));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.80f, 0.68f, 0.46f, 0.85f * alpha));
    ImGui::Begin("##party_alert", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoSavedSettings);
    // Every line centred in the box (party_hud.h WrapNoticeLines: wrapped at spaces to the box).
    const float x0 = ImGui::GetCursorPosX(), inner = ImGui::GetContentRegionAvail().x;
    const auto centred = [&](const std::string& text, float size, ImVec4 colour) {
        ImGui::PushFont(game_font, size);
        const auto measure = [](const std::string& s) { return ImGui::CalcTextSize(s.c_str()).x; };
        for (const std::string& line : party::hud::WrapNoticeLines(text, inner, measure)) {
            ImGui::SetCursorPosX(party::hud::CenteredLineX(x0, inner, measure(line)));
            ImGui::TextColored(colour, "%s", line.c_str());
        }
        ImGui::PopFont();
    };
    centred(title, 22.0f, ImVec4(0.96f, 0.88f, 0.70f, alpha));
    if (!body.empty()) {
        ImGui::Dummy(ImVec2(0.0f, 6.0f * base_scale));
        centred(body, 18.0f, ImVec4(0.90f, 0.90f, 0.88f, alpha));
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

/// While the game's full-screen menu is up the shown notice, alert and caption keep their remaining
/// time: their end moves on by each held frame (a notice that arrives under the menu waits too).
void HoldPartyNoticesInGameMenu() {
    const std::int64_t now = SteadyMs();
    if (!party::hud::GameMenu()) {
        menu_hold_last_ms = 0;
        return;
    }
    const std::int64_t dt = menu_hold_last_ms ? std::clamp<std::int64_t>(now - menu_hold_last_ms, 0, 1000) : 0;
    menu_hold_last_ms = now;
    // A notice / alert that came in under the menu starts its time when the menu closes.
    if (party::status::EventSeq() != toast_seen.load()) {
        const party::status::Board b = party::status::Snapshot();
        toast_seen = b.event_seq;
        toast_text = b.last_event;
        toast_until_ms = toast_text.empty() ? 0 : now + party::status::ToastMs(toast_text);
    } else if (toast_until_ms.load() > now) {
        toast_until_ms += dt;
    }
    if (party::status::AlertSeq() != alert_seen.load()) {
        const party::status::Board b = party::status::Snapshot();
        alert_seen = b.alert_seq;
        alert_text = b.alert;
        alert_until_ms = alert_text.empty() ? 0 : now + AlertMs;
    } else if (alert_until_ms.load() > now) {
        alert_until_ms += dt;
    }
    const std::int64_t cu = coop::npctalk::CurrentCaption().until_ms;
    if (caption_hold_key.load() != cu) {
        caption_hold_key = cu;
        caption_hold_ms = 0;
    }
    if (CaptionUntilMs(cu) > now) caption_hold_ms += dt;
}

/// A companion's NPC conversation (party_npctalk.h): "Hunter talking to Gilbert" and the talker's
/// choice / gift (or the line itself when the game's own subtitle could not be used) in the game's
/// Baskerville, centred above the game's subtitles; fades out over its last second. No input.
void PartyNpcCaption() {
    if (!party::status::Enabled()) {
        return;
    }
    const coop::npctalk::Caption c = coop::npctalk::CurrentCaption();
    const std::int64_t left = CaptionUntilMs(c.until_ms) - SteadyMs();
    if (left <= 0 || (c.header.empty() && c.line.empty())) {
        return;
    }
    const float alpha = std::clamp(float(left) / 1000.0f, 0.0f, 1.0f);
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float width = std::min(viewport->WorkSize.x - 32.0f * base_scale, 900.0f * base_scale);
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.78f),
                            ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(width, viewport->WorkSize.y * 0.5f));
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##party_npc_caption", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoSavedSettings);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const auto centred = [&](const std::string& text, float size, ImVec4 colour) {
        ImGui::PushFont(game_font, size);
        const float wrap = width - 24.0f * base_scale;
        const ImVec2 sz = ImGui::CalcTextSize(text.c_str(), nullptr, false, wrap);
        const float avail = std::max(ImGui::GetContentRegionAvail().x, sz.x);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (avail - sz.x) * 0.5f));
        const ImVec2 at = ImGui::GetCursorScreenPos();
        // A soft shadow like the game's subtitles (no box).
        draw->AddText(nullptr, 0.0f, ImVec2(at.x + 1.5f * base_scale, at.y + 1.5f * base_scale),
                      ImGui::GetColorU32(ImVec4(0, 0, 0, 0.85f * alpha)), text.c_str(), nullptr, wrap);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
        ImGui::TextColored(colour, "%s", text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
    };
    if (!c.header.empty()) {
        centred("[" + c.header + "]", 17.0f, ImVec4(0.80f, 0.68f, 0.46f, 0.95f * alpha));
    }
    if (!c.line.empty()) {
        centred(c.line, 21.0f, ImVec4(0.93f, 0.92f, 0.88f, alpha));
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void TextPrompt() {
    std::string title, text;
    {
        std::scoped_lock lock{prompt_mutex};
        title = prompt_title;
        text = prompt_text;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.9f);
    ImGui::Begin("##textprompt", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextUnformatted(title.c_str());
    ImGui::Separator();
    ImGui::Text("%s_", text.c_str());
    ImGui::Separator();
    ImGui::TextUnformatted("Keyboard: type, Backspace = delete, Enter = OK, Esc = cancel");
    ImGui::TextUnformatted("Controller: Cross (A) = OK, Circle (B) = cancel");
    ImGui::End();
}

/// BB_MENU_KEYS_FILE=<file> (scripted tests): tokens toggle up down left right enter back l1 r1,
/// consumed when the file appears (it is removed), one key press per frame.
void ScriptedKeys() {
    static const char* path = std::getenv("BB_MENU_KEYS_FILE");
    static std::chrono::steady_clock::time_point last_check{};
    static std::vector<std::string> queue;
    static bool release = false;
    static ImGuiKey held = ImGuiKey_None;
    if (!path) {
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    if (release) {
        io.AddKeyEvent(held, false);
        release = false;
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (queue.empty() && now - last_check > std::chrono::milliseconds(100)) {
        last_check = now;
        if (FILE* f = std::fopen(path, "r")) {
            char token[32];
            while (std::fscanf(f, "%31s", token) == 1) {
                queue.emplace_back(token);
            }
            std::fclose(f);
            std::remove(path);
        }
    }
    if (queue.empty()) {
        return;
    }
    const std::string token = queue.front();
    queue.erase(queue.begin());
    if (token == "toggle") {
        SetOpen(!menu_open);
        return;
    }
    held = token == "up" ? ImGuiKey_GamepadDpadUp : token == "down" ? ImGuiKey_GamepadDpadDown
         : token == "left" ? ImGuiKey_GamepadDpadLeft : token == "right" ? ImGuiKey_GamepadDpadRight
         : token == "enter" ? ImGuiKey_GamepadFaceDown : token == "l1" ? ImGuiKey_GamepadL1
         : token == "r1" ? ImGuiKey_GamepadR1 : token == "kdown" ? ImGuiKey_DownArrow
         : token == "kup" ? ImGuiKey_UpArrow : ImGuiKey_None;
    if (token == "back") {
        SetOpen(false);
        return;
    }
    if (held != ImGuiKey_None) {
        io.AddKeyEvent(held, true);
        release = true;
    }
}

} // namespace

void SetTextPrompt(bool active, const std::string& prompt, const std::string& text) {
    {
        std::scoped_lock lock{prompt_mutex};
        prompt_title = prompt;
        prompt_text = text;
    }
    prompt_active = active;
}

void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count) {
    if (const char* env = std::getenv("BB_OVERLAY")) {
        if (!std::strcmp(env, "0") || !std::strcmp(env, "off") || !std::strcmp(env, "false")) {
            std::printf("Overlay: disabled (BB_OVERLAY=0)\n");
            return;
        }
    }
    if (const char* env = std::getenv("BB_OVERLAY_PAD")) {
        pad_toggle = env[0] == '1';
    }
    std::scoped_lock lock{imgui_mutex};
    if (initialized) {
        return;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // window positions are not kept
    // The menu moves its own focus (Ui::State); ImGui only gets the keys.
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.BackendPlatformName = "bbport";

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.Colors[ImGuiCol_WindowBg].w = 0.92f;

    ImFontConfig font_config;
    font_config.FontDataOwnedByAtlas = false;
    sans_font = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(bb_font_ttf),
                                               int(bb_font_ttf_end - bb_font_ttf), 18.0f, &font_config);
    ImFontConfig serif_config;
    serif_config.FontDataOwnedByAtlas = false;
    serif_font = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(bb_serif_ttf),
                                                int(bb_serif_ttf_end - bb_serif_ttf), 21.0f, &serif_config);
    ImFontConfig game_config;
    game_config.FontDataOwnedByAtlas = false;
    game_font = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(bb_game_ttf),
                                               int(bb_game_ttf_end - bb_game_ttf), 21.0f, &game_config);
    if (game_font) {
        ImFontConfig merge;  // glyphs Libre Baskerville lacks (Cyrillic): DejaVu Serif
        merge.FontDataOwnedByAtlas = false;
        merge.MergeMode = true;
        io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(bb_serif_ttf), int(bb_serif_ttf_end - bb_serif_ttf),
                                       21.0f, &merge);
    } else {
        game_font = serif_font;
    }

    const vk::Instance vk_instance = instance.GetInstance();
    ImGui_ImplVulkan_LoadFunctions(
        instance.ApiVersion(),
        [](const char* name, void* user) {
            return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr(
                *static_cast<const vk::Instance*>(user), name);
        },
        const_cast<vk::Instance*>(&vk_instance));

    const VkFormat color_format = static_cast<VkFormat>(format);
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = instance.ApiVersion();
    info.Instance = vk_instance;
    info.PhysicalDevice = instance.GetPhysicalDevice();
    info.Device = instance.GetDevice();
    info.QueueFamily = instance.GetGraphicsQueueFamilyIndex();
    info.Queue = instance.GetGraphicsQueue();
    info.DescriptorPoolSize = 16;
    info.MinImageCount = std::max(image_count, 2u);
    info.ImageCount = std::max(image_count, 2u);
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color_format,
    };
    if (!ImGui_ImplVulkan_Init(&info)) {
        std::printf("Overlay: ImGui Vulkan backend init failed\n");
        ImGui::DestroyContext();
        return;
    }
    initialized = true;
    // Performance HUD telemetry: the game's GPU (NVML for NVIDIA, else its adapter's PDH counters).
    Telemetry::instance = &instance;
    {
        vk::PhysicalDeviceIDProperties id{};
        vk::PhysicalDeviceProperties2 properties{.pNext = &id};
        instance.GetPhysicalDevice().getProperties2(&properties);
        Telemetry::vendor_id = properties.properties.vendorID;
        Telemetry::device_luid_valid = id.deviceLUIDValid;
        std::memcpy(Telemetry::device_luid, id.deviceLUID.data(), sizeof(Telemetry::device_luid));
    }
    std::printf("Overlay: menu ready (Insert%s), performance HUD on F11\n", pad_toggle ? " or L3+R3" : "");
}

void UpdateTextInput(SDL_Window* window) {
    bool want = false;
    {
        std::scoped_lock lock{imgui_mutex};
        want = initialized && menu_open && ImGui::GetIO().WantTextInput;
    }
    if (want != SDL_TextInputActive(window)) {
        if (want) {
            SDL_StartTextInput(window);
        } else {
            SDL_StopTextInput(window);
        }
    }
}

bool HandleEvent(const SDL_Event& event) {
    std::scoped_lock lock{imgui_mutex};
    if (!initialized) {
        return false;
    }
    ImGuiIO& io = ImGui::GetIO();
    const bool is_open = menu_open;
    if (!is_open) {
        Ui::key_rows.capturing = -1; // a row waiting for a key when the menu closed
    } else if (Ui::KeyCaptureEvent(event)) {
        return true;
    }
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        if (down && !event.key.repeat &&
            (event.key.key == SDLK_INSERT || (is_open && event.key.key == SDLK_ESCAPE))) {
            SetOpen(event.key.key == SDLK_INSERT ? !is_open : false);
            return true;
        }
        // F11: the performance HUD (Mrsuss60/bloodborne_pc_windows_port). Their Shift+Tab is
        // not taken: Shift and Tab are game keys here (Circle + touchpad; in mouse & keyboard
        // mode the attack modifier + touchpad). F11 is bound to nothing in either layout.
        // F9: a user mark in the logs (party_log.h; works without a party too).
        if (event.key.key == SDLK_F9) {
            if (down && !event.key.repeat) {
                Ui::MarkIssue("F9");
            }
            return true;
        }
        if (event.key.key == SDLK_F11) {
            if (down && !event.key.repeat) {
                auto& s = BbSettings::Get();
                s.show_hud = !s.show_hud.load();
                BbSettings::Save();
            }
            return true;
        }
        // V: a ping (I2, party_ping.h). Not taken from the game: a key.* binding of V still works.
        if (!is_open && down && !event.key.repeat && event.key.key == SDLK_V && coop::ping::Enabled()) {
            coop::ping::Request("V key");
            return false;
        }
        // H: the host gathers the party (I5, party_gather.h; as System > Party > Gather the party).
        // Not taken from the game either; on a guest it is only the game's key.
        if (!is_open && down && !event.key.repeat && event.key.key == SDLK_H && coop::gather::HotkeyAvailable()) {
            coop::gather::RequestFromHotkey("H key");
            return false;
        }
        if (!is_open) {
            return false;
        }
        io.AddKeyEvent(ImGuiMod_Ctrl, (event.key.mod & SDL_KMOD_CTRL) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (event.key.mod & SDL_KMOD_SHIFT) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (event.key.mod & SDL_KMOD_ALT) != 0);
        if (const ImGuiKey key = KeyFromSdl(event.key.key); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        const bool down = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        const u8 button = event.gbutton.button;
        if (button == SDL_GAMEPAD_BUTTON_LEFT_STICK) {
            l3_down = down;
        } else if (button == SDL_GAMEPAD_BUTTON_RIGHT_STICK) {
            r3_down = down;
        }
        if (pad_toggle && down && l3_down && r3_down) {
            SetOpen(!is_open);
            return true;
        }
        if (!is_open) {
            return false;
        }
        if (down && button == SDL_GAMEPAD_BUTTON_EAST) { // Circle: back to the game
            SetOpen(false);
            return true;
        }
        if (const ImGuiKey key = KeyFromGamepad(button); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_TEXT_INPUT: {
        // Typed characters (Ctrl+click on a slider, a text field): key events alone erase but
        // do not type. SDL sends them while text input is on (UpdateTextInput).
        if (!is_open) {
            return false;
        }
        io.AddInputCharactersUTF8(event.text.text);
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        if (!is_open) {
            return false;
        }
        const float density = PixelDensity(event.motion.windowID);
        io.AddMousePosEvent(event.motion.x * density, event.motion.y * density);
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (!is_open) {
            return false;
        }
        const int button = event.button.button == SDL_BUTTON_LEFT    ? 0
                           : event.button.button == SDL_BUTTON_RIGHT  ? 1
                           : event.button.button == SDL_BUTTON_MIDDLE ? 2
                                                                      : -1;
        if (button >= 0) {
            io.AddMouseButtonEvent(button, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        }
        return true;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (!is_open) {
            return false;
        }
        io.AddMouseWheelEvent(event.wheel.x, event.wheel.y);
        return true;
    default:
        return false;
    }
}

/// The HDR calibration pattern's labels (the pattern itself: hdr_compose.frag).
bool HdrPatternShown() {
    const auto& s = BbSettings::Get();
    return s.hdr_calibration && s.hdr_active != 0;
}

void HdrPatternLabels() {
    const auto& s = BbSettings::Get();
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    const float left = BbHdr::PatternLeft * size.x, top = BbHdr::PatternTop * size.y;
    const float width = (BbHdr::PatternRight - BbHdr::PatternLeft) * size.x;
    const float height = (BbHdr::PatternBottom - BbHdr::PatternTop) * size.y;
    const float boxes = height * BbHdr::PatternSplitY;
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const auto label = [&](float x0, float x1, float y, const char* text) {
        const ImVec2 extent = ImGui::CalcTextSize(text);
        const ImVec2 at{left + (x0 + x1) * 0.5f * width - extent.x * 0.5f, y};
        draw->AddRectFilled({at.x - 6, at.y - 3}, {at.x + extent.x + 6, at.y + extent.y + 3},
                            IM_COL32(0, 0, 0, 200), 4.0f);
        draw->AddText(at, IM_COL32(235, 235, 235, 255), text);
    };
    char text[128];
    const float y = top + boxes + 4.0f;
    std::snprintf(text, sizeof(text), BbSettings::MenuText("Square: peak %.0f nits (should vanish)",
                                                           "Квадрат: пик %.0f нит (должен исчезнуть)"),
                  BbSettings::HdrPeakNits());
    label(BbHdr::PeakBoxLeft, BbHdr::PeakBoxRight, y, text);
    std::snprintf(text, sizeof(text), BbSettings::MenuText("Paper white %d nits", "Белый лист %d нит"),
                  s.hdr_paper_white.load());
    label(BbHdr::PaperBoxLeft, BbHdr::PaperBoxRight, y, text);
    std::snprintf(text, sizeof(text), BbSettings::MenuText("0 .. %.0f nits", "0 .. %.0f нит"),
                  BbSettings::HdrPeakNits());
    label(BbHdr::RampLeft, BbHdr::RampRight, y, text);
    label(0.0f, 1.0f, top + height + 4.0f,
          BbSettings::MenuText("The game's levels 0 .. 1 through the HDR curve",
                               "Уровни игры 0 .. 1 после кривой HDR"));
}

bool Visible() {
    return initialized &&
           (menu_open || prompt_active || BbSettings::Get().show_fps || BbSettings::Get().show_hud || HdrPatternShown() ||
            CompileIndicatorShown() || coop::warper::Active() || ToastWanted() || AlertWanted() || NpcCaptionWanted() || PartyHudWanted() || PartyTagsWanted() ||
            PartyPingsWanted() || PartyStainWanted());
}

bool MenuOpen() {
    return menu_open;
}

bool CapturesInput() {
    // The text dialog too: keys typed into it (Backspace is the touchpad) stay out of the game.
    return menu_open || prompt_active;
}

void RenderPrecompile(vk::CommandBuffer cmdbuf, vk::Extent2D extent) {
    std::scoped_lock lock{imgui_mutex};
    if (!initialized) {
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(extent.width), float(extent.height));
    io.DeltaTime = 1.0f / 30.0f;
    const float scale = std::max(float(extent.height) / 1080.0f, 0.75f);
    if (std::abs(scale - base_scale) > 0.01f) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(scale / base_scale);
        style.FontScaleMain = scale;
        base_scale = scale;
    }
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();

    const u32 total = BbCompileProgress::total.load();
    const u32 done = std::min(BbCompileProgress::done.load(), total);
    const float fraction = total ? float(done) / float(total) : 0.0f;
    const auto phase = BbCompileProgress::Phase(BbCompileProgress::phase.load());
    const int percent = std::min(99, int(fraction * 100.0f));
    char title[256];
    if (phase == BbCompileProgress::Phase::Rebuild) {
        std::snprintf(title, sizeof(title),
                      BbSettings::MenuText("Rebuilding shader cache for this GPU... %d%%",
                                           "Пересборка кэша шейдеров для этой видеокарты... %d%%"),
                      percent);
    } else if (BbCompileProgress::first_launch.load()) {
        std::snprintf(title, sizeof(title),
                      BbSettings::MenuText("Preparing shaders for this GPU (first launch)... %d%%",
                                           "Подготовка шейдеров для этой видеокарты (первый "
                                           "запуск)... %d%%"),
                      percent);
    } else {
        std::snprintf(title, sizeof(title),
                      BbSettings::MenuText("Compiling shaders... %d%%", "Компиляция шейдеров... %d%%"),
                      percent);
    }
    char detail[160];
    const double elapsed =
        double(BbCompileProgress::NowNs() - BbCompileProgress::batch_start_ns.load()) / 1e9;
    if (fraction > 0.03f && fraction < 1.0f && elapsed > 1.0) {
        const int eta = int(elapsed * (1.0 - fraction) / fraction + 0.5);
        std::snprintf(detail, sizeof(detail),
                      BbSettings::MenuText("%u / %u   about %d:%02d left   Esc: quit",
                                           "%u / %u   осталось около %d:%02d   Esc: выход"),
                      done, total, eta / 60, eta % 60);
    } else {
        std::snprintf(detail, sizeof(detail),
                      BbSettings::MenuText("%u / %u   Esc: quit", "%u / %u   Esc: выход"), done,
                      total);
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("##precompile", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
    const float width = viewport->WorkSize.x, height = viewport->WorkSize.y;
    const float bar_width = std::min(width * 0.6f, 900.0f * base_scale);
    if (serif_font) {
        ImGui::PushFont(serif_font, 30.0f);
    }
    const ImVec2 title_size = ImGui::CalcTextSize(title);
    ImGui::SetCursorPos(ImVec2((width - title_size.x) * 0.5f, height * 0.5f - title_size.y * 2.2f));
    ImGui::TextUnformatted(title);
    if (serif_font) {
        ImGui::PopFont();
    }
    ImGui::SetCursorPos(ImVec2((width - bar_width) * 0.5f, height * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.80f, 0.68f, 0.46f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.15f, 0.14f, 0.13f, 1.0f));
    ImGui::ProgressBar(fraction, ImVec2(bar_width, 10.0f * base_scale), "");
    ImGui::PopStyleColor(2);
    if (sans_font) {
        ImGui::PushFont(sans_font, 18.0f);
    }
    const ImVec2 detail_size = ImGui::CalcTextSize(detail);
    ImGui::SetCursorPos(ImVec2((width - detail_size.x) * 0.5f, height * 0.5f + 24.0f * base_scale));
    ImGui::TextDisabled("%s", detail);
    if (sans_font) {
        ImGui::PopFont();
    }
    ImGui::End();
    ImGui::Render();
    std::scoped_lock submit_lock{Vulkan::Scheduler::submit_mutex};
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmdbuf);
}

bool Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent, bool clear) {

    // Present interval for the FPS readout (measured also while nothing is drawn).
    const auto now = std::chrono::steady_clock::now();
    const float ms = std::chrono::duration<float, std::milli>(now - last_present).count();
    last_present = now;
    if (ms > 0.0f && ms < 1000.0f) {
        frame_ms_avg = frame_ms_avg == 0.0f ? ms : frame_ms_avg * 0.95f + ms * 0.05f;
        frame_history[frame_history_next] = ms;
        frame_history_next = (frame_history_next + 1) % FrameHistory;
    }
    Telemetry::wanted.store(initialized && BbSettings::Get().show_hud, std::memory_order_relaxed);
    if (std::getenv("BB_MENU_KEYS_FILE") && initialized) {
        std::scoped_lock lock{imgui_mutex};
        ScriptedKeys();
    }
    if (initialized) {
        PartyHudDiag();
    }
    if (!Visible()) {
        return false;
    }
    std::scoped_lock lock{imgui_mutex};
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(extent.width), float(extent.height));
    io.DeltaTime = ms > 0.0f && ms < 1000.0f ? ms / 1000.0f : 1.0f / 60.0f;
    // UI scale follows the display height (1080p = 1).
    const float scale = std::max(float(extent.height) / 1080.0f, 0.75f);
    if (std::abs(scale - base_scale) > 0.01f) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(scale / base_scale);
        style.FontScaleMain = scale;
        base_scale = scale;
    }

    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    perf_hud_bottom_left = false;
    party_hud_top = -1.0f;
    if (menu_open) {
        Menu();
    }
    bool game_menu = false;
    if (BbSettings::Get().show_hud || BbSettings::Get().show_fps) {
        coop::PerfHudGameState(&game_menu, &perf_hud_banner);
    }
    // Under the game's full-screen menus (inventory, equipment, level up...) neither is drawn,
    // as the party HUD (0.6.2).
    if (BbSettings::Get().show_hud && !menu_open && !game_menu) {
        PerformanceHud(); // includes the frame rate: the FPS counter is not drawn too
    } else if (BbSettings::Get().show_fps && !menu_open && !game_menu) {
        FpsCounter();
    }
    HoldPartyNoticesInGameMenu();
    if (!menu_open) {
        CompileIndicator();
        WarperPanel();
        if (!party::hud::GameMenu()) {  // the game's full-screen menu: none of these over it
            PartyHud(); // before the toast: it stacks above the panel
            PartyPings();
            PartyStainMarker();
            PartyToast();
            PartyAlert();
            PartyNpcCaption();
        }
    }
    if (prompt_active && !menu_open) {
        TextPrompt();
    }
    if (HdrPatternShown()) {
        HdrPatternLabels();
    }
    ImGui::Render();

    const vk::RenderingAttachmentInfo attachment{
        .imageView = view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue = vk::ClearValue{vk::ClearColorValue{std::array{0.0f, 0.0f, 0.0f, 0.0f}}},
    };
    cmdbuf.beginRendering(vk::RenderingInfo{
        .renderArea = {{0, 0}, extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    });
    {
        // Font atlas uploads submit to the graphics queue themselves.
        std::scoped_lock submit_lock{Vulkan::Scheduler::submit_mutex};
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmdbuf);
    }
    cmdbuf.endRendering();
    return true;
}

} // namespace BbOverlay

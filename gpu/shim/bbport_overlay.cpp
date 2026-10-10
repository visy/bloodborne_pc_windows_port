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
#include <string>
#include <string_view>
#include <vector>

#include <SDL3/SDL.h>
#include "bbport_compile_progress.h"
#include "bbport_frame_state.h"
#include "bbport_settings.h"
#include "party/party_status.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_vulkan.h"
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

extern "C" void runtime_restart(void); // bb-probe (probe.c)

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
bool focus_request = true; // the menu was opened: its first row takes the focus

// Party mode notices (join / leave / travel, party_status SetLastEvent): a toast in a top corner
// for a few seconds, never taking input. Atomics: Visible() reads them without imgui_mutex.
std::atomic<std::uint64_t> toast_seen{0};  // the party event already shown
std::atomic<std::int64_t> toast_until_ms{0}; // steady clock
std::string toast_text;                      // present thread (imgui_mutex)
constexpr std::int64_t ToastMs = 4000;

std::int64_t SteadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
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
    Store(value, v, ImGui::Checkbox(label, &v));
}

void Slider(const char* label, std::atomic<float>& value, float lo, float hi) {
    float v = value;
    Store(value, v, ImGui::SliderFloat(label, &v, lo, hi, "%.2f"));
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
    const ImVec2 label_size = ImGui::CalcTextSize(label);
    draw->AddText(ImVec2(at.x + S(28.0f), mid_y - label_size.y * 0.5f), label_color, label);

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
    auto& s = BbSettings::Get();
    bool restart = s.object_motion != s.startup_object_motion || s.model_lod != s.startup_model_lod ||
                   s.live_resolution != s.startup_live_resolution ||
                   s.gpl != s.startup_gpl || BbSettings::ResolutionNeedsRestart();
    for (int e = 0; e < BbSettings::EffectCount; ++e) {
        restart |= s.effects[e] != s.startup_effects[e];
    }
    return restart;
}

void DisplayTab() {
    auto& s = BbSettings::Get();
    static const char* outputs[] = {"1280 x 720", "1920 x 1080", "2560 x 1440", "3840 x 2160"};
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
    Toggle("fps", T("FPS counter", "Счётчик FPS"), s.show_fps,
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
}

void EffectsTab() {
    auto& s = BbSettings::Get();
    const char* lods[] = {T("Highest (-2)", "Максимальная (-2)"), T("Game default", "Как в игре"),
                          T("Lower (1)", "Ниже (1)"), T("Lowest (2)", "Минимальная (2)")};
    static constexpr int lod_values[] = {-2, 0, 1, 2};
    int lod_index = 1;
    for (int i = 0; i < 4; ++i) {
        if (lod_values[i] == s.model_lod) lod_index = i;
    }
    Choice("lod", T("Model detail", "Детализация моделей"), lod_index, 4, [&](int i) { return lods[i]; },
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
    ImGui::Spacing();
    Note(mk ? T("Mouse: move = camera, left = R1, Shift + left = R2, right = L2, Shift + right = L1, "
                "middle = R3 (lock-on), side buttons = L1 / R2.  Keys: WASD move, Space = Circle "
                "(dodge), E / Enter = Cross, R = Square, X = Triangle, Q = R3, C / Z = L3, Esc / F1 = "
                "Options, Tab / G = touchpad, Backspace = right touchpad, arrows or 1 2 3 4 = d-pad, "
                "IJKL = camera.",
                "Мышь: движение — камера, левая — R1, Shift + левая — R2, правая — L2, Shift + правая "
                "— L1, средняя — R3 (захват цели), боковые — L1 / R2.  Клавиши: WASD — движение, "
                "Пробел — Круг (уклонение), E / Enter — Крест, R — Квадрат, X — Треугольник, Q — R3, "
                "C / Z — L3, Esc / F1 — Options, Tab / G — тачпад, Backspace — правая сторона тачпада, "
                "стрелки или 1 2 3 4 — крестовина, IJKL — камера.")
            : T("Classic layout: WASD move, IJKL camera, Space / Enter = Cross, Left Shift / Esc = "
                "Circle, E = Square, Q = Triangle, 1 / 3 = L1 / R1, R / F = L2 / R2, Z / C = L3 / R3, "
                "F1 / O = Options, Tab / Backspace = touchpad, arrows = d-pad.",
                "Классическая раскладка: WASD — движение, IJKL — камера, Пробел / Enter — Крест, "
                "Левый Shift / Esc — Круг, E — Квадрат, Q — Треугольник, 1 / 3 — L1 / R1, R / F — "
                "L2 / R2, Z / C — L3 / R3, F1 / O — Options, Tab / Backspace — тачпад, стрелки — "
                "крестовина."),
         Dim());
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
    const ImVec2 label_size = ImGui::CalcTextSize(label);
    draw->AddText(ImVec2(at.x + S(28.0f), mid_y - label_size.y * 0.5f), Plain(0.8f), label);
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

struct PartyUiState {
    double copied_until = 0.0;       ///< "Copied" shown in the code row until then (ImGui time)
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

    // Party code + copy.
    const bool copied = now < party_ui.copied_until;
    const char* code_value = copied          ? T("Copied to clipboard", "Скопирован в буфер обмена")
                             : b.code.empty() ? T("not yet known", "ещё неизвестен")
                                              : b.code.c_str();
    if (ActionRow("party_copy", T("Party code: copy", "Код группы: копировать"), code_value,
                  b.role == ps::Role::Host
                      ? T("The code friends paste into the launcher to join you: your address, port and "
                          "the party's secret. Share it only with them.",
                          "Код, который друзья вставляют в лаунчер, чтобы присоединиться: ваш адрес, "
                          "порт и секрет группы. Давайте его только им.")
                      : T("The code of the party you joined. Copies it to the clipboard.",
                          "Код группы, к которой вы присоединились. Копирует его в буфер обмена."),
                  on && !b.code.empty())) {
        if (ps::RequestCopyCode()) {
            party_ui.copied_until = now + 2.0;
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
    }

    // Title.
    ImGui::PushFont(serif_font, 34.0f);
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
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + (right ? viewport->WorkSize.x - pad : pad),
                                   viewport->WorkPos.y + (bottom ? viewport->WorkSize.y - pad : pad)),
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

/// Party notices (join / leave / travel): the latest party_status event for ToastMs in a top
/// corner (top left; top right while the performance HUD has the top left), fading out. Not
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
        toast_until_ms = toast_text.empty() ? 0 : now + ToastMs;
    }
    const std::int64_t left = toast_until_ms.load() - now;
    if (left <= 0 || toast_text.empty()) {
        return;
    }
    const float alpha = std::clamp(float(left) / 500.0f, 0.0f, 1.0f); // fades over the last 0.5 s
    const auto& s = BbSettings::Get();
    const bool right = s.show_hud && s.hud_position == BbSettings::HudTopLeft;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + (right ? viewport->WorkSize.x - pad : pad),
                                   viewport->WorkPos.y + pad),
                            ImGuiCond_Always, ImVec2(right ? 1.0f : 0.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.6f * alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f * base_scale, 8.0f * base_scale));
    ImGui::PushFont(serif_font, 19.0f);
    ImGui::Begin("##party_toast", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::TextColored(ImVec4(0.96f, 0.88f, 0.70f, alpha), "%s", toast_text.c_str());
    // The game's gold underline, fading to both ends.
    const float w = ImGui::GetItemRectSize().x, y = ImGui::GetItemRectMax().y + 3.0f * base_scale;
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
        if (event.key.key == SDLK_F11) {
            if (down && !event.key.repeat) {
                auto& s = BbSettings::Get();
                s.show_hud = !s.show_hud.load();
                BbSettings::Save();
            }
            return true;
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

bool Visible() {
    return initialized &&
           (menu_open || prompt_active || BbSettings::Get().show_fps || BbSettings::Get().show_hud ||
            CompileIndicatorShown() || ToastWanted());
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

void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent) {

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
    if (!Visible()) {
        return;
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
    if (menu_open) {
        Menu();
    }
    if (BbSettings::Get().show_hud && !menu_open) {
        PerformanceHud(); // includes the frame rate: the FPS counter is not drawn too
    } else if (BbSettings::Get().show_fps && !menu_open) {
        FpsCounter();
    }
    if (!menu_open) {
        CompileIndicator();
        PartyToast();
    }
    if (prompt_active && !menu_open) {
        TextPrompt();
    }
    ImGui::Render();

    const vk::RenderingAttachmentInfo attachment{
        .imageView = view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
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
}

} // namespace BbOverlay

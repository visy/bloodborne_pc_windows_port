// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_overlay.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>

#include <SDL3/SDL.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <vk_mem_alloc.h>
#include "bbport_settings.h"
#include "bbport_toggles.h"
#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

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

extern "C" void runtime_restart(void); // bb-probe (probe.c)

#include "sdl_window.h"
extern Frontend::WindowSDL* g_window;

namespace BbOverlay {

namespace {

using BbSettings::Tr; // English by default, Russian with language=ru (bbport.ini, BB_LANG)

std::mutex imgui_mutex; // the ImGui context: window thread (input) and present thread
bool initialized = false;
std::atomic<bool> menu_open{false};
bool l3_down = false, r3_down = false;
bool dirty = false; // settings changed while open: saved on close
float base_scale = 1.0f;

// Present rate for the FPS counter and HUD telemetry.
std::chrono::steady_clock::time_point last_present{};
float frame_ms_avg = 0.0f;
VmaAllocator g_vma_allocator = nullptr;
const Vulkan::Instance* g_vk_instance = nullptr;

// Real-time frametime variance history buffer
constexpr int FrametimeHistorySize = 120;
float frametime_history[FrametimeHistorySize] = {};
int frametime_history_idx = 0;

// Host system CPU, RAM, and GPU telemetry
float cached_cpu_usage = 0.0f;
float cached_gpu_usage = -1.0f;
float cached_ram_used_gb = 0.0f;
float cached_ram_total_gb = 0.0f;
std::chrono::steady_clock::time_point last_sys_query{};

#ifdef _WIN32
uint64_t last_idle_time = 0;
uint64_t last_kernel_time = 0;
uint64_t last_user_time = 0;

// Dynamic NVML loader for NVIDIA GPU utilization
struct NvmlUtilizationRates {
    unsigned int gpu;
    unsigned int memory;
};
typedef int (*NvmlInit_t)();
typedef int (*NvmlShutdown_t)();
typedef int (*NvmlDeviceGetHandleByIndex_t)(unsigned int, void**);
typedef int (*NvmlDeviceGetUtilizationRates_t)(void*, NvmlUtilizationRates*);

static HMODULE g_nvml_lib = nullptr;
static NvmlDeviceGetUtilizationRates_t g_nvml_get_util = nullptr;
static void* g_nvml_device = nullptr;
static bool g_nvml_initialized = false;

// Dynamic PDH loader for AMD / Intel / universal GPU 3D engine utilization
struct PDH_ITEM_W {
    const wchar_t* szName;
    uint32_t CStatus;
    uint32_t dummy;
    double doubleValue;
};
typedef long (*PdhOpenQueryW_t)(const wchar_t*, uintptr_t, void**);
typedef long (*PdhAddEnglishCounterW_t)(void*, const wchar_t*, uintptr_t, void**);
typedef long (*PdhCollectQueryData_t)(void*);
typedef long (*PdhGetFormattedCounterArrayW_t)(void*, uint32_t, uint32_t*, uint32_t*, void*);
typedef long (*PdhCloseQuery_t)(void*);

static HMODULE g_pdh_lib = nullptr;
static PdhOpenQueryW_t g_pdh_open = nullptr;
static PdhAddEnglishCounterW_t g_pdh_add = nullptr;
static PdhCollectQueryData_t g_pdh_collect = nullptr;
static PdhGetFormattedCounterArrayW_t g_pdh_get_array = nullptr;
static PdhCloseQuery_t g_pdh_close = nullptr;
static void* g_pdh_query = nullptr;
static void* g_pdh_counter = nullptr;
static bool g_pdh_initialized = false;
static std::vector<char> g_pdh_buffer;

void InitGpuTelemetry() {
    if (g_nvml_initialized) return;
    g_nvml_initialized = true;

    // 1. Try NVIDIA Management Library (NVML)
    g_nvml_lib = LoadLibraryA("nvml.dll");
    if (g_nvml_lib) {
        auto nvml_init = reinterpret_cast<NvmlInit_t>(GetProcAddress(g_nvml_lib, "nvmlInit_v2"));
        if (!nvml_init) {
            nvml_init = reinterpret_cast<NvmlInit_t>(GetProcAddress(g_nvml_lib, "nvmlInit"));
        }
        if (nvml_init && nvml_init() == 0) {
            auto nvml_get_handle = reinterpret_cast<NvmlDeviceGetHandleByIndex_t>(GetProcAddress(g_nvml_lib, "nvmlDeviceGetHandleByIndex_v2"));
            if (!nvml_get_handle) {
                nvml_get_handle = reinterpret_cast<NvmlDeviceGetHandleByIndex_t>(GetProcAddress(g_nvml_lib, "nvmlDeviceGetHandleByIndex"));
            }
            g_nvml_get_util = reinterpret_cast<NvmlDeviceGetUtilizationRates_t>(GetProcAddress(g_nvml_lib, "nvmlDeviceGetUtilizationRates"));
            if (nvml_get_handle && g_nvml_get_util) {
                nvml_get_handle(0, &g_nvml_device);
                if (g_nvml_device) return; // NVML ready
            }
        }
    }

    // 2. Fallback to Windows PDH (AMD Radeon, Intel Arc, or generic)
    if (!g_pdh_initialized) {
        g_pdh_initialized = true;
        g_pdh_lib = LoadLibraryA("pdh.dll");
        if (g_pdh_lib) {
            g_pdh_open = reinterpret_cast<PdhOpenQueryW_t>(GetProcAddress(g_pdh_lib, "PdhOpenQueryW"));
            g_pdh_add = reinterpret_cast<PdhAddEnglishCounterW_t>(GetProcAddress(g_pdh_lib, "PdhAddEnglishCounterW"));
            g_pdh_collect = reinterpret_cast<PdhCollectQueryData_t>(GetProcAddress(g_pdh_lib, "PdhCollectQueryData"));
            g_pdh_get_array = reinterpret_cast<PdhGetFormattedCounterArrayW_t>(GetProcAddress(g_pdh_lib, "PdhGetFormattedCounterArrayW"));
            g_pdh_close = reinterpret_cast<PdhCloseQuery_t>(GetProcAddress(g_pdh_lib, "PdhCloseQuery"));
            if (g_pdh_open && g_pdh_add && g_pdh_collect && g_pdh_get_array) {
                if (g_pdh_open(nullptr, 0, &g_pdh_query) == 0 && g_pdh_query) {
                    if (g_pdh_add(g_pdh_query, L"\\GPU Engine(*engtype_3D)\\Utilization Percentage", 0, &g_pdh_counter) == 0) {
                        g_pdh_collect(g_pdh_query);
                    } else if (g_pdh_add(g_pdh_query, L"\\GPU Engine(*engtype_3d)\\Utilization Percentage", 0, &g_pdh_counter) == 0) {
                        g_pdh_collect(g_pdh_query);
                    }
                }
            }
        }
    }
}

void QueryGpuTelemetry() {
    if (!g_nvml_initialized) {
        InitGpuTelemetry();
    }
    // NVIDIA NVML path
    if (g_nvml_device && g_nvml_get_util) {
        NvmlUtilizationRates rates{};
        if (g_nvml_get_util(g_nvml_device, &rates) == 0) {
            cached_gpu_usage = float(rates.gpu);
            return;
        }
    }
    // AMD / Intel PDH fallback path
    if (g_pdh_query && g_pdh_counter && g_pdh_collect && g_pdh_get_array) {
        if (g_pdh_collect(g_pdh_query) == 0) {
            uint32_t buf_size = 0;
            uint32_t item_count = 0;
            g_pdh_get_array(g_pdh_counter, 0x00000200 /* PDH_FMT_DOUBLE */, &buf_size, &item_count, nullptr);
            if (buf_size > 0) {
                if (g_pdh_buffer.size() < buf_size) {
                    g_pdh_buffer.resize(buf_size);
                }
                if (g_pdh_get_array(g_pdh_counter, 0x00000200, &buf_size, &item_count, g_pdh_buffer.data()) == 0) {
                    auto* items = reinterpret_cast<PDH_ITEM_W*>(g_pdh_buffer.data());
                    double total_gpu = 0.0;
                    for (uint32_t i = 0; i < item_count; ++i) {
                        if (items[i].CStatus == 0 && items[i].doubleValue > 0.0) {
                            total_gpu += items[i].doubleValue;
                        }
                    }
                    cached_gpu_usage = std::clamp(float(total_gpu), 0.0f, 100.0f);
                }
            }
        }
    }
}
#endif

void UpdateSystemTelemetry() {
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration<float>(now - last_sys_query).count() < 0.5f) {
        return;
    }
    last_sys_query = now;
#ifdef _WIN32
    QueryGpuTelemetry();
    FILETIME idle, kernel, user;
    if (GetSystemTimes(&idle, &kernel, &user)) {
        ULARGE_INTEGER i, k, u;
        i.LowPart = idle.dwLowDateTime; i.HighPart = idle.dwHighDateTime;
        k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
        u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
        if (last_kernel_time != 0 && last_user_time != 0) {
            uint64_t diff_idle = i.QuadPart - last_idle_time;
            uint64_t diff_kernel = k.QuadPart - last_kernel_time;
            uint64_t diff_user = u.QuadPart - last_user_time;
            uint64_t total_sys = diff_kernel + diff_user;
            if (total_sys > 0) {
                float busy = total_sys > diff_idle ? float(total_sys - diff_idle) : 0.0f;
                cached_cpu_usage = std::clamp((busy / float(total_sys)) * 100.0f, 0.0f, 100.0f);
            }
        }
        last_idle_time = i.QuadPart;
        last_kernel_time = k.QuadPart;
        last_user_time = u.QuadPart;
    }
    MEMORYSTATUSEX mem;
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem)) {
        cached_ram_total_gb = float(mem.ullTotalPhys) / (1024.0f * 1024.0f * 1024.0f);
        cached_ram_used_gb = float(mem.ullTotalPhys - mem.ullAvailPhys) / (1024.0f * 1024.0f * 1024.0f);
    }
#endif
}

void SetOpen(bool value) {
    if (menu_open.load() == value) {
        return;
    }
    // Save before the menu is marked closed: the window thread re-reads bbport.ini only while
    // the menu is closed (window.cpp), so it can never read the file before this write.
    if (!value && dirty) {
        dirty = false;
        BbSettings::Save();
    }
    menu_open = value;
    ImGui::GetIO().MouseDrawCursor = value;
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

void Menu() {
    auto& s = BbSettings::Get();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + 40.0f * base_scale,
                                   viewport->WorkPos.y + 40.0f * base_scale),
                            ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(620.0f * base_scale, 0.0f), ImGuiCond_Appearing);
    bool keep_open = true;
    if (!ImGui::Begin(Tr("Bloodborne — settings  (Insert / L3+R3)###bb_menu",
                         "Bloodborne — настройки  (Insert / L3+R3)###bb_menu"),
                      &keep_open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    ImGui::Text(Tr("%.0f FPS  (%.1f ms)", "%.0f FPS  (%.1f мс)"),
                frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f, frame_ms_avg);

    ImGui::SeparatorText(Tr("Temporal upscaler", "Временной апскейлер"));
    const char* const upscalers[] = {Tr("Off", "Выкл"), "FSR 3.1", "FSR 4 (INT8)",
                                     "FSR 4.1.1 (INT8)",
                                     Tr("TAA (native anti-aliasing)", "TAA (нативное сглаживание)")};
    static const char* later[] = {"DLSS", "XeSS"};
    int upscaler = s.upscaler;
    if (ImGui::BeginCombo(Tr("Upscaler", "Апскейлер"), upscalers[upscaler])) {
        for (int i = 0; i < BbSettings::UpscalerCount; ++i) {
            const bool supported = i == BbSettings::UpscalerFsr4 ? s.fsr4_supported.load()
                : i == BbSettings::UpscalerFsr411 ? s.fsr411_supported.load() : true;
            ImGui::BeginDisabled(!supported);
            if (ImGui::Selectable(upscalers[i], i == upscaler)) {
                Store(s.upscaler, i, true);
            }
            ImGui::EndDisabled();
            if (!supported) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", Tr("— not supported by the graphics card",
                                             "— не поддерживается видеокартой"));
            }
        }
        for (const char* name : later) {
            ImGui::BeginDisabled();
            ImGui::Selectable(name, false);
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("%s", Tr("— in development", "— в работе"));
        }
        ImGui::EndCombo();
    }
    if (const char* problem = s.fsr4_problem.load()) {
        ImGui::PushTextWrapPos();
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), Tr("FSR 4 unavailable: %s", "FSR 4 недоступен: %s"),
                           problem);
        if (!BbSettings::IsFsr4(s.upscaler))
            ImGui::TextUnformatted(Tr("The mode chosen above is active. You can select FSR 4 again.",
                                      "Активен режим, выбранный выше. FSR 4 можно выбрать снова."));
        ImGui::PopTextWrapPos();
    }
    if (BbSettings::IsFsr4(s.upscaler)) {
        if (s.upscaler == BbSettings::UpscalerFsr411) {
            Hint(Tr("FSR 4.1.1 in INT8 mode: the model from AMD's 4.1.1 DLL, reproduced in Vulkan "
                    "(the result matches the DLL). One model for Native..Performance and a separate "
                    "one for Ultra Performance. Assets: tools/fsr4cap/build_assets.sh (needs the "
                    "DLLs and Proton).",
                    "FSR 4.1.1 в режиме INT8: модель из DLL AMD 4.1.1, воспроизведённая в Vulkan "
                    "(результат совпадает с DLL). Одна модель для Native..Performance и отдельная "
                    "для Ultra Performance. Ассеты: tools/fsr4cap/build_assets.sh (нужны DLL и Proton)."));
        } else {
            Hint(Tr("FSR 4 in INT8 mode (the v07 model from the AMD FidelityFX SDK sources). Higher "
                    "quality than FSR 3.1, but a heavier pass. Changing the preset rebuilds the "
                    "model (a short pause). Assets: tools/fetch_fsr4_assets.sh.",
                    "FSR 4 в режиме INT8 (модель v07 из исходников AMD FidelityFX SDK). Качество выше, "
                    "чем у FSR 3.1, но проход тяжелее. Смена пресета пересобирает модель (короткая "
                    "пауза). Ассеты: tools/fetch_fsr4_assets.sh."));
        }
        Checkbox(Tr("FSR 4: auto exposure", "FSR 4: авто-экспозиция"), s.fsr4_auto_exposure);
        Checkbox(Tr("FSR 4: invert jitter sign", "FSR 4: обратный знак jitter"), s.fsr4_invert_jitter);
        Hint(Tr("Ghosting check: the FSR 4 network normalizes colour by exposure and uses it to "
                "decide when to discard past frames. Applied immediately, no restart needed.",
                "Проверка при гостинге: сеть FSR 4 нормирует цвет по экспозиции и по ней решает, "
                "когда отбросить прошлые кадры. Меняются сразу, без перезапуска."));
    }
    const bool upscaler_on = s.upscaler != BbSettings::UpscalerOff;
    const bool taa = s.upscaler == BbSettings::UpscalerTaa;
    ImGui::BeginDisabled(!upscaler_on);
    ImGui::BeginDisabled(taa);
    int preset = taa ? BbSettings::NativeAA : s.preset.load();
    char preset_label[64];
    std::snprintf(preset_label, sizeof(preset_label), "%s (x%.1f)", BbSettings::PresetName(preset),
                  BbSettings::PresetScale(preset));
    if (ImGui::BeginCombo(Tr("Preset", "Пресет"), preset_label)) {
        for (int i = 0; i < BbSettings::PresetCount; ++i) {
            char label[64];
            const float scale = BbSettings::PresetScale(i);
            const int output = s.output_res;
            std::snprintf(label, sizeof(label), Tr("%s (x%.1f, render %dx%d)", "%s (x%.1f, рендер %dx%d)"),
                          BbSettings::PresetName(i), scale,
                          int(std::lround(BbSettings::OutputWidths[output] / scale / 2) * 2),
                          int(std::lround(BbSettings::OutputHeights[output] / scale / 2) * 2));
            if (ImGui::Selectable(label, i == preset)) {
                Store(s.preset, i, true);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (taa) {
        ImGui::TextWrapped("%s", Tr("TAA smooths the scene at the output resolution, without the FSR "
                                    "model or upscaling. The saved FSR preset is restored when you "
                                    "choose FSR.",
                                    "TAA сглаживает сцену в разрешении вывода, без модели FSR и апскейлинга. "
                                    "Сохранённый пресет FSR восстановится при выборе FSR."));
    }
    ImGui::Text(Tr("Active scene render: %d x %d", "Активный рендер сцены: %d x %d"),
                s.active_render_width.load(), s.active_render_height.load());
    if (BbSettings::FixedRenderSession()) {
        ImGui::Text(Tr("Preset at start: %s", "Пресет при запуске: %s"),
                    BbSettings::PresetName(s.startup_preset));
        if (const char* automatic = std::getenv("BB_AUTO_RENDER_RES");
            automatic && automatic[0] == '1') {
            Hint(Tr("When the output is not 1080p the whole game is drawn at the preset resolution "
                    "(a startup patch): this is fastest on the Steam Deck and weak GPUs. Changing "
                    "the preset or the output resolution needs a restart. \"Live resolution "
                    "changes\" below enables changes without a restart (post-processing then "
                    "stays at 1080p, which is slower).",
                    "При выводе не 1080p вся игра рисуется в разрешении пресета (патч при запуске): "
                    "это быстрее всего на Steam Deck и слабых GPU. Смена пресета или разрешения "
                    "вывода — после перезапуска. Пункт «Смена разрешения на лету» ниже включает "
                    "смену без перезапуска (постобработка тогда остаётся в 1080p, медленнее)."));
        } else {
            Hint(Tr("BB_RENDER_RES fixes the scene size at start. Unset this explicit variable "
                    "to change the resolution and presets without restarting the game.",
                    "BB_RENDER_RES фиксирует размер сцены при запуске. Уберите эту явную переменную "
                    "для смены разрешения и пресетов без перезапуска игры."));
        }
    } else {
        Hint(Tr("Native AA: FSR works as anti-aliasing. The other presets lower the scene render "
                "resolution relative to the output. The UI is drawn at the output resolution. "
                "A preset applies from the next frame, without restarting the game.",
                "Native AA: FSR работает как сглаживание. Остальные пресеты уменьшают разрешение "
                "отрисовки сцены относительно вывода. Интерфейс рисуется в разрешении вывода. "
                "Пресет применяется со следующего кадра без перезапуска игры."));
    }
    Checkbox(Tr("Sharpening (RCAS)", "Резкость (RCAS)"), s.sharpen);
    ImGui::BeginDisabled(!s.sharpen);
    Slider(Tr("Sharpening strength", "Сила резкости"), s.sharpness, 0.0f, 2.0f);
    Hint(Tr("Up to 1 is the upscaler's own sharpening (RCAS). Above 1 an extra RCAS pass is "
            "added. Ctrl+click on the slider to type an exact value.",
            "До 1 — резкость самого апскейлера (RCAS). Выше 1 добавляется ещё один проход RCAS. "
            "Ctrl+клик по ползунку — ввести точное значение."));
    ImGui::EndDisabled();
    Checkbox(Tr("Sub-pixel shift (jitter)", "Субпиксельный сдвиг (jitter)"), s.jitter);
    Hint(Tr("Every frame the scene is shifted by a fraction of a pixel, and the upscaler gathers "
            "more detail from several frames. Without it you only get history-based smoothing.",
            "Каждый кадр сцена сдвигается на долю пикселя, и апскейлер собирает из нескольких "
            "кадров больше деталей. Без него получается только сглаживание по истории."));

    ImGui::SeparatorText(Tr("Reactive mask", "Маска реактивности"));
    ImGui::BeginDisabled(taa);
    Checkbox(Tr("Enable mask", "Включить маску"), s.reactive);
    Hint(Tr("Marks transparent effects (particles, haze) so the upscaler relies less on past "
            "frames. Fewer trails behind effects, but shimmer returns under them.",
            "Помечает прозрачные эффекты (частицы, дымку), чтобы апскейлер меньше опирался на "
            "прошлые кадры. Меньше шлейфов за эффектами, но под ними возвращается дрожание."));
    ImGui::BeginDisabled(!s.reactive);
    Slider(Tr("Scale", "Масштаб"), s.reactive_scale, 0.0f, 4.0f);
    Slider(Tr("Threshold", "Порог"), s.reactive_threshold, 0.0f, 1.0f);
    Slider(Tr("Maximum", "Максимум"), s.reactive_max, 0.0f, 1.0f);
    bool show_mask = s.debug_view == BbSettings::DebugReactive;
    if (ImGui::Checkbox(Tr("Show mask (debug)", "Показать маску (отладка)"), &show_mask)) {
        s.debug_view = show_mask ? BbSettings::DebugReactive : BbSettings::DebugNone;
        dirty = true;
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    Checkbox(Tr("Character motion vectors", "Векторы движения персонажей"), s.object_motion);
    Hint(Tr("Accurate vectors for animated objects: clothes and weapons fall apart less when "
            "moving. A static scene gets no extra pass. A change applies after restarting the "
            "game.",
            "Точные векторы для анимированных объектов: одежда и оружие меньше рассыпаются "
            "при движении. Статичная сцена не получает дополнительный проход. "
            "Изменение применяется после перезапуска игры."));
    bool show_motion = s.debug_view == BbSettings::DebugMotion;
    if (ImGui::Checkbox(Tr("Show motion vectors (debug)", "Показать векторы движения (отладка)"),
                        &show_motion)) {
        s.debug_view = show_motion ? BbSettings::DebugMotion : BbSettings::DebugNone;
        dirty = true;
    }
    Hint(Tr("Red/green: horizontal/vertical motion (8 pixels = full brightness). Blue: the pixel "
            "got an exact object vector, not just camera motion. A moving object with no blue and "
            "no red/green is treated by the upscaler as static, hence the trail.",
            "Красный/зелёный: движение по горизонтали/вертикали (8 пикселей = полная яркость). "
            "Синий: пиксель получил точный вектор объекта, а не только движение камеры. "
            "Движущийся предмет без синего и без красного/зелёного апскейлер считает "
            "неподвижным, отсюда шлейф."));
    ImGui::EndDisabled(); // upscaler off

    ImGui::SeparatorText(Tr("Output resolution", "Разрешение вывода"));
    static const char* outputs[] = {"1280 x 720", "1920 x 1080", "2560 x 1440", "3840 x 2160"};
    int output = s.output_res;
    if (ImGui::BeginCombo(Tr("Output resolution", "Разрешение вывода"), outputs[output])) {
        for (int i = 0; i < BbSettings::OutputCount; ++i) {
            if (ImGui::Selectable(outputs[i], i == output)) {
                Store(s.output_res, i, true);
            }
        }
        ImGui::EndCombo();
    }
    if (BbSettings::FixedRenderSession()) {
        Hint(Tr("The size of the finished frame and the UI. The preset sets the scene size "
                "relative to the output: 4K Performance = 1920x1080. Applies after restarting "
                "the game.",
                "Размер готового кадра и интерфейса. Пресет задаёт размер сцены относительно "
                "вывода: 4K Performance = 1920x1080. Применяется после перезапуска игры."));
    } else {
        Hint(Tr("The size of the finished frame and the UI changes at the next frame boundary. "
                "The preset sets the scene size relative to the output: 4K Performance = "
                "1920x1080. Changing the size resets the FSR history and can cause a short pause.",
                "Размер готового кадра и интерфейса меняется на границе следующего кадра. "
                "Пресет задаёт размер сцены относительно вывода: 4K Performance = 1920x1080. "
                "Смена размера сбрасывает историю FSR и может вызвать короткую паузу."));
    }
    const char* const live_modes[] = {Tr("Auto (by graphics card)", "Авто (по видеокарте)"),
                                      Tr("Off (faster)", "Выключена (быстрее)"),
                                      Tr("On", "Включена")};
    int live = s.live_resolution + 1;
    if (ImGui::BeginCombo(Tr("Live resolution changes", "Смена разрешения на лету"), live_modes[live])) {
        for (int i = 0; i < 3; ++i) {
            if (ImGui::Selectable(live_modes[i], i == live)) {
                Store(s.live_resolution, i - 1, true);
            }
        }
        ImGui::EndCombo();
    }
    Hint(Tr("On: the output resolution and preset change without a restart, but the game's "
            "post-processing stays at 1080p, which is noticeably slower on the Steam Deck and "
            "older graphics cards. Off: everything is drawn at the preset resolution and a "
            "change needs a restart. Auto turns it on for powerful discrete graphics cards. "
            "Applies after restarting the game.",
            "Включена: разрешение вывода и пресет меняются без перезапуска, но постобработка игры "
            "остаётся в 1080p — на Steam Deck и старых видеокартах это заметно медленнее. "
            "Выключена: всё рисуется в разрешении пресета, смена — через перезапуск. Авто включает "
            "её на мощных дискретных видеокартах. Применяется после перезапуска игры."));
    ImGui::SeparatorText(Tr("Game effects (after restart)", "Эффекты игры (после перезапуска)"));
    const char* const lods[] = {Tr("Highest (-2)", "Максимальная (-2)"),
                                Tr("Game default", "Как в игре"),
                                Tr("Lower (1)", "Ниже (1)"),
                                Tr("Lowest (2)", "Минимальная (2)")};
    static constexpr int lod_values[] = {-2, 0, 1, 2};
    int lod_index = 1;
    for (int i = 0; i < 4; ++i) {
        if (lod_values[i] == s.model_lod) lod_index = i;
    }
    if (ImGui::BeginCombo(Tr("Model detail", "Детализация моделей"), lods[lod_index])) {
        for (int i = 0; i < 4; ++i) {
            if (ImGui::Selectable(lods[i], i == lod_index)) {
                Store(s.model_lod, lod_values[i], true);
            }
        }
        ImGui::EndCombo();
    }
    for (int e = 0; e < BbSettings::EffectCount; ++e) {
        Checkbox(Tr(BbSettings::Effects[e].label_en, BbSettings::Effects[e].label_ru), s.effects[e]);
    }
    Hint(Tr("Effects are switched on and off by game patches at start (patches/Bloodborne.xml). "
            "Motion blur and dynamic light shadows put a noticeable load on the GPU.",
            "Эффекты включаются и выключаются патчами игры при запуске (patches/Bloodborne.xml). "
            "Размытие в движении и тени от динамических источников заметно нагружают GPU."));
    Hint(Tr("Free camera: hold Cross and press L3 (keyboard: Space + Z). Debug menu: left "
            "touchpad / Tab. Needs DbgFont14h.ccm and DbgFont14h.tpf in dvdroot_ps4/font from "
            "Nexus mod #253. Right touchpad: Backspace.",
            "Свободная камера: удерживайте Cross и нажимайте L3 (клавиатура: Space + Z). "
            "Debug menu: левый touchpad / Tab. Нужны DbgFont14h.ccm и DbgFont14h.tpf "
            "в dvdroot_ps4/font из мода Nexus #253. Правый touchpad: Backspace."));

    bool restart = s.object_motion != s.startup_object_motion ||
                   s.model_lod != s.startup_model_lod ||
                   s.live_resolution != s.startup_live_resolution ||
                   BbSettings::ResolutionNeedsRestart();
    for (int e = 0; e < BbSettings::EffectCount; ++e) {
        restart |= s.effects[e] != s.startup_effects[e];
    }
    if (restart) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s",
                           Tr("Changes apply after restarting the game",
                              "Изменения применятся после перезапуска игры"));
        if (ImGui::Button(Tr("Apply and restart the game", "Применить и перезапустить игру"))) {
            BbSettings::Save();
            runtime_restart();
        }
    }

    ImGui::SeparatorText(Tr("Other", "Прочее"));
    {
        const char* const languages[] = {"English", "Русский"};
        int language = s.language.load();
        if (ImGui::BeginCombo("Language / Язык", languages[language])) {
            for (int i = 0; i < BbSettings::LanguageCount; ++i) {
                if (ImGui::Selectable(languages[i], i == language)) {
                    Store(s.language, i, true);
                }
            }
            ImGui::EndCombo();
        }
    }
    Checkbox(Tr("FPS counter in the corner", "Счётчик FPS в углу"), s.show_fps);
    Checkbox(Tr("Native performance overlay (HUD)", "Нативный оверлей производительности (HUD)"),
             s.show_hud);
    Hint(Tr("In-game telemetry overlay (F11 / Shift+Tab): FPS, frametime graph, VRAM, CPU, RAM "
            "and render metrics",
            "Внутриигровой оверлей телеметрии (F11 / Shift+Tab): FPS, график фреймтайма, VRAM, CPU, RAM и метрики рендера"));
    if (s.show_hud) {
        const char* const quadrants[] = {Tr("Top left", "Сверху слева"),
                                         Tr("Top right", "Сверху справа"),
                                         Tr("Bottom left", "Снизу слева"),
                                         Tr("Bottom right", "Снизу справа")};
        int quad = s.hud_quadrant.load();
        if (ImGui::BeginCombo(Tr("HUD position", "Расположение HUD"), quadrants[quad])) {
            for (int q = 0; q < BbSettings::HudQuadrantCount; ++q) {
                if (ImGui::Selectable(quadrants[q], q == quad)) {
                    Store(s.hud_quadrant, q, true);
                }
            }
            ImGui::EndCombo();
        }
        Slider(Tr("HUD opacity", "Прозрачность HUD"), s.hud_opacity, 0.1f, 1.0f);
        Slider(Tr("HUD scale", "Масштаб HUD"), s.hud_scale, 0.5f, 2.0f);
    }

    ImGui::SeparatorText(Tr("Mouse and keyboard controls (M&K)", "Управление мышью и клавиатурой (M&K)"));
    Checkbox(Tr("Enable mouse and keyboard controls", "Включить управление мышью и клавиатурой"),
             s.mk_enabled);
    Hint(Tr("Souls PC style control scheme: mouse look (cursor capture), LMB - normal attack (R1), "
            "Shift+LMB - strong attack (R2), RMB - gun/left hand (L2), MMB - target lock (R3), "
            "Tab - weapon transform (L1)",
            "Схема управления в стиле Souls PC: обзор мышью (захват курсора), ЛКМ - обычная атака (R1), Shift+ЛКМ - сильная (R2), ПКМ - выстрел/левая рука (L2), СКМ - захват цели (R3), Tab - трансформация оружия (L1)"));
    if (s.mk_enabled) {
        Slider(Tr("Horizontal sensitivity (X)", "Чувствительность по горизонтали (X)"), s.mk_sens_x, 0.1f, 5.0f);
        Slider(Tr("Vertical sensitivity (Y)", "Чувствительность по вертикали (Y)"), s.mk_sens_y, 0.1f, 5.0f);
        Checkbox(Tr("Invert horizontal look (X)", "Инвертировать обзор по горизонтали (X)"), s.mk_invert_x);
        Checkbox(Tr("Invert vertical look (Y)", "Инвертировать обзор по вертикали (Y)"), s.mk_invert_y);
        Slider(Tr("Mouse dead zone", "Мёртвая зона мыши"), s.mk_deadzone, 0.0f, 0.2f);
        Slider(Tr("Mouse smoothing", "Сглаживание движений мыши"), s.mk_smoothing, 0.0f, 0.8f);
    }

    ImGui::Spacing();
    if (ImGui::Button(Tr("Close", "Закрыть"))) {
        keep_open = false;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", Tr("Settings are saved to bbport.ini", "Настройки сохраняются в bbport.ini"));
    ImGui::End();
    if (!keep_open) {
        SetOpen(false);
    }
}

void FpsCounter() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - pad,
                                   viewport->WorkPos.y + pad),
                            ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.5f);
    ImGui::Begin("##fps", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    const auto& s = BbSettings::Get();
    ImGui::Text(Tr("%.0f FPS  %.1f ms  %s", "%.0f FPS  %.1f мс  %s"),
                frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f, frame_ms_avg,
                s.upscaler == BbSettings::UpscalerFsr3   ? "FSR 3.1"
                : s.upscaler == BbSettings::UpscalerFsr4 ? "FSR 4"
                : s.upscaler == BbSettings::UpscalerFsr411 ? "FSR 4.1.1"
                : s.upscaler == BbSettings::UpscalerTaa ? "TAA"
                                                         : "");
    ImGui::End();
}

void HudOverlay() {
    const auto& s = BbSettings::Get();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 14.0f * base_scale;
    const float hud_scale_val = std::clamp(s.hud_scale.load(), 0.5f, 2.5f);
    const float alpha = std::clamp(s.hud_opacity.load(), 0.1f, 1.0f);

    ImVec2 pos, pivot;
    switch (s.hud_quadrant.load()) {
    case BbSettings::HudTopLeft:
        pos = ImVec2(viewport->WorkPos.x + pad, viewport->WorkPos.y + pad);
        pivot = ImVec2(0.0f, 0.0f);
        break;
    case BbSettings::HudTopRight:
        pos = ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - pad, viewport->WorkPos.y + pad);
        pivot = ImVec2(1.0f, 0.0f);
        break;
    case BbSettings::HudBottomLeft:
        pos = ImVec2(viewport->WorkPos.x + pad, viewport->WorkPos.y + viewport->WorkSize.y - pad);
        pivot = ImVec2(0.0f, 1.0f);
        break;
    case BbSettings::HudBottomRight:
    default:
        pos = ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - pad,
                     viewport->WorkPos.y + viewport->WorkSize.y - pad);
        pivot = ImVec2(1.0f, 1.0f);
        break;
    }

    ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);
    ImGui::SetNextWindowBgAlpha(alpha);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                                   ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f * base_scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f * base_scale * hud_scale_val, 8.0f * base_scale * hud_scale_val));

    if (ImGui::Begin("##bb_hud", nullptr, flags)) {
        // Update host telemetry (CPU, RAM)
        UpdateSystemTelemetry();

        // 1. Performance: FPS & Frametime
        const float cur_fps = frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f;
        const char* upscaler_label = s.upscaler == BbSettings::UpscalerFsr3   ? " [FSR 3.1]"
                                   : s.upscaler == BbSettings::UpscalerFsr4 ? " [FSR 4]"
                                   : s.upscaler == BbSettings::UpscalerFsr411 ? " [FSR 4.1.1]"
                                   : s.upscaler == BbSettings::UpscalerTaa ? " [TAA]"
                                   : "";

        ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "FPS: %.1f", cur_fps);
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.0f, 1.0f), "(%.2f ms)%s", frame_ms_avg, upscaler_label);

        // Frametime variance graph
        char overlay_text[32];
        std::snprintf(overlay_text, sizeof(overlay_text), "%.1f ms", frame_ms_avg);
        ImGui::PlotLines("##frametime_plot", frametime_history, FrametimeHistorySize,
                         frametime_history_idx, overlay_text, 0.0f, 50.0f,
                         ImVec2(190.0f * base_scale * hud_scale_val, 36.0f * base_scale * hud_scale_val));

        ImGui::Separator();

        // 2. Vulkan VRAM telemetry
        if (g_vma_allocator && g_vk_instance) {
            const auto& mem_props = g_vk_instance->GetMemoryProperties();
            VmaBudget budgets[VK_MAX_MEMORY_HEAPS] = {};
            vmaGetHeapBudgets(g_vma_allocator, budgets);

            VkDeviceSize total_vram_alloc = 0;
            VkDeviceSize total_vram_budget = 0;
            const u32 heap_count = std::min<u32>(mem_props.memoryHeapCount, VK_MAX_MEMORY_HEAPS);

            for (u32 h = 0; h < heap_count; ++h) {
                const bool is_device_local = (mem_props.memoryHeaps[h].flags & vk::MemoryHeapFlagBits::eDeviceLocal) != vk::MemoryHeapFlags{};
                // Only count dedicated DeviceLocal heaps (VRAM)
                if (is_device_local) {
                    total_vram_alloc += budgets[h].statistics.allocationBytes;
                    if (budgets[h].budget > 0) {
                        total_vram_budget += budgets[h].budget;
                    } else {
                        total_vram_budget += mem_props.memoryHeaps[h].size;
                    }
                }
            }

            // Fallback to total dedicated local memory if budget calculation is 0
            if (total_vram_budget == 0 && g_vk_instance->GetDeviceLocalMemory() > 0) {
                total_vram_budget = g_vk_instance->GetDeviceLocalMemory();
            }

            const float alloc_mb = float(total_vram_alloc) / (1024.0f * 1024.0f);
            const float budget_mb = float(total_vram_budget) / (1024.0f * 1024.0f);
            if (budget_mb > 0.0f) {
                ImGui::Text("VRAM: %.0f / %.0f MB (%.0f%%)", alloc_mb, budget_mb, std::clamp((alloc_mb / budget_mb) * 100.0f, 0.0f, 100.0f));
            } else {
                ImGui::Text("VRAM: %.0f MB", alloc_mb);
            }
        }

        ImGui::Separator();

        // 3. System metrics: GPU, CPU, and RAM
        if (cached_gpu_usage >= 0.0f) {
            ImGui::Text("GPU: %.0f%%", cached_gpu_usage);
        }
        ImGui::Text("CPU: %.1f%%", cached_cpu_usage);
        if (cached_ram_total_gb > 0.0f) {
            ImGui::Text("RAM: %.2f / %.2f GB", cached_ram_used_gb, cached_ram_total_gb);
        }

        ImGui::End();
    }
    ImGui::PopStyleVar(2);
}

} // namespace

void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count) {
    if (const char* env = std::getenv("BB_OVERLAY")) {
        if (!std::strcmp(env, "0") || !std::strcmp(env, "off") || !std::strcmp(env, "false")) {
            std::printf("Overlay: disabled (BB_OVERLAY=0)\n");
            return;
        }
    }
    std::scoped_lock lock{imgui_mutex};
    if (initialized) {
        return;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // window positions are not kept
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
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
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(bb_font_ttf),
                                   int(bb_font_ttf_end - bb_font_ttf), 18.0f, &font_config);

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
    g_vk_instance = &instance;
    g_vma_allocator = instance.GetAllocator();
    std::printf("Overlay: menu ready (Insert or L3+R3)\n");
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
        if (down && !event.key.repeat) {
            if (event.key.key == SDLK_INSERT || (is_open && event.key.key == SDLK_ESCAPE)) {
                SetOpen(event.key.key == SDLK_INSERT ? !is_open : false);
                return true;
            }
            // HUD overlay hotkeys: F11 or Shift + Tab
            const bool shift = (event.key.mod & SDL_KMOD_SHIFT) != 0;
            if (event.key.key == SDLK_F11 || (shift && event.key.key == SDLK_TAB)) {
                auto& s = BbSettings::Get();
                s.show_hud = !s.show_hud.load();
                BbSettings::Save();
                return true;
            }
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
        if (down && l3_down && r3_down) {
            SetOpen(!is_open);
            return true;
        }
        if (!is_open) {
            return false;
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
    return initialized && (menu_open || BbSettings::Get().show_fps || BbSettings::Get().show_hud || (g_window && g_window->IsTextInputActive()));
}

bool CapturesInput() {
    return menu_open;
}

void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent) {
    // Present interval for the FPS readout and HUD telemetry.
    const auto now = std::chrono::steady_clock::now();
    const float ms = std::chrono::duration<float, std::milli>(now - last_present).count();
    last_present = now;
    if (ms > 0.0f && ms < 1000.0f) {
        frame_ms_avg = frame_ms_avg == 0.0f ? ms : frame_ms_avg * 0.95f + ms * 0.05f;
        frametime_history[frametime_history_idx] = ms;
        frametime_history_idx = (frametime_history_idx + 1) % FrametimeHistorySize;
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
        HudOverlay();
    } else if (BbSettings::Get().show_fps && !menu_open) {
        FpsCounter();
    }
    if (g_window && g_window->IsTextInputActive()) {
        const std::string prompt = g_window->GetTextInputPrompt();
        const std::string current_val = g_window->GetTextInputValue();
        ImGui::SetNextWindowPos(ImVec2(float(extent.width) * 0.5f, float(extent.height) * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(std::min(480.0f * scale, float(extent.width) * 0.9f), 0.0f));
        if (ImGui::Begin("Name Input", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::Spacing();
            ImGui::Text("%s", prompt.c_str());
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "> %s_", current_val.c_str());
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextDisabled("Press Enter to Confirm, Esc to Cancel");
            ImGui::End();
        }
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

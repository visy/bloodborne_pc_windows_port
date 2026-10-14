// bbport: SDL3 window for the Vulkan swapchain (X11 or Wayland).
#include "bbport_headless.h"
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <SDL3/SDL.h>
#include "common/assert.h"
#include "common/logging/log.h"
#include "sdl_window.h"
#include "bbport_overlay.h"
#include "bbport_settings.h"
#include "bbport_mouse.h"
#include "bloodborne_cam.h"

namespace Frontend {

namespace {

// ReadMouse's bits: 0 left, 1 right, 2 middle, 3 X1, 4 X2.
u32 MouseButtonBit(u8 button) {
    switch (button) {
    case SDL_BUTTON_LEFT: return 1u << 0;
    case SDL_BUTTON_RIGHT: return 1u << 1;
    case SDL_BUTTON_MIDDLE: return 1u << 2;
    case SDL_BUTTON_X1: return 1u << 3;
    case SDL_BUTTON_X2: return 1u << 4;
    default: return 0;
    }
}

// Case-insensitive substring search (strcasestr is a GNU extension, missing on Windows).
bool ContainsNoCase(const char* haystack, const char* needle) {
    return SDL_strcasestr(haystack, needle) != nullptr;
}

// Issue #69: the monitor the window (and fullscreen) goes to. BB_DISPLAY: its number in SDL's
// order (1, 2, ...; bb-gpu-capabilities --displays lists them) or a part of its name; without it
// SDL's primary display. The monitors are logged so a report says which one was taken.
SDL_DisplayID ChooseDisplay() {
    const SDL_DisplayID primary = SDL_GetPrimaryDisplay();
    const char* wanted = std::getenv("BB_DISPLAY");
    int count = 0;
    SDL_DisplayID* ids = SDL_GetDisplays(&count);
    SDL_DisplayID chosen = 0;
    if (wanted && *wanted) {
        char* end = nullptr;
        const long number = std::strtol(wanted, &end, 10);
        if (end && *end == '\0') {
            if (number >= 1 && number <= count) {
                chosen = ids[number - 1];
            }
        } else {
            for (int i = 0; i < count && !chosen; ++i) {
                const char* name = SDL_GetDisplayName(ids[i]);
                if (name && ContainsNoCase(name, wanted)) {
                    chosen = ids[i];
                }
            }
        }
    }
    const SDL_DisplayID display = chosen ? chosen : primary;
    for (int i = 0; i < count; ++i) {
        const char* name = SDL_GetDisplayName(ids[i]);
        const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(ids[i]);
        std::printf("Display %d: %s %dx%d%s%s\n", i + 1, name ? name : "?", mode ? mode->w : 0,
                    mode ? mode->h : 0, ids[i] == primary ? " (primary)" : "",
                    ids[i] == display ? " <- the game's (BB_DISPLAY)" : "");
    }
    if (wanted && *wanted && !chosen) {
        std::printf("Display: BB_DISPLAY=%s matches none, the primary one is used\n", wanted);
    }
    SDL_free(ids);
    return display;
}

} // namespace

WindowSDL::WindowSDL(s32 width_, s32 height_, const char* title) : width{width_}, height{height_} {
    // Gamepads are sampled by runtime_pad.c; their events are pumped here with the window's.
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        UNREACHABLE_MSG("Failed to initialize SDL video: {}", SDL_GetError());
    }
    if (const char* mute = std::getenv("BB_MUTE_UNFOCUSED")) {
        mute_unfocused = mute[0] != '0';
    }
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title);
    const SDL_DisplayID display = ChooseDisplay();
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(display));
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(display));
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width_);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height_);
    // Test harness tiling (tools/mp/instances.py): BB_WINDOW_X/Y place the window, BB_WINDOW_W/H
    // size it (the render resolution is unchanged; the swapchain follows the window).
    auto env_int = [](const char* name, int* out) {
        const char* v = std::getenv(name);
        if (!v || !*v) return false;
        *out = std::atoi(v);
        return true;
    };
    int wx = 0, wy = 0, ww = 0, wh = 0;
    if (env_int("BB_WINDOW_X", &wx) && env_int("BB_WINDOW_Y", &wy)) {
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, wx);
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, wy);
    }
    if (env_int("BB_WINDOW_W", &ww) && env_int("BB_WINDOW_H", &wh) && ww > 0 && wh > 0) {
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, ww);
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, wh);
    }
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN, true);
    // Window mode (ported from Mrsuss60/bloodborne_pc_windows_port e2a647d, adapted): BB_WINDOW_MODE
    // = windowed | borderless | fullscreen; without it BB_FULLSCREEN=1 means fullscreen. Borderless
    // is a frameless window covering the whole display (no video mode switch).
    const char* fullscreen = std::getenv("BB_FULLSCREEN");
    const char* win_mode = std::getenv("BB_WINDOW_MODE");
    bool is_fullscreen = fullscreen && fullscreen[0] == '1';
    bool is_borderless = false;
    if (win_mode && win_mode[0]) {
        is_fullscreen = !SDL_strcasecmp(win_mode, "fullscreen");
        is_borderless = !SDL_strcasecmp(win_mode, "borderless");
    }
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN, is_fullscreen);
    if (is_borderless) {
        SDL_Rect bounds{};
        if (SDL_GetDisplayBounds(display, &bounds)) {
            SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, bounds.x);
            SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, bounds.y);
            SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, bounds.w);
            SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, bounds.h);
        }
        SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_BORDERLESS_BOOLEAN, true);
    }
    // Test runs (BB_WINDOW_BACKGROUND=1): never take focus from the desktop's user. The window is
    // shown without activation and cannot be focused; scripted input (BB_PAD_FILE) does not need it.
    if (const char* bg = std::getenv("BB_WINDOW_BACKGROUND"); bg && bg[0] == '1') {
        SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
        SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FOCUSABLE_BOOLEAN, false);
    }
    base_title = title;
    // Test harness label (tools/mp/instances.py --label): what this test run is for.
    if (const char* t = std::getenv("BB_WINDOW_TITLE"); t && *t) {
        base_title = t;
        SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, t);
    }
    // Headless-lite test mode (bbport_headless.h): minimised, never focused, nothing presented.
    if (BbHeadless::On()) {
        SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
        SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FOCUSABLE_BOOLEAN, false);
        SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_MINIMIZED_BOOLEAN, true);
    }
    window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    ASSERT_MSG(window, "Failed to create window: {}", SDL_GetError());
    // BB_WINDOW_MODE=fullscreen is exclusive: a window in SDL's fullscreen state is borderless
    // desktop fullscreen until it is given a display mode, here the desktop's (from
    // Supermedo/bloodborne_pc PR #68 by RafaelHGOliveira). BB_FULLSCREEN=1 alone stays desktop
    // fullscreen, and test runs (BB_WINDOW_BACKGROUND=1) never switch modes.
    if (is_fullscreen && win_mode && !SDL_strcasecmp(win_mode, "fullscreen")) {
        const char* bg = std::getenv("BB_WINDOW_BACKGROUND");
        const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(SDL_GetDisplayForWindow(window));
        if (bg && bg[0] == '1') {
            std::printf("Window: exclusive fullscreen skipped (BB_WINDOW_BACKGROUND=1)\n");
        } else if (!desktop || !SDL_SetWindowFullscreenMode(window, desktop)) {
            std::printf("Window: exclusive fullscreen unavailable, desktop fullscreen (%s)\n",
                        SDL_GetError());
        } else {
            std::printf("Window: exclusive fullscreen %dx%d@%.0f Hz\n", desktop->w, desktop->h,
                        desktop->refresh_rate);
        }
    }

    const char* driver = SDL_GetCurrentVideoDriver();
    const SDL_PropertiesID wp = SDL_GetWindowProperties(window);
#if defined(_WIN32)
    window_info.type = WindowSystemType::Windows;
    window_info.render_surface = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#else
    if (driver && !std::strcmp(driver, "x11")) {
        window_info.type = WindowSystemType::X11;
        window_info.display_connection = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
        window_info.render_surface = reinterpret_cast<void*>(SDL_GetNumberProperty(wp, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
    } else if (driver && !std::strcmp(driver, "wayland")) {
        window_info.type = WindowSystemType::Wayland;
        window_info.display_connection = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
        window_info.render_surface = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
    } else {
        UNREACHABLE_MSG("Unsupported SDL video driver {}", driver ? driver : "(none)");
    }
#endif
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    width = w;
    height = h;
    LOG_INFO(Frontend, "Window {}x{} on {}", w, h, driver);
}

WindowSDL::~WindowSDL() {
    if (mouse_sampler_started) {
        BbMouse::Stop();
    }
    Core::BloodborneCam::Instance().Shutdown();
    SDL_DestroyWindow(window);
}

void WindowSDL::BeginTextInput(const std::string& initial, const std::string& prompt) {
    std::scoped_lock lock{text_mutex};
    text = initial;
    text_prompt = prompt;
    text_state = 0;
    text_requested = true;
}

void WindowSDL::TestType(const std::string& keys) {
    // Text-input events point at their text: one static, NUL-terminated string per ASCII character.
    static char ascii[128][2];
    for (const char c : keys) {
        SDL_Event e;
        SDL_zero(e);
        if (c == '\n' || c == '\x1b' || c == '\b') {
            e.type = SDL_EVENT_KEY_DOWN;
            e.key.windowID = SDL_GetWindowID(window);
            e.key.key = c == '\n' ? SDLK_RETURN : c == '\b' ? SDLK_BACKSPACE : SDLK_ESCAPE;
            e.key.down = true;
        } else if (static_cast<unsigned char>(c) >= 0x20 && static_cast<unsigned char>(c) < 0x7f) {
            ascii[int(c)][0] = c;
            e.type = SDL_EVENT_TEXT_INPUT;
            e.text.windowID = SDL_GetWindowID(window);
            e.text.text = ascii[int(c)];
        } else {
            continue;
        }
        SDL_PushEvent(&e);
    }
}

int WindowSDL::PollTextInput(std::string& out) {
    std::scoped_lock lock{text_mutex};
    out = text;
    return text_state;
}

std::string WindowSDL::GetTextInputPrompt() {
    std::scoped_lock lock{text_mutex};
    return text_prompt;
}

std::string WindowSDL::GetTextInputValue() {
    std::scoped_lock lock{text_mutex};
    return text;
}

void WindowSDL::UpdateTextTitle() {
    // A password (the party flow's prompt says so) is shown as asterisks, one per character.
    std::string lower = text_prompt;
    for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
    std::string shown = text;
    if (lower.find("password") != std::string::npos) {
        shown.clear();
        for (const char c : text) {
            if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) shown += '*';
        }
    }
    const std::string title = text_active ? base_title + " \u2014 " + text_prompt + ": " + shown + "_  (Enter = OK, Esc = cancel)"
                                          : base_title;
    SDL_SetWindowTitle(window, title.c_str());
    BbOverlay::SetTextPrompt(text_active, text_prompt, shown);
}

bool WindowSDL::PollEvents() {
    {
        std::scoped_lock lock{text_mutex};
        if (text_requested) { // SDL text input must be toggled from the window thread
            text_requested = false;
            text_active = true;
            SDL_StartTextInput(window);
            UpdateTextTitle();
        }
    }
    if (!text_active) {
        BbOverlay::UpdateTextInput(window);
    }
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_MOUSE_MOTION) {
            last_mouse_motion_ms = SDL_GetTicks();
        }
        if (text_active && (event.type == SDL_EVENT_TEXT_INPUT || event.type == SDL_EVENT_KEY_DOWN)) {
            std::scoped_lock lock{text_mutex};
            if (event.type == SDL_EVENT_TEXT_INPUT) {
                text += event.text.text;
            } else if (event.key.key == SDLK_V && (event.key.mod & SDL_KMOD_CTRL)) {
                // Ctrl+V: the clipboard's first line (a party code, an address).
                if (char* clip = SDL_GetClipboardText()) {
                    std::string line = clip;
                    SDL_free(clip);
                    line = line.substr(0, line.find_first_of("\r\n"));
                    if (line.size() > 256) line.resize(256);
                    text += line;
                }
            } else if (event.key.key == SDLK_BACKSPACE && !text.empty()) {
                size_t cut = text.size() - 1; // drop one UTF-8 code point
                while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
                text.erase(cut);
            } else if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER || event.key.key == SDLK_ESCAPE) {
                text_state = event.key.key == SDLK_ESCAPE ? 2 : 1;
                text_active = false;
                SDL_StopTextInput(window);
            }
            UpdateTextTitle();
            continue;
        }
        // Controller-only players: Cross (A) confirms the name, Circle (B) cancels (from
        // Supermedo/bloodborne_pc 1.4). The pad is held neutral while the box is open.
        if (text_active && event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
            (event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH ||
             event.gbutton.button == SDL_GAMEPAD_BUTTON_EAST)) {
            std::scoped_lock lock{text_mutex};
            text_state = event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH ? 1 : 2;
            text_active = false;
            SDL_StopTextInput(window);
            UpdateTextTitle();
            continue;
        }
        // Alt+Enter: switch between windowed and fullscreen at any time.
        if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && (event.key.mod & SDL_KMOD_ALT) &&
            (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER)) {
            const bool fullscreen = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
            SDL_SetWindowFullscreen(window, !fullscreen);
            continue;
        }
        // Mouse & keyboard mode: a release always ends the press, also when the menu takes it.
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
            std::scoped_lock lock{mouse_mutex};
            mouse_buttons &= ~MouseButtonBit(event.button.button);
        }
        if (BbOverlay::HandleEvent(event)) {
            continue;
        }
        // Mouse & keyboard mode (from Mrsuss60/bloodborne_pc_windows_port): motion and presses
        // for runtime_pad.c, never while the menu or the name box has the input.
        if ((event.type == SDL_EVENT_MOUSE_MOTION || event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) &&
            BbSettings::Get().mk_enabled && !text_active && !BbOverlay::CapturesInput()) {
            std::scoped_lock lock{mouse_mutex};
            // Direct camera: the BbMouse thread turns motion into camera angles, so it must not
            // also reach the stick, except while locked on, where a flick switches targets.
            const Core::BloodborneCam& cam = Core::BloodborneCam::Instance();
            if (event.type == SDL_EVENT_MOUSE_MOTION && (!cam.IsEnabled() || cam.IsLockedOn())) {
                mouse_dx += event.motion.xrel;
                mouse_dy += event.motion.yrel;
            } else {
                mouse_buttons |= MouseButtonBit(event.button.button);
            }
        }
        // The wheel (upstream 0.51, #52): notches for runtime_pad.c (d-pad up / down, quick items).
        if (event.type == SDL_EVENT_MOUSE_WHEEL && BbSettings::Get().mk_enabled && !text_active &&
            !BbOverlay::CapturesInput()) {
            const float notches = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y : event.wheel.y;
            if (notches != 0.0f) {
                std::scoped_lock lock{mouse_mutex};
                (notches > 0.0f ? wheel_up : wheel_down) += 1;
            }
        }
        switch (event.type) {
        case SDL_EVENT_WINDOW_FOCUS_LOST:
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            if (mute_unfocused) {
                audible = event.type == SDL_EVENT_WINDOW_FOCUS_GAINED;
            }
            if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
                std::scoped_lock lock{mouse_mutex};
                mouse_dx = mouse_dy = 0.0f;
                mouse_buttons = 0;
                wheel_up = wheel_down = 0;
            }
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_RESIZED: {
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(window, &w, &h);
            width = w;
            height = h;
            break;
        }
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            is_open = false;
            break;
        default:
            break;
        }
    }
    UpdateMouseMode();
    UpdateMouseCamera();
    UpdateCursor();
    return is_open;
}

// Direct 1:1 mouse camera (mk_direct_camera; from Supermedo/bloodborne_pc edd20a5, imedved's
// Bloodborne mouse camera ported by mcrib884): hooked while the mouse is captured in mouse &
// keyboard mode, unhooked otherwise so the stick drives the camera again. If the game's camera
// code is not found it logs once and stays on the mouse-to-stick look.
void WindowSDL::UpdateMouseCamera() {
    Core::BloodborneCam& cam = Core::BloodborneCam::Instance();
    const auto& s = BbSettings::Get();
    const bool wanted = s.mk_enabled.load() && s.mk_direct_camera.load();
    cam.Configure(wanted, 1.0f);
    if (!wanted) {
        cam_hook_failed = false;
        if (cam.IsEnabled()) {
            cam.SetEnabled(false);
        }
    } else if (!cam.IsEnabled() && !cam_hook_failed && relative_mouse) {
        if (!mouse_sampler_started) {
            BbMouse::Start();
            mouse_sampler_started = true;
        }
        cam_hook_failed = !cam.SetEnabled(true);
    }
    if (mouse_sampler_started) {
        BbMouse::SetActive(relative_mouse && cam.IsEnabled());
    }
}

// Mouse & keyboard mode: while the game has the input (menu and name box closed, window
// focused) the mouse is in relative mode, captured by the window and turning the camera; the menu
// releases it. Off: nothing changes (the cursor is only hidden, UpdateCursor).
void WindowSDL::UpdateMouseMode() {
    const bool mk = BbSettings::Get().mk_enabled.load();
    const bool focused = (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
    const bool want = mk && focused && !text_active && !BbOverlay::CapturesInput();
    if (want != relative_mouse) {
        relative_mouse = want;
        SDL_SetWindowRelativeMouseMode(window, want);
    }
    if (!want) {
        std::scoped_lock lock{mouse_mutex};
        mouse_dx = mouse_dy = 0.0f;
        mouse_buttons = 0;
        wheel_up = wheel_down = 0;
    }
    // The launcher's mouse & keyboard dialog may change the tuning while the game runs.
    const u64 now = SDL_GetTicks();
    if (mk && now - last_tuning_check_ms >= 1000 && !BbOverlay::MenuOpen()) {
        last_tuning_check_ms = now;
        BbSettings::ReloadMouseTuning();
    }
}

void WindowSDL::TakeWheel(int& up, int& down) {
    std::scoped_lock lock{mouse_mutex};
    up = wheel_up;
    down = wheel_down;
    wheel_up = wheel_down = 0;
}

void WindowSDL::TestKey(u32 key) {
    for (const bool down : {true, false}) {
        SDL_Event e;
        SDL_zero(e);
        e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        e.key.windowID = SDL_GetWindowID(window);
        e.key.key = SDL_Keycode(key);
        e.key.scancode = SDL_GetScancodeFromKey(SDL_Keycode(key), nullptr);
        e.key.down = down;
        SDL_PushEvent(&e);
    }
}

void WindowSDL::ReadMouse(float& dx, float& dy, u32& buttons, bool consume) {
    std::scoped_lock lock{mouse_mutex};
    dx = mouse_dx;
    dy = mouse_dy;
    buttons = mouse_buttons;
    if (consume) {
        mouse_dx = mouse_dy = 0.0f;
    }
}

// Issue #3: the OS cursor over the game. bbport (Windows fork): the game takes no pointer input
// (mouse & keyboard mode reads relative motion, UpdateMouseMode), so the cursor shows only while the overlay captures input (settings menu or text dialog) and is
// hidden otherwise, windowed or fullscreen. Checked every poll because the menu can also close
// from the present thread. ImGui never draws its own cursor (MouseDrawCursor stays off).
void WindowSDL::UpdateCursor() {
    const bool hide = !BbOverlay::CapturesInput();
    if (hide != cursor_hidden) {
        cursor_hidden = hide;
        hide ? SDL_HideCursor() : SDL_ShowCursor();
    }
}

} // namespace Frontend

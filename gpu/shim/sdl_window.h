// bbport: the game window. Created by the VideoOut driver on first open; the
// event pump runs on the port's window thread (see window.cpp).
#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include "common/types.h"
#include "mouse_buttons.h"

struct SDL_Window;

namespace Frontend {

enum class WindowSystemType : u8 { Headless, Windows, X11, Wayland, Metal };

struct WindowSystemInfo {
    void* display_connection = nullptr;
    void* render_surface = nullptr;
    float render_surface_scale = 1.0f;
    WindowSystemType type = WindowSystemType::Headless;
};

class WindowSDL {
public:
    WindowSDL(s32 width, s32 height, const char* title);
    ~WindowSDL();
    s32 GetWidth() const { return width.load(std::memory_order_relaxed); }
    s32 GetHeight() const { return height.load(std::memory_order_relaxed); }
    SDL_Window* GetSDLWindow() const { return window; }
    WindowSystemInfo GetWindowInfo() const { return window_info; }
    bool IsOpen() const { return is_open.load(std::memory_order_relaxed); }
    /// Windows only: the window is on a display that Windows runs in HDR, where the desktop
    /// decodes an SDR swapchain with the sRGB curve instead of the display's gamma 2.2.
    bool IsOnHdrDisplay() const { return on_hdr_display.load(std::memory_order_relaxed); }
    /// Processes pending window events. Returns false once the user closed the window.
    bool PollEvents();
    /// Keyboard text entry for the system IME dialog; typed text shows in the title bar.
    /// `choices`: a pick list (value, label) shown above the box - Up / Down (keyboard, d-pad) move the
    /// selection, which sets the text, Enter / Cross confirm; typing or Backspace leaves the list for the
    /// keyboard. Empty: the plain box. (Chalice glyphs, docs/party/chalice.md 11.)
    /// A choice with a `tab` name (the community Chalice list: Blood gems, Equipment...) is shown under that tab;
    /// with two or more tabs, Left / Right (keyboard) or L1 / R1 (controller) switch tab, D-pad Left / Right and
    /// PageUp / PageDown jump ten rows.
    struct TextChoice {
        std::string value, label, tab;
    };
    void BeginTextInput(const std::string& initial, const std::string& prompt, const std::vector<TextChoice>& choices = {});
    /// Test aid (any thread): keys (TestType's) the next text box takes as soon as it opens.
    void QueueTestKeys(const std::string& keys);
    /// 0 while typing, 1 confirmed (Enter), 2 cancelled (Escape); text is UTF-8.
    int PollTextInput(std::string& text);
    bool IsTextInputActive() const { return text_active; }
    /// Test aid (any thread): queues SDL key events into this window's event queue as if typed - each
    /// ASCII character a text-input event, '\n' Enter, '\x1b' Escape, '\b' Backspace - so the text box
    /// takes them through PollEvents like the keyboard's. No OS input, no focus needed.
    void TestType(const std::string& keys);
    /// False while the window is in the background (BB_MUTE_UNFOCUSED, default on): audio muted.
    bool IsAudible() const { return audible.load(std::memory_order_relaxed); }
    std::string GetTextInputPrompt();
    std::string GetTextInputValue();
    /// Mouse & keyboard mode (any thread): mouse motion since the last consuming call (pixels)
    /// and the buttons held (bit 0 left, 1 right, 2 middle, 3 X1, 4 X2).
    void ReadMouse(float& dx, float& dy, u32& buttons, bool consume);
    /// Wheel notches since the last call (taken with a consuming ReadMouse, upstream 0.51 #52).
    void TakeWheel(int& up, int& down);
    /// Test aid (any thread): a key press and release queued as if typed (no OS input, no focus).
    void TestKey(u32 key);

private:
    std::atomic<s32> width, height;
    std::atomic<bool> is_open{true};
    std::atomic<bool> audible{true};
    bool mute_unfocused{true};
    std::atomic<bool> on_hdr_display{false};
    float sdr_white_nits{80.0f};
    void UpdateHdrLuminance();
    std::mutex text_mutex;
    bool text_requested{}, text_active{};
    int text_state{};
    std::string text, text_prompt, base_title;
    std::vector<std::pair<std::string, std::string>> text_choices;  ///< the shown tab's rows (all rows without tabs)
    std::vector<TextChoice> text_all;
    std::vector<std::string> text_tabs;  ///< tab names in order of appearance (empty: no tabs)
    int text_tab{0};
    void SelectTab(int tab);  ///< text_mutex held
    int text_sel{-1};  ///< the selected choice, -1: typing
    std::string text_test_keys;
    void UpdateChoices();
    void UpdateTextTitle();
    void UpdateCursor();
    void UpdateMouseMode();
    /// Direct mouse camera (bloodborne_cam.h): hooks/unhooks it to match mk_direct_camera.
    void UpdateMouseCamera();
    int cam_hook_tries{};         ///< failed direct-camera hooks since it was turned on (max 12)
    std::uint64_t cam_hook_next_ms{}; ///< SDL ticks of the next try (every 5 s: the game code may load later)
    bool mouse_sampler_started{}; ///< BbMouse thread runs (started on first direct use)
    u64 last_mouse_motion_ms{}; ///< SDL_GetTicks of the last mouse motion (UpdateCursor)
    bool cursor_hidden{};
    // Mouse & keyboard mode: relative mouse while the game has the input (UpdateMouseMode).
    std::mutex mouse_mutex;
    float mouse_dx{}, mouse_dy{};
    BbMouseButtons::Latch mouse_latch;
    int wheel_up{}, wheel_down{};
    bool relative_mouse{};
    /// SDL_GetTicks when the window last got the focus: the click that brought it to the front
    /// is not a game press (capture-click discard, Mrsuss60/bloodborne_pc_windows_port 90008b39).
    u64 focus_gained_ms{};
    u64 last_tuning_check_ms{};
    SDL_Window* window{};
    WindowSystemInfo window_info{};
};

} // namespace Frontend

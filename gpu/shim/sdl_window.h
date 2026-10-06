// bbport: the game window. Created by the VideoOut driver on first open; the
// event pump runs on the port's window thread (see window.cpp).
#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include "common/types.h"

struct SDL_Window;
union SDL_Event;

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
    bool IsMinimized() const { return is_minimized.load(std::memory_order_relaxed); }
    /// Processes pending window events. Returns false once the user closed the window.
    bool PollEvents();

private:
    std::atomic<s32> width, height;
    std::atomic<bool> is_open{true};
    std::atomic<bool> is_minimized{false};
    std::string base_title;
    /// Cursor hiding (hide_cursor): shown on mouse motion, hidden after a pause or a pad input.
    void UpdateCursor(const union SDL_Event* event);
    u64 last_mouse_motion_ms{};
    bool cursor_hidden{};
    /// Screen mode (fullscreen, maximized settings): 0 window, 1 maximised, 2 full screen.
    void ApplyScreenMode();
    int screen_mode{};
    bool hidden_window{};
    SDL_Window* window{};
    WindowSystemInfo window_info{};
};

} // namespace Frontend

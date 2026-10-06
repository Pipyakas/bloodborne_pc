// bbport: SDL3 window for the Vulkan swapchain (X11, Wayland or Win32).
#include <cstdlib>
#include <cstring>
#include <SDL3/SDL.h>
#include "common/assert.h"
#include "common/logging/log.h"
#include "sdl_window.h"
#include "bbport_native_settings.h"
#include "bbport_overlay.h"
#include "bbport_settings.h"

namespace Frontend {

WindowSDL::WindowSDL(s32 width_, s32 height_, const char* title) : width{width_}, height{height_} {
    // Gamepads are sampled by runtime_pad.c; their events are pumped here with the window's.
    // SDL drops gamepad state while another window has focus unless asked not to.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,
                BbSettings::Get().background_gamepad ? "1" : "0");
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        UNREACHABLE_MSG("Failed to initialize SDL video: {}", SDL_GetError());
    }
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width_);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height_);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN, true);
    // BB_HIDDEN=1: the window is never shown (agents driving the game through BB_CONTROL while
    // the desktop is in use); the swapchain keeps the window's size.
    const char* hidden = std::getenv("BB_HIDDEN");
    const char* minimized = std::getenv("BB_MINIMIZED");
    const char* fullscreen = std::getenv("BB_FULLSCREEN");
    hidden_window = hidden && hidden[0] == '1';
    if (hidden_window) {
        // Background runs (BB_HIDDEN): never shown, maximised or made full screen.
        SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN, true);
    } else if (minimized && minimized[0] == '1') {
        // Create minimized, rather than showing then minimizing (which can steal focus or
        // flash on the desktop). SDL's Win32 backend uses SW_SHOWMINNOACTIVE here.
        // Ignore saved fullscreen settings; the user can restore this window from the taskbar.
        SDL_SetHintWithPriority(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0", SDL_HINT_OVERRIDE);
        SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_MINIMIZED_BOOLEAN, true);
    } else {
        const bool full = fullscreen ? fullscreen[0] == '1' : BbSettings::Get().fullscreen.load();
        SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN, full);
        SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_MAXIMIZED_BOOLEAN,
                               BbSettings::Get().maximized.load());
        BbSettings::Get().fullscreen = full; // BB_FULLSCREEN: this start's mode
    }
    screen_mode = BbSettings::Get().fullscreen ? 2 : BbSettings::Get().maximized ? 1 : 0;
    base_title = title;
    window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    ASSERT_MSG(window, "Failed to create window: {}", SDL_GetError());
    is_minimized = (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) != 0;

    const char* driver = SDL_GetCurrentVideoDriver();
    const SDL_PropertiesID wp = SDL_GetWindowProperties(window);
#ifdef _WIN32
    if (driver && !std::strcmp(driver, "windows")) {
        window_info.type = WindowSystemType::Windows;
        window_info.render_surface = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    } else
#endif
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
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    width = w;
    height = h;
    LOG_INFO(Frontend, "Window {}x{} on {}", w, h, driver);
}

WindowSDL::~WindowSDL() {
    SDL_DestroyWindow(window);
}

bool WindowSDL::PollEvents() {
    // The system text dialog is an ImGui overlay (bbport_overlay.cpp): it draws a centered
    // input box over a dimmed frame and takes the keyboard and mouse while it is open.
    BbOverlay::UpdateTextInput(window);
    BbNative::Poll(); // the game's options screen edits the port's settings
    ApplyScreenMode();
    SDL_Event event;
    UpdateCursor(nullptr);
    while (SDL_PollEvent(&event)) {
        UpdateCursor(&event);
        if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED || event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
            BbSettings::Get().window_focused = event.type == SDL_EVENT_WINDOW_FOCUS_GAINED;
        }
        if (BbOverlay::HandleEvent(event)) {
            continue;
        }
        switch (event.type) {
        case SDL_EVENT_WINDOW_MINIMIZED:
            is_minimized = true;
            break;
        case SDL_EVENT_WINDOW_RESTORED:
        case SDL_EVENT_WINDOW_MAXIMIZED:
            is_minimized = false;
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_RESIZED: {
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(window, &w, &h);
            width = w;
            height = h;
            break;
        }
        case SDL_EVENT_KEY_DOWN:
            // F11: borderless fullscreen at the desktop size, or back to the window.
            if (event.key.key == SDLK_F11 && !event.key.repeat) {
                auto& s = BbSettings::Get();
                s.fullscreen = !s.fullscreen;
                BbSettings::Save();
            }
            break;
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            is_open = false;
            break;
        default:
            break;
        }
    }
    return is_open;
}

void WindowSDL::UpdateCursor(const SDL_Event* event) {
    constexpr u64 IdleMs = 500;
    // Stick motion below this (of 32767) is drift, not input.
    constexpr int AxisThreshold = 8000;
    const u64 now = SDL_GetTicks();
    bool show = false, hide = false;
    if (!event) {
        hide = now - last_mouse_motion_ms >= IdleMs;
    } else if (event->type == SDL_EVENT_MOUSE_MOTION || event->type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
               event->type == SDL_EVENT_MOUSE_WHEEL) {
        last_mouse_motion_ms = now;
        show = true;
    } else if (event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ||
               (event->type == SDL_EVENT_GAMEPAD_AXIS_MOTION &&
                (event->gaxis.value > AxisThreshold || event->gaxis.value < -AxisThreshold))) {
        hide = true;
    }
    if (!BbSettings::Get().hide_cursor) {
        hide = false;
        show = cursor_hidden;
    }
    if (hide && !cursor_hidden) {
        SDL_HideCursor();
        cursor_hidden = true;
    } else if (show && cursor_hidden) {
        SDL_ShowCursor();
        cursor_hidden = false;
    }
}

void WindowSDL::ApplyScreenMode() {
    const auto& s = BbSettings::Get();
    const int want = s.fullscreen ? 2 : s.maximized ? 1 : 0;
    if (want == screen_mode || hidden_window) return;
    screen_mode = want;
    if (want == 2) {
        SDL_SetWindowFullscreen(window, true);
        return;
    }
    SDL_SetWindowFullscreen(window, false);
    if (want == 1) {
        SDL_MaximizeWindow(window);
    } else {
        SDL_RestoreWindow(window);
    }
}

} // namespace Frontend

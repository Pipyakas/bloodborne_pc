// SPDX-License-Identifier: GPL-2.0-or-later
// Shared, readable controller targets for both ImGui contexts (game and first launch).
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <SDL3/SDL.h>
#include "imgui.h"

namespace BbImGui {
constexpr float FontSize = 28.0f; // pixels at 1080p; renderer/DPI scale is applied separately

inline float AxisValue(Sint16 value) {
    const float raw = float(value);
    constexpr float deadzone = 8000.0f;
    return raw < 0 ? -std::clamp((-raw - deadzone) / (32768.0f - deadzone), 0.0f, 1.0f)
                   : std::clamp((raw - deadzone) / (32767.0f - deadzone), 0.0f, 1.0f);
}

inline void Directions(uint32_t buttons, const std::array<float, SDL_GAMEPAD_AXIS_COUNT>& axes) {
    auto& io = ImGui::GetIO();
    const auto direction = [&](ImGuiKey key, int button, float value) {
        value = std::max(value, (buttons & (uint32_t(1) << button)) ? 1.0f : 0.0f);
        io.AddKeyAnalogEvent(key, value > 0, value);
    };
    // ImGui uses D-pad keys for focus movement and LStick keys for scrolling. Translate
    // left stick into the former and right stick into the latter (without clobbering a
    // physical D-pad held at the same time).
    direction(ImGuiKey_GamepadDpadLeft, SDL_GAMEPAD_BUTTON_DPAD_LEFT, -axes[SDL_GAMEPAD_AXIS_LEFTX]);
    direction(ImGuiKey_GamepadDpadRight, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, axes[SDL_GAMEPAD_AXIS_LEFTX]);
    direction(ImGuiKey_GamepadDpadUp, SDL_GAMEPAD_BUTTON_DPAD_UP, -axes[SDL_GAMEPAD_AXIS_LEFTY]);
    direction(ImGuiKey_GamepadDpadDown, SDL_GAMEPAD_BUTTON_DPAD_DOWN, axes[SDL_GAMEPAD_AXIS_LEFTY]);
    const auto scroll = [&](ImGuiKey key, float value) {
        value = std::max(value, 0.0f);
        io.AddKeyAnalogEvent(key, value > 0, value);
    };
    scroll(ImGuiKey_GamepadLStickLeft, -axes[SDL_GAMEPAD_AXIS_RIGHTX]);
    scroll(ImGuiKey_GamepadLStickRight, axes[SDL_GAMEPAD_AXIS_RIGHTX]);
    scroll(ImGuiKey_GamepadLStickUp, -axes[SDL_GAMEPAD_AXIS_RIGHTY]);
    scroll(ImGuiKey_GamepadLStickDown, axes[SDL_GAMEPAD_AXIS_RIGHTY]);
}

// SDL's platform backend opens the gamepads and supplies the other navigation keys.
// Apply the same stick convention to the first-launch context after its backend NewFrame.
inline void PollDirections() {
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    uint32_t buttons = 0;
    std::array<float, SDL_GAMEPAD_AXIS_COUNT> axes{};
    for (int i = 0; i < count; ++i) {
        SDL_Gamepad* pad = SDL_GetGamepadFromID(ids[i]);
        if (!pad) continue;
        for (int button = SDL_GAMEPAD_BUTTON_DPAD_UP; button <= SDL_GAMEPAD_BUTTON_DPAD_RIGHT; ++button)
            if (SDL_GetGamepadButton(pad, SDL_GamepadButton(button))) buttons |= uint32_t(1) << button;
        for (int axis = 0; axis < SDL_GAMEPAD_AXIS_COUNT; ++axis) {
            const float value = AxisValue(SDL_GetGamepadAxis(pad, SDL_GamepadAxis(axis)));
            if (std::abs(value) > std::abs(axes[axis])) axes[axis] = value;
        }
    }
    SDL_free(ids);
    Directions(buttons, axes);
}

inline void ControllerStyle() {
    auto& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigNavCursorVisibleAlways = true;
    ImGui::StyleColorsDark();
    auto& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(24.0f, 20.0f);
    style.FramePadding = ImVec2(16.0f, 10.0f);
    style.ItemSpacing = ImVec2(14.0f, 12.0f);
    style.ItemInnerSpacing = ImVec2(12.0f, 8.0f);
    style.ScrollbarSize = 24.0f;
    style.GrabMinSize = 24.0f;
    style.WindowRounding = 8.0f;
    style.FrameRounding = 5.0f;
    style.GrabRounding = 5.0f;
    style.WindowBorderSize = 1.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.07f, 0.08f, 0.96f);
    style.Colors[ImGuiCol_Border] = ImVec4(0.55f, 0.48f, 0.32f, 0.8f);
    style.Colors[ImGuiCol_NavCursor] = ImVec4(1.0f, 0.82f, 0.35f, 1.0f);
    style.Colors[ImGuiCol_Button] = ImVec4(0.22f, 0.20f, 0.17f, 1.0f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.38f, 0.32f, 0.22f, 1.0f);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.50f, 0.42f, 0.27f, 1.0f);
    style.Colors[ImGuiCol_CheckMark] = ImVec4(0.95f, 0.78f, 0.36f, 1.0f);
}
} // namespace BbImGui

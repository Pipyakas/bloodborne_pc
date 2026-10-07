// SPDX-License-Identifier: GPL-2.0-or-later
// Headless regression test: uses the real dialog widget and ImGui input queue.
#include "../gpu/shim/bbport_overlay.cpp"
#include <cassert>
#include "imgui_internal.h"

static void frame() {
    BbOverlay::DrawUi();
}

static void button(Uint8 code, bool down) {
    SDL_Event event{};
    event.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
    event.gbutton.button = code;
    BbOverlay::HandleEvent(event);
    frame();
}

static void press(Uint8 code) {
    button(code, true);
    button(code, false);
    frame();
}

static void axis(Uint8 code, Sint16 value) {
    SDL_Event event{};
    event.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
    event.gaxis.axis = code;
    event.gaxis.value = value;
    BbOverlay::HandleEvent(event);
    frame();
}

int main() {
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1024, 576);
    io.DeltaTime = 1.0f / 60.0f;
    BbImGui::ControllerStyle();
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    BbOverlay::initialized = true;
    std::string out;

    assert(BbOverlay::BeginTextInput("", "Name"));
    frame();
    frame(); // ImGui applies the focus request on the next frame
    io.AddInputCharactersUTF8("Hunter");
    frame();
    assert(BbOverlay::PollTextInput(out) == 0 && out == "Hunter");
    io.AddKeyEvent(ImGuiKey_Enter, true);
    frame();
    assert(!BbOverlay::TextInputActive());
    assert(BbOverlay::PollTextInput(out) == 1 && out == "Hunter");
    assert(BbOverlay::PollTextInput(out) == 1 && out == "Hunter");
    assert(!BbOverlay::SubmitText("too late"));
    io.AddKeyEvent(ImGuiKey_Enter, false);
    frame();

    // A new request resets completion; multibyte input survives copying back from ImGui.
    assert(BbOverlay::BeginTextInput("", "Name"));
    frame();
    frame();
    io.AddInputCharactersUTF8("\xD0\x90\xD0\xBD\xD0\xBD\xD0\xB0"); // Anna, Cyrillic
    frame();
    assert(BbOverlay::PollTextInput(out) == 0 && out == "\xD0\x90\xD0\xBD\xD0\xBD\xD0\xB0");
    BbOverlay::FinishTextInput(1); // same completion path as clicking OK
    assert(BbOverlay::PollTextInput(out) == 1 && out == "\xD0\x90\xD0\xBD\xD0\xBD\xD0\xB0");

    assert(BbOverlay::BeginTextInput("Old name", "Name"));
    frame();
    frame();
    io.AddInputCharactersUTF8(" edited");
    frame();
    io.AddKeyEvent(ImGuiKey_Escape, true);
    frame();
    assert(!BbOverlay::TextInputActive());
    assert(BbOverlay::PollTextInput(out) == 2 && out == "Old name");
    io.AddKeyEvent(ImGuiKey_Escape, false);
    frame();

    assert(BbOverlay::BeginTextInput("Old name", "Name"));
    assert(BbOverlay::SubmitText("New name"));
    assert(BbOverlay::PollTextInput(out) == 1 && out == "New name");
    assert(BbOverlay::BeginTextInput("", "Name"));
    assert(BbOverlay::SubmitText(std::string(300, 'X')));
    assert(BbOverlay::PollTextInput(out) == 1 && out == std::string(255, 'X'));

    // Real SDL button/axis events, not direct changes to ImGui's navigation state.
    press(SDL_GAMEPAD_BUTTON_SOUTH); // remember controller input before opening the next request
    assert(BbOverlay::BeginTextInput("", "Name"));
    frame();
    frame();
    assert(BbOverlay::PollTextInput(out) == 0 && out.empty());
    press(SDL_GAMEPAD_BUTTON_SOUTH);
    assert(BbOverlay::PollTextInput(out) == 0 && out == "q");
    press(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    press(SDL_GAMEPAD_BUTTON_SOUTH);
    assert(BbOverlay::PollTextInput(out) == 0 && out == "qw");
    axis(SDL_GAMEPAD_AXIS_LEFTX, 32767);
    axis(SDL_GAMEPAD_AXIS_LEFTX, 0);
    frame();
    press(SDL_GAMEPAD_BUTTON_SOUTH);
    assert(BbOverlay::PollTextInput(out) == 0);
    assert(out == "qwe");
    button(SDL_GAMEPAD_BUTTON_START, true);
    assert(BbOverlay::PollTextInput(out) == 1 && out == "qwe");
    assert(BbOverlay::CapturesInput()); // do not leak the held confirm to the game
    button(SDL_GAMEPAD_BUTTON_START, false);
    assert(!BbOverlay::CapturesInput());

    assert(BbOverlay::BeginTextInput("\xD0\x90\xD0\xBD\xD0\xBD\xD0\xB0", "Name"));
    frame();
    frame();
    for (int i = 0; i < 3; ++i) press(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    press(SDL_GAMEPAD_BUTTON_SOUTH);
    assert(BbOverlay::text_dialog.uppercase);
    press(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    press(SDL_GAMEPAD_BUTTON_SOUTH); // Space
    assert(BbOverlay::PollTextInput(out) == 0 && out == "\xD0\x90\xD0\xBD\xD0\xBD\xD0\xB0 ");
    press(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    press(SDL_GAMEPAD_BUTTON_SOUTH); // Delete space
    press(SDL_GAMEPAD_BUTTON_SOUTH); // Delete a whole UTF-8 character
    assert(BbOverlay::PollTextInput(out) == 0 && out == "\xD0\x90\xD0\xBD\xD0\xBD");
    press(SDL_GAMEPAD_BUTTON_START);

    assert(BbOverlay::BeginTextInput("Original", "Name"));
    frame();
    frame();
    press(SDL_GAMEPAD_BUTTON_SOUTH);
    button(SDL_GAMEPAD_BUTTON_EAST, true);
    assert(BbOverlay::PollTextInput(out) == 2 && out == "Original");
    button(SDL_GAMEPAD_BUTTON_EAST, false);
    assert(!BbOverlay::CapturesInput());

    BbSettings::Get().overlay_docked = false;
    button(SDL_GAMEPAD_BUTTON_LEFT_STICK, true);
    button(SDL_GAMEPAD_BUTTON_RIGHT_STICK, true);
    assert(BbOverlay::menu_open);
    press(SDL_GAMEPAD_BUTTON_DPAD_DOWN); // held chord must not toggle again on another button
    assert(BbOverlay::menu_open);
    button(SDL_GAMEPAD_BUTTON_LEFT_STICK, false);
    button(SDL_GAMEPAD_BUTTON_RIGHT_STICK, false);
    frame();
    const ImGuiID before = GImGui->NavId;
    assert(before != 0);
    press(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    assert(GImGui->NavId != before);
    axis(SDL_GAMEPAD_AXIS_LEFTY, 32767);
    axis(SDL_GAMEPAD_AXIS_LEFTY, 0);
    assert(io.KeysData[ImGuiKey_GamepadLStickDown - ImGuiKey_NamedKey_BEGIN].AnalogValue == 0.0f);
    press(SDL_GAMEPAD_BUTTON_EAST);
    assert(!BbOverlay::menu_open && !BbOverlay::CapturesInput());

    BbOverlay::SetOpen(true);
    frame();
    frame();
    press(SDL_GAMEPAD_BUTTON_SOUTH);
    assert(GImGui->OpenPopupStack.Size > 0);
    press(SDL_GAMEPAD_BUTTON_EAST);
    assert(GImGui->OpenPopupStack.Size == 0 && BbOverlay::menu_open);
    const float scroll_before = GImGui->NavWindow->Scroll.y;
    axis(SDL_GAMEPAD_AXIS_RIGHTY, 32767);
    for (int i = 0; i < 20; ++i) frame();
    axis(SDL_GAMEPAD_AXIS_RIGHTY, 0);
    assert(GImGui->NavWindow->Scroll.y > scroll_before);
    press(SDL_GAMEPAD_BUTTON_EAST);
    assert(!BbOverlay::menu_open);

    // Disconnect clears held controls/release guards rather than trapping input forever.
    assert(BbOverlay::BeginTextInput("", "Name"));
    button(SDL_GAMEPAD_BUTTON_EAST, true);
    SDL_Event removed{};
    removed.type = SDL_EVENT_GAMEPAD_REMOVED;
    BbOverlay::HandleEvent(removed);
    assert(!BbOverlay::CapturesInput());

    // The first-launch polling path must not alternate pressed/released events while a
    // direction is held. Use an actual SDL virtual gamepad, not a hand-written key state.
    assert(SDL_Init(SDL_INIT_GAMEPAD));
    SDL_VirtualJoystickDesc desc{};
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.axis_mask = (uint32_t(1) << SDL_GAMEPAD_AXIS_COUNT) - 1;
    desc.button_mask = (uint32_t(1) << SDL_GAMEPAD_BUTTON_COUNT) - 1;
    desc.name = "bbport navigation test";
    const SDL_JoystickID virtual_id = SDL_AttachVirtualJoystick(&desc);
    assert(virtual_id);
    SDL_Gamepad* pad = SDL_OpenGamepad(virtual_id);
    assert(pad);
    SDL_Joystick* joystick = SDL_GetGamepadJoystick(pad);
    assert(SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFTX, 32767));
    SDL_UpdateJoysticks();
    float duration = -1.0f;
    for (int i = 0; i < 6; ++i) {
        BbImGui::PollGamepad(pad);
        frame();
        const auto& key = io.KeysData[ImGuiKey_GamepadDpadRight - ImGuiKey_NamedKey_BEGIN];
        assert(key.Down && key.DownDuration > duration);
        duration = key.DownDuration;
    }
    assert(SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFTX, 0));
    assert(SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_DPAD_DOWN, true));
    SDL_UpdateJoysticks();
    BbImGui::PollGamepad(pad);
    frame();
    assert(!io.KeysData[ImGuiKey_GamepadDpadRight - ImGuiKey_NamedKey_BEGIN].Down);
    assert(io.KeysData[ImGuiKey_GamepadDpadDown - ImGuiKey_NamedKey_BEGIN].Down);
    SDL_CloseGamepad(pad);
    assert(SDL_DetachVirtualJoystick(virtual_id));
    BbImGui::PollGamepad(nullptr);
    frame();
    assert(!(io.BackendFlags & ImGuiBackendFlags_HasGamepad));
    assert(!io.KeysData[ImGuiKey_GamepadDpadDown - ImGuiKey_NamedKey_BEGIN].Down);
    SDL_Quit();

    ImGui::DestroyContext();
    std::puts("PASS: text entry, gamepad keyboard, stick/D-pad navigation, menu toggle/back and input isolation");
}

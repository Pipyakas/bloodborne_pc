// SPDX-License-Identifier: GPL-2.0-or-later
// Headless regression test: uses the real dialog widget and ImGui input queue.
#include "../gpu/shim/bbport_overlay.cpp"
#include <cassert>

static void frame() {
    ImGui::NewFrame();
    if (BbOverlay::TextInputActive()) BbOverlay::TextDialogWindow();
    ImGui::Render();
}

int main() {
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1024, 576);
    io.DeltaTime = 1.0f / 60.0f;
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

    ImGui::DestroyContext();
    std::puts("PASS: text entry typing, confirm, cancel, reopen, UTF-8 and control submission");
}

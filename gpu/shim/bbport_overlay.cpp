// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_overlay.h"

#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

#include <SDL3/SDL.h>
#include "bbport_settings.h"
#include "bbport_imgui.h"
#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

// DejaVu Sans (Cyrillic), embedded (third_party/fonts, Bitstream Vera license).
#ifdef _WIN32
// PE/COFF assemblers have no .hidden/.previous: the compiler embeds the file (#embed, a GCC
// extension in C++).
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
alignas(16) static const unsigned char bb_font_ttf[] = {
#embed BB_FONT_PATH
};
#pragma GCC diagnostic pop
static const unsigned char* const bb_font_ttf_end = bb_font_ttf + sizeof(bb_font_ttf);
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
extern "C" const unsigned char bb_font_ttf[];
extern "C" const unsigned char bb_font_ttf_end[];
#endif

extern "C" void runtime_restart(void); // bb-probe (probe.c)

namespace BbOverlay {

namespace {

std::mutex imgui_mutex; // the ImGui context: window thread (input) and present thread
bool initialized = false;
std::atomic<bool> menu_open{false};
bool l3_down = false, r3_down = false;
bool controller_input = false;
uint32_t controller_buttons = 0;
std::array<float, SDL_GAMEPAD_AXIS_COUNT> controller_axes{};
std::atomic<bool> input_release{false};
bool menu_focus = false;
bool dirty = false; // settings changed while open: saved on close
float base_scale = 1.0f; // UI scale factor for the display height (1080p = 1)

// System text entry (the PS4 IME dialog), drawn as an input box over a dimmed frame. The game
// thread (runtime_services.c) opens it and polls it; the window thread feeds it SDL input and
// the present thread draws it, so every access to these fields takes imgui_mutex.
struct TextDialog {
    bool open = false;          // an entry is in progress (the guest is polling)
    bool focus = false;         // focus the text field once when opened
    bool keyboard_focus = false;
    bool editing = false;
    bool uppercase = false;
    int state = 0;              // 0 typing, 1 confirmed, 2 cancelled
    std::string text;           // UTF-8, the value the guest reads
    std::string title;          // the prompt above the box
    std::string initial;        // the value to restore when cancelled
    std::size_t capacity = 255;  // the guest's buffer size in code points
    std::size_t cursor = 0;     // caret, in code points from the start
} text_dialog;
std::atomic<bool> text_input_open{false}; // readable without imgui_mutex

// Present rate for the FPS counter (Render measures the interval between presents).
std::chrono::steady_clock::time_point last_present{};
float frame_ms_avg = 0.0f;

// Code points in a UTF-8 string up to `cursor` (caret positions are code points, so multi-byte
// characters cannot be split).
std::vector<std::string> SplitCodePoints(const std::string& utf8) {
    std::vector<std::string> out;
    for (size_t i = 0; i < utf8.size();) {
        size_t n = 1;
        while (i + n < utf8.size() && (static_cast<unsigned char>(utf8[i + n]) & 0xC0) == 0x80) {
            ++n;
        }
        out.push_back(utf8.substr(i, n));
        i += n;
    }
    return out;
}

// The centered input box with a dimmed backdrop, drawn over the game frame.
// The game's IME buffer is UTF-16 with a bounded length; the dialog must not grow past it.
std::size_t TextCapacity() { return text_dialog.capacity; }

void ClampTextToCapacity() {
    const std::vector<std::string> points = SplitCodePoints(text_dialog.text);
    if (points.size() <= TextCapacity()) {
        return;
    }
    std::string kept;
    for (std::size_t i = 0; i < TextCapacity(); ++i) {
        kept += points[i];
    }
    text_dialog.text = kept;
}

void NormalizeCaret() {
    text_dialog.cursor = std::min(text_dialog.cursor, SplitCodePoints(text_dialog.text).size());
}

bool ControllerHeld() {
    if (controller_buttons) return true;
    for (float axis : controller_axes) if (std::abs(axis) > 0.0f) return true;
    return false;
}

void DeleteTextCharacter() {
    auto points = SplitCodePoints(text_dialog.text);
    if (!points.empty()) text_dialog.text.resize(text_dialog.text.size() - points.back().size());
}

void TextKeyboard() {
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float key_width = (ImGui::GetContentRegionAvail().x - gap * 9.0f) / 10.0f;
    const char* keys = text_dialog.uppercase ? "1234567890QWERTYUIOPASDFGHJKL-ZXCVBNM.,'"
                                           : "1234567890qwertyuiopasdfghjkl-zxcvbnm.,'";
    for (int i = 0; keys[i]; ++i) {
        if (i % 10) ImGui::SameLine();
        ImGui::PushID(i);
        char label[16];
        std::snprintf(label, sizeof(label), "%c###key", keys[i]);
        if (text_dialog.keyboard_focus && i == 10) {
            ImGui::SetKeyboardFocusHere();
            text_dialog.keyboard_focus = false;
        }
        if (ImGui::Button(label, ImVec2(key_width, 0.0f))) {
            text_dialog.text += keys[i];
            ClampTextToCapacity();
        }
        ImGui::PopID();
    }
    const float third = (ImGui::GetContentRegionAvail().x - gap * 2.0f) / 3.0f;
    if (ImGui::Button(text_dialog.uppercase ? "Lowercase###case" : "Uppercase###case", ImVec2(third, 0)))
        text_dialog.uppercase = !text_dialog.uppercase;
    ImGui::SameLine();
    if (ImGui::Button("Space", ImVec2(third, 0))) {
        text_dialog.text += ' ';
        ClampTextToCapacity();
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete", ImVec2(third, 0))) DeleteTextCharacter();
}

// Closes the entry with `state` (1 confirmed, 2 cancelled) and hands the result to the guest.
void FinishTextInput(int state) {
    if (state != 1) { // a cancelled entry leaves the guest's buffer as it was
        text_dialog.text = text_dialog.initial;
    }
    text_dialog.state = state;
    text_dialog.open = false;
    text_input_open = false;
    input_release = ControllerHeld();
    // The cursor follows the settings menu again (both cannot be open at once).
    ImGui::GetIO().MouseDrawCursor = menu_open;
}

// The centered input box over a dimmed frame: the PS4 system text dialog, drawn like one.
void TextDialogWindow() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    draw->AddRectFilled(viewport->WorkPos,
                        ImVec2(viewport->WorkPos.x + viewport->WorkSize.x,
                               viewport->WorkPos.y + viewport->WorkSize.y),
                        IM_COL32(0, 0, 0, 150));

    const ImVec2 center = ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                 viewport->WorkPos.y + viewport->WorkSize.y * 0.5f);
    const float width = std::min(960.0f * base_scale, viewport->WorkSize.x * 0.94f);
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(width, viewport->WorkSize.y * 0.94f));
    ImGui::SetNextWindowBgAlpha(0.98f);
    constexpr ImGuiWindowFlags Flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
                                      ImGuiWindowFlags_NoSavedSettings |
                                      ImGuiWindowFlags_AlwaysAutoResize;
    ImGui::Begin("##bbport_text_input", nullptr, Flags);
    // Centered title without PushTextAlign (this ImGui version has no text-align stack): the
    // title is drawn in the window's own horizontal offset, so a window-padding offset is
    // computed from the remaining width.
    const float title_width = ImGui::CalcTextSize(text_dialog.title.c_str()).x;
    const float title_offset = std::max(0.0f, (width - title_width) * 0.5f - ImGui::GetStyle().WindowPadding.x);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + title_offset);
    ImGui::TextUnformatted(text_dialog.title.c_str());

    // The value, in a framed field: it accepts typing directly, like the PS4 dialog, and the
    // buttons below take Enter (OK) and Escape (Cancel) the way the game's dialogs do.
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.06f, 0.06f, 0.07f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.08f, 0.08f, 0.09f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.10f, 0.10f, 0.12f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f * base_scale, 8.0f * base_scale));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.55f, 0.48f, 0.32f, 1.0f));
    // InputText needs writable, sized storage, not std::string::data() with spare capacity:
    // it changes the bytes but cannot update the string's length. Allow UTF-8's four bytes
    // per code point, then apply the character limit after copying back.
    std::array<char, 255 * 4 + 1> text{};
    std::snprintf(text.data(), text.size(), "%s", text_dialog.text.c_str());
    if (text_dialog.focus) {
        ImGui::SetKeyboardFocusHere();
        text_dialog.focus = false;
    }
    if (ImGui::InputText("##bbport_text_value", text.data(), text.size())) {
        text_dialog.text = text.data();
        ClampTextToCapacity();
    }
    text_dialog.editing = ImGui::IsItemActive();
    ImGui::PopStyleColor(4); // FrameBg, FrameBgHovered, FrameBgActive, Border
    ImGui::PopStyleVar(2);
    ImGui::Spacing();
    TextKeyboard();
    ImGui::TextWrapped("D-pad / left stick: move    Cross / A: select\n"
                       "Circle / B: cancel    Options / Start: confirm");
    // Buttons take half the box each: a Button sizes itself from its label unless an explicit
    // size is given, so the width is passed to the label.
    const ImVec2 button_size((ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f,
                             ImGui::GetTextLineHeightWithSpacing() * 1.6f);
    if (ImGui::Button("OK", button_size)) {
        FinishTextInput(1);
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", button_size)) {
        FinishTextInput(2);
    }
    ImGui::End();

    // Enter confirms and Escape cancels, the way the game's own system dialogs do.
    NormalizeCaret();
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
        ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false)) {
        FinishTextInput(1);
    } else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
               ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
        FinishTextInput(2);
    }
}

void SetOpen(bool value) {
    if (menu_open.exchange(value) == value) {
        return;
    }
    ImGui::GetIO().MouseDrawCursor = value || text_dialog.open;
    menu_focus = value;
    if (value) {
        ImGui::GetIO().ClearEventsQueue();
        ImGui::GetIO().ClearInputKeys();
    }
    if (!value) input_release = ControllerHeld();
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
    default: return ImGuiKey_None;
    }
}

ImGuiKey KeyFromGamepad(u8 button) {
    return BbImGui::ButtonKey(SDL_GamepadButton(button));
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

// The widget runs before Store reads v: argument evaluation order is unspecified (clang on
// Windows copied v before the widget changed it, so clicks stored the old value).
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

/// "FSR 4" with the model that ran: the DLL's version from its manifest, or v07.
const char* Fsr4Label() {
    static char label[64];
    const auto& s = BbSettings::Get();
    const char* version = s.fsr4_dll_version.load();
    if (s.fsr4_model_active != BbSettings::Fsr4ModelDll) {
        return s.fsr4_model_active == BbSettings::Fsr4ModelSdk ? "FSR 4 v07" : "FSR 4";
    }
    std::snprintf(label, sizeof(label), "FSR 4 %s", version && version[0] ? version : "(DLL)");
    return label;
}

void Hint(const char* text) {
    ImGui::SameLine();
    ImGui::PushID(text);
    if (ImGui::Button("?")) ImGui::OpenPopup("Help");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    if (ImGui::BeginPopup("Help")) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        if (ImGui::Button("Close help")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

void Menu() {
    auto& s = BbSettings::Get();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    // A dock space over the whole window: the menu opens docked in it (full screen) unless
    // "overlay_docked" is off; its tab undocks it. The game shows through empty space.
    const ImGuiID dock = ImGui::DockSpaceOverViewport(0, viewport, ImGuiDockNodeFlags_PassthruCentralNode);
    if (s.overlay_docked) {
        ImGui::SetNextWindowDockID(dock, ImGuiCond_Appearing);
    } else {
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + 40.0f * base_scale,
                                       viewport->WorkPos.y + 40.0f * base_scale),
                                ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(ImVec2(std::min(1040.0f * base_scale, viewport->WorkSize.x * 0.94f),
                                      viewport->WorkSize.y * 0.9f), ImGuiCond_Appearing);
    }
    if (menu_focus) ImGui::SetNextWindowFocus();
    bool keep_open = true;
    if (!ImGui::Begin("Bloodborne — settings  (F1 / L3+R3)", &keep_open,
                      ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    ImGui::Text("%.0f FPS  (%.1f ms)", frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f,
                frame_ms_avg);
    ImGui::TextWrapped("D-pad / left stick: move    Cross / A: select or edit    Circle / B: back\n"
                       "Right stick: scroll    L1 / R1: slow / fast adjustment    L3+R3: close");

    ImGui::SeparatorText("Temporal upscaler");
    static const char* upscalers[] = {"Off", "FSR 3.1", "FSR 4 (INT8)",
                                     "TAA (native anti-aliasing)", "DLSS (NVIDIA)"};
    static const char* later[] = {"XeSS"};
    int upscaler = s.upscaler;
    const bool upscaler_open = ImGui::BeginCombo("Upscaler", upscalers[upscaler]);
    if (menu_focus) {
        ImGui::SetItemDefaultFocus();
        menu_focus = false;
    }
    if (upscaler_open) {
        for (int i = 0; i < BbSettings::UpscalerCount; ++i) {
            const bool supported = i == BbSettings::UpscalerFsr4 ? s.fsr4_supported.load()
                : i == BbSettings::UpscalerDlss ? s.dlss_supported.load() : true;
            ImGui::BeginDisabled(!supported);
            if (ImGui::Selectable(upscalers[i], i == upscaler)) {
                Store(s.upscaler, i, true);
            }
            ImGui::EndDisabled();
            if (!supported) {
                ImGui::SameLine();
                ImGui::TextDisabled("— not supported by this GPU");
            }
        }
        for (const char* name : later) {
            ImGui::BeginDisabled();
            ImGui::Selectable(name, false);
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("— in progress");
        }
        ImGui::EndCombo();
    }
    if (const char* problem = s.fsr4_problem.load()) {
        ImGui::PushTextWrapPos();
        // FSR 4 running on v07 with a note: only the chosen DLL model is unavailable.
        const bool model_note = BbSettings::IsFsr4(s.upscaler) &&
                                s.fsr4_model_active == BbSettings::Fsr4ModelSdk;
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "%s: %s",
                           model_note ? "AMD DLL model not used" : "Upscaler unavailable", problem);
        if (!BbSettings::IsFsr4(s.upscaler))
            ImGui::TextUnformatted("The mode selected above is active. You can select it again to retry.");
        ImGui::PopTextWrapPos();
    }
    if (BbSettings::IsFsr4(s.upscaler)) {
        // The DLL model's label comes from its assets' manifest (the DLL it was recorded from).
        const char* version = s.fsr4_dll_version.load();
        char dll_label[96];
        std::snprintf(dll_label, sizeof(dll_label), "AMD DLL model%s%s%s",
                      version && version[0] ? " (" : "", version && version[0] ? version : "",
                      version && version[0] ? ")" : "");
        const char* models[] = {"Auto (DLL model if installed)", dll_label, "SDK v07 (bundled)"};
        int model = s.fsr4_model;
        if (ImGui::BeginCombo("FSR 4 model", models[model])) {
            for (int i = 0; i < BbSettings::Fsr4ModelCount; ++i) {
                const bool supported = i != BbSettings::Fsr4ModelDll || s.fsr4_dll_supported.load();
                ImGui::BeginDisabled(!supported);
                if (ImGui::Selectable(models[i], i == model)) {
                    Store(s.fsr4_model, i, true);
                }
                ImGui::EndDisabled();
                if (!supported) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("— not supported by this GPU");
                }
            }
            ImGui::EndCombo();
        }
        const int active = s.fsr4_model_active;
        if (active == BbSettings::Fsr4ModelDll) {
            ImGui::Text("In use: %s", dll_label);
        } else if (active == BbSettings::Fsr4ModelSdk) {
            ImGui::TextUnformatted("In use: SDK v07");
        }
        Hint("The DLL model is the network of your AMD upscaler DLL (amd_fidelityfx_upscaler_dx12, "
             "e.g. from a game or OptiScaler), recorded once and replayed on Vulkan with output "
             "matching the DLL; build it with tools/fsr4cap/build_assets.sh. Its version is the "
             "DLL's. The SDK v07 model comes from AMD's FidelityFX SDK sources and is bundled. "
             "Auto uses the DLL model where its assets and GPU features are present.");
        Checkbox("FSR 4: auto exposure", s.fsr4_auto_exposure);
        Checkbox("FSR 4: invert jitter sign", s.fsr4_invert_jitter);
        Hint("For ghosting checks: the FSR 4 network normalizes color by exposure and uses it "
             "to decide when to drop past frames. Applied immediately, no restart.");
    }
    if (s.upscaler == BbSettings::UpscalerDlss) {
        int model = s.dlss_model;
        if (ImGui::BeginCombo("DLSS model", BbSettings::DlssModels[model].label)) {
            for (int i = 0; i < BbSettings::DlssModelCount; ++i) {
                if (ImGui::Selectable(BbSettings::DlssModels[i].label, i == model)) {
                    Store(s.dlss_model, i, true);
                }
            }
            ImGui::EndCombo();
        }
        if (const int active = s.dlss_active_preset; active > 0) {
            ImGui::Text("In use: preset %c, %s", char('A' + active - 1),
                        BbSettings::DlssGeneration(active));
        }
        Hint("DLSS 3 CNN (E) is the lightest; DLSS 4 Transformer (K, J) is sharper and more "
             "stable at about twice the cost; DLSS 4.5 Transformer 2 (M for Performance, L for "
             "Ultra Performance) is the newest and heaviest, strongest at low render resolutions. "
             "On RTX 20/30 the transformer models cost noticeably more GPU time. "
             "Auto uses the driver's choice for the preset (K, M for Performance, L for Ultra "
             "Performance). Changing it rebuilds DLSS (a short pause).");
    }
    const bool upscaler_on = s.upscaler != BbSettings::UpscalerOff;
    const bool taa = s.upscaler == BbSettings::UpscalerTaa;
    if (!upscaler_on || taa) {
        // No presets: the render resolution is a percentage of the output.
        int scale = s.render_scale;
        const bool changed = ImGui::SliderInt("Render resolution", &scale, BbSettings::RenderScaleMin,
                                              BbSettings::RenderScaleMax, "%d%%");
        scale = std::clamp((scale + BbSettings::RenderScaleStep / 2) / BbSettings::RenderScaleStep *
                               BbSettings::RenderScaleStep,
                           BbSettings::RenderScaleMin, BbSettings::RenderScaleMax);
        Store(s.render_scale, scale, changed);
        Hint("Percent of the output resolution the scene renders at, in steps of 5. Below 100 "
             "it renders fewer pixels and is scaled up (soft); above 100 it renders more and "
             "is scaled down (supersampling, sharper, much more GPU time). The HUD and menus "
             "stay at the output resolution.");
        ImGui::SameLine();
        Checkbox("Dynamic", s.dynamic_resolution);
        Hint("Dynamic: while the GPU cannot reach the frame rate limit, the scene's render resolution "
             "drops in steps of 1% or more; it rises again while the GPU has headroom, up to 100% of the "
             "output. It stops lowering when the CPU limits the frame rate, or when a step no "
             "longer saves GPU time. Each change is a short pause." " A render resolution above 100% stays the top.");
    }
    ImGui::BeginDisabled(!upscaler_on);
    // The upscalers' levels; with upscaling off or TAA the render resolution above takes
    // this place (the same setting: the scene's size).
    if (upscaler_on && !taa) {
        int preset = s.preset.load();
        const bool dynamic = s.dynamic_resolution.load();
        char preset_label[64];
        if (dynamic) {
            std::snprintf(preset_label, sizeof(preset_label), "Dynamic");
        } else {
            std::snprintf(preset_label, sizeof(preset_label), "%s (x%.1f)",
                          BbSettings::PresetName(preset), BbSettings::PresetScale(preset));
        }
        if (ImGui::BeginCombo("Preset", preset_label)) {
            for (int i = 0; i < BbSettings::PresetCount; ++i) {
                char label[64];
                const float scale = BbSettings::PresetScale(i);
                const int output = s.output_res;
                std::snprintf(label, sizeof(label), "%s (x%.1f, render %dx%d)",
                              BbSettings::PresetName(i), scale,
                              int(std::lround(BbSettings::OutputWidths[output] / scale / 2) * 2),
                              int(std::lround(BbSettings::OutputHeights[output] / scale / 2) * 2));
                if (ImGui::Selectable(label, !dynamic && i == preset)) {
                    Store(s.preset, i, true);
                    Store(s.dynamic_resolution, false, true);
                }
            }
            // Dynamic resolution replaces the presets' fixed sizes.
            if (ImGui::Selectable("Dynamic (follows the frame rate limit, up to native)", dynamic)) {
                Store(s.dynamic_resolution, true, true);
            }
            ImGui::EndCombo();
        }
        Hint("Dynamic: while the GPU cannot reach the frame rate limit, the scene's render resolution "
                 "drops in steps of 1% or more; it rises again while the GPU has headroom, up to 100% of the "
                 "output. It stops lowering when the CPU limits the frame rate, or when a step no "
                 "longer saves GPU time. Each change is a short pause.");
    }
    if (s.dynamic_resolution) {
        ImGui::Text("Dynamic resolution: %d%%, GPU %.1f ms per frame", s.dynamic_percent.load(),
                    s.gpu_frame_ms.load());
    }
    if (taa) {
        ImGui::TextWrapped("TAA anti-aliases the scene at the render resolution above, without "
                           "an FSR model. The saved preset returns when an upscaler is selected.");
    }
    ImGui::Text("Active scene render: %d x %d", s.active_render_width.load(),
                s.active_render_height.load());
    if (BbSettings::FixedRenderSession()) {
        ImGui::Text("Preset at startup: %s", BbSettings::PresetName(s.startup_preset));
        if (const char* automatic = std::getenv("BB_AUTO_RENDER_RES");
            automatic && automatic[0] == '1') {
            Hint("With an output other than 1080p the whole game renders at the preset resolution "
                 "(patched at startup): fastest on the Steam Deck and weaker GPUs. Preset or output "
                 "changes apply after a restart. \"Live resolution changes\" below allows changing "
                 "without a restart (post-processing then stays at 1080p, slower).");
        } else {
            Hint("BB_RENDER_RES fixes the scene size at startup. Remove this explicit variable "
                 "to change resolution and presets without restarting the game.");
        }
    } else {
        Hint("Native AA: the upscaler works as anti-aliasing. The other presets lower the scene "
             "render resolution relative to the output. The UI is drawn at the output resolution. "
             "The preset applies from the next frame without restarting the game.");
    }
    Checkbox("Sharpening (RCAS)", s.sharpen);
    ImGui::BeginDisabled(!s.sharpen);
    Slider("Sharpness", s.sharpness, 0.0f, 2.0f);
    Hint("Up to 1: the upscaler's own sharpening (RCAS). Above 1 another RCAS pass is added. "
         "DLSS has no sharpening of its own: an RCAS pass does all of it. "
         "Ctrl+click the slider to type an exact value.");
    ImGui::EndDisabled();
    Checkbox("Sub-pixel jitter", s.jitter);
    Hint("Each frame the scene shifts by a fraction of a pixel, and the upscaler gathers more "
         "detail from several frames. Without it you only get history-based anti-aliasing.");

    ImGui::SeparatorText("Reactive mask");
    ImGui::BeginDisabled(taa);
    Checkbox("Enable mask", s.reactive);
    Hint("Marks transparent effects (particles, haze) so the upscaler relies less on past "
         "frames. Fewer trails behind effects, but shimmer returns underneath them.");
    ImGui::BeginDisabled(!s.reactive);
    Slider("Scale", s.reactive_scale, 0.0f, 4.0f);
    Slider("Threshold", s.reactive_threshold, 0.0f, 1.0f);
    Slider("Maximum", s.reactive_max, 0.0f, 1.0f);
    bool show_mask = s.debug_view == BbSettings::DebugReactive;
    if (ImGui::Checkbox("Show mask (debug)", &show_mask)) {
        s.debug_view = show_mask ? BbSettings::DebugReactive : BbSettings::DebugNone;
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    Checkbox("Character motion vectors", s.object_motion);
    Hint("Exact vectors for animated objects: clothing and weapons break up less in motion. "
         "A static scene gets no extra pass. "
         "The change applies after restarting the game.");
    bool show_motion = s.debug_view == BbSettings::DebugMotion;
    if (ImGui::Checkbox("Show motion vectors (debug)", &show_motion)) {
        s.debug_view = show_motion ? BbSettings::DebugMotion : BbSettings::DebugNone;
    }
    Hint("Red/green: horizontal/vertical motion (8 pixels = full brightness). "
         "Blue: the pixel got an exact object vector, not only camera motion. "
         "A moving object with neither blue nor red/green is treated as still by the "
         "upscaler, hence the trail.");
    ImGui::EndDisabled(); // upscaler off

#ifdef _WIN32
    ImGui::SeparatorText("Frame generation (DLSS-G)");
    int frame_gen = s.frame_gen;
    if (ImGui::BeginCombo("Frame generation", BbSettings::FrameGenLabel(frame_gen))) {
        for (int i = 0; i < BbSettings::FrameGenCount; ++i) {
            if (ImGui::Selectable(BbSettings::FrameGenLabel(i), i == frame_gen)) {
                Store(s.frame_gen, i, true);
            }
        }
        ImGui::EndCombo();
    }
    if (s.frame_gen_ready) {
        const char* status = s.frame_gen_active ? "Generating frames"
            : s.frame_gen == BbSettings::FrameGenOff ? "Off (presenting through DXGI)"
            : s.frame_gen_problem.load() ? "No generated frames" : "Waiting for a scene";
        ImGui::Text("%s", status);
    } else if ((s.frame_gen != BbSettings::FrameGenOff) != (s.startup_frame_gen != BbSettings::FrameGenOff)) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Applies after restarting the game");
    }
    if (const char* problem = s.frame_gen_problem.load()) {
        ImGui::PushTextWrapPos();
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "Frame generation: %s", problem);
        ImGui::PopTextWrapPos();
    }
    Hint("NVIDIA DLSS Frame Generation through Streamline: the game renders one frame and DLSS-G "
         "adds 1-3 generated ones (Dynamic picks the count to reach the display refresh rate). "
         "The FPS limit caps real rendered frames; generated frames are added on top. "
         "Generation pauses while the window "
         "is not focused (DLSS-G's own rule). Needs an upscaler (DLSS, FSR or TAA); outputs other "
         "than 1080p also give it the scene without the HUD (cleaner UI). Files: out/streamline "
         "(tools/fetch_streamline.sh) and on RTX 20/30 the dlssg_sm86 mod in out/dlssg_sm86. "
         "Switching it on or off needs a restart; the multiplier changes live.");
#endif

    ImGui::SeparatorText("Output resolution");
    static const char* outputs[] = {"1280 x 720", "1920 x 1080", "2560 x 1440", "3840 x 2160"};
    int output = s.output_res;
    if (ImGui::BeginCombo("Output resolution", outputs[output])) {
        for (int i = 0; i < BbSettings::OutputCount; ++i) {
            if (ImGui::Selectable(outputs[i], i == output)) {
                Store(s.output_res, i, true);
            }
        }
        ImGui::EndCombo();
    }
    if (BbSettings::FixedRenderSession()) {
        Hint("Size of the final frame and the UI. The preset sets the scene size relative to "
             "the output: 4K Performance = 1920x1080. Applies after restarting the game.");
    } else {
        Hint("The size of the final frame and the UI changes at the next frame boundary. "
             "The preset sets the scene size relative to the output: 4K Performance = 1920x1080. "
             "Changing the size resets the upscaler history and may cause a short pause.");
    }
    static const char* live_modes[] = {"Auto (by GPU)", "Off (faster)", "On"};
    int live = s.live_resolution + 1;
    if (ImGui::BeginCombo("Live resolution changes", live_modes[live])) {
        for (int i = 0; i < 3; ++i) {
            if (ImGui::Selectable(live_modes[i], i == live)) {
                Store(s.live_resolution, i - 1, true);
            }
        }
        ImGui::EndCombo();
    }
    Hint("On: output resolution and preset change without a restart, but the game's "
         "post-processing stays at 1080p — noticeably slower on the Steam Deck and older GPUs. "
         "Off: everything renders at the preset resolution; changes need a restart. Auto turns "
         "it on for powerful discrete GPUs. Applies after restarting the game.");
    ImGui::SeparatorText("Game effects (after restart)");
    static const char* lods[] = {"Highest (-2)", "As in the game", "Lower (1)", "Lowest (2)"};
    static constexpr int lod_values[] = {-2, 0, 1, 2};
    int lod_index = 1;
    for (int i = 0; i < 4; ++i) {
        if (lod_values[i] == s.model_lod) lod_index = i;
    }
    if (ImGui::BeginCombo("Model detail", lods[lod_index])) {
        for (int i = 0; i < 4; ++i) {
            if (ImGui::Selectable(lods[i], i == lod_index)) {
                Store(s.model_lod, lod_values[i], true);
            }
        }
        ImGui::EndCombo();
    }
    for (int e = 0; e < BbSettings::EffectCount; ++e) {
        Checkbox(BbSettings::Effects[e].label, s.effects[e]);
    }
    Hint("Effects are switched by game patches (patches/Bloodborne.xml): chromatic aberration, "
         "depth of field, motion blur and SSAO while the game runs, the others at startup. "
         "Motion blur and shadows from dynamic lights cost noticeable GPU time.");
    Hint("Free camera: hold Cross and press L3 (keyboard: Space + Z). "
         "Debug menu: left touchpad / Tab. Needs DbgFont14h.ccm and DbgFont14h.tpf "
         "in dvdroot_ps4/font from Nexus mod #253. Right touchpad: Backspace.");

    bool restart = s.object_motion != s.startup_object_motion ||
                   s.model_lod != s.startup_model_lod ||
                   s.live_resolution != s.startup_live_resolution ||
                   BbSettings::ResolutionNeedsRestart();
    for (int e = 0; e < BbSettings::EffectCount; ++e) {
        restart |= s.effects[e] != s.startup_effects[e];
    }
    if (restart) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                           "Changes apply after restarting the game");
        if (ImGui::Button("Apply and restart the game")) {
            BbSettings::Save();
            runtime_restart();
        }
    }

    ImGui::SeparatorText("Window, input and audio");
    Checkbox("Full screen (F11)", s.fullscreen);
    Checkbox("Maximised window", s.maximized);
    Checkbox("Read the gamepad while the window is in the background", s.background_gamepad);
    Checkbox("Hide the mouse cursor (idle 0.5 s or gamepad input)", s.hide_cursor);
    Checkbox("Keyboard controls (WASD, arrows, Enter, Esc...)", s.keyboard_controls);
    ImGui::SameLine();
    Hint("Off: the keyboard no longer plays the game, so its keys stay free for other "
         "programs' hotkeys. F1 still opens this menu.");
    Checkbox("Open this menu docked full screen", s.overlay_docked);
    Checkbox("Mute all audio", s.mute);
    Checkbox("Mute while the window is in the background", s.mute_background);
    {
        const int launch = s.launch_saved;
        if (ImGui::BeginCombo("Launch into", BbSettings::LaunchLabel(launch))) {
            for (int i = 0; i < BbSettings::LaunchCount; ++i) {
                if (ImGui::Selectable(BbSettings::LaunchLabel(i), i == launch)) {
                    Store(s.launch_saved, i, true); // the next start's
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        Hint("At the next start the title screen's rows are selected for you: Play Offline, then "
             "the chosen row. Continue without a save stops at the menu. --launch NAME (title, "
             "offline, continue, load, new_game, system) overrides it for one start.");
    }

    ImGui::SeparatorText("Other");
    Checkbox("FPS counter in the corner", s.show_fps);

    ImGui::Spacing();
    if (ImGui::Button("Close")) {
        keep_open = false;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Settings are saved to bbport.ini");
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
    const float fps = frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f;
    if (s.frame_gen_active) {
        // Rendered frames, and what DLSS-G shows with its generated ones.
        const int shown = s.frame_gen_presented;
        ImGui::Text("%.0f FPS  FG %dx (rendered %.0f FPS, %.1f ms)", fps * shown, shown, fps,
                    frame_ms_avg);
        ImGui::End();
        return;
    }
    // The upscaler's name only while it runs: 2D menus and loading screens are presented as
    // drawn, scaled to the window without it.
    char dynamic[24] = "";
    // Scene frames only (menus and loading screens are not scaled).
    if (s.dynamic_resolution && s.dynamic_percent && s.scene_frame) {
        std::snprintf(dynamic, sizeof(dynamic), "  %d%%", s.dynamic_percent.load());
    }
    ImGui::Text("%.0f FPS  %.1f ms  %s%s", fps, frame_ms_avg,
                !s.upscaler_ran                          ? ""
                : s.upscaler == BbSettings::UpscalerFsr3   ? "FSR 3.1"
                : s.upscaler == BbSettings::UpscalerFsr4 ? Fsr4Label()
                : s.upscaler == BbSettings::UpscalerTaa ? "TAA"
                : s.upscaler == BbSettings::UpscalerDlss ? "DLSS"
                                                         : "",
                dynamic);
    ImGui::End();
}

// Also used by the headless navigation regression test: the same widgets and frame logic,
// without the Vulkan presenter. Back only closes the outer menu when no editor/popup was open.
void DrawUi() {
    const bool was_editing = ImGui::IsAnyItemActive() ||
        ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    ImGui::NewFrame();
    if (menu_open && !text_dialog.open) {
        Menu();
        if (!was_editing && (ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
                             ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))) SetOpen(false);
    }
    if (BbSettings::Get().show_fps && !menu_open) FpsCounter();
    if (text_dialog.open) TextDialogWindow();
    ImGui::Render();
}

} // namespace

void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count) {
    std::scoped_lock lock{imgui_mutex};
    if (initialized) {
        return;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // window positions are not kept
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad |
                      ImGuiConfigFlags_DockingEnable;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.BackendPlatformName = "bbport";

    BbImGui::ControllerStyle();

    ImFontConfig font_config;
    font_config.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(bb_font_ttf),
                                   int(bb_font_ttf_end - bb_font_ttf), BbImGui::FontSize, &font_config);

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
    std::printf("Overlay: menu ready (F1 or L3+R3)\n");
}

void UpdateTextInput(SDL_Window* window) {
    bool want = false;
    {
        std::scoped_lock lock{imgui_mutex};
        // The menu edits a value only while a field is active; the text dialog always wants it.
        want = initialized && (text_dialog.open || (menu_open && ImGui::GetIO().WantTextInput));
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
    // While the text dialog is open it takes every key and character: the game's own dialogs
    // are modal, and the game's pad input is held neutral meanwhile (CapturesInput).
    const bool text_open = text_dialog.open;
    const bool is_open = menu_open || text_open;
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        // Escape backs out of editing/popups before closing (handled after ImGui).
        const bool toggle = !text_open && (event.key.key == SDLK_F1 || event.key.key == SDLK_INSERT);
        if (down && !event.key.repeat && toggle) {
            SetOpen(!is_open);
            return true;
        }
        if (!is_open) {
            return false;
        }
        if (down) controller_input = false;
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
        if (button >= 32) return is_open;
        const bool chord_before = l3_down && r3_down;
        if (down) controller_buttons |= uint32_t(1) << button;
        else controller_buttons &= ~(uint32_t(1) << button);
        if (button == SDL_GAMEPAD_BUTTON_LEFT_STICK) {
            l3_down = down;
        } else if (button == SDL_GAMEPAD_BUTTON_RIGHT_STICK) {
            r3_down = down;
        }
        if (input_release && !ControllerHeld()) input_release = false;
        if (down) {
            controller_input = true;
            if (text_open && text_dialog.editing) text_dialog.keyboard_focus = true;
        }
        if (!text_open && !chord_before && l3_down && r3_down) {
            SetOpen(!is_open);
            return true;
        }
        if (!is_open) {
            return false;
        }
        if (button >= SDL_GAMEPAD_BUTTON_DPAD_UP && button <= SDL_GAMEPAD_BUTTON_DPAD_RIGHT) {
            BbImGui::Directions(controller_buttons, controller_axes);
        } else if (const ImGuiKey key = KeyFromGamepad(button); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        const int axis = event.gaxis.axis;
        if (axis < 0 || axis >= SDL_GAMEPAD_AXIS_COUNT) return is_open;
        controller_axes[axis] = BbImGui::AxisValue(event.gaxis.value);
        if (input_release && !ControllerHeld()) input_release = false;
        if (std::abs(controller_axes[axis]) > 0.0f) controller_input = true;
        if (!is_open) return false;
        if (text_open && text_dialog.editing &&
            (axis == SDL_GAMEPAD_AXIS_LEFTX || axis == SDL_GAMEPAD_AXIS_LEFTY) &&
            std::abs(controller_axes[axis]) > 0.0f) text_dialog.keyboard_focus = true;
        BbImGui::Directions(controller_buttons, controller_axes);
        switch (axis) {
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
            io.AddKeyAnalogEvent(ImGuiKey_GamepadL2, controller_axes[axis] > 0, controller_axes[axis]); break;
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:
            io.AddKeyAnalogEvent(ImGuiKey_GamepadR2, controller_axes[axis] > 0, controller_axes[axis]); break;
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_REMOVED:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        controller_buttons = 0;
        controller_axes.fill(0.0f);
        l3_down = r3_down = false;
        input_release = false;
        io.ClearInputKeys();
        return false;
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
            if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) controller_input = false;
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

bool BeginTextInput(const std::string& initial, const std::string& title) {
    std::scoped_lock lock{imgui_mutex};
    if (!initialized) {
        return false; // no overlay yet (before the first present): the guest falls back
    }
    ImGui::GetIO().ClearEventsQueue();
    ImGui::GetIO().ClearInputKeys();
    text_dialog.text = initial;
    ClampTextToCapacity();
    text_dialog.initial = initial;
    text_dialog.title = title;
    text_dialog.cursor = SplitCodePoints(initial).size();
    text_dialog.state = 0;
    text_dialog.open = true;
    text_dialog.focus = !controller_input;
    text_dialog.keyboard_focus = controller_input;
    text_dialog.editing = false;
    text_dialog.uppercase = false;
    text_input_open = true;
    // The dialog is modal: the cursor stays visible while it is up, whatever the settings menu
    // did before.
    ImGui::GetIO().MouseDrawCursor = true;
    std::printf("Overlay: text dialog opened (%s)\n", title.c_str());
    return true;
}

int PollTextInput(std::string& out) {
    std::scoped_lock lock{imgui_mutex};
    // Closing the UI is not cancelling the request: retain its completed state and text
    // until BeginTextInput starts the next request (the guest polls after the UI closes).
    out = text_dialog.text;
    return text_dialog.state;
}

bool SubmitText(const std::string& submitted) {
    std::scoped_lock lock{imgui_mutex};
    if (!text_dialog.open) {
        return false;
    }
    text_dialog.text = submitted;
    ClampTextToCapacity();
    FinishTextInput(1);
    return true;
}

bool TextInputActive() { return text_input_open.load(std::memory_order_acquire); }

bool Visible() {
    return initialized && (menu_open || text_dialog.open || BbSettings::Get().show_fps);
}

void FontData(const unsigned char** data, int* size) {
    *data = bb_font_ttf;
    *size = int(bb_font_ttf_end - bb_font_ttf);
}

bool CapturesInput() {
    return menu_open || text_input_open || input_release;
}

void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent) {
    // Present interval for the FPS readout (measured also while nothing is drawn).
    const auto now = std::chrono::steady_clock::now();
    const float ms = std::chrono::duration<float, std::milli>(now - last_present).count();
    last_present = now;
    if (ms > 0.0f && ms < 1000.0f) {
        frame_ms_avg = frame_ms_avg == 0.0f ? ms : frame_ms_avg * 0.95f + ms * 0.05f;
    }
    if (!Visible()) {
        return;
    }
    std::scoped_lock lock{imgui_mutex};
    if (!initialized) {
        return;
    }
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
    DrawUi();

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

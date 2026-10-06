// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: in-game settings menu (Dear ImGui), drawn by the presenter into the swapchain image
// after the game frame, at display resolution. F1 (keyboard; Insert too) or L3+R3 (gamepad) opens it;
// while it is open the game gets no pad/keyboard input. Settings live in bbport_settings.h.

#pragma once

#include <string>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

union SDL_Event;
struct SDL_Window;

namespace Vulkan {
class Instance;
}

namespace BbOverlay {

/// Present thread, once: the ImGui context and its Vulkan backend.
void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count);

/// Window thread, for every SDL event: true when the menu or the text dialog consumed it.
bool HandleEvent(const SDL_Event& event);
/// Turns SDL text input on while the menu edits a value or the text dialog is open
/// (window thread, once per poll).
void UpdateTextInput(SDL_Window* window);

/// System text entry (the PS4 IME dialog) as a centered input box over a dimmed frame.
/// Guest thread (runtime_services.c) and window/present threads; false when the overlay is
/// not up yet, so the guest can fall back.
bool BeginTextInput(const std::string& initial, const std::string& title);
/// 0 while typing, 1 confirmed, 2 cancelled; the text is UTF-8.
int PollTextInput(std::string& out);
/// Confirms the open text entry with `submitted`; false when none is open.
bool SubmitText(const std::string& submitted);
/// True while an entry is open.
bool TextInputActive();

/// Whether anything is drawn this frame (menu, text dialog or FPS counter on).
bool Visible();

/// Present thread: draws into `view` (layout ColorAttachmentOptimal).
void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent);

/// The menu or the text dialog is open: the game's input is held neutral.
bool CapturesInput();

} // namespace BbOverlay

// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: in-game settings menu (Dear ImGui), drawn by the presenter into the swapchain image
// after the game frame, at display resolution. Insert (keyboard) or L3+R3 (gamepad) opens it;
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

/// Window thread, for every SDL event: true when the menu consumed it.
bool HandleEvent(const SDL_Event& event);
/// Turns SDL text input on while the menu edits a value (window thread, once per poll).
void UpdateTextInput(SDL_Window* window);

/// Whether anything is drawn this frame (menu open or FPS counter on).
bool Visible();

/// Present thread: draws into `view` (layout ColorAttachmentOptimal).
void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent);
/// Every presented image, generated frames included (FPS readout; BB_FPS_LOG=1 prints the rate
/// every 5 seconds).
void NotePresent();

/// The menu is open or a text entry is shown: the game's input is held neutral.
bool CapturesInput();

/// Window thread: the game's text dialog (sceImeDialog). Shown as a box on screen until
/// `active` is false; typing goes to the window (sdl_window), the box only displays it.
void SetTextEntry(bool active, const std::string& prompt, const std::string& text);

/// Any thread: an amber banner at the top of the screen for ten seconds: `headline` in the
/// highlight colour, a bar filled to `used_fraction` (0..1; negative draws no bar) and `detail`
/// under it. Drawn by the present thread; replaces a banner still showing.
void Warn(const std::string& headline, const std::string& detail, float used_fraction);

/// Any thread: video memory in use, the limit it is compared against (0: no limit known), the
/// collector's pressure and critical marks (0: none) and what the card can still give this
/// process beyond that limit, all shown in the FPS counter. The collector calls it once per pass.
void SetVramUsage(u64 used_bytes, u64 limit_bytes, u64 pressure_bytes, u64 critical_bytes,
                  u64 card_free_bytes);

} // namespace BbOverlay

// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: button prompts that follow the input device. The game's key-guide icons (KG_*) are
// found in guest memory as the texture cache uploads them, and their pixels are swapped between
// the game's PlayStation icons, keyboard and mouse icons drawn from input.ini, and Xbox and
// Switch buttons (bbport_prompt_icons.h) whenever the device in use changes (bbport.ini
// button_prompts).

#pragma once

#include <cstddef>
#include <cstdint>

namespace BbPrompts {

/// Pad thread (input.ini load and F8): what each icon shows on keyboard and mouse, one string
/// per BbPromptIcons::Icon ("e", "lshift+leftbutton", "w a s d", "mouse", "" for nothing).
void Configure(const char* const* inputs, int count);

/// What the prompts show: the keyboard and mouse, or a kind of controller.
enum Device : int { Keyboard, PlayStation, Xbox, Switch };

/// Window thread: an input event from the keyboard or mouse, or a controller of that kind.
void OnInput(Device device);
/// Window thread: a controller of that kind is connected. Before any input, the prompts are
/// the controller's then; without one, keyboard and mouse.
void OnControllerConnected(Device device);

/// Window thread, every PollEvents: writes the icons that should change (outside every
/// renderer lock: a write to a tracked page faults into the texture cache).
void Apply();

/// GPU thread (TextureCache::RefreshImage), before a texture is read from guest memory: remembers
/// the game's button pictures, its KG_* icons (menus and their messages) and the Circle, Cross
/// and L3 in the HUD atlas (prompts in the world). Never writes guest memory itself.
/// BB_PROMPT_TRACE=1 logs every small texture once and saves its bytes (prompt-trace-*.bin),
/// to find icons drawn from other textures.
void OnUpload(uintptr_t address, size_t size, uint32_t width, uint32_t height, uint32_t levels,
              uint32_t bits);

} // namespace BbPrompts

// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

union SDL_Event;
namespace Vulkan { class Instance; }

namespace BbNativeMods {
void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count);
bool HandleEvent(const SDL_Event& event);
bool TickAndVisible(bool host_overlay_open);
void Render(vk::CommandBuffer command_buffer, vk::ImageView image_view, vk::Extent2D extent);
u32 RoutePad(u32 buttons, u8 lx, u8 ly, u8 rx, u8 ry);
bool CapturesInput();
bool ReleasesMouse();
}

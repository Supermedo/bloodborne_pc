// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: AMD Anti-Lag (VK_AMD_anti_lag). The driver is told when the game samples its input for
// a frame (Input, from the pad reads) and when that frame is presented (Present, real frames
// only: not the ones frame generation inserts). It then delays the input sampling so that the
// CPU does not run frames ahead of the GPU, which shortens the time from input to screen.
// bbport.ini anti_lag=0 (or BB_ANTI_LAG=0) turns it off.
#pragma once

#include "video_core/renderer_vulkan/vk_common.h"

namespace BbAntiLag {

/// After device creation: `supported` when VK_AMD_anti_lag and its feature were enabled.
void Init(VkDevice device, bool supported);
/// The driver offers Anti-Lag (the in-game menu greys the option out otherwise).
bool Supported();
/// A pad read by the game: the first one after a present starts the next frame's input stage.
void Input();
/// Right before a real frame is queued for presentation.
void Present();

} // namespace BbAntiLag

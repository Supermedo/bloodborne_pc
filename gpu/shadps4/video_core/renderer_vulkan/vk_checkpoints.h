// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
}

/// bbport: GPU checkpoints (BB_GPU_CHECKPOINTS=1). Every draw and dispatch of the game leaves a
/// numbered marker in the command stream before and after it: NVIDIA's diagnostic checkpoints
/// (VK_NV_device_diagnostic_checkpoints) or AMD's buffer markers (VK_AMD_buffer_marker). After a
/// device loss, Dump names the last work the GPU started and finished, with the game's shader
/// hashes and the frame: the code 23 crashes ("fault address WriteInvalid 0") had no other trace
/// of what the GPU was doing. Off, a mark costs a branch.
namespace Vulkan::Checkpoints {

/// Whether BB_GPU_CHECKPOINTS asks for them (the extensions are enabled only then): 1, or 2 to print
/// the markers every 600 frames as well (a check without a device loss).
bool Wanted();

/// After the device and its allocator were made: NVIDIA's checkpoints or AMD's markers, whichever
/// the Instance enabled (AMD's need a buffer the CPU can read after the loss).
void Init(const Instance& instance);

/// A new marker for work about to be recorded ("draw", "dispatch"...), with up to three numbers
/// (shader hashes, counts) for its description; 0 when checkpoints are off.
u32 Note(const char* what, u64 a = 0, u64 b = 0, u64 c = 0);

/// In the command buffer, before and after the work `Note` returned `id` for.
void Start(vk::CommandBuffer cmdbuf, u32 id);
void Finish(vk::CommandBuffer cmdbuf, u32 id);

/// A frame went to the display (the descriptions carry its number).
void NextFrame();

/// After a device loss: the last markers the GPU started and finished, and the work around them.
void Dump(vk::Queue queue);

} // namespace Vulkan::Checkpoints

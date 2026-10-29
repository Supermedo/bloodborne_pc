// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
}

/// bbport: lower input latency: NVIDIA Reflex (VK_NV_low_latency2) or AMD Anti-Lag 2
/// (VK_AMD_anti_lag), chosen with BB_LOW_LATENCY=on|boost. The game's render thread waits after
/// it hands a frame to the GPU (FrameSubmitted) until the driver says the next one should begin,
/// so frames do not queue up between the game and the display: the game reads the pad later
/// relative to when its picture is shown.
///
/// A frame keeps one id from the game thread to the display: FrameSubmitted queues it,
/// the GPU thread takes it at the frame's flip (RenderSubmitEnd), and the presenter shows the
/// frame with it as its Vulkan present id (PresentStart/PresentEnd).
namespace Vulkan::LowLatency {

/// Whether BB_LOW_LATENCY asks for it (the extensions are enabled only then).
bool Wanted();

/// After the device was made: picks Reflex or Anti-Lag 2 from what the Instance enabled.
void Init(const Instance& instance);

/// Reflex: the swapchain the markers and the sleep go to (null while it is recreated).
void OnSwapchain(vk::SwapchainKHR swapchain);
/// Reflex: chained into the swapchain's create info while Reflex is on.
const void* SwapchainCreateNext();

/// Game render thread, after it submitted a frame with its flip.
void FrameSubmitted();
/// True when the frame limit is kept before the game starts a frame (low latency on with a
/// limit): VideoOut then shows a finished frame at once instead of holding it for its slot.
bool PacesFrames();
/// GPU thread, at the flip of a submitted frame: the frame's id (0 for other flips).
u64 RenderSubmitEnd();
/// Presenter, around the present of a game frame (id 0: not a game frame, nothing is done).
void PresentStart(u64 id);
void PresentEnd(u64 id);
/// Presenter: the GPU work of game frame `id` is done when `semaphore` (timeline) reaches
/// `value`; logged as when the GPU finished it (measurement only).
void WatchGpu(u64 id, vk::Semaphore semaphore, u64 value);

} // namespace Vulkan::LowLatency

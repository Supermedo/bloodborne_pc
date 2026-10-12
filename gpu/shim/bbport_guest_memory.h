// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: guest direct memory in GPU-visible system memory (BB_GUEST_GPU_MEMORY=1).
//
// The runtime asks for direct memory in chunks (runtime_memory.c, CHUNK); each chunk is a
// Vulkan allocation of cached system memory, exported as a dma-buf that the runtime maps at the
// game's addresses and in its backing view. The GPU can then read the game's data in place: no
// CPU copy into staging buffers before an upload. A chunk that cannot be made stays in the
// runtime's memfd, as before.
#pragma once

#include <cstdint>
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
}

namespace BbGuestMemory {
struct Chunk {
    std::uint64_t phys = 0; ///< first byte of direct memory it holds
    std::uint64_t size = 0;
    vk::Buffer buffer;      ///< the whole chunk, for transfers
    vk::DeviceMemory memory;
    std::uint32_t index = 0; ///< creation order (stable key)
};

/// Whether the runtime can use such chunks: dma-buf export and mmap of the dma-buf. Checked once
/// with a small chunk. A driver that maps at page offsets gets 256 MiB chunks (the game's mappings
/// start inside them); NVIDIA's dma-buf maps at offset 0 only (the grid made every mapping after
/// the first fail and the game panic at startup), so there each direct allocation is its own chunk:
/// the game maps its allocations whole, at offset 0.
bool Usable(const Vulkan::Instance& instance);
/// Whether the PC memory model (BB_GUEST_IN_PLACE, the launcher's new memory and translation
/// model) may run on this GPU: tested on AMD only. Elsewhere it stays off (the 0.3 model) unless
/// BB_PC_MODEL_ANY_GPU=1.
bool PcModelGpu(const Vulkan::Instance& instance);
/// Hands the runtime direct memory chunks from now on (when enabled and supported).
void Install(const Vulkan::Instance& instance);
/// The chunk holding direct memory address `phys`, or null.
const Chunk* Find(std::uint64_t phys);
} // namespace BbGuestMemory

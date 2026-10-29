// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "bbport_copy.h"

#include <deque>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>
#include <boost/container/small_vector.hpp>

#include "common/interval_set.h"
#include "common/types.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/fault_manager.h"
#include "video_core/buffer_cache/range_set.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"

namespace AmdGpu {
struct Liverpool;
}

namespace Core {
class MemoryManager;
}

namespace Vulkan {
class GraphicsPipeline;
struct SubmitInfo;
class Runtime;
class StagingBufferPool;
} // namespace Vulkan

namespace VideoCore {

class TextureCache;
class MemoryTracker;
class PageManager;

class BufferCache {
    static constexpr u64 ADDRESS_SPACE_BITS = 40;
    // bbport: arena pages are 4 GiB unless the driver's buffer size limit is lower (AMD's
    // Windows driver: 2 GiB); an arena spans up to two pages, so they shrink to half the limit.
    static constexpr u64 MAX_ARENA_PAGE_BITS = 32;
    static constexpr u64 MIN_ARENA_PAGE_BITS = 30;
    static constexpr u64 MAX_ARENA_PAGES = u64{1} << (ADDRESS_SPACE_BITS - MIN_ARENA_PAGE_BITS);
    static constexpr u64 MIN_BLOCK_SIZE = 16_KB;

public:
    /// Read-only bindings up to this size are copied into a stream buffer (bbport: public for
    /// the draw pipeline's constant ring).
    static constexpr u64 STREAM_THRESHOLD = 16_KB;

    explicit BufferCache(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                         Vulkan::Runtime& runtime, AmdGpu::Liverpool* liverpool,
                         TextureCache& texture_cache, PageManager& tracker);
    ~BufferCache();

    /// Returns a pointer to GDS device local buffer.
    [[nodiscard]] const Buffer* GetGdsBuffer() const noexcept {
        return &gds_buffer;
    }

    /// Retrieves the device local DBA page table buffer.
    [[nodiscard]] Buffer* GetBdaPageTableBuffer() noexcept {
        return bda_pagetable_buffer.get();
    }

    /// Retrieves the fault buffer.
    [[nodiscard]] Buffer* GetFaultBuffer() noexcept {
        return fault_manager->GetFaultBuffer();
    }

    /// Retrieves the stream buffer.
    StreamBuffer& GetStreamBuffer() noexcept {
        return stream_buffer;
    }

    /// Returns minimum granularity of a sparse memory bind.
    u32 GetSparsePageShift() const noexcept {
        return block_shift;
    }

    /// Invalidates any buffer in the logical page range.
    void InvalidateMemory(VAddr device_addr, u64 size, bool assume_locks = false);

    /// bbport: a guest write fault at `device_addr` was handled; unprotect its neighbourhood.
    void ExtendWriteFault(VAddr device_addr);

    /// Flushes any GPU modified buffer in the logical page range back to CPU memory.
    void ReadMemory(VAddr device_addr, u64 size, bool is_write = false, bool assume_locks = false);

    /// Finds a buffer for the specified region.
    [[nodiscard]] std::pair<const Buffer*, u64> ObtainBuffer(VAddr device_addr, u32 size,
                                                             bool is_written,
                                                             bool is_texel_buffer = false);

    /// Attempts to obtain a buffer without modifying the cache contents.
    [[nodiscard]] std::pair<const Buffer*, u64> ObtainBufferForImage(VAddr device_addr, u32 size);

    /// Return true when a region is modified from the CPU
    [[nodiscard]] bool IsRegionCpuModified(VAddr addr, size_t size);

    /// Return true when a region is modified from the GPU
    [[nodiscard]] bool IsRegionGpuModified(VAddr addr, size_t size);

    /// Processes the fault buffer.
    void ProcessFaultBuffer();

    /// bbport: memory backing guest memory for the GPU, the reserve kept for it, and the part
    /// of the reserve used (for the memory report).
    [[nodiscard]] u64 ArenaBackedBytes() const { return arena_backed; }
    [[nodiscard]] u64 ArenaReserveBytes() const { return reserve_size; }
    [[nodiscard]] u64 ArenaReserveUsedBytes() const { return reserve_used; }

    /// Synchronizes all buffers needed for DMA.
    void SynchronizeDmaBuffers();

    /// Commits pending sparse buffer memory binds. Must be called before every scheduler submit.
    void SubmitPendingArenaBinds(Vulkan::SubmitInfo& info);

    /// bbport: after every submission: puts back pieces of game memory the GPU is past, and once
    /// a second, when VRAM runs short (or BB_ARENA_TARGET_MB is exceeded), releases game memory
    /// no binding has used for BB_ARENA_IDLE_SECONDS (60). It came back with every area, never
    /// released: 4 GB cards ran out after a few area changes (ErrorOutOfDeviceMemory).
    void RunArenaCollector();

    /// bbport: a shader read this range through its page table (it faulted). Such reads are not
    /// seen here, so its game memory is not released again.
    void KeepFaultedRange(VAddr device_addr, u64 size);

private:
    /// bbport: game memory backing comes in pieces of one block from slabs, so a released piece
    /// is taken again and a slab without pieces in use goes back to the driver.
    struct Slab {
        vk::DeviceMemory memory{};
        bool vram{};    ///< in the VRAM heap (else system memory the GPU reaches)
        bool reserve{}; ///< the reserve kept at start: taken last, never freed
        bool pinned{};  ///< alive at an arena migration: the old arena still binds its pieces
        u32 pieces{};   ///< pieces in the slab
        u32 used{};     ///< pieces bound, or released but not yet put back
        std::vector<u32> free; ///< pieces to take, sorted so the lowest is taken first
    };
    struct Run {
        u32 slab;
        u32 index;
        u32 count;
    };
    struct Released {
        u64 tick; ///< the submission that unbinds them: taken again once the GPU is past it
        u32 slab;
        u32 piece;
        u32 count;
    };
    struct Unbind {
        const Buffer* arena;
        u64 start; ///< blocks
        u64 end;
        u32 slab; ///< the slab and piece backing `start`
        u32 piece;
        u64 clear_tick; ///< the submission clearing their page table entries
    };
    static constexpr u8 CHUNK_RELEASED = 1;
    static constexpr u8 CHUNK_FAULTED = 2;

    /// Up to `count` contiguous pieces: free ones in VRAM, a new VRAM slab, system memory the
    /// GPU reaches, the reserve; std::nullopt when none is left.
    std::optional<Run> TakeRun(u64 count);
    std::optional<u32> NewSlab(u32 type_index, bool vram);
    void PutBackReleased();
    /// Releases the backing of chunks no binding used for `idle_seconds`, oldest first, until
    /// `wanted` bytes are released; returns the bytes.
    u64 ReleaseIdleChunks(u64 wanted, u32 idle_seconds);
    u64 ReleaseChunk(u64 chunk);
    /// Unbinds released chunks whose page table entries the GPU has cleared (at submission).
    void FlushUnbinds();
    /// A range is bound again before its release was unbound: that unbind is dropped.
    void CancelUnbinds(const Buffer* arena, u64 start, u64 end);
    void NoteChunkUse(VAddr device_addr, u64 size);
    void ReportArena(bool force);

private:
    struct ArenaBinds {
        const Buffer* arena;
        boost::container::small_vector<vk::SparseMemoryBind, 32> binds;
    };

    ArenaBinds* BindsForArena(const Buffer* arena) {
        auto it = std::ranges::find(pending_binds, arena, &ArenaBinds::arena);
        if (it != pending_binds.end()) {
            return std::addressof(*it);
        }
        return &pending_binds.emplace_back(arena);
    }

    const Buffer* GetArena(u64 first_block, u64 last_block);

    void EnsureResident(const Buffer* arena, u64 first_block, u64 last_block);

    void DownloadMemory(const Buffer* arena, VAddr device_addr, u64 size);

    bool SynchronizeMemory(const Buffer* arena, VAddr device_addr, u32 size, bool is_written,
                           bool is_texel_buffer);

    /// bbport: batched small copies on the copy threads (BbCopy::QueueCopy).
    static void RunGuestCopy(const BbCopy::Item& item);
    void SmallGuestCopy(const BbCopy::Item& item);

    const Buffer* UploadCopies(const Buffer* arena, std::span<vk::BufferCopy> copies,
                               size_t total_size_bytes);

    bool SynchronizeMemoryFromImage(const Buffer* arena, VAddr device_addr, u32 size);

    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    // bbport: texel buffer ranges known not to alias an image, per image registry generation.
    struct ImageMiss {
        VAddr address = 0;
        u32 size = 0;
        u64 generation = ~0ULL;
    };
    std::array<ImageMiss, 256> image_miss_cache{};
    Vulkan::Runtime& runtime;
    Vulkan::StagingBufferPool& staging_pool;
    AmdGpu::Liverpool* liverpool;
    Core::MemoryManager* memory;
    TextureCache& texture_cache;
    std::unique_ptr<MemoryTracker> memory_tracker;

    StreamBuffer stream_buffer;
    Buffer gds_buffer;
    RangeSet gpu_modified_ranges;

    std::unique_ptr<FaultManager> fault_manager;
    std::unique_ptr<Buffer> bda_pagetable_buffer;

    std::array<const Buffer*, MAX_ARENA_PAGES> address_space{};
    u64 arena_page_bits{MAX_ARENA_PAGE_BITS};
    u64 arena_page_size{u64{1} << MAX_ARENA_PAGE_BITS};
    u64 num_arena_pages{u64{1} << (ADDRESS_SPACE_BITS - MAX_ARENA_PAGE_BITS)};
    std::deque<Buffer> arenas;
    std::vector<ArenaBinds> pending_binds;
    Vulkan::Semaphore memory_semaphore;

    struct Backing : public Interval {
        vk::DeviceMemory memory;
        /// bbport: in blocks, like start and end: SubRange and CanMergeWith add block counts to it
        /// (in bytes, a range split by a release pointed one piece too early and two ranges
        /// shared pieces).
        u64 offset;
        constexpr bool CanMergeWith(const Backing& other) const noexcept {
            return memory == other.memory && offset + (end - start) == other.offset;
        }
        constexpr Backing SubRange(u64 a, u64 b) const noexcept {
            return {{a, b}, memory, offset + (a - start)};
        }
    };
    IntervalList<Backing> resident_ranges;

    u32 arena_memory_type_index{};
    u32 arena_memory_type_bits{}; ///< bbport: the types the arena accepts (system memory fallback)
    /// bbport: VRAM kept for the arena when the GPU has no other room (discrete GPUs).
    vk::DeviceMemory reserve_memory{};
    u64 reserve_size{};
    u64 reserve_used{};
    u64 arena_backed{};
    /// bbport: the slabs (a freed one keeps its place with a null memory), each memory's slab,
    /// pieces unbound but maybe still in use by work in flight, and released ranges not yet
    /// unbound (their page table entries are cleared first).
    std::vector<Slab> slabs;
    std::unordered_map<VkDeviceMemory, u32> slab_of;
    std::deque<Released> released;
    std::vector<Unbind> unbinds;
    /// Seconds since start of the last binding of each 2 MiB chunk, and CHUNK_* flags.
    std::vector<u32> chunk_used;
    std::vector<u8> chunk_flags;
    u32 arena_now{};
    u32 arena_last_collect{};
    u32 arena_last_report{};
    u32 vram_full_until{}; ///< no new VRAM slab is tried before this second (one just failed)
    u64 vram_slab_bytes{}, system_slab_bytes{};
    u64 released_bytes{}, released_chunks{}, refetched_chunks{}, faulted_chunks{};
    u64 arena_start_ns{};
    u32 block_size{};
    u32 block_shift{};
    u32 blocks_per_arena_page{};
    u32 blocks_per_arena_page_shift{};
};

} // namespace VideoCore

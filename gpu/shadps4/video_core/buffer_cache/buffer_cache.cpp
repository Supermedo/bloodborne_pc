// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <array>
#include <map>
#include <unordered_map>
#include <algorithm>
#include <bit>
#include <cstdlib>
#include <magic_enum/magic_enum.hpp>
#include "bbport_copy.h"
#include "bbport_toggles.h"
#include "bbport_sections.h"
#include "bbport_free_check.h"
#include "bbport_guest_memory.h"
#include "bbport_guest_hooks.h"
#include "common/alignment.h"
#include "core/memory.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/buffer_cache/memory_tracker.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

#include <mutex>
#include <pthread.h>
#include <x86intrin.h>
#include <vk_mem_alloc.h>

extern "C" int runtime_memory_vma_info(uintptr_t address, int* prot, int* type, uintptr_t* end);
extern "C" int runtime_memory_direct_phys(uintptr_t address, uint64_t* phys, uintptr_t* end);

namespace VideoCore {

namespace {
// bbport: the tick of the newest GPU write per 64 KiB of guest memory, per 1 MiB for writes over
// 16 MiB (ObtainBuffer with is_written: the only place ranges become GPU-modified). A readback
// waits for that submission only and copies on the readback queue, not behind the queued
// frames; a coarser grain let a buffer the GPU writes every frame hold up its neighbours.
// Read for every small constant binding of every draw (the GPU command thread, thousands of times
// a frame) while the draw recording thread notes writes: lock-free sparse page tables (a mutex and
// hash maps here took a third of that thread). Guest addresses below 2^40.
struct GpuWriteTicks {
    static constexpr u64 FineShift = 16, CoarseShift = 20;
    static constexpr u64 FineLimit = 16_MB;
    static constexpr u64 AddressBits = 40, LeafBits = 12, LeafMask = (1ull << LeafBits) - 1;
    struct Table {
        explicit Table(u64 shift_)
            : shift{shift_}, leaves((1ull << (AddressBits - shift_)) >> LeafBits) {}
        u64 shift;
        /// Page -> tick of the newest write; a leaf of 4096 pages is made at its first write.
        std::vector<std::atomic<std::atomic<u64>*>> leaves;
        std::atomic<u64>* Leaf(u64 page) const {
            return leaves[page >> LeafBits].load(std::memory_order_acquire);
        }
    };
    Table fine{FineShift};   ///< 64 KiB pages
    Table coarse{CoarseShift}; ///< 1 MiB pages (writes over FineLimit)
    std::mutex mutex;          ///< leaf creation
    void Note(u64 address, u64 size, u64 tick) {
        if (size == 0 || (address + size - 1) >> AddressBits) {
            return;
        }
        auto& table = size <= FineLimit ? fine : coarse;
        for (u64 page = address >> table.shift; page <= (address + size - 1) >> table.shift;
             ++page) {
            std::atomic<u64>* leaf = table.Leaf(page);
            if (!leaf) {
                std::scoped_lock lk{mutex};
                leaf = table.Leaf(page);
                if (!leaf) {
                    leaf = new std::atomic<u64>[1ull << LeafBits]{};
                    table.leaves[page >> LeafBits].store(leaf, std::memory_order_release);
                }
            }
            leaf[page & LeafMask].store(tick, std::memory_order_release);
        }
    }
    /// The newest write tick in the range, 0 when none is known.
    u64 Newest(u64 address, u64 size) const {
        if (size == 0 || (address + size - 1) >> AddressBits) {
            return 0;
        }
        u64 newest = 0;
        for (const Table* table : {&fine, &coarse}) {
            const u64 last = (address + size - 1) >> table->shift;
            for (u64 page = address >> table->shift; page <= last; ++page) {
                if (const std::atomic<u64>* leaf = table->Leaf(page)) {
                    newest = std::max(newest, leaf[page & LeafMask].load(std::memory_order_acquire));
                } else {
                    page |= LeafMask; // nothing written in this leaf
                }
            }
        }
        return newest;
    }
};
GpuWriteTicks& WriteTicks() {
    static GpuWriteTicks write_ticks;
    return write_ticks;
}

// bbport (BB_READBACK_LOG): the newest GPU-written bindings, to show which writes a readback that
// waited for the GPU overlapped (a binding wider than what the shader writes would show here).
struct WrittenBinding {
    u64 address, size, tick;
};
std::array<WrittenBinding, 4096> written_bindings;
std::atomic<u64> written_head{0};

// bbport: BB_READBACK_LOG=path — every download of GPU-written buffer memory back to guest
// memory (a CPU write to a range the GPU wrote): who asked, where, how much, how long it took,
// how old the GPU's newest write there was, and whether the readback queue served it.
struct ReadbackLog {
    FILE* file = nullptr;
    std::mutex mutex;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};
ReadbackLog* Readbacks() {
    static ReadbackLog* const log = [] () -> ReadbackLog* {
        const char* path = std::getenv("BB_READBACK_LOG");
        if (!path || !*path) {
            return nullptr;
        }
        auto* readbacks = new ReadbackLog;
        readbacks->file = std::fopen(path, "w");
        if (!readbacks->file) {
            delete readbacks;
            return nullptr;
        }
        std::fprintf(readbacks->file, "t_s,requester,address,window_kb,downloaded_kb,copies,is_write,"
                                      "wait_ms,ticks_since_write,write_done,readback_queue\n");
        return readbacks;
    }();
    return log;
}
thread_local const char* readback_requester = "";
thread_local bool readback_is_write = false;

/// bbport: copies finished GPU data out of an arena on the readback queue (Instance), waiting
/// only for those writes (the timeline semaphore at their tick), not for the queued frames.
class FastReadback {
public:
    explicit FastReadback(const Vulkan::Instance& instance)
        : device{instance.GetDevice()}, queue{instance.GetReadbackQueue()},
          allocator{instance.GetAllocator()} {
        if (!queue) {
            return;
        }
        pool = Vulkan::Check(device.createCommandPoolUnique({
            .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
            .queueFamilyIndex = instance.GetReadbackQueueFamilyIndex(),
        }));
        cmdbuf = Vulkan::Check(device.allocateCommandBuffers({
            .commandPool = *pool,
            .level = vk::CommandBufferLevel::ePrimary,
            .commandBufferCount = 1,
        }))[0];
        fence = Vulkan::Check(device.createFenceUnique({}));
    }
    ~FastReadback() {
        if (buffer) {
            vmaDestroyBuffer(allocator, buffer, allocation);
        }
    }

    [[nodiscard]] bool Available() const noexcept {
        return bool(queue);
    }

    /// Copies `copies` (destination offsets from 0) of `arena` once `semaphore` reaches `tick`;
    /// returns the host copy, or null on failure (the caller takes the regular path).
    const u8* Copy(const Buffer* arena, std::span<const vk::BufferCopy> copies, u64 total,
                   vk::Semaphore semaphore, u64 tick) {
        if (!Ensure(total)) {
            return nullptr;
        }
        Vulkan::Check(cmdbuf.reset());
        Vulkan::Check(cmdbuf.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit}));
        cmdbuf.copyBuffer(arena->Handle(), vk::Buffer{buffer}, copies);
        const vk::MemoryBarrier2 to_host{
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eHost,
            .dstAccessMask = vk::AccessFlagBits2::eHostRead,
        };
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &to_host});
        Vulkan::Check(cmdbuf.end());
        const vk::SemaphoreSubmitInfo wait{
            .semaphore = semaphore,
            .value = tick,
            .stageMask = vk::PipelineStageFlagBits2::eTransfer,
        };
        const vk::CommandBufferSubmitInfo command{.commandBuffer = cmdbuf};
        const vk::SubmitInfo2 submit{
            .waitSemaphoreInfoCount = 1,
            .pWaitSemaphoreInfos = &wait,
            .commandBufferInfoCount = 1,
            .pCommandBufferInfos = &command,
        };
        if (queue.submit2(submit, *fence) != vk::Result::eSuccess) {
            return nullptr;
        }
        const auto result = device.waitForFences(*fence, vk::True, UINT64_MAX);
        Vulkan::Check(device.resetFences(*fence));
        if (result != vk::Result::eSuccess) {
            return nullptr;
        }
        vmaInvalidateAllocation(allocator, allocation, 0, VK_WHOLE_SIZE);
        return mapped;
    }

private:
    bool Ensure(u64 size) {
        if (size <= capacity) {
            return true;
        }
        if (buffer) {
            vmaDestroyBuffer(allocator, buffer, allocation);
            buffer = VK_NULL_HANDLE;
            capacity = 0;
        }
        const u64 new_capacity = std::max<u64>(std::bit_ceil(size), 1_MB);
        const VkBufferCreateInfo buffer_ci{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = new_capacity,
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        const VmaAllocationCreateInfo alloc_ci{
            .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                     VMA_ALLOCATION_CREATE_MAPPED_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        };
        VmaAllocationInfo info{};
        if (vmaCreateBuffer(allocator, &buffer_ci, &alloc_ci, &buffer, &allocation, &info) !=
            VK_SUCCESS) {
            buffer = VK_NULL_HANDLE;
            return false;
        }
        mapped = static_cast<u8*>(info.pMappedData);
        capacity = new_capacity;
        return true;
    }

    vk::Device device;
    vk::Queue queue;
    VmaAllocator allocator;
    vk::UniqueCommandPool pool;
    vk::CommandBuffer cmdbuf;
    vk::UniqueFence fence;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    u8* mapped = nullptr;
    u64 capacity = 0;
};
std::mutex fast_readback_mutex;

// bbport: BB_GPU_WRITE_TWINS=1 — a guest write into a page that holds bytes the GPU is still
// writing (counters, small compute outputs rewritten every frame) does not wait for the GPU: the
// guest's copy of those bytes is kept as a "twin" and the page becomes CPU-modified at once.
// Uploads leave the GPU's bytes in the arena while the guest bytes still match their twin (the
// guest did not write them); downloads do not overwrite guest bytes that no longer match it (the
// guest wrote them since). Only pages with at most BB_GPU_WRITE_TWINS_MAX (1024) GPU-written
// bytes: larger outputs (vertices the CPU reads back, FaceGen) keep waiting for the GPU.
bool TwinsEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_GPU_WRITE_TWINS");
        return env && env[0] == '1';
    }();
    return enabled;
}
u64 TwinsMaxBytes() {
    static const u64 max_bytes = [] {
        const char* env = std::getenv("BB_GPU_WRITE_TWINS_MAX");
        return env ? std::strtoull(env, nullptr, 10) : 1024ull;
    }();
    return max_bytes;
}
std::mutex twins_mutex;
std::map<VAddr, std::vector<u8>> twins; ///< start -> guest bytes when the twin was made
std::atomic<u64> twin_faults{0}, twin_kept_bytes{0}, twin_guest_bytes{0};

enum class TwinRun { None, Unchanged, Changed };

/// Calls func(start, end, TwinRun) over [begin, end) in order (twins_mutex held).
template <typename Func>
void ForEachTwinRun(VAddr begin, VAddr end, Func&& func) {
    VAddr cursor = begin;
    auto it = twins.upper_bound(begin);
    if (it != twins.begin()) {
        --it;
    }
    for (; it != twins.end() && it->first < end; ++it) {
        const VAddr start = it->first, stop = it->first + it->second.size();
        if (stop <= begin) {
            continue;
        }
        const VAddr from = std::max(start, begin), to = std::min(stop, end);
        if (cursor < from) {
            func(cursor, from, TwinRun::None);
        }
        const u8* guest = reinterpret_cast<const u8*>(from);
        const u8* twin = it->second.data() + (from - start);
        VAddr run = from;
        bool same = guest[0] == twin[0];
        for (VAddr at = from + 1; at <= to; ++at) {
            const bool now_same = at < to && guest[at - from] == twin[at - from];
            if (at == to || now_same != same) {
                func(run, at, same ? TwinRun::Unchanged : TwinRun::Changed);
                run = at;
                same = now_same;
            }
        }
        cursor = to;
    }
    if (cursor < end) {
        func(cursor, end, TwinRun::None);
    }
}

/// Drops the twins' parts in [begin, end), keeping the parts outside it (twins_mutex held).
void EraseTwins(VAddr begin, VAddr end) {
    auto it = twins.upper_bound(begin);
    if (it != twins.begin()) {
        --it;
    }
    while (it != twins.end() && it->first < end) {
        const VAddr start = it->first, stop = it->first + it->second.size();
        if (stop <= begin) {
            ++it;
            continue;
        }
        std::vector<u8> bytes = std::move(it->second);
        it = twins.erase(it);
        if (start < begin) {
            twins[start] = std::vector<u8>(bytes.begin(), bytes.begin() + (begin - start));
        }
        if (stop > end) {
            twins[end] = std::vector<u8>(bytes.begin() + (end - start), bytes.end());
        }
    }
}

/// bbport: BB_PREUPLOAD — background upload of the game's GPU memory into the arena ahead of use.
/// 1 (run.sh default): only memory already resident in the arena that the game rewrote, and
/// textures from the arena only where it is resident: no new VRAM. 2: all of the game's GPU
/// memory, textures always from the arena (~3 GB more VRAM, the shared memory on a Steam Deck).
/// 0: off.
int PreuploadMode() {
    static const int mode = [] {
        const char* env = std::getenv("BB_PREUPLOAD");
        if (GuestInPlace()) {
            return 0; // the GPU reads the game's memory itself: nothing to upload
        }
        return env && (env[0] == '1' || env[0] == '2') ? env[0] - '0' : 0;
    }();
    return mode;
}
bool PreuploadEnabled() {
    return PreuploadMode() != 0;
}
} // namespace

/// On an integrated GPU (Steam Deck) VRAM is the same memory: everything stays in place there.
bool integrated_gpu = false;
/// bbport BB_GUEST_IN_PLACE: Garlic memory (type 3) is cached in VRAM, Onion in place.
/// bbport BB_GUEST_IN_PLACE optimizations, for A/B: VRAM copies uploaded by GPU copies from the
/// guest chunks (BB_CHUNK_UPLOADS=0: CPU staging), texture data bound in place whatever its type
/// (BB_INPLACE_TEXTURES=1; off: by type).
bool ChunkUploads() {
    static const bool on = [] {
        const char* env = std::getenv("BB_CHUNK_UPLOADS");
        return !(env && env[0] == '0');
    }();
    return on;
}
bool ChunkTextures() {
    static const bool on = [] {
        const char* env = std::getenv("BB_CHUNK_TEXTURES");
        return !(env && env[0] == '0');
    }();
    return on;
}
bool InPlaceTextures() {
    static const bool on = [] {
        // Off by default: sampled data looked smeared and ghosted with it (2026-10-04).
        const char* env = std::getenv("BB_INPLACE_TEXTURES");
        return env && env[0] == '1';
    }();
    return on;
}

bool GarlicInVram() {
    static const bool on = [] {
        const char* env = std::getenv("BB_GARLIC_VRAM");
        return env ? env[0] != '0' : !integrated_gpu;
    }();
    return on;
}

void DisableGuestInPlace() {
    Detail::guest_in_place.store(2, std::memory_order_relaxed);
    Detail::write_tracking.store(0, std::memory_order_relaxed); // recomputed: now on
}

bool Detail::ComputeWriteTracking() {
    static const bool forced = [] {
        const char* env = std::getenv("BB_WRITE_TRACKING");
        return env && env[0] == '1';
    }();
    const bool on = forced || !GuestInPlace();
    write_tracking.store(on ? 1 : 2, std::memory_order_relaxed);
    return on;
}

bool WriteVerify() {
    static const bool on = [] {
        const char* env = std::getenv("BB_WRITE_VERIFY");
        return env && env[0] == '1';
    }();
    return on && !WriteTracking();
}

bool Detail::ComputeGuestInPlace() {
    static const bool on = [] {
        const char* env = std::getenv("BB_GUEST_IN_PLACE");
        const bool enabled = env && env[0] == '1';
        if (enabled) {
            std::printf("Guest memory: the GPU uses the game's direct memory in place "
                        "(BB_GUEST_IN_PLACE=1)\n");
        }
        return enabled;
    }();
    // DisableGuestInPlace may have run first: it stays off then.
    u8 expected = 0;
    guest_in_place.compare_exchange_strong(expected, on ? 1 : 2, std::memory_order_relaxed);
    return guest_in_place.load(std::memory_order_relaxed) == 1;
}

static constexpr size_t GDS_BUFFER_SIZE = 64_KB;
static constexpr size_t STREAM_BUFFER_SIZE = 128_MB;

static constexpr auto ARENA_USAGE =
    vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst |
    vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eStorageBuffer |
    vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eVertexBuffer |
    vk::BufferUsageFlagBits::eIndirectBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress;

std::optional<u32> FindMemoryType(const vk::PhysicalDeviceMemoryProperties& properties,
                                  vk::MemoryPropertyFlags wanted, u32 memory_type_bits) {
    for (u32 i = 0; i < properties.memoryTypeCount; ++i) {
        if (((memory_type_bits >> i) & 1) == 0) {
            continue;
        }
        const auto flags = properties.memoryTypes[i].propertyFlags;
        if ((flags & wanted) == wanted) {
            return i;
        }
    }
    return std::nullopt;
}

BufferCache::BufferCache(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                         Vulkan::Runtime& runtime_, AmdGpu::Liverpool* liverpool_,
                         TextureCache& texture_cache_, PageManager& tracker)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_},
      staging_pool{runtime_.GetStagingPool()}, liverpool{liverpool_},
      memory{Core::Memory::Instance()}, texture_cache{texture_cache_},
      memory_tracker{std::make_unique<MemoryTracker>(tracker)},
      stream_buffer{instance, scheduler, MemoryType::Stream, STREAM_BUFFER_SIZE},
      gds_buffer{instance, 0, GDS_BUFFER_SIZE, MemoryType::Stream, "GDS Buffer"},
      memory_semaphore{instance} {
    while (arena_page_bits > MIN_ARENA_PAGE_BITS &&
           (u64{2} << arena_page_bits) > instance.GetMaxBufferSize()) {
        --arena_page_bits;
    }
    arena_page_size = u64{1} << arena_page_bits;
    num_arena_pages = u64{1} << (ADDRESS_SPACE_BITS - arena_page_bits);
    if (arena_page_bits != MAX_ARENA_PAGE_BITS) {
        LOG_INFO(Render_Vulkan, "Buffer arenas of {} MiB (driver buffer limit {} MiB)",
                 arena_page_size >> 20, instance.GetMaxBufferSize() >> 20);
    }
    integrated_gpu = instance.IsIntegrated();
    // bbport: the PC memory model needs the game's direct memory in dma-buf chunks the runtime can
    // map at any offset (BbGuestMemory::Usable), and an AMD GPU for now (PcModelGpu). Without
    // them it is off, as BB_GUEST_IN_PLACE=0.
    if (GuestInPlace() &&
        (!BbGuestMemory::PcModelGpu(instance) || !BbGuestMemory::Usable(instance))) {
        DisableGuestInPlace();
        std::printf("Guest memory: the PC memory model is off with this driver; the GPU uses copies "
                    "in VRAM (as BB_GUEST_IN_PLACE=0)\n");
    }
    if (GuestInPlace()) {
        static BufferCache* hooked_cache = this;
        BbGuestHooks::Install([](u64 address, u64 size) { hooked_cache->NoteFreshRange(address, size); });
    }
    const std::array<u32, 2> families = {instance.GetGraphicsQueueFamilyIndex(),
                                         instance.GetReadbackQueueFamilyIndex()};
    const bool shared = bool(instance.GetReadbackQueue()); // as Buffer creates the arenas
    const vk::BufferCreateInfo probe_ci = {
        .flags =
            vk::BufferCreateFlagBits::eSparseBinding | vk::BufferCreateFlagBits::eSparseResidency,
        .size = arena_page_size,
        .usage = ARENA_USAGE,
        .sharingMode = shared ? vk::SharingMode::eConcurrent : vk::SharingMode::eExclusive,
        .queueFamilyIndexCount = shared ? 2u : 0u,
        .pQueueFamilyIndices = shared ? families.data() : nullptr,
    };
    const vk::DeviceBufferMemoryRequirements req_info = {
        .pCreateInfo = &probe_ci,
    };
    const auto device = instance.GetDevice();
    const auto reqs = device.getBufferMemoryRequirements(req_info).memoryRequirements;
    block_size = Common::AlignUp(std::max<u64>(reqs.alignment, MIN_BLOCK_SIZE), reqs.alignment);
    ASSERT_MSG(std::popcount(block_size) == 1, "Sparse block size {} is not a power of 2",
               block_size);
    block_shift = std::bit_width(block_size) - 1;
    blocks_per_arena_page = arena_page_size / block_size;
    blocks_per_arena_page_shift = arena_page_bits - block_shift;
    group_use.assign(u64{1} << (ADDRESS_SPACE_BITS - USE_GROUP_BITS), 0);
    group_demoted.assign(group_use.size(), 0);
    arena_memory_type_index =
        FindMemoryType(instance.GetMemoryProperties(), vk::MemoryPropertyFlagBits::eDeviceLocal,
                       reqs.memoryTypeBits)
            .value();

    const u64 bda_pagetable_size =
        (blocks_per_arena_page * num_arena_pages) * sizeof(vk::DeviceAddress);
    fault_manager = std::make_unique<FaultManager>(instance, scheduler, *this, block_shift,
                                                   blocks_per_arena_page * num_arena_pages);
    bda_pagetable_buffer = std::make_unique<Buffer>(
        instance, 0, bda_pagetable_size, MemoryType::DeviceLocal, "BDA Page Table Buffer");
    runtime.FillBuffer(bda_pagetable_buffer.get(), 0u, bda_pagetable_size, 0u);
}

BufferCache::~BufferCache() = default;

void BufferCache::InvalidateMemory(VAddr device_addr, u64 size, bool assume_locks) {
    memory_tracker->InvalidateRegion(device_addr, size, [this, device_addr, size, assume_locks] {
        ReadMemory(device_addr, size, true, assume_locks);
    });
}

// bbport: the game fills its per-frame buffers (constants, skinning output) sequentially and
// every 4 KiB page cost a protection fault (~110k/s in Hunter's Nightmare, a fifth of each
// render worker's time in the kernel). A fault unprotects the aligned window around it instead;
// pages marked CPU-modified without being written only cost an upload when bound.
// Item: context = BufferCache, source = guest address, destination = host pointer, size,
// extra = the Buffer whose mapping holds the destination (flushed after the copy).
void BufferCache::RunGuestCopy(const BbCopy::Item& item) {
    auto* cache = static_cast<BufferCache*>(item.context);
    auto* dst = reinterpret_cast<u8*>(item.destination);
    cache->memory->CopySparseMemory(item.source, dst, item.size);
    auto* buffer = reinterpret_cast<Buffer*>(item.extra);
    buffer->Flush(dst - buffer->mapped_data.data(), item.size);
}

// Small guest copies run on the recording thread (it spins for work: no wakeup, and it is
// idle most of the time); PoolSmallCopies (toggle 524288) batches them for the copy threads.
// Each source is noted once the copy is queued (a WaitHostCopies started after the note covers it).
void BufferCache::SmallGuestCopy(const BbCopy::Item& item) {
    if (!scheduler.IsRecordingDeferred()) {
        // bbport: commands recorded on this thread (direct mode: the draw or dispatch after the
        // upscaler's passes; or BB_VK_RECORD_THREAD=0): the copy runs now, before the commands
        // that read it. In this thread's copy batch it ran at the next submission at the
        // earliest, possibly after a WRITE_DATA had changed its source (that wait runs only the
        // GpuComm thread's own batch). Suspected in the vertex explosions with
        // BB_VK_RECORD_THREAD=0 (issue #39).
        item.run(item);
    } else if (!BbToggle::Disabled(BbToggle::PoolSmallCopies)) {
        scheduler.RecordHostCopy([item] { item.run(item); });
    } else {
        BbCopy::QueueCopy(item);
    }
    scheduler.NoteHostCopySource(item.source, item.size);
}

void BufferCache::ExtendWriteFault(VAddr device_addr, u64 guest_rip) {
    if (GuestInPlace()) {
        NoteCpuWrite(device_addr, guest_rip);
    }
    static const u64 window = [] {
        const char* env = std::getenv("BB_FAULT_WINDOW");
        const u64 kib = env ? std::strtoull(env, nullptr, 10) : 256;
        return std::bit_ceil(std::clamp<u64>(kib, 4, 1024)) * 1024;
    }();
    if (window <= TRACKER_BYTES_PER_PAGE || BbToggle::Disabled(BbToggle::FaultWindow)) {
        return;
    }
    memory_tracker->ExtendWriteFault(Common::AlignDown(device_addr, window), window);
}

void BufferCache::ReadMemory(VAddr device_addr, u64 size, bool is_write, bool assume_locks) {
    BbStats::readbacks.fetch_add(1, std::memory_order_relaxed);
    std::array<char, 16> requester{};
    if (Readbacks()) {
        pthread_getname_np(pthread_self(), requester.data(), requester.size());
    }
    const auto flush_request = [this, device_addr, size, is_write, requester] {
        readback_requester = requester.data();
        readback_is_write = is_write;
        const u32 first_block = device_addr >> block_shift;
        const u32 last_block = (device_addr + size - 1) >> block_shift;
        const auto* arena = GetArena(first_block, last_block);

        // GPU-modified ranges come as many small scattered islands,
        // so the download is widened to a window around the request
        constexpr u64 WindowSize = 512_KB;
        const VAddr arena_end = arena->cpu_addr + arena->size_bytes;
        const VAddr window_start =
            std::max<VAddr>(Common::AlignDown(device_addr, WindowSize), arena->cpu_addr);
        const VAddr window_end = std::min<VAddr>(
            std::max<VAddr>(window_start + WindowSize, device_addr + size), arena_end);
        // bbport (BB_GPU_WRITE_TWINS): bytes the GPU is still writing on the faulting page are
        // twinned instead of waited for (see TwinsEnabled).
        if (is_write && TwinsEnabled()) {
            const VAddr page_begin = Common::AlignDown(device_addr, TRACKER_BYTES_PER_PAGE);
            const VAddr page_end = Common::AlignUp(device_addr + size, TRACKER_BYTES_PER_PAGE);
            u64 tick = 0, gpu_bytes = 0;
            gpu_modified_ranges.ForEachInRange(page_begin, page_end - page_begin,
                                               [&](VAddr start, VAddr end) {
                                                   gpu_bytes += end - start;
                                                   tick = std::max(tick, WriteTicks().Newest(
                                                                             start, end - start));
                                               });
            if (gpu_bytes != 0 && gpu_bytes <= TwinsMaxBytes() && tick != 0 &&
                !scheduler.IsFree(tick)) {
                {
                    std::scoped_lock lk{twins_mutex};
                    gpu_modified_ranges.ForEachInRange(
                        page_begin, page_end - page_begin, [&](VAddr start, VAddr end) {
                            // Parts with a twin keep their older snapshot: the guest bytes may
                            // have been written since it was taken.
                            ForEachTwinRun(start, end, [&](VAddr from, VAddr to, TwinRun run) {
                                if (run == TwinRun::None) {
                                    const u8* guest = reinterpret_cast<const u8*>(from);
                                    twins[from] = std::vector<u8>(guest, guest + (to - from));
                                }
                            });
                        });
                }
                memory_tracker->MarkRegionAsCpuModified(device_addr, size);
                twin_faults.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
        // bbport (BB_GPU_WRITE_TWINS): the window around the fault is downloaded only when the GPU has
        // done its writes there; else just the faulting page (the other pages stay GPU-modified
        // and protected), instead of waiting for GPU writes that have nothing to do with it.
        VAddr download_start = window_start, download_end = window_end;
        if (is_write && TwinsEnabled()) {
            u64 window_tick = 0;
            gpu_modified_ranges.ForEachInRange(
                window_start, window_end - window_start, [&](VAddr start, VAddr end) {
                    window_tick = std::max(window_tick, WriteTicks().Newest(start, end - start));
                });
            if (window_tick != 0 && !scheduler.IsFree(window_tick)) {
                download_start = Common::AlignDown(device_addr, TRACKER_BYTES_PER_PAGE);
                download_end = Common::AlignUp(device_addr + size, TRACKER_BYTES_PER_PAGE);
            }
        }
        DownloadMemory(arena, download_start, download_end - download_start);
        if (is_write) {
            memory_tracker->MarkRegionAsCpuModified(device_addr, size);
        }
    };
    if (assume_locks) {
        flush_request();
    } else {
        liverpool->SendCommand<true>(std::move(flush_request));
    }
}

void BufferCache::DownloadMemory(const Buffer* arena, VAddr device_addr, u64 size) {
    boost::container::small_vector<vk::BufferCopy, 1> copies;
    u64 total_size_bytes = 0;
    const VAddr arena_base = arena->cpu_addr;
    memory_tracker->ForEachDownloadRange<false>(device_addr, size, [&](u64 address, u64 size) {
        const auto add_download = [&](VAddr start, VAddr end) {
            const u64 new_offset = start - arena_base;
            const u64 new_size = end - start;
            copies.push_back(vk::BufferCopy{
                .srcOffset = new_offset,
                .dstOffset = total_size_bytes,
                .size = new_size,
            });
            // Align up to avoid cache conflicts
            constexpr u64 align = 64ULL;
            constexpr u64 mask = ~(align - 1ULL);
            total_size_bytes += (new_size + align - 1) & mask;
        };
        gpu_modified_ranges.ForEachInRange(address, size, add_download);
        gpu_modified_ranges.Subtract(address, size);
    });
    if (total_size_bytes == 0) {
        return;
    }
    const auto write_range = [&](VAddr guest, const u8* src, u64 bytes) {
        BbFreeCheck::Check(guest, bytes, src, BbFreeCheck::BufferDownload);
        memory->TryWriteBacking(std::bit_cast<u8*>(guest), src, bytes);
    };
    const auto write_back = [&](const u8* data, u64 data_offset) {
        std::unique_lock twins_lock{twins_mutex, std::defer_lock};
        if (TwinsEnabled()) {
            twins_lock.lock();
        }
        for (const auto& copy : copies) {
            const VAddr guest = arena_base + copy.srcOffset;
            const u8* src = data + (copy.dstOffset - data_offset);
            if (!TwinsEnabled() || twins.empty()) {
                write_range(guest, src, copy.size);
                continue;
            }
            // bbport (BB_GPU_WRITE_TWINS): guest bytes written since their twin was made stay;
            // the page is CPU-modified, so its next upload carries them to the arena.
            ForEachTwinRun(guest, guest + copy.size, [&](VAddr from, VAddr to, TwinRun run) {
                if (run != TwinRun::Changed) {
                    write_range(from, src + (from - guest), to - from);
                }
            });
            EraseTwins(guest, guest + copy.size);
        }
    };
    const auto wait_start = std::chrono::steady_clock::now();
    const u64 tick_before = scheduler.CurrentTick();
    // bbport: the readback queue copies the data once the GPU has done the newest write into
    // it (waiting for that submission only when it is still queued); the graphics queue would
    // first run all queued work (~a frame: 20-30 ms stutters whenever the game wrote into
    // memory a GPU pass had written).
    u64 write_tick = 0;
    for (const auto& copy : copies) {
        write_tick = std::max(write_tick, WriteTicks().Newest(arena_base + copy.srcOffset, copy.size));
    }
    const bool write_done = write_tick != 0 && scheduler.IsFree(write_tick);
    bool fast = false;
    static FastReadback readback{instance};
    if (write_tick != 0 && readback.Available() && !BbToggle::Disabled(BbToggle::ReadbackQueue)) {
        if (!write_done) {
            scheduler.Wait(write_tick);
        }
        {
            std::scoped_lock lk{fast_readback_mutex};
            if (const u8* data = readback.Copy(arena, copies, total_size_bytes,
                                               scheduler.GetWorkSemaphore()->Handle(),
                                               write_tick)) {
                write_back(data, 0);
                fast = true;
            }
        }
    }
    if (!fast) {
        const auto download =
            staging_pool.Request(total_size_bytes, VideoCore::MemoryType::HostCached);
        for (auto& copy : copies) {
            copy.dstOffset += download.offset;
        }
        runtime.CopyBuffer(arena, download.buffer, copies);
        scheduler.Finish();
        download.buffer->Invalidate(download.offset, download.size);
        write_back(download.mapped, download.offset);
    }
    if (auto* log = Readbacks()) {
        const auto now = std::chrono::steady_clock::now();
        std::scoped_lock lk{log->mutex};
        std::fprintf(log->file, "%.4f,%s,%#llx,%llu,%llu,%zu,%d,%.3f,%lld,%d,%d\n",
                     std::chrono::duration<double>(now - log->start).count(), readback_requester,
                     (unsigned long long)device_addr, (unsigned long long)(size >> 10),
                     (unsigned long long)(total_size_bytes >> 10), copies.size(),
                     readback_is_write ? 1 : 0,
                     std::chrono::duration<double, std::milli>(now - wait_start).count(),
                     write_tick ? (long long)(tick_before - write_tick) : -1LL, write_done ? 1 : 0,
                     fast ? 1 : 0);
        if (!write_done && write_tick != 0) {
            // The GPU-written bindings this readback waited for (newest first).
            const u64 n = written_head.load();
            u32 shown = 0;
            for (u64 i = n; i-- > (n > written_bindings.size() ? n - written_bindings.size() : 0) &&
                            shown < 4;) {
                const WrittenBinding b = written_bindings[i % written_bindings.size()];
                bool overlaps = false;
                u64 cpu_lo = ~0ull, cpu_hi = 0;
                for (const auto& copy : copies) {
                    const u64 a = arena_base + copy.srcOffset;
                    overlaps |= b.address < a + copy.size && a < b.address + b.size;
                    cpu_lo = std::min(cpu_lo, a);
                    cpu_hi = std::max(cpu_hi, a + copy.size);
                }
                if (overlaps) {
                    std::fprintf(log->file,
                                 "# waited for GPU write %#llx+%llu (%lld ticks before), downloaded "
                                 "%#llx..%#llx\n",
                                 (unsigned long long)b.address, (unsigned long long)b.size,
                                 (long long)(tick_before - b.tick), (unsigned long long)cpu_lo,
                                 (unsigned long long)cpu_hi);
                    ++shown;
                }
            }
        }
        std::fflush(log->file);
    }
    memory_tracker->UnmarkRegionAsGpuModified(device_addr, size);
}

namespace {
// bbport: BB_BUFFER_STATS=1 — how buffer bindings reach the GPU, by guest region (256 MiB),
// printed every 5 s: small read-only copies into the stream buffer, arena bindings, and the
// bytes those re-upload after CPU writes. Input for engine-level short paths.
struct BufferStats {
    struct Region {
        u64 stream_count{}, stream_bytes{}, arena_count{}, arena_bytes{}, upload_bytes{};
    };
    std::map<u64, Region> regions;
    /// Hot window: frame of the last binding per 64 KiB block, and the distribution of the
    /// frames between uses (1, 2, 3, 4, 5-8, 9-16, 17+): the ring's reuse distance.
    std::unordered_map<u64, u64> last_use;
    std::array<u64, 7> reuse{};
    std::chrono::steady_clock::time_point window = std::chrono::steady_clock::now();
    u64 calls = 0;
};
/// 256 MiB regions; 1 MiB inside the hot 0x104xxxxxxx window (the engine's frame data).
BufferStats& Stats();
void NoteHotUse(VAddr address, u32 size) {
    if ((address >> 28) != 0x104) {
        return;
    }
    auto& st = Stats();
    const u64 frame = BbStats::gpu_frames.load(std::memory_order_relaxed);
    for (u64 block = address >> 16; block <= (address + size - 1) >> 16; ++block) {
        auto [it, inserted] = st.last_use.try_emplace(block, frame);
        if (!inserted && it->second != frame) {
            const u64 d = frame - it->second;
            ++st.reuse[d <= 4 ? d - 1 : d <= 8 ? 4 : d <= 16 ? 5 : 6];
            it->second = frame;
        }
    }
}
u64 RegionKey(VAddr address) {
    return (address >> 28) == 0x104 ? (address >> 20) | (1ull << 40) : address >> 28;
}
bool BufferStatsEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("BB_BUFFER_STATS");
        return value && value[0] == '1';
    }();
    return enabled;
}
BufferStats& Stats() {
    static BufferStats stats;
    return stats;
}
void PrintBufferStats() {
    auto& st = Stats();
    if ((++st.calls & 4095) != 0) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - st.window).count();
    if (seconds < 5.0) {
        return;
    }
    std::printf("Buffer stats (%.1f s, per second): region, stream copies/bytes, arena bindings, "
                "re-uploaded bytes\n", seconds);
    std::printf("  hot window reuse distance in frames (1,2,3,4,5-8,9-16,17+): %llu %llu %llu %llu "
                "%llu %llu %llu\n",
                (unsigned long long)st.reuse[0], (unsigned long long)st.reuse[1],
                (unsigned long long)st.reuse[2], (unsigned long long)st.reuse[3],
                (unsigned long long)st.reuse[4], (unsigned long long)st.reuse[5],
                (unsigned long long)st.reuse[6]);
    st.reuse = {};
    for (const auto& [region, r] : st.regions) {
        const u64 base = (region >> 40) ? (region & ((1ull << 40) - 1)) << 20 : region << 28;
        if ((region >> 40) && r.stream_bytes + r.upload_bytes < 1e6 * seconds) {
            continue; // quiet MiB of the hot window
        }
        std::printf("  %#012llx: %8.0f copies %8.2f MB, %8.0f arena %8.2f MB bound, %8.2f MB uploads\n",
                    static_cast<unsigned long long>(base), r.stream_count / seconds,
                    r.stream_bytes / seconds / 1e6, r.arena_count / seconds,
                    r.arena_bytes / seconds / 1e6,
                    r.upload_bytes / seconds / 1e6);
    }
    st.regions.clear();
    st.window = now;
}
} // namespace

std::pair<const Buffer*, u64> BufferCache::ObtainBuffer(VAddr device_addr, u32 size,
                                                        bool is_written, bool is_texel_buffer) {
    BB_SECTION(ObtainBuffer);
    const bool stats = BufferStatsEnabled();
    if (stats) {
        PrintBufferStats();
        NoteHotUse(device_addr, size);
    }
    // bbport BB_GUEST_IN_PLACE: buffers in the game's direct memory are used where they are. Small
    // read-only ones (constants) are still copied when bound (the stream path below): commands
    // decoded later (WriteData, constant RAM dumps) change them on the CPU in decode order, and
    // a draw decoded before must keep its values (buffer renaming). GPU writes are noted for
    // that: a range the GPU has yet to write is not copied.
    if (is_written && !shadows.empty()) {
        DropShadows(device_addr, size);
    }
    if (GuestInPlace() &&
        (is_written || size > STREAM_THRESHOLD || IsRegionGpuModified(device_addr, size))) {
        const u64 first_block = device_addr >> block_shift;
        const u64 last_block = (device_addr + size - 1) >> block_shift;
        const auto* arena = GetArena(first_block, last_block);
        // GPU-written buffers: in place with write tracking (CPU reads of them stay coherent, as
        // before). Without it the GPU is a writer we hear of, like the loaders: VRAM wherever whole
        // blocks are known data, as a default heap. The CPU does not read what the GPU wrote there
        // (no readbacks: BB_READBACKS was relaxed before too); BB_GPU_WRITES_IN_PLACE=1 keeps them
        // in place (PCIe, ~9 ms a frame on an RX 7800 XT).
        static const bool writes_in_place_forced = [] {
            const char* env = std::getenv("BB_GPU_WRITES_IN_PLACE");
            return env && env[0] == '1';
        }();
        const bool writes_in_place = WriteTracking() || writes_in_place_forced;
        if (is_written && !writes_in_place) {
            NoteGpuWrite(device_addr, size);
        }
        EnsureResident(arena, first_block, last_block, is_written && writes_in_place);
        if (is_written && size > 64_KB && BbStats::enabled) {
            NoteLargeGpuWrite(device_addr, size);
        }
        if (IsInPlace(device_addr, size)) {
            if (is_written) {
                WriteTicks().Note(device_addr, size, scheduler.CurrentTick());
            }
            BbStats::bound_in_place_bytes.fetch_add(size, std::memory_order_relaxed);
            if (is_written) {
                BbStats::bound_in_place_written_bytes.fetch_add(size, std::memory_order_relaxed);
            }
            if (is_texel_buffer && !is_written) {
                SynchronizeMemoryFromImage(arena, device_addr, size);
            }
            TraceBinding(device_addr, size, is_written, is_written ? 0 : 2);
            if (!is_written && !is_texel_buffer) {
                if (const auto shadow = ShadowCopy(device_addr, size)) {
                    return *shadow;
                }
            }
            return {arena, arena->Offset(device_addr)};
        }
    }
    // For read-only buffers use device local stream buffer to reduce renderpass breaks.
    if (!is_written && size <= STREAM_THRESHOLD && !IsRegionGpuModified(device_addr, size)) {
        BB_SECTION(ObtainStream);
        if (stats) {
            auto& region = Stats().regions[RegionKey(device_addr)];
            ++region.stream_count;
            region.stream_bytes += size;
        }
        // bbport: the guest data is copied on a copy thread, started now; submission and
        // guest-visible fences wait for it (Scheduler::WaitHostCopies).
        if (!stream_buffer.mapped_data.empty() &&
            !BbToggle::Disabled(BbToggle::DeferredStreamCopies)) {
            if (const auto offset = stream_buffer.Reserve(size, instance.UniformMinAlignment())) {
                SmallGuestCopy({
                    .run = &RunGuestCopy,
                    .context = this,
                    .source = device_addr,
                    .destination = reinterpret_cast<u64>(stream_buffer.mapped_data.data() + *offset),
                    .size = size,
                    .extra = reinterpret_cast<u64>(static_cast<Buffer*>(&stream_buffer)),
                });
                TraceBinding(device_addr, size, false, 3);
                return {&stream_buffer, *offset};
            }
        }
        const auto [data, offset] = stream_buffer.Map(size, instance.UniformMinAlignment());
        memory->CopySparseMemory(device_addr, data, size);
        stream_buffer.Commit();
        TraceBinding(device_addr, size, false, 3);
        return {&stream_buffer, offset};
    }
    const u64 first_block = device_addr >> block_shift;
    const u64 last_block = (device_addr + size - 1) >> block_shift;
    const auto* arena = GetArena(first_block, last_block);
    EnsureResident(arena, first_block, last_block);
    BB_SECTION(ObtainVram);
    const u64 uploaded_before = BbStats::buffer_upload_bytes.load(std::memory_order_relaxed);
    if (GuestInPlace()) {
        BbStats::bound_vram_bytes.fetch_add(size, std::memory_order_relaxed);
    }
    SynchronizeMemory(arena, device_addr, size, is_written, is_texel_buffer);
    if (stats) {
        auto& region = Stats().regions[RegionKey(device_addr)];
        ++region.arena_count;
        region.arena_bytes += size;
        region.upload_bytes +=
            BbStats::buffer_upload_bytes.load(std::memory_order_relaxed) - uploaded_before;
    }
    if (is_written) {
        gpu_modified_ranges.Add(device_addr, size);
        WriteTicks().Note(device_addr, size, scheduler.CurrentTick());
        if (Readbacks()) {
            written_bindings[written_head.fetch_add(1) % written_bindings.size()] =
                WrittenBinding{device_addr, size, scheduler.CurrentTick()};
        }
    }
    TraceBinding(device_addr, size, is_written, is_written ? 1 : 4);
    return {arena, arena->Offset(device_addr)};
}

std::optional<std::pair<const Buffer*, u64>> BufferCache::CommandWriteTarget(VAddr device_addr,
                                                                           u32 size) {
    if (!GuestInPlace() || size == 0) {
        return std::nullopt;
    }
    const u64 first_block = device_addr >> block_shift;
    const u64 last_block = (device_addr + size - 1) >> block_shift;
    const auto* arena = GetArena(first_block, last_block);
    EnsureResident(arena, first_block, last_block);
    if (!IsInPlace(device_addr, size)) {
        return std::nullopt;
    }
    WriteTicks().Note(device_addr, size, scheduler.CurrentTick());
    return std::pair<const Buffer*, u64>{arena, arena->Offset(device_addr)};
}

std::pair<const Buffer*, u64> BufferCache::ObtainBufferForImage(VAddr device_addr, u32 size) {
    // bbport BB_GUEST_IN_PLACE: texture data is read by the GPU from the game's memory itself:
    // through the arena where it is bound in place, else (BB_CHUNK_TEXTURES) from its guest
    // memory chunk.
    if (GuestInPlace()) {
        const u64 first_block = device_addr >> block_shift;
        const u64 last_block = (device_addr + size - 1) >> block_shift;
        if (InPlaceTextures()) {
            EnsureResident(GetArena(first_block, last_block), first_block, last_block, true);
        }
        if (IsInPlace(device_addr, size)) {
            const auto* arena = GetArena(first_block, last_block);
            TraceBinding(device_addr, size, false, 5);
            return {arena, arena->Offset(device_addr)};
        }
        if (ChunkTextures() && !IsRegionGpuModified(device_addr, size)) {
            if (const auto source = GuestChunkSource(device_addr, size)) {
                TraceBinding(device_addr, size, false, 6);
                    return *source;
            }
        }
    }
    if (IsRegionGpuModified(device_addr, size)) {
        TraceBinding(device_addr, size, false, 7);
        return ObtainBuffer(device_addr, size, false);
    }
    // bbport: BB_PREUPLOAD — texture data comes from the arena (device-local): only the pages
    // the CPU changed since are uploaded into it, and the background pre-upload keeps it
    // current, so a streaming burst detiles from VRAM instead of copying everything through
    // the CPU and the PCIe bus in that frame.
    if (PreuploadEnabled()) {
        const u64 first_block = device_addr >> block_shift;
        const u64 last_block = (device_addr + size - 1) >> block_shift;
        // Mode 1 (no new VRAM): only where the arena already holds the range.
        bool resident = true;
        if (PreuploadMode() != 2) {
            resident_ranges.ForEachGap(first_block, last_block + 1,
                                       [&](u64, u64) { resident = false; });
        }
        if (resident) {
            const auto* arena = GetArena(first_block, last_block);
            EnsureResident(arena, first_block, last_block);
            SynchronizeMemory(arena, device_addr, size, false, false);
            TraceBinding(device_addr, size, false, 8);
            return {arena, arena->Offset(device_addr)};
        }
    }
    TraceBinding(device_addr, size, false, 9);
    const auto staging = staging_pool.Request(size, VideoCore::MemoryType::HostUncached,
                                              instance.StorageMinAlignment());
    if (!BbToggle::Disabled(BbToggle::DeferredUploads)) {
        // bbport: texture data is copied on the copy threads (streaming: 100+ MB per frame);
        // the upload reads the staging only after submission, which waits for the copies.
        constexpr u64 Chunk = 1_MB;
        for (u64 offset = 0; offset < staging.size; offset += Chunk) {
            const BbCopy::Item item{
                .run = &RunGuestCopy,
                .context = this,
                .source = device_addr + offset,
                .destination = reinterpret_cast<u64>(staging.mapped + offset),
                .size = std::min(Chunk, staging.size - offset),
                .extra = reinterpret_cast<u64>(staging.buffer),
            };
            if (staging.size < Chunk) {
                SmallGuestCopy(item);
            } else {
                BbCopy::Async([item] { item.run(item); });
                scheduler.NoteHostCopySource(item.source, item.size);
            }
        }
        return {staging.buffer, staging.offset};
    }
    memory->CopySparseMemory(device_addr, staging.mapped, staging.size);
    staging.Flush();
    return {staging.buffer, staging.offset};
}

bool BufferCache::IsRegionCpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionCpuModified(addr, size);
}

bool BufferCache::IsRegionGpuModified(VAddr addr, size_t size) {
    if (memory_tracker->IsRegionGpuModified(addr, size)) {
        return true;
    }
    // bbport BB_GUEST_IN_PLACE: GPU writes are not tracked per page; one not done yet counts.
    if (GuestInPlace()) {
        const u64 tick = WriteTicks().Newest(addr, size);
        if (tick == 0 || scheduler.GetWorkSemaphore()->IsFree(tick)) {
            return false;
        }
        // Asking the driver is a system call, and the GPU command thread asks for thousands of
        // constant bindings a frame: at most every ~250 us per thread (until then the write
        // counts as not done, which only takes the slower, always correct path; the GPU signal
        // thread refreshes the known tick on every fence anyway).
        thread_local u64 last_refresh = 0;
        if (const u64 now = __rdtsc(); now - last_refresh > 800000) {
            last_refresh = now;
            scheduler.GetWorkSemaphore()->Refresh();
        }
        return !scheduler.GetWorkSemaphore()->IsFree(tick);
    }
    return false;
}

void BufferCache::ProcessFaultBuffer() {
    fault_manager->ProcessFaultBuffer();
}

void BufferCache::SynchronizeDmaBuffers() {
    for (const auto& range : resident_ranges) {
        const u64 page = range.start >> (arena_page_bits - block_shift);
        const VAddr device_addr = range.start << block_shift;
        const u64 size = (range.end - range.start) << block_shift;
        SynchronizeMemory(address_space[page], device_addr, size, false, false);
    }
}

const Buffer* BufferCache::GetArena(u64 first_block, u64 last_block) {
    const u64 first_page = first_block >> blocks_per_arena_page_shift;
    const u64 last_page = last_block >> blocks_per_arena_page_shift;
    ASSERT_MSG(last_page - first_page <= 1,
               "Buffer request cannot span more than two VA arena pages");

    const auto* first_arena = address_space[first_page];
    const auto* last_arena = address_space[last_page];
    if (first_arena == last_arena) {
        if (!first_arena) {
            const u64 base_block = Common::AlignDownPow2<u64>(first_block, blocks_per_arena_page);
            const u64 num_pages = last_page - first_page + 1;
            const auto* new_arena =
                &arenas.emplace_back(instance, base_block << block_shift,
                                     num_pages << arena_page_bits, MemoryType::Sparse);
            address_space[first_page] = new_arena;
            address_space[last_page] = new_arena;
        }
        return address_space[first_page];
    }

    LOG_WARNING(Render, "Migrating arena");

    const u64 first_addr = first_arena ? first_arena->cpu_addr : (first_page << arena_page_bits);
    const u64 first_size = first_arena ? first_arena->size_bytes : arena_page_size;
    const u64 last_size = last_arena ? last_arena->size_bytes : arena_page_size;

    const u64 base_block = first_addr >> block_shift;
    const u64 total_size = first_size + last_size;
    const u64 end_block = (first_addr + total_size) >> block_shift;
    auto* new_arena = &arenas.emplace_back(instance, first_addr, total_size, MemoryType::Sparse);
    auto* bind = BindsForArena(new_arena);
    resident_ranges.ForEachInRange(base_block, end_block, [&](const Backing& backing) {
        const u64 start = std::max(base_block, backing.start);
        const u64 end = std::min(end_block, backing.end);
        bind->binds.push_back(vk::SparseMemoryBind{
            .resourceOffset = (start - base_block) << block_shift,
            .size = (end - start) << block_shift,
            .memory = backing.memory,
            .memoryOffset = (backing.offset + (start - backing.start)) << block_shift,
        });
    });

    u64 base_page = first_addr >> arena_page_bits;
    for (u32 page = 0; page < (first_size >> arena_page_bits); ++page) {
        address_space[base_page + page] = new_arena;
    }
    base_page = last_page;
    for (u32 page = 0; page < (last_size >> arena_page_bits); ++page) {
        address_space[base_page + page] = new_arena;
    }
    return new_arena;
}

void BufferCache::EnsureResident(const Buffer* arena, u64 first_block, u64 last_block,
                                 bool in_place) {
    BB_SECTION(EnsureResident);
    NoteUse(first_block << block_shift, (last_block - first_block + 1) << block_shift);
    if (maintained_epoch != packet_epoch) {
        Maintain();
    }
    // bbport: the blocks were all resident the last time and nothing changed since: no interval
    // lookup (it ran for every buffer binding).
    thread_local std::array<ResidencyMemo, 256> resident_memo{};
    auto& memo = resident_memo[first_block & (resident_memo.size() - 1)];
    if (memo.first == first_block && memo.last == last_block &&
        memo.generation == ResidencyGeneration()) {
        return;
    }
    IntervalList bind_ranges;
    resident_ranges.ForEachGap(first_block, last_block + 1,
                               [&](u64 start, u64 end) { bind_ranges.Add({start, end}); });
    if (bind_ranges.Empty()) {
        memo = {first_block, last_block, ResidencyGeneration()};
        return;
    }
    BbStats::Timer timer{BbStats::t_resident};

    // bbport BB_GUEST_IN_PLACE: blocks of the game's direct memory are bound to the memory it
    // lives in; the rest (and everything without it) gets VRAM as before.
    std::vector<ResidentBind> binds;
    if (GuestInPlace()) {
        IntervalList rest;
        for (const auto& range : bind_ranges) {
            BindInPlace(range.start, range.end, binds, rest, in_place);
        }
        bind_ranges = std::move(rest);
    }
    u64 vram_blocks = 0;
    for (const auto& range : bind_ranges) {
        vram_blocks += range.end - range.start;
    }

    if (vram_blocks != 0) {
        // bbport: the slots of blocks that left VRAM first, block by block. A request of several
        // blocks only ever took new space, and the free slots piled up while new 64 MiB chunks
        // were allocated.
        IntervalList rest_ranges;
        for (const auto& range : bind_ranges) {
            u64 block = range.start;
            for (; block < range.end && free_slot_count != 0; ++block) {
                const auto [memory, memory_offset] = AllocateResidency(block_size);
                auto* last = binds.empty() ? nullptr : &binds.back();
                if (last && last->end == block && last->memory == memory &&
                    last->offset + ((block - last->start) << block_shift) == memory_offset) {
                    last->end = block + 1;
                } else {
                    binds.push_back({block, block + 1, memory, memory_offset});
                }
                --vram_blocks;
            }
            if (block < range.end) {
                rest_ranges.Add({block, range.end});
            }
        }
        if (vram_blocks != 0) {
            auto [memory, memory_offset] = AllocateResidency(vram_blocks << block_shift);
            for (const auto& range : rest_ranges) {
                binds.push_back({range.start, range.end, memory, memory_offset});
                memory_offset += (range.end - range.start) << block_shift;
            }
        }
    }

    u64 total_blocks = 0;
    for (const auto& bind : binds) {
        total_blocks += bind.end - bind.start;
    }
    boost::container::small_vector<vk::BufferCopy, 8> copies;
    const auto staging =
        staging_pool.Request(total_blocks * sizeof(vk::DeviceAddress), MemoryType::HostUncached);
    ArenaBinds* arena_binds = BindsForArena(arena);
    auto* bda_addrs = reinterpret_cast<vk::DeviceAddress*>(staging.mapped);
    u64 offset = staging.offset;
    for (const auto& bind : binds) {
        Backing backing;
        backing.start = bind.start;
        backing.end = bind.end;
        backing.memory = bind.memory;
        backing.offset = bind.offset >> block_shift;
        resident_ranges.Add(backing);

        const auto& sparse = arena_binds->binds.emplace_back(vk::SparseMemoryBind{
            .resourceOffset = (bind.start << block_shift) - arena->cpu_addr,
            .size = (bind.end - bind.start) << block_shift,
            .memory = bind.memory,
            .memoryOffset = bind.offset,
        });
        for (u64 block = 0; block < sparse.size; block += block_size) {
            *(bda_addrs++) = arena->BufferDeviceAddress() + sparse.resourceOffset + block;
        }
        const u64 copy_size = (bind.end - bind.start) * sizeof(vk::DeviceAddress);
        copies.emplace_back(offset, bind.start * sizeof(vk::DeviceAddress), copy_size);
        offset += copy_size;
    }

    staging.Flush();
    runtime.CopyBuffer(staging.buffer, bda_pagetable_buffer.get(), copies);
}

std::pair<vk::DeviceMemory, u64> BufferCache::AllocateResidency(u64 bytes) {
    if (bytes == block_size && free_slot_count != 0) {
        // bbport: a slot of the fullest chunk: the emptier ones drain and go back to the driver.
        auto best = residency_chunks.end();
        for (auto it = residency_chunks.begin(); it != residency_chunks.end(); ++it) {
            if (!it->second.free_offsets.empty() &&
                (best == residency_chunks.end() || it->second.used > best->second.used)) {
                best = it;
            }
        }
        if (best != residency_chunks.end()) {
            const u64 offset = best->second.free_offsets.back();
            best->second.free_offsets.pop_back();
            best->second.used += block_size;
            --free_slot_count;
            NoteResidencyUnused();
            return {vk::DeviceMemory{best->first}, offset};
        }
    }
    // bbport: residency comes out of 64 MiB blocks (a larger request gets its own): a streaming
    // burst bound hundreds of small ranges, each its own vkAllocateMemory (~8 ms a burst). At most
    // one block is partly unused. The next block is allocated in the background once half of the
    // current one is used: the kernel clears new memory, on a Steam Deck with the CPU (16-22 ms a
    // burst). At most one spare block.
    constexpr u64 ResidencyBlock = 64_MB;
    const auto allocate = [this](u64 size) {
        const auto memory = Vulkan::Check(instance.GetDevice().allocateMemory({
            .allocationSize = size,
            .memoryTypeIndex = arena_memory_type_index,
        }));
        BbStats::device_alloc_bytes.fetch_add(size, std::memory_order_relaxed);
        BbStats::residency_alloc_bytes.fetch_add(size, std::memory_order_relaxed);
        return memory;
    };
    if (residency_used + bytes > residency_size) {
        const u64 size = std::max(bytes, ResidencyBlock);
        residency_memory = size == ResidencyBlock && spare_residency.valid() ? spare_residency.get()
                                                                              : allocate(size);
        residency_size = size;
        residency_used = 0;
        residency_chunks[static_cast<VkDeviceMemory>(residency_memory)] = {size, 0, 0, 0};
    }
    const u64 offset = residency_used;
    residency_used += bytes;
    residency_chunks[static_cast<VkDeviceMemory>(residency_memory)].used += bytes;
    if (!spare_residency.valid() && residency_used * 2 >= residency_size) {
        spare_residency = std::async(std::launch::async, allocate, ResidencyBlock);
    }
    NoteResidencyUnused();
    return {residency_memory, offset};
}

void BufferCache::NoteResidencyUnused() {
    const u64 unused = free_slot_count * block_size + (residency_size - residency_used) +
                       (spare_residency.valid() ? 64_MB : 0);
    BbStats::residency_unused_bytes.store(unused, std::memory_order_relaxed);
    BbStats::residency_chunk_count.store(residency_chunks.size(), std::memory_order_relaxed);
}

u32 BufferCache::VramIdleSeconds() {
    static const u32 seconds = [] {
        const char* env = std::getenv("BB_VRAM_IDLE_SECONDS");
        return env ? u32(std::strtoul(env, nullptr, 10)) : 60u;
    }();
    return seconds;
}

void BufferCache::NoteUse(VAddr address, u64 size) {
    const u32 now = BbStats::coarse_second.load(std::memory_order_relaxed);
    const u64 last = std::min<u64>((address + size - 1) >> USE_GROUP_BITS, group_use.size() - 1);
    for (u64 group = address >> USE_GROUP_BITS; group <= last; ++group) {
        if (group_use[group] != now) {
            group_use[group] = now;
        }
        if (group_demoted[group]) [[unlikely]] {
            group_demoted[group] = 0;
            const u64 first = (group << USE_GROUP_BITS) >> block_shift;
            const u64 end = ((group + 1) << USE_GROUP_BITS) >> block_shift;
            idle_demoted.ForEachInRange(first, end, [&](const Interval& range) {
                promote_candidates.Add({std::max(first, range.start), std::min(end, range.end)});
            });
            idle_demoted.Subtract(first, end);
        }
    }
}

void BufferCache::ReleaseResidencySlot(vk::DeviceMemory memory, u64 offset) {
    if (const auto it = residency_chunks.find(static_cast<VkDeviceMemory>(memory));
        it != residency_chunks.end()) {
        it->second.free_offsets.push_back(offset);
        ++free_slot_count;
        it->second.used -= block_size;
        if (it->second.used == 0) {
            it->second.empty_second = BbStats::coarse_second.load(std::memory_order_relaxed);
            it->second.empty_tick = scheduler.CurrentTick();
        }
    }
    NoteResidencyUnused();
}

void BufferCache::EvictVramBlock(u64 block) {
    const auto old = resident_ranges.Find(block);
    if (old == resident_ranges.end() || in_place_blocks.Contains(block) ||
        !residency_chunks.contains(static_cast<VkDeviceMemory>(old->memory))) {
        return;
    }
    const vk::DeviceMemory memory = old->memory;
    const u64 offset = (old->offset + (block - old->start)) << block_shift;
    const VAddr address = block << block_shift;
    for (const auto& arena : arenas) {
        if (arena.cpu_addr <= address && address - arena.cpu_addr < arena.size_bytes) {
            BindsForArena(&arena)->binds.emplace_back(vk::SparseMemoryBind{
                .resourceOffset = address - arena.cpu_addr,
                .size = block_size,
                .memory = {},
                .memoryOffset = 0,
            });
        }
    }
    resident_ranges.Subtract(block, block + 1);
    memory_tracker->UnmarkRegionAsGpuModified(address, block_size);
    gpu_modified_ranges.Subtract(address, block_size);
    memory_tracker->MarkRegionAsCpuModified(address, block_size);
    ReleaseResidencySlot(memory, offset);
    // After the work submitted so far: it may still read the block.
    bind_wait_tick = std::max(bind_wait_tick, scheduler.CurrentTick() - 1);
    BbStats::vram_unmapped_bytes.fetch_add(block_size, std::memory_order_relaxed);
}

void BufferCache::ProcessIdleBlocks() {
    const u32 now = BbStats::coarse_second.load(std::memory_order_relaxed);
    if (now == idle_scan_second || !GuestInPlace() || VramIdleSeconds() == 0) {
        return;
    }
    idle_scan_second = now;
    // Blocks in VRAM whose 2 MiB were not bound for the idle time go back in place
    // (ProcessDemotions: only blocks of the game's direct memory without GPU-written data).
    const u64 tick = scheduler.CurrentTick();
    for (const auto& range : resident_ranges) {
        if (!residency_chunks.contains(static_cast<VkDeviceMemory>(range.memory))) {
            continue; // in place
        }
        for (u64 block = range.start; block < range.end; ++block) {
            if (now - group_use[(block << block_shift) >> USE_GROUP_BITS] >= VramIdleSeconds()) {
                demotions_waiting.push_back({block, tick, false, true});
            }
        }
    }
    // Over 128 MiB of free slots: the blocks of the emptiest chunk go back in place too, idle or
    // not, when the free slots of the others can take them; the ones bound again return to VRAM
    // into fuller chunks (AllocateResidency), and the chunk empties. Free slots spread evenly
    // (half of 1.3 GiB) left no chunk empty. One chunk every 10 s, each once a minute at most
    // (blocks with GPU-written data stay).
    if (free_slot_count * block_size >= 128_MB && now - last_evacuation >= 10) {
        auto sparsest = residency_chunks.end();
        for (auto it = residency_chunks.begin(); it != residency_chunks.end(); ++it) {
            const ResidencyChunk& chunk = it->second;
            if (it->first == static_cast<VkDeviceMemory>(residency_memory) || chunk.used == 0 ||
                now - chunk.evacuated_second < 60) {
                continue;
            }
            if (sparsest == residency_chunks.end() || chunk.used < sparsest->second.used) {
                sparsest = it;
            }
        }
        const u64 free_elsewhere =
            sparsest == residency_chunks.end()
                ? 0
                : (free_slot_count - sparsest->second.free_offsets.size()) * block_size;
        if (sparsest != residency_chunks.end() && sparsest->second.used <= free_elsewhere) {
            sparsest->second.evacuated_second = now;
            last_evacuation = now;
            BbStats::evacuations.fetch_add(1, std::memory_order_relaxed);
            for (const auto& range : resident_ranges) {
                if (static_cast<VkDeviceMemory>(range.memory) != sparsest->first) {
                    continue;
                }
                for (u64 block = range.start; block < range.end; ++block) {
                    demotions_waiting.push_back({block, tick, false, true, true});
                }
            }
        }
    }
    // Chunks without blocks for 5 s, the rebinds away from them long done: back to the driver.
    for (auto it = residency_chunks.begin(); it != residency_chunks.end();) {
        const ResidencyChunk& chunk = it->second;
        const VkDeviceMemory memory = it->first;
        if (chunk.used != 0 || memory == static_cast<VkDeviceMemory>(residency_memory) ||
            now - chunk.empty_second < 5 || !pending_binds.empty() ||
            !scheduler.GetWorkSemaphore()->IsFree(chunk.empty_tick + 1)) {
            ++it;
            continue;
        }
        free_slot_count -= chunk.free_offsets.size();
        instance.GetDevice().freeMemory(vk::DeviceMemory{memory});
        BbStats::residency_alloc_bytes.fetch_sub(chunk.size, std::memory_order_relaxed);
        BbStats::device_free_bytes.fetch_add(chunk.size, std::memory_order_relaxed);
        BbStats::vram_chunks_freed_bytes.fetch_add(chunk.size, std::memory_order_relaxed);
        it = residency_chunks.erase(it);
        NoteResidencyUnused();
    }
}

bool BufferCache::VramEligible(u64 block, bool garlic_known) {
    if (!GarlicInVram() || dynamic_blocks.Contains(block)) {
        return false;
    }
    if (!WriteTracking()) {
        // Every byte last written by a loader or by the GPU.
        bool known = true;
        asset_bytes.ForEachGap(block << block_shift, (block + 1) << block_shift,
                               [&](u64 start, u64 end) {
                                   known = known && gpu_written_bytes.Contains(start, end);
                               });
        // Any memory type: what decides is that every write there is one we hear of (Onion data
        // the CPU keeps writing is written by code we do not hear of, so it is not known data).
        return known;
    }
    int prot = 0, type = -1;
    uintptr_t vma_end = 0;
    return garlic_known ||
           (runtime_memory_vma_info(block << block_shift, &prot, &type, &vma_end) && type == 3);
}

void BufferCache::BindInPlace(u64 start, u64 end, std::vector<ResidentBind>& out,
                              IntervalList<>& rest, bool any_type) {
    static std::atomic<u64> in_place_bytes{0}, vram_bytes{0}, reports{0};
    for (u64 block = start; block < end;) {
        const VAddr address = block << block_shift;
        u64 phys = 0;
        uintptr_t mapping_end = 0;
        const BbGuestMemory::Chunk* chunk = nullptr;
        u64 run = 0;
        // PS4 memory types as PC heaps: Onion (CPU-cached; what the CPU writes often: constants,
        // dynamic buffers) stays in place, like an upload heap the GPU reads over PCIe; Garlic
        // (what the GPU reads a lot and the CPU rarely writes: geometry, textures) gets a VRAM
        // copy kept current like a default heap (BB_GARLIC_VRAM=0: in place too).
        // Without write tracking only data whose writes we hear of may keep a VRAM copy.
        const bool garlic = !any_type && VramEligible(block);
        if (!garlic && runtime_memory_direct_phys(address, &phys, &mapping_end) &&
            (phys & (block_size - 1)) == 0 && (chunk = BbGuestMemory::Find(phys)) != nullptr) {
            // Whole blocks within the mapping, the chunk and the range.
            const u64 bytes = std::min<u64>({mapping_end - address, chunk->phys + chunk->size - phys,
                                             (end - block) << block_shift});
            run = bytes >> block_shift;
            // The run ends where data may keep a VRAM copy (with write tracking: Garlic, and blocks of
            // one mapping share its type).
            int prot = 0, type = -1;
            uintptr_t vma_end = 0;
            if (!any_type && GarlicInVram() &&
                (!WriteTracking() ||
                 (runtime_memory_vma_info(address, &prot, &type, &vma_end) && type == 3))) {
                for (u64 i = 1; i < run; ++i) {
                    if (VramEligible(block + i, true)) {
                        run = i;
                        break;
                    }
                }
            }
        }
        if (run == 0) {
            rest.Add({block, block + 1});
            vram_bytes.fetch_add(block_size, std::memory_order_relaxed);
            ++block;
            continue;
        }
        out.push_back({block, block + run, chunk->memory, phys - chunk->phys});
        in_place_blocks.Add({block, block + run});
        in_place_bytes.fetch_add(run << block_shift, std::memory_order_relaxed);
        block += run;
    }
    if (const u64 n = reports.fetch_add(1, std::memory_order_relaxed); (n & (n + 1)) == 0) {
        std::printf("Guest memory: %llu MiB of the arena bound in place, %llu MiB in VRAM "
                    "(Garlic, not direct memory in a guest chunk, or not block-aligned)\n",
                    (unsigned long long)(in_place_bytes.load() >> 20),
                    (unsigned long long)(vram_bytes.load() >> 20));
    }
}

void BufferCache::NoteCpuWrite(VAddr address, u64 guest_rip) {
    constexpr u32 Frames = 3, Window = 60;
    // Guest code known to write per-frame data (reverse engineering): what it writes is an
    // upload heap at its first write. 0x2ab7350: vertices computed on the CPU (cloth) into a
    // ~128 MiB ring, each block written now and then; 0x20858a0: the game's allocator, its
    // bookkeeping in GPU-mapped pages.
    constexpr u64 Image = 0x800000000ull;
    constexpr std::array<std::pair<u64, u64>, 2> DynamicWriters = {{
        {Image + 0x2ab7350, Image + 0x2aba310},
        {Image + 0x20858a0, Image + 0x2085e20},
    }};
    const bool dynamic_writer = std::ranges::any_of(
        DynamicWriters, [&](const auto& r) { return guest_rip >= r.first && guest_rip < r.second; });
    const u64 block = address >> block_shift;
    const u32 frame = static_cast<u32>(BbStats::frame_number.load(std::memory_order_relaxed));
    std::scoped_lock lk{dynamic_mutex};
    auto& [last, frames] = cpu_write_frames[block];
    if (dynamic_writer && frames < Frames) {
        frames = Frames;
        last = frame;
        demote_requests.push_back(block);
        demotes_requested.store(true, std::memory_order_release);
        return;
    }
    if (last == frame && frames != 0) {
        return;
    }
    frames = frame - last > Window ? 1 : frames + 1;
    last = frame;
    if (frames == Frames) {
        demote_requests.push_back(block);
        demotes_requested.store(true, std::memory_order_release);
    }
}

void BufferCache::ProcessDemotions() {
    if (demotes_requested.load(std::memory_order_acquire)) {
        std::scoped_lock lk{dynamic_mutex};
        for (const u64 block : demote_requests) {
            demotions_waiting.push_back({block, scheduler.CurrentTick(), true});
        }
        demote_requests.clear();
        demotes_requested.store(false, std::memory_order_release);
    }
    if (demotions_waiting.empty()) {
        return;
    }
    static std::atomic<u64> moved_bytes{0}, kept{0};
    const u64 submitted_below = scheduler.CurrentTick(); // ticks under it are submitted
    std::erase_if(demotions_waiting, [&](const Demotion& request) {
        const auto [block, tick, cpu_written, idle, force] = request;
        if (tick >= submitted_below) {
            return false; // its uploads are still being recorded
        }
        const VAddr address = block << block_shift;
        if (cpu_written) {
            dynamic_blocks.Add({block, block + 1}); // bound in place when it is next used
        }
        if (in_place_blocks.Contains(block) || !resident_ranges.Contains(block)) {
            return true;
        }
        if (idle) {
            if (!force &&
                BbStats::coarse_second.load(std::memory_order_relaxed) -
                        group_use[address >> USE_GROUP_BITS] <
                    VramIdleSeconds()) {
                return true; // bound again since
            }
        } else if (!cpu_written && VramEligible(block)) {
            return true; // written by a loader since it was handed out again
        }
        // Data only the GPU has (in the VRAM copy) would be lost: such blocks stay.
        if (IsRegionGpuModified(address, block_size) ||
            gpu_modified_ranges.Intersects(address, block_size)) {
            kept.fetch_add(1, std::memory_order_relaxed);
            if (force) {
                BbStats::evac_kept_gpu.fetch_add(1, std::memory_order_relaxed);
            }
            return true;
        }
        u64 phys = 0;
        uintptr_t mapping_end = 0;
        const BbGuestMemory::Chunk* chunk = nullptr;
        if (!runtime_memory_direct_phys(address, &phys, &mapping_end) ||
            (phys & (block_size - 1)) != 0 || mapping_end - address < block_size ||
            (chunk = BbGuestMemory::Find(phys)) == nullptr) {
            if (force) {
                BbStats::evac_kept_other.fetch_add(1, std::memory_order_relaxed);
            }
            return true;
        }
        if (!address_space[block >> blocks_per_arena_page_shift]) {
            return true;
        }
        Backing backing;
        backing.start = block;
        backing.end = block + 1;
        backing.memory = chunk->memory;
        backing.offset = (phys - chunk->phys) >> block_shift;
        // Its VRAM slot is reused by a later move to VRAM (bound once this rebind is done).
        if (const auto old = resident_ranges.Find(block); old != resident_ranges.end()) {
            ReleaseResidencySlot(old->memory, (old->offset + (block - old->start)) << block_shift);
        }
        resident_ranges.Subtract(block, block + 1);
        resident_ranges.Add(backing);
        in_place_blocks.Add({block, block + 1});
        // Every arena over it: one replaced by a wider one (GetArena) still serves the shaders
        // whose device addresses (the BDA page table) were taken from it.
        for (const auto& arena : arenas) {
            if (arena.cpu_addr <= address && address - arena.cpu_addr < arena.size_bytes) {
                BindsForArena(&arena)->binds.emplace_back(vk::SparseMemoryBind{
                    .resourceOffset = address - arena.cpu_addr,
                    .size = block_size,
                    .memory = chunk->memory,
                    .memoryOffset = backing.offset << block_shift,
                });
            }
        }
        // Its uploads up to `tick` land in the VRAM copy first; the rebind waits for them.
        bind_wait_tick = std::max(bind_wait_tick, tick);
        // Never uploaded or protected again: writes go to the memory the GPU reads.
        memory_tracker->MarkRegionAsCpuModified(address, block_size);
        if (idle) {
            BbStats::vram_idle_bytes.fetch_add(block_size, std::memory_order_relaxed);
            idle_demoted.Add({block, block + 1});
            group_demoted[address >> USE_GROUP_BITS] = 1;
        }
        const u64 total = moved_bytes.fetch_add(block_size, std::memory_order_relaxed) + block_size;
        if (!idle && (total >> 20) != ((total - block_size) >> 20)) {
            std::printf("Guest memory: %llu MiB of CPU-written blocks moved in place from VRAM, "
                        "%llu blocks kept (GPU-written)\n",
                        (unsigned long long)(total >> 20), (unsigned long long)kept.load());
        }
        return true;
    });
}

std::optional<std::pair<const Buffer*, u64>> BufferCache::GuestChunkSource(VAddr address, u64 size) {
    u64 phys = 0;
    uintptr_t mapping_end = 0;
    const BbGuestMemory::Chunk* chunk = nullptr;
    if (size == 0 || !runtime_memory_direct_phys(address, &phys, &mapping_end) ||
        address + size > mapping_end || (chunk = BbGuestMemory::Find(phys)) == nullptr ||
        phys + size > chunk->phys + chunk->size) {
        return std::nullopt;
    }
    auto& buffer = chunk_buffers[chunk->index % chunk_buffers.size()];
    if (!buffer) {
        buffer = std::make_unique<Buffer>(instance, chunk->size, chunk->memory,
                                          fmt::format("bbport guest memory {:#x}", chunk->phys));
    }
    return std::pair<const Buffer*, u64>{buffer.get(), phys - chunk->phys};
}

void BufferCache::NoteAssetWrite(VAddr addr, u64 size) {
    if (!GuestInPlace() || WriteTracking() || size == 0) {
        return;
    }
    std::scoped_lock lk{pending_assets_mutex};
    pending_assets.push_back({addr, size, false});
    assets_pending.store(true, std::memory_order_release);
}

void BufferCache::NoteFreshRange(VAddr addr, u64 size) {
    BbStats::gpu_range_allocs.fetch_add(1, std::memory_order_relaxed);
    BbStats::gpu_range_alloc_bytes.fetch_add(size, std::memory_order_relaxed);
    if (!GuestInPlace() || WriteTracking()) {
        return; // every write is caught, or nothing is bound in place
    }
    // The GPU's old data there is dead: whole pages inside stop being GPU-modified now, so the
    // writes that follow (announced after the fact) read none of it back over themselves.
    const VAddr pages_start = Common::AlignUp(addr, u64(4096));
    const VAddr pages_end = Common::AlignDown(addr + size, u64(4096));
    if (pages_start < pages_end) {
        memory_tracker->UnmarkRegionAsGpuModified(pages_start, pages_end - pages_start);
    }
    std::scoped_lock lk{pending_assets_mutex};
    pending_assets.push_back({addr, size, true});
    assets_pending.store(true, std::memory_order_release);
}

void BufferCache::NoteCpuWriteRange(VAddr addr, u64 size) {
    if (!GuestInPlace() || size == 0) {
        return;
    }
    for (VAddr block = addr >> block_shift; block <= (addr + size - 1) >> block_shift; ++block) {
        NoteCpuWrite(block << block_shift, 0);
    }
}

void BufferCache::ProcessPendingAssets() {
    if (!assets_pending.load(std::memory_order_acquire)) {
        return;
    }
    std::vector<PendingRange> ranges;
    {
        std::scoped_lock lk{pending_assets_mutex};
        ranges.swap(pending_assets);
        assets_pending.store(false, std::memory_order_release);
    }
    for (const auto& [addr, size, fresh] : ranges) {
        const u64 first = addr >> block_shift, end = (addr + size + block_size - 1) >> block_shift;
        if (fresh) {
            // Its old contents are dead and the next writer may be code we do not hear of: in
            // place until a loader writes it. Until the move, uploads keep the VRAM copy current.
            for (auto& [key, write] : large_writes) {
                if (addr < write.addr + write.size && write.addr < addr + size) {
                    ++write.fresh;
                }
            }
            asset_bytes.Subtract(addr, addr + size);
            gpu_written_bytes.Subtract(addr, addr + size);
            promote_candidates.Subtract(first, end);
            for (u64 block = first; block < end; ++block) {
                const VAddr block_addr = block << block_shift;
                const VAddr from = std::max<VAddr>(addr, block_addr);
                const VAddr to = std::min<VAddr>(addr + size, block_addr + block_size);
                if (resident_ranges.Contains(block) && !in_place_blocks.Contains(block)) {
                    if (memory_tracker->IsRegionGpuModified(block_addr, block_size) ||
                        gpu_modified_ranges.Intersects(block_addr, block_size)) {
                        // The GPU's data beside it is only in VRAM: it goes back into the game's
                        // memory as the submission ends, then the block moves (QueueCopyBacks).
                        // An upload now would put stale bytes over that data (whole pages).
                        copy_back_blocks.Add({block, block + 1});
                        continue;
                    }
                    demotions_waiting.push_back({block, scheduler.CurrentTick(), false});
                }
                memory_tracker->MarkRegionAsCpuModified(from, to - from);
            }
            continue;
        }
        asset_bytes.Add({addr, addr + size});
        for (u64 block = first; block < end; ++block) {
            if (in_place_blocks.Contains(block) && resident_ranges.Contains(block) &&
                VramEligible(block)) {
                promote_candidates.Add({block, block + 1});
            }
        }
    }
}

void BufferCache::NoteLateCommandWrite(VAddr addr, const void* data, u64 size) {
    if (!GuestInPlace() || WriteTracking() || size == 0) {
        return;
    }
    const auto* bytes = static_cast<const u8*>(data);
    std::scoped_lock lk{late_writes_mutex};
    late_writes.emplace_back(addr, std::vector<u8>(bytes, bytes + size));
    late_writes_pending.store(true, std::memory_order_release);
}

void BufferCache::TraceBinding(VAddr addr, u64 size, bool is_written, int kind) {
    static const u64 trace_size = [] {
        const char* env = std::getenv("BB_TRACE_SIZE");
        return env ? std::strtoull(env, nullptr, 0) : u64(0);
    }();
    if (trace_size == 0) {
        return;
    }
    if (is_written && size == trace_size &&
        std::ranges::find(traced_ranges, std::pair<VAddr, u64>{addr, size}) == traced_ranges.end()) {
        if (traced_ranges.size() >= 16) {
            traced_ranges.erase(traced_ranges.begin());
        }
        traced_ranges.emplace_back(addr, size);
    }
    const bool traced = std::ranges::any_of(traced_ranges, [&](const auto& range) {
        return addr < range.first + range.second && range.first < addr + size;
    });
    if (traced) {
        ++trace_counts[kind];
        if (!is_written) {
            trace_min = std::min(trace_min, size);
            trace_max = std::max(trace_max, size);
        }
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - trace_report < std::chrono::seconds(5)) {
        return;
    }
    trace_report = now;
    std::printf("Trace %#llx-byte GPU buffers (%zu, 5 s): write in place %llu, write VRAM %llu; read "
                "in place %llu, snapshot %llu, VRAM %llu (sizes %llu-%llu); image source: in place "
                "%llu, chunk %llu, GPU data %llu, preupload %llu, staging %llu\n",
                (unsigned long long)trace_size, traced_ranges.size(),
                (unsigned long long)trace_counts[0], (unsigned long long)trace_counts[1],
                (unsigned long long)trace_counts[2], (unsigned long long)trace_counts[3],
                (unsigned long long)trace_counts[4], (unsigned long long)(trace_max ? trace_min : 0),
                (unsigned long long)trace_max, (unsigned long long)trace_counts[5],
                (unsigned long long)trace_counts[6], (unsigned long long)trace_counts[7],
                (unsigned long long)trace_counts[8], (unsigned long long)trace_counts[9]);
    trace_counts = {};
    trace_min = ~0ull;
    trace_max = 0;
}

void BufferCache::NoteLargeGpuWrite(VAddr addr, u64 size) {
    auto& write = large_writes[addr ^ (size << 40)];
    if (write.count++ == 0) {
        write.addr = addr;
        write.size = size;
        write.images = texture_cache.DescribeImagesIn(addr, size);
    }
    write.in_place = IsInPlace(addr, size);
    const auto now = std::chrono::steady_clock::now();
    if (now - large_writes_report < std::chrono::seconds(5)) {
        return;
    }
    large_writes_report = now;
    std::vector<const LargeWrite*> rows;
    for (const auto& [key, entry] : large_writes) {
        rows.push_back(&entry);
    }
    std::ranges::sort(rows, [](const auto* a, const auto* b) { return a->count > b->count; });
    std::printf("GPU-written over 64 KiB (5 s):");
    for (std::size_t i = 0; i < std::min<std::size_t>(rows.size(), 12); ++i) {
        std::printf("%s %#llx+%#llx x%llu %s fresh %llu%s", i ? ";" : "",
                    (unsigned long long)rows[i]->addr, (unsigned long long)rows[i]->size,
                    (unsigned long long)rows[i]->count, rows[i]->in_place ? "in-place" : "VRAM",
                    (unsigned long long)rows[i]->fresh, rows[i]->images.c_str());
    }
    std::printf("\n");
    large_writes.clear();
}

void BufferCache::NotePreciseUpload(VAddr addr, u64 size) {
    if (!GuestInPlace() || WriteTracking() || size == 0) {
        return;
    }
    std::scoped_lock lk{late_writes_mutex};
    precise_uploads.emplace_back(addr, size);
    late_writes_pending.store(true, std::memory_order_release);
}

void BufferCache::ProcessLateWrites() {
    if (!late_writes_pending.load(std::memory_order_acquire)) {
        return;
    }
    std::vector<std::pair<VAddr, u64>> uploads;
    std::vector<std::pair<VAddr, std::vector<u8>>> writes;
    {
        std::scoped_lock lk{late_writes_mutex};
        writes.swap(late_writes);
        uploads.swap(precise_uploads);
        late_writes_pending.store(false, std::memory_order_release);
    }
    for (const auto& [addr, bytes] : writes) {
        const u64 size = bytes.size();
        const u64 first = addr >> block_shift, last = (addr + size - 1) >> block_shift;
        if (((addr | size) & 3) != 0 || !resident_ranges.Contains(first, last + 1) ||
            in_place_blocks.Overlaps(first, last + 1)) {
            continue; // the GPU reads the game's memory there (or nothing yet)
        }
        const auto* arena = GetArena(first, last);
        runtime.UpdateBuffer(arena, arena->Offset(addr), bytes);
        BbStats::late_vram_writes.fetch_add(1, std::memory_order_relaxed);
    }
    // CPU bytes next to GPU data in VRAM: copied by the GPU from the game's memory, block by
    // block (a VRAM block's guest memory is one contiguous run of a chunk).
    for (const auto& [addr, size] : uploads) {
        const VAddr end = addr + size;
        for (VAddr at = addr; at < end;) {
            const u64 block = at >> block_shift;
            const VAddr to = std::min<VAddr>(end, (block + 1) << block_shift);
            u64 phys = 0;
            uintptr_t mapping_end = 0;
            const BbGuestMemory::Chunk* chunk = nullptr;
            if (resident_ranges.Contains(block) && !in_place_blocks.Contains(block) &&
                runtime_memory_direct_phys(at, &phys, &mapping_end) && to <= mapping_end &&
                (chunk = BbGuestMemory::Find(phys)) != nullptr &&
                phys + (to - at) <= chunk->phys + chunk->size) {
                const auto* arena = GetArena(block, block);
                const vk::BufferCopy copy{phys - chunk->phys, at - arena->cpu_addr, to - at};
                runtime.CopyFromGuestChunk(chunk->buffer, arena, std::span{&copy, 1});
                BbStats::late_vram_writes.fetch_add(1, std::memory_order_relaxed);
            }
            at = to;
        }
    }
}

std::optional<std::pair<const Buffer*, u64>> BufferCache::ShadowCopy(VAddr addr, u64 size) {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_SHADOWS");
        return env && env[0] == '1';
    }();
    constexpr u64 MaxShadow = 4_MB, ShadowRing = 128_MB;
    // A GPU write there not done yet: the copy (through the chunk's own buffer) would not wait
    // for it; the arena is read instead.
    if (!enabled || !GarlicInVram() || WriteTracking() || size > MaxShadow ||
        IsRegionGpuModified(addr, size)) {
        return std::nullopt;
    }
    if (const u64 tick = scheduler.CurrentTick(); tick != shadow_tick) {
        shadows.clear();
        shadow_tick = tick;
    }
    for (const auto& shadow : shadows) {
        if (shadow.addr == addr && shadow.size == size) {
            return std::pair<const Buffer*, u64>{shadow_buffer.get(), shadow.offset};
        }
    }
    const auto source = GuestChunkSource(addr, size);
    if (!source) {
        return std::nullopt;
    }
    if (!shadow_buffer) {
        shadow_buffer = std::make_unique<StreamBuffer>(instance, scheduler, MemoryType::DeviceLocal,
                                                       ShadowRing);
    }
    const auto offset = shadow_buffer->Reserve(size, instance.StorageMinAlignment());
    if (!offset) {
        return std::nullopt;
    }
    const vk::BufferCopy copy{source->second, *offset, size};
    runtime.CopyBuffer(source->first, shadow_buffer.get(), std::span{&copy, 1});
    shadows.push_back({addr, size, *offset});
    BbStats::shadow_copies.fetch_add(1, std::memory_order_relaxed);
    BbStats::shadow_bytes.fetch_add(size, std::memory_order_relaxed);
    return std::pair<const Buffer*, u64>{shadow_buffer.get(), *offset};
}

void BufferCache::DropShadows(VAddr addr, u64 size) {
    std::erase_if(shadows, [&](const Shadow& shadow) {
        return addr < shadow.addr + shadow.size && shadow.addr < addr + size;
    });
}

bool BufferCache::UpdateGpuWritten(VAddr addr, std::span<const u8> data) {
    const u64 size = data.size();
    if (!GuestInPlace() || WriteTracking() || size == 0 || ((addr | size) & 3) != 0 ||
        IsAnyInPlace(addr, size) || !memory_tracker->IsRegionGpuModified(addr, size)) {
        return false;
    }
    const u64 first = addr >> block_shift, last = (addr + size - 1) >> block_shift;
    if (!resident_ranges.Contains(first, last + 1)) {
        return false;
    }
    const auto* arena = GetArena(first, last);
    runtime.UpdateBuffer(arena, arena->Offset(addr), data);
    BbStats::gpu_data_updates.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void BufferCache::NoteGpuWrite(VAddr addr, u64 size) {
    if (size == 0 || gpu_written_bytes.Contains(addr, addr + size)) {
        return;
    }
    // Only what the GPU writes regularly (in 3 frames of 60) is its data, like a default heap: a
    // buffer the GPU writes only now and then stays in place, where writes by the CPU are seen
    // (0x104ff30000, 192 KiB, written by the GPU every few seconds: in VRAM the picture smeared
    // while moving).
    constexpr u32 Frames = 3, Window = 60;
    const u32 frame = static_cast<u32>(BbStats::frame_number.load(std::memory_order_relaxed));
    auto& [last, frames] = gpu_write_frames[addr ^ (size << 40)];
    if (last != frame || frames == 0) {
        frames = frame - last > Window ? 1 : frames + 1;
        last = frame;
    }
    if (frames < Frames) {
        if (gpu_write_frames.size() > 65536) {
            std::erase_if(gpu_write_frames,
                          [frame](const auto& entry) { return frame - entry.second.first > Window; });
        }
        return;
    }
    gpu_written_bytes.Add({addr, addr + size});
    // Blocks the GPU already writes in place move to VRAM once they are all known data.
    for (u64 block = addr >> block_shift; block <= (addr + size - 1) >> block_shift; ++block) {
        if (in_place_blocks.Contains(block) && resident_ranges.Contains(block) &&
            VramEligible(block)) {
            promote_candidates.Add({block, block + 1});
        }
    }
}

void BufferCache::QueueCopyBacks(u64 submitted) {
    if (copy_back_blocks.Empty()) {
        return;
    }
    std::vector<u64> blocks;
    for (const auto& range : copy_back_blocks) {
        for (u64 block = range.start; block < range.end; ++block) {
            blocks.push_back(block);
        }
    }
    copy_back_blocks.Clear();
    bool moved = false;
    for (const u64 block : blocks) {
        const VAddr address = block << block_shift;
        u64 phys = 0;
        uintptr_t mapping_end = 0;
        const BbGuestMemory::Chunk* chunk = nullptr;
        if (!resident_ranges.Contains(block) || in_place_blocks.Contains(block) ||
            !runtime_memory_direct_phys(address, &phys, &mapping_end) ||
            (phys & (block_size - 1)) != 0 || mapping_end - address < block_size ||
            (chunk = BbGuestMemory::Find(phys)) == nullptr ||
            !address_space[block >> blocks_per_arena_page_shift]) {
            continue;
        }
        const auto source = GuestChunkSource(address, block_size);
        if (!source) {
            continue;
        }
        // What the GPU wrote there since the block was handed out (not the bytes handed out
        // again: the CPU may be writing them now), copied at the end of this submission.
        const auto* arena = GetArena(block, block);
        boost::container::small_vector<vk::BufferCopy, 8> copies;
        gpu_written_bytes.ForEachInRange(address, address + block_size, [&](const Interval& iv) {
            const VAddr from = std::max<VAddr>(iv.start, address);
            const VAddr to = std::min<VAddr>(iv.end, address + block_size);
            if (from < to) {
                copies.push_back({arena->Offset(from), source->second + (from - address), to - from});
            }
        });
        if (!copies.empty()) {
            runtime.CopyBuffer(arena, source->first, copies);
        }
        // Then in place from the next submission on (as ProcessDemotions).
        if (const auto old = resident_ranges.Find(block); old != resident_ranges.end()) {
            ReleaseResidencySlot(old->memory, (old->offset + (block - old->start)) << block_shift);
        }
        Backing backing;
        backing.start = block;
        backing.end = block + 1;
        backing.memory = chunk->memory;
        backing.offset = (phys - chunk->phys) >> block_shift;
        resident_ranges.Subtract(block, block + 1);
        resident_ranges.Add(backing);
        in_place_blocks.Add({block, block + 1});
        for (const auto& covering : arenas) {
            if (covering.cpu_addr <= address && address - covering.cpu_addr < covering.size_bytes) {
                BindsForArena(&covering)->binds.emplace_back(vk::SparseMemoryBind{
                    .resourceOffset = address - covering.cpu_addr,
                    .size = block_size,
                    .memory = chunk->memory,
                    .memoryOffset = backing.offset << block_shift,
                });
            }
        }
        memory_tracker->UnmarkRegionAsGpuModified(address, block_size);
        gpu_modified_ranges.Subtract(address, block_size);
        memory_tracker->MarkRegionAsCpuModified(address, block_size);
        BbStats::copied_back_blocks.fetch_add(1, std::memory_order_relaxed);
        moved = true;
    }
    if (moved) {
        bind_wait_tick = std::max(bind_wait_tick, submitted);
    }
}

void BufferCache::QueuePromotions(u64 submitted) {
    if (promote_candidates.Empty()) {
        return;
    }
    std::vector<u64> blocks;
    for (const auto& range : promote_candidates) {
        for (u64 block = range.start; block < range.end; ++block) {
            blocks.push_back(block);
        }
    }
    promote_candidates.Clear();
    u64 moved = 0;
    for (const u64 block : blocks) {
        const VAddr address = block << block_shift;
        // What the GPU wrote in place is in the game's memory: the copy is filled from there.
        if (!in_place_blocks.Contains(block) || !resident_ranges.Contains(block) ||
            !VramEligible(block)) {
            continue;
        }
        memory_tracker->UnmarkRegionAsGpuModified(address, block_size);
        gpu_modified_ranges.Subtract(address, block_size);
        const auto [memory, offset] = AllocateResidency(block_size);
        Backing backing;
        backing.start = block;
        backing.end = block + 1;
        backing.memory = memory;
        backing.offset = offset >> block_shift;
        resident_ranges.Subtract(block, block + 1);
        resident_ranges.Add(backing);
        in_place_blocks.Subtract(block, block + 1);
        // Every arena over it (as ProcessDemotions). The commands recorded so far read it in place;
        // the bind waits for them, and the next submission fills the VRAM copy before reading it.
        for (const auto& arena : arenas) {
            if (arena.cpu_addr <= address && address - arena.cpu_addr < arena.size_bytes) {
                BindsForArena(&arena)->binds.emplace_back(vk::SparseMemoryBind{
                    .resourceOffset = address - arena.cpu_addr,
                    .size = block_size,
                    .memory = memory,
                    .memoryOffset = offset,
                });
            }
        }
        memory_tracker->MarkRegionAsCpuModified(address, block_size);
        moved += block_size;
    }
    if (moved == 0) {
        return;
    }
    bind_wait_tick = std::max(bind_wait_tick, submitted);
    if (((promoted_bytes + moved) >> 20) != (promoted_bytes >> 20)) {
        std::printf("Guest memory: %llu MiB of loaded blocks moved from in place to VRAM\n",
                    (unsigned long long)((promoted_bytes + moved) >> 20));
    }
    promoted_bytes += moved;
}

void BufferCache::UnmapInPlace(VAddr addr, u64 size) {
    if (!GuestInPlace() || size == 0) {
        return;
    }
    std::scoped_lock lk{pending_unmaps_mutex};
    pending_unmaps.emplace_back(addr, size);
    unmaps_pending.store(true, std::memory_order_release);
}

void BufferCache::Maintain() {
    maintained_epoch = packet_epoch;
    ProcessPendingUnmaps();
    ProcessIdleBlocks();
    ProcessDemotions();
    ProcessPendingAssets();
    ProcessLateWrites();
}

void BufferCache::ProcessPendingUnmaps() {
    if (!unmaps_pending.load(std::memory_order_acquire)) {
        return;
    }
    std::vector<std::pair<VAddr, u64>> unmaps;
    {
        std::scoped_lock lk{pending_unmaps_mutex};
        unmaps.swap(pending_unmaps);
        unmaps_pending.store(false, std::memory_order_release);
    }
    // The blocks bound to memory the game no longer maps there: unbound, and bound again (to
    // whatever is mapped then) when next used. Partly unmapped blocks go too.
    for (const auto& [addr, size] : unmaps) {
        const u64 first = addr >> block_shift, end = (addr + size + block_size - 1) >> block_shift;
        IntervalList<> unbind;
        asset_bytes.Subtract(first << block_shift, end << block_shift);
        gpu_written_bytes.Subtract(first << block_shift, end << block_shift);
        copy_back_blocks.Subtract(first, end);
        promote_candidates.Subtract(first, end);
        idle_demoted.Subtract(first, end);
        in_place_blocks.ForEachInRange(first, end, [&](const Interval& iv) {
            unbind.Add({std::max(first, iv.start), std::min(end, iv.end)});
        });
        for (const auto& range : unbind) {
            const VAddr from = range.start << block_shift, to = range.end << block_shift;
            for (const auto& arena : arenas) {
                const VAddr a = std::max<VAddr>(from, arena.cpu_addr);
                const VAddr b = std::min<VAddr>(to, arena.cpu_addr + arena.size_bytes);
                if (a < b) {
                    BindsForArena(&arena)->binds.emplace_back(vk::SparseMemoryBind{
                        .resourceOffset = a - arena.cpu_addr,
                        .size = b - a,
                        .memory = {},
                        .memoryOffset = 0,
                    });
                }
            }
            resident_ranges.Subtract(range.start, range.end);
            in_place_blocks.Subtract(range.start, range.end);
            asset_bytes.Subtract(range.start << block_shift, range.end << block_shift);
            gpu_written_bytes.Subtract(range.start << block_shift, range.end << block_shift);
            promote_candidates.Subtract(range.start, range.end);
        }
        // bbport: blocks wholly unmapped that have a VRAM copy leave VRAM too (their slots go
        // to the next blocks moved there; uploaded again if mapped and used).
        for (u64 block = (addr + block_size - 1) >> block_shift; block < (addr + size) >> block_shift;
             ++block) {
            EvictVramBlock(block);
        }
    }
}

bool BufferCache::IsInPlace(VAddr addr, u64 size) const {
    if (!GuestInPlace() || size == 0) {
        return false;
    }
    const u64 first = addr >> block_shift, last = (addr + size - 1) >> block_shift;
    // bbport: asked for every binding; the answer holds while the residency is unchanged.
    thread_local std::array<ResidencyMemo, 256> in_place_memo{};
    auto& memo = in_place_memo[first & (in_place_memo.size() - 1)];
    const u64 generation = ResidencyGeneration();
    if (memo.first != first || memo.last != last || memo.generation != generation) {
        memo = {first, last, generation, false, in_place_blocks.Contains(first, last + 1)};
    }
    return memo.in_place;
}

bool BufferCache::SynchronizeMemory(const Buffer* arena, VAddr device_addr, u32 size,
                                    bool is_written, bool is_texel_buffer) {
    boost::container::small_vector<std::pair<vk::Buffer, vk::BufferCopy>, 4> chunk_copies;
    // bbport BB_GUEST_IN_PLACE: the arena is the game's memory there: nothing to upload, no
    // write tracking, and GPU writes need no readback. Images aliasing it still sync.
    if (IsInPlace(device_addr, size)) {
        return is_texel_buffer && !is_written && SynchronizeMemoryFromImage(arena, device_addr, size);
    }
    // Partly in place: only the parts in VRAM are synced. An upload over a block in place would
    // write an older copy of the game's memory back into it after the game wrote it again.
    if (GuestInPlace() && in_place_blocks.Overlaps(device_addr >> block_shift,
                                                  ((device_addr + size - 1) >> block_shift) + 1)) {
        const VAddr end = device_addr + size;
        bool image = false;
        in_place_blocks.ForEachGap(
            device_addr >> block_shift, ((end - 1) >> block_shift) + 1, [&](u64 first, u64 last) {
                const VAddr from = std::max<VAddr>(device_addr, first << block_shift);
                const VAddr to = std::min<VAddr>(end, last << block_shift);
                if (from < to) {
                    image |= SynchronizeMemory(arena, from, static_cast<u32>(to - from),
                                               is_written, is_texel_buffer);
                }
            });
        return image;
    }
    boost::container::small_vector<vk::BufferCopy, 4> copies;
    size_t total_size_bytes{};
    const Buffer* src_buffer{};
    memory_tracker->ForEachUploadRange(
        device_addr, size, is_written,
        [&](u64 addr, u64 size) {
            copies.emplace_back(total_size_bytes, addr, size);
            total_size_bytes += size;
        },
        [&] {
            // bbport (BB_GPU_WRITE_TWINS): bytes whose guest copy still matches its twin keep the
            // GPU's data in the arena; twin bytes the guest wrote since are uploaded and no
            // longer GPU-modified.
            if (TwinsEnabled() && !copies.empty()) {
                std::scoped_lock lk{twins_mutex};
                if (!twins.empty()) {
                    boost::container::small_vector<vk::BufferCopy, 4> kept;
                    boost::container::small_vector<std::pair<VAddr, VAddr>, 4> changed;
                    u64 offset = 0;
                    for (const auto& copy : copies) {
                        ForEachTwinRun(copy.dstOffset, copy.dstOffset + copy.size,
                                       [&](VAddr from, VAddr to, TwinRun run) {
                                           if (run == TwinRun::Unchanged) {
                                               twin_kept_bytes.fetch_add(to - from);
                                               return;
                                           }
                                           if (run == TwinRun::Changed) {
                                               twin_guest_bytes.fetch_add(to - from);
                                               gpu_modified_ranges.Subtract(from, to - from);
                                               changed.emplace_back(from, to);
                                           }
                                           kept.push_back({offset, from, to - from});
                                           offset += to - from;
                                       });
                    }
                    // Changed parts are uploaded now: their twins are done.
                    for (const auto& [from, to] : changed) {
                        EraseTwins(from, to);
                    }
                    copies.assign(kept.begin(), kept.end());
                    total_size_bytes = offset;
                }
            }
            // bbport BB_GUEST_IN_PLACE: what lies in a guest memory chunk is copied by the GPU
            // straight from it (no CPU copy into staging); the rest goes through staging.
            if (GuestInPlace() && ChunkUploads() && !copies.empty()) {
                boost::container::small_vector<vk::BufferCopy, 4> staged;
                u64 staged_bytes = 0;
                for (const auto& copy : copies) {
                    const VAddr address = copy.dstOffset;
                    u64 phys = 0;
                    uintptr_t mapping_end = 0;
                    const BbGuestMemory::Chunk* chunk = nullptr;
                    if (runtime_memory_direct_phys(address, &phys, &mapping_end) &&
                        address + copy.size <= mapping_end &&
                        (chunk = BbGuestMemory::Find(phys)) != nullptr &&
                        phys + copy.size <= chunk->phys + chunk->size) {
                        chunk_copies.push_back({chunk->buffer,
                                                {phys - chunk->phys, address - arena->cpu_addr,
                                                 copy.size}});
                        continue;
                    }
                    staged.push_back({staged_bytes, copy.dstOffset, copy.size});
                    staged_bytes += copy.size;
                }
                copies.assign(staged.begin(), staged.end());
                total_size_bytes = staged_bytes;
            }
            src_buffer = UploadCopies(arena, copies, total_size_bytes);
        });

    if (src_buffer) {
        runtime.CopyBuffer(src_buffer, arena, copies);
    }
    for (const auto& [chunk_buffer, copy] : chunk_copies) {
        BbStats::buffer_upload_bytes.fetch_add(copy.size, std::memory_order_relaxed);
        runtime.CopyFromGuestChunk(chunk_buffer, arena, std::span{&copy, 1});
    }
    if (is_texel_buffer && !is_written) {
        return SynchronizeMemoryFromImage(arena, device_addr, size);
    }
    return false;
}

const Buffer* BufferCache::UploadCopies(const Buffer* arena, std::span<vk::BufferCopy> copies,
                                        size_t total_size_bytes) {
    if (copies.empty()) {
        return nullptr;
    }
    BbStats::buffer_upload_bytes.fetch_add(total_size_bytes, std::memory_order_relaxed);
    const auto staging = staging_pool.Request(total_size_bytes, MemoryType::HostUncached);
    // bbport: the guest memory is copied into staging on the copy threads, started now in
    // groups of about 1 MiB; submission and guest-visible fences wait for them
    // (Scheduler::WaitHostCopies).
    if (!BbToggle::Disabled(BbToggle::DeferredUploads) && total_size_bytes < 1_MB) {
        // Small uploads join the calling thread's batch.
        for (auto& copy : copies) {
            SmallGuestCopy({
                .run = &RunGuestCopy,
                .context = this,
                .source = copy.dstOffset,
                .destination = reinterpret_cast<u64>(staging.mapped + copy.srcOffset),
                .size = copy.size,
                .extra = reinterpret_cast<u64>(staging.buffer),
            });
            copy.srcOffset += staging.offset;
            copy.dstOffset -= arena->cpu_addr;
        }
        return staging.buffer;
    }
    if (!BbToggle::Disabled(BbToggle::DeferredUploads)) {
        struct HostCopy {
            VAddr source;
            u8* destination;
            u64 size;
        };
        using Group = boost::container::small_vector<HostCopy, 8>;
        auto group = std::make_shared<Group>();
        u64 group_bytes = 0;
        const auto launch = [&] {
            BbCopy::Async([group, memory = memory, staging] {
                for (const auto& copy : *group) {
                    memory->CopySparseMemory(copy.source, copy.destination, copy.size);
                }
                staging.Flush();
            });
            for (const auto& copy : *group) {
                scheduler.NoteHostCopySource(copy.source, copy.size);
            }
        };
        for (auto& copy : copies) {
            group->push_back({copy.dstOffset, staging.mapped + copy.srcOffset, copy.size});
            group_bytes += copy.size;
            copy.srcOffset += staging.offset;
            copy.dstOffset -= arena->cpu_addr;
            if (group_bytes >= 1_MB) {
                launch();
                group = std::make_shared<Group>();
                group_bytes = 0;
            }
        }
        if (!group->empty()) {
            launch();
        }
        return staging.buffer;
    }
    const auto copy = [&](std::size_t i) {
        memory->CopySparseMemory(copies[i].dstOffset, staging.mapped + copies[i].srcOffset,
                                 copies[i].size);
    };
    if (total_size_bytes >= 2_MB && copies.size() > 1) {
        BbCopy::ParallelFor(copies.size(), copy);
    } else {
        for (std::size_t i = 0; i < copies.size(); ++i) {
            copy(i);
        }
    }
    for (auto& copy : copies) {
        copy.srcOffset += staging.offset;
        copy.dstOffset -= arena->cpu_addr;
    }
    staging.Flush();
    return staging.buffer;
}

bool BufferCache::SynchronizeMemoryFromImage(const Buffer* arena, VAddr device_addr, u32 size) {
    if (auto type = texture_cache.IsMeta(device_addr)) {
        if (*type == TextureCache::MetaType::HTile) {
            static constexpr u32 ZmaskUncompressed = 0xf;
            runtime.FillBuffer(arena, arena->Offset(device_addr), size, ZmaskUncompressed);
            return true;
        } else {
            LOG_WARNING(Render_Vulkan, "Unhandled metadata type {}", magic_enum::enum_name(*type));
        }
    }
    // bbport: most texel buffers alias no image; remember misses until images change.
    const u64 generation = texture_cache.RegistryGeneration();
    auto& miss = image_miss_cache[((device_addr >> 6) ^ size * 0x9E3779B1u) % image_miss_cache.size()];
    if (miss.address == device_addr && miss.size == size && miss.generation == generation &&
        !BbToggle::Disabled(BbToggle::TextureBindingMemo)) {
        return false;
    }
    const ImageId image_id = texture_cache.FindImageFromRange(device_addr, size);
    if (!image_id) {
        miss = {device_addr, size, generation};
        return false;
    }
    // bbport: the lookups above only read what the texture binding helper leaves alone; the
    // copy below changes image state, so the helper finishes first.
    runtime.BeforeImageAccess();
    Image& image = texture_cache.GetImage(image_id);
    ASSERT_MSG(device_addr == image.info.guest_address,
               "Texel buffer aliases image subresources {:x} : {:x}", device_addr,
               image.info.guest_address);
    const u64 arena_offset = arena->Offset(device_addr);
    boost::container::small_vector<vk::BufferImageCopy, 8> buffer_copies;
    for (u32 mip = 0; mip < image.info.resources.levels; mip++) {
        const auto& mip_info = image.info.mips_layout[mip];
        const u32 width = std::max(image.info.size.width >> mip, 1u);
        const u32 height = std::max(image.info.size.height >> mip, 1u);
        const u32 depth = std::max(image.info.size.depth >> mip, 1u);
        if (arena_offset + mip_info.offset + mip_info.size > arena->size_bytes) {
            break;
        }
        buffer_copies.push_back(vk::BufferImageCopy{
            .bufferOffset = mip_info.offset,
            .bufferRowLength = mip_info.pitch,
            .bufferImageHeight = mip_info.height,
            .imageSubresource{
                .aspectMask = image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = mip,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {width, height, depth},
        });
    }
    if (buffer_copies.empty()) {
        return false;
    }
    auto& tile_manager = texture_cache.GetTileManager();
    tile_manager.TileImage(image, buffer_copies, arena, arena_offset);
    DropShadows(device_addr, size);
    // A GPU write like any other: what the GPU copies there each frame (a render target read as a
    // texel buffer) goes to VRAM instead of crossing PCIe twice (written there, read back).
    if (GuestInPlace() && !WriteTracking()) {
        NoteGpuWrite(device_addr, size);
    }
    return true;
}


void BufferCache::NotePreuploadMapping(VAddr addr, u64 size, bool mapped) {
    if (!PreuploadEnabled() || size == 0) {
        return;
    }
    if (mapped) {
        // Only the game's GPU memory: direct memory of type 3 (write-combined "garlic") that the
        // GPU may access. Other memory is the CPU's; pre-uploading it would only cost VRAM.
        int prot = 0, type = -1;
        uintptr_t end = 0;
        if (!runtime_memory_vma_info(addr, &prot, &type, &end) || type != 3 || !(prot & 0x30)) {
            return;
        }
    }
    std::scoped_lock lk{preupload_mutex};
    const VAddr end = addr + size;
    // Remove what overlaps [addr, end), keeping the parts outside it.
    for (auto it = preupload_ranges.lower_bound(addr) == preupload_ranges.begin()
                       ? preupload_ranges.begin()
                       : std::prev(preupload_ranges.lower_bound(addr));
         it != preupload_ranges.end() && it->first < end;) {
        const VAddr start = it->first, stop = it->second;
        if (stop <= addr) {
            ++it;
            continue;
        }
        it = preupload_ranges.erase(it);
        if (start < addr) {
            preupload_ranges[start] = addr;
        }
        if (stop > end) {
            preupload_ranges[end] = stop;
        }
    }
    if (mapped) {
        preupload_ranges[addr] = end;
    }
}

void BufferCache::DropTwins(VAddr addr, u64 size) {
    if (TwinsEnabled()) {
        std::scoped_lock lk{twins_mutex};
        EraseTwins(addr, addr + size);
    }
}

void BufferCache::Preupload(u64 budget) {
    if (!PreuploadEnabled()) {
        return;
    }
    const bool full = PreuploadMode() == 2;
    // BB_FRAME_STATS: what the pre-upload put into the arena: regions for the first time, or again
    // (rewritten by the game since).
    static u32 report_frame = 0;
    if (BbStats::enabled && BbStats::frame_number.load(std::memory_order_relaxed) - report_frame >= 600) {
        report_frame = BbStats::frame_number.load(std::memory_order_relaxed);
        std::printf("Pre-upload: %.1f MB first, %.1f MB again in 600 frames; GPU write twins: %llu "
                    "writes without waiting, %.1f KB kept for the GPU, %.1f KB written by the game\n",
                    preupload_first_bytes / 1e6, preupload_again_bytes / 1e6,
                    (unsigned long long)twin_faults.exchange(0), twin_kept_bytes.exchange(0) / 1e3,
                    twin_guest_bytes.exchange(0) / 1e3);
        preupload_first_bytes = preupload_again_bytes = 0;
    }
    // Regions written within these frames are left alone: the game is still filling them.
    constexpr u32 QuietFrames = 10;
    // Regions looked at per call (CPU time bound): 256 x 4 MiB.
    constexpr u32 MaxRegions = 256;
    std::vector<std::pair<VAddr, VAddr>> ranges;
    {
        std::scoped_lock lk{preupload_mutex};
        ranges.assign(preupload_ranges.begin(), preupload_ranges.end());
    }
    if (ranges.empty()) {
        return;
    }
    const u32 frame = BbStats::frame_number.load(std::memory_order_relaxed);
    const u32 quiet_since = frame > QuietFrames ? frame - QuietFrames : 0;
    const u64 uploaded_before = BbStats::buffer_upload_bytes.load(std::memory_order_relaxed);
    // Start at the range holding the cursor (or the next one), wrap around once.
    std::size_t first = 0;
    while (first < ranges.size() && ranges[first].second <= preupload_cursor) {
        ++first;
    }
    if (first == ranges.size()) {
        first = 0;
    }
    u32 visited = 0;
    for (std::size_t n = 0; n < ranges.size() && visited < MaxRegions; ++n) {
        const auto [start, end] = ranges[(first + n) % ranges.size()];
        VAddr at = n == 0 ? std::max(start, preupload_cursor) : start;
        if (at >= end) {
            at = start;
        }
        while (at < end && visited < MaxRegions) {
            const VAddr region = at & ~(TRACKER_HIGHER_PAGE_SIZE - 1);
            const VAddr stop = std::min(end, region + TRACKER_HIGHER_PAGE_SIZE);
            ++visited;
            preupload_cursor = stop;
            if (memory_tracker->PreuploadCandidate(region, quiet_since, !full)) {
                const u64 first_block = at >> block_shift;
                const u64 last_block = (stop - 1) >> block_shift;
                const auto* arena = GetArena(first_block, last_block);
                const u64 region_before =
                    BbStats::buffer_upload_bytes.load(std::memory_order_relaxed);
                if (full) {
                    EnsureResident(arena, first_block, last_block);
                    SynchronizeMemory(arena, at, static_cast<u32>(stop - at), false, false);
                } else {
                    // Mode 1: only the parts already resident in the arena (no new VRAM).
                    const auto sync = [&](u64 block_begin, u64 block_end) {
                        const VAddr from = std::max<VAddr>(at, block_begin << block_shift);
                        const VAddr to = std::min<VAddr>(stop, block_end << block_shift);
                        if (from < to) {
                            SynchronizeMemory(arena, from, static_cast<u32>(to - from), false,
                                              false);
                        }
                    };
                    u64 cursor = first_block;
                    resident_ranges.ForEachGap(first_block, last_block + 1,
                                               [&](u64 gap_begin, u64 gap_end) {
                                                   sync(cursor, gap_begin);
                                                   cursor = gap_end;
                                               });
                    sync(cursor, last_block + 1);
                }
                const u64 region_bytes =
                    BbStats::buffer_upload_bytes.load(std::memory_order_relaxed) - region_before;
                if (memory_tracker->NotePreuploaded(region, frame) > 0) {
                    preupload_again_bytes += region_bytes;
                } else {
                    preupload_first_bytes += region_bytes;
                }
                const u64 uploaded =
                    BbStats::buffer_upload_bytes.load(std::memory_order_relaxed) - uploaded_before;
                if (uploaded >= budget) {
                    BbStats::preupload_bytes.fetch_add(uploaded, std::memory_order_relaxed);
                    return;
                }
            }
            at = stop;
        }
    }
    BbStats::preupload_bytes.fetch_add(
        BbStats::buffer_upload_bytes.load(std::memory_order_relaxed) - uploaded_before,
        std::memory_order_relaxed);
}

void BufferCache::SubmitPendingArenaBinds(Vulkan::SubmitInfo& info) {
    // The submission going out has the tick before the current one.
    const u64 submitted = scheduler.CurrentTick() - 1;
    if (pending_binds.empty()) {
        QueueCopyBacks(submitted);
        QueuePromotions(submitted);
        return;
    }

    // bbport BB_GUEST_IN_PLACE: blocks moved in place wait for the submission that may still
    // upload into their VRAM copy (ProcessDemotions).
    const u64 wait_tick = bind_wait_tick;
    const auto wait_sema = scheduler.GetWorkSemaphore()->Handle();
    bind_wait_tick = 0;
    const u64 signal_tick = memory_semaphore.NextTick();
    const auto signal_sema = memory_semaphore.Handle();
    info.AddWait(signal_sema, signal_tick);

    // The binds go into the queue right before the submission (BB_ASYNC_SUBMIT: later, from a
    // recording thread), after the submissions still on their way there.
    auto binds = std::make_shared<std::vector<ArenaBinds>>(std::move(pending_binds));
    pending_binds.clear();
    info.before_submit.push_back([this, binds, wait_tick, wait_sema, signal_tick, signal_sema] {
        std::vector<vk::SparseBufferMemoryBindInfo> buffer_binds;
        buffer_binds.reserve(binds->size());
        for (const auto& arena_binds : *binds) {
            buffer_binds.emplace_back(vk::SparseBufferMemoryBindInfo{
                .buffer = arena_binds.arena->Handle(),
                .bindCount = static_cast<u32>(arena_binds.binds.size()),
                .pBinds = arena_binds.binds.data(),
            });
        }
        const vk::TimelineSemaphoreSubmitInfo timeline_si = {
            .waitSemaphoreValueCount = wait_tick ? 1u : 0u,
            .pWaitSemaphoreValues = wait_tick ? &wait_tick : nullptr,
            .signalSemaphoreValueCount = 1u,
            .pSignalSemaphoreValues = &signal_tick,
        };
        const vk::BindSparseInfo sparse_info = {
            .pNext = &timeline_si,
            .waitSemaphoreCount = wait_tick ? 1u : 0u,
            .pWaitSemaphores = wait_tick ? &wait_sema : nullptr,
            .bufferBindCount = static_cast<u32>(buffer_binds.size()),
            .pBufferBinds = buffer_binds.data(),
            .signalSemaphoreCount = 1u,
            .pSignalSemaphores = &signal_sema,
        };
        auto submit_result = instance.GetGraphicsQueue().bindSparse(sparse_info);
        if (submit_result == vk::Result::eErrorDeviceLost) {
            Vulkan::Breadcrumbs::ReportDeviceLost("submit");
        }
        ASSERT_MSG(submit_result != vk::Result::eErrorDeviceLost, "Device lost during submit");
    });
    QueueCopyBacks(submitted); // bound with the next submission
    QueuePromotions(submitted);
}

} // namespace VideoCore

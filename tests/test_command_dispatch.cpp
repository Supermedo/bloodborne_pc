// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise the production dispatcher with a real draw-pipe worker and Vulkan recorder.
#include <atomic>
#include <cassert>
#include <cstring>
#include <functional>
#include <mutex>
#include <queue>
#include <semaphore>
#include <thread>
#ifdef _WIN32
#include "bb_win_compat.h"
#endif
#include "video_core/amdgpu/command_dispatch.h"
#include "video_core/renderer_vulkan/vk_draw_pipe.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include <vk_mem_alloc.h>
#include "test_assert.h"

namespace {
struct BlockedDraw {
    std::binary_semaphore entered{0}, release{0};
    std::atomic<bool> active{false};
    static void Run(void* context, const u8*, u32) {
        auto& draw = *static_cast<BlockedDraw*>(context);
        draw.active.store(true);
        draw.entered.release();
        draw.release.acquire();
        draw.active.store(false);
    }
};

void LateArrival() {
    BlockedDraw draw;
    Vulkan::DrawPipe pipe(BlockedDraw::Run, &draw);
    *pipe.Begin(1) = 0;
    pipe.Commit(1);
    draw.entered.acquire();
    std::queue<std::function<void()>> commands;
    unsigned executed = 0, drained = 0;
    bool inject = true;
    const auto pending = [&] {
        const bool had_work = !commands.empty();
        if (inject) {
            // A guest enqueues immediately AFTER the command processor observed empty.
            inject = false;
            commands.emplace([&] {
                assert(!draw.active.load());
                assert(pipe.Idle());
                ++executed;
            });
        }
        return had_work;
    };
    const auto pop = [&] {
        auto callback = std::move(commands.front());
        commands.pop();
        return callback;
    };
    const auto drain = [&] {
        ++drained;
        draw.release.release();
        pipe.Drain(Vulkan::DrawPipe::ReasonCommands);
    };
    AmdGpu::DispatchHostCommands(pending, pop, drain);
    assert(executed == 0 && drained == 0);
    AmdGpu::DispatchHostCommands(pending, pop, drain);
    assert(executed == 1 && drained == 1);
}

void DrainEveryCallback() {
    BlockedDraw draw;
    Vulkan::DrawPipe pipe(BlockedDraw::Run, &draw);
    std::queue<std::function<void()>> commands;
    unsigned drained = 0, executed = 0;
    commands.emplace([&] {
        assert(drained == 1);
        ++executed;
        *pipe.Begin(1) = 0;
        pipe.Commit(1);
        draw.entered.acquire();
        // Reentrant enqueue must not hold the dispatcher's queue lock or skip the next drain.
        commands.emplace([&] {
            assert(drained == 2 && pipe.Idle() && !draw.active.load());
            ++executed;
        });
    });
    AmdGpu::DispatchHostCommands([&] { return !commands.empty(); }, [&] {
        auto callback = std::move(commands.front());
        commands.pop();
        return callback;
    }, [&] {
        if (++drained == 2) draw.release.release();
        pipe.Drain(Vulkan::DrawPipe::ReasonCommands);
    });
    assert(executed == 2 && drained == 2);
}

struct Recorder {
    Vulkan::Scheduler& scheduler;
    vk::Buffer buffer;
    vk::detail::DispatchLoaderDynamic& dispatcher;
    std::atomic<unsigned> host_copies{0};
    static void Draw(void* context, const u8* packet, u32 size) {
        assert(size == sizeof(u32));
        u32 slot;
        std::memcpy(&slot, packet, sizeof(slot));
        static_cast<Recorder*>(context)->Record(slot);
    }
    void Record(u32 slot) {
        // Force 128 KiB chunk rollover, ordered host copies, and segment handovers.
        for (unsigned i = 0; i < 4096; ++i) {
            scheduler.Record([](vk::CommandBuffer) {});
        }
        scheduler.RecordHostCopy([this] { host_copies.fetch_add(1); });
        scheduler.Record([buffer = buffer, slot, d = &dispatcher](vk::CommandBuffer cmd) {
            cmd.fillBuffer(buffer, slot * sizeof(u32), sizeof(u32), slot + 1, *d);
        });
        scheduler.KickRecording();
    }
};

void VulkanStress() {
    Vulkan::Instance instance(0, false);
    static vk::detail::DynamicLoader loader;
    vk::detail::DispatchLoaderDynamic d;
    d.init(loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
    d.init(instance.GetInstance());
    d.init(instance.GetDevice());
    Vulkan::Scheduler scheduler(instance, true);
    constexpr u32 Count = 256;
    VkBuffer buffer{};
    VmaAllocation allocation{};
    VmaAllocationInfo mapped{};
    const VkBufferCreateInfo bi{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = Count * sizeof(u32), .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const VmaAllocationCreateInfo ai{.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT, .usage = VMA_MEMORY_USAGE_AUTO};
    assert(vmaCreateBuffer(instance.GetAllocator(), &bi, &ai, &buffer, &allocation, &mapped) == VK_SUCCESS);
    Recorder recorder{scheduler, vk::Buffer{buffer}, d};
    {
        Vulkan::DrawPipe pipe(Recorder::Draw, &recorder);
        std::mutex mutex;
        std::queue<std::function<void()>> commands;
        std::binary_semaphore request{0}, enqueued{0};
        std::jthread guest([&] {
            for (u32 slot = 1; slot < Count; slot += 2) {
                request.acquire();
                {
                    std::scoped_lock lk{mutex};
                    commands.emplace([&, slot] {
                        assert(pipe.Idle());
                        recorder.Record(slot);
                        // Readback-like submission between the draw worker's packets.
                        if (slot % 16 == 15) scheduler.Finish();
                    });
                }
                enqueued.release();
            }
        });
        for (u32 slot = 0; slot < Count; slot += 2) {
            std::memcpy(pipe.Begin(sizeof(slot)), &slot, sizeof(slot));
            pipe.Commit(sizeof(slot));
            request.release();
            enqueued.acquire();
            AmdGpu::DispatchHostCommands([&] {
                std::scoped_lock lk{mutex};
                return !commands.empty();
            }, [&] {
                std::scoped_lock lk{mutex};
                auto callback = std::move(commands.front());
                commands.pop();
                return callback;
            }, [&] { pipe.Drain(Vulkan::DrawPipe::ReasonCommands); });
        }
        pipe.Drain();
    }
    scheduler.Record([&d](vk::CommandBuffer cmd) {
        const vk::MemoryBarrier2 barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eHost,
            .dstAccessMask = vk::AccessFlagBits2::eHostRead};
        cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier}, d);
    });
    scheduler.Finish();
    assert(recorder.host_copies.load() == Count);
    assert(vmaInvalidateAllocation(instance.GetAllocator(), allocation, 0, VK_WHOLE_SIZE) == VK_SUCCESS);
    const auto* values = static_cast<const u32*>(mapped.pMappedData);
    for (u32 slot = 0; slot < Count; ++slot) assert(values[slot] == slot + 1);
    vmaDestroyBuffer(instance.GetAllocator(), buffer, allocation);
}
} // namespace

int main(int argc, char**) {
    LateArrival();
    DrainEveryCallback();
    if (argc == 1) VulkanStress();
    std::puts("Command arrival, cache ownership, host copies and GPU readback: OK");
}

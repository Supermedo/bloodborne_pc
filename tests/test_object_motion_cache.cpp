// SPDX-License-Identifier: GPL-2.0-or-later
// Full readback/validity regression across alternating sizes and cache eviction.
#include "bbport_settings.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_object_motion.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <set>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <vk_mem_alloc.h>

using namespace Vulkan;
#ifdef _WIN32
extern "C" void runtime_restart() { std::abort(); }
#endif

namespace Vulkan {
struct ObjectMotionCacheTest {
    static std::unique_ptr<ObjectMotion> Make(const Instance& instance, Scheduler& scheduler) {
        if (std::getenv("BB_OMV_TEST_SSBO"))
            return std::unique_ptr<ObjectMotion>(new ObjectMotion(instance, scheduler, true));
        return std::make_unique<ObjectMotion>(instance, scheduler);
    }
    static vk::Image RawImage(const ObjectMotion& motion) { return vk::Image(motion.image); }
    static size_t Retired(const ObjectMotion& motion) { return motion.retired_images.size(); }
    static u64 RetiredBytes(const ObjectMotion& motion) { return motion.retired_bytes; }
    static void Collect(ObjectMotion& motion) { motion.CollectRetiredImages(); }
    static u64 CachedTick(const ObjectMotion& motion, u32 w, u32 h) {
        for (const auto& cached : motion.cached_images)
            if (cached.image && cached.width == w && cached.height == h)
                return cached.last_tick;
        throw std::runtime_error("expected cached target missing");
    }
    static bool Has(const ObjectMotion& motion, u32 w, u32 h) {
        if (motion.image && motion.image_width == w && motion.image_height == h)
            return true;
        return std::any_of(motion.cached_images.begin(), motion.cached_images.end(),
                           [=](const auto& c) { return c.image && c.width == w && c.height == h; });
    }
    static void Limits(const ObjectMotion& motion) {
        if (Retired(motion) > ObjectMotion::MaxRetiredImages ||
            RetiredBytes(motion) > ObjectMotion::MaxRetiredBytes)
            throw std::runtime_error("retirement exceeds its count/VRAM limit");
    }
};
} // namespace Vulkan

void Require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

bool CacheEnabled() {
    const char* setting = std::getenv("BB_OBJECT_MOTION_CACHE");
    return setting && std::strcmp(setting, "1") == 0;
}

u32 Allocations(const Instance& instance) {
    VmaTotalStatistics stats{};
    vmaCalculateStatistics(instance.GetAllocator(), &stats);
    return stats.total.statistics.allocationCount;
}

void AttachSize(ObjectMotion& motion, Scheduler& scheduler, u32 w, u32 h) {
    RenderState state{};
    state.width = w;
    state.height = h;
    state.num_layers = 1;
    motion.Attach(state, w, h);
    scheduler.BeginRendering(state);
    scheduler.EndRendering();
}

// A real queued submission stays blocked on a host-signalled timeline
// semaphore. The watchdog attempts release if a regression blocks the CPU.
// Driver/device failures can still prevent completion; signal errors are
// reported on the parent thread, never thrown across it.
class Gate {
    vk::Device device;
    vk::UniqueSemaphore semaphore;
    std::atomic<bool> release{false};
    std::exception_ptr signal_error;
    std::jthread watchdog;
    void Join() {
        release.store(true);
        if (watchdog.joinable())
            watchdog.join();
    }

public:
    explicit Gate(vk::Device device, bool fail_after_signal = false) : device{device} {
        const vk::SemaphoreTypeCreateInfo type{.semaphoreType = vk::SemaphoreType::eTimeline};
        auto [result, value] = device.createSemaphoreUnique({.pNext = &type});
        Require(result == vk::Result::eSuccess, "gate semaphore allocation failed");
        semaphore = std::move(value);
        watchdog = std::jthread([this, fail_after_signal] {
            try {
                const auto deadline =
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(750);
                while (!release.load() && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                Require(this->device.signalSemaphore({.semaphore = *semaphore, .value = 1}) ==
                            vk::Result::eSuccess,
                        "gate signal failed");
                // Exercise error propagation after safely releasing queued GPU work.
                Require(!fail_after_signal, "injected gate reporting failure");
            } catch (...) {
                signal_error = std::current_exception();
            }
        });
    }
    void Submit(Scheduler& scheduler) {
        SubmitInfo info{};
        info.AddWait(*semaphore);
        scheduler.Flush(info);
    }
    void Release() {
        Join();
        if (signal_error)
            std::rethrow_exception(signal_error);
    }
    ~Gate() noexcept {
        if (watchdog.joinable()) {
            Join();
            if (signal_error)
                std::fputs("FAIL: gate signal error during cleanup\n", stderr);
        }
    }
};

void SpecialCase(const std::string& kind, Instance& instance, Scheduler& scheduler) {
    const u32 before = Allocations(instance);
    {
        auto motion_owner = ObjectMotionCacheTest::Make(instance, scheduler);
        auto& motion = *motion_owner;
        Require(motion.Enabled(), "object motion unavailable");
        const u32 initial = Allocations(instance);
        if (kind == "destruct" || kind == "drain") {
            for (auto [w, h] : std::vector<std::pair<u32, u32>>{
                     {128, 72}, {96, 54}, {64, 36}, {48, 27}, {32, 18}, {16, 9}})
                AttachSize(motion, scheduler, w, h);
            Require(ObjectMotionCacheTest::Retired(motion) == 2,
                    "expected two unsubmitted retirements");
            Require(Allocations(instance) == initial + 6,
                    "retained images missing before completion");
            if (kind == "drain") {
                scheduler.Finish();
                motion.OnFrameStart();
                Require(ObjectMotionCacheTest::Retired(motion) == 0,
                        "frame start did not collect retired images");
            }
            // 'destruct' leaves current work unsubmitted: finish it before releasing
            // both the resident targets and the retirement queue.
        } else if (kind == "lru") {
            for (auto [w, h] : std::vector<std::pair<u32, u32>>{
                     {128, 72}, {96, 54}, {64, 36}, {48, 27}, {128, 72}, {32, 18}})
                AttachSize(motion, scheduler, w, h);
            Require(ObjectMotionCacheTest::Has(motion, 128, 72),
                    "recently revisited A was evicted");
            Require(!ObjectMotionCacheTest::Has(motion, 96, 54),
                    "least recently used B was retained");
            Require(ObjectMotionCacheTest::Has(motion, 64, 36) &&
                        ObjectMotionCacheTest::Has(motion, 48, 27),
                    "a newer cache entry was evicted instead");
            scheduler.Finish();
            ObjectMotionCacheTest::Collect(motion);
        } else {
            const bool bytes = kind == "budget";
            const bool oversized = kind == "oversized";
            AttachSize(motion, scheduler, 128, 72);
            AttachSize(motion, scheduler,
                       oversized ? 4096
                       : bytes   ? 2560
                                 : 96,
                       oversized ? 2160
                       : bytes   ? 1792
                                 : 54);
            AttachSize(motion, scheduler, bytes ? 2688 : 64, bytes ? 1792 : 36);
            AttachSize(motion, scheduler, 48, 27);
            const u64 held = scheduler.CurrentTick();
            Gate gate(instance.GetDevice());
            gate.Submit(scheduler);
            Require(!scheduler.IsFree(held), "gate did not hold GPU work");
            // A gets a new use in the current submission; B is the oldest inactive size.
            AttachSize(motion, scheduler, 128, 72);
            const u64 current = scheduler.CurrentTick();
            AttachSize(motion, scheduler, 32, 18);
            if (oversized) {
                Require(scheduler.IsFree(held), "oversized retirement did not await completion");
                Require(ObjectMotionCacheTest::Retired(motion) == 0,
                        "oversized image was deferred");
            } else {
                Require(!scheduler.IsFree(held), "first eviction waited for unfinished GPU work");
                Require(ObjectMotionCacheTest::Retired(motion) == 1,
                        "old B image was not retained");
                Require(Allocations(instance) == initial + 5,
                        "pending image was freed before its GPU tick");
                AttachSize(motion, scheduler, 16, 9);
                if (bytes) {
                    Require(scheduler.IsFree(held), "VRAM retirement limit failed to wait");
                    Require(ObjectMotionCacheTest::Retired(motion) == 0,
                            "completed images not released");
                } else {
                    Require(!scheduler.IsFree(held), "second eviction unexpectedly waited");
                    Require(ObjectMotionCacheTest::Retired(motion) == 2,
                            "two pending images expected");
                    Require(Allocations(instance) == initial + 6,
                            "pending allocation accounting differs");
                    // The count cap must await the older submission.
                    AttachSize(motion, scheduler, 8, 4);
                    Require(scheduler.IsFree(held), "retirement count cap did not wait");
                    // D was stashed in the current tick after A was revisited.
                    // Its bound must survive even though the older B/C completed.
                    Require(ObjectMotionCacheTest::Retired(motion) == 1,
                            "current-tick retirement was not retained");
                }
            }
            // A full Finish would submit current work too; the narrow wait must not.
            Require(scheduler.CurrentTick() == current,
                    "eviction drained/submitted current GPU work");
            ObjectMotionCacheTest::Limits(motion);
            gate.Release();
            scheduler.Finish();
            ObjectMotionCacheTest::Collect(motion);
        }
        if (kind != "destruct") {
            Require(ObjectMotionCacheTest::Retired(motion) == 0,
                    "retired images remain after GPU completion");
            Require(Allocations(instance) == initial + 4,
                    "resident target count differs from four");
        }
    }
    Require(Allocations(instance) == before, "object-motion teardown leaked VMA allocations");
    std::printf("PASS kind=%s LRU/deferred_lifetime/bounds/teardown checked\n", kind.c_str());
}

// Reproduce Rasterizer::Draw's Attach -> upload Wait/Flush -> BeginRendering
// order. Test the lifetime bound before any eviction so the old code fails
// without freeing an in-flight image. Then hold the delayed clear/readback on
// the GPU while evicting its size.
void SplitWriteCase(Instance& instance, Scheduler& scheduler) {
    const u32 before = Allocations(instance);
    VkBuffer staging{};
    VmaAllocation allocation{};
    VmaAllocationInfo info{};
    const VkBufferCreateInfo buffer{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                    .size = 128 * 72 * 16,
                                    .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const VmaAllocationCreateInfo host{.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                                VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                       .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    Require(vmaCreateBuffer(instance.GetAllocator(), &buffer, &host, &staging, &allocation,
                            &info) == VK_SUCCESS,
            "split-write staging allocation failed");
    try {
        auto motion_owner = ObjectMotionCacheTest::Make(instance, scheduler);
        auto& motion = *motion_owner;
        Require(motion.Enabled(), "split-write object motion unavailable");
        motion.OnFrameStart();
        const u32 initial = Allocations(instance);
        RenderState state{};
        state.width = 128;
        state.height = 72;
        state.num_layers = 1;
        motion.Attach(state, 128, 72);
        const auto attach_tick = scheduler.CurrentTick();
        const auto image = ObjectMotionCacheTest::RawImage(motion);
        scheduler.Wait(attach_tick); // Submits the attachment barrier, as an upload wait can.
        const auto write_tick = scheduler.CurrentTick();
        Require(write_tick > attach_tick && scheduler.IsFree(attach_tick),
                "split-write needs a completed attachment tick and a newer write tick");
        const std::array<u32, 4> expected{std::bit_cast<u32>(0.25f), std::bit_cast<u32>(-0.5f),
                                          std::bit_cast<u32>(0.75f), std::bit_cast<u32>(1.0f)};
        state.color_attachments[Shader::MotionVectors::Output].clear_value = expected;
        scheduler.BeginRendering(state);
        scheduler.EndRendering();
        // Read the recorded attachment directly: adding PrepareRead's barrier would
        // change the layout instead of exercising the delayed attachment use alone.
        auto cmd = scheduler.CommandBuffer();
        const vk::ImageMemoryBarrier2 to_copy{
            .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
            .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .newLayout = vk::ImageLayout::eTransferSrcOptimal,
            .image = image,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_copy});
        const vk::BufferImageCopy region{
            .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .imageExtent = {128, 72, 1}};
        cmd.copyImageToBuffer(image, vk::ImageLayout::eTransferSrcOptimal, staging, region);
        auto restore = to_copy;
        restore.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
        restore.srcAccessMask = vk::AccessFlagBits2::eTransferRead;
        restore.dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
        restore.dstAccessMask =
            vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite;
        std::swap(restore.oldLayout, restore.newLayout);
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &restore});
        const vk::MemoryBarrier2 to_host{.srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                                         .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                                         .dstStageMask = vk::PipelineStageFlagBits2::eHost,
                                         .dstAccessMask = vk::AccessFlagBits2::eHostRead};
        cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &to_host});
        AttachSize(motion, scheduler, 96, 54);
        Require(ObjectMotionCacheTest::CachedTick(motion, 128, 72) == write_tick,
                "stashed target lost the post-attachment write tick");
        Gate gate(instance.GetDevice());
        gate.Submit(scheduler);
        for (const auto [w, h] : std::array<std::pair<u32, u32>, 3>{{{64, 36}, {48, 27}, {32, 18}}})
            AttachSize(motion, scheduler, w, h);
        Require(!scheduler.IsFree(write_tick), "split-write gate released too soon");
        Require(!ObjectMotionCacheTest::Has(motion, 128, 72), "split-write target was not evicted");
        Require(ObjectMotionCacheTest::Retired(motion) == 1 && Allocations(instance) == initial + 5,
                "split-write image freed before its delayed GPU use");
        ObjectMotionCacheTest::Limits(motion);
        gate.Release();
        scheduler.Finish();
        ObjectMotionCacheTest::Collect(motion);
        Require(ObjectMotionCacheTest::Retired(motion) == 0,
                "split-write retirement did not drain");
        Require(vmaInvalidateAllocation(instance.GetAllocator(), allocation, 0, VK_WHOLE_SIZE) ==
                    VK_SUCCESS,
                "split-write invalidate failed");
        const auto* words = static_cast<const u32*>(info.pMappedData);
        for (u32 i = 0; i < 128 * 72 * 4; ++i)
            Require(words[i] == expected[i % 4], "split-write clear/readback pixels differ");
    } catch (...) {
        vmaDestroyBuffer(instance.GetAllocator(), staging, allocation);
        throw;
    }
    vmaDestroyBuffer(instance.GetAllocator(), staging, allocation);
    Require(Allocations(instance) == before, "split-write teardown leaked allocations");
    std::printf("PASS split-write: attachment submission, delayed "
                "clear/readback, gated eviction\n");
}

void LateReadCase(bool view_read, Instance& instance, Scheduler& scheduler) {
    const auto device = instance.GetDevice();
    const u32 before = Allocations(instance);
    {
        auto motion_owner = ObjectMotionCacheTest::Make(instance, scheduler);
        auto& motion = *motion_owner;
        motion.OnFrameStart();
        for (const auto [w, h] : std::array<std::pair<u32, u32>, 5>{
                 {{128, 72}, {96, 54}, {64, 36}, {48, 27}, {128, 72}}})
            AttachSize(motion, scheduler, w, h);
        bool valid = false;
        motion.PrepareRead(scheduler.CommandBuffer(), 128, 72, valid);
        Require(valid, "initial motion target invalid");
        const auto old_tick = scheduler.CurrentTick();
        scheduler.Finish(); // Simulate a provider context/size change after PrepareRead.
        const auto read_tick = scheduler.CurrentTick();
        Require(old_tick != read_tick && scheduler.IsFree(old_tick),
                "test needs two distinct GPU ticks");
        const ObjectMotion& read_only_motion = motion;
        Require(!read_only_motion.Image(129, 72), "wrong-sized image accepted");
        if (view_read)
            Require(bool(read_only_motion.View()), "debug view missing");
        else
            Require(bool(read_only_motion.Image(128, 72)), "dump image missing");
        VkBuffer staging{};
        VmaAllocation allocation{};
        VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer.size = 128 * 72 * 16;
        buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo host{};
        host.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        if (vmaCreateBuffer(instance.GetAllocator(), &buffer, &host, &staging, &allocation,
                            nullptr) != VK_SUCCESS)
            throw std::runtime_error("late-read staging allocation failed");
        const auto image = ObjectMotionCacheTest::RawImage(motion);
        auto cmd = scheduler.CommandBuffer();
        const vk::MemoryBarrier2 barrier{.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                                         .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                                         .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                                         .dstAccessMask = vk::AccessFlagBits2::eTransferRead};
        cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
        const vk::BufferImageCopy region{
            .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .imageExtent = {128, 72, 1}};
        cmd.copyImageToBuffer(image, vk::ImageLayout::eGeneral, staging, region);
        AttachSize(motion, scheduler, 32, 18);
        // Check the real lifetime bound before evicting this in-flight target.
        Require(ObjectMotionCacheTest::CachedTick(motion, 128, 72) == read_tick,
                "stashed target lost the late-read GPU tick");
        {
            Gate gate(device);
            gate.Submit(scheduler);
            for (const auto [w, h] :
                 std::array<std::pair<u32, u32>, 3>{{{16, 9}, {8, 4}, {4, 2}}})
                AttachSize(motion, scheduler, w, h);
            Require(!scheduler.IsFree(read_tick), "late-read gate released too soon");
            Require(ObjectMotionCacheTest::Retired(motion) == 1,
                    "late-read target freed instead of retained");
            ObjectMotionCacheTest::Limits(motion);
            gate.Release();
            scheduler.Finish();
            ObjectMotionCacheTest::Collect(motion);
            Require(ObjectMotionCacheTest::Retired(motion) == 0,
                    "late-read target not collected after completion");
        }
        vmaDestroyBuffer(instance.GetAllocator(), staging, allocation);
    }
    Require(Allocations(instance) == before, "late-read test leaked allocations");
    std::printf("PASS late-%s: two-tick read, blocked GPU eviction and teardown\n",
                view_read ? "view" : "image");
}

int main(int argc, char** argv) {
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _CALL_REPORTFAULT);
#endif
    try {
        const std::string kind = argc > 1 ? argv[1] : "tiny";
        const u32 steps = kind == "game" ? 12 : 64;
        std::vector<std::pair<u32, u32>> sizes;
        if (kind == "game")
            sizes = {{2560, 1440}, {1280, 720}, {2560, 1440}, {2560, 1440}};
        else if (kind == "churn")
            sizes = {{128, 72}, {96, 54}, {64, 36}, {48, 27}, {32, 18}, {16, 9}, {1, 1}};
        else
            sizes = {{128, 72}, {64, 36}, {128, 72}, {128, 72}, {64, 36}, {64, 36}};
        const u64 stride = u64(sizes[0].first) * sizes[0].second * 16;
        BbSettings::Get().object_motion = true;
        const int upscaler = argc > 2 ? std::atoi(argv[2]) : BbSettings::UpscalerFsr3;
        BbSettings::Get().upscaler = upscaler;
        const bool validation = argc > 3 && std::string(argv[3]) == "validation";
        Instance instance(0, validation);
        VULKAN_HPP_DEFAULT_DISPATCHER.init(instance.GetInstance());
        VULKAN_HPP_DEFAULT_DISPATCHER.init(instance.GetDevice());
        const auto device = instance.GetDevice();
        const char* threaded_setting = std::getenv("BB_CACHE_TEST_THREADED");
        const bool threaded = threaded_setting && std::strcmp(threaded_setting, "1") == 0;
        Scheduler scheduler(instance, threaded);
        Require(scheduler.IsRecordingDeferred() == threaded,
                "requested recording mode is not active");
        std::printf("Cache fixture: %s recording\n", threaded ? "threaded" : "direct");
        if (kind == "gate-error") {
            Gate gate(device, true);
            gate.Release();
            throw std::runtime_error("gate error was not propagated");
        }
        if (kind == "split-write") {
            Require(CacheEnabled(), "split-write needs cache on");
            SplitWriteCase(instance, scheduler);
            return 0;
        }
        if (kind == "late-view" || kind == "late-image") {
            Require(CacheEnabled(), "late reads need cache on");
            LateReadCase(kind == "late-view", instance, scheduler);
            return 0;
        }
        if (kind == "lru" || kind == "deferred" || kind == "budget" || kind == "oversized" ||
            kind == "drain" || kind == "destruct") {
            Require(CacheEnabled(), "special cases need cache on");
            SpecialCase(kind, instance, scheduler);
            return 0;
        }
        VkBuffer staging{};
        VmaAllocation allocation{};
        VmaAllocationInfo allocation_info{};
        VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer.size = stride * steps;
        buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo host{};
        host.usage = VMA_MEMORY_USAGE_AUTO;
        host.flags =
            VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        if (vmaCreateBuffer(instance.GetAllocator(), &buffer, &host, &staging, &allocation,
                            &allocation_info) != VK_SUCCESS)
            throw std::runtime_error("readback allocation failed");
        std::vector<std::array<u32, 4>> expected;
        u64 fnv = 1469598103934665603ull;
        {
            auto motion_owner = ObjectMotionCacheTest::Make(instance, scheduler);
            auto& motion = *motion_owner;
            if (!motion.Enabled())
                throw std::runtime_error("object motion unavailable");
            const bool cache = CacheEnabled();
            VmaTotalStatistics initial_stats{};
            vmaCalculateStatistics(instance.GetAllocator(), &initial_stats);
            std::set<std::pair<u32, u32>> observed_sizes;
            std::array<u32, 4> contents{};
            for (u32 i = 0; i < steps; ++i) {
                if (i % 4 == 0)
                    motion.OnFrameStart();
                const auto [w, h] = sizes[i % sizes.size()];
                RenderState state{};
                state.width = w;
                state.height = h;
                state.num_layers = 1;
                motion.Attach(state, w, h);
                observed_sizes.emplace(w, h);
                VmaTotalStatistics stats{};
                vmaCalculateStatistics(instance.GetAllocator(), &stats);
                const u32 targets = stats.total.statistics.allocationCount -
                                    initial_stats.total.statistics.allocationCount;
                const u32 expected_targets = cache ? std::min<size_t>(observed_sizes.size(), 4) : 1;
                ObjectMotionCacheTest::Limits(motion);
                if (targets != expected_targets + ObjectMotionCacheTest::Retired(motion))
                    throw std::runtime_error(
                        "target allocation count differs from selected cache mode");
                auto& attachment = state.color_attachments[Shader::MotionVectors::Output];
                if (attachment.is_clear) {
                    contents = {std::bit_cast<u32>(float(i + 1) / 128),
                                std::bit_cast<u32>(-float(i + 1) / 256), std::bit_cast<u32>(0.5f),
                                std::bit_cast<u32>(1.0f)};
                }
                attachment.clear_value = contents;
                scheduler.BeginRendering(state);
                scheduler.EndRendering();
                bool valid = false;
                auto cmd = scheduler.CommandBuffer();
                const u32 requested = i % 5 == 0 ? w + 1 : w;
                motion.PrepareRead(cmd, requested, h, valid);
                if (valid != (requested == w))
                    throw std::runtime_error("validity differs from legacy resize semantics");
                if (bool(motion.Image(requested, h)) != valid)
                    throw std::runtime_error("Image validity mismatch");
                const vk::BufferImageCopy copy{
                    .bufferOffset = i * stride,
                    .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                    .imageExtent = {w, h, 1}};
                // PrepareRead exposes shader reads; the fixture also performs a transfer read.
                const vk::MemoryBarrier2 readback_barrier{
                    .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                    .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                    .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                    .dstAccessMask = vk::AccessFlagBits2::eTransferRead};
                cmd.pipelineBarrier2(
                    {.memoryBarrierCount = 1, .pMemoryBarriers = &readback_barrier});
                cmd.copyImageToBuffer(motion.Image(w, h), vk::ImageLayout::eGeneral, staging, copy);
                expected.push_back(contents);
            }
            auto cmd = scheduler.CommandBuffer();
            const vk::MemoryBarrier2 host_barrier{
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eHost,
                .dstAccessMask = vk::AccessFlagBits2::eHostRead};
            cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &host_barrier});
            scheduler.Finish();
            ObjectMotionCacheTest::Collect(motion);
            Require(ObjectMotionCacheTest::Retired(motion) == 0, "retired images did not drain");
            const u32 expected_targets = cache ? std::min<size_t>(observed_sizes.size(), 4) : 1;
            Require(Allocations(instance) ==
                        initial_stats.total.statistics.allocationCount + expected_targets,
                    "final resident allocation count differs");
            vmaInvalidateAllocation(instance.GetAllocator(), allocation, 0, VK_WHOLE_SIZE);
            for (u32 i = 0; i < steps; ++i) {
                const auto [w, h] = sizes[i % sizes.size()];
                const auto* values = reinterpret_cast<const u32*>(
                    static_cast<const u8*>(allocation_info.pMappedData) + i * stride);
                for (u64 p = 0; p < u64(w) * h * 4; ++p) {
                    if (values[p] != expected[i][p % 4]) {
                        std::printf("MISMATCH step=%u word=%llu actual=%08x expected=%08x\n", i,
                                    (unsigned long long)p, values[p], expected[i][p % 4]);
                        throw std::runtime_error("motion pixels changed");
                    }
                    fnv = (fnv ^ values[p]) * 1099511628211ull;
                }
            }
        }
        vmaDestroyBuffer(instance.GetAllocator(), staging, allocation);
        std::printf("PASS kind=%s upscaler=%d steps=%u "
                    "all_pixels_identical_to_expected hash=%016llx\n",
                    kind.c_str(), upscaler, steps, (unsigned long long)fnv);
        return 0;
    } catch (const std::exception& e) {
        std::printf("FAIL: %s\n", e.what());
        return 1;
    }
}

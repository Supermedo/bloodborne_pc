// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <deque>
#include <mutex>
#include <optional>
#include <boost/container/small_vector.hpp>
#include <boost/container/static_vector.hpp>

#include "common/enum.h"
#include "common/incremental_id.h"
#include "common/lru_cache.h"
#include "common/small_vector.h"
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/host_compatibility.h"
#include "video_core/texture_cache/image_info.h"
#include "video_core/texture_cache/image_view.h"

namespace Vulkan {
class Instance;
class Runtime;
} // namespace Vulkan

VK_DEFINE_HANDLE(VmaAllocation)
VK_DEFINE_HANDLE(VmaAllocator)

namespace VideoCore {

enum ImageFlagBits : u32 {
    Empty = 0,
    MaybeCpuDirty = 1 << 0, ///< The page this image is in was touched before the image address
    CpuDirty = 1 << 1,      ///< Contents have been modified from the CPU
    GpuDirty =
        1 << 2, ///< Image contents have been modified from the GPU (valid data in buffer cache)
    Dirty = MaybeCpuDirty | CpuDirty | GpuDirty,
    GpuModified = 1 << 3,   ///< Contents have been modified from the GPU
    MaybeGpuDirty = 1 << 4, ///< Image contents may have been modified in the buffer cache
    Registered = 1 << 6,    ///< True when the image is registered
};
DECLARE_ENUM_FLAG_OPERATORS(ImageFlagBits)

struct UniqueImage {
    explicit UniqueImage() = default;
    explicit UniqueImage(vk::Device device, VmaAllocator allocator)
        : device{device}, allocator{allocator} {}
    ~UniqueImage();

    UniqueImage(const UniqueImage&) = delete;
    UniqueImage& operator=(const UniqueImage&) = delete;

    UniqueImage(UniqueImage&& other)
        : allocator{std::exchange(other.allocator, VK_NULL_HANDLE)},
          allocation{std::exchange(other.allocation, VK_NULL_HANDLE)},
          image{std::exchange(other.image, VK_NULL_HANDLE)}, image_ci{std::move(other.image_ci)},
          size_bytes{std::exchange(other.size_bytes, 0)} {}
    UniqueImage& operator=(UniqueImage&& other) {
        if (this == &other) return *this;
        Destroy();
        device = other.device;
        image = std::exchange(other.image, VK_NULL_HANDLE);
        allocator = std::exchange(other.allocator, VK_NULL_HANDLE);
        allocation = std::exchange(other.allocation, VK_NULL_HANDLE);
        image_ci = std::move(other.image_ci);
        size_bytes = std::exchange(other.size_bytes, 0);
        return *this;
    }

    void Create(const vk::ImageCreateInfo& image_ci);
    /// bbport: as Create, but returns false where the GPU has no memory left for the image
    /// (the texture cache frees unused images and tries again).
    bool TryCreate(const vk::ImageCreateInfo& image_ci);

    void Destroy();

    operator vk::Image() const {
        return image;
    }

    operator bool() const {
        return image;
    }

public:
    vk::Device device{};
    VmaAllocator allocator{};
    VmaAllocation allocation{};
    vk::Image image{};
    vk::ImageCreateInfo image_ci{};
    vk::DeviceSize size_bytes{};
};

struct Image : public Common::LRUNode<> {
    explicit Image(const Vulkan::Instance& instance, Vulkan::Runtime& runtime,
                   Common::SlotVector<ImageView>& slot_image_views, const ImageInfo& info);
    ~Image();

    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;

    Image(Image&&) = delete;
    Image& operator=(Image&&) = delete;

    bool Overlaps(VAddr addr, size_t size) const noexcept {
        return info.guest_address < (addr + size) && addr < (info.guest_address + info.guest_size);
    }

    vk::Image GetImage() const {
        return backing->image.image;
    }

    bool IsUntracked() {
        return track_addr == 0 || track_addr_end == 0;
    }

    bool HasGpuValidData() const {
        return True(flags & ImageFlagBits::GpuModified) && False(flags & ImageFlagBits::Dirty);
    }

    bool CanBeDownloaded() {
        return HasGpuValidData() && info.num_samples == 1 && !info.props.has_stencil &&
               IsVulkanFormatCompatible(info.pixel_format, backing->image.image_ci.format);
    }

    void AssociateDepth(ImageId depth_image_id, u64 depth_image_uid) {
        depth_id = depth_image_id;
        depth_uid = depth_image_uid;
    }

    void DisassociateDepth() {
        depth_id = {};
        depth_uid = {};
    }

    vk::DeviceSize SizeBytes() const {
        return std::accumulate(
            backing_images.begin(), backing_images.end(), vk::DeviceSize(0),
            [](vk::DeviceSize sum, const BackingImage& bi) { return sum + bi.image.size_bytes; });
    }

    ImageView& FindView(const ImageViewInfo& view_info, bool ensure_guest_samples = true);

    /// bbport: creates the Vulkan image again after allocation_failed; false while no memory.
    bool RetryAllocation();

    using Barriers = SmallVector<vk::ImageMemoryBarrier2, 32>;
    void GetBarriers(Barriers& out_barriers, vk::ImageLayout dst_layout, vk::AccessFlags2 dst_mask,
                     vk::PipelineStageFlags2 dst_stage,
                     std::optional<SubresourceRange> subres_range = {});

public:
    struct BackingImage;
    // bbport: fields every binding of the image reads, together ahead of the large ImageInfo
    // (they were spread over several cache lines: one miss each per texture per draw).
    ImageFlagBits flags = ImageFlagBits::Dirty;
    struct {
        u32 is_bound : 1;
        u32 is_target : 1;
        u32 needs_rebind : 1;
        u32 force_general : 1;
    } binding{};
    struct {
        u32 texture : 1;
        u32 storage : 1;
        u32 render_target : 1;
        u32 depth_target : 1;
        u32 vo_surface : 1;
    } usage{};
    bool scene_proxy = false; ///< bbport: SceneTargets holds a reduced-size proxy of it
    /// bbport: no GPU memory for the image when it was created (RetryAllocation).
    bool allocation_failed = false;
    VAddr track_addr = 0;
    VAddr track_addr_end = 0;
    VAddr guest_begin = 0; ///< info.guest_address
    VAddr guest_end = 0;   ///< info.guest_address + info.guest_size
    BackingImage* backing{};
    /// bbport: gc tick of the last LRU touch; skips the LRU list (a cache miss) when current.
    mutable u64 lru_touched_tick = ~0ULL;
    u64 tick_accessed_last{};
    ImageId depth_id{};
    u64 depth_uid{};
    u64 image_uid{};

    ImageInfo info;
    Vulkan::Runtime* runtime;
    Common::SlotVector<ImageView>* slot_image_views;
    std::mutex mutex; ///< upstream garbage collector (downloads before eviction)
    vk::ImageAspectFlags aspect_mask = vk::ImageAspectFlagBits::eColor;
    vk::SampleCountFlags supported_samples = vk::SampleCountFlagBits::e1;

    vk::ImageUsageFlags usage_flags;
    vk::FormatFeatureFlags2 format_features;
    struct State {
        vk::PipelineStageFlags2 pl_stage = vk::PipelineStageFlagBits2::eAllCommands;
        vk::AccessFlags2 access_mask = vk::AccessFlagBits2::eNone;
        vk::ImageLayout layout = vk::ImageLayout::eUndefined;
    };
    struct BackingImage {
        UniqueImage image;
        State state;
        std::vector<State> subresource_states;
        boost::container::small_vector<ImageViewInfo, 4> image_view_infos;
        boost::container::small_vector<ImageViewId, 4> image_view_ids;
        u32 last_view = 0; ///< bbport: index of the view FindView returned last
        u32 num_samples;
    };
    std::deque<BackingImage> backing_images;
    boost::container::static_vector<u64, 16> mip_hashes{};
    /// bbport: mip_hashes match guest memory (BB_KEEP_MIP_HASHES). Cleared on a CPU write.
    bool mip_hashes_current = false;
    u64* memory_usage_counter{}; ///< upstream garbage collector: the cache's usage total
    u64 lru_id{};
    u64 cpu_hash{};
    u64 gpu_hash{};

private:
    static Common::IncrementalIdProvider<u64> global_image_uid;
};

} // namespace VideoCore

// SPDX-License-Identifier: GPL-2.0-or-later
// Execute the actual emitted vertex/fragment pair: duplicate indices,
// instancing, physical history writes/reads, invalid parameters and eight
// colour attachments.
#include "bbport_settings.h"
#include "shader_recompiler/resource.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_motion_parameters.h"
#include "video_core/renderer_vulkan/vk_object_motion.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>
#include <vk_mem_alloc.h>

#ifdef _WIN32
extern "C" void runtime_restart() { std::abort(); }
#endif

void WriteMotionShaders(const std::filesystem::path&, uint64_t, uint64_t);
namespace Vulkan {
struct ObjectMotionParameterTest {
    static std::unique_ptr<ObjectMotion> Ssbo(const Instance& instance, Scheduler& scheduler) {
        return std::unique_ptr<ObjectMotion>(new ObjectMotion(instance, scheduler, true));
    }
    static void* Mapped(ObjectMotion& motion) { return motion.params_mapped; }
    static vk::Buffer HistoryBuffer(ObjectMotion& motion) { return motion.positions_buffer; }
};
} // namespace Vulkan
namespace {
void Need(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
struct Buffer {
    VmaAllocator allocator;
    VkBuffer buffer{};
    VmaAllocation allocation{};
    void* mapped{};
    Buffer(VmaAllocator a, size_t bytes, VkBufferUsageFlags usage) : allocator(a) {
        VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        ci.size = bytes;
        ci.usage = usage;
        VmaAllocationCreateInfo ai{};
        ai.usage = VMA_MEMORY_USAGE_AUTO;
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        ai.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        VmaAllocationInfo info{};
        Need(vmaCreateBuffer(a, &ci, &ai, &buffer, &allocation, &info) == VK_SUCCESS,
             "test buffer allocation");
        mapped = info.pMappedData;
    }
    ~Buffer() { vmaDestroyBuffer(allocator, buffer, allocation); }
};
struct Image {
    VmaAllocator allocator;
    VkImage image{};
    VmaAllocation allocation{};
    vk::UniqueImageView view;
    Image(VmaAllocator a, vk::Device device) : allocator(a) {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        ci.extent = {64, 64, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo ai{};
        ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        Need(vmaCreateImage(a, &ci, &ai, &image, &allocation, nullptr) == VK_SUCCESS,
             "test attachment allocation");
        auto [r, v] = device.createImageViewUnique(
            {.image = vk::Image(image),
             .viewType = vk::ImageViewType::e2D,
             .format = vk::Format::eR32G32B32A32Sfloat,
             .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}});
        Need(r == vk::Result::eSuccess, "test attachment view");
        view = std::move(v);
    }
    ~Image() {
        view.reset();
        vmaDestroyImage(allocator, image, allocation);
    }
};
std::vector<uint32_t> Read(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    Need(bool(f), "shader missing");
    auto bytes = f.tellg();
    f.seekg(0);
    std::vector<uint32_t> words(size_t(bytes) / 4);
    f.read(reinterpret_cast<char*>(words.data()), bytes);
    Need(bool(f), "shader read failed");
    return words;
}
} // namespace
#include "test_motion_history_ssbo.inc"
int MotionGpuTest(const std::filesystem::path& dir) { return MotionHistorySsboGpuTest(dir); }

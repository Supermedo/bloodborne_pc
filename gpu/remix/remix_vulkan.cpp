// SPDX-License-Identifier: GPL-2.0-or-later
#include "remix_vulkan.h"
#include <cstdio>
#include <cstring>
#include <limits>

namespace BbRemix {
VulkanOutput::VulkanOutput(VkPhysicalDevice physical, VkDevice device)
    : physical(physical), device(device) {}
VulkanOutput::~VulkanOutput() { Reset(); }
bool VulkanOutput::Fail(const char *operation, VkResult result) {
  error = std::string(operation) + " failed (" + std::to_string(result) + ")";
  std::printf("RTX Remix Vulkan output: %s\n", error.c_str());
  Reset();
  return false;
}
void VulkanOutput::Reset() {
  if (image)
    vkDestroyImage(device, image, nullptr);
  if (memory)
    vkFreeMemory(device, memory, nullptr);
  image = VK_NULL_HANDLE;
  memory = VK_NULL_HANDLE;
  generation = 0;
  source = nullptr;
}
bool VulkanOutput::Import(const Renderer::SharedOutput &output) {
  if (!output.memory || !output.width || !output.height)
    return Fail("invalid shared output", VK_ERROR_INITIALIZATION_FAILED);
  if (image && generation == output.generation && source == output.memory)
    return true;
  Reset();
  VkPhysicalDeviceIDProperties id{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
  VkPhysicalDeviceProperties2 physical_properties{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
  vkGetPhysicalDeviceProperties2(physical, &physical_properties);
  if (!id.deviceLUIDValid ||
      std::memcmp(id.deviceLUID, output.adapter_luid.data(), VK_LUID_SIZE))
    return Fail("consumer and Remix adapters differ",
                VK_ERROR_INVALID_EXTERNAL_HANDLE);
  constexpr auto type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT;
  const VkPhysicalDeviceExternalImageFormatInfo external_info{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO, nullptr,
      type};
  const VkPhysicalDeviceImageFormatInfo2 format_info{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
      &external_info,
      VK_FORMAT_R16G16B16A16_SFLOAT,
      VK_IMAGE_TYPE_2D,
      VK_IMAGE_TILING_OPTIMAL,
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
      0};
  VkExternalImageFormatProperties external_properties{
      VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
  VkImageFormatProperties2 properties{
      VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &external_properties};
  const auto supported = vkGetPhysicalDeviceImageFormatProperties2(
      physical, &format_info, &properties);
  if (supported != VK_SUCCESS)
    return Fail("shared image format", supported);
  if (!(external_properties.externalMemoryProperties.externalMemoryFeatures &
        VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT))
    return Fail("KMT import unsupported", VK_ERROR_FEATURE_NOT_PRESENT);
  const VkExternalMemoryImageCreateInfo external_image{
      VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, nullptr, type};
  const VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                 &external_image,
                                 0,
                                 VK_IMAGE_TYPE_2D,
                                 VK_FORMAT_R16G16B16A16_SFLOAT,
                                 {output.width, output.height, 1},
                                 1,
                                 1,
                                 VK_SAMPLE_COUNT_1_BIT,
                                 VK_IMAGE_TILING_OPTIMAL,
                                 format_info.usage,
                                 VK_SHARING_MODE_EXCLUSIVE,
                                 0,
                                 nullptr,
                                 VK_IMAGE_LAYOUT_UNDEFINED};
  auto result = vkCreateImage(device, &create, nullptr, &image);
  if (result != VK_SUCCESS)
    return Fail("vkCreateImage", result);
  VkMemoryRequirements requirements{};
  vkGetImageMemoryRequirements(device, image, &requirements);
  VkPhysicalDeviceMemoryProperties memory_properties{};
  vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
  // OPAQUE_WIN32_KMT: vkGetMemoryWin32HandlePropertiesKHR is not permitted by
  // Vulkan for opaque handles. Memory type must match the exporting adapter.
  // DXVK's shared RT is device-local; reject import failures instead of falling
  // back to raw handles or guessing another adapter's allocation.
  uint32_t index = std::numeric_limits<uint32_t>::max();
  for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i)
    if ((requirements.memoryTypeBits & (1u << i)) &&
        (memory_properties.memoryTypes[i].propertyFlags &
         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
      index = i;
      break;
    }
  if (index == std::numeric_limits<uint32_t>::max())
    return Fail("device-local import memory type",
                VK_ERROR_FEATURE_NOT_PRESENT);
  const VkMemoryDedicatedAllocateInfo dedicated{
      VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, nullptr, image,
      VK_NULL_HANDLE};
  const VkImportMemoryWin32HandleInfoKHR imported{
      VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR, &dedicated, type,
      output.memory, nullptr};
  const VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                        &imported, requirements.size, index};
  result = vkAllocateMemory(device, &allocation, nullptr, &memory);
  if (result != VK_SUCCESS)
    return Fail("vkAllocateMemory shared import", result);
  result = vkBindImageMemory(device, image, memory, 0);
  if (result != VK_SUCCESS)
    return Fail("vkBindImageMemory", result);
  generation = output.generation;
  source = output.memory;
  return true;
}
void VulkanOutput::Acquire(VkCommandBuffer command,
                           uint32_t queue_family) const {
  // DXVK CopyRenderingOutput leaves this explicitly shared image in GENERAL.
  // Renderer::Render has waited for the D3D9 completion event before this call.
  const VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                     nullptr,
                                     0,
                                     VK_ACCESS_TRANSFER_READ_BIT |
                                         VK_ACCESS_SHADER_READ_BIT,
                                     VK_IMAGE_LAYOUT_GENERAL,
                                     VK_IMAGE_LAYOUT_GENERAL,
                                     VK_QUEUE_FAMILY_EXTERNAL,
                                     queue_family,
                                     image,
                                     {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
  vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT |
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       0, 0, nullptr, 0, nullptr, 1, &barrier);
}
void VulkanOutput::Release(VkCommandBuffer command,
                           uint32_t queue_family) const {
  const VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                     nullptr,
                                     VK_ACCESS_TRANSFER_READ_BIT |
                                         VK_ACCESS_SHADER_READ_BIT,
                                     0,
                                     VK_IMAGE_LAYOUT_GENERAL,
                                     VK_IMAGE_LAYOUT_GENERAL,
                                     queue_family,
                                     VK_QUEUE_FAMILY_EXTERNAL,
                                     image,
                                     {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
  vkCmdPipelineBarrier(command,
                       VK_PIPELINE_STAGE_TRANSFER_BIT |
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &barrier);
}
} // namespace BbRemix

// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "remix_renderer.h"
#include <vulkan/vulkan.h>

namespace BbRemix {
// Imported image belongs to the consumer Vulkan device. The KMT handle belongs
// to Renderer. Destroy this object before Renderer::Resize/Shutdown.
class VulkanOutput {
public:
  VulkanOutput(VkPhysicalDevice physical, VkDevice device);
  ~VulkanOutput();
  VulkanOutput(const VulkanOutput &) = delete;
  VulkanOutput &operator=(const VulkanOutput &) = delete;
  bool Import(const Renderer::SharedOutput &output);
  void Reset(); // all consumer queue reads must have completed
  VkImage Image() const { return image; }
  // Queue ownership is transferred from Remix, then back to external in the
  // same consumer submission. No raw SDK VkImage/VkSemaphore crosses device
  // boundaries.
  void Acquire(VkCommandBuffer command, uint32_t queue_family) const;
  void Release(VkCommandBuffer command, uint32_t queue_family) const;
  const std::string &Error() const { return error; }

private:
  bool Fail(const char *operation, VkResult result);
  VkPhysicalDevice physical{};
  VkDevice device{};
  VkImage image{};
  VkDeviceMemory memory{};
  uint64_t generation{};
  HANDLE source{};
  std::string error;
};
} // namespace BbRemix

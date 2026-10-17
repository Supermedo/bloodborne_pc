// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace Vulkan::Fsr4VkPost {

// Keep the automatic optimization within the tested Navi 48 / 4K Quality SDR case.
bool Eligible(uint32_t vendor, uint32_t device, uint32_t output_width, uint32_t output_height,
              uint32_t render_width, uint32_t render_height, int preset, bool hdr);

// Empty means unrecognized input: the caller must pass the original module to Vulkan.
std::vector<uint32_t> Rewrite(std::span<const uint32_t> original);

} // namespace Vulkan::Fsr4VkPost

// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/fsr4vk_post.h"

#include <array>
#include <cstddef>
#include <xxhash.h>

namespace Vulkan::Fsr4VkPost {
namespace {

struct Edit {
    size_t offset, removed;
    std::span<const uint32_t> replacement;
};

// Only inserted instructions and four replaced output stores, not the original shader/model.
// Reproduce with tools/fsr411_post_lds.py and the pinned upstream shader documented there.
#include "video_core/renderer_vulkan/fsr4vk_post_edits.inc"

} // namespace

bool Eligible(uint32_t vendor, uint32_t device, uint32_t output_width, uint32_t output_height,
              uint32_t render_width, uint32_t render_height, int preset, bool hdr) {
    return vendor == 0x1002 && device == 0x7550 && output_width == 3840 && output_height == 2160 &&
           render_width == 2560 && render_height == 1440 && preset == 1 && !hdr;
}

std::vector<uint32_t> Rewrite(std::span<const uint32_t> original) {
    if (original.size_bytes() != 145456) {
        return {};
    }
    // A content fingerprint, not a security boundary. Never rewrite an unknown revision.
    const auto hash = XXH3_128bits(original.data(), original.size_bytes());
    if (hash.low64 != 0xea7406876ce8a57aULL || hash.high64 != 0xbfee2f8f378b20c7ULL) {
        return {};
    }
    std::vector<uint32_t> result;
    result.reserve(original.size() + 1024);
    size_t cursor = 0;
    for (const auto& edit : Edits) {
        result.insert(result.end(), original.begin() + cursor, original.begin() + edit.offset);
        result.insert(result.end(), edit.replacement.begin(), edit.replacement.end());
        cursor = edit.offset + edit.removed;
    }
    result.insert(result.end(), original.begin() + cursor, original.end());
    return result;
}

} // namespace Vulkan::Fsr4VkPost

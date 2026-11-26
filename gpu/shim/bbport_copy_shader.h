// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// bbport: the copy shader shortcut (vk_shader_hle.cpp) ended the game with an assertion when the
// compute registers held buffers that are not this shader's (after 1.5 million
// frames, while an area loaded). The control buffer it found had a stride of 16 and one
// RGBA32F record, the source was R16G16F and the destination R32 with 0x39f3fddc records. In every
// other dispatch the shader had one shape: an RGB32 Uint control of stride 12, an
// R32 Uint source and destination of stride 4 (a new shape compiles a new permutation, and there
// was only the one). Nothing here depends on Vulkan so that the decisions can be tested on their own.

#include <algorithm>
#include <bit>
#include <cstdint>

namespace BbCopyShader {

/// One record of the control buffer: copy end + 1 elements from src_idx to dst_idx.
struct Control {
    std::uint32_t dst_idx;
    std::uint32_t src_idx;
    std::uint32_t end;
};
static_assert(sizeof(Control) == 12);

/// What a buffer's descriptor says about its elements (amdgpu/resource.h Buffer).
struct Elements {
    std::uint32_t stride;
    std::uint32_t data_format;
    std::uint32_t number_format;
    constexpr bool operator==(const Elements&) const = default;
};

/// amdgpu/pixel_format.h: DataFormat::Format32_32_32 and NumberFormat::Uint.
constexpr std::uint32_t ControlDataFormat = 13;
constexpr std::uint32_t ControlNumberFormat = 4;

/// The shader's threads per group, and so the most elements one record copies: LocalSize 64, 1, 1,
/// and each thread copies its own element only when end >= its index.
constexpr std::uint64_t ElementsPerRecord = 64;

/// Whether the three buffers can be the copy shader's: the control buffer as above, and a source and
/// destination of one element type, whichever it is. When they are not, the dispatch is dropped.
/// Running the game's own shader on them instead would bind what they describe: 3.9 GB of memory
/// marked as written by the GPU in the case above, and every texture in it invalidated.
constexpr bool ShapeFits(const Elements& control, const Elements& source,
                         const Elements& destination) {
    return control.stride == sizeof(Control) && control.data_format == ControlDataFormat &&
           control.number_format == ControlNumberFormat && source.stride != 0 &&
           source == destination;
}

struct Copy {
    std::uint64_t src_offset;
    std::uint64_t dst_offset;
    std::uint64_t size; // bytes; 0 when nothing of the record is inside both buffers
    bool cut;           // less than the record asked for
};

/// The bytes one control record copies, with the GPU's limits: the elements per record above, and the
/// record counts it checks every element against (reads past the end give zero, writes past it are
/// dropped). What the game relies on is inside them, so they change nothing for it. The shortcut had
/// no limits of its own: a record read from stale memory would have it copy gigabytes. Offsets are 64
/// bits, as end + 1 and the products wrapped around in 32.
constexpr Copy Range(const Control& control, std::uint32_t stride, std::uint32_t src_records,
                     std::uint32_t dst_records) {
    const std::uint64_t asked = std::uint64_t{control.end} + 1;
    std::uint64_t count = asked;
    // Runs about 58,000 times a frame (57 dispatches of about 1024 records): the game's own records
    // pass this one test and take no other branch.
    if (asked > ElementsPerRecord || control.src_idx + asked > src_records ||
        control.dst_idx + asked > dst_records) [[unlikely]] {
        const std::uint64_t src_left =
            control.src_idx < src_records ? std::uint64_t{src_records} - control.src_idx : 0;
        const std::uint64_t dst_left =
            control.dst_idx < dst_records ? std::uint64_t{dst_records} - control.dst_idx : 0;
        count = std::min({asked, ElementsPerRecord, src_left, dst_left});
    }
    return {std::uint64_t{control.src_idx} * stride, std::uint64_t{control.dst_idx} * stride,
            count * stride, count != asked};
}

/// Whether the report numbered `count` (from 1) of one kind goes to the log: the first eight, then at
/// powers of two, so that a run where this keeps happening writes a few dozen lines at most.
constexpr bool ShouldReport(std::uint64_t count) {
    return count <= 8 || std::has_single_bit(count);
}

} // namespace BbCopyShader

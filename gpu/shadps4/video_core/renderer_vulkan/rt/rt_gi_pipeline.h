#pragma once

#include <array>
#include <cstdint>

#include "rt_gi.h"

namespace Vulkan::Rt {

struct GiPipelineMode {
    bool pipeline;
    bool ser;
};

constexpr uint32_t GiAmdVendor = 0x1002u;

constexpr bool GiPipelineWanted(const char* pipeline_env, uint32_t vendor) {
    return pipeline_env && pipeline_env[0] ? pipeline_env[0] == '1' : vendor != GiAmdVendor;
}

constexpr uint32_t GiPipelineOmmBit = 0x01000000u;

constexpr uint32_t GiPipelineFlags(bool omm) {
    return omm ? GiPipelineOmmBit : 0u;
}

constexpr GiPipelineMode GiPipelineFromEnv(const char* pipeline_env, const char* ser_env,
                                           bool pipeline_exposed, bool ser_exposed,
                                           uint32_t vendor) {
    const bool pipeline = pipeline_exposed && GiPipelineWanted(pipeline_env, vendor);
    const bool ser_wanted = !ser_env || ser_env[0] != '0';
    return {pipeline, pipeline && ser_exposed && ser_wanted};
}

constexpr uint32_t GiAlignUp(uint32_t value, uint32_t alignment) {
    return alignment ? (value + alignment - 1) / alignment * alignment : value;
}

struct GiSbtLayout {
    uint32_t stride;
    uint32_t raygen;
    uint32_t miss;
    uint32_t hit;
    uint32_t size;
};

constexpr GiSbtLayout GiSbt(uint32_t handle_size, uint32_t handle_alignment,
                            uint32_t base_alignment) {
    const uint32_t stride = GiAlignUp(handle_size, handle_alignment);
    const uint32_t region = GiAlignUp(stride, base_alignment);
    return {stride, 0u, region, 2u * region, 3u * region};
}

constexpr std::array<uint32_t, 2> GiInitLaunch(uint32_t width, uint32_t height, uint32_t sparse) {
    const auto groups = GiInitGroups(width, height, sparse);
    return {groups[0] * 8u, groups[1] * 8u};
}

}

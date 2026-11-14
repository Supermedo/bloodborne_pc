#include "video_core/renderer_vulkan/rt/vk_ray_tracing.h"
#include "bbport_stdout.h"
#include "video_core/renderer_vulkan/rt/rt_hitch.h"
#include "video_core/renderer_vulkan/vk_object_motion.h"
#include "video_core/renderer_vulkan/rt/rt_address_map.h"

#include <algorithm>
#include <charconv>
#include <bit>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <unordered_set>

#include <vk_mem_alloc.h>
#include <xxhash.h>
#include <fstream>
#include "shader_recompiler/runtime_info.h"
#include "video_core/host_shaders/rt_debug_comp.h"
#include "video_core/host_shaders/rt_gather_comp.h"
#include "video_core/host_shaders/rt_omm_bake_comp.h"
#include "video_core/host_shaders/rt_gi_apply_comp.h"
#include "video_core/host_shaders/rt_gi_comp.h"
#include "video_core/host_shaders/rt_light_vis_comp.h"
#include "video_core/host_shaders/rt_sky_layer_comp.h"
#include "video_core/host_shaders/rt_reflect_comp.h"
#include "video_core/host_shaders/rt_sharc_comp.h"
#include "video_core/host_shaders/rt_restir_gi_init_comp.h"
#include "video_core/host_shaders/rt_restir_gi_init_rgen.h"
#include "video_core/host_shaders/rt_restir_gi_init_ser_rgen.h"
#include "video_core/host_shaders/rt_gi_ahit.h"
#include "video_core/host_shaders/rt_gi_chit.h"
#include "video_core/host_shaders/rt_gi_miss.h"
#include "video_core/renderer_vulkan/rt/rt_gi_pipeline.h"
#include "video_core/host_shaders/rt_restir_gi_resolve_comp.h"
#include "video_core/host_shaders/rt_restir_gi_spatial_comp.h"
#include "video_core/host_shaders/rt_restir_gi_temporal_comp.h"
#include "video_core/renderer_vulkan/rt/rt_toggle.h"
#include "video_core/host_shaders/rt_probe_comp.h"
#include "video_core/host_shaders/rt_shadow_comp.h"
#include "video_core/host_shaders/rt_fog_comp.h"
#include "video_core/host_shaders/rt_fog_apply_comp.h"
#include "video_core/host_shaders/rt_fog_opaque_comp.h"
#include "video_core/host_shaders/rt_sky_comp.h"
#include "video_core/host_shaders/rt_exposure_comp.h"
#include "video_core/host_shaders/rt_world_set_comp.h"
#include "video_core/host_shaders/rt_moon_dir_comp.h"
#include "video_core/host_shaders/rt_direct_comp.h"
#include "video_core/host_shaders/rt_ao_comp.h"
#include "video_core/host_shaders/rt_restir_spatial_comp.h"
#include "video_core/host_shaders/rt_restir_temporal_comp.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/renderer_vulkan/rt/rt_hot.h"
#include "video_core/renderer_vulkan/rt/rt_omm.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scene_resolution.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {

namespace {

RayTracing* crumb_owner = nullptr;

const char* HotShaderName(const u32* code) {
    static const std::pair<const u32*, const char*> names[] = {
        {RT_GATHER_COMP, "rt_gather_comp"},
        {RT_OMM_BAKE_COMP, "rt_omm_bake_comp"},
        {RT_DEBUG_COMP, "rt_debug_comp"},
        {RT_PROBE_COMP, "rt_probe_comp"},
        {RT_GI_COMP, "rt_gi_comp"},
        {RT_GI_APPLY_COMP, "rt_gi_apply_comp"},
        {RT_SHARC_COMP, "rt_sharc_comp"},
        {RT_REFLECT_COMP, "rt_reflect_comp"},
        {RT_RESTIR_GI_INIT_COMP, "rt_restir_gi_init_comp"},
        {RT_RESTIR_GI_RESOLVE_COMP, "rt_restir_gi_resolve_comp"},
        {RT_LIGHT_VIS_COMP, "rt_light_vis_comp"},
        {RT_SKY_LAYER_COMP, "rt_sky_layer_comp"},
        {RT_RESTIR_GI_TEMPORAL_COMP, "rt_restir_gi_temporal_comp"},
        {RT_RESTIR_GI_SPATIAL_COMP, "rt_restir_gi_spatial_comp"},
        {RT_SHADOW_COMP, "rt_shadow_comp"},
        {RT_FOG_COMP, "rt_fog_comp"},
        {RT_FOG_APPLY_COMP, "rt_fog_apply_comp"},
        {RT_FOG_OPAQUE_COMP, "rt_fog_opaque_comp"},
        {RT_SKY_COMP, "rt_sky_comp"},
        {RT_EXPOSURE_COMP, "rt_exposure_comp"},
        {RT_WORLD_SET_COMP, "rt_world_set_comp"},
        {RT_MOON_DIR_COMP, "rt_moon_dir_comp"},
        {RT_DIRECT_COMP, "rt_direct_comp"},
        {RT_AO_COMP, "rt_ao_comp"},
        {RT_RESTIR_TEMPORAL_COMP, "rt_restir_temporal_comp"},
        {RT_RESTIR_SPATIAL_COMP, "rt_restir_spatial_comp"},
    };
    for (const auto& [data, name] : names) {
        if (data == code) {
            return name;
        }
    }
    return nullptr;
}

struct GatherConstants {
    u64 positions_address;
    u32 record_count;
    u32 triangle_count;
    float xs, ys;
    u64 uvs_address;
    u64 normals_address;
    u64 pad;
    std::array<std::array<float, 4>, 3> view_rows;
};
static_assert(sizeof(GatherConstants) == 96);

struct OmmConstants {
    u32 uv_base;
    u32 triangle_count;
    u32 data_word_offset;
    u32 record_word_offset;
    u32 index_word_offset;
    u32 texture_slot; // the slot the shadow rays' alpha test reads (SurfaceAlphaTexture)
    u32 level;
    float alpha_ref;
    u32 alpha_channel; // and its channel (SurfaceAlphaChannel)
};
static_assert(sizeof(OmmConstants) == 36);

constexpr u64 OmmAlign = 256;

vk::MicromapUsageEXT OmmUsage(u32 triangles, u32 level) {
    return vk::MicromapUsageEXT{
        .count = triangles,
        .subdivisionLevel = level,
        .format = u32(vk::OpacityMicromapFormatEXT::e4State),
    };
}

struct OmmInputLayout {
    u64 data{}, records{}, indices{}, end{};
};

OmmInputLayout OmmLayout(u64 cursor, u32 triangles, u32 level) {
    OmmInputLayout out{};
    out.data = Rt::AlignUp(cursor, OmmAlign);
    out.records = Rt::AlignUp(out.data + u64(triangles) * Rt::OmmDataBytes(level), OmmAlign);
    out.indices = Rt::AlignUp(out.records + u64(triangles) * Rt::OmmTriangleRecordBytes, OmmAlign);
    out.end = Rt::AlignUp(out.indices + u64(triangles) * Rt::OmmIndexBytes, OmmAlign);
    return out;
}

struct ProbeConstants {
    u64 positions_address;
    u32 probe_count;
    u32 pad;
    float xs, ys;
    float shelf_metres;
    u32 pad1;
};
static_assert(sizeof(ProbeConstants) == 32);

struct DebugConstants {
    u64 tlas_address;
    float xs, ys;
    float width, height;
    u32 mode;
    u32 frame;
    float range;
    float ao_radius;
};
static_assert(sizeof(DebugConstants) == 40);

struct ApplyConstants {
    float width, height;
    float strength;
    u32 mode;
    float max_history;
    u32 albedo_valid;
    u32 records;
    u32 denoise;
    float debug_threshold;
    u32 glass_keep;
    u32 halos;
};
static_assert(sizeof(ApplyConstants) == 44);

struct SharcPush {
    u64 grid;
    u32 slots;
    u32 frame;
};
static_assert(sizeof(SharcPush) == 16);

// rt_world_set.comp: the moon set vote, written as set + 1 into one word of a GiParams slot.
struct WorldSetPush {
    float width, height;
    u32 set_count;
    u32 word;
};
static_assert(sizeof(WorldSetPush) == 16);
// GiParams.sharc.w, read by rt_gi.comp WorldSet().
constexpr u32 WorldSetWord = u32((offsetof(Rt::GiParams, sharc) + 12) / 4);

// rt_moon_dir.comp: light 1 of each set in the set copy points at the sky's moon.
struct MoonDirPush {
    u32 set_count;
    u32 moon_slot;
    u32 pad0, pad1;
};
static_assert(sizeof(MoonDirPush) == 16);

// rt_exposure.comp push constants.
struct ExposurePush {
    std::array<float, 4> size_cfg; // width, height, phase, apply
    std::array<float, 4> params;   // key, max stops, rate, reset
    std::array<float, 4> tone;     // white level (stops above the key)
};
static_assert(sizeof(ExposurePush) == 48);
constexpr u32 ExposureSlotBytes = 64;

// rt_gi.comp RT_LIGHT_VIS push constants (VisPush).
struct FillVisPush {
    u32 count;
    u32 rays;
    float offset;
    float emit_dist;
    float emit_luma;
    float smoke;
};
static_assert(sizeof(FillVisPush) == 24);

// rt_gi.comp RT_SKY_LAYER push constants (SkyLayerPush).
struct SkyLayerPush {
    u32 layer_base;
    u32 moon_slot;
    u32 phase;
    float moon_min;
    std::array<float, 4> moon_seed;
    u32 moon_reset;
    u32 pad[3];
};
static_assert(sizeof(SkyLayerPush) == 48);

// rt_gi.comp (RT_REFLECT) GlassPush.
struct GlassPush {
    float gloss;
    float tint;
    u32 flags;
    float skip;
    float water_rough;
    float water_f0;
    float base_cut;
    float sheet_depth;
    float murk;
    float ripple;
    float sheet_f0;
    float mist;
    float mist_height;
    float mist_albedo;
    float mist_steps;
    float mist_patch;
    float mist_glow;
    float mist_glow_dark;
    float mist_sheet;
    float mist_fog;
    float raise;
    float body;
    float sheen;
    float sheen_far;
    float puddle_edge;
    float puddle_wet;
};
static_assert(sizeof(GlassPush) == 104);
constexpr u32 GlassOn = 1, GlassDebugMask = 2, GlassDebugCandidates = 4, GlassIrr = 8,
              GlassAlbedo = 16, GlassKeep = 32, WaterOn = 64, WaterDebugMask = 128, WaterAtFloor = 256,
              ReflectSkyDome = 512, WaterSheets = 1024, WaterPuddles = 2048;
// The transmitted ray starts this far behind the pane, past the pane's own back face.
constexpr float GlassSkip = 0.03f;

class LogWriter {
public:
    LogWriter() {
        const char* env = std::getenv("BB_RT_LOG_ASYNC");
        async = !env || std::strcmp(env, "0") != 0;
        if (async) {
            std::thread([this] { Run(); }).detach();
            std::atexit([] { Instance().Drain(std::chrono::milliseconds(500)); });
        }
    }

    bool Async() const { return async; }

    static LogWriter& Instance() {
        static LogWriter* writer = new LogWriter;
        return *writer;
    }

    void Write(const char* data, size_t size) {
        if (!async) {
            std::fwrite(data, 1, size, stdout);
            return;
        }
        {
            std::lock_guard lock{mutex};
            if (pending.size() + size > MaxPending) {
                ++dropped;
                return;
            }
            pending.append(data, size);
        }
        wake.notify_one();
    }

    void Drain(std::chrono::milliseconds timeout) {
        std::unique_lock lock{mutex};
        drained.wait_for(lock, timeout, [&] { return pending.empty() && !writing; });
    }

private:
    static constexpr size_t MaxPending = 8u << 20;

    void Run() {
        std::string batch;
        std::unique_lock lock{mutex};
        for (;;) {
            wake.wait(lock, [&] { return !pending.empty(); });
            batch.swap(pending);
            const u64 lost = dropped;
            dropped = 0;
            writing = true;
            lock.unlock();
            std::fwrite(batch.data(), 1, batch.size(), stdout);
            if (lost) {
                std::fprintf(stdout, "Ray tracing: %llu log lines dropped, stdout too slow\n",
                             (unsigned long long)lost);
            }
            batch.clear();
            lock.lock();
            writing = false;
            if (pending.empty()) {
                drained.notify_all();
            }
        }
    }

    bool async{};
    bool writing{};
    u64 dropped{};
    std::string pending;
    std::mutex mutex;
    std::condition_variable wake, drained;
};

__attribute__((format(printf, 1, 2))) void Log(const char* format, ...) {
    char buffer[4096];
    va_list args;
    va_start(args, format);
    const int length = std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (length > 0) {
        LogWriter::Instance().Write(buffer, std::min<size_t>(size_t(length), sizeof(buffer) - 1));
    }
}

class CpuTimer {
public:
    CpuTimer(bool on, double& target_) : target{on ? &target_ : nullptr} {
        if (target) {
            start = std::chrono::steady_clock::now();
        }
    }
    ~CpuTimer() {
        if (target) {
            *target += std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - start)
                           .count();
        }
    }
    CpuTimer(const CpuTimer&) = delete;
    CpuTimer& operator=(const CpuTimer&) = delete;

private:
    double* target;
    std::chrono::steady_clock::time_point start{};
};

constexpr u32 ParamsSlotBytes = 512;
static_assert(sizeof(Rt::GiParams) <= ParamsSlotBytes);
constexpr u32 RestirParamsSlotBytes = 3 * ParamsSlotBytes;
static_assert(sizeof(Rt::RestirGiParams) <= ParamsSlotBytes);
constexpr u32 ShadowParamsSlotBytes = 512;
static_assert(sizeof(Rt::ShadowParams) <= ShadowParamsSlotBytes);
constexpr u32 DirectParamsSlotBytes = 512;
constexpr u32 FogParamsSlotBytes = 256;
constexpr u32 FogSetsSlotBytes = 32768;
// Lamp halos for the GI apply pass: Rt::MaxHalos x (view pos + radius, colour + core).
constexpr u32 HaloSlotBytes = 16 * 32;
static_assert(sizeof(Rt::FogParams) <= FogParamsSlotBytes);
static_assert(sizeof(Rt::DirectParams) <= DirectParamsSlotBytes);
constexpr u32 AoParamsSlotBytes = 256;
constexpr u32 AoStatsSlotBytes = 256;
static_assert(Rt::AoStatsStride * sizeof(u32) <= AoStatsSlotBytes);
static_assert(sizeof(Rt::AoParams) <= AoParamsSlotBytes);
constexpr u32 DirectLightsSlotBytes = Rt::MaxDirectLights * sizeof(Rt::DirectLightGpu);
constexpr u32 GiLightsSlotBytes =
    (DirectLightsSlotBytes + (Rt::LightGridWords + Rt::FlameBlockWords + Rt::FogBlockWords + Rt::HaloBlockWords) *
         u32(sizeof(u32)) + 255u) /
    256u * 256u;
static_assert(Rt::LightGridWords == 36873);
// rt_gi.comp RT_LIGHT_VIS binding 28: FillVisBatch input positions, then FillVisOutputs result
// vec4s per light.
constexpr u32 FillVisSlotBytes =
    (1 + Rt::FillVisOutputs) * Rt::FillVisBatch * 4 * u32(sizeof(float));
// A light traced while off screen or hidden is retried this often until a trusted result lands.
constexpr u64 FillVisRetryFrames = 30;

bool IsSrgb(vk::Format format) {
    return format == vk::Format::eR8G8B8A8Srgb || format == vk::Format::eB8G8R8A8Srgb ||
           format == vk::Format::eA8B8G8R8SrgbPack32;
}

vk::ImageMemoryBarrier2 ImageBarrier(vk::Image image, vk::ImageLayout old_layout,
                                     vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                                     vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
    return vk::ImageMemoryBarrier2{
        .srcStageMask = src_stage,
        .srcAccessMask = src_access,
        .dstStageMask = dst_stage,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = image,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
}

u64 EnvU64(const char* name, u64 fallback, u64 lo, u64 hi) {
    const char* value = Rt::Knob(name);
    if (!value || !*value) {
        return fallback;
    }
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    if (end == value) {
        return fallback;
    }
    return std::clamp<u64>(parsed, lo, hi);
}

float EnvFloat(const char* name, float fallback, float lo, float hi) {
    const char* value = Rt::Knob(name);
    if (!value || !*value) {
        return fallback;
    }
    char* end = nullptr;
    const float parsed = std::strtof(value, &end);
    if (end == value) {
        return fallback;
    }
    return std::clamp(parsed, lo, hi);
}

bool CreateRawBuffer(const Instance& instance, u64 size, VkBufferUsageFlags usage, bool host,
                     u64 alignment, vk::Buffer& buffer, VmaAllocation& allocation, void** mapped,
                     u64& address) {
    const VkBufferCreateInfo buffer_ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
    };
    VmaAllocationCreateInfo alloc_ci{
        .usage = VMA_MEMORY_USAGE_AUTO,
    };
    if (host) {
        alloc_ci.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        alloc_ci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                         VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    } else {
        alloc_ci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    }
    VkBuffer raw{};
    VmaAllocationInfo info{};
    if (vmaCreateBufferWithAlignment(instance.GetAllocator(), &buffer_ci, &alloc_ci,
                                     std::max<u64>(alignment, 16), &raw, &allocation,
                                     &info) != VK_SUCCESS) {
        return false;
    }
    buffer = raw;
    if (mapped) {
        *mapped = info.pMappedData;
    }
    address = instance.GetDevice().getBufferAddress({.buffer = buffer});
    if (address) {
        const u32 serial = Rt::AddressMap::Add(address, size);
        std::printf("RT buffer #%u: %#llx-%#llx (%llu KiB)\n", serial, (unsigned long long)address,
                    (unsigned long long)(address + size), (unsigned long long)(size >> 10));
    }
    return address != 0 && (!host || info.pMappedData);
}

static_assert(Rt::GiPipelineOmmBit == VK_PIPELINE_CREATE_RAY_TRACING_OPACITY_MICROMAP_BIT_EXT);

constexpr vk::ShaderStageFlags GiRtStages =
    vk::ShaderStageFlagBits::eRaygenKHR | vk::ShaderStageFlagBits::eAnyHitKHR |
    vk::ShaderStageFlagBits::eClosestHitKHR | vk::ShaderStageFlagBits::eMissKHR;

vk::AccelerationStructureGeometryKHR SoupGeometry(u64 soup_address, u32 max_vertex) {
    return vk::AccelerationStructureGeometryKHR{
        .geometryType = vk::GeometryTypeKHR::eTriangles,
        .geometry = {.triangles = vk::AccelerationStructureGeometryTrianglesDataKHR{
                         .vertexFormat = vk::Format::eR32G32B32Sfloat,
                         .vertexData = {.deviceAddress = soup_address},
                         .vertexStride = Rt::VertexStride,
                         .maxVertex = max_vertex,
                         .indexType = vk::IndexType::eNoneKHR,
                     }},
        .flags = vk::GeometryFlagBitsKHR::eOpaque,
    };
}

vk::AccelerationStructureGeometryKHR InstanceGeometry(u64 instances_address) {
    return vk::AccelerationStructureGeometryKHR{
        .geometryType = vk::GeometryTypeKHR::eInstances,
        .geometry = {.instances = vk::AccelerationStructureGeometryInstancesDataKHR{
                         .arrayOfPointers = false,
                         .data = {.deviceAddress = instances_address},
                     }},
    };
}

static_assert(Rt::BuildAllowUpdate == VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR);
static_assert(Rt::BuildAllowCompaction == VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR);
static_assert(Rt::BuildFastTrace == VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR);
static_assert(Rt::BuildFastBuild == VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR);

vk::AccelerationStructureBuildGeometryInfoKHR BlasInfo(
    const vk::AccelerationStructureGeometryKHR* geometry, u32 flags) {
    return vk::AccelerationStructureBuildGeometryInfoKHR{
        .type = vk::AccelerationStructureTypeKHR::eBottomLevel,
        .flags = vk::BuildAccelerationStructureFlagsKHR(flags),
        .mode = vk::BuildAccelerationStructureModeKHR::eBuild,
        .geometryCount = 1,
        .pGeometries = geometry,
    };
}

void Barrier(vk::CommandBuffer cmdbuf, vk::PipelineStageFlags2 src_stage,
             vk::AccessFlags2 src_access, vk::PipelineStageFlags2 dst_stage,
             vk::AccessFlags2 dst_access) {
    const vk::MemoryBarrier2 barrier{
        .srcStageMask = src_stage,
        .srcAccessMask = src_access,
        .dstStageMask = dst_stage,
        .dstAccessMask = dst_access,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
}

}

bool RayTracing::FxSmokeWanted() {
    return Rt::FxSmokeWanted();
}

bool RayTracing::Wanted() {
    static const bool wanted = [] {
        const char* value = std::getenv("BB_RT");
        return value && value[0] == '1';
    }();
    return wanted;
}

RayTracing::RayTracing(const Instance& instance_, Scheduler& scheduler_, Runtime& runtime_,
                       VideoCore::TextureCache& texture_cache_,
                       VideoCore::BufferCache& buffer_cache_)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_},
      texture_cache{texture_cache_}, buffer_cache{buffer_cache_} {
    if (!Wanted()) {
        return;
    }
    if (!instance.IsRayQuerySupported()) {
        Log("Ray tracing: off (device extensions unavailable)\n");
        return;
    }
    Rt::LiveKnobs().Open(std::getenv("BB_RT_CFG"));
    if (const char* cfg = std::getenv("BB_RT_CFG"); cfg && cfg[0]) {
        glass_mask_dir = (std::filesystem::path(cfg).parent_path() / "glass_masks").string();
    }
    if (Rt::LiveKnobs().Poll()) {
        Log("Ray tracing: live config %s read (%zu keys, polled every 30 frames)\n",
            Rt::LiveKnobs().Path().c_str(), Rt::LiveKnobs().Values().size());
    }
    if (const char* dir = std::getenv("BB_RT_SHADER_DIR")) {
        shader_dir = dir;
        Log("Ray tracing: compute shaders reload from %s/<name>.spv when the file changes\n", dir);
    }
    positions_address = Shader::MotionVectors::positions_address;
    if (!positions_address) {
        Log("Ray tracing: off (object motion capture is off: enable Object motion vectors "
            "and an upscaler, or BB_OBJECT_MOTION=1)\n");
        return;
    }
    range = EnvFloat("BB_RT_RANGE", 50.0f, 0.01f, 1e6f);
    ao_radius = EnvFloat("BB_RT_AO_RADIUS", 0.5f, 0.001f, 1e5f);
    capacity.triangles = u32(EnvU64("BB_RT_TRIANGLES", 4u << 20, 1024, 16u << 20));
    capacity.index_bytes =
        u32((std::min<u64>(u64(capacity.triangles) * 6, 1u << 30) + 255) & ~u64(255));
    capacity.records = 8192;
    const Rt::LiveToggles wanted = Rt::ReadToggles(Rt::Knob, instance.GetVendorID());
    gi_spp = u32(EnvU64("BB_RT_SPP", 1, 1, 16));
    gi_debug = u32(EnvU64("BB_RT_GI_DEBUG", 0, 0, 27));
    gi_max_history = u32(EnvU64("BB_RT_GI_HISTORY", 16, 1, 256));
    gi_bounces = u32(EnvU64("BB_RT_BOUNCES", 2, 0, Rt::MaxBounces));
    gi_bounce_rays = u32(EnvU64("BB_RT_BOUNCE_RAYS", 1, 0, Rt::MaxBounceRays));
    gi_emissive_luma = EnvFloat("BB_RT_EMISSIVE_LUMA", 2.0f, 0.0f, 1e4f);
    gi_last_bounce_cache = EnvU64("BB_RT_GI_LAST_BOUNCE_CACHE", 1, 0, 1) != 0;
    gi_light_grid = EnvU64("BB_RT_GI_LIGHT_GRID", 1, 0, 1) != 0;
    skin_fast_trace = EnvU64("BB_RT_SKIN_FAST_TRACE", 1, 0, 1) != 0;
    skin_log = EnvU64("BB_RT_SKIN_LOG", 0, 0, 1) != 0;
    moving_refit = EnvU64("BB_RT_MOVING_REFIT", 1, 0, 1) != 0;
    moving_tris = u32(EnvU64("BB_RT_MOVING_TRIS", 65536, 1, 16u << 20));
    cache.moving_frames = EnvU64("BB_RT_MOVING_FRAMES", 600, 1, 1000000);
    probe_keep = EnvU64("BB_RT_PROBE_KEEP", 1, 0, 1) != 0;
    skin_alpha = EnvU64("BB_RT_SKIN_ALPHA", 1, 0, 1) != 0;
    Log("Ray tracing: draws the probe caught moving refit every frame %s (BB_RT_MOVING_REFIT, up "
        "to %u triangles BB_RT_MOVING_TRIS, for %llu frames after the last mismatch "
        "BB_RT_MOVING_FRAMES); probe mismatches on refittable BLAS keep the last "
        "pose %s (BB_RT_PROBE_KEEP); skinned alpha test %s (BB_RT_SKIN_ALPHA); skinned instance "
        "log %s (BB_RT_SKIN_LOG)\n",
        moving_refit ? "on" : "off", moving_tris, (unsigned long long)cache.moving_frames,
        probe_keep ? "on" : "off",

        skin_alpha ? "on" : "off, traced opaque", skin_log ? "on" : "off");
    refit_anchor = EnvU64("BB_RT_REFIT_ANCHOR", 1, 0, 1) != 0;
    Log("Ray tracing: refits keep the camera space of their last full build %s "
        "(BB_RT_REFIT_ANCHOR, off refits in the current camera space)\n",
        refit_anchor ? "on" : "off");
    cache.max_refits = u32(EnvU64("BB_RT_REFIT_MAX", 20, 1, 1000));
    emissive_target_index = int(EnvU64("BB_RT_EMISSIVE_TARGET", Rt::EmissiveTargetDefault, 0, 0xFF));
    if (emissive_target_index == 0xFF || emissive_target_index >= int(gbuffer_targets.size())) {
        emissive_target_index = -1;
    }
    restir_gi_m_clamp = u32(EnvU64("BB_RT_RESTIR_GI_M", Rt::RestirGiMClampDefault, 1, 1024));
    restir_gi_max_age = u32(EnvU64("BB_RT_RESTIR_GI_AGE", Rt::RestirGiMaxAgeDefault, 1, 1024));
    restir_gi_spatial =
        u32(EnvU64("BB_RT_RESTIR_GI_SPATIAL", Rt::RestirGiSpatialDefault, 0, Rt::RestirGiMaxSpatial));
    restir_gi_neighbours = u32(EnvU64("BB_RT_RESTIR_GI_NEIGHBOURS", Rt::RestirGiNeighboursDefault, 1,
                                      Rt::RestirGiMaxNeighbours));
    restir_gi_radius = EnvFloat("BB_RT_RESTIR_GI_RADIUS", Rt::RestirGiRadiusDefault, 1.0f, 256.0f);
    if (const char* hash = std::getenv("BB_UPSCALE_BEFORE_CS")) {
        trigger_hash = std::strtoull(hash, nullptr, 16);
    }
    if (const char* normal = std::getenv("BB_RT_GBUF_NORMAL")) {
        if (!Rt::ParseNormalSource(normal, normal_source)) {
            Log("Ray tracing: BB_RT_GBUF_NORMAL=%s not understood (depth or "
                        "<target>.<swizzle>[.u|s][w]), using depth\n", normal);
            normal_source = {};
        }
    }
    albedo_target = int(EnvU64("BB_RT_GBUF_ALBEDO", 0xFF, 0, 0xFF));
    if (albedo_target == 0xFF) {
        albedo_target = -1;
    }
    if (gi_debug >= 11) {
        debug_target = int(gi_debug % 10);
    }
    const char* shadows_env = std::getenv("BB_RT_SHADOWS");
    shadows_enabled = shadows_env && shadows_env[0] == '1';
    shadow_spec_target = int(EnvU64("BB_RT_SHADOW_SPEC", 3, 0, 0xFF));
    if (shadow_spec_target == 0xFF) {
        shadow_spec_target = -1;
    }
    light_max_local = u32(EnvU64("BB_RT_SHADOW_LIGHTS", 4, 0, Rt::MaxShadowLights));
    ReadLiveKnobs();
    ao_max_history = u32(EnvU64("BB_RT_AO_HISTORY", 16, 0, 64));
    ao_scale = u32(EnvU64("BB_RT_AO_SCALE", 2, 1, 4));
    exclude_hashes = Rt::ParseHashList(std::getenv("BB_RT_EXCLUDE"));
    fx_smoke = FxSmokeWanted();
    fx_cap = u32(EnvU64("BB_RT_FX_CAP", 256, 0, 4096));
    {
        const char* sky_env = std::getenv("BB_RT_SKY");
        sky_enabled = !sky_env || sky_env[0] != '0';
    }
    keep_rules.unused_frames = UnusedFrames;
    direct_g1_target = int(EnvU64("BB_RT_DIRECT_G1", 1, 0, 0xFF));
    if (direct_g1_target == 0xFF) {
        direct_g1_target = -1;
    }
    restir_candidates = u32(EnvU64("BB_RT_RESTIR_M", 8, 1, Rt::RestirMaxCandidates));
    restir_history = u32(EnvU64("BB_RT_RESTIR_HISTORY", 20, 0, Rt::RestirMaxHistory));
    restir_spatial = u32(EnvU64("BB_RT_RESTIR_SPATIAL", 5, 0, Rt::RestirMaxSpatial));
    light_dump_first = u32(EnvU64("BB_RT_LIGHT_DUMP_FRAMES", 3, 1, 100000));
    uvs_address = Shader::MotionVectors::uvs_address;
    normals_address = Shader::MotionVectors::normals_address;
    const bool surface_ok = uvs_address != 0 && instance.IsRtSurfaceSupported();
    surface_avail = surface_ok;
    toggle_avail = {.gi = surface_ok,
                    .direct = true,
                    .reflect = surface_ok,
                    .fog = true,
                    .ao = true,
                    .restir_gi = surface_ok,
                    .restir_di = true,
                    .gi_pipeline = surface_ok && instance.IsRayTracingPipelineSupported(),
                    .gi_ser = surface_ok && instance.IsRayTracingReorderSupported(),
                    .ptlas = instance.IsPartitionedAccelerationStructureSupported(),
                    .rr = true};
    if (!surface_ok) {
        Log("Ray tracing: hit surfaces off (%s), GI, reflections and ReSTIR GI unavailable\n",
            uvs_address ? "descriptor indexing unavailable" : "no UV capture buffer");
    }
    ApplyToggles(wanted);
    if (restir_gi_enabled) {
        Log("Ray tracing: ReSTIR GI on (BB_RT_RESTIR_GI=1, default; 0 = full-ray GI): first-bounce "
            "sample reservoirs, glass and painted-window aware spatial visibility, temporal "
            "reuse M clamp %u (BB_RT_RESTIR_GI_M), sample age limit %u (BB_RT_RESTIR_GI_AGE), %u "
            "spatial pass(es) (BB_RT_RESTIR_GI_SPATIAL, 0-2) of %u neighbours "
            "(BB_RT_RESTIR_GI_NEIGHBOURS) within %g px (BB_RT_RESTIR_GI_RADIUS), one visibility "
            "ray per reused sample\n",
            restir_gi_m_clamp, restir_gi_max_age, restir_gi_spatial, restir_gi_neighbours,
            restir_gi_radius);
        Log("Ray tracing: ReSTIR GI traces 1 of %u pixels per frame (BB_RT_GI_SPARSE 1, 2 or 4; "
            "every pixel on camera cuts)\n",
            gi_sparse);
    }
    const auto env_on = [](const char* name) {
        const char* value = std::getenv(name);
        return !value || value[0] != '0';
    };
    compact_enabled = env_on("BB_RT_COMPACT");
    time_enabled = env_on("BB_RT_GPU_TIME");
    omm_enabled = env_on("BB_RT_OMM") && instance.IsOpacityMicromapSupported() && surface_avail &&
                  Rt::NeedsSurfaces(toggles);
    omm_level = u32(EnvU64("BB_RT_OMM_LEVEL", Rt::OmmDefaultLevel, 0, Rt::OmmMaxLevel));
    omm_budget = u32(EnvU64("BB_RT_OMM_TRIANGLES", 1u << 18, 0, 1u << 24));
    omm_log = Rt::KnobIs("BB_RT_OMM_LOG", '1');
    Log("Ray tracing: BLAS compaction %s (BB_RT_COMPACT), GPU pass timing %s (BB_RT_GPU_TIME), "
        "opacity micromaps %s level %u, %u triangles per frame (BB_RT_OMM, BB_RT_OMM_LEVEL, "
        "BB_RT_OMM_TRIANGLES), cluster acceleration structures %s and not used\n",
        compact_enabled ? "on" : "off", time_enabled ? "on" : "off",
        omm_enabled ? "on" : instance.IsOpacityMicromapSupported() ? "off" : "unavailable",
        omm_level, omm_budget,
        instance.IsClusterAccelerationStructureExposed() ? "exposed" : "not exposed");
    if (!toggle_avail.gi_pipeline) {
        Log("Ray tracing: GI pipeline unavailable (hit surfaces %s, VK_KHR_ray_tracing_pipeline %s; "
            "enabled at launch by BB_RT_GI_PIPELINE=1 or the non-AMD default), compute GI kept\n",
            surface_ok ? "on" : "off",
            instance.IsRayTracingPipelineExposed() ? "exposed but not enabled" : "not exposed");
    } else if (!gi_rt_pipeline) {
        const char* gi_pipeline_env = Rt::Knob("BB_RT_GI_PIPELINE");
        Log("Ray tracing: GI pipeline off at start (BB_RT_GI_PIPELINE=%s, needs ReSTIR GI; live "
            "switch), compute GI kept\n",
            gi_pipeline_env && gi_pipeline_env[0] ? gi_pipeline_env : "unset");
    }
    direct_lights_wanted = direct_enabled || reflect_enabled || (gi_enabled && gi_bounces > 0);
    made = Rt::MergeFeatures(Rt::NoFeatures(), Rt::BuildSet(toggles));
    if (!CreateResources() ||
        (Rt::NeedsSurfaces(made) && !(surface_made = CreateSurfaceResources())) ||
        !CreatePipelines(made)) {
        Log("Ray tracing: off (resource creation failed)\n");
        DestroyResources();
        return;
    }
    surface_enabled = surface_avail && surface_made && (gi_enabled || direct_enabled);
    const char* hitch_env = std::getenv("BB_RT_HITCH_LOG");
    hitch_log = !hitch_env || hitch_env[0] != '0';
    OpenLightDump();
    light_capture = light_dump != nullptr || shadows_enabled || direct_lights_wanted || ao_enabled;
    if (direct_enabled) {
        Log("Ray tracing: direct lighting on (BB_RT_DIRECT): the light accumulation targets are "
            "overwritten before GXLightComposite with all captured point/spot lights and the "
            "per-pixel directional set; up to %u shadow rays per pixel (BB_RT_DIRECT_RAYS), "
            "lights kept %u frames after their last draw (BB_RT_DIRECT_KEEP), GBuffer1 target %d "
            "(BB_RT_DIRECT_G1, 255 off), GBuffer3 target %d (BB_RT_SHADOW_SPEC), debug %u "
            "(BB_RT_DIRECT_DEBUG: 1 our diffuse, 2 tile light count, 3 |game - ours|, 4 red = tile "
            "has lights, green bar = light count, blue = pass wrote, 5 strongest light blocker: black "
            "lit, blue forced opaque, cyan OMM opaque, red candidate without alpha test, green = "
            "sampled alpha, +red when it blocked, 6 AO mask alone, 7 light set id (set 0 red, top strip = set 0 moon colour x4), 8 strongest "
            "point/spot light per pixel as its index colour, brightness = its share, 11 moon: green lit, black shadowed, blue faces away; BB_RT_DIRECT_LIGHT_LOG=<m> "
            "lists the lights within m metres of the camera once a second), "
            "visibility %s (BB_RT_DIRECT_NOVIS), moon shadow %s (BB_RT_MOON=trace|mask, BB_RT_MOON_ANGLE tenths of a degree), moon ray sample %s (BB_RT_MOON_SEED=static|frame), directional set log %s (BB_RT_DIR_LOG), depth-only draws %s (BB_RT_DEPTH_DRAWS); directional sets copied at the game's light draw of the same frame, never averaged across frames because the game renumbers its sets per frame (BB_RT_DIR_SETS=snap|gpu|host)\n",
            direct_rays, direct_keep, direct_g1_target, shadow_spec_target, direct_debug,
            direct_novis ? "forced 1" : "traced",
            moon_mask ? "the game's cascade mask and FC_ShadowColor"
                      : "traced in the moon cone with the FC_ShadowColor fill",
            moon_static ? "fixed per pixel" : "new every frame", dir_log ? "on" : "off",
            depth_only_draws ? "kept" : "skipped");
        Log("Ray tracing: shadow caster pick %s (BB_RT_PICK=fx,fy screen fraction: once a second "
            "logs what blocks the two strongest lights at that pixel); alpha-tested draws without a "
            "usable texture or UVs %s (BB_RT_ALPHA_UNRESOLVED=solid|skip); %zu shader hashes "
            "excluded from capture (BB_RT_EXCLUDE=hex ps or vs hashes)\n",
            pick_enabled ? "on" : "off",
            alpha_skip_unresolved ? "left out of the TLAS" : "traced as solid",
            exclude_hashes.size());
        if (pick_enabled) {
            Log("Ray tracing: pick at %.3f,%.3f of the frame%s\n", pick_at[0], pick_at[1],
                restir_enabled ? " (needs BB_RT_RESTIR=0: the ReSTIR pass does not run it)" : "");
        }
        Log("Ray tracing: ambient occlusion %s (BB_RT_AO: 0 = none, the game's AO draw skipped "
            "unless BB_RT_GAME_AO=1; 1 = ray traced into the SSAO channel of the shadow mask "
            "before GXLightDeferredAO): %u cosine rays of "
            "%.2f m with distance falloff per %ux%u pixel block (BB_RT_AO_RAYS, BB_RT_AO_RADIUS, "
            "BB_RT_AO_SCALE), history %u frames (BB_RT_AO_HISTORY), debug %u (BB_RT_AO_DEBUG: 1 = "
            "no history)\n",
            ao_enabled ? "ray traced" : "off", ao_rays, ao_radius, ao_scale, ao_scale,
            ao_max_history, ao_debug);
    }
    if (restir_enabled) {
        Log("Ray tracing: ReSTIR DI on (BB_RT_RESTIR): %u light candidates per pixel from the "
            "tile list (BB_RT_RESTIR_M), temporal reuse with history clamp %u (BB_RT_RESTIR_HISTORY, "
            "0 off), %u spatial taps (BB_RT_RESTIR_SPATIAL), one shadow ray per pixel; the "
            "directional set keeps the slice 6 path\n",
            restir_candidates, restir_history, restir_spatial);
    }
    if (reflect_enabled) {
        Log("Ray tracing: reflections on (BB_RT_REFLECT): one GGX reflection ray per pixel "
            "replaces the game's SSR term in the specular accumulation before GXLightComposite "
            "for roughness <= %g (BB_RT_REFLECT_MAX_ROUGH), strength %g (BB_RT_REFLECT_STRENGTH), "
            "debug %u (BB_RT_REFLECT_DEBUG: 1 reflection only, 2 weight/roughness/miss, 3 hit "
            "radiance); %s\n",
            reflect_max_rough, reflect_strength, reflect_debug,
            direct_enabled ? "the specular is rebuilt from the AO pass inputs with our term when "
                             "the game's light draws were skipped, else added on top"
                           : "added on top of the game's specular (the replacement needs "
                             "BB_RT_DIRECT=1)");
    }
    if (shadows_enabled) {
        Log("Ray tracing: shadows on, %u strongest unshadowed point lights per frame "
            "(BB_RT_SHADOW_LIGHTS, max %u), strength %g (BB_RT_SHADOW_STRENGTH), bias %g "
            "(BB_RT_SHADOW_BIAS), tmin %g (BB_RT_SHADOW_TMIN), end gap %g (BB_RT_SHADOW_GAP), "
            "debug %u (BB_RT_SHADOW_DEBUG=1 visibility mask), specular target %d "
            "(BB_RT_SHADOW_SPEC, 255 off)\n",
            light_max_local, Rt::MaxShadowLights, shadow_strength, shadow_bias, shadow_tmin,
            shadow_gap, shadow_debug, shadow_spec_target);
    }
    rebuild_budget.Reset(EnvU64("BB_RT_REBUILD_BUDGET", 1u << 20, 0, 1u << 30));
    refit_budget.Reset(EnvU64("BB_RT_REFIT_BUDGET", 1u << 19, 0, 1u << 30),
                       EnvU64("BB_RT_REFIT_HOLD", 3, 1, 60));
    Log("Ray tracing: refit budget %llu triangles per frame (BB_RT_REFIT_BUDGET, 0 unlimited), "
        "skinned BLAS over it keep their last pose for up to %llu frames (BB_RT_REFIT_HOLD), "
        "hitch log %s above %.0f ms (BB_RT_HITCH_LOG, BB_RT_HITCH_MS); skinned BLAS rebuilt "
        "after %u refits (BB_RT_REFIT_MAX), built for %s (BB_RT_SKIN_FAST_TRACE); log written "
        "by a background thread: %s (BB_RT_LOG_ASYNC)\n",
        (unsigned long long)refit_budget.Budget(), (unsigned long long)refit_budget.MaxHold(),
        hitch_log ? "on" : "off", hitch_threshold_ms, cache.max_refits,
        skin_fast_trace ? "fast trace" : "fast build", LogWriter::Instance().Async() ? "on" : "off");
    Log("Ray tracing: live switches from BB_RT_CFG (startup values from the env): GI %u, direct "
        "%u, reflections %u, fog %u, AO %u, ReSTIR GI %u, GI sparse %u, ReSTIR DI %u, GI pipeline "
        "%u, SER %u, partitioned TLAS %u, DLSS RR %u; unavailable on this device:%s%s%s%s%s; "
        "startup only: BB_RT_OMM, BB_RT_COMPACT, BB_RT_SHADOWS, BB_RT_SKY, BB_RT_FX_SMOKE, "
        "BB_RT_BOUNCES, BB_RT_GI_HISTORY\n",
        u32(toggles.gi), u32(toggles.direct), u32(toggles.reflect), u32(toggles.fog),
        u32(toggles.ao), u32(toggles.restir_gi), toggles.gi_sparse, u32(toggles.restir_di),
        u32(toggles.gi_pipeline), u32(toggles.gi_ser), u32(toggles.ptlas), u32(toggles.rr),
        toggle_avail.gi ? "" : " GI/reflections/ReSTIR GI",
        toggle_avail.gi_pipeline ? "" : " GI pipeline", toggle_avail.gi_ser ? "" : " SER",
        toggle_avail.ptlas ? "" : " partitioned TLAS",
        toggle_avail.gi && toggle_avail.gi_pipeline && toggle_avail.gi_ser && toggle_avail.ptlas
            ? " none"
            : "");
    LogLiveState();
    enabled = true;
    crumb_owner = this;
    Instance::lost_hook = [] {
        if (crumb_owner) {
            crumb_owner->PrintCrumbs();
        }
        LogWriter::Instance().Drain(std::chrono::milliseconds(2000));
    };
    BeginSlot();
    Log("Ray tracing: on, debug view %u (BB_RT_DEBUG=0 off, 1 hit distance, 2 AO, "
                "3 distance blended, 4 AO on the frame; drawn into the scene colour at the post "
                "trigger), %u triangles per frame (BB_RT_TRIANGLES), soup %llu MiB, BLAS pool "
                "%llu MiB (BB_RT_BLAS_MB), scratch %llu MiB, index ring %u MiB x %u, up to %u "
                "cached BLAS, rebuild budget %llu triangles per frame (BB_RT_REBUILD_BUDGET, 0 "
                "unlimited), hitch log %s (BB_RT_HITCH_LOG)\n",
                debug_mode, capacity.triangles, (unsigned long long)(soup.size >> 20),
                (unsigned long long)(pool_buffer.size >> 20),
                (unsigned long long)(scratch.size >> 20), indices_ring.slot_bytes >> 20, Slots,
                capacity.records, (unsigned long long)rebuild_budget.Budget(),
                hitch_log ? "on" : "off");
    if (gi_enabled) {
        Log("Ray tracing: GI bounces %u (BB_RT_BOUNCES, 0 = single segment with screen/cache "
            "colour), %u shadow rays per bounce hit (BB_RT_BOUNCE_RAYS), emission from the game's "
            "emissive G-buffer target %d (BB_RT_EMISSIVE_TARGET, 255 off; per-BLAS emissive cache "
            "off screen), else cache luma above %g (BB_RT_EMISSIVE_LUMA, 0 off); sky on miss from "
            "the screen, a %ux%u octahedral world-direction sky probe, the learned sky mean, then "
            "BB_RT_SKY; denoiser aux images written every GI frame; bounces after the first end in "
            "the screen/material cache: %s (BB_RT_GI_LAST_BOUNCE_CACHE); bounce lights culled by "
            "a view-space light grid: %s (BB_RT_GI_LIGHT_GRID)\n",
            gi_bounces, gi_bounce_rays, emissive_target_index, gi_emissive_luma, Rt::SkyProbeSize,
            Rt::SkyProbeSize, gi_last_bounce_cache ? "on" : "off", gi_light_grid ? "on" : "off");
        Log("Ray tracing: GI on, %u spp (BB_RT_SPP), strength %g (BB_RT_GI_STRENGTH), "
                    "sky %g (BB_RT_SKY), history %u (BB_RT_GI_HISTORY), clamp %g, debug %u "
                    "(BB_RT_GI_DEBUG: 1 raw, 2 accumulated, 3 albedo, 4 normal, 5 history "
                    "count, 8 hit source: green reprojected, blue material cache, red sky, grey miss, "
                    "10+i G-buffer target i, 20+i its alpha), normal source %s, albedo "
                    "target %s, trigger cs %llx\n",
                    gi_spp, gi_strength, gi_sky, gi_max_history, gi_clamp, gi_debug,
                    normal_source.target == 0xFF      ? "depth"
                    : std::getenv("BB_RT_GBUF_NORMAL") ? std::getenv("BB_RT_GBUF_NORMAL")
                                                       : "0.rgb.uw (G-buffer 0, world space)",
                    albedo_target < 0 ? "auto (first sRGB target)"
                                      : std::to_string(albedo_target).c_str(),
                    (unsigned long long)trigger_hash);
    }
}

RayTracing::~RayTracing() {
    Instance::lost_hook = nullptr;
    crumb_owner = nullptr;
    if (enabled || pool_buffer.buffer) {
        scheduler.Finish();
    }
    DestroyGiImages();
    DestroyFogImages();
    DestroyResources();
}

bool RayTracing::CreateFogImages(u32 width, u32 height) {
    DestroyFogImages();
    const auto device = instance.GetDevice();
    for (OwnImage& own : fog_images) {
        own.image = VideoCore::UniqueImage(device, instance.GetAllocator());
        if (!own.image.TryCreate(vk::ImageCreateInfo{
                .imageType = vk::ImageType::e2D,
                .format = vk::Format::eR16G16B16A16Sfloat,
                .extent = {width, height, 1},
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = vk::SampleCountFlagBits::e1,
                .tiling = vk::ImageTiling::eOptimal,
                .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
                .initialLayout = vk::ImageLayout::eUndefined,
            })) {
            DestroyFogImages();
            return false;
        }
        auto view = device.createImageViewUnique({
            .image = vk::Image(own.image),
            .viewType = vk::ImageViewType::e2D,
            .format = vk::Format::eR16G16B16A16Sfloat,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        });
        if (view.result != vk::Result::eSuccess) {
            DestroyFogImages();
            return false;
        }
        own.view = std::move(view.value);
        own.written = false;
    }
    fog_width = width;
    fog_height_px = height;
    fog_last_frame = ~0ull;
    return true;
}

void RayTracing::DestroyFogImages() {
    if (fog_width) {
        scheduler.Finish();
    }
    for (OwnImage& own : fog_images) {
        own.view.reset();
        own.image.Destroy();
        own.written = false;
    }
    fog_width = fog_height_px = 0;
}

bool RayTracing::MakeOwnImage(OwnImage& own, vk::Format format, u32 width, u32 height,
                              vk::ImageUsageFlags extra_usage) {
    const auto device = instance.GetDevice();
    own.image = VideoCore::UniqueImage(device, instance.GetAllocator());
    if (!own.image.TryCreate(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = format,
            .extent = {width, height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                     extra_usage,
            .initialLayout = vk::ImageLayout::eUndefined,
        })) {
        return false;
    }
    auto view = device.createImageViewUnique({
        .image = vk::Image(own.image),
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    });
    if (view.result != vk::Result::eSuccess) {
        return false;
    }
    own.view = std::move(view.value);
    own.written = false;
    return true;
}

bool RayTracing::CreateReflectImages(u32 width, u32 height) {
    CpuTimer timer{hitch_log, hitch.images};
    auto& r = reflect_own;
    const auto destroy = [&] {
        for (OwnImage* own : {&r.raw, &r.aux_albedo, &r.aux_specular, &r.aux_normal, &r.aux_hit,
                              &r.spec_hit, &r.glass_keep}) {
            own->view.reset();
            own->image.Destroy();
            own->written = false;
        }
        r.width = r.height = 0;
    };
    if (r.width) {
        scheduler.Finish();
    }
    destroy();
    if (!MakeOwnImage(r.raw, vk::Format::eR16G16B16A16Sfloat, width, height) ||
        !MakeOwnImage(r.aux_albedo, vk::Format::eR16G16B16A16Sfloat, width, height) ||
        !MakeOwnImage(r.aux_specular, vk::Format::eR16G16B16A16Sfloat, width, height) ||
        !MakeOwnImage(r.aux_normal, vk::Format::eR16G16B16A16Sfloat, width, height) ||
        !MakeOwnImage(r.aux_hit, vk::Format::eR16Sfloat, width, height) ||
        !MakeOwnImage(r.spec_hit, vk::Format::eR16Sfloat, width, height) ||
        !MakeOwnImage(r.glass_keep, vk::Format::eR16Sfloat, width, height)) {
        destroy();
        return false;
    }
    r.width = width;
    r.height = height;
    Log("Ray tracing: reflection images %ux%u of their own (GI images are %ux%u)\n", width, height,
        gi_width, gi_height);
    return true;
}

bool RayTracing::CreateGiImages(u32 width, u32 height) {
    CpuTimer timer{hitch_log, hitch.images};
    DestroyGiImages();
    const auto make = [&](OwnImage& own, vk::Format format) {
        return MakeOwnImage(own, format, width, height);
    };
    for (u32 i = 0; i < 2; ++i) {
        if (!make(history[i], vk::Format::eR16G16B16A16Sfloat) ||
            !make(history_z[i], vk::Format::eR32Sfloat)) {
            DestroyGiImages();
            return false;
        }
    }
    if (!make(raw, vk::Format::eR16G16B16A16Sfloat) ||
        !make(aux_albedo, vk::Format::eR16G16B16A16Sfloat) ||
        !make(aux_specular, vk::Format::eR16G16B16A16Sfloat) ||
        !make(aux_normal, vk::Format::eR16G16B16A16Sfloat) ||
        !make(aux_hit, vk::Format::eR16Sfloat) ||
        (made.reflect && !make(aux_spec_hit, vk::Format::eR16Sfloat)) ||
        (made.reflect && !make(glass_keep, vk::Format::eR16Sfloat))) {
        DestroyGiImages();
        return false;
    }
    if (made.restir_gi) {
        for (auto& set : restir_gi_sets) {
            if (!make(set[0], vk::Format::eR32G32B32A32Sfloat) ||
                !make(set[1], vk::Format::eR16G16B16A16Sfloat) ||
                !make(set[2], vk::Format::eR16G16B16A16Sfloat) ||
                !make(set[3], vk::Format::eR16G16B16A16Sfloat)) {
                DestroyGiImages();
                return false;
            }
        }
        restir_gi_history = 0;
        Log("Ray tracing: ReSTIR GI reservoirs %ux%u x%u (%llu MiB)\n", width, height,
            Rt::RestirGiSets,
            (unsigned long long)((u64(width) * height * 40 * Rt::RestirGiSets) >> 20));
    }
    gi_width = width;
    gi_height = height;
    history_index = 0;
    history_valid = false;
    Log("Ray tracing: GI images %ux%u (%llu MiB, denoiser aux %llu MiB)\n", width, height,
        (unsigned long long)((u64(width) * height * (8 + 4) * 2 + u64(width) * height * 8) >> 20),
        (unsigned long long)((u64(width) * height * (8 * 3 + 2)) >> 20));
    return true;
}

void RayTracing::DestroyGiImages() {
    if (gi_width) {
        scheduler.Finish();
    }
    for (OwnImage* own : {&history[0], &history[1], &history_z[0], &history_z[1], &raw, &aux_albedo,
                          &aux_specular, &aux_normal, &aux_hit, &aux_spec_hit, &glass_keep,
                          &reflect_own.raw, &reflect_own.aux_albedo, &reflect_own.aux_specular,
                          &reflect_own.aux_normal, &reflect_own.aux_hit, &reflect_own.spec_hit,
                          &reflect_own.glass_keep}) {
        own->view.reset();
        own->image.Destroy();
        own->written = false;
    }
    reflect_own.width = reflect_own.height = 0;
    glass_keep_frame = ~0ull;
    for (auto& set : restir_gi_sets) {
        for (OwnImage& own : set) {
            own.view.reset();
            own.image.Destroy();
            own.written = false;
        }
    }
    gi_width = gi_height = 0;
}

bool RayTracing::CreateAoImages(u32 width, u32 height) {
    DestroyAoImages();
    const auto device = instance.GetDevice();
    for (OwnImage& own : ao_history) {
        own.image = VideoCore::UniqueImage(device, instance.GetAllocator());
        if (!own.image.TryCreate(vk::ImageCreateInfo{
                .imageType = vk::ImageType::e2D,
                .format = vk::Format::eR16G16B16A16Sfloat,
                .extent = {width, height, 1},
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = vk::SampleCountFlagBits::e1,
                .tiling = vk::ImageTiling::eOptimal,
                .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
                .initialLayout = vk::ImageLayout::eUndefined,
            })) {
            DestroyAoImages();
            return false;
        }
        auto view = device.createImageViewUnique({
            .image = vk::Image(own.image),
            .viewType = vk::ImageViewType::e2D,
            .format = vk::Format::eR16G16B16A16Sfloat,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        });
        if (view.result != vk::Result::eSuccess) {
            DestroyAoImages();
            return false;
        }
        own.view = std::move(view.value);
        own.written = false;
    }
    ao_width = width;
    ao_height = height;
    ao_history_index = 0;
    ao_history_valid = false;
    Log("Ray tracing: AO history %ux%u (%llu MiB)\n", width, height,
        (unsigned long long)((u64(width) * height * 16) >> 20));
    return true;
}

void RayTracing::DestroyAoImages() {
    if (ao_width) {
        scheduler.Finish();
    }
    for (OwnImage& own : ao_history) {
        own.view.reset();
        own.image.Destroy();
        own.written = false;
    }
    ao_width = ao_height = 0;
    ao_history_valid = false;
}

bool RayTracing::CreateResources() {
    const auto device = instance.GetDevice();
    const auto host_ring = [&](HostRing& ring, u32 slot_bytes, VkBufferUsageFlags usage) {
        ring.slot_bytes = slot_bytes;
        void* mapped = nullptr;
        if (!CreateRawBuffer(instance, u64(slot_bytes) * Slots, usage, true, 16, ring.buffer,
                             ring.allocation, &mapped, ring.address)) {
            return false;
        }
        ring.mapped = static_cast<u8*>(mapped);
        return true;
    };
    if (!host_ring(records_ring, capacity.records * sizeof(Rt::DrawRecord),
                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
        !host_ring(indices_ring, capacity.index_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
        !host_ring(instances_ring, capacity.records * sizeof(VkAccelerationStructureInstanceKHR),
                   VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
        !host_ring(params_ring, ParamsSlotBytes,
                   VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
        !host_ring(shadow_params_ring, ShadowParamsSlotBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) ||
        !CreateFeatureRings(made) ||
        !host_ring(probes_ring, capacity.records * sizeof(ProbeRecord),
                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
        !host_ring(flags_ring, capacity.records * ProbeFlagWords * sizeof(u32),
                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
        (fx_smoke && !host_ring(smoke_ring, Rt::FxSmokeSlotBytes,
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT))) {
        return false;
    }
    smoke_triangle_base = Rt::FxSmokeTriangleBase(capacity.triangles);
    if (fx_smoke && !smoke_triangle_base) {
        fx_smoke = false;
    }
    crumbs.size = sizeof(Rt::CrumbWords);
    if (!CreateRawBuffer(instance, crumbs.size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, 16,
                         crumbs.buffer, crumbs.allocation,
                         reinterpret_cast<void**>(&crumbs_mapped), crumbs.address)) {
        return false;
    }
    *crumbs_mapped = Rt::CrumbWords{};
    const auto input = Rt::BlasInputFor(capacity.triangles);
    soup.size = input.vertex_bytes;
    fingerprints.size = u64(capacity.records) * Rt::ProbeSamples * 16;
    if (!CreateRawBuffer(instance, soup.size,
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                             VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
                         false, 16, soup.buffer, soup.allocation, nullptr, soup.address) ||
        !CreateRawBuffer(instance, fingerprints.size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false,
                         16, fingerprints.buffer, fingerprints.allocation, nullptr,
                         fingerprints.address)) {
        return false;
    }
    const auto blas_geometry = SoupGeometry(soup.address, input.max_vertex);
    auto blas_sizes = device.getAccelerationStructureBuildSizesKHR(
        vk::AccelerationStructureBuildTypeKHR::eDevice, BlasInfo(&blas_geometry, Rt::BlasBuildFlags(false, compact_enabled, skin_fast_trace)),
        capacity.triangles);
    const auto update_sizes = device.getAccelerationStructureBuildSizesKHR(
        vk::AccelerationStructureBuildTypeKHR::eDevice, BlasInfo(&blas_geometry, Rt::BlasBuildFlags(true, false, skin_fast_trace)),
        capacity.triangles);
    auto basis_info = BlasInfo(&blas_geometry, Rt::BlasBuildFlags(false, compact_enabled, skin_fast_trace));
    basis_info.flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastBuild;
    const u64 pool_basis = device.getAccelerationStructureBuildSizesKHR(
        vk::AccelerationStructureBuildTypeKHR::eDevice, basis_info, capacity.triangles)
                               .accelerationStructureSize;
    blas_sizes.accelerationStructureSize =
        std::max(blas_sizes.accelerationStructureSize, update_sizes.accelerationStructureSize);
    blas_sizes.buildScratchSize =
        std::max({blas_sizes.buildScratchSize, update_sizes.buildScratchSize,
                  update_sizes.updateScratchSize});
    const auto tlas_geometry = InstanceGeometry(instances_ring.address);
    const vk::AccelerationStructureBuildGeometryInfoKHR tlas_info{
        .type = vk::AccelerationStructureTypeKHR::eTopLevel,
        .flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace,
        .mode = vk::BuildAccelerationStructureModeKHR::eBuild,
        .geometryCount = 1,
        .pGeometries = &tlas_geometry,
    };
    const auto tlas_sizes = device.getAccelerationStructureBuildSizesKHR(
        vk::AccelerationStructureBuildTypeKHR::eDevice, tlas_info, capacity.records);
    if (!blas_sizes.accelerationStructureSize || !tlas_sizes.accelerationStructureSize) {
        return false;
    }
    const auto as_props =
        instance.GetPhysicalDevice()
            .getProperties2<vk::PhysicalDeviceProperties2,
                            vk::PhysicalDeviceAccelerationStructurePropertiesKHR>()
            .get<vk::PhysicalDeviceAccelerationStructurePropertiesKHR>();
    scratch_alignment = std::max<u64>(as_props.minAccelerationStructureScratchOffsetAlignment, 256);
    const u64 pool_default = (pool_basis * (compact_enabled ? 3 : 5) / 2 + (1u << 20)) >> 20;
    pool_buffer.size = EnvU64("BB_RT_BLAS_MB", pool_default, 16, 8192) << 20;
    tlas_buffer.size = Rt::PtlasHeaderBytes + tlas_sizes.accelerationStructureSize;
    scratch.size = std::max(blas_sizes.buildScratchSize, tlas_sizes.buildScratchSize) +
                   u64(capacity.records) * scratch_alignment;
    if (!CreateRawBuffer(instance, pool_buffer.size,
                         VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false, 256,
                         pool_buffer.buffer, pool_buffer.allocation, nullptr,
                         pool_buffer.address) ||
        !CreateRawBuffer(instance, tlas_buffer.size,
                         VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         false, 256,
                         tlas_buffer.buffer, tlas_buffer.allocation, nullptr,
                         tlas_buffer.address) ||
        !CreateRawBuffer(instance, scratch.size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false,
                         scratch_alignment, scratch.buffer, scratch.allocation, nullptr,
                         scratch.address)) {
        return false;
    }
    pool.Reset(pool_buffer.size, 256);
    if (omm_enabled) {
        omm_pool_buffer.size = EnvU64("BB_RT_OMM_MB", 64, 4, 2048) << 20;
        omm_input_slot_bytes = Rt::AlignUp(
            Rt::OmmInputBytes(omm_budget, omm_level) + u64(capacity.records) * OmmAlign * 4,
            OmmAlign);
        omm_input.size = omm_input_slot_bytes * Slots;
        if (!CreateRawBuffer(instance, omm_pool_buffer.size,
                             VK_BUFFER_USAGE_MICROMAP_STORAGE_BIT_EXT, false, OmmAlign,
                             omm_pool_buffer.buffer, omm_pool_buffer.allocation, nullptr,
                             omm_pool_buffer.address) ||
            !CreateRawBuffer(instance, omm_input.size,
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                 VK_BUFFER_USAGE_MICROMAP_BUILD_INPUT_READ_ONLY_BIT_EXT |
                                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                             false, OmmAlign, omm_input.buffer, omm_input.allocation, nullptr,
                             omm_input.address)) {
            Log("Ray tracing: opacity micromaps off (buffer creation failed)\n");
            omm_enabled = false;
        } else {
            omm_pool.Reset(omm_pool_buffer.size, OmmAlign);
            omm_handles.resize(capacity.records);
            omm_index_buffer.size = EnvU64("BB_RT_OMM_INDEX_MB", 32, 1, 1024) << 20;
            if (CreateRawBuffer(instance, omm_index_buffer.size,
                                VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                    VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
                                false, OmmAlign, omm_index_buffer.buffer,
                                omm_index_buffer.allocation, nullptr, omm_index_buffer.address)) {
                omm_index_pool.Reset(omm_index_buffer.size, OmmAlign);
            } else {
                omm_index_buffer = {};
            }
            Log("Ray tracing: skinned opacity micromaps %s (BB_RT_OMM_SKINNED, live for new "
                "builds), persistent index pool %llu MiB (BB_RT_OMM_INDEX_MB)\n",
                omm_index_buffer.buffer ? (omm_skinned ? "on" : "off") : "unavailable",
                (unsigned long long)(omm_index_buffer.size >> 20));
        }
    }
    if (compact_enabled) {
        auto qp = device.createQueryPoolUnique(
            {.queryType = vk::QueryType::eAccelerationStructureCompactedSizeKHR,
             .queryCount = capacity.records * Slots});
        if (qp.result == vk::Result::eSuccess) {
            compact_pool = std::move(qp.value);
        } else {
            compact_enabled = false;
        }
    }
    if (time_enabled) {
        const auto& limits = instance.GetPhysicalDevice().getProperties().limits;
        timestamp_period_ns = limits.timestampPeriod;
        auto qp = device.createQueryPoolUnique(
            {.queryType = vk::QueryType::eTimestamp, .queryCount = TimePasses * 2 * Slots});
        if (limits.timestampComputeAndGraphics && qp.result == vk::Result::eSuccess) {
            time_pool = std::move(qp.value);
        } else {
            time_enabled = false;
        }
    }
    entry_handles.resize(capacity.records);
    entry_addresses.assign(capacity.records, 0);
    free_ids.clear();
    for (u32 id = capacity.records; id > 0; --id) {
        free_ids.push_back(id - 1);
    }
    auto tlas_result = device.createAccelerationStructureKHRUnique({
        .buffer = tlas_buffer.buffer,
        .offset = Rt::PtlasHeaderBytes,
        .size = tlas_buffer.size - Rt::PtlasHeaderBytes,
        .type = vk::AccelerationStructureTypeKHR::eTopLevel,
    });
    if (tlas_result.result != vk::Result::eSuccess) {
        return false;
    }
    tlas = std::move(tlas_result.value);
    tlas_address = device.getAccelerationStructureAddressKHR({.accelerationStructure = *tlas});
    khr_tlas_address = tlas_address;
    if (!toggle_avail.ptlas) {
        Log("Ray tracing: partitioned TLAS off (%s; the live switch needs it at launch), KHR TLAS "
            "rebuilt every frame\n",
            instance.PartitionedAccelerationStructureProblem().c_str());
    } else if (!toggles.ptlas) {
        Log("Ray tracing: partitioned TLAS off (BB_RT_PTLAS=1 or the live switch turns it on), "
            "KHR TLAS rebuilt every frame\n");
    }
    if (toggles.ptlas && PreparePtlas()) {
        ptlas_pending = true;
        FlipTlasMode();
    } else {
        toggles.ptlas = ptlas_pending = false;
    }
    return tlas_address != 0;
}

bool RayTracing::CreateFeatureRings(const Rt::LiveToggles& f) {
    const auto host_ring = [&](HostRing& ring, u32 slot_bytes, VkBufferUsageFlags usage) {
        if (ring.buffer) {
            return true;
        }
        ring.slot_bytes = slot_bytes;
        void* mapped = nullptr;
        if (!CreateRawBuffer(instance, u64(slot_bytes) * Slots, usage, true, 16, ring.buffer,
                             ring.allocation, &mapped, ring.address)) {
            return false;
        }
        ring.mapped = static_cast<u8*>(mapped);
        return true;
    };
    if ((f.fog &&
         !host_ring(fog_params_ring, FogParamsSlotBytes,
                    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) ||
        (f.direct &&
         (!host_ring(direct_params_ring, DirectParamsSlotBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) ||
          !host_ring(direct_lights_ring, DirectLightsSlotBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
          !host_ring(direct_sets_ring, Rt::DirectSetBytes,
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT) ||
          !host_ring(sets_readback_ring, Rt::DirectSetBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT))) ||
        (f.direct && !host_ring(pick_ring, Rt::KeptProbeBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) ||
        (f.ao &&
         (!host_ring(ao_params_ring, AoParamsSlotBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) ||
          !host_ring(ao_stats_ring, AoStatsSlotBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT))) ||
        (f.restir_di && !host_ring(restir_remap_ring, Rt::MaxDirectLights * sizeof(u32),
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) ||
        (f.gi &&
         (!host_ring(gi_lights_ring, GiLightsSlotBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
          !host_ring(gi_sets_ring, Rt::DirectSetBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
          !host_ring(gi_fog_ring, FogSetsSlotBytes,
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT) ||
          !host_ring(fill_vis_ring, FillVisSlotBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
          !host_ring(halo_ring, HaloSlotBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
          !host_ring(moon_ring, MoonRingSlotBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT) ||
          !host_ring(exposure_ring, ExposureSlotBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT))) ||
        (f.reflect &&
         !host_ring(reflect_params_ring, ParamsSlotBytes,
                    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) ||
        (f.restir_gi &&
         !host_ring(restir_gi_params_ring, RestirParamsSlotBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT))) {
        return false;
    }
    material.size = material_accum.size = u64(Rt::MaterialSlots(capacity.records)) * 16;
    if (f.gi) {
        for (DeviceBuffer* buffer : {&material, &material_accum}) {
            if (!buffer->buffer &&
                !CreateRawBuffer(instance, buffer->size,
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                 false, 16, buffer->buffer, buffer->allocation, nullptr,
                                 buffer->address)) {
                return false;
            }
        }
    }
    return true;
}

void RayTracing::DestroyResources() {
    const auto allocator = instance.GetAllocator();
    made = Rt::NoFeatures();
    surface_made = false;
    DestroySurfaceResources();
    DestroyReservoirs();
    DestroyMoonHistory();
    cache.Clear([](const Rt::CacheEntry&) {});
    entry_handles.clear();
    entry_addresses.clear();
    retired.clear();
    omm_handles.clear();
    retired_omm.clear();
    retired_omm_index.clear();
    tlas_frame = 0;
    tlas_tick = 0;
    omm_pipeline.reset();
    omm_pipeline_layout.reset();
    omm_layout.reset();
    for (DeviceBuffer* buffer : {&omm_pool_buffer, &omm_input, &omm_index_buffer}) {
        if (buffer->buffer) {
            vmaDestroyBuffer(allocator, buffer->buffer, buffer->allocation);
            buffer->buffer = vk::Buffer{};
        }
    }
    tlas.reset();
    ptlas.Destroy(instance);
    ptlas_on = ptlas_made = ptlas_pending = false;
    tlas_space_written = false;
    tlas_hidden_last = 0;
    sharc_cleared = false;
    sharc_failed = false;
    time_pool.reset();
    compact_pool.reset();
    for (auto& list : slot_compact) {
        list.clear();
    }
    Instance::lost_hook = nullptr;
    crumbs_mapped = nullptr;
    for (DeviceBuffer* buffer : {&soup, &scratch, &pool_buffer, &tlas_buffer, &fingerprints,
                                 &material, &material_accum, &crumbs, &gi_sbt_buffer, &sharc_grid}) {
        if (buffer->buffer) {
            vmaDestroyBuffer(allocator, buffer->buffer, buffer->allocation);
            buffer->buffer = vk::Buffer{};
        }
    }
    for (HostRing* ring : {&records_ring, &indices_ring, &instances_ring, &params_ring, &shadow_params_ring,
                           &probes_ring, &flags_ring, &direct_params_ring, &direct_lights_ring,
                           &direct_sets_ring, &gi_lights_ring, &gi_sets_ring, &gi_fog_ring,
                           &reflect_params_ring, &restir_remap_ring, &restir_gi_params_ring,
                           &ao_params_ring, &ao_stats_ring, &fog_params_ring, &pick_ring,
                           &smoke_ring, &fill_vis_ring, &halo_ring, &moon_ring,
                           &exposure_ring}) {
        if (ring->buffer) {
            vmaDestroyBuffer(allocator, ring->buffer, ring->allocation);
            ring->buffer = vk::Buffer{};
            ring->mapped = nullptr;
        }
    }
    if (sky_io_buffer) {
        vmaDestroyBuffer(allocator, sky_io_buffer, sky_io_allocation);
        sky_io_buffer = vk::Buffer{};
        sky_io_mapped = nullptr;
        sky_io = SkyIo::Idle;
    }
    DestroyAoImages();
    ao_pipeline.reset();
    ao_pipeline_layout.reset();
    ao_layout.reset();
}

bool RayTracing::CreateReservoirs(u32 width, u32 height) {
    DestroyReservoirs();
    const u64 bytes = u64(width) * height * Rt::ReservoirBytes;
    for (DeviceBuffer& buffer : reservoirs) {
        buffer.size = bytes;
        if (!CreateRawBuffer(instance, bytes,
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             false, 16, buffer.buffer, buffer.allocation, nullptr, buffer.address)) {
            DestroyReservoirs();
            return false;
        }
    }
    reservoir_width = width;
    reservoir_height = height;
    reservoir_index = 0;
    Log("Ray tracing: ReSTIR reservoirs %ux%u (%llu MiB)\n", width, height,
        (unsigned long long)((bytes * 3) >> 20));
    return true;
}

void RayTracing::DestroyReservoirs() {
    if (reservoir_width) {
        scheduler.Finish();
    }
    const auto allocator = instance.GetAllocator();
    for (DeviceBuffer& buffer : reservoirs) {
        if (buffer.buffer) {
            vmaDestroyBuffer(allocator, buffer.buffer, buffer.allocation);
            buffer.buffer = vk::Buffer{};
        }
    }
    reservoir_width = reservoir_height = 0;
}

bool RayTracing::CreateMoonHistory(u32 width, u32 height) {
    DestroyMoonHistory();
    const u64 bytes = u64(width) * height * 8u;
    for (DeviceBuffer& buffer : moon_hist) {
        buffer.size = bytes;
        if (!CreateRawBuffer(instance, bytes,
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             false, 16, buffer.buffer, buffer.allocation, nullptr, buffer.address)) {
            DestroyMoonHistory();
            return false;
        }
    }
    moon_hist_width = width;
    moon_hist_height = height;
    moon_hist_index = 0;
    moon_hist_frame = ~0ull;
    Log("Ray tracing: moon visibility history %ux%u (%llu MiB)\n", width, height,
        (unsigned long long)((bytes * 2) >> 20));
    return true;
}

void RayTracing::DestroyMoonHistory() {
    if (moon_hist_width) {
        scheduler.Finish();
    }
    const auto allocator = instance.GetAllocator();
    for (DeviceBuffer& buffer : moon_hist) {
        if (buffer.buffer) {
            vmaDestroyBuffer(allocator, buffer.buffer, buffer.allocation);
            buffer.buffer = vk::Buffer{};
        }
    }
    moon_hist_width = moon_hist_height = 0;
}

void RayTracing::ReadLiveKnobs() {
    reflect_max_rough = EnvFloat("BB_RT_REFLECT_MAX_ROUGH", Rt::ReflectMaxRoughDefault, 0.0f, 1.0f);
    direct_debug = u32(EnvU64("BB_RT_DIRECT_DEBUG", 0, 0, 11));
    // Live: instance opacity is set every frame, so cut-outs of skinned draws can be A/B'd.
    skin_alpha = EnvU64("BB_RT_SKIN_ALPHA", 1, 0, 1) != 0;
    direct_light_log = EnvFloat("BB_RT_DIRECT_LIGHT_LOG", 0.0f, 0.0f, 1000.0f);
    light_gate = EnvU64("BB_RT_LIGHT_GATE", 1, 0, 1) == 1;
    light_depth = EnvU64("BB_RT_LIGHT_DEPTH", 1, 0, 1) == 1;
    {
        const char* fill = Rt::Knob("BB_RT_FILL_LIGHT");
        fill_light_off = !fill || std::string_view(fill) != "on";
    }
    ao_rays = u32(EnvU64("BB_RT_AO_RAYS", 2, 1, 16));
    ao_radius = EnvFloat("BB_RT_AO_RADIUS", 0.5f, 0.001f, 1e5f);
    ao_falloff = EnvU64("BB_RT_AO_FALLOFF", 1, 0, 1) != 0;
    ao_occlude_y = EnvU64("BB_RT_AO_Y", 1, 0, 1) != 0;
    gi_tmax_m = u32(EnvU64("BB_RT_GI_TMAX", 0, 0, Rt::GiTmaxMax));
    gi_sharc = EnvU64("BB_RT_SHARC", 0, 0, 1) != 0;
    gi_last_bounce_cache = EnvU64("BB_RT_GI_LAST_BOUNCE_CACHE", 1, 0, 1) != 0;
    gi_bounces = u32(EnvU64("BB_RT_BOUNCES", 2, 0, Rt::MaxBounces));
    adaptive_ms = EnvFloat("BB_RT_ADAPTIVE_MS", 12.0f, 0.0f, float(Rt::AdaptiveBudgetMaxMs));
    far_start = EnvFloat("BB_RT_FAR_START", 0.0f, 0.0f, Rt::FarFadeMaxM);
    far_band = EnvFloat("BB_RT_FAR_BAND", Rt::FarBandDefaultM, 0.0f, Rt::FarFadeMaxM);
    {
        const bool was = omm_skinned;
        omm_skinned = EnvU64("BB_RT_OMM_SKINNED", 1, 0, 1) != 0;
        if (was != omm_skinned && omm_index_buffer.buffer) {
            Log("Ray tracing live: BB_RT_OMM_SKINNED %u -> %u (next rebuild of each swaying "
                "BLAS)\n",
                was ? 1u : 0u, omm_skinned ? 1u : 0u);
        }
    }
    ao_debug = u32(EnvU64("BB_RT_AO_DEBUG", 0, 0, 1));
    const char* novis_env = Rt::Knob("BB_RT_DIRECT_NOVIS");
    direct_novis = novis_env && novis_env[0] == '1';
    const char* moon_env = Rt::Knob("BB_RT_MOON");
    moon_mask = moon_env && std::strcmp(moon_env, "mask") == 0;
    moon_cone_cos = Rt::MoonConeCos(float(EnvU64("BB_RT_MOON_ANGLE", 15, 0, 100)) * 0.1f);
    // Lowest moonlight elevation in degrees (rt_moon_dir.comp; 0 = the sky moon as drawn).
    moon_min_elevation = EnvFloat("BB_RT_MOON_MIN_EL", 0.0f, 0.0f, 80.0f);
    // 1: the white spot of the traced moon layer mesh also counts as a moon find. Off: only the
    // moon drawn on screen (rt_sky.comp FindScreenMoon); the late-game layer spot sat 30 degrees
    // from the drawn disc.
    moon_layer_find = EnvU64("BB_RT_MOON_LAYER_FIND", 0, 0, 1) != 0;
    {
        const char* seed = Rt::Knob("BB_RT_MOON_SEED");
        moon_static = !seed || std::strcmp(seed, "frame") != 0;
        moon_world = EnvU64("BB_RT_MOON_WORLD", 1, 0, 1) == 1;
    }
    moon_half = u32(EnvU64("BB_RT_MOON_HALF", 2, 0, 2));
    moon_history = EnvU64("BB_RT_MOON_HISTORY", 1, 0, 1) != 0;
    moon_object_motion = EnvU64("BB_RT_MOON_OBJECT_MOTION", 1, 0, 1) != 0;
    restir_gi_jacobian = EnvFloat("BB_RT_RESTIR_GI_JACOBIAN", Rt::RestirGiJacobianDefault, 0.0f, 1000.0f);
    dir_log = EnvU64("BB_RT_DIR_LOG", 0, 0, 1) == 1;
    dir_dump = EnvU64("BB_RT_DIR_DUMP", 0, 0, 1) == 1;
    gi_world_set = !Rt::KnobIs("BB_RT_GI_WORLD_SET", '0');
    moon_from_sky = !Rt::KnobIs("BB_RT_MOON_FROM_SKY", '0');
    moon_min = EnvFloat("BB_RT_MOON_MIN", 0.2f, 0.0f, 100.0f);
    {
        const bool was = exposure_on;
        exposure_on = !Rt::KnobIs("BB_RT_EXPOSURE", '0');
        exposure_reset = exposure_reset || was != exposure_on;
    }
    exposure_key = EnvFloat("BB_RT_EXPOSURE_KEY", ExposureKeyDefault, -20.0f, 20.0f);
    exposure_max = EnvFloat("BB_RT_EXPOSURE_MAX", 1.5f, 0.0f, 6.0f);
    exposure_rate = EnvFloat("BB_RT_EXPOSURE_RATE", 0.03f, 0.001f, 1.0f);
    exposure_white = EnvFloat("BB_RT_EXPOSURE_WHITE", 3.0f, 0.5f, 12.0f);
    thin_transmit = EnvFloat("BB_RT_THIN_TRANSMIT", 0.4f, 0.0f, 1.0f);
    const char* depth_draws_env = Rt::Knob("BB_RT_DEPTH_DRAWS");
    depth_only_draws = depth_draws_env && depth_draws_env[0] == '1';
    pick_enabled = Rt::ParsePick(Rt::Knob("BB_RT_PICK"), pick_at);
    pick_every = u32(EnvU64("BB_RT_PICK_EVERY", 60, 1, 600));
    miss_log = Rt::KnobIs("BB_RT_MISS_LOG", '1');
    gi_fog = !Rt::KnobIs("BB_RT_GI_FOG", '0');
    gi_at_composite = !Rt::KnobIs("BB_RT_GI_AT_COMPOSITE", '0');
    gi_denoise = u32(EnvU64("BB_RT_GI_DENOISE", 2, 0, 8));
    const char* unresolved_env = Rt::Knob("BB_RT_ALPHA_UNRESOLVED");
    alpha_skip_unresolved = !unresolved_env || std::strcmp(unresolved_env, "solid") != 0;
    fx_smoke_opacity = EnvFloat("BB_RT_FX_SMOKE_OPACITY", 0.6f, 0.0f, 4.0f);
    fx_debug = u32(EnvU64("BB_RT_FX_DEBUG", 0, 0, 2));
    fog_shaft = EnvFloat("BB_RT_FX_FOG_SHAFT", 0.15f, 0.0f, 10.0f);
    fog_height = EnvFloat("BB_RT_FX_FOG_HEIGHT", 0.1f, 0.0f, 10.0f);
    fog_strength = EnvFloat("BB_RT_FX_FOG_STRENGTH", 1.0f, 0.0f, 100.0f);
    fog_distance = EnvFloat("BB_RT_FX_FOG_DIST", 60.0f, 1.0f, 10000.0f);
    fog_g = EnvFloat("BB_RT_FX_FOG_G", 0.0f, -0.99f, 0.99f);
    fog_sun_cal = EnvU64("BB_RT_FX_FOG_SUN_CAL", 1, 0, 1) != 0;
    fog_sun_share = EnvFloat("BB_RT_FX_FOG_SUN_SHARE", 0.5f, 0.0f, 1.0f);
    fog_steps = u32(EnvU64("BB_RT_FX_FOG_STEPS", 4, 1, 128));
    fog_alpha_distance = EnvFloat("BB_RT_FX_FOG_ALPHA_DIST", 5.0f, 0.0f, 10000.0f);
    fog_debug = u32(EnvU64("BB_RT_FX_FOG_DEBUG", 0, 0, 6));
    fog_stock = Rt::KnobIs("BB_RT_FOG_STOCK", '1');
    sky_debug = u32(EnvU64("BB_RT_SKY_DEBUG", 0, 0, 4));
    shelf_metres = EnvFloat("BB_RT_SHELF", 1.0f, 0.0f, 10000.0f);
    // Learn the screen's sky into the 512x512 sky image the mirrors read (BB_RT_SKY_IMAGE).
    sky_image = EnvFloat("BB_RT_SKY_IMAGE", 1.0f, 0.0f, 1.0f) > 0.5f;
    sky_u_offset = EnvFloat("BB_RT_SKY_U0", 0.313f, -4.0f, 4.0f);
    sky_u_repeat = EnvFloat("BB_RT_SKY_UREP", 1.0f, -8.0f, 8.0f);
    sky_v_divisor = EnvFloat("BB_RT_SKY_VDIV", 2048.0f, 64.0f, 65536.0f);
    fx_sheets = Rt::KnobIs("BB_RT_FX_SHEETS", '1');
    skip_shadows = !Rt::KnobIs("BB_RT_SKIP_SHADOWS", '0');
    // Fill-light visibility: designer point/spot lights keep their strength only where traced
    // rays from them reach the sky (window, door, opening) or a real emitter (lamp mesh).
    fill_vis_on = !Rt::KnobIs("BB_RT_FILL_VIS", '0');
    fill_vis_rays = u32(EnvU64("BB_RT_FILL_RAYS", 256, 16, 4096));
    fill_vis_sky_full = EnvFloat("BB_RT_FILL_SKY", 0.12f, 0.0f, 1.0f);
    // BB_RT_FILL_SKY_KEEP=1 keeps fill lights that see the sky (old rule: they stood in for
    // moonlight). Default off: the moon is traced, so such a light is fake light too.
    fill_vis_sky_keep = Rt::KnobIs("BB_RT_FILL_SKY_KEEP", '1');
    fill_vis_floor = EnvFloat("BB_RT_FILL_FLOOR", 0.0f, 0.0f, 1.0f);
    fill_vis_offscreen = EnvU64("BB_RT_FILL_OFFSCREEN", 1, 0, 1) != 0;
    late_mode = u32(EnvU64("BB_RT_LATE_DRAWS", 1, 0, 2));
    fill_vis_emit_dist = EnvFloat("BB_RT_FILL_EMIT_DIST", 0.6f, 0.0f, 10.0f);
    fill_vis_emit_luma = EnvFloat("BB_RT_FILL_EMIT_LUMA", 0.02f, 0.0f, 100.0f);
    fill_vis_enclosed = EnvFloat("BB_RT_FILL_ENCLOSED", 0.1f, 0.0f, 1.01f);
    fill_vis_refresh = u32(EnvU64("BB_RT_FILL_REFRESH", 120, 0, 1000000));
    // A fill light within this distance of a found emitter is that fixture's own light (the
    // game's chandelier glow light next to its candles): it counts as an emitter. 0 = off.
    fill_vis_cluster = EnvFloat("BB_RT_FILL_CLUSTER", 2.0f, 0.0f, 10.0f);
    // A light whose colour flickers is a flame (candles, torches): the game animates fire
    // lights and keeps designer fills constant (hot74-76: every fill relative sd 0.000, flame
    // lamps 0.04-0.14). Steps above this fraction that reverse direction FillFlickerTurns times
    // latch the light as an emitter, so open candelabras (enclosed 6%, flame sprites not in
    // the emissive target) keep their light. 0 = off.
    fill_vis_flicker = EnvFloat("BB_RT_FILL_FLICKER", 0.01f, 0.0f, 1.0f);
    fill_vis_warm = !Rt::KnobIs("BB_RT_FILL_WARM", '0');
    // Lamp hue for the warm rule: blue >= this x red (Rt::WarmLampBlue), unless fire-bright.
    // 0 = any warm light (the old rule).
    fill_vis_warm_hue = EnvFloat("BB_RT_FILL_WARM_HUE", Rt::WarmLampBlue, 0.0f, 0.6f);
    // Emitters found by the fill pass get inverse-square falloff out to radius x this (<= 1 off).
    // One cut for every emitter: the light ends where colour x s2 / d2 falls to 0.025 x colour,
    // i.e. reach = radius x source_frac / sqrt(0.025): 1.26 for BB_RT_EMITTER_SOURCE 0.2 and 3.8
    // for BB_RT_LAMP_SOURCE 0.6 (was 2.5 / 6: a hand torch lit 21 m, stock 8.5 m).
    emitter_range = EnvFloat("BB_RT_EMITTER_RANGE", 1.26f, 0.0f, 16.0f);
    // A brighter fire reaches further: inverse-square light falls to the same cut-off level at a
    // distance growing with sqrt(intensity), so an emitter brighter than this game colour (max
    // channel; lanterns and torches are 2.0) gets its reach x sqrt(colour / this). The burning
    // house's fires (12.8) stopped at 9.5 m like a lantern and left the garden moon-blue.
    // 0 = off. Lamps and candles keep their own reach.
    emitter_bright = EnvFloat("BB_RT_EMITTER_BRIGHT", 2.0f, 0.0f, 100.0f);
    // A big fire flickers harder: the steady-flame flicker of such an emitter is
    // BB_RT_EMITTER_STEADY_FLICKER x grow^this (grow = sqrt(colour / BB_RT_EMITTER_BRIGHT)).
    // The burning house's lights are constant in the game; at x1 their light on the garden
    // varied 3% (sd) against stock's 10%. 0 = same as a candle.
    emitter_big_flicker = EnvFloat("BB_RT_EMITTER_BIG_FLICKER", 0.5f, 0.0f, 4.0f);
    // ... plus a faster octave (the noise at 3x the rate: ~0.75-5 Hz) of this weight, so the light
    // dances with the flames drawn over it instead of only swelling. Big fires only; 0 = off.
    emitter_big_fast = EnvFloat("BB_RT_EMITTER_BIG_FAST", 0.5f, 0.0f, 4.0f);
    // Brightness of a big fire's light (grow > 1) x this; the user asked for the burning house a
    // little brighter (2026-11-09). 1 = the game's colour.
    emitter_big_gain = EnvFloat("BB_RT_EMITTER_BIG_GAIN", 1.5f, 0.0f, 8.0f);
    // Source size of a converted emitter as a fraction of the game's radius, and how far (m, world
    // +y) the light moves up from the game's anchor in the holder towards the flame.
    emitter_source = EnvFloat("BB_RT_EMITTER_SOURCE", 0.2f, 0.01f, 1.0f);
    emitter_lift = EnvFloat("BB_RT_EMITTER_LIFT", 0.0f, -1.0f, 1.0f);
    // Brightness at the fixture (1 = the game's peak); the far field is kept.
    emitter_peak = EnvFloat("BB_RT_EMITTER_PEAK", 0.4f, 0.01f, 1.0f);
    // Physical flame size (m) of every converted emitter; the far field is kept (0 = off).
    emitter_flame = EnvFloat("BB_RT_EMITTER_FLAME", 1.0f, 0.0f, 4.0f);
    // Gain on the game's own flame flicker around its running mean (1 = as the game, 0 = steady; 2.5: the
    // GI bounce is accumulated and flattens the game's ~9% swing on screen to ~3.6%).
    emitter_flicker = EnvFloat("BB_RT_EMITTER_FLICKER", 2.5f, 0.0f, 8.0f);
    // Per-frame weight of that running mean (0.02: about a second, slower than the flicker).
    emitter_flicker_avg = EnvFloat("BB_RT_EMITTER_FLICKER_AVG", 0.02f, 0.001f, 1.0f);
    // Per-frame weight that smooths the game's flicker before the gain (1 = raw: it changes every
    // frame; 0.1: ~10 frames, a flame's pace instead of a strobe).
    emitter_flicker_smooth = EnvFloat("BB_RT_EMITTER_FLICKER_SMOOTH", 0.1f, 0.01f, 1.0f);
    // Flame sway (m): carried flames wander by up to this (Rt::FlameSway), so the lit
    // pattern and shadows move like under a real flame instead of only dimming in step.
    emitter_sway = EnvFloat("BB_RT_EMITTER_SWAY", 0.15f, 0.0f, 0.5f);
    // Flicker of our own for flames the game keeps steady: x exp(this x Rt::FlameNoise), so 0.35
    // swings between x0.70 and x1.42 (at 0.9 linear the dips read as the light going out).
    emitter_steady_flicker = EnvFloat("BB_RT_EMITTER_STEADY_FLICKER", 0.35f, 0.0f, 1.0f);
    // Below this mean |colour / running mean - 1| the game's own flicker counts as steady and the
    // flame gets the flicker above (the fireplace swell measures 0.015; game-flickered flames 0.06+).
    emitter_steady_dev = EnvFloat("BB_RT_EMITTER_STEADY_DEV", 0.05f, 0.0f, 1.0f);
    // Sway (m) of fixed flames that are not lamps (candles, candelabra, fireplace).
    fixed_sway = EnvFloat("BB_RT_FIXED_SWAY", 0.015f, 0.0f, 0.5f);
    // Largest game radius (m) of a fixed light that sways as a candle (candles 2 m, wall lamps 5).
    candle_rmax = EnvFloat("BB_RT_CANDLE_RMAX", 3.0f, 0.0f, 100.0f);
    // Source size and reach of fixed candles (game r up to BB_RT_CANDLE_RMAX, not lamps, not carried):
    // at the emitter's 0.2 / 1.26 a candle lit its own statue at 2/3 of stock; 0.8 / 5 matches it
    // (statue crop luma 9.8 -> 15.5, stock 14.3) but four r 2 candles reaching 10 m cost direct
    // 3.2 -> 7.5 ms (35 FPS); 0.85 / 2.5 lights the statues the same for 3.8 ms (41 FPS).
    // Torches and wall lamps keep the emitter values.
    candle_source = EnvFloat("BB_RT_CANDLE_SOURCE", 0.85f, 0.01f, 1.0f);
    candle_range = EnvFloat("BB_RT_CANDLE_RANGE", 2.5f, 0.0f, 16.0f);
    // Lamp halo: fixed flames light the air around them (Rt::BuildHalos, rt_gi_apply.comp), with
    // the light's own flickered colour. Strength (0 = off), Gaussian width (m), hot core size (m).
    // The halo scales with the light's power and the lamp glass is drawn over it afterwards, so
    // a halo brighter than the glass turns the panes black. 0.03 was set when post lamps had ~1/12
    // of their power now (glow lamps, candles); 0.005 keeps the glass glowing.
    halo_strength = EnvFloat("BB_RT_HALO", 0.005f, 0.0f, 10.0f);
    halo_radius = EnvFloat("BB_RT_HALO_RADIUS", 0.12f, 0.05f, 10.0f);
    halo_core = EnvFloat("BB_RT_HALO_CORE", 0.1f, 0.01f, 2.0f);
    halo_match = EnvFloat("BB_RT_HALO_MATCH", 2.0f, 0.0f, 10.0f);
    // Lamps: static emitters whose game radius is in [BB_RT_LAMP_RMIN, BB_RT_LAMP_RMAX] m and
    // whose fixture encloses at least BB_RT_LAMP_ENCLOSED of the rays around the light (glass
    // housing: wall lamps measure 60-68%; candelabra candles, also r 2 m, measure ~10%) get their
    // own source size and reach, so their light carries across a room and through a glass door
    // without making candles reach further. Candles, the torch and small lights stay on the
    // BB_RT_EMITTER_* values. Same peak at the fixture; the
    // far field grows with BB_RT_LAMP_SOURCE squared. 0.6 / 6 tuned by eye at the glass door (hot103).
    lamp_source = EnvFloat("BB_RT_LAMP_SOURCE", 0.6f, 0.01f, 1.0f);
    lamp_range = EnvFloat("BB_RT_LAMP_RANGE", 3.8f, 0.0f, 16.0f);
    lamp_rmin = EnvFloat("BB_RT_LAMP_RMIN", 1.0f, 0.0f, 100.0f);
    lamp_rmax = EnvFloat("BB_RT_LAMP_RMAX", 6.0f, 0.0f, 100.0f);
    lamp_enclosed = EnvFloat("BB_RT_LAMP_ENCLOSED", 0.4f, 0.0f, 1.0f);
    // ... and capped: this fraction of the upward rays (world y > 0.5) also hits (fire pits 24-29%).
    lamp_up = EnvFloat("BB_RT_LAMP_UP", 0.5f, 0.0f, 1.0f);
    // Fire pits (enclosed, open above, fixed): the light moves up this far (m, world +y) out of
    // the log pile so its shadow rays clear the sticks. Forest, ground at the fires, mean luma:
    // stock 28.4, lift 0 22.3, 0.3 25.5, 0.5 27.8, 0.7 27.3 (1.0 over-lit the ground next to a
    // fire pit, play182).
    fire_lift = EnvFloat("BB_RT_FIRE_LIFT", 0.5f, 0.0f, 3.0f);
    ambient_cut = EnvFloat("BB_RT_AMBIENT_CUT", 1.0f, 0.0f, 1.0f);
    ambient_spec_cut = EnvFloat("BB_RT_AMBIENT_SPEC_CUT", 1.0f, 0.0f, 1.0f);
    ambient_keep_lo = EnvFloat("BB_RT_AMBIENT_KEEP_LO", 0.1f, 0.0f, 1.0f);
    ambient_keep_hi = EnvFloat("BB_RT_AMBIENT_KEEP_HI", 0.3f, 0.0f, 1.0f);
    ambient_keep_lit = EnvFloat("BB_RT_AMBIENT_KEEP_LIT", 0.25f, 0.0f, 100.0f);
    {
        const char* cool = Rt::Knob("BB_RT_AMBIENT_COOL");
        float r = 0.39f, g = 0.68f, b = 1.0f;
        if (cool && std::sscanf(cool, "%f,%f,%f", &r, &g, &b) == 3 && r > 0.0f && g > 0.0f &&
            b > 0.0f) {
            ambient_cool = {r, g, b};
        } else {
            ambient_cool = {0.39f, 0.68f, 1.0f};
        }
    }
    skip_volumes = !Rt::KnobIs("BB_RT_SKIP_VOLUMES", '0');
    {
        const char* list = Rt::Knob("BB_RT_SKY_PS");
        sky_ps = Rt::ParseHashList(list ? list : "dda49e89,765450df");
    }
    // See-through glass, in the reflection pass: G-buffer1.x > BB_RT_GLASS_GLOSS on an instance
    // drawn by a BB_RT_GLASS_PS pixel shader. The cracked door panes are ps 8aba0ecd (found with
    // the BB_RT_PICK camera ray); 9abe2f09 is the door frame and walls, not glass.
    // 1c3953c6 is the alpha-tested cracked-pane layer of the same doors (hot100 pick): from the
    // dark side it is the surface in the G-buffer, so without it the panes were never glass.
    glass_on = !Rt::KnobIs("BB_RT_GLASS", '0');
    {
        const char* list = Rt::Knob("BB_RT_GLASS_PS");
        // 8aba0ecd is NOT glass-only: it is a general building shader (ceilings, walls, 62 to
        // 59000-triangle meshes, hot105 picks); listed, the vaulted ceilings let the sky through
        // and looked like glass. The cracked door panes are 1c3953c6 (28-48 triangles).
        // ca7226ec: the workshop wardrobe and desk-cabinet door panes (85-triangle mesh, front and
        // back faces 2 cm apart, gloss 0.75; 2026-11-09 pick). With BB_RT_GLASS_DEBUG=1 only those
        // panes were accepted in the workshop.
        // "hash:cap" gives one shader its own triangle cap. 1c3953c6 also draws the 110-triangle
        // cloth on the workshop altar (gloss 0.88, 2026-11-09 pick): the cracked door panes are
        // 28-48 triangles, so it is capped at 64.
        glass_ps.clear();
        glass_caps.clear();
        for (const char* t = list ? list : "1c3953c6:64,ca7226ec"; t && *t;) {
            char* end = nullptr;
            const u64 hash = std::strtoull(t, &end, 16);
            if (end == t) {
                ++t;
                continue;
            }
            t = end;
            u32 cap = 0;
            if (*t == ':') {
                cap = u32(std::strtoul(t + 1, &end, 10));
                t = end;
            }
            if (hash) {
                glass_ps.push_back(hash);
                glass_caps.push_back(cap);
            }
        }
    }
    // BB_RT_GLASS_PASS=0: glass instances stay opaque (rays stop at panes again).
    glass_pass = !Rt::KnobIs("BB_RT_GLASS_PASS", '0');
    glass_mask_dump = Rt::KnobIs("BB_RT_GLASS_MASK_DUMP", '1');
    glass_mask_by_uid.clear();
    glass_mask_by_hash.clear(); // look at the mask files again (GlassMaskLoad)
    glass_gloss = EnvFloat("BB_RT_GLASS_GLOSS", 0.55f, 0.0f, 1.0f);
    water_on = !Rt::KnobIs("BB_RT_WATER", '0');
    water_layer = Rt::KnobIs("BB_RT_WATER_LAYER", '1');
    water_mark_layer = !Rt::KnobIs("BB_RT_WATER_MARK", '0');
    {
        const char* list = Rt::Knob("BB_RT_WATER_PS");
        water_ps = Rt::ParseHashList(list ? list : "e54449c7");
    }
    water_rough = EnvFloat("BB_RT_WATER_ROUGH", 0.05f, 0.0f, 1.0f);
    // G-buffer puddles take real water's F0: at 0.5 (chosen for the dark sewer) a puddle seen from
    // above mirrored half the bright sky and read as a white blob (Hunter's Dream path).
    water_f0 = EnvFloat("BB_RT_WATER_F0", 0.02f, 0.0f, 1.0f);
    // A puddle fades in over this much G-buffer gloss above 0.6 instead of a hard cut. 0 = hard.
    water_puddle_edge = EnvFloat("BB_RT_PUDDLE_EDGE", 0.25f, 0.0f, 0.4f);
    // The ground under a puddle keeps this share of its dry light (wet soil is darker). 1 = off.
    water_puddle_wet = EnvFloat("BB_RT_PUDDLE_WET", 0.6f, 0.0f, 1.0f);
    water_sheet_depth = EnvFloat("BB_RT_WATER_SHEET_DEPTH", 0.03f, -1.0f, 10.0f);
    water_murk = EnvFloat("BB_RT_WATER_MURK", 5.0f, 0.0f, 100.0f);
    water_raise = EnvFloat("BB_RT_WATER_RAISE", 0.1f, 0.0f, 2.0f);
    water_body = EnvFloat("BB_RT_WATER_BODY", 1.0f, 0.0f, 100.0f);
    water_sheen = EnvFloat("BB_RT_WATER_SHEEN", 0.0f, 0.0f, 100.0f);
    water_sheen_far = EnvFloat("BB_RT_WATER_SHEEN_FAR", 1.0f, 0.0f, 100.0f);
    water_ripple = EnvFloat("BB_RT_WATER_RIPPLE", 0.1f, 0.0f, 2.0f);
    water_sheet_f0 = EnvFloat("BB_RT_WATER_SHEET_F0", 0.5f, 0.0f, 1.0f);
    water_mist = EnvFloat("BB_RT_MIST", 0.0f, 0.0f, 100.0f);
    stock_haze = water_mist <= 0.0f && !Rt::KnobIs("BB_RT_FX_SHEETS", '0');
    haze_lit = !Rt::KnobIs("BB_RT_HAZE_LIT", '0');
    haze_floor = EnvFloat("BB_RT_HAZE_FLOOR", 0.3f, 0.0f, 1.0f);
    haze_gain = EnvFloat("BB_RT_HAZE_GAIN", 1.0f, 0.0f, 1000.0f);
    haze_tint = EnvFloat("BB_RT_HAZE_TINT", 0.5f, 0.0f, 1.0f);
    water_mist_height = EnvFloat("BB_RT_MIST_HEIGHT", 5.0f, 0.0f, 20.0f);
    water_mist_albedo = EnvFloat("BB_RT_MIST_ALBEDO", 1.0f, 0.0f, 100.0f);
    water_mist_steps = u32(EnvU64("BB_RT_MIST_STEPS", 8, 1, 32));
    water_mist_patch = EnvFloat("BB_RT_MIST_PATCH", 0.5f, 0.0f, 0.9f);
    water_mist_glow = EnvFloat("BB_RT_MIST_GLOW", 0.25f, 0.0f, 100.0f);
    water_mist_glow_dark = EnvFloat("BB_RT_MIST_GLOW_DARK", 0.6f, 0.0f, 1.0f);
    mist_sheets = !Rt::KnobIs("BB_RT_MIST_SHEETS", '0');
    water_mist_fog = EnvFloat("BB_RT_MIST_FOG", 5.0f, 0.0f, 1000.0f);
    mist_sheet_scale = EnvFloat("BB_RT_MIST_SHEET_SCALE", 0.5f, 0.05f, 100.0f);
    water_direct_keep = EnvU64("BB_RT_WATER_DIRECT_KEEP", 1, 0, 1) != 0;
    water_scatter = EnvFloat("BB_RT_WATER_SCATTER", 0.04f, 0.0f, 10.0f);
    water_debug = u32(EnvU64("BB_RT_WATER_DEBUG", 0, 0, 1));
    water_at_floor = Rt::KnobIs("BB_RT_WATER_AT_FLOOR", '1');
    glass_max_tris = u32(EnvU64("BB_RT_GLASS_MAX_TRIS", 2000, 0, 100000000));
    // 2390c887: the lit window panes of the Hunter's Dream workshop (hot113 pick, BLAS of 108
    // triangles a few cm in front of the wall; BB_RT_SKIP_PS=2390c887 hides both windows only).
    portal_on = !Rt::KnobIs("BB_RT_PORTAL", '0');
    {
        const char* list = Rt::Knob("BB_RT_PORTAL_PS");
        portal_ps = Rt::ParseHashList(list ? list : "2390c887");
    }
    {
        const char* list = Rt::Knob("BB_RT_FLAME_PS");
        flame_ps = Rt::ParseHashList(list ? list : "d729c469");
    }
    if (Rt::KnobIs("BB_RT_GLOW", '0')) {
        glow_ps.clear();
    } else {
        const char* list = Rt::Knob("BB_RT_GLOW_PS");
        glow_ps = Rt::ParseHashList(list ? list : "5e224cb6");
    }
    glow_strength = EnvFloat("BB_RT_GLOW_STRENGTH", 1.2f, 0.0f, 4.0f);
    // Post lamps have no game light; a glow lamp stands in. Radius 5, strength 1.2 and a 1 m flame
    // give it about twice the power (colour x source^2) of the game's own r 5.5 wall lamps: at
    // radius 2 / strength 0.6 it had ~1/12 of that and the street read as lit by ambient.
    glow_radius = EnvFloat("BB_RT_GLOW_RADIUS", 5.0f, 0.1f, 20.0f);
    // Flame size (m) of a glow lamp, as BB_RT_EMITTER_FLAME (0 = off: the source keeps its 4.7 m
    // BB_RT_LAMP_SOURCE size, which flattens the near field: 1/3 of the light at 3 m).
    glow_flame = EnvFloat("BB_RT_GLOW_FLAME", 1.0f, 0.0f, 4.0f);
    // Reach of a glow lamp as a multiple of BB_RT_GLOW_RADIUS (like BB_RT_LAMP_RANGE): 9 m looks
    // the same as 19 m on the Cathedral bridge and costs less (direct 2.55 -> 2.07 ms; old 1.39).
    glow_range = EnvFloat("BB_RT_GLOW_RANGE", 1.8f, 0.0f, 16.0f);
    glow_skip = EnvFloat("BB_RT_GLOW_SKIP", 1.5f, 0.0f, 10.0f);
    glow_dist = EnvFloat("BB_RT_GLOW_DIST", 40.0f, 1.0f, 500.0f);
    glow_merge = EnvFloat("BB_RT_GLOW_MERGE", 2.5f, 0.0f, 20.0f);
    glow_max_lights = u32(EnvU64("BB_RT_GLOW_MAX", 8, 0, 256));
    glow_spec_power = EnvFloat("BB_RT_GLOW_SPEC_POWER", 16.0f, 1.0f, 256.0f);
    glow_keep = EnvU64("BB_RT_GLOW_KEEP", 1800, 1, 100000000);
    glow_min_hits = u32(EnvU64("BB_RT_GLOW_MIN_HITS", 30, 0, 100000));
    glow_max_drift = EnvFloat("BB_RT_GLOW_MAX_DRIFT", 0.1f, 0.0f, 10.0f);
    // Mirror rays that miss read the sky dome image (1) or the 32x32 sky probe (0).
    reflect_sky_dome = EnvFloat("BB_RT_REFLECT_SKY_DOME", 1.0f, 0.0f, 1.0f) > 0.5f;
    glow_warm_max = EnvFloat("BB_RT_GLOW_WARM_MAX", 0.8f, 0.0f, 10.0f);
    {
        const char* tint = Rt::Knob("BB_RT_GLOW_TINT");
        float r = 1.0f, g = 0.8f, b = 0.55f;
        if (tint && std::sscanf(tint, "%f,%f,%f", &r, &g, &b) == 3 && r >= 0.0f && g >= 0.0f &&
            b >= 0.0f) {
            glow_tint = {r, g, b};
        } else {
            glow_tint = {1.0f, 0.8f, 0.55f};
        }
    }
    {
        const char* hue = Rt::Knob("BB_RT_LAMP_HUE");
        float r = 1.0f, g = 0.61f, b = 0.27f;
        lamp_hue_on = !(hue && hue[0] == '0' && hue[1] == '\0');
        if (hue && std::sscanf(hue, "%f,%f,%f", &r, &g, &b) == 3 && r >= 0.0f && g >= 0.0f &&
            b >= 0.0f) {
            lamp_hue = {r, g, b};
        } else {
            lamp_hue = {1.0f, 0.61f, 0.27f};
        }
    }
    lamp_warm_max = EnvFloat("BB_RT_LAMP_WARM", 0.6f, 0.0f, 1.0f);
    glass_tint = EnvFloat("BB_RT_GLASS_TINT", 1.0f, 0.0f, 2.0f);
    glass_debug = u32(EnvU64("BB_RT_GLASS_DEBUG", 0, 0, 3));
    skip_ps.clear();
    if (const char* list = Rt::Knob("BB_RT_SKIP_PS")) {
        for (std::string_view rest{list}; !rest.empty();) {
            const size_t comma = rest.find(',');
            std::string_view item = rest.substr(0, comma);
            rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
            while (!item.empty() && item.front() == ' ') {
                item.remove_prefix(1);
            }
            while (!item.empty() && item.back() == ' ') {
                item.remove_suffix(1);
            }
            if (item.size() > 2 && item[0] == '0' && (item[1] == 'x' || item[1] == 'X')) {
                item.remove_prefix(2);
            }
            u64 value = 0;
            const auto parsed = std::from_chars(item.data(), item.data() + item.size(), value, 16);
            if (!item.empty() && parsed.ec == std::errc{} && parsed.ptr == item.data() + item.size()) {
                skip_ps.push_back(value);
            }
        }
    }
    direct_base = Rt::KnobOn("BB_RT_DIRECT_BASE");
    game_ao = Rt::KnobIs("BB_RT_GAME_AO", '1');
    direct_rays = u32(EnvU64("BB_RT_DIRECT_RAYS", 32, 0, Rt::MaxDirectRays));
    direct_keep = u32(EnvU64("BB_RT_DIRECT_KEEP", 2, 0, 60));
    direct_keep_offscreen = u32(EnvU64("BB_RT_DIRECT_KEEP_OFF", 1800, 0, 1000000));
    keep_rules.distance = EnvFloat("BB_RT_KEEP_DIST", 40.0f, 0.0f, 10000.0f);
    keep_rules.skinned_frames = EnvU64("BB_RT_KEEP_SKINNED", 30, 0, 1000000);
    // Off-screen casters stay (user, 2026-11-12): at 3600 frames a building or lamp post behind
    // the camera lost its moon shadow after a minute of standing still. Distance still drops them.
    keep_rules.kept_unused_frames = EnvU64("BB_RT_KEEP_UNUSED", 10000000, UnusedFrames, 10000000);
    keep_rules.triangle_budget = EnvU64("BB_RT_KEEP_TRIS", 0, 0, 100000000);
    keep_rules.max_triangles = u32(EnvU64("BB_RT_KEEP_MAX_TRIS", 0, 0, 100000000));
    keep_gi = EnvU64("BB_RT_KEEP_GI", 0, 0, 1) != 0;
    off_switches.lights = EnvU64("BB_RT_OFF_LIGHTS", 1, 0, 1) != 0;
    off_switches.cone = EnvU64("BB_RT_OFF_CONE", 1, 0, 1) != 0;
    off_switches.instances = EnvU64("BB_RT_OFF_INSTANCES", 1, 0, 1) != 0;
    off_switches.evict = EnvU64("BB_RT_OFF_EVICT", 1, 0, 1) != 0;
    off_switches.texresolve = EnvU64("BB_RT_OFF_TEXRESOLVE", 1, 0, 1) != 0;
    off_switches.holds = EnvU64("BB_RT_OFF_HOLDS", 1, 0, 1) != 0;
    light_keep_scene = EnvU64("BB_RT_LIGHT_KEEP_SCENE", 1, 0, 1) != 0;
    kept_probe = EnvU64("BB_RT_KEPT_PROBE", 1, 0, 1) != 0;
    keep_rules.probe_hide = kept_probe;
    debug_mode = u32(EnvU64("BB_RT_DEBUG", 0, 0, 4));
    gi_strength = EnvFloat("BB_RT_GI_STRENGTH", 1.0f, 0.0f, 100.0f);
    gi_fill_sky = EnvFloat("BB_RT_GI_FILL_SKY", 1.0f, 0.0f, 10.0f);
    gi_sky = EnvFloat("BB_RT_SKY", 0.0f, 0.0f, 100.0f);
    gi_clamp = EnvFloat("BB_RT_GI_CLAMP", 4.0f, 0.01f, 1e4f);
    gi_bias = EnvFloat("BB_RT_GI_BIAS", 0.02f, 0.0f, 10.0f);
    reflect_strength = EnvFloat("BB_RT_REFLECT_STRENGTH", 1.0f, 0.0f, 100.0f);
    reflect_debug = u32(EnvU64("BB_RT_REFLECT_DEBUG", 0, 0, 3));
    shadow_debug = u32(EnvU64("BB_RT_SHADOW_DEBUG", 0, 0, 1));
    shadow_strength = EnvFloat("BB_RT_SHADOW_STRENGTH", 0.75f, 0.0f, 100.0f);
    shadow_bias = EnvFloat("BB_RT_SHADOW_BIAS", 0.02f, 0.0f, 10.0f);
    shadow_tmin = EnvFloat("BB_RT_SHADOW_TMIN", 0.0f, 0.0f, 10.0f);
    shadow_gap = EnvFloat("BB_RT_SHADOW_GAP", 0.3f, 0.0f, 10.0f);
    hitch_threshold_ms = double(EnvU64("BB_RT_HITCH_MS", 8, 1, 1000));
    dir_sets_mode = Rt::KnobEquals("BB_RT_DIR_SETS", "gpu")    ? DirSetsMode::Gpu
                    : Rt::KnobEquals("BB_RT_DIR_SETS", "host") ? DirSetsMode::Host
                                                                : DirSetsMode::Snapshot;
    ao_live = Rt::KnobOn("BB_RT_AO_LIVE");
    dir_sets_bind_first = Rt::KnobEquals("BB_RT_DIR_SETS_BIND", "first");
    OpenLightDump();
    const char* pause = Rt::LiveKnobs().Get("BB_RT_PAUSE");
    const bool paused = pause && pause[0] == '1';
    if (pause_knob != int(paused)) {
        pause_knob = int(paused);
        if (enabled && paused != Rt::effects_paused.load(std::memory_order_relaxed)) {
            Log("Ray tracing live: BB_RT_PAUSE %u -> %u\n", u32(!paused), u32(paused));
        }
        Rt::effects_paused.store(paused, std::memory_order_relaxed);
    }
    gi_debug = u32(EnvU64("BB_RT_GI_DEBUG", 0, 0, 27));
    // 10+i shows G-buffer target i, 20+i its alpha; the target follows the live knob.
    debug_target = gi_debug >= 10 ? int(gi_debug % 10) : -1;
    gi_debug_threshold = EnvFloat("BB_RT_DEBUG_THRESH", 0.0f, 0.0f, 65504.0f);
    gpu_line_frames = Rt::GpuLineFrames(Rt::Knob("BB_RT_GPU_LINE_FRAMES"));
}

void RayTracing::PollHot() {
    if (frame % 30 != 0) {
        return;
    }
    Rt::LiveConfig& cfg = Rt::LiveKnobs();
    if (cfg.Poll()) {
        ReadLiveKnobs();
        std::string keys;
        for (const auto& [k, v] : cfg.Values()) {
            keys += " " + k + "=" + v;
        }
        Log("Ray tracing: live config %s reloaded at frame %llu (%zu keys):%s\n", cfg.Path().c_str(),
            (unsigned long long)frame, cfg.Values().size(), keys.c_str());
        ApplyToggles(Rt::ReadToggles(Rt::Knob, instance.GetVendorID()));
        LogLiveState();
    }
    if (shader_dir.empty()) {
        return;
    }
    for (HotShader& hs : hot_shaders) {
        const std::string path = shader_dir + "/" + hs.name + ".spv";
        Rt::FileStamp stamp{};
        if (!Rt::StampFile(path.c_str(), stamp) || stamp == hs.stamp) {
            continue;
        }
        hs.stamp = stamp;
        const std::vector<u32> code = Rt::ReadSpv(path.c_str());
        if (code.empty()) {
            Log("Ray tracing: shader %s not reloaded (%s is not SPIR-V)\n", hs.name, path.c_str());
            continue;
        }
        if (RebuildHotShader(hs, code)) {
            Log("Ray tracing: shader %s reloaded from %s (%zu words) at frame %llu\n", hs.name,
                path.c_str(), code.size(), (unsigned long long)frame);
        } else {
            Log("Ray tracing: shader %s reload failed, old pipeline kept\n", hs.name);
        }
    }
}

void RayTracing::ApplyToggles(const Rt::LiveToggles& wanted) {
    const Rt::LiveToggles prev = toggles;
    if (enabled) {
        for (const char* key : Rt::BlockedKeys(last_wanted, wanted, toggle_avail)) {
            const std::string_view k{key};
            const std::string why =
                k == "BB_RT_PTLAS"
                    ? instance.PartitionedAccelerationStructureProblem()
                : k == "BB_RT_GI_PIPELINE"
                    ? std::string(surface_avail ? "VK_KHR_ray_tracing_pipeline not enabled at launch "
                                                  "(BB_RT_GI_PIPELINE=1 or the non-AMD default)"
                                                : "no hit surfaces")
                : k == "BB_RT_GI_SER"
                    ? std::string("VK_NV_ray_tracing_invocation_reorder not enabled at launch "
                                  "(BB_RT_GI_SER, needs the GI pipeline)")
                : k == "BB_RT_GI" || k == "BB_RT_REFLECT" || k == "BB_RT_RESTIR_GI"
                    ? std::string("no hit surfaces")
                    : std::string("creation failed earlier");
            Log("Ray tracing live: %s=1 ignored, unavailable (%s)\n", key, why.c_str());
        }
    }
    last_wanted = wanted;
    Rt::LiveToggles next = Rt::ResolveToggles(wanted, toggle_avail);
    while (enabled && Rt::HasFeatures(Rt::NewFeatures(made, Rt::BuildSet(next)))) {
        MakeFeatures(Rt::BuildSet(next));
        next = Rt::ResolveToggles(wanted, toggle_avail);
    }
    if (enabled && next.ptlas && !prev.ptlas && !PreparePtlas()) {
        next.ptlas = false;
    }
    if (enabled) {
        for (const Rt::LiveChange& change : Rt::DiffToggles(prev, next)) {
            Log("Ray tracing live: %s %u -> %u\n", change.key, change.from, change.to);
        }
    }
    const Rt::LiveResets resets = Rt::ResetsFor(prev, next);
    toggles = next;
    gi_enabled = next.gi;
    direct_enabled = next.direct;
    reflect_enabled = next.reflect;
    fog_enabled = next.fog;
    ao_enabled = next.ao;
    restir_gi_enabled = next.restir_gi;
    gi_sparse = next.gi_sparse;
    restir_enabled = next.restir_di;
    gi_rt_pipeline = next.gi_pipeline;
    gi_rt_ser = next.gi_ser;
    ptlas_pending = next.ptlas;
    direct_lights_wanted = direct_enabled || reflect_enabled || (gi_enabled && gi_bounces > 0);
    light_capture = light_dump != nullptr || shadows_enabled || direct_lights_wanted || ao_enabled;
    surface_enabled = surface_avail && surface_made && (gi_enabled || direct_enabled);
    if (!gi_enabled) {
        material_cleared = false;
        material_resets.clear();
    }
    if (!enabled || !resets.histories) {
        return;
    }
    history_valid = false;
    history_prev_frame_ok = false;
    restir_last_frame = ~0ull;
    ao_history_valid = false;
    fog_last_frame = ~0ull;
    glass_keep_frame = ~0ull;
    Log("Ray tracing live: histories reset at frame %llu (GI, ReSTIR GI/DI reservoirs, AO, fog)\n",
        (unsigned long long)frame);
}

bool RayTracing::MakeFeatures(const Rt::LiveToggles& need) {
    const Rt::LiveToggles fresh = Rt::NewFeatures(made, need);
    if (!Rt::HasFeatures(fresh)) {
        return true;
    }
    const auto start = std::chrono::steady_clock::now();
    scheduler.Finish();
    const Rt::LiveToggles build = Rt::MergeFeatures(made, need);
    bool ok = CreateFeatureRings(build);
    bool surfaces_failed = false;
    if (ok && Rt::NeedsSurfaces(build) && !surface_made) {
        surface_made = CreateSurfaceResources();
        if (!surface_made) {
            DestroySurfaceResources();
            surfaces_failed = true;
            ok = false;
        } else if (fog_pipeline && !fog_surfaces && surface_avail) {
            std::erase_if(hot_shaders,
                          [&](const HotShader& hs) { return hs.pipeline == &fog_pipeline; });
            fog_pipeline.reset();
            fog_pipeline_layout.reset();
            fog_layout.reset();
        }
    }
    ok = ok && CreatePipelines(build);
    std::string names;
    const auto name = [&](bool on, const char* key) {
        if (on) {
            names += " ";
            names += key;
        }
    };
    name(fresh.gi, "BB_RT_GI");
    name(fresh.direct, "BB_RT_DIRECT");
    name(fresh.reflect, "BB_RT_REFLECT");
    name(fresh.fog, "BB_RT_FX_FOG");
    name(fresh.ao, "BB_RT_AO");
    name(fresh.restir_gi, "BB_RT_RESTIR_GI");
    name(fresh.restir_di, "BB_RT_RESTIR");
    name(fresh.gi_pipeline, "BB_RT_GI_PIPELINE");
    name(fresh.gi_ser, "BB_RT_GI_SER");
    if (!ok) {
        if (surfaces_failed) {
            toggle_avail.gi = toggle_avail.direct = false;
        }
        toggle_avail.gi &= !fresh.gi;
        toggle_avail.direct &= !fresh.direct;
        toggle_avail.reflect &= !fresh.reflect;
        toggle_avail.fog &= !fresh.fog;
        toggle_avail.ao &= !fresh.ao;
        toggle_avail.restir_gi &= !fresh.restir_gi;
        toggle_avail.restir_di &= !fresh.restir_di;
        toggle_avail.gi_pipeline &= !fresh.gi_pipeline;
        toggle_avail.gi_ser &= !fresh.gi_ser;
        Log("Ray tracing live: resource creation failed for%s%s, kept off\n", names.c_str(),
            surfaces_failed ? " (hit surfaces)" : "");
        return false;
    }
    if ((fresh.reflect || fresh.restir_gi) && gi_width) {
        DestroyGiImages();
    }
    made = build;
    Log("Ray tracing live: created resources for%s at frame %llu after a GPU wait (%.1f ms)\n",
        names.c_str(), (unsigned long long)frame,
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
            .count());
    return true;
}

void RayTracing::LogLiveState() {
    Log("Ray tracing live state:%s BB_RT_OMM_SKINNED=%u BB_RT_GI_LAST_BOUNCE_CACHE=%u "
        "BB_RT_BOUNCES=%u BB_RT_ADAPTIVE_MS=%u BB_RT_MOON_HALF=%u BB_RT_RESTIR_GI_JACOBIAN=%u "
        "BB_RT_SHARC=%u BB_RT_FAR_START=%u BB_RT_FAR_BAND=%u\n",
        Rt::LiveStateLine(toggles, Rt::effects_paused.load(std::memory_order_relaxed)).c_str(),
        omm_skinned ? 1u : 0u, gi_last_bounce_cache ? 1u : 0u, gi_bounces,
        u32(adaptive_ms + 0.5f), moon_half, u32(restir_gi_jacobian + 0.5f), gi_sharc ? 1u : 0u,
        u32(far_start + 0.5f), u32(far_band + 0.5f));
}

bool RayTracing::PreparePtlas() {
    if (!toggle_avail.ptlas) {
        return false;
    }
    if (!ptlas_made) {
        ptlas_made = ptlas.Create(instance, capacity.records, capacity.records, Slots);
        if (!ptlas_made) {
            toggle_avail.ptlas = false;
            Log("Ray tracing: partitioned TLAS unavailable (size query or buffer creation "
                "failed), KHR TLAS kept\n");
            return false;
        }
        Log("Ray tracing: partitioned TLAS on (BB_RT_PTLAS, live, "
            "VK_NV_partitioned_acceleration_structure): %u instances, %u partitions + global, "
            "world-space instances, only changed ones written per frame, %llu KiB\n",
            ptlas.InstanceCount(), Rt::PtlasPartitions, (unsigned long long)(ptlas.Bytes() >> 10));
        return true;
    }
    if (!ptlas_on) {
        scheduler.Finish();
        ptlas.Restart();
    }
    return true;
}

void RayTracing::FlipTlasMode() {
    const bool want = ptlas_pending && ptlas_made;
    if (want == ptlas_on) {
        return;
    }
    ptlas_on = want;
    tlas_address = ptlas_on ? ptlas.Address() : khr_tlas_address;
    tlas_hidden_last = ptlas_on ? ptlas.HiddenTotal() : 0u;
    if (enabled) {
        Log("Ray tracing live: TLAS rebuilt as %s at frame %llu\n",
            ptlas_on ? "partitioned (PTLAS)" : "KHR", (unsigned long long)frame);
    }
}

bool RayTracing::RebuildHotShader(HotShader& hs, std::span<const u32> code) {
    const auto device = instance.GetDevice();
    const auto [module_result, module] = device.createShaderModule(
        vk::ShaderModuleCreateInfo{.codeSize = code.size() * sizeof(u32), .pCode = code.data()});
    if (module_result != vk::Result::eSuccess) {
        return false;
    }
    auto pipeline_result = device.createComputePipelineUnique(
        {}, vk::ComputePipelineCreateInfo{
                .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                          .module = module,
                          .pName = "main"},
                .layout = **hs.layout,
            });
    device.destroyShaderModule(module);
    if (pipeline_result.result != vk::Result::eSuccess) {
        return false;
    }
    scheduler.Finish();
    *hs.pipeline = std::move(pipeline_result.value);
    return true;
}

bool RayTracing::CreatePipelines(const Rt::LiveToggles& f) {
    const auto device = instance.GetDevice();
    const auto make = [&](std::span<const vk::DescriptorSetLayoutBinding> bindings,
                          u32 push_bytes, std::span<const u32> code,
                          vk::UniqueDescriptorSetLayout& layout,
                          vk::UniquePipelineLayout& pipeline_layout,
                          vk::UniquePipeline& pipeline, vk::DescriptorSetLayout extra = {}) {
        if (pipeline) {
            return true;
        }
        auto layout_result = device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = static_cast<u32>(bindings.size()),
            .pBindings = bindings.data(),
        });
        if (layout_result.result != vk::Result::eSuccess) {
            return false;
        }
        layout = std::move(layout_result.value);
        const vk::PushConstantRange range{
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
            .offset = 0,
            .size = push_bytes,
        };
        const std::array<vk::DescriptorSetLayout, 2> set_layouts{*layout, extra};
        auto pipeline_layout_result = device.createPipelineLayoutUnique({
            .setLayoutCount = extra ? 2u : 1u,
            .pSetLayouts = set_layouts.data(),
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &range,
        });
        if (pipeline_layout_result.result != vk::Result::eSuccess) {
            return false;
        }
        pipeline_layout = std::move(pipeline_layout_result.value);
        const char* hot_name = HotShaderName(code.data());
        std::vector<u32> file_code;
        Rt::FileStamp stamp{};
        if (hot_name && !shader_dir.empty()) {
            const std::string path = shader_dir + "/" + hot_name + ".spv";
            if (Rt::StampFile(path.c_str(), stamp)) {
                file_code = Rt::ReadSpv(path.c_str());
                Log("Ray tracing: shader %s %s %s\n", hot_name,
                    file_code.empty() ? "ignored (not SPIR-V):" : "loaded from", path.c_str());
            }
        }
        const auto build = [&](std::span<const u32> spv) {
            const auto module = CompileSPV(spv, device);
            auto result = device.createComputePipelineUnique(
                {}, vk::ComputePipelineCreateInfo{
                        .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                                  .module = module,
                                  .pName = "main"},
                        .layout = *pipeline_layout,
                    });
            device.destroyShaderModule(module);
            return result;
        };
        auto pipeline_result = build(file_code.empty() ? code : std::span<const u32>(file_code));
        if (pipeline_result.result != vk::Result::eSuccess && !file_code.empty()) {
            Log("Ray tracing: shader %s from %s failed to build, using the embedded code\n",
                hot_name, shader_dir.c_str());
            stamp = {};
            pipeline_result = build(code);
        }
        if (pipeline_result.result != vk::Result::eSuccess) {
            return false;
        }
        pipeline = std::move(pipeline_result.value);
        if (hot_name) {
            hot_shaders.push_back({hot_name, &pipeline_layout, &pipeline, stamp});
        }
        return true;
    };
    const auto binding = [](u32 index, vk::DescriptorType type) {
        return vk::DescriptorSetLayoutBinding{.binding = index,
                                              .descriptorType = type,
                                              .descriptorCount = 1,
                                              .stageFlags = vk::ShaderStageFlagBits::eCompute};
    };
    using Type = vk::DescriptorType;
    const std::array<vk::DescriptorSetLayoutBinding, 4> gather_bindings = {{
        binding(0, Type::eStorageBuffer),
        binding(1, Type::eStorageBuffer),
        binding(2, Type::eStorageBuffer),
        binding(3, Type::eStorageBuffer),
    }};
    const std::array<vk::DescriptorSetLayoutBinding, 2> omm_bindings = {{
        binding(0, Type::eStorageBuffer),
        binding(1, Type::eStorageBuffer),
    }};
    if (omm_enabled && !make(omm_bindings, sizeof(OmmConstants), RT_OMM_BAKE_COMP, omm_layout,
                             omm_pipeline_layout, omm_pipeline, *surface_layout)) {
        Log("Ray tracing: opacity micromaps off (bake pipeline failed)\n");
        omm_enabled = false;
    }
    const std::array<vk::DescriptorSetLayoutBinding, 4> probe_bindings = {{
        binding(0, Type::eStorageBuffer),
        binding(1, Type::eStorageBuffer),
        binding(2, Type::eStorageBuffer),
        binding(3, Type::eStorageBuffer),
    }};
    const std::array<vk::DescriptorSetLayoutBinding, 1> debug_bindings = {{
        binding(0, Type::eStorageImage),
    }};
    const std::array<vk::DescriptorSetLayoutBinding, 23> gi_bindings = {{
        binding(0, Type::eSampledImage),
        binding(1, Type::eSampledImage),
        binding(2, Type::eSampledImage),
        binding(3, Type::eSampledImage),
        binding(4, Type::eSampledImage),
        binding(5, Type::eStorageImage),
        binding(6, Type::eStorageImage),
        binding(7, Type::eStorageImage),
        binding(8, Type::eUniformBuffer),
        binding(9, Type::eStorageBuffer),
        binding(10, Type::eStorageBuffer),
        binding(11, Type::eStorageBuffer),
        binding(12, Type::eStorageBuffer),
        binding(13, Type::eSampledImage),
        binding(14, Type::eSampledImage),
        binding(15, Type::eSampledImage),
        binding(16, Type::eStorageBuffer),
        binding(17, Type::eStorageBuffer),
        binding(18, Type::eStorageImage),
        binding(19, Type::eStorageImage),
        binding(20, Type::eStorageImage),
        binding(21, Type::eStorageImage),
        binding(27, Type::eSampledImage),
    }};
    const std::array<vk::DescriptorSetLayoutBinding, 13> apply_bindings = {{
        binding(0, Type::eStorageImage),
        binding(1, Type::eSampledImage),
        binding(2, Type::eSampledImage),
        binding(3, Type::eSampledImage),
        binding(4, Type::eSampledImage),
        binding(5, Type::eStorageBuffer),
        binding(6, Type::eStorageBuffer),
        binding(7, Type::eSampledImage),
        binding(8, Type::eSampledImage),
        binding(9, Type::eUniformBuffer),
        binding(10, Type::eStorageBuffer),
        binding(11, Type::eSampledImage),
        binding(12, Type::eStorageBuffer),
    }};
    const std::array<vk::DescriptorSetLayoutBinding, 6> shadow_bindings = {{
        binding(0, Type::eSampledImage),
        binding(1, Type::eStorageImage),
        binding(2, Type::eSampledImage),
        binding(3, Type::eSampledImage),
        binding(4, Type::eUniformBuffer),
        binding(5, Type::eSampledImage),
    }};
    if (shadows_enabled && !make(shadow_bindings, 4, RT_SHADOW_COMP, shadow_layout,
                                 shadow_pipeline_layout, shadow_pipeline)) {
        return false;
    }
    const std::array<vk::DescriptorSetLayoutBinding, 9> fog_bindings = {{
        binding(0, Type::eSampledImage),
        binding(1, Type::eStorageImage),
        binding(2, Type::eStorageImage),
        binding(3, Type::eSampledImage),
        binding(4, Type::eUniformBuffer),
        binding(5, Type::eStorageBuffer),
        binding(6, Type::eSampledImage),
        binding(9, Type::eSampledImage),
        binding(10, Type::eStorageBuffer),
    }};
    const std::array<vk::DescriptorSetLayoutBinding, 12> fog_opaque_bindings = {{
        binding(13, Type::eStorageBuffer),
        binding(0, Type::eSampledImage),
        binding(1, Type::eStorageImage),
        binding(2, Type::eStorageImage),
        binding(3, Type::eSampledImage),
        binding(4, Type::eUniformBuffer),
        binding(5, Type::eStorageBuffer),
        binding(6, Type::eSampledImage),
        binding(9, Type::eSampledImage),
        binding(10, Type::eStorageBuffer),
        binding(11, Type::eStorageBuffer),
        binding(12, Type::eStorageBuffer),
    }};
    const std::array<vk::DescriptorSetLayoutBinding, 14> fog_march_bindings = {{
        binding(13, Type::eStorageBuffer),
        binding(0, Type::eSampledImage),
        binding(1, Type::eStorageImage),
        binding(2, Type::eStorageImage),
        binding(3, Type::eSampledImage),
        binding(4, Type::eUniformBuffer),
        binding(5, Type::eStorageBuffer),
        binding(6, Type::eSampledImage),
        binding(7, Type::eStorageBuffer),
        binding(8, Type::eStorageBuffer),
        binding(9, Type::eSampledImage),
        binding(10, Type::eStorageBuffer),
        binding(11, Type::eStorageBuffer),
        binding(12, Type::eStorageBuffer),
    }};
    if (f.fog && !fog_pipeline) {
        fog_surfaces = surface_avail && surface_made && static_cast<bool>(*surface_layout);
    }
    if (f.fog &&
        (!(fog_surfaces ? make(fog_march_bindings, 4, RT_FOG_COMP, fog_layout, fog_pipeline_layout,
                               fog_pipeline, *surface_layout)
                        : make(fog_opaque_bindings, 4, RT_FOG_OPAQUE_COMP, fog_layout,
                               fog_pipeline_layout, fog_pipeline)) ||
         !make(fog_bindings, 4, RT_FOG_APPLY_COMP, fog_apply_layout, fog_apply_pipeline_layout,
               fog_apply_pipeline))) {
        return false;
    }
    const std::array<vk::DescriptorSetLayoutBinding, 4> sky_bindings = {{
        binding(0, Type::eSampledImage),
        binding(1, Type::eStorageImage),
        binding(2, Type::eCombinedImageSampler),
        binding(3, Type::eStorageBuffer),
    }};
    if (sky_enabled && !make(sky_bindings, sizeof(Rt::SkyPush), RT_SKY_COMP, sky_layout,
                             sky_pipeline_layout, sky_pipeline)) {
        return false;
    }
    const std::array<vk::DescriptorSetLayoutBinding, 4> world_set_bindings = {{
        binding(0, Type::eSampledImage),
        binding(1, Type::eSampledImage),
        binding(2, Type::eStorageBuffer),
        binding(3, Type::eStorageBuffer),
    }};
    if ((f.gi || f.reflect) && !world_set_pipeline &&
        !make(world_set_bindings, sizeof(WorldSetPush), RT_WORLD_SET_COMP, world_set_layout,
              world_set_pipeline_layout, world_set_pipeline)) {
        Log("Ray tracing: world set vote unavailable (pipeline failed): GI/reflection hits use "
            "each pixel's own directional set\n");
        world_set_pipeline.reset();
    }
    const std::array<vk::DescriptorSetLayoutBinding, 2> moon_dir_bindings = {{
        binding(0, Type::eStorageBuffer),
        binding(1, Type::eStorageBuffer),
    }};
    if (sky_enabled && !moon_dir_pipeline &&
        !make(moon_dir_bindings, sizeof(MoonDirPush), RT_MOON_DIR_COMP, moon_dir_layout,
              moon_dir_pipeline_layout, moon_dir_pipeline)) {
        Log("Ray tracing: moon direction from the sky unavailable (pipeline failed): light 1 "
            "keeps the game's direction\n");
        moon_dir_pipeline.reset();
    }
    const std::array<vk::DescriptorSetLayoutBinding, 3> exposure_bindings = {{
        binding(0, Type::eSampledImage),
        binding(1, Type::eStorageImage),
        binding(2, Type::eStorageBuffer),
    }};
    if (f.gi && !exposure_pipeline &&
        !make(exposure_bindings, sizeof(ExposurePush), RT_EXPOSURE_COMP, exposure_layout,
              exposure_pipeline_layout, exposure_pipeline)) {
        Log("Ray tracing: eye adaptation unavailable (pipeline failed)\n");
        exposure_pipeline.reset();
    }
    const std::array<vk::DescriptorSetLayoutBinding, 17> direct_bindings = {{
        binding(0, Type::eSampledImage),
        binding(1, Type::eStorageImage),
        binding(2, Type::eStorageImage),
        binding(3, Type::eSampledImage),
        binding(4, Type::eSampledImage),
        binding(5, Type::eSampledImage),
        binding(6, Type::eUniformBuffer),
        binding(7, Type::eStorageBuffer),
        binding(8, Type::eStorageBuffer),
        binding(12, Type::eStorageBuffer),
        binding(13, Type::eStorageBuffer),
        binding(14, Type::eSampledImage),
        binding(15, Type::eStorageBuffer),
        binding(16, Type::eSampledImage),
        binding(17, Type::eStorageBuffer),
        binding(18, Type::eStorageBuffer),
        binding(19, Type::eSampledImage),
    }};
    if (f.direct && !make(direct_bindings, 4, RT_DIRECT_COMP, direct_layout,
                                direct_pipeline_layout, direct_pipeline, *surface_layout)) {
        return false;
    }
    const std::array<vk::DescriptorSetLayoutBinding, 9> ao_bindings = {{
        binding(0, Type::eSampledImage),
        binding(1, Type::eStorageImage),
        binding(2, Type::eSampledImage),
        binding(3, Type::eSampledImage),
        binding(4, Type::eStorageImage),
        binding(5, Type::eUniformBuffer),
        binding(6, Type::eStorageBuffer),
        binding(12, Type::eStorageBuffer),
        binding(13, Type::eStorageBuffer),
    }};
    if (f.ao && !make(ao_bindings, 4, RT_AO_COMP, ao_layout, ao_pipeline_layout,
                                 ao_pipeline, *surface_layout)) {
        Log("Ray tracing: ambient occlusion unavailable (pipeline failed)\n");
        toggle_avail.ao = toggles.ao = ao_enabled = false;
    }
    std::array<vk::DescriptorSetLayoutBinding, 16> restir_bindings{};
    for (u32 i = 0; i < 9; ++i) {
        restir_bindings[i] = direct_bindings[i];
    }
    restir_bindings[12] = direct_bindings[9];
    restir_bindings[13] = direct_bindings[10];
    restir_bindings[14] = direct_bindings[11];
    restir_bindings[15] = direct_bindings[13];
    restir_bindings[9] = binding(9, Type::eStorageBuffer);
    restir_bindings[10] = binding(10, Type::eStorageBuffer);
    restir_bindings[11] = binding(11, Type::eStorageBuffer);
    if (f.restir_di &&
        (!make(restir_bindings, 4, RT_RESTIR_TEMPORAL_COMP, restir_temporal_layout,
               restir_temporal_pipeline_layout, restir_temporal_pipeline, *surface_layout) ||
         !make(restir_bindings, 4, RT_RESTIR_SPATIAL_COMP, restir_spatial_layout,
               restir_spatial_pipeline_layout, restir_spatial_pipeline, *surface_layout))) {
        return false;
    }
    std::array<vk::DescriptorSetLayoutBinding, 32> reflect_bindings{};
    for (u32 i = 0; i < 22; ++i) {
        reflect_bindings[i] = gi_bindings[i];
    }
    reflect_bindings[27] = gi_bindings[22];
    reflect_bindings[22] = binding(22, Type::eStorageImage);
    reflect_bindings[23] = binding(23, Type::eSampledImage);
    reflect_bindings[24] = binding(24, Type::eSampledImage);
    reflect_bindings[25] = binding(25, Type::eSampledImage);
    reflect_bindings[26] = binding(26, Type::eStorageImage);
    // See-through glass: the irradiance target, written on glass pixels.
    reflect_bindings[28] = binding(28, Type::eStorageImage);
    // Diffuse keep factor (1, or the decal cover on glass) for the GI apply pass.
    reflect_bindings[29] = binding(29, Type::eStorageImage);
    reflect_bindings[30] = binding(30, Type::eStorageBuffer);
    // The sky dome panorama, so mirror rays that miss show the sky image.
    reflect_bindings[31] = binding(31, Type::eCombinedImageSampler);
    std::array<vk::DescriptorSetLayoutBinding, 27> restir_init_bindings{};
    std::array<vk::DescriptorSetLayoutBinding, 27> restir_resolve_bindings{};
    for (u32 i = 0; i < 22; ++i) {
        restir_init_bindings[i] = gi_bindings[i];
        restir_resolve_bindings[i] = gi_bindings[i];
    }
    restir_init_bindings[26] = gi_bindings[22];
    restir_resolve_bindings[26] = gi_bindings[22];
    for (u32 i = 22; i < 26; ++i) {
        restir_init_bindings[i] = binding(i, Type::eStorageImage);
        restir_resolve_bindings[i] = binding(i, Type::eSampledImage);
    }
    std::array<vk::DescriptorSetLayoutBinding, 17> restir_pass_bindings{};
    restir_pass_bindings[0] = binding(0, Type::eSampledImage);
    restir_pass_bindings[1] = binding(1, Type::eSampledImage);
    restir_pass_bindings[2] = binding(2, Type::eUniformBuffer);
    for (u32 i = 3; i < 11; ++i) {
        restir_pass_bindings[i] = binding(i, Type::eSampledImage);
    }
    for (u32 i = 11; i < 15; ++i) {
        restir_pass_bindings[i] = binding(i, Type::eStorageImage);
    }
    restir_pass_bindings[15] = binding(15, Type::eStorageBuffer);
    restir_pass_bindings[16] = binding(16, Type::eStorageBuffer);
    if (f.restir_gi &&
        !(make(restir_init_bindings, 4, RT_RESTIR_GI_INIT_COMP, restir_gi_init_layout,
               restir_gi_init_pipeline_layout, restir_gi_init_pipeline, *surface_layout) &&
          make(restir_resolve_bindings, 4, RT_RESTIR_GI_RESOLVE_COMP, restir_gi_resolve_layout,
               restir_gi_resolve_pipeline_layout, restir_gi_resolve_pipeline, *surface_layout) &&
          make(restir_pass_bindings, 4, RT_RESTIR_GI_TEMPORAL_COMP, restir_gi_temporal_layout,
               restir_gi_temporal_pipeline_layout, restir_gi_temporal_pipeline, *surface_layout) &&
          make(restir_pass_bindings, 4, RT_RESTIR_GI_SPATIAL_COMP, restir_gi_spatial_layout,
               restir_gi_spatial_pipeline_layout, restir_gi_spatial_pipeline, *surface_layout))) {
        return false;
    }
    const bool rt_plain_new = f.gi_pipeline && !restir_gi_rt_pipelines[0];
    const bool rt_ser_new = f.gi_ser && !restir_gi_rt_pipelines[1];
    if (rt_plain_new || rt_ser_new) {
        bool ser_ok = !rt_ser_new || CreateGiRtPipeline(restir_init_bindings, 1);
        if (!ser_ok) {
            Log("Ray tracing: GI pipeline SER variant failed, SER unavailable\n");
            toggle_avail.gi_ser = false;
            toggles.gi_ser = gi_rt_ser = false;
        }
        const bool plain_ok = (!rt_plain_new && ser_ok) || CreateGiRtPipeline(restir_init_bindings, 0);
        if (plain_ok) {
            Log("Ray tracing: GI pipeline %s (BB_RT_GI_PIPELINE, default 1, live): ReSTIR GI initial "
                "trace as raygen + any-hit alpha test (VK_KHR_ray_tracing_pipeline), SER %s "
                "(VK_NV_ray_tracing_invocation_reorder %s, BB_RT_GI_SER, live), SBT stride %u, "
                "opacity micromaps %s\n",
                gi_rt_pipeline ? "on" : "ready but off", gi_rt_ser ? "on" : "off",
                toggle_avail.gi_ser                     ? "enabled"
                : instance.IsRayTracingReorderExposed() ? "exposed"
                                                        : "not exposed",
                gi_sbt.stride, omm_enabled ? "honoured (pipeline flag set)" : "off");
        } else {
            Log("Ray tracing: GI pipeline unavailable (BB_RT_GI_PIPELINE: pipeline or SBT failed), "
                "compute GI kept\n");
            toggle_avail.gi_pipeline = toggle_avail.gi_ser = false;
            toggles.gi_pipeline = toggles.gi_ser = gi_rt_pipeline = gi_rt_ser = false;
        }
    }
    if (!(make(gather_bindings, sizeof(GatherConstants), RT_GATHER_COMP, gather_layout,
               gather_pipeline_layout, gather_pipeline) &&
          make(probe_bindings, sizeof(ProbeConstants), RT_PROBE_COMP, probe_layout,
               probe_pipeline_layout, probe_pipeline) &&
          make(debug_bindings, sizeof(DebugConstants), RT_DEBUG_COMP, debug_layout,
               debug_pipeline_layout, debug_pipeline) &&
          (!f.reflect ||
           make(reflect_bindings, sizeof(GlassPush), RT_REFLECT_COMP, reflect_layout,
                reflect_pipeline_layout, reflect_pipeline, *surface_layout)) &&
          (!f.gi ||
           (make(gi_bindings, 4, RT_GI_COMP, gi_layout, gi_pipeline_layout, gi_pipeline,
                 *surface_layout) &&
            make(apply_bindings, sizeof(ApplyConstants), RT_GI_APPLY_COMP, apply_layout,
                 apply_pipeline_layout, apply_pipeline))))) {
        return false;
    }
    // Fill-light visibility (BB_RT_FILL_VIS): the GI layout plus binding 28 for the light batch.
    if (f.gi) {
        std::array<vk::DescriptorSetLayoutBinding, 24> vis_bindings{};
        std::copy(gi_bindings.begin(), gi_bindings.end(), vis_bindings.begin());
        vis_bindings[23] = binding(28, Type::eStorageBuffer);
        if (!make(vis_bindings, sizeof(FillVisPush), RT_LIGHT_VIS_COMP, fill_vis_layout,
                  fill_vis_pipeline_layout, fill_vis_pipeline, *surface_layout)) {
            Log("Ray tracing: fill-light visibility off (pipeline failed)\n");
            fill_vis_pipeline.reset();
        }
        // The moon and cloud layer grid: the GI layout as is, its own push constants.
        if (!make(gi_bindings, sizeof(SkyLayerPush), RT_SKY_LAYER_COMP, sky_layer_layout,
                  sky_layer_pipeline_layout, sky_layer_pipeline, *surface_layout)) {
            Log("Ray tracing: sky layer off (pipeline failed): light 1 keeps the game's "
                "direction\n");
            sky_layer_pipeline.reset();
        }
        if (!make(std::span<const vk::DescriptorSetLayoutBinding>{}, sizeof(SharcPush), RT_SHARC_COMP,
                  sharc_layout, sharc_pipeline_layout, sharc_pipeline)) {
            Log("Ray tracing: radiance cache off (BB_RT_SHARC: pipeline failed)\n");
            sharc_pipeline.reset();
        }
    }
    return true;
}

bool RayTracing::CreateGiRtPipeline(
    std::span<const vk::DescriptorSetLayoutBinding> compute_bindings, u32 variant) {
    if (restir_gi_rt_pipelines[variant]) {
        return true;
    }
    const auto device = instance.GetDevice();
    if (!restir_gi_rt_pipeline_layout) {
        std::vector<vk::DescriptorSetLayoutBinding> bindings(compute_bindings.begin(),
                                                             compute_bindings.end());
        for (auto& b : bindings) {
            b.stageFlags = GiRtStages;
        }
        auto layout_result = device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = static_cast<u32>(bindings.size()),
            .pBindings = bindings.data(),
        });
        if (layout_result.result != vk::Result::eSuccess) {
            return false;
        }
        restir_gi_rt_layout = std::move(layout_result.value);
        const vk::PushConstantRange range{.stageFlags = GiRtStages, .offset = 0, .size = 4};
        const std::array<vk::DescriptorSetLayout, 2> set_layouts{*restir_gi_rt_layout,
                                                                 *surface_layout};
        auto pipeline_layout_result = device.createPipelineLayoutUnique({
            .setLayoutCount = 2,
            .pSetLayouts = set_layouts.data(),
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &range,
        });
        if (pipeline_layout_result.result != vk::Result::eSuccess) {
            return false;
        }
        restir_gi_rt_pipeline_layout = std::move(pipeline_layout_result.value);
    }
    const std::array<vk::RayTracingShaderGroupCreateInfoKHR, 3> groups{{
        {.type = vk::RayTracingShaderGroupTypeKHR::eGeneral,
         .generalShader = 0,
         .closestHitShader = VK_SHADER_UNUSED_KHR,
         .anyHitShader = VK_SHADER_UNUSED_KHR,
         .intersectionShader = VK_SHADER_UNUSED_KHR},
        {.type = vk::RayTracingShaderGroupTypeKHR::eGeneral,
         .generalShader = 1,
         .closestHitShader = VK_SHADER_UNUSED_KHR,
         .anyHitShader = VK_SHADER_UNUSED_KHR,
         .intersectionShader = VK_SHADER_UNUSED_KHR},
        {.type = vk::RayTracingShaderGroupTypeKHR::eTrianglesHitGroup,
         .generalShader = VK_SHADER_UNUSED_KHR,
         .closestHitShader = 2,
         .anyHitShader = 3,
         .intersectionShader = VK_SHADER_UNUSED_KHR},
    }};
    const std::span<const u32> raygen = variant ? std::span<const u32>(RT_RESTIR_GI_INIT_SER_RGEN)
                                                : std::span<const u32>(RT_RESTIR_GI_INIT_RGEN);
    const std::array<vk::ShaderModule, 4> modules{
        CompileSPV(raygen, device), CompileSPV(RT_GI_MISS, device),
        CompileSPV(RT_GI_CHIT, device), CompileSPV(RT_GI_AHIT, device)};
    const std::array<vk::PipelineShaderStageCreateInfo, 4> stages{{
        {.stage = vk::ShaderStageFlagBits::eRaygenKHR, .module = modules[0], .pName = "main"},
        {.stage = vk::ShaderStageFlagBits::eMissKHR, .module = modules[1], .pName = "main"},
        {.stage = vk::ShaderStageFlagBits::eClosestHitKHR, .module = modules[2], .pName = "main"},
        {.stage = vk::ShaderStageFlagBits::eAnyHitKHR, .module = modules[3], .pName = "main"},
    }};
    auto pipeline_result = device.createRayTracingPipelineKHRUnique(
        {}, {},
        vk::RayTracingPipelineCreateInfoKHR{
            .flags = vk::PipelineCreateFlags(Rt::GiPipelineFlags(omm_enabled)),
            .stageCount = u32(stages.size()),
            .pStages = stages.data(),
            .groupCount = u32(groups.size()),
            .pGroups = groups.data(),
            .maxPipelineRayRecursionDepth = 1,
            .layout = *restir_gi_rt_pipeline_layout,
        });
    for (const auto module : modules) {
        device.destroyShaderModule(module);
    }
    if (pipeline_result.result != vk::Result::eSuccess) {
        return false;
    }
    const auto props =
        instance.GetPhysicalDevice()
            .getProperties2<vk::PhysicalDeviceProperties2,
                            vk::PhysicalDeviceRayTracingPipelinePropertiesKHR>()
            .get<vk::PhysicalDeviceRayTracingPipelinePropertiesKHR>();
    std::vector<u8> handles(size_t(groups.size()) * props.shaderGroupHandleSize);
    if (device.getRayTracingShaderGroupHandlesKHR(*pipeline_result.value, 0, u32(groups.size()),
                                                  handles.size(), handles.data()) !=
        vk::Result::eSuccess) {
        return false;
    }
    if (!gi_sbt_buffer.buffer) {
        gi_sbt = Rt::GiSbt(props.shaderGroupHandleSize, props.shaderGroupHandleAlignment,
                           props.shaderGroupBaseAlignment);
        void* mapped = nullptr;
        gi_sbt_buffer.size = u64(gi_sbt.size) * 2;
        if (!CreateRawBuffer(instance, gi_sbt_buffer.size,
                             VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR, true,
                             props.shaderGroupBaseAlignment, gi_sbt_buffer.buffer,
                             gi_sbt_buffer.allocation, &mapped, gi_sbt_buffer.address)) {
            return false;
        }
        gi_sbt_mapped = static_cast<u8*>(mapped);
        std::memset(gi_sbt_mapped, 0, gi_sbt_buffer.size);
    }
    const std::array<u32, 3> offsets{gi_sbt.raygen, gi_sbt.miss, gi_sbt.hit};
    for (u32 i = 0; i < offsets.size(); ++i) {
        std::memcpy(gi_sbt_mapped + u64(variant) * gi_sbt.size + offsets[i],
                    handles.data() + i * props.shaderGroupHandleSize, props.shaderGroupHandleSize);
    }
    restir_gi_rt_pipelines[variant] = std::move(pipeline_result.value);
    return true;
}

static bool CrumbStrict() {
    static const bool strict = [] {
        const char* env = std::getenv("BB_RT_CRUMB_STRICT");
        return env && env[0] == '1';
    }();
    return strict;
}

void RayTracing::CrumbBegin(vk::CommandBuffer cmdbuf, Rt::Crumb what) {
    if (!crumbs.buffer) {
        return;
    }
    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;
    ++crumb_serial;
    Checkpoint::Set(cmdbuf, Checkpoint::Op::RtPass, u32(what));
    if (CrumbStrict()) {
        Barrier(cmdbuf, Stage::eAllCommands, Access::eMemoryWrite, Stage::eTransfer,
                Access::eTransferWrite);
    }
    cmdbuf.fillBuffer(crumbs.buffer, offsetof(Rt::CrumbWords, started), 4, u32(what));
    cmdbuf.fillBuffer(crumbs.buffer, offsetof(Rt::CrumbWords, started_frame), 4, u32(frame));
    cmdbuf.fillBuffer(crumbs.buffer, offsetof(Rt::CrumbWords, started_serial), 4, crumb_serial);
    Barrier(cmdbuf, Stage::eTransfer, Access::eTransferWrite,
            Stage::eComputeShader | Stage::eAccelerationStructureBuildKHR |
                Stage::eMicromapBuildEXT | Stage::eAccelerationStructureCopyKHR,
            Access::eShaderRead | Access::eAccelerationStructureReadKHR);
}

void RayTracing::CrumbEnd(vk::CommandBuffer cmdbuf, Rt::Crumb what) {
    if (!crumbs.buffer) {
        return;
    }
    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;
    if (CrumbStrict()) {
        Barrier(cmdbuf, Stage::eAllCommands, Access::eMemoryWrite, Stage::eTransfer,
                Access::eTransferWrite);
    } else {
        Barrier(cmdbuf,
                Stage::eComputeShader | Stage::eAccelerationStructureBuildKHR |
                    Stage::eMicromapBuildEXT | Stage::eAccelerationStructureCopyKHR,
                Access::eShaderWrite | Access::eAccelerationStructureWriteKHR, Stage::eTransfer,
                Access::eTransferWrite);
    }
    cmdbuf.fillBuffer(crumbs.buffer, offsetof(Rt::CrumbWords, finished), 4, u32(what));
    cmdbuf.fillBuffer(crumbs.buffer, offsetof(Rt::CrumbWords, finished_frame), 4, u32(frame));
    cmdbuf.fillBuffer(crumbs.buffer, offsetof(Rt::CrumbWords, finished_serial), 4, crumb_serial);
    Checkpoint::Set(cmdbuf, Checkpoint::Op::RtPassDone, u32(what));
}

void RayTracing::PrintCrumbs() const {
    if (!crumbs_mapped) {
        std::printf("GPU: ray tracing breadcrumbs unavailable\n");
        return;
    }
    Rt::CrumbWords words{};
    std::memcpy(&words, crumbs_mapped, sizeof(words));
    std::printf("%s\n", Rt::DescribeCrumbs(words).c_str());
    std::fflush(stdout);
}

void RayTracing::TimeBegin(vk::CommandBuffer cmdbuf, u32 pass) {
    if (!time_pool) {
        return;
    }
    cmdbuf.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, *time_pool,
                           u32(frame % Slots) * TimePasses * 2 + pass * 2);
}

void RayTracing::TimeEnd(vk::CommandBuffer cmdbuf, u32 pass) {
    if (!time_pool) {
        return;
    }
    const u32 slot = u32(frame % Slots);
    cmdbuf.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, *time_pool,
                           slot * TimePasses * 2 + pass * 2 + 1);
    time_written[slot] |= 1u << pass;
}

void RayTracing::ReadTimes(u32 slot) {
    if (!time_pool || !time_written[slot]) {
        return;
    }
    time_last_ms = {};
    std::array<u64, TimePasses * 4> results{};
    const auto result = instance.GetDevice().getQueryPoolResults(
        *time_pool, slot * TimePasses * 2, TimePasses * 2, sizeof(results), results.data(),
        sizeof(u64) * 2,
        vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWithAvailability);
    if (result != vk::Result::eSuccess && result != vk::Result::eNotReady) {
        return;
    }
    for (u32 pass = 0; pass < TimePasses; ++pass) {
        if (!(time_written[slot] & (1u << pass))) {
            continue;
        }
        const u64 begin = results[pass * 4], begin_ok = results[pass * 4 + 1];
        const u64 end = results[pass * 4 + 2], end_ok = results[pass * 4 + 3];
        if (!begin_ok || !end_ok || end < begin) {
            continue;
        }
        time_last_ms[pass] = double(end - begin) * timestamp_period_ns * 1e-6;
        time_sum_ms[pass] += time_last_ms[pass];
        ++time_count[pass];
    }
    adaptive_frame_ms = 0.0;
    for (const double ms : time_last_ms) {
        adaptive_frame_ms += ms;
    }
    time_written[slot] = 0;
}

void RayTracing::ApplyCompaction(u32 slot) {
    auto& list = slot_compact[slot];
    if (!compact_pool || list.empty()) {
        return;
    }
    std::vector<u64> sizes(list.size());
    const auto device = instance.GetDevice();
    const auto result = device.getQueryPoolResults(
        *compact_pool, slot * capacity.records, u32(list.size()), sizes.size() * sizeof(u64),
        sizes.data(), sizeof(u64), vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWait);
    if (result != vk::Result::eSuccess) {
        list.clear();
        return;
    }
    std::vector<vk::CopyAccelerationStructureInfoKHR> copies;
    for (size_t i = 0; i < list.size(); ++i) {
        Rt::CacheEntry* e = cache.ById(list[i].entry_id);
        if (!Rt::CompactCandidate(e, list[i].built_frame, sizes[i])) {
            continue;
        }
        const u64 bytes = sizes[i];
        const u64 offset = pool.Alloc(bytes);
        if (offset == Rt::BlockPool::Fail) {
            continue;
        }
        auto handle = device.createAccelerationStructureKHRUnique({
            .buffer = pool_buffer.buffer,
            .offset = offset,
            .size = bytes,
            .type = vk::AccelerationStructureTypeKHR::eBottomLevel,
        });
        if (handle.result != vk::Result::eSuccess) {
            pool.Free(offset, bytes);
            continue;
        }
        copies.push_back({.src = *entry_handles[e->id],
                          .dst = *handle.value,
                          .mode = vk::CopyAccelerationStructureModeKHR::eCompact});
        const auto old = entry_handles[e->id].release();
        scheduler.DeferOperation([device, old] { device.destroyAccelerationStructureKHR(old); });
        retired.push_back({e->offset, e->bytes, scheduler.CurrentTick(), frame});
        retired_bytes += e->bytes;
        cache.stats.compacted_saved += e->bytes - bytes;
        ++cache.stats.compacted;
        entry_handles[e->id] = std::move(handle.value);
        entry_addresses[e->id] = device.getAccelerationStructureAddressKHR(
            {.accelerationStructure = *entry_handles[e->id]});
        e->offset = offset;
        e->bytes = bytes;
        e->compacted = true;
    }
    list.clear();
    if (copies.empty()) {
        return;
    }
    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();
    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;
    Barrier(cmdbuf, Stage::eAccelerationStructureBuildKHR | Stage::eComputeShader,
            Access::eAccelerationStructureReadKHR | Access::eAccelerationStructureWriteKHR,
            Stage::eAccelerationStructureCopyKHR,
            Access::eAccelerationStructureReadKHR | Access::eAccelerationStructureWriteKHR);
    CrumbBegin(cmdbuf, Rt::Crumb::CompactCopy);
    for (const auto& copy : copies) {
        cmdbuf.copyAccelerationStructureKHR(copy);
    }
    CrumbEnd(cmdbuf, Rt::Crumb::CompactCopy);
    Barrier(cmdbuf, Stage::eAccelerationStructureCopyKHR, Access::eAccelerationStructureWriteKHR,
            Stage::eAccelerationStructureBuildKHR | Stage::eComputeShader,
            Access::eAccelerationStructureReadKHR);
}

void RayTracing::ReclaimBlocks() {
    const auto is_free = [&](u64 tick) { return scheduler.IsFree(tick); };
    std::erase_if(retired, [&](RetiredBlock& block) {
        if (!Rt::RetireReady(block, tlas_frame, tlas_tick, is_free)) {
            return false;
        }
        pool.Free(block.offset, block.bytes);
        retired_bytes -= block.bytes;
        return true;
    });
    std::erase_if(retired_uv, [&](RetiredBlock& block) {
        if (!Rt::RetireReady(block, tlas_frame, tlas_tick, is_free)) {
            return false;
        }
        uv_pool.Free(block.offset, block.bytes);
        return true;
    });
    std::erase_if(retired_omm, [&](RetiredBlock& block) {
        if (!Rt::RetireReady(block, tlas_frame, tlas_tick, is_free)) {
            return false;
        }
        omm_pool.Free(block.offset, block.bytes);
        return true;
    });
    std::erase_if(retired_omm_index, [&](RetiredBlock& block) {
        if (!Rt::RetireReady(block, tlas_frame, tlas_tick, is_free)) {
            return false;
        }
        omm_index_pool.Free(block.offset, block.bytes);
        return true;
    });
}

void RayTracing::ReleaseEntry(const Rt::CacheEntry& entry) {
    if (entry.id < instance_state_prev.size()) {
        instance_state_prev[entry.id] = 0;
    }
    if (entry.id >= entry_handles.size()) {
        return;
    }
    if (entry_handles[entry.id]) {
        const auto device = instance.GetDevice();
        const auto handle = entry_handles[entry.id].release();
        scheduler.DeferOperation([device, handle] { device.destroyAccelerationStructureKHR(handle); });
    }
    entry_addresses[entry.id] = 0;
    retired.push_back({entry.offset, entry.bytes, scheduler.CurrentTick(), frame});
    retired_bytes += entry.bytes;
    if (entry.uv_base != Rt::NoUv) {
        retired_uv.push_back({u64(entry.uv_base) * Rt::UvBytes, entry.uv_bytes, scheduler.CurrentTick(), frame});
    }
    if (entry.omm) {
        if (entry.id < omm_handles.size() && omm_handles[entry.id]) {
            const auto device = instance.GetDevice();
            const auto mm = omm_handles[entry.id].release();
            scheduler.DeferOperation([device, mm] { device.destroyMicromapEXT(mm); });
        }
        retired_omm.push_back({entry.omm_offset, entry.omm_bytes, scheduler.CurrentTick(), frame});
        if (entry.omm_index_bytes) {
            retired_omm_index.push_back(
                {entry.omm_index_offset, entry.omm_index_bytes, scheduler.CurrentTick(), frame});
        }
    }
    free_ids.push_back(entry.id);
    if (material.buffer && gi_enabled) {
        material_resets.push_back(entry.id);
    }
}

u64 RayTracing::ScratchBytes(u32 triangles, bool updatable, bool update, u32 omm_entry) const {
    const auto input = Rt::BlasInputFor(triangles);
    auto geometry_desc = SoupGeometry(soup.address, input.max_vertex);
    const auto usage = OmmUsage(triangles, omm_level);
    vk::AccelerationStructureTrianglesOpacityMicromapEXT omm_chain{
        .indexType = vk::IndexType::eUint32,
        .indexStride = Rt::OmmIndexBytes,
        .usageCountsCount = 1,
        .pUsageCounts = &usage,
    };
    const auto flags = Rt::BlasBuildFlags(updatable, compact_enabled, skin_fast_trace);
    const auto plain = instance.GetDevice().getAccelerationStructureBuildSizesKHR(
        vk::AccelerationStructureBuildTypeKHR::eDevice, BlasInfo(&geometry_desc, flags), triangles);
    u64 bytes = update ? plain.updateScratchSize : plain.buildScratchSize;
    if (omm_entry < omm_handles.size() && omm_handles[omm_entry]) {
        omm_chain.micromap = *omm_handles[omm_entry];
        geometry_desc.geometry.triangles.pNext = &omm_chain;
        const auto sizes = instance.GetDevice().getAccelerationStructureBuildSizesKHR(
            vk::AccelerationStructureBuildTypeKHR::eDevice, BlasInfo(&geometry_desc, flags), triangles);
        bytes = std::max(bytes, update ? sizes.updateScratchSize : sizes.buildScratchSize);
    }
    return (bytes + scratch_alignment - 1) / scratch_alignment * scratch_alignment;
}

u64 RayTracing::OmmScratchBytes(u32 triangles) const {
    const auto usage = OmmUsage(triangles, omm_level);
    const vk::MicromapBuildInfoEXT info{
        .type = vk::MicromapTypeEXT::eOpacityMicromap,
        .flags = vk::BuildMicromapFlagBitsEXT::ePreferFastTrace,
        .mode = vk::BuildMicromapModeEXT::eBuild,
        .usageCountsCount = 1,
        .pUsageCounts = &usage,
    };
    const auto sizes = instance.GetDevice().getMicromapBuildSizesEXT(
        vk::AccelerationStructureBuildTypeKHR::eDevice, info);
    return (sizes.buildScratchSize + scratch_alignment - 1) / scratch_alignment *
           scratch_alignment;
}

bool RayTracing::AllocateEntry(u32 triangles, bool updatable, bool omm, Rt::CacheEntry& out,
                               u64& scratch_bytes, u64& omm_scratch_bytes) {
    CpuTimer timer{hitch_log, hitch.alloc};
    const auto device = instance.GetDevice();
    const auto input = Rt::BlasInputFor(triangles);
    auto geometry_desc = SoupGeometry(soup.address, input.max_vertex);
    const auto release = [&](const Rt::CacheEntry& e) { ReleaseEntry(e); };
    if (free_ids.empty()) {
        cache.EvictLeastRecent(release);
        return false;
    }
    const u32 id = free_ids.back();
    omm = omm && omm_enabled && (!updatable || (omm_skinned && omm_index_buffer.buffer));
    vk::UniqueMicromapEXT micromap;
    u64 omm_offset = 0, omm_bytes = 0;
    u64 omm_index_offset = 0, omm_index_bytes = 0;
    omm_scratch_bytes = 0;
    const auto usage = OmmUsage(triangles, omm_level);
    vk::AccelerationStructureTrianglesOpacityMicromapEXT omm_chain{
        .indexType = vk::IndexType::eUint32,
        .indexStride = Rt::OmmIndexBytes,
        .usageCountsCount = 1,
        .pUsageCounts = &usage,
    };
    if (omm) {
        const vk::MicromapBuildInfoEXT mm_info{
            .type = vk::MicromapTypeEXT::eOpacityMicromap,
            .flags = vk::BuildMicromapFlagBitsEXT::ePreferFastTrace,
            .mode = vk::BuildMicromapModeEXT::eBuild,
            .usageCountsCount = 1,
            .pUsageCounts = &usage,
        };
        const auto mm_sizes = device.getMicromapBuildSizesEXT(
            vk::AccelerationStructureBuildTypeKHR::eDevice, mm_info);
        omm_scratch_bytes = (mm_sizes.buildScratchSize + scratch_alignment - 1) /
                            scratch_alignment * scratch_alignment;
        omm_bytes = mm_sizes.micromapSize;
        omm_offset = omm_bytes && omm_scratch_bytes <= scratch.size
                         ? omm_pool.Alloc(omm_bytes)
                         : Rt::BlockPool::Fail;
        if (omm_offset != Rt::BlockPool::Fail) {
            auto mm = device.createMicromapEXTUnique({
                .buffer = omm_pool_buffer.buffer,
                .offset = omm_offset,
                .size = omm_bytes,
                .type = vk::MicromapTypeEXT::eOpacityMicromap,
            });
            if (mm.result == vk::Result::eSuccess) {
                micromap = std::move(mm.value);
            } else {
                omm_pool.Free(omm_offset, omm_bytes);
                omm_offset = Rt::BlockPool::Fail;
            }
        }
        if (omm_offset != Rt::BlockPool::Fail && updatable) {
            omm_index_bytes = u64(triangles) * Rt::OmmIndexBytes;
            omm_index_offset = omm_index_pool.Alloc(omm_index_bytes);
            if (omm_index_offset == Rt::BlockPool::Fail) {
                micromap.reset();
                omm_pool.Free(omm_offset, omm_bytes);
                omm_offset = Rt::BlockPool::Fail;
                omm_index_offset = 0;
                omm_index_bytes = 0;
            }
        }
        if (omm_offset == Rt::BlockPool::Fail) {
            omm = false;
            omm_scratch_bytes = 0;
            ++omm_over_budget;
        } else {
            omm_chain.micromap = *micromap;
            geometry_desc.geometry.triangles.pNext = &omm_chain;
        }
    }
    const auto info = BlasInfo(&geometry_desc, Rt::BlasBuildFlags(updatable, compact_enabled, skin_fast_trace));
    auto sizes = device.getAccelerationStructureBuildSizesKHR(
        vk::AccelerationStructureBuildTypeKHR::eDevice, info, triangles);
    if (geometry_desc.geometry.triangles.pNext) {
        auto plain_desc = geometry_desc;
        plain_desc.geometry.triangles.pNext = nullptr;
        const auto plain = device.getAccelerationStructureBuildSizesKHR(
            vk::AccelerationStructureBuildTypeKHR::eDevice,
            BlasInfo(&plain_desc, Rt::BlasBuildFlags(updatable, compact_enabled, skin_fast_trace)),
            triangles);
        sizes.accelerationStructureSize =
            std::max(sizes.accelerationStructureSize, plain.accelerationStructureSize);
        sizes.buildScratchSize = std::max(sizes.buildScratchSize, plain.buildScratchSize);
        sizes.updateScratchSize = std::max(sizes.updateScratchSize, plain.updateScratchSize);
    }
    const auto undo_omm = [&] {
        if (omm) {
            micromap.reset();
            omm_pool.Free(omm_offset, omm_bytes);
            if (omm_index_bytes) {
                omm_index_pool.Free(omm_index_offset, omm_index_bytes);
            }
        }
    };
    if (!sizes.accelerationStructureSize || sizes.buildScratchSize > scratch.size) {
        undo_omm();
        return false;
    }
    const u64 offset = pool.Alloc(sizes.accelerationStructureSize);
    if (offset == Rt::BlockPool::Fail) {
        undo_omm();
        cache.EvictForSpace(sizes.accelerationStructureSize, pool.Size(), 256, release);
        return false;
    }
    auto handle = device.createAccelerationStructureKHRUnique({
        .buffer = pool_buffer.buffer,
        .offset = offset,
        .size = sizes.accelerationStructureSize,
        .type = vk::AccelerationStructureTypeKHR::eBottomLevel,
    });
    if (handle.result != vk::Result::eSuccess) {
        undo_omm();
        pool.Free(offset, sizes.accelerationStructureSize);
        return false;
    }
    free_ids.pop_back();
    out.omm = omm;
    out.omm_offset = omm ? omm_offset : 0;
    out.omm_bytes = omm ? omm_bytes : 0;
    out.omm_index_offset = omm ? omm_index_offset : 0;
    out.omm_index_bytes = omm ? omm_index_bytes : 0;
    out.omm_ready = false;
    out.compacted = false;
    if (omm) {
        omm_handles[id] = std::move(micromap);
    }
    entry_handles[id] = std::move(handle.value);
    entry_addresses[id] =
        device.getAccelerationStructureAddressKHR({.accelerationStructure = *entry_handles[id]});
    out.id = id;
    out.offset = offset;
    out.bytes = sizes.accelerationStructureSize;
    out.triangles = triangles;
    out.valid = false;
    out.dirty = false;
    out.updatable = updatable;
    out.refits = 0;
    out.uv_base = Rt::NoUv;
    out.uv_bytes = 0;
    if (uv_buffer.buffer) {
        const u64 uv_bytes = u64(triangles) * Rt::TriangleUvBytes;
        const u64 uv_offset = uv_pool.Alloc(uv_bytes);
        if (uv_offset != Rt::BlockPool::Fail) {
            out.uv_base = u32(uv_offset / Rt::UvBytes);
            out.uv_bytes = uv_bytes;
        } else {
            ++surface_uv_full;
        }
    }
    scratch_bytes = (sizes.buildScratchSize + scratch_alignment - 1) / scratch_alignment *
                    scratch_alignment;
    return true;
}

void RayTracing::BeginSlot() {
    const u32 slot = u32(frame % Slots);
    if (slot_ticks[slot]) {
        if (!scheduler.IsFree(slot_ticks[slot])) {
            CpuTimer timer{hitch_log, hitch.wait};
            scheduler.Wait(slot_ticks[slot]);
        }
        slot_ticks[slot] = 0;
        ReadTimes(slot);
        ApplyCompaction(slot);
        CpuTimer timer{hitch_log, hitch.readback};
        const u32* flags = reinterpret_cast<const u32*>(flags_ring.mapped +
                                                        u64(slot) * flags_ring.slot_bytes);
        const auto& entries = slot_probe_entries[slot];
        const auto& skin_keys = slot_skin_keys[slot];
        const auto& skin_keep = slot_skin_keep[slot];
        last_masked = 0;
        const u64 probe_frame = slot_probe_frames[slot];
        const auto& views = slot_probe_views[slot];
        const auto& modes = slot_probe_modes[slot];
        struct Moved {
            u32 id;
            Rt::DrawKey home;
            std::array<float, 3> at;
            u32 k;
            float jump;
        };
        std::vector<Moved> moved;
        for (size_t i = 0; i < entries.size(); ++i) {
            const u32* words = flags + i * ProbeFlagWords;
            const u32 k = words[4];
            std::array<float, 3> at{};
            Rt::CacheEntry* pe = nullptr;
            if (k < Rt::ProbeSamples && i < views.size() && i < modes.size()) {
                float p[3];
                std::memcpy(p, words + 1, sizeof(p));
                at = Rt::Transform(views[i], {p[0], p[1], p[2]});
                pe = cache.ById(entries[i]);
                if (pe && modes[i] == 0 && pe->built_frame == probe_frame) {
                    pe->anchor = at;
                    pe->anchor_k = k;
                }
            }
            if (words[0]) {
                const bool kept = i < skin_keep.size() && skin_keep[i];
                ++last_masked;
                probe_kept += kept ? 1u : 0u;
                // A shelved placement is never drawn; a late flag from before it was shelved
                // must not dirty it out of the TLAS.
                const Rt::DrawKey* home = cache.KeyOf(entries[i]);
                if (home && (home->occurrence & Rt::BlasCache::ShelfBit)) {
                    continue;
                }
                // A static mesh found metres from where it was built is the same draw at another
                // placement (a reused building): keep this BLAS where it stands, as a kept
                // instance, and give the draw a shelved one that matches, or a new build.
                const float jump = pe && pe->anchor_k == k
                                       ? std::sqrt(Rt::Distance2(pe->anchor, at)) : 0.0f;
                if (shelf_metres > 0.0f && home && pe && words[0] != 1u && modes[i] == 1 &&
                    jump > shelf_metres && !pe->skinned && !pe->moving &&
                    pe->built_frame <= probe_frame) {
                    moved.push_back({entries[i], *home, at, k, jump});
                    continue;
                }
                const bool marked = cache.MarkDirty(entries[i], slot_probe_frames[slot]);
                Rt::CacheEntry* e = marked && moving_refit ? cache.ById(entries[i]) : nullptr;
                if (e && e->triangles <= moving_tris) {
                    moving_marked += e->moving ? 0u : 1u;
                    e->moving = true;
                    e->moving_frame = frame;
                }
                if (skin_log) {
                    u64 key = i < skin_keys.size() ? skin_keys[i] : 0;
                    if (!key) {
                        if (const Rt::DrawKey* k = cache.KeyOf(entries[i])) {
                            key = u64(Rt::DrawKeyHash{}(*k)) | 1ull;
                            if (skin_watch.size() < 4096) {
                                skin_watch.emplace(key, 1);
                            }
                        }
                    }
                    ++skin_sec_probe;
                    skin_sec_probe_masked += kept ? 0u : 1u;
                    Log("RT skin: frame %llu probe mismatch from frame %llu, key %016llx entry %u "
                        "%s on the GPU, %s\n",
                        (unsigned long long)frame, (unsigned long long)slot_probe_frames[slot],
                        (unsigned long long)key, entries[i],
                        kept ? "kept with its last pose" : "left out of the TLAS",
                        e && e->moving ? "now refit every frame" : marked ? "rebuilt next draw" : "stale flag");
                }
            }
        }
        for (Moved& m : moved) {
            Rt::DrawKey shelf = m.home;
            shelf.occurrence = Rt::BlasCache::ShelfBit | m.id;
            if (cache.Rekey(m.id, shelf)) {
                ++shelved_total;
            } else {
                m.id = ~0u;
            }
        }
        for (const Moved& m : moved) {
            if (m.id == ~0u) {
                continue;
            }
            Rt::CacheEntry* back = cache.FindShelved(m.home, [&](const Rt::CacheEntry& e) {
                return e.id != m.id && e.valid && !e.dirty && e.anchor_k == m.k &&
                       Rt::Distance2(e.anchor, m.at) < 0.25f;
            });
            const u32 back_id = back ? back->id : ~0u;
            const bool took = back && cache.Rekey(back_id, m.home);
            unshelved_total += took ? 1u : 0u;
            if (shelf_log_lines < 200) {
                ++shelf_log_lines;
                const Rt::CacheEntry* e = cache.ById(m.id);
                Log("RT shelf frame %llu (probed %llu): entry %u (%u tris) jumped %.1f m, kept in "
                    "place; draw now at (%.1f %.1f %.1f) %s %u\n",
                    (unsigned long long)frame, (unsigned long long)probe_frame, m.id,
                    e ? e->triangles : 0u, m.jump, m.at[0], m.at[1], m.at[2],
                    took ? "takes back shelved entry" : "gets a new build, no shelved match",
                    took ? back_id : 0u);
            }
        }
        masked_instances += last_masked;
    }
    slot_probe_entries[slot].clear();
    {
        CpuTimer timer{hitch_log, hitch.evict};
        ReclaimBlocks();
        cache.BeginFrame(frame);
        const std::array<float, 3> evict_camera =
            frame_camera_valid ? Rt::Transform(frame_inv_view, {0.0f, 0.0f, 0.0f})
                               : std::array<float, 3>{};
        const bool kept_limits = off_switches.instances && off_switches.evict;
        cache.EvictUnusedBy(
            [&](const Rt::CacheEntry& e) {
                return kept_limits && frame_camera_valid
                           ? Rt::UnusedLimit(e, evict_camera, keep_rules)
                           : keep_rules.unused_frames;
            },
            [&](const Rt::CacheEntry& e) { ReleaseEntry(e); });
        while (pool.Used() - retired_bytes > pool.Size() / 4 * 3 &&
               cache.EvictLeastRecent([&](const Rt::CacheEntry& e) { ReleaseEntry(e); })) {
        }
    }
    rebuild_budget.BeginFrame();
    refit_budget.BeginFrame(frame);
    Rt::Capacity frame_capacity = capacity;
    if (fx_smoke && smoke_ring.buffer) {
        frame_capacity.triangles = smoke_triangle_base;
    }
    geometry.Begin(reinterpret_cast<Rt::DrawRecord*>(records_ring.mapped +
                                                     u64(slot) * records_ring.slot_bytes),
                   indices_ring.mapped + u64(slot) * indices_ring.slot_bytes, frame_capacity);
    smoke_draws = smoke_triangles = smoke_vertex_cursor = 0;
    pending.clear();
    instances.clear();
    probes.clear();
    frame_draws = 0;
    fx_blended_last = fx_frame_blended;
    fx_blended_draws += fx_frame_blended;
    fx_frame_blended = 0;
    textures.BeginFrame();
    surface_written = 1;
    slot_textures[slot].assign(1, {});
    last_add_accepted = false;
    omm_used = 0;
}

const std::vector<std::array<float, 3>>& RayTracing::RecentFlames() const {
    static const std::vector<std::array<float, 3>> no_flames;
    return flames_frame == frame ? flames_done : flames_frame + 1 == frame ? flames_filling : no_flames;
}

const std::vector<std::array<float, 3>>& RayTracing::RecentWisps() const {
    static const std::vector<std::array<float, 3>> no_wisps;
    return flames_frame == frame ? wisps_done : flames_frame + 1 == frame ? wisps_filling : no_wisps;
}

void RayTracing::AddFlames(std::span<const Rt::FxParticle> world_particles) {
    if (flames_frame != frame) {
        const bool last = flames_frame + 1 == frame;
        flames_done = last ? std::move(flames_filling) : std::vector<std::array<float, 3>>{};
        wisps_done = last ? std::move(wisps_filling) : std::vector<std::array<float, 3>>{};
        flames_filling.clear();
        wisps_filling.clear();
        flames_frame = frame;
    }
    const bool pale_gate = !Rt::KnobIs("BB_RT_FLAME_PALE", '1');
    const bool log = Rt::KnobIs("BB_RT_FLAME_LOG", '1') && frame % 30 == 0;
    for (const Rt::FxParticle& p : world_particles) {
        // Pale particles are loot wisps, not fire: their orange flame image in water was fake.
        if (pale_gate && Rt::PaleParticle(p.rgb)) {
            if (wisps_filling.size() < Rt::FlameMaxParticles) {
                wisps_filling.push_back(p.centre);
            }
            continue;
        }
        if (flames_filling.size() >= Rt::FlameMaxParticles) {
            break;
        }
        flames_filling.push_back(p.centre);
        if (log) {
            std::printf("RT flame particle frame %llu world (%.2f %.2f %.2f) rgb %06x\n", (unsigned long long)frame,
                        p.centre[0], p.centre[1], p.centre[2], p.rgb);
        }
    }
}

u32 RayTracing::HazeColor(const std::array<float, 3>& world, u32 rgba) const {
    std::array<float, 3> sum{};
    for (const Rt::Light& l : direct_lights) {
        if (!l.has_world) {
            continue;
        }
        const float r = Rt::SpotCullRadius(l);
        const float dx = l.world[0] - world[0];
        const float dy = l.world[1] - world[1];
        const float dz = l.world[2] - world[2];
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (!(r > 0.0f) || d2 >= r * r) {
            continue;
        }
        float atten = std::max(0.0f, 1.0f / std::max(d2, 1e-2f) - 1.0f / (r * r));
        if (l.legacy) {
            const float f = 1.0f - std::sqrt(d2 / (r * r));
            atten = f * f;
        } else if (l.source > 0.0f && l.kind == Rt::LightKind::Point) {
            const float s2 = l.source * l.source;
            atten = std::max(0.0f, s2 / (d2 + s2) - s2 / (r * r + s2));
        }
        for (u32 c = 0; c < 3; ++c) {
            sum[c] += std::max(l.color[c], 0.0f) * atten;
        }
    }
    const float luma = (0.2126f * sum[0] + 0.7152f * sum[1] + 0.0722f * sum[2]) * haze_gain;
    const float lit = std::clamp(haze_floor + luma, 0.0f, 1.0f);
    const float peak = std::max({sum[0], sum[1], sum[2]});
    u32 out = 0;
    for (u32 c = 0; c < 3; ++c) {
        const float tint = peak > 1e-6f ? 1.0f + (sum[c] / peak - 1.0f) * haze_tint * std::min(luma, 1.0f) : 1.0f;
        out |= u32(std::clamp(float((rgba >> (8 * c)) & 0xFFu) * tint + 0.5f, 0.0f, 255.0f)) << (8 * c);
    }
    out |= u32(std::clamp(float(rgba >> 24) * lit + 0.5f, 0.0f, 255.0f)) << 24;
    return out;
}

void RayTracing::AddFog(std::span<const Rt::FxParticle> world_particles) {
    if (fog_frame != frame) {
        fog_done = fog_frame + 1 == frame ? std::move(fog_filling) : std::vector<Rt::FxParticle>{};
        fog_filling.clear();
        fog_frame = frame;
    }
    for (const Rt::FxParticle& p : world_particles) {
        if (fog_filling.size() >= Rt::FogMaxParticles) {
            break;
        }
        fog_filling.push_back(p);
    }
}

void RayTracing::AddGlows(std::span<const std::array<float, 6>> vertices) {
    constexpr float JoinRadius = 0.6f;
    constexpr size_t MaxGlowLamps = 256;
    struct Cluster {
        std::array<float, 6> sum{};
        float n = 0.0f;
    };
    std::vector<Cluster> clusters;
    for (const auto& v : vertices) {
        Cluster* hit = nullptr;
        for (Cluster& c : clusters) {
            const float dx = c.sum[0] / c.n - v[0], dy = c.sum[1] / c.n - v[1],
                        dz = c.sum[2] / c.n - v[2];
            if (dx * dx + dy * dy + dz * dz <= JoinRadius * JoinRadius) {
                hit = &c;
                break;
            }
        }
        if (!hit) {
            clusters.emplace_back();
            hit = &clusters.back();
        }
        for (size_t k = 0; k < 6; ++k) {
            hit->sum[k] += v[k];
        }
        hit->n += 1.0f;
    }
    for (const Cluster& c : clusters) {
        const std::array<float, 3> world{c.sum[0] / c.n, c.sum[1] / c.n, c.sum[2] / c.n};
        const std::array<float, 3> color{c.sum[3] / c.n, c.sum[4] / c.n, c.sum[5] / c.n};
        GlowLamp* lamp = nullptr;
        for (GlowLamp& g : glow_lamps) {
            const float dx = g.world[0] - world[0], dy = g.world[1] - world[1],
                        dz = g.world[2] - world[2];
            if (dx * dx + dy * dy + dz * dz <= JoinRadius * JoinRadius) {
                lamp = &g;
                break;
            }
        }
        if (!lamp) {
            if (glow_lamps.size() < MaxGlowLamps) {
                lamp = &glow_lamps.emplace_back();
            } else {
                lamp = &*std::min_element(glow_lamps.begin(), glow_lamps.end(),
                                          [](const GlowLamp& a, const GlowLamp& b) {
                                              return a.seen < b.seen;
                                          });
                *lamp = {};
            }
        }
        if (lamp->hits > 0) {
            const float dx = world[0] - lamp->world[0], dy = world[1] - lamp->world[1],
                        dz = world[2] - lamp->world[2];
            lamp->drift += (std::sqrt(dx * dx + dy * dy + dz * dz) - lamp->drift) * 0.1f;
        }
        const float w = 1.0f / float(std::min(lamp->hits, 63u) + 1u);
        for (size_t k = 0; k < 3; ++k) {
            lamp->world[k] += (world[k] - lamp->world[k]) * w;
            lamp->color[k] += (color[k] - lamp->color[k]) * w;
        }
        lamp->seen = frame;
        ++lamp->hits;
    }
}

void RayTracing::TintLampLights() {
    lamp_tinted_last = 0;
    const float hue_luma = 0.2126f * lamp_hue[0] + 0.7152f * lamp_hue[1] + 0.0722f * lamp_hue[2];
    if (!lamp_hue_on || hue_luma <= 0.0f || lamp_hue[0] <= 0.0f) {
        return;
    }
    const float hue_blue = lamp_hue[2] / lamp_hue[0];
    for (Rt::Light& l : direct_lights) {
        if (l.kind != Rt::LightKind::Point || l.camera_fixed || l.held || l.shader_name == nullptr ||
            std::strcmp(l.shader_name, "GlowLamp") == 0) {
            continue;
        }
        const auto& c0 = l.color;
        // Loot glow is yellow-white (red = green): retinting it orange made a fake fire pool.
        const bool warm = Rt::FlameColour(l) && c0[0] >= c0[1] && c0[1] >= c0[2] &&
                          c0[2] < c0[0] * lamp_warm_max && c0[2] > c0[0] * hue_blue * 1.05f;
        if (!warm) {
            continue;
        }
        for (auto* c : {&l.color, &l.spec}) {
            const float luma = 0.2126f * (*c)[0] + 0.7152f * (*c)[1] + 0.0722f * (*c)[2];
            for (u32 k = 0; k < 3; ++k) {
                (*c)[k] = lamp_hue[k] * luma / hue_luma;
            }
        }
        ++lamp_tinted_last;
    }
}

void RayTracing::AddGlowLights(const Rt::Affine* view) {
    glow_added_last = 0;
    halo_spots.clear();
    if (glow_ps.empty() || glow_lamps.empty()) {
        return;
    }
    struct Pick {
        float d2;
        size_t k;
    };
    std::vector<Pick> picks;
    for (size_t k = 0; k < glow_lamps.size(); ++k) {
        const GlowLamp& g = glow_lamps[k];
        if (frame - g.seen > glow_keep ||
            !Rt::GlowLampSteady(g.hits, g.drift, g.color, glow_min_hits, glow_max_drift,
                                glow_warm_max)) {
            continue;
        }
        float d2 = 0.0f;
        if (view) {
            const auto p = Rt::Transform(*view, g.world);
            d2 = p[0] * p[0] + p[1] * p[1] + p[2] * p[2];
            if (d2 > glow_dist * glow_dist) {
                continue;
            }
        }
        halo_spots.push_back(g.world);
        const bool covered = std::any_of(direct_lights.begin(), direct_lights.end(),
                                         [&](const Rt::Light& l) {
            if (l.kind != Rt::LightKind::Point || l.camera_fixed || l.held) {
                return false;
            }
            const float dx = l.world[0] - g.world[0], dy = l.world[1] - g.world[1],
                        dz = l.world[2] - g.world[2];
            return dx * dx + dy * dy + dz * dz <= glow_skip * glow_skip;
        });
        if (!covered) {
            picks.push_back({d2, k});
        }
    }
    std::sort(picks.begin(), picks.end(), [](const Pick& a, const Pick& b) { return a.d2 < b.d2; });
    struct Group {
        std::array<float, 3> sum{};
        float weight = 0.0f;
        std::array<float, 3> color{};
    };
    std::vector<Group> groups;
    for (const Pick& pick : picks) {
        const GlowLamp& g = glow_lamps[pick.k];
        const float w = std::max(g.color[0] + g.color[1] + g.color[2], 1e-3f);
        Group* hit = nullptr;
        for (Group& grp : groups) {
            const float dx = grp.sum[0] / grp.weight - g.world[0],
                        dy = grp.sum[1] / grp.weight - g.world[1],
                        dz = grp.sum[2] / grp.weight - g.world[2];
            if (dx * dx + dy * dy + dz * dz <= glow_merge * glow_merge) {
                hit = &grp;
                break;
            }
        }
        if (!hit) {
            if (groups.size() >= glow_max_lights) {
                continue;
            }
            hit = &groups.emplace_back();
        }
        for (size_t c = 0; c < 3; ++c) {
            hit->sum[c] += g.world[c] * w;
            hit->color[c] = std::max(hit->color[c], g.color[c]);
        }
        hit->weight += w;
    }
    for (size_t i = 0; i < groups.size() && direct_lights.size() < Rt::MaxDirectLights; ++i) {
        const Group& g = groups[i];
        const std::array<float, 3> world{g.sum[0] / g.weight, g.sum[1] / g.weight,
                                         g.sum[2] / g.weight};
        Rt::Light l{};
        l.kind = Rt::LightKind::Point;
        l.has_world = true;
        l.world = world;
        l.position = view ? Rt::Transform(*view, world) : world;
        l.radius = glow_radius;
        for (size_t c = 0; c < 3; ++c) {
            l.color[c] = g.color[c] * glow_tint[c] * glow_strength;
            l.spec[c] = l.color[c] * 0.5f;
        }
        l.spec_power = glow_spec_power;
        l.wrap = {1.0f, 1.0f};
        l.legacy = true;
        l.emitter = true;
        l.lamp = true;
        l.housed = true;
        l.shader_name = "GlowLamp";
        // No game light, so no track: the lamp gets the fixed-flame flicker from its position.
        if (emitter_steady_flicker > 0.0f) {
            const double t =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - flicker_t0).count();
            const float k = std::exp(emitter_steady_flicker *
                                     Rt::FlameNoise(Rt::PositionFlickerId(world), t));
            for (size_t c = 0; c < 3; ++c) {
                l.color[c] *= k;
                l.spec[c] *= k;
            }
        }
        if (Rt::EmitterFalloff(l, glow_range, lamp_source, 0.0f, emitter_peak, glow_flame)) {
            direct_lights.push_back(l);
            ++glow_added_last;
        }
    }
    if (Rt::KnobIs("BB_RT_GLOW_LOG", '1') && frame % 60 == 0) {
        Log("RT glow lamps frame %llu: %zu cached, %zu uncovered, %zu groups, %u added "
            "(BB_RT_GLOW_PS), %u pale warm game lights take BB_RT_LAMP_HUE, scene cull %llu "
            "frames (light camera %.1f m off)\n",
            (unsigned long long)frame, glow_lamps.size(), picks.size(), groups.size(),
            glow_added_last, lamp_tinted_last, (unsigned long long)light_keep_scene_frames,
            light_keep_cam_gap);
        for (size_t i = 0; i < picks.size() && i < 8; ++i) {
            const GlowLamp& g = glow_lamps[picks[i].k];
            Log("  glow world (%.2f %.2f %.2f) cam %.1f colour (%.2f %.2f %.2f) hits %u age %llu drift %.3f\n",
                g.world[0], g.world[1], g.world[2], std::sqrt(picks[i].d2), g.color[0], g.color[1],
                g.color[2], g.hits, (unsigned long long)(frame - g.seen), g.drift);
        }
    }
}

void RayTracing::AddSmoke(std::span<const Rt::FxParticle> particles, const Rt::DrawTag& tag) {
    last_add_accepted = false;
    if (!enabled || !fx_smoke || built_this_frame || !smoke_ring.buffer || particles.empty() ||
        !uv_buffer.buffer) {
        return;
    }
    const u32 room = (Rt::FxSmokeVertices - 1 - smoke_vertex_cursor) / Rt::FxSmokeQuadVertices;
    const u32 count = u32(std::min<size_t>(particles.size(), room));
    if (smoke_draws >= Rt::FxSmokeMaxDraws || count == 0 || instances.size() >= capacity.records) {
        ++fx_smoke_capped;
        return;
    }
    if (!exclude_hashes.empty() && Rt::HashListed(exclude_hashes, tag.ps, tag.vs)) {
        ++excluded_draws;
        return;
    }
    const u32 triangles = count * 2;
    Rt::DrawKey key{};
    key.shader = tag.vs;
    key.geometry = Rt::FxSmokeGeometryKey;
    key.occurrence = smoke_draws;
    auto lookup = cache.Find(key, 0, false, false, triangles);
    Rt::CacheEntry entry{};
    u64 scratch_bytes = 0, omm_scratch_bytes = 0;
    const auto release = [&](const Rt::CacheEntry& e) { ReleaseEntry(e); };
    const bool reuse = Rt::ReuseAllocation(lookup.entry, triangles, false, false) &&
                       !lookup.entry->omm && lookup.entry->uv_base != Rt::NoUv;
    if (reuse) {
        entry = *lookup.entry;
        scratch_bytes = ScratchBytes(triangles, false, false);
    } else {
        if (lookup.entry) {
            cache.Erase(lookup.entry->id, release);
        }
        if (!AllocateEntry(triangles, false, false, entry, scratch_bytes, omm_scratch_bytes)) {
            ++pool_full_draws;
            return;
        }
    }
    if (entry.uv_base == Rt::NoUv) {
        ++fx_smoke_no_uv;
        ReleaseEntry(entry);
        return;
    }
    const u32 slot = u32(frame % Slots);
    u8* base = smoke_ring.mapped + u64(slot) * smoke_ring.slot_bytes;
    const u32 store_base = 1 + smoke_vertex_cursor;
    auto* positions = reinterpret_cast<std::array<float, 4>*>(base + Rt::FxSmokePositionsOffset);
    auto* uvs = reinterpret_cast<u32*>(base + Rt::FxSmokeUvsOffset);
    struct ClipSink {
        std::array<float, 4>* out;
        u32* uv;
        u32 n;
        float xs, ys;
        void push_back(const std::array<float, 3>& v) {
            out[n] = Rt::FxClipFromView(v, xs, ys);
            const auto t = Rt::FxQuadUv(n);
            uv[n] = Rt::PackUv(t[0], t[1]);
            ++n;
        }
    } sink{positions + store_base, uvs + store_base, 0, 1.0f, 1.0f};
    Rt::FxBuildQuads(particles.data(), count, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, sink);
    auto* record = reinterpret_cast<Rt::DrawRecord*>(base) + smoke_draws;
    *record = {
        .triangle_base = smoke_triangles,
        .triangle_count = triangles,
        .store_base = store_base,
        .index_lo = 0,
        .vertex_count = sink.n,
        .prims_per_instance = triangles,
        .index_offset = 0,
        .flags = Rt::FlagHasUv,
        .uv_base = entry.uv_base,
    };
    entry.palette = 0;
    entry.inv_view = Rt::IdentityAffine();
    entry.dirty = true;
    entry.valid = true;
    entry.built_frame = frame;
    entry.uv_captured = true;
    entry.refits = 0;
    entry.blended = true;
    cache.Insert(key, entry);
    if (material.buffer && gi_enabled) {
        material_resets.push_back(entry.id);
    }
    pending.push_back({entry.id, smoke_triangle_base + smoke_triangles, triangles, scratch_bytes,
                       false, false, false, u32(instances.size()), 0});
    instances.push_back({entry.id, entry.inv_view, true, 0, true, 0.5f});
    instances.back().blended = true;
    instances.back().tag = tag;
    instances.back().tag.draw = frame_draws++;
    instances.back().mode = 0;
    ProbeRecord probe{};
    probe.entry = entry.id;
    probe.mode = 2;
    probes.push_back(probe);
    if (entry_tags.size() <= entry.id) {
        entry_tags.resize(entry.id + 1);
    }
    entry_tags[entry.id] = instances.back().tag;
    ++hitch.rebuilds;
    hitch.rebuilt_triangles += triangles;
    ++smoke_draws;
    smoke_triangles += triangles;
    smoke_vertex_cursor += sink.n;
    ++fx_frame_blended;
    ++fx_smoke_draws;
    fx_smoke_quads += count;
    ++alpha_accepted;
    last_add_accepted = true;
    if (Rt::FxSmokeLog() && (fx_smoke_draws <= 8 || fx_smoke_draws % 300 == 0)) {
        std::array<float, 3> lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
        float size_lo = 1e30f, size_hi = 0.0f;
        for (u32 i = 0; i < count; ++i) {
            for (u32 a = 0; a < 3; ++a) {
                lo[a] = std::min(lo[a], particles[i].centre[a]);
                hi[a] = std::max(hi[a], particles[i].centre[a]);
            }
            size_lo = std::min(size_lo, particles[i].size);
            size_hi = std::max(size_hi, particles[i].size);
        }
        std::printf("RT smoke: draw %llu ps %016llx vs %016llx: %u quads (%zu offered), %u "
                    "triangles, %u bytes, entry %u uv_base %u, view centre x %.2f..%.2f y "
                    "%.2f..%.2f z %.2f..%.2f, size %.2f..%.2f\n",
                    (unsigned long long)fx_smoke_draws, (unsigned long long)tag.ps,
                    (unsigned long long)tag.vs, count, particles.size(), triangles,
                    sink.n * 20u, entry.id, entry.uv_base, lo[0], hi[0], lo[1], hi[1], lo[2],
                    hi[2], size_lo, size_hi);
    }
}

void RayTracing::AddDraw(const Rt::DrawInput& draw) {
    last_add_accepted = false;
    if (u32(draw.tag.ps) == Rt::SkyLayerPs) {
        sky_key.Add(frame, draw.index_count, draw.vertex_count);
    }
    if (enabled && built_this_frame && draw.blended) {
        if (fx_held.size() >= fx_cap) {
            ++fx_blended_capped;
            return;
        }
        auto& held = fx_held.emplace_back();
        held.draw = draw;
        if (draw.indices && draw.index_count && draw.index_size) {
            const auto* bytes = static_cast<const u8*>(draw.indices);
            held.indices.assign(bytes, bytes + u64(draw.index_count) * draw.index_size);
            held.draw.indices = held.indices.data();
        }
        ++fx_blended_held;
        return;
    }
    if (enabled && built_this_frame && late_mode && !(draw.depth_only && !depth_only_draws)) {
        if (late_held.size() < capacity.records) {
            late_held.push_back({draw, frame_inv_view, frame_camera_valid});
            // The sky draws after the TLAS build: keep the layer's texture for the replay.
            last_add_late = u32(draw.tag.ps) == Rt::SkyLayerPs;
        } else {
            ++late_dropped;
        }
        return;
    }
    if (!enabled || built_this_frame || (draw.depth_only && !depth_only_draws)) {
        return;
    }
    const u32 draw_index = frame_draws++;
    if (!exclude_hashes.empty() && Rt::HashListed(exclude_hashes, draw.tag.ps, draw.tag.vs)) {
        ++excluded_draws;
        return;
    }
    const auto tag_instance = [&](u32 entry_id, u32 mode) {
        FrameInstance& fi = instances.back();
        fi.tag = draw.tag;
        fi.tag.draw = draw_index;
        fi.mode = mode;
        // Always kept, not only for BB_RT_PICK: a kept off-screen instance takes its tag from here
        // and its TLAS mask from the tag. Without it the moon mesh (SkyLayerPs, 32 triangles about
        // 750 m out, drawn only while in view) stayed in the TLAS as a solid card once the camera
        // turned, right in front of the moon: every moon ray stopped on it and whole fields went
        // black. Mist, water sheets, panes and portals lost their masks the same way.
        if (entry_tags.size() <= entry_id) {
            entry_tags.resize(entry_id + 1);
        }
        entry_tags[entry_id] = fi.tag;
    };
    alpha_offered += draw.alpha_test ? 1u : 0u;
    if (draw.blended && fx_frame_blended >= fx_cap) {
        ++fx_blended_capped;
        return;
    }
    CpuTimer timer{hitch_log, hitch.add};
    ++hitch.adds;
    if (!Rt::FrameGeometry::Valid(draw)) {
        ++geometry.stats.draws;
        ++geometry.stats.invalid;
        return;
    }
    const u64 count = Rt::FrameGeometry::TriangleCount(draw);
    if (count > capacity.triangles || instances.size() >= capacity.records) {
        ++geometry.stats.draws;
        ++geometry.stats.capacity;
        return;
    }
    Rt::DrawKey key = draw.key;
    auto lookup = cache.Find(key, draw.palette, draw.skinned, draw.unreadable, u32(count));
    const Rt::Step step = Rt::PlanStep(lookup, u32(count), rebuild_budget, refit_budget);
    const bool draw_alpha = draw.alpha_test && (skin_alpha || !draw.skinned);
    u64 skin_key = 0;
    if (skin_log) {
        const u64 hashed = u64(Rt::DrawKeyHash{}(key)) | 1ull;
        if (draw.skinned || draw.unreadable || skin_watch.count(hashed) ||
            (lookup.entry && (lookup.entry->updatable || lookup.entry->moving))) {
            skin_key = hashed;
            if (skin_watch.size() < 4096) {
                skin_watch.emplace(hashed, 1);
            }
        }
    }
    if (step == Rt::Step::Defer) {
        ++geometry.stats.draws;
        ++cache.stats.deferred;
        ++hitch.deferred;
        if (skin_key) {
            SkinNote(skin_key, {.entry = lookup.entry ? lookup.entry->id : 0u,
                                .step = 6,
                                .triangles = u32(count),
                                .alpha = draw_alpha,
                                .ps = draw.tag.ps,
                                .vs = draw.tag.vs});
        }
        return;
    }
    if (step == Rt::Step::Hold) {
        ++cache.stats.held;
        ++hitch.held;
    }
    if ((step == Rt::Step::Hit || step == Rt::Step::Hold) && !late_entries.empty() &&
        late_entries.count(lookup.entry->id)) {
        ++late_duplicates;
        last_add_accepted = true;
        return;
    }
    if (step == Rt::Step::Hit || step == Rt::Step::Hold) {
        ++geometry.stats.draws;
        ++geometry.stats.accepted;
        instances.push_back({lookup.entry->id, lookup.entry->inv_view, false, 0, draw_alpha,
                             draw.alpha_ref});
        instances.back().blended = draw.blended;
        tag_instance(lookup.entry->id, 1);
        instances.back().skin_key = skin_key;
        instances.back().skin_step = step == Rt::Step::Hold ? 5u : 4u;
        fx_frame_blended += draw.blended ? 1u : 0u;
        alpha_accepted += draw.alpha_test ? 1u : 0u;
        last_add_accepted = true;
        ProbeRecord probe{};
        probe.slots = Rt::ProbeSlots(draw);
        probe.entry = lookup.entry->id;
        probe.mode = lookup.superset ? 2u : 1u;
        hitch.superset += lookup.superset ? 1u : 0u;
        if (lookup.superset && miss_log && superset_log_lines < 20) {
            ++superset_log_lines;
            Log("RT superset frame %llu: entry %u keeps %u triangles for a draw of %u (vs %016llx "
                "geo %016llx idx %016llx n %u inst %u), updatable %d moving %d dirty %d\n",
                (unsigned long long)frame, lookup.entry->id, lookup.entry->triangles, u32(count),
                (unsigned long long)key.shader, (unsigned long long)key.geometry,
                (unsigned long long)key.indices, key.index_count, draw.instances,
                int(lookup.entry->updatable), int(lookup.entry->moving), int(lookup.entry->dirty));
        }
        probe.pad0 = probe_keep && (lookup.entry->updatable || lookup.entry->moving) ? 1u : 0u;
        // A static mesh that jumps metres is another placement of this draw: its BLAS still
        // stands where it was built, so the probe leaves it in the TLAS (BB_RT_SHELF).
        probe.pad1 = !lookup.entry->skinned && !lookup.entry->moving ? 1u : 0u;
        probes.push_back(probe);
        return;
    }
    if (!geometry.Add(draw)) {
        return;
    }
    const u32 triangle_base = geometry.Triangles() - u32(count);
    Rt::CacheEntry entry{};
    u64 scratch_bytes = 0, omm_scratch_bytes = 0;
    bool update = false;
    const auto release = [&](const Rt::CacheEntry& e) { ReleaseEntry(e); };
    const bool omm_kind = draw.alpha_test && draw.uv_captured &&
                          (!lookup.updatable || (omm_skinned && omm_index_buffer.buffer));
    const bool omm_room = omm_enabled && omm_used + count <= omm_budget;
    const bool omm_wanted = omm_room && omm_kind;
    const bool reuse =
        step == Rt::Step::Refit ||
        (Rt::ReuseAllocation(lookup.entry, u32(count), lookup.updatable, omm_room) &&
         (!lookup.updatable || lookup.entry->omm == omm_wanted));
    if (reuse) {
        entry = *lookup.entry;
        update = step == Rt::Step::Refit;
        if (update) {
            ++cache.stats.refits;
            if (!lookup.refit) {
                ++cache.stats.budget_refits;
            }
        }
        scratch_bytes = ScratchBytes(u32(count), entry.updatable, update, entry.omm ? entry.id : ~0u);
        if (entry.omm && !update) {
            omm_scratch_bytes = OmmScratchBytes(u32(count));
        }
    } else {
        const bool was_moving = lookup.entry && lookup.entry->moving;
        if (lookup.entry) {
            cache.Erase(lookup.entry->id, release);
        }
        if (!AllocateEntry(u32(count), lookup.updatable, omm_wanted, entry, scratch_bytes,
                           omm_scratch_bytes)) {
            geometry.PopLast();
            ++pool_full_draws;
            return;
        }
        entry.moving = was_moving;
    }
    if (entry.omm && !update) {
        omm_used += u32(count);
        ++cache.stats.omm_built;
        cache.stats.omm_triangles += count;
        omm_skinned_built += entry.updatable ? 1u : 0u;
    } else if (entry.omm) {
        omm_skinned_refits += entry.omm_ready ? 1u : 0u;
    } else if (omm_enabled && omm_kind && !omm_room) {
        ++omm_over_budget;
    }
    entry.refits = Rt::RefitsAfter(step, lookup, entry.id, cache.max_refits);
    if (update) {
        ++hitch.refits;
        hitch.refit_triangles += count;
        hitch.skeleton += lookup.reason == Rt::Rebuild::Skeleton ? 1u : 0u;
    } else {
        ++hitch.rebuilds;
        hitch.rebuilt_triangles += count;
        hitch.misses += lookup.reason == Rt::Rebuild::Miss ? 1u : 0u;
        hitch.shifted += lookup.reason == Rt::Rebuild::Miss && lookup.shifted ? 1u : 0u;
        hitch.dirty += lookup.reason == Rt::Rebuild::Dirty ? 1u : 0u;
        ++hitch.reasons[std::min<size_t>(size_t(lookup.reason), hitch.reasons.size() - 1)];
        if (lookup.reason == Rt::Rebuild::Size && miss_log && frame > 600 && rebuild_log_lines < 40) {
            ++rebuild_log_lines;
            Log("RT rebuild frame %llu reason %u (2 size, 3 dirty, 4 unreadable, 5 skeleton, 6 "
                "palette): entry %u had %u triangles, draw %u (inst %u), skinned %d unreadable %d "
                "palette %016llx/%016llx updatable %d moving %d dirty %d refit %d can_refit %d "
                "limit %d\n",
                (unsigned long long)frame, u32(lookup.reason), lookup.entry ? lookup.entry->id : 0u,
                lookup.entry ? lookup.entry->triangles : 0u, u32(count), draw.instances,
                int(draw.skinned), int(draw.unreadable), (unsigned long long)draw.palette,
                (unsigned long long)(lookup.entry ? lookup.entry->palette : 0),
                int(lookup.entry && lookup.entry->updatable),
                int(lookup.entry && lookup.entry->moving), int(lookup.entry && lookup.entry->dirty),
                int(lookup.refit), int(lookup.can_refit), int(lookup.limit));
        }
        if (lookup.reason == Rt::Rebuild::Miss && miss_log && frame > 600 && miss_log_lines < 40) {
            Rt::DrawKey match{};
            const u32 kind = cache.ClassifyMiss(key, match);
            ++hitch.miss_kind[std::min<u32>(kind, 4)];
            if (kind && miss_log_lines < 40) {
                ++miss_log_lines;
                Log("RT miss frame %llu kind %u (1 same buffers+shader, 2 same buffers other "
                    "shader, 3 same shader+indices other vertices, 4 same shader+counts other "
                    "buffers): new vs %016llx geo %016llx idx %016llx n %u inst %u fi %u fv %u "
                    "vc %u occ %u | had vs %016llx geo %016llx idx %016llx n %u inst %u fi %u fv "
                    "%u vc %u occ %u\n",
                    (unsigned long long)frame, kind, (unsigned long long)key.shader,
                    (unsigned long long)key.geometry, (unsigned long long)key.indices,
                    key.index_count, draw.instances, key.first_instance, key.first_vertex,
                    key.vertex_count, key.occurrence, (unsigned long long)match.shader,
                    (unsigned long long)match.geometry, (unsigned long long)match.indices,
                    match.index_count, match.instances, match.first_instance, match.first_vertex,
                    match.vertex_count, match.occurrence);
            }
        }
        if (material.buffer && gi_enabled) {
            material_resets.push_back(entry.id);
        }
    }
    const bool anchored = update && refit_anchor && entry.view_valid;
    entry.palette = draw.palette;
    entry.skinned = draw.skinned;
    entry.instances = std::max(entry.instances, draw.instances);
    entry.inv_view = anchored ? entry.inv_view : Rt::IdentityAffine();
    entry.view_valid = anchored;
    entry.dirty = true;
    entry.valid = true;
    entry.built_frame = frame;
    entry.uv_captured = draw.uv_captured;
    geometry.SetLastUvBase(entry.uv_base, draw.uv_captured, draw.normal_captured);
    if (!update) {
        entry.omm_ready = false;
    }
    cache.Insert(key, entry);
    pending.push_back({entry.id, triangle_base, u32(count), scratch_bytes, update, entry.updatable,
                       entry.omm && !update, u32(instances.size()), omm_scratch_bytes,
                       geometry.Records() - 1, anchored});
    instances.push_back({entry.id, entry.inv_view, true, 0, draw_alpha, draw.alpha_ref});
    instances.back().blended = draw.blended;
    instances.back().anchored = anchored;
    tag_instance(entry.id, 0);
    instances.back().skin_key = skin_key;
    instances.back().skin_step = update ? 3u
                                 : lookup.reason == Rt::Rebuild::Miss  ? 0u
                                 : lookup.reason == Rt::Rebuild::Dirty ? 1u
                                                                       : 2u;
    fx_frame_blended += draw.blended ? 1u : 0u;
    alpha_accepted += draw.alpha_test ? 1u : 0u;
    last_add_accepted = true;
    ProbeRecord probe{};
    probe.slots = Rt::ProbeSlots(draw);
    probe.entry = entry.id;
    probe.mode = 0;
    probes.push_back(probe);
}

bool RayTracing::Build(const SceneInputs& inputs) {
    if (built_this_frame) {
        return true;
    }
    if (instances.empty()) {
        return false;
    }
    CpuTimer timer{hitch_log, hitch.build};
    const std::array<float, 2> scale{inputs.projection[0], inputs.projection[1]};
    if (scale[0] == 0.0f || scale[1] == 0.0f) {
        return false;
    }
    frame_camera_valid = inputs.camera_valid;
    frame_inv_view = inputs.inv_view;
    frame_proj_scale = scale;
    const u32 slot = u32(frame % Slots);
    const u32 triangles = geometry.Triangles();
    if (auto* profiler = GpuProfiler::Get()) {
        profiler->Mark(0xB1A5'0001ull, [] { return std::string{"ray tracing build"}; });
    }
    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();
    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;
    const auto build_stages = omm_pipeline ? Stage::eComputeShader | Stage::eAccelerationStructureBuildKHR |
                                                 Stage::eMicromapBuildEXT
                                           : Stage::eComputeShader | Stage::eAccelerationStructureBuildKHR;
    const auto build_access = omm_pipeline ? Access::eShaderRead | Access::eShaderWrite |
                                                 Access::eAccelerationStructureReadKHR |
                                                 Access::eAccelerationStructureWriteKHR |
                                                 Access::eMicromapReadEXT | Access::eMicromapWriteEXT
                                           : Access::eShaderRead | Access::eShaderWrite |
                                                 Access::eAccelerationStructureReadKHR |
                                                 Access::eAccelerationStructureWriteKHR;
    Barrier(cmdbuf, Stage::eVertexShader | build_stages, build_access, build_stages, build_access);

    if (time_pool) {
        cmdbuf.resetQueryPool(*time_pool, slot * TimePasses * 2, TimePasses * 2);
        time_written[slot] = 0;
    }
    TimeBegin(cmdbuf, 0);
    for (PendingBuild& build : pending) {
        if (!build.anchored) {
            continue;
        }
        FrameInstance& fi = instances[build.instance_index];
        if (!frame_camera_valid) {
            build.anchored = fi.anchored = false;
            continue;
        }
        const Rt::Affine rebase = Rt::Reprojection(fi.inv_view, frame_inv_view);
        geometry.SetRebase(build.record, rebase);
        const float cos_turn = std::clamp((rebase[0] + rebase[5] + rebase[10] - 1.0f) * 0.5f, -1.0f, 1.0f);
        const float turn = std::acos(cos_turn) * 57.2957795f;
        ++hitch.anchored;
        ++anchored_refits;
        hitch.anchor_turn = std::max(hitch.anchor_turn, turn);
        anchor_turn_max = std::max(anchor_turn_max, turn);
    }
    if (triangles || smoke_triangles) {
        const vk::DescriptorBufferInfo records_info{
            records_ring.buffer, u64(slot) * records_ring.slot_bytes, records_ring.slot_bytes};
        const vk::DescriptorBufferInfo indices_info{
            indices_ring.buffer, u64(slot) * indices_ring.slot_bytes, indices_ring.slot_bytes};
        const vk::DescriptorBufferInfo soup_info{soup.buffer, 0, soup.size};
        const vk::DescriptorBufferInfo uv_info{uv_buffer.buffer ? uv_buffer.buffer : soup.buffer, 0,
                                              uv_buffer.buffer ? uv_buffer.size : soup.size};
        const std::array<vk::WriteDescriptorSet, 4> gather_writes = {{
            {.dstBinding = 0,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eStorageBuffer,
             .pBufferInfo = &records_info},
            {.dstBinding = 1,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eStorageBuffer,
             .pBufferInfo = &indices_info},
            {.dstBinding = 2,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eStorageBuffer,
             .pBufferInfo = &soup_info},
            {.dstBinding = 3,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eStorageBuffer,
             .pBufferInfo = &uv_info},
        }};
        const GatherConstants gather_push{
            .positions_address = positions_address,
            .record_count = geometry.Records(),
            .triangle_count = triangles,
            .xs = scale[0],
            .ys = scale[1],
            .uvs_address = uv_buffer.buffer ? uvs_address : 0,
            .normals_address = uv_buffer.buffer ? normals_address : 0,
            .pad = 0,
            .view_rows = Rt::Rows(Rt::InverseRigid(
                frame_camera_valid ? frame_inv_view : Rt::IdentityAffine())),
        };
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *gather_pipeline);
        if (triangles) {
            cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *gather_pipeline_layout,
                                        0, gather_writes);
            cmdbuf.pushConstants(*gather_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
                                 sizeof(gather_push), &gather_push);
            CrumbBegin(cmdbuf, Rt::Crumb::Gather);
            cmdbuf.dispatch((triangles + 63) / 64, 1, 1);
            CrumbEnd(cmdbuf, Rt::Crumb::Gather);
        }
        if (smoke_triangles) {
            const u64 smoke_slot = u64(slot) * smoke_ring.slot_bytes;
            const vk::DescriptorBufferInfo smoke_records_info{smoke_ring.buffer, smoke_slot,
                                                              Rt::FxSmokePositionsOffset};
            const vk::DescriptorBufferInfo smoke_soup_info{
                soup.buffer, u64(smoke_triangle_base) * Rt::TriangleBytes,
                u64(Rt::FxSmokeMaxTriangles) * Rt::TriangleBytes};
            const std::array<vk::WriteDescriptorSet, 4> smoke_writes = {{
                {.dstBinding = 0,
                 .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageBuffer,
                 .pBufferInfo = &smoke_records_info},
                {.dstBinding = 1,
                 .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageBuffer,
                 .pBufferInfo = &indices_info},
                {.dstBinding = 2,
                 .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageBuffer,
                 .pBufferInfo = &smoke_soup_info},
                {.dstBinding = 3,
                 .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageBuffer,
                 .pBufferInfo = &uv_info},
            }};
            const GatherConstants smoke_push{
                .positions_address = smoke_ring.address + smoke_slot + Rt::FxSmokePositionsOffset,
                .record_count = smoke_draws,
                .triangle_count = smoke_triangles,
                .xs = 1.0f,
                .ys = 1.0f,
                .uvs_address = uv_buffer.buffer ? smoke_ring.address + smoke_slot + Rt::FxSmokeUvsOffset
                                                : 0,
                .normals_address = 0,
                .pad = 0,
                .view_rows = gather_push.view_rows,
            };
            cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *gather_pipeline_layout,
                                        0, smoke_writes);
            cmdbuf.pushConstants(*gather_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
                                 sizeof(smoke_push), &smoke_push);
            CrumbBegin(cmdbuf, Rt::Crumb::Gather);
            cmdbuf.dispatch((smoke_triangles + 63) / 64, 1, 1);
            CrumbEnd(cmdbuf, Rt::Crumb::Gather);
        }
        Barrier(cmdbuf, Stage::eComputeShader, Access::eShaderWrite,
                Stage::eAccelerationStructureBuildKHR | Stage::eComputeShader,
                Access::eShaderRead);

        std::vector<vk::AccelerationStructureTrianglesOpacityMicromapEXT> omm_chains;
        std::vector<vk::MicromapUsageEXT> omm_usages;
        std::vector<u64> omm_index_addresses(pending.size(), 0);
        omm_chains.reserve(pending.size());
        omm_usages.reserve(pending.size() * 2);
        if (omm_pipeline) {
            std::vector<vk::MicromapBuildInfoEXT> mm_infos;
            const u64 input_base = omm_input.address + u64(slot) * omm_input_slot_bytes;
            const u64 input_offset = u64(slot) * omm_input_slot_bytes;
            u64 cursor = 0;
            bool bound = false;
            std::vector<vk::BufferCopy> index_copies;
            for (size_t i = 0; i < pending.size(); ++i) {
                PendingBuild& build = pending[i];
                if (!build.omm) {
                    continue;
                }
                Rt::CacheEntry* e = cache.ById(build.entry_id);
                const auto layout = OmmLayout(cursor, build.triangles, omm_level);
                if (!e || !e->omm || e->uv_base == Rt::NoUv || layout.end > omm_input_slot_bytes ||
                    !omm_handles[build.entry_id]) {
                    if (omm_log && e && e->omm) {
                        Log("OMM dropped frame %llu: entry %u, %u triangles, uv %u, input end %llu "
                            "of %llu, micromap %d\n",
                            (unsigned long long)frame, build.entry_id, build.triangles, e->uv_base,
                            (unsigned long long)layout.end, (unsigned long long)omm_input_slot_bytes,
                            int(bool(omm_handles[build.entry_id])));
                    }
                    build.omm = false;
                    continue;
                }
                cursor = layout.end;
                if (!bound) {
                    const vk::DescriptorBufferInfo uv_pool_info{uv_buffer.buffer, 0, uv_buffer.size};
                    const vk::DescriptorBufferInfo out_info{omm_input.buffer, input_offset,
                                                            omm_input_slot_bytes};
                    const std::array<vk::WriteDescriptorSet, 2> omm_writes = {{
                        {.dstBinding = 0,
                         .descriptorCount = 1,
                         .descriptorType = vk::DescriptorType::eStorageBuffer,
                         .pBufferInfo = &uv_pool_info},
                        {.dstBinding = 1,
                         .descriptorCount = 1,
                         .descriptorType = vk::DescriptorType::eStorageBuffer,
                         .pBufferInfo = &out_info},
                    }};
                    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *omm_pipeline);
                    cmdbuf.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
                                              *omm_pipeline_layout, 1, SurfaceSet(cmdbuf, slot), {});
                    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute,
                                                *omm_pipeline_layout, 0, omm_writes);
                    bound = true;
                }
                const FrameInstance& fi = instances[build.instance_index];
                const OmmConstants omm_push{
                    .uv_base = e->uv_base,
                    .triangle_count = build.triangles,
                    .data_word_offset = u32(layout.data / 4),
                    .record_word_offset = u32(layout.records / 4),
                    .index_word_offset = u32(layout.indices / 4),
                    // The any-hit alpha test reads the alpha texture (else the albedo) in the
                    // draw's alpha channel; the bake used the albedo's .a, so on draws with a
                    // separate mask or channel the micromap marked micro-triangles solid or open
                    // from the wrong data and shadows came out in blocks.
                    .texture_slot = fi.alpha_texture ? fi.alpha_texture : fi.texture,
                    .level = omm_level,
                    .alpha_ref = fi.alpha_ref,
                    .alpha_channel = u32(fi.alpha_channel) & 3u,
                };
                cmdbuf.pushConstants(*omm_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
                                     sizeof(omm_push), &omm_push);
                CrumbBegin(cmdbuf, Rt::Crumb::OmmBake);
                cmdbuf.dispatch((build.triangles + 63) / 64, 1, 1);
                CrumbEnd(cmdbuf, Rt::Crumb::OmmBake);
                omm_usages.push_back(OmmUsage(build.triangles, omm_level));
                mm_infos.push_back(vk::MicromapBuildInfoEXT{
                    .type = vk::MicromapTypeEXT::eOpacityMicromap,
                    .flags = vk::BuildMicromapFlagBitsEXT::ePreferFastTrace,
                    .mode = vk::BuildMicromapModeEXT::eBuild,
                    .dstMicromap = *omm_handles[build.entry_id],
                    .usageCountsCount = 1,
                    .pUsageCounts = &omm_usages.back(),
                    .data = {.deviceAddress = input_base + layout.data},
                    .triangleArray = {.deviceAddress = input_base + layout.records},
                    .triangleArrayStride = Rt::OmmTriangleRecordBytes,
                });
                omm_index_addresses[i] = input_base + layout.indices;
                if (e->omm_index_bytes) {
                    index_copies.push_back({input_offset + layout.indices, e->omm_index_offset,
                                            u64(build.triangles) * Rt::OmmIndexBytes});
                    omm_index_addresses[i] = omm_index_buffer.address + e->omm_index_offset;
                }
                e->omm_ready = true;
                if (omm_log) {
                    Log("OMM build frame %llu: entry %u, %u triangles, uv %u, texture %u, alpha_ref "
                        "%.3f, pool %llu+%llu, scratch %llu, BLAS scratch %llu, BLAS %llu+%llu\n",
                        (unsigned long long)frame, build.entry_id, build.triangles, e->uv_base,
                        fi.texture, fi.alpha_ref, (unsigned long long)e->omm_offset,
                        (unsigned long long)e->omm_bytes,
                        (unsigned long long)build.omm_scratch_bytes,
                        (unsigned long long)build.scratch_bytes, (unsigned long long)e->offset,
                        (unsigned long long)e->bytes);
                }
            }
            if (!index_copies.empty()) {
                Barrier(cmdbuf, Stage::eComputeShader, Access::eShaderWrite, Stage::eTransfer,
                        Access::eTransferRead);
                cmdbuf.copyBuffer(omm_input.buffer, omm_index_buffer.buffer, index_copies);
                Barrier(cmdbuf, Stage::eTransfer, Access::eTransferWrite,
                        Stage::eAccelerationStructureBuildKHR, Access::eShaderRead);
            }
            if (!mm_infos.empty()) {
                Barrier(cmdbuf, Stage::eComputeShader, Access::eShaderWrite,
                        Stage::eMicromapBuildEXT | Stage::eAccelerationStructureBuildKHR,
                        Access::eShaderRead | Access::eMicromapReadEXT);
                size_t first = 0;
                u64 used = 0;
                size_t k = 0;
                for (size_t i = 0; i < pending.size(); ++i) {
                    if (!pending[i].omm) {
                        continue;
                    }
                    if (used + pending[i].omm_scratch_bytes > scratch.size) {
                        CrumbBegin(cmdbuf, Rt::Crumb::OmmBuild);
                        cmdbuf.buildMicromapsEXT(u32(k - first), mm_infos.data() + first);
                        CrumbEnd(cmdbuf, Rt::Crumb::OmmBuild);
                        Barrier(cmdbuf, Stage::eMicromapBuildEXT, Access::eMicromapWriteEXT,
                                Stage::eMicromapBuildEXT,
                                Access::eMicromapReadEXT | Access::eMicromapWriteEXT);
                        first = k;
                        used = 0;
                    }
                    mm_infos[k].scratchData = {.deviceAddress = scratch.address + used};
                    used += pending[i].omm_scratch_bytes;
                    ++k;
                }
                CrumbBegin(cmdbuf, Rt::Crumb::OmmBuild);
                cmdbuf.buildMicromapsEXT(u32(k - first), mm_infos.data() + first);
                CrumbEnd(cmdbuf, Rt::Crumb::OmmBuild);
                Barrier(cmdbuf, Stage::eMicromapBuildEXT, Access::eMicromapWriteEXT,
                        Stage::eAccelerationStructureBuildKHR,
                        Access::eMicromapReadEXT | Access::eShaderRead |
                            Access::eAccelerationStructureWriteKHR);
            }
        }

        std::vector<vk::AccelerationStructureGeometryKHR> geometries;
        std::vector<vk::AccelerationStructureBuildGeometryInfoKHR> infos;
        std::vector<vk::AccelerationStructureBuildRangeInfoKHR> ranges;
        std::vector<const vk::AccelerationStructureBuildRangeInfoKHR*> range_pointers;
        geometries.reserve(pending.size());
        infos.reserve(pending.size());
        ranges.reserve(pending.size());
        range_pointers.reserve(pending.size());
        const auto flush = [&] {
            if (infos.empty()) {
                return;
            }
            for (size_t i = 0; i < infos.size(); ++i) {
                infos[i].pGeometries = &geometries[i];
                range_pointers[i] = &ranges[i];
            }
            CrumbBegin(cmdbuf, Rt::Crumb::BlasBuild);
            cmdbuf.buildAccelerationStructuresKHR(u32(infos.size()), infos.data(),
                                                  range_pointers.data());
            CrumbEnd(cmdbuf, Rt::Crumb::BlasBuild);
            Barrier(cmdbuf, Stage::eAccelerationStructureBuildKHR,
                    Access::eAccelerationStructureWriteKHR | Access::eAccelerationStructureReadKHR,
                    Stage::eAccelerationStructureBuildKHR,
                    Access::eAccelerationStructureReadKHR | Access::eAccelerationStructureWriteKHR);
            geometries.clear();
            infos.clear();
            ranges.clear();
            range_pointers.clear();
        };
        u64 scratch_used = 0;
        for (size_t i = 0; i < pending.size(); ++i) {
            const PendingBuild& build = pending[i];
            if (scratch_used + build.scratch_bytes > scratch.size) {
                flush();
                scratch_used = 0;
            }
            const auto input = Rt::BlasInputFor(build.triangles);
            geometries.push_back(SoupGeometry(
                soup.address + u64(build.triangle_base) * Rt::TriangleBytes, input.max_vertex));
            if (build.update && build.updatable && omm_index_buffer.buffer &&
                build.entry_id < omm_handles.size() && omm_handles[build.entry_id]) {
                const Rt::CacheEntry* e = cache.ById(build.entry_id);
                if (e && e->omm && e->omm_ready && e->omm_index_bytes) {
                    omm_index_addresses[i] = omm_index_buffer.address + e->omm_index_offset;
                }
            }
            if (omm_index_addresses[i] && omm_handles[build.entry_id]) {
                omm_usages.push_back(OmmUsage(build.triangles, omm_level));
                omm_chains.push_back(vk::AccelerationStructureTrianglesOpacityMicromapEXT{
                    .indexType = vk::IndexType::eUint32,
                    .indexBuffer = {.deviceAddress = omm_index_addresses[i]},
                    .indexStride = Rt::OmmIndexBytes,
                    .usageCountsCount = 1,
                    .pUsageCounts = &omm_usages.back(),
                    .micromap = *omm_handles[build.entry_id],
                });
                geometries.back().geometry.triangles.pNext = &omm_chains.back();
            }
            auto info = BlasInfo(nullptr, Rt::BlasBuildFlags(build.updatable, compact_enabled, skin_fast_trace));
            info.dstAccelerationStructure = *entry_handles[build.entry_id];
            if (build.update) {
                info.mode = vk::BuildAccelerationStructureModeKHR::eUpdate;
                info.srcAccelerationStructure = info.dstAccelerationStructure;
            }
            info.scratchData = {.deviceAddress = scratch.address + scratch_used};
            infos.push_back(info);
            ranges.push_back({.primitiveCount = input.primitives});
            range_pointers.push_back(nullptr);
            scratch_used += build.scratch_bytes;
        }
        flush();
        if (compact_pool) {
            std::vector<vk::AccelerationStructureKHR> handles;
            auto& list = slot_compact[slot];
            list.clear();
            for (const PendingBuild& build : pending) {
                if (!build.update && !build.updatable && handles.size() < capacity.records) {
                    handles.push_back(*entry_handles[build.entry_id]);
                    list.push_back({build.entry_id, frame});
                }
            }
            if (!handles.empty()) {
                cmdbuf.resetQueryPool(*compact_pool, slot * capacity.records, u32(handles.size()));
                cmdbuf.writeAccelerationStructuresPropertiesKHR(
                    handles, vk::QueryType::eAccelerationStructureCompactedSizeKHR, *compact_pool,
                    slot * capacity.records);
            }
        }
    }

    const Rt::Affine current_inv_view =
        frame_camera_valid ? frame_inv_view : Rt::IdentityAffine();
    if (off_switches.instances) {
        AppendKeptInstances(current_inv_view);
    }
    GlassMaskSlots(cmdbuf);
    auto* instance_data = reinterpret_cast<VkAccelerationStructureInstanceKHR*>(
        instances_ring.mapped + u64(slot) * instances_ring.slot_bytes);
    u64 unresolved_count = 0;
    flips_last = 0;
    portal_instances = 0;
    water_sheets = 0;
    water_puddles = 0;
    if (pick_enabled) {
        ReadPick();
        pick_snap[slot].clear();
        pick_snap_frame[slot] = frame;
    }
    if (kept_probe) {
        ReadKeptProbe();
        kept_snap[slot].assign(instances.size(), ~0u);
        kept_snap_frame[slot] = frame;
    }
    const auto upload_start = std::chrono::steady_clock::now();
    FlipTlasMode();
    if (ptlas_on) {
        ptlas.Begin();
        for (const PendingBuild& build : pending) {
            ptlas.MarkBuilt(build.entry_id);
        }
    }
    for (size_t i = 0; i < instances.size(); ++i) {
        FrameInstance& fi = instances[i];
        ProbeRecord& probe = probes[i];
        const bool untextured_smoke = fi.blended && fi.tag.prim == Rt::FxPrimPatch &&
                                      !fi.texture && !fi.alpha_texture;
        const bool hidden = (!fi.fresh && !frame_camera_valid) || untextured_smoke;
        if (fi.fresh) {
            fi.inv_view = fi.anchored ? fi.inv_view : current_inv_view;
            if (Rt::CacheEntry* e = cache.ById(fi.entry_id)) {
                e->inv_view = fi.inv_view;
                e->dirty = !frame_camera_valid;
                e->view_valid = frame_camera_valid;
            }
        }
        if (hidden) {
            probe.mode = 2;
            ++camera_masked;
        }
        const Rt::Affine transform =
            hidden ? Rt::IdentityAffine() : Rt::InstanceTransform(current_inv_view, fi.inv_view);
        VkAccelerationStructureInstanceKHR data{};
        std::memcpy(&data.transform.matrix[0][0], transform.data(), sizeof(float) * 12);
        data.instanceCustomIndex = fi.entry_id & 0xFFFFFF;
        const Rt::CacheEntry* entry = cache.ById(fi.entry_id);
        const bool unresolved =
            surface_enabled && !fi.blended &&
            Rt::AlphaUnresolved(fi.alpha_test, fi.texture || fi.alpha_texture, entry && entry->uv_captured);
        unresolved_count += unresolved && !hidden ? 1u : 0u;
        data.mask = Rt::InstanceMask(hidden, fi.blended, unresolved, alpha_skip_unresolved,
                                     fi.kept && !keep_gi,
                                     PortalDraw(fi.tag.ps, entry ? entry->triangles : 0u),
                                     WaterSheet(fi.tag.ps));
        if (Rt::HashListed(sky_ps, fi.tag.ps, 0) ||
            ((fi.tag.ps == Rt::FxMistShader || fi.tag.ps == Rt::WaterLayerShader) && !WaterSheet(fi.tag.ps))) {
            // The moon and cloud layer: only the sky layer pass traces it.
            data.mask = u32(fi.tag.ps) == Rt::SkyLayerPs && !hidden ? Rt::MaskSky : 0u;
        }
        const bool alpha_instance = surface_enabled && fi.alpha_test && (fi.texture || fi.alpha_texture);
        const Rt::CacheEntry* omm_entry = alpha_instance ? entry : nullptr;
        const bool omm_instance = omm_entry && omm_entry->omm && omm_entry->omm_ready;
        // Glass panes go through the any-hit loops, which let rays pass them (rt_gi/rt_direct).
        const bool glass_instance =
            glass_pass && (fi.glass_mask || GlassDraw(fi.tag.ps, entry ? entry->triangles : 0u));
        portal_instances += !hidden && PortalDraw(fi.tag.ps, entry ? entry->triangles : 0u) ? 1u : 0u;
        water_sheets += !hidden && data.mask == Rt::MaskWater ? 1u : 0u;
        water_puddles += !hidden && data.mask != 0 && data.mask != Rt::MaskWater && WaterDraw(fi.tag.ps) ? 1u : 0u;
        data.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR |
                     (omm_instance && !glass_instance ? 0u
                      : alpha_instance || fi.blended || glass_instance ? u32(VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR)
                                       : u32(VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR));
        data.accelerationStructureReference = entry_addresses[fi.entry_id];
        if (ptlas_on) {
            const bool dynamic = fi.fresh || (entry && (entry->updatable || entry->moving));
            const Rt::PtlasCenter center =
                !dynamic && fi.kept ? ptlas.Center(fi.entry_id) : Rt::PtlasCenter{};
            ptlas.Add(fi.entry_id, fi.inv_view, data,
                      Rt::PtlasPartition(dynamic, fi.kept, fi.inv_view, center));
        } else {
            instance_data[i] = data;
            if (kept_probe && fi.kept && !hidden && i < kept_snap[slot].size()) {
                kept_snap[slot][i] = fi.entry_id;
            }
        }
        NoteInstanceState(fi.entry_id, hidden ? 3u : std::min(fi.mode, 2u), fi.tag.ps,
                          Rt::InstanceState(fi.alpha_test, fi.texture || fi.alpha_texture, data.mask),
                          unresolved && !hidden);
        if (skin_log) {
            if (!fi.skin_key && fi.mode == 2) {
                const auto it = skin_entry_keys.find(fi.entry_id);
                if (it != skin_entry_keys.end()) {
                    fi.skin_key = it->second;
                    fi.skin_step = 7;
                }
            }
            if (fi.skin_key) {
                SkinNote(fi.skin_key, {.entry = fi.entry_id,
                                       .mode = hidden ? 3u : std::min(fi.mode, 2u),
                                       .step = fi.skin_step,
                                       .mask = data.mask,
                                       .flags = data.flags,
                                       .slot = fi.texture,
                                       .triangles = entry ? entry->triangles : 0u,
                                       .present = data.mask != 0,
                                       .alpha = fi.alpha_test,
                                       .textured = fi.texture || fi.alpha_texture,
                                       .ps = fi.tag.ps,
                                       .vs = fi.tag.vs,
                                       .view = fi.view});
            }
        }
        probe.rows = Rt::Rows(Rt::InverseRigid(transform));
        if (pick_enabled) {
            pick_snap[slot].push_back({.entry_id = fi.entry_id,
                                       .mode = hidden ? 3u : fi.mode,
                                       .texture = fi.texture,
                                       .alpha_texture = fi.alpha_texture,
                                       .mask = data.mask,
                                       .flags = data.flags,
                                       .triangles = entry ? entry->triangles : 0u,
                                       .alpha_test = fi.alpha_test,
                                       .blended = fi.blended,
                                       .uv = entry && entry->uv_captured,
                                       .omm = omm_instance,
                                       .tag = fi.tag});
        }
    }
    if (ptlas_on) {
        ptlas.Plan(slot);
        tlas_writes_sum += ptlas.Writes();
        tlas_refit_writes_sum += ptlas.Refits();
        if (pick_enabled) {
            std::vector<PickInstance> by_index;
            const auto indices = ptlas.Indices();
            for (size_t i = 0; i < indices.size() && i < pick_snap[slot].size(); ++i) {
                if (indices[i] == Rt::PtlasNoIndex) {
                    continue;
                }
                if (by_index.size() <= indices[i]) {
                    by_index.resize(indices[i] + 1);
                }
                by_index[indices[i]] = pick_snap[slot][i];
            }
            pick_snap[slot] = std::move(by_index);
        }
    } else {
        tlas_writes_sum += instances.size();
    }
    tlas_upload_ms_sum +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - upload_start)
            .count();
    ++tlas_upload_frames;
    flips_total += flips_last;
    SkinFrame();
    alpha_unresolved_last = unresolved_count;
    alpha_unresolved_total += unresolved_count;
    std::memcpy(probes_ring.mapped + u64(slot) * probes_ring.slot_bytes, probes.data(),
                probes.size() * sizeof(ProbeRecord));
    auto& slot_entries = slot_probe_entries[slot];
    slot_entries.resize(probes.size());
    auto& keep_flags = slot_skin_keep[slot];
    keep_flags.resize(probes.size());
    auto& probe_views = slot_probe_views[slot];
    auto& probe_modes = slot_probe_modes[slot];
    probe_views.resize(probes.size());
    probe_modes.resize(probes.size());
    for (size_t i = 0; i < probes.size(); ++i) {
        slot_entries[i] = probes[i].entry;
        keep_flags[i] = u8(probes[i].pad0 != 0);
        probe_views[i] = i < instances.size() ? instances[i].inv_view : Rt::IdentityAffine();
        probe_modes[i] = u8(probes[i].mode);
    }
    auto& skin_keys = slot_skin_keys[slot];
    skin_keys.assign(skin_log ? probes.size() : 0u, 0);
    for (size_t i = 0; i < skin_keys.size() && i < instances.size(); ++i) {
        skin_keys[i] = instances[i].skin_key;
    }
    slot_probe_frames[slot] = frame;
    WriteSurfaces(slot);

    const vk::DescriptorBufferInfo probes_info{
        probes_ring.buffer, u64(slot) * probes_ring.slot_bytes, probes_ring.slot_bytes};
    const vk::DescriptorBufferInfo fingerprints_info{fingerprints.buffer, 0, fingerprints.size};
    const vk::DescriptorBufferInfo flags_info{
        flags_ring.buffer, u64(slot) * flags_ring.slot_bytes, flags_ring.slot_bytes};
    const vk::DescriptorBufferInfo instance_info =
        ptlas_on ? ptlas.ProbeInfo(slot)
                 : vk::DescriptorBufferInfo{instances_ring.buffer,
                                            u64(slot) * instances_ring.slot_bytes,
                                            instances_ring.slot_bytes};
    const std::array<vk::WriteDescriptorSet, 4> probe_writes = {{
        {.dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageBuffer,
         .pBufferInfo = &probes_info},
        {.dstBinding = 1,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageBuffer,
         .pBufferInfo = &fingerprints_info},
        {.dstBinding = 2,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageBuffer,
         .pBufferInfo = &flags_info},
        {.dstBinding = 3,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageBuffer,
         .pBufferInfo = &instance_info},
    }};
    const ProbeConstants probe_push{
        .positions_address = positions_address,
        .probe_count = u32(probes.size()),
        .pad = ptlas_on ? 1u : 0u,
        .xs = scale[0],
        .ys = scale[1],
        .shelf_metres = shelf_metres,
    };
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *probe_pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *probe_pipeline_layout, 0,
                                probe_writes);
    cmdbuf.pushConstants(*probe_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
                         sizeof(probe_push), &probe_push);
    CrumbBegin(cmdbuf, Rt::Crumb::Probe);
    cmdbuf.dispatch((u32(probes.size()) + 63) / 64, 1, 1);
    CrumbEnd(cmdbuf, Rt::Crumb::Probe);
    Barrier(cmdbuf, Stage::eComputeShader, Access::eShaderWrite,
            Stage::eAccelerationStructureBuildKHR | Stage::eHost,
            Access::eShaderRead | Access::eHostRead);

    const vk::PipelineStageFlags2 tlas_readers =
        restir_gi_rt_pipelines[0] || restir_gi_rt_pipelines[1] ? Stage::eComputeShader | Stage::eRayTracingShaderKHR
                                 : vk::PipelineStageFlags2{Stage::eComputeShader};
    TimeBegin(cmdbuf, TlasPass);
    if (ptlas_on) {
        CrumbBegin(cmdbuf, Rt::Crumb::TlasBuild);
        ptlas.Record(cmdbuf, slot, current_inv_view, tlas_readers);
        CrumbEnd(cmdbuf, Rt::Crumb::TlasBuild);
    } else {
        if (!tlas_space_written) {
            PartitionedTlas::WriteSpace(cmdbuf, tlas_buffer.buffer, Rt::IdentityAffine(),
                                        tlas_readers);
            tlas_space_written = true;
        }
        const auto tlas_geometry =
            InstanceGeometry(instances_ring.address + u64(slot) * instances_ring.slot_bytes);
        const vk::AccelerationStructureBuildGeometryInfoKHR tlas_info{
            .type = vk::AccelerationStructureTypeKHR::eTopLevel,
            .flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace,
            .mode = vk::BuildAccelerationStructureModeKHR::eBuild,
            .dstAccelerationStructure = *tlas,
            .geometryCount = 1,
            .pGeometries = &tlas_geometry,
            .scratchData = {.deviceAddress = scratch.address},
        };
        const vk::AccelerationStructureBuildRangeInfoKHR tlas_range{.primitiveCount = u32(instances.size())};
        const vk::AccelerationStructureBuildRangeInfoKHR* tlas_ranges = &tlas_range;
        CrumbBegin(cmdbuf, Rt::Crumb::TlasBuild);
        cmdbuf.buildAccelerationStructuresKHR(1, &tlas_info, &tlas_ranges);
        CrumbEnd(cmdbuf, Rt::Crumb::TlasBuild);
    }
    TimeEnd(cmdbuf, TlasPass);
    tlas_frame = frame;
    tlas_tick = scheduler.CurrentTick();

    Barrier(cmdbuf, Stage::eAccelerationStructureBuildKHR,
            Access::eAccelerationStructureWriteKHR, tlas_readers,
            Access::eAccelerationStructureReadKHR | Access::eShaderRead);
    Barrier(cmdbuf, Stage::eComputeShader | Stage::eAccelerationStructureBuildKHR,
            Access::eShaderRead | Access::eAccelerationStructureReadKHR, Stage::eVertexShader,
            Access::eShaderWrite);
    TimeEnd(cmdbuf, 0);
    last_rebuilt = u32(pending.size());
    last_instances = u32(instances.size());
    max_rebuilt_seen = std::max(max_rebuilt_seen, triangles);
    built_this_frame = true;
    built_slot = Rt::PassSurfaceSlot(frame, Slots);
    ++built_frames;
    return true;
}

void RayTracing::OnGBufferPass(std::span<const VideoCore::ImageId> targets) {
    if (!enabled || !(gi_enabled || direct_enabled || reflect_enabled)) {
        return;
    }
    u32 draw_bound = 0;
    for (const auto id : targets) {
        draw_bound += id ? 1u : 0u;
    }
    if (emissive_target_index >= 0 && u32(emissive_target_index) < targets.size() &&
        targets[emissive_target_index]) {
        emissive_target = targets[emissive_target_index];
    }
    if (!Rt::GBufferLayoutReplaces(gbuffer_bound, draw_bound)) {
        gbuffer_layouts_ignored += draw_bound < gbuffer_bound ? 1u : 0u;
        return;
    }
    gbuffer_bound = draw_bound;
    gbuffer_count = 0;
    for (const auto id : targets) {
        if (gbuffer_count < gbuffer_targets.size()) {
            gbuffer_targets[gbuffer_count++] = id;
        }
    }
    if (gbuffer_count && gbuffer_targets[0]) {
        const u32 width = texture_cache.GetImage(gbuffer_targets[0]).info.size.width;
        if (width >= ao_gbuffer_width) {
            ao_gbuffer_width = width;
            ao_gbuffer = gbuffer_targets;
            ao_gbuffer_count = gbuffer_count;
        }
    }
    if (!gbuffer_printed && gbuffer_count >= 5) {
        gbuffer_printed = true;
        Log("Ray tracing: G-buffer targets:");
        for (u32 i = 0; i < gbuffer_count; ++i) {
            if (!gbuffer_targets[i]) {
                Log(" %u unbound", i);
                continue;
            }
            const auto& image = texture_cache.GetImage(gbuffer_targets[i]);
            Log(" %u %s %ux%u", i, vk::to_string(image.info.pixel_format).c_str(),
                        image.info.size.width, image.info.size.height);
        }
        Log("\n");
    }
}

void RayTracing::OnSceneColor(VideoCore::ImageId color) {
    if (enabled && (gi_enabled || debug_mode)) {
        scene_color = color;
    }
}

void RayTracing::DebugLine(const char* text) {
    Log("%s\n", text);
}

void RayTracing::Trace(const char* format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    const int length = std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (length > 0) {
        Log("Ray tracing: trace seq %llu frame %llu: %s\n", (unsigned long long)++trace_seq,
            (unsigned long long)frame, buffer);
    }
}

void RayTracing::TraceImage(const char* what, VideoCore::ImageId id) {
    if (!id) {
        Trace("%s image none", what);
        return;
    }
    const auto& img = texture_cache.GetImage(id);
    Trace("%s image %u %s %ux%u guest %#llx usage tex %u storage %u rt %u", what, id.index,
          vk::to_string(img.info.pixel_format).c_str(), img.info.size.width, img.info.size.height,
          (unsigned long long)img.info.guest_address, u32(img.usage.texture), u32(img.usage.storage),
          u32(img.usage.render_target));
}

void RayTracing::OnColorTarget(VideoCore::ImageId image, bool clear, u64 ps_hash) {
    if (!DirectTrace() || !image) {
        return;
    }
    const bool acc = image == acc_diffuse || image == acc_specular ||
                     image == direct_written_diffuse || image == direct_written_specular;
    const auto& img = texture_cache.GetImage(image);
    Trace("render target image %u %s bound by ps %08llx, load %s%s", image.index,
          vk::to_string(img.info.pixel_format).c_str(), (unsigned long long)ps_hash,
          clear ? "CLEAR" : "load", acc ? " (an accumulation target)" : "");
}

void RayTracing::OnFastClearEliminate(VideoCore::ImageId image) {
    if (!DirectTrace() || !image) {
        return;
    }
    Trace("fast clear eliminate clears image %u%s", image.index,
          image == acc_diffuse || image == acc_specular || image == direct_written_diffuse ||
                  image == direct_written_specular
              ? " (an accumulation target)"
              : "");
}

void RayTracing::OnCompositeDraw(VideoCore::ImageId target, VideoCore::ImageId depth) {
    if (!DirectTrace()) {
        return;
    }
    Trace("%s draw recorded after the hook (direct wrote frame %llu)", HookName(),
          (unsigned long long)direct_written_frame);
    TraceImage("hook draw target", target);
    TraceImage("hook draw depth", depth);
}

void RayTracing::OnLightDraw(u64 ps_hash) {
    light_draw_ps = ps_hash;
    ++frame_light_draws;
    if (!light_capture) {
        return;
    }
    if (light_frame != frame) {
        EndLightFrame();
        light_frame = frame;
    }
    const auto start = std::chrono::steady_clock::now();
    light_draw_shader = Rt::FindLightShader(ps_hash);
    if (!light_draw_shader && acc_draw_logged.insert(ps_hash).second) {
        Log("Ray tracing: light-VS draw with an unknown pixel shader %016llx at frame %llu (not "
            "captured, not skipped)\n",
            (unsigned long long)ps_hash, (unsigned long long)frame);
    }
    if (light_draw_shader && !light_dump && !direct_lights_wanted &&
        (light_draw_shader->kind != Rt::LightKind::Point || light_draw_shader->shadowed)) {
        light_draw_shader = nullptr;
    }
    if (light_draw_shader) {
        ++light_draws_total;
        if (!light_dump && !light_draws.empty() && light_draws.back().dir_bytes == 0 &&
            pending_lights.size() == light_draw_first) {
            Rt::ResetLightDrawRecord(light_draws.back(), light_draw_shader);
        } else {
            light_draws.emplace_back().shader = light_draw_shader;
        }
        light_draw_first = pending_lights.size();
    }
    light_cpu_ns += u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count());
}

void RayTracing::OnAccTargetDraw(VideoCore::ImageId target, u64 vs_hash, u64 ps_hash,
                                 u32 mrt_mask, bool light_draw) {
    if (light_draw || !target || !acc_diffuse || target != acc_diffuse || acc_frame != frame) {
        return;
    }
    const u64 key = (vs_hash * 0x9E3779B97F4A7C15ull) ^ ps_hash ^ (u64(mrt_mask) << 56);
    if (acc_draw_logged.insert(key).second) {
        Log("Ray tracing: draw into the light accumulation target with vs %016llx ps %016llx mrt "
            "mask %x at frame %llu (not a light-VS draw)\n",
            (unsigned long long)vs_hash, (unsigned long long)ps_hash, mrt_mask,
            (unsigned long long)frame);
    }
}

void RayTracing::OnLightStencil(u32 stencil) {
    if (light_capture && light_draw_shader && !light_draws.empty()) {
        light_draws.back().stencil = stencil;
    }
}

void RayTracing::OnLightState(const Rt::LightDrawState& state) {
    if (light_capture && light_draw_shader && !light_draws.empty()) {
        light_draws.back().state = state;
    }
}

void RayTracing::OnLightVtxParam(u32 size, const void* data) {
    if (!light_capture || !light_draw_shader || light_draws.empty() || !data ||
        size != Rt::LightVtxParamBytes) {
        return;
    }
    Rt::LightDrawState& st = light_draws.back().state;
    float z;
    std::memcpy(&z, data, sizeof(z));
    if (std::isfinite(z)) {
        st.has_vtx = true;
        st.vtx_z = z;
    }
}

void RayTracing::LogGatedLights(const Rt::LightTargets& main) {
    u32 listed = 0;
    for (const Rt::Light& l : gated_lights) {
        if (listed++ >= 16) {
            break;
        }
        const Rt::LightDrawState& st = l.state;
        Log("RT light gate frame %llu: %s%s view (%.2f %.2f %.2f) radius %.2f colour (%.3f %.3f %.3f) "
            "removed: %s; targets %u/%u %upx (main %u/%u %upx), mask %x/%x, blend %u src %u dst %u "
            "func %u / %u src %u dst %u func %u, depth %u func %u bounds %u [%.6f %.6f] write %u cull "
            "%u quad z %.6f (vtx %s)\n",
            (unsigned long long)light_frame, l.shader_name ? l.shader_name : "?",
            l.camera_fixed ? " player fill light (camera-fixed)" : "", l.position[0],
            l.position[1], l.position[2], l.radius, l.color[0], l.color[1], l.color[2],
            l.gate ? l.gate : "?", st.diffuse, st.specular, st.width, main.diffuse, main.specular,
            main.width, st.mask0,
            st.mask1, u32(st.blend0), st.src0, st.dst0, st.func0, u32(st.blend1), st.src1, st.dst1,
            st.func1, u32(st.depth), st.zfunc, u32(st.bounds), st.bounds_min, st.bounds_max,
            u32(st.depth_write), st.cull, Rt::LightDepthTest(st).quad, st.has_vtx ? "on" : "off");
    }
    for (const Rt::Light& l : pending_lights) {
        if (!l.camera_fixed && !l.gate) {
            continue;
        }
        if (listed++ >= 16) {
            break;
        }
        const Rt::LightDrawState& st = l.state;
        Log("RT light gate frame %llu: %s%s view (%.2f %.2f %.2f) radius %.2f colour (%.3f %.3f %.3f) "
            "spec (%.3f %.3f %.3f) kept: %s; targets %u/%u (main %u/%u), mask %x/%x, blend %u src %u "
            "dst %u func %u / %u src %u dst %u func %u, depth %u func %u bounds %u [%.6f %.6f] write %u "
            "cull %u quad z %.6f (vtx %s)\n",
            (unsigned long long)light_frame, l.shader_name ? l.shader_name : "?",
            l.camera_fixed ? " player fill light (camera-fixed)" : "", l.position[0],
            l.position[1], l.position[2], l.radius, l.color[0], l.color[1], l.color[2], l.spec[0],
            l.spec[1], l.spec[2], l.gate ? l.gate : "same target, mask and blend as the lamps",
            st.diffuse, st.specular, main.diffuse, main.specular, st.mask0, st.mask1, u32(st.blend0),
            st.src0, st.dst0, st.func0, u32(st.blend1), st.src1, st.dst1, st.func1, u32(st.depth),
            st.zfunc, u32(st.bounds), st.bounds_min, st.bounds_max, u32(st.depth_write), st.cull,
            Rt::LightDepthTest(st).quad, st.has_vtx ? "on" : "off");
    }
    Log("RT light gate frame %llu: %zu removed, %llu scaled so far, %llu removed so far, %llu frames "
        "with a camera-fixed light, player fill light x %u this frame (signature %u, tracked %u), gate "
        "reason: %s (BB_RT_LIGHT_GATE %u, BB_RT_FILL_LIGHT %s, BB_RT_LIGHT_DEPTH %u)\n",
        (unsigned long long)light_frame, gated_lights.size(), (unsigned long long)gate_scaled,
        (unsigned long long)gate_removed, (unsigned long long)gate_fixed_frames, fixed_count.total,
        fixed_count.signature, fixed_count.tracked,
        fill_light_off ? Rt::FillLightOffReason : "kept, same target, mask and blend as the lamps",
        u32(light_gate), fill_light_off ? "off" : "on", u32(light_depth));
}

void RayTracing::OnLightBuffer(u32 size, const void* data) {
    if (!light_capture || !light_draw_shader || light_draws.empty() || !data || size < 16) {
        return;
    }
    if (light_draw_shader->kind == Rt::LightKind::Directional && light_draw_shader->shadowed &&
        size == Rt::SceneParamBytes) {
        std::memcpy(shadow_color.data(), static_cast<const char*>(data) + Rt::ShadowColorOffset,
                    sizeof(shadow_color));
    }
    const auto start = std::chrono::steady_clock::now();
    const size_t before = pending_lights.size();
    if (Rt::RecordLightBuffer(light_draws.back(), u32(light_draws.size() - 1), size, data,
                              pending_lights, light_dump ? nullptr : &light_dedup) ==
        Rt::LightBufferRole::Other) {
        ++light_unknown_buffers;
    }
    light_lights_total += pending_lights.size() - before;
    light_cpu_ns += u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count());
}

void RayTracing::OnCompositeBuffer(u32 index, VAddr address, u32 size) {
    if (!gi_enabled || !gi_fog_ring.mapped || address == 0 || size < 96 * 64 || size % 96 != 0) {
        return;
    }
    const u32 bytes = std::min(size, gi_fog_ring.slot_bytes);
    if (gi_fog_log_lines < 4 || frame % 600 == 0) {
        ++gi_fog_log_lines;
        const float* f = reinterpret_cast<const float*>(gi_fog_ring.mapped +
                                                        u64((frame + 1) % Slots) * gi_fog_ring.slot_bytes);
        u32 used = 0;
        for (u32 s = 0; s < bytes / 96; ++s) {
            if (f[s * 24 + 8] != 0.0f || f[s * 24 + 12] != 0.0f) {
                used = s;
                break;
            }
        }
        std::string head;
        for (u32 i = used * 24; i < used * 24 + 24; ++i) {
            head += fmt::format(" {:.4g}", f[i]);
        }
        Log("Ray tracing: fog sets buffer %u at frame %llu: %#llx, %u bytes, GPU-modified %s; "
            "snapshot of frame %llu, first non-zero set %u:%s\n",
            index, (unsigned long long)frame, (unsigned long long)address, size,
            buffer_cache.IsRegionGpuModified(address, size) ? "yes" : "no",
            (unsigned long long)(frame - 2), used, head.c_str());
    }
    const auto [buffer, offset] = buffer_cache.ObtainBuffer(address, bytes, false, true);
    if (!buffer) {
        return;
    }
    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;
    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();
    Barrier(cmdbuf, Stage::eAllCommands, Access::eMemoryWrite, Stage::eTransfer,
            Access::eTransferRead | Access::eTransferWrite);
    const vk::BufferCopy region{offset, u64(frame % Slots) * gi_fog_ring.slot_bytes, bytes};
    cmdbuf.copyBuffer(buffer->Handle(), gi_fog_ring.buffer, region);
    const bool fog_runs = fog_enabled && !fog_stock && static_cast<bool>(*fog_pipeline) &&
                          !instances.empty() && (!fog_surfaces || surfaces_ring.buffer) &&
                          shadow_spec_target >= 0 && u32(shadow_spec_target) < gbuffer_count &&
                          gbuffer_targets[shadow_spec_target] &&
                          fog_ran_frame != ~0ull && frame - fog_ran_frame <= 1 &&
                          !Rt::effects_paused.load(std::memory_order_relaxed);
    if (fog_runs) {
        Barrier(cmdbuf, Stage::eTransfer, Access::eTransferRead | Access::eTransferWrite,
                Stage::eTransfer, Access::eTransferWrite);
        cmdbuf.fillBuffer(buffer->Handle(), offset, bytes, 0u);
    }
    Barrier(cmdbuf, Stage::eTransfer, Access::eTransferWrite, Stage::eAllCommands,
            Access::eShaderRead | Access::eUniformRead | Access::eTransferRead);
    gi_fog_frame = frame;
    gi_fog_bytes = bytes;
}

void RayTracing::OnLightSetStorageWrite(VAddr address, u32 bytes, u64 pgm_hash, u32 stage) {
    if (!direct_sets_source.address || sets_storage_logged >= 48 ||
        address + bytes <= direct_sets_source.address ||
        address >= direct_sets_source.address + direct_sets_source.bytes) {
        return;
    }
    ++sets_storage_logged;
    Log("Ray tracing: storage write into the directional set buffer at frame %llu: stage %u shader "
        "%016llx, %#llx + %u (set buffer %#llx + %u), light draw %llu of the frame\n",
        (unsigned long long)frame, stage, (unsigned long long)pgm_hash, (unsigned long long)address,
        bytes, (unsigned long long)direct_sets_source.address, direct_sets_source.bytes,
        (unsigned long long)frame_light_draws);
}

void RayTracing::OnLightSetWrite(VAddr dst, u32 bytes, VAddr src, bool host_copy) {
    if (!direct_sets_source.address || sets_write_logged >= 32 ||
        dst + bytes <= direct_sets_source.address ||
        dst >= direct_sets_source.address + direct_sets_source.bytes) {
        return;
    }
    ++sets_write_logged;
    const float* g = src && host_copy && bytes >= 76 ? reinterpret_cast<const float*>(src) : nullptr;
    Log("Ray tracing: %s into the directional set buffer at frame %llu: dst %#llx src %#llx, %u "
        "bytes (set buffer %#llx + %u); source set 0 colour (%.4f %.4f %.4f)\n",
        host_copy ? (src ? "host memcpy" : "host fill") : (src ? "GPU copy" : "GPU fill"),
        (unsigned long long)frame, (unsigned long long)dst, (unsigned long long)src, bytes,
        (unsigned long long)direct_sets_source.address, direct_sets_source.bytes,
        g ? g[16] : -1.0f, g ? g[17] : -1.0f, g ? g[18] : -1.0f);
}

void RayTracing::OnLightSetBuffer(VAddr address, u32 size) {
    if (!light_capture || !direct_lights_wanted || !light_draw_shader || address == 0 ||
        !Rt::IsDirLightSetBuffer(*light_draw_shader, size)) {
        return;
    }
    if (sets_bind_frame != frame) {
        if (sets_bind_frame != ~0ull && sets_bind_count != 1 && sets_bind_lines < 16) {
            ++sets_bind_lines;
            Log("Ray tracing: %u directional set buffer binds at frame %llu\n", sets_bind_count,
                (unsigned long long)sets_bind_frame);
        }
        sets_bind_frame = frame;
        sets_bind_count = 0;
    }
    ++sets_bind_count;
    if (sets_bind_logged < 24) {
        ++sets_bind_logged;
        const float* g = reinterpret_cast<const float*>(address);
        const VideoCore::ImageId alias = texture_cache.FindImageFromRange(address, size);
        u32 alias_w = 0, alias_h = 0, alias_fmt = 0;
        if (alias) {
            const auto& info = texture_cache.GetImage(alias).info;
            alias_w = info.size.width;
            alias_h = info.size.height;
            alias_fmt = u32(info.pixel_format);
        }
        Log("Ray tracing: directional set buffer bind %u at frame %llu: %s ps %08x, %#llx, %u "
            "bytes, light draw %llu of the frame; guest memory set 0 moon dir (%.3f %.3f "
            "%.3f) colour (%.4f %.4f %.4f), GPU-modified %s, aliased image %u (%ux%u format %u)\n",
            sets_bind_count, (unsigned long long)frame, light_draw_shader->name,
            light_draw_shader->ps_hash, (unsigned long long)address, size,
            (unsigned long long)frame_light_draws, g[0], g[1], g[2], g[16], g[17], g[18],
            buffer_cache.IsRegionGpuModified(address, size) ? "yes" : "no",
            alias ? alias.index + 1u : 0u, alias_w, alias_h, alias_fmt);
    }
    if (sets_bind_count > 1 && dir_sets_bind_first) {
        return;
    }
    direct_sets_source = {address, size, frame};
    if (dir_sets_mode == DirSetsMode::Gpu || dir_sets_mode == DirSetsMode::Host ||
        !direct_sets_ring.buffer || size > Rt::DirectSetBytes) {
        return;
    }
    const auto [buffer, offset] = buffer_cache.ObtainBuffer(address, size, false, true);
    if (!buffer) {
        return;
    }
    const vk::Buffer source = buffer->Handle();
    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;
    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();
    const u32 slot = u32(frame % Slots);
    Barrier(cmdbuf, Stage::eAllCommands, Access::eMemoryWrite, Stage::eTransfer,
            Access::eTransferRead | Access::eTransferWrite);
    const vk::BufferCopy region{offset, u64(slot) * direct_sets_ring.slot_bytes, size};
    cmdbuf.copyBuffer(source, direct_sets_ring.buffer, region);
    Barrier(cmdbuf, Stage::eTransfer, Access::eTransferWrite, Stage::eAllCommands,
            Access::eShaderRead | Access::eTransferRead);
    RecordMoonDir(cmdbuf, u64(slot) * direct_sets_ring.slot_bytes, size);
    sets_snapshot_frame = frame;
    sets_snapshot_bytes = size;
    ++sets_snapshot_frames;
}

void RayTracing::LogSetSource(const char* pass, const GpuSets& s, u32 sets, u32 lights) {
    if (pick_every >= 60 || !pick_enabled) {
        return;
    }
    Log("RT %s frame %llu: sets %s (%u), lights %u, history %d\n", pass,
        (unsigned long long)frame,
        !s.buffer ? (sets ? "cpu readback" : "none")
                  : s.buffer == direct_sets_ring.buffer ? "snapshot" : "game buffer",
        sets, lights, int(history_prev_frame_ok));
}

RayTracing::GpuSets RayTracing::ObtainGpuLightSets(u64 max_age) {
    if (dir_sets_mode == DirSetsMode::Host ||
        !Rt::GpuLightSetsFresh(direct_sets_source, frame, max_age)) {
        return {};
    }
    if (dir_sets_mode != DirSetsMode::Gpu) {
        if (sets_snapshot_frame == ~0ull || sets_snapshot_frame > frame ||
            frame - sets_snapshot_frame > max_age || sets_snapshot_bytes == 0) {
            return {};
        }
        return {direct_sets_ring.buffer, u64(sets_snapshot_frame % Slots) * direct_sets_ring.slot_bytes,
                sets_snapshot_bytes};
    }
    const auto [buffer, offset] = buffer_cache.ObtainBuffer(direct_sets_source.address,
                                                            direct_sets_source.bytes, false, true);
    if (!buffer || !Rt::GpuLightSetsAligned(offset, instance.StorageMinAlignment())) {
        return {};
    }
    return {buffer->Handle(), offset, direct_sets_source.bytes};
}

void RayTracing::RecordWorldSet(vk::CommandBuffer cmdbuf, vk::ImageView depth_view,
                                vk::ImageView g3_view, const vk::DescriptorBufferInfo& sets,
                                u32 set_count, u32 width, u32 height, vk::Buffer params,
                                u64 params_offset) {
    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;
    using Type = vk::DescriptorType;
    // Same switch as the direct pass (BB_RT_MOON_WORLD=0 keeps each pixel's own set there too);
    // BB_RT_GI_WORLD_SET=0 (live) turns off only this vote, for A/B against the direct pass.
    if (!world_set_pipeline || !moon_world || !gi_world_set || set_count == 0) {
        return;
    }
    const vk::DescriptorImageInfo depth_info{.imageView = depth_view,
                                             .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo g3_info{.imageView = g3_view,
                                          .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorBufferInfo out_info{params, params_offset, ParamsSlotBytes};
    const auto write = [](u32 binding, Type type, const vk::DescriptorImageInfo* image,
                          const vk::DescriptorBufferInfo* buffer) {
        return vk::WriteDescriptorSet{.dstBinding = binding,
                                      .descriptorCount = 1,
                                      .descriptorType = type,
                                      .pImageInfo = image,
                                      .pBufferInfo = buffer};
    };
    const std::array<vk::WriteDescriptorSet, 4> writes = {{
        write(0, Type::eSampledImage, &depth_info, nullptr),
        write(1, Type::eSampledImage, &g3_info, nullptr),
        write(2, Type::eStorageBuffer, nullptr, &sets),
        write(3, Type::eStorageBuffer, nullptr, &out_info),
    }};
    const WorldSetPush push{float(width), float(height), set_count, WorldSetWord};
    Barrier(cmdbuf, Stage::eHost | Stage::eAllCommands, Access::eHostWrite | Access::eShaderRead,
            Stage::eComputeShader, Access::eShaderWrite | Access::eShaderRead);
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *world_set_pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *world_set_pipeline_layout, 0,
                                writes);
    cmdbuf.pushConstants(*world_set_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
                         sizeof(push), &push);
    cmdbuf.dispatch(1, 1, 1);
    Barrier(cmdbuf, Stage::eComputeShader, Access::eShaderWrite, Stage::eAllCommands,
            Access::eUniformRead | Access::eShaderRead);
    if (world_set_frames++ == 0) {
        Log("Ray tracing: world set vote on (BB_RT_MOON_WORLD): GI and reflection hits take the "
            "moon from the directional set the direct pass votes for, not the pixel's own set\n");
    }
}

// Light 1 of every set in this frame's set copy points at the moon the sky draws (rt_sky.comp
// phase 3 finds it, rt_moon_dir.comp writes it). Runs on the copy only; BB_RT_MOON_FROM_SKY=0
// (live) keeps the game's direction.
void RayTracing::RecordMoonDir(vk::CommandBuffer cmdbuf, u64 sets_offset, u32 sets_bytes) {
    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;
    using Type = vk::DescriptorType;
    const u32 set_count = sets_bytes / Rt::DirLightSetStride;
    if (!moon_dir_pipeline || !moon_from_sky || !material.buffer || set_count == 0) {
        return;
    }
    const vk::DescriptorBufferInfo sets_info{direct_sets_ring.buffer, sets_offset,
                                             u64(set_count) * Rt::DirLightSetStride};
    const vk::DescriptorBufferInfo material_info{material.buffer, 0, material.size};
    const std::array<vk::WriteDescriptorSet, 2> writes = {{
        {.dstBinding = 0, .descriptorCount = 1, .descriptorType = Type::eStorageBuffer,
         .pBufferInfo = &sets_info},
        {.dstBinding = 1, .descriptorCount = 1, .descriptorType = Type::eStorageBuffer,
         .pBufferInfo = &material_info},
    }};
    const float min_sin = std::sin(moon_min_elevation * 0.017453293f);
    u32 min_sin_bits;
    std::memcpy(&min_sin_bits, &min_sin, sizeof(min_sin_bits));
    const MoonDirPush push{set_count, Rt::MoonDirSlot(capacity.records), min_sin_bits, 0u};
    Barrier(cmdbuf, Stage::eAllCommands, Access::eMemoryWrite, Stage::eComputeShader,
            Access::eShaderRead | Access::eShaderWrite);
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *moon_dir_pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *moon_dir_pipeline_layout, 0,
                                writes);
    cmdbuf.pushConstants(*moon_dir_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
                         sizeof(push), &push);
    cmdbuf.dispatch((set_count + 63) / 64, 1, 1);
    Barrier(cmdbuf, Stage::eComputeShader, Access::eShaderWrite, Stage::eAllCommands,
            Access::eShaderRead | Access::eTransferRead);
    if (moon_dir_frames++ == 0) {
        Log("Ray tracing: moon direction from the sky on (BB_RT_MOON_FROM_SKY): light 1 of the "
            "set copy points at the moon the sky draws once it has been on screen\n");
    }
}

void RayTracing::ApplySetReadback() {
    if (dir_sets_mode == DirSetsMode::Host || !sets_readback_ring.mapped || !direct_lights_wanted) {
        return;
    }
    const int ready = Rt::NewestReadySetReadback(
        sets_readback, [&](u64 tick) { return scheduler.IsFree(tick); });
    if (ready < 0) {
        return;
    }
    const Rt::SetReadback& r = sets_readback[size_t(ready)];
    const float* data =
        reinterpret_cast<const float*>(sets_readback_ring.mapped + u64(ready) * sets_readback_ring.slot_bytes);
    for (size_t d = 0; d < light_draws.size(); ++d) {
        LightDrawRecord& rec = light_draws[d];
        if (rec.dir_bytes < Rt::DirLightSetStride || rec.dir_bytes > Rt::DirectSetBytes) {
            continue;
        }
        const u32 bytes = std::min(rec.dir_bytes, r.bytes);
        std::memcpy(rec.dir.data(), data, bytes);
        std::erase_if(pending_lights, [&](const Rt::Light& l) {
            return l.draw == u32(d) && l.kind == Rt::LightKind::Directional;
        });
        Rt::ParseDirectionalSets(std::span<const float>(rec.dir.data(), bytes / 4), u32(d),
                                 rec.shader->shadowed, pending_lights);
        ++sets_readback_frames;
        return;
    }
}

void RayTracing::LogDirFrame(u32 state) {
    DirLogStats& st = dir_log_stats;
    ++st.direct;
    st.no_sets += (state & 1u) ? 0u : 1u;
    st.host += (state & 1u) && !(state & 2u) ? 1u : 0u;
    st.no_mask += (state & 4u) ? 0u : 1u;
    st.pass += (state & 16u) ? 1u : 0u;
    if (state == dir_log_state) {
        return;
    }
    if (dir_log_state != ~0u) {
        ++st.state;
    }
    dir_log_state = state;
    if (st.lines < 8) {
        ++st.lines;
        Log("RT dir frame %llu state: sets %s, %s, %s\n", (unsigned long long)frame,
            !(state & 1u) ? "none (moon kept from the game)"
            : (state & 2u) ? "from the game's GPU buffer"
                           : "from the host copy",
            (state & 4u) ? "game mask bound" : "no game mask",
            (state & 16u) ? "game light draws kept (pass-through)"
            : (state & 8u) ? "added (game light draws skipped)"
                           : "overwritten");
    }
}

void RayTracing::LogDirSets() {
    if (!dir_log || !sets_readback_ring.mapped || !direct_lights_wanted) {
        return;
    }
    constexpr u32 stride = Rt::DirLightSetStride / 4;
    std::array<size_t, Slots> order{};
    size_t ready = 0;
    for (size_t i = 0; i < Slots; ++i) {
        const Rt::SetReadback& r = sets_readback[i];
        if (r.bytes == 0 || r.frame == ~0ull || !scheduler.IsFree(r.tick) ||
            (dir_log_frame != ~0ull && r.frame <= dir_log_frame)) {
            continue;
        }
        order[ready++] = i;
    }
    std::sort(order.begin(), order.begin() + ready,
              [&](size_t a, size_t b) { return sets_readback[a].frame < sets_readback[b].frame; });
    const auto now = std::chrono::steady_clock::now();
    if (dir_log_tick == std::chrono::steady_clock::time_point{}) {
        dir_log_tick = now;
    }
    for (size_t o = 0; o < ready; ++o) {
        const Rt::SetReadback& r = sets_readback[order[o]];
        const float* data = reinterpret_cast<const float*>(
            sets_readback_ring.mapped + u64(order[o]) * sets_readback_ring.slot_bytes);
        const std::span<const float> cur(data, r.bytes / 4);
        const size_t sets = cur.size() / stride;
        const bool first = dir_log_prev.empty();
        const Rt::DirSetDiff diff = Rt::CompareDirSets(dir_log_prev, cur);
        const bool sets_changed = first || diff.changed > 0 || dir_log_prev.size() != cur.size();
        const bool shadow_changed = first || r.shadow_color != dir_log_last.shadow_color;
        const bool address_changed = first || r.address != dir_log_last.address;
        const bool flags_changed = first || r.flags != dir_log_last.flags;
        DirLogStats& st = dir_log_stats;
        if (!first && r.frame > dir_log_frame + 1) {
            st.gaps += r.frame - dir_log_frame - 1;
        }
        ++st.frames;
        if (!first) {
            st.sets += sets_changed ? 1u : 0u;
            st.shadow += shadow_changed ? 1u : 0u;
            st.address += address_changed ? 1u : 0u;
            st.flags += flags_changed ? 1u : 0u;
            st.color_delta = std::max(st.color_delta, diff.color_delta);
            st.dir_delta = std::max(st.dir_delta, diff.dir_delta);
        }
        st.zero = std::max<u64>(st.zero, diff.zero);
        if (sets > 0 && (sets_changed || shadow_changed || address_changed || flags_changed) &&
            st.lines < 8) {
            ++st.lines;
            const u32 fs = diff.first < sets ? diff.first : 0u;
            const float* s0 = data;
            const float* sf = data + size_t(fs) * stride;
            const bool have_prev = !first && size_t(fs + 1) * stride <= dir_log_prev.size();
            const float* pf = have_prev ? dir_log_prev.data() + size_t(fs) * stride : sf;
            Log("RT dir frame %llu (read %llu frames later): %s%s%s%s%u of %zu sets changed (first %u: "
                "moon colour %.4f %.4f %.4f was %.4f %.4f %.4f; max colour delta %.4f, max direction "
                "delta %.4f), %u all-zero sets; set 0 moon dir (%.3f %.3f %.3f) colour (%.4f %.4f "
                "%.4f) spec (%.4f %.4f %.4f), light 2 dir (%.3f %.3f %.3f) colour (%.4f %.4f %.4f), "
                "factors (%.3f %.3f %.3f %.3f); FC_ShadowColor %.4f %.4f %.4f (was %.4f %.4f %.4f); "
                "buffer %#llx (was %#llx); %s, %s, moon %s, ray sample %s\n",
                (unsigned long long)r.frame, (unsigned long long)(frame - r.frame),
                first ? "first, " : "", shadow_changed && !first ? "FC_ShadowColor changed, " : "",
                address_changed && !first ? "buffer moved, " : "",
                flags_changed && !first ? "mode changed, " : "", diff.changed, sets,
                diff.first == Rt::NoLight ? 0u : diff.first, sf[16], sf[17], sf[18], pf[16], pf[17],
                pf[18], diff.color_delta, diff.dir_delta, diff.zero, s0[0], s0[1], s0[2], s0[16],
                s0[17], s0[18], s0[32], s0[33], s0[34], s0[4], s0[5], s0[6], s0[20], s0[21], s0[22],
                s0[68], s0[69], s0[70], s0[71], r.shadow_color[0], r.shadow_color[1],
                r.shadow_color[2], dir_log_last.shadow_color[0], dir_log_last.shadow_color[1],
                dir_log_last.shadow_color[2], (unsigned long long)r.address,
                (unsigned long long)dir_log_last.address,
                (r.flags & Rt::DirLogAdd) ? "added (game light draws skipped)" : "game light draws kept",
                (r.flags & Rt::DirLogMask) ? "game mask bound" : "no game mask",
                (r.flags & Rt::DirLogMoonMask) ? "game mask" : "traced",
                (r.flags & Rt::DirLogMoonStatic) ? "fixed per pixel" : "new every frame");
        }
        if (dir_dump && r.frame >= dir_dump_frame + 60) {
            dir_dump_frame = r.frame;
            for (size_t si = 0; si < sets; ++si) {
                const float* sp = data + si * stride;
                float sum = 0.0f;
                for (u32 k = 16; k < 32; ++k) {
                    sum += std::fabs(sp[k]);
                }
                if (!(sum > 0.0f)) {
                    continue;
                }
                std::string line;
                for (u32 k = 0; k < 4; ++k) {
                    char buf[160];
                    std::snprintf(buf, sizeof(buf), " L%u dir (%.3f %.3f %.3f) col (%.4f %.4f %.4f) spec (%.4f %.4f %.4f);",
                                  k + 1, sp[4 * k], sp[4 * k + 1], sp[4 * k + 2], sp[16 + 4 * k],
                                  sp[17 + 4 * k], sp[18 + 4 * k], sp[32 + 4 * k], sp[33 + 4 * k],
                                  sp[34 + 4 * k]);
                    line += buf;
                }
                char tail[200];
                std::snprintf(tail, sizeof(tail), " w48 (%.3f %.3f %.3f %.3f) w52 (%.3f %.3f %.3f %.3f) w68 (%.3f %.3f %.3f %.3f)",
                              sp[48], sp[49], sp[50], sp[51], sp[52], sp[53], sp[54], sp[55], sp[68],
                              sp[69], sp[70], sp[71]);
                Log("RT dir dump frame %llu set %zu:%s%s\n", (unsigned long long)r.frame, si,
                    line.c_str(), tail);
            }
        }
        dir_log_prev.assign(cur.begin(), cur.end());
        dir_log_last = r;
        dir_log_frame = r.frame;
    }
    if (now - dir_log_tick >= std::chrono::seconds(1)) {
        const DirLogStats& st = dir_log_stats;
        Log("RT dir summary: %llu frames read, %llu missed; changed in %llu (sets), %llu "
            "(FC_ShadowColor), %llu (buffer address), %llu (mode); max colour delta %.4f, max "
            "direction delta %.4f, up to %llu all-zero sets; %llu direct frames, state changed %llu "
            "times, %llu without sets, %llu from the host copy, %llu pass-through, %llu without the "
            "game mask (BB_RT_DIR_LOG)\n",
            (unsigned long long)st.frames, (unsigned long long)st.gaps,
            (unsigned long long)st.sets, (unsigned long long)st.shadow,
            (unsigned long long)st.address, (unsigned long long)st.flags, st.color_delta,
            st.dir_delta, (unsigned long long)st.zero, (unsigned long long)st.direct,
            (unsigned long long)st.state, (unsigned long long)st.no_sets,
            (unsigned long long)st.host, (unsigned long long)st.pass,
            (unsigned long long)st.no_mask);
        dir_log_stats = {};
        dir_log_tick = now;
    }
}

void RayTracing::SetLightKeepCamera(const SceneInputs& inputs) noexcept {
    light_keep_proj = {inputs.projection[0], inputs.projection[1]};
    light_keep_cam = inputs.camera_valid;
    if (light_keep_cam) {
        light_keep_inv_view = inputs.inv_view;
    }
}

void RayTracing::EndLightFrame() {
    if (light_draws.empty() && pending_lights.empty()) {
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    LogDirSets();
    ApplySetReadback();
    if (light_dump && (light_dump_frames < light_dump_first || light_frame % 600 == 0)) {
        DumpLights();
    }
    for (size_t d = 0; d < light_draws.size(); ++d) {
        Rt::ApplyCameraMatrix(light_draws[d], u32(d), pending_lights);
        Rt::ApplyDrawStencil(light_draws[d], u32(d), pending_lights);
        Rt::ApplyDrawState(light_draws[d], u32(d), pending_lights);
    }
    // Also for BB_RT_FILL_VIS: camera-fixed player lights must never become candidates.
    if (direct_light_log > 0.0f || fill_light_off || direct_lights_wanted ||
        (fill_vis_on && fill_vis_pipeline)) {
        fixed_count = Rt::MarkCameraFixed(pending_lights, fixed_tracks);
        gate_fixed_frames += fixed_count.total > 0 ? 1 : 0;
    }
    gated_lights.clear();
    const Rt::LightTargets main_targets = Rt::MainLightTargets(light_draws);
    gate_scaled += Rt::ApplyStockGate(pending_lights, main_targets, light_gate, fill_light_off,
                                      &gated_lights);
    gate_removed += gated_lights.size();
    // BB_RT_LIGHT_DROP=<text>: experiment, drops every point/spot light whose game shader name
    // contains the text (e.g. Shadowed2), to test which designer lights have no emitter.
    if (const char* drop = Rt::Knob("BB_RT_LIGHT_DROP"); drop && drop[0] && drop[0] != '0') {
        std::erase_if(pending_lights, [&](const Rt::Light& l) {
            return l.kind != Rt::LightKind::Directional && l.shader_name &&
                   std::strstr(l.shader_name, drop) != nullptr;
        });
    }
    // Before MergeDirectLights: direct, GI and fog all read the scaled colour.
    ApplyFillVis();
    if (direct_light_log > 0.0f && light_frame >= gate_log_frame + 60) {
        gate_log_frame = light_frame;
        LogGatedLights(main_targets);
    }
    if (direct_lights_wanted) {
        Rt::LightKeepView keep_view{};
        keep_view.offscreen_frames = direct_keep_offscreen;
        keep_view.xs = light_keep_proj[0];
        keep_view.ys = light_keep_proj[1];
        keep_view.reproject = off_switches.cone;
        for (const LightDrawRecord& rec : light_draws) {
            if (rec.param_bytes < (Rt::CameraMatrixFloat + 12) * 4) {
                continue;
            }
            Rt::Affine camera{};
            std::copy_n(rec.param.data() + Rt::CameraMatrixFloat, 12, camera.begin());
            bool finite = true;
            for (const float v : camera) {
                finite = finite && std::isfinite(v);
            }
            if (!finite) {
                continue;
            }
            keep_view.view = Rt::InverseRigid(camera);
            keep_view.valid = true;
            break;
        }
        if (light_keep_scene && light_keep_cam) {
            keep_view.cull_view = Rt::InverseRigid(light_keep_inv_view);
            keep_view.cull_valid = true;
            ++light_keep_scene_frames;
            if (keep_view.valid) {
                // How far the light-draw camera sits from the scene camera (logged).
                const Rt::Affine a = Rt::InverseRigid(keep_view.view);
                const float dx = a[3] - light_keep_inv_view[3], dy = a[7] - light_keep_inv_view[7],
                            dz = a[11] - light_keep_inv_view[11];
                light_keep_cam_gap = std::sqrt(dx * dx + dy * dy + dz * dz);
            }
        }
        direct_duplicates +=
            Rt::MergeDirectLights(pending_lights, light_frame, direct_keep, direct_tracks, direct_lights,
                                  &direct_keys, off_switches.lights ? &keep_view : nullptr);
        // Real emitters get the inverse-square reach here, on the list the direct pass, GI and
        // fog read. MergeDirectLights rebuilds it each frame from the unconverted track copies,
        // so a light is converted once per frame. frame_lights (shadow selection only) keeps the
        // game's values so the conversion cannot rank candles out of the shadow lights.
        emitter_falloff_last = 0;
        lamp_falloff_last = 0;
        fire_lift_last = 0;
        const double flicker_seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - flicker_t0).count();
        for (size_t i = 0; i < direct_lights.size(); ++i) {
            Rt::Light& l = direct_lights[i];
            if (!l.emitter) {
                continue;
            }
            const bool tracked = i < direct_tracks.list.size();
            const bool mobile = tracked && direct_tracks.list[i].mobile;
            bool housed = false, fire_pit = false;
            if (tracked) {
                const auto f = fill_vis.find(Rt::FillVisKey(direct_tracks.list[i].light));
                housed = f != fill_vis.end() && f->second.lamp;
                fire_pit = f != fill_vis.end() && f->second.fire_pit;
            }
            const bool lamp = !mobile && housed && l.radius >= lamp_rmin && l.radius <= lamp_rmax;
            l.lamp = lamp;
            const bool candle = !lamp && !mobile && l.radius <= candle_rmax;
            // How much brighter than a lantern (BB_RT_EMITTER_BRIGHT) the game makes this fire, from
            // its colour before our flicker: the reach and, for a fixed fire, the sway grow with it.
            float grow = 1.0f;
            if (!lamp && !candle && emitter_bright > 0.0f) {
                const float peak_c = std::max({l.color[0], l.color[1], l.color[2]});
                grow = std::sqrt(std::max(peak_c / emitter_bright, 1.0f));
            }
            if (tracked) {
                const u64 id = direct_tracks.list[i].id;
                const auto m = flicker_mean.find(id);
                Rt::FlickerState st = m != flicker_mean.end() ? m->second : Rt::FlickerState{};
                float k = Rt::FlickerScale(l.color[0] + l.color[1] + l.color[2], st,
                                                 emitter_flicker, emitter_flicker_avg,
                                                 emitter_flicker_smooth);
                flicker_mean_next[id] = st;
                // The game keeps most fixed flames steady or only swells them slowly (the workshop
                // fireplace: +9% for 20 frames every ~1.7 s, mean |dev| 1.5%): they get a flicker
                // of their own on top. A flame the game itself flickers (dev ~6%+) keeps its own.
                if (st.dev < emitter_steady_dev && emitter_steady_flicker > 0.0f) {
                    float noise = Rt::FlameNoise(id, flicker_seconds);
                    if (grow > 1.0f) {
                        noise += emitter_big_fast * Rt::FlameNoise(id ^ 0x9E3779B97F4A7C15ull,
                                                                   flicker_seconds * 3.0);
                    }
                    k *= std::exp(emitter_steady_flicker * std::pow(grow, emitter_big_flicker) * noise);
                }
                if (grow > 1.0f) {
                    k *= emitter_big_gain;
                }
                for (float& c : l.color) c *= k;
                for (float& c : l.spec) c *= k;
                // Carried flames (hand torch, enemy torches) sway, scaled by the game radius. Fixed
                // flames (candles, candelabra, fireplace) sway by BB_RT_FIXED_SWAY: an indoor
                // flame's light centre moves about 1 cm; 5 cm doubled the measured shadow motion
                // (optical flow 0.023 -> 0.044 px/f) and read as too much wobble, 1.5 cm stays near
                // stock (0.015-0.022). The game radius says nothing of the flame size; its brightness
                // does: a fire brighter than a lantern (the burning house, 6.4-12.8 vs 2.0) is metres
                // of flame, and its light centre wanders BB_RT_EMITTER_SWAY x (grow - 1) more
                // (house: ~0.25 m), so the shadows it casts dance.
                if (!lamp) {
                    const float amp = mobile
                                          ? emitter_sway * std::clamp(l.radius / 8.5f, 0.2f, 1.0f)
                                          : fixed_sway + emitter_sway * (grow - 1.0f);
                    const auto o = Rt::FlameSway(id, flicker_seconds, amp);
                    for (int a = 0; a < 3; ++a) l.world[a] += o[a];
                }
            }
            if (!lamp && !mobile && fire_pit) {
                l.world[1] += fire_lift;
                ++fire_lift_last;
            }
            const float range = (lamp ? lamp_range : candle ? candle_range : emitter_range) * grow;
            l.wide = grow > 1.0f;
            if (Rt::EmitterFalloff(l, range,
                                   lamp ? lamp_source : candle ? candle_source : emitter_source,
                                   emitter_lift, emitter_peak, emitter_flame)) {
                ++emitter_falloff_last;
                lamp_falloff_last += lamp ? 1u : 0u;
            }
        }
        flicker_mean.swap(flicker_mean_next);
        flicker_mean_next.clear();
        TintLampLights();
        AddGlowLights(keep_view.cull_valid ? &keep_view.cull_view
                      : keep_view.valid    ? &keep_view.view
                                           : nullptr);
        // BB_RT_DIRECT_LIST_LOG=1 (live): every 30 frames, the state of each direct light (game
        // radius, reach used, emitter, fill-vis state, track age and mobility).
        // =2: every frame (flicker timing).
        const bool list_every = Rt::KnobIs("BB_RT_DIRECT_LIST_LOG", '2');
        if ((list_every || Rt::KnobIs("BB_RT_DIRECT_LIST_LOG", '1')) &&
            light_frame >= direct_list_log + (list_every ? 1 : 30)) {
            direct_list_log = light_frame;
            const size_t n = std::min(direct_lights.size(), direct_tracks.list.size());
            const auto cam = [&](size_t k) {
                const auto& p = direct_lights[k].position;
                return std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
            };
            std::vector<size_t> order(n);
            for (size_t k = 0; k < n; ++k) {
                order[k] = k;
            }
            std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return cam(a) < cam(b); });
            // The game's own draws of this frame within 6 m of the camera (its colour and timing
            // per light, before tracking, flicker and sway).
            for (size_t k = 0; k < pending_lights.size() && k < 256; ++k) {
                const Rt::Light& r = pending_lights[k];
                const float d = std::sqrt(r.position[0] * r.position[0] + r.position[1] * r.position[1] +
                                          r.position[2] * r.position[2]);
                if (d <= 6.0f && Rt::DirectCandidate(r)) {
                    Log("RT dlist raw frame %llu #%zu %s draw %u legacy %d shadowed %d world (%.2f %.2f "
                        "%.2f) r %.2f colour (%.3f %.3f %.3f) spec (%.3f %.3f %.3f) stencil %08x key "
                        "%016llx cam %.1f\n",
                        (unsigned long long)light_frame, k, r.shader_name ? r.shader_name : "?", r.draw,
                        r.legacy ? 1 : 0, r.shadowed ? 1 : 0, r.world[0], r.world[1], r.world[2],
                        r.radius, r.color[0], r.color[1], r.color[2], r.spec[0], r.spec[1], r.spec[2],
                        r.stencil, (unsigned long long)Rt::DirectKey(r), d);
                }
            }
            for (size_t j = 0; j < n && j < 64; ++j) {
                const size_t i = order[j];
                const Rt::DirectTrack& t = direct_tracks.list[i];
                const Rt::Light& l = direct_lights[i];
                const auto f = fill_vis.find(Rt::FillVisKey(t.light));
                const bool has = f != fill_vis.end();
                Log("RT dlist frame %llu #%zu id %llu %s world (%.2f %.2f %.2f) game r %.2f reach r %.2f "
                    "lum %.3f emitter %d lamp %d age %u frames %u mobile %d fill %s applied %.2f "
                    "enclosed %.1f%% up %.1f%% near %.2f m visible %d game colour (%.3f %.3f %.3f) "
                    "track colour (%.3f %.3f %.3f) cam %.1f\n",
                    (unsigned long long)light_frame, i, (unsigned long long)t.id,
                    t.light.shader_name ? t.light.shader_name : "?", t.light.world[0],
                    t.light.world[1], t.light.world[2], t.light.radius, l.radius,
                    l.color[0] + l.color[1] + l.color[2], l.emitter ? 1 : 0, l.lamp ? 1 : 0, l.age,
                    t.frames,
                    t.mobile ? 1 : 0,
                    !has ? "none" : f->second.inherited ? "inherited" : f->second.measured ? "measured" : "waiting",
                    has ? f->second.applied : -1.0f, has ? f->second.last.enclosed * 100.0f : -1.0f,
                    has ? f->second.last.enclosed_up * 100.0f : -1.0f,
                    has ? f->second.last.near_solid : -1.0f,
                    has && f->second.last.visible ? 1 : 0, has ? f->second.game_color[0] : -1.0f,
                    has ? f->second.game_color[1] : -1.0f, has ? f->second.game_color[2] : -1.0f,
                    t.light.color[0], t.light.color[1], t.light.color[2], cam(i));
            }
        }
        direct_set_count = 0;
        for (const LightDrawRecord& rec : light_draws) {
            if (rec.dir_bytes >= Rt::DirLightSetStride && rec.dir_bytes <= Rt::DirectSetBytes) {
                direct_sets.assign(rec.dir.begin(), rec.dir.begin() + rec.dir_bytes / 4);
                direct_set_count = rec.dir_bytes / Rt::DirLightSetStride;
                break;
            }
        }
    }
    frame_lights.swap(pending_lights);
    Rt::UpdateShadowTracks(frame_lights, light_max_local, shadow_tracks);
    Rt::ShadowDrawLights(shadow_tracks, shadow_lights);
    Rt::SelectStrongest(frame_lights, light_max_local);
    pending_lights.clear();
    light_draws.clear();
    light_dedup.Clear();
    light_draw_first = 0;
    light_cpu_ns += u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count());
}

// BB_RT_FILL_VIS: scales each measured point/spot light by its traced sky/emitter factor and
// queues the lights whose result is missing or due for a refresh. Lights without a trusted result
// keep full strength (retried every FillVisRetryFrames); a new factor blends in over ~20 frames.
// A light entering a new 0.25 m cell (a moving light) starts from its nearest measured neighbour
// cell instead of from full strength, so it does not flicker while its own trace is pending.
void RayTracing::ApplyFillVis() {
    const char* lamp_min = Rt::Knob("BB_RT_FILL_LAMP_MIN");
    fill_vis_lamp_min = lamp_min ? std::clamp(std::strtof(lamp_min, nullptr), 0.0f, 100.0f) : Rt::LampMinColour;
    fill_vis_scaled_last = 0;
    fill_vis_off_last = 0;
    fill_vis_cluster_last = 0;
    if (!fill_vis_on || !fill_vis_pipeline || !fill_vis_ring.mapped) {
        return;
    }
    constexpr size_t MaxEntries = 4096;
    constexpr size_t MaxQueue = 256;
    constexpr u64 UnseenFrames = 600;
    constexpr float FadeStep = 0.05f;
    ++fill_vis_frames;
    if (fill_vis.size() > MaxEntries) {
        std::erase_if(fill_vis, [&](const auto& kv) {
            return !kv.second.queued && kv.second.seen_frame + UnseenFrames < light_frame;
        });
        if (fill_vis.size() > MaxEntries) {
            fill_vis.clear();
            fill_vis_queue.clear();
        }
    }
    // Emitters already found among this frame's lights (BB_RT_FILL_CLUSTER).
    fill_vis_emitter_pos.clear();
    if (fill_vis_cluster > 0.0f) {
        for (const Rt::Light& l : pending_lights) {
            if (!Rt::FillVisCandidate(l)) {
                continue;
            }
            const auto f = fill_vis.find(Rt::FillVisKey(l));
            if (f != fill_vis.end() && f->second.emitter) {
                fill_vis_emitter_pos.push_back(l.world);
            }
        }
    }
    for (Rt::Light& l : pending_lights) {
        if (!Rt::FillVisCandidate(l)) {
            continue;
        }
        const u64 key = Rt::FillVisKey(l);
        const auto [it, inserted] = fill_vis.try_emplace(key);
        FillVisEntry& e = it->second;
        if (fill_vis_flicker > 0.0f && !e.pulse) {
            constexpr u32 FillFlickerTurns = 6;
            const float lum = l.color[0] + l.color[1] + l.color[2];
            if (l.radius != e.flicker_radius) {
                e.flicker_radius = l.radius;
                e.flicker_last = -1.0f;
                e.flicker_sign = 0;
                e.flicker_turns = 0;
            }
            if (lum > 0.0f && e.flicker_last > 0.0f) {
                const float step = lum - e.flicker_last;
                if (std::fabs(step) > fill_vis_flicker * std::max(lum, e.flicker_last)) {
                    const int sign = step > 0.0f ? 1 : -1;
                    e.flicker_turns += e.flicker_sign != 0 && sign != e.flicker_sign ? 1u : 0u;
                    e.flicker_sign = sign;
                }
            }
            e.flicker_last = lum;
            if (e.flicker_turns >= FillFlickerTurns) {
                e.emitter = true;
                e.pulse = true;
                if (direct_light_log > 0.0f) {
                    Log("RT fill vis frame %llu: %s world (%.2f %.2f %.2f) radius %.2f flickers "
                        "(%u turns over BB_RT_FILL_FLICKER %.3f): emitter\n",
                        (unsigned long long)light_frame, l.shader_name ? l.shader_name : "?",
                        l.world[0], l.world[1], l.world[2], l.radius, e.flicker_turns,
                        fill_vis_flicker);
                }
            }
        }
        if (fill_vis_warm && !e.emitter && Rt::WarmEmitter(l, fill_vis_warm_hue)) {
            e.emitter = true;
        }
        if (inserted) {
            // Nearest measured neighbour of the same kind (faces first, then edges, corners).
            const FillVisEntry* seed = nullptr;
            int seed_dist = 4;
            for (int dx = -1; dx <= 1; ++dx) {
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dz = -1; dz <= 1; ++dz) {
                        const int dist = std::abs(dx) + std::abs(dy) + std::abs(dz);
                        if (dist == 0 || dist >= seed_dist) {
                            continue;
                        }
                        Rt::Light n = l;
                        n.world[0] += float(dx) * Rt::FillVisCell;
                        n.world[1] += float(dy) * Rt::FillVisCell;
                        n.world[2] += float(dz) * Rt::FillVisCell;
                        const auto nb = fill_vis.find(Rt::FillVisKey(n));
                        if (nb != fill_vis.end() && nb != it && nb->second.measured) {
                            seed = &nb->second;
                            seed_dist = dist;
                        }
                    }
                }
            }
            if (seed) {
                e.applied = seed->applied;
                e.sky = seed->sky;
                e.emitter = seed->emitter;
                e.last = seed->last;
                e.measured = true;
                e.inherited = true;
            }
        }
        e.seen_frame = light_frame;
        e.game_color = l.color;
        e.radius = l.radius;
        e.shader_name = l.shader_name;
        bool stale;
        if (!e.measured || e.inherited) {
            stale = e.tried_frame == 0 || light_frame >= e.tried_frame + FillVisRetryFrames;
        } else {
            stale = fill_vis_refresh != 0 && light_frame >= e.measured_frame + fill_vis_refresh;
        }
        if (!e.queued && stale && fill_vis_queue.size() < MaxQueue) {
            e.world = l.world;
            e.queued = true;
            fill_vis_queue.push_back(key);
        }
        // A latched emitter keeps its flag whether or not it was measured this frame, so its
        // falloff does not flip between the game's cutoff and inverse square.
        bool cluster = false;
        if (!e.emitter) {
            const float c2 = fill_vis_cluster * fill_vis_cluster;
            for (const auto& p : fill_vis_emitter_pos) {
                const float dx = p[0] - l.world[0], dy = p[1] - l.world[1], dz = p[2] - l.world[2];
                if (dx * dx + dy * dy + dz * dz <= c2) {
                    cluster = true;
                    break;
                }
            }
            fill_vis_cluster_last += cluster ? 1u : 0u;
        }
        // A light that has moved (a carried torch, an enemy's lantern) is a real carried light:
        // designer fill lights never move. Its holder is a skinned mesh, which the enclosure
        // test ignores, so without this a torch held still would fade out.
        bool carried = false;
        if (!e.emitter) {
            for (const Rt::DirectTrack& t : direct_tracks.list) {
                if (t.mobile && Rt::WorldDistance2(l, t.light) <= 1.0f) {
                    carried = true;
                    break;
                }
            }
        }
        bool held = false;
        const float cam2 = l.position[0] * l.position[0] + l.position[1] * l.position[1] +
                           l.position[2] * l.position[2];
        if (cam2 <= Rt::HeldCameraDistance * Rt::HeldCameraDistance) {
            for (const auto& p : RecentFlames()) {
                const float dx = p[0] - l.world[0], dy = p[1] - l.world[1], dz = p[2] - l.world[2];
                if (dx * dx + dy * dy + dz * dz <= Rt::HeldFlameDistance * Rt::HeldFlameDistance) {
                    held = true;
                    break;
                }
            }
        }
        // Loot is drawn with the fire FX shader too, so "near a fire" alone is not a flame.
        const bool flame = Rt::FlameColour(l);
        held = held && flame;
        carried = carried && flame;
        l.held = held;
        l.emitter = flame && (e.emitter || cluster || carried || held);
        // A pale light whose colour pulses is an animated source (the messengers' bath in the
        // Hunter's Dream breathes on a ~1 s cycle): designer fills hold still. It keeps the game's
        // light as is, like the loot light below; only flames take the emitter falloff.
        if (!flame && e.pulse) {
            continue;
        }
        // The loot's own light (pale, next to its wisp) is real: it keeps the game's light as is,
        // instead of fading out like a designer fill light.
        if (!l.emitter && !flame) {
            bool wisp = false;
            for (const auto& p : RecentWisps()) {
                const float dx = p[0] - l.world[0], dy = p[1] - l.world[1], dz = p[2] - l.world[2];
                if (dx * dx + dy * dy + dz * dz <= Rt::HeldFlameDistance * Rt::HeldFlameDistance) {
                    wisp = true;
                    break;
                }
            }
            if (wisp) {
                continue;
            }
        }
        if (l.emitter && !carried && !held) {
            Rt::LampFloor(l, fill_vis_lamp_min);
        }
        if (carried && direct_light_log > 0.0f && light_frame >= fill_vis_carried_log + 60) {
            fill_vis_carried_log = light_frame;
            Log("RT carried light frame %llu: %s world (%.2f %.2f %.2f) view (%.2f %.2f %.2f) radius "
                "%.2f colour (%.3f %.3f %.3f) spec (%.3f %.3f %.3f) legacy %d kind %d shadowed %d\n",
                (unsigned long long)light_frame, l.shader_name ? l.shader_name : "?", l.world[0],
                l.world[1], l.world[2], l.position[0], l.position[1], l.position[2], l.radius,
                l.color[0], l.color[1], l.color[2], l.spec[0], l.spec[1], l.spec[2],
                l.legacy ? 1 : 0, int(l.kind), l.shadowed ? 1 : 0);
        }
        if (!e.measured) {
            continue;
        }
        if (e.step_frame != light_frame) {
            e.step_frame = light_frame;
            const float target =
                Rt::FillVisScale(fill_vis_sky_keep ? e.sky : 0.0f, l.emitter, fill_vis_sky_full, fill_vis_floor);
            e.applied = e.applied < target ? std::min(e.applied + FadeStep, target)
                                           : std::max(e.applied - FadeStep, target);
        }
        if (e.applied >= 1.0f) {
            continue;
        }
        for (float& c : l.color) {
            c *= e.applied;
        }
        for (float& c : l.spec) {
            c *= e.applied;
        }
        ++fill_vis_scaled_last;
        fill_vis_off_last += e.applied <= 0.0f ? 1u : 0u;
    }
    fill_vis_scaled += fill_vis_scaled_last;
}

void RayTracing::ReadFillVis() {
    if (!fill_vis_ring.mapped) {
        return;
    }
    constexpr float NoHit = 1000.0f; // the shader reports MissHitDistance when nothing was hit
    for (u32 s = 0; s < Slots; ++s) {
        if (fill_vis_frame[s] == ~0ull || fill_vis_frame[s] == frame ||
            !scheduler.IsFree(fill_vis_tick[s])) {
            continue;
        }
        fill_vis_frame[s] = ~0ull;
        const u8* out = fill_vis_ring.mapped + u64(s) * fill_vis_ring.slot_bytes +
                        u64(Rt::FillVisBatch) * 4 * sizeof(float);
        for (u32 i = 0; i < fill_vis_slot_count[s]; ++i) {
            const auto it = fill_vis.find(fill_vis_slot_keys[s][i]);
            if (it == fill_vis.end()) {
                continue;
            }
            std::array<float, 4 * Rt::FillVisOutputs> v{};
            std::memcpy(v.data(), out + u64(i) * sizeof(v), sizeof(v));
            for (size_t k = 0; k < v.size(); ++k) {
                // [9] (scene depth minus light depth) may be negative.
                v[k] = std::isfinite(v[k]) ? (k == 9 ? v[k] : std::max(v[k], 0.0f)) : 0.0f;
            }
            Rt::FillVisResult r;
            r.sky = std::min(v[0], 1.0f);
            r.emit = std::min(v[1], 1.0f);
            r.emit_luma = v[2];
            r.near_solid = std::min(v[3], NoHit);
            r.enclosed = std::min(v[4], 1.0f);
            r.blended = std::min(v[5], 1.0f);
            r.near_blended = std::min(v[6], NoHit);
            r.screen_luma = v[7];
            r.visible = v[8] > 0.5f;
            r.depth_gap = v[9];
            r.enclosed_up = std::min(v[10], 1.0f);
            FillVisEntry& e = it->second;
            e.queued = false;
            e.last = r;
            e.tried_frame = light_frame;
            // Sticky: any sign of an emitter (emissive-target hit, emissive pixels on screen
            // around the light, or the light enclosed by a fixture) keeps the light whole. This
            // holds even for an untrusted result, since a false emitter only keeps a fill light.
            e.emitter = e.emitter || Rt::FillVisEmitter(r, fill_vis_emit_luma, fill_vis_enclosed);
            const bool was_lamp = e.lamp;
            e.lamp = e.lamp || Rt::FillVisLamp(r, lamp_enclosed, lamp_up);
            e.fire_pit = e.fire_pit || Rt::FillVisFirePit(r, lamp_enclosed, lamp_up);
            if (e.lamp && !was_lamp) {
                Log("RT fill vis frame %llu: %s world (%.2f %.2f %.2f) radius %.2f latched as a lamp: "
                    "enclosed %.1f%% up %.1f%% blended %.1f%% nearest solid %.2f m visible %d\n",
                    (unsigned long long)light_frame, e.shader_name ? e.shader_name : "?",
                    e.world[0], e.world[1], e.world[2], e.radius, r.enclosed * 100.0f,
                    r.enclosed_up * 100.0f, r.blended * 100.0f, r.near_solid, r.visible ? 1 : 0);
            }
            // Only a light on screen and not hidden is trusted to fade: the TLAS holds the
            // geometry drawn this frame, so off screen the fixture around it may be missing.
            if (r.visible || (keep_gi && fill_vis_offscreen)) {
                e.measured = true;
                e.inherited = false;
                e.sky = r.sky;
                e.measured_frame = light_frame;
            }
            ++fill_vis_results;
            if (direct_light_log <= 0.0f) {
                continue;
            }
            const char* const name = e.shader_name ? e.shader_name : "?";
            if (!e.measured || e.inherited) {
                if (!e.logged_waiting) {
                    e.logged_waiting = true;
                    Log("RT fill vis frame %llu: %s world (%.2f %.2f %.2f) radius %.2f waiting: "
                        "light off screen or hidden (depth gap %.2f m), enclosed %.1f%% blended "
                        "%.1f%% emitter %s; keeps full strength, retry every %llu frames\n",
                        (unsigned long long)light_frame, name, e.world[0], e.world[1],
                        e.world[2], e.radius, r.depth_gap, r.enclosed * 100.0f,
                        r.blended * 100.0f, e.emitter ? "yes" : "no",
                        (unsigned long long)FillVisRetryFrames);
                }
                continue;
            }
            const float scale =
                Rt::FillVisScale(fill_vis_sky_keep ? e.sky : 0.0f, e.emitter, fill_vis_sky_full, fill_vis_floor);
            if (e.logged_scale >= 0.0f && std::fabs(scale - e.logged_scale) < 0.05f) {
                continue;
            }
            e.logged_scale = scale;
            Log("RT fill vis frame %llu: %s world (%.2f %.2f %.2f) radius %.2f sky %.3f, emitter %s "
                "(within %.2f m of %u rays: emissive %.1f%% max luma %.3f, enclosed %.1f%%, "
                "blended %.1f%%; nearest solid %.2f m blended %.2f m; screen %s gap %.2f m "
                "emissive luma %.3f) -> scale %.2f (BB_RT_FILL_SKY %.2f, BB_RT_FILL_FLOOR %.2f, "
                "BB_RT_FILL_ENCLOSED %.2f)\n",
                (unsigned long long)light_frame, name, e.world[0], e.world[1], e.world[2],
                e.radius, e.sky, e.emitter ? "yes" : "no", fill_vis_emit_dist, fill_vis_rays,
                r.emit * 100.0f, r.emit_luma, r.enclosed * 100.0f, r.blended * 100.0f,
                r.near_solid, r.near_blended, r.visible ? "on" : "hidden", r.depth_gap,
                r.screen_luma, scale, fill_vis_sky_full, fill_vis_floor, fill_vis_enclosed);
        }
    }
}

// The game's moon and cloud layer (rt_gi.comp RT_SKY_LAYER): trace the grid, then find the moon.
void RayTracing::RecordSkyLayer(vk::CommandBuffer cmdbuf,
                                std::span<const vk::WriteDescriptorSet> gi_writes) {
    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;
    if (!moon_from_sky || !sky_layer_pipeline || !moon_dir_pipeline || !material.buffer ||
        frame % Rt::SkyLayerEvery != 0) {
        return;
    }
    ReadMoonRing();
    const u64 key = sky_key.Recent(frame, Rt::SkyLayerEvery * 2) ? sky_key.Key() : 0;
    const std::array<float, 3> cam{frame_inv_view[3], frame_inv_view[7], frame_inv_view[11]};
    std::array<float, 3> seed_dir{};
    // The cached moon of this sky's draws, also while the camera looks away from the sky (the
    // game culls every cloud segment when the view points at the ground): without the seed the
    // moon ran out after MoonHold searches and light 1 fell back to the game's own direction,
    // 10-40 degrees off, so shadows swung. Held MoonSeedHold frames past the last layer draw so
    // an interior reached by a load does not keep the last map's moon.
    constexpr u64 MoonSeedHold = 1800;
    const bool seed = sky_key.Recent(frame, MoonSeedHold) && frame_camera_valid &&
                      moon_cache.DirFromDraws(sky_key.Draws(), cam, seed_dir);
    const bool reset = sky_key.Generation() != moon_generation;
    moon_generation = sky_key.Generation();
    if (reset) {
        moon_found_run = false;
        moon_last_found = false;
    }
    SkyLayerPush push{
        .layer_base = Rt::SkyLayerSlot(capacity.records, 0),
        .moon_slot = Rt::MoonDirSlot(capacity.records),
        .phase = 0u,
        .moon_min = moon_min,
        .moon_seed = seed ? std::array<float, 4>{seed_dir[0], seed_dir[1], seed_dir[2], 1.0f}
                          : std::array<float, 4>{},
        .moon_reset = (reset ? 1u : 0u) | (moon_layer_find ? 2u : 0u),
    };
    const vk::DescriptorSet surface_set =
        surface_last.set && surface_last.slot == built_slot &&
                surface_last.tick == scheduler.CurrentTick()
            ? surface_last.set
            : SurfaceSet(cmdbuf, built_slot);
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *sky_layer_pipeline);
    cmdbuf.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *sky_layer_pipeline_layout, 1,
                              surface_set, {});
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *sky_layer_pipeline_layout, 0,
                                gi_writes);
    cmdbuf.pushConstants(*sky_layer_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
                         sizeof(push), &push);
    cmdbuf.dispatch((Rt::SkyLayerWidth + 7) / 8, (Rt::SkyLayerHeight + 7) / 8, 1);
    Barrier(cmdbuf, Stage::eComputeShader, Access::eShaderWrite, Stage::eComputeShader,
            Access::eShaderRead | Access::eShaderWrite);
    push.phase = 1u;
    cmdbuf.pushConstants(*sky_layer_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
                         sizeof(push), &push);
    cmdbuf.dispatch(1, 1, 1);
    const u32 ring = u32(sky_layer_frames % Slots);
    if (moon_ring.mapped && moon_ring_frame[ring] == ~0ull) {
        // The moon slot and the search stats back to the CPU for the moon cache.
        Barrier(cmdbuf, Stage::eComputeShader, Access::eShaderWrite, Stage::eTransfer,
                Access::eTransferRead);
        const u64 dst = u64(ring) * moon_ring.slot_bytes;
        const std::array<vk::BufferCopy, 2> regions{{
            {u64(push.moon_slot) * 16, dst, 16},
            {u64(Rt::SkyLayerSlot(capacity.records, Rt::SkyLayerCells)) * 16, dst + 16, 64},
        }};
        cmdbuf.copyBuffer(material.buffer, moon_ring.buffer, regions);
        Barrier(cmdbuf, Stage::eTransfer, Access::eTransferWrite, Stage::eHost, Access::eHostRead);
        moon_ring_tick[ring] = scheduler.CurrentTick();
        moon_ring_frame[ring] = frame;
        // The cache is per layer draw (SkyKey::Draws), so a find counts while the layer draws,
        // also when no draw was in every frame of the run (Key() 0, kept here as 1): Cathedral
        // Ward under the blood moon dropped its finds ("sky 0000000000000000 found 30.63 degrees
        // from its cached position") until one cloud segment had stayed in view.
        const bool layer_draws =
            sky_key.Recent(frame, Rt::SkyLayerEvery * 2) && !sky_key.Draws().empty();
        moon_ring_key[ring] = key ? key : (layer_draws ? 1 : 0);
        moon_ring_gen[ring] = sky_key.Generation();
        moon_ring_view[ring] = frame_inv_view;
    }
    SkyImageIo(cmdbuf, key);
    if (sky_layer_frames++ == 0) {
        Log("Ray tracing: moon and cloud layer on (%ux%u grid, moon score > %.2f), moon cache %s "
            "(%zu skies)\n",
            Rt::SkyLayerWidth, Rt::SkyLayerHeight, moon_min, moon_cache_path.c_str(),
            moon_cache.Size());
    }
}

void RayTracing::SkyImageIo(vk::CommandBuffer cmdbuf, u64 key) {
    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;
    constexpr u64 cells = Rt::SkyImageCells;
    constexpr u64 bytes = cells * 16;
    const u64 offset = u64(Rt::SkyImageSlot(capacity.records, 0)) * 16;
    if (sky_io_failed || offset + bytes > material.size) {
        return;
    }
    const auto allocator = instance.GetAllocator();
    if (!sky_io_buffer) {
        const char* dir = Rt::Knob("BB_RT_SKY_CACHE");
        sky_cache_dir = dir && dir[0] ? dir : "rt_sky";
        // Cached host memory: the CPU reads the whole image back.
        const VkBufferCreateInfo buffer_ci{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = bytes,
            .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        };
        const VmaAllocationCreateInfo alloc_ci{
            .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                     VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO,
        };
        VkBuffer raw{};
        VmaAllocationInfo info{};
        if (vmaCreateBuffer(allocator, &buffer_ci, &alloc_ci, &raw, &sky_io_allocation, &info) !=
                VK_SUCCESS ||
            !info.pMappedData) {
            if (raw) {
                vmaDestroyBuffer(allocator, raw, sky_io_allocation);
            }
            sky_io_failed = true;
            Log("Ray tracing: sky image cache off (no %llu MiB host buffer)\n",
                (unsigned long long)(bytes >> 20));
            return;
        }
        sky_io_buffer = raw;
        sky_io_mapped = static_cast<float*>(info.pMappedData);
    }
    const auto path_of = [&](u64 k) {
        char name[32];
        std::snprintf(name, sizeof(name), "/%016llx.bin", (unsigned long long)k);
        return sky_cache_dir + name;
    };
    if (sky_io != SkyIo::Idle) {
        if (!scheduler.IsFree(sky_io_tick)) {
            return;
        }
        if (sky_io == SkyIo::Readback && key && key == sky_io_key &&
            sky_key.Generation() == sky_io_generation) {
            vmaInvalidateAllocation(allocator, sky_io_allocation, 0, VK_WHOLE_SIZE);
            size_t seen = 0;
            for (u64 i = 0; i < cells; ++i) {
                seen += sky_io_mapped[i * 4 + 3] > 0.0f ? 1 : 0;
            }
            size_t& saved = sky_saved_seen[key];
            // Saved again once it grew by 0.5 % (a look at new sky, not capture noise).
            if (seen > saved + saved / 200 + 64) {
                const auto start = std::chrono::steady_clock::now();
                std::error_code ec;
                std::filesystem::create_directories(sky_cache_dir, ec);
                const std::string path = path_of(key);
                const bool ok = Rt::SaveSkyFile(
                    path.c_str(), Rt::EncodeSkyWords(Rt::PackSkyCells(sky_io_mapped, cells, nullptr)));
                const double ms = std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - start)
                                      .count();
                Log("Ray tracing: sky image of sky %016llx saved to %s, %zu cells (%.1f %%), "
                    "%.1f ms%s\n",
                    (unsigned long long)key, path.c_str(), seen, 100.0 * double(seen) / double(cells),
                    ms, ok ? "" : " FAILED");
                if (ok) {
                    saved = seen;
                }
            }
        }
        sky_io = SkyIo::Idle;
    }
    if (!key) {
        return;
    }
    const u64 generation = sky_key.Generation();
    if (generation != sky_seed_generation || key != sky_seed_key) {
        // A new sky (or the key narrowed): its cached image, else nothing from the last sky.
        const bool new_run = generation != sky_seed_generation;
        sky_seed_generation = generation;
        sky_seed_key = key;
        sky_io_next = frame + SkyIoEvery;
        const std::string path = path_of(key);
        std::vector<u32> encoded, words;
        if (Rt::LoadSkyFile(path.c_str(), encoded) && Rt::DecodeSkyWords(encoded, cells, words)) {
            const size_t n = Rt::UnpackSkyCells(words, sky_io_mapped);
            vmaFlushAllocation(allocator, sky_io_allocation, 0, VK_WHOLE_SIZE);
            Barrier(cmdbuf, Stage::eComputeShader, Access::eShaderRead | Access::eShaderWrite,
                    Stage::eTransfer, Access::eTransferWrite);
            cmdbuf.copyBuffer(sky_io_buffer, material.buffer, vk::BufferCopy{0, offset, bytes});
            Barrier(cmdbuf, Stage::eTransfer, Access::eTransferWrite, Stage::eComputeShader,
                    Access::eShaderRead | Access::eShaderWrite);
            sky_io = SkyIo::Upload;
            sky_io_tick = scheduler.CurrentTick();
            sky_saved_seen[key] = std::max(sky_saved_seen[key], n);
            Log("Ray tracing: sky image of sky %016llx loaded from %s, %zu cells (%.1f %%)\n",
                (unsigned long long)key, path.c_str(), n, 100.0 * double(n) / double(cells));
        } else if (new_run) {
            Barrier(cmdbuf, Stage::eComputeShader, Access::eShaderRead | Access::eShaderWrite,
                    Stage::eTransfer, Access::eTransferWrite);
            cmdbuf.fillBuffer(material.buffer, offset, bytes, 0u);
            Barrier(cmdbuf, Stage::eTransfer, Access::eTransferWrite, Stage::eComputeShader,
                    Access::eShaderRead | Access::eShaderWrite);
            Log("Ray tracing: sky image of sky %016llx not cached yet (%s)\n",
                (unsigned long long)key, path.c_str());
        }
        return;
    }
    if (frame < sky_io_next) {
        return;
    }
    sky_io_next = frame + SkyIoEvery;
    Barrier(cmdbuf, Stage::eComputeShader, Access::eShaderWrite, Stage::eTransfer,
            Access::eTransferRead);
    cmdbuf.copyBuffer(material.buffer, sky_io_buffer, vk::BufferCopy{offset, 0, bytes});
    Barrier(cmdbuf, Stage::eTransfer, Access::eTransferWrite, Stage::eHost, Access::eHostRead);
    sky_io = SkyIo::Readback;
    sky_io_tick = scheduler.CurrentTick();
    sky_io_key = key;
    sky_io_generation = generation;
}

void RayTracing::ReadMoonRing() {
    if (moon_cache_path.empty()) {
        const char* path = Rt::Knob("BB_RT_MOON_CACHE");
        moon_cache_path = path && path[0] ? path : "rt_moon.txt";
        moon_cache.Load(moon_cache_path.c_str());
    }
    for (u32 i = 0; i < Slots; ++i) {
        if (moon_ring_frame[i] == ~0ull || !scheduler.IsFree(moon_ring_tick[i])) {
            continue;
        }
        const auto* v = reinterpret_cast<const float*>(moon_ring.mapped + u64(i) * moon_ring.slot_bytes);
        const u64 key = moon_ring_key[i];
        const u64 ring_frame = moon_ring_frame[i];
        moon_ring_frame[i] = ~0ull;
        {
            // Where light 1 points from (rt_moon_dir.comp): the sky moon, or the game's own
            // per-map directions once the slot runs out.
            const int source =
                !(v[3] > 0.0f) ? 0 : (v[3] >= Rt::MoonHoldSearches - 1.0f ? 1 : 2);
            // While no sky moon is known, the search stats every ~5 s: peak score, ring mean
            // (found needs peak > BB_RT_MOON_MIN and > 4 x ring) and the peak's grid cell.
            // Also while a cached moon lights the scene but this run has not confirmed a disc.
            if ((source == 0 || !moon_found_run) && ring_frame >= moon_stats_logged + 300) {
                moon_stats_logged = ring_frame;
                Log("Ray tracing: moon search at frame %llu: peak %.3f, ring %.3f, cell %.0f %.0f, "
                    "w %.0f, light %.3f %.3f %.3f\n",
                    (unsigned long long)ring_frame, v[4], v[5], v[6], v[7], v[3], v[0], v[1], v[2]);
            }
            // BB_RT_MOON_SCREEN_LOG=1: where the light-1 direction lands on screen (uv 0..1, y
            // down) every ~5 s, to check it against the moon the game draws.
            if (Rt::KnobIs("BB_RT_MOON_SCREEN_LOG", '1') && source != 0 &&
                ring_frame >= moon_screen_logged + 300) {
                moon_screen_logged = ring_frame;
                const Rt::Affine& iv = moon_ring_view[i];
                const float vx = iv[0] * v[0] + iv[4] * v[1] + iv[8] * v[2];
                const float vy = iv[1] * v[0] + iv[5] * v[1] + iv[9] * v[2];
                const float vz = iv[2] * v[0] + iv[6] * v[1] + iv[10] * v[2];
                const float u = vz > 1e-4f ? 0.5f + 0.5f * vx * frame_proj_scale[0] / vz : -1.0f;
                const float w = vz > 1e-4f ? 0.5f - 0.5f * vy * frame_proj_scale[1] / vz : -1.0f;
                Log("Ray tracing: moon light on screen at frame %llu: uv %.3f %.3f (view %.3f %.3f "
                    "%.3f, world %.3f %.3f %.3f, proj %.3f %.3f, sky %016llx, camera %.1f %.1f "
                    "%.1f, right %.3f %.3f %.3f, up %.3f %.3f %.3f, forward %.3f %.3f %.3f; "
                    "search peak %.3f ring %.3f cell %.0f %.0f w %.0f "
                    "found %.3f %.3f %.3f; screen moon %.3f %.3f %.3f, peak %.3f mean %.3f spread "
                    "%.1f bright samples %.0f)\n",
                    (unsigned long long)ring_frame, u, w, vx, vy, vz, v[0], v[1], v[2],
                    frame_proj_scale[0], frame_proj_scale[1], (unsigned long long)key, iv[3],
                    iv[7], iv[11], iv[0], iv[4], iv[8], iv[1], iv[5], iv[9], iv[2], iv[6], iv[10],
                    v[4], v[5], v[6], v[7], v[3], v[9],
                    v[10], v[11], v[12], v[13], v[14], v[16], v[17], v[18], v[19]);
            }
            if (source != moon_source_logged) {
                static constexpr const char* names[] = {"the game's sets (no sky moon)",
                                                        "the sky moon", "the sky moon, fading out"};
                Log("Ray tracing: moon light from %s at frame %llu (sky %016llx, w %.0f, "
                    "direction %.3f %.3f %.3f)\n",
                    names[source], (unsigned long long)ring_frame, (unsigned long long)key, v[3],
                    v[0], v[1], v[2]);
                moon_source_logged = source;
            }
        }
        // w == MoonHold (rt_gi.comp) only on the search that found the disc; a kept moon after a
        // find this run is the same disc, so it also fills a key that narrowed since.
        const bool found = v[3] == Rt::MoonHoldSearches;
        // v[8]: the disc's distance along the found direction (rt_gi.comp LayerHitT); v[9..11]
        // the found direction itself (v[0..2] is the light, which follows the cached moon).
        if (found && v[8] > 0.0f) {
            // Two finds in a row within MoonCache::Same confirm the disc; its world position is
            // the camera of that search plus the distance along the direction.
            const bool found_dir = v[9] != 0.0f || v[10] != 0.0f || v[11] != 0.0f;
            const std::array<float, 3> dir = found_dir ? std::array<float, 3>{v[9], v[10], v[11]}
                                                       : std::array<float, 3>{v[0], v[1], v[2]};
            bool confirmed = false;
            if (moon_last_found &&
                dir[0] * moon_last[0] + dir[1] * moon_last[1] + dir[2] * moon_last[2] >
                    std::cos(Rt::MoonCache::Same)) {
                const Rt::Affine& iv = moon_ring_view[i];
                moon_confirmed = {iv[3] + dir[0] * v[8], iv[7] + dir[1] * v[8],
                                  iv[11] + dir[2] * v[8]};
                // First confirmed find of this run: how far the cached moon (the seed) was off.
                std::array<float, 3> cached{};
                if (!moon_found_run &&
                    moon_cache.DirFromDraws(sky_key.Draws(), {iv[3], iv[7], iv[11]}, cached)) {
                    const float d = cached[0] * dir[0] + cached[1] * dir[1] + cached[2] * dir[2];
                    Log("Ray tracing: moon of sky %016llx found %.2f degrees from its cached "
                        "position (camera %.1f %.1f %.1f)\n",
                        (unsigned long long)key,
                        std::acos(std::clamp(d, -1.0f, 1.0f)) * 57.29578f, iv[3], iv[7], iv[11]);
                }
                moon_found_run = true;
                confirmed = true;
            }
            moon_last = dir;
            moon_last_found = true;
            // Each confirmed find once into the cache's running mean (rt_moon_cache.h).
            if (!confirmed || key == 0 || moon_ring_gen[i] != sky_key.Generation() ||
                !moon_cache.PutDraws(sky_key.Draws(), moon_confirmed)) {
                continue;
            }
        } else {
            continue;
        }
        const bool saved = moon_cache.Save(moon_cache_path.c_str());
        std::array<float, 3> mean = moon_confirmed;
        moon_cache.FindDraws(sky_key.Draws(), mean);
        const Rt::Affine& iv = moon_ring_view[i];
        Log("Ray tracing: moon of sky %016llx cached at (%.1f %.1f %.1f), mean of the finds (last "
            "find %.1f %.1f %.1f, camera %.1f %.1f %.1f, direction %.4f %.4f %.4f) to %s%s\n",
            (unsigned long long)key, mean[0], mean[1], mean[2], moon_confirmed[0],
            moon_confirmed[1], moon_confirmed[2], iv[3], iv[7], iv[11], moon_last[0], moon_last[1],
            moon_last[2], moon_cache_path.c_str(), saved ? "" : " FAILED");
    }
}

void RayTracing::RecordFillVis(vk::CommandBuffer cmdbuf,
                               std::span<const vk::WriteDescriptorSet> gi_writes,
                               const Rt::Affine& view, u32 slot) {
    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;
    constexpr size_t MaxWrites = 24;
    ReadFillVis();
    if (!fill_vis_on || !fill_vis_pipeline || !fill_vis_ring.mapped || fill_vis_queue.empty() ||
        fill_vis_frame[slot] != ~0ull || gi_writes.size() + 1 != MaxWrites) {
        return;
    }
    u8* base = fill_vis_ring.mapped + u64(slot) * fill_vis_ring.slot_bytes;
    u32 count = 0;
    size_t taken = 0;
    for (; taken < fill_vis_queue.size() && count < Rt::FillVisBatch; ++taken) {
        const u64 key = fill_vis_queue[taken];
        const auto it = fill_vis.find(key);
        if (it == fill_vis.end()) {
            continue;
        }
        const std::array<float, 3> p = Rt::Transform(view, it->second.world);
        const std::array<float, 4> in{p[0], p[1], p[2], it->second.radius};
        std::memcpy(base + u64(count) * sizeof(in), in.data(), sizeof(in));
        fill_vis_slot_keys[slot][count++] = key;
    }
    fill_vis_queue.erase(fill_vis_queue.begin(), fill_vis_queue.begin() + std::ptrdiff_t(taken));
    if (count == 0) {
        return;
    }
    std::array<vk::WriteDescriptorSet, MaxWrites> writes{};
    std::copy(gi_writes.begin(), gi_writes.end(), writes.begin());
    const vk::DescriptorBufferInfo vis_info{fill_vis_ring.buffer,
                                            u64(slot) * fill_vis_ring.slot_bytes,
                                            fill_vis_ring.slot_bytes};
    writes[MaxWrites - 1] = vk::WriteDescriptorSet{.dstBinding = 28,
                                                   .descriptorCount = 1,
                                                   .descriptorType = vk::DescriptorType::eStorageBuffer,
                                                   .pBufferInfo = &vis_info};
    const FillVisPush push{
        .count = count,
        .rays = fill_vis_rays,
        .offset = 0.02f,
        .emit_dist = fill_vis_emit_dist,
        .emit_luma = fill_vis_emit_luma,
        .smoke = fx_smoke_opacity,
    };
    const vk::DescriptorSet surface_set =
        surface_last.set && surface_last.slot == built_slot &&
                surface_last.tick == scheduler.CurrentTick()
            ? surface_last.set
            : SurfaceSet(cmdbuf, built_slot);
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *fill_vis_pipeline);
    cmdbuf.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *fill_vis_pipeline_layout, 1,
                              surface_set, {});
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *fill_vis_pipeline_layout, 0,
                                writes);
    cmdbuf.pushConstants(*fill_vis_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
                         sizeof(push), &push);
    cmdbuf.dispatch(count, 1, 1);
    Barrier(cmdbuf, Stage::eComputeShader, Access::eShaderWrite, Stage::eHost, Access::eHostRead);
    fill_vis_tick[slot] = scheduler.CurrentTick();
    fill_vis_frame[slot] = frame;
    fill_vis_slot_count[slot] = count;
    ++fill_vis_dispatches;
}

void RayTracing::OpenLightDump() {
    const char* dump_env = Rt::Knob("BB_RT_LIGHT_DUMP");
    if (light_dump || !dump_env || !dump_env[0] || dump_env[0] == '0') {
        return;
    }
    const char* path = dump_env[0] == '1' && dump_env[1] == 0 ? "rt_lights.txt" : dump_env;
    light_dump = std::fopen(path, "w");
    light_dump_frames = 0;
    Log("Ray tracing: light dump to %s (%s) from frame %llu\n", path,
        light_dump ? "open" : "cannot open", (unsigned long long)frame);
}

void RayTracing::DumpLights() {
    ++light_dump_frames;
    std::fprintf(light_dump, "frame %llu: %zu light draws, %zu lights parsed\n",
                 (unsigned long long)light_frame, light_draws.size(), pending_lights.size());
    for (size_t d = 0; d < light_draws.size(); ++d) {
        const LightDrawRecord& rec = light_draws[d];
        const Rt::LightDrawState& st = rec.state;
        std::fprintf(light_dump,
                     " draw %zu %s ps %08x: stencil %08x, targets %u/%u mask %x/%x blend %u(%u,%u,%u)/%u(%u,%u,%u) "
                     "depth %u func %u bounds %u [%.6f %.6f] write %u cull %u vtx %u z %.6f, param %u bytes, "
                     "lights %u bytes, dir %u bytes, %u other",
                     d, rec.shader->name, rec.shader->ps_hash, rec.stencil, st.diffuse, st.specular,
                     st.mask0, st.mask1, u32(st.blend0), st.src0, st.dst0, st.func0, u32(st.blend1),
                     st.src1, st.dst1, st.func1, u32(st.depth), st.zfunc, u32(st.bounds), st.bounds_min,
                     st.bounds_max, u32(st.depth_write), st.cull, u32(st.has_vtx), st.vtx_z,
                     rec.param_bytes, rec.light_bytes, rec.dir_bytes, rec.others);
        for (u32 i = 0; i < std::min<u32>(rec.others, u32(rec.other_bytes.size())); ++i) {
            std::fprintf(light_dump, " %u", rec.other_bytes[i]);
        }
        std::fprintf(light_dump, "\n");
        const auto floats = [&](const char* label, const float* f, u32 bytes, u32 per_line) {
            if (bytes == 0) {
                return;
            }
            std::fprintf(light_dump, "  %s:", label);
            for (u32 i = 0; i < bytes / 4; ++i) {
                if (i % per_line == 0) {
                    std::fprintf(light_dump, "\n   [%3u]", i);
                }
                std::fprintf(light_dump, " %10.4g", f[i]);
            }
            std::fprintf(light_dump, "\n");
        };
        floats("cbLightAccParam", rec.param.data(), rec.param_bytes, 4);
        const u32 stride = rec.light_bytes ? Rt::LightStride(*rec.shader, rec.light_bytes) : 0;
        floats("cbLightAccLightParam", rec.light_data.data(), rec.light_bytes,
               stride ? stride / 4 : 4);
        floats("cbLight", rec.dir.data(), rec.dir_bytes, 4);
    }
    for (const Rt::Light& l : pending_lights) {
        std::fprintf(light_dump,
                     " light draw %u index %u kind %u shadowed %u pos %.3f %.3f %.3f dir %.3f %.3f "
                     "%.3f radius %.3f color %.3f %.3f %.3f\n",
                     l.draw, l.index, u32(l.kind), u32(l.shadowed), l.position[0],
                     l.position[1], l.position[2], l.direction[0], l.direction[1],
                     l.direction[2], l.radius, l.color[0], l.color[1], l.color[2]);
    }
    std::fflush(light_dump);
}

void RayTracing::OnLightTargets(VideoCore::ImageId diffuse, VideoCore::ImageId specular) {
    if (!(direct_enabled || reflect_enabled) || !diffuse || !specular) {
        return;
    }
    if (texture_cache.GetImage(diffuse).info.pixel_format != vk::Format::eB10G11R11UfloatPack32 ||
        texture_cache.GetImage(specular).info.pixel_format != vk::Format::eB10G11R11UfloatPack32) {
        return;
    }
    if (DirectTrace() &&
        (light_draw_ps == Rt::DeferredAoShader || acc_frame != frame || diffuse != acc_diffuse ||
         specular != acc_specular)) {
        Trace("light draw %u ps %08llx targets %u and %u%s", frame_light_draws,
              (unsigned long long)light_draw_ps, diffuse.index, specular.index,
              light_draw_ps == Rt::DeferredAoShader ? " (AO pass)" : "");
        if (light_draw_ps == Rt::DeferredAoShader) {
            TraceImage("AO mrt0", diffuse);
            TraceImage("AO mrt1", specular);
        }
    }
    acc_diffuse = diffuse;
    acc_specular = specular;
    acc_frame = frame;
}

void RayTracing::OnAoInput(VideoCore::ImageId image) {
    if (!image) {
        return;
    }
    const auto& ao_info = texture_cache.GetImage(image).info;
    if (ao_enabled && ao_info.size.width == ao_main_width) {
        if (ao_input_frame != frame) {
            ao_input_frame = frame;
            ao_input_count = 0;
        }
        bool seen = false;
        for (u32 i = 0; i < ao_input_count; ++i) {
            seen = seen || ao_inputs[i] == image;
        }
        if (!seen && ao_input_count < ao_inputs.size() &&
            ao_info.pixel_format == vk::Format::eR8G8B8A8Unorm &&
            ao_info.type == AmdGpu::ImageType::Color2D && ao_info.resources.levels == 1 &&
            ao_info.size.width > 1 && ao_info.size.height > 1) {
            ao_inputs[ao_input_count++] = image;
        }
    }
    if (!reflect_enabled) {
        return;
    }
    const auto& info = texture_cache.GetImage(image).info;
    const bool acc_now = acc_frame == frame && acc_diffuse;
    const auto format = info.pixel_format;
    const bool target_size =
        acc_now && info.size.width == texture_cache.GetImage(acc_diffuse).info.size.width &&
        info.size.height == texture_cache.GetImage(acc_diffuse).info.size.height;
    const auto kind = Rt::ClassifyAoInput(
        format == vk::Format::eB10G11R11UfloatPack32,
        format == vk::Format::eR16G16B16A16Sfloat || format == vk::Format::eR32G32B32A32Sfloat,
        info.resources.levels, target_size);
    static u32 logged = 0;
    if (logged < 24) {
        ++logged;
        Log("Ray tracing: AO draw frame %llu binds image %u %s %ux%u levels %u (acc pair %u/%u %s)"
            " -> %s\n",
            (unsigned long long)frame, image.index, vk::to_string(format).c_str(),
            info.size.width, info.size.height, info.resources.levels, acc_diffuse.index,
            acc_specular.index, acc_now ? "this frame" : "stale",
            image == acc_diffuse || image == acc_specular ? "own target"
            : kind == Rt::AoInput::Reject                 ? "rejected"
            : kind == Rt::AoInput::HdrAlpha               ? "pre-AO input (alpha)"
                                                          : "pre-AO input");
    }
    if (!acc_now) {
        return;
    }
    pre_ao_pair.Input(frame, image, acc_diffuse, acc_specular, kind);
}

void RayTracing::OnDirShadowInput(VideoCore::ImageId image) {
    if (!image) {
        return;
    }
    if (dir_shadow_frame != frame) {
        dir_shadow_frame = frame;
        dir_shadow_input_count = 0;
    }
    for (u32 i = 0; i < dir_shadow_input_count; ++i) {
        if (dir_shadow_inputs[i] == image) {
            return;
        }
    }
    if (dir_shadow_input_count < dir_shadow_inputs.size()) {
        dir_shadow_inputs[dir_shadow_input_count++] = image;
    }
}

void RayTracing::OnCompositeInput(VideoCore::ImageId image, bool proxy) {
    if (DirectTrace() && image) {
        char what[64];
        std::snprintf(what, sizeof(what), "%s samples (%s)", HookName(),
                      proxy ? "proxy" : "native");
        TraceImage(what, image);
    }
    if (!direct_enabled || !image || direct_written_frame != frame || composite_logged >= 3) {
        return;
    }
    const auto& img = texture_cache.GetImage(image);
    if (img.info.pixel_format != vk::Format::eB10G11R11UfloatPack32) {
        return;
    }
    ++composite_logged;
    Log("Ray tracing: %s draw frame %llu samples image %u (%s) %ux%u; direct wrote images "
        "%u and %u (%s)\n",
        HookName(), (unsigned long long)frame, image.index, proxy ? "proxy" : "native",
        img.info.size.width, img.info.size.height, direct_written_diffuse.index,
        direct_written_specular.index, direct_wrote_proxy ? "proxy" : "native");
}

bool RayTracing::SkipGameAo() const {
    // Same gate as Rt::SkipLightDraw: only while our direct pass wrote last frame and is not
    // paused or comparing against the game (debug 3).
    return direct_enabled && !direct_base && !ao_enabled && !game_ao && direct_debug != 3 &&
           direct_written_frame + 1 == frame && !Rt::effects_paused.load(std::memory_order_relaxed);
}

bool RayTracing::SkipShadowMaps() const {
    return skip_shadows && direct_enabled && !direct_base && direct_debug != 3 && !fog_stock &&
           !moon_mask &&
           direct_written_frame + 1 == frame && !Rt::effects_paused.load(std::memory_order_relaxed);
}

// The volume pass draws after our direct pass in the same frame, so the frame marker may
// already be current (>=), unlike the AO and shadow draws that run before it.
bool RayTracing::SkipHalfResLight(VideoCore::ImageId target, bool blended, u64 ps_hash) const {
    // Only moon (directional) and shadow-mapped lights: the game's shadow maps are skipped, so
    // those draw unshadowed (the hard-edged DDirLight1 patch). Unshadowed lamp lights stay: with
    // every half-res light gone the ground mist the game lights from this buffer turned into a
    // flat grey veil over everything in it (Old Yharnam ravens, 2026-11-13).
    const Rt::LightShader* shader = Rt::FindLightShader(ps_hash);
    if (shader && shader->kind != Rt::LightKind::Directional && !shader->shadowed) {
        return false;
    }
    if (!blended || !target || !direct_written_diffuse || !enabled || !skip_volumes ||
        !direct_enabled || fog_stock || direct_debug == 3 || direct_written_frame + 1 < frame ||
        Rt::effects_paused.load(std::memory_order_relaxed)) {
        return false;
    }
    return texture_cache.GetImage(target).info.size.width <
           texture_cache.GetImage(direct_written_diffuse).info.size.width;
}

bool RayTracing::SkipGameVolumes() const {
    return skip_volumes && fog_enabled && !fog_stock && direct_enabled && !direct_base &&
           direct_debug != 3 && direct_written_frame + 1 >= frame &&
           !Rt::effects_paused.load(std::memory_order_relaxed);
}

void RayTracing::OnAoDraw(const SceneInputs& inputs) {
    if (!enabled || !ao_enabled || !inputs.depth) {
        return;
    }
    const u32 depth_width = texture_cache.GetImage(inputs.depth).info.size.width;
    ao_main_width = std::max(ao_main_width, depth_width);
    if (ao_frame == frame || depth_width < ao_main_width) {
        return;
    }
    ao_frame = frame;
    if (Rt::effects_paused.load(std::memory_order_relaxed)) {
        return;
    }
    RecordAo(inputs);
}

void RayTracing::RecordAo(const SceneInputs& inputs) {
    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;
    const auto skip = [&](const char* why) {
        ++ao_skipped;
        static const char* last = nullptr;
        static u32 printed = 0;
        if (why != last && printed < 16) {
            last = why;
            ++printed;
            Log("Ray tracing: AO skipped at frame %llu (%s)\n", (unsigned long long)frame, why);
        }
    };
    const bool camera = inputs.camera_valid && inputs.projection[0] != 0.0f &&
                        inputs.projection[1] != 0.0f && inputs.projection[3] != 0.0f;
    if (!inputs.depth || !camera) {
        skip("no depth or camera this frame");
        return;
    }
    if (ao_input_frame == ~0ull || ao_input_frame + 4 < frame) {
        skip("no AO draw inputs captured yet");
        return;
    }
    const VideoCore::ImageId mask_id = Rt::PickAoMask<VideoCore::ImageId>(
        {ao_inputs.data(), ao_input_count}, {ao_gbuffer.data(), ao_gbuffer_count}, inputs.depth);
    const VideoCore::Image* mask_image = mask_id ? texture_cache.TryGetImage(mask_id, 0) : nullptr;
    if (!mask_image || !mask_image->backing || !mask_image->backing->image.image) {
        skip("no SSAO mask among the AO draw inputs");
        return;
    }
    {
        const auto& depth_info = texture_cache.GetImage(inputs.depth).info;
        if (mask_image->info.size.width != depth_info.size.width ||
            mask_image->info.size.height != depth_info.size.height) {
            skip("SSAO mask is not the depth size");
            return;
        }
    }
    if (instances.empty()) {
        skip("no G-buffer geometry this frame");
        return;
    }
    if (!Build(inputs)) {
        skip("acceleration structure not built");
        return;
    }
    if (auto* profiler = GpuProfiler::Get()) {
        profiler->Mark(0xB1A5'0009ull, [] { return std::string{"ray tracing ao"}; });
    }
    scheduler.EndRendering();
    Bound depth{}, mask{}, normal{};
    if (!BindScene(inputs.depth, inputs, Access::eShaderRead, depth) ||
        !BindScene(mask_id, inputs, Access::eShaderRead | Access::eShaderWrite, mask) ||
        mask.width != depth.width || mask.height != depth.height) {
        runtime.FlushBarriers();
        skip("depth or SSAO mask not bindable at one size");
        return;
    }
    bool normal_valid = false;
    if (normal_source.target != 0xFF && normal_source.target < ao_gbuffer_count &&
        ao_gbuffer[normal_source.target]) {
        Bound bound{};
        normal_valid = BindScene(ao_gbuffer[normal_source.target], inputs, Access::eShaderRead,
                                 bound) &&
                       bound.width == depth.width && bound.height == depth.height;
        if (normal_valid) {
            normal = bound;
        }
    }
    runtime.FlushBarriers();
    const u32 cells_x = Rt::AoCells(depth.width, ao_scale), cells_y = Rt::AoCells(depth.height, ao_scale);
    if (ao_width != cells_x || ao_height != cells_y) {
        if (!CreateAoImages(cells_x, cells_y)) {
            skip("AO history allocation failed");
            return;
        }
    }
    const bool history = ao_history_valid && ao_history_frame + 1 == frame &&
                         !Rt::CameraCut(ao_prev_inv_view, inputs.inv_view, 3.0f, 0.9f);
    const Rt::Affine view = Rt::InverseRigid(inputs.inv_view);
    const u32 slot = u32(frame % Slots);
    ReadAoStats();
    std::memset(ao_stats_ring.mapped + u64(slot) * ao_stats_ring.slot_bytes, 0, ao_stats_ring.slot_bytes);
    const Rt::AoParams params{
        .proj = inputs.projection,
        .view_rows = Rt::Rows(view),
        .size_jitter = {float(depth.width), float(depth.height), inputs.jitter[0], inputs.jitter[1]},
        .tlas_rays_flags = {u32(tlas_address), u32(tlas_address >> 32), ao_rays,
                            (history ? Rt::AoFlagHistory : 0u) | (normal_valid ? Rt::AoFlagNormal : 0u)},
        .cfg = {normal_source.mode | normal_source.swizzle << 8 | normal_source.components << 16,
                u32(frame), ao_max_history, ao_debug},
        .radius_bias_tmin = {ao_radius, shadow_bias, shadow_tmin, float((ao_falloff ? 1u : 0u) | (ao_occlude_y ? 2u : 0u))},
        .prev_proj = ao_prev_projection,
        .reproject = Rt::Rows(history ? Rt::Reprojection(ao_prev_inv_view, inputs.inv_view) : view),
        .prev_jitter = {ao_prev_jitter[0], ao_prev_jitter[1], 0.0f, 0.0f},
        .cell = {ao_scale, cells_x, cells_y, 0u},
    };
    std::memcpy(ao_params_ring.mapped + u64(slot) * ao_params_ring.slot_bytes, &params, sizeof(params));
    const auto cmdbuf = scheduler.CommandBuffer();
    TimeBegin(cmdbuf, 6u);
    const u32 in = ao_history_index, out = ao_history_index ^ 1u;
    const auto all = Stage::eAllCommands;
    const std::array<vk::ImageMemoryBarrier2, 2> to_general = {{
        ImageBarrier(vk::Image(ao_history[in].image),
                     ao_history[in].written ? vk::ImageLayout::eGeneral : vk::ImageLayout::eUndefined,
                     all, Access::eShaderWrite, Stage::eComputeShader, Access::eShaderRead),
        ImageBarrier(vk::Image(ao_history[out].image), vk::ImageLayout::eUndefined, all,
                     Access::eShaderRead, Stage::eComputeShader, Access::eShaderWrite),
    }};
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = u32(to_general.size()),
                             .pImageMemoryBarriers = to_general.data()});
    const auto image = [](vk::ImageView view) {
        return vk::DescriptorImageInfo{.imageView = view, .imageLayout = vk::ImageLayout::eGeneral};
    };
    const vk::DescriptorImageInfo depth_info = image(depth.view);
    const vk::DescriptorImageInfo mask_info = image(mask.view);
    const vk::DescriptorImageInfo normal_info = image(normal_valid ? normal.view : depth.view);
    const vk::DescriptorImageInfo history_in_info = image(*ao_history[in].view);
    const vk::DescriptorImageInfo history_out_info = image(*ao_history[out].view);
    const vk::DescriptorBufferInfo params_info{ao_params_ring.buffer,
                                               u64(slot) * ao_params_ring.slot_bytes, sizeof(params)};
    const vk::DescriptorBufferInfo stats_info{ao_stats_ring.buffer,
                                              u64(slot) * ao_stats_ring.slot_bytes, ao_stats_ring.slot_bytes};
    const vk::DescriptorBufferInfo surfaces_info{surfaces_ring.buffer,
                                                 u64(built_slot) * surfaces_ring.slot_bytes,
                                                 surfaces_ring.slot_bytes};
    const vk::DescriptorBufferInfo uv_pool_info{uv_buffer.buffer ? uv_buffer.buffer : soup.buffer, 0,
                                                uv_buffer.buffer ? uv_buffer.size : soup.size};
    const auto write = [](u32 binding, vk::DescriptorType type, const vk::DescriptorImageInfo* image,
                          const vk::DescriptorBufferInfo* buffer) {
        return vk::WriteDescriptorSet{.dstBinding = binding,
                                      .descriptorCount = 1,
                                      .descriptorType = type,
                                      .pImageInfo = image,
                                      .pBufferInfo = buffer};
    };
    using Type = vk::DescriptorType;
    const std::array<vk::WriteDescriptorSet, 9> writes = {{
        write(0, Type::eSampledImage, &depth_info, nullptr),
        write(1, Type::eStorageImage, &mask_info, nullptr),
        write(2, Type::eSampledImage, &normal_info, nullptr),
        write(3, Type::eSampledImage, &history_in_info, nullptr),
        write(4, Type::eStorageImage, &history_out_info, nullptr),
        write(5, Type::eUniformBuffer, nullptr, &params_info),
        write(6, Type::eStorageBuffer, nullptr, &stats_info),
        write(12, Type::eStorageBuffer, nullptr, &surfaces_info),
        write(13, Type::eStorageBuffer, nullptr, &uv_pool_info),
    }};
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *ao_pipeline);
    cmdbuf.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *ao_pipeline_layout, 1,
                              SurfaceSet(cmdbuf, built_slot), {});
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *ao_pipeline_layout, 0, writes);
    const u32 trace_pass = 0, resolve_pass = 1;
    cmdbuf.pushConstants(*ao_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
                         sizeof(trace_pass), &trace_pass);
    CrumbBegin(cmdbuf, Rt::Crumb::Direct);
    cmdbuf.dispatch((cells_x + Rt::DirectTile - 1) / Rt::DirectTile,
                    (cells_y + Rt::DirectTile - 1) / Rt::DirectTile, 1);
    if (ao_scale > 1 && !Rt::KnobIs("BB_RT_AO_RESOLVE", '0')) {
        Barrier(cmdbuf, Stage::eComputeShader, Access::eShaderWrite, Stage::eComputeShader,
                Access::eShaderRead | Access::eShaderWrite);
        cmdbuf.pushConstants(*ao_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
                             sizeof(resolve_pass), &resolve_pass);
        cmdbuf.dispatch((depth.width + Rt::DirectTile - 1) / Rt::DirectTile,
                        (depth.height + Rt::DirectTile - 1) / Rt::DirectTile, 1);
    }
    CrumbEnd(cmdbuf, Rt::Crumb::Direct);
    Barrier(cmdbuf, Stage::eComputeShader, Access::eShaderWrite, Stage::eAllCommands | Stage::eHost,
            Access::eShaderRead | Access::eShaderWrite | Access::eColorAttachmentRead |
                Access::eColorAttachmentWrite | Access::eTransferRead | Access::eHostRead);
    TimeEnd(cmdbuf, 6u);
    ao_stats_tick[slot] = scheduler.CurrentTick();
    ao_stats_frame[slot] = frame;
    ao_history[in].written = true;
    ao_history[out].written = true;
    ao_history_index = out;
    ao_history_valid = true;
    ao_history_frame = frame;
    ao_prev_inv_view = inputs.inv_view;
    ao_prev_projection = inputs.projection;
    ao_prev_jitter = inputs.jitter;
    const bool mask_changed = ao_mask != mask_id;
    ao_mask_mismatch += ao_mask && ao_mask != mask_id ? 1u : 0u;
    ao_mask = mask_id;
    ++ao_frames;
    static u32 printed = 0;
    if (printed < 3 || mask_changed) {
        ++printed;
        Log("Ray tracing: AO frame %llu wrote image %u (%ux%u, %ux%u cells) before "
            "GXLightDeferredAO: %u rays, radius %.2f, normal %s, history %s\n",
            (unsigned long long)frame, mask_id.index, depth.width, depth.height, cells_x, cells_y,
            ao_rays, ao_radius, normal_valid ? "G-buffer" : "depth", history ? "reused" : "reset");
    }
}

void RayTracing::ReadKeptProbe() {
    if (!pick_ring.mapped || pick_ring.slot_bytes < Rt::KeptProbeBytes) {
        return;
    }
    for (u32 s = 0; s < Slots; ++s) {
        if (kept_probe_frame[s] == ~0ull || kept_probe_frame[s] == frame ||
            !scheduler.IsFree(kept_probe_tick[s])) {
            continue;
        }
        const u64 probed = kept_probe_frame[s];
        kept_probe_frame[s] = ~0ull;
        const u32 built = kept_probe_built[s];
        if (built >= Slots || kept_snap_frame[built] != probed) {
            continue; // that TLAS slot was built again since: its indices mean other instances
        }
        const u8* words = pick_ring.mapped + u64(s) * pick_ring.slot_bytes;
        u32 count = 0;
        std::memcpy(&count, words + Rt::KeptProbeBase * 4, 4);
        count = std::min(count, Rt::KeptProbeMax);
        ++kept_probe_reads;
        kept_probe_hits += count;
        const std::vector<u32>& snap = kept_snap[built];
        for (u32 k = 0; k < count; ++k) {
            u32 index = 0;
            std::memcpy(&index, words + (Rt::KeptProbeBase + 1 + k) * 4, 4);
            const u32 id = index < snap.size() ? snap[index] : ~0u;
            Rt::CacheEntry* e = id != ~0u ? cache.ById(id) : nullptr;
            if (!e || e->last_used >= probed) {
                continue; // not an instance of that frame, or drawn again since the probe
            }
            if (e->probe_hidden <= e->last_used) {
                ++kept_probe_hidden;
                if (kept_probe_log_lines < 40) {
                    ++kept_probe_log_lines;
                    const Rt::DrawTag tag = id < entry_tags.size() ? entry_tags[id] : Rt::DrawTag{};
                    Log("RT kept probe frame %llu: kept entry %u (%u triangles, ps %016llx, last "
                        "drawn %llu frames ago) lies in front of the drawn surface: not kept until "
                        "drawn again (BB_RT_KEPT_PROBE); %llu so far\n",
                        (unsigned long long)probed, id, e->triangles,
                        (unsigned long long)tag.ps, (unsigned long long)(probed - e->last_used),
                        (unsigned long long)kept_probe_hidden);
                }
            }
            e->probe_hidden = probed;
        }
    }
}

void RayTracing::ReadPick() {
    if (!pick_enabled || !pick_ring.mapped) {
        return;
    }
    for (u32 s = 0; s < Slots; ++s) {
        if (pick_frame[s] == ~0ull || pick_frame[s] == frame || !scheduler.IsFree(pick_tick[s])) {
            continue;
        }
        const u64 picked = pick_frame[s];
        pick_frame[s] = ~0ull;
        if (pick_logged_frame && picked < pick_logged_frame + pick_every) {
            continue;
        }
        const u32 built = pick_built[s];
        if (built >= Slots || pick_snap_frame[built] != picked) {
            continue;
        }
        pick_logged_frame = picked;
        std::array<u32, 96> words{};
        std::memcpy(words.data(), pick_ring.mapped + u64(s) * pick_ring.slot_bytes, sizeof(words));
        LogPick(words, pick_snap[built]);
        std::array<u32, 17> fw{};
        std::memcpy(fw.data(), pick_ring.mapped + u64(s) * pick_ring.slot_bytes + 96 * 4, sizeof(fw));
        if (fw[0] == 1u) {
            const auto g = [&](u32 i) { return std::bit_cast<float>(fw[i]); };
            Log("RT pick   fog: set %u, sky reference luma %.5f, fog colour luma %.5f (colour / "
                "reference %.2f), sun/moon colour luma %.4f, cos(view, sun) %.3f, phase x 4pi %.3f, "
                "transmittance to the surface %.4f (surface %.1f m, extinction %.5f/m from %.1f m), "
                "march integral %.5f; parts: sun %.5f, sky %.5f, lamps %.5f (sky sum %.5f); albedo "
                "reference %.5f (colour / it %.2f)\n",
                fw[15], g(1), g(2), g(2) / std::max(g(1), 1e-6f), g(3), g(4), g(5), g(6), g(12), g(13),
                g(14), g(7), g(8), g(9), g(10), g(11), g(16), g(2) / std::max(g(16), 1e-6f));
        }
    }
}

void RayTracing::LogPick(const std::array<u32, 96>& w, std::span<const PickInstance> snap) {
    const auto f = [&](u32 i) {
        float v;
        std::memcpy(&v, &w[i], sizeof(v));
        return v;
    };
    if (!w[0]) {
        Log("RT pick %.3f,%.3f: the direct pass found no surface at that pixel this frame (sky, or "
            "the pass did not run)\n",
            pick_at[0], pick_at[1]);
        return;
    }
    Log("RT pick %.3f,%.3f = pixel %u,%u, frame %llu: surface %.2f m from the camera, view normal "
        "(%.2f %.2f %.2f); %u of %u lights touch this tile, %u shadow rays probed, %zu TLAS "
        "instances, %llu alpha-tested instances without a usable texture/UVs this frame (%s); "
        "G-buffer1 spec %.3f wrap %.3f, light set %.0f, AO mask %.3f, directional set diffuse "
        "luma %.4f specular %.4f\n",
        pick_at[0], pick_at[1], w[1] & 0xFFFFu, w[1] >> 16, (unsigned long long)pick_logged_frame,
        std::fabs(f(6)), f(7), f(8), f(9), w[2], w[3], w[10], snap.size(),
        (unsigned long long)alpha_unresolved_last,
        alpha_skip_unresolved ? "left out, BB_RT_ALPHA_UNRESOLVED=skip" : "traced as solid cards",
        f(12), f(13), f(14) * 255.0f, f(15), f(16 + 2 * 24), f(16 + 2 * 24 + 1));
    if (w[88] == 1u) {
        Log("RT pick   moon: set %u, view dir to the moon (%.3f %.3f %.3f), colour luma %.4f, "
            "n.L %.3f, moon visibility %.2f (-1 = faces away, not traced), diffuse factor (1 - spec "
            "colour^2) %.3f\n",
            w[87], f(80), f(81), f(82), f(84), f(83), f(85), f(86));
        if (w[90] == 1u) {
            const u32 iid = w[91];
            const PickInstance* pi = iid < snap.size() ? &snap[iid] : nullptr;
            Log("RT pick   moon centre ray blocker: TLAS instance %u, BLAS entry %u, triangle %u, %s "
                "face, %.2f m along the ray; draw %s ps %016llx vs %016llx, %u triangles, mode %u, "
                "TLAS mask 0x%02x, alpha-tested %s, blended %s\n",
                iid, w[93], w[94], w[95] ? "front" : "back", f(92), pi ? "known" : "unknown",
                pi ? (unsigned long long)pi->tag.ps : 0ull, pi ? (unsigned long long)pi->tag.vs : 0ull,
                pi ? pi->triangles : 0u, pi ? pi->mode : 0u, pi ? pi->mask : 0u,
                pi && pi->alpha_test ? "yes" : "no", pi && pi->blended ? "yes" : "no");
        } else if (f(83) > 0.0f) {
            Log("RT pick   moon centre ray: no blocker (open sky)\n");
        }
    }
    const char* modes[] = {"BLAS built this frame", "cached BLAS drawn this frame",
                           "kept off-screen instance (not drawn this frame)", "hidden (no camera)"};
    for (u32 k = 0; k < std::min<u32>(w[10], 2); ++k) {
        const u32 b = 16 + k * 24;
        const float vis = f(b + 2);
        const float ray_len = f(b + 8);
        Log("RT pick   light %u: #%u %s at view (%.2f %.2f %.2f), %.2f m from the surface, radius "
            "%.2f m, %.0f%% of this pixel's point/spot light, n.L %.2f, diffuse luma %.4f; "
            "visibility %.2f => %s\n",
            k + 1, w[b], f(b + 20) < 0.5f ? "point" : "spot", f(b + 16), f(b + 17), f(b + 18),
            ray_len, f(b + 19), f(b + 1) * 100.0f, f(b + 23), f(b + 22), vis,
            vis >= 0.99f   ? "LIT (this light is not blocked here)"
            : vis <= 0.01f ? "BLOCKED"
                           : "PARTLY BLOCKED (smoke or alpha)");
        if (!w[b + 3]) {
            if (vis < 0.99f) {
                Log("RT pick     no solid blocker on the closest-hit ray: the loss comes from "
                    "blended smoke cards (%u candidates skipped)\n",
                    w[b + 14]);
            }
            continue;
        }
        const u32 iid = w[b + 5];
        const PickInstance* pi = iid < snap.size() ? &snap[iid] : nullptr;
        const float t = f(b + 7);
        Log("RT pick     blocker: TLAS instance %u, BLAS entry %u, triangle %u, %s face, %.2f m "
            "along the ray (%.0f%% of the way to the light); %u alpha/smoke candidates passed "
            "through before it\n",
            iid, w[b + 4], w[b + 6], w[b + 15] ? "front" : "back", t,
            ray_len > 0.0f ? 100.0f * t / ray_len : 0.0f, w[b + 14]);
        bool unresolved = false;
        if (pi) {
            const auto& g = pi->tag;
            unresolved = !pi->blended &&
                         Rt::AlphaUnresolved(pi->alpha_test, pi->texture || pi->alpha_texture, pi->uv);
            Log("RT pick     blocker draw: %s, RT draw #%u this frame, ps %016llx vs %016llx, "
                "%u triangles, prim %u; alpha %s, alpha-tested %s, albedo slot %u, mask slot %u, "
                "UVs %s%s; TLAS mask 0x%02x, %s%s; colour exports 0x%x, bound targets 0x%x "
                "(write mask 0x%08x), cb0 %010llx, depth %010llx, z test %u func %u write %u, "
                "stencil %u\n",
                pi->mode < 4 ? modes[pi->mode] : "?", g.draw, (unsigned long long)g.ps,
                (unsigned long long)g.vs, pi->triangles, g.prim,
                Rt::CasterAlphaName(Rt::CasterAlpha(g.caster)), pi->alpha_test ? "yes" : "no",
                pi->texture, pi->alpha_texture, pi->uv ? "yes" : "no",
                unresolved ? " => UNRESOLVED: traced as a solid card" : "", pi->mask,
                (pi->flags & VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR) ? "forced opaque"
                : pi->omm                                              ? "OMM"
                                                                       : "any-hit alpha test",
                pi->blended ? ", blended smoke" : "", g.mrt_mask, g.bound_mask, g.target_mask,
                (unsigned long long)g.cb0, (unsigned long long)g.zbuf, g.ztest, g.zfunc, g.zwrite,
                g.stencil);
        } else {
            Log("RT pick     blocker draw: unknown (TLAS instance %u not in the %zu-entry snapshot)\n",
                iid, snap.size());
        }
        const Rt::PickRaster raster =
            Rt::PickRasterCode(w[b + 9] != 0, w[b + 10] != 0, f(b + 11), f(b + 12));
        const char* raster_text[] = {
            "OFFSCREEN: the blocker point is outside the frame, no raster check",
            "BEHIND: nearer raster geometry covers that point, inconclusive",
            "VISIBLE: raster shows a surface at the blocker point, it is real visible geometry",
            "GHOST: raster shows nothing there (sees through it), the blocker exists only in the RT "
            "scene"};
        Log("RT pick     raster check: blocker at %.2f m projects to pixel %u,%u, raster depth there "
            "%s%.2f m => %s\n",
            std::fabs(f(b + 12)), w[b + 13] & 0xFFFFu, w[b + 13] >> 16, w[b + 10] ? "" : "sky/none ",
            std::fabs(f(b + 11)), raster_text[u32(raster)]);
        std::string hint;
        if (unresolved) {
            hint += " alpha-tested draw without its texture is traced as a solid card: A/B "
                    "BB_RT_ALPHA_UNRESOLVED=skip;";
        }
        if (pi && pi->mode == 2) {
            hint += " the blocker is a kept off-screen instance: A/B BB_RT_OFF_INSTANCES=0;";
        }
        if (pi && (raster == Rt::PickRaster::Ghost || raster == Rt::PickRaster::Behind)) {
            char text[160];
            std::snprintf(text, sizeof(text),
                          " exclude this draw class: A/B BB_RT_EXCLUDE=%llx (ps) or "
                          "BB_RT_EXCLUDE=%llx (vs);",
                          (unsigned long long)pi->tag.ps, (unsigned long long)pi->tag.vs);
            hint += text;
        }
        if (raster == Rt::PickRaster::Visible && !unresolved) {
            hint += " a real visible surface blocks this light: the shadow is geometrically right "
                    "for this light position, so check the light position/radius instead;";
        }
        Log("RT pick     verdict:%s\n", hint.empty() ? " no hint" : hint.c_str());
    }
    LogPickCamera(w, snap);
}

// The camera ray through the pick pixel (rt_direct.comp PickCamera, words 66..79): what it hits
// and whether the glass confirm ray of the reflection pass (rt_gi.comp GlassFind) accepts it.
void RayTracing::LogPickCamera(const std::array<u32, 96>& w, std::span<const PickInstance> snap) {
    const auto f = [&](u32 i) {
        float v;
        std::memcpy(&v, &w[i], sizeof(v));
        return v;
    };
    const u32 head = w[66];
    if (!(head & (1u << 27))) {
        return;
    }
    const auto draw = [&](u32 iid) {
        char text[384];
        const PickInstance* pi = iid < snap.size() ? &snap[iid] : nullptr;
        if (!pi) {
            std::snprintf(text, sizeof(text), "draw unknown (not in the %zu-entry snapshot)",
                          snap.size());
        } else {
            const Rt::CacheEntry* ce = cache.ById(pi->entry_id);
            std::snprintf(text, sizeof(text),
                          "BLAS entry %u (%u triangles), ps %016llx vs %016llx, alpha %s, alpha-tested %s%s%s, "
                          "TLAS mask 0x%02x (%s), glass ps %s",
                          pi->entry_id, pi->triangles, (unsigned long long)pi->tag.ps,
                          (unsigned long long)pi->tag.vs,
                          Rt::CasterAlphaName(Rt::CasterAlpha(pi->tag.caster)),
                          pi->alpha_test ? "yes" : "no", pi->blended ? ", blended" : "",
                          !ce ? ", cache entry gone" : ce->skinned ? ", skinned" : ", not skinned",
                          pi->mask,
                          (pi->mask & Rt::MaskPortal) ? "portal pane"
                          : (pi->mask & 0x01u)        ? "solid"
                                                      : "not in the solid mask",
                          Rt::HashListed(glass_ps, pi->tag.ps, 0) ? "listed" : "not listed");
        }
        return std::string(text);
    };
    const float dist = std::sqrt(f(4) * f(4) + f(5) * f(5) + f(6) * f(6));
    const float gloss = f(12);
    const bool gloss_ok = gloss > glass_gloss;
    const u32 hits = (head >> 24) & 7u;
    Log("RT pick   camera ray: G-buffer surface %.3f m along the ray, gloss %.3f (%s BB_RT_GLASS_GLOSS "
        "%.2f), %u hits walked (any TLAS mask, no alpha test, 1 mm skip)\n",
        dist, gloss, gloss_ok ? ">" : "<=", glass_gloss, hits);
    for (u32 k = 0; k < hits; ++k) {
        const u32 b = 68 + k * 3;
        const float t = f(b);
        const u32 iid = w[b + 1] & 0xFFFFFFu;
        const u32 bits = w[b + 1] >> 24;
        Log("RT pick     cam hit %u: t %.3f m (%+.3f m vs G-buffer), TLAS instance %u, triangle %u, "
            "%s face, glass bit %s, alpha test %s%s; %s\n",
            k + 1, t, t - dist, iid, w[b + 2], (bits & 1u) ? "front" : "back",
            (bits & 2u) ? "SET" : "clear", (bits & 4u) ? "passes" : "FAILS",
            (bits & 8u) ? ", blended smoke record" : "", draw(iid).c_str());
    }
    const float window = 0.01f * dist + 0.03f; // rt_gi.comp GlassDepthWindow, GlassDepthSlack
    const bool on = glass_on && !glass_ps.empty();
    const bool hit = (head & (1u << 28)) != 0;
    const bool glass_bit = (head & (1u << 30)) != 0;
    std::string reason;
    if (!on) {
        reason += " BB_RT_GLASS off or BB_RT_GLASS_PS empty;";
    }
    if (!gloss_ok) {
        reason += " gloss too low;";
    }
    if (!hit) {
        reason += " confirm ray missed (nothing solid and alpha-passing in the window);";
    } else if (!glass_bit) {
        reason += " confirm hit has no glass bit (its ps is not in BB_RT_GLASS_PS);";
    }
    if (hit) {
        const u32 iid = head & 0xFFFFFFu;
        Log("RT pick   glass confirm ray (t %.3f..%.3f m, solid mask, alpha-tested): t %.3f m "
            "(%+.3f m vs G-buffer), TLAS instance %u, %s face, glass bit %s; %s => %s%s\n",
            std::max(dist - window, 0.0f), dist + window, f(67), f(67) - dist, iid,
            (head & (1u << 29)) ? "front" : "back", glass_bit ? "SET" : "clear", draw(iid).c_str(),
            reason.empty() ? "ACCEPTED as glass" : "rejected:", reason.c_str());
    } else {
        Log("RT pick   glass confirm ray (t %.3f..%.3f m, solid mask, alpha-tested): miss => "
            "rejected:%s\n",
            std::max(dist - window, 0.0f), dist + window, reason.c_str());
    }
}

void RayTracing::ReadAoStats() {
    if (!ao_stats_ring.mapped) {
        return;
    }
    for (u32 s = 0; s < Slots; ++s) {
        if (ao_stats_frame[s] == ~0ull || ao_stats_frame[s] == frame || !scheduler.IsFree(ao_stats_tick[s])) {
            continue;
        }
        ao_stats_frame[s] = ~0ull;
        std::array<u32, 3> sums{};
        std::memcpy(sums.data(), ao_stats_ring.mapped + u64(s) * ao_stats_ring.slot_bytes, sizeof(sums));
        if (sums[2] == 0) {
            continue;
        }
        const Rt::AoStatsMean mean = Rt::AoStatsReadback(sums);
        ao_stats_stock += mean.stock;
        ao_stats_ours += mean.ours;
        ++ao_stats_frames;
    }
}

void RayTracing::OnLightComposite(const SceneInputs& inputs, u64 ps_hash) {
    hook_ps = ps_hash;
    if (!enabled || !(direct_enabled || reflect_enabled) || acc_frame != frame) {
        if (DirectTrace()) {
            Trace("%s hook without light targets this frame (acc frame %llu)", HookName(),
                  (unsigned long long)acc_frame);
        }
        return;
    }
    ++frame_direct_calls;
    if (DirectTrace()) {
        Trace("%s hook: %u light-VS draws so far, %u skipped, call %u this frame, depth "
              "%s, camera %s",
              HookName(), frame_light_draws, frame_light_draws_skipped, frame_direct_calls,
              inputs.depth ? "set" : "none", inputs.camera_valid ? "valid" : "invalid");
        TraceImage("acc diffuse", acc_diffuse);
        TraceImage("acc specular", acc_specular);
        TraceImage("scene depth", inputs.depth);
    }
    acc_frame = ~0ull;
    SetLightKeepCamera(inputs);
    if (light_capture && light_frame == frame) {
        EndLightFrame();
    }
    if (Rt::effects_paused.load(std::memory_order_relaxed) && !light_draws_skipped_this_frame) {
        return;
    }
    CpuTimer timer{hitch_log, hitch.gi};
    direct_mode = Rt::DirectWriteMode(direct_debug, light_draws_skipped_this_frame);
    // Reflections go first only when the direct pass adds onto the targets; without
    // BB_RT_DIRECT_BASE it overwrites them (no DirectFlagAdd) and would erase the reflection.
    const bool reflect_first = reflect_enabled && direct_enabled &&
                               direct_mode == Rt::DirectWrite::Add && direct_base;
    if (reflect_first) {
        RecordReflect(inputs);
    }
    if (direct_enabled) {
        RecordDirect(inputs);
    }
    if (reflect_enabled && !reflect_first) {
        RecordReflect(inputs);
    }
}

bool RayTracing::SkipLightDraw(VideoCore::ImageId diffuse, VideoCore::ImageId specular,
                               bool composite, bool blended, bool writes_depth,
                               bool writes_stencil) {
    const auto id = [](VideoCore::ImageId i) { return i ? i.index + 1u : 0u; };
    const bool skip = Rt::SkipLightDraw({
        .direct_enabled = enabled && direct_enabled && direct_debug != 3,
        .paused = Rt::effects_paused.load(std::memory_order_relaxed),
        .composite = composite,
        .blended = blended,
        .writes_depth = writes_depth,
        .writes_stencil = writes_stencil,
        .frame = frame,
        .direct_frame = direct_written_frame,
        .diffuse = id(diffuse),
        .specular = id(specular),
        .direct_diffuse = id(direct_written_diffuse),
        .direct_specular = id(direct_written_specular),
    });
    if (skip) {
        ++light_draws_skipped;
        ++frame_light_draws_skipped;
        light_draws_skipped_this_frame = true;
    }
    return skip;
}

void RayTracing::RecordDirect(const SceneInputs& inputs) {
    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;
    const auto skip = [&](const char* why) {
        ++direct_skipped;
        if (DirectTrace()) {
            Trace("direct skipped (%s)", why);
        }
        static const char* last = nullptr;
        static u32 printed = 0;
        if (why != last && printed < 32) {
            last = why;
            ++printed;
            Log("Ray tracing: direct skipped at frame %llu (%s)\n", (unsigned long long)frame, why);
        }
    };
    const bool camera = inputs.camera_valid && inputs.projection[0] != 0.0f &&
                        inputs.projection[1] != 0.0f && inputs.projection[3] != 0.0f;
    if (!acc_diffuse || !acc_specular || !inputs.depth || !camera) {
        skip("no light accumulation targets, depth or camera this frame");
        return;
    }
    if (direct_debug == 4 && trace_until == ~0ull) {
        trace_until = frame + 1;
        Trace("trace starts: first %s hook with targets, depth and a valid camera", HookName());
    }
    if (instances.empty()) {
        skip("no G-buffer geometry this frame");
        return;
    }
    {
        const auto& depth = texture_cache.GetImage(inputs.depth);
        for (VideoCore::ImageId id : {acc_diffuse, acc_specular}) {
            const auto& acc = texture_cache.GetImage(id);
            if (acc.info.pixel_format != vk::Format::eB10G11R11UfloatPack32 ||
                !(acc.usage_flags & vk::ImageUsageFlagBits::eStorage) ||
                acc.info.size.width != depth.info.size.width ||
                acc.info.size.height != depth.info.size.height) {
                skip("light accumulation target is not a storage B10G11R11 image of the depth size");
                return;
            }
        }
    }
    if (!direct_novis && !Build(inputs)) {
        skip("acceleration structure not built");
        return;
    }
    if (auto* profiler = GpuProfiler::Get()) {
        if (restir_enabled) {
            profiler->Mark(0xB1A5'0008ull, [] { return std::string{"ray tracing restir"}; });
        } else {
            profiler->Mark(0xB1A5'0006ull, [] { return std::string{"ray tracing direct"}; });
        }
    }
    scheduler.EndRendering();
    Bound diffuse{}, specular{}, depth{}, normal{}, g3{}, g1{};
    if (!BindScene(acc_diffuse, inputs, Access::eShaderRead | Access::eShaderWrite, diffuse) ||
        !BindScene(acc_specular, inputs, Access::eShaderRead | Access::eShaderWrite, specular) ||
        !BindScene(inputs.depth, inputs, Access::eShaderRead, depth) ||
        diffuse.width != depth.width || diffuse.height != depth.height ||
        specular.width != depth.width || specular.height != depth.height) {
        runtime.FlushBarriers();
        skip("accumulation targets or depth not bindable at one size");
        return;
    }
    const auto bind_target = [&](int index, vk::Format format, Bound& out) {
        if (index < 0 || u32(index) >= gbuffer_count || !gbuffer_targets[index]) {
            return false;
        }
        if (format != vk::Format::eUndefined &&
            texture_cache.GetImage(gbuffer_targets[index]).info.pixel_format != format) {
            return false;
        }
        Bound bound{};
        if (!BindScene(gbuffer_targets[index], inputs, Access::eShaderRead, bound) ||
            bound.width != depth.width || bound.height != depth.height) {
            return false;
        }
        out = bound;
        return true;
    };
    const bool normal_valid = normal_source.target != 0xFF &&
                              bind_target(int(normal_source.target), vk::Format::eUndefined, normal);
    const bool g3_valid = bind_target(shadow_spec_target, vk::Format::eR8G8B8A8Unorm, g3);
    const bool g1_valid = bind_target(direct_g1_target, vk::Format::eR8G8B8A8Unorm, g1);
    Bound mask{};
    bool mask_valid = false;
    if (dir_shadow_frame == frame) {
        const VideoCore::ImageId mask_id = Rt::PickShadowMask<VideoCore::ImageId>(
            {dir_shadow_inputs.data(), dir_shadow_input_count},
            {gbuffer_targets.data(), gbuffer_count}, inputs.depth);
        mask_valid = BindMask(mask_id, inputs, depth, mask);
    }
    runtime.FlushBarriers();
    direct_depth_normal_frames += normal_valid ? 0u : 1u;

    const Rt::Affine view = Rt::InverseRigid(inputs.inv_view);
    const u32 slot = u32(frame % Slots);
    const u32 light_count = u32(std::min<size_t>(direct_lights.size(), Rt::MaxDirectLights));
    auto* gpu_lights = reinterpret_cast<Rt::DirectLightGpu*>(direct_lights_ring.mapped +
                                                              u64(slot) * direct_lights_ring.slot_bytes);
    depth_packed_last = 0;
    for (u32 i = 0; i < light_count; ++i) {
        gpu_lights[i] = Rt::PackDirectLight(direct_lights[i], view);
        if (light_depth && Rt::PackStockDepth(gpu_lights[i], direct_lights[i])) {
            ++depth_packed_last;
        }
    }
    depth_packed_frames += depth_packed_last > 0 ? 1 : 0;
    if (direct_light_log > 0.0f && frame >= direct_light_log_frame + 60) {
        direct_light_log_frame = frame;
        u32 listed = 0;
        for (u32 i = 0; i < light_count && listed < 32; ++i) {
            const Rt::Light& l = direct_lights[i];
            const auto& p = gpu_lights[i].pos_inv_r2;
            const float dist = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
            if (!(dist - l.radius < direct_light_log)) {
                continue;
            }
            ++listed;
            const auto c = Rt::DirectDebugColor(i);
            Log("RT light frame %llu #%u %s (%s%s%s), view (%.2f %.2f %.2f), %.2f m from the camera, "
                "radius %.2f, colour (%.3f %.3f %.3f), spec (%.3f %.3f %.3f), wrap (%.2f %.2f), age %u, "
                "stencil %s func %u ref %02x mask %02x, depth %u func %u write %u quad z %.6f (vtx %s "
                "%.6f) bounds %u [%.6f %.6f] cull %u, stock depth test %s, "
                "debug 8 colour (%.2f %.2f %.2f)%s%s%s\n",
                (unsigned long long)frame, i, l.shader_name ? l.shader_name : "?",
                l.kind == Rt::LightKind::Spot ? "spot" : "point", l.legacy ? " legacy" : "",
                l.shadowed ? " shadowed" : "", p[0], p[1], p[2], dist, l.radius, l.color[0],
                l.color[1], l.color[2], l.spec[0], l.spec[1], l.spec[2], l.wrap[0], l.wrap[1],
                l.age, (l.stencil & Rt::LightStencilEnable) ? "on" : "off", (l.stencil >> 16) & 7u,
                l.stencil & 0xFFu, (l.stencil >> 8) & 0xFFu, u32(l.state.depth), l.state.zfunc,
                u32(l.state.depth_write), Rt::LightDepthTest(l.state).quad,
                l.state.has_vtx ? "on" : "off", l.state.vtx_z, u32(l.state.bounds),
                l.state.bounds_min, l.state.bounds_max, l.state.cull,
                gpu_lights[i].cone[5] > 0.5f && gpu_lights[i].spec_kind[3] == 0.0f ? "applied"
                                                                                  : "not applied",
                c[0], c[1], c[2],
                l.camera_fixed ? ", player fill light (camera-fixed)" : "", l.gate ? ", gate: " : "",
                l.gate ? l.gate : "");
        }
        Log("RT light frame %llu: %u of %u direct lights within %.1f m of the camera, stock depth "
            "test on %u (BB_RT_LIGHT_DEPTH %u)\n",
            (unsigned long long)frame, listed, light_count, direct_light_log, depth_packed_last,
            u32(light_depth));
    }
    vk::DescriptorBufferInfo sets_info{direct_sets_ring.buffer,
                                       u64(slot) * direct_sets_ring.slot_bytes,
                                       direct_sets_ring.slot_bytes};
    bool sets_gpu = false;
    GpuSets gpu_sets{};
    if (g3_valid) {
        gpu_sets = ObtainGpuLightSets(0);
        if (gpu_sets.buffer) {
            sets_info = {gpu_sets.buffer, gpu_sets.offset, gpu_sets.bytes};
            direct_set_count = gpu_sets.bytes / Rt::DirLightSetStride;
            sets_gpu = true;
            ++direct_sets_gpu_frames;
        }
    }
    const bool sets_valid =
        g3_valid && direct_set_count > 0 &&
        (sets_gpu || direct_sets.size() * 4 >= size_t(direct_set_count) * Rt::DirLightSetStride);
    if (sets_valid) {
        fog_sets_info = sets_info;
        fog_set_count = direct_set_count;
        fog_sets_frame = frame;
    }
    if (sets_valid && !sets_gpu) {
        std::memcpy(direct_sets_ring.mapped + u64(slot) * direct_sets_ring.slot_bytes,
                    direct_sets.data(), size_t(direct_set_count) * Rt::DirLightSetStride);
    }
    if (sets_valid) {
        ++direct_sun_frames;
    }
    bool restir_reset = false;
    u32 prev_light_count = 0;
    if (restir_enabled) {
        if (reservoir_width != depth.width || reservoir_height != depth.height) {
            if (!CreateReservoirs(depth.width, depth.height)) {
                skip("ReSTIR reservoir allocation failed");
                return;
            }
            restir_last_frame = ~0ull;
        }
        restir_reset = restir_last_frame + 1 != frame ||
                       Rt::CameraCut(restir_prev_inv_view, inputs.inv_view, 3.0f, 0.9f);
        if (restir_reset) {
            ++restir_resets;
        } else {
            Rt::RemapLightIds(restir_prev_keys, direct_keys, restir_remap);
            prev_light_count = u32(std::min<size_t>(restir_remap.size(), Rt::MaxDirectLights));
            std::memcpy(restir_remap_ring.mapped + u64(slot) * restir_remap_ring.slot_bytes,
                        restir_remap.data(), size_t(prev_light_count) * sizeof(u32));
        }
    }
    // Moon visibility history (plain direct pass only; the ReSTIR passes own the reprojection
    // params). The buffers are always bound there; the flag turns the shader step on.
    bool moon_hist_on = false, moon_hist_valid = false;
    if (!restir_enabled) {
        if ((moon_hist_width != depth.width || moon_hist_height != depth.height) &&
            !CreateMoonHistory(depth.width, depth.height)) {
            skip("moon history allocation failed");
            return;
        }
        moon_hist_on = moon_history && moon_half != 0 && !(mask_valid && moon_mask);
        moon_hist_valid = moon_hist_on && moon_hist_frame + 1 == frame &&
                          !Rt::CameraCut(moon_prev_inv_view, inputs.inv_view, 3.0f, 0.9f);
        if (moon_hist_on && !moon_hist_valid) {
            ++moon_hist_resets;
        }
    }
    // The game's object motion (written by the G-buffer draws, complete here): the moon history
    // follows moving objects with it instead of the camera path alone.
    vk::ImageView object_view{};
    bool object_valid = false;
    if (moon_hist_valid && moon_object_motion && object_motion && object_motion->Enabled()) {
        scheduler.EndRendering();
        object_view = object_motion->PrepareRead(scheduler.CommandBuffer(), depth.width,
                                                 depth.height, object_valid);
        moon_object_frames += object_valid ? 1u : 0u;
    }
    const Rt::Affine& prev_inv_view = restir_enabled ? restir_prev_inv_view : moon_prev_inv_view;
    const bool prev_reset = restir_enabled ? restir_reset : !moon_hist_valid;
    const std::array<float, 4>& prev_projection =
        restir_enabled ? restir_prev_projection : moon_prev_projection;
    const std::array<float, 2>& prev_jitter = restir_enabled ? restir_prev_jitter : moon_prev_jitter;
    std::array<float, 2> pick_px{-1.0f, -1.0f};
    if (pick_enabled && pick_ring.mapped && !restir_enabled) {
        ReadPick();
        pick_px = {std::floor(pick_at[0] * float(depth.width - 1) + 0.5f),
                   std::floor(pick_at[1] * float(depth.height - 1) + 0.5f)};
        std::memset(pick_ring.mapped + u64(slot) * pick_ring.slot_bytes, 0, pick_ring.slot_bytes);
    }
    const bool kept_probe_on = kept_probe && pick_ring.mapped && !restir_enabled && !ptlas_on &&
                               pick_ring.slot_bytes >= Rt::KeptProbeBytes &&
                               built_slot < Slots && kept_snap_frame[built_slot] == frame;
    if (kept_probe_on) {
        ReadKeptProbe();
        std::memset(pick_ring.mapped + u64(slot) * pick_ring.slot_bytes + Rt::KeptProbeBase * 4, 0,
                    4);
    }
    fog_pick_px = {pick_px[0], pick_px[1], pick_px[0] >= 0.0f ? 1.0f : 0.0f, 0.0f};
    const bool water_keep_valid = water_direct_keep && glass_keep.view && glass_keep_frame == frame &&
                                  direct_mode == Rt::DirectWrite::Add && direct_base;
    const Rt::DirectParams params{
        .proj = inputs.projection,
        .view_rows = Rt::Rows(view),
        .size_jitter = {float(depth.width), float(depth.height), inputs.jitter[0], inputs.jitter[1]},
        .tlas_count_flags = {u32(tlas_address), u32(tlas_address >> 32), light_count,
                             (direct_novis ? Rt::DirectFlagNoVis : 0u) |
                                 (g3_valid ? Rt::DirectFlagSpec : 0u) |
                                 (g1_valid ? Rt::DirectFlagG1 : 0u) |
                                 (sets_valid ? Rt::DirectFlagDirSets : 0u) |
                                 (restir_reset ? Rt::DirectFlagRestirReset : 0u) |
                                 (direct_mode == Rt::DirectWrite::Add && direct_base
                                      ? Rt::DirectFlagAdd
                                      : 0u) |
                                 (mask_valid ? Rt::DirectFlagShadowMask : 0u) |
                                 (mask_valid && moon_mask ? Rt::DirectFlagMoonMask : 0u) |
                                 (moon_static ? Rt::DirectFlagMoonStatic : 0u) |
                                 (moon_world ? 0u : Rt::DirectFlagMoonOwnSet) |
                                 (portal_instances ? Rt::DirectFlagPortals : 0u) |
                                 (water_keep_valid ? Rt::DirectFlagWaterKeep : 0u) |
                                 (moon_hist_on ? Rt::DirectFlagMoonHistory : 0u) |
                                 (moon_hist_valid ? Rt::DirectFlagMoonHistoryValid : 0u) |
                                 (object_valid ? Rt::DirectFlagObjectMotion : 0u) |
                                 (kept_probe_on ? Rt::DirectFlagKeptProbe : 0u) |
                                 Rt::MoonHalfFlags(moon_half, mask_valid && moon_mask, direct_debug)},
        .cfg = {(normal_valid ? normal_source.mode : Rt::NormalFromDepth) |
                    normal_source.swizzle << 8 | normal_source.components << 16,
                u32(frame), direct_rays, direct_debug},
        .bias_tmin_gap_sets = {shadow_bias, shadow_tmin, shadow_gap, float(sets_valid ? direct_set_count : 0)},
        .prev_proj = prev_projection,
        .reproject = Rt::Rows(prev_reset ? view : Rt::Reprojection(prev_inv_view, inputs.inv_view)),
        .prev_jitter = {prev_jitter[0], prev_jitter[1], water_keep_valid ? water_scatter : 0.0f, thin_transmit},
        .restir = {restir_candidates, restir_history, restir_spatial, prev_light_count},
        .shadow_color = {shadow_color[0], shadow_color[1], shadow_color[2], moon_cone_cos},
        .fx = {fx_smoke_opacity, float(fx_debug), pick_px[0], pick_px[1]},
        .far_fade = Rt::FarFadeParams(far_start, far_band),
        .ambient_cut = {ambient_cool[0], ambient_cool[1], ambient_cool[2], ambient_cut},
        .ambient_spec_cut = {spec_cut_frame == frame ? 0.0f : ambient_spec_cut, ambient_keep_lo,
                             ambient_keep_hi, ambient_keep_lit},
    };
    std::memcpy(direct_params_ring.mapped + u64(slot) * direct_params_ring.slot_bytes, &params,
                sizeof(params));
    direct_mask_frames += mask_valid ? 1u : 0u;
    if (dir_log) {
        LogDirFrame((sets_valid ? 1u : 0u) | (sets_gpu ? 2u : 0u) | (mask_valid ? 4u : 0u) |
                    (direct_mode == Rt::DirectWrite::Add ? 8u : 0u) |
                    (direct_mode == Rt::DirectWrite::PassThrough ? 16u : 0u));
    }
    static u64 printed = 0;
    if (printed < 3) {
        ++printed;
        Log("Ray tracing: direct frame %llu, %u lights (%u duplicates so far), directional sets %u "
            "(%s), normal %s, GBuffer3 %s, GBuffer1 %s, moon shadow %s (FC_ShadowColor %.3f %.3f "
            "%.3f), targets %ux%u, %s; off-screen switches "
            "lights %u cone %u instances %u evict %u texresolve %u holds %u\n",
            (unsigned long long)frame, light_count, u32(direct_duplicates),
            direct_set_count,
            sets_valid ? (sets_gpu ? (dir_sets_mode != DirSetsMode::Gpu
                                          ? "copied from the game's GPU buffer at its draw"
                                          : "bound from the game's GPU buffer")
                                   : "bound from the host copy")
                       : "none: sun kept from the game",
            normal_valid ? "G-buffer" : "depth", g3_valid ? "bound" : "none",
            g1_valid ? "bound" : "none",
            mask_valid && moon_mask ? "game mask (BB_RT_MOON=mask)"
            : mask_valid              ? "traced, SSAO from the game mask"
            : moon_mask               ? "traced (no mask this frame)"
                                      : "traced, no SSAO (no mask this frame)",
            shadow_color[0], shadow_color[1], shadow_color[2], depth.width, depth.height,
            direct_mode == Rt::DirectWrite::Add
                ? "added to the game's ambient/AO base (light draws skipped)"
                : (direct_mode == Rt::DirectWrite::PassThrough
                       ? "game light draws ran: kept this frame, skipped from the next"
                       : "overwritten (debug)"),
            unsigned(off_switches.lights), unsigned(off_switches.cone),
            unsigned(off_switches.instances), unsigned(off_switches.evict),
            unsigned(off_switches.texresolve), unsigned(off_switches.holds));
    }
    if (direct_mode == Rt::DirectWrite::PassThrough) {
        if (DirectTrace()) {
            Trace("direct pass-through: nothing written this frame");
        }
        ++direct_pass_through;
        direct_written_frame = frame;
        direct_written_diffuse = acc_diffuse;
        direct_written_specular = acc_specular;
        return;
    }

    const auto cmdbuf = scheduler.CommandBuffer();
    TimeBegin(cmdbuf, restir_enabled ? 3u : 2u);
    const auto image = [](vk::ImageView view) {
        return vk::DescriptorImageInfo{.imageView = view, .imageLayout = vk::ImageLayout::eGeneral};
    };
    const vk::DescriptorImageInfo depth_info = image(depth.view);
    const vk::DescriptorImageInfo diffuse_info = image(diffuse.view);
    const vk::DescriptorImageInfo specular_info = image(specular.view);
    const vk::DescriptorImageInfo normal_info = image(normal_valid ? normal.view : depth.view);
    const vk::DescriptorImageInfo g3_info = image(g3_valid ? g3.view : depth.view);
    const vk::DescriptorImageInfo g1_info = image(g1_valid ? g1.view : depth.view);
    const vk::DescriptorImageInfo mask_info = image(mask_valid ? mask.view : depth.view);
    const vk::DescriptorBufferInfo params_info{direct_params_ring.buffer,
                                               u64(slot) * direct_params_ring.slot_bytes, sizeof(params)};
    const vk::DescriptorBufferInfo lights_info{direct_lights_ring.buffer,
                                               u64(slot) * direct_lights_ring.slot_bytes,
                                               direct_lights_ring.slot_bytes};
    const vk::DescriptorBufferInfo surfaces_info{surfaces_ring.buffer,
                                                 u64(built_slot) * surfaces_ring.slot_bytes,
                                                 surfaces_ring.slot_bytes};
    const vk::DescriptorBufferInfo uv_pool_info{uv_buffer.buffer ? uv_buffer.buffer : soup.buffer, 0,
                                                uv_buffer.buffer ? uv_buffer.size : soup.size};
    const auto write = [](u32 binding, vk::DescriptorType type, const vk::DescriptorImageInfo* image,
                          const vk::DescriptorBufferInfo* buffer) {
        return vk::WriteDescriptorSet{.dstBinding = binding,
                                      .descriptorCount = 1,
                                      .descriptorType = type,
                                      .pImageInfo = image,
                                      .pBufferInfo = buffer};
    };
    using Type = vk::DescriptorType;
    const vk::DescriptorBufferInfo pick_info{pick_ring.buffer, u64(slot) * pick_ring.slot_bytes,
                                             pick_ring.slot_bytes};
    const vk::DescriptorImageInfo water_keep_info = image(water_keep_valid ? *glass_keep.view : depth.view);
    const DeviceBuffer& hist_in = moon_hist[moon_hist_index];
    const DeviceBuffer& hist_out = moon_hist[moon_hist_index ^ 1u];
    const vk::DescriptorBufferInfo hist_in_info{hist_in.buffer, 0, hist_in.size};
    const vk::DescriptorBufferInfo hist_out_info{hist_out.buffer, 0, hist_out.size};
    const vk::DescriptorImageInfo object_info = image(object_valid ? object_view : depth.view);
    const std::array<vk::WriteDescriptorSet, 17> writes = {{
        write(0, Type::eSampledImage, &depth_info, nullptr),
        write(1, Type::eStorageImage, &diffuse_info, nullptr),
        write(2, Type::eStorageImage, &specular_info, nullptr),
        write(3, Type::eSampledImage, &normal_info, nullptr),
        write(4, Type::eSampledImage, &g3_info, nullptr),
        write(5, Type::eSampledImage, &g1_info, nullptr),
        write(6, Type::eUniformBuffer, nullptr, &params_info),
        write(7, Type::eStorageBuffer, nullptr, &lights_info),
        write(8, Type::eStorageBuffer, nullptr, &sets_info),
        write(12, Type::eStorageBuffer, nullptr, &surfaces_info),
        write(13, Type::eStorageBuffer, nullptr, &uv_pool_info),
        write(14, Type::eSampledImage, &mask_info, nullptr),
        write(15, Type::eStorageBuffer, nullptr, &pick_info),
        write(16, Type::eSampledImage, &water_keep_info, nullptr),
        write(17, Type::eStorageBuffer, nullptr, &hist_in_info),
        write(18, Type::eStorageBuffer, nullptr, &hist_out_info),
        write(19, Type::eSampledImage, &object_info, nullptr),
    }};
    if (sets_valid) {
        Barrier(cmdbuf, Stage::eTransfer | Stage::eComputeShader | Stage::eHost,
                Access::eTransferWrite | Access::eShaderWrite | Access::eHostWrite,
                Stage::eComputeShader | Stage::eTransfer,
                Access::eShaderRead | Access::eTransferRead);
        if (sets_readback_ring.buffer) {
            const u32 set_bytes = direct_set_count * Rt::DirLightSetStride;
            const vk::DescriptorBufferInfo raw_info = sets_info;
            const vk::BufferCopy region{raw_info.offset, u64(slot) * sets_readback_ring.slot_bytes,
                                        set_bytes};
            cmdbuf.copyBuffer(raw_info.buffer, sets_readback_ring.buffer, region);
            Barrier(cmdbuf, Stage::eTransfer, Access::eTransferWrite, Stage::eHost,
                    Access::eHostRead);
            sets_readback[slot] = {
                scheduler.CurrentTick(), frame, set_bytes, direct_sets_source.address,
                {shadow_color[0], shadow_color[1], shadow_color[2]},
                (sets_gpu ? Rt::DirLogGpu : 0u) | (direct_mode == Rt::DirectWrite::Add ? Rt::DirLogAdd : 0u) |
                    (mask_valid ? Rt::DirLogMask : 0u) |
                    (mask_valid && moon_mask ? Rt::DirLogMoonMask : 0u) |
                    (moon_static ? Rt::DirLogMoonStatic : 0u)};
        }
    }
    const u32 groups_x = (depth.width + Rt::DirectTile - 1) / Rt::DirectTile;
    const u32 groups_y = (depth.height + Rt::DirectTile - 1) / Rt::DirectTile;
    if (restir_enabled) {
        const u32 prev = reservoir_index, next = reservoir_index ^ 1, temp = 2;
        const vk::DescriptorBufferInfo prev_info{reservoirs[prev].buffer, 0, reservoirs[prev].size};
        const vk::DescriptorBufferInfo temp_info{reservoirs[temp].buffer, 0, reservoirs[temp].size};
        const vk::DescriptorBufferInfo next_info{reservoirs[next].buffer, 0, reservoirs[next].size};
        const vk::DescriptorBufferInfo remap_info{restir_remap_ring.buffer,
                                                  u64(slot) * restir_remap_ring.slot_bytes,
                                                  restir_remap_ring.slot_bytes};
        std::array<vk::WriteDescriptorSet, 16> restir_writes{};
        for (u32 i = 0; i < 9; ++i) {
            restir_writes[i] = writes[i];
        }
        restir_writes[12] = writes[9];
        restir_writes[13] = writes[10];
        restir_writes[14] = writes[11];
        restir_writes[15] = writes[13];
        restir_writes[9] = write(9, Type::eStorageBuffer, nullptr, &prev_info);
        restir_writes[10] = write(10, Type::eStorageBuffer, nullptr, &temp_info);
        restir_writes[11] = write(11, Type::eStorageBuffer, nullptr, &remap_info);
        Barrier(cmdbuf, Stage::eAllCommands, Access::eShaderRead | Access::eShaderWrite,
                Stage::eComputeShader, Access::eShaderRead | Access::eShaderWrite);
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *restir_temporal_pipeline);
        cmdbuf.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *restir_temporal_pipeline_layout,
                                  1, SurfaceSet(cmdbuf, built_slot), {});
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *restir_temporal_pipeline_layout,
                                    0, restir_writes);
        CrumbBegin(cmdbuf, Rt::Crumb::RestirTemporal);
        cmdbuf.dispatch(groups_x, groups_y, 1);
        CrumbEnd(cmdbuf, Rt::Crumb::RestirTemporal);
        Barrier(cmdbuf, Stage::eComputeShader, Access::eShaderWrite, Stage::eComputeShader,
                Access::eShaderRead);
        restir_writes[9] = write(9, Type::eStorageBuffer, nullptr, &temp_info);
        restir_writes[10] = write(10, Type::eStorageBuffer, nullptr, &next_info);
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *restir_spatial_pipeline);
        cmdbuf.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *restir_spatial_pipeline_layout,
                                  1, SurfaceSet(cmdbuf, built_slot), {});
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *restir_spatial_pipeline_layout,
                                    0, restir_writes);
        CrumbBegin(cmdbuf, Rt::Crumb::RestirSpatial);
        cmdbuf.dispatch(groups_x, groups_y, 1);
        CrumbEnd(cmdbuf, Rt::Crumb::RestirSpatial);
        reservoir_index = next;
        restir_last_frame = frame;
        restir_prev_inv_view = inputs.inv_view;
        restir_prev_projection = inputs.projection;
        restir_prev_jitter = inputs.jitter;
        restir_prev_keys = direct_keys;
        if (restir_prev_keys.size() > Rt::MaxDirectLights) {
            restir_prev_keys.resize(Rt::MaxDirectLights);
        }
        ++restir_frames;
    } else {
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *direct_pipeline);
        cmdbuf.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *direct_pipeline_layout, 1,
                                  SurfaceSet(cmdbuf, built_slot), {});
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *direct_pipeline_layout, 0, writes);
        CrumbBegin(cmdbuf, Rt::Crumb::Direct);
        cmdbuf.dispatch(groups_x, groups_y, 1);
        CrumbEnd(cmdbuf, Rt::Crumb::Direct);
        if (moon_hist_on) {
            moon_hist_index ^= 1u;
            moon_hist_frame = frame;
            moon_prev_inv_view = inputs.inv_view;
            moon_prev_projection = inputs.projection;
            moon_prev_jitter = inputs.jitter;
            ++moon_hist_frames;
        }
        if (pick_px[0] >= 0.0f || kept_probe_on) {
            Barrier(cmdbuf, Stage::eComputeShader, Access::eShaderWrite, Stage::eHost,
                    Access::eHostRead);
        }
        if (pick_px[0] >= 0.0f) {
            pick_tick[slot] = scheduler.CurrentTick();
            pick_frame[slot] = frame;
            pick_built[slot] = built_slot;
        }
        if (kept_probe_on) {
            kept_probe_tick[slot] = scheduler.CurrentTick();
            kept_probe_frame[slot] = frame;
            kept_probe_built[slot] = built_slot;
        }
    }
    Barrier(cmdbuf, Stage::eComputeShader, Access::eShaderWrite, Stage::eAllCommands,
            Access::eShaderRead | Access::eShaderWrite | Access::eColorAttachmentRead |
                Access::eColorAttachmentWrite | Access::eTransferRead);
    TimeEnd(cmdbuf, restir_enabled ? 3u : 2u);
    ++direct_frames;
    direct_written_frame = frame;
    direct_written_diffuse = acc_diffuse;
    direct_written_specular = acc_specular;
    direct_wrote_proxy = inputs.reduced;
    if (DirectTrace()) {
        Trace("direct dispatch recorded: %ux%u groups, %u lights, mode %s, wrote images %u and "
              "%u (%s), surface test on depth %ux%u",
              groups_x, groups_y, light_count,
              direct_mode == Rt::DirectWrite::Add ? "add" : "overwrite", acc_diffuse.index,
              acc_specular.index, inputs.reduced ? "proxy" : "native", depth.width, depth.height);
    }
}

void RayTracing::RecordReflect(const SceneInputs& inputs) {
    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;
    const auto skip = [&](const char* why) {
        ++reflect_skipped;
        static const char* last = nullptr;
        static u32 printed = 0;
        if (why != last && printed < 32) {
            last = why;
            ++printed;
            Log("Ray tracing: reflections skipped at frame %llu (%s)\n", (unsigned long long)frame,
                why);
        }
    };
    const bool camera = inputs.camera_valid && inputs.projection[0] != 0.0f &&
                        inputs.projection[1] != 0.0f && inputs.projection[3] != 0.0f;
    if (!acc_diffuse || !acc_specular || !inputs.depth || !camera) {
        skip("no light accumulation targets, depth or camera this frame");
        return;
    }
    if (instances.empty()) {
        skip("no G-buffer geometry this frame");
        return;
    }
    {
        const auto& depth = texture_cache.GetImage(inputs.depth);
        const auto& acc = texture_cache.GetImage(acc_specular);
        if (acc.info.pixel_format != vk::Format::eB10G11R11UfloatPack32 ||
            !(acc.usage_flags & vk::ImageUsageFlagBits::eStorage) ||
            acc.info.size.width != depth.info.size.width ||
            acc.info.size.height != depth.info.size.height) {
            skip("specular accumulation target is not a storage B10G11R11 image of the depth size");
            return;
        }
    }
    if (!Build(inputs)) {
        skip("acceleration structure not built");
        return;
    }
    if (auto* profiler = GpuProfiler::Get()) {
        profiler->Mark(0xB1A5'0007ull, [] { return std::string{"ray tracing reflections"}; });
    }
    scheduler.EndRendering();
    Bound specular{}, diffuse{}, depth{}, normal{}, g3{}, g1{}, pre_diffuse{}, pre_specular{};
    // See-through glass: glass pixels also scale the irradiance target (binding 28,
    // r11f_g11f_b10f). Not when the direct pass runs after us and adds the pane's diffuse back
    // (reflect_first, BB_RT_DIRECT_BASE=1): there the pane diffuse stays.
    const bool glass = glass_on && !glass_ps.empty();
    const bool glass_after_direct =
        !(direct_enabled && direct_mode == Rt::DirectWrite::Add && direct_base);
    const auto& acc_diffuse_image = texture_cache.GetImage(acc_diffuse);
    const bool glass_irr_format =
        bool(acc_diffuse_image.usage_flags & vk::ImageUsageFlagBits::eStorage) &&
        acc_diffuse_image.info.pixel_format == vk::Format::eB10G11R11UfloatPack32;
    const bool water_keep = water_on && !water_ps.empty();
    const bool glass_irr = (glass || water_keep) && glass_after_direct && glass_irr_format;
    if (!BindScene(acc_specular, inputs, Access::eShaderRead | Access::eShaderWrite, specular) ||
        !BindScene(acc_diffuse, inputs,
                   glass_irr ? Access::eShaderRead | Access::eShaderWrite : Access::eShaderRead,
                   diffuse) ||
        !BindScene(inputs.depth, inputs, Access::eShaderRead, depth) ||
        specular.width != depth.width || specular.height != depth.height ||
        diffuse.width != depth.width || diffuse.height != depth.height) {
        runtime.FlushBarriers();
        skip("accumulation targets or depth not bindable at one size");
        return;
    }
    const auto is_gbuffer = [&](VideoCore::ImageId id) {
        return std::find(gbuffer_targets.begin(), gbuffer_targets.begin() + gbuffer_count, id) !=
               gbuffer_targets.begin() + gbuffer_count;
    };
    const auto ordered = pre_ao_pair.Ordered(is_gbuffer);
    const VideoCore::ImageId acc_prev_diffuse = ordered[0];
    const VideoCore::ImageId acc_prev_specular = ordered[1];
    const bool reflect_first = direct_mode == Rt::DirectWrite::Add && direct_base;
    const bool pair_wanted = reflect_first &&
                             Rt::ReflectUsesPreAo(pre_ao_pair.Valid(frame), direct_enabled,
                                                  light_draws_skipped_this_frame, direct_debug);
    const bool pre_ao = pair_wanted &&
                        BindScene(acc_prev_diffuse, inputs, Access::eShaderRead, pre_diffuse) &&
                        BindScene(acc_prev_specular, inputs, Access::eShaderRead, pre_specular) &&
                        pre_diffuse.width == depth.width && pre_diffuse.height == depth.height &&
                        pre_specular.width == depth.width && pre_specular.height == depth.height;
    static u32 pair_logged = 0;
    if (!pre_ao && direct_enabled && light_draws_skipped_this_frame && direct_debug == 0 &&
        pair_logged < 3) {
        ++pair_logged;
        Log("Ray tracing: reflections frame %llu without the pre-AO pair: %u inputs from frame "
            "%llu%s, %s, pair sizes %ux%u/%ux%u vs %ux%u\n",
            (unsigned long long)frame, pre_ao_pair.count,
            (unsigned long long)pre_ao_pair.frame, pre_ao_pair.overflow ? " (more than two)" : "",
            pair_wanted ? "not bindable" : "not a pair", pre_diffuse.width, pre_diffuse.height,
            pre_specular.width, pre_specular.height, depth.width, depth.height);
    }
    const auto bind_target = [&](int index, vk::Format format, Bound& out) {
        if (index < 0 || u32(index) >= gbuffer_count || !gbuffer_targets[index]) {
            return false;
        }
        if (format != vk::Format::eUndefined &&
            texture_cache.GetImage(gbuffer_targets[index]).info.pixel_format != format) {
            return false;
        }
        Bound bound{};
        if (!BindScene(gbuffer_targets[index], inputs, Access::eShaderRead, bound) ||
            bound.width != depth.width || bound.height != depth.height) {
            return false;
        }
        out = bound;
        return true;
    };
    const bool normal_valid = normal_source.target != 0xFF &&
                              bind_target(int(normal_source.target), vk::Format::eUndefined, normal);
    const bool g3_valid = bind_target(shadow_spec_target, vk::Format::eR8G8B8A8Unorm, g3);
    const bool g1_valid = bind_target(direct_g1_target, vk::Format::eR8G8B8A8Unorm, g1);
    // Glass: the G-buffer albedo (with the crack/dirt decals) against the pane's own texture;
    // sheet water: against the floor's (blood decals under the water).
    const bool want_albedo = glass || water_keep;
    int albedo_index = albedo_target;
    for (u32 i = 0; want_albedo && albedo_index < 0 && i < gbuffer_count; ++i) {
        if (gbuffer_targets[i] && IsSrgb(texture_cache.GetImage(gbuffer_targets[i]).info.pixel_format)) {
            albedo_index = int(i);
        }
    }
    Bound albedo{};
    const bool albedo_valid = want_albedo && bind_target(albedo_index, vk::Format::eUndefined, albedo);
    Bound emissive{};
    const bool emissive_valid = BindEmissive(inputs, depth.width, depth.height, emissive);
    // Mirrors keep the last dome texture while it lives (uid check), not only while the game draws
    // it: looking down at puddles the game skips the dome draw some frames, and the 2-frame
    // SkyDomeLive window flipped the puddles between the sky image and the dark screen fallback.
    const bool dome_known = sky_enabled && sky_pipeline && gi_enabled && material.buffer &&
                            sky_dome_frame != ~0ull;
    VideoCore::Image* dome = dome_known ? texture_cache.TryGetImage(sky_dome_image, 0) : nullptr;
    const bool dome_valid = reflect_sky_dome && dome && dome->image_uid == sky_dome_uid;
    if (dome_valid) {
        runtime.Transit(dome, vk::ImageLayout::eShaderReadOnlyOptimal, Stage::eComputeShader,
                        Access::eShaderRead);
    }
    runtime.FlushBarriers();
    const bool water = water_on && !water_ps.empty() && depth.width >= ao_main_width &&
                       (water_sheets > 0 || water_puddles > 0 || water_mist > 0.0f);
    static int water_logged = -1;
    const int water_state = (water_sheets > 0 ? 1 : 0) | (water_puddles > 0 ? 2 : 0);
    if (water_on && water_state != water_logged) {
        water_logged = water_state;
        Log("Ray tracing: water probes frame %llu: %u sheets, %u puddle floors in the TLAS\n",
            (unsigned long long)frame, water_sheets, water_puddles);
    }
    if (!g1_valid && !water) {
        skip("GBuffer1 (roughness) not bound: every pixel keeps the game's term");
        return;
    }
    if (!gi_width && !CreateGiImages(depth.width, depth.height)) {
        skip("GI image allocation failed");
        return;
    }
    const bool own_set = gi_width != depth.width || gi_height != depth.height;
    if (own_set && (reflect_own.width != depth.width || reflect_own.height != depth.height) &&
        !CreateReflectImages(depth.width, depth.height)) {
        skip("reflection image allocation failed");
        return;
    }
    OwnImage& r_raw = own_set ? reflect_own.raw : raw;
    OwnImage& r_aux_albedo = own_set ? reflect_own.aux_albedo : aux_albedo;
    OwnImage& r_aux_specular = own_set ? reflect_own.aux_specular : aux_specular;
    OwnImage& r_aux_normal = own_set ? reflect_own.aux_normal : aux_normal;
    OwnImage& r_aux_hit = own_set ? reflect_own.aux_hit : aux_hit;
    OwnImage& r_spec_hit = own_set ? reflect_own.spec_hit : aux_spec_hit;
    OwnImage& r_glass_keep = own_set ? reflect_own.glass_keep : glass_keep;
    const Rt::Affine view = Rt::InverseRigid(inputs.inv_view);
    const u32 slot = u32(frame % Slots);
    u32 bounce_flags = (g3_valid ? Rt::BounceFlagG3 : 0u) | (g1_valid ? Rt::BounceFlagG1 : 0u) |
                       (emissive_valid ? Rt::BounceFlagEmissive : 0u) |
                       (portal_instances ? Rt::BounceFlagPortals : 0u);
    u32 light_count = 0;
    u32 set_count = 0;
    GpuSets gpu_sets{};
    if (gi_lights_ring.mapped) {
        light_count = WriteGiLights(slot, view);
        bounce_flags |= Rt::BounceFlagLights;
        if (g3_valid) {
            gpu_sets = ObtainGpuLightSets(Rt::GiLightSetsMaxAge);
        }
        if (gpu_sets.buffer) {
            set_count = gpu_sets.bytes / Rt::DirLightSetStride;
            bounce_flags |= Rt::BounceFlagDirSets;
            ++gi_sets_gpu_frames;
        } else if (g3_valid && direct_set_count > 0 &&
                   direct_sets.size() * 4 >= size_t(direct_set_count) * Rt::DirLightSetStride) {
            set_count = direct_set_count;
            std::memcpy(gi_sets_ring.mapped + u64(slot) * gi_sets_ring.slot_bytes,
                        direct_sets.data(), size_t(set_count) * Rt::DirLightSetStride);
            bounce_flags |= Rt::BounceFlagDirSets;
        }
    }
    LogSetSource("reflect", gpu_sets, set_count, light_count);
    const u32 reflect_flags = (pre_ao ? Rt::ReflectFlagPreAo : 0u) | (g3_valid ? Rt::ReflectFlagF0 : 0u);
    const Rt::GiParams params{
        .proj = inputs.projection,
        .prev_proj = previous_projection,
        .reproject = Rt::Rows(view),
        .view_rows = Rt::Rows(view),
        .size_jitter = {float(depth.width), float(depth.height), inputs.jitter[0], inputs.jitter[1]},
        .prev_jitter_sky_clamp = {previous_jitter[0], previous_jitter[1], gi_sky, gi_clamp},
        .tlas_frame_flags = {u32(tlas_address), u32(tlas_address >> 32), u32(frame), Rt::GiFlagNoScreen},
        .cfg = {1u, gi_max_history,
                (normal_valid ? normal_source.mode : Rt::NormalFromDepth) |
                    normal_source.swizzle << 8 | normal_source.components << 16,
                0u},
        .strength_bias = {reflect_strength, gi_bias, reflect_max_rough, 0.0f},
        .bounce = {1u, gi_bounce_rays, light_count, set_count},
        .bounce_cfg = {Rt::SkySlot(capacity.records), bounce_flags, reflect_flags, reflect_debug},
        .bounce_misc = {gi_emissive_luma, shadow_gap, 0.0f, 0.0f},
        .shadow_color = {shadow_color[0], shadow_color[1], shadow_color[2], moon_cone_cos},
        .sharc = {},
    };
    std::memcpy(reflect_params_ring.mapped + u64(slot) * reflect_params_ring.slot_bytes, &params,
                sizeof(params));
    static u64 printed = 0;
    if (printed < 3) {
        ++printed;
        Log("Ray tracing: reflections frame %llu, %u lights, directional sets %u, normal %s, "
            "GBuffer3 %s, %s, targets %ux%u\n",
            (unsigned long long)frame, light_count, set_count, normal_valid ? "G-buffer" : "depth",
            g3_valid ? "bound (F0 = g^2)" : "none (F0 0.04)",
            pre_ao ? "rebuilt from the AO pass inputs" : "added on top",
            depth.width, depth.height);
    }
    // Logged when the glass state changes (on, irradiance writable, albedo bound), 4 times at most.
    static u32 glass_logged = 0, glass_logged_state = 0;
    const u32 glass_state = (glass ? 1u : 0u) | (glass_irr ? 2u : 0u) | (albedo_valid ? 4u : 0u) |
                            (glass_after_direct ? 8u : 0u);
    if (glass && glass_logged < 4 && glass_state != glass_logged_state) {
        ++glass_logged;
        glass_logged_state = glass_state;
        Log("Ray tracing: see-through glass (BB_RT_GLASS) frame %llu: %zu glass pixel shaders "
            "(BB_RT_GLASS_PS), gloss > %.2f (BB_RT_GLASS_GLOSS), tint %.2f (BB_RT_GLASS_TINT), "
            "debug %u (BB_RT_GLASS_DEBUG); irradiance target %s, G-buffer albedo %s (target "
            "%d)\n",
            (unsigned long long)frame, glass_ps.size(), glass_gloss, glass_tint, glass_debug,
            glass_irr            ? "scaled under the pane"
            : !glass_after_direct ? "kept (the direct pass adds after us, BB_RT_DIRECT_BASE=1)"
                                  : "not a storage r11f_g11f_b10f image: pane diffuse kept",
            albedo_valid ? "bound (decal cover)" : "not bound (no decal cover)", albedo_index);
    }

    const auto cmdbuf = scheduler.CommandBuffer();
    TimeBegin(cmdbuf, 4);
    if (gpu_sets.buffer) {
        Barrier(cmdbuf, Stage::eTransfer | Stage::eComputeShader,
                Access::eTransferWrite | Access::eShaderWrite, Stage::eComputeShader,
                Access::eShaderRead);
    }
    const auto all = Stage::eAllCommands;
    const auto discard = [&](OwnImage& own) {
        return ImageBarrier(vk::Image(own.image), vk::ImageLayout::eUndefined, all,
                            Access::eShaderRead, Stage::eComputeShader, Access::eShaderWrite);
    };
    const std::array<vk::ImageMemoryBarrier2, 7> hit_barriers = {{
        discard(r_spec_hit), discard(r_glass_keep), discard(r_raw), discard(r_aux_albedo),
        discard(r_aux_specular), discard(r_aux_normal), discard(r_aux_hit),
    }};
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = own_set ? 7u : 2u,
                             .pImageMemoryBarriers = hit_barriers.data()});
    if (!material_cleared) {
        material_cleared = true;
        material_resets.clear();
        cmdbuf.fillBuffer(material.buffer, 0, material.size, 0u);
        cmdbuf.fillBuffer(material_accum.buffer, 0, material_accum.size, 0u);
        Barrier(cmdbuf, Stage::eTransfer, Access::eTransferWrite, Stage::eComputeShader,
                Access::eShaderRead | Access::eShaderWrite);
    }
    const auto sampled = [](vk::ImageView view) {
        return vk::DescriptorImageInfo{.imageView = view, .imageLayout = vk::ImageLayout::eGeneral};
    };
    const bool history_ok =
        !own_set && history[history_index].view && history_z[history_index].view;
    const vk::DescriptorImageInfo depth_info = sampled(depth.view);
    const vk::DescriptorImageInfo normal_info = sampled(normal_valid ? normal.view : depth.view);
    const vk::DescriptorImageInfo history_info =
        sampled(history_ok ? *history[history_index].view : *r_raw.view);
    const vk::DescriptorImageInfo history_z_info =
        sampled(history_ok ? *history_z[history_index].view : *r_raw.view);
    const vk::DescriptorImageInfo raw_info = sampled(*r_raw.view);
    const vk::DescriptorBufferInfo params_info{reflect_params_ring.buffer,
                                               u64(slot) * reflect_params_ring.slot_bytes, sizeof(params)};
    const auto write = [](u32 binding, vk::DescriptorType type, const vk::DescriptorImageInfo* image,
                          const vk::DescriptorBufferInfo* buffer) {
        return vk::WriteDescriptorSet{.dstBinding = binding,
                                      .descriptorCount = 1,
                                      .descriptorType = type,
                                      .pImageInfo = image,
                                      .pBufferInfo = buffer};
    };
    using Type = vk::DescriptorType;
    const vk::DescriptorBufferInfo material_info{material.buffer, 0, material.size};
    const vk::DescriptorBufferInfo accum_info{material_accum.buffer, 0, material_accum.size};
    const vk::DescriptorBufferInfo surfaces_info{surfaces_ring.buffer,
                                                 u64(built_slot) * surfaces_ring.slot_bytes,
                                                 surfaces_ring.slot_bytes};
    const vk::DescriptorBufferInfo uv_pool_info{uv_buffer.buffer ? uv_buffer.buffer : soup.buffer, 0,
                                                uv_buffer.buffer ? uv_buffer.size : soup.size};
    const vk::DescriptorImageInfo g3_info = sampled(g3_valid ? g3.view : depth.view);
    const vk::DescriptorImageInfo g1_info = sampled(g1_valid ? g1.view : depth.view);
    const vk::DescriptorImageInfo emissive_info = sampled(emissive_valid ? emissive.view : depth.view);
    const vk::DescriptorImageInfo aux_albedo_info = sampled(*r_aux_albedo.view);
    const vk::DescriptorImageInfo aux_spec_info = sampled(*r_aux_specular.view);
    const vk::DescriptorImageInfo aux_normal_info = sampled(*r_aux_normal.view);
    const vk::DescriptorImageInfo aux_hit_info = sampled(*r_aux_hit.view);
    const vk::DescriptorImageInfo spec_hit_info = sampled(*r_spec_hit.view);
    const vk::DescriptorImageInfo specular_info = sampled(specular.view);
    const vk::DescriptorImageInfo irr_info = sampled(diffuse.view);
    const vk::DescriptorImageInfo pre_irr_info = sampled(pre_ao ? pre_diffuse.view : diffuse.view);
    const vk::DescriptorImageInfo pre_spec_info = sampled(pre_ao ? pre_specular.view : specular.view);
    const vk::DescriptorImageInfo albedo_info = sampled(albedo_valid ? albedo.view : depth.view);
    // Binding 28 is only written with GlassIrr; otherwise it aliases the specular target unused.
    const vk::DescriptorImageInfo glass_irr_info = sampled(glass_irr ? diffuse.view : specular.view);
    const vk::DescriptorImageInfo glass_keep_info = sampled(*r_glass_keep.view);
    const bool lights_bound = gi_lights_ring.buffer && gi_sets_ring.buffer;
    const vk::DescriptorBufferInfo lights_info{lights_bound ? gi_lights_ring.buffer : material.buffer,
                                               lights_bound ? u64(slot) * gi_lights_ring.slot_bytes : 0,
                                               lights_bound ? gi_lights_ring.slot_bytes : material.size};
    const vk::DescriptorBufferInfo sets_info =
        gpu_sets.buffer
            ? vk::DescriptorBufferInfo{gpu_sets.buffer, gpu_sets.offset, gpu_sets.bytes}
            : vk::DescriptorBufferInfo{lights_bound ? gi_sets_ring.buffer : material.buffer,
                                       lights_bound ? u64(slot) * gi_sets_ring.slot_bytes : 0,
                                       lights_bound ? gi_sets_ring.slot_bytes : material.size};
    const bool fog_table = gi_fog_ring.buffer && gi_fog_bytes >= 192 &&
                           (gi_fog_frame == frame || gi_fog_frame + 1 == frame);
    const vk::DescriptorBufferInfo fog_table_info{
        fog_table ? gi_fog_ring.buffer : material.buffer,
        fog_table ? u64(gi_fog_frame % Slots) * gi_fog_ring.slot_bytes : 0,
        fog_table ? gi_fog_bytes : material.size};
    const vk::DescriptorImageInfo dome_info{
        .sampler = *surface_sampler,
        .imageView = dome_valid ? sky_dome_view : depth.view,
        .imageLayout = dome_valid ? vk::ImageLayout::eShaderReadOnlyOptimal : vk::ImageLayout::eGeneral};
    const std::array<vk::WriteDescriptorSet, 32> writes = {{
        write(0, Type::eSampledImage, &depth_info, nullptr),
        write(1, Type::eSampledImage, &depth_info, nullptr),
        write(2, Type::eSampledImage, &normal_info, nullptr),
        write(3, Type::eSampledImage, &history_info, nullptr),
        write(4, Type::eSampledImage, &history_z_info, nullptr),
        write(5, Type::eStorageImage, &raw_info, nullptr),
        write(6, Type::eStorageImage, &raw_info, nullptr),
        write(7, Type::eStorageImage, &raw_info, nullptr),
        write(8, Type::eUniformBuffer, nullptr, &params_info),
        write(9, Type::eStorageBuffer, nullptr, &material_info),
        write(10, Type::eStorageBuffer, nullptr, &accum_info),
        write(11, Type::eStorageBuffer, nullptr, &surfaces_info),
        write(12, Type::eStorageBuffer, nullptr, &uv_pool_info),
        write(13, Type::eSampledImage, &g3_info, nullptr),
        write(14, Type::eSampledImage, &g1_info, nullptr),
        write(15, Type::eSampledImage, &albedo_info, nullptr),
        write(16, Type::eStorageBuffer, nullptr, &lights_info),
        write(17, Type::eStorageBuffer, nullptr, &sets_info),
        write(18, Type::eStorageImage, &aux_albedo_info, nullptr),
        write(19, Type::eStorageImage, &aux_spec_info, nullptr),
        write(20, Type::eStorageImage, &aux_normal_info, nullptr),
        write(21, Type::eStorageImage, &aux_hit_info, nullptr),
        write(22, Type::eStorageImage, &specular_info, nullptr),
        write(23, Type::eSampledImage, &irr_info, nullptr),
        write(24, Type::eSampledImage, &pre_irr_info, nullptr),
        write(25, Type::eSampledImage, &pre_spec_info, nullptr),
        write(26, Type::eStorageImage, &spec_hit_info, nullptr),
        write(27, Type::eSampledImage, &emissive_info, nullptr),
        write(28, Type::eStorageImage, &glass_irr_info, nullptr),
        write(29, Type::eStorageImage, &glass_keep_info, nullptr),
        write(30, Type::eStorageBuffer, nullptr, &fog_table_info),
        write(31, Type::eCombinedImageSampler, &dome_info, nullptr),
    }};
    if (g3_valid && (bounce_flags & Rt::BounceFlagDirSets) != 0u) {
        RecordWorldSet(cmdbuf, depth.view, g3.view, sets_info, set_count, depth.width,
                       depth.height, reflect_params_ring.buffer,
                       u64(slot) * reflect_params_ring.slot_bytes);
    }
    const float base_cut =
        reflect_first && direct_enabled ? std::clamp(ambient_spec_cut, 0.0f, 1.0f) : 0.0f;
    const GlassPush glass_push{
        .gloss = glass_gloss,
        .tint = glass_tint,
        .flags = (glass ? GlassOn : 0u) | (glass || water ? GlassKeep : 0u) |
                 (glass && (glass_debug == 1 || glass_debug == 2) ? GlassDebugMask : 0u) |
                 (glass && glass_debug == 2 ? GlassDebugCandidates : 0u) |
                 (glass_irr ? GlassIrr : 0u) | (albedo_valid ? GlassAlbedo : 0u) |
                 (water ? WaterOn : 0u) |
                 (water_on && water_debug ? WaterDebugMask : 0u) | (water_at_floor ? WaterAtFloor : 0u) |
                 (dome_valid ? ReflectSkyDome : 0u) | (water_sheets ? WaterSheets : 0u) |
                 (water_puddles ? WaterPuddles : 0u),
        .skip = GlassSkip,
        .water_rough = water_rough,
        .water_f0 = water_f0,
        .base_cut = base_cut,
        .sheet_depth = water_sheet_depth,
        .murk = water_murk,
        .ripple = water_ripple,
        .sheet_f0 = water_sheet_f0,
        .mist = water_mist,
        .mist_height = water_mist_height,
        .mist_albedo = water_mist_albedo,
        .mist_steps = float(water_mist_steps),
        .mist_patch = water_mist_patch,
        .mist_glow = water_mist_glow,
        .mist_glow_dark = water_mist_glow_dark,
        .mist_sheet = mist_sheets ? 1.0f : 0.0f,
        .mist_fog = fog_table ? water_mist_fog : 0.0f,
        .raise = water_raise,
        .body = water_body,
        .sheen = water_sheen,
        .sheen_far = water_sheen_far,
        .puddle_edge = water_puddle_edge,
        .puddle_wet = water_puddle_wet,
    };
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *reflect_pipeline);
    cmdbuf.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *reflect_pipeline_layout, 1,
                              SurfaceSet(cmdbuf, built_slot), {});
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *reflect_pipeline_layout, 0, writes);
    cmdbuf.pushConstants(*reflect_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
                         sizeof(glass_push), &glass_push);
    CrumbBegin(cmdbuf, Rt::Crumb::Reflect);
    cmdbuf.dispatch((depth.width + 7) / 8, (depth.height + 7) / 8, 1);
    CrumbEnd(cmdbuf, Rt::Crumb::Reflect);
    spec_cut_frame = base_cut > 0.0f ? frame : ~0ull;
    Barrier(cmdbuf, Stage::eComputeShader, Access::eShaderWrite | Access::eShaderRead, all,
            Access::eShaderRead | Access::eShaderWrite | Access::eColorAttachmentRead |
                Access::eColorAttachmentWrite | Access::eTransferRead);
    TimeEnd(cmdbuf, 4);
    if (own_set) {
        glass_keep_frame = ~0ull;
    } else {
        aux_spec_hit.written = true;
        glass_keep_frame = glass || water ? frame : ~0ull;
        reflect_frame = frame;
    }
    ++reflect_frames;
    if (pre_ao) {
        ++reflect_pre_ao_frames;
    }
}

void RayTracing::OnAfterComposite(const SceneInputs& inputs) {
    if (!enabled || !gi_enabled || !gi_at_composite || gi_composite_frame == frame ||
        Rt::effects_paused.load(std::memory_order_relaxed)) {
        return;
    }
    // With FSR 4.1 the composite shader also draws once before the scene colour target of the
    // frame exists. GI waits for the real composite (or the post trigger): a skip here would
    // reset the GI history every frame.
    if (!scene_color) {
        ++gi_early_composites;
        return;
    }
    SetLightKeepCamera(inputs);
    if (light_capture && light_frame == frame) {
        EndLightFrame();
    }
    CpuTimer timer{hitch_log, hitch.gi};
    if (RecordGi(inputs)) {
        gi_composite_frame = frame;
        last_gi_frame = frame;
    }
}

void RayTracing::OnDispatch(u64 cs_hash, const SceneInputs& inputs) {
    if (!enabled || cs_hash != trigger_hash || last_post_frame == frame) {
        return;
    }
    SetLightKeepCamera(inputs);
    if (light_capture && light_frame == frame) {
        EndLightFrame();
    }
    if (!(gi_enabled || debug_mode || shadows_enabled || fog_enabled)) {
        return;
    }
    const bool paused = Rt::effects_paused.load(std::memory_order_relaxed);
    if (paused != effects_were_paused) {
        effects_were_paused = paused;
        Log("Ray tracing: effects %s (F9)\n", paused ? "off" : "on");
        if (!paused) {
            history_valid = false;
        }
    }
    if (paused) {
        RecordExposure(inputs, false);
        return;
    }
    last_post_frame = frame;
    if (gi_enabled && gi_composite_frame != frame) {
        last_gi_frame = frame;
        CpuTimer timer{hitch_log, hitch.gi};
        RecordGi(inputs);
    }
    if (shadows_enabled) {
        CpuTimer timer{hitch_log, hitch.gi};
        RecordShadows(inputs);
    }
    if (sky_enabled) {
        CpuTimer timer{hitch_log, hitch.gi};
        RecordSky(inputs);
    }
    if (fog_enabled) {
        CpuTimer timer{hitch_log, hitch.gi};
        RecordFog(inputs);
    }
    if (gi_enabled) {
        CpuTimer timer{hitch_log, hitch.gi};
        RecordExposure(inputs, true);
    }
    if (debug_mode) {
        CpuTimer timer{hitch_log, hitch.debug};
        RecordDebug(inputs);
    }
}

void RayTracing::OnSkyDome(vk::ImageView view, VideoCore::ImageId image, u64 uid) {
    if (!enabled || !sky_enabled || !view || !image) {
        return;
    }
    // The same pixel shader also draws a square sky layer; only the 4:1 panorama is the dome.
    const VideoCore::Image* img = texture_cache.TryGetImage(image, 0);
    if (!img || img->info.size.width != 4 * img->info.size.height) {
        return;
    }
    if (image != sky_dome_image || uid != sky_dome_uid) {
        {
            Log("Ray tracing: sky dome texture image %u %ux%u, %u levels\n", image.index,
                img->info.size.width, img->info.size.height, img->info.resources.levels);
        }
    }
    sky_dome_view = view;
    sky_dome_image = image;
    sky_dome_uid = uid;
    sky_dome_frame = frame;
}

bool RayTracing::SkyDomeLive() const {
    return sky_enabled && sky_pipeline && gi_enabled && material.buffer && sky_dome_frame != ~0ull &&
           frame - sky_dome_frame <= Rt::SkyDomeMaxAge;
}

void RayTracing::RecordSky(const SceneInputs& inputs) {
    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;
    const auto skip = [&](const char* why) {
        ++sky_skipped;
        static u64 printed = 0;
        if (printed < 8) {
            ++printed;
            Log("Ray tracing: sky pass skipped (%s)\n", why);
        }
    };
    // Where the game draws no dome (Upper Cathedral Ward: its sky is another shader's) only
    // the moon search runs: it needs depth and scene colour, not the dome. Without it that
    // map's moon was never looked for and the light stayed on the game's own direction.
    const bool dome_live = SkyDomeLive();
    if (!dome_live && !(moon_from_sky && sky_enabled && sky_pipeline && gi_enabled &&
                        material.buffer)) {
        return;
    }
    const bool camera = inputs.camera_valid && inputs.projection[0] != 0.0f &&
                        inputs.projection[1] != 0.0f;
    if (!scene_color || !inputs.depth || !camera) {
        skip("no scene colour, depth or camera this frame");
        return;
    }
    VideoCore::Image* dome = dome_live ? texture_cache.TryGetImage(sky_dome_image, 0) : nullptr;
    if (dome_live && (!dome || dome->image_uid != sky_dome_uid)) {
        skip("dome texture gone");
        sky_dome_frame = ~0ull;
        return;
    }
    {
        const auto& color = texture_cache.GetImage(scene_color);
        if (color.info.pixel_format != vk::Format::eR16G16B16A16Sfloat ||
            !(color.usage_flags & vk::ImageUsageFlagBits::eStorage)) {
            skip("scene colour is not a storage RGBA16F image");
            return;
        }
    }
    scheduler.EndRendering();
    Bound color{}, depth{};
    if (!BindScene(scene_color, inputs, Access::eShaderRead | Access::eShaderWrite, color) ||
        !BindScene(inputs.depth, inputs, Access::eShaderRead, depth) ||
        color.width != depth.width || color.height != depth.height) {
        runtime.FlushBarriers();
        skip("scene colour or depth not bindable at one size");
        return;
    }
    if (dome) {
        runtime.Transit(dome, vk::ImageLayout::eShaderReadOnlyOptimal, Stage::eComputeShader,
                        Access::eShaderRead);
    }
    runtime.FlushBarriers();
    const Rt::Affine view = Rt::InverseRigid(inputs.inv_view);
    Rt::SkyPush push{
        .view_rows = Rt::Rows(view),
        .proj = inputs.projection,
        .size_jitter = {float(depth.width), float(depth.height), inputs.jitter[0], inputs.jitter[1]},
        .cfg = {Rt::SkySlot(capacity.records), sky_debug, 0u, 0u},
        .dome = {sky_u_offset, sky_u_repeat, sky_v_divisor, gi_clamp},
    };
    const auto cmdbuf = scheduler.CommandBuffer();
    const vk::DescriptorImageInfo depth_info{.imageView = depth.view,
                                             .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo scene_info{.imageView = color.view,
                                             .imageLayout = vk::ImageLayout::eGeneral};
    // No dome: the depth view stands in (the moon search does not sample the dome).
    const vk::DescriptorImageInfo dome_info{
        .sampler = *surface_sampler,
        .imageView = dome ? sky_dome_view : depth.view,
        .imageLayout = dome ? vk::ImageLayout::eShaderReadOnlyOptimal : vk::ImageLayout::eGeneral};
    const vk::DescriptorBufferInfo material_info{material.buffer, 0, material.size};
    using Type = vk::DescriptorType;
    const std::array<vk::WriteDescriptorSet, 4> writes = {{
        {.dstBinding = 0, .descriptorCount = 1, .descriptorType = Type::eSampledImage,
         .pImageInfo = &depth_info},
        {.dstBinding = 1, .descriptorCount = 1, .descriptorType = Type::eStorageImage,
         .pImageInfo = &scene_info},
        {.dstBinding = 2, .descriptorCount = 1, .descriptorType = Type::eCombinedImageSampler,
         .pImageInfo = &dome_info},
        {.dstBinding = 3, .descriptorCount = 1, .descriptorType = Type::eStorageBuffer,
         .pBufferInfo = &material_info},
    }};
    Barrier(cmdbuf, Stage::eAllCommands, Access::eShaderWrite | Access::eColorAttachmentWrite,
            Stage::eComputeShader, Access::eShaderRead | Access::eShaderWrite);
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *sky_pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *sky_pipeline_layout, 0, writes);
    cmdbuf.pushConstants(*sky_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push),
                         &push);
    if (dome) {
        cmdbuf.dispatch(1, 1, 1);
    }
    // The moon on screen (rt_sky.comp FindScreenMoon), for the layer search's next run.
    if (moon_from_sky) {
        Barrier(cmdbuf, Stage::eComputeShader, Access::eShaderWrite, Stage::eComputeShader,
                Access::eShaderRead | Access::eShaderWrite);
        push.cfg[2] = 3u;
        cmdbuf.pushConstants(*sky_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
                             sizeof(push), &push);
        cmdbuf.dispatch(1, 1, 1);
    }
    if (sky_image && dome) {
        Barrier(cmdbuf, Stage::eComputeShader, Access::eShaderWrite, Stage::eComputeShader,
                Access::eShaderRead | Access::eShaderWrite);
        push.cfg[2] = 2u;
        cmdbuf.pushConstants(*sky_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
                             sizeof(push), &push);
        cmdbuf.dispatch((((depth.width + 1) / 2) * ((depth.height + 1) / 2) + 255) / 256, 1, 1);
    }
    if (sky_debug && dome) {
        Barrier(cmdbuf, Stage::eComputeShader, Access::eShaderWrite, Stage::eComputeShader,
                Access::eShaderRead | Access::eShaderWrite);
        push.cfg[2] = 1u;
        cmdbuf.pushConstants(*sky_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
                             sizeof(push), &push);
        cmdbuf.dispatch((depth.width * depth.height + 255) / 256, 1, 1);
    }
    Barrier(cmdbuf, Stage::eComputeShader, Access::eShaderWrite, Stage::eAllCommands,
            Access::eShaderRead | Access::eShaderWrite | Access::eColorAttachmentRead |
                Access::eColorAttachmentWrite | Access::eTransferRead);
    if (sky_frames++ == 0) {
        Log("Ray tracing: sky dome light on (u = (%.3f + azimuth) x %.2f, v / %.0f), debug %u\n",
            sky_u_offset, sky_u_repeat, sky_v_divisor, sky_debug);
    }
}

void RayTracing::RecordExposure(const SceneInputs& inputs, bool apply) {
    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;
    if (!exposure_pipeline || !exposure_ring.mapped || !scene_color || !inputs.depth) {
        return;
    }
    const float* state = reinterpret_cast<const float*>(exposure_ring.mapped);
    if (exposure_frames > 0 && exposure_frames % 120 == 0) {
        Log("RT exposure: geometry log2 luminance %.2f, %.0f samples, boost %+.2f EV (x%.2f); "
            "stock (F9) %.2f; key %.2f, max %.1f EV (BB_RT_EXPOSURE %s)\n",
            state[1], state[2], state[0], state[3], state[4], exposure_key, exposure_max,
            exposure_on ? "on" : "off");
    }
    ++exposure_frames;
    if (apply && !exposure_on) {
        exposure_reset = true;
        return;
    }
    {
        const auto& color = texture_cache.GetImage(scene_color);
        if (color.info.pixel_format != vk::Format::eR16G16B16A16Sfloat ||
            !(color.usage_flags & vk::ImageUsageFlagBits::eStorage)) {
            return;
        }
    }
    scheduler.EndRendering();
    Bound color{}, depth{};
    if (!BindScene(scene_color, inputs, Access::eShaderRead | Access::eShaderWrite, color) ||
        !BindScene(inputs.depth, inputs, Access::eShaderRead, depth) ||
        color.width != depth.width || color.height != depth.height) {
        runtime.FlushBarriers();
        return;
    }
    runtime.FlushBarriers();
    ExposurePush push{
        .size_cfg = {float(color.width), float(color.height), 0.0f, apply ? 1.0f : 0.0f},
        .params = {exposure_key, exposure_max, exposure_rate, exposure_reset && apply ? 1.0f : 0.0f},
        .tone = {exposure_white, 0.0f, 0.0f, 0.0f},
    };
    if (apply) {
        exposure_reset = false;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    const vk::DescriptorImageInfo depth_info{.imageView = depth.view,
                                             .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo scene_info{.imageView = color.view,
                                             .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorBufferInfo state_info{exposure_ring.buffer, 0, ExposureSlotBytes};
    using Type = vk::DescriptorType;
    const std::array<vk::WriteDescriptorSet, 3> writes = {{
        {.dstBinding = 0, .descriptorCount = 1, .descriptorType = Type::eSampledImage,
         .pImageInfo = &depth_info},
        {.dstBinding = 1, .descriptorCount = 1, .descriptorType = Type::eStorageImage,
         .pImageInfo = &scene_info},
        {.dstBinding = 2, .descriptorCount = 1, .descriptorType = Type::eStorageBuffer,
         .pBufferInfo = &state_info},
    }};
    Barrier(cmdbuf, Stage::eAllCommands, Access::eShaderWrite | Access::eColorAttachmentWrite,
            Stage::eComputeShader, Access::eShaderRead | Access::eShaderWrite);
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *exposure_pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *exposure_pipeline_layout, 0,
                                writes);
    cmdbuf.pushConstants(*exposure_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
                         sizeof(push), &push);
    cmdbuf.dispatch(1, 1, 1);
    if (apply) {
        Barrier(cmdbuf, Stage::eComputeShader, Access::eShaderWrite, Stage::eComputeShader,
                Access::eShaderRead | Access::eShaderWrite);
        push.size_cfg[2] = 1.0f;
        cmdbuf.pushConstants(*exposure_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
                             sizeof(push), &push);
        cmdbuf.dispatch((color.width * color.height + 255) / 256, 1, 1);
    }
    Barrier(cmdbuf, Stage::eComputeShader, Access::eShaderWrite, Stage::eAllCommands,
            Access::eShaderRead | Access::eShaderWrite | Access::eColorAttachmentRead |
                Access::eColorAttachmentWrite | Access::eTransferRead);
}

void RayTracing::RecordFog(const SceneInputs& inputs) {
    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;
    const auto skip = [&](const char* why) {
        ++fog_skipped;
        static u64 printed = 0;
        if (printed < 8) {
            ++printed;
            Log("Ray tracing: fog skipped (%s)\n", why);
        }
    };
    const bool camera = inputs.camera_valid && inputs.projection[0] != 0.0f &&
                        inputs.projection[1] != 0.0f && inputs.projection[3] != 0.0f;
    if (!scene_color || !inputs.depth || !camera) {
        skip("no scene colour, depth or camera this frame");
        return;
    }
    if (fog_stock) {
        return;
    }
    if (gi_fog_frame != frame || gi_fog_bytes < 192) {
        skip("no fog table snapshot this frame");
        return;
    }
    if (fog_surfaces && !surfaces_ring.buffer) {
        skip("surface buffers missing");
        return;
    }
    if (instances.empty()) {
        skip("no G-buffer geometry this frame");
        return;
    }
    {
        const auto& color = texture_cache.GetImage(scene_color);
        const auto& depth = texture_cache.GetImage(inputs.depth);
        if (color.info.pixel_format != vk::Format::eR16G16B16A16Sfloat ||
            !(color.usage_flags & vk::ImageUsageFlagBits::eStorage) ||
            color.info.size.width != depth.info.size.width ||
            color.info.size.height != depth.info.size.height) {
            skip("scene colour is not a storage RGBA16F image of the depth size");
            return;
        }
    }
    if (!Build(inputs)) {
        skip("acceleration structure not built");
        return;
    }
    if (auto* profiler = GpuProfiler::Get()) {
        profiler->Mark(0xB1A50006ull, [] { return std::string{"ray tracing fog"}; });
    }
    scheduler.EndRendering();
    Bound color{}, depth{};
    if (!BindScene(scene_color, inputs, Access::eShaderRead | Access::eShaderWrite, color) ||
        !BindScene(inputs.depth, inputs, Access::eShaderRead, depth) ||
        color.width != depth.width || color.height != depth.height) {
        runtime.FlushBarriers();
        skip("scene colour or depth not bindable at one size");
        return;
    }
    runtime.FlushBarriers();
    const u32 qw = (color.width + Rt::FogScale - 1) / Rt::FogScale;
    const u32 qh = (color.height + Rt::FogScale - 1) / Rt::FogScale;
    if (fog_width != qw || fog_height_px != qh) {
        if (!CreateFogImages(qw, qh)) {
            skip("fog image allocation failed");
            return;
        }
    }
    const bool sets_valid = fog_sets_frame == frame && fog_set_count > 0 && fog_sets_info.buffer;
    Bound g3{};
    const bool g3_valid =
        shadow_spec_target >= 0 && u32(shadow_spec_target) < gbuffer_count &&
        gbuffer_targets[shadow_spec_target] &&
        texture_cache.GetImage(gbuffer_targets[shadow_spec_target]).info.pixel_format ==
            vk::Format::eR8G8B8A8Unorm &&
        BindScene(gbuffer_targets[shadow_spec_target], inputs, Access::eShaderRead, g3) &&
        g3.width == depth.width && g3.height == depth.height;
    if (!g3_valid) {
        runtime.FlushBarriers();
        skip("G-buffer 3 (fog set index) not bindable");
        return;
    }
    runtime.FlushBarriers();
    const bool history = fog_last_frame + 1 == frame && fog_images[fog_index].written &&
                         !Rt::CameraCut(fog_prev_inv_view, inputs.inv_view, 3.0f, 0.9f);
    const Rt::Affine view = Rt::InverseRigid(inputs.inv_view);
    const u32 slot = u32(frame % Slots);
    const bool lamps_ring = gi_enabled && gi_lights_ring.mapped && gi_lights_ring.buffer;
    if (lamps_ring && gi_lights_frame != frame) {
        WriteGiLights(slot, view);
    }
    const bool lamps_valid = lamps_ring && gi_lights_frame == frame &&
                             gi_lights_count > 0;
    const bool sky_valid = gi_enabled && static_cast<bool>(material.buffer);
    const Rt::FogParams params{
        .proj = inputs.projection,
        .view_rows = Rt::Rows(view),
        .size_jitter = {float(depth.width), float(depth.height), inputs.jitter[0], inputs.jitter[1]},
        .tlas_count_flags = {u32(tlas_address), u32(tlas_address >> 32), sets_valid ? fog_set_count : 0u,
                             (sets_valid ? Rt::FogFlagSets : 0u) | (history ? Rt::FogFlagHistory : 0u) |
                                 Rt::FogFlagTable},
        .cfg = {u32(frame), fog_steps, fog_debug, std::bit_cast<u32>(fog_alpha_distance)},
        .fog = {fog_shaft, fog_height, fog_strength, fog_distance},
        .prev_proj = fog_prev_projection,
        .reproject = Rt::Rows(history ? Rt::Reprojection(fog_prev_inv_view, inputs.inv_view) : view),
        .prev_jitter = {fog_prev_jitter[0], fog_prev_jitter[1], fx_smoke_opacity,
                        float(gi_fog_bytes / 16u)},
        // Moon, sky and lamp in-scatter share one unit (light x phase, over the sky reference), so
        // there is no per-term scale (full path tracing plan item 6).
        .moon = {1.0f, 1.0f, 1.0f, fog_g},
        .light_cfg = {lamps_valid ? gi_lights_count : 0u, Rt::SkySlot(capacity.records),
                      (lamps_valid ? Rt::FogLightLamps : 0u) | (sky_valid ? Rt::FogLightSky : 0u) |
                          (SkyDomeLive() ? Rt::FogLightDome : 0u) |
                          (fog_sun_cal ? Rt::FogLightSunCal : 0u) |
                          (portal_instances ? Rt::FogLightPortals : 0u),
                      std::bit_cast<u32>(gi_clamp)},
        .pick = pick_ring.buffer
                    ? std::array<float, 4>{fog_pick_px[0], fog_pick_px[1], fog_pick_px[2], fog_sun_share}
                    : std::array<float, 4>{-1.0f, -1.0f, 0.0f, fog_sun_share},
    };
    std::memcpy(fog_params_ring.mapped + u64(slot) * fog_params_ring.slot_bytes, &params,
                sizeof(params));
    const u32 prev = fog_index, next = fog_index ^ 1;
    const auto cmdbuf = scheduler.CommandBuffer();
    TimeBegin(cmdbuf, 7);
    const auto layout = [&](OwnImage& own) {
        vk::ImageMemoryBarrier2 barrier{
            .srcStageMask = Stage::eAllCommands,
            .srcAccessMask = Access::eShaderRead | Access::eShaderWrite,
            .dstStageMask = Stage::eComputeShader,
            .dstAccessMask = Access::eShaderRead | Access::eShaderWrite,
            .oldLayout = own.written ? vk::ImageLayout::eGeneral : vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = vk::Image(own.image),
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
        own.written = true;
    };
    layout(fog_images[prev]);
    layout(fog_images[next]);
    const auto sampled = [](vk::ImageView view) {
        return vk::DescriptorImageInfo{.imageView = view, .imageLayout = vk::ImageLayout::eGeneral};
    };
    const vk::DescriptorImageInfo depth_info = sampled(depth.view);
    const vk::DescriptorImageInfo scene_info = sampled(color.view);
    const vk::DescriptorImageInfo out_info = sampled(*fog_images[next].view);
    const vk::DescriptorImageInfo prev_info = sampled(*fog_images[prev].view);
    const vk::DescriptorBufferInfo params_info{fog_params_ring.buffer,
                                               u64(slot) * fog_params_ring.slot_bytes, sizeof(params)};
    const vk::DescriptorBufferInfo sets_info =
        sets_valid ? fog_sets_info
                   : vk::DescriptorBufferInfo{fog_params_ring.buffer, 0, fog_params_ring.slot_bytes};
    const auto write = [](u32 binding, vk::DescriptorType type, const vk::DescriptorImageInfo* image,
                          const vk::DescriptorBufferInfo* buffer) {
        return vk::WriteDescriptorSet{.dstBinding = binding,
                                      .descriptorCount = 1,
                                      .descriptorType = type,
                                      .pImageInfo = image,
                                      .pBufferInfo = buffer};
    };
    using Type = vk::DescriptorType;
    const vk::DescriptorImageInfo g3_info = sampled(g3.view);
    const vk::DescriptorBufferInfo table_info{gi_fog_ring.buffer, u64(slot) * gi_fog_ring.slot_bytes,
                                              gi_fog_bytes};
    const std::array<vk::WriteDescriptorSet, 9> writes = {{
        write(0, Type::eSampledImage, &depth_info, nullptr),
        write(1, Type::eStorageImage, &scene_info, nullptr),
        write(2, Type::eStorageImage, &out_info, nullptr),
        write(3, Type::eSampledImage, &prev_info, nullptr),
        write(4, Type::eUniformBuffer, nullptr, &params_info),
        write(5, Type::eStorageBuffer, nullptr, &sets_info),
        write(6, Type::eSampledImage, &out_info, nullptr),
        write(9, Type::eSampledImage, &g3_info, nullptr),
        write(10, Type::eStorageBuffer, nullptr, &table_info),
    }};
    const vk::DescriptorBufferInfo surfaces_info{surfaces_ring.buffer,
                                                 u64(built_slot) * surfaces_ring.slot_bytes,
                                                 surfaces_ring.slot_bytes};
    const vk::DescriptorBufferInfo uv_pool_info{uv_buffer.buffer ? uv_buffer.buffer : soup.buffer, 0,
                                                uv_buffer.buffer ? uv_buffer.size : soup.size};
    const vk::DescriptorBufferInfo lights_info{
        lamps_ring ? gi_lights_ring.buffer : fog_params_ring.buffer,
        lamps_ring ? u64(slot) * gi_lights_ring.slot_bytes : 0,
        lamps_ring ? gi_lights_ring.slot_bytes : fog_params_ring.slot_bytes};
    const vk::DescriptorBufferInfo material_info{
        sky_valid ? material.buffer : fog_params_ring.buffer, 0,
        sky_valid ? material.size : fog_params_ring.slot_bytes};
    const vk::DescriptorBufferInfo fog_pick_info{
        pick_ring.buffer ? pick_ring.buffer : fog_params_ring.buffer,
        pick_ring.buffer ? u64(slot) * pick_ring.slot_bytes : 0,
        pick_ring.buffer ? pick_ring.slot_bytes : fog_params_ring.slot_bytes};
    const std::array<vk::WriteDescriptorSet, 12> opaque_writes = {{
        writes[0], writes[1], writes[2], writes[3], writes[4], writes[5], writes[6], writes[7],
        writes[8],
        write(11, Type::eStorageBuffer, nullptr, &lights_info),
        write(12, Type::eStorageBuffer, nullptr, &material_info),
        write(13, Type::eStorageBuffer, nullptr, &fog_pick_info),
    }};
    const std::array<vk::WriteDescriptorSet, 14> march_writes = {{
        writes[0], writes[1], writes[2], writes[3], writes[4], writes[5], writes[6], writes[7],
        writes[8],
        write(13, Type::eStorageBuffer, nullptr, &fog_pick_info),
        write(7, Type::eStorageBuffer, nullptr, &surfaces_info),
        write(8, Type::eStorageBuffer, nullptr, &uv_pool_info),
        write(11, Type::eStorageBuffer, nullptr, &lights_info),
        write(12, Type::eStorageBuffer, nullptr, &material_info),
    }};
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *fog_pipeline);
    if (fog_surfaces && surfaces_ring.buffer) {
        cmdbuf.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *fog_pipeline_layout, 1,
                                  SurfaceSet(cmdbuf, built_slot), {});
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *fog_pipeline_layout, 0,
                                    march_writes);
    } else {
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *fog_pipeline_layout, 0,
                                    opaque_writes);
    }
    cmdbuf.dispatch((qw + 7) / 8, (qh + 7) / 8, 1);
    Barrier(cmdbuf, Stage::eComputeShader, Access::eShaderWrite, Stage::eComputeShader,
            Access::eShaderRead);
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *fog_apply_pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *fog_apply_pipeline_layout, 0, writes);
    cmdbuf.dispatch((color.width + 7) / 8, (color.height + 7) / 8, 1);
    Barrier(cmdbuf, Stage::eComputeShader, Access::eShaderWrite, Stage::eAllCommands,
            Access::eShaderRead | Access::eShaderWrite | Access::eColorAttachmentRead |
                Access::eColorAttachmentWrite | Access::eTransferRead);
    TimeEnd(cmdbuf, 7);
    fog_index = next;
    fog_last_frame = frame;
    fog_prev_inv_view = inputs.inv_view;
    fog_prev_projection = inputs.projection;
    fog_prev_jitter = inputs.jitter;
    ++fog_frames;
    fog_ran_frame = frame;
    static u64 printed = 0;
    if (printed < 2) {
        ++printed;
        Log("Ray tracing: fog frame %llu, %ux%u in-scatter, %u steps, sets %s, history %s, shaft "
            "%.3f, height falloff %.3f, strength %.2f, distance %.1f, lamps %u, sky %s\n",
            (unsigned long long)frame, qw, qh, fog_steps, sets_valid ? "bound" : "none",
            history ? "on" : "off", fog_shaft, fog_height, fog_strength, fog_distance,
            lamps_valid ? gi_lights_count : 0u, sky_valid ? "probe" : "none");
    }
}

bool RayTracing::BindScene(VideoCore::ImageId id, const SceneInputs& inputs,
                           vk::AccessFlags2 access, Bound& out) {
    if (!id) {
        return false;
    }
    auto& image = texture_cache.GetImage(id);
    if (inputs.reduced) {
        if (!inputs.scene_targets || !inputs.scene_targets->EligibleScene(image)) {
            return false;
        }
        VideoCore::ImageViewInfo info;
        info.format = image.info.pixel_format;
        const auto target = inputs.scene_targets->Read(
            id, info, vk::PipelineStageFlagBits2::eComputeShader, access);
        const auto size = inputs.scene_targets->Size();
        out = {target.image, target.view, size.width, size.height};
        return out.image && out.view;
    }
    runtime.Transit(&image, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    access);
    const bool depth = bool(image.aspect_mask & vk::ImageAspectFlagBits::eDepth);
    const auto device = instance.GetDevice();
    const auto view = Check(device.createImageView({
        .image = image.GetImage(),
        .viewType = vk::ImageViewType::e2D,
        .format = image.info.pixel_format,
        .subresourceRange = {depth ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor,
                             0, 1, 0, 1},
    }));
    scheduler.DeferOperation([device, view] { device.destroyImageView(view); });
    out = {image.GetImage(), view, image.info.size.width, image.info.size.height};
    return true;
}

bool RayTracing::BindMask(VideoCore::ImageId id, const SceneInputs& inputs, const Bound& depth,
                          Bound& out) {
    const VideoCore::Image* image = id ? texture_cache.TryGetImage(id, 0) : nullptr;
    if (!image || !image->backing || !image->backing->image.image ||
        bool(image->aspect_mask & vk::ImageAspectFlagBits::eDepth) ||
        image->info.props.is_depth || image->info.type != AmdGpu::ImageType::Color2D ||
        image->info.size.width != depth.width || image->info.size.height != depth.height) {
        return false;
    }
    if (inputs.reduced && (!inputs.scene_targets || !inputs.scene_targets->ProxyCurrent(*image))) {
        return false;
    }
    Bound bound{};
    if (!BindScene(id, inputs, vk::AccessFlagBits2::eShaderRead, bound) || !bound.image ||
        !bound.view || bound.width != depth.width || bound.height != depth.height) {
        return false;
    }
    out = bound;
    return true;
}

bool RayTracing::BindEmissive(const SceneInputs& inputs, u32 width, u32 height, Bound& out) {
    if (!emissive_target) {
        return false;
    }
    const auto format = texture_cache.GetImage(emissive_target).info.pixel_format;
    if (format != vk::Format::eB10G11R11UfloatPack32 && format != vk::Format::eR16G16B16A16Sfloat) {
        return false;
    }
    Bound bound{};
    if (!BindScene(emissive_target, inputs, vk::AccessFlagBits2::eShaderRead, bound) ||
        bound.width != width || bound.height != height) {
        return false;
    }
    out = bound;
    return true;
}

bool RayTracing::RecordGi(const SceneInputs& inputs) {
    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;
    const auto skip = [&](const char* why) {
        ++gi_skipped;
        static u64 printed = 0;
        if (printed < 8 || gi_skipped % 600 == 0) {
            ++printed;
            Log("Ray tracing: GI skipped (%s) frame %llu, colour %d depth %d camera %d proj %g %g %g, "
                "composite frame %llu\n",
                why, (unsigned long long)frame, bool(scene_color), bool(inputs.depth),
                int(inputs.camera_valid), inputs.projection[0], inputs.projection[1],
                inputs.projection[3], (unsigned long long)gi_composite_frame);
        }
        history_valid = false;
    };
    const bool camera = inputs.camera_valid && inputs.projection[0] != 0.0f &&
                        inputs.projection[1] != 0.0f && inputs.projection[3] != 0.0f;
    if (!scene_color || !inputs.depth || !camera) {
        skip("no scene colour, depth or camera this frame");
        return false;
    }
    if (instances.empty()) {
        skip("no G-buffer geometry this frame");
        return false;
    }
    {
        const auto& color = texture_cache.GetImage(scene_color);
        const auto& depth = texture_cache.GetImage(inputs.depth);
        if (color.info.pixel_format != vk::Format::eR16G16B16A16Sfloat ||
            !(color.usage_flags & vk::ImageUsageFlagBits::eStorage) ||
            color.info.size.width != depth.info.size.width ||
            color.info.size.height != depth.info.size.height) {
            skip("scene colour is not a storage RGBA16F image of the depth size");
            return false;
        }
    }
    if (!Build(inputs)) {
        skip("acceleration structure not built");
        return false;
    }
    if (auto* profiler = GpuProfiler::Get()) {
        profiler->Mark(0xB1A5'0002ull, [] { return std::string{"ray tracing GI"}; });
    }
    scheduler.EndRendering();
    u32 bind_width = texture_cache.GetImage(scene_color).info.size.width;
    u32 bind_height = texture_cache.GetImage(scene_color).info.size.height;
    if (inputs.reduced && inputs.scene_targets) {
        const auto size = inputs.scene_targets->Size();
        bind_width = size.width;
        bind_height = size.height;
    }
    if (gi_width != bind_width || gi_height != bind_height) {
        if (!CreateGiImages(bind_width, bind_height)) {
            skip("GI image allocation failed");
            return false;
        }
    }
    Bound color{}, depth{}, albedo{}, normal{}, debug{};
    if (!BindScene(scene_color, inputs, Access::eShaderRead | Access::eShaderWrite, color) ||
        !BindScene(inputs.depth, inputs, Access::eShaderRead, depth) ||
        color.width != depth.width || color.height != depth.height) {
        runtime.FlushBarriers();
        skip("scene colour or depth not bindable at one size");
        return false;
    }
    int albedo_index = albedo_target;
    if (albedo_index < 0) {
        for (u32 i = 0; i < gbuffer_count; ++i) {
            if (gbuffer_targets[i] && IsSrgb(texture_cache.GetImage(gbuffer_targets[i]).info.pixel_format)) {
                albedo_index = int(i);
                break;
            }
        }
    }
    const auto bind_target = [&](int index, Bound& out) {
        if (index < 0 || u32(index) >= gbuffer_count || !gbuffer_targets[index]) {
            return false;
        }
        Bound bound{};
        if (!BindScene(gbuffer_targets[index], inputs, Access::eShaderRead, bound) ||
            bound.width != color.width || bound.height != color.height) {
            return false;
        }
        out = bound;
        return true;
    };
    const bool albedo_valid = bind_target(albedo_index, albedo);
    const bool normal_valid =
        normal_source.target != 0xFF && bind_target(int(normal_source.target), normal);
    const bool debug_valid = debug_target >= 0 && bind_target(debug_target, debug);
    const auto bind_rgba8 = [&](int index, Bound& out) {
        return index >= 0 && u32(index) < gbuffer_count && gbuffer_targets[index] &&
               texture_cache.GetImage(gbuffer_targets[index]).info.pixel_format ==
                   vk::Format::eR8G8B8A8Unorm &&
               bind_target(index, out);
    };
    Bound g3{}, g1{};
    const bool g3_valid = bind_rgba8(shadow_spec_target, g3);
    const bool g1_valid = bind_rgba8(direct_g1_target, g1);
    Bound emissive{};
    const bool emissive_valid = BindEmissive(inputs, color.width, color.height, emissive);
    runtime.FlushBarriers();
    static bool sources_printed = false;
    if (!sources_printed) {
        sources_printed = true;
        Log("Ray tracing: GI albedo target %d (%s), normal %s, emissive target %d (%s), %ux%u%s\n",
            albedo_index, albedo_valid ? "bound" : "not bound: white",
            normal_valid ? "G-buffer" : "depth", emissive_target_index,
            emissive_valid ? "bound" : "not bound: luma fallback", color.width, color.height,
            inputs.reduced ? " reduced proxies" : "");
    }

    if (gi_width != color.width || gi_height != color.height) {
        skip("GI image size does not match the bound scene");
        return false;
    }
    const bool cut = !history_valid || !history_prev_frame_ok ||
                     Rt::CameraCut(previous_inv_view, inputs.inv_view, 3.0f, 0.9f);
    if (cut) {
        ++gi_resets;
        sharc_cleared = false;
    }
    const u32 in = history_index, out = history_index ^ 1;
    const Rt::Affine view = Rt::InverseRigid(inputs.inv_view);
    const Rt::Affine reproject = Rt::Reprojection(previous_inv_view, inputs.inv_view);
    u32 flags = cut ? Rt::GiFlagReset : 0;
    if (gi_debug == 4) {
        flags |= Rt::GiFlagNormalDebug;
    }
    if (SkyDomeLive()) {
        flags |= Rt::GiFlagSkyDome;
    }
    const u32 slot = u32(frame % Slots);
    u32 bounce_flags = (g3_valid ? Rt::BounceFlagG3 : 0u) | (g1_valid ? Rt::BounceFlagG1 : 0u) |
                       (albedo_valid ? Rt::BounceFlagAlbedo : 0u) |
                       (emissive_valid ? Rt::BounceFlagEmissive : 0u) |
                       (portal_instances ? Rt::BounceFlagPortals : 0u) |
                       (water_on ? Rt::BounceFlagWater : 0u);
    u32 light_count = 0;
    u32 set_count = 0;
    GpuSets gpu_sets{};
    {
        const u32 was = gi_bounces_used;
        gi_bounces_used = Rt::AdaptiveStep(adaptive, adaptive_frame_ms, adaptive_ms, gi_bounces);
        adaptive_frame_ms = 0.0;
        if (was != gi_bounces_used) {
            Log("Ray tracing adaptive: GI bounces %u -> %u (RT %.2f ms smoothed, budget %.1f ms, "
                "BB_RT_ADAPTIVE_MS)\n",
                was, gi_bounces_used, adaptive.ema_ms, double(adaptive_ms));
        }
    }
    if (gi_bounces > 0 && gi_lights_ring.mapped) {
        light_count = WriteGiLights(slot, view);
        bounce_flags |= Rt::BounceFlagLights;
        if (g3_valid) {
            gpu_sets = ObtainGpuLightSets(Rt::GiLightSetsMaxAge);
        }
        if (gpu_sets.buffer) {
            set_count = gpu_sets.bytes / Rt::DirLightSetStride;
            bounce_flags |= Rt::BounceFlagDirSets;
            ++gi_sets_gpu_frames;
        } else if (g3_valid && direct_set_count > 0 &&
                   direct_sets.size() * 4 >= size_t(direct_set_count) * Rt::DirLightSetStride) {
            set_count = direct_set_count;
            std::memcpy(gi_sets_ring.mapped + u64(slot) * gi_sets_ring.slot_bytes,
                        direct_sets.data(), size_t(set_count) * Rt::DirLightSetStride);
            bounce_flags |= Rt::BounceFlagDirSets;
        }
    }
    LogSetSource("gi", gpu_sets, set_count, light_count);
    const bool fog_bound = gi_fog && !(fog_enabled && !fog_stock) && gi_fog_frame == frame &&
                           gi_fog_bytes >= 192;
    if (gi_sharc && gi_bounces_used > 0 && sharc_pipeline && !sharc_failed && !sharc_grid.buffer) {
        sharc_grid.size = Rt::SharcBytes;
        if (CreateRawBuffer(instance, sharc_grid.size,
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            false, 16, sharc_grid.buffer, sharc_grid.allocation, nullptr,
                            sharc_grid.address)) {
            sharc_cleared = false;
            Log("Ray tracing: radiance cache on (BB_RT_SHARC, live): %u slots, %llu MiB, first GI "
                "hit reads it, 1 in %u traced paths updates it\n",
                Rt::SharcSlots, (unsigned long long)(Rt::SharcBytes >> 20), Rt::SharcUpdateMask + 1);
        } else {
            sharc_grid = {};
            sharc_failed = true;
            Log("Ray tracing: radiance cache off (BB_RT_SHARC: buffer failed)\n");
        }
    }
    const bool sharc_live =
        gi_sharc && gi_bounces_used > 0 && sharc_pipeline && sharc_grid.buffer;
    const Rt::GiParams params{
        .proj = inputs.projection,
        .prev_proj = previous_projection,
        .reproject = Rt::Rows(reproject),
        .view_rows = Rt::Rows(view),
        .size_jitter = {float(color.width), float(color.height), inputs.jitter[0], inputs.jitter[1]},
        .prev_jitter_sky_clamp = {previous_jitter[0], previous_jitter[1], gi_sky, gi_clamp},
        .tlas_frame_flags = {u32(tlas_address), u32(tlas_address >> 32), u32(frame), flags},
        .cfg = {gi_spp, gi_max_history,
                (normal_valid ? normal_source.mode : Rt::NormalFromDepth) |
                    normal_source.swizzle << 8 | normal_source.components << 16,
                gi_debug},
        .strength_bias = {gi_strength, gi_bias, 0.0f, gi_fill_sky},
        .bounce = {gi_bounces_used, gi_bounce_rays, light_count, set_count},
        .bounce_cfg = {Rt::SkySlot(capacity.records), bounce_flags,
                       fog_bound ? gi_fog_bytes / 16u : 0u, 0u},
        .bounce_misc = {gi_emissive_luma, shadow_gap,
                        float(Rt::GiTraceCfg(restir_gi_enabled ? gi_sparse : 1u,
                                             gi_last_bounce_cache, gi_tmax_m)),
                        float(gi_frames & 0xFFFFu)},
        .shadow_color = {shadow_color[0], shadow_color[1], shadow_color[2], moon_cone_cos},
        .sharc = {u32(sharc_grid.address), u32(sharc_grid.address >> 32), sharc_live ? 1u : 0u, 0u},
        .far_fade = Rt::FarFadeParams(far_start, far_band),
    };
    std::memcpy(params_ring.mapped + u64(slot) * params_ring.slot_bytes, &params, sizeof(params));

    const auto cmdbuf = scheduler.CommandBuffer();
    TimeBegin(cmdbuf, 1);
    if (gpu_sets.buffer) {
        Barrier(cmdbuf, Stage::eTransfer | Stage::eComputeShader,
                Access::eTransferWrite | Access::eShaderWrite, Stage::eComputeShader,
                Access::eShaderRead);
    }
    const auto all = Stage::eAllCommands;
    const auto aux_barrier = [&](const OwnImage& own) {
        return ImageBarrier(vk::Image(own.image), vk::ImageLayout::eUndefined, all,
                            Access::eShaderRead, Stage::eComputeShader, Access::eShaderWrite);
    };
    const std::array<vk::ImageMemoryBarrier2, 9> to_general = {{
        aux_barrier(aux_albedo),
        aux_barrier(aux_specular),
        aux_barrier(aux_normal),
        aux_barrier(aux_hit),
        ImageBarrier(vk::Image(history[in].image),
                     history[in].written ? vk::ImageLayout::eGeneral : vk::ImageLayout::eUndefined,
                     all, Access::eShaderWrite, Stage::eComputeShader, Access::eShaderRead),
        ImageBarrier(vk::Image(history_z[in].image),
                     history_z[in].written ? vk::ImageLayout::eGeneral : vk::ImageLayout::eUndefined,
                     all, Access::eShaderWrite, Stage::eComputeShader, Access::eShaderRead),
        ImageBarrier(vk::Image(history[out].image), vk::ImageLayout::eUndefined, all,
                     Access::eShaderRead, Stage::eComputeShader, Access::eShaderWrite),
        ImageBarrier(vk::Image(history_z[out].image), vk::ImageLayout::eUndefined, all,
                     Access::eShaderRead, Stage::eComputeShader, Access::eShaderWrite),
        ImageBarrier(vk::Image(raw.image), vk::ImageLayout::eUndefined, all, Access::eShaderRead,
                     Stage::eComputeShader, Access::eShaderWrite),
    }};
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = u32(to_general.size()),
                             .pImageMemoryBarriers = to_general.data()});
    if (!material_cleared || material_resets.size() > 256) {
        const bool keep_sky = material_cleared && !cut;
        material_cleared = true;
        material_resets.clear();
        const u64 sky_begin = u64(Rt::SkySlot(capacity.records)) * 16;
        const u64 emissive_begin = u64(Rt::EmissiveSlot(capacity.records, 0)) * 16;
        const u64 probe_begin = u64(Rt::SkyProbeSlot(capacity.records, 0)) * 16;
        for (const vk::Buffer target : {vk::Buffer(material.buffer), vk::Buffer(material_accum.buffer)}) {
            if (!keep_sky) {
                cmdbuf.fillBuffer(target, 0, material.size, 0u);
                continue;
            }
            cmdbuf.fillBuffer(target, 0, sky_begin, 0u);
            cmdbuf.fillBuffer(target, emissive_begin, probe_begin - emissive_begin, 0u);
        }
    } else {
        for (const u32 id : material_resets) {
            for (const u32 index : {id, Rt::EmissiveSlot(capacity.records, id)}) {
                cmdbuf.fillBuffer(material.buffer, u64(index) * 16, 16, 0u);
                cmdbuf.fillBuffer(material_accum.buffer, u64(index) * 16, 16, 0u);
            }
        }
        material_resets.clear();
    }
    if (sharc_live && !sharc_cleared) {
        cmdbuf.fillBuffer(sharc_grid.buffer, 0, sharc_grid.size, 0u);
        sharc_cleared = true;
    }
    Barrier(cmdbuf, Stage::eTransfer, Access::eTransferWrite, Stage::eComputeShader,
            Access::eShaderRead | Access::eShaderWrite);
    if (sharc_live) {
        const SharcPush sharc_push{
            .grid = sharc_grid.address, .slots = Rt::SharcSlots, .frame = u32(frame)};
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *sharc_pipeline);
        cmdbuf.pushConstants(*sharc_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
                             sizeof(sharc_push), &sharc_push);
        cmdbuf.dispatch(Rt::SharcGroups(), 1, 1);
        Barrier(cmdbuf, Stage::eComputeShader, Access::eShaderWrite | Access::eShaderRead,
                Stage::eComputeShader, Access::eShaderRead | Access::eShaderWrite);
    }

    const auto sampled = [](vk::ImageView view) {
        return vk::DescriptorImageInfo{.imageView = view, .imageLayout = vk::ImageLayout::eGeneral};
    };
    const vk::DescriptorImageInfo depth_info = sampled(depth.view);
    const vk::DescriptorImageInfo scene_info = sampled(color.view);
    const vk::DescriptorImageInfo normal_info = sampled(normal_valid ? normal.view : depth.view);
    const vk::DescriptorImageInfo history_in_info = sampled(*history[in].view);
    const vk::DescriptorImageInfo history_z_in_info = sampled(*history_z[in].view);
    const vk::DescriptorImageInfo history_out_info = sampled(*history[out].view);
    const vk::DescriptorImageInfo history_z_out_info = sampled(*history_z[out].view);
    const vk::DescriptorImageInfo raw_info = sampled(*raw.view);
    const vk::DescriptorBufferInfo params_info{params_ring.buffer,
                                               u64(slot) * params_ring.slot_bytes, sizeof(params)};
    const auto write = [](u32 binding, vk::DescriptorType type, const vk::DescriptorImageInfo* image,
                          const vk::DescriptorBufferInfo* buffer) {
        return vk::WriteDescriptorSet{.dstBinding = binding,
                                      .descriptorCount = 1,
                                      .descriptorType = type,
                                      .pImageInfo = image,
                                      .pBufferInfo = buffer};
    };
    using Type = vk::DescriptorType;
    const vk::DescriptorBufferInfo material_info{material.buffer, 0, material.size};
    const vk::DescriptorBufferInfo accum_info{material_accum.buffer, 0, material_accum.size};
    const vk::DescriptorBufferInfo surfaces_info{surfaces_ring.buffer,
                                                 u64(built_slot) * surfaces_ring.slot_bytes,
                                                 surfaces_ring.slot_bytes};
    const vk::DescriptorBufferInfo uv_pool_info{uv_buffer.buffer ? uv_buffer.buffer : soup.buffer, 0,
                                                uv_buffer.buffer ? uv_buffer.size : soup.size};
    const vk::DescriptorImageInfo albedo_info = sampled(albedo_valid ? albedo.view : color.view);
    const vk::DescriptorImageInfo g3_info = sampled(g3_valid ? g3.view : depth.view);
    const vk::DescriptorImageInfo g1_info = sampled(g1_valid ? g1.view : depth.view);
    const vk::DescriptorImageInfo emissive_info = sampled(emissive_valid ? emissive.view : depth.view);
    const vk::DescriptorImageInfo aux_albedo_info = sampled(*aux_albedo.view);
    const vk::DescriptorImageInfo aux_spec_info = sampled(*aux_specular.view);
    const vk::DescriptorImageInfo aux_normal_info = sampled(*aux_normal.view);
    const vk::DescriptorImageInfo aux_hit_info = sampled(*aux_hit.view);
    const bool lights_bound = gi_lights_ring.buffer && gi_sets_ring.buffer;
    const vk::DescriptorBufferInfo lights_info{lights_bound ? gi_lights_ring.buffer : material.buffer,
                                               lights_bound ? u64(slot) * gi_lights_ring.slot_bytes : 0,
                                               lights_bound ? gi_lights_ring.slot_bytes : material.size};
    const vk::DescriptorBufferInfo sets_info =
        gpu_sets.buffer
            ? vk::DescriptorBufferInfo{gpu_sets.buffer, gpu_sets.offset, gpu_sets.bytes}
            : vk::DescriptorBufferInfo{lights_bound ? gi_sets_ring.buffer : material.buffer,
                                       lights_bound ? u64(slot) * gi_sets_ring.slot_bytes : 0,
                                       lights_bound ? gi_sets_ring.slot_bytes : material.size};
    const vk::DescriptorBufferInfo fog_info{fog_bound ? gi_fog_ring.buffer : material.buffer,
                                            fog_bound ? u64(slot) * gi_fog_ring.slot_bytes : 0,
                                            fog_bound ? gi_fog_bytes : material.size};
    const std::array<vk::WriteDescriptorSet, 23> gi_writes = {{
        write(0, Type::eSampledImage, &depth_info, nullptr),
        write(1, Type::eSampledImage, &scene_info, nullptr),
        write(2, Type::eSampledImage, &normal_info, nullptr),
        write(3, Type::eSampledImage, &history_in_info, nullptr),
        write(4, Type::eSampledImage, &history_z_in_info, nullptr),
        write(5, Type::eStorageImage, &history_out_info, nullptr),
        write(6, Type::eStorageImage, &history_z_out_info, nullptr),
        write(7, Type::eStorageImage, &raw_info, nullptr),
        write(8, Type::eUniformBuffer, nullptr, &params_info),
        write(9, Type::eStorageBuffer, nullptr, &material_info),
        write(10, Type::eStorageBuffer, nullptr, &accum_info),
        write(11, Type::eStorageBuffer, nullptr, &surfaces_info),
        write(12, Type::eStorageBuffer, nullptr, &uv_pool_info),
        write(13, Type::eSampledImage, &g3_info, nullptr),
        write(14, Type::eSampledImage, &g1_info, nullptr),
        write(15, Type::eSampledImage, &albedo_info, nullptr),
        write(16, Type::eStorageBuffer, nullptr, &lights_info),
        write(17, Type::eStorageBuffer, nullptr, &sets_info),
        write(18, Type::eStorageImage, &aux_albedo_info, nullptr),
        write(19, Type::eStorageImage, &aux_spec_info, nullptr),
        write(20, Type::eStorageImage, &aux_normal_info, nullptr),
        write(21, Type::eStorageImage, &aux_hit_info, nullptr),
        write(27, Type::eSampledImage, &emissive_info, nullptr),
    }};
    if (g3_valid && (bounce_flags & Rt::BounceFlagDirSets) != 0u) {
        RecordWorldSet(cmdbuf, depth.view, g3.view, sets_info, set_count, depth.width,
                       depth.height, params_ring.buffer, u64(slot) * params_ring.slot_bytes);
    }
    const u32 groups_x = (color.width + 7) / 8, groups_y = (color.height + 7) / 8;
    if (restir_gi_enabled) {
        RecordRestirGi(inputs, depth, slot, flags, gi_writes, groups_x, groups_y);
    } else {
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *gi_pipeline);
        cmdbuf.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *gi_pipeline_layout, 1,
                                  SurfaceSet(cmdbuf, built_slot), {});
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *gi_pipeline_layout, 0,
                                    gi_writes);
        CrumbBegin(cmdbuf, Rt::Crumb::Gi);
        cmdbuf.dispatch(groups_x, groups_y, 1);
        CrumbEnd(cmdbuf, Rt::Crumb::Gi);
    }
    // Same TLAS, params and descriptors as the GI pass; it only writes its own batch buffer.
    RecordFillVis(cmdbuf, gi_writes, view, slot);
    // Same again; it writes only the layer grid and the moon slot of the material buffer.
    RecordSkyLayer(cmdbuf, gi_writes);

    Barrier(cmdbuf, Stage::eComputeShader, Access::eShaderWrite | Access::eShaderRead,
            Stage::eComputeShader, Access::eShaderRead | Access::eShaderWrite);

    const vk::DescriptorImageInfo debug_info = sampled(debug_valid ? debug.view : color.view);
    // See-through glass: the reflection pass's diffuse keep factor, only when it wrote it this
    // frame (raw stands in, unread, otherwise).
    const bool keep_valid = glass_keep.view && glass_keep_frame == frame;
    const vk::DescriptorImageInfo keep_info = sampled(keep_valid ? *glass_keep.view : *raw.view);
    const vk::DescriptorBufferInfo halo_info{halo_ring.buffer, u64(slot) * halo_ring.slot_bytes,
                                             HaloSlotBytes};
    const std::array<vk::WriteDescriptorSet, 13> apply_writes = {{
        write(0, Type::eStorageImage, &scene_info, nullptr),
        write(1, Type::eSampledImage, &albedo_info, nullptr),
        write(2, Type::eSampledImage, &history_out_info, nullptr),
        write(3, Type::eSampledImage, &raw_info, nullptr),
        write(4, Type::eSampledImage, &debug_info, nullptr),
        write(5, Type::eStorageBuffer, nullptr, &material_info),
        write(6, Type::eStorageBuffer, nullptr, &accum_info),
        write(7, Type::eSampledImage, &depth_info, nullptr),
        write(8, Type::eSampledImage, &g3_info, nullptr),
        write(9, Type::eUniformBuffer, nullptr, &params_info),
        write(10, Type::eStorageBuffer, nullptr, &fog_info),
        write(11, Type::eSampledImage, &keep_info, nullptr),
        write(12, Type::eStorageBuffer, nullptr, &halo_info),
    }};
    u32 apply_mode = gi_debug == Rt::GiDebugSource ? 1 : gi_debug;
    if (gi_debug == 27) {
        apply_mode = 8;
    } else if (gi_debug == 9 || gi_debug == 10 || gi_debug == 6) {
        apply_mode = 1;
    } else if (gi_debug >= 20) {
        apply_mode = debug_valid ? 7 : 0;
    } else if (gi_debug >= 11) {
        apply_mode = debug_valid ? 6 : 0;
    }
    const ApplyConstants apply_push{
        .width = float(color.width),
        .height = float(color.height),
        .strength = gi_strength,
        .mode = apply_mode,
        .max_history = float(gi_max_history),
        .albedo_valid = albedo_valid ? 1u : 0u,
        .records = Rt::MaterialSlots(capacity.records) - 1,
        .denoise = gi_denoise,
        .debug_threshold = gi_debug_threshold,
        .glass_keep = keep_valid ? (glass_debug == 3 ? 2u : 1u) : 0u,
        .halos = halo_count,
    };
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *apply_pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *apply_pipeline_layout, 0,
                                apply_writes);
    cmdbuf.pushConstants(*apply_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
                         sizeof(apply_push), &apply_push);
    CrumbBegin(cmdbuf, Rt::Crumb::GiApply);
    cmdbuf.dispatch(groups_x, groups_y, 1);
    CrumbEnd(cmdbuf, Rt::Crumb::GiApply);
    Barrier(cmdbuf, Stage::eComputeShader, Access::eShaderWrite, all,
            Access::eShaderRead | Access::eShaderWrite | Access::eColorAttachmentRead |
                Access::eColorAttachmentWrite | Access::eTransferRead);

    TimeEnd(cmdbuf, 1);
    history[out].written = history_z[out].written = raw.written = true;
    aux_albedo.written = aux_specular.written = aux_normal.written = aux_hit.written = true;
    aux_frame = frame;
    history_index = out;
    history_valid = true;
    history_prev_frame_ok = true;
    previous_inv_view = inputs.inv_view;
    previous_projection = inputs.projection;
    previous_jitter = inputs.jitter;
    ++gi_frames;
    return true;
}

void RayTracing::RecordRestirGi(const SceneInputs& inputs, const Bound& depth, u32 slot, u32 flags,
                                std::span<const vk::WriteDescriptorSet> gi_writes, u32 groups_x,
                                u32 groups_y) {
    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;
    const auto cmdbuf = scheduler.CommandBuffer();
    const auto all = Stage::eAllCommands;
    std::array<vk::ImageMemoryBarrier2, 4 * Rt::RestirGiSets> to_general{};
    for (u32 s = 0; s < Rt::RestirGiSets; ++s) {
        for (u32 i = 0; i < 4; ++i) {
            const OwnImage& own = restir_gi_sets[s][i];
            to_general[s * 4 + i] = ImageBarrier(
                vk::Image(own.image),
                own.written ? vk::ImageLayout::eGeneral : vk::ImageLayout::eUndefined, all,
                Access::eShaderRead | Access::eShaderWrite, Stage::eComputeShader,
                Access::eShaderRead | Access::eShaderWrite);
        }
    }
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = u32(to_general.size()),
                             .pImageMemoryBarriers = to_general.data()});
    const auto sampled = [](vk::ImageView view) {
        return vk::DescriptorImageInfo{.imageView = view, .imageLayout = vk::ImageLayout::eGeneral};
    };
    const auto write = [](u32 binding, vk::DescriptorType type, const vk::DescriptorImageInfo* image,
                          const vk::DescriptorBufferInfo* buffer) {
        return vk::WriteDescriptorSet{.dstBinding = binding,
                                      .descriptorCount = 1,
                                      .descriptorType = type,
                                      .pImageInfo = image,
                                      .pBufferInfo = buffer};
    };
    using Type = vk::DescriptorType;
    const auto compute_barrier = [&] {
        Barrier(cmdbuf, Stage::eComputeShader, Access::eShaderWrite | Access::eShaderRead,
                Stage::eComputeShader, Access::eShaderRead | Access::eShaderWrite);
    };
    std::array<std::array<vk::DescriptorImageInfo, 4>, Rt::RestirGiSets> set_infos{};
    for (u32 s = 0; s < Rt::RestirGiSets; ++s) {
        for (u32 i = 0; i < 4; ++i) {
            set_infos[s][i] = sampled(*restir_gi_sets[s][i].view);
        }
    }
    const u32 hist = restir_gi_history;
    const u32 init = Rt::RestirGiInitSet(hist);
    const u32 temporal = Rt::RestirGiTemporalSet(hist);
    const u32 final_set = Rt::RestirGiFinalSet(hist, restir_gi_spatial);

    std::vector<vk::WriteDescriptorSet> init_writes(gi_writes.begin(), gi_writes.end());
    for (u32 i = 0; i < 4; ++i) {
        init_writes.push_back(write(22 + i, Type::eStorageImage, &set_infos[init][i], nullptr));
    }
    if (gi_rt_pipeline) {
        const auto rt_bind = vk::PipelineBindPoint::eRayTracingKHR;
        Barrier(cmdbuf, all, Access::eMemoryWrite, Stage::eRayTracingShaderKHR,
                Access::eShaderRead | Access::eShaderWrite | Access::eAccelerationStructureReadKHR);
        cmdbuf.bindPipeline(rt_bind, *restir_gi_rt_pipelines[gi_rt_ser ? 1 : 0]);
        cmdbuf.bindDescriptorSets(rt_bind, *restir_gi_rt_pipeline_layout, 1,
                                  SurfaceSet(cmdbuf, built_slot), {});
        cmdbuf.pushDescriptorSetKHR(rt_bind, *restir_gi_rt_pipeline_layout, 0, init_writes);
        const auto launch = Rt::GiInitLaunch(gi_width, gi_height, gi_sparse);
        const u64 base = gi_sbt_buffer.address + (gi_rt_ser ? u64(gi_sbt.size) : 0u);
        const vk::StridedDeviceAddressRegionKHR raygen{base + gi_sbt.raygen, gi_sbt.stride,
                                                       gi_sbt.stride};
        const vk::StridedDeviceAddressRegionKHR miss{base + gi_sbt.miss, gi_sbt.stride,
                                                     gi_sbt.stride};
        const vk::StridedDeviceAddressRegionKHR hit{base + gi_sbt.hit, gi_sbt.stride,
                                                    gi_sbt.stride};
        CrumbBegin(cmdbuf, Rt::Crumb::RestirGiInit);
        cmdbuf.traceRaysKHR(raygen, miss, hit, {}, launch[0], launch[1], 1);
        CrumbEnd(cmdbuf, Rt::Crumb::RestirGiInit);
        Barrier(cmdbuf, Stage::eRayTracingShaderKHR, Access::eShaderWrite | Access::eShaderRead,
                Stage::eComputeShader, Access::eShaderRead | Access::eShaderWrite);
    } else {
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *restir_gi_init_pipeline);
        cmdbuf.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *restir_gi_init_pipeline_layout,
                                  1, SurfaceSet(cmdbuf, built_slot), {});
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute,
                                    *restir_gi_init_pipeline_layout, 0, init_writes);
        const auto init_groups = Rt::GiInitGroups(gi_width, gi_height, gi_sparse);
        CrumbBegin(cmdbuf, Rt::Crumb::RestirGiInit);
        cmdbuf.dispatch(init_groups[0], init_groups[1], 1);
        CrumbEnd(cmdbuf, Rt::Crumb::RestirGiInit);
        compute_barrier();
    }

    if (auto* profiler = GpuProfiler::Get()) {
        profiler->Mark(0xB1A5'0008ull, [] { return std::string{"ray tracing restir gi"}; });
    }
    const Rt::Affine reproject = Rt::Reprojection(previous_inv_view, inputs.inv_view);
    const Rt::Affine unproject = Rt::Reprojection(inputs.inv_view, previous_inv_view);
    const u32 restir_flags = (flags & Rt::GiFlagReset) ? Rt::RestirGiFlagReset : 0u;
    const auto record_pass = [&](vk::Pipeline pipeline, vk::PipelineLayout layout, u32 in,
                                 u32 out, u32 pass) {
        const Rt::RestirGiParams params{
            .proj = inputs.projection,
            .prev_proj = previous_projection,
            .reproject = Rt::Rows(reproject),
            .unproject = Rt::Rows(unproject),
            .size_jitter = {float(gi_width), float(gi_height), inputs.jitter[0], inputs.jitter[1]},
            .prev_jitter_bias_gap = {previous_jitter[0], previous_jitter[1], gi_bias, shadow_gap},
            .tlas_frame_flags = {u32(tlas_address), u32(tlas_address >> 32), u32(frame),
                                 restir_flags},
            .cfg = {restir_gi_m_clamp, restir_gi_max_age, restir_gi_neighbours, pass},
            .misc = {restir_gi_radius, restir_gi_jacobian, 0.0f, 0.0f},
        };
        const u64 offset = u64(slot) * restir_gi_params_ring.slot_bytes + u64(pass) * ParamsSlotBytes;
        std::memcpy(restir_gi_params_ring.mapped + offset, &params, sizeof(params));
        const vk::DescriptorBufferInfo params_info{restir_gi_params_ring.buffer, offset,
                                                   sizeof(params)};
        const vk::DescriptorImageInfo depth_info = sampled(depth.view);
        const vk::DescriptorImageInfo normal_info = sampled(*aux_normal.view);
        const std::array<vk::WriteDescriptorSet, 17> writes = {{
            write(0, Type::eSampledImage, &depth_info, nullptr),
            write(1, Type::eSampledImage, &normal_info, nullptr),
            write(2, Type::eUniformBuffer, nullptr, &params_info),
            write(3, Type::eSampledImage, &set_infos[in][0], nullptr),
            write(4, Type::eSampledImage, &set_infos[in][1], nullptr),
            write(5, Type::eSampledImage, &set_infos[in][2], nullptr),
            write(6, Type::eSampledImage, &set_infos[in][3], nullptr),
            write(7, Type::eSampledImage, &set_infos[init][0], nullptr),
            write(8, Type::eSampledImage, &set_infos[init][1], nullptr),
            write(9, Type::eSampledImage, &set_infos[init][2], nullptr),
            write(10, Type::eSampledImage, &set_infos[init][3], nullptr),
            write(11, Type::eStorageImage, &set_infos[out][0], nullptr),
            write(12, Type::eStorageImage, &set_infos[out][1], nullptr),
            write(13, Type::eStorageImage, &set_infos[out][2], nullptr),
            write(14, Type::eStorageImage, &set_infos[out][3], nullptr),
            write(15, Type::eStorageBuffer, nullptr, gi_writes[11].pBufferInfo),
            write(16, Type::eStorageBuffer, nullptr, gi_writes[12].pBufferInfo),
        }};
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
        cmdbuf.bindDescriptorSets(vk::PipelineBindPoint::eCompute, layout, 1, SurfaceSet(cmdbuf, built_slot), {});
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0, writes);
        CrumbBegin(cmdbuf, Rt::Crumb::RestirGiPass);
        cmdbuf.dispatch(groups_x, groups_y, 1);
        CrumbEnd(cmdbuf, Rt::Crumb::RestirGiPass);
        compute_barrier();
    };
    record_pass(*restir_gi_temporal_pipeline, *restir_gi_temporal_pipeline_layout, hist, temporal,
                0);
    u32 in = temporal, out = init;
    for (u32 pass = 0; pass < restir_gi_spatial; ++pass) {
        record_pass(*restir_gi_spatial_pipeline, *restir_gi_spatial_pipeline_layout, in, out,
                    pass + 1);
        std::swap(in, out);
    }

    std::vector<vk::WriteDescriptorSet> resolve_writes(gi_writes.begin(), gi_writes.end());
    for (u32 i = 0; i < 4; ++i) {
        resolve_writes.push_back(
            write(22 + i, Type::eSampledImage, &set_infos[final_set][i], nullptr));
    }
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *restir_gi_resolve_pipeline);
    cmdbuf.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *restir_gi_resolve_pipeline_layout,
                              1, SurfaceSet(cmdbuf, built_slot), {});
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute,
                                *restir_gi_resolve_pipeline_layout, 0, resolve_writes);
    CrumbBegin(cmdbuf, Rt::Crumb::RestirGiResolve);
    cmdbuf.dispatch(groups_x, groups_y, 1);
    CrumbEnd(cmdbuf, Rt::Crumb::RestirGiResolve);
    if (auto* profiler = GpuProfiler::Get()) {
        profiler->Mark(0xB1A5'0002ull, [] { return std::string{"ray tracing GI"}; });
    }
    for (auto& set : restir_gi_sets) {
        for (OwnImage& own : set) {
            own.written = true;
        }
    }
    restir_gi_history = final_set;
}

void RayTracing::RecordShadows(const SceneInputs& inputs) {
    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;
    const auto skip = [&](const char* why) {
        ++shadow_skipped;
        static u64 printed = 0;
        if (printed < 8) {
            ++printed;
            Log("Ray tracing: shadows skipped (%s)\n", why);
        }
    };
    if (shadow_lights.empty() && !shadow_debug) {
        return;
    }
    const bool camera = inputs.camera_valid && inputs.projection[0] != 0.0f &&
                        inputs.projection[1] != 0.0f && inputs.projection[3] != 0.0f;
    if (!scene_color || !inputs.depth || !camera) {
        skip("no scene colour, depth or camera this frame");
        return;
    }
    if (instances.empty()) {
        skip("no G-buffer geometry this frame");
        return;
    }
    {
        const auto& color = texture_cache.GetImage(scene_color);
        const auto& depth = texture_cache.GetImage(inputs.depth);
        if (color.info.pixel_format != vk::Format::eR16G16B16A16Sfloat ||
            !(color.usage_flags & vk::ImageUsageFlagBits::eStorage) ||
            color.info.size.width != depth.info.size.width ||
            color.info.size.height != depth.info.size.height) {
            skip("scene colour is not a storage RGBA16F image of the depth size");
            return;
        }
    }
    if (!Build(inputs)) {
        skip("acceleration structure not built");
        return;
    }
    if (auto* profiler = GpuProfiler::Get()) {
        profiler->Mark(0xB1A5'0004ull, [] { return std::string{"ray tracing shadows"}; });
    }
    scheduler.EndRendering();
    Bound color{}, depth{}, albedo{}, normal{};
    if (!BindScene(scene_color, inputs, Access::eShaderRead | Access::eShaderWrite, color) ||
        !BindScene(inputs.depth, inputs, Access::eShaderRead, depth) ||
        color.width != depth.width || color.height != depth.height) {
        runtime.FlushBarriers();
        skip("scene colour or depth not bindable at one size");
        return;
    }
    int albedo_index = albedo_target;
    if (albedo_index < 0) {
        for (u32 i = 0; i < gbuffer_count; ++i) {
            if (gbuffer_targets[i] && IsSrgb(texture_cache.GetImage(gbuffer_targets[i]).info.pixel_format)) {
                albedo_index = int(i);
                break;
            }
        }
    }
    const auto bind_target = [&](int index, Bound& out) {
        if (index < 0 || u32(index) >= gbuffer_count || !gbuffer_targets[index]) {
            return false;
        }
        Bound bound{};
        if (!BindScene(gbuffer_targets[index], inputs, Access::eShaderRead, bound) ||
            bound.width != color.width || bound.height != color.height) {
            return false;
        }
        out = bound;
        return true;
    };
    const bool albedo_valid = bind_target(albedo_index, albedo);
    const bool normal_valid =
        normal_source.target != 0xFF && bind_target(int(normal_source.target), normal);
    Bound spec{};
    const bool spec_valid =
        shadow_spec_target >= 0 && u32(shadow_spec_target) < gbuffer_count &&
        gbuffer_targets[shadow_spec_target] &&
        texture_cache.GetImage(gbuffer_targets[shadow_spec_target]).info.pixel_format ==
            vk::Format::eR8G8B8A8Unorm &&
        bind_target(shadow_spec_target, spec);
    runtime.FlushBarriers();

    const Rt::Affine view = Rt::InverseRigid(inputs.inv_view);
    Rt::ShadowParams params{
        .proj = inputs.projection,
        .view_rows = Rt::Rows(view),
        .size_jitter = {float(color.width), float(color.height), inputs.jitter[0], inputs.jitter[1]},
        .tlas_count_flags = {u32(tlas_address), u32(tlas_address >> 32),
                             u32(std::min<size_t>(shadow_lights.size(), Rt::MaxShadowLights)),
                             (albedo_valid ? Rt::ShadowFlagAlbedo : 0u) |
                                 (shadow_debug ? Rt::ShadowFlagDebug : 0u) |
                                 (spec_valid ? Rt::ShadowFlagSpecular : 0u)},
        .cfg = {(normal_valid ? normal_source.mode : Rt::NormalFromDepth) |
                    normal_source.swizzle << 8 | normal_source.components << 16,
                u32(frame), 0, 0},
        .strength_bias = {shadow_strength, shadow_bias, shadow_tmin, shadow_gap},
        .lights = {},
    };
    for (u32 i = 0; i < params.tlas_count_flags[2]; ++i) {
        params.lights[i] = Rt::PackShadowLight(shadow_lights[i], view);
    }
    static u64 printed = 0;
    if (printed < 3 && params.tlas_count_flags[2]) {
        ++printed;
        const Rt::Light& l = shadow_lights[0];
        Log("Ray tracing: shadows frame %llu, %u lights (albedo %s, normal %s, specular %s); "
            "strongest world "
            "%.2f %.2f %.2f, game view %.2f %.2f %.2f, rt view %.2f %.2f %.2f, radius %.2f, "
            "colour %.2f %.2f %.2f; camera %.2f %.2f %.2f\n",
            (unsigned long long)frame, params.tlas_count_flags[2],
            albedo_valid ? "bound" : "white", normal_valid ? "G-buffer" : "depth",
            spec_valid ? "bound" : "none", l.world[0],
            l.world[1], l.world[2], l.position[0], l.position[1], l.position[2],
            params.lights[0].pos_inv_r2[0], params.lights[0].pos_inv_r2[1],
            params.lights[0].pos_inv_r2[2], l.radius, l.color[0], l.color[1], l.color[2],
            inputs.inv_view[3], inputs.inv_view[7], inputs.inv_view[11]);
    }
    const u32 slot = u32(frame % Slots);
    std::memcpy(shadow_params_ring.mapped + u64(slot) * shadow_params_ring.slot_bytes, &params,
                sizeof(params));

    const auto cmdbuf = scheduler.CommandBuffer();
    TimeBegin(cmdbuf, 5);
    const auto sampled = [](vk::ImageView view) {
        return vk::DescriptorImageInfo{.imageView = view, .imageLayout = vk::ImageLayout::eGeneral};
    };
    const vk::DescriptorImageInfo depth_info = sampled(depth.view);
    const vk::DescriptorImageInfo scene_info = sampled(color.view);
    const vk::DescriptorImageInfo albedo_info = sampled(albedo_valid ? albedo.view : color.view);
    const vk::DescriptorImageInfo normal_info = sampled(normal_valid ? normal.view : depth.view);
    const vk::DescriptorImageInfo spec_info = sampled(spec_valid ? spec.view : depth.view);
    const vk::DescriptorBufferInfo params_info{shadow_params_ring.buffer,
                                               u64(slot) * shadow_params_ring.slot_bytes,
                                               sizeof(params)};
    const auto write = [](u32 binding, vk::DescriptorType type, const vk::DescriptorImageInfo* image,
                          const vk::DescriptorBufferInfo* buffer) {
        return vk::WriteDescriptorSet{.dstBinding = binding,
                                      .descriptorCount = 1,
                                      .descriptorType = type,
                                      .pImageInfo = image,
                                      .pBufferInfo = buffer};
    };
    using Type = vk::DescriptorType;
    const std::array<vk::WriteDescriptorSet, 6> writes = {{
        write(0, Type::eSampledImage, &depth_info, nullptr),
        write(1, Type::eStorageImage, &scene_info, nullptr),
        write(2, Type::eSampledImage, &albedo_info, nullptr),
        write(3, Type::eSampledImage, &normal_info, nullptr),
        write(4, Type::eUniformBuffer, nullptr, &params_info),
        write(5, Type::eSampledImage, &spec_info, nullptr),
    }};
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *shadow_pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *shadow_pipeline_layout, 0, writes);
    CrumbBegin(cmdbuf, Rt::Crumb::Shadow);
    cmdbuf.dispatch((color.width + 7) / 8, (color.height + 7) / 8, 1);
    CrumbEnd(cmdbuf, Rt::Crumb::Shadow);
    Barrier(cmdbuf, Stage::eComputeShader, Access::eShaderWrite, Stage::eAllCommands,
            Access::eShaderRead | Access::eShaderWrite | Access::eColorAttachmentRead |
                Access::eColorAttachmentWrite | Access::eTransferRead);
    TimeEnd(cmdbuf, 5);
    ++shadow_frames;
    if (params.tlas_count_flags[2]) {
        ++shadow_light_frames;
    }
}

void RayTracing::RecordDebug(const SceneInputs& inputs) {
    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;
    static u64 printed = 0;
    const auto skip = [&](const char* why) {
        if (printed < 8) {
            ++printed;
            Log("Ray tracing: debug view skipped (%s)\n", why);
        }
    };
    if (!scene_color || inputs.projection[0] == 0.0f || inputs.projection[1] == 0.0f) {
        skip("no scene colour or camera this frame");
        return;
    }
    if (instances.empty()) {
        return;
    }
    {
        const auto& color = texture_cache.GetImage(scene_color);
        if (color.info.pixel_format != vk::Format::eR16G16B16A16Sfloat ||
            !(color.usage_flags & vk::ImageUsageFlagBits::eStorage)) {
            skip("scene colour is not a storage RGBA16F image");
            return;
        }
    }
    if (!Build(inputs)) {
        skip("acceleration structure not built");
        return;
    }
    if (auto* profiler = GpuProfiler::Get()) {
        profiler->Mark(0xB1A5'0003ull, [] { return std::string{"ray tracing debug"}; });
    }
    scheduler.EndRendering();
    Bound color{};
    if (!BindScene(scene_color, inputs, Access::eShaderRead | Access::eShaderWrite, color)) {
        runtime.FlushBarriers();
        skip("scene colour not bindable");
        return;
    }
    runtime.FlushBarriers();
    const auto cmdbuf = scheduler.CommandBuffer();
    const vk::DescriptorImageInfo color_info{.imageView = color.view,
                                             .imageLayout = vk::ImageLayout::eGeneral};
    const std::array<vk::WriteDescriptorSet, 1> debug_writes = {{
        {.dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageImage,
         .pImageInfo = &color_info},
    }};
    const DebugConstants debug_push{
        .tlas_address = tlas_address,
        .xs = inputs.projection[0],
        .ys = inputs.projection[1],
        .width = float(color.width),
        .height = float(color.height),
        .mode = debug_mode,
        .frame = u32(frame),
        .range = range,
        .ao_radius = ao_radius,
    };
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *debug_pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *debug_pipeline_layout, 0,
                                debug_writes);
    cmdbuf.pushConstants(*debug_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
                         sizeof(debug_push), &debug_push);
    CrumbBegin(cmdbuf, Rt::Crumb::Debug);
    cmdbuf.dispatch((color.width + 7) / 8, (color.height + 7) / 8, 1);
    CrumbEnd(cmdbuf, Rt::Crumb::Debug);
    Barrier(cmdbuf, Stage::eComputeShader, Access::eShaderWrite, Stage::eAllCommands,
            Access::eShaderRead | Access::eShaderWrite | Access::eColorAttachmentRead |
                Access::eColorAttachmentWrite | Access::eTransferRead);
    static bool announced = false;
    if (!announced) {
        announced = true;
        Log("Ray tracing: debug view %u drawn into the scene colour %ux%u at the post "
                    "trigger\n", debug_mode, color.width, color.height);
    }
}

void RayTracing::OnDisplayPass(const SceneInputs& inputs) {
    if (!enabled) {
        return;
    }
    {
        CpuTimer timer{hitch_log, hitch.display};
        FinishFrame(inputs);
    }
    ReportHitch();
}

u32 RayTracing::WriteGiLights(u32 slot, const Rt::Affine& view) {
    const u32 count = u32(std::min<size_t>(direct_lights.size(), Rt::MaxDirectLights));
    u8* base = gi_lights_ring.mapped + u64(slot) * gi_lights_ring.slot_bytes;
    gi_lights_scratch.resize(count);
    for (u32 i = 0; i < count; ++i) {
        gi_lights_scratch[i] = Rt::PackDirectLight(direct_lights[i], view);
        for (const Rt::DirectTrack& t : direct_tracks.list) {
            if (t.mobile && Rt::WorldDistance2(direct_lights[i], t.light) <= 1.0f) {
                gi_lights_scratch[i].wrap_r_flag[3] += 8.0f;
                break;
            }
        }
    }
    if (count) {
        std::memcpy(base, gi_lights_scratch.data(), count * sizeof(Rt::DirectLightGpu));
    }
    halo_view_scratch.resize(halo_spots.size());
    for (size_t k = 0; k < halo_spots.size(); ++k) {
        halo_view_scratch[k] = Rt::Transform(view, halo_spots[k]);
    }
    std::array<float, 8 * Rt::MaxHalos> halos{};
    halo_count = Rt::BuildHalos(gi_lights_scratch, halo_view_scratch, halo_strength, halo_radius,
                                halo_core, halo_match, halos.data());
    if (halo_ring.mapped) {
        std::memcpy(halo_ring.mapped + u64(slot) * halo_ring.slot_bytes, halos.data(),
                    halo_count * 8 * sizeof(float));
    }
    gi_grid_scratch.resize(Rt::LightGridWords);
    u32* grid = reinterpret_cast<u32*>(base + DirectLightsSlotBytes);
    if (gi_light_grid && Rt::BuildLightGrid(gi_lights_scratch, gi_grid_scratch)) {
        const u32 cells = gi_grid_scratch[4] * gi_grid_scratch[5] * gi_grid_scratch[6];
        const u32 words = Rt::LightGridHeader + cells + 1 + gi_grid_scratch[Rt::LightGridHeader + cells];
        std::memcpy(grid, gi_grid_scratch.data(), words * sizeof(u32));
        ++gi_grid_frames;
    } else {
        const std::array<u32, Rt::LightGridHeader> off{};
        std::memcpy(grid, off.data(), sizeof(off));
        gi_grid_fallbacks += gi_light_grid && count ? 1u : 0u;
    }
    // Fire clusters, then loot wisp clusters (word 3 = 1: drawn pale in water mirrors).
    const auto clusters = Rt::ClusterFlames(RecentFlames(), Rt::FlameClusterRadius);
    const auto wisps = Rt::ClusterFlames(RecentWisps(), Rt::FlameClusterRadius);
    u32* fx = grid + Rt::LightGridWords;
    const size_t fx_count = std::min(clusters.size() + wisps.size(), Rt::FlameMaxClusters);
    fx[0] = u32(fx_count);
    for (size_t k = 0; k < fx_count; ++k) {
        const bool wisp = k >= clusters.size();
        const std::array<float, 3> v =
            Rt::Transform(view, wisp ? wisps[k - clusters.size()] : clusters[k]);
        fx[4 + 4 * k] = Rt::FloatWord(v[0]);
        fx[5 + 4 * k] = Rt::FloatWord(v[1]);
        fx[6 + 4 * k] = Rt::FloatWord(v[2]);
        fx[7 + 4 * k] = wisp ? 1u : 0u;
    }
    u32* fog = fx + Rt::FlameBlockWords;
    const std::vector<Rt::FxParticle>& held = fog_frame == frame ? fog_done : fog_filling;
    const bool fresh = fog_frame == frame || fog_frame + 1 == frame;
    const size_t fog_count = fresh ? std::min(held.size(), Rt::FogMaxParticles) : 0;
    fog[0] = u32(fog_count);
    for (size_t k = 0; k < fog_count; ++k) {
        const std::array<float, 3> v = Rt::Transform(view, held[k].centre);
        fog[4 + 4 * k] = (Rt::FloatWord(v[0]) & ~0xFFu) | (held[k].rgb & 0xFFu);
        fog[5 + 4 * k] = (Rt::FloatWord(v[1]) & ~0xFFu) | ((held[k].rgb >> 8) & 0xFFu);
        fog[6 + 4 * k] = (Rt::FloatWord(v[2]) & ~0xFFu) | ((held[k].rgb >> 16) & 0xFFu);
        fog[7 + 4 * k] = (Rt::FloatWord(held[k].size) & ~0xFFu) |
                         u32(std::clamp(held[k].alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
    }
    // The halos again after the fog block, so water mirrors draw the ones the screen does.
    u32* halo_block = fog + Rt::FogBlockWords;
    halo_block[0] = halo_count;
    halo_block[1] = Rt::FloatWord(halo_strength);
    halo_block[2] = Rt::FloatWord(halo_radius);
    halo_block[3] = Rt::FloatWord(halo_core);
    std::memcpy(halo_block + 4, halos.data(), halo_count * 8 * sizeof(float));
    gi_lights_frame = frame;
    gi_lights_count = count;
    return count;
}

void RayTracing::PrintGpuLine() {
    const auto& c = cache.stats;
    if (time_enabled || compact_enabled || omm_enabled) {
        const auto avg = [&](u32 p) {
            return time_count[p] ? time_sum_ms[p] / double(time_count[p]) : 0.0;
        };
        const u32 hidden_total = ptlas_on ? ptlas.HiddenTotal() : 0u;
        Log("Ray tracing GPU ms/frame: build %.3f, GI %.3f, direct %.3f, ReSTIR %.3f, reflect "
            "%.3f, shadows %.3f, AO %.3f, fog %.3f (%llu timed frames); compaction %llu BLAS, "
            "%llu MiB saved; OMM %llu BLAS, %llu triangles, %llu over budget, skinned %llu built %llu "
            "refits; TLAS %.3f ms GPU, "
            "instance upload %.3f ms CPU, %.1f instance writes/frame, %.1f refit writes/frame, "
            "%.1f probe-hidden\n",
            avg(0), avg(1), avg(2), avg(3), avg(4), avg(5), avg(6), avg(7),
            (unsigned long long)time_count[0],
            (unsigned long long)c.compacted, (unsigned long long)(c.compacted_saved >> 20),
            (unsigned long long)c.omm_built, (unsigned long long)c.omm_triangles,
            (unsigned long long)omm_over_budget, (unsigned long long)omm_skinned_built,
            (unsigned long long)omm_skinned_refits, avg(TlasPass),
            tlas_upload_frames ? tlas_upload_ms_sum / double(tlas_upload_frames) : 0.0,
            tlas_upload_frames ? double(tlas_writes_sum) / double(tlas_upload_frames) : 0.0,
            tlas_upload_frames ? double(tlas_refit_writes_sum) / double(tlas_upload_frames) : 0.0,
            tlas_upload_frames ? double(u32(hidden_total - tlas_hidden_last)) / double(tlas_upload_frames)
                               : 0.0);
        tlas_hidden_last = hidden_total;
        tlas_upload_ms_sum = 0.0;
        tlas_upload_frames = 0;
        tlas_writes_sum = 0;
        tlas_refit_writes_sum = 0;
        time_sum_ms = {};
        time_count = {};
    }
}

void RayTracing::PrintStats(u32 triangles) {
    CpuTimer timer{hitch_log, hitch.stats};
    const auto& s = geometry.stats;
    const auto& c = cache.stats;
    Log("Ray tracing: %llu frames built, %llu empty, %llu without camera; last frame "
                "%u triangles in %u instances, %u BLAS rebuilt (%u triangles gathered, %u KiB "
                "of indices), max %u of %u per frame, max rebuilt %u; draws %llu, %llu "
                "accepted, %llu over capacity, %llu invalid, %llu pool full; cache %zu "
                "entries, %llu hits, %llu misses, %llu rebuilt, %llu evicted, %llu probe "
                "dirty, pool %llu of %llu MiB (%llu retired); GI %llu frames, %llu skipped, "
                "%llu history resets, %llu early composites\n",
                (unsigned long long)built_frames, (unsigned long long)empty_frames,
                (unsigned long long)skipped_frames, triangles, last_instances, last_rebuilt,
                geometry.Triangles(), geometry.IndexBytes() >> 10, max_triangles_seen,
                capacity.triangles, max_rebuilt_seen, (unsigned long long)s.draws,
                (unsigned long long)s.accepted, (unsigned long long)s.capacity,
                (unsigned long long)s.invalid, (unsigned long long)pool_full_draws,
                cache.Size(), (unsigned long long)c.hits, (unsigned long long)c.misses,
                (unsigned long long)c.rebuilt, (unsigned long long)c.evicted,
                (unsigned long long)c.probe_dirty, (unsigned long long)(pool.Used() >> 20),
                (unsigned long long)(pool.Size() >> 20),
                (unsigned long long)(retired_bytes >> 20), (unsigned long long)gi_frames,
                (unsigned long long)gi_skipped, (unsigned long long)gi_resets,
                (unsigned long long)gi_early_composites);
    LogLiveState();
    if (frame % gpu_line_frames == 0) {
        PrintGpuLine();
    }
    if (shadows_enabled) {
        Log("Ray tracing: shadows %llu frames (%llu with lights, %llu skipped), %zu lights "
            "this frame; light draws %llu, lights %llu, unknown buffers %llu; light capture "
            "%.3f ms/frame\n",
            (unsigned long long)shadow_frames, (unsigned long long)shadow_light_frames,
            (unsigned long long)shadow_skipped, shadow_lights.size(),
            (unsigned long long)light_draws_total, (unsigned long long)light_lights_total,
            (unsigned long long)light_unknown_buffers,
            frame > light_cpu_frame ? double(light_cpu_ns) * 1e-6 / double(frame - light_cpu_frame)
                                    : 0.0);
        light_cpu_ns = 0;
        light_cpu_frame = frame;
    }
    if (direct_enabled) {
        Log("Ray tracing: moon history %s (BB_RT_MOON_HISTORY), %llu frames, %llu resets, "
            "%llu with object motion (BB_RT_MOON_OBJECT_MOTION %s)\n",
            moon_history ? "on" : "off", (unsigned long long)moon_hist_frames,
            (unsigned long long)moon_hist_resets, (unsigned long long)moon_object_frames,
            moon_object_motion ? "on" : "off");
        Log("Ray tracing: direct %llu frames (%llu skipped, %llu with a directional set, %llu "
            "from the game's GPU buffer, %llu GI/reflection frames from it, %llu CPU set readbacks), "
            "%zu lights this frame (%zu tracked, %llu duplicate "
            "draws), %llu light draws, %llu lights parsed, %llu duplicate lights skipped before "
            "parsing, %llu light draws skipped, %llu frames kept from the game, %llu frames with depth "
            "normals, %llu smaller G-buffer layouts ignored, %llu frames with the game's moon shadow "
            "mask; light capture %.3f ms/frame; RTAO %llu frames, %llu skipped, %llu mask "
            "changes, mask SSAO mean stock %.3f ours %.3f (%llu frames); %u blended draws in "
            "the TLAS last frame, %llu total, %llu held over to the next frame, %llu over the "
            "cap of %u (BB_RT_FX_SMOKE, opacity %.2f)\n",
            (unsigned long long)direct_frames, (unsigned long long)direct_skipped,
            (unsigned long long)direct_sun_frames, (unsigned long long)direct_sets_gpu_frames,
            (unsigned long long)gi_sets_gpu_frames, (unsigned long long)sets_readback_frames,
            direct_lights.size(), direct_tracks.size(),
            (unsigned long long)direct_duplicates, (unsigned long long)light_draws_total,
            (unsigned long long)light_lights_total, (unsigned long long)light_dedup.skipped,
            (unsigned long long)light_draws_skipped, (unsigned long long)direct_pass_through,
            (unsigned long long)direct_depth_normal_frames,
            (unsigned long long)gbuffer_layouts_ignored, (unsigned long long)direct_mask_frames,
            frame > light_cpu_frame ? double(light_cpu_ns) * 1e-6 / double(frame - light_cpu_frame)
                                    : 0.0,
            (unsigned long long)ao_frames, (unsigned long long)ao_skipped,
            (unsigned long long)ao_mask_mismatch,
            ao_stats_frames ? ao_stats_stock / double(ao_stats_frames) : 1.0,
            ao_stats_frames ? ao_stats_ours / double(ao_stats_frames) : 1.0,
            (unsigned long long)ao_stats_frames, fx_blended_last,
            (unsigned long long)fx_blended_draws, (unsigned long long)fx_blended_held,
            (unsigned long long)fx_blended_capped, fx_cap, fx_smoke_opacity);
        ao_stats_stock = ao_stats_ours = 0.0;
        ao_stats_frames = 0;
        light_cpu_ns = 0;
        light_cpu_frame = frame;
    }
    if (fx_smoke && smoke_ring.buffer) {
        Log("Ray tracing: smoke %llu patch draws, %llu quads, %llu capped, %llu without uv "
            "(BB_RT_FX_SMOKE, BB_RT_FX_SMOKE_LOG)\n",
            (unsigned long long)fx_smoke_draws, (unsigned long long)fx_smoke_quads,
            (unsigned long long)fx_smoke_capped, (unsigned long long)fx_smoke_no_uv);
    }
    if (fog_enabled) {
        Log("Ray tracing: fog %llu frames (%llu skipped) (BB_RT_FX_FOG, debug %u)\n",
            (unsigned long long)fog_frames, (unsigned long long)fog_skipped, fog_debug);
    }
    if (restir_enabled) {
        Log("Ray tracing: ReSTIR %llu frames, %llu reservoir resets, %zu light ids remapped\n",
            (unsigned long long)restir_frames, (unsigned long long)restir_resets,
            restir_prev_keys.size());
    }
    if (gi_lights_ring.mapped) {
        Log("Ray tracing: GI light grid %llu passes, %llu full-list fallbacks (BB_RT_GI_LIGHT_GRID)\n",
            (unsigned long long)gi_grid_frames, (unsigned long long)gi_grid_fallbacks);
    }
    if (fill_vis_pipeline) {
        size_t measured = 0, emitters = 0;
        for (const auto& [key, e] : fill_vis) {
            measured += e.measured ? 1 : 0;
            emitters += e.emitter ? 1 : 0;
        }
        Log("Ray tracing: fill-light visibility %s: %zu lights cached (%zu measured, %zu at an "
            "emitter), %zu queued, %llu dispatches, %llu results, %u lights scaled and %u off last "
            "frame, %llu scaled over %llu frames (BB_RT_FILL_VIS, BB_RT_FILL_SKY %.2f, "
            "BB_RT_FILL_FLOOR %.2f, BB_RT_FILL_RAYS %u); %u emitters inverse square to radius x "
            "%.1f last frame (BB_RT_EMITTER_RANGE), %u lamps, %u fire pits lifted %.2f m (BB_RT_FIRE_LIFT); %u fill lights within %.1f m of an "
            "emitter count as it (BB_RT_FILL_CLUSTER)\n",
            fill_vis_on ? "on" : "off", fill_vis.size(), measured, emitters, fill_vis_queue.size(),
            (unsigned long long)fill_vis_dispatches, (unsigned long long)fill_vis_results,
            fill_vis_scaled_last, fill_vis_off_last, (unsigned long long)fill_vis_scaled,
            (unsigned long long)fill_vis_frames, fill_vis_sky_full, fill_vis_floor, fill_vis_rays,
            emitter_falloff_last, emitter_range, lamp_falloff_last, fire_lift_last, fire_lift,
            fill_vis_cluster_last, fill_vis_cluster);
    }
    if (reflect_enabled) {
        Log("Ray tracing: reflections %llu frames (%llu skipped, %llu with the game's pre-AO pair)\n",
            (unsigned long long)reflect_frames, (unsigned long long)reflect_skipped,
            (unsigned long long)reflect_pre_ao_frames);
    }
    const auto reason = [&](Rt::Rebuild r) {
        return (unsigned long long)c.reasons[size_t(r)];
    };
    Log("Ray tracing: rebuild reasons %llu miss, %llu size, %llu dirty, %llu unreadable "
                "bones, %llu skeleton changed, %llu palette changed; %llu refits (%llu anchored to "
                "their build camera, up to %.1f deg turned), %llu space "
                "evictions, %llu stale probe flags; masked %llu by the probe (last frame %llu), "
                "%llu without camera; budget %llu deferred (new draws), %llu held stale; kept %llu "
                "off-screen instances (last frame %llu: %llu triangles, %llu alpha-tested, %llu "
                "sharing a drawn texture; %llu untextured, %llu dropped by the BB_RT_KEEP_TRIS %llu "
                "/ BB_RT_KEEP_MAX_TRIS %u limits, %s), %u off-screen lights\n",
                reason(Rt::Rebuild::Miss), reason(Rt::Rebuild::Size),
                reason(Rt::Rebuild::Dirty), reason(Rt::Rebuild::Unreadable),
                reason(Rt::Rebuild::Skeleton), reason(Rt::Rebuild::Palette),
                (unsigned long long)c.refits, (unsigned long long)anchored_refits,
                double(anchor_turn_max), (unsigned long long)c.space_evictions,
                (unsigned long long)c.stale_flags, (unsigned long long)masked_instances,
                (unsigned long long)last_masked, (unsigned long long)camera_masked,
                (unsigned long long)c.deferred, (unsigned long long)c.held,
                (unsigned long long)kept_instances, (unsigned long long)last_kept,
                (unsigned long long)kept_tris_last, (unsigned long long)kept_alpha_last,
                (unsigned long long)kept_shared_tex_last, (unsigned long long)kept_untextured,
                (unsigned long long)kept_dropped_total,
                (unsigned long long)keep_rules.triangle_budget, keep_rules.max_triangles,
                keep_gi ? "GI/AO see them" : "direct shadows only", direct_tracks.kept_offscreen);
    Log("Ray tracing: alpha-tested surfaces %llu, %llu with the discard mask texture or channel, "
        "alpha-tested draws offered %llu accepted %llu (BB_RT_ALPHA_SOURCE %s, "
        "BB_RT_ALPHA_INHERIT %s); alpha-tested instances without a usable texture or UVs %llu "
        "(last frame %llu, %s, BB_RT_ALPHA_UNRESOLVED); %llu draws excluded by BB_RT_EXCLUDE\n",
        (unsigned long long)surface_alpha, (unsigned long long)surface_alpha_split,
        (unsigned long long)alpha_offered, (unsigned long long)alpha_accepted,
        Rt::AlphaSourceWanted() ? "on" : "off", Rt::AlphaInheritWanted() ? "on" : "off",
        (unsigned long long)alpha_unresolved_total, (unsigned long long)alpha_unresolved_last,
        alpha_skip_unresolved ? "left out" : "solid", (unsigned long long)excluded_draws);
    const auto top = [](const std::unordered_map<u64, u64>& map) {
        std::vector<std::pair<u64, u64>> list(map.begin(), map.end());
        std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        std::string text;
        for (size_t i = 0; i < list.size() && i < 4; ++i) {
            char part[48];
            std::snprintf(part, sizeof(part), " %016llx x%llu", (unsigned long long)list[i].first,
                          (unsigned long long)list[i].second);
            text += part;
        }
        return text.empty() ? std::string(" none") : text;
    };
    Log("Ray tracing: instance alpha/texture/mask changes vs the previous frame %llu (last frame "
        "%llu; built %llu, cached %llu, kept %llu, hidden %llu; ps%s); unresolved alpha by mode "
        "built %llu, cached %llu, kept %llu (ps%s)\n",
        (unsigned long long)flips_total, (unsigned long long)flips_last,
        (unsigned long long)flips_by_mode[0], (unsigned long long)flips_by_mode[1],
        (unsigned long long)flips_by_mode[2], (unsigned long long)flips_by_mode[3],
        top(flips_by_ps).c_str(), (unsigned long long)unresolved_by_mode[0],
        (unsigned long long)unresolved_by_mode[1], (unsigned long long)unresolved_by_mode[2],
        top(unresolved_by_ps).c_str());
    flips_total = 0;
    flips_by_mode = {};
    unresolved_by_mode = {};
    flips_by_ps.clear();
    unresolved_by_ps.clear();
    if (hitch_log) {
        Log("Ray tracing hitch: worst RT CPU %.2f ms, worst wait %.2f ms, %llu hitch frames "
            "(600 frames); %llu deferred by the rebuild budget, %llu budget refits\n",
            hitch_worst, hitch_worst_wait, (unsigned long long)hitch_frames,
            (unsigned long long)cache.stats.deferred,
            (unsigned long long)cache.stats.budget_refits);
        hitch_worst = hitch_worst_wait = 0.0;
        hitch_frames = 0;
    }
}

void RayTracing::ReportHitch() {
    if (!hitch_log) {
        hitch = {};
        return;
    }
    const Hitch& h = hitch;
    const double total = h.add + h.gi + h.debug + h.display;
    hitch_worst = std::max(hitch_worst, total);
    hitch_worst_wait = std::max(hitch_worst_wait, h.wait);
    double gpu_total = 0.0;
    for (const double ms : time_last_ms) {
        gpu_total += ms;
    }
    if (Rt::IsHitch({.cpu_ms = total, .wait_ms = h.wait, .gpu_ms = gpu_total}, hitch_threshold_ms)) {
        ++hitch_frames;
        Log("[%.1f ms] Ray tracing hitch: frame %llu, RT CPU %.2f ms (draws %.2f in %u calls, alloc %.2f, "
            "build %.2f, GI %.2f, GI images %.2f, debug %.2f, display %.2f: wait %.2f, readback "
            "%.2f, evict %.2f, stats %.2f); GPU %.2f ms %u frames back (build %.2f, GI %.2f, "
            "direct %.2f, ReSTIR %.2f, reflect %.2f, shadows %.2f, AO %.2f, fog %.2f); %u BLAS "
            "rebuilt (%llu triangles: %u new of which %u repeat draws shifted, miss kinds %u/%u/%u/%u/%u, %u dirty, reasons miss %u size %u dirty %u unreadable %u skeleton %u palette %u), %u superset hits, %u refit (%llu triangles, %u skeletons, "
            "%u anchored to their build camera, up to %.1f deg turned), "
            "%u held, %u deferred, budgets %llu/%llu, pool %llu of %llu MiB; kept %llu (%llu "
            "triangles, %llu alpha-tested, %llu sharing a drawn texture, %llu over BB_RT_KEEP_TRIS "
            "%llu, %llu over BB_RT_KEEP_MAX_TRIS, %s)\n",
            BbOut::Ms(), (unsigned long long)(frame - 1), total, h.add, h.adds, h.alloc, h.build, h.gi,
            h.images, h.debug, h.display, h.wait, h.readback, h.evict, h.stats, gpu_total, Slots,
            time_last_ms[0], time_last_ms[1], time_last_ms[2], time_last_ms[3], time_last_ms[4],
            time_last_ms[5], time_last_ms[6], time_last_ms[7], h.rebuilds,
            (unsigned long long)h.rebuilt_triangles, h.misses, h.shifted, h.miss_kind[0],
            h.miss_kind[1], h.miss_kind[2], h.miss_kind[3], h.miss_kind[4], h.dirty, h.reasons[1],
            h.reasons[2], h.reasons[3], h.reasons[4], h.reasons[5], h.reasons[6], h.superset,
            h.refits,
            (unsigned long long)h.refit_triangles, h.skeleton, h.anchored, double(h.anchor_turn),
            h.held,
            h.deferred, (unsigned long long)rebuild_budget.Budget(),
            (unsigned long long)refit_budget.Budget(),
            (unsigned long long)((pool.Used() - retired_bytes) >> 20),
            (unsigned long long)(pool.Size() >> 20), (unsigned long long)last_kept,
            (unsigned long long)kept_tris_last, (unsigned long long)kept_alpha_last,
            (unsigned long long)kept_shared_tex_last, (unsigned long long)kept_over_budget_last,
            (unsigned long long)keep_rules.triangle_budget, (unsigned long long)kept_too_big_last,
            keep_gi ? "traced by every ray, BB_RT_KEEP_GI" : "direct shadows only, BB_RT_KEEP_GI 0");
    }
    hitch = {};
}

void RayTracing::FinishFrame(const SceneInputs& inputs) {
    PollHot();
    const u32 triangles = u32(std::min<u64>(
        [&] {
            u64 total = 0;
            for (const FrameInstance& fi : instances) {
                if (const Rt::CacheEntry* e = cache.ById(fi.entry_id)) {
                    total += e->triangles;
                }
            }
            return total;
        }(),
        ~0u));
    max_triangles_seen = std::max(max_triangles_seen, triangles);
    if (frame % 600 == 0) {
        PrintStats(triangles);
    } else if (frame % gpu_line_frames == 0) {
        PrintGpuLine();
    }
    const u32 slot = u32(frame % Slots);
    const u32 pass_slot = built_slot;
    const bool passes_ran = tlas_address != 0;
    const bool camera = inputs.projection[0] != 0.0f && inputs.projection[1] != 0.0f;
    if (!instances.empty() && camera) {
        Build(inputs);
    } else if (instances.empty()) {
        ++empty_frames;
    } else {
        ++skipped_frames;
    }
    if (!built_this_frame) {
        for (const FrameInstance& fi : instances) {
            if (fi.fresh) {
                cache.Erase(fi.entry_id, [&](const Rt::CacheEntry& e) { ReleaseEntry(e); });
            }
        }
    }
    if (gi_enabled && last_gi_frame != frame) {
        history_prev_frame_ok = false;
    }
    if (built_this_frame) {
        slot_ticks[slot] = scheduler.CurrentTick();
    }
    if (passes_ran) {
        slot_ticks[pass_slot] = scheduler.CurrentTick();
    }
    surface_holds.Finish(scheduler.CurrentTick(), built_this_frame, slot, HoldFn());
    const u64 end_tick = scheduler.CurrentTick();
    Rt::CloseFrame(retired, frame, end_tick);
    Rt::CloseFrame(retired_uv, frame, end_tick);
    Rt::CloseFrame(retired_omm, frame, end_tick);
    Rt::CloseFrame(retired_omm_index, frame, end_tick);
    if (DirectTrace()) {
        Trace("frame end: %u light-VS draws, %u skipped, %u composite hooks", frame_light_draws,
              frame_light_draws_skipped, frame_direct_calls);
    }
    ++frame;
    Checkpoint::frame.store(u32(frame), std::memory_order_relaxed);
    frame_light_draws = 0;
    frame_light_draws_skipped = 0;
    frame_direct_calls = 0;
    light_draws_skipped_this_frame = false;
    built_this_frame = false;
    gbuffer_count = 0;
    gbuffer_bound = 0;
    emissive_target = {};
    scene_color = {};
    BeginSlot();
    if (!fx_held.empty()) {
        auto held = std::move(fx_held);
        fx_held.clear();
        for (const auto& h : held) {
            AddDraw(h.draw);
        }
        last_add_accepted = false;
    }
    late_entries.clear();
    late_last = u32(late_held.size());
    late_total += late_last;
    if (!late_held.empty()) {
        auto held = std::move(late_held);
        late_held.clear();
        for (const auto& h : held) {
            const size_t before = instances.size();
            AddDraw(h.draw);
            if (instances.size() > before) {
                FrameInstance& fi = instances.back();
                if (late_mode == 1 && fi.fresh && h.camera_valid) {
                    fi.inv_view = h.inv_view;
                    fi.anchored = true;
                }
                late_entries.insert(fi.entry_id);
                const VideoCore::Image* img =
                    h.albedo_view ? texture_cache.TryGetImage(h.albedo_image, 0) : nullptr;
                if (img && img->image_uid == h.albedo_uid) {
                    OnDrawBound(h.albedo_view, h.albedo_image, h.albedo_uid, {}, {}, {}, {}, 0, 3);
                }
            }
        }
        last_add_accepted = false;
    }
    if (frame % 600 == 0 && late_total) {
        Log("Ray tracing: %llu scene draws after the TLAS build held over to the next frame (%u "
            "last frame, BB_RT_LATE_DRAWS %u), %llu repeats skipped, %llu over the cap\n",
            (unsigned long long)late_total, late_last, late_mode,
            (unsigned long long)late_duplicates, (unsigned long long)late_dropped);
    }
}

}

namespace Vulkan {

bool RayTracing::CreateSurfaceResources() {
    const auto device = instance.GetDevice();
    const u64 uv_default = (u64(capacity.triangles) * Rt::TriangleUvBytes) >> 20;
    uv_buffer.size = surface_avail
                         ? EnvU64("BB_RT_UV_MB", std::max<u64>(uv_default, 16), 4, 2048) << 20
                         : 0;
    if (uv_buffer.size &&
        !CreateRawBuffer(instance, uv_buffer.size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, 16,
                         uv_buffer.buffer, uv_buffer.allocation, nullptr, uv_buffer.address)) {
        uv_buffer.size = 0;
        Log("Ray tracing: UV pool allocation failed, hit surfaces off\n");
        surface_avail = false;
    }
    uv_pool.Reset(uv_buffer.size, 16);
    surfaces_ring.slot_bytes = capacity.records * sizeof(Rt::SurfaceRecord);
    void* mapped = nullptr;
    if (!CreateRawBuffer(instance, u64(surfaces_ring.slot_bytes) * Slots,
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, 16, surfaces_ring.buffer,
                         surfaces_ring.allocation, &mapped, surfaces_ring.address)) {
        return false;
    }
    surfaces_ring.mapped = static_cast<u8*>(mapped);
    std::memset(surfaces_ring.mapped, 0, u64(surfaces_ring.slot_bytes) * Slots);
    const vk::DescriptorSetLayoutBinding array_binding{
        .binding = 0,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
        .descriptorCount = Rt::MaxSurfaceTextures,
        .stageFlags = toggle_avail.gi_pipeline
                          ? vk::ShaderStageFlagBits::eCompute | GiRtStages
                          : vk::ShaderStageFlags{vk::ShaderStageFlagBits::eCompute},
    };
    const vk::DescriptorBindingFlags binding_flags = vk::DescriptorBindingFlagBits::ePartiallyBound;
    const vk::DescriptorSetLayoutBindingFlagsCreateInfo flags_info{
        .bindingCount = 1,
        .pBindingFlags = &binding_flags,
    };
    auto layout = device.createDescriptorSetLayoutUnique({
        .pNext = &flags_info,
        .bindingCount = 1,
        .pBindings = &array_binding,
    });
    if (layout.result != vk::Result::eSuccess) {
        return false;
    }
    surface_layout = std::move(layout.value);
    const vk::DescriptorPoolSize pool_size{
        .type = vk::DescriptorType::eCombinedImageSampler,
        .descriptorCount = Rt::MaxSurfaceTextures * SurfaceSetRing,
    };
    auto pool_result = device.createDescriptorPoolUnique({
        .maxSets = SurfaceSetRing,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size,
    });
    if (pool_result.result != vk::Result::eSuccess) {
        return false;
    }
    surface_descriptor_pool = std::move(pool_result.value);
    std::array<vk::DescriptorSetLayout, SurfaceSetRing> layouts;
    layouts.fill(*surface_layout);
    const auto sets = device.allocateDescriptorSets({
        .descriptorPool = *surface_descriptor_pool,
        .descriptorSetCount = SurfaceSetRing,
        .pSetLayouts = layouts.data(),
    });
    if (sets.result != vk::Result::eSuccess || sets.value.size() != SurfaceSetRing) {
        return false;
    }
    for (u32 i = 0; i < SurfaceSetRing; ++i) {
        surface_ring[i] = {sets.value[i], 0};
    }
    surface_ring_next = 0;
    surface_fallback.image = VideoCore::UniqueImage(device, instance.GetAllocator());
    if (!surface_fallback.image.TryCreate(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = vk::Format::eR8G8B8A8Unorm,
            .extent = {1, 1, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
            .initialLayout = vk::ImageLayout::eUndefined,
        })) {
        return false;
    }
    auto fallback_view = device.createImageViewUnique({
        .image = vk::Image(surface_fallback.image),
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR8G8B8A8Unorm,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    });
    if (fallback_view.result != vk::Result::eSuccess) {
        return false;
    }
    surface_fallback.view = std::move(fallback_view.value);
    surface_fallback.written = false;
    auto sampler = device.createSamplerUnique({
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eLinear,
        .addressModeU = vk::SamplerAddressMode::eRepeat,
        .addressModeV = vk::SamplerAddressMode::eRepeat,
        .addressModeW = vk::SamplerAddressMode::eRepeat,
        .maxLod = 16.0f,
    });
    if (sampler.result != vk::Result::eSuccess) {
        return false;
    }
    surface_sampler = std::move(sampler.value);
    if (surface_avail) {
        Log("Ray tracing: hit surfaces on, UV pool %llu MiB (BB_RT_UV_MB), %u textures per frame, "
            "debug 9 albedo, 10 alpha-test rejections (BB_RT_GI_DEBUG)\n",
            (unsigned long long)(uv_buffer.size >> 20), Rt::MaxSurfaceTextures);
    }
    return true;
}

void RayTracing::DestroySurfaceResources() {
    const auto allocator = instance.GetAllocator();
    surface_holds.ReleaseAll(scheduler.CurrentTick(), HoldFn());
    retired_uv.clear();
    surface_sampler.reset();
    surface_ring = {};
    surface_fallback.view.reset();
    surface_fallback.image.Destroy();
    surface_fallback.written = false;
    for (auto& refs : slot_textures) {
        refs.clear();
    }
    surface_descriptor_pool.reset();
    surface_layout.reset();
    if (uv_buffer.buffer) {
        vmaDestroyBuffer(allocator, uv_buffer.buffer, uv_buffer.allocation);
        uv_buffer.buffer = vk::Buffer{};
    }
    if (surfaces_ring.buffer) {
        vmaDestroyBuffer(allocator, surfaces_ring.buffer, surfaces_ring.allocation);
        surfaces_ring.buffer = vk::Buffer{};
        surfaces_ring.mapped = nullptr;
    }
}

void RayTracing::OnDrawBound(vk::ImageView albedo_view, VideoCore::ImageId image, u64 uid,
                             vk::ImageView normal_view, VideoCore::ImageId normal_image,
                             vk::ImageView alpha_view, VideoCore::ImageId alpha_image,
                             u64 alpha_uid, u32 alpha_channel) {
    if (std::exchange(last_add_late, false)) {
        if (!late_held.empty() && albedo_view && image) {
            late_held.back().albedo_view = albedo_view;
            late_held.back().albedo_image = image;
            late_held.back().albedo_uid = uid;
        }
        return;
    }
    if (!enabled || !surface_enabled || !last_add_accepted || instances.empty()) {
        return;
    }
    last_add_accepted = false;
    ++surface_draws;
    if (alpha_view && alpha_image && instances.back().alpha_test &&
        (alpha_view != albedo_view || alpha_channel != 3)) {
        const u32 alpha_slot = SurfaceSlot(alpha_view, alpha_image);
        if (alpha_slot) {
            instances.back().alpha_texture = alpha_slot;
            instances.back().alpha_channel = alpha_channel & 3u;
            instances.back().alpha_view = u64(VkImageView(alpha_view));
            instances.back().alpha_image = alpha_image.index;
            instances.back().alpha_uid = alpha_uid;
            ++surface_alpha_split;
            surface_holds.Bound(alpha_image.index, alpha_uid, HoldFn());
        }
    }
    if (!albedo_view) {
        return;
    }
    const u32 slot = SurfaceSlot(albedo_view, image);
    if (!slot) {
        return;
    }
    instances.back().view = u64(VkImageView(albedo_view));
    instances.back().image = image.index;
    instances.back().uid = uid;
    instances.back().texture = slot;
    ++surface_textured;
    surface_holds.Bound(image.index, uid, HoldFn());
    if (normal_view && normal_image) {
        const u32 normal_slot = SurfaceSlot(normal_view, normal_image);
        if (normal_slot) {
            const VideoCore::Image* nimg = texture_cache.TryGetImage(normal_image, 0);
            instances.back().normal_map = normal_slot;
            instances.back().normal_view = u64(VkImageView(normal_view));
            instances.back().normal_uid = nimg ? nimg->image_uid : 0;
            instances.back().normal_image = normal_image.index;
            ++surface_normal_mapped;
            surface_holds.Bound(normal_image.index, nimg ? nimg->image_uid : 0, HoldFn());
        }
    }
}

std::function<void(u32, u64, u64)> RayTracing::HoldFn() {
    if (!off_switches.holds) {
        return [](u32, u64, u64) {};
    }
    return [this](u32 img, u64 id, u64 tick) {
        texture_cache.HoldImage(VideoCore::ImageId{img}, id, tick);
    };
}

u32 RayTracing::SurfaceSlot(vk::ImageView albedo_view, VideoCore::ImageId albedo_image) {
    const u32 slot_index = u32(frame % Slots);
    const u32 slot = textures.Slot(u64(VkImageView(albedo_view)));
    if (!slot) {
        return 0;
    }
    if (slot >= surface_written) {
        auto& refs = slot_textures[slot_index];
        if (refs.size() != slot) {
            return 0;
        }
        const VideoCore::Image* image =
            albedo_image ? texture_cache.TryGetImage(albedo_image, 0) : nullptr;
        refs.push_back({albedo_image, image ? image->image_uid : 0, albedo_view});
        surface_written = slot + 1;
    }
    return slot;
}

void RayTracing::SkinNote(u64 key, const SkinState& state) {
    SkinState& cur = skin_cur[key];
    cur = state;
    cur.last_frame = frame;
}

void RayTracing::SkinFrame() {
    if (!skin_log) {
        return;
    }
    for (const auto& [key, prev] : skin_prev) {
        if (skin_cur.count(key) || frame - prev.last_frame > 600) {
            continue;
        }
        SkinState gone = prev;
        if (prev.present) {
            gone.present = false;
            gone.step = 8;
            gone.mask = 0;
        }
        skin_cur.emplace(key, gone);
    }
    static constexpr const char* mode_names[] = {"built", "cached", "kept", "hidden"};
    static constexpr const char* step_names[] = {"new",      "rebuilt-dirty", "rebuilt",
                                                 "refit",    "cached",        "held",
                                                 "deferred", "kept",          "not drawn"};
    u64 present = 0;
    u32 lines = 0;
    for (auto& [key, cur] : skin_cur) {
        present += cur.present ? 1u : 0u;
        const auto it = skin_prev.find(key);
        const SkinState* prev = it == skin_prev.end() ? nullptr : &it->second;
        if (prev && !cur.ps) {
            cur.ps = prev->ps;
            cur.vs = prev->vs;
        }
        std::string changed;
        if (!prev) {
            changed = " first";
        } else {
            if (prev->present != cur.present) changed += " present";
            if (prev->mode != cur.mode) changed += " mode";
            if (prev->step != cur.step) changed += " step";
            if (prev->alpha != cur.alpha) changed += " alpha";
            if (prev->textured != cur.textured || prev->view != cur.view) changed += " texture";
            if (prev->mask != cur.mask) changed += " mask";
            if (prev->flags != cur.flags) changed += " flags";
            if (prev->entry != cur.entry) changed += " entry";
            if (prev->triangles != cur.triangles) changed += " triangles";
        }
        if (changed.empty()) {
            continue;
        }
        skin_sec_absent += prev && prev->present && !cur.present ? 1u : 0u;
        skin_sec_flips += prev ? 1u : 0u;
        if (lines >= 48) {
            ++skin_sec_suppressed;
            continue;
        }
        ++lines;
        Log("RT skin: frame %llu key %016llx entry %u present %s mode %s step %s alpha %u "
            "textured %u mask 0x%02x flags 0x%x slot %u tris %u ps %016llx vs %016llx; "
            "changed:%s\n",
            (unsigned long long)frame, (unsigned long long)key, cur.entry,
            cur.present ? "yes" : "no", mode_names[cur.mode & 3u],
            cur.step < 9 ? step_names[cur.step] : "?", u32(cur.alpha), u32(cur.textured), cur.mask,
            cur.flags, cur.slot, cur.triangles, (unsigned long long)cur.ps,
            (unsigned long long)cur.vs, changed.c_str());
    }
    skin_entry_keys.clear();
    for (const auto& [key, cur] : skin_cur) {
        if (cur.entry) {
            skin_entry_keys[cur.entry] = key;
        }
    }
    skin_prev.swap(skin_cur);
    skin_cur.clear();
    ++skin_sec_frames;
    skin_sec_max = std::max(skin_sec_max, present);
    const double now = std::chrono::duration<double>(
                           std::chrono::steady_clock::now().time_since_epoch()).count();
    if (skin_sec_start == 0.0) {
        skin_sec_start = now;
    }
    if (now - skin_sec_start < 1.0) {
        return;
    }
    Log("RT skin: last %.1f s, %llu frames: skinned instances %llu (max %llu), absent-this-frame "
        "%llu, flipped %llu, probe mismatches %llu (%llu left out), lines suppressed %llu; moving "
        "entries %llu, probe mismatches kept %llu (totals)\n",
        now - skin_sec_start, (unsigned long long)skin_sec_frames, (unsigned long long)present,
        (unsigned long long)skin_sec_max, (unsigned long long)skin_sec_absent,
        (unsigned long long)skin_sec_flips, (unsigned long long)skin_sec_probe,
        (unsigned long long)skin_sec_probe_masked, (unsigned long long)skin_sec_suppressed,
        (unsigned long long)moving_marked, (unsigned long long)probe_kept);
    skin_sec_start = now;
    skin_sec_frames = skin_sec_max = skin_sec_absent = skin_sec_flips = 0;
    skin_sec_probe = skin_sec_probe_masked = skin_sec_suppressed = 0;
}

void RayTracing::NoteInstanceState(u32 entry_id, u32 mode, u64 ps, u8 state, bool unresolved) {

    if (entry_id >= instance_state_prev.size()) {
        instance_state_prev.resize(entry_id + 1, 0);
    }
    const auto bump = [](std::unordered_map<u64, u64>& map, u64 key) {
        if (map.size() < 4096 || map.count(key)) {
            ++map[key];
        }
    };
    u8& prev = instance_state_prev[entry_id];
    if (prev && prev != state) {
        ++flips_last;
        ++flips_by_mode[mode];
        bump(flips_by_ps, ps);
    }
    prev = state;
    if (unresolved) {
        ++unresolved_by_mode[mode];
        bump(unresolved_by_ps, ps);
    }
}

void RayTracing::AppendKeptInstances(const Rt::Affine& current_inv_view) {
    last_kept = 0;
    const std::array<float, 3> camera = Rt::Transform(current_inv_view, {0.0f, 0.0f, 0.0f});
    for (const FrameInstance& fi : instances) {
        Rt::CacheEntry* e = cache.ById(fi.entry_id);
        if (!e) {
            continue;
        }
        e->seen = frame_camera_valid;
        e->seen_camera = camera;
        e->alpha_test = fi.alpha_test;
        e->alpha_ref = fi.alpha_ref;
        e->blended = fi.blended;
        if (!Rt::RememberSurface(fi.texture, fi.alpha_texture)) {
            continue;
        }
        e->tex_view = fi.view;
        e->tex_uid = fi.uid;
        e->tex_image = fi.image;
        e->alpha_view = fi.alpha_view;
        e->alpha_uid = fi.alpha_uid;
        e->alpha_image = fi.alpha_image;
        e->alpha_channel = fi.alpha_channel;
        e->normal_view = fi.normal_map ? fi.normal_view : 0;
        e->normal_uid = fi.normal_map ? fi.normal_uid : 0;
        e->normal_image = fi.normal_map ? fi.normal_image : 0;
    }
    kept_tris_last = kept_alpha_last = kept_over_budget_last = kept_too_big_last = 0;
    kept_shared_tex_last = 0;
    if (!frame_camera_valid || keep_rules.distance <= 0.0f) {
        return;
    }
    std::vector<Rt::CacheEntry*> candidates;
    cache.ForEachOrdered([&](Rt::CacheEntry& e) {
        if (!e.blended && Rt::KeepInstance(e, frame, camera, keep_rules)) {
            candidates.push_back(&e);
        }
    });
    const Rt::KeptPick pick = Rt::PickKept(std::move(candidates), camera, keep_rules);
    kept_over_budget_last = pick.over_budget;
    kept_too_big_last = pick.too_big;
    kept_dropped_total += pick.over_budget + pick.too_big;
    std::unordered_set<u64> fresh_textures;
    for (const FrameInstance& fi : instances) {
        if (fi.uid) {
            fresh_textures.insert(fi.uid);
        }
    }
    for (Rt::CacheEntry* kept_entry : pick.picked) {
        Rt::CacheEntry& e = *kept_entry;
        if (instances.size() >= capacity.records) {
            break;
        }
        kept_tris_last += e.triangles;
        kept_alpha_last += e.alpha_test ? 1u : 0u;
        kept_shared_tex_last += e.tex_uid && fresh_textures.contains(e.tex_uid) ? 1u : 0u;
        FrameInstance fi{};
        fi.kept = true;
        fi.entry_id = e.id;
        fi.inv_view = e.inv_view;
        fi.alpha_test = e.alpha_test;
        fi.alpha_ref = e.alpha_ref;
        fi.view = e.tex_view;
        fi.uid = e.tex_uid;
        fi.image = e.tex_image;
        fi.mode = 2;
        if (e.id < entry_tags.size()) {
            fi.tag = entry_tags[e.id];
        }
        if (surface_enabled && fi.view) {
            const VideoCore::Image* image =
                fi.uid ? texture_cache.TryGetImage(VideoCore::ImageId{fi.image}, fi.uid) : nullptr;
            const bool alive = Rt::KeptTextureAlive(
                image != nullptr, true,
                image && True(image->flags & VideoCore::ImageFlagBits::Registered));
            fi.texture = alive && off_switches.texresolve
                             ? SurfaceSlot(vk::ImageView{VkImageView(fi.view)}, VideoCore::ImageId{fi.image})
                             : 0;
            if (fi.texture) {
                surface_holds.Bound(fi.image, fi.uid, HoldFn());
            } else {
                ++kept_untextured;
            }
        }
        if (surface_enabled && e.alpha_view) {
            const VideoCore::Image* alpha =
                e.alpha_uid ? texture_cache.TryGetImage(VideoCore::ImageId{e.alpha_image}, e.alpha_uid)
                            : nullptr;
            const bool alpha_alive = Rt::KeptTextureAlive(
                alpha != nullptr, true,
                alpha && True(alpha->flags & VideoCore::ImageFlagBits::Registered));
            fi.alpha_texture = alpha_alive && off_switches.texresolve
                                   ? SurfaceSlot(vk::ImageView{VkImageView(e.alpha_view)},
                                                 VideoCore::ImageId{e.alpha_image})
                                   : 0;
            if (fi.alpha_texture) {
                fi.alpha_channel = e.alpha_channel;
                fi.alpha_view = e.alpha_view;
                fi.alpha_uid = e.alpha_uid;
                fi.alpha_image = e.alpha_image;
                surface_holds.Bound(e.alpha_image, e.alpha_uid, HoldFn());
            }
        }
        if (surface_enabled && fi.texture && e.normal_view) {
            const VideoCore::Image* normal =
                e.normal_uid ? texture_cache.TryGetImage(VideoCore::ImageId{e.normal_image}, e.normal_uid)
                             : nullptr;
            const bool normal_alive = Rt::KeptTextureAlive(
                normal != nullptr, true,
                normal && True(normal->flags & VideoCore::ImageFlagBits::Registered));
            fi.normal_map = normal_alive && off_switches.texresolve
                                ? SurfaceSlot(vk::ImageView{VkImageView(e.normal_view)},
                                              VideoCore::ImageId{e.normal_image})
                                : 0;
            if (fi.normal_map) {
                fi.normal_view = e.normal_view;
                fi.normal_uid = e.normal_uid;
                fi.normal_image = e.normal_image;
                surface_holds.Bound(e.normal_image, e.normal_uid, HoldFn());
            }
        }
        if (!fi.texture) {
            fi.view = 0;
            fi.uid = 0;
            fi.image = 0;
        }
        instances.push_back(fi);
        ProbeRecord probe{};
        probe.entry = e.id;
        probe.mode = 2;
        probes.push_back(probe);
        ++last_kept;
    }
    kept_instances += last_kept;
}

vk::DescriptorSet RayTracing::SurfaceSet(vk::CommandBuffer cmdbuf, u32 slot) {
    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;
    SurfaceSetSlot& ring = surface_ring[surface_ring_next];
    surface_ring_next = (surface_ring_next + 1) % SurfaceSetRing;
    if (ring.tick && !scheduler.IsFree(ring.tick)) {
        scheduler.Wait(ring.tick);
    }
    ring.tick = scheduler.CurrentTick();
    if (!surface_fallback.written) {
        const vk::ImageMemoryBarrier2 to_general{
            .srcStageMask = Stage::eTopOfPipe,
            .dstStageMask = Stage::eTransfer,
            .dstAccessMask = Access::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = vk::Image(surface_fallback.image),
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_general});
        const vk::ClearColorValue white{std::array{1.0f, 1.0f, 1.0f, 1.0f}};
        const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
        cmdbuf.clearColorImage(vk::Image(surface_fallback.image), vk::ImageLayout::eGeneral, white,
                               range);
        Barrier(cmdbuf, Stage::eTransfer, Access::eTransferWrite, Stage::eComputeShader,
                Access::eShaderRead);
        surface_fallback.written = true;
    }
    const auto& refs = slot_textures[slot];
    std::vector<vk::DescriptorImageInfo> infos;
    infos.reserve(refs.size());
    for (size_t i = 0; i < refs.size(); ++i) {
        const TextureRef& ref = refs[i];
        const VideoCore::Image* now = ref.id ? texture_cache.TryGetImage(ref.id, 0) : nullptr;
        const bool alive = (i && ref.own && ref.view) ||
                           Rt::SurfaceTextureAlive(u32(i), u64(VkImageView(ref.view)), ref.id.index,
                                                   ref.uid, now ? now->image_uid : 0);
        if (i && !alive) {
            ++surface_dead;
        }
        infos.push_back({.sampler = *surface_sampler,
                         .imageView = alive ? ref.view : *surface_fallback.view,
                         .imageLayout = vk::ImageLayout::eGeneral});
    }
    if (!infos.empty()) {
        const vk::WriteDescriptorSet write{
            .dstSet = ring.set,
            .dstBinding = 0,
            .dstArrayElement = 0,
            .descriptorCount = u32(infos.size()),
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .pImageInfo = infos.data(),
        };
        instance.GetDevice().updateDescriptorSets(write, {});
    }
    surface_last = {ring.set, ring.tick, slot};
    return ring.set;
}

void RayTracing::GlassMaskSlots(vk::CommandBuffer cmdbuf) {
    if (!glass_on || !glass_pass || glass_ps.empty() || !surface_enabled || glass_mask_dir.empty()) {
        return;
    }
    for (FrameInstance& fi : instances) {
        fi.glass_mask = 0;
        if (!fi.texture || !fi.uid || fi.alpha_test || !Rt::HashListed(glass_ps, fi.tag.ps, 0)) {
            continue;
        }
        // Small draws too: an all-black mask turns a listed draw that is no glass (the workshop
        // mirror, ca7226ec, 8 triangles) back into an opaque surface.
        const Rt::CacheEntry* e = cache.ById(fi.entry_id);
        if (!e) {
            continue;
        }
        auto it = glass_mask_by_uid.find(fi.uid);
        if (it == glass_mask_by_uid.end()) {
            const VideoCore::Image* image =
                texture_cache.TryGetImage(VideoCore::ImageId{fi.image}, fi.uid);
            if (!image || !image->info.guest_address || !image->info.guest_size) {
                continue;
            }
            const u64 hash = XXH3_64bits(std::bit_cast<const u8*>(image->info.guest_address),
                                         image->info.guest_size);
            auto known = glass_mask_by_hash.find(hash);
            if (known == glass_mask_by_hash.end()) {
                known = glass_mask_by_hash.emplace(hash, GlassMaskLoad(cmdbuf, hash)).first;
            }
            it = glass_mask_by_uid.emplace(fi.uid, known->second).first;
            Log("RT glass mask: ps %08llx %u tris albedo %016llx %ux%u fmt %d -> %s\n",
                (unsigned long long)fi.tag.ps, e->triangles, (unsigned long long)hash,
                image->info.size.width, image->info.size.height, int(image->info.pixel_format),
                known->second >= 0 ? "masked" : "no mask");
            if (glass_mask_dump && glass_mask_dumped.insert(hash).second) {
                GlassMaskDump(cmdbuf, *image, hash);
            }
        }
        if (it->second < 0) {
            continue;
        }
        const vk::ImageView view = *glass_mask_images[it->second].view;
        const u32 mask_slot = SurfaceSlot(view, VideoCore::ImageId{});
        auto& refs = slot_textures[u32(frame % Slots)];
        if (mask_slot && mask_slot < refs.size() && refs[mask_slot].view == view) {
            refs[mask_slot].own = true;
            fi.glass_mask = mask_slot;
        }
    }
}

s32 RayTracing::GlassMaskLoad(vk::CommandBuffer cmdbuf, u64 hash) {
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.pgm", (unsigned long long)hash);
    const std::filesystem::path file = std::filesystem::path(glass_mask_dir) / name;
    std::error_code ec;
    const s64 stamp = s64(std::filesystem::last_write_time(file, ec).time_since_epoch().count());
    if (ec) {
        return -1;
    }
    // An unchanged file keeps its image; an edited one is uploaded again.
    if (const auto it = glass_mask_files.find(hash); it != glass_mask_files.end() && it->second.first == stamp) {
        return it->second.second;
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return -1;
    }
    std::string magic;
    u32 width = 0, height = 0, maxval = 0;
    in >> magic >> width >> height >> maxval;
    in.get();
    if (magic != "P5" || !width || !height || width > 8192 || height > 8192 || maxval != 255) {
        Log("RT glass mask: %s is not an 8-bit binary PGM\n", name);
        return -1;
    }
    std::vector<u8> pixels(size_t(width) * height);
    if (!in.read(reinterpret_cast<char*>(pixels.data()), std::streamsize(pixels.size()))) {
        Log("RT glass mask: %s is short\n", name);
        return -1;
    }
    OwnImage own;
    if (!MakeOwnImage(own, vk::Format::eR8Unorm, width, height,
                      vk::ImageUsageFlagBits::eTransferDst)) {
        Log("RT glass mask: no image for %s\n", name);
        return -1;
    }
    const VkBufferCreateInfo buffer_ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = pixels.size(),
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
    };
    const VmaAllocationCreateInfo alloc_ci{
        .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
    };
    VkBuffer buffer{};
    VmaAllocation allocation{};
    VmaAllocationInfo info{};
    const VmaAllocator allocator = instance.GetAllocator();
    if (vmaCreateBuffer(allocator, &buffer_ci, &alloc_ci, &buffer, &allocation, &info) != VK_SUCCESS) {
        return -1;
    }
    std::memcpy(info.pMappedData, pixels.data(), pixels.size());
    vmaFlushAllocation(allocator, allocation, 0, VK_WHOLE_SIZE);
    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;
    const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    const vk::ImageMemoryBarrier2 to_dst{
        .srcStageMask = Stage::eTopOfPipe,
        .dstStageMask = Stage::eTransfer,
        .dstAccessMask = Access::eTransferWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eTransferDstOptimal,
        .image = vk::Image(own.image),
        .subresourceRange = range,
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_dst});
    const vk::BufferImageCopy region{
        .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .imageExtent = {width, height, 1},
    };
    cmdbuf.copyBufferToImage(buffer, vk::Image(own.image), vk::ImageLayout::eTransferDstOptimal,
                             region);
    const vk::ImageMemoryBarrier2 to_general{
        .srcStageMask = Stage::eTransfer,
        .srcAccessMask = Access::eTransferWrite,
        .dstStageMask = Stage::eAllCommands,
        .dstAccessMask = Access::eShaderRead,
        .oldLayout = vk::ImageLayout::eTransferDstOptimal,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = vk::Image(own.image),
        .subresourceRange = range,
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_general});
    scheduler.DeferPriorityOperation(
        [allocator, buffer, allocation] { vmaDestroyBuffer(allocator, buffer, allocation); });
    own.written = true;
    glass_mask_images.push_back(std::move(own));
    glass_mask_files[hash] = {stamp, s32(glass_mask_images.size() - 1)};
    Log("RT glass mask: loaded %s (%ux%u)\n", name, width, height);
    return s32(glass_mask_images.size() - 1);
}

void RayTracing::GlassMaskDump(vk::CommandBuffer cmdbuf, const VideoCore::Image& image, u64 hash) {
    const vk::Format format = image.info.pixel_format;
    const u32 width = image.info.size.width, height = image.info.size.height;
    // BC1-BC7 (4x4 blocks of 8 or 16 bytes) and 8-bit RGBA/BGRA; anything else is not dumped.
    const int f = int(format);
    const bool bc = f >= 131 && f <= 146;
    const std::array<u32, 2> extent{bc ? 4u : 1u, bc ? 4u : 1u};
    const u32 block = bc ? ((f <= 134 || f == 139 || f == 140) ? 8u : 16u) : (f >= 37 && f <= 50) ? 4u : 0u;
    const vk::ImageLayout layout = image.backing ? image.backing->state.layout : vk::ImageLayout::eUndefined;
    if (!block || layout == vk::ImageLayout::eUndefined) {
        Log("RT glass mask: no dump of %016llx (fmt %d layout %d)\n", (unsigned long long)hash,
            int(format), int(layout));
        return;
    }
    const VkDeviceSize size = VkDeviceSize((width + extent[0] - 1) / extent[0]) *
                              ((height + extent[1] - 1) / extent[1]) * block;
    const VkBufferCreateInfo buffer_ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    };
    const VmaAllocationCreateInfo alloc_ci{
        .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
    };
    VkBuffer buffer{};
    VmaAllocation allocation{};
    VmaAllocationInfo info{};
    const VmaAllocator allocator = instance.GetAllocator();
    if (vmaCreateBuffer(allocator, &buffer_ci, &alloc_ci, &buffer, &allocation, &info) != VK_SUCCESS) {
        return;
    }
    // The texture stays in its own layout for the game: to transfer-src for the copy and back.
    const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    const vk::ImageMemoryBarrier2 to_src{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
        .oldLayout = layout,
        .newLayout = vk::ImageLayout::eTransferSrcOptimal,
        .image = image.GetImage(),
        .subresourceRange = range,
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_src});
    const vk::BufferImageCopy region{
        .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .imageExtent = {width, height, 1},
    };
    cmdbuf.copyImageToBuffer(image.GetImage(), vk::ImageLayout::eTransferSrcOptimal, buffer, region);
    const vk::ImageMemoryBarrier2 back{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
        .newLayout = layout,
        .image = image.GetImage(),
        .subresourceRange = range,
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &back});
    const std::filesystem::path dir = std::filesystem::path(glass_mask_dir) / "dump";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    char name[96];
    std::snprintf(name, sizeof(name), "%016llx_%ux%u_%d.raw", (unsigned long long)hash, width,
                  height, int(format));
    scheduler.DeferPriorityOperation([allocator, buffer, allocation, info, size,
                                      file = (dir / name).string()] {
        vmaInvalidateAllocation(allocator, allocation, 0, VK_WHOLE_SIZE);
        if (FILE* f = std::fopen(file.c_str(), "wb")) {
            std::fwrite(info.pMappedData, 1, size, f);
            std::fclose(f);
        }
        vmaDestroyBuffer(allocator, buffer, allocation);
    });
    Log("RT glass mask: dumped %s\n", name);
}

void RayTracing::WriteSurfaces(u32 slot) {
    if (!surfaces_ring.mapped) {
        return;
    }
    auto* records = reinterpret_cast<Rt::SurfaceRecord*>(surfaces_ring.mapped +
                                                         u64(slot) * surfaces_ring.slot_bytes);
    std::memset(records, 0, surfaces_ring.slot_bytes);
    const bool glass = glass_on && !glass_ps.empty();
    for (const FrameInstance& fi : instances) {
        if (fi.entry_id >= capacity.records) {
            continue;
        }
        Rt::SurfaceRecord& record = records[fi.entry_id];
        // An entry shared by several draws keeps the glass bit if any of them is a glass draw.
        const Rt::CacheEntry* ge = cache.ById(fi.entry_id);
        const u32 glass_bit = (record.flags & Rt::SurfaceGlass) |
                              (glass && GlassDraw(fi.tag.ps, ge ? ge->triangles : 0u) ? Rt::SurfaceGlass : 0u);
        const u32 portal_bit = (record.flags & Rt::SurfacePortal) |
                               (PortalDraw(fi.tag.ps, ge ? ge->triangles : 0u) ? Rt::SurfacePortal : 0u);
        const u32 water_bit = (record.flags & Rt::SurfaceWater) |
                              (WaterDraw(fi.tag.ps) ? Rt::SurfaceWater : 0u);
        if (surface_enabled) {
            const Rt::CacheEntry* e = cache.ById(fi.entry_id);
            const u32 uv_base = e ? e->uv_base : Rt::NoUv;
            record = Rt::MakeSurface(fi.texture, fi.alpha_test, uv_base, fi.alpha_ref,
                                     e && e->uv_captured, e && e->omm, fi.normal_map, fi.blended,
                                     fi.alpha_texture, fi.alpha_channel);
            if (fi.alpha_test && (fi.texture || fi.alpha_texture)) {
                ++surface_alpha;
            }
        }
        record.flags |= glass_bit | portal_bit | water_bit;
        record.flags2 = fi.tag.cull != 0u ? Rt::Surface2OneSided : 0u;
        if (fi.glass_mask && surface_enabled && !fi.alpha_test) {
            record.flags |= Rt::SurfaceGlass;
            record.alpha_ref = -float(fi.glass_mask);
        }
        if (const Rt::CacheEntry* e = cache.ById(fi.entry_id); e && (e->skinned || e->palette)) {
            record.flags |= Rt::SurfaceSkinned;
        }
    }
}

}

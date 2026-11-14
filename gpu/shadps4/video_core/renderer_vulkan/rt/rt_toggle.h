#pragma once

#include <atomic>
#include <cstdlib>

#include "video_core/amdgpu/regs.h"
#include "video_core/renderer_vulkan/rt/rt_direct.h"
#include "video_core/renderer_vulkan/rt/rt_fx.h"

namespace Vulkan::Rt {

inline std::atomic<bool> effects_paused{false};

static_assert(u32(AmdGpu::BlendControl::BlendFactor::SrcAlpha) == FxBlendSrcAlpha);
static_assert(u32(AmdGpu::BlendControl::BlendFactor::OneMinusSrcAlpha) == FxBlendOneMinusSrcAlpha);
static_assert(u32(AmdGpu::BlendControl::BlendFunc::Add) == FxBlendAdd);
static_assert(u32(AmdGpu::DataFormat::Format16_16_16_16) == FxFormat16_16_16_16);
static_assert(u32(AmdGpu::PrimitiveType::TriangleList) == FxPrimTriangleList);
static_assert(u32(AmdGpu::PrimitiveType::TriangleStrip) == FxPrimTriangleStrip);
static_assert(u32(AmdGpu::PrimitiveType::PatchPrimitive) == FxPrimPatch);
static_assert(u32(AmdGpu::ShaderStageEnable::VgtStages::Vs) == FxStagesVs);
static_assert(u32(AmdGpu::ShaderStageEnable::VgtStages::LsHs) == FxStagesLsHs);

inline bool FxSmokeWanted() {
    static const bool wanted = [] {
        const char* rt = std::getenv("BB_RT");
        const char* value = std::getenv("BB_RT_FX_SMOKE");
        return rt && rt[0] == '1' && !(value && value[0] == '0');
    }();
    return wanted;
}

inline bool WaterSheetWanted() {
    static const bool wanted = [] {
        const char* rt = std::getenv("BB_RT");
        const char* value = std::getenv("BB_RT_WATER_SHEET");
        return rt && rt[0] == '1' && !(value && value[0] == '0');
    }();
    return wanted;
}

inline bool WaterSheetShader(uint64_t ps) {
    return WaterSheetWanted() && (ps == FxMistShader || ps == WaterLayerShader);
}

inline FxDrawState FxState(const AmdGpu::Regs& regs) {
    const auto& bc = regs.blend_control[0];
    const auto& cb = regs.color_buffers[0];
    return {
        .blend_enable = bc.enable != 0,
        .blend_bypass = cb.info.blend_bypass != 0,
        .src_factor = u32(bc.color_src_factor),
        .dst_factor = u32(bc.color_dst_factor),
        .blend_func = u32(bc.color_func),
        .depth_test = regs.depth_control.depth_enable != 0,
        .depth_write = regs.depth_control.depth_write_enable != 0,
        .color_format = u32(cb.GetDataFmt()),
        .primitive = u32(regs.primitive_type),
        .indices = regs.num_indices,
        .stages = u32(regs.stage_enable.raw) & 0xFFu,
    };
}

inline bool FxSmokeDraw(const AmdGpu::Regs& regs) {
    return FxSmokeWanted() && FxSmokeRule(FxState(regs));
}

inline float FxSmokeEnvFloat(const char* name, float fallback) {
    const char* value = std::getenv(name);
    if (!value || !value[0]) {
        return fallback;
    }
    char* end = nullptr;
    const float v = std::strtof(value, &end);
    return end != value && FxFinite(v) ? v : fallback;
}

inline uint32_t FxSmokeSizeAttr() {
    static const uint32_t v = uint32_t(FxSmokeEnvFloat("BB_RT_FX_SMOKE_SIZE_ATTR", 4.0f));
    return v;
}

inline uint32_t FxSmokeSizeComponent() {
    static const uint32_t v = uint32_t(FxSmokeEnvFloat("BB_RT_FX_SMOKE_SIZE_COMP", 0.0f)) & 3u;
    return v;
}

inline float FxSmokeSizeScale() {
    static const float v = FxSmokeEnvFloat("BB_RT_FX_SMOKE_SIZE_SCALE", 1.0f);
    return v;
}

inline float FxSmokeSizeFixed() {
    static const float v = FxSmokeEnvFloat("BB_RT_FX_SMOKE_SIZE", 0.0f);
    return v;
}

inline float FxSmokeSizeFallback() {
    static const float v = FxSmokeEnvFloat("BB_RT_FX_SMOKE_SIZE_FALLBACK", 0.5f);
    return v;
}

inline bool FxSmokeLog() {
    static const bool wanted = [] {
        const char* value = std::getenv("BB_RT_FX_SMOKE_LOG");
        return value && value[0] == '1';
    }();
    return wanted;
}

inline bool AlphaSourceWanted() {
    static const bool wanted = [] {
        const char* value = std::getenv("BB_RT_ALPHA_SOURCE");
        return !value || value[0] != '0';
    }();
    return wanted;
}

inline bool AlphaSourceLog() {
    static const bool wanted = [] {
        const char* value = std::getenv("BB_RT_ALPHA_LOG");
        return value && value[0] == '1';
    }();
    return wanted;
}

inline bool AlphaInheritWanted() {
    static const bool wanted = [] {
        const char* value = std::getenv("BB_RT_ALPHA_INHERIT");
        return !value || value[0] != '0';
    }();
    return wanted;
}

inline bool CasterLog() {
    static const bool wanted = [] {
        const char* value = std::getenv("BB_RT_CASTER_LOG");
        return value && value[0] == '1';
    }();
    return wanted;
}

inline bool ToggleEffects() {
    const bool paused = !effects_paused.load(std::memory_order_relaxed);
    effects_paused.store(paused, std::memory_order_relaxed);
    return !paused;
}

}

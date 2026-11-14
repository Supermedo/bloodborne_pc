#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "rt_gi.h"

namespace Vulkan::Rt {

enum class LightKind : uint8_t { None, Point, Spot, Line, Directional };

struct LightShader {
    uint32_t ps_hash;
    LightKind kind;
    uint8_t count;
    bool shadowed;
    bool legacy;
    const char* name;
};

inline constexpr uint32_t LightAccVertexShader = 0x8747a367u;
inline constexpr uint32_t LightAccParamBytes = 208;
inline constexpr uint32_t DirLightSetBytes = 576;
inline constexpr uint32_t DirLightSetStride = 288;
inline constexpr uint32_t SceneParamBytes = 864;
inline constexpr uint32_t MaxDirLightSets = 8;
inline constexpr uint32_t MaxShadowLights = 8;

inline constexpr std::array<LightShader, 86> LightShaders{{
    {0x31b90cc5u, LightKind::Directional, 1, false, false, "DDirLight1"},
    {0xd92266b9u, LightKind::Directional, 1, true, false, "DDirLightShadowed1"},
    {0x02aee188u, LightKind::Line, 1, false, false, "DLineLight1"},
    {0xc37e6323u, LightKind::Line, 2, false, false, "DLineLight2"},
    {0x3b397883u, LightKind::Line, 3, false, false, "DLineLight3"},
    {0xd3143e39u, LightKind::Line, 4, false, false, "DLineLight4"},
    {0x370174f7u, LightKind::Point, 1, false, false, "DPointLight1"},
    {0x600f86b0u, LightKind::Point, 2, false, false, "DPointLight2"},
    {0x89f9ee3eu, LightKind::Point, 3, false, false, "DPointLight3"},
    {0x149432ffu, LightKind::Point, 4, false, false, "DPointLight4"},
    {0x5de04266u, LightKind::Point, 5, false, false, "DPointLight5"},
    {0x5d7150dau, LightKind::Point, 6, false, false, "DPointLight6"},
    {0x8d5387d9u, LightKind::Point, 7, false, false, "DPointLight7"},
    {0xf81596cdu, LightKind::Point, 8, false, false, "DPointLight8"},
    {0x0b2ace4cu, LightKind::Point, 9, false, false, "DPointLight9"},
    {0x201cb43du, LightKind::Point, 10, false, false, "DPointLight10"},
    {0x5f76eb3du, LightKind::Point, 11, false, false, "DPointLight11"},
    {0x799bfa32u, LightKind::Point, 12, false, false, "DPointLight12"},
    {0xf40da260u, LightKind::Point, 1, true, false, "DPointLightShadowed1"},
    {0xb5b52d63u, LightKind::Point, 2, true, false, "DPointLightShadowed2"},
    {0xbb2ac1cbu, LightKind::Point, 3, true, false, "DPointLightShadowed3"},
    {0x205c82b3u, LightKind::Point, 4, true, false, "DPointLightShadowed4"},
    {0xbae5454eu, LightKind::Spot, 1, false, false, "DSpotLight1"},
    {0x896e7242u, LightKind::Spot, 2, false, false, "DSpotLight2"},
    {0x02cd8f9bu, LightKind::Spot, 3, false, false, "DSpotLight3"},
    {0x8ea5be99u, LightKind::Spot, 4, false, false, "DSpotLight4"},
    {0x82951fa5u, LightKind::Spot, 1, true, false, "DSpotLightShadowed1"},
    {0x294503bau, LightKind::Spot, 2, true, false, "DSpotLightShadowed2"},
    {0x761d2013u, LightKind::Spot, 3, true, false, "DSpotLightShadowed3"},
    {0x376e0a37u, LightKind::Spot, 4, true, false, "DSpotLightShadowed4"},
    {0x7cc2c044u, LightKind::Point, 1, false, true, "LegacyDPointLight1"},
    {0x186d69d5u, LightKind::Point, 2, false, true, "LegacyDPointLight2"},
    {0xd22cbfd1u, LightKind::Point, 3, false, true, "LegacyDPointLight3"},
    {0x3105494cu, LightKind::Point, 4, false, true, "LegacyDPointLight4"},
    {0xd75fe2ffu, LightKind::Point, 5, false, true, "LegacyDPointLight5"},
    {0x07d07cf3u, LightKind::Point, 6, false, true, "LegacyDPointLight6"},
    {0xf2531727u, LightKind::Point, 7, false, true, "LegacyDPointLight7"},
    {0xc6edb19au, LightKind::Point, 8, false, true, "LegacyDPointLight8"},
    {0xb59b4c4eu, LightKind::Point, 9, false, true, "LegacyDPointLight9"},
    {0x505ebdbeu, LightKind::Point, 10, false, true, "LegacyDPointLight10"},
    {0x22b357f3u, LightKind::Point, 11, false, true, "LegacyDPointLight11"},
    {0x9f9ef267u, LightKind::Point, 12, false, true, "LegacyDPointLight12"},
    {0xe96447c3u, LightKind::Point, 1, true, true, "LegacyDPointLightShadowed1"},
    {0xa22c7f71u, LightKind::Point, 2, true, true, "LegacyDPointLightShadowed2"},
    {0x6b42fc89u, LightKind::Point, 3, true, true, "LegacyDPointLightShadowed3"},
    {0x0ef50047u, LightKind::Point, 4, true, true, "LegacyDPointLightShadowed4"},
    {0x6ab6cecbu, LightKind::Point, 1, false, true, "LegacyPointLight1"},
    {0xecda90f6u, LightKind::Point, 2, false, true, "LegacyPointLight2"},
    {0xd776fb27u, LightKind::Point, 3, false, true, "LegacyPointLight3"},
    {0xc023d70fu, LightKind::Point, 4, false, true, "LegacyPointLight4"},
    {0xb47e0972u, LightKind::Point, 5, false, true, "LegacyPointLight5"},
    {0xb3c63c01u, LightKind::Point, 6, false, true, "LegacyPointLight6"},
    {0x43d26ee3u, LightKind::Point, 7, false, true, "LegacyPointLight7"},
    {0xb464a1f2u, LightKind::Point, 8, false, true, "LegacyPointLight8"},
    {0xa422d554u, LightKind::Point, 9, false, true, "LegacyPointLight9"},
    {0x428a80bfu, LightKind::Point, 10, false, true, "LegacyPointLight10"},
    {0xd3a7b5a0u, LightKind::Point, 11, false, true, "LegacyPointLight11"},
    {0xe4daeaaeu, LightKind::Point, 12, false, true, "LegacyPointLight12"},
    {0x8aa6908eu, LightKind::Point, 1, true, true, "LegacyPointLightShadowed1"},
    {0x0a8a518bu, LightKind::Point, 2, true, true, "LegacyPointLightShadowed2"},
    {0x5e210809u, LightKind::Point, 3, true, true, "LegacyPointLightShadowed3"},
    {0x2994a496u, LightKind::Point, 4, true, true, "LegacyPointLightShadowed4"},
    {0x57a66dedu, LightKind::Point, 1, false, false, "PointLight1"},
    {0xdea6db05u, LightKind::Point, 2, false, false, "PointLight2"},
    {0xc3eb1d3au, LightKind::Point, 3, false, false, "PointLight3"},
    {0x42bd4804u, LightKind::Point, 4, false, false, "PointLight4"},
    {0xba81fc0au, LightKind::Point, 5, false, false, "PointLight5"},
    {0x3cd2ac34u, LightKind::Point, 6, false, false, "PointLight6"},
    {0x2aaa5cb1u, LightKind::Point, 7, false, false, "PointLight7"},
    {0xe1610d5bu, LightKind::Point, 8, false, false, "PointLight8"},
    {0x3aafbe57u, LightKind::Point, 9, false, false, "PointLight9"},
    {0xdb92e75cu, LightKind::Point, 10, false, false, "PointLight10"},
    {0x093a99d8u, LightKind::Point, 11, false, false, "PointLight11"},
    {0x65ff4c81u, LightKind::Point, 12, false, false, "PointLight12"},
    {0x98506debu, LightKind::Point, 1, true, false, "PointLightShadowed1"},
    {0x07aa7c3eu, LightKind::Point, 2, true, false, "PointLightShadowed2"},
    {0xea36a1b0u, LightKind::Point, 3, true, false, "PointLightShadowed3"},
    {0x40727870u, LightKind::Point, 4, true, false, "PointLightShadowed4"},
    {0xbcf67c52u, LightKind::Spot, 1, false, false, "SpotLight1"},
    {0xc12ac9e5u, LightKind::Spot, 2, false, false, "SpotLight2"},
    {0x46323550u, LightKind::Spot, 3, false, false, "SpotLight3"},
    {0x1ac8be70u, LightKind::Spot, 4, false, false, "SpotLight4"},
    {0x8e4d8f54u, LightKind::Spot, 1, true, false, "SpotLightShadowed1"},
    {0xe72d12aeu, LightKind::Spot, 2, true, false, "SpotLightShadowed2"},
    {0xc44db5d0u, LightKind::Spot, 3, true, false, "SpotLightShadowed3"},
    {0x7b33e949u, LightKind::Spot, 4, true, false, "SpotLightShadowed4"},
}};

inline const LightShader* FindLightShader(uint64_t ps_hash) {
    const uint32_t low = uint32_t(ps_hash);
    if ((ps_hash >> 32) != 0) {
        return nullptr;
    }
    for (const LightShader& s : LightShaders) {
        if (s.ps_hash == low) {
            return &s;
        }
    }
    return nullptr;
}

inline constexpr uint32_t NoLightTarget = 0xFFFFFFFFu;

struct LightDrawState {
    bool valid{};
    uint32_t diffuse{NoLightTarget};
    uint32_t specular{NoLightTarget};
    uint32_t width{}; // diffuse target width in pixels (0 = unknown)
    uint8_t mask0{};
    uint8_t mask1{};
    bool blend0{};
    bool blend1{};
    uint8_t src0{};
    uint8_t dst0{};
    uint8_t func0{};
    uint8_t src1{};
    uint8_t dst1{};
    uint8_t func1{};
    bool depth{};
    uint8_t zfunc{};
    bool bounds{};
    bool depth_write{};
    uint8_t cull{};
    float bounds_min{};
    float bounds_max{1.0f};
    float zscale{1.0f};
    float zoffset{};
    bool has_vtx{};
    float vtx_z{};
    bool operator==(const LightDrawState&) const = default;
};

struct Light {
    LightKind kind{LightKind::None};
    bool shadowed{};
    bool has_world{};
    std::array<float, 3> position{};
    std::array<float, 3> world{};
    std::array<float, 3> direction{};
    float radius{};
    std::array<float, 3> color{};
    uint32_t draw{};
    uint32_t index{};
    float spec_power{};
    std::array<float, 3> spec{};
    std::array<float, 2> wrap{};
    float spot_flag{};
    bool legacy{};
    std::array<float, 16> cone{};
    uint32_t stencil{};
    uint32_t age{};
    const char* shader_name{};
    LightDrawState state{};
    bool camera_fixed{};
    const char* gate{};
    // Found to be a real emitter by the fill-visibility pass (gets EmitterFalloff).
    bool emitter{};
    // Source size (m) of a converted emitter: the bounded inverse square s2 / (d2 + s2).
    float source{};
    // An enclosed lamp (glass housing, BB_RT_LAMP_ENCLOSED): its own reach, and no stock depth
    // test, which would clip that reach back to the game's sphere.
    bool lamp{};
    // A bright emitter whose reach grew with its intensity (BB_RT_EMITTER_BRIGHT): no stock
    // depth test either, for the same reason as a lamp.
    bool wide{};
    bool held{};
    // A glow lamp (post lamp, no game light) inside its glass: it does not light its own housing
    // (flag 16, Rt::HousingFade in the shaders).
    bool housed{};
};

inline constexpr uint32_t LightStencilEnable = 1u << 31;

constexpr uint32_t PackLightStencil(bool enable, uint32_t func, uint32_t ref, uint32_t mask) {
    return enable ? LightStencilEnable | ((func & 7u) << 16) | ((mask & 0xFFu) << 8) | (ref & 0xFFu)
                  : 0u;
}

inline float LightStrength(const Light& l) {
    const float lum = 0.2126f * l.color[0] + 0.7152f * l.color[1] + 0.0722f * l.color[2];
    return l.kind == LightKind::Directional ? lum * 1e6f : lum * l.radius;
}

inline bool Finite3(const std::array<float, 3>& v) {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

inline uint32_t LightDataStride(const LightShader& shader) {
    switch (shader.kind) {
    case LightKind::Point:
        return shader.shadowed ? 96 : 64;
    case LightKind::Spot:
        return shader.shadowed ? 144 : 128;
    default:
        return 0;
    }
}

inline uint32_t LightStride(const LightShader& shader, uint32_t buffer_bytes) {
    const uint32_t stride = LightDataStride(shader);
    if (stride == 0 || shader.count == 0 || buffer_bytes % 16 != 0 ||
        buffer_bytes < stride * shader.count) {
        return 0;
    }
    return stride;
}

inline uint64_t HashLightBytes(const void* data, uint32_t size, uint64_t seed) {
    const auto* p = static_cast<const unsigned char*>(data);
    uint64_t h = (seed + 0x9E3779B97F4A7C15ull) * 0xBF58476D1CE4E5B9ull ^ size;
    uint32_t i = 0;
    for (; i + 8 <= size; i += 8) {
        uint64_t w;
        std::memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 31;
    }
    uint64_t tail = 0;
    std::memcpy(&tail, p + i, size - i);
    h = (h ^ tail) * 0x94D049BB133111EBull;
    return h ^ (h >> 29);
}

struct LightDedup {
    std::unordered_set<uint64_t> seen;
    uint64_t skipped{};
    void Clear() {
        seen.clear();
    }
    bool Insert(uint64_t key) {
        if (seen.insert(key).second) {
            return true;
        }
        ++skipped;
        return false;
    }
};

inline uint32_t ParseLocalLights(const LightShader& shader, std::span<const float> data,
                                 uint32_t draw, std::vector<Light>& out,
                                 LightDedup* dedup = nullptr, uint64_t salt = 0) {
    if (shader.kind != LightKind::Point && shader.kind != LightKind::Spot) {
        return 0;
    }
    const uint32_t bytes = uint32_t(data.size() * sizeof(float));
    const uint32_t stride = LightStride(shader, bytes);
    if (stride == 0) {
        return 0;
    }
    const uint32_t floats = stride / 4;
    const uint64_t seed = (uint64_t(shader.kind) << 8 | uint64_t(shader.shadowed) << 4 |
                           uint64_t(shader.legacy)) ^
                          salt;
    uint32_t added = 0;
    for (uint32_t i = 0; i < shader.count; ++i) {
        const float* f = data.data() + i * floats;
        if (dedup && !dedup->Insert(HashLightBytes(f, stride, seed))) {
            continue;
        }
        Light l{};
        l.kind = shader.kind;
        l.shadowed = shader.shadowed;
        l.position = {f[0], f[1], f[2]};
        if (shader.kind == LightKind::Point) {
            l.radius = f[3] > 0.0f && std::isfinite(f[3]) ? 1.0f / std::sqrt(f[3]) : 0.0f;
        } else {
            l.radius = f[3];
        }
        l.color = {f[4], f[5], f[6]};
        l.spec_power = f[7];
        l.spec = {f[8], f[9], f[10]};
        l.wrap = {f[12], f[13]};
        l.spot_flag = f[15];
        l.legacy = shader.legacy;
        l.shader_name = shader.name;
        if (shader.kind == LightKind::Spot) {
            std::copy(f + 16, f + 32, l.cone.begin());
        }
        l.draw = draw;
        l.index = i;
        if (!Finite3(l.position) || !Finite3(l.color) || !std::isfinite(l.radius) ||
            l.radius <= 0.0f) {
            continue;
        }
        out.push_back(l);
        ++added;
    }
    return added;
}

inline bool ParseDirectionalLight(std::span<const float> data, uint32_t set, uint32_t draw,
                                  bool shadowed, Light& out) {
    const uint32_t base = set * (DirLightSetStride / 4);
    if (data.size() < base + 8) {
        return false;
    }
    const float* f = data.data() + base;
    Light l{};
    l.kind = LightKind::Directional;
    l.shadowed = shadowed;
    l.direction = {f[0], f[1], f[2]};
    l.color = {f[4], f[5], f[6]};
    l.radius = 1.0f;
    l.draw = draw;
    l.index = set;
    if (!Finite3(l.direction) || !Finite3(l.color)) {
        return false;
    }
    if (l.color[0] <= 0.0f && l.color[1] <= 0.0f && l.color[2] <= 0.0f) {
        return false;
    }
    const float len = std::sqrt(l.direction[0] * l.direction[0] +
                                l.direction[1] * l.direction[1] +
                                l.direction[2] * l.direction[2]);
    if (!(len > 1e-6f)) {
        return false;
    }
    for (float& d : l.direction) {
        d /= len;
    }
    out = l;
    return true;
}

inline uint32_t ParseDirectionalSets(std::span<const float> data, uint32_t draw, bool shadowed,
                                     std::vector<Light>& out) {
    const uint32_t sets = uint32_t(data.size() * 4 / DirLightSetStride);
    uint32_t added = 0;
    for (uint32_t set = 0; set < sets && added < MaxDirLightSets; ++set) {
        Light l{};
        if (ParseDirectionalLight(data, set, draw, shadowed, l)) {
            out.push_back(l);
            ++added;
        }
    }
    return added;
}

inline bool IsDirLightSetBuffer(const LightShader& shader, uint32_t size) {
    return shader.kind == LightKind::Directional && size >= DirLightSetStride &&
           size % DirLightSetStride == 0 && size != SceneParamBytes;
}

inline constexpr uint32_t MaxLightDataBytes = 12 * 144;

struct LightDrawRecord {
    const LightShader* shader{};
    uint32_t param_bytes{};
    uint32_t light_bytes{};
    uint32_t dir_bytes{};
    std::array<float, LightAccParamBytes / 4> param{};
    std::array<float, MaxLightDataBytes / 4> light_data{};
    std::vector<float> dir;
    std::array<uint32_t, 6> other_bytes{};
    uint32_t others{};
    bool dir_duplicate{};
    uint32_t stencil{};
    LightDrawState state{};
};

inline void ResetLightDrawRecord(LightDrawRecord& rec, const LightShader* shader) {
    rec.shader = shader;
    rec.param_bytes = 0;
    rec.light_bytes = 0;
    rec.dir_bytes = 0;
    rec.dir.clear();
    rec.others = 0;
    rec.dir_duplicate = false;
    rec.stencil = 0;
    rec.state = {};
}

inline uint32_t ApplyDrawStencil(const LightDrawRecord& rec, uint32_t draw, std::span<Light> lights) {
    uint32_t applied = 0;
    for (Light& l : lights) {
        if (l.draw == draw) {
            l.stencil = rec.stencil;
            ++applied;
        }
    }
    return applied;
}

inline uint64_t LightStateSalt(const LightDrawState& s) {
    if (!s.valid) {
        return 0;
    }
    const uint64_t h = (uint64_t(s.diffuse) << 32 | s.specular) * 0x9E3779B97F4A7C15ull;
    return (h ^ (uint64_t(s.mask0) << 4 | s.mask1) ^ (uint64_t(s.blend0) << 12 | uint64_t(s.blend1) << 13)) << 16;
}

inline uint32_t ApplyDrawState(const LightDrawRecord& rec, uint32_t draw, std::span<Light> lights) {
    uint32_t applied = 0;
    for (Light& l : lights) {
        if (l.draw == draw) {
            l.state = rec.state;
            ++applied;
        }
    }
    return applied;
}

struct LightTargets {
    uint32_t diffuse{NoLightTarget};
    uint32_t specular{NoLightTarget};
    uint32_t width{};
};

// The scene's light buffers: the widest target pair, then the one with the most light draws.
// Some areas also light a smaller capture view (its lights 90+ m from the camera) with more
// draws than the scene; a count alone then dropped every scene light, the held torch too.
inline LightTargets MainLightTargets(std::span<const LightDrawRecord> draws) {
    LightTargets best{};
    uint32_t best_count = 0;
    for (const LightDrawRecord& rec : draws) {
        const LightDrawState& a = rec.state;
        if (!a.valid || a.diffuse == NoLightTarget || a.specular == NoLightTarget) {
            continue;
        }
        uint32_t count = 0;
        for (const LightDrawRecord& r : draws) {
            count += r.state.valid && r.state.diffuse == a.diffuse && r.state.specular == a.specular;
        }
        if (a.width > best.width || (a.width == best.width && count > best_count)) {
            best_count = count;
            best = {a.diffuse, a.specular, a.width};
        }
    }
    return best;
}

inline constexpr uint8_t BlendFactorZero = 0;
inline constexpr uint8_t BlendFuncSubtract = 1;
inline constexpr uint8_t BlendFuncReverseSubtract = 4;
inline constexpr uint8_t CompareNever = 0;
inline constexpr uint8_t CompareAlways = 7;
inline constexpr uint32_t LightVtxParamBytes = 16;

struct StockDepthTest {
    bool active{};
    uint8_t func{CompareAlways};
    float quad{};
    bool bounds{};
    float bounds_min{};
    float bounds_max{1.0f};
};

inline StockDepthTest LightDepthTest(const LightDrawState& s) {
    StockDepthTest t{};
    if (!s.valid) {
        return t;
    }
    if (s.depth && s.has_vtx) {
        t.func = s.zfunc & 7u;
        t.quad = std::clamp(s.zoffset + s.zscale * s.vtx_z, 0.0f, 1.0f);
    }
    t.bounds = s.bounds;
    t.bounds_min = s.bounds_min;
    t.bounds_max = s.bounds_max;
    t.active = t.func != CompareAlways || t.bounds;
    return t;
}

inline bool CompareDepth(uint8_t func, float frag, float stored) {
    switch (func & 7u) {
    case 0:
        return false;
    case 1:
        return frag < stored;
    case 2:
        return frag == stored;
    case 3:
        return frag <= stored;
    case 4:
        return frag > stored;
    case 5:
        return frag != stored;
    case 6:
        return frag >= stored;
    default:
        return true;
    }
}

inline bool StockDepthPasses(const StockDepthTest& t, float stored) {
    if (!t.active) {
        return true;
    }
    if (t.bounds && (stored < t.bounds_min || stored > t.bounds_max)) {
        return false;
    }
    return CompareDepth(t.func, t.quad, stored);
}

struct LightGate {
    float diffuse{1.0f};
    float specular{1.0f};
    const char* reason{};
};

inline float ChannelGate(bool target, uint8_t mask, bool blend, uint8_t src, uint8_t func,
                         const char* off_target, const char* off_mask, const char* off_blend,
                         const char*& reason) {
    const char* why = !target            ? off_target
                      : (mask & 7u) == 0 ? off_mask
                      : blend && (src == BlendFactorZero || func == BlendFuncSubtract ||
                                 func == BlendFuncReverseSubtract) ? off_blend
                                                                                   : nullptr;
    if (why && !reason) {
        reason = why;
    }
    return why ? 0.0f : 1.0f;
}

inline LightGate StockLightGate(const LightDrawState& s, LightTargets main) {
    LightGate g{};
    if (!s.valid || main.diffuse == NoLightTarget) {
        return g;
    }
    if (s.depth && s.zfunc == CompareNever) {
        return {0.0f, 0.0f, "depth test never passes"};
    }
    const bool d = s.diffuse == main.diffuse;
    const bool sp = s.specular == main.specular;
    if (!d && !sp) {
        return {0.0f, 0.0f, "drawn to another render target"};
    }
    g.diffuse = ChannelGate(d, s.mask0, s.blend0, s.src0, s.func0, "diffuse to another target",
                            "diffuse colour mask 0", "diffuse blend not additive", g.reason);
    g.specular = ChannelGate(sp, s.mask1, s.blend1, s.src1, s.func1, "specular to another target",
                             "specular colour mask 0", "specular blend not additive", g.reason);
    return g;
}

inline constexpr const char* FillLightOffReason = "camera-fixed light off (BB_RT_FILL_LIGHT=off)";
inline constexpr const char* SpecOnlyLightOffReason =
    "specular-only light off (no diffuse colour, BB_RT_FILL_LIGHT=off)";

inline uint32_t ApplyStockGate(std::vector<Light>& lights, LightTargets main, bool gate_on,
                               bool fill_off, std::vector<Light>* removed_out = nullptr) {
    uint32_t gated = 0;
    std::erase_if(lights, [&](Light& l) {
        l.gate = nullptr;
        if (l.kind == LightKind::Directional) {
            return false;
        }
        LightGate g = gate_on ? StockLightGate(l.state, main) : LightGate{};
        if (fill_off && l.camera_fixed) {
            g = {0.0f, 0.0f, FillLightOffReason};
        } else if (fill_off && l.color[0] == 0.0f && l.color[1] == 0.0f && l.color[2] == 0.0f) {
            g = {0.0f, 0.0f, SpecOnlyLightOffReason};
        }
        l.gate = g.reason;
        if (!g.reason) {
            return false;
        }
        ++gated;
        for (float& c : l.color) {
            c *= g.diffuse;
        }
        for (float& c : l.spec) {
            c *= g.specular;
        }
        if (g.diffuse == 0.0f && g.specular == 0.0f) {
            if (removed_out) {
                removed_out->push_back(l);
            }
            return true;
        }
        return false;
    });
    return gated;
}

struct FixedLightTrack {
    std::array<float, 3> view{};
    std::array<float, 3> world{};
    std::array<float, 3> color{};
    float radius{};
    uint32_t moved{};
};

inline constexpr float FixedViewEps = 0.01f;
inline constexpr float FixedWorldMove = 0.02f;
inline constexpr uint32_t FixedMovedFrames = 3;
inline constexpr float FixedMaxDistance = 8.0f;

inline constexpr float PlayerLightMaxRadius = 10.0f;
inline constexpr float PlayerLightMaxX = 1.5f;
inline constexpr float PlayerLightMaxY = 5.0f;
inline constexpr float PlayerLightMinZ = 1.5f;
inline constexpr float PlayerLightMaxZ = 8.0f;
inline constexpr float PlayerLightGrey = 0.02f;

inline bool Greyish(const std::array<float, 3>& c) {
    const float hi = std::max({c[0], c[1], c[2]});
    const float lo = std::min({c[0], c[1], c[2]});
    return hi > 0.0f && hi - lo <= PlayerLightGrey * hi + 1e-4f;
}

inline bool PlayerLightSignature(const Light& l) {
    return l.kind == LightKind::Point && !l.shadowed && l.radius > 0.0f &&
           l.radius <= PlayerLightMaxRadius && std::fabs(l.position[0]) <= PlayerLightMaxX &&
           std::fabs(l.position[1]) <= PlayerLightMaxY && l.position[2] >= PlayerLightMinZ &&
           l.position[2] <= PlayerLightMaxZ && Greyish(l.color) && Greyish(l.spec);
}

struct CameraFixedCount {
    uint32_t total{};
    uint32_t signature{};
    uint32_t tracked{};
};

inline CameraFixedCount MarkCameraFixed(std::span<Light> lights,
                                        std::vector<FixedLightTrack>& tracks) {
    std::vector<FixedLightTrack> next;
    CameraFixedCount count{};
    for (Light& l : lights) {
        l.camera_fixed = PlayerLightSignature(l);
        count.signature += l.camera_fixed ? 1u : 0u;
        if (l.kind == LightKind::Directional || !l.has_world ||
            !(l.position[0] * l.position[0] + l.position[1] * l.position[1] +
                  l.position[2] * l.position[2] <
              FixedMaxDistance * FixedMaxDistance)) {
            count.total += l.camera_fixed ? 1u : 0u;
            continue;
        }
        FixedLightTrack t{l.position, l.world, l.color, l.radius, 0};
        for (const FixedLightTrack& p : tracks) {
            float dv = 0.0f, dw = 0.0f, dc = std::fabs(p.radius - l.radius);
            for (uint32_t i = 0; i < 3; ++i) {
                dv = std::max(dv, std::fabs(p.view[i] - l.position[i]));
                dw = std::max(dw, std::fabs(p.world[i] - l.world[i]));
                dc = std::max(dc, std::fabs(p.color[i] - l.color[i]));
            }
            if (dv <= FixedViewEps && dc <= 1e-3f) {
                t.moved = std::min(p.moved + (dw > FixedWorldMove ? 1u : 0u), 1000u);
                break;
            }
        }
        if (t.moved >= FixedMovedFrames) {
            ++count.tracked;
            l.camera_fixed = true;
        }
        count.total += l.camera_fixed ? 1u : 0u;
        next.push_back(t);
    }
    tracks.swap(next);
    return count;
}

enum class LightBufferRole : uint8_t { Rejected, Param, Dir, Local, Other };

inline LightBufferRole RecordLightBuffer(LightDrawRecord& rec, uint32_t draw, uint32_t size,
                                         const void* data, std::vector<Light>& out,
                                         LightDedup* dedup = nullptr) {
    if (!rec.shader || !data || size < 16) {
        return LightBufferRole::Rejected;
    }
    const LightShader& shader = *rec.shader;
    const uint32_t floats = size / 4;
    if (size == LightAccParamBytes && rec.param_bytes == 0) {
        rec.param_bytes = size;
        std::memcpy(rec.param.data(), data, size);
        return LightBufferRole::Param;
    }
    if (rec.dir_bytes == 0 && !rec.dir_duplicate && IsDirLightSetBuffer(shader, size)) {
        if (dedup &&
            !dedup->Insert(HashLightBytes(data, size, 0xD1ull << 8 | uint64_t(shader.shadowed)))) {
            rec.dir_duplicate = true;
            return LightBufferRole::Dir;
        }
        rec.dir_bytes = size;
        rec.dir.resize(floats);
        std::memcpy(rec.dir.data(), data, size_t(floats) * 4);
        ParseDirectionalSets(rec.dir, draw, shader.shadowed, out);
        return LightBufferRole::Dir;
    }
    if (rec.light_bytes == 0 && LightStride(shader, size) != 0 &&
        (shader.kind == LightKind::Point || shader.kind == LightKind::Spot ||
         shader.kind == LightKind::Line)) {
        const uint32_t used =
            std::min(LightStride(shader, size) * shader.count, MaxLightDataBytes);
        rec.light_bytes = used;
        std::memcpy(rec.light_data.data(), data, used);
        ParseLocalLights(shader, std::span<const float>(rec.light_data.data(), used / 4), draw,
                         out, dedup, LightStateSalt(rec.state));
        return LightBufferRole::Local;
    }
    if (rec.others < rec.other_bytes.size()) {
        rec.other_bytes[rec.others] = size;
    }
    ++rec.others;
    return LightBufferRole::Other;
}

inline constexpr uint32_t CameraMatrixFloat = 32;

inline uint32_t ApplyCameraMatrix(const LightDrawRecord& rec, uint32_t draw,
                                  std::span<Light> lights) {
    if (rec.param_bytes < (CameraMatrixFloat + 12) * 4) {
        return 0;
    }
    const float* m = rec.param.data() + CameraMatrixFloat;
    for (uint32_t r = 0; r < 12; ++r) {
        if (!std::isfinite(m[r])) {
            return 0;
        }
    }
    uint32_t applied = 0;
    for (Light& l : lights) {
        if (l.draw != draw || l.kind == LightKind::Directional) {
            continue;
        }
        for (uint32_t r = 0; r < 3; ++r) {
            const float* row = m + r * 4;
            l.world[r] = row[0] * l.position[0] + row[1] * l.position[1] +
                         row[2] * l.position[2] + row[3];
        }
        l.has_world = Finite3(l.world);
        applied += l.has_world ? 1 : 0;
    }
    return applied;
}

inline bool ShadowCandidate(const Light& l) {
    return l.kind == LightKind::Point && !l.shadowed && l.has_world && l.radius > 0.0f;
}

inline bool NearRel(float a, float b, float rel, float abs_eps) {
    return std::fabs(a - b) <= std::max(abs_eps, rel * std::max(std::fabs(a), std::fabs(b)));
}

inline bool SameLight(const Light& a, const Light& b) {
    for (uint32_t i = 0; i < 3; ++i) {
        if (!NearRel(a.world[i], b.world[i], 0.0f, 0.01f) ||
            !NearRel(a.color[i], b.color[i], 0.01f, 1e-4f)) {
            return false;
        }
    }
    return NearRel(a.radius, b.radius, 0.01f, 1e-4f);
}

inline float ShadowScore(const Light& l) {
    const float d = std::sqrt(l.position[0] * l.position[0] + l.position[1] * l.position[1] +
                              l.position[2] * l.position[2]);
    const float gap = std::isfinite(d) ? std::max(0.0f, d - l.radius) : 1e6f;
    return LightStrength(l) / (1.0f + gap * gap);
}

inline void SelectShadowLights(std::vector<Light>& lights, uint32_t max_lights) {
    std::erase_if(lights, [](const Light& l) { return !ShadowCandidate(l); });
    std::stable_sort(lights.begin(), lights.end(), [](const Light& a, const Light& b) {
        return ShadowScore(a) > ShadowScore(b);
    });
    size_t kept = 0;
    for (size_t i = 0; i < lights.size() && kept < max_lights; ++i) {
        bool duplicate = false;
        for (size_t k = 0; k < kept && !duplicate; ++k) {
            duplicate = SameLight(lights[k], lights[i]);
        }
        if (!duplicate) {
            lights[kept++] = lights[i];
        }
    }
    lights.resize(kept);
}

struct ShadowTrack {
    Light light{};
    float weight{};
    bool selected{};
    uint32_t missing{};
};

inline constexpr float ShadowKeepBonus = 1.5f;
inline constexpr float ShadowFadeStep = 0.1f;
inline constexpr uint32_t ShadowMissingFrames = 8;
inline constexpr float ShadowMatchDistance = 0.5f;

inline float WorldDistance2(const Light& a, const Light& b) {
    float d2 = 0.0f;
    for (uint32_t i = 0; i < 3; ++i) {
        const float d = a.world[i] - b.world[i];
        d2 += d * d;
    }
    return d2;
}

inline bool SameSource(const Light& a, const Light& b) {
    return WorldDistance2(a, b) <= ShadowMatchDistance * ShadowMatchDistance &&
           NearRel(a.radius, b.radius, 0.25f, 1e-3f);
}

inline void UpdateShadowTracks(std::vector<Light> candidates, uint32_t max_lights,
                               std::vector<ShadowTrack>& tracks) {
    SelectShadowLights(candidates, 2 * MaxShadowLights);
    std::vector<int> match(candidates.size(), -1);
    std::vector<bool> used(tracks.size(), false);
    std::vector<float> score(candidates.size());
    for (size_t i = 0; i < candidates.size(); ++i) {
        float best = std::numeric_limits<float>::max();
        for (size_t t = 0; t < tracks.size(); ++t) {
            if (used[t] || !SameSource(candidates[i], tracks[t].light)) {
                continue;
            }
            const float d2 = WorldDistance2(candidates[i], tracks[t].light);
            if (d2 < best) {
                best = d2;
                match[i] = int(t);
            }
        }
        const bool kept = match[i] >= 0 && tracks[size_t(match[i])].selected;
        if (match[i] >= 0) {
            used[size_t(match[i])] = true;
        }
        score[i] = ShadowScore(candidates[i]) * (kept ? ShadowKeepBonus : 1.0f);
    }
    std::vector<size_t> order(candidates.size());
    for (size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::stable_sort(order.begin(), order.end(),
                     [&](size_t a, size_t b) { return score[a] > score[b]; });
    std::vector<ShadowTrack> next;
    for (size_t r = 0; r < order.size(); ++r) {
        const size_t i = order[r];
        const bool selected = r < max_lights;
        const float prev = match[i] >= 0 ? tracks[size_t(match[i])].weight : 0.0f;
        const float weight = selected ? std::min(1.0f, prev + ShadowFadeStep)
                                      : std::max(0.0f, prev - ShadowFadeStep);
        if (selected || weight > 0.0f) {
            next.push_back({candidates[i], weight, selected, 0});
        }
    }
    for (size_t t = 0; t < tracks.size(); ++t) {
        if (!used[t] && tracks[t].missing < ShadowMissingFrames && tracks[t].weight > 0.0f) {
            ShadowTrack kept = tracks[t];
            ++kept.missing;
            next.push_back(kept);
        }
    }
    tracks.swap(next);
}

inline void ShadowDrawLights(const std::vector<ShadowTrack>& tracks, std::vector<Light>& out) {
    out.clear();
    std::vector<const ShadowTrack*> live;
    for (const ShadowTrack& t : tracks) {
        if (t.missing == 0 && t.weight > 0.0f) {
            live.push_back(&t);
        }
    }
    std::stable_sort(live.begin(), live.end(), [](const ShadowTrack* a, const ShadowTrack* b) {
        return a->selected != b->selected ? a->selected : a->weight > b->weight;
    });
    for (size_t i = 0; i < live.size() && i < MaxShadowLights; ++i) {
        Light l = live[i]->light;
        for (float& c : l.color) {
            c *= live[i]->weight;
        }
        out.push_back(l);
    }
}

struct ShadowLightGpu {
    std::array<float, 4> pos_inv_r2;
    std::array<float, 4> color;
};

struct ShadowParams {
    std::array<float, 4> proj;
    std::array<std::array<float, 4>, 3> view_rows;
    std::array<float, 4> size_jitter;
    std::array<uint32_t, 4> tlas_count_flags;
    std::array<uint32_t, 4> cfg;
    std::array<float, 4> strength_bias;
    std::array<ShadowLightGpu, MaxShadowLights> lights;
};
static_assert(sizeof(ShadowParams) == 128 + 32 * MaxShadowLights);

constexpr uint32_t ShadowFlagAlbedo = 1;
constexpr uint32_t ShadowFlagDebug = 2;
constexpr uint32_t ShadowFlagSpecular = 4;

inline ShadowLightGpu PackShadowLight(const Light& l, const Affine& view) {
    const std::array<float, 3> p = Transform(view, l.world);
    return {{p[0], p[1], p[2], 1.0f / (l.radius * l.radius)}, {l.color[0], l.color[1], l.color[2], 0.0f}};
}

// Fill-light visibility (BB_RT_FILL_VIS, rt_gi.comp RT_LIGHT_VIS): the game lights interiors with
// designer point/spot lights that have no emitter. A light keeps its strength where traced rays
// from it reach the sky or a real emitter; elsewhere it fades towards a floor.
inline constexpr uint32_t FillVisBatch = 64; // lights per dispatch, one workgroup each (shader)
inline constexpr uint32_t FillVisOutputs = 3; // result vec4s per light (shader VisOutputs)
inline constexpr float FillVisCell = 0.25f;  // cache grid, metres

// Raw per-light result of the rt_gi.comp RT_LIGHT_VIS pass.
struct FillVisResult {
    float sky{};          // fraction of rays reaching the sky (blended-attenuated)
    float emit{};         // fraction of rays with emission above the threshold within reach
    float emit_luma{};    // max emission luma within reach
    float near_solid{};   // nearest solid hit, metres
    float enclosed{};     // fraction of rays meeting solid or blended geometry within reach
    float blended{};      // fraction of rays meeting blended geometry (glass) within reach
    float near_blended{}; // nearest blended hit, metres
    float screen_luma{};  // max emissive-target luma on screen around the light
    bool visible{};       // light position on screen and not hidden: this frame's TLAS holds
                          // the geometry drawn around it, so the result can be trusted
    float depth_gap{};    // scene depth minus light depth at its pixel
    float enclosed_up{};  // enclosed fraction of the upward rays (world y > 0.5)
};

// A lamp is much worse to lose than a fill light is to keep, so any one sign counts: emission
// at a hit within reach, emission on screen around the light, or a light inside a fixture
// (housing, glass, candle) that encloses at least enclosed_min of its rays.
inline bool FillVisEmitter(const FillVisResult& r, float emit_luma, float enclosed_min) {
    return r.emit > 0.0f || r.screen_luma > emit_luma || r.enclosed >= enclosed_min;
}

// A lamp (BB_RT_LAMP_*: far reach, no stock depth test) is a light in a housing: enclosed all
// round (BB_RT_LAMP_ENCLOSED) and capped above (BB_RT_LAMP_UP of the up rays). A fire pit's
// log pile and the ground enclose as much (56-58%) but its top is open (24-29% up), and as a
// lamp it lit the forest 19 m out at 9x the game's colour.
inline bool FillVisLamp(const FillVisResult& r, float enclosed_min, float up_min) {
    return enclosed_min > 0.0f && r.enclosed >= enclosed_min && r.enclosed_up >= up_min;
}

// A fire pit: as enclosed as a lamp but open above, the light sitting in the log pile (nearest
// solid 0.03 m). Its shadow rays all ended in the sticks, so the ground around it stayed dark
// where the game draws a lit pool; BB_RT_FIRE_LIFT (0.5 m) moves the light up out of the pile.
inline bool FillVisFirePit(const FillVisResult& r, float enclosed_min, float up_min) {
    return enclosed_min > 0.0f && r.enclosed >= enclosed_min && r.enclosed_up < up_min;
}

constexpr float WarmEmitterRed = 1.0f;
constexpr float WarmEmitterBlue = 0.6f;
constexpr float HeldFlameDistance = 1.0f;
constexpr float HeldCameraDistance = 6.0f;

constexpr float LanternGreen = 0.7f;
constexpr float LanternBlue = 0.4f;
// The lamps the warm rule is for are a pale warm (blue 0.5 x red: 2.000 1.356 0.997). The
// Hunter's Dream's designer key fills are a saturated amber (2.000 1.243 0.352, 2.000 1.110
// 0.603: blue 0.18 and 0.30 x red), the mirror of its blue fills (0.586 1.243 2.000), steady and
// with nothing around them. Bigger than lamp-bright, a light is a fire (the burning house's
// 12.800 5.039 0.806) and needs no hue.
constexpr float WarmLampBlue = 0.4f;
constexpr float WarmFirePeak = 3.0f;

// A flame is clearly redder than green (game flames: green <= 0.76 x red). Loot lights are
// neutral (0.817 0.817 0.636) yet move with the corpse, glow on screen and sit enclosed,
// and the loot wisps use the fire FX shader, so without this they became carried or held
// torches: orange, flickering and swaying.
constexpr float FlameGreenMax = 0.9f;
// Loot wisps are drawn with the fire FX shader too, but their particles are pale (ffe6be):
// every channel close to the brightest. Fire particles are orange.
constexpr float PaleParticleMin = 0.6f;
inline bool PaleParticle(uint32_t rgb) {
    const uint32_t c[3] = {rgb & 0xFFu, (rgb >> 8) & 0xFFu, (rgb >> 16) & 0xFFu};
    const uint32_t hi = std::max(c[0], std::max(c[1], c[2]));
    const uint32_t lo = std::min(c[0], std::min(c[1], c[2]));
    return hi > 0 && float(lo) >= PaleParticleMin * float(hi);
}

inline bool FlameColour(const Light& l) {
    return l.color[0] > 0.0f && l.color[1] <= FlameGreenMax * l.color[0];
}

inline bool WarmEmitter(const Light& l, float lamp_blue = WarmLampBlue) {
    const float r = l.color[0];
    if (r >= WarmEmitterRed && l.color[2] < WarmEmitterBlue * r &&
        (l.color[2] >= lamp_blue * r || r > WarmFirePeak)) {
        return true;
    }
    return r > 0.0f && l.color[1] >= LanternGreen * r && l.color[2] <= LanternBlue * r;
}

constexpr float LampMinColour = 2.0f;

inline bool LampFloor(Light& l, float min_sum) {
    const float sum = l.color[0] + l.color[1] + l.color[2];
    if (min_sum <= 0.0f || sum <= 0.0f || sum >= min_sum || l.color[0] <= l.color[2]) {
        return false;
    }
    const float k = min_sum / sum;
    for (float& c : l.color) {
        c *= k;
    }
    for (float& c : l.spec) {
        c *= k;
    }
    return true;
}

inline bool FillVisCandidate(const Light& l) {
    return (l.kind == LightKind::Point || l.kind == LightKind::Spot) && l.has_world &&
           !l.camera_fixed && l.radius > 0.0f && Finite3(l.world);
}

// Cache key: world position on a 0.25 m grid (20 bits per axis, +-131 km) and the light kind.
inline uint64_t FillVisKey(const Light& l) {
    const auto q = [](float v) {
        return uint64_t(int64_t(std::llround(v / FillVisCell)) & 0xFFFFF);
    };
    return q(l.world[0]) | q(l.world[1]) << 20 | q(l.world[2]) << 40 | uint64_t(l.kind) << 60;
}

// Strength factor: 1 at an emitter, else floor + (1 - floor) * smoothstep(0, sky_full, sky).
inline float FillVisScale(float sky, bool emitter, float sky_full, float floor) {
    if (emitter) {
        return 1.0f;
    }
    const float f = std::clamp(floor, 0.0f, 1.0f);
    const float x = sky_full > 0.0f ? std::clamp(sky / sky_full, 0.0f, 1.0f) : (sky > 0.0f ? 1.0f : 0.0f);
    return f + (1.0f - f) * (x * x * (3.0f - 2.0f * x));
}

// Flame flicker: the game animates a flame's colour by about +-12% a frame. The colour is first
// smoothed (EMA, weight smooth per frame: no frame-to-frame strobe), then gain scales its
// deviation from the slow running mean (weight alpha per frame), so gain 1 keeps the game's
// (smoothed) flicker and gain 2 doubles it. State per light; mean 0 = new light. Returns the
// scale for colour and spec, clamped to [0.25, 2.5] of the mean.
struct FlickerState {
    float mean{};
    float slow{};
    float dev{}; // running mean of |lum / mean - 1|: how much the game itself flickers it
};
inline float FlickerScale(float lum, FlickerState& s, float gain, float alpha = 0.1f,
                          float smooth = 1.0f) {
    if (!(lum > 0.0f) || !std::isfinite(lum)) return 1.0f;
    if (s.mean > 0.0f) {
        s.mean += alpha * (lum - s.mean);
        s.slow += smooth * (lum - s.slow);
        s.dev += alpha * (std::fabs(lum / s.mean - 1.0f) - s.dev);
    } else {
        s.mean = s.slow = lum;
    }
    if (gain == 1.0f && smooth >= 1.0f) return 1.0f;
    return std::clamp(1.0f + gain * (s.slow / s.mean - 1.0f), 0.25f, 2.5f) * s.mean / lum;
}

// A flame's own flicker for lights the game keeps steady: smooth irregular noise in [-1, 1],
// three incommensurate sines (0.25-0.6, 0.6-1.0, 1.0-1.6 Hz, weights 0.5 / 0.3 / 0.2), phases
// and rates from the light's id; std ~0.44. Used as exp(amp x noise): dips as gentle as flares.
inline float FlameNoise(uint64_t id, double t) {
    uint64_t h = (id + 0x51ED27ull) * 0xD6E8FEB86659FD93ull;
    const auto next = [&] {
        h ^= h >> 31;
        h *= 0xBF58476D1CE4E5B9ull;
        h ^= h >> 29;
        return float(h & 0xFFFFFF) / float(0x1000000);
    };
    constexpr float TwoPi = 6.28318531f;
    constexpr float lo[3] = {0.25f, 0.6f, 1.0f}, span[3] = {0.35f, 0.4f, 0.6f},
                    w[3] = {0.5f, 0.3f, 0.2f};
    float v = 0.0f;
    for (int k = 0; k < 3; ++k) {
        const float f = lo[k] + span[k] * next();
        v += w[k] * std::sin(float(std::fmod(t * f, 1.0)) * TwoPi + TwoPi * next());
    }
    return v;
}

// A glow capture is a lamp only once it has been seen enough frames at a steady spot (lamps never
// move; blood echoes and other effects drawn with the lamp-glass shader travel) and is warm
// (item glows are white). drift: the capture's running mean distance from its average (m).
inline bool GlowLampSteady(uint32_t hits, float drift, const std::array<float, 3>& color,
                           uint32_t min_hits, float max_drift, float warm_max) {
    return hits >= min_hits && drift <= max_drift && color[0] > 0.0f &&
           color[2] <= warm_max * color[0];
}

// Seed for FlameNoise of a light with no track (glow lamps: the game draws the lit glass but
// gives no light). From the world position on a 2 m grid, so the same lamp keeps its flicker
// while its centroid jitters; the top bit keeps it apart from track ids.
inline uint64_t PositionFlickerId(const std::array<float, 3>& world) {
    uint64_t h = 0x8000000000000000ull;
    for (const float v : world) {
        const int64_t c = std::isfinite(v) ? int64_t(std::floor(v * 0.5f)) : 0;
        h = (h ^ uint64_t(c)) * 0x100000001B3ull;
    }
    return h | 0x8000000000000000ull;
}

// A flame sways: an offset (m) of a smooth irregular wander per light, two incommensurate sines
// per axis (0.4-1.3 Hz) with phases from the light's id, vertical half as wide. t in seconds.
inline std::array<float, 3> FlameSway(uint64_t id, double t, float amp) {
    std::array<float, 3> o{};
    if (!(amp > 0.0f)) return o;
    uint64_t h = id * 0x9E3779B97F4A7C15ull;
    const auto next = [&] {
        h ^= h >> 31;
        h *= 0xBF58476D1CE4E5B9ull;
        h ^= h >> 29;
        return float(h & 0xFFFFFF) / float(0x1000000);
    };
    constexpr float TwoPi = 6.28318531f;
    for (int a = 0; a < 3; ++a) {
        const float f1 = 0.4f + 0.5f * next(), f2 = 0.8f + 0.5f * next();
        const float p1 = TwoPi * next(), p2 = TwoPi * next();
        const float v = 0.6f * std::sin(float(std::fmod(t * f1, 1.0)) * TwoPi + p1) +
                        0.4f * std::sin(float(std::fmod(t * f2, 1.0)) * TwoPi + p2);
        o[a] = v * amp * (a == 1 ? 0.5f : 1.0f);
    }
    return o;
}

// Real emitters (lamps, candles) keep the game's colour but lose its hard cutoff: the legacy
// falloff (1 - d/r)^2 ends at r (2 m for a candle), so nothing past it is lit or can bounce the
// light on. The light becomes a bounded inverse square, s2 / (d2 + s2) (shifted to 0 at
// R = r * range_mul), with source size s = r / 4: 1 at the fixture like the game's peak, close to
// the legacy curve inside r ((1 - d/r)^2 vs s2/(d2+s2) at d = r/2: 0.25 vs 0.2) and 1/d2 beyond,
// so it never outshines the game's own light near the fixture. The shaders read flag 4 and s2.
// source_frac sets s = r * source_frac (smaller = faster fall near the fixture, dimmer far).
// lift moves the light up (world +y) from the game's anchor in the holder to the flame.
// peak scales the light at the fixture; the source size grows by 1/sqrt(peak) so the far field
// (peak * s2 / d2) stays the same: a lower peak only dims the holder right next to the anchor.
// flame > 0 (m): a flame is a small source. The far field (intensity = colour x s2) is kept and the
// source shrinks to the flame, so the light falls as 1/d2 from a hot spot instead of a soft blob
// metres wide (the 2.7 m source of a hand torch read as a generic glow).
// range_mul <= 1 leaves the light unchanged.
inline bool EmitterFalloff(Light& l, float range_mul, float source_frac = 0.25f,
                           float lift = 0.0f, float peak = 1.0f, float flame = 0.0f) {
    if (range_mul <= 1.0f || !l.legacy || l.kind != LightKind::Point || !(l.radius > 0.0f)) {
        return false;
    }
    peak = std::clamp(peak, 0.01f, 1.0f);
    l.source = std::max(source_frac, 0.01f) * l.radius / std::sqrt(peak);
    float gain = peak;
    if (flame > 0.0f && flame < l.source) {
        gain *= (l.source * l.source) / (flame * flame);
        l.source = flame;
    }
    for (float& c : l.color) {
        c *= gain;
    }
    for (float& c : l.spec) {
        c *= gain;
    }
    l.world[1] += lift;
    l.radius *= range_mul;
    l.legacy = false;
    return true;
}


inline void SelectStrongest(std::vector<Light>& lights, uint32_t max_local) {
    std::stable_sort(lights.begin(), lights.end(), [](const Light& a, const Light& b) {
        return LightStrength(a) > LightStrength(b);
    });
    uint32_t local = 0;
    std::erase_if(lights, [&](const Light& l) {
        if (l.kind == LightKind::Directional) {
            return false;
        }
        return local++ >= max_local;
    });
}

}

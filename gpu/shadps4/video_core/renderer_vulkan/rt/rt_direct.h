#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <utility>
#include <limits>
#include <span>
#include <unordered_set>
#include <vector>

#include "rt_lights.h"

namespace Vulkan::Rt {

inline constexpr uint32_t MaxDirectLights = 1024;
inline constexpr uint32_t DirectTile = 16;
inline constexpr uint32_t MaxTileLights = MaxDirectLights;
inline constexpr uint32_t MaxDirectRays = 64;
inline constexpr float DirectRayCutoff = 0.01f;
inline constexpr uint32_t DirectSetBytes = 225 * DirLightSetStride;
inline constexpr float DirectFormatMax = 64512.0f;

constexpr uint32_t DirectFlagNoVis = 1;
constexpr uint32_t DirectFlagSpec = 2;
constexpr uint32_t DirectFlagG1 = 4;
constexpr uint32_t DirectFlagDirSets = 8;
constexpr uint32_t DirectFlagRestirReset = 16;
constexpr uint32_t DirectFlagAdd = 32;
constexpr uint32_t DirectFlagShadowMask = 64;
constexpr uint32_t DirectFlagMoonMask = 128;
constexpr uint32_t DirectFlagMoonStatic = 256;
constexpr uint32_t DirectFlagMoonOwnSet = 512;
constexpr uint32_t DirectFlagPortals = 1024; // portal panes in the TLAS this frame
constexpr uint32_t DirectFlagMoonHalf = 2048;
constexpr uint32_t DirectFlagMoonHalfAlt = 4096;
constexpr uint32_t DirectFlagWaterKeep = 8192;
constexpr uint32_t DirectFlagMoonHistory = 16384;      // rt_direct.comp MoonHistory runs
constexpr uint32_t DirectFlagMoonHistoryValid = 32768; // last frame's history fits this camera
constexpr uint32_t DirectFlagObjectMotion = 65536;     // binding 19 holds this frame's object motion
// rt_direct.comp KeptProbe: grid camera rays report what lies in front of the drawn surface in
// pick ring word KeptProbeBase (count) and the KeptProbeMax words after it (TLAS indices).
constexpr uint32_t DirectFlagKeptProbe = 131072;
constexpr uint32_t KeptProbeBase = 128;
constexpr uint32_t KeptProbeMax = 127;
constexpr uint32_t KeptProbeBytes = (KeptProbeBase + 1 + KeptProbeMax) * 4;

constexpr uint32_t MoonHalfFlags(uint32_t knob, bool moon_mask, uint32_t debug) {
    if (knob == 0 || moon_mask || debug != 0) {
        return 0;
    }
    return DirectFlagMoonHalf | (knob >= 2 ? DirectFlagMoonHalfAlt : 0u);
}

constexpr bool MoonHalfTraces(uint32_t x, uint32_t y, uint32_t frame, uint32_t flags) {
    const uint32_t parity = (flags & DirectFlagMoonHalfAlt) != 0 ? (frame & 1u) : 0u;
    return ((x + y + parity) & 1u) == 0u;
}
inline constexpr uint32_t DirShadowedShader = 0xd92266b9u;
inline constexpr uint32_t ShadowColorOffset = 304;
inline constexpr uint32_t NoLight = 0xFFFFFFFFu;
inline constexpr uint32_t RestirMaxCandidates = 16;
inline constexpr uint32_t RestirMaxHistory = 64;
inline constexpr uint32_t RestirMaxSpatial = 16;
inline constexpr uint32_t ReservoirBytes = 16;

struct GpuSetSource {
    uint64_t address{};
    uint32_t bytes{};
    uint64_t frame{~0ull};
};

constexpr bool GpuLightSetsFresh(const GpuSetSource& s, uint64_t frame, uint64_t max_age) {
    return s.address != 0 && s.frame <= frame && frame - s.frame <= max_age &&
           s.bytes >= DirLightSetStride && s.bytes <= DirectSetBytes &&
           s.bytes % DirLightSetStride == 0 && s.bytes != SceneParamBytes;
}

constexpr bool GpuLightSetsThisFrame(const GpuSetSource& s, uint64_t frame) {
    return GpuLightSetsFresh(s, frame, 0);
}

inline constexpr uint64_t GiLightSetsMaxAge = 1;

inline constexpr uint32_t DirLogGpu = 1;
inline constexpr uint32_t DirLogAdd = 2;
inline constexpr uint32_t DirLogMask = 4;
inline constexpr uint32_t DirLogMoonMask = 8;
inline constexpr uint32_t DirLogMoonStatic = 16;

struct SetReadback {
    uint64_t tick{};
    uint64_t frame{~0ull};
    uint32_t bytes{};
    uint64_t address{};
    std::array<float, 3> shadow_color{};
    uint32_t flags{};
};

struct DirSetDiff {
    uint32_t changed{};
    uint32_t zero{};
    uint32_t first{NoLight};
    float color_delta{};
    float dir_delta{};
};

inline DirSetDiff CompareDirSets(std::span<const float> prev, std::span<const float> cur) {
    constexpr uint32_t stride = DirLightSetStride / 4;
    DirSetDiff d{};
    const uint32_t sets = uint32_t(cur.size() / stride);
    for (uint32_t s = 0; s < sets; ++s) {
        const float* c = cur.data() + size_t(s) * stride;
        bool zero = true;
        for (uint32_t i = 0; i < stride; ++i) {
            zero = zero && c[i] == 0.0f;
        }
        d.zero += zero ? 1u : 0u;
        if (size_t(s + 1) * stride > prev.size()) {
            ++d.changed;
            d.first = std::min(d.first, s);
            continue;
        }
        const float* p = prev.data() + size_t(s) * stride;
        if (std::memcmp(c, p, stride * sizeof(float)) == 0) {
            continue;
        }
        ++d.changed;
        d.first = std::min(d.first, s);
        for (uint32_t v = 0; v < 4; ++v) {
            for (uint32_t k = 0; k < 3; ++k) {
                d.dir_delta = std::max(d.dir_delta, std::fabs(c[v * 4 + k] - p[v * 4 + k]));
            }
        }
        for (uint32_t v = 4; v < 18; ++v) {
            if (v == 16) {
                continue;
            }
            for (uint32_t k = 0; k < 4; ++k) {
                d.color_delta = std::max(d.color_delta, std::fabs(c[v * 4 + k] - p[v * 4 + k]));
            }
        }
    }
    return d;
}

template <typename IsFree>
int NewestReadySetReadback(std::span<const SetReadback> slots, IsFree&& is_free) {
    int best = -1;
    for (size_t i = 0; i < slots.size(); ++i) {
        const SetReadback& r = slots[i];
        if (r.bytes == 0 || r.frame == ~0ull || !is_free(r.tick)) {
            continue;
        }
        if (best < 0 || r.frame > slots[size_t(best)].frame) {
            best = int(i);
        }
    }
    return best;
}

inline constexpr float MoonRayEpsilon = 1e-3f;

constexpr bool MoonRayNeeded(float nl1, float c1_sq, float fx, float fy, float lum1, float lum2) {
    return nl1 > 0.0f && c1_sq > 0.0f && fx > 0.0f && fx * (lum1 + fy * lum2) > MoonRayEpsilon;
}

constexpr bool GpuLightSetsAligned(uint64_t offset, uint64_t alignment) {
    return alignment != 0 && offset % alignment == 0;
}

struct DirectLightGpu {
    std::array<float, 4> pos_inv_r2;
    std::array<float, 4> color_sp;
    std::array<float, 4> spec_kind;
    std::array<float, 4> wrap_r_flag;
    std::array<float, 16> cone;
};
static_assert(sizeof(DirectLightGpu) == 128);

struct LightDrawSkip {
    bool direct_enabled = false;
    bool paused = false;
    bool composite = false;
    bool blended = false;
    bool writes_depth = false;
    bool writes_stencil = false;
    uint64_t frame = 0;
    uint64_t direct_frame = ~0ull;
    uint32_t diffuse = 0, specular = 0;
    uint32_t direct_diffuse = 0, direct_specular = 0;
};

constexpr bool SkipLightDraw(const LightDrawSkip& s) {
    return s.direct_enabled && !s.paused && !s.composite && s.blended && !s.writes_depth &&
           !s.writes_stencil && s.direct_frame + 1 == s.frame && s.diffuse != 0 &&
           s.specular != 0 && s.diffuse == s.direct_diffuse && s.specular == s.direct_specular;
}

inline constexpr uint32_t DeferredAoShader = 0x3fe9e964u;
inline constexpr uint32_t LightCmbAlbedoShader = 0x8650418fu;
inline constexpr uint32_t LightCompositeShader = 0x32e47788u;

constexpr bool DirShadowMaskDraw(uint64_t ps_hash) {
    return uint32_t(ps_hash) == DirShadowedShader;
}

template <class Id>
Id PickShadowMask(std::span<const Id> candidates, std::span<const Id> gbuffer, Id depth) {
    for (const Id& c : candidates) {
        if (!c || c == depth) {
            continue;
        }
        if (std::find(gbuffer.begin(), gbuffer.end(), c) != gbuffer.end()) {
            continue;
        }
        return c;
    }
    return Id{};
}

struct MoonShadow {
    float ssao_mask;
    std::array<float, 3> shadow;
};

constexpr MoonShadow MoonShadowFromMask(std::array<float, 4> mask, float ssao_weight,
                                        std::array<float, 3> shadow_color) {
    MoonShadow m{1.0f + ssao_weight * (mask[0] - 1.0f), {}};
    for (int i = 0; i < 3; ++i) {
        m.shadow[i] = mask[2] * (1.0f - shadow_color[i] + shadow_color[i] * mask[3]);
    }
    return m;
}

constexpr std::array<float, 3> MoonShadowTraced(float visibility, std::array<float, 3> shadow_color) {
    std::array<float, 3> shadow{};
    for (int i = 0; i < 3; ++i) {
        shadow[i] = 1.0f - shadow_color[i] + shadow_color[i] * visibility;
    }
    return shadow;
}

constexpr float HalfLambertSq(float nl) {
    const float h = 0.5f * nl + 0.5f;
    const float v = h * h;
    return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

constexpr float MoonConeCos(float half_angle_degrees) {
    const float r = half_angle_degrees * 3.14159265f / 180.0f;
    const float r2 = r * r;
    return 1.0f - r2 / 2.0f + r2 * r2 / 24.0f - r2 * r2 * r2 / 720.0f;
}

constexpr bool DirectHookDraw(uint64_t ps_hash) {
    return ps_hash == LightCmbAlbedoShader || ps_hash == LightCompositeShader;
}

constexpr bool DirectHookFires(uint64_t ps_hash, uint64_t acc_frame, uint64_t frame) {
    return DirectHookDraw(ps_hash) && acc_frame == frame;
}

enum class DirectWrite : uint8_t { Overwrite, Add, PassThrough };

constexpr DirectWrite DirectWriteMode(uint32_t debug, bool light_draws_skipped) {
    return debug != 0 ? DirectWrite::Overwrite
                      : (light_draws_skipped ? DirectWrite::Add : DirectWrite::PassThrough);
}

constexpr bool ReflectUsesPreAo(bool pair_valid, bool direct_enabled, bool light_draws_skipped,
                                uint32_t direct_debug) {
    return pair_valid && direct_enabled && light_draws_skipped && direct_debug == 0;
}

enum class AoInput : uint8_t { Reject, Hdr, HdrAlpha };

constexpr AoInput ClassifyAoInput(bool hdr_rgb, bool hdr_rgba, uint32_t levels, bool target_size) {
    if (levels != 1 || !target_size) {
        return AoInput::Reject;
    }
    return hdr_rgba ? AoInput::HdrAlpha : (hdr_rgb ? AoInput::Hdr : AoInput::Reject);
}

template <typename Id>
struct PreAoPair {
    uint64_t frame = ~0ull;
    std::array<Id, 2> inputs{};
    std::array<bool, 2> alpha{};
    uint32_t count = 0;
    bool overflow = false;

    void Input(uint64_t now, Id id, Id out_diffuse, Id out_specular, AoInput kind = AoInput::Hdr) {
        if (now != frame) {
            frame = now;
            inputs = {};
            alpha = {};
            count = 0;
            overflow = false;
        }
        if (kind == AoInput::Reject || !id || id == out_diffuse || id == out_specular) {
            return;
        }
        for (uint32_t i = 0; i < count; ++i) {
            if (inputs[i] == id) {
                return;
            }
        }
        if (count < 2) {
            alpha[count] = kind == AoInput::HdrAlpha;
            inputs[count++] = id;
        } else {
            overflow = true;
        }
    }

    bool Valid(uint64_t now) const {
        return frame == now && count == 2 && !overflow;
    }

    template <typename IsGbuffer>
    std::array<Id, 2> Ordered(IsGbuffer&& is_gbuffer) const {
        std::array<Id, 2> out = inputs;
        if (alpha[0] != alpha[1]) {
            if (alpha[0]) {
                std::swap(out[0], out[1]);
            }
        } else if (is_gbuffer(out[1]) && !is_gbuffer(out[0])) {
            std::swap(out[0], out[1]);
        }
        return out;
    }
};

struct DirectParams {
    std::array<float, 4> proj;
    std::array<std::array<float, 4>, 3> view_rows;
    std::array<float, 4> size_jitter;
    std::array<uint32_t, 4> tlas_count_flags;
    std::array<uint32_t, 4> cfg;
    std::array<float, 4> bias_tmin_gap_sets;
    std::array<float, 4> prev_proj;
    std::array<std::array<float, 4>, 3> reproject;
    std::array<float, 4> prev_jitter;
    std::array<uint32_t, 4> restir;
    std::array<float, 4> shadow_color;
    std::array<float, 4> fx;
    std::array<float, 4> far_fade;
    std::array<float, 4> ambient_cut;
    std::array<float, 4> ambient_spec_cut;
};
static_assert(sizeof(DirectParams) == 304);

struct FogParams {
    std::array<float, 4> proj;
    std::array<std::array<float, 4>, 3> view_rows;
    std::array<float, 4> size_jitter;
    std::array<uint32_t, 4> tlas_count_flags;
    std::array<uint32_t, 4> cfg;
    std::array<float, 4> fog;
    std::array<float, 4> prev_proj;
    std::array<std::array<float, 4>, 3> reproject;
    std::array<float, 4> prev_jitter;
    std::array<float, 4> moon;
    std::array<uint32_t, 4> light_cfg;
    std::array<float, 4> pick; // xy pick pixel, z 1 when the pick is on (rt_fog.comp FogPick), w sun share
};
static_assert(sizeof(FogParams) == 256);
constexpr uint32_t FogFlagSets = 1;
constexpr uint32_t FogFlagHistory = 2;
constexpr uint32_t FogFlagTable = 4;
constexpr uint32_t FogLightLamps = 1;
constexpr uint32_t FogLightSky = 2;
constexpr uint32_t FogLightDome = 4;
constexpr uint32_t FogLightSunCal = 8; // rt_fog.comp LightSunCal (BB_RT_FX_FOG_SUN_CAL)
constexpr uint32_t FogLightPortals = 16; // rt_fog.comp LightPortals
constexpr uint64_t FxSheetShader = 0x71c96598ull;
constexpr uint64_t FxMistShader = 0xa28d2a7aull;
constexpr uint64_t WaterLayerShader = 0xe3021062ull;
// The game's volumetric light shafts: a full-screen additive quad per volume light into a
// half-res buffer (1148x480 at 2296x960), one flat colour per light from the vertex stream.
// It is light with no traced source, so it is skipped while our traced fog runs.
constexpr uint64_t VolumeLightShader = 0xe55e45ebull;
constexpr uint32_t FogScale = 4;

struct Reservoir {
    uint32_t light = NoLight;
    float w_sum = 0.0f;
    float m = 0.0f;
    float p_hat = 0.0f;
};

inline void ReservoirUpdate(Reservoir& r, uint32_t light, float w, float p_hat, float u) {
    r.w_sum += w;
    r.m += 1.0f;
    if (w > 0.0f && u * r.w_sum < w) {
        r.light = light;
        r.p_hat = p_hat;
    }
}

inline float ReservoirWeight(const Reservoir& r) {
    return (r.p_hat > 0.0f && r.m > 0.0f) ? r.w_sum / (r.m * r.p_hat) : 0.0f;
}

inline void ReservoirMerge(Reservoir& r, const Reservoir& other, float p_hat_here, float u,
                           float max_m) {
    const float m = std::min(other.m, max_m);
    const float w = p_hat_here * ReservoirWeight(other) * m;
    r.w_sum += w;
    r.m += m;
    if (w > 0.0f && u * r.w_sum < w) {
        r.light = other.light;
        r.p_hat = p_hat_here;
    }
}

inline void RemapLightIds(const std::vector<uint64_t>& previous, const std::vector<uint64_t>& current,
                          std::vector<uint32_t>& out) {
    out.assign(previous.size(), NoLight);
    size_t c = 0;
    for (size_t i = 0; i < previous.size(); ++i) {
        while (c < current.size() && current[c] < previous[i]) {
            ++c;
        }
        if (c < current.size() && current[c] == previous[i]) {
            out[i] = uint32_t(c);
        }
    }
}

inline constexpr float SpotKindNone = 0.0f;
inline constexpr float SpotKindFalloff = 1.0f;
inline constexpr float SpotKindAxial = 2.0f;

inline float SpotKind(const Light& l) {
    if (l.kind != LightKind::Spot) {
        return SpotKindNone;
    }
    return l.spot_flag > 0.0f ? SpotKindFalloff : SpotKindAxial;
}

inline std::array<float, 4> ConeApply(const std::array<float, 16>& cone,
                                      const std::array<float, 3>& p) {
    std::array<float, 4> q{};
    for (uint32_t r = 0; r < 4; ++r) {
        q[r] = cone[r * 4] * p[0] + cone[r * 4 + 1] * p[1] + cone[r * 4 + 2] * p[2] +
               cone[r * 4 + 3];
    }
    return q;
}

inline float SpotTanHalf(const std::array<float, 16>& cone) {
    const auto len = [&](uint32_t row) {
        return std::sqrt(cone[row * 4] * cone[row * 4] + cone[row * 4 + 1] * cone[row * 4 + 1] +
                         cone[row * 4 + 2] * cone[row * 4 + 2]);
    };
    const float axis = len(3);
    const float lateral = std::min(len(0), len(1));
    if (!(lateral > 1e-8f) || !std::isfinite(axis) || !std::isfinite(lateral)) {
        return 1.0f;
    }
    return std::min(axis / lateral, 16.0f);
}

inline float SpotCullRadius(const Light& l) {
    if (SpotKind(l) != SpotKindAxial) {
        return l.radius;
    }
    const float t = SpotTanHalf(l.cone);
    return l.radius * std::sqrt(1.0f + t * t);
}

struct SpotTerms {
    bool lit;
    float cone;
    float atten;
    float range;
    bool legacy_spec;
};

inline SpotTerms SpotFactor(const std::array<float, 16>& cone, const std::array<float, 3>& p,
                            const std::array<float, 3>& light, float radius, float kind) {
    const float dx = light[0] - p[0], dy = light[1] - p[1], dz = light[2] - p[2];
    const float d2 = dx * dx + dy * dy + dz * dz;
    const float inv_r2 = 1.0f / (radius * radius);
    SpotTerms t{true, 1.0f, std::max(0.0f, 1.0f / d2 - inv_r2),
                1.0f - std::clamp(d2 * inv_r2, 0.0f, 1.0f), false};
    if (kind == SpotKindNone) {
        return t;
    }
    const std::array<float, 4> q = ConeApply(cone, p);
    if (q[2] <= 0.0f || q[3] == 0.0f) {
        return {false, 0.0f, 0.0f, 0.0f, false};
    }
    const float x = q[0] / q[3], y = q[1] / q[3];
    const float disc = std::clamp(1.0f - x * x - y * y, 0.0f, 1.0f);
    if (kind == SpotKindFalloff) {
        t.cone = disc;
    } else {
        const float axial = std::clamp(1.0f - q[3] / radius, 0.0f, 1.0f);
        t.cone = disc * disc * axial * axial;
        t.atten = 1.0f;
        t.range = 1.0f;
        t.legacy_spec = true;
    }
    t.lit = t.cone > 0.0f;
    return t;
}

constexpr std::array<float, 3> DirectDebugColor(uint32_t index) {
    return {float(((index * 37u) & 7u) + 1u) / 8.0f, float(((index * 101u) & 7u) + 1u) / 8.0f,
            float(((index * 59u) & 7u) + 1u) / 8.0f};
}

inline DirectLightGpu PackDirectLight(const Light& l, const Affine& view) {
    const std::array<float, 3> p = Transform(view, l.world);
    const float cull = SpotCullRadius(l);
    DirectLightGpu g{};
    g.pos_inv_r2 = {p[0], p[1], p[2], 1.0f / (cull * cull)};
    g.color_sp = {l.color[0], l.color[1], l.color[2], l.spec_power};
    g.spec_kind = {l.spec[0], l.spec[1], l.spec[2], SpotKind(l)};
    // Flag 4: converted emitter (EmitterFalloff); z then carries its source size squared, which
    // only spot lights otherwise read (their axial range), and emitters are points.
    const bool emitter_atten = l.source > 0.0f && !l.legacy && l.kind == LightKind::Point;
    g.wrap_r_flag = {l.wrap[0], l.wrap[1], emitter_atten ? l.source * l.source : l.radius,
                     float((l.legacy ? 1u : 0u) | (l.camera_fixed ? 2u : 0u) |
                           (emitter_atten ? 4u : 0u) | (l.housed ? 16u : 0u))};
    g.cone = l.cone;
    return g;
}

// Lamp halos sit on lamp glass: spots are the view positions of steady warm glow captures (the
// lit glass the game draws), so loot glows, fills and lamp lights anchored off their fixture get
// none. Each spot takes its intensity from the nearest fixed flame light (flag 4, not near a
// carried torch: flag 8) within match m, so it flickers with that light; no light, no halo.
// The nearest MaxHalos spots; out gets 8 floats each: view position, Gaussian width (m);
// intensity (colour x source^2, already flickered) x strength, hot core (m). rt_gi_apply.comp
// and the water mirror integrate strength x I / (d^2 + core^2) along the ray up to the surface,
// times exp(-h^2 / width^2) of the ray's closest approach h. Returns the count.
constexpr uint32_t MaxHalos = 16;
// Halo block after the fog block in the GI light ring: count, strength, radius, core, halos.
constexpr uint32_t HaloBlockWords = 4 + 8 * MaxHalos;
inline uint32_t BuildHalos(std::span<const DirectLightGpu> lights,
                           std::span<const std::array<float, 3>> spots, float strength,
                           float radius, float core, float match, float* out) {
    if (!(strength > 0.0f) || !out) {
        return 0;
    }
    struct Pick {
        float d2;
        uint32_t spot, light;
        bool operator<(const Pick& o) const { return d2 < o.d2; }
    };
    std::vector<Pick> picks;
    for (uint32_t s = 0; s < spots.size(); ++s) {
        const auto& p = spots[s];
        float best = match * match;
        uint32_t found = UINT32_MAX;
        for (uint32_t i = 0; i < lights.size(); ++i) {
            const DirectLightGpu& g = lights[i];
            const uint32_t flags = uint32_t(g.wrap_r_flag[3] + 0.5f);
            if ((flags & 4u) == 0u || (flags & 8u) != 0u ||
                !std::isfinite(g.color_sp[0] * g.wrap_r_flag[2])) {
                continue;
            }
            const float dx = g.pos_inv_r2[0] - p[0], dy = g.pos_inv_r2[1] - p[1],
                        dz = g.pos_inv_r2[2] - p[2];
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 <= best) {
                best = d2;
                found = i;
            }
        }
        const float v2 = p[0] * p[0] + p[1] * p[1] + p[2] * p[2];
        if (found != UINT32_MAX && std::isfinite(v2)) {
            picks.push_back({v2, s, found});
        }
    }
    const size_t n = std::min<size_t>(picks.size(), MaxHalos);
    std::partial_sort(picks.begin(), picks.begin() + n, picks.end());
    for (size_t k = 0; k < n; ++k) {
        const DirectLightGpu& g = lights[picks[k].light];
        const auto& p = spots[picks[k].spot];
        float* o = out + 8 * k;
        o[0] = p[0];
        o[1] = p[1];
        o[2] = p[2];
        o[3] = radius;
        for (int c = 0; c < 3; ++c) {
            o[4 + c] = std::max(g.color_sp[c], 0.0f) * g.wrap_r_flag[2] * strength;
        }
        o[7] = core;
    }
    return uint32_t(n);
}

// A lamp reaches past the game's light volume, so the game's depth test on that volume would clip
// it back to the game's radius (a wall lamp's light stopped at its 2 m sphere instead of reaching
// through a glass door, hot103). Candles keep the test: their look was approved with it.
inline bool PackStockDepth(DirectLightGpu& g, const Light& l) {
    if (g.spec_kind[3] != 0.0f || l.age != 0 || l.lamp || l.wide) {
        return false;
    }
    const StockDepthTest t = LightDepthTest(l.state);
    if (!t.active) {
        return false;
    }
    g.cone[0] = float(t.func);
    g.cone[1] = t.quad;
    g.cone[2] = t.bounds_min;
    g.cone[3] = t.bounds_max;
    g.cone[4] = t.bounds ? 1.0f : 0.0f;
    g.cone[5] = 1.0f;
    return true;
}

constexpr uint32_t AoFlagHistory = 1;
constexpr uint32_t AoFlagNormal = 2;

struct AoParams {
    std::array<float, 4> proj;
    std::array<std::array<float, 4>, 3> view_rows;
    std::array<float, 4> size_jitter;
    std::array<uint32_t, 4> tlas_rays_flags;
    std::array<uint32_t, 4> cfg;
    std::array<float, 4> radius_bias_tmin;
    std::array<float, 4> prev_proj;
    std::array<std::array<float, 4>, 3> reproject;
    std::array<float, 4> prev_jitter;
    std::array<uint32_t, 4> cell;
};
static_assert(sizeof(AoParams) == 224);

constexpr uint32_t AoStatsStride = 8;
constexpr float AoStatsScale = 1024.0f;
constexpr float AoBlockZTolerance = 0.02f;

constexpr uint32_t AoCells(uint32_t pixels, uint32_t scale) {
    return (pixels + scale - 1) / scale;
}

constexpr std::array<uint32_t, 2> AoBlockPixel(std::array<uint32_t, 2> cell, uint32_t scale,
                                               std::array<uint32_t, 2> size) {
    return {std::min(cell[0] * scale, size[0] - 1), std::min(cell[1] * scale, size[1] - 1)};
}

constexpr bool AoBlockKeepsStock(float z, float traced_z, float tolerance) {
    const float diff = z - traced_z;
    const float scale = traced_z < 0.0f ? -traced_z : traced_z;
    return (diff < 0.0f ? -diff : diff) > tolerance * (scale > 1.0f ? scale : 1.0f);
}

constexpr float AoOcclusionWeight(float hit_t, float radius) {
    if (!(hit_t < radius)) {
        return 0.0f;
    }
    const float w = 1.0f - hit_t / radius;
    return w < 0.0f ? 0.0f : w;
}

inline float AoVisibility(std::span<const float> hit_t, float radius) {
    float occluded = 0.0f;
    for (float t : hit_t) {
        occluded += AoOcclusionWeight(t, radius);
    }
    return hit_t.empty() ? 1.0f : 1.0f - occluded / float(hit_t.size());
}

constexpr float AoMaskTerm(float ssao_weight, float mask_x) {
    return 1.0f + ssao_weight * (mask_x - 1.0f);
}

struct AoStatsMean {
    float stock;
    float ours;
};

constexpr AoStatsMean AoStatsReadback(std::array<uint32_t, 3> sums) {
    if (sums[2] == 0) {
        return {1.0f, 1.0f};
    }
    const float n = float(sums[2]) * AoStatsScale;
    return {float(sums[0]) / n, float(sums[1]) / n};
}

template <class Id>
Id PickAoMask(std::span<const Id> candidates, std::span<const Id> gbuffer, Id depth) {
    return PickShadowMask(candidates, gbuffer, depth);
}

constexpr float AoBlend(float history_count, uint32_t max_history) {
    const float n = history_count < float(max_history) ? history_count + 1.0f : float(max_history);
    return 1.0f / n;
}

constexpr bool AoHistoryAccepts(float prev_z, float stored_z, float tolerance) {
    const float scale = prev_z < 0.0f ? -prev_z : prev_z;
    const float diff = prev_z - stored_z;
    return (diff < 0.0f ? -diff : diff) <= tolerance * (scale > 1.0f ? scale : 1.0f);
}

inline uint64_t DirectKey(const Light& l) {
    const auto q = [](float v, float step) {
        return uint64_t(int64_t(std::llround(v / step)) & 0x1FFFFF);
    };
    const uint64_t pos = q(l.world[0], 0.02f) | q(l.world[1], 0.02f) << 21 | q(l.world[2], 0.02f) << 42;
    const uint64_t r = uint64_t(std::llround(std::log2(std::max(l.radius, 1e-3f)) * 16.0f) & 0xFF);
    const uint64_t kind = (l.kind == LightKind::Spot ? 1ull : 0ull) | (l.legacy ? 1ull << 9 : 0ull);
    return (pos * 0x9E3779B97F4A7C15ull) ^ (r << 1) ^ kind;
}

inline bool DirectCandidate(const Light& l) {
    return (l.kind == LightKind::Point || l.kind == LightKind::Spot) && l.has_world &&
           l.radius > 0.0f && Finite3(l.color);
}

inline constexpr float DirectDuplicateDistance = 0.02f;
inline constexpr float DirectMatchDistance = 0.5f;

// A light that drifts more than this (m) from where it was first seen is mobile. Larger than
// DirectMatchDistance: in a candelabra a track can jump to a neighbour candle (0.2-0.5 m away)
// when one is culled, and that must not make the candles mobile (they would then be dropped
// off screen). A carried torch passes it within a step.
constexpr float DirectStaticDistance = 0.6f;

// A light must be drawn by the game in this many frames before it is kept as a static emitter
// once the game stops drawing it. A light that shows up for only a few frames (the player's
// follow light slipping past the camera-fixed filter next to the torch, hot101) is not a lamp.
constexpr uint32_t DirectKeepMinFrames = 30;

struct DirectTrack {
    Light light{};
    uint64_t seen{};
    uint64_t id{};
    Affine seen_view{};
    bool seen_valid{};
    // Moved more than DirectStaticDistance since it was first seen (a carried torch, a lantern on
    // an enemy): never kept as a static emitter once the game stops drawing it.
    bool mobile{};
    std::array<float, 3> first_world{};
    uint32_t frames{}; // frames the game drew it in
};

struct LightKeepView {
    Affine view{};
    float xs{}, ys{};
    uint32_t offscreen_frames{};
    bool valid{};
    bool reproject{true};
    // The scene camera (the one RT renders with) for the frustum test. Looking straight down, the
    // camera in the game's light draws is not the scene camera (a player fill light 3 m from the
    // camera read 62.8 m away), so lights and glow lamps behind the player were culled by it.
    Affine cull_view{};
    bool cull_valid{};
};

inline bool SphereInFrustum(const std::array<float, 3>& p, float r, float xs, float ys) {
    if (p[2] + r < 0.0f) {
        return false;
    }
    const auto side = [&](float a, float s) {
        const float n = std::sqrt(s * s + 1.0f);
        return (p[2] - s * a) / n >= -r && (p[2] + s * a) / n >= -r;
    };
    return side(p[0], xs) && side(p[1], ys);
}

inline bool KeepOffscreenLight(const DirectTrack& t, uint64_t frame_no, uint32_t keep,
                               const LightKeepView& kv) {
    const uint64_t age = frame_no - t.seen;
    if (age <= keep) {
        return true;
    }
    if (!kv.valid || age > kv.offscreen_frames || kv.xs == 0.0f || kv.ys == 0.0f) {
        return false;
    }
    // A real emitter (candle, lamp) does not switch off: the game also skips drawing it when its
    // small legacy sphere is still partly on screen (its own culling), but our extended reach can
    // still light what is visible, so it is kept whether or not its sphere is in the frustum.
    if (t.light.emitter && !t.light.held && !t.mobile && t.frames >= DirectKeepMinFrames) {
        return true;
    }
    return !SphereInFrustum(Transform(kv.cull_valid ? kv.cull_view : kv.view, t.light.world),
                            t.light.radius, kv.xs, kv.ys);
}

inline std::array<float, 16> ReprojectCone(const std::array<float, 16>& cone, const Affine& a) {
    std::array<float, 16> out{};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            float v = cone[r * 4 + 3] * (c == 3 ? 1.0f : 0.0f);
            for (int k = 0; k < 3; ++k) {
                v += cone[r * 4 + k] * a[k * 4 + c];
            }
            out[r * 4 + c] = v;
        }
    }
    return out;
}

inline Light KeptLight(const DirectTrack& t, uint64_t frame_no, const LightKeepView& kv) {
    Light l = t.light;
    if (t.seen != frame_no && kv.valid && kv.reproject && t.seen_valid &&
        l.kind == LightKind::Spot) {
        l.cone = ReprojectCone(t.light.cone, MultiplyAffine(t.seen_view, InverseRigid(kv.view)));
    }
    return l;
}

struct DirectTracks {
    std::vector<DirectTrack> list;
    std::unordered_set<uint64_t> frame_keys;
    uint64_t next_id = 1;
    uint32_t kept_offscreen = 0;
    size_t size() const {
        return list.size();
    }
    bool empty() const {
        return list.empty();
    }
};

inline bool DirectSameKind(const Light& a, const Light& b) {
    return a.kind == b.kind && a.legacy == b.legacy && NearRel(a.radius, b.radius, 0.25f, 1e-3f);
}

inline uint32_t MergeDirectLights(const std::vector<Light>& frame, uint64_t frame_no, uint32_t keep,
                                  DirectTracks& tracks, std::vector<Light>& out,
                                  std::vector<uint64_t>* keys = nullptr,
                                  const LightKeepView* keep_view = nullptr) {
    uint32_t duplicates = 0;
    const LightKeepView kv = keep_view ? *keep_view : LightKeepView{};
    tracks.frame_keys.clear();
    tracks.kept_offscreen = 0;
    const float dup2 = DirectDuplicateDistance * DirectDuplicateDistance;
    const float match2 = DirectMatchDistance * DirectMatchDistance;
    for (const Light& l : frame) {
        if (!DirectCandidate(l)) {
            continue;
        }
        const uint64_t key = DirectKey(l);
        if (tracks.frame_keys.contains(key)) {
            ++duplicates;
            continue;
        }
        int best = -1;
        float best_d2 = std::numeric_limits<float>::max();
        bool duplicate = false;
        for (size_t t = 0; t < tracks.list.size() && !duplicate; ++t) {
            const DirectTrack& track = tracks.list[t];
            if (!DirectSameKind(l, track.light)) {
                continue;
            }
            const float d2 = WorldDistance2(l, track.light);
            if (track.seen == frame_no) {
                duplicate = d2 <= dup2 && NearRel(l.radius, track.light.radius, 0.01f, 1e-4f);
                continue;
            }
            if (d2 <= match2 && d2 < best_d2) {
                best_d2 = d2;
                best = int(t);
            }
        }
        if (duplicate) {
            ++duplicates;
            continue;
        }
        if (best >= 0) {
            DirectTrack& bt = tracks.list[size_t(best)];
            const float mx = l.world[0] - bt.first_world[0];
            const float my = l.world[1] - bt.first_world[1];
            const float mz = l.world[2] - bt.first_world[2];
            bt.mobile = bt.mobile || mx * mx + my * my + mz * mz >
                                         DirectStaticDistance * DirectStaticDistance;
            tracks.list[size_t(best)].light = l;
            tracks.list[size_t(best)].seen = frame_no;
            tracks.list[size_t(best)].seen_view = kv.view;
            tracks.list[size_t(best)].seen_valid = kv.valid;
            tracks.list[size_t(best)].frames = std::min(bt.frames + 1u, 1000000u);
            tracks.frame_keys.insert(key);
        } else {
            tracks.frame_keys.insert(key);
            tracks.list.push_back({l, frame_no, tracks.next_id++, kv.view, kv.valid, false, l.world, 1});
        }
    }
    std::erase_if(tracks.list, [&](const DirectTrack& t) {
        return !KeepOffscreenLight(t, frame_no, keep, kv);
    });
    out.clear();
    if (keys) {
        keys->clear();
    }
    for (size_t i = 0; i < tracks.list.size() && i < MaxDirectLights; ++i) {
        if (frame_no - tracks.list[i].seen > keep) {
            ++tracks.kept_offscreen;
        }
        out.push_back(KeptLight(tracks.list[i], frame_no, kv));
        out.back().age = uint32_t(std::min<uint64_t>(frame_no - tracks.list[i].seen, 0xFFFFFFFFull));
        if (keys) {
            keys->push_back(tracks.list[i].id);
        }
    }
    return duplicates;
}

struct PointTerms {
    std::array<float, 3> diffuse;
    std::array<float, 3> specular;
};

inline float Dot3(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

inline std::array<float, 3> Normalize3(std::array<float, 3> v) {
    const float len = std::sqrt(Dot3(v, v));
    for (float& c : v) {
        c /= len;
    }
    return v;
}

inline float SpecPower(float spec_term, float g1x) {
    const float e = std::exp2(13.0f * g1x);
    return std::max(1.0f, spec_term * e / (0.01f * e + spec_term));
}

inline float LegacyAtten(float d2, float inv_r2) {
    const float f = 1.0f - std::sqrt(std::clamp(d2 * inv_r2, 0.0f, 1.0f));
    return f * f;
}

inline float LegacySpecular(const std::array<float, 3>& n, const std::array<float, 3>& V,
                            const std::array<float, 3>& L, float sp) {
    const float nv = Dot3(n, V);
    const std::array<float, 3> R{2.0f * nv * n[0] - V[0], 2.0f * nv * n[1] - V[1],
                                 2.0f * nv * n[2] - V[2]};
    const float rl = std::clamp(Dot3(R, L), 0.0f, 1.0f);
    return std::pow(rl, 0.25f * sp) * (0.25f + 0.125f * sp);
}

inline PointTerms PointLightTerms(const std::array<float, 3>& p, const std::array<float, 3>& n,
                                  const DirectLightGpu& l, const std::array<float, 3>& g3,
                                  float g1x, float g1y, float visibility) {
    const std::array<float, 3> d{l.pos_inv_r2[0] - p[0], l.pos_inv_r2[1] - p[1],
                                 l.pos_inv_r2[2] - p[2]};
    const float d2 = Dot3(d, d);
    const float inv_r2 = l.pos_inv_r2[3];
    const bool legacy = (uint32_t(l.wrap_r_flag[3] + 0.5f) & 1u) != 0u;
    const float atten = legacy ? LegacyAtten(d2, inv_r2) : std::max(0.0f, 1.0f / d2 - inv_r2);
    const float range = 1.0f - std::clamp(d2 * inv_r2, 0.0f, 1.0f);
    const std::array<float, 3> L = Normalize3(d);
    const std::array<float, 3> V = Normalize3({-p[0], -p[1], -p[2]});
    const float nl = Dot3(n, L);
    const float nlc = std::clamp(nl, 0.0f, 1.0f);
    const std::array<float, 3> H = Normalize3({L[0] + V[0], L[1] + V[1], L[2] + V[2]});
    const float nh = std::clamp(Dot3(n, H), 0.0f, 1.0f);
    const float sp = SpecPower(l.color_sp[3], g1x);
    // Plain Lambert for every material, as rt_direct.comp (no wrap or rim terms).
    (void)g1y;
    const float diffuse_term = nlc * atten;
    const float spec_term = legacy ? LegacySpecular(n, V, L, sp) * atten
                                   : std::pow(nh, sp) * std::pow(nlc, 0.1f) * range * (0.25f + 0.125f * sp);
    PointTerms t{};
    for (int c = 0; c < 3; ++c) {
        const float g2 = g3[c] * g3[c];
        t.diffuse[c] = l.color_sp[c] * diffuse_term * (1.0f - g2 * 0.5f) * visibility;
        t.specular[c] = l.spec_kind[c] * spec_term * g2 * visibility;
    }
    return t;
}

inline float RestirTargetPdf(const std::array<float, 3>& p, const std::array<float, 3>& n,
                             const DirectLightGpu& l, const std::array<float, 3>& g3, float g1x,
                             float g1y) {
    const PointTerms t = PointLightTerms(p, n, l, g3, g1x, g1y, 1.0f);
    const float v = 0.299f * (t.diffuse[0] + t.specular[0]) + 0.587f * (t.diffuse[1] + t.specular[1]) +
                    0.114f * (t.diffuse[2] + t.specular[2]);
    return std::isfinite(v) ? std::max(v, 0.0f) : 0.0f;
}

inline std::array<float, 3> TileCornerDir(float px, float py, const std::array<float, 4>& proj,
                                          float width, float height, float zsign) {
    const float ndc_x = px / width * 2.0f - 1.0f;
    const float ndc_y = 1.0f - py / height * 2.0f;
    return {ndc_x * zsign / proj[0], ndc_y * zsign / proj[1], zsign};
}

inline bool SphereInTile(const std::array<float, 3>& center, float radius, uint32_t tile_x,
                         uint32_t tile_y, const std::array<float, 4>& proj, float width,
                         float height, float zsign, float z_near_abs, float z_far_abs) {
    const float az = center[2] * zsign;
    if (az + radius < z_near_abs || az - radius > z_far_abs) {
        return false;
    }
    const float x0 = float(tile_x * DirectTile), y0 = float(tile_y * DirectTile);
    const std::array<std::array<float, 3>, 4> corners{
        TileCornerDir(x0, y0, proj, width, height, zsign),
        TileCornerDir(x0 + DirectTile, y0, proj, width, height, zsign),
        TileCornerDir(x0 + DirectTile, y0 + DirectTile, proj, width, height, zsign),
        TileCornerDir(x0, y0 + DirectTile, proj, width, height, zsign)};
    const std::array<float, 3> centre_dir =
        TileCornerDir(x0 + DirectTile * 0.5f, y0 + DirectTile * 0.5f, proj, width, height, zsign);
    for (int i = 0; i < 4; ++i) {
        const auto& a = corners[i];
        const auto& b = corners[(i + 1) % 4];
        std::array<float, 3> n{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
                               a[0] * b[1] - a[1] * b[0]};
        n = Normalize3(n);
        if (Dot3(n, centre_dir) < 0.0f) {
            n = {-n[0], -n[1], -n[2]};
        }
        if (Dot3(n, center) < -radius) {
            return false;
        }
    }
    return true;
}

inline constexpr uint32_t LightGridHeader = 8;
inline constexpr uint32_t LightGridMaxDim = 32;
inline constexpr uint32_t LightGridMaxCells = 4096;
inline constexpr uint32_t LightGridMaxEntries = 32768;
inline constexpr uint32_t LightGridWords =
    LightGridHeader + LightGridMaxCells + 1 + LightGridMaxEntries;

inline uint32_t FloatWord(float f) {
    uint32_t u;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}

inline bool BuildLightGrid(std::span<const DirectLightGpu> lights,
                           std::span<uint32_t> out) {
  if (out.size() < LightGridWords) {
    return false;
  }
  std::fill(out.begin(), out.begin() + LightGridHeader, 0u);
  if (lights.empty()) {
    return false;
  }
  std::array<float, 3> lo{}, hi{};
  lo.fill(std::numeric_limits<float>::max());
  hi.fill(-std::numeric_limits<float>::max());
  std::vector<float> radius(lights.size());
  for (size_t i = 0; i < lights.size(); ++i) {
    const auto &p = lights[i].pos_inv_r2;
    if (!(p[3] > 0.0f) || !std::isfinite(p[3]) || !std::isfinite(p[0]) ||
        !std::isfinite(p[1]) || !std::isfinite(p[2])) {
      return false;
    }
    radius[i] = std::sqrt(1.0f / p[3]) * 1.001f + 1e-3f;
    if (!std::isfinite(radius[i])) {
      return false;
    }
    for (int a = 0; a < 3; ++a) {
      lo[a] = std::min(lo[a], p[a] - radius[i]);
      hi[a] = std::max(hi[a], p[a] + radius[i]);
    }
  }
  std::array<float, 3> extent{};
  float volume = 1.0f;
  for (int a = 0; a < 3; ++a) {
    extent[a] = std::max(hi[a] - lo[a], 1e-3f);
    volume *= extent[a];
  }
  float cell = std::cbrt(volume / float(LightGridMaxCells));
  for (int a = 0; a < 3; ++a) {
    cell = std::max(cell, extent[a] / float(LightGridMaxDim));
  }
  if (!(cell > 0.0f) || !std::isfinite(cell)) {
    return false;
  }
  for (int attempt = 0; attempt < 24; ++attempt, cell *= 2.0f) {
    std::array<uint32_t, 3> dims{};
    uint64_t cells = 0;
    for (int iter = 0; iter < 16; ++iter) {
      cells = 1;
      for (int a = 0; a < 3; ++a) {
        dims[a] = uint32_t(std::clamp(std::ceil(extent[a] / cell), 1.0f,
                                      float(LightGridMaxDim)));
        cells *= dims[a];
      }
      if (cells <= LightGridMaxCells) {
        break;
      }
      cell *= 1.1f;
    }
    if (cells > LightGridMaxCells) {
      continue;
    }
    uint32_t *offsets = out.data() + LightGridHeader;
    uint32_t *entries = offsets + cells + 1;
    const size_t entry_room = out.size() - LightGridHeader - (cells + 1);
    std::fill(offsets, offsets + cells + 1, 0u);
    const auto range = [&](size_t i, int a, uint32_t &c0, uint32_t &c1) {
      const float p = lights[i].pos_inv_r2[a];
      const float r = radius[i];
      const float f0 = std::floor((p - r - lo[a]) / cell);
      const float f1 = std::floor((p + r - lo[a]) / cell);
      c0 = uint32_t(std::clamp(f0, 0.0f, float(dims[a] - 1)));
      c1 = uint32_t(std::clamp(f1, 0.0f, float(dims[a] - 1)));
    };
    const auto touches = [&](size_t i, uint32_t x, uint32_t y, uint32_t z) {
      const std::array<uint32_t, 3> c{x, y, z};
      float d2 = 0.0f;
      for (int a = 0; a < 3; ++a) {
        const float b0 = c[a] == 0 ? -std::numeric_limits<float>::max()
                                   : lo[a] + float(c[a]) * cell - cell * 1e-3f;
        const float b1 = c[a] + 1 >= dims[a]
                             ? std::numeric_limits<float>::max()
                             : lo[a] + float(c[a] + 1) * cell + cell * 1e-3f;
        const float p = lights[i].pos_inv_r2[a];
        const float d = p < b0 ? b0 - p : p > b1 ? p - b1 : 0.0f;
        d2 += d * d;
      }
      return d2 <= radius[i] * radius[i];
    };
    const auto visit = [&](auto &&fn) {
      for (size_t i = 0; i < lights.size(); ++i) {
        uint32_t x0, x1, y0, y1, z0, z1;
        range(i, 0, x0, x1);
        range(i, 1, y0, y1);
        range(i, 2, z0, z1);
        for (uint32_t z = z0; z <= z1; ++z) {
          for (uint32_t y = y0; y <= y1; ++y) {
            for (uint32_t x = x0; x <= x1; ++x) {
              if (touches(i, x, y, z)) {
                fn(i, (z * dims[1] + y) * dims[0] + x);
              }
            }
          }
        }
      }
    };
    size_t total = 0;
    visit([&](size_t, uint32_t c) {
      ++offsets[c + 1];
      ++total;
    });
    if (total > entry_room || total > LightGridMaxEntries) {
      continue;
    }
    for (uint64_t c = 0; c < cells; ++c) {
      offsets[c + 1] += offsets[c];
    }
    std::vector<uint32_t> fill(offsets, offsets + cells);
    visit([&](size_t i, uint32_t c) { entries[fill[c]++] = uint32_t(i); });
    out[0] = FloatWord(lo[0]);
    out[1] = FloatWord(lo[1]);
    out[2] = FloatWord(lo[2]);
    out[3] = FloatWord(1.0f / cell);
    out[4] = dims[0];
    out[5] = dims[1];
    out[6] = dims[2];
    out[7] = 1u;
    return true;
  }
  return false;
}

} // namespace Vulkan::Rt

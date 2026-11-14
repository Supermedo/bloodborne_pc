#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <vector>

namespace Vulkan::Rt {

using Affine = std::array<float, 12>;

constexpr float Pi = 3.14159265358979f;

constexpr float Abs(float v) {
    return v < 0.0f ? -v : v;
}

struct CosineSample {
    std::array<float, 3> direction;
    float pdf;
};

inline CosineSample CosineHemisphere(float u1, float u2) {
    const float r = std::sqrt(u1);
    const float phi = 2.0f * Pi * u2;
    const float z = std::sqrt(std::max(0.0f, 1.0f - u1));
    return {{r * std::cos(phi), r * std::sin(phi), z}, z / Pi};
}

inline std::array<float, 3> TangentToView(const std::array<float, 3>& local,
                                             const std::array<float, 3>& n) {
    const std::array<float, 3> helper =
        Abs(n[2]) < 0.9f ? std::array<float, 3>{0, 0, 1} : std::array<float, 3>{1, 0, 0};
    std::array<float, 3> t{helper[1] * n[2] - helper[2] * n[1], helper[2] * n[0] - helper[0] * n[2],
                           helper[0] * n[1] - helper[1] * n[0]};
    const float len = std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
    for (float& v : t) {
        v /= len;
    }
    const std::array<float, 3> b{n[1] * t[2] - n[2] * t[1], n[2] * t[0] - n[0] * t[2],
                                 n[0] * t[1] - n[1] * t[0]};
    return {t[0] * local[0] + b[0] * local[1] + n[0] * local[2],
            t[1] * local[0] + b[1] * local[1] + n[1] * local[2],
            t[2] * local[0] + b[2] * local[1] + n[2] * local[2]};
}

struct Screen {
    std::array<float, 2> size;
    std::array<float, 2> jitter;
};

constexpr std::array<float, 3> PixelToView(std::array<float, 2> pixel, float depth,
                                           const std::array<float, 4>& proj, const Screen& s) {
    const float u = (pixel[0] + 0.5f - s.jitter[0]) / s.size[0];
    const float v = (pixel[1] + 0.5f - s.jitter[1]) / s.size[1];
    const float ndc_x = u * 2.0f - 1.0f, ndc_y = 1.0f - v * 2.0f;
    const float denom = std::min(depth - proj[2], -1e-8f);
    const float z = proj[3] / denom;
    return {ndc_x * z / proj[0], ndc_y * z / proj[1], z};
}

constexpr std::array<float, 2> ViewToPixel(const std::array<float, 3>& p,
                                           const std::array<float, 4>& proj, const Screen& s) {
    const float z = std::max(p[2], 1e-4f);
    const float ndc_x = p[0] * proj[0] / z, ndc_y = p[1] * proj[1] / z;
    return {(ndc_x * 0.5f + 0.5f) * s.size[0] + s.jitter[0] - 0.5f,
            (0.5f - ndc_y * 0.5f) * s.size[1] + s.jitter[1] - 0.5f};
}

constexpr float ViewToDepth(float z, const std::array<float, 4>& proj) {
    return proj[2] + proj[3] / z;
}

constexpr Affine MultiplyAffine(const Affine& a, const Affine& b) {
    Affine out{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            float v = c == 3 ? a[r * 4 + 3] : 0.0f;
            for (int k = 0; k < 3; ++k) {
                v += a[r * 4 + k] * b[k * 4 + c];
            }
            out[r * 4 + c] = v;
        }
    }
    return out;
}

constexpr Affine InverseRigid(const Affine& m) {
    Affine out{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            out[r * 4 + c] = m[c * 4 + r];
        }
        out[r * 4 + 3] = -(m[r] * m[3] + m[4 + r] * m[7] + m[8 + r] * m[11]);
    }
    return out;
}

constexpr std::array<float, 3> Transform(const Affine& m, const std::array<float, 3>& p) {
    return {m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3],
            m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7],
            m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11]};
}

constexpr Affine Reprojection(const Affine& previous_inv_view, const Affine& current_inv_view) {
    return MultiplyAffine(InverseRigid(previous_inv_view), current_inv_view);
}

constexpr bool CameraCut(const Affine& previous_inv_view, const Affine& current_inv_view,
                         float max_translation, float min_cos_rotation) {
    const float dx = current_inv_view[3] - previous_inv_view[3];
    const float dy = current_inv_view[7] - previous_inv_view[7];
    const float dz = current_inv_view[11] - previous_inv_view[11];
    if (dx * dx + dy * dy + dz * dz > max_translation * max_translation) {
        return true;
    }
    const float forward_dot = current_inv_view[2] * previous_inv_view[2] +
                              current_inv_view[6] * previous_inv_view[6] +
                              current_inv_view[10] * previous_inv_view[10];
    return forward_dot < min_cos_rotation;
}

constexpr bool HistoryAccept(float reprojected_z, float stored_z, float relative, float absolute) {
    return stored_z > 0.0f && Abs(reprojected_z - stored_z) <= relative * reprojected_z + absolute;
}

constexpr float BlendWeight(float stored_count, float max_count) {
    const float n = std::min(stored_count, max_count) + 1.0f;
    return 1.0f / n;
}

constexpr bool HitVisible(float screen_z, float hit_z, float relative, float absolute) {
    return Abs(screen_z - hit_z) <= relative * hit_z + absolute;
}

enum NormalMode : uint32_t { NormalFromDepth = 0, NormalUnorm = 1, NormalSigned = 2 };
constexpr uint32_t NormalWorldSpace = 0x10;

struct NormalSource {
    uint32_t target = 0xFF;
    uint32_t swizzle = 0;
    uint32_t mode = NormalFromDepth;
    uint32_t components = 3;
};

constexpr NormalSource GameNormalSource{.target = 0,
                                        .swizzle = 0u | 1u << 2 | 2u << 4,
                                        .mode = NormalUnorm | NormalWorldSpace,
                                        .components = 3};

constexpr bool GBufferLayoutReplaces(uint32_t kept_bound, uint32_t draw_bound) {
    return draw_bound > kept_bound;
}

inline bool ParseNormalSource(std::string_view text, NormalSource& out) {
    out = {};
    if (text.empty() || text == "depth") {
        return true;
    }
    const size_t dot = text.find('.');
    if (dot == std::string_view::npos || dot == 0) {
        return false;
    }
    uint32_t target = 0;
    for (const char c : text.substr(0, dot)) {
        if (c < '0' || c > '9') {
            return false;
        }
        target = target * 10 + uint32_t(c - '0');
    }
    if (target > 7) {
        return false;
    }
    std::string_view rest = text.substr(dot + 1);
    const size_t dot2 = rest.find('.');
    const std::string_view swizzle = rest.substr(0, dot2);
    if (swizzle.size() < 2 || swizzle.size() > 3) {
        return false;
    }
    uint32_t packed = 0;
    for (size_t i = 0; i < swizzle.size(); ++i) {
        const char c = swizzle[i];
        const uint32_t index = c == 'r' || c == 'x' ? 0 : c == 'g' || c == 'y' ? 1
                             : c == 'b' || c == 'z' ? 2 : c == 'a' || c == 'w' ? 3 : 4;
        if (index > 3) {
            return false;
        }
        packed |= index << (2 * i);
    }
    uint32_t mode = NormalUnorm;
    if (dot2 != std::string_view::npos) {
        for (const char c : rest.substr(dot2 + 1)) {
            if (c == 's') {
                mode = (mode & NormalWorldSpace) | NormalSigned;
            } else if (c == 'u') {
                mode = (mode & NormalWorldSpace) | NormalUnorm;
            } else if (c == 'w') {
                mode |= NormalWorldSpace;
            } else {
                return false;
            }
        }
    }
    out = {.target = target, .swizzle = packed, .mode = mode, .components = uint32_t(swizzle.size())};
    return true;
}

struct GiParams {
    std::array<float, 4> proj;
    std::array<float, 4> prev_proj;
    std::array<std::array<float, 4>, 3> reproject;
    std::array<std::array<float, 4>, 3> view_rows;
    std::array<float, 4> size_jitter;
    std::array<float, 4> prev_jitter_sky_clamp;
    std::array<uint32_t, 4> tlas_frame_flags;
    std::array<uint32_t, 4> cfg;
    std::array<float, 4> strength_bias;
    std::array<uint32_t, 4> bounce;
    std::array<uint32_t, 4> bounce_cfg;
    std::array<float, 4> bounce_misc;
    std::array<float, 4> shadow_color;
    std::array<uint32_t, 4> sharc;
    std::array<float, 4> far_fade;
};
static_assert(sizeof(GiParams) == 304);

constexpr uint32_t GiFlagReset = 1;
constexpr uint32_t GiFlagNormalDebug = 2;
constexpr uint32_t GiFlagNoScreen = 4;
// The sky pass (rt_sky.comp) fills the sky probe from the game's dome texture: GI must not
// learn the sky from, or look it up on, the screen, which at composite time still holds the
// grey fill the game draws before its sky.
constexpr uint32_t GiFlagSkyDome = 8;

constexpr uint32_t BounceFlagG3 = 1;
constexpr uint32_t BounceFlagG1 = 2;
constexpr uint32_t BounceFlagAlbedo = 4;
constexpr uint32_t BounceFlagLights = 8;
constexpr uint32_t BounceFlagDirSets = 16;
constexpr uint32_t MaxBounces = 8;
constexpr uint32_t MaxBounceRays = 8;
constexpr uint32_t GiSparseDefault = 4;
constexpr uint32_t GiTraceLastBounceCache = 8;

struct GiSparseBlock {
    uint32_t width = 1;
    uint32_t height = 1;
};

constexpr uint32_t GiSparseLevel(uint32_t sparse) {
    return sparse >= 4 ? 4u : sparse >= 2 ? 2u : 1u;
}

constexpr GiSparseBlock GiSparseBlockSize(uint32_t sparse) {
    const uint32_t level = GiSparseLevel(sparse);
    return level == 4 ? GiSparseBlock{2, 2} : level == 2 ? GiSparseBlock{2, 1} : GiSparseBlock{1, 1};
}

constexpr uint32_t GiSparseHash(uint32_t x) {
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}

constexpr uint32_t GiSparseTracedIndex(uint32_t block_x, uint32_t block_y, uint32_t sparse,
                                       uint32_t phase) {
    const GiSparseBlock b = GiSparseBlockSize(sparse);
    const uint32_t count = b.width * b.height;
    return (phase + GiSparseHash(block_x * 7919u + block_y * 104729u)) % count;
}

constexpr bool RestirGiFromEnv(const char* value) {
    return !value || value[0] != '0';
}

constexpr int RayReconstructionFromEnv(const char* value) {
    return value && value[0] == '1' ? 1 : 0;
}

constexpr uint32_t GiTmaxShift = 4;
constexpr uint32_t GiTmaxMax = 4095;

constexpr uint32_t GiTraceCfg(uint32_t sparse, bool last_bounce_cache, uint32_t tmax_m = 0) {
    return GiSparseLevel(sparse) | (last_bounce_cache ? GiTraceLastBounceCache : 0u) |
           ((tmax_m < GiTmaxMax ? tmax_m : GiTmaxMax) << GiTmaxShift);
}

constexpr std::array<uint32_t, 2> GiInitGroups(uint32_t width, uint32_t height, uint32_t sparse) {
    const GiSparseBlock b = GiSparseBlockSize(sparse);
    return {((width + b.width - 1) / b.width + 7) / 8, ((height + b.height - 1) / b.height + 7) / 8};
}
constexpr float RouletteMin = 0.05f;
constexpr float MissHitDistance = 1.0e4f;

constexpr uint32_t SkySlot(uint32_t records) {
    return records;
}

constexpr uint32_t BounceFlagEmissive = 32;
constexpr uint32_t BounceFlagPortals = 64; // rt_direct.h DirectFlagPortals
constexpr uint32_t BounceFlagWater = 128;
constexpr size_t FlameMaxParticles = 512;
constexpr size_t FlameMaxClusters = 32;
constexpr uint32_t FlameBlockWords = 4 + 4 * FlameMaxClusters;
constexpr float FlameClusterRadius = 0.5f;
constexpr uint32_t FlameRecordBytes = 96;
constexpr uint32_t FlameMatrixOffset = 48;
constexpr uint32_t FogColorOffset = 12;
constexpr size_t FogMaxParticles = 1024;
constexpr uint32_t FogBlockWords = 4 + 4 * FogMaxParticles;

inline std::vector<std::array<float, 3>> ClusterFlames(
    const std::vector<std::array<float, 3>>& points, float radius) {
    std::vector<std::array<float, 3>> sum;
    std::vector<float> n;
    for (const auto& p : points) {
        size_t k = 0;
        for (; k < sum.size(); ++k) {
            float d2 = 0.0f;
            for (int a = 0; a < 3; ++a) {
                const float d = sum[k][a] / n[k] - p[a];
                d2 += d * d;
            }
            if (d2 <= radius * radius) {
                break;
            }
        }
        if (k == sum.size()) {
            if (sum.size() >= FlameMaxClusters) {
                continue;
            }
            sum.push_back({});
            n.push_back(0.0f);
        }
        for (int a = 0; a < 3; ++a) {
            sum[k][a] += p[a];
        }
        n[k] += 1.0f;
    }
    for (size_t k = 0; k < sum.size(); ++k) {
        for (int a = 0; a < 3; ++a) {
            sum[k][a] /= n[k];
        }
    }
    return sum;
}
constexpr int EmissiveTargetDefault = 6;
constexpr uint32_t SkyProbeSize = 32;
constexpr uint32_t SkyProbeCells = SkyProbeSize * SkyProbeSize;

constexpr uint32_t EmissiveSlot(uint32_t records, uint32_t id) {
    return records + 1 + id;
}

constexpr uint32_t SkyProbeSlot(uint32_t records, uint32_t cell) {
    return 2 * records + 1 + cell;
}

// rgb: the fitted dome-to-screen gain; w: frames of history plus the fit residual as fraction.
constexpr uint32_t SkyGainSlot(uint32_t records) {
    return SkyProbeSlot(records, SkyProbeCells);
}

// The dome's uv mapping (u offset, u repeat, v divisor, 1 = set), written by the sky pass so
// the reflection pass can read the dome texture itself.
constexpr uint32_t SkyDomeMapSlot(uint32_t records) {
    return SkyGainSlot(records) + 1;
}

// The sky as the game shows it (clouds included), learned from the screen's sky pixels into a
// 1024x1024 octahedral map (~0.2 degree cells; 512 left the clouds in puddles soft), so mirrors
// show the real sky where the 32x32 probe gave a colour block.
constexpr uint32_t SkyImageSize = 1024;
constexpr uint32_t SkyImageCells = SkyImageSize * SkyImageSize;

constexpr uint32_t SkyImageSlot(uint32_t records, uint32_t cell) {
    return SkyDomeMapSlot(records) + 1 + cell;
}

// World direction to the moon (or sun) the sky draws: xyz, w = frames it is kept without
// being seen again (0 = none). The sky layer pass (rt_gi.comp RT_SKY_LAYER) finds the disc in
// the layer grid; rt_moon_dir.comp points light 1 of every directional set at it, so the
// moonlight comes from the moon the sky draws.
constexpr uint32_t MoonDirSlot(uint32_t records) {
    return SkyImageSlot(records, SkyImageCells);
}

// The game's moon and cloud layer (a band mesh around the camera, pixel shader SkyLayerPs),
// traced from the camera over a world azimuth x elevation grid every frame: premultiplied rgb,
// coverage. It needs no look at the sky: the layer is in the TLAS whenever the game draws it.
constexpr uint32_t SkyLayerPs = 0x765450dfu;
constexpr uint32_t SkyLayerWidth = 512;  // azimuth -180..180
constexpr uint32_t SkyLayerHeight = 128; // elevation SkyLayerElMin..SkyLayerElMax
constexpr float SkyLayerElMin = -20.0f;
constexpr float SkyLayerElMax = 90.0f;
constexpr uint32_t SkyLayerCells = SkyLayerWidth * SkyLayerHeight;
// The pass runs every SkyLayerEvery frames (~0.2 ms each time): the moon and clouds move slowly.
constexpr uint32_t SkyLayerEvery = 4;
// rt_gi.comp MoonHold: the moon slot w on the search that found the disc.
constexpr float MoonHoldSearches = 150.0f;

constexpr uint32_t SkyLayerSlot(uint32_t records, uint32_t cell) {
    return MoonDirSlot(records) + 1 + cell;
}

// Two slots after the grid: the moon search's peak score, ring mean score, peak cell x, y; then
// the found disc's distance from the camera along its direction (0 without a hit); then the
// brightest cell by luminance (rgba, then cell x, y, luminance).
constexpr uint32_t MaterialSlots(uint32_t records) {
    return SkyLayerSlot(records, SkyLayerCells) + 4;
}

// Pixel shader of the game's sky dome draw; its image 0 is the dome panorama.
constexpr uint32_t SkyDomePs = 0xdda49e89u;
constexpr uint32_t SkyDomeMaxAge = 2;

struct SkyPush {
    std::array<std::array<float, 4>, 3> view_rows;
    std::array<float, 4> proj;
    std::array<float, 4> size_jitter;
    std::array<uint32_t, 4> cfg;
    std::array<float, 4> dome;
};
static_assert(sizeof(SkyPush) == 112);

inline std::array<float, 2> OctEncode(const std::array<float, 3>& d) {
    const float l1 = Abs(d[0]) + Abs(d[1]) + Abs(d[2]);
    if (!(l1 > 0.0f)) {
        return {0.5f, 0.5f};
    }
    float x = d[0] / l1, y = d[1] / l1;
    if (d[2] < 0.0f) {
        const float ox = (1.0f - Abs(y)) * (x >= 0.0f ? 1.0f : -1.0f);
        const float oy = (1.0f - Abs(x)) * (y >= 0.0f ? 1.0f : -1.0f);
        x = ox;
        y = oy;
    }
    return {x * 0.5f + 0.5f, y * 0.5f + 0.5f};
}

inline uint32_t SkyProbeCell(const std::array<float, 3>& world_dir) {
    const auto uv = OctEncode(world_dir);
    const auto cell = [](float v) {
        return uint32_t(std::clamp(int(v * float(SkyProbeSize)), 0, int(SkyProbeSize) - 1));
    };
    return cell(uv[1]) * SkyProbeSize + cell(uv[0]);
}

inline std::array<float, 3> ViewToWorldDir(const Affine& view, const std::array<float, 3>& v) {
    return {view[0] * v[0] + view[4] * v[1] + view[8] * v[2],
            view[1] * v[0] + view[5] * v[1] + view[9] * v[2],
            view[2] * v[0] + view[6] * v[1] + view[10] * v[2]};
}

constexpr uint32_t PathSegments(uint32_t bounces) {
    return bounces == 0 ? 1u : std::min(bounces, MaxBounces);
}

constexpr float RouletteProbability(const std::array<float, 3>& throughput) {
    const float m = std::max(throughput[0], std::max(throughput[1], throughput[2]));
    return std::min(std::max(m, RouletteMin), 1.0f);
}

constexpr bool RouletteApplies(uint32_t segment) {
    return segment >= 1;
}

struct PathState {
    std::array<float, 3> sum{};
    std::array<float, 3> throughput{1.0f, 1.0f, 1.0f};
    bool alive = true;
};

constexpr void PathHit(PathState& s, const std::array<float, 3>& albedo,
                       const std::array<float, 3>& direct, const std::array<float, 3>& emission) {
    for (int i = 0; i < 3; ++i) {
        s.sum[i] += s.throughput[i] * (albedo[i] * direct[i] + emission[i]);
        s.throughput[i] *= albedo[i];
    }
}

constexpr void PathMiss(PathState& s, const std::array<float, 3>& sky) {
    for (int i = 0; i < 3; ++i) {
        s.sum[i] += s.throughput[i] * sky[i];
    }
    s.alive = false;
}

constexpr void PathRoulette(PathState& s, float u) {
    const float p = RouletteProbability(s.throughput);
    if (u >= p) {
        s.alive = false;
        return;
    }
    for (float& t : s.throughput) {
        t /= p;
    }
}

constexpr float LambertLight(float n_dot_l, float d2, float inv_r2, float cone, float visibility) {
    const float nl = std::min(std::max(n_dot_l, 0.0f), 1.0f);
    const float atten = std::max(0.0f, 1.0f / d2 - inv_r2);
    return nl * atten * cone * visibility;
}

constexpr float Luma(const std::array<float, 3>& c) {
    return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
}

constexpr bool CacheEmissive(const std::array<float, 4>& stored, float threshold) {
    return threshold > 0.0f && stored[3] > 0.0f && Luma({stored[0], stored[1], stored[2]}) > threshold;
}

constexpr float Roughness(float g1x, bool g1_valid) {
    return g1_valid ? std::min(std::max(1.0f - g1x, 0.0f), 1.0f) : 1.0f;
}

// 0.4: rougher pixels keep the game's specular (forest 37.7 -> 42.5 FPS, same look; 0.6 before).
constexpr float ReflectMaxRoughDefault = 0.4f;
constexpr float ReflectMinAlpha = 1e-3f;
constexpr uint32_t ReflectFlagPreAo = 1;
constexpr uint32_t ReflectFlagF0 = 2;
constexpr float ReflectDielectricF0 = 0.04f;
constexpr float ReflectMaxWeight = 1.0f;

constexpr bool ReflectTraced(float roughness, float max_rough) {
    return roughness <= max_rough;
}

constexpr float GgxAlpha(float roughness) {
    return std::max(roughness * roughness, ReflectMinAlpha);
}

struct HalfSample {
    std::array<float, 3> local;
    float cos_theta;
};

inline HalfSample GgxSampleHalf(float u1, float u2, float alpha) {
    const float a2 = alpha * alpha;
    const float cos_theta = std::sqrt(std::max(0.0f, (1.0f - u1) / (1.0f + (a2 - 1.0f) * u1)));
    const float sin_theta = std::sqrt(std::max(0.0f, 1.0f - cos_theta * cos_theta));
    const float phi = 2.0f * Pi * u2;
    return {{sin_theta * std::cos(phi), sin_theta * std::sin(phi), cos_theta}, cos_theta};
}

constexpr float GgxD(float alpha, float n_dot_h) {
    const float a2 = alpha * alpha;
    const float d = n_dot_h * n_dot_h * (a2 - 1.0f) + 1.0f;
    return a2 / (Pi * d * d);
}

constexpr float GgxPdf(float alpha, float n_dot_h, float v_dot_h) {
    return GgxD(alpha, n_dot_h) * n_dot_h / (4.0f * std::max(v_dot_h, 1e-4f));
}

inline float GgxG1(float alpha, float n_dot_x) {
    const float a2 = alpha * alpha;
    const float c = std::max(n_dot_x, 1e-4f);
    return 2.0f * c / (c + std::sqrt(a2 + (1.0f - a2) * c * c));
}

constexpr float SchlickFresnel(float f0, float v_dot_h) {
    const float m = 1.0f - std::min(std::max(v_dot_h, 0.0f), 1.0f);
    const float m2 = m * m;
    return f0 + (1.0f - f0) * m2 * m2 * m;
}

inline float GgxWeight(float alpha, float n_dot_l, float n_dot_v, float n_dot_h, float v_dot_h) {
    if (n_dot_l <= 0.0f || n_dot_v <= 0.0f || n_dot_h <= 0.0f) {
        return 0.0f;
    }
    const float g = GgxG1(alpha, n_dot_l) * GgxG1(alpha, n_dot_v);
    return std::min(g * v_dot_h / (n_dot_v * n_dot_h), ReflectMaxWeight);
}

constexpr float AoFromIrradiance(float luma_after, float luma_before) {
    if (luma_before <= 1e-4f) {
        return 1.0f;
    }
    return std::min(std::max(luma_after / luma_before, 0.0f), 1.0f);
}

constexpr float HitDefaultF0 = 0.04f;
constexpr float HitDefaultRoughness = 0.6f;
constexpr float HitMinAlpha = 0.02f;
constexpr float HitSpecMax = 16.0f;

struct HitMaterial {
    float f0;
    float roughness;
    bool from_screen;
};

constexpr HitMaterial HitMaterialFrom(bool on_screen, float g3, bool g3_valid, float g1x,
                                      bool g1_valid) {
    if (!on_screen) {
        return {HitDefaultF0, HitDefaultRoughness, false};
    }
    const float c = std::min(std::max(g3, 0.0f), 1.0f);
    return {g3_valid ? c * c : HitDefaultF0, g1_valid ? Roughness(g1x, true) : HitDefaultRoughness,
            true};
}

constexpr float HitSpecAlpha(float roughness) {
    const float r = std::min(std::max(roughness, 0.0f), 1.0f);
    return std::max(r * r, HitMinAlpha);
}

inline float HitSpecular(float alpha, float f0, float n_dot_l, float n_dot_v, float n_dot_h,
                         float v_dot_h) {
    if (n_dot_l <= 0.0f || n_dot_v <= 0.0f || n_dot_h <= 0.0f) {
        return 0.0f;
    }
    const float g = GgxG1(alpha, n_dot_l) * GgxG1(alpha, n_dot_v);
    const float s = Pi * GgxD(alpha, n_dot_h) * g * SchlickFresnel(f0, v_dot_h) /
                    (4.0f * n_dot_l * n_dot_v);
    return std::min(s, HitSpecMax);
}


constexpr uint32_t MaterialScale = 256;
constexpr float MaterialMaxRadiance = 16.0f;
constexpr uint32_t MaterialSampleMask = 31;
constexpr float MaterialBlend = 0.125f;
constexpr float MaterialMaxFrames = 255.0f;

constexpr uint32_t MaterialEncode(float radiance) {
    const float v = radiance != radiance ? 0.0f : std::min(std::max(radiance, 0.0f), MaterialMaxRadiance);
    return uint32_t(v * float(MaterialScale) + 0.5f);
}

constexpr bool MaterialSampled(uint32_t x, uint32_t y, uint32_t frame) {
    return ((x * 7u + y * 13u + frame) & MaterialSampleMask) == 0u;
}

constexpr bool MaterialSumSafe(uint32_t pixels) {
    const uint64_t adds = (uint64_t(pixels) + MaterialSampleMask) / (MaterialSampleMask + 1);
    return adds * uint64_t(MaterialEncode(MaterialMaxRadiance)) <= 0xFFFFFFFFull;
}

constexpr std::array<float, 4> MaterialResolve(const std::array<float, 4>& stored,
                                               const std::array<uint32_t, 4>& sum) {
    if (sum[3] == 0) {
        return stored;
    }
    const float inv = 1.0f / (float(sum[3]) * float(MaterialScale));
    const std::array<float, 3> mean{float(sum[0]) * inv, float(sum[1]) * inv, float(sum[2]) * inv};
    const float w = stored[3] > 0.0f ? MaterialBlend : 1.0f;
    return {stored[0] + (mean[0] - stored[0]) * w, stored[1] + (mean[1] - stored[1]) * w,
            stored[2] + (mean[2] - stored[2]) * w, std::min(stored[3] + 1.0f, MaterialMaxFrames)};
}

constexpr bool MaterialValid(const std::array<float, 4>& stored) {
    return stored[3] > 0.0f;
}

enum HitSource : uint32_t { SourceSky = 0, SourceScreen = 1, SourceMaterial = 2, SourceMiss = 3 };

constexpr uint32_t GiDebugSource = 8;

constexpr std::array<std::array<float, 4>, 3> Rows(const Affine& m) {
    return {{{m[0], m[1], m[2], m[3]}, {m[4], m[5], m[6], m[7]}, {m[8], m[9], m[10], m[11]}}};
}

}

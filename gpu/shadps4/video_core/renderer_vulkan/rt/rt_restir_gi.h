#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "rt_gi.h"

namespace Vulkan::Rt {

using Vec3 = std::array<float, 3>;

constexpr uint32_t RestirGiMClampDefault = 30;
constexpr uint32_t RestirGiMaxAgeDefault = 32;
constexpr uint32_t RestirGiSpatialDefault = 1;
constexpr uint32_t RestirGiMaxSpatial = 2;
constexpr uint32_t RestirGiNeighboursDefault = 3;
constexpr uint32_t RestirGiMaxNeighbours = 8;
constexpr float RestirGiRadiusDefault = 16.0f;
constexpr float RestirGiJacobianDefault = 10.0f;
constexpr float RestirGiDepthTolerance = 0.1f;
constexpr float RestirGiNormalTolerance = 0.9f;
constexpr float RestirGiMaxW = 65504.0f;
constexpr uint32_t RestirGiSets = 3;

struct RestirGiParams {
    std::array<float, 4> proj;
    std::array<float, 4> prev_proj;
    std::array<std::array<float, 4>, 3> reproject;
    std::array<std::array<float, 4>, 3> unproject;
    std::array<float, 4> size_jitter;
    std::array<float, 4> prev_jitter_bias_gap;
    std::array<uint32_t, 4> tlas_frame_flags;
    std::array<uint32_t, 4> cfg;
    std::array<float, 4> misc;
};
static_assert(sizeof(RestirGiParams) == 208);

constexpr uint32_t RestirGiFlagReset = 1;

struct GiReservoir {
    Vec3 position{};
    Vec3 normal{};
    Vec3 radiance{};
    float w_sum = 0.0f;
    float M = 0.0f;
    float W = 0.0f;
    float age = 0.0f;
};

constexpr Vec3 Sub(const Vec3& a, const Vec3& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

constexpr float Dot(const Vec3& a, const Vec3& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

inline float Length(const Vec3& v) {
    return std::sqrt(Dot(v, v));
}

inline float ReceiverCos(const Vec3& sample, const Vec3& receiver, const Vec3& receiver_normal) {
    const Vec3 d = Sub(sample, receiver);
    const float len = Length(d);
    if (len <= 1e-6f) {
        return 0.0f;
    }
    return std::max(Dot(d, receiver_normal) / len, 0.0f);
}

inline float GiTargetPdf(const Vec3& radiance, const Vec3& sample, const Vec3& receiver,
                         const Vec3& receiver_normal) {
    return Luma(radiance) * ReceiverCos(sample, receiver, receiver_normal);
}

inline GiReservoir GiInitialReservoir(const Vec3& sample, const Vec3& sample_normal,
                                      const Vec3& radiance, float cos_theta) {
    GiReservoir r{};
    r.position = sample;
    r.normal = sample_normal;
    r.radiance = radiance;
    r.M = 1.0f;
    r.W = cos_theta > 1e-6f ? std::min(Pi / cos_theta, RestirGiMaxW) : 0.0f;
    r.w_sum = 0.0f;
    return r;
}

inline float GiJacobian(const Vec3& sample, const Vec3& sample_normal, const Vec3& from_receiver,
                        const Vec3& to_receiver) {
    const Vec3 dq = Sub(from_receiver, sample);
    const Vec3 dr = Sub(to_receiver, sample);
    const float lq2 = Dot(dq, dq), lr2 = Dot(dr, dr);
    if (lq2 <= 1e-12f || lr2 <= 1e-12f) {
        return 0.0f;
    }
    const float cos_q = Dot(dq, sample_normal) / std::sqrt(lq2);
    const float cos_r = Dot(dr, sample_normal) / std::sqrt(lr2);
    if (cos_q <= 1e-4f || cos_r <= 1e-4f) {
        return 0.0f;
    }
    return (cos_r / cos_q) * (lq2 / lr2);
}

inline bool GiJacobianOk(float j, float max_ratio) {
    if (!(j > 0.0f)) {
        return false;
    }
    return max_ratio <= 0.0f || j <= max_ratio;
}

inline bool GiReservoirMerge(GiReservoir& r, const GiReservoir& in, float target_pdf, float u,
                             float m_clamp) {
    const float m = std::min(in.M, m_clamp);
    if (m <= 0.0f) {
        return false;
    }
    const float w = target_pdf * in.W * m;
    r.M += m;
    if (!(w > 0.0f) || w != w) {
        return false;
    }
    r.w_sum += w;
    if (u * r.w_sum < w) {
        r.position = in.position;
        r.normal = in.normal;
        r.radiance = in.radiance;
        r.age = in.age;
        return true;
    }
    return false;
}

inline void GiReservoirFinalize(GiReservoir& r, float target_pdf) {
    if (r.M <= 0.0f || target_pdf <= 0.0f || r.w_sum <= 0.0f) {
        r.W = 0.0f;
        return;
    }
    r.W = std::min(r.w_sum / (r.M * target_pdf), RestirGiMaxW);
}

inline Vec3 GiResolve(const GiReservoir& r, const Vec3& receiver, const Vec3& receiver_normal) {
    const float scale = ReceiverCos(r.position, receiver, receiver_normal) / Pi * r.W;
    return {r.radiance[0] * scale, r.radiance[1] * scale, r.radiance[2] * scale};
}

constexpr uint32_t RestirGiSurfaceGlass = 1u << 28;
constexpr uint32_t RestirGiMaskSolid = 0x01u;
constexpr uint32_t RestirGiMaskPortal = 0x08u;
constexpr float RestirGiPortalWall = 1.5f;

struct GiRayHit {
    float t = 0.0f;
    uint32_t mask = RestirGiMaskSolid;
    uint32_t surface_flags = 0u;
    bool alpha_passes = true;
};

template <typename Hits>
bool GiBlockedSpan(const Hits& hits, float t_min, float t_max) {
    for (const GiRayHit& h : hits) {
        if ((h.mask & RestirGiMaskSolid) == 0u || h.t < t_min || h.t > t_max) {
            continue;
        }
        if ((h.surface_flags & RestirGiSurfaceGlass) != 0u) {
            continue;
        }
        if (h.alpha_passes) {
            return true;
        }
    }
    return false;
}

template <typename Hits>
bool GiBlocked(const Hits& hits, float t_max) {
    bool pane = false;
    float t_pane = t_max;
    for (const GiRayHit& h : hits) {
        if ((h.mask & RestirGiMaskPortal) != 0u && h.t >= 0.0f && h.t <= t_max &&
            (!pane || h.t < t_pane)) {
            pane = true;
            t_pane = h.t;
        }
    }
    if (pane) {
        return GiBlockedSpan(hits, 0.0f, t_pane) ||
               (t_pane + RestirGiPortalWall < t_max &&
                GiBlockedSpan(hits, t_pane + RestirGiPortalWall, t_max));
    }
    return GiBlockedSpan(hits, 0.0f, t_max);
}

constexpr bool GiTemporalAccept(float reprojected_z, float stored_z, float normal_dot, float age,
                                float max_age) {
    return HistoryAccept(reprojected_z, stored_z, RestirGiDepthTolerance, 0.02f) &&
           normal_dot >= RestirGiNormalTolerance && age < max_age;
}

constexpr uint32_t RestirGiInitSet(uint32_t history) {
    return (history + 1) % RestirGiSets;
}

constexpr uint32_t RestirGiTemporalSet(uint32_t history) {
    return (history + 2) % RestirGiSets;
}

constexpr uint32_t RestirGiFinalSet(uint32_t history, uint32_t spatial_passes) {
    return spatial_passes % 2 == 1 ? RestirGiInitSet(history) : RestirGiTemporalSet(history);
}

}

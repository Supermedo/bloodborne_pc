#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include "gpu/shadps4/video_core/renderer_vulkan/rt/rt_geometry.h"
#include "gpu/shadps4/video_core/renderer_vulkan/rt/rt_restir_gi.h"
#include "gpu/shadps4/video_core/renderer_vulkan/rt/rt_surface.h"

using namespace Vulkan::Rt;

namespace {

bool Near(float a, float b, float eps = 1e-4f) {
    return std::fabs(a - b) < eps;
}

uint32_t Hash(uint32_t x) {
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}

float Random(uint32_t& state) {
    state = Hash(state);
    return float(state & 0x00FFFFFFu) / 16777216.0f;
}

constexpr float CeilingZ = 1.0f;

Vec3 CeilingRadiance(const Vec3& p) {
    return {1.0f + 0.8f * std::sin(2.0f * p[0]), 0.5f + 0.4f * std::cos(1.5f * p[1]),
            0.25f + 0.2f * std::sin(p[0] + p[1])};
}

struct Sample {
    Vec3 position;
    Vec3 normal;
    Vec3 radiance;
    float cos_theta;
    bool hit;
};

Sample TraceFrom(const Vec3& receiver, uint32_t& state) {
    const auto cs = CosineHemisphere(Random(state), Random(state));
    const Vec3 dir = TangentToView(cs.direction, {0.0f, 0.0f, 1.0f});
    if (dir[2] <= 1e-4f) {
        return {{}, {}, {}, 0.0f, false};
    }
    const float t = (CeilingZ - receiver[2]) / dir[2];
    const Vec3 hit{receiver[0] + dir[0] * t, receiver[1] + dir[1] * t, CeilingZ};
    return {hit, {0.0f, 0.0f, -1.0f}, CeilingRadiance(hit), dir[2], true};
}

Vec3 BruteForce(const Vec3& receiver, int count, uint32_t& state) {
    Vec3 sum{};
    for (int i = 0; i < count; ++i) {
        const Sample s = TraceFrom(receiver, state);
        if (!s.hit) {
            continue;
        }
        for (int c = 0; c < 3; ++c) {
            sum[c] += s.radiance[c];
        }
    }
    return {sum[0] / count, sum[1] / count, sum[2] / count};
}

void TestInitialReservoirResolvesToRadiance() {
    uint32_t state = 77u;
    const Vec3 receiver{0.0f, 0.0f, 0.0f};
    const Vec3 n{0.0f, 0.0f, 1.0f};
    for (int i = 0; i < 1000; ++i) {
        const Sample s = TraceFrom(receiver, state);
        if (!s.hit) {
            continue;
        }
        const GiReservoir r = GiInitialReservoir(s.position, s.normal, s.radiance, s.cos_theta);
        const Vec3 out = GiResolve(r, receiver, n);
        for (int c = 0; c < 3; ++c) {
            assert(Near(out[c], s.radiance[c], 2e-3f * (1.0f + s.radiance[c])));
        }
    }
    const GiReservoir grazing = GiInitialReservoir({1, 0, 0}, {0, 0, -1}, {1, 1, 1}, 0.0f);
    assert(grazing.W == 0.0f);
}

GiReservoir MergeTemporal(const Vec3& receiver, const Vec3& n, const GiReservoir& history,
                          uint32_t& state, float m_clamp) {
    GiReservoir r{};
    const Sample s = TraceFrom(receiver, state);
    if (s.hit) {
        const GiReservoir init = GiInitialReservoir(s.position, s.normal, s.radiance, s.cos_theta);
        GiReservoirMerge(r, init, GiTargetPdf(init.radiance, init.position, receiver, n),
                         Random(state), m_clamp);
    }
    if (history.M > 0.0f) {
        GiReservoirMerge(r, history, GiTargetPdf(history.radiance, history.position, receiver, n),
                         Random(state), m_clamp);
    }
    GiReservoirFinalize(r, GiTargetPdf(r.radiance, r.position, receiver, n));
    return r;
}

void TestTemporalMergeUnbiased() {
    uint32_t state = 4242u;
    const Vec3 receiver{0.3f, -0.2f, 0.0f};
    const Vec3 n{0.0f, 0.0f, 1.0f};
    const Vec3 reference = BruteForce(receiver, 400000, state);
    const int trials = 60000;
    const int frames = 8;
    Vec3 sum{};
    float max_m = 0.0f;
    for (int t = 0; t < trials; ++t) {
        GiReservoir history{};
        for (int f = 0; f < frames; ++f) {
            history = MergeTemporal(receiver, n, history, state, 30.0f);
        }
        max_m = std::max(max_m, history.M);
        const Vec3 out = GiResolve(history, receiver, n);
        for (int c = 0; c < 3; ++c) {
            sum[c] += out[c];
        }
    }
    assert(Near(max_m, float(frames), 1e-3f));
    for (int c = 0; c < 3; ++c) {
        const float mean = sum[c] / trials;
        assert(std::fabs(mean - reference[c]) <= 0.03f * reference[c]);
    }
}

void TestMClamp() {
    uint32_t state = 9u;
    const Vec3 receiver{0.0f, 0.0f, 0.0f};
    const Vec3 n{0.0f, 0.0f, 1.0f};
    GiReservoir history{};
    for (int f = 0; f < 100; ++f) {
        history = MergeTemporal(receiver, n, history, state, 30.0f);
    }
    assert(history.M <= 31.0f + 1e-3f);
}

void TestJacobian() {
    const Vec3 sample{0.0f, 0.0f, 1.0f};
    const Vec3 sample_normal{0.0f, 0.0f, -1.0f};
    const Vec3 same{0.0f, 0.0f, 0.0f};
    assert(Near(GiJacobian(sample, sample_normal, same, same), 1.0f));
    const Vec3 from{1.0f, 0.0f, 0.0f};
    const Vec3 to{0.0f, 0.0f, 0.0f};
    const float j = GiJacobian(sample, sample_normal, from, to);
    const float cos_q = 1.0f / std::sqrt(2.0f), cos_r = 1.0f;
    assert(Near(j, (cos_r / cos_q) * (2.0f / 1.0f), 1e-4f));
    assert(GiJacobian(sample, sample_normal, {0.0f, 0.0f, 2.0f}, to) == 0.0f);
    assert(GiJacobian(sample, sample_normal, from, {0.0f, 0.0f, 1.0f}) == 0.0f);
}

void TestJacobianLimit() {
    assert(GiJacobianOk(1.0f, 10.0f));
    assert(GiJacobianOk(9.9f, 10.0f));
    assert(GiJacobianOk(0.11f, 10.0f));
    assert(!GiJacobianOk(10.5f, 10.0f));
    assert(GiJacobianOk(0.05f, 10.0f));
    assert(!GiJacobianOk(0.0f, 10.0f));
    assert(GiJacobianOk(500.0f, 0.0f));
    assert(!GiJacobianOk(0.0f, 0.0f));
    assert(RestirGiJacobianDefault > 1.0f);
}

Vec3 SpatialEstimate(const Vec3& receiver, const Vec3& n, const std::array<Vec3, 4>& neighbours,
                     uint32_t& state, bool jacobian) {
    GiReservoir r{};
    const Sample own = TraceFrom(receiver, state);
    if (own.hit) {
        const GiReservoir init = GiInitialReservoir(own.position, own.normal, own.radiance, own.cos_theta);
        GiReservoirMerge(r, init, GiTargetPdf(init.radiance, init.position, receiver, n),
                         Random(state), 30.0f);
    }
    for (const Vec3& q : neighbours) {
        const Sample s = TraceFrom(q, state);
        if (!s.hit) {
            continue;
        }
        const GiReservoir rq = GiInitialReservoir(s.position, s.normal, s.radiance, s.cos_theta);
        const float j = jacobian ? GiJacobian(rq.position, rq.normal, q, receiver) : 1.0f;
        if (j <= 0.0f) {
            continue;
        }
        GiReservoirMerge(r, rq, GiTargetPdf(rq.radiance, rq.position, receiver, n) * j,
                         Random(state), 30.0f);
    }
    GiReservoirFinalize(r, GiTargetPdf(r.radiance, r.position, receiver, n));
    return GiResolve(r, receiver, n);
}

void TestSpatialReuseMatchesBruteForce() {
    uint32_t state = 31337u;
    const Vec3 receiver{0.0f, 0.0f, 0.0f};
    const Vec3 n{0.0f, 0.0f, 1.0f};
    const std::array<Vec3, 4> neighbours{{{0.6f, 0.0f, 0.0f},
                                          {-0.6f, 0.0f, 0.0f},
                                          {0.0f, 0.6f, 0.0f},
                                          {0.0f, -0.6f, 0.0f}}};
    const Vec3 reference = BruteForce(receiver, 400000, state);
    const int trials = 120000;
    Vec3 with{}, without{};
    for (int t = 0; t < trials; ++t) {
        const Vec3 a = SpatialEstimate(receiver, n, neighbours, state, true);
        const Vec3 b = SpatialEstimate(receiver, n, neighbours, state, false);
        for (int c = 0; c < 3; ++c) {
            with[c] += a[c];
            without[c] += b[c];
        }
    }
    float worst_without = 0.0f;
    for (int c = 0; c < 3; ++c) {
        const float mean = with[c] / trials;
        assert(std::fabs(mean - reference[c]) <= 0.03f * reference[c]);
        worst_without = std::max(worst_without,
                                 std::fabs(without[c] / trials - reference[c]) / reference[c]);
    }
    assert(worst_without > 0.03f);
}

void TestFinalizeAndSets() {
    GiReservoir r{};
    r.M = 2.0f;
    r.w_sum = 4.0f;
    GiReservoirFinalize(r, 0.5f);
    assert(Near(r.W, 4.0f));
    GiReservoirFinalize(r, 0.0f);
    assert(r.W == 0.0f);
    GiReservoir empty{};
    GiReservoir in{};
    in.M = 0.0f;
    in.W = 1.0f;
    assert(!GiReservoirMerge(empty, in, 1.0f, 0.5f, 30.0f) && empty.M == 0.0f);
    static_assert(RestirGiInitSet(0) == 1 && RestirGiTemporalSet(0) == 2);
    static_assert(RestirGiFinalSet(0, 1) == 1 && RestirGiFinalSet(0, 2) == 2 &&
                  RestirGiFinalSet(0, 0) == 2);
    for (uint32_t h = 0; h < RestirGiSets; ++h) {
        assert(RestirGiInitSet(h) != h && RestirGiTemporalSet(h) != h &&
               RestirGiInitSet(h) != RestirGiTemporalSet(h));
        const uint32_t next = RestirGiFinalSet(h, 1);
        assert(RestirGiInitSet(next) != next);
    }
    assert(GiTemporalAccept(10.0f, 10.5f, 0.95f, 3.0f, 32.0f));
    assert(!GiTemporalAccept(10.0f, 10.5f, 0.95f, 32.0f, 32.0f));
    assert(!GiTemporalAccept(10.0f, 12.0f, 0.95f, 3.0f, 32.0f));
    assert(!GiTemporalAccept(10.0f, 10.5f, 0.5f, 3.0f, 32.0f));
}

}

void TestSparseTemporalUnbiased() {
    uint32_t state = 5151u;
    const Vec3 receiver{-0.1f, 0.25f, 0.0f};
    const Vec3 n{0.0f, 0.0f, 1.0f};
    const Vec3 reference = BruteForce(receiver, 400000, state);
    const int trials = 60000;
    const int frames = 9;
    Vec3 sum{};
    float max_m = 0.0f;
    for (int t = 0; t < trials; ++t) {
        GiReservoir history{};
        for (int f = 0; f < frames; ++f) {
            if (f % 4 == 0) {
                history = MergeTemporal(receiver, n, history, state, 30.0f);
                continue;
            }
            GiReservoir r{};
            GiReservoirMerge(r, history, GiTargetPdf(history.radiance, history.position, receiver, n),
                             Random(state), 30.0f);
            GiReservoirFinalize(r, GiTargetPdf(r.radiance, r.position, receiver, n));
            history = r;
        }
        max_m = std::max(max_m, history.M);
        const Vec3 out = GiResolve(history, receiver, n);
        for (int c = 0; c < 3; ++c) {
            sum[c] += out[c];
        }
    }
    assert(Near(max_m, 3.0f, 1e-3f));
    for (int c = 0; c < 3; ++c) {
        const float mean = sum[c] / trials;
        assert(std::fabs(mean - reference[c]) <= 0.03f * reference[c]);
    }
}

enum class Window { Glass, Painted, Wall };

std::array<GiRayHit, 2> WindowHits(Window w, float t_max) {
    const float t_pane = 0.3f * t_max;
    switch (w) {
    case Window::Glass:
        return {{{t_pane, RestirGiMaskSolid, RestirGiSurfaceGlass, true},
                 {t_pane, RestirGiMaskSolid, RestirGiSurfaceGlass, true}}};
    case Window::Painted:
        return {{{t_pane, RestirGiMaskPortal, SurfacePortal, true},
                 {t_pane + 0.5f * RestirGiPortalWall, RestirGiMaskSolid, 0u, true}}};
    case Window::Wall:
        break;
    }
    return {{{t_pane, RestirGiMaskSolid, 0u, true}, {t_pane, RestirGiMaskSolid, 0u, true}}};
}

Vec3 WindowSpatialEstimate(Window w, const Vec3& receiver, const Vec3& n,
                           const std::array<Vec3, 4>& neighbours, uint32_t& state) {
    GiReservoir r{};
    const Sample own = TraceFrom(receiver, state);
    if (own.hit) {
        const GiReservoir init = GiInitialReservoir(own.position, own.normal, own.radiance, own.cos_theta);
        GiReservoirMerge(r, init, GiTargetPdf(init.radiance, init.position, receiver, n),
                         Random(state), 30.0f);
    }
    bool from_neighbour = false;
    for (const Vec3& q : neighbours) {
        const Sample s = TraceFrom(q, state);
        if (!s.hit) {
            continue;
        }
        const GiReservoir rq = GiInitialReservoir(s.position, s.normal, s.radiance, s.cos_theta);
        const float j = GiJacobian(rq.position, rq.normal, q, receiver);
        if (j <= 0.0f) {
            continue;
        }
        if (GiReservoirMerge(r, rq, GiTargetPdf(rq.radiance, rq.position, receiver, n) * j,
                             Random(state), 30.0f)) {
            from_neighbour = true;
        }
    }
    float target = GiTargetPdf(r.radiance, r.position, receiver, n);
    if (from_neighbour && r.position[0] > 0.0f) {
        const float dx = r.position[0] - receiver[0];
        const float dy = r.position[1] - receiver[1];
        const float dz = r.position[2] - receiver[2];
        const float t_max = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (GiBlocked(WindowHits(w, t_max), t_max)) {
            target = 0.0f;
        }
    }
    GiReservoirFinalize(r, target);
    return GiResolve(r, receiver, n);
}

Vec3 WindowSpatialMean(Window w, uint32_t& state) {
    const Vec3 receiver{0.0f, 0.0f, 0.0f};
    const Vec3 n{0.0f, 0.0f, 1.0f};
    const std::array<Vec3, 4> neighbours{{{0.6f, 0.0f, 0.0f},
                                          {-0.6f, 0.0f, 0.0f},
                                          {0.0f, 0.6f, 0.0f},
                                          {0.0f, -0.6f, 0.0f}}};
    const int trials = 120000;
    Vec3 sum{};
    for (int t = 0; t < trials; ++t) {
        const Vec3 a = WindowSpatialEstimate(w, receiver, n, neighbours, state);
        for (int c = 0; c < 3; ++c) {
            sum[c] += a[c];
        }
    }
    return {sum[0] / trials, sum[1] / trials, sum[2] / trials};
}

void TestSpatialSeesThroughWindows() {
    uint32_t state = 4242u;
    const Vec3 reference = BruteForce({0.0f, 0.0f, 0.0f}, 400000, state);
    const Vec3 glass = WindowSpatialMean(Window::Glass, state);
    const Vec3 painted = WindowSpatialMean(Window::Painted, state);
    const Vec3 wall = WindowSpatialMean(Window::Wall, state);
    for (int c = 0; c < 3; ++c) {
        std::printf("window spatial c%d glass %.4f painted %.4f wall %.4f of %.4f\n", c, glass[c],
                    painted[c], wall[c], reference[c]);
        std::fflush(stdout);
        assert(std::fabs(glass[c] - reference[c]) <= 0.03f * reference[c]);
        assert(std::fabs(painted[c] - reference[c]) <= 0.03f * reference[c]);
        assert(wall[c] < 0.8f * reference[c]);
    }
}

void TestBlockedGlassAndPortals() {
    const std::array<GiRayHit, 1> glass{{{4.0f, RestirGiMaskSolid, RestirGiSurfaceGlass, true}}};
    assert(!GiBlocked(glass, 20.0f));
    const std::array<GiRayHit, 1> wall{{{4.0f, RestirGiMaskSolid, 0u, true}}};
    assert(GiBlocked(wall, 20.0f));
    assert(!GiBlocked(wall, 3.0f));
    const std::array<GiRayHit, 1> cutout{{{4.0f, RestirGiMaskSolid, 0u, false}}};
    assert(!GiBlocked(cutout, 20.0f));
    const std::array<GiRayHit, 1> pane_only{{{4.0f, RestirGiMaskPortal, SurfacePortal, true}}};
    assert(!GiBlocked(pane_only, 20.0f));
    const std::array<GiRayHit, 2> behind_pane{{{4.0f, RestirGiMaskPortal, SurfacePortal, true},
                                               {5.0f, RestirGiMaskSolid, 0u, true}}};
    assert(!GiBlocked(behind_pane, 20.0f));
    assert(!GiBlocked(behind_pane, 5.2f));
    const std::array<GiRayHit, 2> past_wall{{{4.0f, RestirGiMaskPortal, SurfacePortal, true},
                                             {6.0f, RestirGiMaskSolid, 0u, true}}};
    assert(GiBlocked(past_wall, 20.0f));
    assert(!GiBlocked(past_wall, 5.4f));
    const std::array<GiRayHit, 2> before_pane{{{4.0f, RestirGiMaskPortal, SurfacePortal, true},
                                               {3.0f, RestirGiMaskSolid, 0u, true}}};
    assert(GiBlocked(before_pane, 20.0f));
    const std::array<GiRayHit, 3> two_panes{{{9.0f, RestirGiMaskPortal, SurfacePortal, true},
                                             {4.0f, RestirGiMaskPortal, SurfacePortal, true},
                                             {5.0f, RestirGiMaskSolid, 0u, true}}};
    assert(!GiBlocked(two_panes, 20.0f));
    const std::array<GiRayHit, 2> far_pane{{{30.0f, RestirGiMaskPortal, SurfacePortal, true},
                                            {5.0f, RestirGiMaskSolid, 0u, true}}};
    assert(GiBlocked(far_pane, 20.0f));
    static_assert(RestirGiSurfaceGlass == SurfaceGlass);
    static_assert(RestirGiMaskSolid == MaskSolid);
    static_assert(RestirGiMaskPortal == MaskPortal);
}

void TestDefaults() {
    assert(RestirGiFromEnv(nullptr));
    assert(RestirGiFromEnv(""));
    assert(RestirGiFromEnv("1"));
    assert(!RestirGiFromEnv("0"));
    assert(RayReconstructionFromEnv(nullptr) == 0);
    assert(RayReconstructionFromEnv("") == 0);
    assert(RayReconstructionFromEnv("1") == 1);
    assert(RayReconstructionFromEnv("0") == 0);
    assert(GiSparseDefault == 4);
}

int main() {
    TestDefaults();
    TestBlockedGlassAndPortals();
    TestSpatialSeesThroughWindows();
    TestInitialReservoirResolvesToRadiance();
    TestTemporalMergeUnbiased();
    TestSparseTemporalUnbiased();
    TestMClamp();
    TestJacobian();
    TestJacobianLimit();
    TestSpatialReuseMatchesBruteForce();
    TestFinalizeAndSets();
    std::puts("rt-restir-gi-test: ok");
    return 0;
}

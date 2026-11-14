#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include "gpu/shadps4/video_core/renderer_vulkan/rt/rt_gi.h"
#include "gpu/shadps4/video_core/renderer_vulkan/rt/rt_gi_pipeline.h"
#include "gpu/shadps4/video_core/renderer_vulkan/rt/rt_moon_cache.h"
#include "gpu/shadps4/video_core/renderer_vulkan/rt/rt_sky_cache.h"

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

void TestCosineSampling() {
    uint32_t state = 12345u;
    const int count = 200000;
    double sum_cos = 0.0, sum_pdf_ratio = 0.0;
    std::array<int, 4> quadrants{};
    for (int i = 0; i < count; ++i) {
        const float u1 = Random(state), u2 = Random(state);
        const auto s = CosineHemisphere(u1, u2);
        const auto& d = s.direction;
        const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        assert(Near(len, 1.0f, 1e-3f));
        assert(d[2] >= 0.0f);
        assert(Near(s.pdf, d[2] / Pi, 1e-4f));
        sum_cos += d[2];
        sum_pdf_ratio += d[2] / (s.pdf > 0.0f ? s.pdf * Pi : 1.0f);
        quadrants[(d[0] >= 0.0f ? 1 : 0) + (d[1] >= 0.0f ? 2 : 0)] += 1;
    }
    const double mean_cos = sum_cos / count;
    assert(std::fabs(mean_cos - 2.0 / 3.0) < 0.01);
    assert(std::fabs(sum_pdf_ratio / count - 1.0) < 1e-3);
    for (const int q : quadrants) {
        assert(std::fabs(double(q) / count - 0.25) < 0.01);
    }

    const auto up = CosineHemisphere(0.0f, 0.0f);
    assert(Near(up.direction[2], 1.0f) && Near(up.pdf, 1.0f / Pi));
    const auto grazing = CosineHemisphere(1.0f, 0.25f);
    assert(Near(grazing.direction[2], 0.0f) && Near(grazing.pdf, 0.0f));

    const std::array<float, 3> n{0.0f, 0.0f, -1.0f};
    const auto world = TangentToView(up.direction, n);
    assert(Near(world[0], 0.0f) && Near(world[1], 0.0f) && Near(world[2], -1.0f));
    const std::array<float, 3> n2{0.6f, 0.0f, 0.8f};
    for (int i = 0; i < 1000; ++i) {
        const auto s = CosineHemisphere(Random(state), Random(state));
        const auto w = TangentToView(s.direction, n2);
        const float dot = w[0] * n2[0] + w[1] * n2[1] + w[2] * n2[2];
        assert(dot >= -1e-4f && Near(dot, s.direction[2], 1e-3f));
    }
}

void TestScreenReprojection() {
    const std::array<float, 4> proj{1.42799f, 2.53865f, 1.00002f, -0.0500679f};
    const Screen screen{{1920.0f, 1080.0f}, {0.25f, -0.125f}};
    const std::array<float, 3> p{3.0f, -1.5f, 12.0f};
    const auto pixel = ViewToPixel(p, proj, screen);
    const float depth = ViewToDepth(p[2], proj);
    const auto back = PixelToView(pixel, depth, proj, screen);
    for (int i = 0; i < 3; ++i) {
        assert(Near(back[i], p[i], 1e-3f));
    }
    const Screen unjittered{{1920.0f, 1080.0f}, {0.0f, 0.0f}};
    const auto centre = ViewToPixel({0.0f, 0.0f, 5.0f}, proj, unjittered);
    assert(Near(centre[0], 959.5f) && Near(centre[1], 539.5f));
    const auto right = ViewToPixel({1.0f, 0.0f, 5.0f}, proj, unjittered);
    assert(right[0] > centre[0]);
    const auto up = ViewToPixel({0.0f, 1.0f, 5.0f}, proj, unjittered);
    assert(up[1] < centre[1]);
    const auto shifted = ViewToPixel({0.0f, 0.0f, 5.0f}, proj, screen);
    assert(Near(shifted[0] - centre[0], 0.25f) && Near(shifted[1] - centre[1], -0.125f));
    const float near_depth = ViewToDepth(-proj[3] / proj[2], proj);
    assert(Near(near_depth, 0.0f, 1e-5f));
    assert(Near(ViewToDepth(3000.0f, proj), 1.0f, 1e-4f) && ViewToDepth(10.0f, proj) < ViewToDepth(20.0f, proj));
    assert(HitVisible(12.0f, 12.1f, 0.03f, 0.05f));
    assert(!HitVisible(8.0f, 12.0f, 0.03f, 0.05f));
}

Affine Rotation(float yaw, std::array<float, 3> t) {
    const float c = std::cos(yaw), s = std::sin(yaw);
    return {c, 0, s, t[0], 0, 1, 0, t[1], -s, 0, c, t[2]};
}

void TestTemporalReprojection() {
    const Affine prev_inv_view = Rotation(0.3f, {1.0f, 2.0f, 3.0f});
    const Affine cur_inv_view = Rotation(0.45f, {1.5f, 2.0f, 2.0f});
    const auto identity = MultiplyAffine(InverseRigid(cur_inv_view), cur_inv_view);
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            assert(Near(identity[r * 4 + c], r == c ? 1.0f : 0.0f, 1e-5f));
        }
    }
    const auto reproject = Reprojection(prev_inv_view, cur_inv_view);
    const std::array<float, 3> p_cur{0.5f, -0.25f, 7.0f};
    const auto world = Transform(cur_inv_view, p_cur);
    const auto expected_prev = Transform(InverseRigid(prev_inv_view), world);
    const auto got_prev = Transform(reproject, p_cur);
    for (int i = 0; i < 3; ++i) {
        assert(Near(got_prev[i], expected_prev[i], 1e-4f));
    }
    const auto same = Reprojection(cur_inv_view, cur_inv_view);
    const auto still = Transform(same, p_cur);
    for (int i = 0; i < 3; ++i) {
        assert(Near(still[i], p_cur[i], 1e-5f));
    }
    const auto rows = Rows(reproject);
    assert(Near(rows[0][3], reproject[3]) && Near(rows[2][2], reproject[10]));
}

void TestHistoryRules() {
    assert(Near(BlendWeight(0.0f, 16.0f), 1.0f));
    assert(Near(BlendWeight(1.0f, 16.0f), 0.5f));
    assert(Near(BlendWeight(15.0f, 16.0f), 1.0f / 16.0f));
    assert(Near(BlendWeight(100.0f, 16.0f), 1.0f / 17.0f));
    assert(HistoryAccept(10.0f, 10.3f, 0.05f, 0.02f));
    assert(!HistoryAccept(10.0f, 11.0f, 0.05f, 0.02f));
    assert(!HistoryAccept(10.0f, 0.0f, 0.05f, 0.02f));
    assert(HistoryAccept(0.1f, 0.115f, 0.05f, 0.02f));

    const Affine a = Rotation(0.0f, {0, 0, 0});
    assert(!CameraCut(a, Rotation(0.1f, {0.5f, 0.0f, 0.2f}), 3.0f, 0.9f));
    assert(CameraCut(a, Rotation(0.0f, {10.0f, 0.0f, 0.0f}), 3.0f, 0.9f));
    assert(CameraCut(a, Rotation(1.2f, {0.0f, 0.0f, 0.0f}), 3.0f, 0.9f));
    assert(!CameraCut(a, a, 3.0f, 0.9f));
}

void TestNormalSourceParsing() {
    NormalSource n;
    assert(ParseNormalSource("", n) && n.target == 0xFF && n.mode == NormalFromDepth);
    assert(ParseNormalSource("depth", n) && n.target == 0xFF);
    assert(ParseNormalSource("1.rgb", n) && n.target == 1 && n.mode == NormalUnorm &&
           n.components == 3 && n.swizzle == (0u | 1u << 2 | 2u << 4));
    assert(ParseNormalSource("2.bgr.s", n) && n.target == 2 && n.mode == NormalSigned &&
           n.swizzle == (2u | 1u << 2 | 0u << 4));
    assert(ParseNormalSource("3.rg", n) && n.components == 2 && n.swizzle == (0u | 1u << 2));
    assert(ParseNormalSource("5.xyz.uw", n) && n.mode == (NormalUnorm | NormalWorldSpace));
    assert(ParseNormalSource("4.ab.sw", n) && n.mode == (NormalSigned | NormalWorldSpace) &&
           n.swizzle == (3u | 2u << 2));
    assert(ParseNormalSource("0.rgb.uw", n) && n.target == GameNormalSource.target &&
           n.swizzle == GameNormalSource.swizzle && n.mode == GameNormalSource.mode &&
           n.components == GameNormalSource.components);
    assert(!ParseNormalSource("8.rgb", n));
    assert(!ParseNormalSource("1", n));
    assert(!ParseNormalSource("1.rgbq", n));
    assert(!ParseNormalSource("1.r", n));
    assert(!ParseNormalSource("x.rgb", n));
    assert(!ParseNormalSource("1.rgb.k", n));
    assert(n.target == 0xFF);
    assert(sizeof(GiParams) == 304);
}

void TestGBufferLayoutKeepsLargest() {
    assert(GBufferLayoutReplaces(0, 5));
    assert(GBufferLayoutReplaces(5, 6));
    assert(!GBufferLayoutReplaces(6, 5));
    assert(!GBufferLayoutReplaces(6, 6));
    assert(!GBufferLayoutReplaces(5, 0));
}

void TestMaterialCache() {
    assert(MaterialEncode(-1.0f) == 0u);
    assert(MaterialEncode(1.0f) == MaterialScale);
    assert(MaterialEncode(100.0f) == MaterialEncode(MaterialMaxRadiance));
    assert(MaterialEncode(std::nanf("")) == 0u);
    assert(MaterialSumSafe(3840 * 2160));
    assert(!MaterialSumSafe(0xFFFFFFFFu));
    int sampled = 0;
    for (uint32_t y = 0; y < 64; ++y) {
        for (uint32_t x = 0; x < 64; ++x) {
            sampled += MaterialSampled(x, y, 7) ? 1 : 0;
        }
    }
    assert(sampled == 64 * 64 / int(MaterialSampleMask + 1));
    std::array<float, 4> stored{0.0f, 0.0f, 0.0f, 0.0f};
    assert(!MaterialValid(stored));
    assert(MaterialResolve(stored, {5, 5, 5, 0}) == stored);
    const std::array<uint32_t, 4> sum{MaterialEncode(0.5f) * 4, MaterialEncode(0.25f) * 4,
                                      MaterialEncode(2.0f) * 4, 4};
    stored = MaterialResolve(stored, sum);
    assert(MaterialValid(stored));
    assert(Near(stored[0], 0.5f, 1e-3f) && Near(stored[1], 0.25f, 1e-3f) &&
           Near(stored[2], 2.0f, 1e-3f));
    assert(stored[3] == 1.0f);
    const std::array<uint32_t, 4> brighter{MaterialEncode(1.5f) * 2, MaterialEncode(1.25f) * 2,
                                           MaterialEncode(3.0f) * 2, 2};
    for (int i = 0; i < 400; ++i) {
        stored = MaterialResolve(stored, brighter);
    }
    assert(Near(stored[0], 1.5f, 1e-3f) && Near(stored[1], 1.25f, 1e-3f) &&
           Near(stored[2], 3.0f, 1e-3f));
    assert(stored[3] == MaterialMaxFrames);
    const auto one = MaterialResolve({1.0f, 1.0f, 1.0f, 1.0f}, {0, 0, 0, 1});
    assert(Near(one[0], 1.0f - MaterialBlend, 1e-5f));
    assert(GiDebugSource == 8u);
}

void TestEmissiveSky() {
    static_assert(EmissiveSlot(8192, 0) == SkySlot(8192) + 1);
    static_assert(EmissiveSlot(8192, 8191) + 1 == SkyProbeSlot(8192, 0));
    static_assert(SkyGainSlot(8192) == SkyProbeSlot(8192, SkyProbeCells));
    // records, sky, emissive per record, probe cells, gain, dome map, then the sky image (sky in
    // mirrors, 6378f81). The buffer is sized from MaterialSlots, 16 bytes a slot.
    static_assert(SkyDomeMapSlot(8192) == SkyGainSlot(8192) + 1);
    static_assert(SkyImageSlot(8192, 0) == SkyDomeMapSlot(8192) + 1);
    // Then the moon direction, the moon and cloud layer grid and its search stats.
    static_assert(MoonDirSlot(8192) == SkyImageSlot(8192, SkyImageCells));
    static_assert(SkyLayerSlot(8192, 0) == MoonDirSlot(8192) + 1);
    static_assert(MaterialSlots(8192) ==
                  2 * 8192 + 8 + SkyProbeCells + SkyImageCells + SkyLayerCells);
    static_assert(uint64_t(MaterialSlots(8192)) * 16u <= 64ull << 20);
    static_assert(BounceFlagEmissive != BounceFlagDirSets && BounceFlagEmissive > BounceFlagDirSets);
    const auto center = OctEncode({0.0f, 0.0f, 1.0f});
    assert(Near(center[0], 0.5f) && Near(center[1], 0.5f));
    const auto px = OctEncode({1.0f, 0.0f, 0.0f});
    assert(Near(px[0], 1.0f) && Near(px[1], 0.5f));
    const auto back = OctEncode({0.0f, 0.0f, -1.0f});
    assert((Near(back[0], 0.0f) || Near(back[0], 1.0f)) && (Near(back[1], 0.0f) || Near(back[1], 1.0f)));
    assert(SkyProbeCell({0.0f, 1.0f, 0.0f}) != SkyProbeCell({0.0f, -1.0f, 0.0f}));
    assert(SkyProbeCell({0.0f, 1.0f, 0.0f}) != SkyProbeCell({1.0f, 0.0f, 0.0f}));
    assert(SkyProbeCell({0.3f, 0.9f, 0.1f}) == SkyProbeCell({0.3001f, 0.9f, 0.1f}));
    for (float a = 0.0f; a < 6.28f; a += 0.37f) {
        for (float b = -1.5f; b < 1.5f; b += 0.29f) {
            const std::array<float, 3> d{std::cos(a) * std::cos(b), std::sin(b), std::sin(a) * std::cos(b)};
            assert(SkyProbeCell(d) < SkyProbeCells);
        }
    }
    const Affine yaw90{0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f};
    const auto w = ViewToWorldDir(yaw90, {0.0f, 0.0f, 1.0f});
    assert(Near(w[0], 1.0f) && Near(w[1], 0.0f) && Near(w[2], 0.0f));
    const auto v = std::array<float, 3>{yaw90[0] * w[0] + yaw90[1] * w[1] + yaw90[2] * w[2],
                                        yaw90[4] * w[0] + yaw90[5] * w[1] + yaw90[6] * w[2],
                                        yaw90[8] * w[0] + yaw90[9] * w[1] + yaw90[10] * w[2]};
    assert(Near(v[0], 0.0f) && Near(v[1], 0.0f) && Near(v[2], 1.0f));
    const Affine moved{1.0f, 0.0f, 0.0f, 5.0f, 0.0f, 1.0f, 0.0f, -3.0f, 0.0f, 0.0f, 1.0f, 2.0f};
    const auto still = ViewToWorldDir(moved, {0.0f, 0.0f, 1.0f});
    assert(Near(still[2], 1.0f) && Near(still[0], 0.0f));
}

void TestBouncePath() {
    static_assert(sizeof(GiParams) == 304);
    static_assert(offsetof(GiParams, far_fade) == 288);
    assert(PathSegments(0) == 1 && PathSegments(2) == 2 && PathSegments(100) == MaxBounces);
    assert(SkySlot(8192) == 8192);
    PathState s{};
    PathHit(s, {0.5f, 0.5f, 0.5f}, {1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 0.0f});
    assert(Near(s.sum[0], 0.5f) && Near(s.throughput[0], 0.5f) && s.alive);
    PathHit(s, {0.5f, 0.5f, 0.5f}, {1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 0.0f});
    assert(Near(s.sum[0], 0.75f) && Near(s.throughput[0], 0.25f));
    PathMiss(s, {2.0f, 2.0f, 2.0f});
    assert(Near(s.sum[0], 1.25f) && !s.alive);
    PathState e{};
    PathHit(e, {0.0f, 0.0f, 0.0f}, {5.0f, 5.0f, 5.0f}, {3.0f, 1.0f, 0.0f});
    assert(Near(e.sum[0], 3.0f) && Near(e.sum[1], 1.0f) && Near(e.sum[2], 0.0f) &&
           Near(e.throughput[0], 0.0f));
    PathState r{};
    r.throughput = {0.2f, 0.1f, 0.4f};
    assert(Near(RouletteProbability(r.throughput), 0.4f));
    assert(Near(RouletteProbability({0.001f, 0.0f, 0.0f}), RouletteMin));
    assert(Near(RouletteProbability({3.0f, 0.0f, 0.0f}), 1.0f));
    PathState dead = r;
    PathRoulette(dead, 0.4f);
    assert(!dead.alive);
    PathRoulette(r, 0.39f);
    assert(r.alive && Near(r.throughput[2], 1.0f) && Near(r.throughput[0], 0.5f));
    assert(!RouletteApplies(0) && RouletteApplies(1));
    uint32_t state = 7u;
    double expect = 0.0;
    const int trials = 20000;
    for (int i = 0; i < trials; ++i) {
        PathState t{};
        t.throughput = {0.3f, 0.3f, 0.3f};
        PathRoulette(t, Random(state));
        expect += t.alive ? t.throughput[0] : 0.0;
    }
    assert(std::fabs(expect / trials - 0.3) < 0.02);
    assert(Near(LambertLight(0.5f, 4.0f, 0.0f, 1.0f, 1.0f), 0.125f));
    assert(Near(LambertLight(-0.5f, 4.0f, 0.0f, 1.0f, 1.0f), 0.0f));
    assert(Near(LambertLight(1.0f, 4.0f, 0.25f, 1.0f, 1.0f), 0.0f));
    assert(Near(LambertLight(1.0f, 1.0f, 0.5f, 0.5f, 0.0f), 0.0f));
    assert(Near(LambertLight(1.0f, 1.0f, 0.5f, 0.5f, 1.0f), 0.25f));
    assert(CacheEmissive({4.0f, 4.0f, 4.0f, 1.0f}, 2.0f));
    assert(!CacheEmissive({4.0f, 4.0f, 4.0f, 0.0f}, 2.0f));
    assert(!CacheEmissive({1.0f, 1.0f, 1.0f, 1.0f}, 2.0f));
    assert(!CacheEmissive({4.0f, 4.0f, 4.0f, 1.0f}, 0.0f));
    assert(Near(Roughness(0.75f, true), 0.25f) && Near(Roughness(0.75f, false), 1.0f));
    assert(MissHitDistance > 1000.0f);
}

void TestReflect() {
    assert(ReflectTraced(0.3f, ReflectMaxRoughDefault) && ReflectTraced(0.6f, 0.6f));
    assert(!ReflectTraced(0.7f, ReflectMaxRoughDefault));
    assert(!ReflectTraced(Roughness(0.5f, false), ReflectMaxRoughDefault));
    assert(ReflectTraced(Roughness(0.9f, true), ReflectMaxRoughDefault));
    assert(Near(GgxAlpha(0.5f), 0.25f) && Near(GgxAlpha(0.0f), ReflectMinAlpha));
    const auto mirror = GgxSampleHalf(0.5f, 0.3f, ReflectMinAlpha);
    assert(mirror.cos_theta > 0.999f);
    uint32_t state = 99u;
    const float alpha = 0.3f;
    double pdf_sum = 0.0, cos_sum = 0.0;
    const int count = 100000;
    for (int i = 0; i < count; ++i) {
        const auto h = GgxSampleHalf(Random(state), Random(state), alpha);
        const auto& l = h.local;
        assert(Near(l[0] * l[0] + l[1] * l[1] + l[2] * l[2], 1.0f, 1e-3f));
        assert(h.cos_theta >= 0.0f && Near(h.cos_theta, l[2]));
        cos_sum += h.cos_theta;
        const float d = GgxD(alpha, h.cos_theta);
        pdf_sum += 1.0 / std::max(d * h.cos_theta, 1e-6f);
    }
    assert(cos_sum / count > 0.9);
    assert(std::fabs(pdf_sum / count - 2.0 * Pi) < 0.05 * 2.0 * Pi);
    uint32_t s2 = 5u;
    double integral = 0.0;
    const int n = 200000;
    for (int i = 0; i < n; ++i) {
        const float u = Random(s2);
        integral += GgxD(alpha, u) * u * 2.0 * Pi;
    }
    assert(std::fabs(integral / n - 1.0) < 0.03);
    assert(Near(GgxPdf(alpha, 1.0f, 1.0f), GgxD(alpha, 1.0f) / 4.0f));
    assert(Near(SchlickFresnel(0.04f, 1.0f), 0.04f));
    assert(Near(SchlickFresnel(0.04f, 0.0f), 1.0f));
    assert(SchlickFresnel(0.04f, 0.5f) > 0.04f && SchlickFresnel(0.04f, 0.5f) < 0.1f);
    assert(GgxG1(ReflectMinAlpha, 1.0f) > 0.999f);
    assert(GgxG1(1.0f, 0.1f) < GgxG1(1.0f, 0.9f));
    assert(Near(GgxWeight(ReflectMinAlpha, 1.0f, 1.0f, 1.0f, 1.0f), 1.0f, 1e-3f));
    assert(Near(GgxWeight(0.5f, -0.1f, 1.0f, 1.0f, 1.0f), 0.0f));
    assert(GgxWeight(0.5f, 0.05f, 0.05f, 0.9f, 0.9f) <= ReflectMaxWeight);
    assert(Near(AoFromIrradiance(0.5f, 1.0f), 0.5f) && Near(AoFromIrradiance(2.0f, 1.0f), 1.0f));
    assert(Near(AoFromIrradiance(0.0f, 0.0f), 1.0f));
    assert(ReflectFlagPreAo != ReflectFlagF0);
    assert((GiFlagNoScreen & (GiFlagReset | GiFlagNormalDebug)) == 0);
}

void TestHitSpecular() {
    const HitMaterial off = HitMaterialFrom(false, 0.9f, true, 0.9f, true);
    assert(!off.from_screen && Near(off.f0, HitDefaultF0) && Near(off.roughness, HitDefaultRoughness));
    const HitMaterial on = HitMaterialFrom(true, 0.5f, true, 0.75f, true);
    assert(on.from_screen && Near(on.f0, 0.25f) && Near(on.roughness, 0.25f));
    const HitMaterial partial = HitMaterialFrom(true, 0.5f, false, 0.75f, false);
    assert(Near(partial.f0, HitDefaultF0) && Near(partial.roughness, HitDefaultRoughness));
    assert(Near(HitSpecAlpha(0.5f), 0.25f) && Near(HitSpecAlpha(0.0f), HitMinAlpha));
    assert(Near(HitSpecular(0.5f, 1.0f, -0.1f, 1.0f, 1.0f, 1.0f), 0.0f));
    assert(Near(HitSpecular(0.5f, 1.0f, 1.0f, -0.1f, 1.0f, 1.0f), 0.0f));
    assert(HitSpecular(HitMinAlpha, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f) <= HitSpecMax);
    double previous = 2.0;
    for (const float alpha : {0.25f, 0.5f, 1.0f}) {
        uint32_t state = 17u;
        double furnace = 0.0;
        const int n = 400000;
        for (int i = 0; i < n; ++i) {
            const float cl = Random(state);
            const float nh = std::sqrt((1.0f + cl) * 0.5f);
            furnace += 2.0 * HitSpecular(alpha, 1.0f, cl, 1.0f, nh, nh) * cl;
        }
        furnace /= n;
        assert(furnace > 0.2 && furnace < 1.02 && furnace < previous);
        assert(alpha > 0.3f || furnace > 0.85);
        previous = furnace;
    }
    const float grazing = HitSpecular(0.5f, HitDefaultF0, 0.7f, 0.7f, 0.99f, 0.2f);
    const float normal = HitSpecular(0.5f, HitDefaultF0, 0.7f, 0.7f, 0.99f, 1.0f);
    assert(grazing > normal && normal > 0.0f);
}

}

void TestSparseTrace() {
    for (uint32_t sparse : {1u, 2u, 3u, 4u}) {
        const GiSparseBlock b = GiSparseBlockSize(sparse);
        const uint32_t count = b.width * b.height;
        assert(count == (sparse >= 4 ? 4u : sparse >= 2 ? 2u : 1u));
        for (uint32_t by = 0; by < 17; ++by) {
            for (uint32_t bx = 0; bx < 23; ++bx) {
                for (uint32_t start = 0; start < 9; start += 4) {
                    uint32_t seen = 0;
                    for (uint32_t f = 0; f < count; ++f) {
                        const uint32_t index = GiSparseTracedIndex(bx, by, sparse, start + f);
                        assert(index < count);
                        seen |= 1u << index;
                    }
                    assert(seen == (1u << count) - 1u);
                }
            }
        }
        const auto groups = GiInitGroups(2296, 960, sparse);
        assert(groups[0] * 8 * b.width >= 2296 && groups[1] * 8 * b.height >= 960);
        assert((groups[0] - 1) * 8 * b.width < 2296 && (groups[1] - 1) * 8 * b.height < 960);
    }
    assert(GiTraceCfg(4, false) == 4u && GiTraceCfg(2, true) == (2u | GiTraceLastBounceCache));
    assert(GiTraceCfg(0, false) == 1u && GiTraceCfg(9, false) == 4u);
    assert(((GiTraceCfg(4, true, 20) >> GiTmaxShift) & 0xFFFu) == 20u);
    assert((GiTraceCfg(4, true, 20) & 0xFu) == (4u | GiTraceLastBounceCache));
    assert(((GiTraceCfg(1, false, 99999) >> GiTmaxShift) & 0xFFFu) == GiTmaxMax);
    assert(GiTraceCfg(4, true, GiTmaxMax) < (1u << 24));
    assert(GiSparseHash(12345u) == Hash(12345u));
}

void TestGiPipelineMode() {
    constexpr uint32_t nv = 0x10DEu;
    const auto def = GiPipelineFromEnv(nullptr, nullptr, true, true, nv);
    assert(def.pipeline && def.ser);
    const auto empty = GiPipelineFromEnv("", nullptr, true, true, nv);
    assert(empty.pipeline && empty.ser);
    const auto def_no_rt = GiPipelineFromEnv(nullptr, nullptr, false, true, nv);
    assert(!def_no_rt.pipeline && !def_no_rt.ser);
    const auto amd = GiPipelineFromEnv(nullptr, nullptr, true, false, GiAmdVendor);
    assert(!amd.pipeline && !amd.ser);
    const auto amd_forced = GiPipelineFromEnv("1", nullptr, true, false, GiAmdVendor);
    assert(amd_forced.pipeline && !amd_forced.ser);
    const auto zero = GiPipelineFromEnv("0", "1", true, true, nv);
    assert(!zero.pipeline && !zero.ser);
    const auto on = GiPipelineFromEnv("1", nullptr, true, true, nv);
    assert(on.pipeline && on.ser);
    const auto no_ser_env = GiPipelineFromEnv("1", "0", true, true, nv);
    assert(no_ser_env.pipeline && !no_ser_env.ser);
    const auto def_no_ser_env = GiPipelineFromEnv(nullptr, "0", true, true, nv);
    assert(def_no_ser_env.pipeline && !def_no_ser_env.ser);
    const auto no_ser_ext = GiPipelineFromEnv("1", "1", true, false, nv);
    assert(no_ser_ext.pipeline && !no_ser_ext.ser);
    const auto no_rt = GiPipelineFromEnv("1", "1", false, true, nv);
    assert(!no_rt.pipeline && !no_rt.ser);
}

void TestGiSbt() {
    const GiSbtLayout ada = GiSbt(32, 32, 64);
    assert(ada.stride == 32 && ada.raygen == 0 && ada.miss == 64 && ada.hit == 128);
    assert(ada.size == 192);
    const GiSbtLayout odd = GiSbt(20, 16, 48);
    assert(odd.stride == 32 && odd.miss == 48 && odd.hit == 96 && odd.size == 144);
    const auto launch = GiInitLaunch(2296, 960, 4);
    const auto groups = GiInitGroups(2296, 960, 4);
    assert(launch[0] == groups[0] * 8 && launch[1] == groups[1] * 8);
}

void TestGiPipelineFlags() {
    assert(GiPipelineOmmBit == 0x01000000u);
    assert(GiPipelineFlags(true) == GiPipelineOmmBit);
    assert(GiPipelineFlags(false) == 0u);
}

void TestSkyCache() {
    using namespace Vulkan::Rt;
    // RGB9E5 keeps 9 bits of the brightest channel.
    for (const float v : {0.0003f, 0.02f, 0.5f, 1.0f, 3.7f, 250.0f}) {
        const auto c = UnpackRgb9e5(PackRgb9e5(v, v * 0.5f, v * 0.25f));
        assert(std::fabs(c[0] - v) <= v / 256.0f);
        assert(std::fabs(c[1] - v * 0.5f) <= v / 256.0f);
        assert(std::fabs(c[2] - v * 0.25f) <= v / 256.0f);
    }
    assert(PackRgb9e5(0.0f, 0.0f, 0.0f) == 0u);
    assert(PackRgb9e5(-1.0f, NAN, 0.0f) == 0u);
    // Unseen cells stay 0, a seen black cell does not; runs survive the round trip.
    const float cells[6 * 4] = {0, 0, 0, 0,   0.5f, 0.25f, 0.1f, 1,  0, 0, 0, 1,
                                9, 9, 9, 0,   0, 0, 0, 0,          2.0f, 1.0f, 0.5f, 1};
    size_t seen = 0;
    const std::vector<uint32_t> words = PackSkyCells(cells, 6, &seen);
    assert(seen == 3);
    assert(words[0] == 0u && words[1] != 0u && words[2] != 0u && words[3] == 0u &&
           words[4] == 0u && words[5] != 0u);
    const std::vector<uint32_t> enc = EncodeSkyWords(words);
    std::vector<uint32_t> back;
    assert(DecodeSkyWords(enc, 6, back) && back == words);
    assert(!DecodeSkyWords(enc, 7, back));
    std::vector<uint32_t> cut(enc.begin(), enc.end() - 1);
    assert(!DecodeSkyWords(cut, 6, back));
    float out[6 * 4];
    assert(UnpackSkyCells(words, out) == 3);
    assert(out[3] == 0.0f && out[7] == 1.0f && std::fabs(out[4] - 0.5f) < 0.004f);
    assert(out[11] == 1.0f && out[8] < 1e-6f);
    assert(out[15] == 0.0f && std::fabs(out[20] - 2.0f) < 0.01f);
}

void TestSkyKeyAndMoonCache() {
    // Clouds draw every frame, the moon only while in view: the key is the clouds alone.
    SkyKey key;
    assert(key.Add(10, 576, 147));
    key.Add(10, 216, 63);
    key.Add(10, 96, 24); // moon in view
    assert(key.Key() == 0);
    assert(!key.Add(11, 576, 147));
    key.Add(11, 216, 63); // moon culled
    const uint64_t with_moon = key.Key();
    assert(with_moon != 0);
    key.Add(12, 576, 147);
    const uint64_t clouds = key.Key();
    assert(clouds != 0 && clouds != with_moon);
    key.Add(12, 216, 63);
    key.Add(12, 96, 24); // the moon back does not change the key
    key.Add(13, 576, 147);
    key.Add(13, 216, 63);
    key.Add(14, 576, 147);
    assert(key.Key() == clouds);
    // The same sky after a load (layer gone for longer than RestartFrames) gets the same key.
    const uint64_t gen = key.Generation();
    assert(key.Add(14 + SkyKey::RestartFrames + 1, 216, 63));
    assert(key.Generation() == gen + 1);
    key.Add(14 + SkyKey::RestartFrames + 1, 576, 147);
    key.Add(14 + SkyKey::RestartFrames + 2, 576, 147);
    assert(key.Key() == clouds);
    assert(key.Recent(14 + SkyKey::RestartFrames + 4, 2));
    assert(!key.Recent(14 + SkyKey::RestartFrames + 5, 2));

    MoonCache cache;
    assert(!cache.Put(0, {0.0f, 1.0f, 0.0f}));
    assert(!cache.Put(clouds, {0.0f, NAN, 0.0f}));
    assert(cache.Put(clouds, {0.0f, 700.0f, 0.0f}));
    assert(Near((*cache.Find(clouds))[1], 700.0f));
    assert(!cache.Put(clouds, {3.0f, 700.0f, 0.0f})); // the mean moved 1.5 m: not saved
    assert(Near((*cache.Find(clouds))[0], 1.5f));
    // Finds hopping between two grid cells 25 m apart settle on the middle.
    for (int i = 0; i < 200; ++i) {
        cache.Put(clouds, {i % 2 ? 12.5f : -12.5f, 700.0f, 0.0f});
    }
    assert(std::fabs((*cache.Find(clouds))[0]) < 1.0f);
    // From one spot every find lands 10 m off the same way: the mean of 202 finds barely moves.
    for (int i = 0; i < 50; ++i) {
        cache.Put(clouds, {10.0f, 700.0f, 0.0f});
    }
    assert(std::fabs((*cache.Find(clouds))[0]) < 2.5f);
    // Strays (52 m off from one spot, one at the horizon) never outvote the moon.
    for (int i = 0; i < 200; ++i) {
        assert(!cache.Put(clouds, {52.0f, 700.0f, 0.0f}));
    }
    assert(!cache.Put(clouds, {1650.0f, 70.0f, -600.0f}));
    assert(std::fabs((*cache.Find(clouds))[0]) < 2.5f);
    assert(Near(cache.Votes(clouds), 252.0f));
    // A moon far from the cached one (a wrong spot voted in, or the game's moon of another area)
    // takes over after SwitchVotes finds, not after outnumbering the 252 old ones.
    int puts = 0;
    bool moved = false;
    for (; puts < 300 && !moved; ++puts) {
        moved = cache.Put(clouds, {600.0f, 800.0f, 0.0f});
    }
    assert(moved && Near((*cache.Find(clouds))[0], 600.0f));
    assert(puts == int(MoonCache::SwitchVotes));
    // The light points from the camera at the moon, so its direction moves with the player.
    std::array<float, 3> dir{};
    assert(cache.DirFrom(clouds, {0.0f, 0.0f, 0.0f}, dir));
    assert(Near(dir[0], 0.6f) && Near(dir[1], 0.8f) && Near(dir[2], 0.0f));
    assert(cache.DirFrom(clouds, {600.0f, 0.0f, 0.0f}, dir));
    assert(Near(dir[0], 0.0f) && Near(dir[1], 1.0f));
    assert(!cache.DirFrom(clouds, {600.0f, 800.0f, 0.0f}, dir)); // at the moon
    assert(!cache.DirFrom(with_moon, {0.0f, 0.0f, 0.0f}, dir));
    MoonCache back;
    back.Parse(cache.Text() + "junk line\n");
    assert(back.Size() == 1);
    assert(Near((*back.Find(clouds))[0], 600.0f) && Near((*back.Find(clouds))[1], 800.0f));
    assert(Near(back.Votes(clouds), MoonCache::SwitchVotes)); // votes saved with the position
    MoonCache old;
    old.Parse("00000000000000aa 1 2 3\n"); // a line without votes counts LoadedSamples
    assert(Near(old.Votes(0xaa), MoonCache::LoadedSamples));
    assert(!back.Find(with_moon));

    // Per draw: a run that saw other segments of the same sky still finds its moon, and a
    // segment shared with another sky is outvoted.
    SkyKey run1;
    run1.Add(100, 576, 147);
    run1.Add(100, 216, 63);
    run1.Add(101, 300, 80);
    assert(run1.Draws().size() == 3);
    MoonCache draws;
    assert(draws.PutDraws(run1.Draws(), {0.0f, 700.0f, 0.0f}));
    for (int i = 0; i < 50; ++i) {
        draws.Put(uint64_t(576) << 32 | 147, {0.0f, 700.0f, 0.0f}); // same sky, more finds
    }
    SkyKey run2; // a later boot facing elsewhere: only one segment in common
    run2.Add(5, 300, 80);
    run2.Add(5, 999, 11);
    std::array<float, 3> pos{};
    assert(draws.FindDraws(run2.Draws(), pos) && Near(pos[1], 700.0f));
    // Another sky shares segment 576/147 at a different moon; its own segments outvote it.
    MoonCache two;
    two.PutDraws({uint64_t(576) << 32 | 147, uint64_t(216) << 32 | 63}, {0.0f, 700.0f, 0.0f});
    two.PutDraws({uint64_t(400) << 32 | 1, uint64_t(401) << 32 | 2}, {700.0f, 0.0f, 0.0f});
    two.Put(uint64_t(576) << 32 | 147, {700.0f, 0.0f, 0.0f}, 2000.0f); // shared, now wrong sky
    assert(two.FindDraws({uint64_t(400) << 32 | 1, uint64_t(401) << 32 | 2,
                          uint64_t(576) << 32 | 147},
                         pos) &&
           Near(pos[0], 700.0f));
    assert(two.FindDraws({uint64_t(216) << 32 | 63, uint64_t(576) << 32 | 147, uint64_t(7)}, pos));
    assert(!two.FindDraws({uint64_t(7), uint64_t(8)}, pos));
}

int main() {
    TestSkyKeyAndMoonCache();
    TestSkyCache();
    TestGiPipelineFlags();
    TestGiPipelineMode();
    TestGiSbt();
    TestSparseTrace();
    TestCosineSampling();
    TestScreenReprojection();
    TestTemporalReprojection();
    TestHistoryRules();
    TestNormalSourceParsing();
    TestGBufferLayoutKeepsLargest();
    TestMaterialCache();
    TestBouncePath();
    TestEmissiveSky();
    TestReflect();
    TestHitSpecular();
    std::puts("rt-gi-test: ok");
    return 0;
}

#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>
#include "gpu/shadps4/video_core/renderer_vulkan/rt/rt_direct.h"
#include "gpu/shadps4/video_core/renderer_vulkan/rt/rt_geometry.h"
#include "gpu/shadps4/video_core/renderer_vulkan/rt/rt_gi.h"
#include "gpu/shadps4/video_core/renderer_vulkan/scene_resolution.h"

using namespace Vulkan::Rt;

namespace {

bool Near(float a, float b, float eps = 1e-3f) {
    return std::fabs(a - b) <= eps * std::max(1.0f, std::fabs(b));
}

Light MakeLight(std::array<float, 3> world, float radius, std::array<float, 3> color,
                LightKind kind = LightKind::Point) {
    Light l{};
    l.kind = kind;
    l.has_world = true;
    l.world = world;
    l.position = world;
    l.radius = radius;
    l.color = color;
    l.spec = {1.0f, 1.0f, 1.0f};
    l.spec_power = 20.0f;
    return l;
}

void TestPointBrdfHand() {
    DirectLightGpu l{};
    l.pos_inv_r2 = {0.0f, 1.0f, -1.0f, 1.0f / 16.0f};
    l.color_sp = {2.0f, 1.0f, 0.5f, 20.0f};
    l.spec_kind = {1.0f, 0.5f, 0.25f, 0.0f};
    const std::array<float, 3> p{0.0f, 0.0f, -2.0f};
    const std::array<float, 3> n{0.0f, 0.0f, 1.0f};
    const PointTerms t = PointLightTerms(p, n, l, {0.5f, 0.5f, 0.5f}, 0.0f, 0.0f, 1.0f);
    const float nl = 0.70711f, atten = 0.5f - 1.0f / 16.0f, g2 = 0.25f;
    const float diffuse_scale = nl * atten * (1.0f - g2 * 0.5f);
    assert(Near(t.diffuse[0], 2.0f * diffuse_scale));
    assert(Near(t.diffuse[1], 1.0f * diffuse_scale));
    assert(Near(t.diffuse[2], 0.5f * diffuse_scale));
    assert(Near(diffuse_scale, 0.27069f));
    const float spec_scale = 0.29283f * g2;
    assert(Near(t.specular[0], spec_scale, 2e-3f));
    assert(Near(t.specular[1], 0.5f * spec_scale, 2e-3f));
    assert(Near(t.specular[2], 0.25f * spec_scale, 2e-3f));
    const PointTerms half = PointLightTerms(p, n, l, {0.5f, 0.5f, 0.5f}, 0.0f, 0.0f, 0.5f);
    assert(Near(half.diffuse[0], t.diffuse[0] * 0.5f));
    const PointTerms far = PointLightTerms({0.0f, 0.0f, -6.0f}, n, l, {0.0f, 0.0f, 0.0f}, 0.0f, 0.0f, 1.0f);
    assert(far.diffuse[0] == 0.0f && far.specular[0] == 0.0f);
    assert(Near(SpecPower(20.0f, 0.0f), 1.0f));
    assert(Near(SpecPower(20.0f, 1.0f), 20.0f * 8192.0f / (81.92f + 20.0f)));
    assert(SpecPower(0.5f, 0.0f) == 1.0f);
}

void TestWrapBranch() {
    DirectLightGpu l{};
    l.pos_inv_r2 = {0.0f, 0.0f, 0.0f, 1.0f / 100.0f};
    l.color_sp = {1.0f, 1.0f, 1.0f, 20.0f};
    l.wrap_r_flag = {1.0f, 1.0f, 10.0f, 0.0f};
    const std::array<float, 3> p{0.0f, 0.0f, -2.0f};
    const std::array<float, 3> n{0.0f, 1.0f, 0.0f};
    const PointTerms plain = PointLightTerms(p, n, l, {0.0f, 0.0f, 0.0f}, 0.0f, 0.0f, 1.0f);
    assert(plain.diffuse[0] == 0.0f);
    // Wrap materials (g1.y > 0) get no wrap or back-light rim: grazing and back lights add nothing.
    const PointTerms wrap = PointLightTerms(p, n, l, {0.0f, 0.0f, 0.0f}, 0.0f, 0.1f, 1.0f);
    assert(wrap.diffuse[0] == 0.0f);
    DirectLightGpu behind = l;
    behind.pos_inv_r2 = {0.0f, 0.0f, -4.0f, 1.0f / 100.0f};
    const PointTerms back = PointLightTerms(p, n, behind, {0.0f, 0.0f, 0.0f}, 0.0f, 0.1f, 1.0f);
    assert(back.diffuse[0] == 0.0f);
}

void TestTileCull() {
    const std::array<float, 4> proj{1.0f, 1.0f, 0.0f, 1.0f};
    const float w = 160.0f, h = 160.0f, zsign = -1.0f;
    assert(SphereInTile({0.0f, 0.0f, -5.0f}, 1.0f, 5, 5, proj, w, h, zsign, 4.0f, 6.0f));
    assert(SphereInTile({0.0f, 0.0f, -5.0f}, 1.0f, 4, 4, proj, w, h, zsign, 4.0f, 6.0f));
    assert(!SphereInTile({-3.0f, 0.0f, -5.0f}, 0.5f, 5, 5, proj, w, h, zsign, 4.0f, 6.0f));
    assert(SphereInTile({-3.0f, 0.0f, -5.0f}, 0.5f, 8, 5, proj, w, h, zsign, 4.0f, 6.0f));
    assert(!SphereInTile({0.0f, 0.0f, -8.0f}, 1.0f, 5, 5, proj, w, h, zsign, 4.0f, 6.0f));
    assert(SphereInTile({0.0f, 0.0f, -8.0f}, 3.0f, 5, 5, proj, w, h, zsign, 4.0f, 6.0f));
    assert(!SphereInTile({0.0f, 0.0f, -1.0f}, 1.0f, 5, 5, proj, w, h, zsign, 4.0f, 6.0f));
    assert(SphereInTile({0.0f, 0.0f, -1.0f}, 20.0f, 0, 0, proj, w, h, zsign, 4.0f, 6.0f));
    assert(SphereInTile({-0.6f, 0.4f, -2.0f}, 0.2f, 6, 6, proj, w, h, zsign, 1.0f, 3.0f));
    assert(!SphereInTile({-0.6f, 0.4f, -2.0f}, 0.05f, 8, 2, proj, w, h, zsign, 1.0f, 3.0f));
    uint32_t hits = 0;
    for (uint32_t ty = 0; ty < 10; ++ty) {
        for (uint32_t tx = 0; tx < 10; ++tx) {
            hits += SphereInTile({-0.6f, 0.4f, -2.0f}, 0.05f, tx, ty, proj, w, h, zsign, 1.0f, 3.0f);
        }
    }
    assert(hits >= 1 && hits <= 4);
}

void TestMergeStableAndKeepAlive() {
    DirectTracks tracks;
    std::vector<Light> out;
    std::vector<Light> frame;
    for (uint32_t d = 0; d < 80; ++d) {
        frame.push_back(MakeLight({10.8f + float(d % 3) * 1e-4f, -0.441f, -1.267f}, 12.0f, {4.0f, 3.0f, 2.0f}));
    }
    for (uint32_t d = 0; d < 64; ++d) {
        frame.push_back(MakeLight({0.024f, -0.512f, 4.568f}, 8.0f, {3.0f + float(d) * 0.01f, 2.0f, 1.0f}));
    }
    frame.push_back(MakeLight({2.0f, 0.0f, 0.0f}, 4.0f, {1.0f, 1.0f, 1.0f}, LightKind::Spot));
    Light dir{};
    dir.kind = LightKind::Directional;
    frame.push_back(dir);
    Light no_world = MakeLight({5.0f, 0.0f, 0.0f}, 4.0f, {1.0f, 1.0f, 1.0f});
    no_world.has_world = false;
    frame.push_back(no_world);
    const uint32_t dup = MergeDirectLights(frame, 10, 2, tracks, out);
    assert(dup == 79 + 63);
    assert(out.size() == 3 && tracks.size() == 3);
    std::vector<uint64_t> order;
    for (const Light& l : out) {
        order.push_back(DirectKey(l));
    }
    std::vector<Light> shuffled(frame.rbegin(), frame.rend());
    MergeDirectLights(shuffled, 11, 2, tracks, out);
    assert(out.size() == 3);
    for (size_t i = 0; i < out.size(); ++i) {
        assert(DirectKey(out[i]) == order[i]);
    }
    std::vector<Light> only_spot{frame[144]};
    MergeDirectLights(only_spot, 12, 2, tracks, out);
    assert(out.size() == 3);
    MergeDirectLights(only_spot, 13, 2, tracks, out);
    assert(out.size() == 3);
    MergeDirectLights(only_spot, 14, 2, tracks, out);
    assert(out.size() == 1 && out[0].kind == LightKind::Spot && tracks.size() == 1);
    MergeDirectLights(frame, 15, 0, tracks, out);
    assert(out.size() == 3);
    std::vector<Light> none;
    MergeDirectLights(none, 16, 0, tracks, out);
    assert(out.empty() && tracks.empty());
    const Light moved = MakeLight({10.8f + 0.5f, -0.441f, -1.267f}, 12.0f, {4.0f, 3.0f, 2.0f});
    assert(DirectKey(moved) != DirectKey(frame[0]));
    const Light flicker = MakeLight({10.8f, -0.441f, -1.267f}, 12.0f, {1.0f, 1.0f, 1.0f});
    assert(DirectKey(flicker) == DirectKey(frame[0]));
}

void TestMovingTorchKeepsOneTrack() {
    DirectTracks tracks;
    std::vector<Light> out;
    std::vector<uint64_t> keys;
    const Light lamp = MakeLight({4.0f, 0.0f, 0.0f}, 4.0f, {2.0f, 2.0f, 2.0f});
    const Light near_lamp = MakeLight({4.3f, 0.0f, 0.0f}, 4.0f, {2.0f, 2.0f, 2.0f});
    uint64_t torch_id = 0;
    for (uint64_t f = 1; f <= 40; ++f) {
        const float x = 0.07f * float(f);
        const float flick = 1.0f + 0.2f * float(f % 3);
        Light torch = MakeLight({x, 1.0f, 0.0f}, 5.0f * (1.0f + 0.03f * float(f % 2)),
                                {6.0f * flick, 3.0f * flick, 1.0f});
        torch.legacy = true;
        std::vector<Light> frame;
        for (int d = 0; d < 12; ++d) {
            frame.push_back(torch);
        }
        frame.push_back(lamp);
        frame.push_back(near_lamp);
        if (f % 5 == 0) {
            frame.erase(frame.begin(), frame.begin() + 12);
        }
        const uint32_t dup = MergeDirectLights(frame, f, 2, tracks, out, &keys);
        assert(dup == (f % 5 == 0 ? 0u : 11u));
        assert(out.size() == 3 && keys.size() == 3);
        assert(keys[0] < keys[1] && keys[1] < keys[2]);
        uint32_t torches = 0;
        for (size_t i = 0; i < out.size(); ++i) {
            if (out[i].legacy) {
                ++torches;
                if (torch_id == 0) {
                    torch_id = keys[i];
                }
                assert(keys[i] == torch_id);
                if (f % 5 != 0) {
                    assert(Near(out[i].world[0], x, 1e-5f));
                }
            }
        }
        assert(torches == 1);
    }
    std::vector<Light> far{MakeLight({0.07f * 40.0f + 2.0f, 1.0f, 0.0f}, 5.0f, {6.0f, 3.0f, 1.0f})};
    far[0].legacy = true;
    MergeDirectLights(far, 41, 2, tracks, out, &keys);
    assert(out.size() == 4 && tracks.size() == 4);
}

// A static emitter is kept off screen only after the game drew it for DirectKeepMinFrames: a
// light seen for a few frames (the player's follow light leaking past its filter) is dropped.
void TestStaticEmitterKeepNeedsFrames() {
    LightKeepView kv{};
    kv.valid = true;
    kv.xs = 1.0f;
    kv.ys = 1.0f;
    kv.offscreen_frames = 1800;
    kv.view = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    for (const uint32_t seen_frames : {3u, DirectKeepMinFrames}) {
        DirectTracks tracks;
        std::vector<Light> out;
        Light lamp = MakeLight({0.0f, 0.0f, 5.0f}, 2.0f, {2.0f, 1.0f, 0.5f});
        lamp.emitter = true;
        uint64_t f = 1;
        for (; f <= seen_frames; ++f) {
            MergeDirectLights({lamp}, f, 2, tracks, out, nullptr, &kv);
        }
        // In the frustum, no longer drawn, well past the 2-frame keep.
        MergeDirectLights({}, f + 10, 2, tracks, out, nullptr, &kv);
        assert(out.size() == (seen_frames >= DirectKeepMinFrames ? 1u : 0u));
    }
}

void TestPackAndLimits() {
    Light l = MakeLight({1.0f, 2.0f, 3.0f}, 4.0f, {1.0f, 2.0f, 3.0f}, LightKind::Spot);
    l.cone[0] = 7.0f;
    l.wrap = {0.5f, 0.25f};
    l.spot_flag = 1.0f;
    Affine view{};
    view[0] = view[5] = view[10] = 1.0f;
    view[3] = -1.0f;
    const DirectLightGpu g = PackDirectLight(l, view);
    assert(g.pos_inv_r2[0] == 0.0f && g.pos_inv_r2[1] == 2.0f && g.pos_inv_r2[3] == 1.0f / 16.0f);
    assert(g.spec_kind[3] == 1.0f && g.wrap_r_flag[2] == 4.0f && g.cone[0] == 7.0f);
    assert(g.wrap_r_flag[0] == 0.5f && g.wrap_r_flag[3] == 0.0f);
    assert(DirectSetBytes == 64800u);
    assert(DirectFormatMax < 65024.0f);
    assert(MaxDirectLights * sizeof(DirectLightGpu) == 128u * 1024u);
}

std::array<float, 16> SpotCone(std::array<float, 3> light, std::array<float, 3> axis,
                               float half_deg) {
    const float len = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    for (float& a : axis) {
        a /= len;
    }
    const std::array<float, 3> helper = std::fabs(axis[2]) < 0.9f ? std::array<float, 3>{0.0f, 0.0f, 1.0f}
                                                                   : std::array<float, 3>{1.0f, 0.0f, 0.0f};
    std::array<float, 3> u{helper[1] * axis[2] - helper[2] * axis[1],
                           helper[2] * axis[0] - helper[0] * axis[2],
                           helper[0] * axis[1] - helper[1] * axis[0]};
    const float ul = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    for (float& c : u) {
        c /= ul;
    }
    const std::array<float, 3> v{axis[1] * u[2] - axis[2] * u[1], axis[2] * u[0] - axis[0] * u[2],
                                 axis[0] * u[1] - axis[1] * u[0]};
    const float cot = 1.0f / std::tan(half_deg * 3.14159265f / 180.0f);
    const auto dot = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    };
    std::array<float, 16> m{};
    const std::array<std::array<float, 3>, 4> rows{u, v, axis, axis};
    const std::array<float, 4> scale{cot, cot, 1.0f, 1.0f};
    for (uint32_t r = 0; r < 4; ++r) {
        m[r * 4] = rows[r][0] * scale[r];
        m[r * 4 + 1] = rows[r][1] * scale[r];
        m[r * 4 + 2] = rows[r][2] * scale[r];
        m[r * 4 + 3] = -dot(rows[r], light) * scale[r];
    }
    return m;
}

struct StockSpot {
    float diffuse;
    float specular;
};

StockSpot StockSpotDis(const std::array<float, 16>& s, std::array<float, 3> P,
                       std::array<float, 3> L, float s7, float s15) {
    const float v23 = s[11] + s[10] * P[2] + s[9] * P[1] + s[8] * P[0];
    if (0.0f >= v23) {
        return {0.0f, 0.0f};
    }
    float v24 = s[3] + s[2] * P[2] + s[1] * P[1] + s[0] * P[0];
    float v25 = s[7] + s[6] * P[2] + s[5] * P[1] + s[4] * P[0];
    float v26 = s[15] + s[14] * P[2] + s[13] * P[1] + s[12] * P[0];
    const float v17 = L[0] - P[0], v7 = L[1] - P[1], v4 = L[2] - P[2];
    float v31 = v4 * v4 + v7 * v7 + v17 * v17;
    const auto clamp01 = [](float x) { return std::min(1.0f, std::max(0.0f, x)); };
    float v36, v35;
    if (s15 > 0.0f) {
        const float v34 = 1.0f / (s7 * s7);
        v35 = 1.0f / v31;
        v36 = std::max(0.0f, v35 - v34);
        v26 = 1.0f / v26;
        v24 = v24 * v26;
        v25 = v25 * v26;
        v24 = v24 * v24 + v25 * v25;
        v26 = clamp01(1.0f - v24);
        v25 = clamp01(v31 * v34);
        v31 = 1.0f - v25;
        return {v36 * v26, v31 * v26};
    }
    v31 = 1.0f;
    v36 = 1.0f;
    v35 = 1.0f / s7;
    v35 = v26 * v35;
    v35 = clamp01(1.0f - v35);
    v26 = 1.0f / v26;
    v24 = v24 * v26;
    v25 = v25 * v26;
    v24 = v24 * v24 + v25 * v25;
    v24 = clamp01(1.0f - v24);
    v26 = v24 * v24;
    v35 = v35 * v35;
    const float v1 = v26 * v35;
    return {v1, v1};
}

void TestSpotMatchesStock() {
    const std::array<float, 3> light{0.5f, 1.5f, -4.0f};
    const std::array<float, 3> axis{0.2f, -0.3f, -1.0f};
    const float radius = 6.0f;
    const float half = 30.0f;
    const auto cone = SpotCone(light, axis, half);
    const float len = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    const std::array<float, 3> a{axis[0] / len, axis[1] / len, axis[2] / len};
    const std::array<float, 3> u{0.0f, 1.0f, 0.0f};
    std::array<float, 3> side{u[1] * a[2] - u[2] * a[1], u[2] * a[0] - u[0] * a[2],
                              u[0] * a[1] - u[1] * a[0]};
    const float sl = std::sqrt(side[0] * side[0] + side[1] * side[1] + side[2] * side[2]);
    for (float& c : side) {
        c /= sl;
    }
    uint32_t checked = 0, lit = 0;
    for (float flag : {1.0f, 0.0f}) {
        const float kind = flag > 0.0f ? SpotKindFalloff : SpotKindAxial;
        for (float deg : {0.0f, 10.0f, 20.0f, 29.0f, 31.0f, 45.0f, 100.0f}) {
            for (float dist : {0.5f, 1.0f, 2.0f, 4.0f, 5.9f, 6.5f, 8.0f}) {
                const float rad = deg * 3.14159265f / 180.0f;
                const std::array<float, 3> dir{a[0] * std::cos(rad) + side[0] * std::sin(rad),
                                               a[1] * std::cos(rad) + side[1] * std::sin(rad),
                                               a[2] * std::cos(rad) + side[2] * std::sin(rad)};
                const std::array<float, 3> p{light[0] + dir[0] * dist, light[1] + dir[1] * dist,
                                             light[2] + dir[2] * dist};
                const StockSpot stock = StockSpotDis(cone, p, light, radius, flag);
                const SpotTerms rt = SpotFactor(cone, p, light, radius, kind);
                const float rt_diffuse = rt.lit ? rt.atten * rt.cone : 0.0f;
                const float rt_spec = rt.lit ? rt.range * rt.cone : 0.0f;
                assert(Near(rt_diffuse, stock.diffuse, 2e-3f));
                assert(Near(rt_spec, stock.specular, 2e-3f));
                assert(!rt.lit || rt.legacy_spec == (flag <= 0.0f));
                if (deg > half + 0.5f) {
                    assert(stock.diffuse == 0.0f && !rt.lit);
                } else if (deg < half - 0.5f && dist * std::cos(rad) < radius - 0.1f &&
                           (flag > 0.0f ? dist < radius : true)) {
                    assert(stock.diffuse > 0.0f && rt.lit);
                    ++lit;
                }
                ++checked;
            }
        }
    }
    assert(checked == 98 && lit >= 30);
    const SpotTerms centre = SpotFactor(cone, {light[0] + a[0], light[1] + a[1], light[2] + a[2]},
                                        light, radius, SpotKindFalloff);
    assert(Near(centre.cone, 1.0f));
    const SpotTerms point = SpotFactor(cone, {light[0] + a[0], light[1] + a[1], light[2] + a[2]},
                                       light, radius, SpotKindNone);
    assert(point.lit && point.cone == 1.0f && Near(point.atten, 1.0f - 1.0f / 36.0f));
    const SpotTerms behind = SpotFactor(cone, {light[0] - a[0], light[1] - a[1], light[2] - a[2]},
                                        light, radius, SpotKindFalloff);
    assert(!behind.lit);
}

void TestSpotCullRadius() {
    Light l = MakeLight({0.0f, 0.0f, 0.0f}, 6.0f, {1.0f, 1.0f, 1.0f}, LightKind::Spot);
    l.cone = SpotCone({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, 30.0f);
    l.spot_flag = 0.0f;
    assert(Near(SpotTanHalf(l.cone), std::tan(30.0f * 3.14159265f / 180.0f)));
    const float cull = SpotCullRadius(l);
    assert(Near(cull, 6.0f / std::cos(30.0f * 3.14159265f / 180.0f)));
    Affine view{};
    view[0] = view[5] = view[10] = 1.0f;
    const DirectLightGpu g = PackDirectLight(l, view);
    assert(g.spec_kind[3] == SpotKindAxial && Near(g.pos_inv_r2[3], 1.0f / (cull * cull)));
    assert(g.wrap_r_flag[2] == 6.0f);
    const float rad = 29.5f * 3.14159265f / 180.0f;
    const std::array<float, 3> edge{std::tan(rad) * 5.9f, 0.0f, -5.9f};
    const float d2 = edge[0] * edge[0] + edge[2] * edge[2];
    assert(d2 > 36.0f && d2 * g.pos_inv_r2[3] < 1.0f);
    const SpotTerms t = SpotFactor(l.cone, edge, {0.0f, 0.0f, 0.0f}, 6.0f, SpotKindAxial);
    assert(t.lit && t.cone > 0.0f && t.atten == 1.0f);
    l.spot_flag = 1.0f;
    assert(SpotCullRadius(l) == 6.0f && SpotKind(l) == SpotKindFalloff);
    Light point = MakeLight({0.0f, 0.0f, 0.0f}, 6.0f, {1.0f, 1.0f, 1.0f});
    assert(SpotCullRadius(point) == 6.0f && SpotKind(point) == SpotKindNone);
}

void TestAoHistory() {
    assert(Near(AoBlend(0.0f, 16), 1.0f));
    assert(Near(AoBlend(3.0f, 16), 0.25f));
    assert(Near(AoBlend(40.0f, 16), 1.0f / 16.0f));
    assert(AoHistoryAccepts(-10.0f, -10.3f, 0.05f));
    assert(!AoHistoryAccepts(-10.0f, -11.0f, 0.05f));
    assert(AoHistoryAccepts(-0.5f, -0.52f, 0.05f));
    assert(!AoHistoryAccepts(-0.5f, -0.6f, 0.05f));
    assert(sizeof(AoParams) == 224);
    assert(AoFlagHistory == 1u && AoFlagNormal == 2u);
    const std::array<uint32_t, 4> inputs{5, 7, 9, 0};
    const std::array<uint32_t, 2> gbuffer{5, 9};
    assert(PickAoMask<uint32_t>(inputs, gbuffer, 7u) == 0u);
    assert(PickAoMask<uint32_t>(inputs, gbuffer, 1u) == 7u);
}

void TestAoEncoding() {
    const float inf = std::numeric_limits<float>::infinity();
    const std::array<float, 2> open{inf, inf};
    const std::array<float, 2> contact{0.0f, 0.0f};
    const std::array<float, 2> half{0.25f, inf};
    const std::array<float, 2> far{0.5f, 2.0f};
    assert(Near(AoVisibility(open, 0.5f), 1.0f));
    assert(Near(AoVisibility(contact, 0.5f), 0.0f));
    assert(Near(AoVisibility(half, 0.5f), 0.75f));
    assert(Near(AoVisibility(far, 0.5f), 1.0f));
    assert(Near(AoVisibility({}, 0.5f), 1.0f));
    for (float f : {0.0f, 0.5f, 1.0f}) {
        const float stock_open = MoonShadowFromMask({1.0f, 1.0f, 1.0f, 1.0f}, f, {}).ssao_mask;
        const float stock_dark = MoonShadowFromMask({0.0f, 1.0f, 1.0f, 1.0f}, f, {}).ssao_mask;
        assert(Near(AoMaskTerm(f, AoVisibility(open, 0.5f)), stock_open));
        assert(Near(AoMaskTerm(f, AoVisibility(contact, 0.5f)), stock_dark));
        assert(Near(AoMaskTerm(f, 1.0f), 1.0f));
        assert(Near(AoMaskTerm(f, 0.0f), 1.0f - f));
        assert(AoMaskTerm(f, 0.3f) >= 1.0f - f && AoMaskTerm(f, 0.3f) <= 1.0f);
    }
    assert(AoCells(2296, 2) == 1148 && AoCells(960, 2) == 480 && AoCells(961, 2) == 481);
    assert(AoCells(100, 1) == 100);
    const auto p0 = AoBlockPixel({3, 4}, 2, {2296, 960});
    const auto edge = AoBlockPixel({480, 1}, 2, {961, 960});
    const auto one = AoBlockPixel({3, 4}, 1, {100, 100});
    assert(p0[0] == 6 && p0[1] == 8 && edge[0] == 960 && edge[1] == 2 && one[0] == 3 && one[1] == 4);
    for (float f : {0.3f, 0.7f, 1.0f}) {
        for (float x : {0.1f, 0.25f, 0.5f, 0.75f, 0.9f}) {
            const float stock = MoonShadowFromMask({x, 1.0f, 1.0f, 1.0f}, f, {}).ssao_mask;
            assert(Near(AoMaskTerm(f, x), stock));
            const std::array<float, 1> hit{0.5f * x};
            assert(Near(AoMaskTerm(f, AoVisibility(hit, 0.5f)), stock));
        }
    }
    assert(!AoBlockKeepsStock(-10.1f, -10.0f, AoBlockZTolerance));
    assert(AoBlockKeepsStock(-10.5f, -10.0f, AoBlockZTolerance));
    assert(!AoBlockKeepsStock(-0.51f, -0.5f, AoBlockZTolerance));
    assert(AoBlockKeepsStock(-0.6f, -0.5f, AoBlockZTolerance));
    const AoStatsMean none = AoStatsReadback({0, 0, 0});
    assert(Near(none.stock, 1.0f) && Near(none.ours, 1.0f));
    const AoStatsMean half_mean = AoStatsReadback({uint32_t(AoStatsScale) * 2, uint32_t(AoStatsScale), 2});
    assert(Near(half_mean.stock, 1.0f) && Near(half_mean.ours, 0.5f));
}

void TestDirectHookDraw() {
    assert(DirectHookDraw(LightCmbAlbedoShader));
    assert(DirectHookDraw(LightCompositeShader));
    assert(!DirectHookDraw(DeferredAoShader));
    assert(!DirectHookDraw(0xd92266b9ull));
    assert(!DirectHookDraw(0));
    assert(DirectHookFires(LightCmbAlbedoShader, 10, 10));
    assert(!DirectHookFires(LightCompositeShader, ~0ull, 10));
    assert(!DirectHookFires(DeferredAoShader, 10, 10));
    LightDrawSkip s{};
    s.direct_enabled = true;
    s.blended = true;
    s.frame = 11;
    s.direct_frame = 10;
    s.diffuse = s.direct_diffuse = 82;
    s.specular = s.direct_specular = 83;
    assert(SkipLightDraw(s));
    s.composite = DirectHookDraw(LightCmbAlbedoShader);
    assert(!SkipLightDraw(s));
}

void TestLightDrawSkip() {
    LightDrawSkip s{};
    s.direct_enabled = true;
    s.blended = true;
    s.frame = 101;
    s.direct_frame = 100;
    s.diffuse = s.direct_diffuse = 7;
    s.specular = s.direct_specular = 9;
    assert(SkipLightDraw(s));
    LightDrawSkip off = s;
    off.direct_enabled = false;
    assert(!SkipLightDraw(off));
    LightDrawSkip paused = s;
    paused.paused = true;
    assert(!SkipLightDraw(paused));
    LightDrawSkip first_on = s;
    first_on.direct_frame = 50;
    assert(!SkipLightDraw(first_on));
    LightDrawSkip never = s;
    never.direct_frame = ~0ull;
    assert(!SkipLightDraw(never));
    LightDrawSkip same_frame = s;
    same_frame.direct_frame = 101;
    assert(!SkipLightDraw(same_frame));
    LightDrawSkip composite = s;
    composite.composite = true;
    assert(!SkipLightDraw(composite));
    LightDrawSkip other_targets = s;
    other_targets.diffuse = 8;
    assert(!SkipLightDraw(other_targets));
    LightDrawSkip one_target = s;
    one_target.specular = one_target.direct_specular = 0;
    assert(!SkipLightDraw(one_target));
    LightDrawSkip opaque = s;
    opaque.blended = false;
    assert(!SkipLightDraw(opaque));
    LightDrawSkip depth = s;
    depth.writes_depth = true;
    assert(!SkipLightDraw(depth));
    LightDrawSkip stencil = s;
    stencil.writes_stencil = true;
    assert(!SkipLightDraw(stencil));
}

void TestLegacyBrdf() {
    DirectLightGpu l{};
    l.pos_inv_r2 = {0.0f, 1.0f, -1.0f, 1.0f / 16.0f};
    l.color_sp = {2.0f, 1.0f, 0.5f, 20.0f};
    l.spec_kind = {1.0f, 0.5f, 0.25f, 0.0f};
    l.wrap_r_flag = {0.0f, 0.0f, 4.0f, 1.0f};
    const std::array<float, 3> p{0.0f, 0.0f, -2.0f};
    const std::array<float, 3> n{0.0f, 0.0f, 1.0f};
    const PointTerms t = PointLightTerms(p, n, l, {0.5f, 0.5f, 0.5f}, 0.0f, 0.0f, 1.0f);
    const float f = 1.0f - std::sqrt(2.0f) / 4.0f;
    const float atten = f * f, nl = 0.70711f, g2 = 0.25f;
    assert(Near(t.diffuse[0], 2.0f * nl * atten * (1.0f - g2 * 0.5f)));
    assert(Near(t.diffuse[2], 0.5f * nl * atten * (1.0f - g2 * 0.5f)));
    const float sp = SpecPower(20.0f, 0.0f);
    const float spec = std::pow(0.70711f, 0.25f * sp) * (0.25f + 0.125f * sp) * atten;
    assert(Near(t.specular[0], spec * g2));
    assert(Near(t.specular[1], 0.5f * spec * g2));
    l.pos_inv_r2[3] = 0.01f;
    DirectLightGpu d = l;
    d.wrap_r_flag[3] = 0.0f;
    const std::array<float, 3> far_p{0.0f, 1.0f, -4.0f};
    const PointTerms tl = PointLightTerms(far_p, n, l, {0.5f, 0.5f, 0.5f}, 0.0f, 0.0f, 1.0f);
    const PointTerms tdf = PointLightTerms(far_p, n, d, {0.5f, 0.5f, 0.5f}, 0.0f, 0.0f, 1.0f);
    assert(tl.diffuse[0] > tdf.diffuse[0] * 2.0f);
    const std::array<float, 3> near_p{0.0f, 1.0f, -1.2f};
    const PointTerms tn = PointLightTerms(near_p, n, l, {0.5f, 0.5f, 0.5f}, 0.0f, 0.0f, 1.0f);
    const PointTerms tdn = PointLightTerms(near_p, n, d, {0.5f, 0.5f, 0.5f}, 0.0f, 0.0f, 1.0f);
    assert(tdn.diffuse[0] > tn.diffuse[0] * 10.0f);
    assert(LegacyAtten(16.0f, 1.0f / 16.0f) == 0.0f && LegacyAtten(0.0f, 1.0f / 16.0f) == 1.0f);

    Light a = MakeLight({1.0f, 2.0f, 3.0f}, 4.0f, {1.0f, 1.0f, 1.0f});
    Light b = a;
    b.legacy = true;
    assert(DirectKey(a) != DirectKey(b));
    Affine view{};
    view[0] = view[5] = view[10] = 1.0f;
    assert(PackDirectLight(b, view).wrap_r_flag[3] == 1.0f);
    assert(PackDirectLight(a, view).wrap_r_flag[3] == 0.0f);
    assert(PackDirectLight(b, view).spec_kind[3] == 0.0f);
}

void TestRestirTargetLegacy() {
    DirectLightGpu l{};
    l.pos_inv_r2 = {0.0f, 1.0f, -1.0f, 0.01f};
    l.color_sp = {2.0f, 1.0f, 0.5f, 20.0f};
    l.spec_kind = {1.0f, 0.5f, 0.25f, 0.0f};
    l.wrap_r_flag = {0.0f, 0.0f, 10.0f, 1.0f};
    DirectLightGpu d = l;
    d.wrap_r_flag[3] = 0.0f;
    const std::array<float, 3> g3{0.5f, 0.5f, 0.5f};
    const std::array<float, 3> n{0.0f, 0.0f, 1.0f};
    const std::array<float, 3> p{0.0f, 1.0f, -4.0f};
    const PointTerms t = PointLightTerms(p, n, l, g3, 0.0f, 0.0f, 1.0f);
    float luma = 0.0f;
    for (int c = 0; c < 3; ++c) {
        luma += (t.diffuse[c] + t.specular[c]) * std::array<float, 3>{0.299f, 0.587f, 0.114f}[c];
    }
    const float legacy = RestirTargetPdf(p, n, l, g3, 0.0f, 0.0f);
    assert(legacy > 0.0f && Near(legacy, luma));
    assert(legacy > RestirTargetPdf(p, n, d, g3, 0.0f, 0.0f) * 2.0f);
    const std::array<float, 3> grazing{2.0f, 0.0f, -2.0f};
    DirectLightGpu behind = l;
    behind.pos_inv_r2 = {4.0f, 0.0f, -2.2f, 1.0f / 16.0f};
    DirectLightGpu behind_d = behind;
    behind_d.wrap_r_flag[3] = 0.0f;
    assert(RestirTargetPdf(grazing, n, behind, g3, 0.0f, 0.0f) > 0.0f);
    assert(RestirTargetPdf(grazing, n, behind_d, g3, 0.0f, 0.0f) == 0.0f);
    DirectLightGpu out = l;
    out.pos_inv_r2[3] = 1.0f;
    assert(RestirTargetPdf(p, n, out, g3, 0.0f, 0.0f) == 0.0f);
}

void TestDirectWriteMode() {
    assert(DirectWriteMode(0, true) == DirectWrite::Add);
    assert(DirectWriteMode(0, false) == DirectWrite::PassThrough);
    assert(DirectWriteMode(1, true) == DirectWrite::Overwrite);
    assert(DirectWriteMode(3, false) == DirectWrite::Overwrite);
}

void TestProxyWriteByReadMarksDirty() {
    using Vulkan::SceneResolution::Coherence;
    Coherence s;
    s.NativeWrite();
    s.CopiedToProxy();
    s.ProxyAccess(false);
    assert(s.valid && !s.dirty);
    s.ProxyAccess(true);
    assert(s.valid && s.dirty);
    s.Resolved();
    assert(s.valid && !s.dirty);
    s.NativeWrite();
    assert(!s.valid && !s.dirty);
}

void TestPreAoPairWithSkippedLightDraws() {
    constexpr uint32_t gbuffer_irr = 3, ao_spec_in = 4, out_diffuse = 7, out_specular = 9;
    constexpr uint32_t rlr = 11;
    PreAoPair<uint32_t> pair;
    assert(!pair.Valid(100));
    pair.Input(100, gbuffer_irr, out_diffuse, out_specular);
    pair.Input(100, ao_spec_in, out_diffuse, out_specular);
    pair.Input(100, gbuffer_irr, out_diffuse, out_specular);
    pair.Input(100, out_diffuse, out_diffuse, out_specular);
    pair.Input(100, 0u, out_diffuse, out_specular);
    assert(pair.Valid(100));
    assert(pair.inputs[0] == gbuffer_irr && pair.inputs[1] == ao_spec_in);
    assert(ReflectUsesPreAo(pair.Valid(100), true, true, 0));
    assert(!ReflectUsesPreAo(pair.Valid(100), true, false, 0));
    assert(!ReflectUsesPreAo(pair.Valid(100), false, false, 0));
    assert(!ReflectUsesPreAo(pair.Valid(100), true, true, 3));
    assert(!pair.Valid(101));
    pair.Input(101, gbuffer_irr, out_diffuse, out_specular);
    assert(!pair.Valid(101));
    pair.Input(101, ao_spec_in, out_diffuse, out_specular);
    pair.Input(101, rlr, out_diffuse, out_specular);
    assert(!pair.Valid(101));
    assert(!ReflectUsesPreAo(pair.Valid(101), true, true, 0));
    pair.Input(102, ao_spec_in, out_diffuse, out_specular);
    pair.Input(102, gbuffer_irr, out_diffuse, out_specular);
    assert(pair.Valid(102));
    const auto is_gbuffer = [&](uint32_t id) { return id == gbuffer_irr; };
    assert(pair.Ordered(is_gbuffer)[0] == gbuffer_irr && pair.Ordered(is_gbuffer)[1] == ao_spec_in);

    assert(ClassifyAoInput(true, false, 1, true) == AoInput::Hdr);
    assert(ClassifyAoInput(false, true, 1, true) == AoInput::HdrAlpha);
    assert(ClassifyAoInput(false, false, 1, true) == AoInput::Reject);
    assert(ClassifyAoInput(false, true, 7, true) == AoInput::Reject);
    assert(ClassifyAoInput(true, false, 1, false) == AoInput::Reject);
    constexpr uint32_t shadow = 13, gbuffer3 = 15;
    PreAoPair<uint32_t> live;
    live.Input(200, shadow, out_diffuse, out_specular, ClassifyAoInput(false, false, 1, true));
    live.Input(200, gbuffer3, out_diffuse, out_specular, ClassifyAoInput(false, false, 1, true));
    live.Input(200, rlr, out_diffuse, out_specular, ClassifyAoInput(false, true, 7, true));
    live.Input(200, ao_spec_in, out_diffuse, out_specular, ClassifyAoInput(false, true, 1, true));
    live.Input(200, gbuffer_irr, out_diffuse, out_specular, ClassifyAoInput(true, false, 1, true));
    live.Input(200, out_diffuse, out_diffuse, out_specular, ClassifyAoInput(true, false, 1, true));
    assert(live.Valid(200));
    const auto none = [](uint32_t) { return false; };
    assert(live.Ordered(none)[0] == gbuffer_irr && live.Ordered(none)[1] == ao_spec_in);
    PreAoPair<uint32_t> fp16;
    fp16.Input(201, gbuffer_irr, out_diffuse, out_specular, AoInput::HdrAlpha);
    fp16.Input(201, ao_spec_in, out_diffuse, out_specular, AoInput::HdrAlpha);
    assert(fp16.Valid(201) && fp16.Ordered(none)[0] == gbuffer_irr);
    PreAoPair<uint32_t> old_filter;
    old_filter.Input(202, gbuffer_irr, out_diffuse, out_specular, AoInput::Hdr);
    old_filter.Input(202, ao_spec_in, out_diffuse, out_specular, AoInput::Reject);
    assert(!old_filter.Valid(202));

    LightDrawSkip s{};
    s.direct_enabled = true;
    s.blended = true;
    s.frame = 101;
    s.direct_frame = 100;
    s.diffuse = s.direct_diffuse = out_diffuse;
    s.specular = s.direct_specular = out_specular;
    assert(SkipLightDraw(s));
    LightDrawSkip ao = s;
    ao.blended = false;
    assert(!SkipLightDraw(ao));
}

struct Lcg {
    uint64_t state = 0x9E3779B97F4A7C15ull;
    float Next() {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return float(state >> 40) * (1.0f / 16777216.0f);
    }
};

struct SyntheticLights {
    std::vector<float> f;
    std::vector<float> pdf;
    std::vector<float> cdf;
    float Sum() const {
        float s = 0.0f;
        for (float v : f) {
            s += v;
        }
        return s;
    }
    uint32_t Sample(float u) const {
        for (uint32_t i = 0; i < cdf.size(); ++i) {
            if (u * cdf.back() < cdf[i]) {
                return i;
            }
        }
        return uint32_t(cdf.size() - 1);
    }
};

SyntheticLights MakeSynthetic() {
    SyntheticLights s;
    s.f = {0.0f, 3.0f, 0.5f, 12.0f, 1.0f, 0.0f, 7.0f, 0.25f, 2.0f, 5.0f};
    std::vector<float> weight = {1.0f, 2.0f, 0.5f, 4.0f, 1.0f, 0.1f, 3.0f, 0.5f, 1.0f, 2.0f};
    float total = 0.0f;
    for (float w : weight) {
        total += w;
        s.cdf.push_back(total);
    }
    for (float w : weight) {
        s.pdf.push_back(w / total);
    }
    return s;
}

Reservoir Candidates(const SyntheticLights& s, uint32_t m, Lcg& rng) {
    Reservoir r;
    for (uint32_t i = 0; i < m; ++i) {
        const uint32_t light = s.Sample(rng.Next());
        const float p_hat = s.f[light];
        ReservoirUpdate(r, light, p_hat / s.pdf[light], p_hat, rng.Next());
    }
    return r;
}

float Estimate(const SyntheticLights& s, const Reservoir& r) {
    return r.light == NoLight ? 0.0f : s.f[r.light] * ReservoirWeight(r);
}

void TestReservoirUpdate() {
    Reservoir r;
    assert(r.light == NoLight && ReservoirWeight(r) == 0.0f);
    ReservoirUpdate(r, 3, 0.0f, 0.0f, 0.5f);
    assert(r.light == NoLight && r.m == 1.0f && r.w_sum == 0.0f);
    ReservoirUpdate(r, 4, 2.0f, 4.0f, 0.99f);
    assert(r.light == 4 && r.m == 2.0f && Near(r.w_sum, 2.0f));
    assert(Near(ReservoirWeight(r), 2.0f / (2.0f * 4.0f)));
    ReservoirUpdate(r, 5, 6.0f, 1.0f, 0.74f);
    assert(r.light == 5 && Near(r.w_sum, 8.0f) && r.p_hat == 1.0f);
    ReservoirUpdate(r, 6, 8.0f, 1.0f, 0.5f);
    assert(r.light == 5 && Near(r.w_sum, 16.0f) && r.m == 4.0f);
    ReservoirUpdate(r, 6, 8.0f, 1.0f, 0.3f);
    assert(r.light == 6 && r.m == 5.0f);
}

void TestReservoirMergeClamp() {
    Reservoir a;
    ReservoirUpdate(a, 1, 4.0f, 2.0f, 0.1f);
    Reservoir b;
    b.light = 2;
    b.m = 100.0f;
    b.p_hat = 1.0f;
    b.w_sum = 100.0f;
    assert(Near(ReservoirWeight(b), 1.0f));
    Reservoir merged = a;
    ReservoirMerge(merged, b, 3.0f, 0.95f, 20.0f);
    assert(merged.m == 21.0f);
    assert(Near(merged.w_sum, 4.0f + 3.0f * 1.0f * 20.0f));
    assert(merged.light == 1);
    Reservoir pick = a;
    ReservoirMerge(pick, b, 3.0f, 0.5f, 20.0f);
    assert(pick.light == 2 && pick.p_hat == 3.0f);
    Reservoir dead = a;
    ReservoirMerge(dead, b, 0.0f, 0.0f, 20.0f);
    assert(dead.light == 1 && dead.m == 21.0f && Near(dead.w_sum, 4.0f));
    Reservoir empty;
    ReservoirMerge(dead, empty, 5.0f, 0.0f, 20.0f);
    assert(dead.light == 1 && dead.m == 21.0f);
}

void TestRisUnbiased() {
    const SyntheticLights s = MakeSynthetic();
    const float truth = s.Sum();
    Lcg rng;
    const int trials = 200000;
    for (uint32_t m : {1u, 4u, 8u, 16u}) {
        double sum = 0.0;
        for (int t = 0; t < trials; ++t) {
            sum += Estimate(s, Candidates(s, m, rng));
        }
        const float mean = float(sum / trials);
        assert(std::fabs(mean - truth) < 0.02f * truth);
    }
    double sum = 0.0;
    for (int t = 0; t < trials; ++t) {
        Reservoir r = Candidates(s, 8, rng);
        const Reservoir other = Candidates(s, 8, rng);
        const float p_hat_here = other.light == NoLight ? 0.0f : s.f[other.light];
        ReservoirMerge(r, other, p_hat_here, rng.Next(), 20.0f);
        assert(r.m == 16.0f);
        sum += Estimate(s, r);
    }
    const float merged_mean = float(sum / trials);
    assert(std::fabs(merged_mean - truth) < 0.02f * truth);
    double temporal = 0.0;
    Reservoir history;
    for (int t = 0; t < trials; ++t) {
        Reservoir r = Candidates(s, 8, rng);
        const float p_hat_here = history.light == NoLight ? 0.0f : s.f[history.light];
        ReservoirMerge(r, history, p_hat_here, rng.Next(), 20.0f);
        assert(r.m <= 28.0f);
        temporal += Estimate(s, r);
        history = r;
    }
    const float temporal_mean = float(temporal / trials);
    assert(std::fabs(temporal_mean - truth) < 0.05f * truth);
}

void TestRemapLightIds() {
    std::vector<uint32_t> out;
    RemapLightIds({}, {}, out);
    assert(out.empty());
    RemapLightIds({10, 20, 30, 40}, {5, 20, 25, 40, 50}, out);
    assert(out.size() == 4);
    assert(out[0] == NoLight && out[1] == 1 && out[2] == NoLight && out[3] == 3);
    RemapLightIds({1, 2, 3}, {}, out);
    assert(out.size() == 3 && out[0] == NoLight && out[2] == NoLight);
    std::vector<Light> frame = {MakeLight({0, 0, 0}, 2, {1, 1, 1}), MakeLight({5, 0, 0}, 2, {1, 1, 1}),
                                MakeLight({9, 0, 0}, 2, {1, 1, 1})};
    DirectTracks tracks;
    std::vector<Light> out_lights;
    std::vector<uint64_t> keys_a, keys_b;
    MergeDirectLights(frame, 1, 0, tracks, out_lights, &keys_a);
    assert(keys_a.size() == 3 && keys_a[0] < keys_a[1] && keys_a[1] < keys_a[2]);
    frame.erase(frame.begin() + 1);
    MergeDirectLights(frame, 2, 0, tracks, out_lights, &keys_b);
    assert(keys_b.size() == 2);
    RemapLightIds(keys_a, keys_b, out);
    int kept = 0;
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] != NoLight) {
            assert(keys_b[out[i]] == keys_a[i]);
            ++kept;
        }
    }
    assert(kept == 2);
}

void TestFireLightFromDump() {
    LightDrawRecord rec{};
    rec.shader = FindLightShader(0x3105494cu);
    assert(rec.shader && rec.shader->legacy && rec.shader->count == 4);
    std::array<float, 52> param{};
    const std::array<float, 12> inv_view{-0.6186f, 0.08903f, 0.7806f, 91.48f, 0.0f, 0.9936f,
                                         -0.1133f, -66.12f, -0.7857f, -0.0701f, -0.6146f, 25.06f};
    for (int i = 0; i < 12; ++i) {
        param[32 + i] = inv_view[i];
    }
    param[47] = 1.0f;
    std::vector<float> data(64, 0.0f);
    const std::array<float, 16> fire{1.021f, -1.598f, 2.471f, 0.04f, 6.536f, 3.257f, 1.005f, 1.0f,
                                     1.307f, 0.6514f, 0.201f, 0.7959f, 1.0f, 1.0f, 0.0f, 0.0f};
    std::copy(fire.begin(), fire.end(), data.begin());
    std::vector<Light> parsed;
    LightDedup dedup;
    assert(RecordLightBuffer(rec, 0, 208, param.data(), parsed, &dedup) == LightBufferRole::Param);
    assert(RecordLightBuffer(rec, 0, 256, data.data(), parsed, &dedup) == LightBufferRole::Local);
    assert(parsed.size() == 1 && parsed[0].legacy && Near(parsed[0].radius, 5.0f));
    assert(ApplyCameraMatrix(rec, 0, parsed) == 1);
    DirectTracks tracks;
    std::vector<Light> kept;
    MergeDirectLights(parsed, 8400, 2, tracks, kept);
    assert(kept.size() == 1);
    Affine iv{};
    std::copy(inv_view.begin(), inv_view.end(), iv.begin());
    const DirectLightGpu g = PackDirectLight(kept[0], InverseRigid(iv));
    assert(Near(g.pos_inv_r2[0], 1.021f, 2e-3f) && Near(g.pos_inv_r2[1], -1.598f, 2e-3f) &&
           Near(g.pos_inv_r2[2], 2.471f, 2e-3f) && g.wrap_r_flag[3] == 1.0f);
    const std::array<float, 4> proj{1.0f / 0.941f, 1.0f / 0.3939f, 1.00002f, -0.0500679f};
    const Screen screen{{2296.0f, 960.0f}, {0.0f, 0.0f}};
    const std::array<float, 3> target{0.5f, -0.6f, 2.0f};
    const std::array<float, 2> px = ViewToPixel(target, proj, screen);
    assert(px[0] >= 0.0f && px[0] < 2296.0f && px[1] >= 0.0f && px[1] < 960.0f);
    const std::array<float, 3> p = PixelToView({std::floor(px[0]), std::floor(px[1])},
                                               ViewToDepth(target[2], proj), proj, screen);
    assert(Near(p[2], 2.0f, 1e-3f));
    const float zsign = proj[3] / std::min(0.5f - proj[2], -1e-8f) > 0.0f ? 1.0f : -1.0f;
    assert(zsign == 1.0f);
    assert(SphereInTile({g.pos_inv_r2[0], g.pos_inv_r2[1], g.pos_inv_r2[2]}, g.wrap_r_flag[2],
                        uint32_t(px[0]) / DirectTile, uint32_t(px[1]) / DirectTile, proj, 2296.0f,
                        960.0f, zsign, p[2] - 0.05f, p[2] + 0.05f));
    const std::array<float, 3> d{g.pos_inv_r2[0] - p[0], g.pos_inv_r2[1] - p[1],
                                 g.pos_inv_r2[2] - p[2]};
    const std::array<float, 3> n = Normalize3(d);
    const PointTerms t = PointLightTerms(p, n, g, {0.2f, 0.2f, 0.2f}, 0.5f, 0.0f, 1.0f);
    const float f = 1.0f - std::sqrt(Dot3(d, d) * 0.04f);
    const float game = f * f * (1.0f - 0.04f * 0.5f);
    for (int c = 0; c < 3; ++c) {
        assert(Near(t.diffuse[c], fire[4 + c] * game, 2e-3f));
    }
    assert(t.diffuse[0] > 3.0f);
}

void TestMoonHalf() {
    assert(MoonHalfFlags(0, false, 0) == 0u);
    assert(MoonHalfFlags(1, false, 0) == DirectFlagMoonHalf);
    assert(MoonHalfFlags(2, false, 0) == (DirectFlagMoonHalf | DirectFlagMoonHalfAlt));
    assert(MoonHalfFlags(1, true, 0) == 0u);
    assert(MoonHalfFlags(2, false, 5) == 0u);
    assert((DirectFlagMoonHalf & (DirectFlagPortals | DirectFlagMoonOwnSet | DirectFlagMoonStatic)) == 0u);
    assert((DirectFlagMoonHalfAlt & (DirectFlagPortals | DirectFlagMoonHalf)) == 0u);
    for (uint32_t flags : {DirectFlagMoonHalf, DirectFlagMoonHalf | DirectFlagMoonHalfAlt}) {
        for (uint32_t frame = 0; frame < 4; ++frame) {
            uint32_t traced = 0;
            for (uint32_t y = 0; y < DirectTile; ++y) {
                for (uint32_t x = 0; x < DirectTile; ++x) {
                    const bool own = MoonHalfTraces(x, y, frame, flags);
                    traced += own ? 1u : 0u;
                    if (own) {
                        continue;
                    }
                    uint32_t near = 0;
                    near += x > 0 && MoonHalfTraces(x - 1, y, frame, flags) ? 1u : 0u;
                    near += x + 1 < DirectTile && MoonHalfTraces(x + 1, y, frame, flags) ? 1u : 0u;
                    near += y > 0 && MoonHalfTraces(x, y - 1, frame, flags) ? 1u : 0u;
                    near += y + 1 < DirectTile && MoonHalfTraces(x, y + 1, frame, flags) ? 1u : 0u;
                    assert(near >= 2u);
                }
            }
            assert(traced == DirectTile * DirectTile / 2);
        }
    }
    assert(MoonHalfTraces(3, 5, 0, DirectFlagMoonHalf) == MoonHalfTraces(3, 5, 1, DirectFlagMoonHalf));
    const uint32_t alt = DirectFlagMoonHalf | DirectFlagMoonHalfAlt;
    assert(MoonHalfTraces(3, 5, 0, alt) != MoonHalfTraces(3, 5, 1, alt));
    assert(MoonHalfTraces(0, 0, 0, alt) == MoonHalfTraces(0, 0, 0, DirectFlagMoonHalf));
}

void TestPyreTileGridLikeGpu() {
    static_assert(offsetof(DirectParams, tlas_count_flags) == 80);
    static_assert(offsetof(DirectParams, cfg) == 96);
    static_assert(offsetof(DirectParams, restir) == 208);
    static_assert(offsetof(DirectParams, fx) == 240);
    static_assert(offsetof(DirectParams, far_fade) == 256);
    static_assert(offsetof(DirectLightGpu, wrap_r_flag) == 48);
    static_assert(offsetof(DirectLightGpu, cone) == 64);
    const std::array<float, 4> proj{1.0f / 0.941f, 1.0f / 0.3939f, 1.00002f, -0.0500679f};
    const float width = 2296.0f, height = 960.0f;
    const float zsign = proj[3] / std::min(0.5f - proj[2], -1e-8f) > 0.0f ? 1.0f : -1.0f;
    const std::array<float, 3> pyre{1.021f, -1.598f, 2.471f};
    const float floor_y = -1.65f;
    const uint32_t tiles_x = uint32_t(width) / DirectTile, tiles_y = uint32_t(height) / DirectTile;
    uint32_t floor_tiles = 0, lit_tiles = 0, bottom_lit = 0;
    for (uint32_t ty = 0; ty < tiles_y; ++ty) {
        for (uint32_t tx = 0; tx < tiles_x; ++tx) {
            uint32_t zmin = 0xFFFFFFFFu, zmax = 0;
            for (uint32_t py = 0; py < DirectTile; ++py) {
                for (uint32_t px = 0; px < DirectTile; ++px) {
                    const std::array<float, 3> dir = TileCornerDir(
                        float(tx * DirectTile + px) + 0.5f, float(ty * DirectTile + py) + 0.5f, proj,
                        width, height, zsign);
                    if (dir[1] >= -1e-4f) {
                        continue;
                    }
                    const float z = floor_y / dir[1] * dir[2];
                    if (z > 60.0f) {
                        continue;
                    }
                    const uint32_t az = uint32_t(std::clamp(std::fabs(z) * 64.0f, 0.0f, 4.0e9f));
                    zmin = std::min(zmin, az);
                    zmax = std::max(zmax, az);
                }
            }
            if (zmax < zmin) {
                continue;
            }
            ++floor_tiles;
            const bool hit = SphereInTile(pyre, 5.0f, tx, ty, proj, width, height, zsign,
                                          float(zmin) / 64.0f, float(zmax) / 64.0f + 1.0f / 64.0f);
            lit_tiles += hit;
            bottom_lit += hit && ty == tiles_y - 1;
        }
    }
    assert(floor_tiles > 1000);
    assert(bottom_lit == tiles_x);
    assert(lit_tiles > 2000);
}

}

void TestGpuLightSets() {
    const GpuSetSource none{};
    assert(!GpuLightSetsThisFrame(none, 7));
    const GpuSetSource sets{0x1000, 225 * DirLightSetStride, 7};
    assert(GpuLightSetsThisFrame(sets, 7));
    assert(!GpuLightSetsThisFrame(sets, 8));
    const GpuSetSource odd{0x1000, 225 * DirLightSetStride + 4, 7};
    assert(!GpuLightSetsThisFrame(odd, 7));
    const GpuSetSource scene{0x1000, SceneParamBytes, 7};
    assert(!GpuLightSetsThisFrame(scene, 7));
    const GpuSetSource big{0x1000, DirectSetBytes + DirLightSetStride, 7};
    assert(!GpuLightSetsThisFrame(big, 7));
    assert(GpuLightSetsAligned(0, 16));
    assert(GpuLightSetsAligned(256, 16));
    assert(!GpuLightSetsAligned(8, 16));
    assert(!GpuLightSetsAligned(0, 0));
    assert(GpuLightSetsFresh(sets, 8, GiLightSetsMaxAge));
    assert(!GpuLightSetsFresh(sets, 9, GiLightSetsMaxAge));
    assert(!GpuLightSetsFresh(sets, 6, GiLightSetsMaxAge));
    assert(!GpuLightSetsFresh(none, 7, 100));
}

void TestSetReadbackPick() {
    const std::array<SetReadback, 3> slots{{{10, 30, 288}, {11, 31, 288}, {12, 32, 288}}};
    const auto free_up_to = [](uint64_t limit) {
        return [limit](uint64_t tick) { return tick <= limit; };
    };
    assert(NewestReadySetReadback(slots, free_up_to(12)) == 2);
    assert(NewestReadySetReadback(slots, free_up_to(11)) == 1);
    assert(NewestReadySetReadback(slots, free_up_to(9)) == -1);
    const std::array<SetReadback, 3> empty{{{10, 30, 0}, {}, {11, 31, 288}}};
    assert(NewestReadySetReadback(empty, free_up_to(12)) == 2);
    assert(NewestReadySetReadback(empty, free_up_to(10)) == -1);
}

void TestCompareDirSets() {
    constexpr uint32_t stride = DirLightSetStride / 4;
    std::vector<float> a(3 * stride, 0.0f), b;
    a[16] = 0.5f;
    a[stride + 16] = 0.25f;
    const DirSetDiff first = CompareDirSets({}, a);
    assert(first.changed == 3 && first.first == 0 && first.zero == 1);
    b = a;
    const DirSetDiff same = CompareDirSets(a, b);
    assert(same.changed == 0 && same.first == NoLight && same.zero == 1);
    b[stride + 17] = 0.75f;
    b[2 * stride + 1] = 0.1f;
    const DirSetDiff moved = CompareDirSets(a, b);
    assert(moved.changed == 2 && moved.first == 1 && moved.zero == 0);
    assert(Near(moved.color_delta, 0.75f) && Near(moved.dir_delta, 0.1f));
    b = a;
    b[stride + 64] = 9.0f;
    const DirSetDiff extra = CompareDirSets(a, b);
    assert(extra.changed == 1 && Near(extra.color_delta, 0.0f) && Near(extra.dir_delta, 0.0f));
}

void TestMoonRayGate() {
    assert(MoonRayNeeded(0.5f, 1.0f, 1.0f, 0.0f, 0.2f, 0.0f));
    assert(!MoonRayNeeded(0.0f, 1.0f, 1.0f, 0.0f, 0.2f, 0.0f));
    assert(!MoonRayNeeded(0.5f, 0.0f, 1.0f, 0.0f, 0.2f, 0.0f));
    assert(!MoonRayNeeded(0.5f, 1.0f, 0.0f, 1.0f, 0.2f, 0.2f));
    assert(!MoonRayNeeded(0.5f, 1.0f, 1.0f, 0.0f, 0.5e-3f, 0.0f));
    assert(MoonRayNeeded(0.5f, 1.0f, 1.0f, 1.0f, 0.5e-3f, 0.6e-3f));
    assert(!MoonRayNeeded(0.5f, 1.0f, 1.0f, 0.0f, 0.5e-3f, 0.6e-3f));
}

void TestMoonTraced() {
    const auto lit = MoonShadowTraced(1.0f, {0.6f, 0.5f, 0.4f});
    assert(Near(lit[0], 1.0f) && Near(lit[1], 1.0f) && Near(lit[2], 1.0f));
    const auto dark = MoonShadowTraced(0.0f, {0.6f, 0.5f, 0.4f});
    assert(Near(dark[0], 0.4f) && Near(dark[1], 0.5f) && Near(dark[2], 0.6f));
    const auto half = MoonShadowTraced(0.5f, {0.6f, 0.6f, 0.6f});
    assert(Near(half[0], 0.7f));
    const auto no_colour = MoonShadowTraced(0.0f, {0.0f, 0.0f, 0.0f});
    assert(Near(no_colour[0], 1.0f));
    const auto mask = MoonShadowFromMask({1.0f, 1.0f, 1.0f, 0.0f}, 1.0f, {0.6f, 0.5f, 0.4f});
    assert(Near(mask.shadow[0], dark[0]) && Near(mask.shadow[2], dark[2]));
    assert(Near(HalfLambertSq(1.0f), 1.0f) && Near(HalfLambertSq(0.0f), 0.25f) &&
           Near(HalfLambertSq(-1.0f), 0.0f));
    assert(MoonConeCos(0.0f) > 0.99999f && MoonConeCos(0.0f) <= 1.0f);
    assert(std::fabs(MoonConeCos(1.5f) - std::cos(1.5f * 3.14159265f / 180.0f)) < 1e-6f);
    assert(std::fabs(MoonConeCos(10.0f) - std::cos(10.0f * 3.14159265f / 180.0f)) < 1e-5f);
    assert(DepthOnlyDraw(0u) && !DepthOnlyDraw(1u) && !DepthOnlyDraw(0x1Fu));
}

void TestMoonShadowMask() {
    assert(DirShadowMaskDraw(0xd92266b9ull));
    assert(DirShadowMaskDraw(0x00000001d92266b9ull));
    assert(!DirShadowMaskDraw(0x31b90cc5ull));
    assert(!DirShadowMaskDraw(0x3fe9e964ull));
    const std::array<uint32_t, 4> inputs{7u, 3u, 9u, 11u};
    const std::array<uint32_t, 3> gbuffer{3u, 4u, 11u};
    assert(PickShadowMask<uint32_t>(inputs, gbuffer, 7u) == 9u);
    assert(PickShadowMask<uint32_t>(inputs, gbuffer, 9u) == 7u);
    const std::array<uint32_t, 2> only_gbuffer{3u, 4u};
    assert(PickShadowMask<uint32_t>(only_gbuffer, gbuffer, 7u) == 0u);
    const std::array<uint32_t, 0> none{};
    assert(PickShadowMask<uint32_t>(none, gbuffer, 7u) == 0u);
    const auto lit = MoonShadowFromMask({1.0f, 1.0f, 1.0f, 1.0f}, 1.0f, {0.6f, 0.6f, 0.6f});
    assert(Near(lit.ssao_mask, 1.0f) && Near(lit.shadow[0], 1.0f));
    const auto shadowed = MoonShadowFromMask({1.0f, 1.0f, 1.0f, 0.0f}, 1.0f, {0.6f, 0.5f, 0.4f});
    assert(Near(shadowed.shadow[0], 0.4f) && Near(shadowed.shadow[1], 0.5f) &&
           Near(shadowed.shadow[2], 0.6f));
    const auto ao = MoonShadowFromMask({0.5f, 1.0f, 0.5f, 1.0f}, 0.5f, {0.6f, 0.6f, 0.6f});
    assert(Near(ao.ssao_mask, 0.75f) && Near(ao.shadow[0], 0.5f));
    const auto no_colour = MoonShadowFromMask({1.0f, 1.0f, 1.0f, 0.0f}, 1.0f, {0.0f, 0.0f, 0.0f});
    assert(Near(no_colour.shadow[0], 1.0f));
}

void TestOffscreenLightsKept() {
    LightKeepView kv{};
    kv.view = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    kv.xs = 1.0f;
    kv.ys = 1.0f;
    kv.offscreen_frames = 100;
    kv.valid = true;
    assert(SphereInFrustum({0, 0, 5}, 1, 1, 1));
    assert(SphereInFrustum({6, 0, 5}, 2, 1, 1));
    assert(!SphereInFrustum({0, 0, -5}, 1, 1, 1));
    assert(!SphereInFrustum({20, 0, 5}, 1, 1, 1));
    std::vector<Light> frame = {MakeLight({0, 0, 5}, 2, {1, 1, 1}), MakeLight({0, 0, -5}, 2, {1, 1, 1}),
                                MakeLight({0, 0, 5}, 2, {1, 1, 1}, LightKind::Spot)};
    frame[2].cone = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    DirectTracks tracks;
    std::vector<Light> out;
    std::vector<uint64_t> keys;
    MergeDirectLights(frame, 1, 2, tracks, out, &keys, &kv);
    assert(out.size() == 3 && tracks.kept_offscreen == 0);
    const std::vector<Light> none;
    for (uint64_t f = 2; f <= 4; ++f) {
        MergeDirectLights(none, f, 2, tracks, out, &keys, &kv);
    }
    assert(out.size() == 1 && tracks.kept_offscreen == 1);
    assert(out[0].world[2] == -5.0f && keys[0] == 2);
    MergeDirectLights(none, 4, 2, tracks, out, &keys, nullptr);
    assert(out.empty());

    tracks = DirectTracks{};
    MergeDirectLights(frame, 1, 2, tracks, out, &keys, &kv);
    Affine turned = {0, 0, 1, 0, 0, 1, 0, 0, -1, 0, 0, 0};
    LightKeepView kv2 = kv;
    kv2.view = turned;
    for (uint64_t f = 2; f <= 50; ++f) {
        MergeDirectLights(none, f, 2, tracks, out, &keys, &kv2);
    }
    assert(out.size() == 3 && tracks.kept_offscreen == 3);
    for (const Light& l : out) {
        if (l.kind == LightKind::Spot) {
            const std::array<float, 3> p_new = Transform(turned, {0.3f, 0.2f, 7.0f});
            const std::array<float, 3> q{
                l.cone[0] * p_new[0] + l.cone[1] * p_new[1] + l.cone[2] * p_new[2] + l.cone[3],
                l.cone[4] * p_new[0] + l.cone[5] * p_new[1] + l.cone[6] * p_new[2] + l.cone[7],
                l.cone[8] * p_new[0] + l.cone[9] * p_new[1] + l.cone[10] * p_new[2] + l.cone[11]};
            assert(std::fabs(q[0] - 0.3f) < 1e-5f && std::fabs(q[1] - 0.2f) < 1e-5f &&
                   std::fabs(q[2] - 7.0f) < 1e-5f);
        }
    }
    for (uint64_t f = 51; f <= 102; ++f) {
        MergeDirectLights(none, f, 2, tracks, out, &keys, &kv2);
    }
    assert(out.empty());

    tracks = DirectTracks{};
    MergeDirectLights(frame, 1, 2, tracks, out, &keys, &kv);
    std::vector<Light> back = {frame[1]};
    for (uint64_t f = 2; f <= 10; ++f) {
        MergeDirectLights(back, f, 2, tracks, out, &keys, &kv);
    }
    assert(out.size() == 1 && tracks.kept_offscreen == 0 && keys[0] == 2);
}

void TestOffscreenSwitches() {
    std::vector<Light> frame = {MakeLight({0, 0, 5}, 2, {1, 1, 1}, LightKind::Spot)};
    frame[0].cone = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const std::vector<Light> none;
    LightKeepView kv{};
    kv.view = {0, 0, 1, 0, 0, 1, 0, 0, -1, 0, 0, 0};
    kv.xs = 1.0f;
    kv.ys = 1.0f;
    kv.offscreen_frames = 100;
    kv.valid = true;
    DirectTracks tracks;
    std::vector<Light> out;
    MergeDirectLights(frame, 1, 2, tracks, out, nullptr, nullptr);
    MergeDirectLights(none, 2, 2, tracks, out, nullptr, nullptr);
    assert(out.size() == 1 && out[0].cone == frame[0].cone && tracks.kept_offscreen == 0);
    MergeDirectLights(none, 4, 2, tracks, out, nullptr, nullptr);
    assert(out.empty());

    tracks = DirectTracks{};
    LightKeepView seen = kv;
    seen.view = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    MergeDirectLights(frame, 1, 2, tracks, out, nullptr, &seen);
    MergeDirectLights(none, 2, 2, tracks, out, nullptr, &kv);
    assert(out.size() == 1 && out[0].cone != frame[0].cone);
    kv.reproject = false;
    MergeDirectLights(none, 3, 2, tracks, out, nullptr, &kv);
    assert(out.size() == 1 && out[0].cone == frame[0].cone);
    for (uint64_t f = 4; f <= 50; ++f) {
        MergeDirectLights(none, f, 2, tracks, out, nullptr, &kv);
    }
    assert(out.size() == 1 && tracks.kept_offscreen == 1 && out[0].cone == frame[0].cone);
}

std::vector<uint32_t> GridLights(const std::vector<uint32_t>& grid, std::array<float, 3> p) {
    float origin[3], inv;
    std::memcpy(origin, grid.data(), sizeof(origin));
    std::memcpy(&inv, grid.data() + 3, sizeof(inv));
    const uint32_t dims[3] = {grid[4], grid[5], grid[6]};
    uint32_t c[3];
    for (int a = 0; a < 3; ++a) {
        c[a] = uint32_t(std::clamp(std::floor((p[a] - origin[a]) * inv), 0.0f, float(dims[a] - 1)));
    }
    const uint32_t cells = dims[0] * dims[1] * dims[2];
    const uint32_t cell = std::min((c[2] * dims[1] + c[1]) * dims[0] + c[0], cells - 1);
    std::vector<uint32_t> out;
    for (uint32_t k = grid[8 + cell]; k < grid[9 + cell]; ++k) {
        out.push_back(grid[9 + cells + k]);
    }
    return out;
}

void TestLightGridMatchesFullLoop() {
    uint32_t seed = 12345;
    const auto rnd = [&] {
        seed = seed * 1664525u + 1013904223u;
        return float(seed >> 8) / float(1u << 24);
    };
    for (int scene = 0; scene < 20; ++scene) {
        std::vector<DirectLightGpu> lights(1 + scene * 20);
        for (auto& l : lights) {
            const float r = scene % 4 == 0 ? 0.05f + rnd() * 200.0f : 0.5f + rnd() * 15.0f;
            l.pos_inv_r2 = {rnd() * 400.0f - 200.0f, rnd() * 60.0f - 30.0f, -rnd() * 400.0f,
                            1.0f / (r * r)};
        }
        std::vector<uint32_t> grid(LightGridWords);
        assert(BuildLightGrid(lights, grid));
        assert(grid[7] == 1);
        assert(uint64_t(grid[4]) * grid[5] * grid[6] <= LightGridMaxCells);
        for (int i = 0; i < 4000; ++i) {
            const std::array<float, 3> p{rnd() * 500.0f - 250.0f, rnd() * 100.0f - 50.0f,
                                         60.0f - rnd() * 520.0f};
            std::vector<uint32_t> full;
            for (uint32_t k = 0; k < lights.size(); ++k) {
                const auto& l = lights[k].pos_inv_r2;
                const float dx = l[0] - p[0], dy = l[1] - p[1], dz = l[2] - p[2];
                const float d2 = dx * dx + dy * dy + dz * dz;
                if (!(d2 <= 1e-8f || d2 * l[3] >= 1.0f)) {
                    full.push_back(k);
                }
            }
            std::vector<uint32_t> culled;
            for (const uint32_t k : GridLights(grid, p)) {
                assert(k < lights.size());
                const auto& l = lights[k].pos_inv_r2;
                const float dx = l[0] - p[0], dy = l[1] - p[1], dz = l[2] - p[2];
                const float d2 = dx * dx + dy * dy + dz * dz;
                if (!(d2 <= 1e-8f || d2 * l[3] >= 1.0f)) {
                    assert(culled.empty() || culled.back() < k);
                    culled.push_back(k);
                }
            }
            assert(culled == full);
        }
    }
    std::vector<DirectLightGpu> bad(2);
    bad[0].pos_inv_r2 = {0.0f, 0.0f, -5.0f, 0.0f};
    bad[1].pos_inv_r2 = {1.0f, 0.0f, -5.0f, 0.04f};
    std::vector<uint32_t> grid(LightGridWords, 7u);
    assert(!BuildLightGrid(bad, grid) && grid[7] == 0);
    assert(!BuildLightGrid({}, grid) && grid[7] == 0);
}

void TestPackStockDepth() {
    Affine view{};
    view[0] = view[5] = view[10] = 1.0f;
    Light p = MakeLight({0.0f, 0.3f, 3.2f}, 3.5f, {0.467f, 0.467f, 0.467f}, LightKind::Point);
    p.state.valid = true;
    p.state.depth = true;
    p.state.zfunc = 3;
    p.state.has_vtx = true;
    p.state.vtx_z = 0.9f;
    p.state.bounds = true;
    p.state.bounds_min = 0.5f;
    p.state.bounds_max = 0.95f;
    DirectLightGpu g = PackDirectLight(p, view);
    assert(PackStockDepth(g, p));
    assert(g.cone[0] == 3.0f && g.cone[1] == 0.9f && g.cone[2] == 0.5f && g.cone[3] == 0.95f);
    assert(g.cone[4] == 1.0f && g.cone[5] == 1.0f);
    Light kept = p;
    kept.age = 2;
    DirectLightGpu gk = PackDirectLight(kept, view);
    assert(!PackStockDepth(gk, kept) && gk.cone[5] == 0.0f);
    Light s = MakeLight({0.0f, 0.3f, 3.2f}, 3.5f, {1.0f, 1.0f, 1.0f}, LightKind::Spot);
    s.spot_flag = 1.0f;
    s.state = p.state;
    s.cone[5] = 0.25f;
    DirectLightGpu gs = PackDirectLight(s, view);
    assert(!PackStockDepth(gs, s) && gs.cone[5] == 0.25f);
    Light none = MakeLight({0.0f, 0.3f, 3.2f}, 3.5f, {1.0f, 1.0f, 1.0f}, LightKind::Point);
    DirectLightGpu gn = PackDirectLight(none, view);
    assert(!PackStockDepth(gn, none));
    // A lamp reaches past the game's volume: its depth test would clip it. A candle keeps it.
    Light candle = p;
    candle.legacy = true;
    candle.emitter = true;
    assert(EmitterFalloff(candle, 2.5f, 0.2f, 0.0f, 0.4f));
    DirectLightGpu gc = PackDirectLight(candle, view);
    assert(PackStockDepth(gc, candle));
    Light lamp = candle;
    lamp.lamp = true;
    DirectLightGpu gl = PackDirectLight(lamp, view);
    assert(!PackStockDepth(gl, lamp) && gl.cone[5] == 0.0f);
}

// A flame size keeps the far field (colour x source^2) and makes the source small.
void TestEmitterFlameKeepsFarField() {
    Light a = MakeLight({0.0f, 0.0f, 3.0f}, 8.47f, {1.0f, 0.5f, 0.25f}, LightKind::Point);
    a.legacy = true;
    a.emitter = true;
    Light b = a;
    assert(EmitterFalloff(a, 2.5f, 0.2f, 0.0f, 0.4f));
    assert(EmitterFalloff(b, 2.5f, 0.2f, 0.0f, 0.4f, 0.5f));
    assert(std::fabs(b.source - 0.5f) < 1e-6f && a.source > 2.0f);
    const float ia = a.color[0] * a.source * a.source;
    const float ib = b.color[0] * b.source * b.source;
    assert(std::fabs(ia - ib) < 1e-3f * ia);
    assert(b.radius == a.radius && b.color[1] / b.color[0] == a.color[1] / a.color[0]);
    // A flame larger than the converted source changes nothing.
    Light c = MakeLight({0.0f, 0.0f, 3.0f}, 1.0f, {1.0f, 0.5f, 0.25f}, LightKind::Point);
    c.legacy = true;
    Light d = c;
    assert(EmitterFalloff(c, 2.5f, 0.2f, 0.0f, 0.4f));
    assert(EmitterFalloff(d, 2.5f, 0.2f, 0.0f, 0.4f, 0.5f));
    assert(c.source == d.source && c.color[0] == d.color[0]);
}

// Flicker gain scales the deviation from the running mean; gain 1 and a steady light are unchanged.
void TestFlickerScale() {
    FlickerState s0{};
    assert(FlickerScale(10.0f, s0, 3.0f) == 1.0f && s0.mean == 10.0f); // first frame: no deviation
    FlickerState s1{10.0f, 10.0f};
    assert(FlickerScale(11.0f, s1, 1.0f) == 1.0f);
    FlickerState s2{10.0f, 10.0f};
    const float k = FlickerScale(11.0f, s2, 2.0f); // raw: mean 10.1, +8.9% -> +17.8%
    assert(std::fabs(11.0f * k / s2.mean - (1.0f + 2.0f * (11.0f / s2.mean - 1.0f))) < 1e-5f);
    FlickerState s3{10.0f, 10.0f};
    assert(FlickerScale(10.0f, s3, 4.0f) == 1.0f);
    FlickerState s4{10.0f, 10.0f};
    assert(1.0f * FlickerScale(1.0f, s4, 8.0f) >= 0.25f * s4.mean - 1e-4f); // clamped, never negative
    FlickerState s5{10.0f, 10.0f};
    assert(FlickerScale(0.0f, s5, 2.0f) == 1.0f && s5.mean == 10.0f);
    // Smoothing: a one-frame spike moves the light by smooth x the raw step only.
    FlickerState s6{10.0f, 10.0f};
    const float ks = FlickerScale(12.0f, s6, 1.0f, 0.0f, 0.1f);
    assert(std::fabs(12.0f * ks - 10.2f) < 1e-4f);
}

// Flame sway stays within amp (vertical half), changes slowly and differs between lights.
void TestFlameSway() {
    float maxd = 0.0f, maxstep = 0.0f;
    std::array<float, 3> prev = FlameSway(7, 0.0, 0.05f);
    for (int f = 1; f < 600; ++f) {
        const auto o = FlameSway(7, f / 60.0, 0.05f);
        assert(std::fabs(o[0]) <= 0.05f + 1e-6f && std::fabs(o[1]) <= 0.025f + 1e-6f);
        for (int a = 0; a < 3; ++a) {
            maxd = std::max(maxd, std::fabs(o[a]));
            maxstep = std::max(maxstep, std::fabs(o[a] - prev[a]));
        }
        prev = o;
    }
    assert(maxd > 0.02f && maxstep < 0.01f);
    assert(FlameSway(7, 1.0, 0.05f) != FlameSway(8, 1.0, 0.05f));
    assert(FlameSway(7, 1.0, 0.0f) == (std::array<float, 3>{}));
}

// Steady-flame noise: within [-1, 1], ~0.44 rel. std, slow steps, its own rhythm per light.
void TestFlameNoise() {
    double sum = 0.0, sum2 = 0.0;
    float maxstep = 0.0f, prev = FlameNoise(3, 0.0);
    const int n = 6000;
    for (int f = 1; f <= n; ++f) {
        const float v = FlameNoise(3, f / 60.0);
        assert(std::fabs(v) <= 1.0f + 1e-6f);
        sum += v;
        sum2 += double(v) * v;
        maxstep = std::max(maxstep, std::fabs(v - prev));
        prev = v;
    }
    const double mean = sum / n, sd = std::sqrt(sum2 / n - mean * mean);
    assert(std::fabs(mean) < 0.05 && sd > 0.35 && sd < 0.5);
    assert(maxstep < 0.1f); // 60 fps: smooth, no frame-to-frame jumps
    assert(FlameNoise(3, 1.0) != FlameNoise(4, 1.0));
}

// Glow lamps: same seed while the centroid jitters inside its 2 m cell, a different one for the
// next lamp, never a small (track-like) id.
// Flames are red over green; the corpse loot light (neutral) is not one.
void TestPaleParticle() {
    assert(PaleParticle(0xffe6beu));
    assert(PaleParticle(0xffffffu));
    assert(!PaleParticle(0x2060ffu));
    assert(!PaleParticle(0xff6020u));
    assert(!PaleParticle(0u));
}

void TestFlameColour() {
    Light l{};
    l.color = {0.817f, 0.817f, 0.636f};
    assert(!FlameColour(l));
    for (const auto& c : {std::array<float, 3>{2.0f, 1.356f, 0.997f}, {0.2f, 0.15f, 0.072f},
                          {2.967f, 1.665f, 0.748f}, {1.0f, 0.731f, 0.455f}}) {
        l.color = c;
        assert(FlameColour(l));
    }
    l.color = {0.315f, 0.543f, 0.8f};
    assert(!FlameColour(l));
}

// Glow lamps: steady + warm pass; too new, drifting (flying FX) or white (item glow) do not.
void TestGlowLampSteady() {
    const std::array<float, 3> warm{1.0f, 0.65f, 0.4f}, white{1.0f, 1.0f, 1.0f};
    assert(GlowLampSteady(100, 0.01f, warm, 30, 0.1f, 0.8f));
    assert(!GlowLampSteady(10, 0.01f, warm, 30, 0.1f, 0.8f));
    assert(!GlowLampSteady(100, 0.4f, warm, 30, 0.1f, 0.8f));
    assert(!GlowLampSteady(100, 0.01f, white, 30, 0.1f, 0.8f));
}

void TestPositionFlickerId() {
    const uint64_t a = PositionFlickerId({-165.3f, -72.1f, -67.8f});
    assert(a == PositionFlickerId({-165.1f, -72.4f, -67.5f}));
    assert(a != PositionFlickerId({-161.3f, -72.1f, -67.8f}));
    assert((a >> 63) == 1u);
}

// Halos: only fixed converted emitters, nearest first, intensity = colour x source^2 x strength.
void TestBuildHalos() {
    std::vector<DirectLightGpu> l(20);
    std::vector<std::array<float, 3>> spots(20);
    for (size_t i = 0; i < l.size(); ++i) {
        l[i].pos_inv_r2 = {0.0f, 0.0f, -1.0f - float(i), 1.0f};
        l[i].color_sp = {1.0f, 0.5f, 0.25f, 16.0f};
        l[i].wrap_r_flag = {0.0f, 0.0f, 4.0f, 4.0f}; // emitter, source^2 4
        spots[i] = {0.0f, 0.3f, -1.0f - float(i)};    // glass 0.3 m above each light
    }
    l[0].wrap_r_flag[3] = 12.0f; // near a carried torch
    l[1].wrap_r_flag[3] = 1.0f;  // legacy, not converted
    std::array<float, 8 * MaxHalos> out{};
    assert(BuildHalos(l, spots, 0.0f, 1.0f, 0.1f, 0.5f, out.data()) == 0);
    // No light within match: no halo (loot glows, fills). A spot sits on the glass and takes
    // the nearest light's intensity.
    const std::array<std::array<float, 3>, 1> lone{{{5.0f, 0.0f, -1.0f}}};
    assert(BuildHalos(l, lone, 0.5f, 1.2f, 0.1f, 2.0f, out.data()) == 0);
    l[3].color_sp = {2.0f, 1.0f, 0.5f, 16.0f};
    const std::array<std::array<float, 3>, 1> one{{{0.0f, 0.3f, -4.1f}}};
    assert(BuildHalos(l, one, 0.5f, 1.2f, 0.1f, 2.0f, out.data()) == 1);
    assert(out[1] == 0.3f && std::fabs(out[4] - 4.0f) < 1e-6f);
    // Spots 0 and 1 only have a torch or a legacy light within 0.5 m: dropped; 16 nearest, in order.
    const uint32_t n = BuildHalos(l, spots, 0.5f, 1.2f, 0.1f, 0.5f, out.data());
    assert(n == MaxHalos);
    assert(out[2] == -3.0f && out[3] == 1.2f && out[7] == 0.1f);
    assert(out[8 * (MaxHalos - 1) + 2] == -float(2 + MaxHalos));
}

int main() {
    TestBuildHalos();
    TestFlickerScale();
    TestFlameSway();
    TestFlameNoise();
    TestPositionFlickerId();
    TestGlowLampSteady();
    TestFlameColour();
    TestPaleParticle();
    TestEmitterFlameKeepsFarField();
    TestLightGridMatchesFullLoop();
    TestPackStockDepth();
    TestMoonTraced();
    TestMoonShadowMask();
    TestGpuLightSets();
    TestSetReadbackPick();
    TestMoonRayGate();
    TestCompareDirSets();
    TestPointBrdfHand();
    TestWrapBranch();
    TestTileCull();
    TestMergeStableAndKeepAlive();
    TestMovingTorchKeepsOneTrack();
    TestStaticEmitterKeepNeedsFrames();
    TestOffscreenLightsKept();
    TestOffscreenSwitches();
    TestPackAndLimits();
    TestSpotMatchesStock();
    TestSpotCullRadius();
    TestAoHistory();
    TestAoEncoding();
    TestDirectHookDraw();
    TestLightDrawSkip();
    TestLegacyBrdf();
    TestDirectWriteMode();
    TestProxyWriteByReadMarksDirty();
    TestPreAoPairWithSkippedLightDraws();
    TestReservoirUpdate();
    TestReservoirMergeClamp();
    TestRisUnbiased();
    TestRemapLightIds();
    TestRestirTargetLegacy();
    TestFireLightFromDump();
    TestPyreTileGridLikeGpu();
    TestMoonHalf();
    std::printf("rt-direct-test: ok\n");
    return 0;
}

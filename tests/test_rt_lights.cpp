#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <set>
#include <vector>
#include "gpu/shadps4/video_core/renderer_vulkan/rt/rt_lights.h"

using namespace Vulkan::Rt;

namespace {

bool Near(float a, float b, float eps = 1e-4f) {
    return std::fabs(a - b) < eps;
}

void TestTable() {
    std::set<uint32_t> hashes;
    for (const LightShader& s : LightShaders) {
        assert(hashes.insert(s.ps_hash).second);
        assert(s.count >= 1 && s.count <= 12);
        assert(s.kind != LightKind::None);
    }
    const LightShader* p1 = FindLightShader(0x370174f7ull);
    assert(p1 && p1->kind == LightKind::Point && p1->count == 1 && !p1->shadowed);
    const LightShader* ps2 = FindLightShader(0xb5b52d63ull);
    assert(ps2 && ps2->kind == LightKind::Point && ps2->count == 2 && ps2->shadowed);
    const LightShader* dir = FindLightShader(0xd92266b9ull);
    assert(dir && dir->kind == LightKind::Directional && dir->shadowed);
    assert(FindLightShader(0x9a9cf8a9ull) == nullptr);
    assert(FindLightShader(0x1'00000000ull | 0x370174f7ull) == nullptr);
    assert(LightStride(*p1, 64) == 64);
    assert(LightStride(*ps2, 192) == 96);
    assert(LightStride(*ps2, 100) == 0);
    assert(LightStride(*ps2, 16) == 0);
    assert(LightStride(*p1, 144) == 64);
    assert(LightStride(*ps2, 288) == 96);
    assert(LightStride(*ps2, 176) == 0);
    const LightShader* l6 = FindLightShader(0x07d07cf3ull);
    assert(l6 && l6->legacy && LightStride(*l6, 864) == 64 && LightStride(*l6, 368) == 0);
    const LightShader* spot = FindLightShader(0xbae5454eull);
    assert(LightStride(*spot, 128) == 128 && LightDataStride(*spot) == 128);
    assert(LightDataStride(*dir) == 0 && LightStride(*dir, 576) == 0);
    assert(IsDirLightSetBuffer(*dir, 64800) && IsDirLightSetBuffer(*dir, 576));
    assert(!IsDirLightSetBuffer(*dir, 864) && !IsDirLightSetBuffer(*dir, 208) &&
           !IsDirLightSetBuffer(*p1, 576));
}

void TestLiveLegacyLayout() {
    const LightShader* l6 = FindLightShader(0x07d07cf3ull);
    std::vector<float> f(216, 0.0f);
    const float live[96] = {
        10.8f,    -0.4412f, -1.267f, 0.01f,    0.02677f, 0.2301f, 0.5f,    20.0f,   0.1511f, 0.2548f,
        0.3f,     0.229f,   1.0f,    1.0f,     0.0f,     0.0f,    0.03767f, -0.5301f, 4.564f, 0.01406f,
        2.347f,   1.317f,   0.5914f, 0.5f,     1.174f,   0.6584f, 0.2957f, 0.7709f, 1.0f,    1.0f,
        0.0f,     0.0f,     -9.5e-7f, 0.299f,  4.2f,     0.08163f, 0.4671f, 0.4671f, 0.4671f, 0.2f,
        0.07785f, 0.07785f, 0.07785f, 0.07785f, 1.0f,    1.0f,    0.0f,    0.0f,    0.6707f, 1.984f,
        7.727f,   0.02778f, 2.0f,    1.11f,    0.6028f,  20.0f,   1.0f,    0.5552f, 0.3014f, 0.6591f,
        1.0f,     1.0f,     0.0f,    0.0f,     -8.155f,  1.889f,  17.64f,  0.02778f, 0.05426f, 0.3359f,
        0.5f,     20.0f,    0.2015f, 0.3397f,  0.4f,     0.3053f, 1.0f,    1.0f,    0.0f,    0.0f,
        -10.47f,  2.289f,   14.84f,  0.04f,    0.05426f, 0.3359f, 0.5f,    20.0f,   0.2519f, 0.4246f,
        0.5f,     0.3816f,  1.0f,    1.0f,     0.0f,     0.0f};
    std::copy(std::begin(live), std::end(live), f.begin());
    std::vector<Light> out;
    assert(ParseLocalLights(*l6, f, 1, out) == 6);
    assert(Near(out[1].position[2], 4.564f) && Near(out[1].color[0], 2.347f));
    assert(Near(out[1].radius, 1.0f / std::sqrt(0.01406f), 1e-3f));
    assert(Near(out[5].position[0], -10.47f) && Near(out[5].radius, 5.0f, 1e-3f));

    LightDrawRecord rec{};
    rec.shader = l6;
    rec.param_bytes = LightAccParamBytes;
    const float camera[16] = {0.9571f, 0.0f, 0.2899f, -10.13f, 0.0f, 1.0f, 0.0f, -5.259f,
                              -0.2899f, 0.0f, 0.9571f, -22.24f, 0.0f, 0.0f, 0.0f, 1.0f};
    std::copy(std::begin(camera), std::end(camera), rec.param.begin() + CameraMatrixFloat);
    assert(ApplyCameraMatrix(rec, 1, out) == 6);
    assert(out[1].has_world);
    assert(Near(out[1].world[0], -8.771f, 2e-3f) && Near(out[1].world[1], -5.789f, 2e-3f) &&
           Near(out[1].world[2], -17.883f, 2e-3f));
    const Affine inv_view{0.9571f, 0.0f, 0.2899f, -10.13f, 0.0f, 1.0f, 0.0f, -5.259f,
                          -0.2899f, 0.0f, 0.9571f, -22.24f};
    const ShadowLightGpu gpu = PackShadowLight(out[1], InverseRigid(inv_view));
    assert(Near(gpu.pos_inv_r2[0], 0.03767f, 2e-3f) && Near(gpu.pos_inv_r2[1], -0.5301f, 2e-3f) &&
           Near(gpu.pos_inv_r2[2], 4.564f, 2e-3f) && Near(gpu.pos_inv_r2[3], 0.01406f, 1e-4f));
    assert(Near(gpu.color[1], 1.317f));
    assert(ApplyCameraMatrix(rec, 2, out) == 0);
    LightDrawRecord no_param{};
    no_param.shader = l6;
    assert(ApplyCameraMatrix(no_param, 1, out) == 0);
}

void TestSelectShadowLights() {
    std::vector<Light> lights;
    const auto add = [&](LightKind kind, bool shadowed, bool world, float radius, float lum,
                         uint32_t index) {
        Light l{};
        l.kind = kind;
        l.shadowed = shadowed;
        l.has_world = world;
        l.radius = radius;
        l.color = {lum, lum, lum};
        l.index = index;
        lights.push_back(l);
    };
    add(LightKind::Point, false, true, 2.0f, 1.0f, 0);
    add(LightKind::Point, true, true, 100.0f, 10.0f, 1);
    add(LightKind::Spot, false, true, 100.0f, 10.0f, 2);
    add(LightKind::Point, false, false, 100.0f, 10.0f, 3);
    add(LightKind::Directional, false, false, 1.0f, 1.0f, 4);
    add(LightKind::Point, false, true, 8.0f, 0.5f, 5);
    add(LightKind::Point, false, true, 1.0f, 3.0f, 6);
    add(LightKind::Point, false, true, 0.0f, 3.0f, 7);
    assert(Near(LightStrength(lights[5]), 4.0f) && Near(LightStrength(lights[6]), 3.0f));
    std::vector<Light> two = lights;
    SelectShadowLights(two, 2);
    assert(two.size() == 2 && two[0].index == 5 && two[1].index == 6);
    std::vector<Light> all = lights;
    SelectShadowLights(all, 8);
    assert(all.size() == 3 && all[2].index == 0);
    SelectShadowLights(all, 0);
    assert(all.empty());
}

void TestSelectShadowLightsDedup() {
    std::vector<Light> lights;
    const auto add = [&](std::array<float, 3> world, float radius, float lum, uint32_t draw) {
        Light l{};
        l.kind = LightKind::Point;
        l.has_world = true;
        l.world = world;
        l.radius = radius;
        l.color = {lum, lum * 0.8f, lum * 0.5f};
        l.draw = draw;
        lights.push_back(l);
    };
    for (uint32_t d = 0; d < 80; ++d) {
        const float noise = float(d % 3) * 1e-4f;
        add({10.800f + noise, -0.441f, -1.267f - noise}, 12.0f, 4.0f, d);
    }
    for (uint32_t d = 0; d < 64; ++d) {
        add({0.024f, -0.512f, 4.568f}, 8.0f, 3.0f, 100 + d);
    }
    add({10.800f, -0.441f, -0.267f}, 12.0f, 4.0f, 300);
    add({2.0f, 0.0f, 0.0f}, 4.0f, 1.0f, 301);
    add({3.0f, 0.0f, 0.0f}, 4.0f, 0.5f, 302);
    std::vector<Light> four = lights;
    SelectShadowLights(four, 4);
    assert(four.size() == 4);
    assert(four[0].draw < 80 && four[1].draw == 300);
    assert(four[2].draw >= 100 && four[2].draw < 164 && four[3].draw == 301);
    std::vector<Light> all = lights;
    SelectShadowLights(all, 8);
    assert(all.size() == 5 && all[4].draw == 302);
    for (size_t i = 0; i < all.size(); ++i) {
        for (size_t k = i + 1; k < all.size(); ++k) {
            assert(!SameLight(all[i], all[k]));
        }
    }
}

std::vector<float> PointBuffer(uint32_t count, uint32_t stride_floats) {
    std::vector<float> f(count * stride_floats, 0.0f);
    for (uint32_t i = 0; i < count; ++i) {
        float* l = f.data() + i * stride_floats;
        l[0] = 1.0f + float(i);
        l[1] = 2.0f;
        l[2] = -3.0f;
        l[3] = 1.0f / ((4.0f + float(i)) * (4.0f + float(i)));
        l[4] = 0.5f * float(i + 1);
        l[5] = 0.25f;
        l[6] = 0.125f;
    }
    return f;
}

void TestParsePoint() {
    const LightShader* p3 = FindLightShader(0x89f9ee3eull);
    assert(p3);
    std::vector<Light> out;
    const auto buf = PointBuffer(3, 16);
    assert(ParseLocalLights(*p3, buf, 7, out) == 3);
    assert(out.size() == 3);
    assert(Near(out[1].position[0], 2.0f) && Near(out[1].position[2], -3.0f));
    assert(Near(out[1].radius, 5.0f));
    assert(Near(out[2].color[0], 1.5f));
    assert(out[2].draw == 7 && out[2].index == 2 && out[2].kind == LightKind::Point);
    const LightShader* s3 = FindLightShader(0xbb2ac1cbull);
    out.clear();
    const auto shadowed = PointBuffer(3, 24);
    assert(ParseLocalLights(*s3, shadowed, 1, out) == 3);
    assert(out[0].shadowed && Near(out[0].radius, 4.0f));
    out.clear();
    assert(ParseLocalLights(*p3, PointBuffer(2, 16), 0, out) == 0);
}

void TestParseRejects() {
    const LightShader* p2 = FindLightShader(0x600f86b0ull);
    auto buf = PointBuffer(2, 16);
    buf[0] = std::numeric_limits<float>::quiet_NaN();
    buf[16 + 3] = 0.0f;
    std::vector<Light> out;
    assert(ParseLocalLights(*p2, buf, 0, out) == 0);
    buf[0] = 1.0f;
    buf[16 + 3] = -1.0f;
    assert(ParseLocalLights(*p2, buf, 0, out) == 1 && out[0].index == 0);
    const LightShader* dir = FindLightShader(0x31b90cc5ull);
    assert(dir && ParseLocalLights(*dir, buf, 0, out) == 0);
}

void TestParseSpot() {
    const LightShader* spot = FindLightShader(0xbae5454eull);
    std::vector<float> f(32, 0.0f);
    f[0] = 10.0f;
    f[1] = 0.0f;
    f[2] = 5.0f;
    f[3] = 12.0f;
    f[4] = 1.0f;
    f[5] = 0.9f;
    f[6] = 0.8f;
    std::vector<Light> out;
    assert(ParseLocalLights(*spot, f, 3, out) == 1);
    assert(out[0].kind == LightKind::Spot && Near(out[0].radius, 12.0f));
    assert(Near(out[0].position[2], 5.0f) && Near(out[0].color[1], 0.9f));
}

void TestParseDirectional() {
    std::vector<float> f(144, 0.0f);
    f[0] = 0.0f;
    f[1] = 2.0f;
    f[2] = 0.0f;
    f[4] = 3.0f;
    f[5] = 2.0f;
    f[6] = 1.0f;
    f[72] = 1.0f;
    f[76] = 0.1f;
    Light l{};
    assert(ParseDirectionalLight(f, 0, 0, true, l));
    assert(l.kind == LightKind::Directional && l.shadowed);
    assert(Near(l.direction[1], 1.0f) && Near(l.color[0], 3.0f));
    assert(ParseDirectionalLight(f, 1, 0, false, l));
    assert(Near(l.direction[0], 1.0f) && Near(l.color[0], 0.1f) && l.index == 1);
    f[0] = f[1] = f[2] = 0.0f;
    assert(!ParseDirectionalLight(f, 0, 0, false, l));
    f[1] = std::numeric_limits<float>::infinity();
    assert(!ParseDirectionalLight(f, 0, 0, false, l));
    assert(!ParseDirectionalLight(std::vector<float>(4, 1.0f), 0, 0, false, l));
    f[1] = 1.0f;
    f[4] = f[5] = f[6] = 0.0f;
    assert(!ParseDirectionalLight(f, 0, 0, false, l));

    std::vector<float> live(64800 / 4, 0.0f);
    for (uint32_t set : {0u, 3u, 224u}) {
        float* s = live.data() + set * (DirLightSetStride / 4);
        s[0] = 0.0f;
        s[1] = -2.0f;
        s[2] = 2.0f;
        s[4] = 0.5f + float(set);
        s[5] = 0.5f;
        s[6] = 0.5f;
    }
    std::vector<Light> out;
    assert(ParseDirectionalSets(live, 9, true, out) == 3);
    assert(out[1].index == 3 && Near(out[1].color[0], 3.5f) && out[1].shadowed && out[1].draw == 9);
    assert(Near(out[2].direction[1], -std::sqrt(0.5f)) && out[2].index == 224);
    for (uint32_t set = 10; set < 30; ++set) {
        live[set * (DirLightSetStride / 4) + 2] = 1.0f;
        live[set * (DirLightSetStride / 4) + 4] = 1.0f;
    }
    out.clear();
    assert(ParseDirectionalSets(live, 0, false, out) == MaxDirLightSets);
}

void TestFillVis() {
    Light l{};
    l.kind = LightKind::Point;
    l.has_world = true;
    l.radius = 5.0f;
    l.world = {10.05f, -3.1f, 200.0f};
    assert(FillVisCandidate(l));
    Light same = l;
    same.world = {10.1f, -3.05f, 200.08f}; // same 0.25 m cell
    assert(FillVisKey(same) == FillVisKey(l));
    Light moved = l;
    moved.world[0] += 0.3f;
    assert(FillVisKey(moved) != FillVisKey(l));
    Light spot = l;
    spot.kind = LightKind::Spot;
    assert(FillVisKey(spot) != FillVisKey(l));
    Light fixed = l;
    fixed.camera_fixed = true;
    assert(!FillVisCandidate(fixed));
    Light dir = l;
    dir.kind = LightKind::Directional;
    assert(!FillVisCandidate(dir));
    // Closed room: off at floor 0; emitter: full; open sky past sky_full: full.
    assert(Near(FillVisScale(0.0f, false, 0.12f, 0.0f), 0.0f));
    assert(Near(FillVisScale(0.0f, true, 0.12f, 0.0f), 1.0f));
    assert(Near(FillVisScale(0.3f, false, 0.12f, 0.0f), 1.0f));
    assert(Near(FillVisScale(0.06f, false, 0.12f, 0.0f), 0.5f));
    assert(Near(FillVisScale(0.0f, false, 0.12f, 0.2f), 0.2f));
    assert(Near(FillVisScale(0.06f, false, 0.12f, 0.2f), 0.6f));
    assert(FillVisScale(0.02f, false, 0.12f, 0.0f) < FillVisScale(0.04f, false, 0.12f, 0.0f));
    // Emitter evidence: any one sign counts. A bare fill light in a room (a wall 0.4 m away
    // covers ~17% of rays within 0.6 m) is not an emitter; a lantern housing is.
    FillVisResult fill{};
    fill.enclosed = 0.17f;
    assert(!FillVisEmitter(fill, 0.02f, 0.2f));
    FillVisResult lantern = fill;
    lantern.enclosed = 0.9f;
    assert(FillVisEmitter(lantern, 0.02f, 0.2f));
    // Chandelier candle lights (radius 2, nearest solid 0.02 m) measure 26-34% enclosed.
    FillVisResult candle = fill;
    candle.enclosed = 0.26f;
    assert(FillVisEmitter(candle, 0.02f, 0.2f));
    FillVisResult glow = fill;
    glow.screen_luma = 0.5f;
    assert(FillVisEmitter(glow, 0.02f, 0.2f));
    FillVisResult hit = fill;
    hit.emit = 0.01f;
    assert(FillVisEmitter(hit, 0.02f, 0.2f));
    assert(!FillVisEmitter(lantern, 0.02f, 1.01f)); // BB_RT_FILL_ENCLOSED above 1 disables it
    // Lamp: enclosed all round and capped above. Forest fire pits (play179) sit in their log pile
    // (nearest solid 0.03 m, 56-58% enclosed) but open upwards (24-29% of the up rays).
    FillVisResult capped = fill;
    capped.enclosed = 0.65f;
    capped.enclosed_up = 0.8f;
    assert(FillVisLamp(capped, 0.4f, 0.5f));
    FillVisResult fire_pit = fill;
    fire_pit.enclosed = 0.578f;
    fire_pit.enclosed_up = 0.273f;
    assert(!FillVisLamp(fire_pit, 0.4f, 0.5f));
    assert(FillVisLamp(fire_pit, 0.4f, 0.0f)); // BB_RT_LAMP_UP 0: the old enclosure-only rule
    assert(!FillVisLamp(capped, 0.0f, 0.5f));  // BB_RT_LAMP_ENCLOSED 0: no lamps
    assert(!FillVisLamp(fill, 0.4f, 0.5f));
    assert(FillVisFirePit(fire_pit, 0.4f, 0.5f));
    assert(!FillVisFirePit(capped, 0.4f, 0.5f));
    assert(!FillVisFirePit(fill, 0.4f, 0.5f)); // a bare fill light is neither
    assert(!FillVisFirePit(fire_pit, 0.0f, 0.5f));
}

void TestSelectStrongest() {
    std::vector<Light> lights;
    for (uint32_t i = 0; i < 6; ++i) {
        Light l{};
        l.kind = LightKind::Point;
        l.radius = 1.0f + float(i);
        l.color = {1.0f, 1.0f, 1.0f};
        l.index = i;
        lights.push_back(l);
    }
    Light dir{};
    dir.kind = LightKind::Directional;
    dir.color = {0.01f, 0.01f, 0.01f};
    dir.radius = 1.0f;
    lights.push_back(dir);
    SelectStrongest(lights, 2);
    assert(lights.size() == 3);
    assert(lights[0].kind == LightKind::Directional);
    assert(lights[1].index == 5 && lights[2].index == 4);
    SelectStrongest(lights, 0);
    assert(lights.size() == 1 && lights[0].kind == LightKind::Directional);
}

struct RingBuffer {
    uint32_t size;
    std::vector<float> data;
};

std::vector<RingBuffer> LightDrawRing(const LightShader& shader) {
    std::vector<float> param(LightAccParamBytes / 4, 0.0f);
    param[0] = 1.0f;
    std::vector<RingBuffer> ring;
    ring.push_back({LightAccParamBytes, param});
    if (shader.kind == LightKind::Directional) {
        std::vector<float> dir(DirLightSetBytes / 4, 0.0f);
        dir[2] = -1.0f;
        dir[4] = 2.0f;
        dir[5] = 2.0f;
        dir[6] = 2.0f;
        ring.push_back({DirLightSetBytes, dir});
    } else {
        const uint32_t stride = shader.shadowed ? 24 : 16;
        std::vector<float> lights(size_t(stride) * shader.count, 0.0f);
        for (uint32_t i = 0; i < shader.count; ++i) {
            float* f = lights.data() + i * stride;
            f[0] = float(i);
            f[3] = 0.25f;
            f[4] = 1.0f;
        }
        ring.push_back({uint32_t(lights.size() * 4), lights});
    }
    ring.push_back({864, std::vector<float>(216, 0.5f)});
    return ring;
}

void TestRecordFromRing() {
    const LightShader* p2 = FindLightShader(0x600f86b0ull);
    LightDrawRecord rec{};
    rec.shader = p2;
    std::vector<Light> out;
    std::vector<LightBufferRole> roles;
    for (const RingBuffer& b : LightDrawRing(*p2)) {
        roles.push_back(RecordLightBuffer(rec, 4, b.size, b.data.data(), out));
    }
    assert(roles.size() == 3 && roles[0] == LightBufferRole::Param &&
           roles[1] == LightBufferRole::Local && roles[2] == LightBufferRole::Other);
    assert(rec.param_bytes == 208 && rec.light_bytes == 128 && rec.dir_bytes == 0);
    assert(rec.others == 1 && rec.other_bytes[0] == 864);
    assert(Near(rec.param[0], 1.0f));
    assert(out.size() == 2 && Near(out[1].position[0], 1.0f) && Near(out[1].radius, 2.0f));
    assert(out[1].draw == 4);

    const LightShader* dir = FindLightShader(0xd92266b9ull);
    LightDrawRecord drec{};
    drec.shader = dir;
    out.clear();
    for (const RingBuffer& b : LightDrawRing(*dir)) {
        RecordLightBuffer(drec, 0, b.size, b.data.data(), out);
    }
    assert(drec.param_bytes == 208 && drec.dir_bytes == 576 && drec.light_bytes == 0);
    assert(drec.others == 1 && drec.other_bytes[0] == 864);
    assert(out.size() == 1 && out[0].kind == LightKind::Directional && out[0].shadowed);
    assert(Near(out[0].direction[2], -1.0f));

    LightDrawRecord live{};
    live.shader = dir;
    out.clear();
    std::vector<float> sets(64800 / 4, 0.0f);
    sets[0] = 1.0f;
    sets[4] = 1.0f;
    const std::vector<float> scene(216, 0.5f);
    roles.clear();
    roles.push_back(RecordLightBuffer(live, 0, 208, LightDrawRing(*dir)[0].data.data(), out));
    roles.push_back(RecordLightBuffer(live, 0, 864, scene.data(), out));
    roles.push_back(RecordLightBuffer(live, 0, 64800, sets.data(), out));
    assert(roles[0] == LightBufferRole::Param && roles[1] == LightBufferRole::Other &&
           roles[2] == LightBufferRole::Dir);
    assert(live.dir_bytes == 64800 && live.dir.size() == 16200 && live.others == 1);
    assert(out.size() == 1 && Near(out[0].direction[0], 1.0f) && out[0].index == 0);

    const LightShader* l4 = FindLightShader(0x3105494cull);
    LightDrawRecord lrec{};
    lrec.shader = l4;
    out.clear();
    std::vector<float> packed(576 / 4, 0.0f);
    for (uint32_t i = 0; i < 4; ++i) {
        packed[i * 16 + 2] = 1.0f + float(i);
        packed[i * 16 + 3] = 0.04f;
        packed[i * 16 + 5] = 1.0f;
    }
    assert(RecordLightBuffer(lrec, 3, 576, packed.data(), out) == LightBufferRole::Local);
    assert(out.size() == 4 && Near(out[3].position[2], 4.0f) && Near(out[3].radius, 5.0f) &&
           out[3].draw == 3);

    LightDrawRecord none{};
    assert(RecordLightBuffer(none, 0, 208, LightDrawRing(*p2)[0].data.data(), out) ==
           LightBufferRole::Rejected);
    assert(RecordLightBuffer(rec, 0, 8, rec.param.data(), out) == LightBufferRole::Rejected);
}

}

std::vector<Light> FlickerFrame(uint32_t frame) {
    std::vector<Light> lights;
    for (uint32_t i = 0; i < 6; ++i) {
        const uint32_t h = (frame * 2654435761u) ^ (i * 40503u + 17u);
        const float flicker = 1.0f + (float((h >> 8) % 1000u) / 1000.0f - 0.5f) * 0.06f;
        Light l{};
        l.kind = LightKind::Point;
        l.has_world = true;
        l.position = {float(i) * 0.5f, 0.0f, 2.0f};
        l.world = {10.0f + float(i) * 2.0f, 0.0f, 5.0f};
        l.radius = 6.0f;
        const float lum = 1.0f + float(i) * 0.005f;
        l.color = {lum * flicker, lum * flicker, lum * flicker};
        l.index = i;
        lights.push_back(l);
    }
    for (uint32_t i = uint32_t(lights.size()); i > 1; --i) {
        const uint32_t j = ((frame + 7u) * 2246822519u + i * 3266489917u) % i;
        std::swap(lights[i - 1], lights[j]);
    }
    return lights;
}

uint32_t SelectionMask(const std::vector<Light>& lights) {
    uint32_t mask = 0;
    for (const Light& l : lights) {
        mask |= 1u << l.index;
    }
    return mask;
}

void TestShadowSelectionStable() {
    uint32_t old_changes = 0, new_changes = 0, old_mask = 0, new_mask = 0;
    std::vector<ShadowTrack> tracks;
    for (uint32_t f = 0; f < 120; ++f) {
        std::vector<Light> old_sel = FlickerFrame(f);
        SelectShadowLights(old_sel, 4);
        const uint32_t om = SelectionMask(old_sel);
        old_changes += f > 0 && om != old_mask ? 1 : 0;
        old_mask = om;
        UpdateShadowTracks(FlickerFrame(f), 4, tracks);
        std::vector<Light> selected;
        for (const ShadowTrack& t : tracks) {
            if (t.selected && t.missing == 0) {
                selected.push_back(t.light);
            }
        }
        assert(selected.size() == 4);
        const uint32_t nm = SelectionMask(selected);
        new_changes += f > 0 && nm != new_mask ? 1 : 0;
        new_mask = nm;
    }
    std::printf("shadow selection changes over 120 flickering shuffled frames: old %u, new %u\n",
                old_changes, new_changes);
    assert(old_changes > 0);
    assert(new_changes == 0);
}

void TestShadowFade() {
    std::vector<ShadowTrack> tracks;
    std::vector<Light> drawn;
    const auto light = [](uint32_t index, float x, float lum, float dist) {
        Light l{};
        l.kind = LightKind::Point;
        l.has_world = true;
        l.position = {0.0f, 0.0f, dist};
        l.world = {x, 0.0f, 0.0f};
        l.radius = 5.0f;
        l.color = {lum, lum, lum};
        l.index = index;
        return l;
    };
    const std::vector<Light> base{light(0, 0.0f, 1.0f, 2.0f), light(1, 3.0f, 0.9f, 2.0f)};
    for (uint32_t f = 0; f < 12; ++f) {
        UpdateShadowTracks(base, 1, tracks);
        ShadowDrawLights(tracks, drawn);
    }
    assert(drawn.size() == 1 && drawn[0].index == 0 && Near(drawn[0].color[0], 1.0f));
    std::vector<Light> with_strong = base;
    with_strong.push_back(light(2, 6.0f, 4.0f, 2.0f));
    UpdateShadowTracks(with_strong, 1, tracks);
    ShadowDrawLights(tracks, drawn);
    assert(drawn.size() == 2 && drawn[0].index == 2 && Near(drawn[0].color[0], 0.4f));
    assert(drawn[1].index == 0 && Near(drawn[1].color[0], 0.9f));
    for (uint32_t f = 0; f < 9; ++f) {
        UpdateShadowTracks(with_strong, 1, tracks);
        ShadowDrawLights(tracks, drawn);
        assert(drawn.size() <= MaxShadowLights);
        for (size_t i = 0; i < drawn.size(); ++i) {
            for (size_t k = i + 1; k < drawn.size(); ++k) {
                assert(!SameSource(drawn[i], drawn[k]));
            }
        }
    }
    assert(drawn.size() == 1 && drawn[0].index == 2 && Near(drawn[0].color[0], 4.0f));
    UpdateShadowTracks({}, 1, tracks);
    ShadowDrawLights(tracks, drawn);
    assert(drawn.empty() && tracks.size() == 1);
    UpdateShadowTracks(with_strong, 1, tracks);
    ShadowDrawLights(tracks, drawn);
    assert(drawn.size() == 1 && drawn[0].index == 2 && Near(drawn[0].color[0], 4.0f));
    std::vector<Light> far_strong = base;
    far_strong.push_back(light(3, 9.0f, 3.0f, 40.0f));
    std::vector<ShadowTrack> fresh;
    UpdateShadowTracks(far_strong, 1, fresh);
    assert(fresh.size() == 1 && fresh[0].light.index == 0);
}

void TestDedupBeforeParse() {
    const LightShader* p3 = FindLightShader(0x89f9ee3eull);
    const LightShader* p2 = FindLightShader(0x600f86b0ull);
    assert(p3 && p2);
    LightDedup dedup;
    std::vector<Light> out;
    const auto a = PointBuffer(3, 16);
    LightDrawRecord r0{};
    r0.shader = p3;
    assert(RecordLightBuffer(r0, 0, 192, a.data(), out, &dedup) == LightBufferRole::Local);
    assert(out.size() == 3 && dedup.skipped == 0);

    LightDrawRecord r1{};
    r1.shader = p3;
    assert(RecordLightBuffer(r1, 1, 192, a.data(), out, &dedup) == LightBufferRole::Local);
    assert(out.size() == 3 && dedup.skipped == 3 && r1.light_bytes == 192);
    const std::vector<float> scene(216, 0.5f);
    assert(RecordLightBuffer(r1, 1, 864, scene.data(), out, &dedup) == LightBufferRole::Other);

    std::vector<float> b(32, 0.0f);
    std::copy(a.begin() + 16, a.begin() + 32, b.begin());
    b[16] = 9.0f;
    b[19] = 0.04f;
    b[20] = 1.0f;
    LightDrawRecord r2{};
    r2.shader = p2;
    assert(RecordLightBuffer(r2, 2, 128, b.data(), out, &dedup) == LightBufferRole::Local);
    assert(out.size() == 4 && dedup.skipped == 4);
    assert(out[3].draw == 2 && out[3].index == 1 && Near(out[3].position[0], 9.0f));
    for (uint32_t i = 0; i < 3; ++i) {
        assert(out[i].draw == 0 && out[i].index == i);
    }

    std::vector<Light> plain;
    LightDrawRecord q0{}, q1{};
    q0.shader = p3;
    q1.shader = p3;
    RecordLightBuffer(q0, 0, 192, a.data(), plain);
    RecordLightBuffer(q1, 1, 192, a.data(), plain);
    assert(plain.size() == 6);

    const LightShader* dir = FindLightShader(0xd92266b9ull);
    const auto ring = LightDrawRing(*dir);
    LightDrawRecord d0{}, d1{};
    d0.shader = dir;
    d1.shader = dir;
    out.clear();
    for (const RingBuffer& rb : ring) {
        RecordLightBuffer(d0, 0, rb.size, rb.data.data(), out, &dedup);
    }
    assert(d0.dir_bytes == 576 && out.size() == 1);
    const uint64_t before = dedup.skipped;
    for (const RingBuffer& rb : ring) {
        RecordLightBuffer(d1, 1, rb.size, rb.data.data(), out, &dedup);
    }
    assert(d1.dir_bytes == 0 && d1.dir_duplicate && d1.dir.empty() && out.size() == 1);
    assert(d1.others == 1 && dedup.skipped == before + 1);

    ResetLightDrawRecord(d1, p3);
    assert(d1.shader == p3 && d1.param_bytes == 0 && d1.light_bytes == 0 && d1.dir_bytes == 0 &&
           d1.others == 0 && !d1.dir_duplicate);

    dedup.Clear();
    out.clear();
    LightDrawRecord r3{};
    r3.shader = p3;
    RecordLightBuffer(r3, 0, 192, a.data(), out, &dedup);
    assert(out.size() == 3);

    assert(HashLightBytes(a.data(), 64, 1) == HashLightBytes(a.data(), 64, 1));
    assert(HashLightBytes(a.data(), 64, 1) != HashLightBytes(a.data(), 64, 2));
    assert(HashLightBytes(a.data(), 64, 1) != HashLightBytes(a.data() + 16, 64, 1));
}

void TestDrawStencil() {
    assert(PackLightStencil(false, 2, 0x40, 0xC0) == 0);
    const uint32_t s = PackLightStencil(true, 2, 0x40, 0xC0);
    assert((s & LightStencilEnable) != 0 && ((s >> 16) & 7u) == 2 && (s & 0xFFu) == 0x40 &&
           ((s >> 8) & 0xFFu) == 0xC0);
    const LightShader* p3 = FindLightShader(0x89f9ee3eull);
    std::vector<Light> out;
    assert(ParseLocalLights(*p3, PointBuffer(3, 16), 4, out) == 3);
    assert(out[0].shader_name && std::string_view(out[0].shader_name) == "DPointLight3");
    LightDrawRecord rec{};
    rec.stencil = s;
    out[1].draw = 5;
    assert(ApplyDrawStencil(rec, 4, out) == 2);
    assert(out[0].stencil == s && out[1].stencil == 0 && out[2].stencil == s);
}

void TestStockGate() {
    LightDrawState main_state{};
    main_state.valid = true;
    main_state.diffuse = 7;
    main_state.specular = 8;
    main_state.mask0 = 0xF;
    main_state.mask1 = 0xF;
    main_state.blend0 = main_state.blend1 = true;
    main_state.src0 = main_state.dst0 = main_state.src1 = main_state.dst1 = 1;
    main_state.depth = true;
    main_state.zfunc = 3;
    std::vector<LightDrawRecord> draws(4);
    for (auto& d : draws) {
        d.state = main_state;
    }
    draws[3].state.diffuse = 9;
    draws[3].state.specular = 10;
    const LightTargets main = MainLightTargets(draws);
    assert(main.diffuse == 7 && main.specular == 8);
    {
        // A narrower capture pass with more light draws must not take the scene's targets.
        std::vector<LightDrawRecord> two(5);
        for (auto& d : two) {
            d.state = main_state;
            d.state.diffuse = 20;
            d.state.specular = 21;
            d.state.width = 512;
        }
        two[0].state = main_state;
        two[0].state.width = 2560;
        const LightTargets wide = MainLightTargets(two);
        assert(wide.diffuse == 7 && wide.specular == 8 && wide.width == 2560);
    }
    assert(!StockLightGate(main_state, main).reason);
    assert(!StockLightGate(LightDrawState{}, main).reason);
    LightGate g = StockLightGate(draws[3].state, main);
    assert(g.diffuse == 0.0f && g.specular == 0.0f && g.reason);
    LightDrawState spec_only = main_state;
    spec_only.mask0 = 0;
    g = StockLightGate(spec_only, main);
    assert(g.diffuse == 0.0f && g.specular == 1.0f &&
           std::string_view(g.reason) == "diffuse colour mask 0");
    LightDrawState zero_src = main_state;
    zero_src.src1 = 0;
    g = StockLightGate(zero_src, main);
    assert(g.diffuse == 1.0f && g.specular == 0.0f);
    LightDrawState max_blend = main_state;
    max_blend.func0 = 3;
    assert(!StockLightGate(max_blend, main).reason);
    LightDrawState sub_blend = main_state;
    sub_blend.func0 = 1;
    assert(StockLightGate(sub_blend, main).diffuse == 0.0f);
    {
        const LightShader* p1 = FindLightShader(0x7cc2c044ull);
        LightDedup dedup;
        std::vector<Light> parsed;
        std::vector<float> buf(16, 0.0f);
        buf[2] = 4.2f;
        buf[3] = 1.0f / 12.25f;
        buf[4] = buf[5] = buf[6] = 0.467f;
        const std::span<const float> span(buf.data(), buf.size());
        assert(ParseLocalLights(*p1, span, 0, parsed, &dedup, LightStateSalt(draws[3].state)) == 1);
        assert(ParseLocalLights(*p1, span, 1, parsed, &dedup, LightStateSalt(main_state)) == 1);
        assert(ParseLocalLights(*p1, span, 2, parsed, &dedup, LightStateSalt(main_state)) == 0);
    }
    LightDrawState never = main_state;
    never.zfunc = 0;
    assert(StockLightGate(never, main).diffuse == 0.0f);
    std::vector<Light> lights(3);
    for (auto& l : lights) {
        l.kind = LightKind::Point;
        l.color = {1.0f, 1.0f, 1.0f};
        l.spec = {1.0f, 1.0f, 1.0f};
        l.state = main_state;
    }
    lights[1].state = spec_only;
    lights[2].state = draws[3].state;
    std::vector<Light> removed;
    assert(ApplyStockGate(lights, main, true, false, &removed) == 2);
    assert(lights.size() == 2 && removed.size() == 1);
    assert(lights[0].color[0] == 1.0f && !lights[0].gate);
    assert(lights[1].color[0] == 0.0f && lights[1].spec[0] == 1.0f && lights[1].gate);
    std::vector<Light> off = lights;
    assert(ApplyStockGate(off, main, false, false) == 0 && off.size() == 2);
    off[0].camera_fixed = true;
    removed.clear();
    assert(ApplyStockGate(off, main, false, true, &removed) == 2 && off.empty());
    assert(std::string_view(removed[0].gate) == FillLightOffReason);
}

void TestCameraFixed() {
    std::vector<FixedLightTrack> tracks;
    for (uint32_t f = 0; f < 6; ++f) {
        std::vector<Light> lights(2);
        for (auto& l : lights) {
            l.kind = LightKind::Point;
            l.has_world = true;
            l.radius = 3.5f;
            l.color = {0.467f, 0.467f, 0.467f};
        }
        lights[0].position = {0.0f, 0.33f, 4.2f};
        lights[0].world = {float(f), 1.0f, 4.0f};
        lights[1].position = {5.0f - float(f), 1.0f, 8.0f};
        lights[1].world = {10.0f, 1.0f, 8.0f};
        MarkCameraFixed(lights, tracks);
        assert(!lights[1].camera_fixed);
        assert(lights[0].camera_fixed == (f >= FixedMovedFrames));
    }
    std::vector<Light> still(1);
    still[0].kind = LightKind::Point;
    still[0].has_world = true;
    still[0].radius = 3.5f;
    still[0].color = {0.467f, 0.467f, 0.467f};
    still[0].position = {0.0f, 0.33f, 4.2f};
    still[0].world = {5.0f, 1.0f, 4.0f};
    MarkCameraFixed(still, tracks);
    assert(still[0].camera_fixed);
}

Light LiveLight(std::array<float, 3> view, float radius, std::array<float, 3> color,
                std::array<float, 3> spec, bool shadowed) {
    Light l{};
    l.kind = LightKind::Point;
    l.legacy = true;
    l.shadowed = shadowed;
    l.has_world = true;
    l.position = view;
    l.world = view;
    l.radius = radius;
    l.color = color;
    l.spec = spec;
    return l;
}

void TestPlayerLightSignature() {
    const std::array<float, 3> grey{0.467f, 0.467f, 0.467f};
    const std::array<float, 3> grey_spec{0.078f, 0.078f, 0.078f};
    std::vector<Light> lights{
        LiveLight({0.00f, 0.31f, 3.15f}, 3.5f, grey, grey_spec, false),
        LiveLight({0.00f, 0.33f, 4.20f}, 3.5f, grey, grey_spec, false),
        LiveLight({-0.71f, 0.28f, 3.31f}, 3.5f, grey, grey_spec, false),
        LiveLight({1.80f, 1.81f, -0.57f}, 2.0f, {0.6f, 0.4f, 0.2f}, {1.5f, 1.03f, 0.54f}, false),
        LiveLight({0.04f, -0.53f, 4.56f}, 8.4f, {2.35f, 1.32f, 0.59f}, {0.5f, 0.3f, 0.1f}, false),
        LiveLight({-2.01f, 9.35f, 1.80f}, 16.0f, {0.2f, 0.134f, 0.086f}, {0.3f, 0.2f, 0.13f}, true),
        LiveLight({0.10f, 0.20f, 3.0f}, 3.5f, grey, grey_spec, true),
    };
    std::vector<FixedLightTrack> tracks;
    const CameraFixedCount c = MarkCameraFixed(lights, tracks);
    assert(lights[0].camera_fixed && lights[1].camera_fixed && lights[2].camera_fixed);
    for (size_t i = 3; i < lights.size(); ++i) {
        assert(!lights[i].camera_fixed);
    }
    assert(c.total == 3 && c.signature == 3 && c.tracked == 0);
    std::vector<Light> dungeon{
        LiveLight({0.04f, 1.40f, 6.15f}, 8.75f, {0.187f, 0.187f, 0.187f}, {0.031f, 0.031f, 0.031f}, false),
        LiveLight({-0.33f, 2.79f, 1.83f}, 8.75f, {0.187f, 0.187f, 0.187f}, {0.031f, 0.031f, 0.031f}, false),
        LiveLight({0.472f, 4.007f, 4.184f}, 3.5f, grey, grey_spec, false),
    };
    std::vector<FixedLightTrack> dungeon_tracks;
    MarkCameraFixed(dungeon, dungeon_tracks);
    assert(dungeon[0].camera_fixed && dungeon[1].camera_fixed && dungeon[2].camera_fixed);
    LightDrawState main{};
    main.valid = true;
    main.diffuse = 1;
    main.specular = 2;
    main.mask0 = main.mask1 = 0xF;
    for (auto& l : lights) {
        l.state = main;
    }
    std::vector<Light> removed;
    ApplyStockGate(lights, {1, 2}, true, true, &removed);
    assert(removed.size() == 3 && lights.size() == 4);
}

void TestStockDepthTest() {
    LightDrawState s{};
    s.valid = true;
    assert(!LightDepthTest(s).active);
    s.depth = true;
    s.zfunc = 3;
    assert(!LightDepthTest(s).active);
    s.has_vtx = true;
    s.vtx_z = 0.4f;
    s.zscale = 0.5f;
    s.zoffset = 0.5f;
    StockDepthTest t = LightDepthTest(s);
    assert(t.active && t.func == 3 && std::fabs(t.quad - 0.7f) < 1e-6f);
    assert(StockDepthPasses(t, 0.8f) && StockDepthPasses(t, 0.71f) && !StockDepthPasses(t, 0.6f));
    s.bounds = true;
    s.bounds_min = 0.75f;
    s.bounds_max = 0.9f;
    t = LightDepthTest(s);
    assert(StockDepthPasses(t, 0.8f) && !StockDepthPasses(t, 0.72f) && !StockDepthPasses(t, 0.95f));
    s.depth = false;
    t = LightDepthTest(s);
    assert(t.active && t.func == CompareAlways && StockDepthPasses(t, 0.1f) == false &&
           StockDepthPasses(t, 0.8f));
    s.zfunc = 0;
    s.depth = true;
    s.bounds = false;
    assert(!StockDepthPasses(LightDepthTest(s), 0.5f));
}


int main() {
    TestStockGate();
    TestCameraFixed();
    TestPlayerLightSignature();
    TestStockDepthTest();
    TestDrawStencil();
    TestDedupBeforeParse();
    TestRecordFromRing();
    TestTable();
    TestParsePoint();
    TestParseRejects();
    TestParseSpot();
    TestParseDirectional();
    TestSelectStrongest();
    TestFillVis();
    TestLiveLegacyLayout();
    TestSelectShadowLights();
    TestSelectShadowLightsDedup();
    TestShadowSelectionStable();
    TestShadowFade();
    std::printf("rt-lights-test: ok\n");
    return 0;
}

#include <cassert>
#include <cmath>
#include <cstdio>
#include <unordered_map>

#include "gpu/shadps4/video_core/renderer_vulkan/rt/rt_surface.h"

using namespace Vulkan::Rt;

static void TestHalf() {
    for (const float v : {0.0f, 1.0f, -1.0f, 0.5f, 0.25f, 3.1415f, 17.0f, 1024.5f, 1e-5f,
                          -0.125f, 65504.0f}) {
        const float back = HalfToFloat(FloatToHalf(v));
        assert(std::fabs(back - v) <= std::fabs(v) * 1e-3f + 1e-7f);
    }
    assert(std::isinf(HalfToFloat(FloatToHalf(1e9f))));
    assert(std::isnan(HalfToFloat(FloatToHalf(std::nanf("")))));
    assert(FloatToHalf(1.0f) == 0x3C00);
    assert(FloatToHalf(-2.0f) == 0xC000);
    assert(HalfToFloat(0x0001) > 0.0f);
}

static void TestPack() {
    const uint32_t packed = PackUv(0.75f, -1.5f);
    const auto uv = UnpackUv(packed);
    assert(uv[0] == 0.75f && uv[1] == -1.5f);
    assert((packed & 0xFFFF) == FloatToHalf(0.75f));
    assert((packed >> 16) == FloatToHalf(-1.5f));
}

static void TestSelect() {
    assert(UvSelect(-1, 0, 1) == 0);
    assert(UvSelect(32, 0, 1) == 0);
    assert(UvSelect(0, 4, 1) == 0);
    const uint32_t s = UvSelect(3, 2, 1);
    assert(UvSelectIsValid(s));
    assert(UvSelectParam(s) == 3);
    assert(UvSelectComponents(s)[0] == 2 && UvSelectComponents(s)[1] == 1);
    assert(!UvSelectIsValid(0));
}

static void TestInterpolate() {
    const std::array<float, 2> a{0.0f, 0.0f}, b{1.0f, 0.0f}, c{0.0f, 1.0f};
    const auto p = InterpolateUv(a, b, c, {0.25f, 0.5f});
    assert(std::fabs(p[0] - 0.25f) < 1e-6f && std::fabs(p[1] - 0.5f) < 1e-6f);
    const auto corner = InterpolateUv(a, b, c, {0.0f, 0.0f});
    assert(corner[0] == 0.0f && corner[1] == 0.0f);
    const auto bb = InterpolateUv(a, b, c, {1.0f, 0.0f});
    assert(bb[0] == 1.0f && bb[1] == 0.0f);
    assert(UvOffset(10, 2, 1) == (10 + 16 + 1) * 4);
    assert(TriangleUvBytes == 32 && TriangleNormalWord == 3 && TriangleVertexNormalWord == 4 &&
           TriangleTangentWord == 7);
    const auto packed = PackNormal({0.0f, -1.0f, 0.5f});
    const auto n = UnpackNormal(packed);
    assert(std::fabs(n[0]) < 1e-6f && std::fabs(n[1] + 1.0f) < 1e-6f && std::fabs(n[2] - 0.5f) < 0.01f);
    assert(UnpackNormal(PackNormal({1.0f, 1.0f, 1.0f}))[0] == 1.0f);
    const auto normal_only = MakeSurface(5, true, 100, 0.5f, false);
    assert((normal_only.flags & SurfaceHasNormal) != 0 && (normal_only.flags & SurfaceHasUv) == 0 &&
           (normal_only.flags & SurfaceAlphaTest) == 0);
    assert((MakeSurface(5, true, 100, 0.5f).flags & (SurfaceHasNormal | SurfaceHasUv)) ==
           (SurfaceHasNormal | SurfaceHasUv));
    assert((MakeSurface(5, true, NoUv, 0.5f).flags & SurfaceHasNormal) == 0);
    assert((MakeSurface(5, false, 100, 0.5f, true, false, 0, true).flags & SurfaceBlended) != 0);
    assert((MakeSurface(5, false, 100, 0.5f, true, false, 0, true).flags & SurfaceAlphaTest) == 0);
    assert((MakeSurface(5, true, 100, 0.5f).flags & SurfaceBlended) == 0);
}

static void TestTable() {
    TextureTable table{4};
    table.BeginFrame();
    assert(table.Slot(0) == 0);
    const uint32_t a = table.Slot(0x1000);
    const uint32_t b = table.Slot(0x2000);
    assert(a == 1 && b == 2);
    assert(table.Slot(0x1000) == 1);
    assert(table.Slot(0x3000) == 3);
    assert(table.Slot(0x4000) == 0);
    assert(table.stats.full == 1);
    assert(table.Used() == 4);
    assert(table.View(1) == 0x1000 && table.View(0) == 0 && table.View(9) == 0);
    table.BeginFrame();
    assert(table.Used() == 1);
    assert(table.Slot(0x2000) == 1);
    assert(table.stats.frames == 2 && table.stats.assigned == 4 && table.stats.reused == 1);
}

static bool Near3(const std::array<float, 3>& a, float x, float y, float z, float eps = 1e-4f) {
    return std::fabs(a[0] - x) < eps && std::fabs(a[1] - y) < eps && std::fabs(a[2] - z) < eps;
}

static void TestNormalSelect() {
    assert(NormalSelect(-1, -1) == 0 && !NormalSelectValid(0));
    const uint32_t ab = NormalSelect(3, 17);
    assert(NormalSelectValid(ab) && NormalSelectParams(ab)[0] == 3 && NormalSelectParams(ab)[1] == 17);
    const uint32_t only_b = NormalSelect(40, 5);
    assert(NormalSelectParams(only_b)[0] == -1 && NormalSelectParams(only_b)[1] == 5);
    const uint32_t uv = UvSelect(7, 0, 1);
    const uint32_t both = uv | ab;
    assert(UvSelectIsValid(both) && UvSelectParam(both) == 7 && UvSelectComponents(both)[1] == 1);
    assert(NormalSelectParams(both)[0] == 3 && NormalSelectParams(both)[1] == 17);
    assert((ab & 0x08200000u) == 0x08200000u);
}

static void TestNormalDecode() {
    assert(Near3(DecodeNormalMap(0.5f, 0.5f, 1.0f), 0.0f, 0.0f, 1.0f, 0.01f));
    assert(Near3(DecodeNormalMap(0.5f, 0.5f, 0.0f), 0.0f, 0.0f, 1.0f, 0.01f));
    const auto bc5 = DecodeNormalMap(1.0f, 0.5f, 0.0f);
    assert(Near3(bc5, 1.0f, 0.0f, 0.0f, 0.01f));
    const auto tilted = DecodeNormalMap(0.75f, 0.5f, 0.0f);
    assert(std::fabs(tilted[0] - 0.5f) < 0.01f && std::fabs(tilted[2] - std::sqrt(0.75f)) < 0.01f);
    const auto rgb = DecodeNormalMap(0.5f, 1.0f, 0.5f);
    assert(Near3(rgb, 0.0f, 1.0f, 0.0f, 0.01f));
}

static void TestNormalInterpolate() {
    const std::array<float, 3> a{0.0f, 0.0f, 1.0f}, b{1.0f, 0.0f, 0.0f}, c{0.0f, 1.0f, 0.0f};
    assert(Near3(InterpolateNormal(a, b, c, {0.0f, 0.0f}), 0.0f, 0.0f, 1.0f));
    assert(Near3(InterpolateNormal(a, b, c, {1.0f, 0.0f}), 1.0f, 0.0f, 0.0f));
    const auto mid = InterpolateNormal(a, b, c, {0.5f, 0.0f});
    const float s = std::sqrt(0.5f);
    assert(Near3(mid, s, 0.0f, s));
    assert(std::fabs(SurfDot(mid, mid) - 1.0f) < 1e-5f);
    assert(Near3(InterpolateNormal({0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0.3f, 0.3f}), 0.0f, 0.0f, 0.0f));
}

static void TestPickVertexNormal() {
    const std::array<float, 3> flat{0.0f, 0.0f, 1.0f};
    const std::array<float, 3> tangent{1.0f, 0.0f, 0.0f};
    const std::array<float, 3> tilted{0.1f, 0.0f, 0.9f};
    const auto pick = PickVertexNormal({tangent, tilted, {0, 0, 0}, {0, 0, 0}}, flat);
    assert(Near3(pick, SurfUnit(tilted)[0], 0.0f, SurfUnit(tilted)[2]));
    const auto flipped = PickVertexNormal({tangent, {0.0f, 0.0f, -1.0f}, {0, 0, 0}, {0, 0, 0}}, flat);
    assert(Near3(flipped, 0.0f, 0.0f, 1.0f));
    const auto none = PickVertexNormal({tangent, {0.0f, 1.0f, 0.0f}, {0, 0, 0}, {0, 0, 0}}, flat);
    assert(Near3(none, 0.0f, 0.0f, 0.0f));
}

static void TestTangent() {
    const std::array<float, 3> p0{0, 0, 0}, p1{1, 0, 0}, p2{0, 1, 0}, n{0, 0, 1};
    const auto t = TangentFromUv(p0, p1, p2, {0, 0}, {1, 0}, {0, 1}, n);
    assert(t.valid && Near3(t.t, 1.0f, 0.0f, 0.0f) && t.handedness == 1.0f);
    const auto mirrored = TangentFromUv(p0, p1, p2, {0, 0}, {1, 0}, {0, -1}, n);
    assert(mirrored.valid && Near3(mirrored.t, 1.0f, 0.0f, 0.0f) && mirrored.handedness == -1.0f);
    const auto swapped = TangentFromUv(p0, p1, p2, {0, 0}, {0, 1}, {1, 0}, n);
    assert(swapped.valid && Near3(swapped.t, 0.0f, 1.0f, 0.0f) && swapped.handedness == -1.0f);
    assert(!TangentFromUv(p0, p1, p2, {0, 0}, {0, 0}, {0, 0}, n).valid);
    assert(Near3(PerturbNormal(n, t, {0.0f, 0.0f, 1.0f}), 0.0f, 0.0f, 1.0f));
    const auto bent = PerturbNormal(n, t, DecodeNormalMap(1.0f, 0.5f, 0.0f));
    assert(Near3(bent, 1.0f, 0.0f, 0.0f, 0.01f));
    const auto bent_y = PerturbNormal(n, mirrored, DecodeNormalMap(0.5f, 1.0f, 0.0f));
    assert(Near3(bent_y, 0.0f, -1.0f, 0.0f, 0.01f));
    Tangent invalid{};
    assert(Near3(PerturbNormal(n, invalid, {1.0f, 0.0f, 0.0f}), 0.0f, 0.0f, 1.0f));
    const auto map_rec = MakeSurface(5, false, 100, 0.5f, true, false, 9);
    assert((map_rec.flags >> SurfaceNormalMapShift) == 9 && SurfaceUsable(map_rec));
    assert((MakeSurface(5, false, 100, 0.5f, false, false, 9).flags >> SurfaceNormalMapShift) == 0);
    assert((MakeSurface(5, false, 100, 0.5f, true, false, 5000).flags >> SurfaceNormalMapShift) == 0);
}

static void TestCasterAlpha() {
    assert(PickCasterAlpha(true, false, false, false, false) == CasterAlpha::Own);
    assert(PickCasterAlpha(true, true, true, true, true) == CasterAlpha::Own);
    assert(PickCasterAlpha(false, false, true, true, true) == CasterAlpha::None);
    assert(PickCasterAlpha(false, true, true, true, true) == CasterAlpha::Prepass);
    assert(PickCasterAlpha(false, true, false, true, true) == CasterAlpha::Coverage);
    assert(PickCasterAlpha(false, true, false, false, true) == CasterAlpha::Blend);
    assert(PickCasterAlpha(false, true, false, false, false) == CasterAlpha::None);
    assert(PrepassKey(0, 6, 1, 0, 0) == 0);
    assert(PrepassKey(0x1000, 0, 1, 0, 0) == 0);
    const uint64_t key = PrepassKey(0x1000, 600, 1, 20, 0);
    assert(key != 0 && key == PrepassKey(0x1000, 600, 1, 20, 0));
    assert(key != PrepassKey(0x1000, 600, 1, 21, 0));
    assert(key != PrepassKey(0x1002, 600, 1, 20, 0));
    assert(key != PrepassKey(0x1000, 603, 1, 20, 0));
    assert(key != PrepassKey(0x1000, 600, 2, 20, 0));
    assert(key != PrepassKey(0x1000, 600, 1, 20, 4));
}

static void TestRecord() {
    const auto none = MakeSurface(0, true, NoUv, 0.5f);
    assert(!SurfaceUsable(none) && none.flags == 0);
    const auto no_uv = MakeSurface(5, true, NoUv, 0.5f);
    assert(!SurfaceUsable(no_uv) && (no_uv.flags & SurfaceAlphaTest) == 0);
    const auto no_tex = MakeSurface(0, true, 100, 0.5f);
    assert(!SurfaceUsable(no_tex) && (no_tex.flags & SurfaceAlphaTest) == 0);
    const auto full = MakeSurface(5, true, 100, 0.3f);
    assert(SurfaceUsable(full) && (full.flags & SurfaceAlphaTest) && full.alpha_ref == 0.3f);
    const auto opaque = MakeSurface(5, false, 100, 0.5f);
    assert(SurfaceUsable(opaque) && (opaque.flags & SurfaceAlphaTest) == 0);
    const auto omm = MakeSurface(5, true, 100, 0.5f, true, true);
    assert((omm.flags & SurfaceOmm) && (omm.flags & SurfaceAlphaTest));
    assert((MakeSurface(5, true, 100, 0.5f, true, false).flags & SurfaceOmm) == 0);
    assert(SurfaceAlphaTexture(full) == 5 && SurfaceAlphaChannel(full) == 3);
    const auto mask = MakeSurface(5, true, 100, 0.5f, true, false, 9, false, 7, 0);
    assert(SurfaceAlphaTexture(mask) == 7 && SurfaceAlphaChannel(mask) == 0);
    assert((mask.flags & SurfaceAlphaTest) && ((mask.flags >> SurfaceNormalMapShift) & 0x3FFu) == 9);
    const auto mask_only = MakeSurface(0, true, 100, 0.5f, true, false, 0, false, 7, 1);
    assert((mask_only.flags & SurfaceAlphaTest) && !SurfaceUsable(mask_only) &&
           SurfaceAlphaTexture(mask_only) == 7 && SurfaceAlphaChannel(mask_only) == 1);
    const auto same = MakeSurface(5, true, 100, 0.5f, true, false, 0, false, 5, 2);
    assert(SurfaceAlphaTexture(same) == 5 && SurfaceAlphaChannel(same) == 2);
    assert(SurfaceAlphaTexture(MakeSurface(5, true, 100, 0.5f, true, false, 0, false, 5000, 3)) == 5);
    assert((MakeSurface(0, true, 100, 0.5f, true, false, 0, false, 7, 3).flags & SurfaceAlphaTest) &&
           (MakeSurface(0, true, NoUv, 0.5f, true, false, 0, false, 7, 3).flags & SurfaceAlphaTest) == 0);
}

static void TestPassSurfaceSlot() {
    constexpr uint32_t slots = 3;
    uint64_t written[slots] = {~0ull, ~0ull, ~0ull};
    for (uint64_t frame = 0; frame < 12; ++frame) {
        const uint64_t built = frame - 1;
        if (frame > 0) {
            assert(SurfaceSlotMatchesBuild(written[PassSurfaceSlot(built, slots)], built));
            assert(frame < slots || !SurfaceSlotMatchesBuild(written[frame % slots], built));
        }
        written[frame % slots] = frame;
    }
}

static void TestTextureAlive() {
    assert(SurfaceTextureAlive(1, 0x1234, 7, 42, 42));
    assert(!SurfaceTextureAlive(0, 0x1234, 7, 42, 42));
    assert(!SurfaceTextureAlive(1, 0, 7, 42, 42));
    assert(!SurfaceTextureAlive(1, 0x1234, 0, 42, 42));
    assert(!SurfaceTextureAlive(1, 0x1234, 7, 0, 0));
    assert(!SurfaceTextureAlive(1, 0x1234, 7, 42, 43));
    assert(!SurfaceTextureAlive(1, 0x1234, 7, 42, 0));
}

static void TestSurfaceHolds() {
    std::unordered_map<uint64_t, uint64_t> ticks;
    const auto hold = [&](uint32_t image, uint64_t uid, uint64_t tick) {
        ticks[uint64_t(image) << 32 | uid] = tick;
    };
    const auto key = [](uint32_t image, uint64_t uid) { return uint64_t(image) << 32 | uid; };
    uint64_t gpu_done = 0;
    const auto is_free = [&](uint64_t tick) { return tick <= gpu_done; };
    assert(HoldReleased(0, is_free));
    assert(!HoldReleased(HeldByRt, is_free));
    assert(!HoldReleased(5, is_free));
    gpu_done = 5;
    assert(HoldReleased(5, is_free));
    assert(KeptTextureAlive(true, true, true));
    assert(!KeptTextureAlive(true, true, false));
    assert(!KeptTextureAlive(false, true, true));

    SurfaceHolds holds{3};
    holds.Bound(7, 0, hold);
    assert(holds.FrameCount() == 0 && ticks.empty());
    holds.Bound(7, 70, hold);
    holds.Bound(7, 70, hold);
    holds.Bound(8, 80, hold);
    assert(holds.FrameCount() == 2);
    assert(ticks[key(7, 70)] == HeldByRt && ticks[key(8, 80)] == HeldByRt);
    holds.Finish(10, false, 0, hold);
    assert(ticks[key(7, 70)] == 10 && ticks[key(8, 80)] == 10);
    assert(holds.HeldSlot() == SurfaceHolds::NoSlot && holds.FrameCount() == 0);

    holds.Bound(7, 70, hold);
    holds.Finish(11, true, 1, hold);
    assert(ticks[key(7, 70)] == HeldByRt && holds.HeldSlot() == 1);
    holds.Bound(9, 90, hold);
    holds.Finish(12, false, 2, hold);
    assert(ticks[key(7, 70)] == HeldByRt && ticks[key(9, 90)] == 12);
    holds.Bound(9, 90, hold);
    holds.Finish(13, true, 2, hold);
    assert(ticks[key(7, 70)] == 13 && ticks[key(9, 90)] == HeldByRt && holds.HeldSlot() == 2);
    holds.Bound(8, 80, hold);
    holds.Finish(14, true, 2, hold);
    assert(ticks[key(9, 90)] == 14 && ticks[key(8, 80)] == HeldByRt);
    holds.ReleaseAll(15, hold);
    assert(ticks[key(8, 80)] == 15 && holds.HeldSlot() == SurfaceHolds::NoSlot);
    holds.Finish(16, true, 0, hold);
    assert(ticks[key(8, 80)] == 15);
}

int main() {
    TestTextureAlive();
    TestHalf();
    TestPack();
    TestSelect();
    TestInterpolate();
    TestTable();
    TestRecord();
    TestCasterAlpha();
    TestNormalSelect();
    TestNormalDecode();
    TestNormalInterpolate();
    TestPickVertexNormal();
    TestTangent();
    TestPassSurfaceSlot();
    TestSurfaceHolds();
    std::puts("rt surface tests passed");
    return 0;
}

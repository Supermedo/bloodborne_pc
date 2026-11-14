#include <array>
#include <cassert>
#include <cstdio>
#include <vector>
#include "gpu/shadps4/video_core/renderer_vulkan/rt/rt_fx.h"

using namespace Vulkan::Rt;

namespace {

FxDrawState Smoke() {
    return {.blend_enable = true,
            .blend_bypass = false,
            .src_factor = FxBlendSrcAlpha,
            .dst_factor = FxBlendOneMinusSrcAlpha,
            .blend_func = FxBlendAdd,
            .depth_test = true,
            .depth_write = false,
            .color_format = FxFormat16_16_16_16,
            .primitive = FxPrimTriangleList,
            .indices = 12};
}

}

int main() {
    assert(FxSmokeRule(Smoke()));
    FxDrawState strip = Smoke();
    strip.primitive = FxPrimTriangleStrip;
    strip.indices = 11;
    assert(FxSmokeRule(strip));
    FxDrawState additive = Smoke();
    additive.dst_factor = 1;
    assert(!FxSmokeRule(additive));
    FxDrawState ui = Smoke();
    ui.color_format = 10;
    ui.depth_test = false;
    assert(!FxSmokeRule(ui));
    FxDrawState depth_write = Smoke();
    depth_write.depth_write = true;
    assert(!FxSmokeRule(depth_write));
    FxDrawState sheet = Smoke();
    sheet.indices = FxSmokeMaxIndices + 3;
    assert(!FxSmokeRule(sheet));
    FxDrawState points = Smoke();
    points.primitive = 1;
    assert(!FxSmokeRule(points));
    {
        const std::vector<uint32_t> strip{0, 1, 2, 3, 0xFFFF, 4, 5, 6, 7, 8};
        std::vector<uint32_t> out;
        ExpandStrip([&](uint32_t i) { return strip[i]; }, uint32_t(strip.size()), true, 0xFFFFu,
                    out);
        const std::vector<uint32_t> expect{0, 1, 2, 2, 1, 3, 4, 5, 6, 6, 5, 7, 6, 7, 8};
        assert(out == expect);
        out.clear();
        ExpandStrip([&](uint32_t i) { return strip[i]; }, uint32_t(strip.size()), false, 0xFFFFu,
                    out);
        assert(out.size() == 24);
        out.clear();
        ExpandStrip([](uint32_t i) { return i; }, 2u, false, 0xFFFFFFFFu, out);
        assert(out.empty());
    }
    assert(FxSmokeTargets(0x1u, 0x1u));
    assert(FxSmokeTargets(0x3u, 0x1u));
    assert(!FxSmokeTargets(0x3u, 0x3u));
    assert(!FxSmokeTargets(0x2u, 0x2u));
    assert(!FxSmokeTargets(0x0u, 0x1u));
    assert(!FxSmokeTargets(0x1u, 0x0u));
    FxDrawState patch = Smoke();
    patch.primitive = FxPrimPatch;
    patch.stages = FxStagesLsHs;
    patch.indices = 10;
    assert(FxSmokePatchRule(patch));
    assert(!FxSmokeRule(patch));
    patch.indices = FxSmokeMaxParticles + 1;
    assert(FxSmokePatchRule(patch));
    patch.indices = 0;
    assert(!FxSmokePatchRule(patch));
    patch.indices = 1;
    patch.dst_factor = 1;
    assert(!FxSmokePatchRule(patch));
    patch.dst_factor = FxBlendOneMinusSrcAlpha;
    patch.stages = FxStagesVs;
    assert(!FxSmokePatchRule(patch));
    FxDrawState tess_list = Smoke();
    tess_list.stages = FxStagesLsHs;
    assert(!FxSmokeRule(tess_list));
    {
        const FxParticle particles[2]{{{1.0f, 2.0f, 3.0f}, 2.0f}, {{0.0f, 0.0f, 0.0f}, 4.0f}};
        std::vector<std::array<float, 3>> verts;
        assert(FxBuildQuads(particles, 2, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, verts) == 2);
        assert(verts.size() == 12);
        const std::array<float, 3> a{0.0f, 1.0f, 3.0f}, b{2.0f, 1.0f, 3.0f}, c{2.0f, 3.0f, 3.0f},
            d{0.0f, 3.0f, 3.0f};
        assert(verts[0] == a && verts[1] == b && verts[2] == c);
        assert(verts[3] == a && verts[4] == c && verts[5] == d);
        assert((verts[6] == std::array<float, 3>{-2.0f, -2.0f, 0.0f}));
        assert((verts[8] == std::array<float, 3>{2.0f, 2.0f, 0.0f}));
        std::vector<FxParticle> many(FxSmokeMaxParticles + 7);
        verts.clear();
        assert(FxBuildQuads(many.data(), uint32_t(many.size()), {1.0f, 0.0f, 0.0f},
                            {0.0f, 1.0f, 0.0f}, verts) == FxSmokeMaxParticles);
        assert(verts.size() == FxSmokeMaxParticles * 6);
    }
    {
        struct Sink {
            std::vector<std::array<float, 4>> clip;
            std::vector<std::array<float, 2>> uv;
            void push_back(const std::array<float, 3>& v) {
                clip.push_back(FxClipFromView(v, 1.0f, 1.0f));
                uv.push_back(FxQuadUv(uint32_t(uv.size())));
            }
        } sink;
        const FxParticle particle{{1.0f, 2.0f, 5.0f}, 2.0f};
        assert(FxBuildQuads(&particle, 1, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, sink) == 1);
        assert(sink.clip.size() == 6);
        assert((sink.clip[0] == std::array<float, 4>{0.0f, 1.0f, 0.0f, 5.0f}));
        assert((sink.clip[2] == std::array<float, 4>{2.0f, 3.0f, 0.0f, 5.0f}));
        assert((sink.uv[0] == std::array<float, 2>{0.0f, 1.0f}));
        assert((sink.uv[1] == std::array<float, 2>{1.0f, 1.0f}));
        assert((sink.uv[2] == std::array<float, 2>{1.0f, 0.0f}));
        assert((sink.uv[5] == std::array<float, 2>{0.0f, 0.0f}));
        assert((FxClipFromView({1.0f, 2.0f, 3.0f}, 2.0f, 4.0f) ==
                std::array<float, 4>{2.0f, 8.0f, 0.0f, 3.0f}));
        const float attr[2]{0.0f, 0.75f};
        assert(FxParticleSize(attr, 2, 1, 2.0f, 0.0f, 0.5f) == 1.5f);
        assert(FxParticleSize(attr, 2, 0, 1.0f, 0.0f, 0.5f) == 0.5f);
        assert(FxParticleSize(attr, 2, 3, 1.0f, 0.0f, 0.5f) == 0.5f);
        assert(FxParticleSize(nullptr, 0, 0, 1.0f, 0.0f, 0.5f) == 0.5f);
        assert(FxParticleSize(attr, 2, 1, 1.0f, 3.0f, 0.5f) == 3.0f);
        assert(FxSmokeTriangleBase(4u << 20) % 64 == 0);
        assert(FxSmokeTriangleBase(4u << 20) + FxSmokeMaxTriangles <= (4u << 20));
        assert(FxSmokeTriangleBase(100) == 0);
        assert((FxSmokeTriangleBase(4u << 20) * 36u) % 256u == 0);
        static_assert(FxSmokePositionsOffset % 256 == 0 && FxSmokeUvsOffset % 256 == 0);
        static_assert(FxSmokeUvsOffset >= FxSmokePositionsOffset + FxSmokeVertices * 16);
        static_assert(FxSmokeSlotBytes >= FxSmokeUvsOffset + FxSmokeVertices * 4);
        static_assert(FxSmokeMaxDraws * 48 <= FxSmokePositionsOffset);
    }
    std::puts("rt-fx-test ok");
    return 0;
}

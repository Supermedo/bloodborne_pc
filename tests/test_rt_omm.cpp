#include <cassert>
#include <cstdio>
#include <set>
#include <vector>

#include "gpu/shadps4/video_core/renderer_vulkan/rt/rt_blas_cache.h"
#include "gpu/shadps4/video_core/renderer_vulkan/rt/rt_omm.h"

using namespace Vulkan::Rt;

static void TestSizes() {
    static_assert(OmmMicroTriangles(0) == 1);
    static_assert(OmmMicroTriangles(3) == 64);
    static_assert(OmmDataWords(0) == 1);
    static_assert(OmmDataWords(2) == 1);
    static_assert(OmmDataWords(3) == 4);
    static_assert(OmmDataWords(4) == 16);
    static_assert(OmmDataBytes(3) == 16);
    static_assert(OmmInputBytes(100, 3) == 100 * (16 + 8 + 4));
    static_assert(OmmSamplesPerEdge(3) == 32);
}

static void TestRetire() {
    uint64_t gpu_done = 0;
    const auto is_free = [&](uint64_t tick) { return tick <= gpu_done; };
    RetiredBlock block{.offset = 0, .bytes = 256, .tick = 50, .frame = 10};
    gpu_done = 1000;
    assert(!RetireReady(block, 0, 0, is_free));
    assert(!RetireReady(block, 9, 49, is_free));
    assert(block.tlas_tick == 0);
    gpu_done = 50;
    assert(!RetireReady(block, 12, 60, is_free));
    assert(block.tlas_tick == 60);
    assert(!RetireReady(block, 13, 70, is_free));
    assert(block.tlas_tick == 60);
    gpu_done = 59;
    assert(!RetireReady(block, 13, 70, is_free));
    gpu_done = 60;
    assert(RetireReady(block, 14, 80, is_free));
    RetiredBlock same_frame{.offset = 0, .bytes = 256, .tick = 50, .frame = 10};
    gpu_done = 1000;
    assert(RetireReady(same_frame, 10, 55, is_free));
    assert(same_frame.tlas_tick == 55);
    RetiredBlock skipped{.offset = 0, .bytes = 256, .tick = 50, .frame = 10};
    gpu_done = 1000;
    for (uint64_t f = 11; f < 20; ++f) {
        assert(!RetireReady(skipped, 9, 49, is_free));
    }
    assert(RetireReady(skipped, 20, 90, is_free));
    // Loading screen: no camera, so no TLAS is ever built after the retirement. The end of the
    // retirement frame stands in for it (every trace builds its TLAS in its own frame), else the
    // pool fills with blocks that wait forever.
    std::vector<RetiredBlock> loading{{.offset = 0, .bytes = 256, .tick = 50, .frame = 10}};
    gpu_done = 1000;
    assert(!RetireReady(loading[0], 9, 49, is_free));
    CloseFrame(loading, 10, 52);
    assert(loading[0].tlas_tick == 52);
    gpu_done = 51;
    assert(!RetireReady(loading[0], 9, 49, is_free));
    gpu_done = 52;
    assert(RetireReady(loading[0], 9, 49, is_free));
    // A TLAS of the same frame after the retirement is kept: it came first.
    std::vector<RetiredBlock> built{{.offset = 0, .bytes = 256, .tick = 50, .frame = 10}};
    NoteTlas(built[0], 10, 51);
    CloseFrame(built, 10, 52);
    assert(built[0].tlas_tick == 51);
    // Blocks retired in a later frame (not possible at a frame end, but kept safe) wait.
    std::vector<RetiredBlock> later{{.offset = 0, .bytes = 256, .tick = 60, .frame = 11}};
    CloseFrame(later, 10, 52);
    assert(later[0].tlas_tick == 0);
}

static void TestCurve() {
    for (uint32_t level = 0; level <= OmmMaxLevel; ++level) {
        std::set<uint32_t> seen;
        const uint32_t n = OmmSamplesPerEdge(level);
        for (uint32_t j = 0; j < n; ++j) {
            for (uint32_t i = 0; i + j < n; ++i) {
                const uint32_t k = OmmSpaceFillingIndex((i + 0.5f) / n, (j + 0.5f) / n, level);
                assert(k < OmmMicroTriangles(level));
                seen.insert(k);
            }
        }
        assert(seen.size() == OmmMicroTriangles(level));
    }
    assert(OmmSpaceFillingIndex(0.01f, 0.01f, 3) == 0);
    assert(OmmSpaceFillingIndex(0.01f, 0.01f, 3) == OmmSpaceFillingIndex(0.05f, 0.03f, 3));
    assert(OmmSpaceFillingIndex(0.98f, 0.01f, 3) != OmmSpaceFillingIndex(0.01f, 0.98f, 3));
    assert(OmmSpaceFillingIndex(2.0f, -1.0f, 2) < OmmMicroTriangles(2));
}

static void TestClassify() {
    static_assert(OmmClassify(true, false) == OmmState::Opaque);
    static_assert(OmmClassify(false, true) == OmmState::Transparent);
    static_assert(OmmClassify(true, true) == OmmState::UnknownOpaque);
    static_assert(OmmClassify(false, false) == OmmState::UnknownOpaque);
    static_assert(OmmTriangleIndex(7, true, false) == OmmSpecialOpaque);
    static_assert(OmmTriangleIndex(7, false, true) == OmmSpecialTransparent);
    static_assert(OmmTriangleIndex(7, false, false) == 7);
}

static void TestBake() {
    uint32_t words[OmmDataWords(OmmMaxLevel)]{};
    uint32_t index = 0;
    OmmBakeTriangle(3, 0.5f, [](float, float) { return 1.0f; }, words, index, 3);
    assert(index == OmmSpecialOpaque);
    for (uint32_t w = 0; w < OmmDataWords(3); ++w) {
        assert(words[w] == 0x55555555u);
    }
    OmmBakeTriangle(3, 0.5f, [](float, float) { return 0.0f; }, words, index, 3);
    assert(index == OmmSpecialTransparent);
    for (uint32_t w = 0; w < OmmDataWords(3); ++w) {
        assert(words[w] == 0u);
    }
    OmmBakeTriangle(3, 0.5f, [](float u, float) { return u < 0.45f ? 0.0f : 1.0f; }, words, index, 9);
    assert(index == 9);
    uint32_t opaque = 0, transparent = 0, unknown = 0;
    for (uint32_t k = 0; k < 64; ++k) {
        const uint32_t state = (words[k / 16] >> ((k % 16) * 2)) & 3;
        opaque += state == 1;
        transparent += state == 0;
        unknown += state == 3;
    }
    assert(opaque > 0 && transparent > 0 && unknown > 0);
    assert(unknown <= 16);
    assert(transparent > opaque);
    const uint32_t k_left = OmmSpaceFillingIndex(0.1f, 0.1f, 3);
    const uint32_t k_right = OmmSpaceFillingIndex(0.9f, 0.05f, 3);
    assert(((words[k_left / 16] >> ((k_left % 16) * 2)) & 3) == 0);
    assert(((words[k_right / 16] >> ((k_right % 16) * 2)) & 3) == 1);
    OmmBakeTriangle(0, 0.5f, [](float u, float) { return u < 0.5f ? 0.0f : 1.0f; }, words, index, 1);
    assert(index == 1 && (words[0] & 3) == 3);
}

static void TestBuildFlags() {
    static_assert(BlasBuildFlags(true, true) == (BuildFastBuild | BuildAllowUpdate));
    static_assert(BlasBuildFlags(true, false) == (BuildFastBuild | BuildAllowUpdate));
    static_assert(BlasBuildFlags(false, true) == (BuildFastTrace | BuildAllowCompaction));
    static_assert(BlasBuildFlags(false, false) == BuildFastTrace);
    static_assert(CompactionWorthIt(1000, 900));
    static_assert(!CompactionWorthIt(1000, 901));
    static_assert(!CompactionWorthIt(1000, 0));
    static_assert(BuildAllowUpdate == 0x1 && BuildAllowCompaction == 0x2 &&
                  BuildFastTrace == 0x4 && BuildFastBuild == 0x8);
    static_assert((BlasBuildFlags(false, true) & BuildAllowUpdate) == 0);
    static_assert((BlasBuildFlags(false, true) & BuildAllowCompaction) != 0);
}

static void TestCompactDecision() {
    CacheEntry e{};
    e.valid = true;
    e.built_frame = 40;
    e.bytes = 1000;
    e.triangles = 12;
    assert(CompactCandidate(&e, 40, 600));
    assert(!CompactCandidate(nullptr, 40, 600));
    assert(!CompactCandidate(&e, 41, 600));
    assert(!CompactCandidate(&e, 40, 950));
    assert(!CompactCandidate(&e, 40, 0));
    CacheEntry skinned = e;
    skinned.updatable = true;
    assert(!CompactCandidate(&skinned, 40, 600));
    CacheEntry invalid = e;
    invalid.valid = false;
    assert(!CompactCandidate(&invalid, 40, 600));
    CacheEntry done = e;
    done.compacted = true;
    assert(!CompactCandidate(&done, 40, 600));
    CacheEntry dirty = e;
    dirty.dirty = true;
    assert(!CompactCandidate(&dirty, 40, 600));
    CacheEntry masked = e;
    masked.omm = true;
    assert(!CompactCandidate(&masked, 40, 600));

    TestRetire();

    assert(ReuseAllocation(&e, 12, false, false));
    assert(!ReuseAllocation(nullptr, 12, false, false));
    assert(!ReuseAllocation(&e, 13, false, false));
    assert(!ReuseAllocation(&e, 12, true, false));
    assert(!ReuseAllocation(&done, 12, false, false));
    CacheEntry omm = e;
    omm.omm = true;
    assert(!ReuseAllocation(&omm, 12, false, false));
    assert(ReuseAllocation(&omm, 12, false, true));
}

int main() {
    TestSizes();
    TestCurve();
    TestClassify();
    TestBake();
    TestBuildFlags();
    TestCompactDecision();
    std::puts("rt-omm-test: ok");
    return 0;
}

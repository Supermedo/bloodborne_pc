#include <cassert>
#include <cmath>
#include <cstdio>
#include <set>

#include "gpu/shadps4/video_core/renderer_vulkan/rt/rt_sharc.h"

using namespace Vulkan::Rt;

static void TestLevel() {
    assert(SharcLevel(0.0f) == 0);
    assert(SharcLevel(5.0f) == 0);
    assert(SharcLevel(12.5f) == 0);
    assert(SharcLevel(13.0f) == 1);
    assert(SharcLevel(25.0f) == 1);
    assert(SharcLevel(26.0f) == 2);
    assert(SharcLevel(1e9f) == SharcMaxLevel);
    assert(SharcLevel(std::nanf("")) == 0);
    for (float d = 0.5f; d < 2000.0f; d *= 1.3f) {
        const float cell = SharcCellSize(SharcLevel(d));
        assert(cell >= SharcBaseCell);
        assert(cell >= d * SharcCellPerMeter * 0.999f);
        assert(cell < std::max(SharcBaseCell, d * SharcCellPerMeter) * 2.0f + 1e-4f);
    }
}

static void TestKey() {
    const SharcKey a = SharcMakeKey({1.1f, 2.0f, 3.0f}, 0, {0.0f, 1.0f, 0.0f});
    const SharcKey b = SharcMakeKey({1.2f, 2.1f, 3.1f}, 0, {0.0f, 1.0f, 0.0f});
    assert(a.lo == b.lo && a.hi == b.hi);
    const SharcKey c = SharcMakeKey({1.6f, 2.0f, 3.0f}, 0, {0.0f, 1.0f, 0.0f});
    assert(!(a.lo == c.lo && a.hi == c.hi));
    const SharcKey d = SharcMakeKey({1.1f, 2.0f, 3.0f}, 0, {0.0f, -1.0f, 0.0f});
    assert(!(a.lo == d.lo && a.hi == d.hi));
    const SharcKey e = SharcMakeKey({1.1f, 2.0f, 3.0f}, 1, {0.0f, 1.0f, 0.0f});
    assert(!(a.lo == e.lo && a.hi == e.hi));
    const SharcKey n = SharcMakeKey({-0.1f, -0.1f, -0.1f}, 0, {1.0f, 1.0f, 1.0f});
    const SharcKey p = SharcMakeKey({0.1f, 0.1f, 0.1f}, 0, {1.0f, 1.0f, 1.0f});
    assert(!(n.lo == p.lo && n.hi == p.hi));
    std::set<unsigned long long> keys, slots;
    int distinct = 0;
    for (int x = -20; x < 20; ++x) {
        for (int y = -20; y < 20; ++y) {
            for (int z = -5; z < 5; ++z) {
                const SharcKey k = SharcMakeKey({x * 0.25f + 0.1f, y * 0.25f + 0.1f, z * 0.25f + 0.1f},
                                                0, {0.0f, 0.0f, 1.0f});
                keys.insert((unsigned long long)k.hi << 32 | k.lo);
                slots.insert((unsigned long long)SharcChecksum(k) << 32 | SharcHashSlot(k));
                assert(SharcChecksum(k) != 0);
                assert(SharcHashSlot(k) < SharcSlots);
                ++distinct;
            }
        }
    }
    assert(int(keys.size()) == distinct);
    assert(int(slots.size()) == distinct);
}

static void TestResolve() {
    SharcResolved r{};
    const uint32_t one = SharcEncode(1.0f);
    assert(SharcEncode(-1.0f) == 0);
    assert(SharcEncode(std::nanf("")) == 0);
    assert(SharcEncode(1e9f) == SharcEncode(SharcMaxRadiance));
    r = SharcResolve(r, {one * 3, one * 6, one * 9}, 3);
    assert(std::fabs(r.rgb[0] - 1.0f) < 1e-3f && std::fabs(r.rgb[2] - 3.0f) < 1e-3f);
    assert(r.weight == 3.0f);
    assert(!SharcReady(r.weight));
    r = SharcResolve(r, {0, 0, 0}, 1);
    assert(std::fabs(r.rgb[0] - 0.75f) < 1e-3f);
    assert(SharcReady(r.weight));
    const SharcResolved same = SharcResolve(r, {0, 0, 0}, 0);
    assert(same.rgb[0] == r.rgb[0] && same.weight == r.weight);
    for (int i = 0; i < 200; ++i) {
        r = SharcResolve(r, {one * 2, one * 2, one * 2}, 1);
    }
    assert(r.weight == SharcMaxWeight);
    assert(std::fabs(r.rgb[0] - 2.0f) < 1e-3f);
    const SharcResolved capped = SharcResolve({}, {one * SharcFrameCap, 0, 0}, 5000);
    assert(capped.weight == SharcMaxWeight);
    assert(std::fabs(capped.rgb[0] - 1.0f) < 1e-3f);
    int frames = 0;
    SharcResolved moving{{0.0f, 0.0f, 0.0f}, SharcMaxWeight};
    while (moving.rgb[0] < 0.9f) {
        moving = SharcResolve(moving, {one * 4, 0, 0}, 4);
        ++frames;
    }
    assert(frames <= 20);
    static_assert(SharcSumSafe(), "per-frame fixed-point sums fit in 32 bits");
}

static void TestStaleAndPick() {
    assert(!SharcStale(100, 100));
    assert(!SharcStale(100, 100 + SharcStaleFrames));
    assert(SharcStale(100, 101 + SharcStaleFrames));
    assert(!SharcStale(0xFFFFFFF0u, 5));
    uint32_t picked = 0;
    for (uint32_t y = 0; y < 64; ++y) {
        for (uint32_t x = 0; x < 64; ++x) {
            picked += SharcUpdatePath(x, y, 7) ? 1 : 0;
        }
    }
    assert(picked > 4096 / (SharcUpdateMask + 1) / 2 && picked < 4096 / (SharcUpdateMask + 1) * 2);
    assert(SharcBytes == uint64_t(SharcSlots) * SharcEntryWords * 4);
    assert(SharcGroups() * 64 == SharcSlots);
}

int main() {
    TestLevel();
    TestKey();
    TestResolve();
    TestStaleAndPick();
    std::printf("rt-sharc-test: ok\n");
    return 0;
}

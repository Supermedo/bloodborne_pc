#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "gpu/shadps4/video_core/renderer_vulkan/rt/rt_blas_cache.h"
#include "gpu/shadps4/video_core/renderer_vulkan/rt/rt_geo_cache.h"
#include "gpu/shadps4/video_core/renderer_vulkan/rt/rt_geometry.h"
#include "gpu/shadps4/video_core/renderer_vulkan/rt/rt_placement.h"
#include "gpu/shadps4/video_core/renderer_vulkan/rt/rt_sky_finder.h"

using namespace Vulkan::Rt;

namespace {

bool Near(float a, float b) {
    return std::fabs(a - b) < 1e-5f;
}

bool IsNan(const std::array<std::array<float, 3>, 3>& tri) {
    for (const auto& v : tri) {
        for (const float f : v) {
            if (!std::isnan(f)) {
                return false;
            }
        }
    }
    return true;
}

void TestClipToView() {
    const float xs = 2.0f, ys = 1.5f;
    const std::array<float, 3> view{3.0f, -4.0f, 10.0f};
    const std::array<float, 4> clip{view[0] * xs, view[1] * ys, 0.25f, view[2]};
    const auto back = ClipToView(clip, xs, ys);
    assert(Near(back[0], view[0]) && Near(back[1], view[1]) && Near(back[2], view[2]));

    const std::array<float, 2> ndc{clip[0] / clip[3], clip[1] / clip[3]};
    const auto dir = RayDirection(ndc, xs, ys);
    for (int i = 0; i < 3; ++i) {
        assert(Near(dir[i] * view[2], view[i]));
    }
}

void TestBlasInput() {
    const auto none = BlasInputFor(0);
    assert(none.vertex_bytes == 0 && none.max_vertex == 0 && none.primitives == 0);
    const auto some = BlasInputFor(1000);
    assert(some.vertex_bytes == 1000ull * 36 && some.max_vertex == 2999 && some.primitives == 1000);
    assert(VertexStride == 12 && TriangleBytes == 36);
}

void TestFrameGeometry() {
    std::vector<DrawRecord> records(4);
    std::vector<uint8_t> indices(64);
    FrameGeometry geometry;
    geometry.Begin(records.data(), indices.data(), {.triangles = 20, .index_bytes = 64, .records = 4});

    const std::array<uint16_t, 3> tri16{1000, 1002, 1001};
    assert(geometry.Add({.store_base = 1, .first_vertex = 100, .vertex_count = 3, .instances = 1,
                         .base_vertex = -900, .indices = tri16.data(), .index_count = 3,
                         .index_size = 2, .restart = true}));
    assert(geometry.Records() == 1 && geometry.Triangles() == 1 && geometry.IndexBytes() == 8);
    assert(records[0].triangle_base == 0 && records[0].triangle_count == 1);
    assert(records[0].index_lo == 1000 && records[0].store_base == 1);
    assert(records[0].flags == (FlagIndex16 | FlagRestart));
    assert(std::memcmp(indices.data(), tri16.data(), 6) == 0);

    const std::array<uint32_t, 6> tri32{0, 1, 2, 2, 1, 0};
    assert(geometry.Add({.store_base = 4, .first_vertex = 10, .vertex_count = 3, .instances = 3,
                         .base_vertex = 10, .indices = tri32.data(), .index_count = 6,
                         .index_size = 4, .restart = false}));
    assert(geometry.Records() == 2 && geometry.Triangles() == 7 && geometry.IndexBytes() == 32);
    assert(records[1].triangle_base == 1 && records[1].triangle_count == 6);
    assert(records[1].prims_per_instance == 2 && records[1].index_offset == 8);
    assert(records[1].index_lo == 0 && records[1].flags == FlagIndex32);
    assert(std::memcmp(indices.data() + 8, tri32.data(), 24) == 0);

    assert(geometry.Add({.store_base = 13, .first_vertex = 7, .vertex_count = 6, .instances = 1,
                         .base_vertex = 7, .indices = nullptr, .index_count = 6, .index_size = 0}));
    assert(geometry.Records() == 3 && geometry.Triangles() == 9 && geometry.IndexBytes() == 32);
    assert(records[2].flags == 0 && records[2].index_lo == 0 && records[2].triangle_base == 7);

    const auto before = geometry.stats;
    assert(!geometry.Add({.store_base = 0, .first_vertex = 0, .vertex_count = 3, .instances = 1,
                          .indices = tri16.data(), .index_count = 3, .index_size = 2}));
    assert(!geometry.Add({.store_base = 1, .first_vertex = 0, .vertex_count = 3, .instances = 1,
                          .indices = tri16.data(), .index_count = 4, .index_size = 2}));
    assert(!geometry.Add({.store_base = 1, .first_vertex = 0, .vertex_count = 3, .instances = 1,
                          .base_vertex = 5, .indices = tri16.data(), .index_count = 3,
                          .index_size = 2}));
    assert(!geometry.Add({.store_base = 1, .first_vertex = 0, .vertex_count = 3, .instances = 1,
                          .indices = nullptr, .index_count = 3, .index_size = 2}));
    assert(!geometry.Add({.store_base = 1, .first_vertex = 0, .vertex_count = 3, .instances = 1,
                          .indices = tri16.data(), .index_count = 3, .index_size = 3}));
    assert(geometry.stats.invalid == before.invalid + 5 && geometry.Records() == 3);
    const std::array<uint16_t, 6> marker16{0, 1, 2, 0xFFFF, 1, 2};
    assert(!geometry.Add({.store_base = 1, .first_vertex = 0, .vertex_count = 3, .instances = 1,
                          .indices = marker16.data(), .index_count = 6, .index_size = 2,
                          .restart = true}));
    const std::array<uint32_t, 3> marker32{0, 0xFFFFFFFFu, 2};
    assert(!geometry.Add({.store_base = 1, .first_vertex = 0, .vertex_count = 3, .instances = 1,
                          .indices = marker32.data(), .index_count = 3, .index_size = 4,
                          .restart = true}));
    assert(geometry.stats.invalid == before.invalid + 7 && geometry.Records() == 3);

    assert(!geometry.Add({.store_base = 1, .first_vertex = 0, .vertex_count = 3, .instances = 12,
                          .indices = tri16.data(), .index_count = 3, .index_size = 2}));
    assert(geometry.stats.capacity == 1 && geometry.Triangles() == 9);
    const std::array<uint32_t, 9> big32{};
    assert(!geometry.Add({.store_base = 1, .first_vertex = 0, .vertex_count = 3, .instances = 1,
                          .indices = big32.data(), .index_count = 9, .index_size = 4}));
    assert(geometry.stats.capacity == 2);
    assert(geometry.Add({.store_base = 1, .first_vertex = 0, .vertex_count = 3, .instances = 11,
                         .indices = tri16.data(), .index_count = 3, .index_size = 2}));
    assert(geometry.Triangles() == 20 && geometry.Records() == 4);
    assert(!geometry.Add({.store_base = 1, .first_vertex = 0, .vertex_count = 3, .instances = 1,
                          .indices = nullptr, .index_count = 3, .index_size = 0}));
    assert(geometry.stats.capacity == 3 && geometry.stats.accepted == 4);

    geometry.Begin(records.data(), indices.data(), {.triangles = 20, .index_bytes = 64, .records = 4});
    assert(geometry.Records() == 0 && geometry.Triangles() == 0 && geometry.IndexBytes() == 0);
}

void TestGather() {
    const float xs = 2.0f, ys = 4.0f;
    std::vector<std::array<float, 4>> positions(32, std::array<float, 4>{0, 0, 0, 1});
    const auto put = [&](uint32_t slot, float x, float y, float z) {
        positions[slot] = {x * xs, y * ys, 0.5f, z};
    };
    put(1, 0, 0, 5);
    put(2, 1, 0, 5);
    put(3, 0, 1, 5);
    put(4, 0, 0, 7);
    put(5, 1, 0, 7);
    put(6, 0, 1, 7);
    put(10, 2, 2, 9);
    put(11, 3, 2, 9);
    put(12, 2, 3, 9);
    put(13, 8, 8, std::numeric_limits<float>::infinity());

    std::vector<DrawRecord> records(3);
    std::vector<uint8_t> indices(64);
    FrameGeometry geometry;
    geometry.Begin(records.data(), indices.data(), {.triangles = 64, .index_bytes = 64, .records = 3});
    const std::array<uint16_t, 6> a{100, 101, 102, 0xFFFF, 100, 101};
    assert(geometry.Add({.store_base = 1, .first_vertex = 100, .vertex_count = 3, .instances = 2,
                         .base_vertex = 0, .indices = a.data(), .index_count = 6, .index_size = 2,
                         .restart = false}));
    const std::array<uint32_t, 6> b{0, 1, 2, 0, 3, 1};
    assert(geometry.Add({.store_base = 10, .first_vertex = 50, .vertex_count = 4, .instances = 1,
                         .base_vertex = 50, .indices = b.data(), .index_count = 6, .index_size = 4}));
    assert(geometry.Add({.store_base = 1, .first_vertex = 0, .vertex_count = 3, .instances = 1,
                         .base_vertex = 0, .indices = nullptr, .index_count = 3, .index_size = 0}));
    assert(geometry.Triangles() == 7);

    const std::span<const DrawRecord> recs(records.data(), geometry.Records());
    const std::span<const uint8_t> idx(indices.data(), geometry.IndexBytes());
    const auto tri0 = GatherTriangle(recs, idx, positions, 0, xs, ys);
    assert(Near(tri0[0][0], 0) && Near(tri0[0][2], 5));
    assert(Near(tri0[1][0], 1) && Near(tri0[1][1], 0) && Near(tri0[1][2], 5));
    assert(Near(tri0[2][0], 0) && Near(tri0[2][1], 1) && Near(tri0[2][2], 5));
    assert(IsNan(GatherTriangle(recs, idx, positions, 1, xs, ys)));
    const auto tri2 = GatherTriangle(recs, idx, positions, 2, xs, ys);
    assert(Near(tri2[0][2], 7) && Near(tri2[1][0], 1) && Near(tri2[2][1], 1));
    assert(IsNan(GatherTriangle(recs, idx, positions, 3, xs, ys)));
    const auto tri4 = GatherTriangle(recs, idx, positions, 4, xs, ys);
    assert(Near(tri4[0][0], 2) && Near(tri4[1][0], 3) && Near(tri4[2][1], 3) && Near(tri4[2][2], 9));
    assert(IsNan(GatherTriangle(recs, idx, positions, 5, xs, ys)));
    const auto tri6 = GatherTriangle(recs, idx, positions, 6, xs, ys);
    assert(Near(tri6[0][2], 5) && Near(tri6[1][0], 1) && Near(tri6[2][1], 1));
    assert(IsNan(GatherTriangle(recs, idx, positions, 7, xs, ys)));
    assert(IsNan(GatherTriangle({}, idx, positions, 0, xs, ys)));

    records[1].index_lo = 1;
    assert(IsNan(GatherTriangle(recs, idx, positions, 4, xs, ys)));
    records[1].index_lo = 0;
    records[1].store_base = 30;
    assert(IsNan(GatherTriangle(recs, idx, positions, 4, xs, ys)));
}

void TestPopLast() {
    std::vector<DrawRecord> records(4);
    std::vector<uint8_t> indices(64);
    FrameGeometry geometry;
    geometry.Begin(records.data(), indices.data(), {.triangles = 20, .index_bytes = 64, .records = 4});
    const std::array<uint16_t, 6> tri16{0, 1, 2, 2, 1, 0};
    assert(geometry.Add({.store_base = 1, .first_vertex = 0, .vertex_count = 3, .instances = 1,
                         .indices = tri16.data(), .index_count = 6, .index_size = 2}));
    assert(geometry.Add({.store_base = 1, .first_vertex = 0, .vertex_count = 3, .instances = 2,
                         .indices = tri16.data(), .index_count = 6, .index_size = 2}));
    assert(geometry.Records() == 2 && geometry.Triangles() == 6 && geometry.IndexBytes() == 24);
    geometry.PopLast();
    assert(geometry.Records() == 1 && geometry.Triangles() == 2 && geometry.IndexBytes() == 12);
    assert(geometry.stats.accepted == 1);
    geometry.PopLast();
    geometry.PopLast();
    assert(geometry.Records() == 0 && geometry.Triangles() == 0 && geometry.IndexBytes() == 0);
}

void TestBlockPool() {
    BlockPool pool;
    pool.Reset(1024, 256);
    const auto a = pool.Alloc(100);
    const auto b = pool.Alloc(300);
    const auto c = pool.Alloc(256);
    assert(a == 0 && b == 256 && c == 768 && pool.Used() == 1024);
    assert(pool.Alloc(1) == BlockPool::Fail);
    pool.Free(b, 300);
    assert(pool.Used() == 512 && pool.FreeBlocks() == 1);
    assert(pool.Alloc(513) == BlockPool::Fail);
    pool.Free(a, 100);
    assert(pool.FreeBlocks() == 1 && pool.Used() == 256);
    assert(pool.Alloc(768) == 0);
    pool.Free(c, 256);
    pool.Free(0, 768);
    assert(pool.FreeBlocks() == 1 && pool.Used() == 0 && pool.Alloc(1024) == 0);
    // Near full is past three quarters of live bytes; blocks retired but not yet freed do not
    // count, or one eviction would trigger the next until nothing is left.
    static_assert(!PoolNearFull(768, 0, 1024) && PoolNearFull(769, 0, 1024));
    static_assert(!PoolNearFull(1000, 300, 1024) && PoolNearFull(1000, 200, 1024));
    static_assert(!PoolNearFull(100, 200, 1024) && !PoolNearFull(1, 0, 0));
}

void TestBlasCache() {
    BlasCache cache;
    std::vector<uint32_t> released;
    const auto release = [&](const CacheEntry& e) { released.push_back(e.id); };
    DrawKey key{.shader = 1, .geometry = 2, .indices = 3, .index_count = 300, .instances = 1,
                .vertex_count = 100};
    DrawKey other = key;
    other.indices = 4;

    cache.BeginFrame(1);
    auto miss = cache.Find(key, 0, false, false, 100);
    assert(!miss.entry && miss.rebuild && key.occurrence == 0);
    cache.Insert(key, {.id = 7, .offset = 0, .bytes = 512, .triangles = 100, .valid = true});
    auto same = cache.Find(key, 0, false, false, 100);
    assert(key.occurrence == 1 && !same.entry);
    assert(cache.stats.misses == 2);

    cache.BeginFrame(2);
    auto hit = cache.Find(key, 0, false, false, 100);
    assert(hit.entry && !hit.rebuild && hit.entry->id == 7 && hit.entry->last_used == 2);
    assert(cache.stats.hits == 1);

    cache.BeginFrame(3);
    auto skinned = cache.Find(key, 0, true, false, 100);
    assert(skinned.entry && !skinned.rebuild && skinned.reason == Rebuild::None);
    cache.BeginFrame(4);
    auto palette = cache.Find(key, 99, false, false, 100);
    assert(palette.entry && palette.rebuild);
    cache.BeginFrame(5);
    auto grown = cache.Find(key, 0, false, false, 101);
    assert(grown.entry && grown.rebuild);
    cache.MarkDirty(7, 5);
    assert(cache.stats.probe_dirty == 1);
    cache.MarkDirty(7, 5);
    assert(cache.stats.probe_dirty == 1);
    cache.BeginFrame(6);
    auto dirty = cache.Find(key, 0, false, false, 100);
    assert(dirty.entry && dirty.rebuild && dirty.entry->dirty);
    CacheEntry rebuilt = *dirty.entry;
    rebuilt.dirty = false;
    cache.Insert(key, rebuilt);
    cache.BeginFrame(7);
    auto clean = cache.Find(key, 0, false, false, 100);
    assert(clean.entry && !clean.rebuild);

    cache.Insert(other, {.id = 8, .offset = 512, .bytes = 512, .triangles = 100, .valid = true});
    assert(cache.Size() == 2 && cache.ById(8) && cache.ById(8)->offset == 512 && !cache.ById(9));
    cache.BeginFrame(8);
    (void)cache.Find(key, 0, false, false, 100);
    cache.BeginFrame(18);
    cache.EvictUnused(10, release);
    assert(cache.Size() == 1 && released.size() == 1 && released[0] == 8);
    assert(cache.stats.evicted == 1);
    assert(cache.EvictLeastRecent(release));
    assert(cache.Size() == 0 && released.back() == 7);
    assert(!cache.EvictLeastRecent(release));

    cache.BeginFrame(21);
    cache.Insert(key, {.id = 1, .triangles = 10, .valid = true});
    (void)cache.Find(key, 0, false, false, 10);
    assert(!cache.EvictLeastRecent(release));
    cache.Clear(release);
    assert(cache.Size() == 0 && released.back() == 1);
}

void TestInstanceTransform() {
    const Affine built{0, -1, 0, 5, 1, 0, 0, -2, 0, 0, 1, 3};
    const Affine now{1, 0, 0, 1, 0, 0, 1, 2, 0, -1, 0, 3};
    const std::array<float, 3> p_built{1.0f, 2.0f, 3.0f};
    const auto world = Transform(built, p_built);
    const auto p_now = Transform(InverseRigid(now), world);
    const auto via = Transform(InstanceTransform(now, built), p_built);
    for (int i = 0; i < 3; ++i) {
        assert(Near(via[i], p_now[i]));
    }
    const auto back = Transform(InverseRigid(InstanceTransform(now, built)), via);
    for (int i = 0; i < 3; ++i) {
        assert(Near(back[i], p_built[i]));
    }
    const auto id = InstanceTransform(now, now);
    for (int i = 0; i < 12; ++i) {
        assert(Near(id[i], IdentityAffine()[i]));
    }
}

void TestRefitAnchor() {
    const float xs = 2.0f, ys = 4.0f;
    const Affine anchor{0, -1, 0, 5, 1, 0, 0, -2, 0, 0, 1, 3};
    const Affine now{1, 0, 0, 1, 0, 0, 1, 2, 0, -1, 0, 3};
    const std::array<std::array<float, 3>, 3> world{{{4, 1, 2}, {5, 1, 2}, {4, 2, 3}}};
    std::vector<std::array<float, 4>> positions(4, std::array<float, 4>{0, 0, 0, 1});
    for (uint32_t k = 0; k < 3; ++k) {
        const auto v = Transform(InverseRigid(now), world[k]);
        positions[k + 1] = {v[0] * xs, v[1] * ys, 0.5f, v[2]};
    }
    std::vector<DrawRecord> records(1);
    std::vector<uint8_t> indices(16);
    FrameGeometry geometry;
    geometry.Begin(records.data(), indices.data(), {.triangles = 4, .index_bytes = 16, .records = 1});
    assert(geometry.Add({.store_base = 1, .first_vertex = 0, .vertex_count = 3, .instances = 1,
                         .base_vertex = 0, .indices = nullptr, .index_count = 3, .index_size = 0}));
    const std::span<const DrawRecord> recs(records.data(), geometry.Records());
    const auto plain = GatherTriangle(recs, {}, positions, 0, xs, ys);
    geometry.SetRebase(5, Reprojection(anchor, now));
    assert(!(records[0].flags & FlagRebase));
    geometry.SetRebase(0, Reprojection(anchor, now));
    assert(records[0].flags & FlagRebase);
    const auto rebased = GatherTriangle(recs, {}, positions, 0, xs, ys);
    const Affine instance = InstanceTransform(now, anchor);
    for (uint32_t k = 0; k < 3; ++k) {
        const auto in_anchor = Transform(InverseRigid(anchor), world[k]);
        const auto traced = Transform(instance, rebased[k]);
        for (int i = 0; i < 3; ++i) {
            assert(std::fabs(rebased[k][i] - in_anchor[i]) < 1e-4f);
            assert(std::fabs(traced[i] - plain[k][i]) < 1e-4f);
        }
    }
    geometry.Begin(records.data(), indices.data(), {.triangles = 4, .index_bytes = 16, .records = 1});
    assert(geometry.Add({.store_base = 1, .first_vertex = 0, .vertex_count = 3, .instances = 1,
                         .base_vertex = 0, .indices = nullptr, .index_count = 3, .index_size = 0}));
    assert(!(records[0].flags & FlagRebase));
}

void TestProbeSlots() {
    const std::array<uint16_t, 12> idx{100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111};
    const auto slots = ProbeSlots({.store_base = 50, .first_vertex = 100, .vertex_count = 12,
                                   .instances = 1, .base_vertex = 0, .indices = idx.data(),
                                   .index_count = 12, .index_size = 2});
    assert(slots[0] == 50 && slots[1] == 53 && slots[2] == 56 && slots[3] == 59);
    const auto plain = ProbeSlots({.store_base = 10, .first_vertex = 7, .vertex_count = 6,
                                   .instances = 1, .base_vertex = 7, .indices = nullptr,
                                   .index_count = 6, .index_size = 0});
    assert(plain[0] == 10 && plain[1] == 10 && plain[2] == 13 && plain[3] == 13);
    const std::array<uint32_t, 3> bad{5, 6, 7};
    const auto out = ProbeSlots({.store_base = 10, .first_vertex = 100, .vertex_count = 3,
                                 .instances = 1, .base_vertex = 0, .indices = bad.data(),
                                 .index_count = 3, .index_size = 4});
    assert(out[0] == 0 && out[3] == 0);
}

void TestRebuildReasons() {
    BlasCache cache;
    DrawKey key{.shader = 5, .geometry = 6, .indices = 7, .index_count = 30, .instances = 1,
                .vertex_count = 10};
    cache.BeginFrame(1);
    (void)cache.Find(key, 0x51, true, false, 10);
    cache.Insert(key, {.id = 1, .triangles = 10, .palette = 0x51, .valid = true,
                       .updatable = true});
    for (uint64_t f = 2; f < 12; ++f) {
        cache.BeginFrame(f);
        const auto still = cache.Find(key, 0x51, true, false, 10);
        assert(still.entry && !still.rebuild && still.reason == Rebuild::None);
    }
    assert(cache.stats.hits == 10 && cache.stats.rebuilt == 0);
    assert(cache.stats.reasons[size_t(Rebuild::Miss)] == 1);

    cache.BeginFrame(12);
    const auto moved = cache.Find(key, 0x52, true, false, 10);
    assert(moved.rebuild && moved.reason == Rebuild::Skeleton && moved.refit && moved.updatable);
    cache.BeginFrame(13);
    const auto unreadable = cache.Find(key, 0x51, true, true, 10);
    assert(unreadable.rebuild && unreadable.reason == Rebuild::Unreadable && unreadable.refit);
    cache.ById(1)->refits = MaxRefits;
    cache.BeginFrame(14);
    const auto full = cache.Find(key, 0x53, true, false, 10);
    assert(full.rebuild && full.reason == Rebuild::Skeleton && !full.refit);
    cache.BeginFrame(15);
    const auto sized = cache.Find(key, 0x51, true, false, 11);
    assert(sized.rebuild && sized.reason == Rebuild::Size && !sized.refit);

    DrawKey prop = key;
    prop.indices = 8;
    cache.BeginFrame(16);
    (void)cache.Find(prop, 0x10, false, false, 10);
    cache.Insert(prop, {.id = 2, .triangles = 10, .palette = 0x10, .valid = true});
    cache.BeginFrame(17);
    const auto prop_still = cache.Find(prop, 0x10, false, false, 10);
    assert(!prop_still.rebuild && !prop_still.updatable);
    cache.BeginFrame(18);
    const auto prop_moved = cache.Find(prop, 0x11, false, false, 10);
    assert(prop_moved.reason == Rebuild::Palette && prop_moved.updatable && !prop_moved.refit);

    assert(RebuildReason(nullptr, 0, false, false, 1) == Rebuild::Miss);
    assert(cache.stats.reasons[size_t(Rebuild::Skeleton)] == 2);
    assert(cache.stats.reasons[size_t(Rebuild::Unreadable)] == 1);
    assert(cache.stats.reasons[size_t(Rebuild::Size)] == 1);
    assert(cache.stats.reasons[size_t(Rebuild::Palette)] == 1);
}

void TestMovingRefit() {
    BlasCache cache;
    DrawKey key{.shader = 9, .geometry = 9, .indices = 9, .index_count = 30, .instances = 1,
                .vertex_count = 10};
    cache.BeginFrame(1);
    (void)cache.Find(key, 0, false, false, 10);
    cache.Insert(key, {.id = 3, .triangles = 10, .built_frame = 1, .valid = true});
    assert(!cache.MarkDirty(3, 0));
    assert(!cache.MarkDirty(4, 5));
    assert(cache.MarkDirty(3, 2));
    assert(cache.KeyOf(3) && *cache.KeyOf(3) == key && !cache.KeyOf(4));
    cache.moving_frames = 10;
    cache.ById(3)->moving = true;
    cache.ById(3)->moving_frame = 3;
    cache.BeginFrame(4);
    const auto dirty = cache.Find(key, 0, false, false, 10);
    assert(dirty.rebuild && dirty.reason == Rebuild::Dirty && dirty.updatable && !dirty.refit);
    CacheEntry rebuilt = *dirty.entry;
    rebuilt.dirty = false;
    rebuilt.updatable = true;
    cache.Insert(key, rebuilt);
    for (uint64_t f = 5; f <= 13; ++f) {
        cache.BeginFrame(f);
        const auto again = cache.Find(key, 0, false, false, 10);
        assert(again.rebuild && again.reason == Rebuild::Palette && again.refit && again.updatable);
        CacheEntry refit = *again.entry;
        refit.refits = RefitsAfter(Step::Refit, again, refit.id, cache.max_refits);
        cache.Insert(key, refit);
    }
    assert(cache.ById(3)->moving && cache.ById(3)->refits == 9);
    cache.BeginFrame(14);
    const auto still = cache.Find(key, 0, false, false, 10);
    assert(!still.rebuild && still.reason == Rebuild::None && !cache.ById(3)->moving);
}


bool Mismatch(const std::array<std::array<float, 3>, 4>& stored, const Affine& to_built,
              const std::array<std::array<float, 3>, 4>& now) {
    for (size_t k = 0; k < 4; ++k) {
        const auto built = Transform(to_built, now[k]);
        const float tol = 2e-3f + 1e-3f * std::fabs(stored[k][2]);
        for (int j = 0; j < 3; ++j) {
            if (std::fabs(built[j] - stored[k][j]) > tol) {
                return true;
            }
        }
    }
    return false;
}

void TestOccurrenceShift() {
    BlasCache cache;
    DrawKey key{.shader = 9, .geometry = 9, .indices = 9, .index_count = 3, .instances = 1,
                .vertex_count = 3};
    const std::array<std::array<std::array<float, 3>, 4>, 3> copies = {{
        {{{0, 0, 5}, {1, 0, 5}, {0, 1, 5}, {0, 0, 5}}},
        {{{10, 0, 5}, {11, 0, 5}, {10, 1, 5}, {10, 0, 5}}},
        {{{20, 0, 5}, {21, 0, 5}, {20, 1, 5}, {20, 0, 5}}},
    }};
    std::array<std::array<std::array<float, 3>, 4>, 3> fingerprint{};
    cache.BeginFrame(1);
    for (uint32_t c = 0; c < 3; ++c) {
        DrawKey k = key;
        const auto miss = cache.Find(k, 0, false, false, 1);
        assert(!miss.entry && k.occurrence == c);
        cache.Insert(k, {.id = c, .triangles = 1, .built_frame = 1, .inv_view = IdentityAffine(),
                         .valid = true});
        fingerprint[c] = copies[c];
    }

    std::vector<uint32_t> masked_entries;
    for (uint64_t f = 2; f <= 4; ++f) {
        cache.BeginFrame(f);
        for (uint32_t c = 1; c < 3; ++c) {
            DrawKey k = key;
            const auto hit = cache.Find(k, 0, false, false, 1);
            assert(hit.entry && !hit.rebuild && hit.entry->id == c - 1);
            const Affine to_built =
                InverseRigid(InstanceTransform(IdentityAffine(), hit.entry->inv_view));
            const bool mismatch = Mismatch(fingerprint[hit.entry->id], to_built, copies[c]);
            assert(mismatch);
            const uint32_t mask = mismatch ? 0x00 : 0xFF;
            assert(mask == 0);
            if (f == 2) {
                masked_entries.push_back(hit.entry->id);
            }
        }
    }
    assert(masked_entries.size() == 2);

    cache.BeginFrame(5);
    for (const uint32_t id : masked_entries) {
        cache.MarkDirty(id, 2);
    }
    assert(cache.stats.probe_dirty == 2);
    for (uint32_t c = 1; c < 3; ++c) {
        DrawKey k = key;
        const auto dirty = cache.Find(k, 0, false, false, 1);
        assert(dirty.rebuild && dirty.reason == Rebuild::Dirty);
        CacheEntry rebuilt = *dirty.entry;
        rebuilt.dirty = false;
        rebuilt.built_frame = 5;
        cache.Insert(k, rebuilt);
        fingerprint[rebuilt.id] = copies[c];
    }
    cache.BeginFrame(6);
    for (const uint32_t id : masked_entries) {
        cache.MarkDirty(id, 3);
        cache.MarkDirty(id, 4);
    }
    assert(cache.stats.stale_flags == 4 && cache.stats.probe_dirty == 2);
    for (uint32_t c = 1; c < 3; ++c) {
        DrawKey k = key;
        const auto hit = cache.Find(k, 0, false, false, 1);
        assert(hit.entry && !hit.rebuild);
        const Affine to_built =
            InverseRigid(InstanceTransform(IdentityAffine(), hit.entry->inv_view));
        assert(!Mismatch(fingerprint[hit.entry->id], to_built, copies[c]));
    }
}

void TestEvictForSpace() {
    BlockPool pool;
    pool.Reset(2048, 256);
    BlasCache cache;
    std::vector<std::pair<uint64_t, uint64_t>> retired;
    const auto release = [&](const CacheEntry& e) { retired.push_back({e.offset, e.bytes}); };
    const auto key = [](uint32_t i) {
        return DrawKey{.shader = 1, .indices = i, .index_count = 3, .instances = 1,
                       .vertex_count = 3};
    };
    cache.BeginFrame(1);
    for (uint32_t i = 0; i < 8; ++i) {
        DrawKey k = key(i);
        (void)cache.Find(k, 0, false, false, 1);
        const uint64_t offset = pool.Alloc(200);
        assert(offset == i * 256ull);
        cache.Insert(k, {.id = i, .offset = offset, .bytes = 200, .triangles = 1, .valid = true});
    }
    assert(pool.Alloc(1) == BlockPool::Fail);
    for (const uint32_t id : {1u, 3u, 5u}) {
        cache.Erase(id, release);
    }
    for (const auto& [offset, bytes] : retired) {
        pool.Free(offset, bytes);
    }
    retired.clear();
    assert(pool.Used() == 5 * 256 && pool.FreeBlocks() == 3);

    cache.BeginFrame(10);
    for (const uint32_t i : {0u, 2u}) {
        DrawKey k = key(i);
        assert(!cache.Find(k, 0, false, false, 1).rebuild);
    }
    assert(pool.Used() <= pool.Size() / 4 * 3);
    assert(pool.Alloc(700) == BlockPool::Fail);
    assert(cache.EvictForSpace(700, pool.Size(), 256, release));
    assert(retired.size() == 1 && retired[0].first == 1024);
    assert(cache.stats.space_evictions == 1 && cache.Size() == 4);
    assert(pool.Alloc(700) == BlockPool::Fail);

    cache.BeginFrame(11);
    for (const auto& [offset, bytes] : retired) {
        pool.Free(offset, bytes);
    }
    retired.clear();
    assert(pool.Alloc(700) == 768);
    assert(cache.ById(0) && cache.ById(2) && cache.ById(6) && cache.ById(7));
    DrawKey big = key(8);
    (void)cache.Find(big, 0, false, false, 1);
    cache.Insert(big, {.id = 8, .offset = 768, .bytes = 700, .triangles = 1, .valid = true});

    cache.BeginFrame(12);
    for (const uint32_t i : {0u, 2u, 6u, 7u, 8u}) {
        DrawKey k = key(i);
        (void)cache.Find(k, 0, false, false, 1);
    }
    assert(!cache.EvictForSpace(512, pool.Size(), 256, release));
    assert(retired.empty());
}

struct BuildSim {
    BlasCache cache;
    RebuildBudget budget;
    RefitBudget refits;
    uint32_t next_id = 0;
    uint64_t frame_rebuilt = 0, frame_refit = 0, current_frame = 0;

    void BeginFrame(uint64_t frame) {
        current_frame = frame;
        cache.BeginFrame(frame);
        budget.BeginFrame();
        refits.BeginFrame(frame);
        frame_rebuilt = 0;
        frame_refit = 0;
    }

    Step Draw(DrawKey key, uint64_t palette, bool skinned, uint32_t triangles) {
        const auto lookup = cache.Find(key, palette, skinned, false, triangles);
        const Step step = PlanStep(lookup, triangles, budget, refits);
        if (step == Step::Hit || step == Step::Hold || step == Step::Defer) {
            return step;
        }
        CacheEntry entry{};
        if (lookup.entry) {
            entry = *lookup.entry;
        } else {
            entry.id = next_id++;
            entry.triangles = triangles;
            entry.updatable = lookup.updatable;
        }
        entry.refits = RefitsAfter(step, lookup, entry.id);
        entry.palette = palette;
        entry.valid = true;
        entry.dirty = false;
        entry.built_frame = current_frame;
        cache.Insert(key, entry);
        if (step == Step::Rebuild) {
            frame_rebuilt += triangles;
        } else {
            frame_refit += triangles;
        }
        return step;
    }
};

DrawKey SimKey(uint64_t i) {
    return DrawKey{.shader = 9, .geometry = 9, .indices = 0x1000 + i, .index_count = 300,
                   .instances = 1, .vertex_count = 100};
}

void TestRefitStagger() {
    constexpr uint32_t Draws = 120;
    BuildSim sim;
    sim.budget.Reset(0);
    sim.BeginFrame(1);
    for (uint32_t i = 0; i < Draws; ++i) {
        assert(sim.Draw(SimKey(i), 1, true, 100) == Step::Rebuild);
    }
    std::vector<uint32_t> run(Draws, 0);
    uint32_t max_full = 0, total_full = 0;
    for (uint64_t f = 2; f < 2 + 3 * (MaxRefits + 1); ++f) {
        sim.BeginFrame(f);
        uint32_t full = 0;
        for (uint32_t i = 0; i < Draws; ++i) {
            const Step step = sim.Draw(SimKey(i), f, true, 100);
            assert(step == Step::Refit || step == Step::Rebuild);
            if (step == Step::Rebuild) {
                ++full;
                run[i] = 0;
            } else {
                ++run[i];
                assert(run[i] <= MaxRefits);
            }
        }
        max_full = std::max(max_full, full);
        total_full += full;
    }
    assert(max_full <= (Draws + MaxRefits - 1) / MaxRefits);
    assert(total_full >= 2 * Draws);

    BuildSim lockstep;
    lockstep.budget.Reset(0);
    lockstep.BeginFrame(1);
    for (uint32_t i = 0; i < Draws; ++i) {
        (void)lockstep.Draw(SimKey(i), 1, true, 100);
        lockstep.cache.ById(i)->refits = 0;
    }
    uint32_t lockstep_max = 0;
    for (uint64_t f = 2; f < 2 + MaxRefits + 1; ++f) {
        lockstep.BeginFrame(f);
        uint32_t full = 0;
        for (uint32_t i = 0; i < Draws; ++i) {
            full += lockstep.Draw(SimKey(i), f, true, 100) == Step::Rebuild;
        }
        lockstep_max = std::max(lockstep_max, full);
    }
    assert(lockstep_max == Draws);
    for (uint32_t id = 0; id < MaxRefits; ++id) {
        assert(RefitPhase(id) == id && RefitPhase(id + MaxRefits) == id);
    }
}

void TestRebuildBudget() {
    BuildSim sim;
    sim.budget.Reset(250);
    uint64_t frame = 1;
    uint32_t built = 0;
    for (; frame < 10 && built < 10; ++frame) {
        sim.BeginFrame(frame);
        built = 0;
        uint32_t deferred = 0;
        for (uint32_t i = 0; i < 10; ++i) {
            const Step step = sim.Draw(SimKey(i), 0, false, 100);
            assert(step == Step::Hit || step == Step::Rebuild || step == Step::Defer);
            if (step == Step::Defer) {
                ++deferred;
            } else {
                ++built;
            }
        }
        assert(sim.frame_rebuilt <= 250);
        assert(sim.cache.Size() == built);
        assert(frame != 1 || (built == 2 && deferred == 8));
    }
    assert(built == 10 && frame == 6);

    BuildSim big;
    big.budget.Reset(250);
    big.BeginFrame(1);
    assert(big.Draw(SimKey(0), 0, false, 1000) == Step::Rebuild);
    assert(big.Draw(SimKey(1), 0, false, 10) == Step::Defer);
    big.BeginFrame(2);
    assert(big.Draw(SimKey(0), 0, false, 1000) == Step::Hit);
    assert(big.Draw(SimKey(1), 0, false, 10) == Step::Rebuild);

    BuildSim unlimited;
    unlimited.budget.Reset(0);
    unlimited.BeginFrame(1);
    for (uint32_t i = 0; i < 10; ++i) {
        assert(unlimited.Draw(SimKey(i), 0, false, 1u << 20) == Step::Rebuild);
    }

    BuildSim limit;
    limit.budget.Reset(250);
    limit.BeginFrame(1);
    assert(limit.Draw(SimKey(0), 1, true, 100) == Step::Rebuild);
    limit.cache.ById(0)->refits = MaxRefits;
    limit.BeginFrame(2);
    assert(limit.Draw(SimKey(1), 0, false, 200) == Step::Rebuild);
    assert(limit.Draw(SimKey(0), 2, true, 100) == Step::Refit);
    assert(limit.cache.ById(0)->refits == MaxRefits + 1);
    assert(limit.Draw(SimKey(2), 0, false, 100) == Step::Defer);
    limit.BeginFrame(3);
    assert(limit.Draw(SimKey(0), 3, true, 100) == Step::Refit);
    assert(limit.Draw(SimKey(2), 0, false, 100) == Step::Rebuild);
    limit.BeginFrame(4);
    assert(limit.Draw(SimKey(0), 4, true, 100) == Step::Rebuild);
    assert(limit.cache.ById(0)->refits == 0);

    BuildSim dirty;
    dirty.budget.Reset(250);
    dirty.BeginFrame(1);
    assert(dirty.Draw(SimKey(0), 0, false, 100) == Step::Rebuild);
    dirty.BeginFrame(2);
    dirty.cache.MarkDirty(0, 2);
    assert(dirty.Draw(SimKey(1), 0, false, 200) == Step::Rebuild);
    assert(dirty.Draw(SimKey(0), 0, false, 100) == Step::Hold);
    assert(dirty.cache.ById(0) && dirty.cache.ById(0)->dirty);
    dirty.BeginFrame(3);
    assert(dirty.Draw(SimKey(0), 0, false, 100) == Step::Rebuild);
    assert(!dirty.cache.ById(0)->dirty);
}

}

void TestRefitBudget() {
    BuildSim sim;
    sim.refits.Reset(250, 3);
    sim.BeginFrame(1);
    for (uint32_t i = 0; i < 10; ++i) {
        assert(sim.Draw(SimKey(i), 1, true, 100) == Step::Rebuild);
    }
    std::array<uint64_t, 10> last_refit{};
    last_refit.fill(1);
    uint32_t forced_frames = 0;
    for (uint64_t frame = 2; frame <= 40; ++frame) {
        sim.BeginFrame(frame);
        uint32_t refit = 0, held = 0;
        for (uint32_t i = 0; i < 10; ++i) {
            const Step step = sim.Draw(SimKey(i), frame, true, 100);
            assert(step == Step::Refit || step == Step::Hold);
            if (step == Step::Refit) {
                ++refit;
                last_refit[i] = frame;
            } else {
                ++held;
                assert(frame - last_refit[i] < 2 * sim.refits.MaxHold());
            }
        }
        assert(sim.frame_rebuilt == 0);
        assert(refit + held == 10);
        assert(sim.frame_refit <= 2 * sim.refits.Budget());
        if (frame == 2) {
            assert(refit == 2 && held == 8);
        }
        assert(refit >= 2);
        forced_frames += refit > 2 ? 1u : 0u;
    }
    assert(forced_frames >= 20);

    BuildSim forced;
    forced.refits.Reset(250, 2);
    forced.BeginFrame(1);
    assert(forced.Draw(SimKey(0), 1, true, 1000) == Step::Rebuild);
    assert(forced.Draw(SimKey(1), 1, true, 10) == Step::Rebuild);
    assert(forced.Draw(SimKey(2), 1, true, 10) == Step::Rebuild);
    forced.BeginFrame(2);
    assert(forced.Draw(SimKey(0), 2, true, 1000) == Step::Refit);
    assert(forced.Draw(SimKey(1), 2, true, 10) == Step::Hold);
    assert(forced.Draw(SimKey(2), 2, true, 10) == Step::Hold);
    forced.BeginFrame(3);
    assert(forced.Draw(SimKey(0), 3, true, 1000) == Step::Refit);
    assert(forced.Draw(SimKey(1), 3, true, 10) == Step::Refit);
    assert(forced.Draw(SimKey(2), 3, true, 10) == Step::Hold);
    forced.BeginFrame(4);
    assert(forced.Draw(SimKey(0), 4, true, 1000) == Step::Refit);
    assert(forced.Draw(SimKey(1), 4, true, 10) == Step::Hold);
    assert(forced.Draw(SimKey(2), 4, true, 10) == Step::Refit);

    BuildSim unlimited;
    unlimited.refits.Reset(0, 3);
    unlimited.BeginFrame(1);
    assert(unlimited.Draw(SimKey(0), 1, true, 1000) == Step::Rebuild);
    assert(unlimited.Draw(SimKey(1), 1, true, 1000) == Step::Rebuild);
    unlimited.BeginFrame(2);
    assert(unlimited.Draw(SimKey(0), 2, true, 1000) == Step::Refit);
    assert(unlimited.Draw(SimKey(1), 2, true, 1000) == Step::Refit);
}

void TestLruKeepsLastFrame() {
    BlasCache cache;
    std::vector<uint32_t> released;
    const auto release = [&](const CacheEntry& e) { released.push_back(e.id); };
    cache.BeginFrame(5);
    for (uint32_t i = 0; i < 3; ++i) {
        DrawKey k = SimKey(i);
        (void)cache.Find(k, 0, false, false, 1);
        CacheEntry e{};
        e.id = i;
        e.valid = true;
        e.triangles = 1;
        cache.Insert(k, e);
    }
    cache.BeginFrame(6);
    {
        DrawKey k = SimKey(0);
        (void)cache.Find(k, 0, false, false, 1);
    }
    cache.BeginFrame(7);
    assert(cache.EvictLeastRecent(release));
    assert(released.size() == 1 && released[0] != 0);
    assert(cache.EvictLeastRecent(release));
    assert(released.size() == 2 && released[1] != 0);
    assert(!cache.EvictLeastRecent(release));
    assert(cache.Size() == 1);
}

void TestPickKept() {
    std::array<CacheEntry, 5> e{};
    for (uint32_t i = 0; i < e.size(); ++i) {
        e[i].id = i;
        e[i].triangles = 100;
        e[i].last_used = 50;
    }
    e[0].last_used = 10;
    e[1].last_used = 90;
    e[2].seen_camera = {5.0f, 0.0f, 0.0f};
    e[3].seen_camera = {1.0f, 0.0f, 0.0f};
    e[4].triangles = 1000;
    const std::array<float, 3> camera{};
    std::vector<CacheEntry*> all{&e[0], &e[1], &e[2], &e[3], &e[4]};
    KeepRules rules{};
    KeptPick open = PickKept(all, camera, rules);
    assert(open.picked.size() == 5 && open.triangles == 1400);
    assert(open.picked[0] == &e[1] && open.picked.back() == &e[0]);
    assert(open.picked[1] == &e[4] && open.picked[2] == &e[3] && open.picked[3] == &e[2]);
    rules.triangle_budget = 250;
    KeptPick budget = PickKept(all, camera, rules);
    assert(budget.picked.size() == 2 && budget.triangles == 200 && budget.over_budget == 3);
    assert(budget.picked[0] == &e[1] && budget.picked[1] == &e[3]);
    rules.triangle_budget = 0;
    rules.max_triangles = 500;
    KeptPick small = PickKept(all, camera, rules);
    assert(small.picked.size() == 4 && small.too_big == 1 && small.triangles == 400);
}

void TestKeepInstances() {
    KeepRules rules{};
    rules.distance = 10.0f;
    rules.skinned_frames = 3;
    rules.unused_frames = 120;
    rules.kept_unused_frames = 3600;
    CacheEntry e{};
    e.valid = true;
    e.seen = true;
    e.seen_camera = {0.0f, 0.0f, 0.0f};
    e.last_used = 100;
    const std::array<float, 3> near{3.0f, 4.0f, 0.0f};
    const std::array<float, 3> far{30.0f, 0.0f, 0.0f};
    assert(KeepInstance(e, 101, near, rules));
    assert(KeepInstance(e, 5000, near, rules));
    assert(!KeepInstance(e, 100, near, rules));
    assert(!KeepInstance(e, 101, far, rules));
    e.dirty = true;
    assert(!KeepInstance(e, 101, near, rules));
    e.dirty = false;
    e.seen = false;
    assert(!KeepInstance(e, 101, near, rules));
    e.seen = true;
    e.valid = false;
    assert(!KeepInstance(e, 101, near, rules));
    e.valid = true;
    // Seen by the kept probe in front of the drawn surface after its last draw: not kept, unless
    // the probe rule is off; drawn again afterwards, kept again.
    e.probe_hidden = 105;
    assert(!KeepInstance(e, 110, near, rules));
    rules.probe_hide = false;
    assert(KeepInstance(e, 110, near, rules));
    rules.probe_hide = true;
    e.last_used = 106;
    assert(KeepInstance(e, 110, near, rules));
    e.last_used = 100;
    e.probe_hidden = 0;
    // Short-lived (KeepShortLived): updatable and deforming (here skinned).
    e.updatable = true;
    e.skinned = true;
    assert(KeepInstance(e, 103, near, rules));
    assert(!KeepInstance(e, 104, near, rules));
    assert(UnusedLimit(e, near, rules) == 120);
    e.updatable = false;
    e.skinned = false;
    assert(RememberSurface(true, false));
    assert(RememberSurface(false, true));
    assert(!RememberSurface(false, false));
    assert(UnusedLimit(e, near, rules) == 3600);
    assert(UnusedLimit(e, far, rules) == 120);
    e.dirty = true;
    assert(UnusedLimit(e, near, rules) == 120);
    rules.distance = 0.0f;
    e.dirty = false;
    assert(!KeepInstance(e, 101, near, rules));
    assert(UnusedLimit(e, near, rules) == 120);

    BlasCache cache;
    std::vector<uint32_t> released;
    const auto release = [&](const CacheEntry& c) { released.push_back(c.id); };
    const auto key = [](uint32_t i) {
        return DrawKey{.shader = 7, .indices = i, .index_count = 3, .instances = 1,
                       .vertex_count = 3};
    };
    cache.BeginFrame(1);
    for (uint32_t i = 0; i < 3; ++i) {
        DrawKey k = key(i);
        (void)cache.Find(k, 0, false, false, 1);
        CacheEntry n{};
        n.id = i;
        n.valid = true;
        n.seen = true;
        n.seen_camera = {float(i) * 20.0f, 0.0f, 0.0f};
        cache.Insert(k, n);
    }
    rules.distance = 10.0f;
    cache.BeginFrame(500);
    cache.EvictUnusedBy([&](const CacheEntry& c) { return UnusedLimit(c, near, rules); }, release);
    assert(released.size() == 2 && cache.Size() == 1);
    std::vector<uint32_t> order;
    cache.ForEachOrdered([&](CacheEntry& c) { order.push_back(c.id); });
    assert(order.size() == 1 && order[0] == 0);
    cache.BeginFrame(4000);
    cache.EvictUnusedBy([&](const CacheEntry& c) { return UnusedLimit(c, near, rules); }, release);
    assert(cache.Size() == 0);
}

// A 3x4 row-major placement: rotation by `angle` about Y, uniform `scale`, translation t.
Affine Placement(float angle, float scale, float tx, float ty, float tz) {
    const float c = std::cos(angle) * scale, s = std::sin(angle) * scale;
    return {c, 0.0f, s, tx, 0.0f, scale, 0.0f, ty, -s, 0.0f, c, tz};
}

void TestPlausiblePlacement() {
    assert(PlausiblePlacement(Placement(0.0f, 1.0f, 0.0f, 0.0f, 0.0f).data()));
    assert(PlausiblePlacement(Placement(0.7f, 2.5f, -1200.0f, 35.0f, 880.0f).data()));
    assert(PlausiblePlacement(Placement(2.0f, 0.02f, 0.0f, 0.0f, 0.0f).data()));
    Affine m = Placement(0.3f, 1.0f, 1.0f, 2.0f, 3.0f);
    // Uniform scale within 1%.
    Affine scaled = m;
    scaled[1] *= 1.005f, scaled[5] *= 1.005f, scaled[9] *= 1.005f;
    assert(PlausiblePlacement(scaled.data()));
    scaled[1] *= 1.05f, scaled[5] *= 1.05f, scaled[9] *= 1.05f;
    assert(!PlausiblePlacement(scaled.data()));
    // Sheared: axis lengths equal within 1%, axes not orthogonal.
    Affine sheared = Placement(0.0f, 1.0f, 0.0f, 0.0f, 0.0f);
    sheared[1] = 0.05f;
    assert(!PlausiblePlacement(sheared.data()));
    Affine bad = m;
    bad[6] = std::nanf("");
    assert(!PlausiblePlacement(bad.data()));
    bad = m;
    bad[10] = std::numeric_limits<float>::infinity();
    assert(!PlausiblePlacement(bad.data()));
    assert(!PlausiblePlacement(Placement(0.3f, 1.0f, 2e5f, 0.0f, 0.0f).data()));
    assert(!PlausiblePlacement(Placement(0.3f, 0.005f, 0.0f, 0.0f, 0.0f).data()));
    assert(!PlausiblePlacement(Placement(0.3f, 200.0f, 0.0f, 0.0f, 0.0f).data()));
    const Affine zero{};
    assert(!PlausiblePlacement(zero.data()));
    // A projection's rows (non-uniform scale) and colour constants fail.
    const Affine projection{1.2f, 0, 0, 0, 0, 2.1f, 0, 0, 0, 0, 1.0001f, -0.1f};
    assert(!PlausiblePlacement(projection.data()));
    const Affine colours{0.5f, 0.4f, 0.3f, 1.0f, 0.2f, 0.2f, 0.2f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    assert(!PlausiblePlacement(colours.data()));
}

void TestPlacementId() {
    // 0.8 / 0.6 rotation, translation away from the 1 mm rounding edges.
    const Affine base{0.8f, 0.0f, 0.6f, 12.3454f, 0.0f, 1.0f, 0.0f, 4.5002f,
                      -0.6f, 0.0f, 0.8f, -7.2496f};
    const uint64_t id = PlacementId(base.data());
    assert(id != 0);
    // Float noise below the quantum (frame-to-frame re-upload, other passes) keeps the id.
    Affine noisy = base;
    for (uint32_t i = 0; i < PlacementFloats; ++i) {
        noisy[i] += (i % 4 == 3 ? 4e-5f : 2e-6f) * ((i & 1) ? 1.0f : -1.0f);
    }
    assert(PlacementId(noisy.data()) == id);
    // Placements 1 cm apart, on any axis, and a rotated copy are different ids.
    for (const uint32_t t : {3u, 7u, 11u}) {
        Affine moved = base;
        moved[t] += 0.01f;
        assert(PlacementId(moved.data()) != id);
    }
    Affine turned = base;
    turned[0] = turned[10] = 0.6f, turned[2] = 0.8f, turned[8] = -0.8f;
    assert(PlacementId(turned.data()) != id);
    const Affine zero{};
    assert(PlacementId(zero.data()) != 0);
}

void TestPlacementKey() {
    const DrawKey a{.shader = 1, .geometry = 2, .indices = 3, .index_count = 30, .instances = 1,
                    .vertex_count = 10, .placement = 0x1234};
    DrawKey b = a;
    assert(a == b && DrawKeyHash{}(a) == DrawKeyHash{}(b));
    b.placement = 0x1235;
    assert(!(a == b) && DrawKeyHash{}(a) != DrawKeyHash{}(b));
    DrawKey none = a;
    none.placement = 0;
    assert(!(a == none) && DrawKeyHash{}(a) != DrawKeyHash{}(none));
    // R7: with no placement the hash is bit for bit the one from before placements (BB_RT_
    // PLACEMENT=0 must not change the cache's buckets).
    const auto old_hash = [](const DrawKey& k) {
        uint64_t h = 0;
        const auto mix = [&](uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2); };
        mix(k.shader);
        mix(k.geometry);
        mix(k.indices);
        mix(k.topology);
        mix(uint64_t(k.index_count) << 32 | k.instances);
        mix(uint64_t(k.first_instance) << 32 | k.first_vertex);
        mix(uint64_t(k.vertex_count) << 32 | k.occurrence);
        return size_t(h);
    };
    DrawKey busy{.shader = 0xdeadbeef, .geometry = 0x1234567890ull, .indices = 77, .topology = 4,
                 .index_count = 900, .instances = 3, .first_instance = 5, .first_vertex = 6,
                 .vertex_count = 300, .occurrence = 2};
    assert(DrawKeyHash{}(none) == old_hash(none) && DrawKeyHash{}(busy) == old_hash(busy));

    // Frame 1 draws three placements of one mesh; frame 2 culls the first two. Keyed by
    // occurrence, the third would take the first one's entry (occurrence 0); keyed by
    // placement it finds its own.
    BlasCache cache;
    const std::array<uint64_t, 3> ids{0x11, 0x22, 0x33};
    cache.BeginFrame(1);
    for (uint32_t i = 0; i < 3; ++i) {
        DrawKey k = a;
        k.placement = ids[i];
        const auto miss = cache.Find(k, 0, false, false, 10);
        assert(!miss.entry && k.occurrence == 0);
        cache.Insert(k, {.id = 10 + i, .triangles = 10, .built_frame = 1, .valid = true});
    }
    cache.BeginFrame(2);
    DrawKey third = a;
    third.placement = ids[2];
    const auto hit = cache.Find(third, 0, false, false, 10);
    assert(hit.entry && !hit.rebuild && hit.entry->id == 12 && third.occurrence == 0);
    // The same placement drawn twice in one frame (another pass) keeps one entry per draw.
    DrawKey again = a;
    again.placement = ids[2];
    const auto second = cache.Find(again, 0, false, false, 10);
    assert(!second.entry && again.occurrence == 1);
    assert(cache.ById(12)->placement == ids[2]);
}

void TestPlacedNeverShelved() {
    BlasCache cache;
    const DrawKey plain{.shader = 4, .geometry = 5, .indices = 6, .index_count = 3,
                        .instances = 1, .vertex_count = 3};
    DrawKey placed = plain;
    placed.placement = 0x77;
    CacheEntry e{};
    assert(BlasCache::MayShelve(plain, e));
    assert(!BlasCache::MayShelve(placed, e));
    e.skinned = true;
    assert(!BlasCache::MayShelve(plain, e));

    cache.BeginFrame(1);
    DrawKey k1 = plain, k2 = placed;
    (void)cache.Find(k1, 0, false, false, 1);
    cache.Insert(k1, {.id = 1, .triangles = 1, .built_frame = 1, .valid = true});
    (void)cache.Find(k2, 0, false, false, 1);
    cache.Insert(k2, {.id = 2, .triangles = 1, .built_frame = 1, .valid = true});
    DrawKey shelf = plain;
    shelf.occurrence = BlasCache::ShelfBit | 1u;
    DrawKey placed_shelf = placed;
    placed_shelf.occurrence = BlasCache::ShelfBit | 1u;
    // A placed entry is never moved to the shelf, and nothing is shelved under a placed key.
    assert(!cache.Rekey(2, placed_shelf));
    assert(!cache.Rekey(2, shelf));
    assert(!cache.Rekey(1, placed_shelf));
    assert(cache.Rekey(1, shelf));
    const auto any = [](const CacheEntry&) { return true; };
    assert(cache.FindShelved(plain, any) && cache.FindShelved(plain, any)->id == 1);
    assert(!cache.FindShelved(placed, any));
    assert(cache.KeyOf(2) && cache.KeyOf(2)->occurrence == 0 && cache.KeyOf(2)->placement == 0x77);
}

void TestSamePlacementRule() {
    // Two detail levels of one piece: same vertex buffer (geometry), other index ranges.
    const DrawKey lod0{.shader = 1, .geometry = 9, .indices = 100, .index_count = 300,
                       .instances = 1, .vertex_count = 100, .placement = 0xabc};
    DrawKey lod1 = lod0;
    lod1.indices = 900;
    lod1.index_count = 90;
    lod1.vertex_count = 60;
    DrawnPlacements drawn;
    assert(drawn.Empty() && !drawn.Contains(lod1));
    drawn.Add(lod0, lod0.placement);
    // lod0 drawn this frame: a kept lod1 at the same placement is left out of the TLAS.
    assert(!drawn.Empty() && drawn.Contains(lod1));
    DrawKey elsewhere = lod1;
    elsewhere.placement = 0xabd;
    assert(!drawn.Contains(elsewhere));
    DrawKey other_mesh = lod1;
    other_mesh.geometry = 10;
    assert(!drawn.Contains(other_mesh));
    // Untrusted (occurrence-keyed) kept entries are never matched.
    DrawKey plain = lod1;
    plain.placement = 0;
    assert(!drawn.Contains(plain));
    // R6: a separate sub-mesh over the same vertex buffer and matrix (vertices 100..199) is
    // not hidden by lod0 (vertices 0..99); ranges that overlap are.
    DrawKey sub = lod0;
    sub.indices = 500;
    sub.first_vertex = 100;
    sub.vertex_count = 100;
    assert(!drawn.Contains(sub));
    sub.first_vertex = 99;
    assert(drawn.Contains(sub));
    assert(VertexRangesOverlap(0, 0, 500, 10) && !VertexRangesOverlap(0, 10, 10, 10));
    drawn.Clear();
    assert(drawn.Empty() && !drawn.Contains(lod1));
}

// R1: a draw that falls back to the occurrence key (pending shader, a moving mesh) still names
// its placements, so a kept placed entry of the same placement stays out of the TLAS.
void TestDrawnFallbackPlacements() {
    std::array<Affine, 2> m{Placement(0.1f, 1.0f, 1, 2, 3), Placement(0.9f, 1.0f, 40, 2, 3)};
    DrawInput fallback{};
    fallback.key = {.shader = 7, .geometry = 70, .indices = 71, .index_count = 30,
                    .instances = 2, .vertex_count = 10};
    fallback.instances = 2;
    fallback.placements = m[0].data();
    fallback.placement_count = 2;
    fallback.placement_static = false; // e.g. untrusted: keyed by occurrence this frame
    DrawnPlacements drawn;
    drawn.Add(fallback);
    DrawKey kept = fallback.key;
    kept.shader = 0;
    kept.instances = 1;
    kept.placement = PlacementId(m[1].data());
    assert(drawn.Contains(kept));
    kept.placement = PlacementId(Placement(0.9f, 1.0f, 41, 2, 3).data());
    assert(!drawn.Contains(kept));
    // A draw without matrices adds nothing.
    DrawInput none = fallback;
    none.placements = nullptr;
    DrawnPlacements empty;
    empty.Add(none);
    assert(empty.Empty());
}

// One BLAS per mesh: the BLAS built from placement A stands in for placement B once moved by
// Relocation(B, A), for any rotation and scale, and a shared mesh key counts no occurrence.
void TestSharedMesh() {
    const Affine a = Placement(0.7f, 0.75f, -212.5f, -40.4f, 69.7f);
    const Affine b = Placement(-1.3f, 1.1f, -255.0f, -34.8f, 116.7f);
    const Affine back = MultiplyAffine(InverseAffine(a), a);
    for (int i = 0; i < 12; ++i) {
        assert(std::fabs(back[size_t(i)] - (i % 5 == 0 ? 1.0f : 0.0f)) < 1e-4f);
    }
    const std::array<float, 3> model{3.0f, 7.0f, -2.0f};
    const std::array<float, 3> at_a = Transform(a, model), at_b = Transform(b, model);
    const std::array<float, 3> moved = Transform(Relocation(b, a), at_a);
    assert(std::fabs(moved[0] - at_b[0]) < 1e-3f && std::fabs(moved[1] - at_b[1]) < 1e-3f &&
           std::fabs(moved[2] - at_b[2]) < 1e-3f);
    const Affine same = Relocation(a, a);
    assert(std::fabs(same[0] - 1.0f) < 1e-4f && std::fabs(same[3]) < 1e-2f);
    // A matrix with no inverse gives the identity instead of NaNs.
    const Affine flat{};
    assert(InverseAffine(flat)[0] == 1.0f && InverseAffine(flat)[5] == 1.0f);

    BlasCache cache;
    cache.BeginFrame(10);
    DrawKey first{.shader = 0, .geometry = 5, .indices = 9, .index_count = 300, .instances = 1,
                  .vertex_count = 100, .placement = SharedMeshPlacement};
    DrawKey second = first;
    assert(cache.Find(first, 0, false, false, 100).entry == nullptr);
    CacheEntry e{};
    e.id = 3;
    e.triangles = 100;
    e.valid = true;
    cache.Insert(first, e);
    // The second placement of the mesh this frame is the same entry, not occurrence 1.
    assert(cache.Find(second, 0, false, false, 100).entry != nullptr && second.occurrence == 0);
}

// L1: a placed piece takes no palette, is never skinned and never short lived.
void TestPlacedInstance() {
    std::array<Affine, 3> m{Placement(0.1f, 1.0f, 1, 2, 3), Placement(0.2f, 1.0f, 4, 5, 6),
                            Placement(0.3f, 1.0f, 7, 8, 9)};
    DrawInput batch{};
    batch.key = {.shader = 0x6a833138, .geometry = 5, .indices = 6, .index_count = 30,
                 .instances = 3, .first_instance = 2, .vertex_count = 10};
    batch.palette = 0x1234;
    batch.skinned = true;
    batch.unreadable = true;
    batch.instances = 3;
    batch.store_base = 100;
    batch.vertex_count = 10;
    batch.placements = m[0].data();
    batch.placement_count = 3;
    const DrawInput one = PlacedInstance(batch, 2);
    assert(one.palette == 0 && !one.skinned && !one.unreadable && one.instances == 1);
    assert(one.store_base == 120 && one.placement_count == 1 && one.placements == m[2].data());
    assert(one.key.shader == 0 && one.key.instances == 1 && one.key.first_instance == 0);
    assert(one.key.placement == PlacementId(m[2].data()) && one.key.geometry == 5);
    // The same placement drawn alone (another shader, other buffers) is the same key.
    DrawInput single = batch;
    single.key.shader = 0x9da6aacb;
    single.key.instances = 1;
    single.key.first_instance = 0;
    single.instances = 1;
    single.palette = 0x9999;
    single.placements = m[2].data();
    single.placement_count = 1;
    assert(PlacedInstance(single, 0).key == one.key && PlacedInstance(single, 0).palette == 0);

    // Through the cache: a palette never rebuilds it, and its entry is never short lived.
    BlasCache cache;
    cache.BeginFrame(1);
    DrawKey k = one.key;
    const auto miss = cache.Find(k, one.palette, one.skinned, one.unreadable, 10);
    assert(!miss.entry && !miss.updatable);
    cache.Insert(k, {.id = 1, .triangles = 10, .built_frame = 1, .valid = true});
    for (uint64_t f = 2; f < 6; ++f) {
        cache.BeginFrame(f);
        const DrawInput d = f % 2 ? PlacedInstance(batch, 2) : PlacedInstance(single, 0);
        DrawKey again = d.key;
        const auto hit = cache.Find(again, d.palette, d.skinned, d.unreadable, 10);
        assert(hit.entry && !hit.rebuild && hit.reason == Rebuild::None);
    }
    CacheEntry e = *cache.ById(1);
    assert(e.placement && e.palette == 0 && !e.skinned && !e.updatable && !KeepShortLived(e));
    // Even an updatable, moving placed entry (the probe saw it change) is not short lived.
    e.updatable = e.moving = true;
    e.instances = 1;
    assert(!KeepShortLived(e));
    e.placement = 0;
    assert(KeepShortLived(e));
}

// L2 / R2: a mover (another placement every frame) is routed to the occurrence key after 4
// frames in a row; a static copy entering while another leaves is not.
void TestPlacementMovers() {
    const DrawKey mesh{.geometry = 11, .indices = 12, .index_count = 30, .instances = 1,
                       .vertex_count = 10};
    const uint64_t route = PlacementRouteKey(mesh);
    // Draws placement `id` this frame; a miss is noted as AddOne does.
    const auto draw = [&](BlasCache& cache, PlacementMovers& movers, uint64_t id,
                          uint32_t& next) {
        DrawKey k = mesh;
        k.placement = id;
        const auto found = cache.Find(k, 0, false, false, 10);
        if (!found.entry) {
            movers.NoteMiss(k);
            cache.Insert(k, {.id = next++, .triangles = 10, .built_frame = 0, .valid = true});
        }
    };
    {
        BlasCache cache;
        PlacementMovers movers;
        uint32_t next = 1;
        std::vector<uint64_t> moved, released;
        // Frame 1: the door at rest; then a new placement every frame.
        for (uint64_t f = 1; f <= 5; ++f) {
            cache.BeginFrame(f);
            draw(cache, movers, 0x1000 + f, next);
            moved.clear();
            movers.EndFrame(cache, f, moved, released);
            // Tells on frames 2, 3, 4, 5: routed at the 4th.
            assert(moved.empty() == (f < 5));
            assert(movers.Routed(route, f) == (f >= 5));
        }
        assert(moved.size() == 1 && moved[0] == route && released.empty());
        assert(movers.Routed(route, 5 + MoverRouteFrames) &&
               !movers.Routed(route, 6 + MoverRouteFrames));
        // The route runs out: the mesh is released (its occurrence-keyed entries go).
        moved.clear();
        cache.BeginFrame(6 + MoverRouteFrames);
        movers.EndFrame(cache, 6 + MoverRouteFrames, moved, released);
        assert(moved.empty() && released.size() == 1 && released[0] == route);
        assert(movers.routed.empty());
    }
    {
        // Static: copies A and B. Frame 2: B enters and A leaves (one tell); then hits only.
        BlasCache cache;
        PlacementMovers movers;
        uint32_t next = 1;
        std::vector<uint64_t> moved, released;
        const std::array<uint64_t, 8> seq{0xA, 0xB, 0xB, 0xB, 0xA, 0xB, 0xA, 0xB};
        for (uint64_t f = 1; f <= seq.size(); ++f) {
            cache.BeginFrame(f);
            draw(cache, movers, seq[f - 1], next);
            movers.EndFrame(cache, f, moved, released);
            assert(moved.empty() && !movers.Routed(route, f));
        }
        assert(next == 3);
        // New copies entering while others leave on up to 3 frames in a row: never routed.
        const std::array<uint64_t, 9> walk{0xC, 0xD, 0xE, 0xE, 0xF, 0x10, 0x11, 0x11, 0x12};
        for (uint64_t i = 0; i < walk.size(); ++i) {
            const uint64_t f = 9 + i;
            cache.BeginFrame(f);
            draw(cache, movers, walk[i], next);
            movers.EndFrame(cache, f, moved, released);
            assert(moved.empty() && !movers.Routed(route, f));
        }
        // A copy that enters while the last one stays drawn is no tell either.
        cache.BeginFrame(18);
        draw(cache, movers, 0x12, next);
        draw(cache, movers, 0x13, next);
        movers.EndFrame(cache, 18, moved, released);
        assert(moved.empty() && movers.streaks.empty());
    }
    assert(PlacementMeshKey(mesh) != PlacementMeshKey(DrawKey{.geometry = 11, .indices = 12}));
    DrawKey placed = mesh;
    placed.placement = 0x55;
    placed.occurrence = 3;
    assert(PlacementMeshKey(placed) == PlacementMeshKey(mesh) &&
           PlacementRouteKey(placed) == route);
}

// R8: a rejected shader erases only the placed entries it built; a flip through both verdicts
// inside one readback purges nothing; checks read after the switch went off count nothing.
void TestPlacementPurges() {
    BlasCache cache;
    cache.BeginFrame(1);
    const DrawKey base{.geometry = 1, .indices = 2, .index_count = 3, .instances = 1,
                       .vertex_count = 3};
    for (uint32_t i = 0; i < 4; ++i) {
        DrawKey k = base;
        k.placement = 0x10 + i;
        (void)cache.Find(k, 0, false, false, 1);
        CacheEntry n{.id = 1 + i, .triangles = 1, .built_frame = 1, .valid = true};
        n.placed_vs = i < 2 ? 0xAAu : 0xBBu;
        cache.Insert(k, n);
    }
    DrawKey plain = base;
    plain.shader = 0xAA;
    (void)cache.Find(plain, 0, false, false, 1);
    CacheEntry n{.id = 9, .triangles = 1, .built_frame = 1, .valid = true};
    n.placed_vs = 0xAA;
    cache.Insert(plain, n);
    assert(cache.ById(9)->placed_vs == 0 && cache.ById(1)->placed_vs == 0xAA);
    const auto none = [](const CacheEntry&) {};
    const size_t erased = cache.EraseIf(
        [](const DrawKey& k, const CacheEntry& e) { return k.placement && e.placed_vs == 0xAA; },
        none);
    assert(erased == 2 && !cache.ById(1) && !cache.ById(2) && cache.ById(3) && cache.ById(4) &&
           cache.ById(9));

    std::unordered_map<uint64_t, PlacementTrust> table;
    PlacementCheck c{};
    c.trust_key = 0x77;
    c.vs = 0xAA;
    const auto check = [&](PlacementFlips& flips, float error, uint32_t i) {
        c.geometry = 100 + i % 3;
        return RecordPlacementCheck(table, flips, true, c, error, 0.02f, 10 + i / 4);
    };
    // Switched off since the check was queued: nothing counted, nothing to purge.
    PlacementFlips off;
    assert(!RecordPlacementCheck(table, off, false, c, 0.0f, 0.02f, 1) && table.empty());
    off.Collect(table);
    assert(off.trusted.empty() && off.rejected.empty());
    // Pending -> Trusted -> Rejected inside one readback: neither purge.
    PlacementFlips both;
    for (uint32_t i = 0; i < PlacementTrustPasses; ++i) {
        (void)check(both, 0.0f, i);
    }
    assert(table[0x77].verdict == PlacementVerdict::Trusted);
    for (uint32_t i = 0; i < 3; ++i) {
        (void)check(both, 1.0f, 20 + i);
    }
    assert(table[0x77].verdict == PlacementVerdict::Rejected);
    both.Collect(table);
    assert(both.trusted.empty() && both.rejected.empty() && both.before.empty());
    // Pending -> Trusted in one readback, Trusted -> Rejected in the next: one purge each.
    table.clear();
    PlacementFlips first, second;
    for (uint32_t i = 0; i < PlacementTrustPasses; ++i) {
        (void)check(first, 0.0f, i);
    }
    first.Collect(table);
    assert(first.trusted.size() == 1 && first.trusted[0] == 0xAA && first.rejected.empty());
    for (uint32_t i = 0; i < 3; ++i) {
        (void)check(second, 1.0f, 20 + i);
    }
    second.Collect(table);
    assert(second.trusted.empty() && second.rejected.size() == 1 && second.rejected[0] == 0xAA);
}

void TestPlacementSettled() {
    KeepRules rules{};
    rules.distance = 10.0f;
    rules.unused_frames = 120;
    rules.kept_unused_frames = 3600;
    const std::array<float, 3> camera{};
    BlasCache cache;
    DrawKey key{.shader = 3, .geometry = 3, .indices = 3, .index_count = 3, .instances = 1,
                .vertex_count = 3, .placement = 0x55};
    cache.BeginFrame(1);
    (void)cache.Find(key, 0, false, false, 1);
    CacheEntry n{.id = 5, .triangles = 1, .built_frame = 1, .valid = true};
    n.seen = true;
    cache.Insert(key, n);
    // Drawn once only (a moving door gets a new placement every frame): never kept.
    CacheEntry& e = *cache.ById(5);
    assert(!e.settled && !KeepInstance(e, 2, camera, rules));
    // Unsettled: gone after a few unused frames, not 120 (a mover leaves one a frame).
    assert(UnusedLimit(e, camera, rules) == PlacementUnsettledFrames);
    assert(UnsettledLimit(e, 120) == PlacementUnsettledFrames && UnsettledLimit(e, 2) == 2);
    cache.BeginFrame(2);
    DrawKey k = key;
    const auto hit = cache.Find(k, 0, false, false, 1);
    assert(hit.entry && hit.entry->settled);
    assert(KeepInstance(e, 3, camera, rules) && UnusedLimit(e, camera, rules) == 3600);
    // A rebuild keeps it settled; an occurrence-keyed entry never needs it.
    cache.Insert(k, *hit.entry);
    assert(cache.ById(5)->settled);
    DrawKey plain = key;
    plain.placement = 0;
    cache.BeginFrame(3);
    (void)cache.Find(plain, 0, false, false, 1);
    cache.Insert(plain, {.id = 6, .triangles = 1, .built_frame = 3, .valid = true});
    CacheEntry& q = *cache.ById(6);
    q.seen = true;
    assert(!q.settled && !q.placement && KeepInstance(q, 4, camera, rules));
}

void TestPlacementBuffers() {
    std::array<Affine, 4> m{Placement(0.1f, 1.0f, 1, 2, 3), Placement(0.2f, 1.0f, 4, 5, 6),
                            Placement(0.3f, 1.0f, 7, 8, 9), Placement(0.4f, 1.0f, 0, 0, 0)};
    const float* rows = m[0].data();
    const Affine colours{0.5f, 0.4f, 0.3f, 1.0f, 0.2f, 0.2f, 0.2f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    // The camera block is never a candidate, even with a rigid matrix first.
    assert(!PlacementBufferFits({0, CameraBlockBytes, rows}, 1));
    assert(PlacementBufferFits({0, 256, rows}, 1));
    assert(!PlacementBufferFits({0, 256, nullptr}, 1));
    assert(!PlacementBufferFits({0, 32, rows}, 1));
    // Instanced: exactly one plausible matrix per instance.
    assert(PlacementBufferFits({0, 3 * PlacementBytes, rows}, 3));
    assert(!PlacementBufferFits({0, 4 * PlacementBytes, rows}, 3));
    assert(!PlacementBufferFits({0, 2 * PlacementBytes, rows}, 3));
    m[1][0] = 3.0f;
    assert(!PlacementBufferFits({0, 3 * PlacementBytes, rows}, 3));
    assert(PlacementBufferFits({0, 3 * PlacementBytes, rows}, 1));
    m[1] = Placement(0.2f, 1.0f, 4, 5, 6);
    const std::array<PlacementBuffer, 3> list{PlacementBuffer{0, CameraBlockBytes, rows},
                                              PlacementBuffer{1, 48, colours.data()},
                                              PlacementBuffer{2, 3 * PlacementBytes, rows}};
    assert(ChoosePlacementBuffer(list, 3) == 2);
    assert(ChoosePlacementBuffer(list, 1) == 2);
    assert(ChoosePlacementBuffer(std::span(list).first(2), 1) == -1);
    assert(PlacementSlotKey(7, true) != PlacementSlotKey(7, false));
    // R3: 864 bytes is the camera block only for a single draw; 18 instances x 48 = 864.
    std::vector<Affine> batch(18);
    for (uint32_t i = 0; i < batch.size(); ++i) {
        batch[i] = Placement(0.1f * i, 1.0f, float(i), 0, 0);
    }
    assert(18 * PlacementBytes == CameraBlockBytes);
    assert(PlacementBufferFits({0, CameraBlockBytes, batch[0].data()}, 18));
    assert(!PlacementBufferFits({0, CameraBlockBytes, batch[0].data()}, 1));
    // R4: a single draw never takes a skeleton (a bone palette with a rigid first bone fits).
    PlacementBuffer bones{0, 1024, batch[0].data(), true};
    assert(!PlacementBufferFits(bones, 1) && !PlacementBufferReadable(bones, 1));
    bones.skeleton = false;
    assert(PlacementBufferFits(bones, 1));
    // R3: once chosen, any finite matrix gives an id (a non-uniformly scaled part).
    m[1][0] = 3.0f;
    const PlacementBuffer three{2, 3 * PlacementBytes, rows};
    assert(!PlacementBufferFits(three, 3) && PlacementBufferReadable(three, 3));
    m[1][5] = std::nanf("");
    assert(!PlacementBufferReadable(three, 3));
    assert(!PlacementBufferReadable({2, 4 * PlacementBytes, rows}, 3));
    assert(!PlacementBufferReadable({2, 256, nullptr}, 1));
}

void TestPlacementTrust() {
    assert(Near(PlacementTolerance(0.0f), 0.02f) && Near(PlacementTolerance(100.0f), 0.12f));
    // Checks spread over frames 10, 11, ... (4 a frame) and meshes 100..102 in turn.
    const auto rec = [](PlacementTrust& t, float error, uint32_t i) {
        return t.Record(error, 0.02f, 10 + i / 4, 100 + i % 3);
    };
    PlacementTrust good{};
    for (uint32_t i = 1; i < PlacementTrustPasses; ++i) {
        assert(!rec(good, 0.001f, i) && good.verdict == PlacementVerdict::Pending);
    }
    assert(rec(good, 0.001f, PlacementTrustPasses) && good.verdict == PlacementVerdict::Trusted);
    assert(good.pass_frames >= 2 && good.geometry_count == 3);
    // One outlier among many agreeing checks keeps a trusted shader trusted.
    assert(!rec(good, 0.5f, 20) && good.verdict == PlacementVerdict::Trusted);
    // A wrong buffer fails nearly always: rejected for good.
    PlacementTrust wrong{};
    assert(!rec(wrong, 3.0f, 0) && !rec(wrong, std::nanf(""), 1));
    assert(rec(wrong, 0.03f, 2) && wrong.verdict == PlacementVerdict::Rejected);
    for (uint32_t i = 0; i < 100; ++i) {
        assert(!rec(wrong, 0.0f, i));
    }
    assert(wrong.verdict == PlacementVerdict::Rejected && Near(wrong.worst, 3.0f));
    // A trusted shader that starts failing is rejected (its placed entries are purged).
    assert(!rec(good, 1.0f, 21));
    assert(rec(good, 1.0f, 22));
    assert(good.fails == 3 && good.passes == 8 && good.verdict == PlacementVerdict::Rejected);
    // One early outlier: trusted after 8 passes per failure plus 8.
    PlacementTrust outlier{};
    (void)rec(outlier, 0.5f, 0);
    for (uint32_t i = 0; i < 15; ++i) {
        assert(!rec(outlier, 0.0f, i));
    }
    assert(rec(outlier, 0.0f, 15) && outlier.verdict == PlacementVerdict::Trusted);
    assert(PlacementTrustKey(1, 2) != PlacementTrustKey(1, 3));
    // R5: the same draws every frame are not enough. 100 passes on one frame, or on many
    // frames from two meshes only, stay pending; the third mesh (on a later frame) trusts.
    PlacementTrust one_frame{};
    for (uint32_t i = 0; i < 100; ++i) {
        assert(!one_frame.Record(0.0f, 0.02f, 7, 100 + i % 3));
    }
    assert(one_frame.verdict == PlacementVerdict::Pending && one_frame.pass_frames == 1);
    assert(one_frame.Record(0.0f, 0.02f, 8, 100) &&
           one_frame.verdict == PlacementVerdict::Trusted);
    PlacementTrust two_meshes{};
    for (uint32_t i = 0; i < 100; ++i) {
        assert(!two_meshes.Record(0.0f, 0.02f, 7 + i, 100 + i % 2));
    }
    assert(two_meshes.verdict == PlacementVerdict::Pending && two_meshes.geometry_count == 2);
    assert(two_meshes.Record(0.0f, 0.02f, 200, 555) &&
           two_meshes.verdict == PlacementVerdict::Trusted);
    // Failures count no frame or mesh.
    PlacementTrust failing{};
    (void)failing.Record(1.0f, 0.02f, 1, 1);
    assert(failing.pass_frames == 0 && failing.geometry_count == 0);
    // The probe's reported sample turns with the frame (rt_probe.comp bits 8-9 of placement),
    // leaving bit 0 (may shelve) alone.
    for (uint64_t f = 0; f < 8; ++f) {
        const uint32_t bits = PlacementProbeRotation(f);
        assert((bits & 0xFFu) == 0 && ((bits >> ProbeFirstSampleShift) & 3u) == f % ProbeSamples);
    }
}

void TestProbeModel() {
    // Positions (v, 2v, 3v) with a 16-byte stride; draw of indices 100..111 at store 50.
    std::vector<float> stream(4 * 120);
    for (uint32_t v = 0; v < 120; ++v) {
        stream[4 * v] = float(v), stream[4 * v + 1] = 2.0f * v, stream[4 * v + 2] = 3.0f * v;
        stream[4 * v + 3] = 1.0f;
    }
    const std::array<uint16_t, 12> idx{100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111};
    DrawInput in{.store_base = 50, .first_vertex = 100, .vertex_count = 12, .instances = 1,
                 .base_vertex = 0, .indices = idx.data(), .index_count = 12, .index_size = 2};
    in.positions = reinterpret_cast<const uint8_t*>(stream.data());
    in.position_stride = 16;
    in.position_vertices = 120;
    const auto vertices = ProbeVertices(in);
    assert(vertices[0] == 100 && vertices[1] == 103 && vertices[2] == 106 && vertices[3] == 109);
    PlacementCheck c{};
    c.mask = ReadProbeModel(in, c.model);
    assert(c.mask == 0xF && Near(c.model[1][0], 103.0f) && Near(c.model[1][2], 309.0f));
    // Vertices past the stream are left out.
    in.position_vertices = 105;
    std::array<std::array<float, 3>, ProbeSamples> part{};
    assert(ReadProbeModel(in, part) == 0x3);
    in.positions = nullptr;
    assert(ReadProbeModel(in, part) == 0);
    // The matrix times the model position must land on the probe's world position.
    const Affine m = Placement(0.5f, 1.0f, 10.0f, 0.0f, -4.0f);
    std::copy(m.begin(), m.end(), c.matrix.begin());
    const auto at = Transform(m, c.model[2]);
    assert(PlacementError(c, 2, at) < 1e-3f);
    const std::array<float, 3> off{at[0] + 0.03f, at[1], at[2]};
    assert(PlacementError(c, 2, off) > PlacementTolerance(0.0f));
    c.mask = 0x1;
    assert(std::isnan(PlacementError(c, 2, at)));
}

static void TestPickHelpers() {
    std::array<float, 2> at{};
    assert(ParsePick("0.80,0.45", at) && std::fabs(at[0] - 0.8f) < 1e-6f && std::fabs(at[1] - 0.45f) < 1e-6f);
    assert(!ParsePick(nullptr, at));
    assert(!ParsePick("", at));
    assert(!ParsePick("0.5", at));
    assert(!ParsePick("1.5,0.2", at));
    assert(!ParsePick("0.2,x", at));
    const auto list = ParseHashList("52738b8a, 0x9e3779b9;ddbe557e");
    assert(list.size() == 3 && list[0] == 0x52738b8aull && list[1] == 0x9e3779b9ull);
    assert(ParseHashList(nullptr).empty());
    assert(HashListed(list, 0x52738b8aull, 0));
    assert(HashListed(list, 0, 0xddbe557eull));
    assert(!HashListed(list, 0, 0));
    assert(!HashListed(list, 1, 2));
    assert(AlphaUnresolved(true, false, true));
    assert(AlphaUnresolved(true, true, false));
    assert(!AlphaUnresolved(true, true, true));
    assert(!AlphaUnresolved(false, false, false));
    assert(InstanceMask(false, false, true, false) == MaskSolid);
    assert(InstanceMask(false, false, true, true) == 0);
    assert(InstanceMask(false, true, false, true) == MaskBlended);
    assert(InstanceMask(true, false, false, false) == 0);
    assert(InstanceMask(false, false, false, false, true) == MaskKept);
    assert((InstanceMask(false, false, false, false, true) & MaskSolid) == 0);
    assert(InstanceMask(true, false, false, false, true) == 0);
    assert(InstanceMask(false, false, true, true, true) == 0);
    assert(InstanceState(true, true, MaskSolid) != InstanceState(true, false, MaskSolid));
    assert(InstanceState(false, false, 0) != InstanceState(false, false, MaskSolid));
    assert(InstanceState(false, false, 0) != 0);
    assert(PickRasterCode(false, true, -5.0f, -5.0f) == PickRaster::Offscreen);
    assert(PickRasterCode(true, false, 0.0f, -5.0f) == PickRaster::Ghost);
    assert(PickRasterCode(true, true, -5.05f, -5.0f) == PickRaster::Visible);
    assert(PickRasterCode(true, true, -2.0f, -5.0f) == PickRaster::Behind);
    assert(PickRasterCode(true, true, -9.0f, -5.0f) == PickRaster::Ghost);
}

// The geometry cache file: a map of two placements of one model, read back.
static void TestGeoCache() {
    const float box[6] = {-1.0f, -2.0f, -3.0f, 1.0f, 2.0f, 3.0f};
    assert(BoxDistance(box, {0.0f, 0.0f, 0.0f}) == 0.0f);
    assert(Near(BoxDistance(box, {4.0f, 0.0f, 0.0f}), 3.0f));
    assert(Near(BoxDistance(box, {4.0f, 6.0f, 0.0f}), 5.0f));

    GeoMesh solid{};
    solid.index_count = 96;
    assert(GeoCaster(solid, 32) && !GeoCaster(solid, 33));
    solid.flags = GeoMeshAlphaTest;
    assert(!GeoCaster(solid, 1));
    solid.flags = GeoMeshMoon;
    assert(!GeoCaster(solid, 1));
    GeoPlace plain{};
    assert(GeoPlaceable(plain));
    plain.flags = GeoPlaceEvent;
    assert(!GeoPlaceable(plain));
    plain.flags = GeoPlaceSky;
    assert(!GeoPlaceable(plain));

    GeoHeader header{};
    std::memcpy(header.magic, "BBGEO1", 6);
    header.version = GeoVersion;
    header.models = 1;
    header.meshes = 1;
    header.places = 2;
    header.collisions = 3;
    GeoModel model{};
    std::memcpy(model.name, "m000001", 7);
    model.mesh_count = 1;
    GeoMesh mesh{};
    mesh.vertex_count = 3;
    mesh.index_count = 3;
    mesh.lod0_indices = 300;
    mesh.lod1_indices = 30;
    mesh.lod2_indices = 3;
    GeoPlace places[2]{};
    const Affine at_a = Placement(0.0f, 1.0f, 10.0f, 2.0f, -4.0f);
    const Affine at_b = Placement(1.2f, 1.0f, -216.004f, -30.0f, 64.0f);
    std::copy(at_a.begin(), at_a.end(), places[0].m);
    std::copy(at_b.begin(), at_b.end(), places[1].m);
    places[0].groups[0] = 0x1; // shown from collision 0 only
    places[1].groups[0] = 0x2; // shown from collisions 1 and 2
    GeoGroups collisions[3]{};
    collisions[0][0] = 0x1;
    collisions[1][0] = 0x2;
    collisions[2][0] = 0x6;
    const float vertices[9] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    const uint32_t indices[3] = {2, 1, 7}; // 7 is out of range: read as vertex 0
    header.vertex_at = sizeof(header) + sizeof(model) + sizeof(mesh) + sizeof(places) +
                       sizeof(collisions);
    header.index_at = header.vertex_at + sizeof(vertices);
    header.size = header.index_at + sizeof(indices);
    const std::string path = "rt_geometry_test.bbgeo";
    std::FILE* f = std::fopen(path.c_str(), "wb");
    assert(f);
    std::fwrite(&header, sizeof(header), 1, f);
    std::fwrite(&model, sizeof(model), 1, f);
    std::fwrite(&mesh, sizeof(mesh), 1, f);
    std::fwrite(places, sizeof(places), 1, f);
    std::fwrite(collisions, sizeof(collisions), 1, f);
    std::fwrite(vertices, sizeof(vertices), 1, f);
    std::fwrite(indices, sizeof(indices), 1, f);
    std::fclose(f);
    {
        GeoMap map;
        assert(map.Open(path) && map.Valid() && map.name == path);
        assert(map.places.size() == 2 && map.collisions.size() == 3);
        // The game's matrix differs from the file's in the fourth decimal.
        Affine drawn = at_b;
        drawn[3] += 0.004f;
        drawn[0] += 0.0005f;
        assert(map.FindPlace(at_a) == 0 && map.FindPlace(drawn) == 1);
        drawn[3] += 0.05f;
        assert(map.FindPlace(drawn) == -1);
        Affine turned = at_a;
        turned[0] += 0.01f;
        assert(map.FindPlace(turned) == -1);
        // A draw is matched to its file mesh by the index count of any detail level.
        assert(map.FindMesh(1, 300) == 0 && map.FindMesh(1, 30) == 0 && map.FindMesh(1, 3) == 0);
        assert(map.FindMesh(1, 31) == -1 && map.FindMesh(1, 0) == -1 && map.FindMesh(2, 3) == -1);
        float soup[9];
        std::vector<float> scratch_v;
        std::vector<uint32_t> scratch_i;
        assert(map.ReadSoup(0, soup, scratch_v, scratch_i));
        assert(soup[0] == 0.0f && soup[1] == 1.0f && soup[3] == 1.0f && soup[4] == 0.0f);
        assert(soup[6] == 0.0f && soup[7] == 0.0f && soup[8] == 0.0f);
        assert(!map.ReadSoup(1, soup, scratch_v, scratch_i));

        // Which collision the player is on, from what the game draws.
        GeoVisible visible;
        visible.Begin(map);
        visible.Update(map, 10);
        assert(visible.Fitting() == 0 && !visible.Shows(map, map.places[0]));
        visible.Note(1, 10);
        visible.Update(map, 11);
        // Collisions 1 and 2 both show the drawn piece: both fit, and both show placement 1.
        assert(visible.Fitting() == 2 && visible.best_misses == 0);
        assert(visible.Shows(map, map.places[1]) && !visible.Shows(map, map.places[0]));
        // The drawn piece is forgotten once it has not been drawn for RecentFrames.
        visible.Update(map, 11 + GeoVisible::RecentFrames);
        assert(visible.Fitting() == 0 && !visible.Shows(map, map.places[1]));
        visible.Note(0, 500);
        visible.Update(map, 500);
        assert(visible.Fitting() == 1 && visible.Shows(map, map.places[0]) &&
               !visible.Shows(map, map.places[1]));

        // A map moved from keeps no file.
        GeoMap moved = std::move(map);
        assert(moved.Valid() && !map.Valid());
    }
    // Another version is refused.
    header.version = GeoVersion + 1;
    f = std::fopen(path.c_str(), "wb");
    assert(f);
    std::fwrite(&header, sizeof(header), 1, f);
    std::fclose(f);
    GeoMap old;
    assert(!old.Open(path) && !old.Valid());
    assert(!old.Open("no_such_file.bbgeo"));
    std::remove(path.c_str());
}

// An entry is found by its id until it is erased; an entry replaced under another id is not
// found under the old one.
static void TestCacheById() {
    BlasCache cache;
    const auto release = [](const CacheEntry&) {};
    DrawKey a{};
    a.index_count = 30;
    DrawKey b = a;
    b.occurrence = 1;
    cache.Insert(a, {.id = 5, .offset = 0, .bytes = 512, .triangles = 10, .valid = true});
    cache.Insert(b, {.id = 9, .offset = 512, .bytes = 512, .triangles = 10, .valid = true});
    assert(cache.ById(5) && cache.ById(5)->id == 5 && cache.ById(9) && cache.ById(9)->id == 9);
    assert(!cache.ById(0) && !cache.ById(7) && !cache.ById(1000));
    assert(cache.KeyOf(9) && cache.KeyOf(9)->occurrence == 1);
    cache.Insert(a, {.id = 6, .offset = 0, .bytes = 512, .triangles = 10, .valid = true});
    assert(!cache.ById(5) && cache.ById(6) && cache.ById(6)->id == 6);
    cache.Erase(6, release);
    assert(!cache.ById(5) && !cache.ById(6) && cache.ById(9));
    cache.Clear(release);
    assert(!cache.ById(9));
}

// The sky the game draws, from its cloud draws' vertices (rt_sky_finder.h).
static void TestSkyFinder() {
    using Samples = std::array<std::array<float, 3>, 4>;
    const auto samples = [](const SkyTableDraw& d) {
        Samples s{};
        for (size_t k = 0; k < 4; ++k) {
            s[k] = {d.samples[k * 3], d.samples[k * 3 + 1], d.samples[k * 3 + 2]};
        }
        return s;
    };
    // The generated table against a draw logged on the Central Yharnam bridge under the blood
    // moon (264 indices, the positions as the game's vertex buffer held them).
    {
        SkyFinder finder;
        assert(finder.Update(1) == -1 && finder.Sky() == -1);
        std::array<float, 3> moon{};
        assert(!finder.Moon(moon));
        const Samples live{{{138.733f, 166.126f, -390.607f},
                            {319.723f, -114.662f, -715.598f},
                            {484.058f, -38.656f, 569.687f},
                            {756.842f, -114.662f, 114.305f}}};
        assert(finder.Draw(10, 264, live));
        assert(finder.Update(10) >= 0 && finder.Ties() == 1);
        assert(std::strcmp(finder.Name(), "m24_01 m900030_0000") == 0);
        assert(finder.Moon(moon));
        assert(std::fabs(moon[0] - 346.53f) < 0.01f && std::fabs(moon[1] - 269.24f) < 0.01f &&
               std::fabs(moon[2] + 132.98f) < 0.01f);
        // Another index count with the same vertices, or the same count a metre off: unknown.
        assert(!finder.Draw(10, 265, live));
        Samples off = live;
        off[2][0] += 1.0f;
        assert(!finder.Draw(10, 264, off));
        // The table is sorted by index count, and every sky index is in range.
        for (size_t i = 0; i < std::size(SkyTableDraws); ++i) {
            assert(SkyTableDraws[i].sky < std::size(SkyTableSkies));
            assert(SkyTableDraws[i].place < std::size(SkyTablePlaces));
            assert(i == 0 || SkyTableDraws[i - 1].indices <= SkyTableDraws[i].indices);
        }
        // No draws for a while: the choice stays. A reset drops it.
        assert(finder.Update(10 + SkyFinder::Window * 3) >= 0);
        finder.Reset();
        assert(finder.Update(20) == -1 && !finder.Moon(moon));
    }
    // Skies that share a mesh. A and B stand at the same place; C is A's model placed 500 m
    // away with another moon; D is B again with B's moon.
    const SkyTableSky skies[] = {
        {"A", {100.0f, 200.0f, 300.0f}, true}, {"B", {-400.0f, 250.0f, 0.0f}, true},
        {"C", {900.0f, 200.0f, 300.0f}, true}, {"D", {-400.0f, 250.0f, 0.5f}, true},
        {"E", {0.0f, 0.0f, 0.0f}, false},
        // A sky without a moon, the part that is its moon, and a moon of another map.
        {"m99_00 F", {0.0f, 0.0f, 0.0f}, false},
        {"m99_00 G", {5.0f, 600.0f, 7.0f}, true},
        {"m98_00 H", {8.0f, 700.0f, 9.0f}, true},
    };
    // Place 0: the origin. Place 1: 500 m along x.
    const SkyTablePlace places[] = {{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}},
                                    {{1, 0, 0, 500, 0, 1, 0, 0, 0, 0, 1, 0}}};
    const SkyTableDraw draws[] = {
        // 300 indices: the mesh A, B, C and D share (mesh 0; C's samples are A's, model space).
        {300, 0, 0, 0, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}},
        {300, 1, 0, 0, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}},
        {300, 2, 0, 1, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}},
        {300, 3, 0, 0, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}},
        // 300 indices again: E's only mesh.
        {300, 4, 0, 0, {9, 9, 9, 8, 8, 8, 7, 7, 7, 6, 6, 6}},
        // 480: A's own mesh (and C's), then B's (and D's), two detail levels of B's below.
        {480, 0, 1, 0, {20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31}},
        {480, 1, 1, 0, {40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51}},
        {480, 2, 1, 1, {20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31}},
        {480, 3, 1, 0, {40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51}},
        {90, 1, 1, 0, {40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51}},
        {90, 3, 1, 0, {40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51}},
        // F's two cloud meshes, G's halo, H's halo.
        {700, 5, 0, 0, {60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71}},
        {710, 5, 1, 0, {72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83}},
        {720, 6, 0, 0, {84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95}},
        {730, 7, 0, 0, {96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107}},
    };
    // The finder wants the draws sorted by index count.
    std::vector<SkyTableDraw> sorted(std::begin(draws), std::end(draws));
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const SkyTableDraw& a, const SkyTableDraw& b) { return a.indices < b.indices; });
    const auto row = [&](uint32_t indices, uint16_t sky) {
        for (const SkyTableDraw& d : sorted) {
            if (d.indices == indices && d.sky == sky) {
                return samples(d);
            }
        }
        assert(false);
        return Samples{};
    };
    SkyFinder finder{skies, places, sorted};
    std::array<float, 3> moon{};
    // The shared mesh alone fits four skies with different moons: no answer.
    assert(finder.Draw(1, 300, row(300, 0)));
    assert(finder.Update(1) == -1 && finder.Ties() == 4 && !finder.Moon(moon));
    // Where the draw stands settles A or C (B and D stand with A): still two moons.
    finder.Placed(1, 300, 2, {7.0f, 8.0f, 9.0f});
    assert(finder.Update(1) == -1);
    // B's own mesh: B and D fit two meshes each, and their moons are one (half a metre apart).
    assert(finder.Draw(2, 480, row(480, 1)));
    assert(finder.Update(2) == 1 && finder.Ties() == 2 && finder.Moon(moon) && moon[0] == -400.0f);
    // Another detail level of that mesh adds nothing to the count.
    assert(finder.Draw(3, 90, row(90, 1)));
    assert(finder.Update(3) == 1);
    // A's own mesh appears while B's is still fresh: three skies tie on two meshes; B stays.
    assert(finder.Draw(4, 480, row(480, 0)));
    finder.Placed(4, 480, 0, {20.0f, 21.0f, 22.0f});
    assert(finder.Update(4) == 1);
    // B's mesh is no longer drawn: once it has left the window, A's draws win, and of A and C
    // the one that stands where the draws are.
    const uint64_t later = 4 + SkyFinder::Window + 10;
    assert(finder.Draw(later, 300, row(300, 0)) && finder.Draw(later, 480, row(480, 0)));
    finder.Placed(later, 480, 0, {20.0f, 21.0f, 22.0f});
    assert(finder.Update(later) == 0 && finder.Moon(moon) && moon[0] == 100.0f);
    // The same model 500 m away is C.
    finder.Reset();
    assert(finder.Draw(1, 300, row(300, 0)) && finder.Draw(1, 480, row(480, 0)));
    finder.Placed(1, 480, 1, {523.0f, 24.0f, 25.0f});
    assert(finder.Update(1) == 2 && finder.Moon(moon) && moon[0] == 900.0f);
    // A probe 50 m off places nothing.
    finder.Reset();
    assert(finder.Draw(1, 300, row(300, 0)) && finder.Draw(1, 480, row(480, 0)));
    finder.Placed(1, 480, 1, {73.0f, 24.0f, 25.0f});
    assert(finder.Update(1) == -1);
    // A sky without a moon is an answer with no moon.
    finder.Reset();
    assert(finder.Draw(1, 300, row(300, 4)));
    assert(finder.Update(1) == 4 && !finder.Moon(moon) && std::strcmp(finder.Name(), "E") == 0);
    // A sky with no moon of its own: the moon is the part of its map whose draw was seen, and
    // stays once that draw is gone; another map's moon part is not taken.
    finder.Reset();
    assert(finder.Draw(1, 700, row(700, 5)) && finder.Draw(1, 710, row(710, 5)));
    assert(finder.Draw(1, 730, row(730, 7)));
    assert(finder.Update(1) == 5 && !finder.Moon(moon) && finder.MoonPart()[0] == 0);
    assert(finder.Draw(2, 720, row(720, 6)));
    assert(finder.Update(2) == 5 && finder.Moon(moon) && moon[1] == 600.0f);
    assert(std::strcmp(finder.MoonPart(), "m99_00 G") == 0);
    const uint64_t gone = 2 + SkyFinder::Window + 5;
    assert(finder.Draw(gone, 700, row(700, 5)) && finder.Draw(gone, 710, row(710, 5)));
    assert(finder.Update(gone) == 5 && finder.Moon(moon) && moon[1] == 600.0f);
    finder.Reset();
    assert(finder.Draw(1, 700, row(700, 5)));
    assert(finder.Update(1) == 5 && !finder.Moon(moon));
}

// The per-instance blocks of the game's tessellated meshes: 208 bytes an instance, the world
// matrix first (rt_placement.h ReadPatchMatrices).
static void TestPatchMatrices() {
    constexpr uint32_t Floats = PatchInstanceBytes / 4;
    std::vector<float> buffer(16 * Floats, 0.25f); // 16 blocks, as the game's 3328-byte buffer
    // A tree: turned, and 1% shorter than wide (measured), which the plain test refuses.
    const auto tree = [](float angle, float x, float y, float z) {
        Affine m = Placement(angle, 1.0f, x, y, z);
        m[1] *= 0.99f;
        m[5] *= 0.99f;
        m[9] *= 0.99f;
        return m;
    };
    for (uint32_t i = 0; i < 9; ++i) {
        const Affine m = tree(0.3f * float(i), -360.0f + float(i), -99.0f, 170.0f);
        std::copy(m.begin(), m.end(), buffer.begin() + size_t(i) * Floats);
    }
    assert(!PlausiblePlacement(buffer.data()) && PlausiblePlacement(buffer.data(), 2.0f));
    std::vector<float> out{7.0f};
    assert(ReadPatchMatrices(buffer.data(), uint32_t(buffer.size() * 4), 0, 9, out));
    assert(out.size() == 1 + 9 * PlacementFloats && out[0] == 7.0f);
    assert(Near(out[1 + 3], -360.0f) && Near(out[1 + 8 * PlacementFloats + 3], -352.0f));
    // From the third instance on.
    out.clear();
    assert(ReadPatchMatrices(buffer.data(), uint32_t(buffer.size() * 4), 2, 3, out));
    assert(out.size() == 3 * PlacementFloats && Near(out[3], -358.0f));
    // A block that is not a placement (the tenth holds 0.25 everywhere): nothing is taken.
    out.assign(1, 7.0f);
    assert(!ReadPatchMatrices(buffer.data(), uint32_t(buffer.size() * 4), 0, 10, out));
    assert(out.size() == 1);
    // More instances than the buffer holds, no data, no instances.
    assert(!ReadPatchMatrices(buffer.data(), 9 * PatchInstanceBytes - 4, 0, 9, out));
    assert(!ReadPatchMatrices(buffer.data(), uint32_t(buffer.size() * 4), 15, 2, out));
    assert(!ReadPatchMatrices(nullptr, 3328, 0, 1, out));
    assert(!ReadPatchMatrices(buffer.data(), uint32_t(buffer.size() * 4), 0, 0, out));
    // A projection-like block fails whatever the scale allowance.
    const float camera[12] = {1.2f, 0.0f, 0.0f, 0.0f, 0.0f, 2.1f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f, -0.1f};
    assert(!PlausiblePlacement(camera, 2.0f));
}

// Still / moving bits ride on top of the kind: every older ray mask sees the same instances.
static void TestMotionMask() {
    assert(MotionMask(MaskSolid, false) == (MaskSolid | MaskStill));
    assert(MotionMask(MaskSolid, true) == (MaskSolid | MaskMoving));
    assert(MotionMask(MaskKept, false) == (MaskKept | MaskStill));
    assert(MotionMask(MaskKept, true) == (MaskKept | MaskMoving));
    assert(MotionMask(MaskBlended, false) == (MaskBlended | MaskMoving));
    assert(MotionMask(MaskPortal, false) == MaskPortal && MotionMask(MaskWater, true) == MaskWater);
    assert(MotionMask(MaskSky, false) == MaskSky && MotionMask(0, true) == 0);
    // The two halves together are what a shadow ray saw before (0x6F without the panes).
    for (const uint32_t kind : {MaskSolid, MaskKept, MaskBlended}) {
        for (const bool dynamic : {false, true}) {
            assert((MotionMask(kind, dynamic) & (MaskStill | MaskMoving)) != 0);
            assert((MotionMask(kind, dynamic) & 0x67u & ~(MaskStill | MaskMoving)) == kind);
        }
    }
}

int main() {
    TestMotionMask();
    TestPatchMatrices();
    TestSkyFinder();
    TestGeoCache();
    TestCacheById();
    TestPickHelpers();
    TestClipToView();
    TestBlasInput();
    TestFrameGeometry();
    TestGather();
    TestPopLast();
    TestBlockPool();
    TestBlasCache();
    TestInstanceTransform();
    TestRefitAnchor();
    TestProbeSlots();
    TestRebuildReasons();
    TestMovingRefit();

    TestOccurrenceShift();
    TestEvictForSpace();
    TestRefitStagger();
    TestRebuildBudget();
    TestRefitBudget();
    TestLruKeepsLastFrame();
    TestKeepInstances();
    TestPickKept();
    TestPlausiblePlacement();
    TestPlacementId();
    TestPlacementKey();
    TestPlacedNeverShelved();
    TestSamePlacementRule();
    TestPlacementSettled();
    TestPlacementBuffers();
    TestPlacementTrust();
    TestProbeModel();
    TestDrawnFallbackPlacements();
    TestPlacedInstance();
    TestSharedMesh();
    TestPlacementMovers();
    TestPlacementPurges();
    std::puts("rt-geometry-test: ok");
    return 0;
}

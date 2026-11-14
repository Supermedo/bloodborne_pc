#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "gpu/shadps4/video_core/renderer_vulkan/rt/rt_blas_cache.h"
#include "gpu/shadps4/video_core/renderer_vulkan/rt/rt_geometry.h"

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
    e.updatable = true;
    assert(KeepInstance(e, 103, near, rules));
    assert(!KeepInstance(e, 104, near, rules));
    assert(UnusedLimit(e, near, rules) == 120);
    e.updatable = false;
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

int main() {
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
    std::puts("rt-geometry-test: ok");
    return 0;
}

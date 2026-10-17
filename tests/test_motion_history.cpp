// SPDX-License-Identifier: GPL-2.0-or-later
// Standalone CPU regression checks: no game, Vulkan device or window required.
#include <array>
#include <cassert>
#include <cstdio>
#include "gpu/shadps4/video_core/renderer_vulkan/motion_history.h"

using namespace Vulkan::Motion;

int main() {
    const std::array<uint16_t, 5> indices{1000, 1007, 1002, 1007, UINT16_MAX};
    const auto range = IndexedRange(std::span(indices), -900, true);
    assert(range.first == 100 && range.count == 8);
    assert(IndexedRange(std::span(indices), -1001, true).count == 0);
    assert(IndexedRange(std::span(indices), 0, false).count == 64536);
    const std::array<uint32_t, 3> large{100000, 100003, UINT32_MAX};
    assert(IndexedRange(std::span(large), 7, true).count == 4);
    assert(IndexedRange(std::span(large), INT32_MAX, true).count == 0);
    const std::array<uint16_t, 1> restart{UINT16_MAX};
    assert(IndexedRange(std::span(restart), 0, true).count == 0);
    assert(IndexedRange(std::span<const uint16_t>{}, 0, false).count == 0);

    // Single animated mesh (instances == 1): motion history preserved
    Draw single_draw{.shader = 1, .geometry = 2, .indices = 3, .topology = 4,
                     .index_count = 5, .instances = 1, .first_instance = 0,
                     .vertices = range};
    History single_hist(32);
    single_hist.NextFrame();
    const auto s_first = single_hist.Prepare(single_draw);
    assert(s_first.store && !s_first.load && s_first.first_vertex == 100);
    assert(single_hist.Used() == 8);
    single_hist.NextFrame();
    const auto s_next = single_hist.Prepare(single_draw);
    assert(s_next.load == s_first.store && s_next.store != s_first.store);
    assert(single_hist.stats.loaded == 1);
    single_hist.NextFrame();
    single_hist.NextFrame();
    assert(!single_hist.Prepare(single_draw).load);

    // Multi-instance batches (instances > 1): rejected by default until stable instance IDs.
    // Culling reorders batches across frames; per-instance history would associate unrelated objects.
    Draw multi_draw{.shader = 1, .geometry = 2, .indices = 3, .topology = 4,
                    .index_count = 5, .instances = 2, .first_instance = 100,
                    .vertices = range};
    History multi_hist(32);
    multi_hist.NextFrame();
    const auto m_first = multi_hist.Prepare(multi_draw);
    assert(!m_first.store && !m_first.load);
    assert(multi_hist.stats.dropped_instanced == 1);
    assert(multi_hist.Used() == 0);

    // Multi-instance reordering / count changes test:
    // With 3 instances then 2 instances or different first_instance, ensure no history is loaded
    multi_hist.NextFrame();
    auto multi_reorder = multi_draw;
    multi_reorder.instances = 3;
    const auto m_reordered = multi_hist.Prepare(multi_reorder);
    assert(!m_reordered.store && !m_reordered.load);
    assert(multi_hist.stats.dropped_instanced == 2);

    // Preserved under unsafe override if explicitly enabled, or tested via draw with single mesh:
    Draw draw = single_draw;
    History history(32);
    history.NextFrame();
    draw.vertices.count = 8;
    const auto first = history.Prepare(draw);
    const auto duplicate = history.Prepare(draw);
    assert(first.store && !first.load && first.first_vertex == 100);
    assert(duplicate.store == first.store + 8); // shared geometry, different draw
    assert(history.Used() == 16);
    draw.vertices.count = 16;
    assert(history.Prepare(draw).store);
    assert(history.Used() == 32);
    assert(!history.Prepare(draw).store && history.stats.exhausted == 1);
    history.NextFrame();
    draw.vertices.count = 8;
    const auto next = history.Prepare(draw);
    assert(next.load == first.store && next.store != first.store);
    assert(history.Prepare(draw).load == duplicate.store);
    history.NextFrame(); // a missing frame invalidates the match
    history.NextFrame();
    assert(!history.Prepare(draw).load);

    // A changed index buffer may reference formerly unused holes inside the same min/max.
    // Do not accept that history merely because its addresses/counts stayed the same.
    history.NextFrame();
    auto changed = draw;
    changed.topology++;
    assert(!history.Prepare(changed).load);
    changed = draw;
    changed.first_instance++;
    assert(!history.Prepare(changed).load);
    history.NextFrame();
    changed = draw;
    changed.vertices.first++;
    assert(!history.Prepare(changed).load);

    // Single-mesh capacity exhaustion and invalid bounds checking:
    // With instances == 1, draw with vertices exceeding capacity (33 > 32) triggers exhausted
    History overflow(32);
    draw.instances = 1;
    draw.first_instance = 0;
    draw.vertices.first = 0;
    draw.vertices.count = 33;
    assert(!overflow.Prepare(draw).store);
    assert(overflow.stats.exhausted == 1);

    // Invalid bounds on single-mesh draws:
    // 1) vertex first + count overflow (> INT32_MAX)
    draw.vertices.first = INT32_MAX;
    draw.vertices.count = 2;
    assert(!overflow.Prepare(draw).store && overflow.stats.invalid == 1);

    // 2) first_instance + instances overflow (> UINT32_MAX)
    draw.vertices.first = 0;
    draw.vertices.count = 10;
    draw.first_instance = UINT32_MAX;
    draw.instances = 2;
    assert(!overflow.Prepare(draw).store && overflow.stats.invalid == 2);

    // 3) zero vertices count is invalid
    draw.first_instance = 0;
    draw.instances = 1;
    draw.vertices.count = 0;
    assert(!overflow.Prepare(draw).store && overflow.stats.invalid == 3);

    // Small skeletons: history only while the bone palette changes (weapon spinning).
    History gated(1024);
    const History::GateKey weapon{.shader = 7, .index_count = 3, .instances = 1};
    const auto frame = [&](uint64_t palette) {
        gated.NextFrame();
        return gated.Moving(weapon, palette);
    };
    assert(!gated.Moving(weapon, 1) && gated.stats.still == 1); // first sight
    assert(!frame(1));                    // same bones: camera motion
    assert(frame(2) && frame(3));         // spinning
    assert(!frame(3));                    // stopped
    gated.NextFrame();
    gated.NextFrame();
    assert(!gated.Moving(weapon, 4));     // after a skipped frame: no comparison
    assert(!gated.Moving(weapon, 5));     // second occurrence of the same draw
    assert(ClassifyBuffer(864) == BufferRole::Other && ClassifyBuffer(416) == BufferRole::Other);
    assert(ClassifyBuffer(96) == BufferRole::SmallSkeleton &&
           ClassifyBuffer(384) == BufferRole::SmallSkeleton);
    assert(ClassifyBuffer(656) == BufferRole::Skeleton && ClassifyBuffer(64) == BufferRole::Other);

    // Index ranges: reused between full scans, rescanned every Revalidate frames, trimmed.
    IndexRangeCache ranges;
    uint32_t scans = 0;
    const IndexRangeCache::Key mesh{.address = 0x1000, .count = 3, .index_size = 2};
    const auto scan = [&] {
        ++scans;
        return IndexRangeCache::Result{{4, 3}, 99};
    };
    assert(ranges.Get(mesh, 0, scan).topology == 99 && scans == 1);
    assert(ranges.Get(mesh, IndexRangeCache::Revalidate - 1, scan).range == (VertexRange{4, 3}));
    assert(scans == 1);
    ranges.Get(mesh, IndexRangeCache::Revalidate, scan);
    assert(scans == 2);
    ranges.Trim(IndexRangeCache::Revalidate + IndexRangeCache::Unused + 1);
    assert(ranges.Size() == 0);

    assert(JitterPhases(1920, 1920) == 8);
    assert(JitterPhases(1280, 1920) == 18);
    assert(JitterPhases(960, 1920) == 32);
    assert(JitterPhases(640, 1920) == 72);
    assert(JitterPhases(0, 1920) == 8);
    std::puts("Motion history: PASS (ranges, restart, offsets, matching, capacity, gating, index cache, jitter)");
}

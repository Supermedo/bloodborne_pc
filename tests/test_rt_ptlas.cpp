#include <cassert>
#include <cstdio>
#include <vector>

#include "gpu/shadps4/video_core/renderer_vulkan/rt/rt_ptlas.h"

using namespace Vulkan::Rt;

static PtlasInstance Make(float x, uint64_t blas, uint32_t custom, uint32_t partition = 0) {
    PtlasInstance in{};
    in.transform = {1, 0, 0, x, 0, 1, 0, 0, 0, 0, 1, 0};
    in.blas = blas;
    in.custom_index = custom;
    in.mask = 0x01;
    in.flags = 0x5;
    in.partition = partition;
    return in;
}

static void TestSame() {
    const PtlasInstance a = Make(1, 0x1000, 3);
    PtlasInstance b = a;
    assert(PtlasActive(a));
    assert(PtlasSame(a, b));
    b.transform[3] = 2;
    assert(!PtlasSame(a, b));
    b = a;
    b.blas = 0x2000;
    assert(!PtlasSame(a, b));
    b = a;
    b.flags = 0x9;
    assert(!PtlasSame(a, b));
    b = a;
    b.mask = 0x02;
    assert(!PtlasSame(a, b));
    b = a;
    b.partition = 3;
    assert(!PtlasSame(a, b));
    b = a;
    b.custom_index = 4;
    assert(!PtlasSame(a, b));
    PtlasInstance off1 = a, off2 = Make(9, 0x3000, 7, 2);
    off1.mask = 0;
    off2.blas = 0;
    assert(!PtlasActive(off1) && !PtlasActive(off2));
    assert(PtlasSame(off1, off2));
    assert(!PtlasSame(off1, a));
}

static void TestPartition() {
    const Affine near_a = {1, 0, 0, 10, 0, 1, 0, 5, 0, 0, 1, 20};
    const Affine near_b = {1, 0, 0, 12, 0, 1, 0, -40, 0, 0, 1, 22};
    assert(PtlasPartition(false, false, near_a) == 0);
    assert(PtlasPartition(true, false, near_a) == PtlasGlobal);
    assert(PtlasPartition(true, true, near_a) == PtlasGlobal);
    const uint32_t kept = PtlasPartition(false, true, near_a);
    assert(kept >= 1 && kept < PtlasPartitions);
    assert(PtlasPartition(false, true, near_b) == kept);
    uint32_t distinct = 0;
    std::vector<bool> used(PtlasPartitions, false);
    for (int x = -8; x < 8; ++x) {
        for (int z = -8; z < 8; ++z) {
            const Affine m = {1, 0, 0, x * PtlasKeptCell + 1.0f, 0, 1, 0, 0, 0, 0, 1, z * PtlasKeptCell + 1.0f};
            const uint32_t p = PtlasPartition(false, true, m);
            assert(p >= 1 && p < PtlasPartitions);
            distinct += used[p] ? 0u : 1u;
            used[p] = true;
        }
    }
    assert(distinct >= PtlasKeptPartitions / 2);
}

static void TestPartitionByCenter() {
    const Affine cam_a = {1, 0, 0, 10, 0, 1, 0, 0, 0, 0, 1, 10};
    const Affine cam_b = {1, 0, 0, 900, 0, 1, 0, 0, 0, 0, 1, -700};
    const Affine at_object = {1, 0, 0, 300, 0, 1, 0, 0, 0, 0, 1, 300};
    const PtlasCenter none{};
    const PtlasCenter local_a{290, 5, 290, 1};
    const PtlasCenter local_b{-600, 5, 1000, 1};
    const uint32_t want = PtlasPartition(false, true, at_object, none);
    assert(PtlasPartition(false, true, cam_a, local_a) == want);
    assert(PtlasPartition(false, true, cam_b, local_b) == want);
    assert(PtlasPartition(false, true, cam_a, none) == PtlasPartition(false, true, cam_a, {0, 0, 0, 0}));
    const Affine rotated = {0, 0, 1, 0, 0, 1, 0, 0, -1, 0, 0, 0};
    const PtlasCenter local_r{-300, 0, 300, 1};
    assert(PtlasPartition(false, true, rotated, local_r) == want);
    assert(PtlasPartition(false, false, cam_a, local_a) == 0);
    assert(PtlasPartition(true, true, cam_a, local_a) == PtlasGlobal);
    bool differs = false;
    for (int k = 1; k < 8 && !differs; ++k) {
        const PtlasCenter far{290.0f + float(k) * PtlasKeptCell, 5, 290, 1};
        differs = PtlasPartition(false, true, cam_a, far) != want;
    }
    assert(differs);
}

static void TestPlannerRefit() {
    PtlasPlanner planner;
    planner.Reset(8);
    std::vector<PtlasInstance> now = {Make(1, 0x100, 2), Make(2, 0x200, 5), Make(3, 0x300, 6)};
    std::vector<uint32_t> entries = {2, 5, 6};
    std::vector<uint8_t> clean = {0, 0, 0};
    planner.Plan(now, entries, clean);
    const PtlasPlan& idle = planner.Plan(now, entries, clean);
    assert(idle.writes.empty() && idle.refits == 0);
    std::vector<uint8_t> refit = {0, 1, 0};
    const PtlasPlan& forced = planner.Plan(now, entries, refit);
    assert(forced.writes.size() == 1 && forced.writes[0].index == 5);
    assert(forced.writes[0].instance.blas == 0x200 && forced.refits == 1);
    assert(forced.write_of[1] == 1 && forced.write_of[0] == 0);
    assert(planner.Plan(now, entries, clean).writes.empty());
    now[2].transform[3] = 9;
    std::vector<uint8_t> both = {0, 0, 1};
    const PtlasPlan& moved = planner.Plan(now, entries, both);
    assert(moved.writes.size() == 1 && moved.refits == 1);
    now[0].mask = 0;
    std::vector<uint8_t> first_dirty = {1, 0, 0};
    const PtlasPlan& off = planner.Plan(now, entries, first_dirty);
    assert(off.writes.size() == 1 && off.writes[0].instance.blas == 0 && off.refits == 0);
    const PtlasPlan& still_off = planner.Plan(now, entries, first_dirty);
    assert(still_off.writes.empty() && still_off.refits == 0);
    std::vector<uint8_t> short_dirty = {1};
    now[0].mask = 1;
    const PtlasPlan& partial = planner.Plan(now, entries, short_dirty);
    assert(partial.writes.size() == 1 && partial.writes[0].index == 2);
}

static void TestFlags() {
    assert(PtlasFlags(0x1 | 0x4) == 0x5);
    assert(PtlasFlags(0x1 | 0x8) == 0x9);
    assert(PtlasFlags(0x2) == 0x2);
    assert(PtlasFlags(0x10 | 0x20 | 0x1) == 0x1);
}

static void TestHostAccess() {
    assert(PtlasHostWrite == 0x400u && PtlasHostRead == 0x800u);
    assert(PtlasHostAccess(true) == PtlasHostRead);
    assert(PtlasHostAccess(false) == PtlasHostWrite);
}

static void TestLayout() {
    const PtlasSlotLayout l = PtlasLayout(16384, 8192);
    assert(l.commands % 8 == 0 && l.commands >= PtlasHeaderWords * 4);
    assert(l.writes >= l.commands + PtlasCommandBytes && l.writes % 16 == 0);
    assert(l.aux >= l.writes + uint64_t(16384 + 8192) * PtlasWriteBytes && l.aux % 16 == 0);
    assert(l.bytes >= l.aux + uint64_t(8192) * PtlasAuxBytes && l.bytes % 256 == 0);
    static_assert(PtlasWriteBytes == 104);
    static_assert(PtlasCommandBytes == 24);
    static_assert(PtlasShadowBytes >= PtlasWriteBytes + 4 && PtlasShadowBytes % 16 == 0);
    static_assert(PtlasStateBytes(2) == PtlasStateHeaderBytes + 2 * PtlasShadowBytes);
}

static void TestPlanner() {
    PtlasPlanner planner;
    planner.Reset(8);
    assert(planner.InstanceCount() == 16);

    std::vector<PtlasInstance> now = {Make(1, 0x100, 2), Make(2, 0x200, 5)};
    std::vector<uint32_t> entries = {2, 5};
    const PtlasPlan& first = planner.Plan(now, entries);
    assert(first.writes.size() == 16);
    assert(first.index.size() == 2 && first.index[0] == 2 && first.index[1] == 5);
    assert(first.write_of[0] != 0 && first.write_of[1] != 0);
    assert(first.writes[first.write_of[0] - 1].index == 2);
    assert(first.writes[first.write_of[1] - 1].instance.blas == 0x200);
    uint32_t disabled = 0;
    for (const PtlasWrite& w : first.writes) {
        disabled += PtlasActive(w.instance) ? 0u : 1u;
    }
    assert(disabled == 14);

    const PtlasPlan& same = planner.Plan(now, entries);
    assert(same.writes.empty());
    assert(same.write_of[0] == 0 && same.write_of[1] == 0);
    assert(same.index[0] == 2 && same.index[1] == 5);

    std::vector<PtlasInstance> swapped = {now[1], now[0]};
    std::vector<uint32_t> swapped_entries = {5, 2};
    const PtlasPlan& reorder = planner.Plan(swapped, swapped_entries);
    assert(reorder.writes.empty());
    assert(reorder.index[0] == 5 && reorder.index[1] == 2);

    now[1].transform[3] = 3;
    const PtlasPlan& moved = planner.Plan(now, entries);
    assert(moved.writes.size() == 1 && moved.writes[0].index == 5);
    assert(moved.write_of[0] == 0 && moved.write_of[1] == 1);

    now[0].blas = 0x300;
    const PtlasPlan& rebuilt = planner.Plan(now, entries);
    assert(rebuilt.writes.size() == 1 && rebuilt.writes[0].index == 2 &&
           rebuilt.writes[0].instance.blas == 0x300);

    now[0].partition = PtlasGlobal;
    const PtlasPlan& repart = planner.Plan(now, entries);
    assert(repart.writes.size() == 1 && repart.writes[0].instance.partition == PtlasGlobal);

    std::vector<PtlasInstance> one = {now[0]};
    std::vector<uint32_t> one_entry = {2};
    const PtlasPlan& gone = planner.Plan(one, one_entry);
    assert(gone.writes.size() == 1 && gone.writes[0].index == 5);
    assert(!PtlasActive(gone.writes[0].instance));
    assert(planner.Plan(one, one_entry).writes.empty());

    const PtlasPlan& back = planner.Plan(now, entries);
    assert(back.writes.size() == 1 && back.writes[0].index == 5 && PtlasActive(back.writes[0].instance));

    now[1].mask = 0;
    const PtlasPlan& hidden = planner.Plan(now, entries);
    assert(hidden.writes.size() == 1 && hidden.writes[0].instance.blas == 0);
    assert(planner.State(5).blas == 0);
    assert(planner.Plan(now, entries).writes.empty());
    now[1].transform[3] = 7;
    assert(planner.Plan(now, entries).writes.empty());
    now[1].mask = 1;
    assert(planner.Plan(now, entries).writes.size() == 1);

    std::vector<PtlasInstance> dup = {now[0], now[0], now[0]};
    std::vector<uint32_t> dup_entries = {2, 2, 2};
    const PtlasPlan& dups = planner.Plan(dup, dup_entries);
    assert(dups.index[0] == 2 && dups.index[1] == 8 + 2 && dups.index[2] == PtlasNoIndex);
    assert(dups.write_of[2] == 0);

    std::vector<uint32_t> bad = {99};
    std::vector<PtlasInstance> bad_now = {now[0]};
    const PtlasPlan& out_of_range = planner.Plan(bad_now, bad);
    assert(out_of_range.index[0] == PtlasNoIndex);

    planner.Reset(8);
    assert(planner.Plan(now, entries).writes.size() == 16);
}

int main() {
    TestSame();
    TestPartition();
    TestFlags();
    TestHostAccess();
    TestLayout();
    TestPlanner();
    TestPartitionByCenter();
    TestPlannerRefit();
    std::puts("rt-ptlas-test: ok");
    return 0;
}

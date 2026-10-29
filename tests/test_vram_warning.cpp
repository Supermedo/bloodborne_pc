// SPDX-License-Identifier: GPL-2.0-or-later
// When the "Video memory is running low" banner comes up (gpu/shim/bbport_vram_warning.h): not in
// normal play on a 4 GB card, as before on 8 GB and up, not again while usage goes back and forth.
// ninja -C out/gpu vram-warning-test, then run out/gpu/vram-warning-test.
#include <cassert>
#include <cstdio>
#include <initializer_list>
#include "bbport_vram_warning.h"

using BbVramWarning::Margin;
using BbVramWarning::MiB;
using BbVramWarning::State;
using BbVramWarning::Step;

int main() {
    constexpr uint64_t Default = 1536 * MiB; // BB_VRAM_WARN_MB unset

    // A 4 GB card with the low-VRAM profile: run.py's budget 4096 - 600 = 3496 MiB, the critical
    // mark at 95% (texture_cache.cpp with BB_GC_BUDGET_MB). 175 MiB to the mark: one step instead.
    {
        const uint64_t limit = 3496 * MiB, critical = limit / 100 * 95;
        const uint64_t margin = Margin(limit, critical, Default);
        assert(margin == Step);
        State warning;
        // Normal play, 2 to 3.2 GB in use and back: beta 8 warned from 1960 MiB on.
        for (const uint64_t used : {2000, 2400, 2900, 2600, 3100, 2800, 3200, 2500}) {
            assert(!warning.Check(limit - used * MiB, margin));
        }
        // Within the last step (the critical mark is at ~3321 MiB, closer to the end than a step):
        // once, near the end.
        assert(warning.Check(limit - 3245 * MiB, margin));
        assert(!warning.Check(limit - 3230 * MiB, margin)); // back a little: not armed again
        assert(!warning.Check(limit - 3245 * MiB, margin));
        assert(!warning.Check(limit - 3400 * MiB, margin)); // not a step closer yet
        // The collector frees enough: armed again with a step more than the margin free.
        assert(!warning.Check(limit - 2984 * MiB, margin));
        assert(warning.Check(limit - 3300 * MiB, margin));
    }

    // An 8 GB card: the constructor's critical mark, 20% of the budget below it (1638 MiB). The
    // 1.5 GB stand: the first banner at 1.5 GB left, again each 256 MiB closer.
    {
        const uint64_t limit = 8192 * MiB, critical = limit - 2 * limit / 10;
        const uint64_t margin = Margin(limit, critical, Default);
        assert(margin == Default);
        State warning;
        assert(!warning.Check(1600 * MiB, margin));
        assert(warning.Check(1500 * MiB, margin));
        assert(!warning.Check(1300 * MiB, margin));
        assert(warning.Check(1200 * MiB, margin));
        assert(!warning.Check(1700 * MiB, margin)); // above the margin, under margin + step
        assert(!warning.Check(1100 * MiB, margin)); // so not armed again: only 100 MiB closer
        assert(!warning.Check(1792 * MiB, margin)); // a step over the margin: armed
        assert(warning.Check(1500 * MiB, margin));
    }

    // A 16 GB card, 1.6 GB from critical to the limit: unchanged too.
    {
        const uint64_t limit = 16304 * MiB, critical = limit - 2 * (8192 * MiB) / 10;
        assert(Margin(limit, critical, Default) == Default);
    }

    // BB_VRAM_WARN_MB=0 turns it off, also over the critical mark.
    {
        const uint64_t limit = 3496 * MiB, critical = limit / 100 * 95;
        State warning;
        assert(Margin(limit, critical, 0) == 0);
        assert(!warning.Check(0, Margin(limit, critical, 0)));
    }

    // A budget whose critical mark is at its end (3 GB without the profile: the 3 GB floor) or
    // past it: one step before the end, not never.
    assert(Margin(3072 * MiB, 3072 * MiB, Default) == Step);
    assert(Margin(2048 * MiB, 3072 * MiB, Default) == Step);
    // A smaller wish than a step is kept.
    assert(Margin(3496 * MiB, 3496 * MiB / 100 * 95, 100 * MiB) == 100 * MiB);

    std::puts("vram warning: ok");
    return 0;
}

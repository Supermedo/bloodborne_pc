// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: real occlusion query results (BB_OCCLUSION, default on; 0: the old fake counts).
//
// The design follows droogie/bbhost commit b68ed17 ("occlusion: the game's own GPU occlusion
// culling works", Kyoskii, after KyoPS4x #217-#220), adapted to this renderer: there the GPU
// writes the counts with a compute shader before the labels; here labels are written when the
// work is recorded (not when the GPU has run it), so the counts are written by the CPU once the
// GPU has finished the submission holding the queries, a frame or so after the game asked.
//
// The PS4 model: each depth block (DB, 8 on a base PS4) keeps a running count of the samples
// that passed; an EVENT_WRITE of PIXEL_PIPE_STAT_DUMP / ZPASS_DONE stores every DB's count, bit
// 63 (valid) set, at the dump's address + 16 * db. Gnm's query block is {begin, end} per DB:
// the game zeroes it, dumps at +0 before the draws it measures and at +8 after, and reads the
// sum of end - begin. Bloodborne tests its culling proxies that way (GX GetData, CPU readback,
// ~180 blocks a frame in a dense view: when the answer is "no samples" it does not submit the
// object's draws at all) and also predicates draws on blocks with SET_PREDICATION (Gnm's
// setZPassPredicationEnable, called from the game at 0x216a79e and 0x2250745).
//
// Here:
// - Stage A counts the draws it decodes and hands each dump to the recording side in order
//   (RunInOrder). Draws recorded after a dump get a Vulkan occlusion query each, begun and ended
//   around the vkCmdDraw itself (never across a render pass): up to BB_OCCLUSION_CAP (64) after
//   a block's begin dump, one after any other dump. An interval holding a draw without a query
//   (past the cap, filtered, its pipeline not ready yet, failed) reads plainly visible.
// - At the dump the interval's queries and the block address are queued; once the GPU has
//   finished that submission (Scheduler::DeferPriorityOperation, its own thread) the results
//   are read without waiting, a running total is kept like the DBs keep theirs, and the dump
//   writes it, split over the DBs with the valid bit, into the game's memory. Never a stall:
//   no thread waits for a query, the game polls its blocks and takes "not ready" as it would on
//   the PS4 (it keeps the object drawn and asks again).
// - Fail-safe towards "visible": an unavailable query, a missing query, an imprecise device's
//   nonzero result, all add a large count. A dump over memory that holds neither the game's
//   zeroes nor our own last value (freed and reused) is not written, nor one more than
//   BB_OCCLUSION_MAX_LAG frames late unless we wrote that block before and it is at most twice
//   that late: the block stays not ready.
// - Hysteresis against flicker (lamps, glare): a block reports the largest of its last three
//   results (OcclusionModel::BlockHistory): occluded only after three in a row found nothing,
//   visible at once.
// - SET_PREDICATION: the draws between a ZPASS set and its clear are skipped only when every
//   dump into the block has been resolved and the block reads occluded; otherwise they are
//   drawn. The results arrive a frame late, so this rarely skips: GPU conditional rendering
//   would need a render pass split per predicated object to copy the result on the GPU and
//   would still record every draw (the CPU cost this port is bound by), so it is not done.
//   BB_FRAME_STATS counts how many predicated draws there are.
//
// Latency: an object coming into view is drawn again once the game has read the result of a
// query it issued while the object was hidden: one or two frames later than on the PS4 (the
// game itself already lags a frame there). Popping at the edge of occluders is the visible risk.

#pragma once

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

class Instance;
class Scheduler;
class Rasterizer;

class Occlusion {
public:
    /// BB_OCCLUSION=1. Off (default): the fake counts and SET_PREDICATION ignored.
    static bool Wanted();

    /// Null when the device cannot do it (no host query reset, no query pool): the fake then.
    static std::unique_ptr<Occlusion> Create(const Instance& instance, Scheduler& scheduler,
                                             Rasterizer& rasterizer);
    ~Occlusion();

    /// Stage A (GPU command thread): a draw packet decoded (direct or indirect).
    void NoteIssued() noexcept {
        ++issued;
    }

    /// Stage A: what a dump hands to the recording side.
    struct DumpTask {
        VAddr va;
        u64 issued;    ///< draws decoded before the dump
        u64 frame;     ///< sceGnmSubmitDone calls when it was decoded
        u32 pipes;     ///< DBs the dump writes
        u32 pad;
    };
    DumpTask MakeDump(VAddr va, u32 num_pipes) const noexcept;

    struct PredicationTask {
        VAddr block;
        u32 op; ///< 0 clear, 1 ZPASS, 2 primitive count (not emulated: drawn)
        u8 draw_if_visible;
        u8 cont;
        u16 pad;
    };
    /// Stage A: whether a SET_PREDICATION region is open (the submission end clears it).
    bool predication_open = false;

    /// Dumps were queued into the submission `current_tick` (not sent to the GPU yet). Read on
    /// stage A with the draw pipe drained.
    bool HasUnsubmittedDumps(u64 current_tick) const noexcept {
        return registered_tick == current_tick;
    }

    // ---- the recording side (stage B, or stage A after a drain), in stream order ----
    void Dump(const DumpTask& task);
    void SetPredication(const PredicationTask& task);
    /// At the start of a draw's recording: true when SET_PREDICATION skips it.
    bool SkipDraw() noexcept {
        if (pred_mode == PredMode::None) [[likely]] {
            return false;
        }
        return SkipPredicated();
    }
    /// The query to wrap the draw about to be recorded in (pool null: none). `scene_size`: the
    /// draw's push_data.scene_size (reduced scene targets count fewer samples).
    struct DrawQuery {
        vk::QueryPool pool{};
        u32 index = 0;
        vk::QueryControlFlags flags{};
    };
    DrawQuery BeginDraw(u32 scene_size) noexcept {
        if (!interval.open || interval.queries >= interval.cap) [[likely]] {
            return {};
        }
        return AllocateQuery(scene_size);
    }
    /// bbport: whether BeginDraw would measure the next draw. A draw the game measures is never
    /// left out while its pipeline compiles (Rasterizer::SkipUncompiledDraw): zero samples
    /// would read as occluded and its predicated draws would vanish too.
    [[nodiscard]] bool Measuring() const noexcept {
        return interval.open && interval.queries < interval.cap;
    }

private:
    Occlusion(const Instance& instance, Scheduler& scheduler, Rasterizer& rasterizer);
    bool Init();
    DrawQuery AllocateQuery(u32 scene_size) noexcept;
    bool SkipPredicated() noexcept;

    static constexpr u32 PoolSize = 16384;

    const Instance& instance;
    Scheduler& scheduler;
    Rasterizer& rasterizer;
    bool precise = false;
    u32 cap = 64;
    u32 pipes = 8; ///< DBs of the last dump (a Neo has 16)

    // Stage A.
    u64 issued = 0;

    // The recording side.
    struct Interval {
        bool open = false;
        u32 cap = 0;
        u32 queries = 0;    ///< queries handed out in it (one per measured draw)
        u64 first = 0;      ///< its first query, as a running count
        u64 issued_at = 0;  ///< stage A's draw count at the dump that opened it
    } interval;
    u64 query_head = 0;     ///< queries handed out so far
    u64 dump_serial = 0;
    u64 registered_tick = ~0ull;
    std::unordered_map<VAddr, u64> block_serial; ///< a block's last dump (either half)
    enum class PredMode : u8 { None, Skip, Draw };
    PredMode pred_mode = PredMode::None;
    bool pred_visible = false, pred_unknown = false; ///< CONTINUE chains OR blocks together

    /// What the resolver thread and the recording side share.
    struct Shared;
    std::shared_ptr<Shared> shared;
};

} // namespace Vulkan

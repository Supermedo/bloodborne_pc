#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>

#include "rt_geometry.h"
#include "rt_gi.h"

namespace Vulkan::Rt {

constexpr Affine IdentityAffine() {
    return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
}

constexpr Affine InstanceTransform(const Affine& current_inv_view, const Affine& built_inv_view) {
    return MultiplyAffine(InverseRigid(current_inv_view), built_inv_view);
}

constexpr uint32_t ProbeSamples = 4;

inline std::array<uint32_t, ProbeSamples> ProbeSlots(const DrawInput& in) {
    std::array<uint32_t, ProbeSamples> slots{};
    const uint32_t prims = in.index_count / 3;
    const int64_t lo = int64_t(in.first_vertex) - in.base_vertex;
    for (uint32_t k = 0; k < ProbeSamples; ++k) {
        const uint32_t i = uint32_t((uint64_t(prims) * k / ProbeSamples)) * 3;
        uint32_t index = i;
        if (in.index_size == 2) {
            index = static_cast<const uint16_t*>(in.indices)[i];
        } else if (in.index_size == 4) {
            index = static_cast<const uint32_t*>(in.indices)[i];
        }
        if (int64_t(index) < lo || int64_t(index) - lo >= int64_t(in.vertex_count)) {
            slots[k] = 0;
            continue;
        }
        slots[k] = in.store_base + uint32_t(int64_t(index) - lo);
    }
    return slots;
}

class BlockPool {
public:
    static constexpr uint64_t Fail = ~0ull;

    void Reset(uint64_t size_, uint64_t alignment_) {
        size = size_;
        alignment = alignment_;
        used = 0;
        free_blocks.clear();
        if (size) {
            free_blocks.emplace(0, size);
        }
    }

    uint64_t Alloc(uint64_t bytes) {
        bytes = (bytes + alignment - 1) / alignment * alignment;
        if (!bytes) {
            bytes = alignment;
        }
        for (auto it = free_blocks.begin(); it != free_blocks.end(); ++it) {
            if (it->second < bytes) {
                continue;
            }
            const uint64_t offset = it->first;
            const uint64_t rest = it->second - bytes;
            free_blocks.erase(it);
            if (rest) {
                free_blocks.emplace(offset + bytes, rest);
            }
            used += bytes;
            return offset;
        }
        return Fail;
    }

    void Free(uint64_t offset, uint64_t bytes) {
        bytes = (bytes + alignment - 1) / alignment * alignment;
        if (!bytes) {
            bytes = alignment;
        }
        used -= bytes;
        auto next = free_blocks.lower_bound(offset);
        if (next != free_blocks.end() && next->first == offset + bytes) {
            bytes += next->second;
            next = free_blocks.erase(next);
        }
        if (next != free_blocks.begin()) {
            auto prev = std::prev(next);
            if (prev->first + prev->second == offset) {
                prev->second += bytes;
                return;
            }
        }
        free_blocks.emplace(offset, bytes);
    }

    uint64_t Used() const { return used; }
    uint64_t Size() const { return size; }
    size_t FreeBlocks() const { return free_blocks.size(); }

private:
    uint64_t size{}, alignment{256}, used{};
    std::map<uint64_t, uint64_t> free_blocks;
};

enum class Rebuild : uint32_t { None, Miss, Size, Dirty, Unreadable, Skeleton, Palette, Count };

constexpr uint32_t MaxRefits = 60;

constexpr uint32_t RefitPhase(uint32_t id, uint32_t max_refits = MaxRefits) {
    return id % (max_refits ? max_refits : 1u);
}

constexpr uint32_t BuildAllowUpdate = 1;
constexpr uint32_t BuildAllowCompaction = 2;
constexpr uint32_t BuildFastTrace = 4;
constexpr uint32_t BuildFastBuild = 8;

constexpr uint32_t BlasBuildFlags(bool updatable, bool compaction, bool updatable_fast_trace = false) {
    if (updatable) {
        return (updatable_fast_trace ? BuildFastTrace : BuildFastBuild) | BuildAllowUpdate;
    }
    return BuildFastTrace | (compaction ? BuildAllowCompaction : 0u);
}

constexpr bool CompactionWorthIt(uint64_t allocated, uint64_t compacted) {
    return compacted && compacted * 10 <= allocated * 9;
}

struct CacheEntry {
    uint32_t id{};
    uint64_t offset{}, bytes{};
    uint32_t triangles{};
    uint64_t palette{};
    uint64_t last_used{};
    uint64_t built_frame{};
    Affine inv_view{};
    bool dirty{};
    bool valid{};
    bool updatable{};
    bool skinned{};
    uint32_t instances{};
    bool moving{};
    bool view_valid{};
    uint64_t moving_frame{};
    // World position of probe sample anchor_k at the last build (rt_probe.comp), ~0 = unknown.
    // It tells two placements of one mesh apart: the game draws a reused building with the same
    // buffers and shader wherever it stands, so the draw key alone cannot.
    std::array<float, 3> anchor{};
    uint32_t anchor_k{~0u};
    uint32_t refits{};
    uint32_t uv_base{0xFFFFFFFFu};
    uint64_t uv_bytes{};
    bool uv_captured{};
    bool omm{};
    bool omm_ready{};
    uint64_t omm_offset{}, omm_bytes{};
    uint64_t omm_index_offset{}, omm_index_bytes{};
    bool compacted{};
    bool seen{};
    std::array<float, 3> seen_camera{};
    uint64_t tex_view{}, tex_uid{};
    uint32_t tex_image{};
    bool alpha_test{};
    float alpha_ref{0.5f};
    bool blended{};
    uint64_t alpha_view{}, alpha_uid{};
    uint32_t alpha_image{};
    uint32_t alpha_channel{3};
    uint64_t normal_view{}, normal_uid{};
    uint32_t normal_image{};
    // Frame a camera ray found this entry, kept, in front of the drawn surface (BB_RT_KEPT_PROBE):
    // the game draws something else there now, so it is not kept until the game draws it again.
    uint64_t probe_hidden{};
};

constexpr bool RememberSurface(bool texture_slot, bool alpha_slot) {
    return texture_slot || alpha_slot;
}

struct KeepRules {
    float distance{};
    uint64_t skinned_frames{};
    uint64_t unused_frames{120};
    uint64_t kept_unused_frames{120};
    uint64_t triangle_budget{};
    uint32_t max_triangles{};
    bool probe_hide{true}; // CacheEntry::probe_hidden counts (BB_RT_KEPT_PROBE)
};

struct KeptPick {
    std::vector<CacheEntry*> picked;
    uint64_t triangles{}, over_budget{}, too_big{};
};

inline float Distance2(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;
}

inline bool KeepNear(const CacheEntry& e, const std::array<float, 3>& camera, const KeepRules& k) {
    return k.distance > 0.0f && e.seen && Distance2(e.seen_camera, camera) <= k.distance * k.distance;
}

inline bool KeepShortLived(const CacheEntry& e) {
    return e.updatable && e.instances <= 1 && (e.skinned || e.palette || e.moving);
}

inline bool KeepInstance(const CacheEntry& e, uint64_t frame, const std::array<float, 3>& camera,
                         const KeepRules& k) {
    if (!e.valid || e.dirty || e.last_used >= frame || !KeepNear(e, camera, k) ||
        (k.probe_hide && e.probe_hidden > e.last_used)) {
        return false;
    }
    if (KeepShortLived(e)) {
        return frame - e.last_used <= k.skinned_frames;
    }
    return true;
}

inline KeptPick PickKept(std::vector<CacheEntry*> candidates, const std::array<float, 3>& camera,
                         const KeepRules& k) {
    std::sort(candidates.begin(), candidates.end(), [&](const CacheEntry* a, const CacheEntry* b) {
        if (a->last_used != b->last_used) {
            return a->last_used > b->last_used;
        }
        const float da = Distance2(a->seen_camera, camera), db = Distance2(b->seen_camera, camera);
        return da != db ? da < db : a->id < b->id;
    });
    KeptPick out;
    for (CacheEntry* e : candidates) {
        if (k.max_triangles && e->triangles > k.max_triangles) {
            ++out.too_big;
            continue;
        }
        if (k.triangle_budget && out.triangles + e->triangles > k.triangle_budget) {
            ++out.over_budget;
            continue;
        }
        out.triangles += e->triangles;
        out.picked.push_back(e);
    }
    return out;
}

inline uint64_t UnusedLimit(const CacheEntry& e, const std::array<float, 3>& camera,
                            const KeepRules& k) {
    return !KeepShortLived(e) && !e.dirty && KeepNear(e, camera, k) ? k.kept_unused_frames
                                                                     : k.unused_frames;
}

inline bool CompactCandidate(const CacheEntry* e, uint64_t queried_frame, uint64_t compacted) {
    return e && e->valid && !e->dirty && !e->omm && e->built_frame == queried_frame &&
           !e->updatable && !e->compacted && CompactionWorthIt(e->bytes, compacted);
}

struct RetiredBlock {
    uint64_t offset{}, bytes{};
    uint64_t tick{};
    uint64_t frame{};
    uint64_t tlas_tick{};
};

inline void NoteTlas(RetiredBlock& block, uint64_t tlas_frame, uint64_t tlas_tick) {
    if (!block.tlas_tick && tlas_tick && tlas_frame >= block.frame) {
        block.tlas_tick = tlas_tick;
    }
}

// At the end of a frame: blocks retired in it or before that no TLAS came after yet take the
// frame's end tick instead. Every trace builds its TLAS in its own frame (Build), so after this
// frame's commands nothing reads an older TLAS. Without it, frames that build no TLAS (loading
// screens: no camera) never free their retired blocks; the pool fills and path tracing stays off.
template <class Blocks>
void CloseFrame(Blocks& blocks, uint64_t frame, uint64_t end_tick) {
    for (RetiredBlock& block : blocks) {
        if (!block.tlas_tick && block.frame <= frame) {
            block.tlas_tick = end_tick;
        }
    }
}

template <class Free>
bool RetireReady(RetiredBlock& block, uint64_t tlas_frame, uint64_t tlas_tick, Free&& is_free) {
    NoteTlas(block, tlas_frame, tlas_tick);
    return block.tlas_tick && is_free(block.tick) && is_free(block.tlas_tick);
}

inline bool ReuseAllocation(const CacheEntry* e, uint32_t triangles, bool updatable, bool omm_room) {
    return e && !e->compacted && e->triangles == triangles && e->updatable == updatable &&
           (!e->omm || omm_room);
}

inline Rebuild RebuildReason(const CacheEntry* e, uint64_t palette, bool skinned, bool unreadable,
                             uint32_t triangles) {
    if (!e || !e->valid) {
        return Rebuild::Miss;
    }
    if (e->triangles != triangles) {
        return Rebuild::Size;
    }
    if (e->dirty) {
        return Rebuild::Dirty;
    }
    if (unreadable) {
        return Rebuild::Unreadable;
    }
    if (e->palette != palette || e->moving) {
        return skinned ? Rebuild::Skeleton : Rebuild::Palette;
    }
    return Rebuild::None;
}

inline uint64_t AlignUp(uint64_t bytes, uint64_t alignment) {
    bytes = (bytes + alignment - 1) / alignment * alignment;
    return bytes ? bytes : alignment;
}

class BlasCache {
public:
    struct Lookup {
        CacheEntry* entry{};
        bool rebuild{};
        Rebuild reason{};
        bool updatable{};
        bool refit{};
        bool can_refit{};
        bool limit{};
        uint64_t stale{};
        bool shifted{};
        bool superset{};
    };
    struct Stats {
        uint64_t hits{}, misses{}, rebuilt{}, evicted{}, probe_dirty{}, refits{}, stale_flags{};
        uint64_t superset_hits{};
        uint64_t space_evictions{}, deferred{}, held{}, budget_refits{};
        uint64_t compacted{}, compacted_saved{}, omm_built{}, omm_triangles{};
        std::array<uint64_t, size_t(Rebuild::Count)> reasons{};
    } stats;

    void BeginFrame(uint64_t frame_) {
        frame = frame_;
        occurrences.clear();
    }

    Lookup Find(DrawKey& key, uint64_t palette, bool skinned, bool unreadable, uint32_t triangles) {
        const uint32_t instances = key.instances;
        key.instances = 0;
        key.occurrence = 0;
        key.occurrence = occurrences[key]++;
        const auto it = entries.find(key);
        CacheEntry* e = it == entries.end() ? nullptr : &it->second;
        if (e && e->moving && frame > e->moving_frame + moving_frames) {
            e->moving = false;
        }
        const Rebuild reason = RebuildReason(e, palette, skinned, unreadable, triangles);
        if (!e) {
            ++stats.misses;
            ++stats.reasons[size_t(reason)];
            DrawKey base = key;
            base.occurrence = 0;
            const bool shifted = key.occurrence > 0 && entries.contains(base);
            return {nullptr, true, reason, skinned || unreadable, false, false, false, 0, shifted};
        }
        e->last_used = frame;
        e->instances = std::max(e->instances, instances);
        if (reason == Rebuild::None) {
            ++stats.hits;
            return {e, false, reason, e->updatable, false, false, false};
        }
        if (reason == Rebuild::Size && triangles < e->triangles && instances > 1 && !e->dirty) {
            ++stats.hits;
            ++stats.superset_hits;
            return {e, false, Rebuild::None, false, false, false, false, 0, false, true};
        }
        ++stats.rebuilt;
        ++stats.reasons[size_t(reason)];
        const bool deforms = reason == Rebuild::Skeleton || reason == Rebuild::Palette ||
                             reason == Rebuild::Unreadable;
        const bool updatable = e->updatable || e->moving || skinned || unreadable || deforms;
        const bool can_refit = e->valid && e->updatable && e->triangles == triangles &&
                               (deforms || reason == Rebuild::Dirty);
        const bool refit = can_refit && e->refits < max_refits;
        const uint64_t stale = frame > e->built_frame ? frame - e->built_frame : 0;
        return {e, true, reason, updatable, refit, can_refit, can_refit && !refit, stale};
    }

    CacheEntry& Insert(const DrawKey& key, const CacheEntry& entry) {
        CacheEntry& e = entries[key];
        e = entry;
        e.last_used = frame;
        by_id[e.id] = key;
        return e;
    }

    uint32_t ClassifyMiss(const DrawKey& key, DrawKey& match) const {
        uint32_t kind = 0;
        for (const auto& [k, e] : entries) {
            if (k.geometry == key.geometry && k.indices == key.indices &&
                k.index_count == key.index_count && k.first_vertex == key.first_vertex) {
                match = k;
                return k.shader == key.shader ? 1u : 2u;
            }
            if (kind == 0 && k.shader == key.shader && k.index_count == key.index_count &&
                k.vertex_count == key.vertex_count && k.indices == key.indices) {
                match = k;
                kind = 3;
            } else if (kind == 0 && k.shader == key.shader && k.index_count == key.index_count &&
                       k.vertex_count == key.vertex_count) {
                match = k;
                kind = 4;
            }
        }
        return kind;
    }

    CacheEntry* ById(uint32_t id) {
        const auto it = by_id.find(id);
        if (it == by_id.end()) {
            return nullptr;
        }
        const auto e = entries.find(it->second);
        return e == entries.end() ? nullptr : &e->second;
    }

    bool MarkDirty(uint32_t id, uint64_t probe_frame) {
        CacheEntry* e = ById(id);
        if (!e) {
            return false;
        }
        if (probe_frame < e->built_frame) {
            ++stats.stale_flags;
            return false;
        }
        if (!e->dirty) {
            ++stats.probe_dirty;
        }
        e->dirty = true;
        return true;
    }

    // Shelved entries: another placement of a mesh whose draw key now draws somewhere else. They
    // sit under their key with ShelfBit in the occurrence, which no draw ever asks for, so they
    // stay in the TLAS as kept instances and can be taken back when the draw returns to them.
    static constexpr uint32_t ShelfBit = 0x40000000u;

    static bool SameDraw(const DrawKey& a, const DrawKey& b) {
        DrawKey x = a, y = b;
        x.occurrence = y.occurrence = 0;
        return x == y;
    }

    bool Rekey(uint32_t id, const DrawKey& to) {
        const auto it = by_id.find(id);
        if (it == by_id.end() || entries.contains(to)) {
            return false;
        }
        auto node = entries.extract(it->second);
        if (node.empty()) {
            return false;
        }
        node.key() = to;
        entries.insert(std::move(node));
        it->second = to;
        return true;
    }

    template <class Pred>
    CacheEntry* FindShelved(const DrawKey& home, Pred&& pred) {
        for (auto& [k, e] : entries) {
            if ((k.occurrence & ShelfBit) && SameDraw(k, home) && pred(e)) {
                return &e;
            }
        }
        return nullptr;
    }

    const DrawKey* KeyOf(uint32_t id) const {
        const auto it = by_id.find(id);
        return it == by_id.end() ? nullptr : &it->second;
    }


    template <class Release>
    void Erase(uint32_t id, Release&& release) {
        const auto it = by_id.find(id);
        if (it == by_id.end()) {
            return;
        }
        const auto e = entries.find(it->second);
        if (e != entries.end()) {
            release(e->second);
            entries.erase(e);
        }
        by_id.erase(it);
        ++stats.evicted;
    }

    template <class Release>
    void EvictUnused(uint64_t max_unused, Release&& release) {
        EvictUnusedBy([&](const CacheEntry&) { return max_unused; }, release);
    }

    template <class Limit, class Release>
    void EvictUnusedBy(Limit&& limit, Release&& release) {
        std::vector<uint32_t> victims;
        for (const auto& [key, e] : entries) {
            if (frame - e.last_used > limit(e)) {
                victims.push_back(e.id);
            }
        }
        for (const uint32_t id : victims) {
            Erase(id, release);
        }
    }

    template <class Fn>
    void ForEach(Fn&& fn) {
        for (auto& [key, e] : entries) {
            fn(e);
        }
    }

    template <class Fn>
    void ForEachOrdered(Fn&& fn) {
        std::vector<CacheEntry*> list;
        list.reserve(entries.size());
        for (auto& [key, e] : entries) {
            list.push_back(&e);
        }
        std::sort(list.begin(), list.end(),
                  [](const CacheEntry* a, const CacheEntry* b) { return a->id < b->id; });
        for (CacheEntry* e : list) {
            fn(*e);
        }
    }

    template <class Release>
    bool EvictLeastRecent(Release&& release) {
        const CacheEntry* victim = nullptr;
        for (const auto& [key, e] : entries) {
            if (e.last_used + 1 >= frame) {
                continue;
            }
            if (!victim || e.last_used < victim->last_used) {
                victim = &e;
            }
        }
        if (!victim) {
            return false;
        }
        Erase(victim->id, release);
        return true;
    }

    template <class Release>
    bool EvictForSpace(uint64_t bytes, uint64_t pool_size, uint64_t alignment, Release&& release) {
        bytes = AlignUp(bytes, alignment);
        struct Span {
            uint64_t begin, end;
            uint64_t last_used;
            uint32_t id;
            bool blocked;
        };
        std::vector<Span> spans;
        spans.reserve(entries.size());
        for (const auto& [key, e] : entries) {
            spans.push_back({e.offset, e.offset + AlignUp(e.bytes, alignment), e.last_used, e.id,
                             e.last_used >= frame});
        }
        std::sort(spans.begin(), spans.end(),
                  [](const Span& a, const Span& b) { return a.begin < b.begin; });
        bool found = false;
        uint64_t best_cost = 0, best_recent = 0, best_begin = 0;
        size_t best_first = 0;
        uint64_t gap_begin = 0;
        size_t first = 0;
        for (size_t i = 0; i <= spans.size(); ++i) {
            const bool last = i == spans.size();
            if (!last && !spans[i].blocked) {
                continue;
            }
            const uint64_t gap_end = last ? pool_size : spans[i].begin;
            if (gap_end >= gap_begin && gap_end - gap_begin >= bytes) {
                uint64_t cost = 0, recent = 0;
                for (size_t j = first; j < i && spans[j].begin < gap_begin + bytes; ++j) {
                    cost += spans[j].end - spans[j].begin;
                    recent = std::max(recent, spans[j].last_used);
                }
                if (!found || cost < best_cost || (cost == best_cost && recent < best_recent)) {
                    found = true;
                    best_cost = cost;
                    best_recent = recent;
                    best_begin = gap_begin;
                    best_first = first;
                }
            }
            if (!last) {
                gap_begin = spans[i].end;
                first = i + 1;
            }
        }
        if (!found) {
            return false;
        }
        for (size_t j = best_first;
             j < spans.size() && !spans[j].blocked && spans[j].begin < best_begin + bytes; ++j) {
            Erase(spans[j].id, release);
            ++stats.space_evictions;
        }
        return true;
    }

    template <class Release>
    void Clear(Release&& release) {
        for (auto& [key, e] : entries) {
            release(e);
        }
        stats.evicted += entries.size();
        entries.clear();
        by_id.clear();
    }

    size_t Size() const { return entries.size(); }

    uint32_t max_refits{MaxRefits};
    uint64_t moving_frames{600};


private:
    uint64_t frame{};
    std::unordered_map<DrawKey, CacheEntry, DrawKeyHash> entries;
    std::unordered_map<DrawKey, uint32_t, DrawKeyHash> occurrences;
    std::unordered_map<uint32_t, DrawKey> by_id;
};

class RebuildBudget {
public:
    void Reset(uint64_t budget_) {
        budget = budget_;
        used = 0;
    }
    void BeginFrame() {
        starved = deferred;
        deferred = false;
        used = 0;
    }
    bool Take(uint64_t triangles) {
        if (budget && used && used + triangles > budget) {
            deferred = true;
            return false;
        }
        used += triangles;
        return true;
    }
    bool TakeSpare(uint64_t triangles) {
        if (starved || (budget && used + triangles > budget)) {
            return false;
        }
        used += triangles;
        return true;
    }
    uint64_t Used() const { return used; }
    uint64_t Budget() const { return budget; }

private:
    uint64_t budget{}, used{};
    bool deferred{}, starved{};
};

class RefitBudget {
public:
    void Reset(uint64_t budget_, uint64_t max_hold_) {
        budget = budget_;
        max_hold = max_hold_;
        used = 0;
    }
    void BeginFrame(uint64_t frame_) {
        frame = frame_;
        used = 0;
    }
    bool Due(uint64_t stale, uint32_t id) const {
        return stale >= max_hold && (frame + id) % max_hold == 0;
    }
    bool Take(uint64_t triangles, uint64_t stale, uint32_t id) {
        if (budget && used && used + triangles > budget && !Due(stale, id)) {
            return false;
        }
        used += triangles;
        return true;
    }
    uint64_t Used() const { return used; }
    uint64_t Budget() const { return budget; }
    uint64_t MaxHold() const { return max_hold; }

private:
    uint64_t budget{}, used{}, max_hold{3}, frame{};
};

enum class Step : uint32_t { Hit, Refit, Rebuild, Defer, Hold };

inline Step PlanStep(const BlasCache::Lookup& lookup, uint32_t triangles, RebuildBudget& budget,
                     RefitBudget& refits) {
    if (lookup.entry && !lookup.rebuild) {
        return Step::Hit;
    }
    if (lookup.refit) {
        return refits.Take(triangles, lookup.stale, lookup.entry->id) ? Step::Refit : Step::Hold;
    }
    if (lookup.limit) {
        if (budget.TakeSpare(triangles)) {
            return Step::Rebuild;
        }
        return refits.Take(triangles, lookup.stale, lookup.entry->id) ? Step::Refit : Step::Hold;
    }
    if (budget.Take(triangles)) {
        return Step::Rebuild;
    }
    return lookup.entry && lookup.entry->valid ? Step::Hold : Step::Defer;
}

inline uint32_t RefitsAfter(Step step, const BlasCache::Lookup& lookup, uint32_t id,
                            uint32_t max_refits = MaxRefits) {
    if (step == Step::Refit) {
        return lookup.entry ? lookup.entry->refits + 1 : 1;
    }
    return lookup.limit ? 0 : RefitPhase(id, max_refits);
}

}

#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

#include "rt_gi.h"

namespace Vulkan::Rt {

constexpr uint32_t PtlasKeptPartitions = 16;
constexpr uint32_t PtlasPartitions = 1 + PtlasKeptPartitions;
constexpr uint32_t PtlasGlobal = ~0u;
constexpr uint32_t PtlasNoIndex = ~0u;
constexpr float PtlasKeptCell = 64.0f;
constexpr uint64_t PtlasHeaderBytes = 256;
constexpr uint32_t PtlasHeaderWords = 16;
constexpr uint64_t PtlasCommandBytes = 24;
constexpr uint64_t PtlasWriteBytes = 104;
constexpr uint64_t PtlasAuxBytes = 16;

struct PtlasInstance {
    Affine transform{};
    uint64_t blas{};
    uint32_t custom_index{};
    uint32_t mask{};
    uint32_t flags{};
    uint32_t partition{};
};

constexpr bool PtlasActive(const PtlasInstance& in) {
    return in.blas != 0 && in.mask != 0;
}

constexpr bool PtlasSame(const PtlasInstance& a, const PtlasInstance& b) {
    if (!PtlasActive(a) || !PtlasActive(b)) {
        return PtlasActive(a) == PtlasActive(b);
    }
    return a.transform == b.transform && a.blas == b.blas && a.custom_index == b.custom_index &&
           a.mask == b.mask && a.flags == b.flags && a.partition == b.partition;
}

constexpr uint32_t PtlasFlags(uint32_t khr_flags) {
    return khr_flags & 0xFu;
}

constexpr uint32_t PtlasHostWrite = 0x400u;
constexpr uint32_t PtlasHostRead = 0x800u;

constexpr uint32_t PtlasHostAccess(bool cpu_reads) {
    return cpu_reads ? PtlasHostRead : PtlasHostWrite;
}

using PtlasCenter = std::array<float, 4>;

inline uint32_t PtlasPartition(bool dynamic, bool kept, const Affine& world,
                               const PtlasCenter& center = {}) {
    if (dynamic) {
        return PtlasGlobal;
    }
    if (!kept) {
        return 0;
    }
    float x = world[3], z = world[11];
    if (center[3] != 0.0f && std::isfinite(center[0]) && std::isfinite(center[1]) &&
        std::isfinite(center[2])) {
        x += world[0] * center[0] + world[1] * center[1] + world[2] * center[2];
        z += world[8] * center[0] + world[9] * center[1] + world[10] * center[2];
    }
    const int64_t cx = int64_t(std::floor(x / PtlasKeptCell));
    const int64_t cz = int64_t(std::floor(z / PtlasKeptCell));
    uint64_t h = uint64_t(cx) * 0x9E3779B97F4A7C15ull ^ uint64_t(cz) * 0xC2B2AE3D27D4EB4Full;
    h ^= h >> 29;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 32;
    return 1 + uint32_t(h % PtlasKeptPartitions);
}

struct PtlasSlotLayout {
    uint64_t commands{};
    uint64_t writes{};
    uint64_t aux{};
    uint64_t bytes{};
};

constexpr uint64_t PtlasAlign(uint64_t v, uint64_t a) {
    return (v + a - 1) / a * a;
}

constexpr PtlasSlotLayout PtlasLayout(uint32_t instances, uint32_t positions) {
    PtlasSlotLayout l{};
    l.commands = PtlasHeaderWords * 4;
    l.writes = PtlasAlign(l.commands + PtlasCommandBytes, 16);
    l.aux = PtlasAlign(l.writes + (uint64_t(instances) + positions) * PtlasWriteBytes, 16);
    l.bytes = PtlasAlign(l.aux + uint64_t(positions) * PtlasAuxBytes, 256);
    return l;
}

constexpr uint64_t PtlasStateHeaderBytes = 256;
constexpr uint64_t PtlasShadowBytes = 112;

constexpr uint64_t PtlasStateBytes(uint32_t instances) {
    return PtlasStateHeaderBytes + uint64_t(instances) * PtlasShadowBytes;
}

struct PtlasWrite {
    uint32_t index{};
    PtlasInstance instance{};
};

struct PtlasPlan {
    std::vector<uint32_t> index;
    std::vector<uint32_t> write_of;
    std::vector<PtlasWrite> writes;
    uint32_t refits{};
};

class PtlasPlanner {
public:
    void Reset(uint32_t entries) {
        entries_ = entries;
        state_.assign(InstanceCount(), {});
        known_.assign(InstanceCount(), 0);
        seen_.assign(InstanceCount(), 0);
        active_.clear();
        stamp_ = 0;
        initialized_ = false;
    }

    uint32_t InstanceCount() const {
        return entries_ * 2;
    }

    const PtlasInstance& State(uint32_t index) const {
        return state_[index];
    }

    const PtlasPlan& Plan(std::span<const PtlasInstance> now, std::span<const uint32_t> entries,
                          std::span<const uint8_t> dirty = {}) {
        ++stamp_;
        plan_.index.assign(now.size(), PtlasNoIndex);
        plan_.write_of.assign(now.size(), 0);
        plan_.writes.clear();
        plan_.refits = 0;
        next_active_.clear();
        for (size_t i = 0; i < now.size() && i < entries.size(); ++i) {
            const uint32_t entry = entries[i];
            if (entry >= entries_) {
                continue;
            }
            uint32_t idx = entry;
            if (seen_[idx] == stamp_) {
                idx = entries_ + entry;
                if (seen_[idx] == stamp_) {
                    continue;
                }
            }
            seen_[idx] = stamp_;
            plan_.index[i] = idx;
            PtlasInstance want = now[i];
            if (!PtlasActive(want)) {
                want.blas = 0;
                if (known_[idx]) {
                    want.partition = state_[idx].partition;
                }
            }
            const bool refit = i < dirty.size() && dirty[i] != 0 && PtlasActive(want);
            if (!known_[idx] || !PtlasSame(state_[idx], want) || refit) {
                plan_.refits += refit ? 1u : 0u;
                plan_.writes.push_back({idx, want});
                plan_.write_of[i] = uint32_t(plan_.writes.size());
                state_[idx] = want;
                known_[idx] = 1;
            }
            if (PtlasActive(state_[idx])) {
                next_active_.push_back(idx);
            }
        }
        for (const uint32_t idx : active_) {
            if (seen_[idx] != stamp_ && PtlasActive(state_[idx])) {
                state_[idx].blas = 0;
                plan_.writes.push_back({idx, state_[idx]});
            }
        }
        if (!initialized_) {
            for (uint32_t idx = 0; idx < InstanceCount(); ++idx) {
                if (!known_[idx]) {
                    state_[idx] = {};
                    known_[idx] = 1;
                    plan_.writes.push_back({idx, state_[idx]});
                }
            }
            initialized_ = true;
        }
        active_.swap(next_active_);
        return plan_;
    }

private:
    uint32_t entries_{};
    std::vector<PtlasInstance> state_;
    std::vector<uint8_t> known_;
    std::vector<uint64_t> seen_;
    std::vector<uint32_t> active_;
    std::vector<uint32_t> next_active_;
    uint64_t stamp_{};
    bool initialized_{};
    PtlasPlan plan_;
};

}

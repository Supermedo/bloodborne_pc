// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <deque>
#include <mutex>
#include <type_traits>
#include <vector>

#include "bbport_toggles.h"
#include "common/debug.h"
#include "common/types.h"
#include "core/emulator_settings.h"
#include "video_core/buffer_cache/region_manager.h"

namespace VideoCore {

class MemoryTracker {
public:
    static constexpr size_t MAX_CPU_PAGE_BITS = 40;
    static constexpr size_t NUM_HIGH_PAGES = 1ULL << (MAX_CPU_PAGE_BITS - TRACKER_HIGHER_PAGE_BITS);
    static constexpr size_t MANAGER_POOL_SIZE = 32;

public:
    explicit MemoryTracker(PageManager& tracker_) : tracker{&tracker_} {}
    ~MemoryTracker() = default;

    /// Returns true if a region has been modified from the CPU
    bool IsRegionCpuModified(VAddr query_cpu_addr, u64 query_size) noexcept {
        return IteratePages<true>(
            query_cpu_addr, query_size, [](RegionManager* manager, u64 offset, size_t size) {
                return manager->template IsRegionModified<Type::CPU>(offset, size);
            });
    }

    /// Returns true if a region has been modified from the GPU
    bool IsRegionGpuModified(VAddr query_cpu_addr, u64 query_size) noexcept {
        return IteratePages<false>(
            query_cpu_addr, query_size, [](RegionManager* manager, u64 offset, size_t size) {
                return manager->template IsRegionModified<Type::GPU>(offset, size);
            });
    }

    /// Mark region as CPU modified, notifying the device_tracker about this change
    void MarkRegionAsCpuModified(VAddr dirty_cpu_addr, u64 query_size) {
        IteratePages<false>(dirty_cpu_addr, query_size,
                            [](RegionManager* manager, u64 offset, size_t size) {
                                std::scoped_lock lk{manager->lock};
                                manager->template ChangeRegionState<Type::CPU, true>(
                                    manager->GetCpuAddr() + offset, size);
                            });
    }

    /// bbport: see RegionManager::ExtendWriteFault.
    void ExtendWriteFault(VAddr window_addr, u64 size) {
        IteratePages<false>(window_addr, size,
                            [](RegionManager* manager, u64 offset, size_t size) {
                                BbStats::LockTimed(manager->lock, BbStats::t_fault_region_wait);
                                std::scoped_lock lk{std::adopt_lock, manager->lock};
                                manager->ExtendWriteFault(manager->GetCpuAddr() + offset, size);
                            });
    }

    /// Unmark region as modified from the host GPU
    void UnmarkRegionAsGpuModified(VAddr dirty_cpu_addr, u64 query_size) noexcept {
        IteratePages<false>(dirty_cpu_addr, query_size,
                            [](RegionManager* manager, u64 offset, size_t size) {
                                std::scoped_lock lk{manager->lock};
                                manager->template ChangeRegionState<Type::GPU, false>(
                                    manager->GetCpuAddr() + offset, size);
                            });
    }

    /// Removes all protection from a page and ensures GPU data has been flushed if requested
    void InvalidateRegion(VAddr cpu_addr, u64 size, auto&& on_flush) noexcept {
        IteratePages<false>(
            cpu_addr, size, [&on_flush](RegionManager* manager, u64 offset, size_t size) {
                const bool should_flush = [&] {
                    // Perform both the GPU modification check and CPU state change with the lock
                    // in case we are racing with GPU thread trying to mark the page as GPU
                    // modified. If we need to flush the flush function is going to perform CPU
                    // state change.
                    BbStats::LockTimed(manager->lock, BbStats::t_fault_region_wait);
                    std::scoped_lock lk{std::adopt_lock, manager->lock};
                    if (EmulatorSettings.GetReadbacksMode() != GpuReadbacksMode::Disabled &&
                        manager->template IsRegionModified<Type::GPU>(offset, size)) {
                        return true;
                    }
                    manager->template ChangeRegionState<Type::CPU, true>(
                        manager->GetCpuAddr() + offset, size);
                    return false;
                }();
                if (should_flush) {
                    on_flush();
                }
            });
    }

    /// bbport: a guest write fault on [cpu_addr, cpu_addr + size), unprotecting the fault window
    /// (`window` bytes, aligned; 0: none) with it in one step (RegionManager::OnWriteFault).
    /// GPU-modified data there is flushed first as in InvalidateRegion, then the window is
    /// extended on its own.
    void InvalidateWriteFault(VAddr cpu_addr, u64 size, u64 window, auto&& on_flush) noexcept {
        const VAddr window_addr = window != 0 ? cpu_addr & ~(window - 1) : cpu_addr;
        RegionManager* manager = top_tier[cpu_addr >> TRACKER_HIGHER_PAGE_BITS];
        if (window == 0 || window > TRACKER_HIGHER_PAGE_SIZE || manager == nullptr ||
            cpu_addr + size > window_addr + window) {
            InvalidateRegion(cpu_addr, size, on_flush);
            if (window != 0) {
                ExtendWriteFault(window_addr, window);
            }
            return;
        }
        const u64 offset = cpu_addr & TRACKER_HIGHER_PAGE_MASK;
        bool should_flush = false;
        {
            BbStats::LockTimed(manager->lock, BbStats::t_fault_region_wait);
            std::scoped_lock lk{std::adopt_lock, manager->lock};
            if (EmulatorSettings.GetReadbacksMode() != GpuReadbacksMode::Disabled &&
                manager->template IsRegionModified<Type::GPU>(offset, size)) {
                should_flush = true;
            } else {
                manager->OnWriteFault(cpu_addr, size, window_addr, window);
            }
        }
        if (should_flush) {
            on_flush();
            ExtendWriteFault(window_addr, window);
        }
    }

    /// Call 'func' for each CPU modified range and unmark those pages as CPU modified
    void ForEachUploadRange(VAddr query_cpu_range, u64 query_size, bool is_written, auto&& func,
                            auto&& on_upload) {
        // Guest write faults in a region wait while it is locked here (re-protecting the
        // uploaded pages; a written binding keeps it across the upload): the time held is
        // counted with BB_FRAME_STATS=1.
        const bool time_held = is_written && BbStats::enabled;
        const auto held_since =
            time_held ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        IteratePages<true>(query_cpu_range, query_size,
                           [&func, is_written](RegionManager* manager, u64 offset, size_t size) {
                               // bbport: read-only bindings skip the region lock when no page
                               // in range is CPU-modified. A guest write sets its bit in the
                               // fault handler before the store, and the draw that uses the
                               // data reaches this thread through the submission queue lock.
                               if (!is_written &&
                                   !BbToggle::Disabled(BbToggle::LockFreeUploadCheck) &&
                                   !manager->template IsRegionModified<Type::CPU>(offset, size)) {
                                   return;
                               }
                               BbStats::LockTimed(manager->lock, BbStats::t_gpu_region_wait);
                               const auto locked_at = BbStats::enabled && !is_written
                                                          ? std::chrono::steady_clock::now()
                                                          : std::chrono::steady_clock::time_point{};
                               manager->template ForEachModifiedRange<Type::CPU, true>(
                                   manager->GetCpuAddr() + offset, size, func);
                               if (!is_written) {
                                   manager->lock.unlock();
                                   if (BbStats::enabled) {
                                       BbStats::t_upload_lock_held.fetch_add(
                                           std::chrono::duration_cast<std::chrono::nanoseconds>(
                                               std::chrono::steady_clock::now() - locked_at)
                                               .count(),
                                           std::memory_order_relaxed);
                                   }
                               }
                           });
        on_upload();
        if (!is_written) {
            return;
        }
        IteratePages<false>(query_cpu_range, query_size,
                            [&func, is_written](RegionManager* manager, u64 offset, size_t size) {
                                manager->template ChangeRegionState<Type::GPU, true>(
                                    manager->GetCpuAddr() + offset, size);
                                manager->lock.unlock();
                            });
        if (time_held) {
            BbStats::t_upload_lock_held.fetch_add(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - held_since)
                    .count(),
                std::memory_order_relaxed);
        }
    }

    /// Call 'func' for each GPU modified range and unmark those pages as GPU modified
    template <bool clear>
    void ForEachDownloadRange(VAddr query_cpu_range, u64 query_size, auto&& func) {
        IteratePages<false>(query_cpu_range, query_size,
                            [&func](RegionManager* manager, u64 offset, size_t size) {
                                std::scoped_lock lk{manager->lock};
                                manager->template ForEachModifiedRange<Type::GPU, clear>(
                                    manager->GetCpuAddr() + offset, size, func);
                            });
    }

private:
    /**
     * @brief IteratePages Iterates L2 word manager page table.
     * @param cpu_address Start byte cpu address
     * @param size Size in bytes of the region of iterate.
     * @param func Callback for each word manager.
     * @return
     */
    template <bool create_region_on_fail, typename Func>
    bool IteratePages(VAddr cpu_address, size_t size, Func&& func) {
        RENDERER_TRACE;
        using FuncReturn = typename std::invoke_result<Func, RegionManager*, u64, size_t>::type;
        static constexpr bool BOOL_BREAK = std::is_same_v<FuncReturn, bool>;
        std::size_t remaining_size{size};
        std::size_t page_index{cpu_address >> TRACKER_HIGHER_PAGE_BITS};
        u64 page_offset{cpu_address & TRACKER_HIGHER_PAGE_MASK};
        while (remaining_size > 0) {
            const std::size_t copy_amount{
                std::min<std::size_t>(TRACKER_HIGHER_PAGE_SIZE - page_offset, remaining_size)};
            auto* manager{top_tier[page_index]};
            if (manager) {
                if constexpr (BOOL_BREAK) {
                    if (func(manager, page_offset, copy_amount)) {
                        return true;
                    }
                } else {
                    func(manager, page_offset, copy_amount);
                }
            } else if constexpr (create_region_on_fail) {
                CreateRegion(page_index);
                manager = top_tier[page_index];
                if constexpr (BOOL_BREAK) {
                    if (func(manager, page_offset, copy_amount)) {
                        return true;
                    }
                } else {
                    func(manager, page_offset, copy_amount);
                }
            }
            page_index++;
            page_offset = 0;
            remaining_size -= copy_amount;
        }
        return false;
    }

    void CreateRegion(std::size_t page_index) {
        const VAddr base_cpu_addr = page_index << TRACKER_HIGHER_PAGE_BITS;
        if (free_managers.empty()) {
            manager_pool.emplace_back();
            auto& last_pool = manager_pool.back();
            for (size_t i = 0; i < MANAGER_POOL_SIZE; i++) {
                std::construct_at(&last_pool[i], tracker, 0);
                free_managers.push_back(&last_pool[i]);
            }
        }
        // Each manager tracks a 4_MB virtual address space.
        auto* new_manager = free_managers.back();
        new_manager->SetCpuAddress(base_cpu_addr);
        free_managers.pop_back();
        top_tier[page_index] = new_manager;
    }

    PageManager* tracker;
    std::deque<std::array<RegionManager, MANAGER_POOL_SIZE>> manager_pool;
    std::vector<RegionManager*> free_managers;
    std::array<RegionManager*, NUM_HIGH_PAGES> top_tier{};
};

} // namespace VideoCore

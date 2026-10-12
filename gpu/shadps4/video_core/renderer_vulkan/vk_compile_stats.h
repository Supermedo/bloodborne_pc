// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project / bbport
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include "common/types.h"

namespace Vulkan {

/// bbport: shader/pipeline compile work, reported by BB_FRAME_STATS.
/// Translation and pipeline creation can run on worker threads; gpu_wait_* is only
/// the GPU command thread blocked on that work.
inline std::atomic<u32> g_bb_compiles{0};
inline std::atomic<u64> g_bb_compile_ns{0};
inline std::atomic<u32> g_bb_translate{0};
inline std::atomic<u64> g_bb_translate_ns{0};
inline std::atomic<u32> g_bb_pipeline{0};
inline std::atomic<u64> g_bb_pipeline_ns{0};
inline std::atomic<u32> g_bb_gpu_wait{0};
inline std::atomic<u64> g_bb_gpu_wait_ns{0};
inline std::atomic<u32> g_bb_motion_compiles{0};

enum class CompileKind : u32 { Translate, Pipeline };

struct CompileTimer {
    CompileKind kind;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~CompileTimer() {
        const u64 ns = u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now() - start)
                               .count());
        g_bb_compile_ns += ns;
        ++g_bb_compiles;
        switch (kind) {
        case CompileKind::Translate:
            g_bb_translate_ns += ns;
            ++g_bb_translate;
            break;
        case CompileKind::Pipeline:
            g_bb_pipeline_ns += ns;
            ++g_bb_pipeline;
            break;
        }
    }
};

struct GpuCompileWaitTimer {
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~GpuCompileWaitTimer() {
        const u64 ns = u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now() - start)
                               .count());
        g_bb_gpu_wait_ns += ns;
        ++g_bb_gpu_wait;
    }
};

} // namespace Vulkan

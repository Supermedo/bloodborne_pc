// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: BB_WAIT_TRACE=1 — where the time waiting for GPU ticks (Scheduler::Wait, "GPU ticks"
// in the frame stats) goes: each wait is attributed to its thread and call stack, and the
// frame stats print the stacks that waited longest. Diagnostic only (a backtrace per wait).
#pragma once

#include <chrono>
#include <cstdint>

namespace BbWaitTrace {
bool Enabled();

/// Measures one wait and attributes it to the calling stack (when enabled).
class Scope {
public:
    Scope();
    ~Scope();

private:
    void* frames[10];
    int depth = 0;
    std::chrono::steady_clock::time_point start;
};

/// Prints the stacks that waited longest since the last report, as a share of `window_s`.
void Report(double window_s);
} // namespace BbWaitTrace

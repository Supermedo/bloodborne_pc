// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace AmdGpu {

// Only the command processor consumes this queue. Guest threads may enqueue between any
// two calls to pending(). Ownership of the caches and scheduler must pass from the draw
// pipe before EVERY callback, including one which arrived after an empty observation.
template <typename Pending, typename Pop, typename Drain>
void DispatchHostCommands(Pending&& pending, Pop&& pop, Drain&& drain) {
    while (pending()) {
        drain();
        auto callback = pop(); // pop releases the queue mutex before the callback runs
        callback();
    }
}

} // namespace AmdGpu

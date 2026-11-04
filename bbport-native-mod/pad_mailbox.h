// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

namespace AtlasInput {
struct PadSample {
    uint32_t buttons;
    uint8_t lx, ly, rx, ry;

    bool Neutral() const {
        const auto centered = [](uint8_t axis) { return axis > 104 && axis < 152; };
        return centered(lx) && centered(ly) && centered(rx) && centered(ry);
    }
};

// Render work must never own this mutex. Button transitions and neutral samples
// survive a busy render thread; repeated analog samples keep only the latest
// position. The neutral transition is significant to the map's opening guard.
class PadMailbox {
public:
    PadMailbox() { pending_.reserve(32); }

    void Push(PadSample sample) {
        std::scoped_lock lock{mutex_};
        if (!pending_.empty() && pending_.back().buttons == sample.buttons &&
            pending_.back().Neutral() == sample.Neutral()) {
            pending_.back() = sample;
        } else {
            pending_.push_back(sample);
        }
    }

    // The caller owns and reuses batch; no routing or rendering runs under the
    // mailbox lock. A separate state lock serializes consumers.
    void Take(std::vector<PadSample>& batch) {
        batch.clear();
        std::scoped_lock lock{mutex_};
        pending_.swap(batch);
    }

    void Clear() {
        std::scoped_lock lock{mutex_};
        pending_.clear();
    }

private:
    std::mutex mutex_;
    std::vector<PadSample> pending_;
};
} // namespace AtlasInput

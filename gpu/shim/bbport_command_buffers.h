// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <span>
#include <vector>

// A submission owns its top-level command bytes until its decoder coroutine
// retires. SubmitDone/new frames cannot reuse a previous submission's storage.
class BbCommandBufferCopy {
public:
    BbCommandBufferCopy(std::span<const std::uint32_t> dcb,
                        std::span<const std::uint32_t> ccb)
        : draw(dcb.begin(), dcb.end()), constant(ccb.begin(), ccb.end()) {}

    std::span<const std::uint32_t> Draw() const { return draw; }
    std::span<const std::uint32_t> Constant() const { return constant; }

private:
    const std::vector<std::uint32_t> draw, constant;
};

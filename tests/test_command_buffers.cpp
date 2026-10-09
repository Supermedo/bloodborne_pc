// SPDX-License-Identifier: GPL-2.0-or-later
#include "gpu/shim/bbport_command_buffers.h"

#include <algorithm>
#include <cassert>
#include <coroutine>
#include <exception>
#include <memory>
#include <vector>

struct Decoder {
    struct promise_type {
        Decoder get_return_object() { return {std::coroutine_handle<promise_type>::from_promise(*this)}; }
        std::suspend_always initial_suspend() { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        std::suspend_always yield_value(int) { return {}; }
        void return_void() {}
        void unhandled_exception() { std::terminate(); }
    };
    std::coroutine_handle<promise_type> handle;
    ~Decoder() { handle.destroy(); }
};

Decoder Decode(std::span<const std::uint32_t> dcb, std::span<const std::uint32_t> ccb,
               std::shared_ptr<const BbCommandBufferCopy> owner) {
    (void)owner;
    assert(dcb.front() == 0xc0033700 && ccb.front() == 0xc0018900);
    co_yield 0;
    assert(dcb.back() == 0xc0331000 && ccb.back() == 7);
}

int main() {
    std::vector<std::uint32_t> draw{0xc0033700, 1, 2, 0xc0331000}, constant{0xc0018900, 7};
    std::weak_ptr<const BbCommandBufferCopy> retired;
    {
        auto copy = std::make_shared<BbCommandBufferCopy>(draw, constant);
        retired = copy;
        const auto dcb = copy->Draw(), ccb = copy->Constant();
        auto decoder = Decode(dcb, ccb, std::move(copy));
        // The guest resets/reuses its frame buffers before GPU execution starts.
        std::fill(draw.begin(), draw.end(), 0);
        constant.clear();
        decoder.handle.resume();
        assert(!decoder.handle.done() && !retired.expired());
        // Another frame and a much larger submission cannot invalidate the old spans.
        draw.assign(1024 * 1024, 0xffffffff);
        BbCommandBufferCopy next(draw, {});
        assert(next.Draw().size() == draw.size() && next.Constant().empty());
        decoder.handle.resume();
        assert(decoder.handle.done() && !retired.expired());
    }
    assert(retired.expired());
    BbCommandBufferCopy empty({}, {});
    assert(empty.Draw().empty() && empty.Constant().empty());
}

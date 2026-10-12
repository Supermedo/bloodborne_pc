// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project / bbport
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/vk_compile_queue.h"

#include <algorithm>
#include <cstdlib>

#include "bbport_threads.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"

namespace Vulkan {

CompileQueue::CompileQueue() {
    u32 count = 2;
    if (const char* env = std::getenv("BB_COMPILE_WORKERS")) {
        count = static_cast<u32>(std::clamp(std::atoi(env), 1, 8));
    } else {
        // Driver pipeline creation is single-threaded per call (~10 ms each on AMD): bursts
        // of a new area finish in proportion to the number of workers.
        count = std::clamp<u32>(BbThreads::Available() / 2, 2, 8);
    }
    threads.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        threads.emplace_back([this](std::stop_token stoken) { Worker(stoken); });
    }
}

CompileQueue::~CompileQueue() {
    {
        std::scoped_lock lock{mutex};
        stop = true;
    }
    cv.notify_all();
    threads.clear();
}

std::shared_future<void> CompileQueue::Request(u64 key, std::function<void()> fn) {
    return Enqueue(key, std::move(fn));
}

std::shared_future<void> CompileQueue::Enqueue(u64 key, std::function<void()> fn) {
    {
        std::scoped_lock lock{mutex};
        auto it = pending.find(key);
        if (it != pending.end() && !it->second.empty()) {
            return it->second.back();
        }
    }
    std::packaged_task<void()> task(std::move(fn));
    std::shared_future<void> future = task.get_future().share();
    {
        std::scoped_lock lock{mutex};
        auto it = pending.find(key);
        if (it != pending.end() && !it->second.empty()) {
            return it->second.back();
        }
        pending[key].push_back(future);
        queue.push_back(Item{key, std::move(task)});
    }
    cv.notify_one();
    return future;
}

void CompileQueue::Wait(u64 key) {
    // Never run compile work on the caller: the GPU command thread waits here during draws.
    // Inline pipeline creation beside the recording threads has caused device lost.
    // The job the draw needs is moved to the front, so it does not sit behind the rest of a
    // new area's burst.
    std::shared_future<void> future;
    {
        std::scoped_lock lock{mutex};
        const auto it = pending.find(key);
        if (it == pending.end() || it->second.empty()) {
            return;
        }
        future = it->second.back();
        const auto queued = std::ranges::find_if(queue, [key](const Item& item) {
            return item.key == key && item.task.valid();
        });
        if (queued != queue.end() && queued != queue.begin()) {
            Item item = std::move(*queued);
            queue.erase(queued);
            queue.push_front(std::move(item));
        }
    }
    cv.notify_one();
    if (future.valid()) {
        future.wait();
    }
}

bool CompileQueue::WaitFor(u64 key, std::chrono::milliseconds timeout) {
    std::vector<std::shared_future<void>> wait;
    {
        std::scoped_lock lock{mutex};
        auto it = pending.find(key);
        if (it == pending.end()) {
            return true;
        }
        wait = it->second;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (auto& future : wait) {
        if (!future.valid()) {
            continue;
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (left.count() < 0 || future.wait_for(left) != std::future_status::ready) {
            return false;
        }
    }
    return true;
}

bool CompileQueue::HasPending(u64 key) const {
    std::scoped_lock lock{mutex};
    auto it = pending.find(key);
    return it != pending.end() && !it->second.empty();
}

void CompileQueue::WaitIdle() {
    for (;;) {
        std::vector<std::shared_future<void>> wait;
        {
            std::scoped_lock lock{mutex};
            for (auto& [_, jobs] : pending) {
                wait.insert(wait.end(), jobs.begin(), jobs.end());
            }
        }
        if (wait.empty()) {
            return;
        }
        for (auto& future : wait) {
            if (future.valid()) {
                future.wait();
            }
        }
        std::scoped_lock lock{mutex};
        bool any = false;
        for (auto& [_, jobs] : pending) {
            any = any || !jobs.empty();
        }
        if (!any) {
            return;
        }
    }
}

void CompileQueue::Run(Item& item) {
    item.task();
    std::scoped_lock lock{mutex};
    auto it = pending.find(item.key);
    if (it != pending.end()) {
        auto& jobs = it->second;
        std::erase_if(jobs, [](const std::shared_future<void>& f) {
            return !f.valid() || f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
        });
        if (jobs.empty()) {
            pending.erase(it);
        }
    }
}

void CompileQueue::Worker(std::stop_token stoken) {
    Common::SetCurrentThreadName("bb:ShaderCompile");
    for (;;) {
        Item item;
        {
            std::unique_lock lock{mutex};
            Common::CondvarWait(cv, lock, stoken, [&] { return stop || !queue.empty(); });
            if (stoken.stop_requested() || (stop && queue.empty())) {
                return;
            }
            item = std::move(queue.front());
            queue.pop_front();
        }
        if (item.task.valid()) {
            Run(item);
        }
    }
}

} // namespace Vulkan

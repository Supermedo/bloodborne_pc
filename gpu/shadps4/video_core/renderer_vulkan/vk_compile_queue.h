// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project / bbport
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "common/types.h"

namespace Vulkan {

/// Background GCN→SPIR-V / pipeline create queue. Jobs for the same key are coalesced;
/// the GPU thread waits on the key instead of compiling inline when work is already in flight.
class CompileQueue {
public:
    CompileQueue();
    ~CompileQueue();

    CompileQueue(const CompileQueue&) = delete;
    CompileQueue& operator=(const CompileQueue&) = delete;

    /// Enqueue `fn` for `key` if nothing is already running for that key.
    /// Returns a future that completes when the (possibly earlier) job for `key` finishes.
    std::shared_future<void> Request(u64 key, std::function<void()> fn);

    /// Wait until every job currently associated with `key` has finished. A job still queued
    /// is moved to the front. The caller never runs it: the GPU thread waits here during draws.
    void Wait(u64 key);

    /// Wait up to `timeout` for jobs of `key`. False if any job is still running.
    bool WaitFor(u64 key, std::chrono::milliseconds timeout);

    [[nodiscard]] bool HasPending(u64 key) const;

    void WaitIdle();

private:
    struct Item {
        u64 key;
        std::packaged_task<void()> task;
    };

    std::shared_future<void> Enqueue(u64 key, std::function<void()> fn);
    void Run(Item& item);
    void Worker(std::stop_token stoken);

    /// A stolen job leaves an Item with an empty task behind; workers skip those.
    std::deque<Item> queue;
    mutable std::mutex mutex;
    std::condition_variable_any cv;
    std::unordered_map<u64, std::vector<std::shared_future<void>>> pending;
    bool stop = false;
    std::vector<std::jthread> threads;
};

} // namespace Vulkan

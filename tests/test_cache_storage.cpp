// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise the production I/O worker without a game, window or Vulkan device.
#include "../gpu/shadps4/video_core/cache_storage.cpp"
#include "test_assert.h"
#include <algorithm>
#include <chrono>
#include <cstdio>

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    const bool benchmark = argc > 1 && std::string_view{argv[1]} == "--benchmark";
    const auto directory = fs::temp_directory_path() /
        ("bb-cache-storage-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(directory);
    constexpr size_t Count = 64, Bytes = 1024 * 1024;
    size_t unmoved = 0;
    const auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < Count; ++i) {
        std::vector<u8> data(Bytes, u8(i));
        Storage::WriteVector(Storage::BlobType::ShaderBinary, directory / std::to_string(i),
                             std::move(data));
        unmoved += !data.empty();
    }
    const double enqueue_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    std::jthread worker{Storage::ProcessIO};
    if (benchmark) {
        // Benchmark old/new producer work with the same payload and I/O workload.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (!fs::exists(directory / "63.spv")) {
            assert(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    worker.request_stop(); // queued writes must complete even on immediate shutdown
    worker.join();
    for (size_t i = 0; i < Count; ++i) {
        std::vector<u8> data;
        auto path = directory / std::to_string(i);
        Storage::LoadVector(Storage::BlobType::ShaderBinary, path, data);
        assert(data.size() == Bytes);
        assert(std::ranges::all_of(data, [i](u8 b) { return b == u8(i); }));
    }
    if (!benchmark) {
        assert(unmoved == 0);
        // Multiple producers, overwritten keys, reopening the same worker queue.
        std::vector<std::thread> producers;
        for (unsigned t = 0; t < 4; ++t) {
            producers.emplace_back([&, t] {
                for (unsigned i = 0; i < 100; ++i) {
                    std::vector<u8> data(4096, u8(i));
                    Storage::WriteVector(Storage::BlobType::ShaderMeta,
                        directory / ("producer-" + std::to_string(t)), std::move(data));
                    assert(data.empty());
                }
            });
        }
        std::jthread next_worker{Storage::ProcessIO};
        for (auto& p : producers) p.join();
        next_worker.request_stop();
        next_worker.join();
        for (unsigned t = 0; t < 4; ++t) {
            std::vector<u8> data;
            auto path = directory / ("producer-" + std::to_string(t));
            Storage::LoadVector(Storage::BlobType::ShaderMeta, path, data);
            assert(data.size() == 4096 && std::ranges::all_of(data, [](u8 b) { return b == 99; }));
        }
        assert(req_queue.empty());
    }
    assert(directory.parent_path() == fs::temp_directory_path() &&
           directory.filename().string().starts_with("bb-cache-storage-"));
    fs::remove_all(directory);
    std::printf("Cache storage: %zu MiB, producer %.3f ms, payloads copied %zu/%zu\n",
                Count * Bytes / (1024 * 1024), enqueue_ms, unmoved, Count);
    std::puts("PASS: cache contents, ownership, immediate-stop drain, concurrent overwrite and reopen");
}

// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/elf_info.h"
#include "common/io_file.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"
#include "core/emulator_settings.h"

#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"

#include <miniz.h>

#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <vector>

namespace {

std::mutex submit_mutex{};
u32 num_requests{};
std::condition_variable_any request_cv{};
std::queue<std::packaged_task<void()>> req_queue{};
std::mutex m_request{};

mz_zip_archive zip_ar{};
bool ar_is_read_only{true};

// bbport: cache statistics (BB_CACHE_STATS=1). Counted on the loading threads, so atomics.
struct CacheStats {
    std::atomic<u64> hits{0};
    std::atomic<u64> misses{0};
    std::atomic<u64> read_bytes{0};
    std::atomic<u64> written_bytes{0};
};
CacheStats cache_stats{};

bool CacheStatsEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("BB_CACHE_STATS");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

// bbport: BB_CACHE_MAX_MB caps the on-disk cache (default 2048, 0 disables); the oldest blobs by
// modification time are removed first, so a long playthrough cannot fill the disk unnoticed.
u64 CacheMaxBytes() {
    static const u64 limit = [] {
        const char* value = std::getenv("BB_CACHE_MAX_MB");
        const long mb = value != nullptr && *value != '\0' ? std::strtol(value, nullptr, 10) : 2048;
        return mb > 0 ? static_cast<u64>(mb) * 1024ULL * 1024ULL : 0ULL;
    }();
    return limit;
}

void PruneCache(const std::filesystem::path& dir) {
    const u64 limit = CacheMaxBytes();
    if (limit == 0) {
        return;
    }
    std::error_code error;
    std::vector<std::pair<std::filesystem::file_time_type, std::pair<u64, std::filesystem::path>>> files;
    u64 total = 0;
    for (const auto& entry : std::filesystem::directory_iterator{dir, error}) {
        if (!entry.is_regular_file(error)) {
            continue;
        }
        const auto size = static_cast<u64>(entry.file_size(error));
        if (error) {
            continue;
        }
        total += size;
        files.emplace_back(entry.last_write_time(error), std::pair<u64, std::filesystem::path>{size, entry.path()});
    }
    if (total <= limit) {
        return;
    }
    std::sort(files.begin(), files.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    // Leave some headroom so the sweep does not run on every start.
    const u64 target = limit - limit / 10;
    u64 removed = 0;
    for (const auto& [time, info] : files) {
        if (total <= target) {
            break;
        }
        if (std::filesystem::remove(info.second, error)) {
            total -= std::min(total, info.first);
            ++removed;
        }
    }
    LOG_WARNING(Render, "Shader cache pruned: {} file(s) removed, {} MiB left (cap {} MiB)", removed,
                total >> 20, limit >> 20);
}

} // namespace

namespace Storage {

void ProcessIO(const std::stop_token& stoken) {
    Common::SetCurrentThreadName("shadPS4:PipelineCacheIO");

    while (!stoken.stop_requested()) {
        {
            std::unique_lock lk{submit_mutex};
            Common::CondvarWait(request_cv, lk, stoken, [&] { return num_requests; });
        }

        if (stoken.stop_requested()) {
            break;
        }

        while (num_requests) {
            std::packaged_task<void()> request{};
            {
                std::scoped_lock lock{m_request};
                if (req_queue.empty()) {
                    continue;
                }
                request = std::move(req_queue.front());
                req_queue.pop();
            }

            if (request.valid()) {
                request();
                request.get_future().wait();
            }

            --num_requests;
        }
    }
}

constexpr const char* DriverCacheExtension = ".vkcache";

constexpr std::string GetBlobFileExtension(BlobType type) {
    switch (type) {
    case BlobType::ShaderMeta: {
        return "meta";
    }
    case BlobType::ShaderBinary: {
        return "spv";
    }
    case BlobType::PipelineKey: {
        return "key";
    }
    case BlobType::ShaderProfile: {
        return "bin";
    }
    default:
        UNREACHABLE();
    }
}

void DataBase::Open() {
    if (opened) {
        return;
    }

    const auto& game_info = Common::ElfInfo::Instance();

    using namespace Common::FS;
    // bbport: BB_CACHE_DIR points every install (installer edition, portable edition, a second
    // build) at one cache root instead of three separate copies under each user directory and
    // %APPDATA%; the blobs are per game serial and share nothing else.
    std::filesystem::path cache_root = GetUserPath(PathType::CacheDir);
    if (const char* override_dir = std::getenv("BB_CACHE_DIR"); override_dir != nullptr && *override_dir != '\0') {
        cache_root = std::filesystem::u8path(override_dir);
    }
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        mz_zip_zero_struct(&zip_ar);

        cache_path = cache_root / std::filesystem::path{game_info.GameSerial()}.replace_extension(".zip");

        if (!mz_zip_reader_init_file(&zip_ar, cache_path.string().c_str(),
                                     MZ_ZIP_FLAG_READ_ALLOW_WRITING) ||
            !mz_zip_validate_archive(&zip_ar, 0)) {
            LOG_INFO(Render, "Cache archive {} is not found or archive is corrupted",
                     cache_path.string().c_str());
            mz_zip_reader_end(&zip_ar);
            mz_zip_writer_init_file(&zip_ar, cache_path.string().c_str(), 0);
        }
    } else {
        cache_path = cache_root / game_info.GameSerial();
        if (!std::filesystem::exists(cache_path)) {
            std::filesystem::create_directories(cache_path);
        }
        PruneCache(cache_path);
    }

    io_worker = std::jthread{ProcessIO};
    opened = true;
}

void DataBase::Close() {
    if (!IsOpened()) {
        return;
    }

    io_worker.request_stop();
    io_worker.join();

    if (EmulatorSettings.IsPipelineCacheArchived()) {
        mz_zip_writer_finalize_archive(&zip_ar);
        mz_zip_writer_end(&zip_ar);
    }

    opened = false;
    if (CacheStatsEnabled()) {
        const u64 hits = cache_stats.hits.load(std::memory_order_relaxed);
        const u64 misses = cache_stats.misses.load(std::memory_order_relaxed);
        const u64 total = hits + misses;
        LOG_INFO(Render,
                 "Shader cache: {} hit(s) / {} miss(es) ({:.1f}% reuse), {} MiB read, {} MiB written, "
                 "dir {}",
                 hits, misses, total ? 100.0 * static_cast<double>(hits) / static_cast<double>(total) : 0.0,
                 cache_stats.read_bytes.load(std::memory_order_relaxed) >> 20,
                 cache_stats.written_bytes.load(std::memory_order_relaxed) >> 20, cache_path.string());
    }
    LOG_INFO(Render, "Cache dumped");
}

template <typename T>
bool WriteVector(const BlobType type, std::filesystem::path&& path_, std::vector<T>&& v) {
    {
        auto request = std::packaged_task<void()>{[=]() {
            auto path{path_};
            path.replace_extension(GetBlobFileExtension(type));
            if (EmulatorSettings.IsPipelineCacheArchived()) {
                ASSERT_MSG(!ar_is_read_only,
                           "The archive is read-only. Did you forget to call `FinishPreload`?");
                if (!mz_zip_writer_add_mem(&zip_ar, path.string().c_str(), v.data(),
                                           v.size() * sizeof(T), MZ_BEST_COMPRESSION)) {
                    LOG_ERROR(Render, "Failed to add {} to the archive", path.string().c_str());
                }
            } else {
                // bbport: write beside and rename into place, so a game that ends during the
                // write (the queue is not drained at exit) never leaves a truncated blob that
                // the next start would load.
                using namespace Common::FS;
                auto temp{path};
                temp += ".tmp";
                {
                    const auto file = IOFile{temp, FileAccessMode::Create};
                    if (!file.IsOpen() || file.Write(v) != v.size()) {
                        return;
                    }
                }
                std::error_code error;
                std::filesystem::rename(temp, path, error);
                if (!error) {
                    cache_stats.written_bytes += v.size() * sizeof(T);
                }
            }
        }};
        std::scoped_lock lock{m_request};
        req_queue.emplace(std::move(request));
    }

    std::scoped_lock lk{submit_mutex};
    ++num_requests;
    request_cv.notify_one();
    return true;
}

template <typename T>
void LoadVector(BlobType type, std::filesystem::path& path, std::vector<T>& v) {
    using namespace Common::FS;
    path.replace_extension(GetBlobFileExtension(type));
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        int index{-1};
        index = mz_zip_reader_locate_file(&zip_ar, path.string().c_str(), nullptr, 0);
        if (index < 0) {
            cache_stats.misses.fetch_add(1, std::memory_order_relaxed);
            LOG_WARNING(Render, "File {} is not found in the archive", path.string().c_str());
            return;
        }
        mz_zip_archive_file_stat stat{};
        mz_zip_reader_file_stat(&zip_ar, index, &stat);
        v.resize(stat.m_uncomp_size / sizeof(T));
        mz_zip_reader_extract_to_mem(&zip_ar, index, v.data(), stat.m_uncomp_size, 0);
        cache_stats.hits.fetch_add(1, std::memory_order_relaxed);
        cache_stats.read_bytes.fetch_add(stat.m_uncomp_size, std::memory_order_relaxed);
    } else {
        // bbport: a miss is "this blob is not on disk yet" (first run in an area, or a new shader).
        if (!std::filesystem::exists(path)) {
            cache_stats.misses.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const auto file = IOFile{path, FileAccessMode::Read};
        if (!file.IsOpen()) {
            cache_stats.misses.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        v.resize(file.GetSize() / sizeof(T));
        file.Read(v);
        cache_stats.hits.fetch_add(1, std::memory_order_relaxed);
        cache_stats.read_bytes.fetch_add(file.GetSize(), std::memory_order_relaxed);
    }
}

bool DataBase::Save(BlobType type, const std::string& name, std::vector<u8>&& data) {
    if (!opened) {
        return false;
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    return WriteVector(type, std::move(path), std::move(data));
}

bool DataBase::Save(BlobType type, const std::string& name, std::vector<u32>&& data) {
    if (!opened) {
        return false;
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    return WriteVector(type, std::move(path), std::move(data));
}

void DataBase::Load(BlobType type, const std::string& name, std::vector<u8>& data) {
    if (!opened) {
        return;
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    return LoadVector(type, path, data);
}

void DataBase::Load(BlobType type, const std::string& name, std::vector<u32>& data) {
    if (!opened) {
        return;
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    return LoadVector(type, path, data);
}

void DataBase::ForEachBlob(BlobType type, const std::function<void(std::vector<u8>&& data)>& func) {
    const auto& ext = GetBlobFileExtension(type);
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        const auto num_files = mz_zip_reader_get_num_files(&zip_ar);
        for (int index = 0; index < num_files; ++index) {
            std::array<char, MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE> file_name{};
            file_name.fill(0);
            mz_zip_reader_get_filename(&zip_ar, index, file_name.data(), file_name.size());
            if (std::string{file_name.data()}.ends_with(ext)) {
                mz_zip_archive_file_stat stat{};
                mz_zip_reader_file_stat(&zip_ar, index, &stat);
                std::vector<u8> data(stat.m_uncomp_size);
                mz_zip_reader_extract_to_mem(&zip_ar, index, data.data(), data.size(), 0);
                func(std::move(data));
            }
        }
    } else {
        for (const auto& file_name : std::filesystem::directory_iterator{cache_path}) {
            if (file_name.path().extension().string().ends_with(ext)) {
                using namespace Common::FS;
                const auto& file = IOFile{file_name, FileAccessMode::Read};
                if (file.IsOpen()) {
                    std::vector<u8> data(file.GetSize());
                    file.Read(data);
                    func(std::move(data));
                }
            }
        }
    }
}

std::filesystem::path DataBase::DriverCachePath() const {
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        return std::filesystem::path{cache_path}.replace_extension(DriverCacheExtension);
    }
    return cache_path / (std::string{"driver"} + DriverCacheExtension);
}

void DataBase::Clear() {
    if (!opened || EmulatorSettings.IsPipelineCacheArchived()) {
        return;
    }
    std::error_code ec;
    u64 removed = 0;
    for (const auto& entry : std::filesystem::directory_iterator(cache_path, ec)) {
        if (entry.is_regular_file(ec) && entry.path().extension() != DriverCacheExtension &&
            std::filesystem::remove(entry.path(), ec)) {
            ++removed;
        }
    }
    LOG_WARNING(Render, "Pipeline cache cleared ({} files): it is rebuilt for this build", removed);
}

void DataBase::FinishPreload() {
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        mz_zip_writer_init_from_reader(&zip_ar, cache_path.string().c_str());
        ar_is_read_only = false;
    }
}

} // namespace Storage

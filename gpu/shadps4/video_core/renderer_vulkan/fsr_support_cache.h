// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <utility>

namespace Vulkan {
struct FsrSupportResult { bool supported{}; int status{5}; };

// Owned by one logical Vulkan device, never shared across adapters/devices.
// Successful discovery is reusable; failures remain retryable even without file changes.
class FsrSupportCache {
    struct Stamp {
        std::filesystem::path path;
        bool present{};
        std::uintmax_t size{};
        std::filesystem::file_time_type modified{};
        bool operator==(const Stamp&) const = default;
    };
    struct Entry { Stamp stamp; FsrSupportResult result; };
    std::array<std::optional<Entry>,2> entries;
    std::mutex mutex;
    static Stamp Read(const std::filesystem::path& path) {
        Stamp result{path.lexically_normal()};
        std::error_code error;
        result.present=std::filesystem::is_regular_file(path,error);
        if (result.present) {
            result.size=std::filesystem::file_size(path,error);
            if (error) { result.present=false;return result; }
            result.modified=std::filesystem::last_write_time(path,error);
            if (error) result.present=false;
        }
        return result;
    }
public:
    template<class Probe>
    FsrSupportResult Get(unsigned method,const std::filesystem::path& runtime,bool refresh,Probe&& probe) {
        const auto stamp=Read(runtime);
        std::scoped_lock lock{mutex};
        auto& entry=entries.at(method);
        if (!refresh && entry && entry->stamp==stamp) return entry->result;
        entry.reset();
        const auto result=std::forward<Probe>(probe)();
        if (result.supported && result.status==0) entry=Entry{stamp,result};
        return result;
    }
};
} // namespace Vulkan

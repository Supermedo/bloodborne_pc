#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <filesystem>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Vulkan::Rt {

struct FileStamp {
    int64_t mtime{-1};
    int64_t size{-1};
    bool operator==(const FileStamp&) const = default;
};

inline bool StampFile(const char* path, FileStamp& out) {
    if (!path || !*path) {
        return false;
    }
    std::error_code ec;
    const std::filesystem::path p{path};
    const auto mtime = std::filesystem::last_write_time(p, ec);
    if (ec) {
        return false;
    }
    const auto size = std::filesystem::file_size(p, ec);
    if (ec) {
        return false;
    }
    out = {int64_t(mtime.time_since_epoch().count()), int64_t(size)};
    return true;
}

inline constexpr uint32_t SpvMagic = 0x07230203u;

inline std::vector<uint32_t> ReadSpv(const char* path) {
    std::vector<uint32_t> code;
    FILE* f = std::fopen(path, "rb");
    if (!f) {
        return code;
    }
    std::fseek(f, 0, SEEK_END);
    const long bytes = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (bytes >= 20 && bytes % 4 == 0) {
        code.resize(size_t(bytes) / 4);
        if (std::fread(code.data(), 1, size_t(bytes), f) != size_t(bytes) || code[0] != SpvMagic) {
            code.clear();
        }
    }
    std::fclose(f);
    return code;
}

inline std::string_view TrimView(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' ||
                          s.front() == '\n')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' ||
                          s.back() == '\n')) {
        s.remove_suffix(1);
    }
    return s;
}

inline bool ParseConfigLine(std::string_view line,
                            std::unordered_map<std::string, std::string>& out) {
    line = TrimView(line);
    if (line.empty() || line.front() == '#' || line.front() == ';') {
        return false;
    }
    const size_t eq = line.find('=');
    if (eq == std::string_view::npos) {
        return false;
    }
    const std::string_view key = TrimView(line.substr(0, eq));
    std::string_view value = TrimView(line.substr(eq + 1));
    if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') &&
        value.back() == value.front()) {
        value = value.substr(1, value.size() - 2);
    }
    if (key.empty()) {
        return false;
    }
    out[std::string(key)] = std::string(value);
    return true;
}

class LiveConfig {
public:
    void Open(const char* path) {
        path_ = path ? path : "";
    }
    bool Enabled() const {
        return !path_.empty();
    }
    const std::string& Path() const {
        return path_;
    }
    bool Poll() {
        if (path_.empty()) {
            return false;
        }
        FileStamp stamp{};
        if (!StampFile(path_.c_str(), stamp) || stamp == stamp_) {
            return false;
        }
        FILE* f = std::fopen(path_.c_str(), "r");
        if (!f) {
            return false;
        }
        std::unordered_map<std::string, std::string> values;
        char line[1024];
        while (std::fgets(line, sizeof(line), f)) {
            ParseConfigLine(line, values);
        }
        const bool failed = std::ferror(f) != 0;
        std::fclose(f);
        if (failed) {
            return false;
        }
        stamp_ = stamp;
        values_ = std::move(values);
        ++generation_;
        return true;
    }
    const char* Get(const char* name) const {
        if (values_.empty()) {
            return nullptr;
        }
        const auto it = values_.find(name);
        return it == values_.end() ? nullptr : it->second.c_str();
    }
    uint64_t Generation() const {
        return generation_;
    }
    const std::unordered_map<std::string, std::string>& Values() const {
        return values_;
    }

private:
    std::string path_;
    FileStamp stamp_{};
    uint64_t generation_ = 0;
    std::unordered_map<std::string, std::string> values_;
};

inline LiveConfig& LiveKnobs() {
    static LiveConfig config;
    return config;
}

inline const char* Knob(const char* name) {
    if (const char* value = LiveKnobs().Get(name)) {
        return value;
    }
    return std::getenv(name);
}

inline bool KnobIs(const char* name, char on) {
    const char* value = Knob(name);
    return value && value[0] == on;
}

inline bool KnobOn(const char* name) {
    const char* value = Knob(name);
    return !value || value[0] != '0';
}

inline bool KnobEquals(const char* name, std::string_view expected) {
    const char* value = Knob(name);
    return value && std::string_view(value) == expected;
}

}

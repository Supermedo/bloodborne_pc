#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
    // Per-map keys: a line `m24_01:BB_RT_X=v` wins over `BB_RT_X=v` while the player stands in
    // map m24_01 (Central Yharnam). Returns true when the map changed (the knobs need a re-read).
    bool SetMap(const std::string& map) {
        if (map == map_) {
            return false;
        }
        map_ = map;
        return true;
    }
    const std::string& Map() const {
        return map_;
    }
    size_t MapOverrides() const {
        size_t n = 0;
        const std::string prefix = map_ + ":";
        for (const auto& kv : values_) {
            n += !map_.empty() && kv.first.compare(0, prefix.size(), prefix) == 0 ? 1u : 0u;
        }
        return n;
    }
    const char* Get(const char* name) const {
        if (values_.empty()) {
            return nullptr;
        }
        if (!map_.empty()) {
            const auto mit = values_.find(map_ + ":" + name);
            if (mit != values_.end()) {
                return mit->second.c_str();
            }
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
    std::string map_;
    std::unordered_map<std::string, std::string> values_;
};

inline LiveConfig& LiveKnobs() {
    static LiveConfig config;
    return config;
}

// Built-in per-map look (docs/PATH_TRACING.md "Per-map look"): used when neither rt.cfg nor the
// environment sets the knob. Stock's own exposure differs per map, so one key cannot match all.
struct MapKnobDefault {
    const char* map;
    const char* name;
    const char* value;
};
// Keys from a 4-5 spot RT/stock survey per map (2026-11-20): the key that leaves the median spot
// just under stock (-0.2 EV, more dramatic). -5.5 where the 1.5-stop boost cap binds first.
inline constexpr MapKnobDefault MapKnobDefaults[] = {
    {"m21_00", "BB_RT_EXPOSURE_KEY", "-5.5"}, // Hunter's Dream (stock -4.7)
    {"m21_01", "BB_RT_EXPOSURE_KEY", "-5.5"}, // Hunter's Dream workshop
    {"m22_00", "BB_RT_EXPOSURE_KEY", "-7.0"}, // Hemwick (at -6.5 +0.5 EV over stock)
    {"m23_00", "BB_RT_EXPOSURE_KEY", "-5.5"}, // Old Yharnam (cap binds)
    {"m24_00", "BB_RT_EXPOSURE_KEY", "-6.2"}, // Cathedral Ward (stock -5.7 .. -6.3)
    {"m24_01", "BB_RT_EXPOSURE_KEY", "-6.5"}, // Central Yharnam (stock -6.47)
    {"m24_02", "BB_RT_EXPOSURE_KEY", "-5.5"}, // Upper Cathedral Ward (cap binds)
    {"m25_00", "BB_RT_EXPOSURE_KEY", "-5.3"}, // Cainhurst (at -6.5 -0.9 EV)
    {"m26_00", "BB_RT_EXPOSURE_KEY", "-6.3"}, // Nightmare of Mensis
    {"m27_00", "BB_RT_EXPOSURE_KEY", "-6.1"}, // Forbidden Woods (stock -5.9)
    {"m28_00", "BB_RT_EXPOSURE_KEY", "-6.9"}, // Yahar'gul (at -6.5 +0.1 EV)
    {"m32_00", "BB_RT_EXPOSURE_KEY", "-5.5"}, // Byrgenwerth (cap binds)
    {"m33_00", "BB_RT_EXPOSURE_KEY", "-4.8"}, // Nightmare Frontier (at -6.5 -1.0 EV)
    {"m34_00", "BB_RT_EXPOSURE_KEY", "-4.8"}, // Hunter's Nightmare (at -6.5 -0.9 EV)
    {"m35_00", "BB_RT_EXPOSURE_KEY", "-5.5"}, // Research Hall (cap binds)
    {"m36_00", "BB_RT_EXPOSURE_KEY", "-5.5"}, // Fishing Hamlet (cap binds)
    // Mist maps: the traced fog's single scatter gets no sky or moon under canopy and cliffs,
    // so the stock pale mist went black (Frontier 65% of pixels under 5 vs stock 14%,
    // Byrgenwerth mid b -2.7 vs stock -8.3). The game's own fog keeps the mist (LOOK: fog
    // stays the stock pale mist colour).
    {"m32_00", "BB_RT_FX_FOG", "0"}, // Byrgenwerth
    {"m33_00", "BB_RT_FX_FOG", "0"}, // Nightmare Frontier
    {"m34_00", "BB_RT_FX_FOG", "0"}, // Hunter's Nightmare
    {"m36_00", "BB_RT_FX_FOG", "0"}, // Fishing Hamlet
    // Yahar'gul: GI sky misses keep the painted purple dome (mid b -1.6 -> -2.4, stock -2.9).
    {"m28_00", "BB_RT_GI_SKY_TINT", "0"},
};

inline const char* MapKnobDefault(const std::string& map, const char* name) {
    for (const auto& d : MapKnobDefaults) {
        if (map == d.map && std::strcmp(name, d.name) == 0) {
            return d.value;
        }
    }
    return nullptr;
}

inline const char* Knob(const char* name) {
    if (const char* value = LiveKnobs().Get(name)) {
        return value;
    }
    if (const char* value = std::getenv(name)) {
        return value;
    }
    return MapKnobDefault(LiveKnobs().Map(), name);
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

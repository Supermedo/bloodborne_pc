// bbport: BB_CE_STATS=1 — what the constant engine's dumps feed (GPU command thread only):
// how many DumpConstRam a second, how many bytes and pages, and how often the translator then
// reads memory a dump wrote in the last 100 ms, by kind. Measures what a translation of the
// constant engine has to keep visible to the translator (docs/EMULATION_REMOVAL_PLAN.ru.md).
#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

namespace BbCeStats {

enum Kind : unsigned {
    UserDataPointer, ///< a 64-bit user data value pointing into dumped pages (SRT/descriptor tables)
    RingConstants,   ///< constant buffer copied into the constant ring
    LargeBuffer,     ///< buffer bound on the recording thread (not copied)
    Vertex,          ///< vertex stream range
    Index,           ///< index buffer range
    DmaStage,        ///< a shader stage that reads memory itself (uses_dma)
    NumKinds,
};

inline bool Enabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_CE_STATS");
        return env && env[0] == '1';
    }();
    return enabled;
}

enum Writer : unsigned { ConstantEngine, WriteData, Dma, NumWriters };

struct State {
    /// 4 KiB page -> last write time, per writer.
    std::array<std::unordered_map<std::uint64_t, std::uint64_t>, NumWriters> page_ns;
    std::array<std::uint64_t, NumWriters> writes{}, bytes{};
    std::array<std::array<std::uint64_t, NumKinds>, NumWriters> hits{};
    std::array<std::uint64_t, NumKinds> checks{};
    std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
};
inline State& Get() {
    static State state;
    return state;
}
inline std::uint64_t NowNs() {
    return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count());
}

inline void Report() {
    auto& s = Get();
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - s.last).count();
    if (seconds < 2.0) {
        return;
    }
    static const char* names[NumKinds] = {"user data pointers", "ring constants", "large buffers",
                                          "vertex", "index", "dma stages"};
    static const char* writers[NumWriters] = {"DumpConstRam", "WriteData", "DmaData"};
    for (unsigned w = 0; w < NumWriters; ++w) {
        std::printf("CE stats: %s %.0f/s, %.1f KiB/s, %zu pages in all; reads of its pages (per s):",
                    writers[w], s.writes[w] / seconds, s.bytes[w] / seconds / 1024.0,
                    s.page_ns[w].size());
        for (unsigned k = 0; k < NumKinds; ++k) {
            std::printf(" %s %.0f/%.0f;", names[k], s.hits[w][k] / seconds, s.checks[k] / seconds);
        }
        std::printf("\n");
    }
    s.writes = {};
    s.bytes = {};
    s.hits = {};
    s.checks = {};
    s.last = now;
}

inline void NoteWrite(Writer writer, std::uint64_t address, std::uint64_t size) {
    auto& s = Get();
    ++s.writes[writer];
    s.bytes[writer] += size;
    const std::uint64_t now = NowNs();
    for (std::uint64_t page = address >> 12; page <= (address + size - 1) >> 12 &&
                                              page - (address >> 12) < 4096; ++page) {
        s.page_ns[writer][page] = now;
    }
    Report();
}
inline void NoteDump(std::uint64_t address, std::uint64_t size) {
    NoteWrite(ConstantEngine, address, size);
}

/// Whether [address, address+size) touches a page `writer` wrote in the last 100 ms.
inline bool Recent(Writer writer, std::uint64_t address, std::uint64_t size) {
    auto& s = Get();
    if (size == 0 || s.page_ns[writer].empty()) {
        return false;
    }
    const std::uint64_t now = NowNs();
    const std::uint64_t last = (address + size - 1) >> 12;
    for (std::uint64_t page = address >> 12; page <= last && page - (address >> 12) < 64; ++page) {
        const auto it = s.page_ns[writer].find(page);
        if (it != s.page_ns[writer].end() && now - it->second < 100'000'000) {
            return true;
        }
    }
    return false;
}

inline void Check(Kind kind, std::uint64_t address, std::uint64_t size) {
    auto& s = Get();
    ++s.checks[kind];
    for (unsigned w = 0; w < NumWriters; ++w) {
        if (Recent(Writer(w), address, size)) {
            ++s.hits[w][kind];
        }
    }
}

} // namespace BbCeStats

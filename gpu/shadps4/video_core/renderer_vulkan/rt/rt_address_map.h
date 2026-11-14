#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <iterator>
#include <map>
#include <mutex>

namespace Vulkan::Rt::AddressMap {

enum class Kind : uint32_t { Rt, Game };

struct Range {
    uint64_t end;
    uint32_t serial;
    Kind kind;
};

struct Freed {
    uint64_t begin;
    uint64_t end;
    Kind kind;
    std::chrono::steady_clock::time_point when;
};

inline std::mutex mutex;
inline std::map<uint64_t, Range> live;
inline std::deque<Freed> freed;
inline uint32_t next_serial = 0;
constexpr size_t FreedKeep = 16384;

inline const char* KindName(Kind kind) {
    return kind == Kind::Rt ? "RT buffer" : "game buffer";
}

inline uint32_t Add(uint64_t begin, uint64_t size, Kind kind = Kind::Rt) {
    std::scoped_lock lock{mutex};
    const uint32_t serial = next_serial++;
    live[begin] = {begin + size, serial, kind};
    return serial;
}

inline void Remove(uint64_t begin) {
    std::scoped_lock lock{mutex};
    const auto it = live.find(begin);
    if (it == live.end()) {
        return;
    }
    freed.push_back({begin, it->second.end, it->second.kind, std::chrono::steady_clock::now()});
    if (freed.size() > FreedKeep) {
        freed.pop_front();
    }
    live.erase(it);
}

inline void Describe(uint64_t address, uint64_t precision) {
    std::scoped_lock lock{mutex};
    const uint64_t lo = address > precision ? address - precision : 0;
    const uint64_t hi = address + precision;
    auto it = live.upper_bound(hi);
    if (it != live.begin()) {
        const auto& [begin, r] = *std::prev(it);
        if (r.end > lo) {
            std::printf("GPU:   inside live %s #%u %#llx-%#llx (%llu KiB), offset %#llx\n",
                        KindName(r.kind), r.serial, (unsigned long long)begin,
                        (unsigned long long)r.end, (unsigned long long)((r.end - begin) >> 10),
                        (unsigned long long)(address - begin));
            return;
        }
    }
    const auto now = std::chrono::steady_clock::now();
    for (auto f = freed.rbegin(); f != freed.rend(); ++f) {
        if (f->end > lo && f->begin <= hi) {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - f->when);
            std::printf("GPU:   inside a FREED %s %#llx-%#llx (%llu KiB), freed %lld ms before "
                        "the report\n",
                        KindName(f->kind), (unsigned long long)f->begin,
                        (unsigned long long)f->end,
                        (unsigned long long)((f->end - f->begin) >> 10), (long long)ms.count());
            return;
        }
    }
    std::printf("GPU:   not inside any live or recently freed buffer (%zu live, %zu freed "
                "tracked): likely an image\n",
                live.size(), freed.size());
}

}

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstddef>
#include <cstdio>

namespace BbOut {

using Sink = void (*)(const char* data, std::size_t size);

inline void StdoutSink(const char* data, std::size_t size) {
    std::fwrite(data, 1, size, stdout);
}

inline Sink& CurrentSink() {
    static Sink sink = StdoutSink;
    return sink;
}

inline const std::chrono::steady_clock::time_point start_time = std::chrono::steady_clock::now();

inline double Ms() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_time)
        .count();
}

class Line {
public:
    static constexpr std::size_t Capacity = 8192;

    Line() = default;
    Line(const Line&) = delete;
    Line& operator=(const Line&) = delete;
    ~Line() {
        Flush();
    }

    __attribute__((format(printf, 2, 3))) Line& Add(const char* format, ...) {
        va_list args;
        va_start(args, format);
        AddV(format, args);
        va_end(args);
        return *this;
    }

    __attribute__((format(printf, 2, 0))) void AddV(const char* format, va_list args) {
        if (used >= Capacity - 1) {
            return;
        }
        const int length = std::vsnprintf(buffer + used, Capacity - used, format, args);
        if (length > 0) {
            used = std::min(Capacity - 1, used + std::size_t(length));
        }
    }

    void Flush() {
        if (used) {
            CurrentSink()(buffer, used);
            used = 0;
        }
    }

private:
    char buffer[Capacity];
    std::size_t used = 0;
};

__attribute__((format(printf, 1, 2))) inline void Print(const char* format, ...) {
    Line line;
    va_list args;
    va_start(args, format);
    line.AddV(format, args);
    va_end(args);
}

}

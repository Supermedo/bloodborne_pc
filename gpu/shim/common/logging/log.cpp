// bbport: see log.h.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include "common/logging/log.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace Common::Log {
static Level threshold() {
    static const Level level = [] {
        const char* v = std::getenv("BB_GPU_LOG");
        if (!v) return Level::warn;
        static const char* names[] = {"trace", "debug", "info", "warning", "error", "critical"};
        for (int i = 0; i < 6; ++i)
            if (!std::strcmp(v, names[i])) return Level(i);
        return Level::warn;
    }();
    return level;
}
bool ShouldLog(Level level) { return level >= threshold(); }
void Write(std::string_view log_class, Level level, const char* file, int line, const char* function,
           std::string&& message) {
    static std::mutex mutex;
    static const char* names[] = {"Trace", "Debug", "Info", "Warning", "Error", "Critical"};
    const char* base = std::strrchr(file, '/');
#ifdef _WIN32
    // bbport: one write for the line. stderr has no buffer, so on the launcher's pipe the C runtime
    // wrote it a character at a time, a system call each and mixed with other threads' output: about
    // 1 ms a line on the GPU thread, and a run with 118 warnings a second spent a tenth of its time so.
    const std::string text = fmt::format("GPU [{}] <{}> {}:{} {}: {}\r\n", log_class, names[int(level)],
                                         base ? base + 1 : file, line, function, message);
    DWORD written = 0;
    std::scoped_lock lock{mutex};
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), text.data(), DWORD(text.size()), &written, nullptr);
#else
    std::scoped_lock lock{mutex};
    std::fprintf(stderr, "GPU [%.*s] <%s> %s:%d %s: %s\n", int(log_class.size()), log_class.data(),
                 names[int(level)], base ? base + 1 : file, line, function, message.c_str());
#endif
}
} // namespace Common::Log

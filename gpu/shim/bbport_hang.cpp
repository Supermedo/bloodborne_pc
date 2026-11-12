// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: where the threads are during a black screen that does not go away (a cutscene
// that never starts, a wait that never ends). Once frames have been shown, a snapshot of every
// thread (bbport_hang_snapshot.h) is written when no frame has been shown for 4 s, again every
// 15 s while that lasts, to <user dir>/hang-snapshots/snapshot-N.txt:
// a ring of 9 files. The one taken when frames stopped keeps its file until frames stop again
// (NextSnapshotFile); the others take the remaining 8 in turn. The log gets a line when frames
// stop and when they come back. BB_HANG_SNAPSHOTS=0 turns it off, =2 also writes a snapshot every
// 20 s, to compare a hang with. Once a minute the log gets the frames shown, the longest gap between
// two of them and how many gaps were over 33 ms.

#include "bbport_hang.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

#include "../bbgpu.h"
#include "bbport_hang_snapshot.h"

namespace {
using Clock = std::chrono::steady_clock;

std::atomic<Clock::rep> last_flip{0};
std::atomic<unsigned long long> flips{0};
std::atomic<Clock::rep> worst_gap{0};
std::atomic<unsigned> long_gaps{0};
std::atomic<std::uintptr_t> image_base{0};
std::atomic<std::uint64_t> image_size{0};

#ifdef _WIN32
std::once_flag started;

double SecondsSinceFlip() {
    return std::chrono::duration<double>(Clock::now().time_since_epoch() -
                                         Clock::duration(last_flip.load(std::memory_order_relaxed)))
        .count();
}

/// Writes the next file of the ring; its path. `stopped`: the first snapshot since frames stopped,
/// whose file the ring keeps until they stop again.
std::string Write(const char* reason, bool stopped) {
    static int last = -1, kept = -1;
    const char* user = std::getenv("BB_GPU_USER_DIR");
    const std::string folder = std::string(user && *user ? user : ".") + "/hang-snapshots";
    CreateDirectoryA(folder.c_str(), nullptr);
    last = BbHang::NextSnapshotFile(last, stopped ? -1 : kept, 9);
    if (stopped) {
        kept = last;
    }
    const std::string path = folder + "/snapshot-" + std::to_string(last) + ".txt";
    SYSTEMTIME now;
    GetLocalTime(&now);
    char head[256];
    std::snprintf(head, sizeof(head),
                  "%04u-%02u-%02u %02u:%02u:%02u %s; %llu frames shown, the last %.1f s ago; "
                  "eboot at 0x%llx\n",
                  now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, reason,
                  flips.load(), SecondsSinceFlip(),
                  static_cast<unsigned long long>(image_base.load()));
    BbHangSnapshot::Taker taker{image_base.load(), image_size.load()};
    const std::string threads = taker.Take();
    if (FILE* file = std::fopen(path.c_str(), "wb")) {
        std::fputs(head, file);
        std::fputs(threads.c_str(), file);
        std::fclose(file);
    }
    return "hang-snapshots/snapshot-" + std::to_string(last) + ".txt"; // no user folder in the log
}

void Watch() {
    using namespace std::chrono_literals;
    auto periodic = Clock::now(), minute = periodic;
    unsigned long long minute_flips = flips.load(std::memory_order_relaxed);
    const char* setting = std::getenv("BB_HANG_SNAPSHOTS");
    const bool periodic_stacks = setting && setting[0] == '2';
    Clock::time_point hang_snapshot{};
    bool hung = false;
    for (;;) {
        std::this_thread::sleep_for(250ms);
        const double quiet = SecondsSinceFlip();
        if (quiet >= 4.0) {
            if (!hung || Clock::now() - hang_snapshot >= 15s) {
                char reason[64];
                std::snprintf(reason, sizeof(reason), "no frame shown for %.1f s", quiet);
                const std::string path = Write(reason, !hung);
                if (!hung) {
                    std::printf("Hang check: no frame shown for %.1f s; threads written to %s "
                                "(kept until frames stop again)\n",
                                quiet, path.c_str());
                }
                hang_snapshot = Clock::now();
                hung = true;
            }
            continue;
        }
        if (hung) {
            std::printf("Hang check: frames shown again\n");
            hung = false;
        }
        if (periodic_stacks && Clock::now() - periodic >= 20s) {
            Write("periodic", false);
            periodic = Clock::now();
        }
        if (const auto now = Clock::now(); now - minute >= 60s) {
            const unsigned long long shown = flips.load(std::memory_order_relaxed);
            const double seconds = std::chrono::duration<double>(now - minute).count();
            const auto worst = Clock::duration(worst_gap.exchange(0, std::memory_order_relaxed));
            std::printf("Frames: %llu in %.0f s (%.1f FPS), worst %.1f ms, %u over 33 ms\n",
                        shown - minute_flips, seconds, double(shown - minute_flips) / seconds,
                        std::chrono::duration<double, std::milli>(worst).count(),
                        long_gaps.exchange(0, std::memory_order_relaxed));
            minute = now;
            minute_flips = shown;
        }
    }
}
#endif
} // namespace

namespace BbHang {
void OnFlip() {
    const Clock::rep now = Clock::now().time_since_epoch().count();
    const Clock::rep before = last_flip.exchange(now, std::memory_order_relaxed);
    if (before) {
        const Clock::rep gap = now - before;
        if (gap > worst_gap.load(std::memory_order_relaxed)) {
            worst_gap.store(gap, std::memory_order_relaxed);
        }
        if (Clock::duration(gap) > std::chrono::milliseconds(33)) {
            long_gaps.fetch_add(1, std::memory_order_relaxed);
        }
    }
    flips.fetch_add(1, std::memory_order_relaxed);
#ifdef _WIN32
    std::call_once(started, [] {
        const char* off = std::getenv("BB_HANG_SNAPSHOTS");
        if (!off || off[0] != '0') {
            std::thread(Watch).detach();
        }
    });
#endif
}
} // namespace BbHang

extern "C" void bbgpu_hang_set_image(unsigned char* image, uint64_t size) {
    image_base.store(reinterpret_cast<std::uintptr_t>(image));
    image_size.store(size);
}

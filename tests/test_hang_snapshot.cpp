// Test of gpu/shim/bbport_hang_snapshot.h and bbport_hang.h: a
// thread blocked in a known function shows with a return address into it, it runs again after the
// snapshot, the call check accepts a call site and refuses other bytes, and the file ring keeps the
// file of the snapshot taken when frames stopped.
#include "gpu/shim/bbport_hang.h"
#include "gpu/shim/bbport_hang_snapshot.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

static int failed = 0;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            std::printf("FAIL line %d: %s\n", __LINE__, #c);                                       \
            ++failed;                                                                              \
        }                                                                                          \
    } while (0)

static HANDLE gate;
static std::atomic<bool> entered{false};

__attribute__((noinline)) static void BlockedHere() {
    entered = true;
    WaitForSingleObject(gate, INFINITE);
    asm volatile("" ::: "memory"); // the wait stays a call, not a tail jump
}

__attribute__((noinline)) static std::uintptr_t CalledFrom() {
    return reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
}

int main() {
    gate = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread blocked(BlockedHere);
    while (!entered) {
        Sleep(1);
    }
    Sleep(50);

    HMODULE self = GetModuleHandleW(nullptr);
    MODULEINFO info{};
    GetModuleInformation(GetCurrentProcess(), self, &info, sizeof(info));
    const auto base = reinterpret_cast<std::uintptr_t>(self);
    BbHangSnapshot::Taker taker{base, info.SizeOfImage}; // this program stands for the game's image
    const auto start = std::chrono::steady_clock::now();
    const std::string text = taker.Take();
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

    // A return address inside BlockedHere, just after its call of WaitForSingleObject.
    const auto blocked_at = reinterpret_cast<std::uintptr_t>(&BlockedHere) - base;
    bool found = false;
    for (auto at = text.find("eboot+0x"); at != std::string::npos; at = text.find("eboot+0x", at + 1)) {
        const auto offset = std::strtoull(text.c_str() + at + 8, nullptr, 16);
        found = found || (offset > blocked_at && offset < blocked_at + 0x80);
    }
    CHECK(found);
    CHECK(text.find("ntdll.dll+0x") != std::string::npos); // the blocked thread waits in ntdll
    CHECK(ms < 1000.0);

    // Resumed: the thread leaves its wait when the gate opens.
    SetEvent(gate);
    blocked.join();

    CHECK(BbHangSnapshot::AfterCall(CalledFrom()));
    static const unsigned char data[16] = {};
    CHECK(!BbHangSnapshot::AfterCall(reinterpret_cast<std::uintptr_t>(data + 8)));
    CHECK(!BbHangSnapshot::AfterCall(0));

    // File ring of 9: in order from the start; with a kept file, the other 8 in turn and never the
    // kept one; a new stop may take any file and is kept from then on.
    int last = -1;
    for (int i = 0; i < 10; ++i) {
        last = BbHang::NextSnapshotFile(last, -1, 9);
        CHECK(last == i % 9);
    }
    bool seen[9] = {};
    last = 2;
    for (int i = 0; i < 16; ++i) {
        last = BbHang::NextSnapshotFile(last, 3, 9);
        CHECK(last != 3);
        seen[last] = true;
    }
    for (int f = 0; f < 9; ++f) {
        CHECK(seen[f] == (f != 3));
    }
    CHECK(BbHang::NextSnapshotFile(8, 0, 9) == 1);
    CHECK(BbHang::NextSnapshotFile(2, -1, 9) == 3); // frames stopped: the next file, kept after

    if (failed) {
        std::printf("%s", text.c_str());
    }
    std::printf("hang snapshot: %s, %.1f ms\n", failed ? "FAILED" : "PASS", ms);
    return failed != 0;
}

// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_mouse_trace.h"
#include "bbport_mouse.h"
#include "test_assert.h"
#include <thread>

int main(int argc, char** argv) {
    const bool disabled = argc > 1 && !std::strcmp(argv[1], "disabled");
    const bool blocked = argc > 1 && !std::strcmp(argv[1], "blocked");
    _putenv_s("BB_MOUSE_TRACE", disabled ? "0" : "1");
    if (blocked) {
        FILE* file = std::fopen("logs", "wb");
        assert(file); std::fclose(file);
    }
    std::array<unsigned char, 128> image{};
    image.fill(0x90);
    image[0] = 0xe8; // call image+80
    const std::int32_t call = 75;
    std::memcpy(image.data() + 1, &call, 4);
    // movss xmm0,[rip+89] -> image+102
    const unsigned char load[]{0xf3,0x0f,0x10,0x05,89,0,0,0};
    std::memcpy(image.data() + 5, load, sizeof(load));
    image[13] = image[80] = 0xc3;
    BbMouse::Trace::Start(image.data(), image.size(), 0, 64);
    if (disabled || blocked) {
        assert(!BbMouse::Trace::enabled.load());
        assert(!BbMouse::Trace::mapping);
        if (disabled) assert(!std::filesystem::exists("logs"));
        return 0;
    }
    using namespace BbMouse::Trace;
    assert(enabled.load());
    assert(!std::memcmp(mapping, "BBMOUSE2", 8));
    std::array<unsigned char, 0x298> camera{};
    const float before = .25f, after = -.5f;
    for (auto offset : Fields) std::memcpy(camera.data() + offset, &before, 4);
    for (auto offset : ExtraFields) std::memcpy(camera.data() + offset, &before, 4);
    camera[FlagFields[0]]=1; camera[FlagFields[3]]=1;
    { Sample ignored(camera.data(), 0, 100.f, 3); }
    assert(next.load() == 0); // title/menu waiting cannot exhaust the budget
    BbMouse::Pad(37,37,128,128,0x2000); // same publisher used by the runtime's final pad sample
    const auto motion = std::uint64_t(std::uint32_t(-256)) | std::uint64_t(512) << 32;
    {
        Sample first(camera.data(), motion, 125.f, 11);
        for (auto offset : AfterFields) std::memcpy(camera.data() + offset, &after, 4);
    }
    auto* first = reinterpret_cast<Record*>(mapping + HeaderSize);
    assert(first->sequence == 1 && first->dx == -256 && first->dy == 512);
    assert(first->ticks && first->sensitivity == 125.f && first->flags == 27);
    for (auto field : first->before) assert(field == before);
    for (auto field : first->after) assert(field == after);
    for (auto field : first->extra) assert(field == before);
    assert(first->native_flags==9 && first->pad==0x8080252500002000ull);
    BbMouse::Pad(218,37,128,128,0);
    { Sample native(camera.data(), 0, 100.f, 0); }
    auto* second = first + 1;
    assert(second->sequence == 2 && second->flags == 16 && second->dx == 0);
    assert(second->pad==0x808025da00000000ull);
    // Distinct slots are safe even if two camera callbacks overlap. Collection
    // cannot write past the fixed budget or block on a logger/allocator.
    const auto fill = [&] { for (unsigned n = 0; n < Capacity; ++n) { Sample sample(camera.data(), 0, 100.f, 3); } };
    std::atomic<bool> finished{false};
    std::thread input([&] {
        while (!finished.load(std::memory_order_relaxed)) {
            Pad(37,37,17,231,0x2000);
            Pad(218,37,128,128,0);
        }
    });
    std::thread a(fill), b(fill);
    a.join(); b.join();
    finished.store(true, std::memory_order_relaxed); input.join();
    assert(!enabled.load());
    for (unsigned n = 0; n < Capacity; ++n) {
        assert(first[n].sequence == n + 1);
        if (n>1) assert(first[n].pad==0xe711252500002000ull || first[n].pad==0x808025da00000000ull);
    }
    if (argc > 1 && !std::strcmp(argv[1], "terminate"))
        TerminateProcess(GetCurrentProcess(), 8); // no destructor or explicit flush
    assert(FlushViewOfFile(mapping, 0));
    std::puts("Mouse trace: disabled/failure paths, signed packets, camera basis/follow state, atomic final pad samples, native ownership and bounded concurrent collection passed");
}

// SPDX-License-Identifier: GPL-2.0-or-later
// Execute the actual generated hooks against a synthetic 1.09 camera function.
#include <windows.h>
#include <xbyak/xbyak.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <thread>
#include "bbport_mouse.h"
#include "bbport_settings.h"
#include "test_assert.h"
#include "mouse_camera_fixture.h"

constexpr std::size_t ImageSize = 0x5540000;
constexpr std::array<std::size_t, 5> Sites{0x143ceaa, 0x143c6e8, 0x143c984, 0x143dde6, 0x143c870};
constexpr unsigned char Loads[]{0xc4,0xc1,0x7a,0x10,0x85,0x40,1,0,0,
                               0xc4,0xc1,0x7a,0x10,0x8d,0x50,1,0,0};
// Include the rotation-state fields missing from the original validator, not
// four copies of a store to +140. See docs/mouse-camera-windows.md for references.
std::array<std::uint32_t,4> StoreOffsets{0x130,0x294,0x140,0x150};
std::array<bool,4> rotation_patched{};
struct Report {
    float pitch, smooth, early_yaw;
    std::uint64_t flags, r10;
    std::array<unsigned char,32> ymm;
    std::uint64_t redzone;
    std::uint32_t mxcsr;
};
alignas(32) const std::array<std::uint64_t,4> Pattern{0x123456789abcdef0, 0xfedcba9876543210,
                                                   0x1020304050607080, 0x9988776655443322};
using Run = void (*)(void*, Report*);

struct Caller : Xbyak::CodeGenerator {
    explicit Caller(void* function) {
        using namespace Xbyak::util;
        push(r12); push(r13); push(rdi); sub(rsp, 96);
        vmovdqu(ptr[rsp + 32], ymm15);
        vmovdqu(ptr[rsp + 80], xmm9);
        mov(r13, rcx); mov(rdi, rcx); mov(r12, rdx);
        mov(rax, reinterpret_cast<uintptr_t>(Pattern.data()));
        vmovdqu(ymm15, ptr[rax]);
        mov(eax, 0x3f600000); vmovd(xmm2, eax); // 0.875 for the original camera store
        vmovd(xmm9, eax);
        mov(r10, 0x12349876abcdef55);
        mov(r11, 0x9876543210abcdef);
        mov(ptr[rsp - 136], r11); // lowest address in the guest call's SysV red zone
        stmxcsr(ptr[rsp + 64]);
        mov(dword[rsp + 68], 0x1f80);
        ldmxcsr(ptr[rsp + 68]);
        mov(rax, reinterpret_cast<uintptr_t>(function));
        stc(); call(rax);
        pushfq(); pop(rax);
        mov(ptr[r12 + offsetof(Report, flags)], rax);
        mov(ptr[r12 + offsetof(Report, r10)], r10);
        mov(r11, ptr[rsp - 136]);
        mov(ptr[r12 + offsetof(Report, redzone)], r11);
        stmxcsr(ptr[r12 + offsetof(Report, mxcsr)]);
        ldmxcsr(ptr[rsp + 64]);
        vmovdqu(ptr[r12 + offsetof(Report, ymm)], ymm15);
        vmovss(ptr[r12 + offsetof(Report, pitch)], xmm0);
        vmovss(ptr[r12 + offsetof(Report, smooth)], xmm1);
        vmovss(ptr[r12 + offsetof(Report, early_yaw)], xmm3);
        vmovdqu(ymm15, ptr[rsp + 32]);
        vmovdqu(xmm9, ptr[rsp + 80]);
        add(rsp, 96); pop(rdi); pop(r13); pop(r12); ret();
        ready();
    }
};

template<std::size_t N, class T> void Put(std::array<unsigned char, N>& camera, std::size_t at, T value) {
    std::memcpy(camera.data() + at, &value, sizeof(value));
}
template<std::size_t N> float Get(const std::array<unsigned char, N>& camera, std::size_t at) {
    float value;
    std::memcpy(&value, camera.data() + at, sizeof(value));
    return value;
}
void Check(const Report& report) {
    assert(report.flags & 1); // original flags survive every gate/callback
    assert(report.r10 == 0x12349876abcdef55);
    assert(report.redzone == 0x9876543210abcdef);
    assert(report.mxcsr == 0x1f80);
    assert(!std::memcmp(report.ymm.data(), Pattern.data(), 32)); // including YMM upper half
}

int main(int argc, char** argv) {
    if (argc > 1 && !std::strcmp(argv[1], "convergence"))
        StoreOffsets = {0x26c,0x270,0x144,0x150};
    auto* image = static_cast<unsigned char*>(VirtualAlloc(nullptr, ImageSize,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    assert(image);
    const auto all_stores = CameraFixture::Populate(image, StoreOffsets);
    // Reject a changed orbit equation before patching any entry or angle store.
    image[CameraFixture::Orbit + 9] ^= 1;
    assert(!BbMouse::Install(image, ImageSize));
    assert(!std::memcmp(image + Sites[0], Loads, sizeof(Loads)));
    image[CameraFixture::Orbit + 9] ^= 1;
    // Similar stores outside the camera routine or to unrelated fields must
    // remain untouched. Discovery is confined to the verified camera routine.
    const std::array<unsigned char,9> outside{0xc4,0xc1,0x7a,0x11,0x95,0x40,1,0,0};
    const std::array<unsigned char,9> unrelated{0xc4,0xc1,0x7a,0x11,0x95,0x38,1,0,0};
    std::memcpy(image + CameraFixture::Begin - 32, outside.data(), 9);
    std::memcpy(image + CameraFixture::Begin + 0x1200, unrelated.data(), 9);
    // A changed site rejects the entire patch, without modifying any other code.
    for (unsigned bad = 0; bad < Sites.size(); ++bad) {
        image[Sites[bad]] ^= 1;
        assert(!BbMouse::Install(image, ImageSize));
        assert(image[Sites[0]] == (bad ? 0xc4 : 0xc5));
        image[Sites[bad]] ^= 1;
    }
    // Semantically unsafe or partial instructions must still reject the entire
    // image. Never accept arbitrary nine-byte replacements just to enable F4.
    const std::array<std::array<unsigned char,9>,6> invalid{{
        {0xc4,0xc1,0x7a,0x11,0x97,0x40,1,0,0}, // R15, not the live camera R13
        {0xc4,0xc1,0x7a,0x10,0x95,0x40,1,0,0}, // load, not a store
        {0xc4,0xc1,0x7a,0x11,0x95,0x38,1,0,0}, // unrelated field +138
        {0xc5,0xfa,0x11,0x15,0x40,1,0,0,0x90}, // RIP-relative, wrong length
        {0x49,0x89,0x85,0x40,1,0,0,0x90,0x90}, // integer store
        {0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0xcc}, // partial NOP patch
    }};
    std::array<unsigned char,9> saved{};
    std::memcpy(saved.data(), image + Sites[1], 9);
    for (const auto& bytes : invalid) {
        std::memcpy(image + Sites[1], bytes.data(), 9);
        assert(!BbMouse::Install(image, ImageSize));
        assert(!std::memcmp(image + Sites[0], Loads, sizeof(Loads)));
        assert(!std::memcmp(image + Sites[1], bytes.data(), 9));
    }
    std::memcpy(image + Sites[1], saved.data(), 9);
    // Reject unsafe relocated prologues and an unverified this-pointer register.
    std::array<unsigned char,27> entry_saved{};
    std::memcpy(entry_saved.data(), image + CameraFixture::Begin, entry_saved.size());
    image[CameraFixture::Begin] = 0xe8;
    assert(!BbMouse::Install(image, ImageSize));
    assert(!std::memcmp(image + Sites[0], Loads, sizeof(Loads)));
    std::memcpy(image + CameraFixture::Begin, entry_saved.data(), entry_saved.size());
    image[CameraFixture::Begin + 8] = 0xf5; // mov r13,rsi instead of rdi
    assert(!BbMouse::Install(image, ImageSize));
    std::memcpy(image + CameraFixture::Begin, entry_saved.data(), entry_saved.size());
    const auto extra_site = all_stores[4].offset;
    std::memcpy(saved.data(), image + extra_site, 9);
    std::memset(image + extra_site, 0x90, 9); // missing response store must reject too
    assert(!BbMouse::Install(image, ImageSize));
    assert(!std::memcmp(image + Sites[0], Loads, sizeof(Loads)));
    std::memcpy(image + extra_site, saved.data(), 9);
    assert(!BbMouse::Install(image, 256));
    for (unsigned i = 0; i < rotation_patched.size(); ++i) {
        rotation_patched[i] = argc > 1 && (!std::strcmp(argv[1], "patched") ||
                              (!std::strcmp(argv[1], "mixed") && i % 2 == 0));
        if (rotation_patched[i]) std::memset(image + Sites[i + 1], 0x90, 9);
    }
    assert(BbMouse::Install(image, ImageSize) && BbMouse::Available());
    assert(!std::memcmp(image + CameraFixture::Begin - 32, outside.data(), 9));
    assert(!std::memcmp(image + CameraFixture::Begin + 0x1200, unrelated.data(), 9));
    std::array<std::array<unsigned char,18>,5> installed{};
    std::array<unsigned char,6> entry_installed{};
    std::memcpy(entry_installed.data(), image + CameraFixture::Begin, entry_installed.size());
    for (unsigned i = 0; i < Sites.size(); ++i)
        std::memcpy(installed[i].data(), image + Sites[i], i ? 9 : 18);
    std::vector<std::array<unsigned char,11>> all_installed(all_stores.size());
    for (unsigned i = 0; i < all_stores.size(); ++i)
        std::memcpy(all_installed[i].data(), image + all_stores[i].offset, all_stores[i].size);
    std::array<unsigned char,9> orbit_installed{};
    std::memcpy(orbit_installed.data(), image + CameraFixture::Orbit, orbit_installed.size());
    DWORD old;
    assert(VirtualProtect(image, ImageSize, PAGE_EXECUTE_READ, &old));
    FlushInstructionCache(GetCurrentProcess(), image, ImageSize);
    Caller main_caller(image + CameraFixture::Begin);
    std::array<std::unique_ptr<Caller>, 4> store_callers;
    std::array<Run, 4> stores;
    for (unsigned i = 0; i < stores.size(); ++i) {
        store_callers[i] = std::make_unique<Caller>(image + Sites[i + 1]);
        stores[i] = store_callers[i]->getCode<Run>();
    }
    auto run = main_caller.getCode<Run>();
    std::array<unsigned char, 0x300> camera{};
    Put(camera, 0x140, .2f); Put(camera, 0x144, .3f); Put(camera, 0x150, .2f);
    Put(camera, 0x1f0, -1.94f); Put(camera, 0x1ec, 1.71f);
    Report report{};
    auto& settings = BbSettings::Get();
    settings.mouse_camera = true;
    // Every additional response/interpolation store must be gated, including
    // idle frames. Four auto-rotation stores alone leave this path unchanged.
    for (const auto& site : all_stores) {
        const auto anchor = std::find(CameraFixture::Anchors.begin(), CameraFixture::Anchors.end(), site.offset);
        const bool patched = anchor != CameraFixture::Anchors.end() &&
            rotation_patched[anchor - CameraFixture::Anchors.begin()];
        Caller caller(image + site.offset);
        const auto store = caller.getCode<Run>();
        BbMouse::SetActive(false);
        Put(camera, site.field, .25f);
        store(camera.data(), &report); Check(report);
        assert(Get(camera, site.field) == (patched ? .25f : site.value));
        BbMouse::SetActive(true);
        BbMouse::Motion(1,1); BbMouse::Apply(camera.data()); // claim camera, not merely cursor capture
        Put(camera, site.field, .25f);
        for (unsigned frame = 0; frame < 120; ++frame) store(camera.data(), &report);
        Check(report);
        assert(Get(camera, site.field) == (site.field == 0x130 && !patched ? site.value : .25f));
        Put(camera, 0x154, 1.f);
        store(camera.data(), &report); Check(report);
        assert(Get(camera, site.field) == (patched ? .25f : site.value));
        Put(camera, 0x154, 0.f);
    }
    BbMouse::SetActive(false);
    Put(camera, 0x140, .2f); Put(camera, 0x144, .3f); Put(camera, 0x150, .2f);
    BbMouse::Motion(90,45);
    run(camera.data(), &report);
    assert(report.pitch == .2f); Check(report);
    for (unsigned i = 0; i < stores.size(); ++i) {
        Put(camera, StoreOffsets[i], .2f);
        stores[i](camera.data(), &report);
        assert(Get(camera, StoreOffsets[i]) == (rotation_patched[i] ? .2f : .875f));
        Check(report);
    }
    Put(camera, 0x140, .2f);
    Put(camera, 0x144, .3f); Put(camera, 0x150, .2f);
    BbMouse::SetActive(true);
    BbMouse::Motion(90,45);
    run(camera.data(), &report);
    Check(report);
    assert(std::abs(report.pitch - .25f) < 1e-6f && report.smooth == report.pitch);
    assert(std::abs(Get(camera, 0x144) - .4f) < 1e-6f);
    assert(report.early_yaw == Get(camera, 0x144)); // both axes updated before their first reads
    assert(Get(camera, 0x26c) == report.pitch && Get(camera, 0x270) == Get(camera, 0x144));
    for (unsigned i = 0; i < stores.size(); ++i) {
        Put(camera, StoreOffsets[i], .25f);
        stores[i](camera.data(), &report);
        assert(Get(camera, StoreOffsets[i]) ==
            (StoreOffsets[i] == 0x130 && !rotation_patched[i] ? .875f : .25f));
        Check(report);
    }
    Put(camera, 0x144, .4f);
    Put(camera, 0x154, 1.f);
    BbMouse::Motion(900,900);
    run(camera.data(), &report);
    assert(Get(camera, 0x144) == .4f);
    for (unsigned i = 0; i < stores.size(); ++i) {
        stores[i](camera.data(), &report);
        assert(Get(camera, StoreOffsets[i]) == (rotation_patched[i] ? .25f : .875f));
        Check(report); // lock-on restores game stores; existing NOPs stay NOPs
    }
    Put(camera, 0x144, .4f);
    Put(camera, 0x154, 0.f);
    run(camera.data(), &report);
    assert(Get(camera, 0x144) == .4f); // no buffered jump after unlocking

    // F4 capture alone never disables either stick axis. A deliberate stick
    // takes both axes back, even while mouse packets arrive; releasing it keeps
    // native ownership until the next nonzero mouse delta, without a stale jump.
    BbMouse::SetActive(false); BbMouse::SetActive(true);
    Put(camera, 0x140, .1f); Put(camera, 0x144, .1f);
    for (unsigned i = 0; i < stores.size(); ++i) {
        Put(camera, StoreOffsets[i], .25f);
        stores[i](camera.data(), &report);
        assert(Get(camera, StoreOffsets[i]) == (rotation_patched[i] ? .25f : .875f));
    }
    for (const auto axes : {std::array<std::uint8_t,2>{128,0}, {255,128}, {0,255}}) {
        BbMouse::Motion(90,90);
        BbMouse::Stick(axes[0], axes[1]);
        BbMouse::Motion(900,900); // held stick has priority, discard mouse motion
        run(camera.data(), &report); Check(report);
        for (unsigned i = 0; i < stores.size(); ++i) {
            Put(camera, StoreOffsets[i], .25f);
            stores[i](camera.data(), &report);
            assert(Get(camera, StoreOffsets[i]) == (rotation_patched[i] ? .25f : .875f));
        }
        BbMouse::Stick(128,128);
        Put(camera, 0x140, .1f); Put(camera, 0x144, .1f);
        run(camera.data(), &report);
        assert(Get(camera, 0x140) == .1f && Get(camera, 0x144) == .1f);
        BbMouse::Motion(90,90); run(camera.data(), &report);
        assert(std::abs(report.pitch - .2f) < 1e-6f);
        assert(report.early_yaw == Get(camera, 0x144));
        assert(std::abs(report.pitch - report.early_yaw) < 1e-6f);
    }

    // Model the stale chase targets/correction left by the previous hook.
    // Entry must synchronize them even on idle updates, before either axis is read.
    for (int frame = 0; frame < 120; ++frame) {
        for (auto field : {0x148,0x14c,0x150,0x26c,0x270}) Put(camera, field, -.75f);
        Put(camera, 0x130, .25f); Put(camera, 0x294, .125f);
        run(camera.data(), &report); Check(report);
        assert(std::abs(report.pitch - .2f) < 1e-6f && report.early_yaw == report.pitch);
        for (auto field : {0x148,0x14c,0x150,0x26c,0x270}) assert(Get(camera, field) == report.pitch);
        assert(Get(camera, 0x130) == .25f && Get(camera, 0x294) == 0.f);
    }

    for (int fps : {30, 60, 120, 240}) {
        Put(camera, 0x140, 0.f); Put(camera, 0x144, 0.f);
        for (int n = 0; n < 8000; ++n) {
            BbMouse::Motion(.125f, 0);
            if (n % (8000 / fps) == 0) run(camera.data(), &report);
        }
        run(camera.data(), &report);
        assert(std::abs(Get(camera, 0x144) - 1000.f / 900.f) < .0001f);
    }
    // Equal raw counts must produce equal angles, from a one-packet flick to a
    // slow stream. Stopping input must stop rotation on the next camera update.
    for (int packet : {1, 3, 30, 300, 900}) {
        Put(camera, 0x140, 0.f); Put(camera, 0x144, 0.f);
        for (int count = 0; count < 900; count += packet) {
            BbMouse::Motion(float(packet), float(packet) / 2);
            run(camera.data(), &report);
        }
        assert(std::abs(Get(camera, 0x144) - 1.f) < .00001f);
        assert(std::abs(Get(camera, 0x140) - .5f) < .00001f);
        for (int frame = 0; frame < 120; ++frame) run(camera.data(), &report);
        assert(std::abs(Get(camera, 0x144) - 1.f) < .00001f);
    }
    settings.mouse_invert_y = true;
    settings.mouse_sensitivity = 200.f;
    Put(camera, 0x140, 0.f);
    BbMouse::Motion(0,90); run(camera.data(), &report);
    assert(std::abs(report.pitch + .2f) < 1e-6f);
    BbMouse::Motion(0,1e30f); run(camera.data(), &report);
    assert(report.pitch == -1.94f);
    BbMouse::Motion(std::numeric_limits<float>::infinity(), 1);
    BbMouse::Motion(std::numeric_limits<float>::quiet_NaN(), 1);
    run(camera.data(), &report);
    assert(std::isfinite(report.pitch));
    BbMouse::Motion(900,900);
    BbMouse::SetActive(false); BbMouse::SetActive(true);
    const auto yaw = Get(camera, 0x144);
    run(camera.data(), &report);
    assert(Get(camera, 0x144) == yaw); // focus/menu/toggle discards old motion

    // Binocular target angles and the follow camera stay synchronized, also at rest.
    std::array<unsigned char,0x90> p1{}, p2{};
    std::array<unsigned char,0x280> binocular{};
    auto* p2_ptr = p2.data(); auto* binocular_ptr = binocular.data(); auto* p1_ptr = p1.data();
    std::memcpy(p1.data() + 0x68, &p2_ptr, 8);
    std::memcpy(p2.data() + 0x68, &binocular_ptr, 8);
    assert(VirtualProtect(image + 0x553e000, 4096, PAGE_READWRITE, &old));
    std::memcpy(image + 0x553e8d0, &p1_ptr, 8);
    const int mono_on = 1, mono_off = 0;
    std::memcpy(p2.data() + 0x84, &mono_on, 4);
    settings.mouse_sensitivity = 100.f;
    settings.mouse_invert_y = false;
    BbMouse::Motion(90,45); run(camera.data(), &report);
    assert(std::abs(Get(binocular, 0x148) - .025f) < 1e-6f);
    assert(std::abs(Get(binocular, 0x14c) - .05f) < 1e-6f);
    assert(Get(camera, 0x140) == Get(binocular, 0x148));
    assert(Get(camera, 0x144) == Get(binocular, 0x14c));
    for (int frame = 0; frame < 120; ++frame) run(camera.data(), &report);
    assert(Get(camera, 0x140) == Get(binocular, 0x148));
    assert(Get(camera, 0x144) == Get(binocular, 0x14c));
    std::memcpy(p2.data() + 0x84, &mono_off, 4);

    // A producer on the actual input thread boundary must never split a
    // diagonal packet into an X-only and then Y-only camera update.
    BbMouse::SetActive(false); BbMouse::SetActive(true);
    std::atomic<bool> packets_done{false};
    std::thread producer([&] {
        for (int n = 0; n < 10000; ++n) BbMouse::Motion(.125f,.125f);
        packets_done = true;
    });
    do {
        Put(camera, 0x140, 0.f); Put(camera, 0x144, 0.f);
        run(camera.data(), &report); Check(report);
        assert(report.pitch == report.early_yaw);
    } while (!packets_done);
    producer.join();
    Put(camera, 0x140, 0.f); Put(camera, 0x144, 0.f);
    run(camera.data(), &report);
    assert(report.pitch == report.early_yaw);

    // Repeated toggles never touch instructions that a guest thread is executing.
    std::atomic<bool> stop{false};
    std::thread guest([&] { Report r{}; while (!stop) run(camera.data(), &r); });
    for (int n = 0; n < 10000; ++n) {
        BbMouse::Motion(1,0);
        BbMouse::SetActive(false); BbMouse::SetActive(true);
    }
    stop = true; guest.join();
    for (unsigned i = 0; i < Sites.size(); ++i)
        assert(!std::memcmp(installed[i].data(), image + Sites[i], i ? 9 : 18));
    assert(!std::memcmp(entry_installed.data(), image + CameraFixture::Begin, entry_installed.size()));
    for (unsigned i = 0; i < all_stores.size(); ++i)
        assert(!std::memcmp(all_installed[i].data(), image + all_stores[i].offset, all_stores[i].size));
    assert(!std::memcmp(orbit_installed.data(), image + CameraFixture::Orbit, orbit_installed.size()));

    const auto bench = [&](bool capture, bool moving) {
        BbMouse::SetActive(capture);
        if (capture) { BbMouse::Motion(1,0); run(camera.data(), &report); }
        const auto start = std::chrono::steady_clock::now();
        for (unsigned n = 0; n < 100000; ++n) {
            if (moving) BbMouse::Motion(1,0);
            run(camera.data(), &report);
        }
        return std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count()/100000;
    };
    const double off = bench(false,false), idle = bench(true,false), moving = bench(true,true);
    std::printf("Mouse camera hook including test ABI wrapper: off %.0f ns, idle %.0f ns, motion %.0f ns\n",
                off,idle,moving);
    BbMouse::SetActive(false);
    VirtualFree(image,0,MEM_RELEASE);
    std::puts("PASS: update-entry camera hooks, equal-axis/early-yaw response, idle target/correction reset, "
              "native stick handoff, full AVX/flags preservation, signature rollback, lock-on, "
              "frame-independent sensitivity, invalid input and concurrent toggles");
}

// SPDX-License-Identifier: GPL-2.0-or-later
// Bloodborne mouse camera by imedved (Nexus Mods), ported to bbport by mcrib884 (#3, bbmouse).

#include "bloodborne_cam.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

#include "common/logging/log.h"

#ifdef _WIN32
#include <windows.h>
#endif

// The camera hook (Windows): the game's camera update jumps here with the camera in r13, just
// before it loads the camera's pitch. The stub stamps the camera and applies the mouse motion
// gathered since the last update (bbcam_apply), then runs the two loads it replaced. Applying
// the motion here, once per update, gives every frame exactly the motion since the one before;
// writing the angles from the mouse thread at any moment split it unevenly between frames (a
// turn that judders by a pixel). The game's registers are kept: the red zone is skipped, and
// what the Windows ABI lets bbcam_apply change (rax, rcx, rdx, r8-r11, xmm0-5, flags) is saved.
#ifdef _WIN32
extern "C" {
uintptr_t bbcam_return;
void bbcam_apply(uintptr_t camera);
extern char bbcam_stub[];
}
__asm__(".text\n"
        ".globl bbcam_stub\nbbcam_stub:\n"
        "  test %r13, %r13\n  jz 1f\n"
        "  movl $0x3F333333, 0x130(%r13)\n" // 0.7f liveness stamp
        "  lea -128(%rsp), %rsp\n  pushfq\n"
        "  push %rax\n  push %rcx\n  push %rdx\n  push %r8\n  push %r9\n  push %r10\n"
        "  push %r11\n  push %rbx\n"
        "  mov %rsp, %rbx\n  and $-16, %rsp\n  sub $0x80, %rsp\n"
        "  movdqu %xmm0, 0x20(%rsp)\n  movdqu %xmm1, 0x30(%rsp)\n  movdqu %xmm2, 0x40(%rsp)\n"
        "  movdqu %xmm3, 0x50(%rsp)\n  movdqu %xmm4, 0x60(%rsp)\n  movdqu %xmm5, 0x70(%rsp)\n"
        "  mov %r13, %rcx\n  call bbcam_apply\n"
        "  movdqu 0x20(%rsp), %xmm0\n  movdqu 0x30(%rsp), %xmm1\n  movdqu 0x40(%rsp), %xmm2\n"
        "  movdqu 0x50(%rsp), %xmm3\n  movdqu 0x60(%rsp), %xmm4\n  movdqu 0x70(%rsp), %xmm5\n"
        "  mov %rbx, %rsp\n"
        "  pop %rbx\n  pop %r11\n  pop %r10\n  pop %r9\n  pop %r8\n  pop %rdx\n  pop %rcx\n"
        "  pop %rax\n  popfq\n  lea 128(%rsp), %rsp\n"
        "  vmovss 0x140(%r13), %xmm0\n  vmovss 0x150(%r13), %xmm1\n" // the replaced loads
        "1:\n  jmp *bbcam_return(%rip)\n");
#endif

namespace Core {

static constexpr uintptr_t ADDR_CAM_MAIN_ORIG = 0x80143CEAA;
static constexpr uintptr_t ADDR_CAM_NOP1_ORIG = 0x80143C6E8;
static constexpr uintptr_t ADDR_CAM_NOP2_ORIG = 0x80143C984;
static constexpr uintptr_t ADDR_CAM_NOP3_ORIG = 0x80143DDE6;
static constexpr uintptr_t ADDR_CAM_NOP4_ORIG = 0x80143C870;
static constexpr uintptr_t MONOCULAR_BASE_ORIG = 0x80553E8D0;

// The guest image is allocated as one of these direct-memory regions on the
// Windows runtime. Restrict the AOB search to these committed candidates so a
// coincidental instruction sequence in another module cannot be hooked.
static constexpr size_t CAMERA_REGION_SIZE_MIN = 0x5000000;
static constexpr size_t CAMERA_REGION_SIZE_MAX = 0x5200000;

#ifdef _WIN32
static bool CanAccess(uintptr_t addr, size_t len, bool write) {
    if (!addr || !len || addr + len < addr) {
        return false;
    }
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<const void*>(addr), &mbi, sizeof(mbi)) != sizeof(mbi)) {
        return false;
    }
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
        return false;
    }
    const uintptr_t region_start = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    if (addr < region_start || addr + len > region_start + mbi.RegionSize) {
        return false;
    }
    if (!write) {
        return true;
    }
    const DWORD protection = mbi.Protect & 0xff;
    return protection == PAGE_READWRITE || protection == PAGE_WRITECOPY ||
           protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
}

template <typename T>
static T ReadMem(uintptr_t addr) {
    if (!CanAccess(addr, sizeof(T), false)) {
        return T{};
    }
    return *reinterpret_cast<const volatile T*>(addr);
}

template <typename T>
static bool WriteMem(uintptr_t addr, T val) {
    if (!CanAccess(addr, sizeof(T), true)) {
        return false;
    }
    *reinterpret_cast<volatile T*>(addr) = val;
    return true;
}

// The camera pointer is validated once per motion sample below. Keep the hot
// path itself as cheap as shadPS4's guarded direct reads/writes; querying the
// Windows page table for every float makes high-polling-rate mice backlog.
template <typename T>
static T ReadMemFast(uintptr_t addr) {
    return *reinterpret_cast<const volatile T*>(addr);
}

template <typename T>
static void WriteMemFast(uintptr_t addr, T val) {
    *reinterpret_cast<volatile T*>(addr) = val;
}

static bool SetRW(uintptr_t addr, size_t len, DWORD* old_protect) {
    return VirtualProtect(reinterpret_cast<LPVOID>(addr), len, PAGE_EXECUTE_READWRITE, old_protect) != 0;
}

static void RestoreProtect(uintptr_t addr, size_t len, DWORD old_protect) {
    DWORD tmp;
    VirtualProtect(reinterpret_cast<LPVOID>(addr), len, old_protect, &tmp);
}

static void Flush(uintptr_t addr, size_t len) {
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(addr), len);
}

static void WriteJump(uintptr_t target, uintptr_t dest, size_t len) {
    u8 buf[64] = {0};
    buf[0] = 0xFF;
    buf[1] = 0x25;
    buf[2] = 0x00;
    buf[3] = 0x00;
    buf[4] = 0x00;
    buf[5] = 0x00;
    std::memcpy(buf + 6, &dest, 8);
    for (size_t i = 14; i < len; ++i) {
        buf[i] = 0x90;
    }
    std::memcpy(reinterpret_cast<void*>(target), buf, len);
}
#else
template <typename T>
static T ReadMem(uintptr_t addr) {
    if (!addr) {
        return T{};
    }
    return *reinterpret_cast<const volatile T*>(addr);
}

template <typename T>
static bool WriteMem(uintptr_t addr, T val) {
    if (!addr) {
        return false;
    }
    *reinterpret_cast<volatile T*>(addr) = val;
    return true;
}

template <typename T>
static T ReadMemFast(uintptr_t addr) {
    return *reinterpret_cast<const volatile T*>(addr);
}

template <typename T>
static void WriteMemFast(uintptr_t addr, T val) {
    *reinterpret_cast<volatile T*>(addr) = val;
}
#endif

BloodborneCam& BloodborneCam::Instance() {
    static BloodborneCam instance;
    return instance;
}

BloodborneCam::~BloodborneCam() {
    Shutdown();
}

void BloodborneCam::Shutdown() {
    // The trampoline is deliberately not freed: the guest may still be executing it.
    SetEnabled(false);
}

void BloodborneCam::Configure(bool direct, float sensitivity, float scale_x, float scale_y, bool sync) {
    if (!(sensitivity > 0.0f) || sensitivity > 20.0f) {
        sensitivity = 1.0f;
    }
    m_scale.store(sensitivity, std::memory_order_relaxed);
    m_scale_x.store(scale_x, std::memory_order_relaxed);
    m_scale_y.store(scale_y, std::memory_order_relaxed);
    m_sync.store(sync, std::memory_order_relaxed);
    m_configured.store(direct, std::memory_order_relaxed);
}

bool BloodborneCam::InitializeAddresses() {
    if (m_addressesResolved) {
        return true;
    }
    if (m_addressSearchFailed) {
        return false; // scanned once already; the result will not change
    }

#ifdef _WIN32
    static constexpr size_t CAM_PATTERN_LEN = 18;
    const u8 pattern[CAM_PATTERN_LEN] = {
        0xC4, 0xC1, 0x7A, 0x10, 0x85, 0x40, 0x01, 0x00, 0x00,
        0xC4, 0xC1, 0x7A, 0x10, 0x8D, 0x50, 0x01, 0x00, 0x00
    };

    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);

    u8* cur = reinterpret_cast<u8*>(sysInfo.lpMinimumApplicationAddress);
    u8* maxAddr = reinterpret_cast<u8*>(sysInfo.lpMaximumApplicationAddress);

    uintptr_t foundCamMain = 0;
    MEMORY_BASIC_INFORMATION mbi;

    while (cur < maxAddr) {
        if (VirtualQuery(cur, &mbi, sizeof(mbi)) == 0) {
            break;
        }

        // This mirrors the Cheat Engine table: the size filter applies to the
        // committed VirtualQuery region itself, not to an address interval.
        const bool isCameraCandidate = mbi.State == MEM_COMMIT &&
                                       mbi.RegionSize >= CAMERA_REGION_SIZE_MIN &&
                                       mbi.RegionSize <= CAMERA_REGION_SIZE_MAX;

        if (isCameraCandidate && mbi.RegionSize >= CAM_PATTERN_LEN) {
            // ReadProcessMemory gives us the same process-wide AOB scan
            // semantics as aobscanregion, without dereferencing a protected
            // page directly while walking the candidate.
            std::vector<u8> bytes(mbi.RegionSize);
            SIZE_T bytesRead = 0;
            if (ReadProcessMemory(GetCurrentProcess(), mbi.BaseAddress, bytes.data(),
                                  bytes.size(), &bytesRead) &&
                bytesRead == bytes.size()) {
                const u8* pStart = bytes.data();
                const u8* pEnd = pStart + bytesRead - CAM_PATTERN_LEN;
                for (const u8* p = pStart; p <= pEnd; ++p) {
                    if (std::memcmp(p, pattern, CAM_PATTERN_LEN) == 0) {
                        // First verified hit in the first committed candidate wins.
                        foundCamMain = reinterpret_cast<uintptr_t>(mbi.BaseAddress) +
                                       static_cast<size_t>(p - pStart);
                        break;
                    }
                }
            }
        }

        if (foundCamMain != 0) {
            break;
        }

        u8* next = reinterpret_cast<u8*>(mbi.BaseAddress) + mbi.RegionSize;
        if (next <= cur) {
            break;
        }
        cur = next;
    }

    if (foundCamMain == 0) {
        m_addressesResolved = false;
        m_addressSearchFailed = true;
        return false;
    }

    m_cameraOffset = static_cast<intptr_t>(foundCamMain) - static_cast<intptr_t>(ADDR_CAM_MAIN_ORIG);
    m_addrCamMain = foundCamMain;
    m_addrCamNop[0] = static_cast<uintptr_t>(static_cast<intptr_t>(ADDR_CAM_NOP1_ORIG) + m_cameraOffset);
    m_addrCamNop[1] = static_cast<uintptr_t>(static_cast<intptr_t>(ADDR_CAM_NOP2_ORIG) + m_cameraOffset);
    m_addrCamNop[2] = static_cast<uintptr_t>(static_cast<intptr_t>(ADDR_CAM_NOP3_ORIG) + m_cameraOffset);
    m_addrCamNop[3] = static_cast<uintptr_t>(static_cast<intptr_t>(ADDR_CAM_NOP4_ORIG) + m_cameraOffset);
    m_monocularBase = static_cast<uintptr_t>(static_cast<intptr_t>(MONOCULAR_BASE_ORIG) + m_cameraOffset);

    m_addressesResolved = true;
    return true;
#else
    return false;
#endif
}

bool BloodborneCam::SetEnabled(bool enabled) {
    if (enabled == m_enabled.load(std::memory_order_relaxed)) {
        return enabled;
    }

#ifdef _WIN32
    static const u8 NOP9[CAM_NOP_LEN] = {
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90
    };
    static const u8 kExpectedMain[CAM_MAIN_LEN] = {
        0xC4, 0xC1, 0x7A, 0x10, 0x85, 0x40, 0x01, 0x00, 0x00,
        0xC4, 0xC1, 0x7A, 0x10, 0x8D, 0x50, 0x01, 0x00, 0x00
    };

    DWORD old_protect = 0;

    if (!enabled) {
        // Stop applying motion first, then restore the original instructions.
        m_enabled.store(false, std::memory_order_release);
        m_lockedOn.store(false, std::memory_order_relaxed);
        if (m_addressesResolved) {
            if (SetRW(m_addrCamMain, CAM_MAIN_LEN, &old_protect)) {
                std::memcpy(reinterpret_cast<void*>(m_addrCamMain), m_camMainOrig, CAM_MAIN_LEN);
                RestoreProtect(m_addrCamMain, CAM_MAIN_LEN, old_protect);
                Flush(m_addrCamMain, CAM_MAIN_LEN);
            }
            for (int i = 0; i < 4; ++i) {
                if (SetRW(m_addrCamNop[i], CAM_NOP_LEN, &old_protect)) {
                    std::memcpy(reinterpret_cast<void*>(m_addrCamNop[i]), m_camNopOrig[i], CAM_NOP_LEN);
                    RestoreProtect(m_addrCamNop[i], CAM_NOP_LEN, old_protect);
                    Flush(m_addrCamNop[i], CAM_NOP_LEN);
                }
            }
        }
        LOG_INFO(Core, "Mouse camera: direct mode off");
        return false;
    }

    if (!InitializeAddresses()) {
        LOG_WARNING(Core, "Mouse camera: camera code not found (game 1.09 needed); using the stick mode");
        return false;
    }

    // Only hook code that still looks like the original camera update.
    if (std::memcmp(reinterpret_cast<const void*>(m_addrCamMain), kExpectedMain, CAM_MAIN_LEN) != 0) {
        LOG_WARNING(Core, "Mouse camera: camera code changed; using the stick mode");
        return false;
    }
    for (int i = 0; i < 4; ++i) {
        if (!CanAccess(m_addrCamNop[i], CAM_NOP_LEN, false)) {
            LOG_WARNING(Core, "Mouse camera: camera code not readable; using the stick mode");
            return false;
        }
    }

    std::memcpy(m_camMainOrig, reinterpret_cast<const void*>(m_addrCamMain), CAM_MAIN_LEN);
    for (int i = 0; i < 4; ++i) {
        std::memcpy(m_camNopOrig[i], reinterpret_cast<const void*>(m_addrCamNop[i]), CAM_NOP_LEN);
    }

    bbcam_return = static_cast<uintptr_t>(m_addrCamMain + CAM_MAIN_LEN);
    m_trampoline = bbcam_stub;
    m_pendingX.store(0.0f, std::memory_order_relaxed);
    m_pendingY.store(0.0f, std::memory_order_relaxed);

    if (!SetRW(m_addrCamMain, CAM_MAIN_LEN, &old_protect)) {
        LOG_ERROR(Core, "Mouse camera: camera code not writable");
        return false;
    }
    WriteJump(m_addrCamMain, reinterpret_cast<uintptr_t>(m_trampoline), CAM_MAIN_LEN);
    RestoreProtect(m_addrCamMain, CAM_MAIN_LEN, old_protect);
    Flush(m_addrCamMain, CAM_MAIN_LEN);

    for (int i = 0; i < 4; ++i) {
        if (SetRW(m_addrCamNop[i], CAM_NOP_LEN, &old_protect)) {
            std::memcpy(reinterpret_cast<void*>(m_addrCamNop[i]), NOP9, CAM_NOP_LEN);
            RestoreProtect(m_addrCamNop[i], CAM_NOP_LEN, old_protect);
            Flush(m_addrCamNop[i], CAM_NOP_LEN);
        }
    }

    m_lockedOn.store(false, std::memory_order_relaxed);
    m_enabled.store(true, std::memory_order_release);
    LOG_INFO(Core, "Mouse camera: direct mode on");
    return true;
#else
    (void)enabled;
    return false;
#endif
}

static int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

static void AddFloat(std::atomic<float>& value, float add) {
    float old = value.load(std::memory_order_relaxed);
    while (!value.compare_exchange_weak(old, old + add, std::memory_order_relaxed)) {
    }
}

void BloodborneCam::OnMouseMotion(float xrel, float yrel) {
    if (!m_enabled.load(std::memory_order_acquire) || !m_trampoline) {
        return;
    }
    // While the camera is not updated (a loading screen), motion is not saved up for later.
    if (NowMs() - m_lastApplyMs.load(std::memory_order_relaxed) > 100) {
        m_pendingX.store(xrel, std::memory_order_relaxed);
        m_pendingY.store(yrel, std::memory_order_relaxed);
        return;
    }
    AddFloat(m_pendingX, xrel);
    AddFloat(m_pendingY, yrel);
}

extern "C" void bbcam_apply(uintptr_t camera) {
    BloodborneCam::Instance().Apply(camera);
}

void BloodborneCam::Apply(uintptr_t pitch) {
    m_lastApplyMs.store(NowMs(), std::memory_order_relaxed);
    if (!m_enabled.load(std::memory_order_acquire)) {
        return;
    }
    const float xrel = m_pendingX.exchange(0.0f, std::memory_order_relaxed);
    const float yrel = m_pendingY.exchange(0.0f, std::memory_order_relaxed);

    // Locked on: the game drives the camera; the window thread routes motion to the stick
    // instead, so a flick switches targets.
    const bool locked = ReadMemFast<float>(pitch + 0x154) == 1.0f;
    m_lockedOn.store(locked, std::memory_order_relaxed);
    if (locked || (xrel == 0.0f && yrel == 0.0f)) {
        return;
    }

    Turn(pitch, xrel, yrel);
}

void BloodborneCam::Turn(uintptr_t pitch, float xrel, float yrel) {
    const float scale = m_scale.load(std::memory_order_relaxed);
    const float deltaPitch = (yrel / kCountsPerRadian) * scale * m_scale_y.load(std::memory_order_relaxed);
    const float deltaYaw = (xrel / kCountsPerRadian) * scale * m_scale_x.load(std::memory_order_relaxed);

    if (m_monocularBase) {
        uintptr_t p1 = ReadMem<uintptr_t>(m_monocularBase);
        if (p1) {
            uintptr_t p2 = ReadMem<uintptr_t>(p1 + 0x68);
            if (p2) {
                int monocular = ReadMem<int>(p2 + 0x84);
                if (monocular != 0) {
                    uintptr_t p3 = ReadMem<uintptr_t>(p2 + 0x68);
                    if (p3) {
                        float monoPitch = ReadMem<float>(p3 + 0x148) + deltaPitch * 0.5f;
                        float monoYaw = ReadMem<float>(p3 + 0x14C) + deltaYaw * 0.5f;

                        if (monoPitch > 1.71f) {
                            monoPitch = 1.71f;
                        } else if (monoPitch < -1.94f) {
                            monoPitch = -1.94f;
                        }

                        while (monoYaw > 3.14159265f) {
                            monoYaw -= 6.28318531f;
                        }
                        while (monoYaw < -3.14159265f) {
                            monoYaw += 6.28318531f;
                        }

                        WriteMem<float>(p3 + 0x148, monoPitch);
                        WriteMem<float>(p3 + 0x14C, monoYaw);
                        return;
                    }
                }
            }
        }
    }

    float curPitch = ReadMemFast<float>(pitch + 0x140);
    float curYaw = ReadMemFast<float>(pitch + 0x144);
    const float oldPitch = curPitch;

    curPitch += deltaPitch;

    float minPitch = ReadMemFast<float>(pitch + 0x1F0);
    float maxPitch = ReadMemFast<float>(pitch + 0x1EC);
    if (minPitch >= maxPitch || minPitch < -3.0f || maxPitch > 3.0f) {
        minPitch = -1.94f;
        maxPitch = 1.71f;
    }
    if (curPitch > maxPitch) {
        curPitch = maxPitch;
    } else if (curPitch < minPitch) {
        curPitch = minPitch;
    }

    curYaw += deltaYaw;
    while (curYaw > 3.14159265f) {
        curYaw -= 6.28318531f;
    }
    while (curYaw < -3.14159265f) {
        curYaw += 6.28318531f;
    }

    // Synchronize target pitch, smoothed render pitch, and convergence pitch.
    WriteMemFast<float>(pitch + 0x140, curPitch);
    WriteMemFast<float>(pitch + 0x150, curPitch);
    WriteMemFast<float>(pitch + 0x26C, curPitch);

    // Synchronize target yaw and convergence yaw.
    WriteMemFast<float>(pitch + 0x144, curYaw);
    WriteMemFast<float>(pitch + 0x270, curYaw);

    if (m_sync.load(std::memory_order_relaxed)) {
        TurnDrawnPosition(pitch, curPitch - oldPitch, deltaYaw);
    }
}

// The camera is drawn from a position (+0x100) that follows its ideal one around the pivot
// (+0xd0) with a spring, so a turn of the angles alone arrives over ~100 ms. Turning the drawn
// position by the same angles leaves the spring nothing to catch up with: the turn is drawn
// whole in the next frame. From Ryan Sousa's keyboard and mouse fork (src/runtime_camhook.c):
// the drawn position is pivot - direction * r, direction = (sin y cos p, -sin p, cos y cos p).
void BloodborneCam::TurnDrawnPosition(uintptr_t camera, float turnPitch, float turnYaw) {
#ifdef _WIN32
    if (!CanAccess(camera + 0xD0, 0x40, true)) {
        return;
    }
#endif
    const float px = ReadMemFast<float>(camera + 0xD0), py = ReadMemFast<float>(camera + 0xD4),
                pz = ReadMemFast<float>(camera + 0xD8);
    const double ox = ReadMemFast<float>(camera + 0x100) - px, oy = ReadMemFast<float>(camera + 0x104) - py,
                 oz = ReadMemFast<float>(camera + 0x108) - pz;
    const double r = std::sqrt(ox * ox + oy * oy + oz * oz);
    if (!(r > 1e-3 && r < 100.0)) {
        return; // not a camera around its pivot (a cutscene, a transition)
    }
    const double s = std::fmin(1.0, std::fmax(-1.0, oy / r));
    const double p = std::asin(s) + turnPitch, y = std::atan2(-ox, -oz) + turnYaw;
    WriteMemFast<float>(camera + 0x100, static_cast<float>(px - std::sin(y) * std::cos(p) * r));
    WriteMemFast<float>(camera + 0x104, static_cast<float>(py + std::sin(p) * r));
    WriteMemFast<float>(camera + 0x108, static_cast<float>(pz - std::cos(y) * std::cos(p) * r));
}

} // namespace Core

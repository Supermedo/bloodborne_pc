// SPDX-License-Identifier: GPL-2.0-or-later
// Ported from shadlixps4's Bloodborne mouse-camera implementation.

#include "bloodborne_cam.h"

#include <cmath>
#include <cstring>
#include <vector>

#include <SDL3/SDL.h>

#include "bbport_settings.h"
#include "common/logging/log.h"

#ifdef _WIN32
#include <windows.h>
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
    if (m_mode != BloodborneCamMode::Disabled) {
        SetMode(BloodborneCamMode::Disabled, nullptr);
    }
#ifdef _WIN32
    if (m_trampoline) {
        VirtualFree(m_trampoline, 0, MEM_RELEASE);
        m_trampoline = nullptr;
    }
#endif
}

void BloodborneCam::CenterMouse(SDL_Window* window) {
    if (window && (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS)) {
        int w = 0;
        int h = 0;
        SDL_GetWindowSize(window, &w, &h);
        if (w > 0 && h > 0) {
            SDL_WarpMouseInWindow(window, static_cast<float>(w) / 2.0f, static_cast<float>(h) / 2.0f);
        }
    }
}

bool BloodborneCam::InitializeAddresses() {
    if (m_addressesResolved) {
        return true;
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

void BloodborneCam::Toggle(SDL_Window* window) {
    if (m_mode == BloodborneCamMode::Enabled) {
        SetMode(BloodborneCamMode::Disabled, window);
    } else {
        SetMode(BloodborneCamMode::Enabled, window);
    }
}

void BloodborneCam::SetMode(BloodborneCamMode mode, SDL_Window* window) {
    if (mode == m_mode) {
        return;
    }

#ifdef _WIN32
    static const u8 NOP9[CAM_NOP_LEN] = {
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90
    };

    DWORD old_protect = 0;

    // Unhook previous active mode.
    if (m_mode == BloodborneCamMode::Enabled && m_addressesResolved) {
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

    if (mode == BloodborneCamMode::Enabled) {
        if (!m_addressesResolved) {
            if (!InitializeAddresses()) {
                LOG_WARNING(Core, "Bloodborne camera instructions not found in memory");
                return;
            }
        }

        std::memcpy(m_camMainOrig, reinterpret_cast<const void*>(m_addrCamMain), CAM_MAIN_LEN);
        for (int i = 0; i < 4; ++i) {
            std::memcpy(m_camNopOrig[i], reinterpret_cast<const void*>(m_addrCamNop[i]), CAM_NOP_LEN);
        }

        if (!m_trampoline) {
            m_trampoline = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (!m_trampoline) {
                LOG_ERROR(Core, "Failed to allocate memory for camera trampoline");
                return;
            }
        }

        u8* t = static_cast<u8*>(m_trampoline);
        std::memset(t, 0, 0x100);

        // test r13, r13
        t[0x00] = 0x49;
        t[0x01] = 0x85;
        t[0x02] = 0xED;

        // jz cam_skip (+0x2B to 0x30)
        t[0x03] = 0x74;
        t[0x04] = 0x2B;

        // mov [rip + 0x2C], r13 (stores to offset 0x38)
        t[0x05] = 0x4C;
        t[0x06] = 0x89;
        t[0x07] = 0x2D;
        t[0x08] = 0x2C;
        t[0x09] = 0x00;
        t[0x0A] = 0x00;
        t[0x0B] = 0x00;

        // mov dword ptr [r13 + 0x130], 0x3F333333 (0.7f liveness stamp)
        t[0x0C] = 0x41;
        t[0x0D] = 0xC7;
        t[0x0E] = 0x85;
        t[0x0F] = 0x30;
        t[0x10] = 0x01;
        t[0x11] = 0x00;
        t[0x12] = 0x00;
        t[0x13] = 0x33;
        t[0x14] = 0x33;
        t[0x15] = 0x33;
        t[0x16] = 0x3F;

        // vmovss xmm0, dword ptr [r13 + 0x140]
        const u8 vmovss_xmm0[9] = {0xC4, 0xC1, 0x7A, 0x10, 0x85, 0x40, 0x01, 0x00, 0x00};
        std::memcpy(&t[0x17], vmovss_xmm0, 9);

        // vmovss xmm1, dword ptr [r13 + 0x150]
        const u8 vmovss_xmm1[9] = {0xC4, 0xC1, 0x7A, 0x10, 0x8D, 0x50, 0x01, 0x00, 0x00};
        std::memcpy(&t[0x20], vmovss_xmm1, 9);

        // jmp qword ptr [rip + 0x11] (reads return address from offset 0x40)
        t[0x29] = 0xFF;
        t[0x2A] = 0x25;
        t[0x2B] = 0x11;
        t[0x2C] = 0x00;
        t[0x2D] = 0x00;
        t[0x2E] = 0x00;

        // 1 byte padding
        t[0x2F] = 0x90;

        // cam_skip: jmp qword ptr [rip + 0x0A] (reads return address from offset 0x40)
        t[0x30] = 0xFF;
        t[0x31] = 0x25;
        t[0x32] = 0x0A;
        t[0x33] = 0x00;
        t[0x34] = 0x00;
        t[0x35] = 0x00;

        // 2 bytes padding
        t[0x36] = 0x90;
        t[0x37] = 0x90;

        // Storage at 0x38: uint64_t cam_pitch_storage = 0
        *reinterpret_cast<uint64_t*>(&t[0x38]) = 0;

        // Return address at 0x40: uint64_t cam_return_addr = m_addrCamMain + 18
        *reinterpret_cast<uint64_t*>(&t[0x40]) = static_cast<uint64_t>(m_addrCamMain + CAM_MAIN_LEN);

        Flush(reinterpret_cast<uintptr_t>(t), 0x48);

        if (!SetRW(m_addrCamMain, CAM_MAIN_LEN, &old_protect)) {
            LOG_ERROR(Core, "Failed to make camera code writable");
            return;
        }
        WriteJump(m_addrCamMain, reinterpret_cast<uintptr_t>(t), CAM_MAIN_LEN);
        RestoreProtect(m_addrCamMain, CAM_MAIN_LEN, old_protect);
        Flush(m_addrCamMain, CAM_MAIN_LEN);

        for (int i = 0; i < 4; ++i) {
            if (SetRW(m_addrCamNop[i], CAM_NOP_LEN, &old_protect)) {
                std::memcpy(reinterpret_cast<void*>(m_addrCamNop[i]), NOP9, CAM_NOP_LEN);
                RestoreProtect(m_addrCamNop[i], CAM_NOP_LEN, old_protect);
                Flush(m_addrCamNop[i], CAM_NOP_LEN);
            }
        }

        m_lockonFlag = false;
        m_mode = BloodborneCamMode::Enabled;

        const bool focused = window && (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS);
        m_hasWindowFocus = focused;
        if (window && focused) {
            SDL_SetWindowRelativeMouseMode(window, true);
            CenterMouse(window);
        }

        LOG_INFO(Core, "Bloodborne camera enabled");
    } else {
        m_mode = BloodborneCamMode::Disabled;
        m_hasWindowFocus = false;

        if (window) {
            SDL_SetWindowRelativeMouseMode(window, false);
            CenterMouse(window);
        }

        LOG_INFO(Core, "Bloodborne camera disabled");
    }
#else
    (void)mode;
    (void)window;
#endif
}

void BloodborneCam::OnMouseMotion(float xrel, float yrel) {
    if (m_mode == BloodborneCamMode::Disabled || !m_trampoline || !m_hasWindowFocus) {
        return;
    }

    if (xrel == 0.0f && yrel == 0.0f) {
        return;
    }

    u8* t = static_cast<u8*>(m_trampoline);
    uintptr_t pitch = static_cast<uintptr_t>(*reinterpret_cast<volatile uint64_t*>(&t[0x38]));
    if (!pitch) {
        return;
    }

    // Validate the complete camera object once. Every field touched below is
    // inside this range, so the actual motion update does not pay for a
    // VirtualQuery per scalar access.
#ifdef _WIN32
    if (!CanAccess(pitch + 0x130, 0x144, true)) {
        return;
    }
#endif

    float stamp = ReadMemFast<float>(pitch + 0x130);
    if (!(stamp > 0.699f && stamp < 0.701f)) {
        return;
    }

    float lockon = ReadMemFast<float>(pitch + 0x154);
    if (lockon == 1.0f) {
        m_lockonFlag = true;
        return;
    } else if (m_lockonFlag) {
        m_lockonFlag = false;
        return;
    }

    float scale = static_cast<float>(BbSettings::Get().mousecam_sensitivity.load()) / 100.0f;
    if (scale <= 0.0f) {
        scale = 1.0f;
    }

    const float deltaPitch = (yrel / m_sensitivity) * scale;
    const float deltaYaw = (xrel / m_sensitivity) * scale;

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
}

void BloodborneCam::OnWindowFocusChanged(bool has_focus, SDL_Window* window) {
    m_hasWindowFocus = has_focus;
    if (m_mode != BloodborneCamMode::Disabled && window) {
        SDL_SetWindowRelativeMouseMode(window, has_focus);
        if (has_focus) {
            CenterMouse(window);
        }
    }
}

} // namespace Core

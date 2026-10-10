// SPDX-License-Identifier: GPL-2.0-or-later
// Bloodborne mouse camera by imedved (Nexus Mods), ported to bbport by mcrib884 (#3, bbmouse).
//
// Direct mouse camera: hooks the game's camera update so mouse motion writes the camera's
// yaw/pitch itself (1:1, no stick deadzone or acceleration), like a native PC game. Game 1.09.
// Window capture (relative mode, focus, menu) is owned by window.cpp; this class only patches
// the guest code and applies motion. While the player is locked on, motion is left to the
// mouse-to-stick path instead (IsLockedOn), so a flick still switches targets.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "common/types.h"

namespace Core {

class BloodborneCam {
public:
    static BloodborneCam& Instance();

    void Shutdown();

    /// Any thread (input.ini load/reload): whether direct mode is wanted and its sensitivity
    /// multiplier (1.0 = 900 mouse counts per radian). Applied by the window thread.
    void Configure(bool direct, float sensitivity);
    [[nodiscard]] bool IsConfigured() const noexcept {
        return m_configured.load(std::memory_order_relaxed);
    }

    /// Window thread: installs (true) or removes (false) the camera hook. Returns whether the
    /// hook is active afterwards (false if the game's camera code was not found).
    bool SetEnabled(bool enabled);
    [[nodiscard]] bool IsEnabled() const noexcept {
        return m_enabled.load(std::memory_order_acquire);
    }
    /// True while the game reports a lock-on target (motion is not applied then).
    [[nodiscard]] bool IsLockedOn() const noexcept {
        return m_lockedOn.load(std::memory_order_relaxed);
    }

    /// Mouse sampler thread: applies relative motion to the camera.
    void OnMouseMotion(float xrel, float yrel);

private:
    BloodborneCam() = default;
    ~BloodborneCam();

    BloodborneCam(const BloodborneCam&) = delete;
    BloodborneCam& operator=(const BloodborneCam&) = delete;

    bool InitializeAddresses();

    std::atomic<bool> m_enabled{false};
    std::atomic<bool> m_configured{false};
    std::atomic<float> m_scale{1.0f};
    std::atomic<bool> m_lockedOn{false};
    bool m_addressesResolved = false;
    bool m_addressSearchFailed = false;

    uintptr_t m_addrCamMain = 0;
    uintptr_t m_addrCamNop[4] = {0, 0, 0, 0};
    uintptr_t m_monocularBase = 0;
    intptr_t m_cameraOffset = 0;

    static constexpr size_t CAM_MAIN_LEN = 18;
    static constexpr size_t CAM_NOP_LEN = 9;

    u8 m_camMainOrig[CAM_MAIN_LEN] = {};
    u8 m_camNopOrig[4][CAM_NOP_LEN] = {};

    static constexpr float kCountsPerRadian = 900.0f;

    void* m_trampoline = nullptr;
};

} // namespace Core

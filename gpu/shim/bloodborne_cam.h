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

    /// Any thread (input.ini load/reload): whether direct mode is wanted, its sensitivity
    /// multiplier (1.0 = 900 mouse counts per radian) and the per-axis multipliers of the motion
    /// (-1 inverts; the vertical one carries mouse_sensitivity_y). Applied by the window thread.
    /// `sync` also turns the drawn position with the angles (TurnDrawnPosition).
    void Configure(bool direct, float sensitivity, float scale_x = 1.0f, float scale_y = 1.0f,
                   bool sync = true);
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

    /// Mouse sampler thread: gathers relative motion for the next camera update.
    void OnMouseMotion(float xrel, float yrel);
    /// The game's thread, from the camera hook: turns `camera` by the gathered motion.
    void Apply(uintptr_t camera);
    /// Turns `camera` by mouse counts (the game's thread).
    void Turn(uintptr_t camera, float xrel, float yrel);

private:
    BloodborneCam() = default;
    ~BloodborneCam();

    BloodborneCam(const BloodborneCam&) = delete;
    BloodborneCam& operator=(const BloodborneCam&) = delete;

    bool InitializeAddresses();
    void TurnDrawnPosition(uintptr_t camera, float turnPitch, float turnYaw);

    std::atomic<bool> m_enabled{false};
    std::atomic<bool> m_configured{false};
    std::atomic<float> m_scale{1.0f};
    std::atomic<float> m_scale_x{1.0f}, m_scale_y{1.0f};
    std::atomic<bool> m_sync{true};
    std::atomic<bool> m_lockedOn{false};
    std::atomic<float> m_pendingX{0.0f}, m_pendingY{0.0f}; ///< mouse counts since the last update
    std::atomic<int64_t> m_lastApplyMs{0};
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

// SPDX-License-Identifier: GPL-2.0-or-later
// Ported from shadlixps4's Bloodborne mouse-camera implementation.

#pragma once

#include <cstddef>
#include <cstdint>

#include "common/types.h"

struct SDL_Window;

namespace Core {

enum class BloodborneCamMode : u8 {
    Disabled = 0,
    Enabled = 1,
    DirectWrite = 1,
};

class BloodborneCam {
public:
    static BloodborneCam& Instance();

    void Shutdown();

    void Toggle(SDL_Window* window);
    void CycleMode(SDL_Window* window) {
        Toggle(window);
    }
    void SetMode(BloodborneCamMode mode, SDL_Window* window);

    [[nodiscard]] BloodborneCamMode GetMode() const noexcept {
        return m_mode;
    }
    [[nodiscard]] bool IsEnabled() const noexcept {
        return m_mode != BloodborneCamMode::Disabled;
    }

    void OnMouseMotion(float xrel, float yrel);
    void OnWindowFocusChanged(bool has_focus, SDL_Window* window);

private:
    BloodborneCam() = default;
    ~BloodborneCam();

    BloodborneCam(const BloodborneCam&) = delete;
    BloodborneCam& operator=(const BloodborneCam&) = delete;

    bool InitializeAddresses();
    void CenterMouse(SDL_Window* window);

    BloodborneCamMode m_mode = BloodborneCamMode::Disabled;
    bool m_addressesResolved = false;
    bool m_lockonFlag = false;
    bool m_hasWindowFocus = false;

    uintptr_t m_addrCamMain = 0;
    uintptr_t m_addrCamNop[4] = {0, 0, 0, 0};
    uintptr_t m_monocularBase = 0;
    intptr_t m_cameraOffset = 0;

    static constexpr size_t CAM_MAIN_LEN = 18;
    static constexpr size_t CAM_NOP_LEN = 9;

    u8 m_camMainOrig[CAM_MAIN_LEN] = {};
    u8 m_camNopOrig[4][CAM_NOP_LEN] = {};

    float m_sensitivity = 900.0f;

    void* m_trampoline = nullptr;
};

} // namespace Core

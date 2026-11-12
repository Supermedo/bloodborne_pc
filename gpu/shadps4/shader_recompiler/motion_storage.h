// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

namespace Shader::MotionStorage {
// Navi21/22/23/24 device IDs, excluding RDNA1, APUs and compute accelerators.
// Reference: Linux amdgpu_drv.c, commit 3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0.
constexpr bool UsesSsbo(std::uint32_t vendor, std::uint32_t device) {
    if (vendor != 0x1002)
        return false;
    switch (device) {
    case 0x73a0:
    case 0x73a1:
    case 0x73a2:
    case 0x73a3:
    case 0x73a5:
    case 0x73a8:
    case 0x73a9:
    case 0x73ab:
    case 0x73ac:
    case 0x73ad:
    case 0x73ae:
    case 0x73af:
    case 0x73bf:
    case 0x73c0:
    case 0x73c1:
    case 0x73c3:
    case 0x73da:
    case 0x73db:
    case 0x73dc:
    case 0x73dd:
    case 0x73de:
    case 0x73df:
    case 0x73e0:
    case 0x73e1:
    case 0x73e2:
    case 0x73e3:
    case 0x73e8:
    case 0x73e9:
    case 0x73ea:
    case 0x73eb:
    case 0x73ec:
    case 0x73ed:
    case 0x73ef:
    case 0x73ff:
    case 0x7420:
    case 0x7421:
    case 0x7422:
    case 0x7423:
    case 0x7424:
    case 0x743f:
        return true;
    default:
        return false;
    }
}
} // namespace Shader::MotionStorage

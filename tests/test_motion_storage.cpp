// SPDX-License-Identifier: GPL-2.0-or-later
#include "shader_recompiler/motion_storage.h"
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
static void Require(bool condition) {
    if (!condition)
        std::abort();
}
int main() {
    using Shader::MotionStorage::UsesSsbo;
    for (auto device : {0x73a0u, 0x73bfu, 0x73dfu, 0x73efu, 0x73ffu, 0x7420u, 0x743fu}) {
        Require(UsesSsbo(0x1002, device));
        Require(!UsesSsbo(0x10de, device));
        Require(!UsesSsbo(0x8086, device));
    }
    for (auto device : {0u, 0x731fu, 0x7340u, 0x7360u, 0x7408u, 0x744cu, 0x7550u, 0x1681u, 0x9999u})
        Require(!UsesSsbo(0x1002, device));
    std::puts("PASS RX6000 storage policy; RDNA1, RDNA3/4, APUs, accelerators and other vendors "
              "unchanged");
}

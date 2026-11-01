// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdio>
#include <stdexcept>
#include "shader_recompiler/motion_varyings.h"

inline void TestMotionVaryings() {
    using namespace Shader;
    const auto check = [](const Info& vs, const HwFragmentRuntimeInfo& fs,
                          const Profile& profile, bool expected) {
        if (MotionVaryings::SsboVaryingsAvailable(vs, fs, profile) != expected)
            throw std::runtime_error("SSBO varying collision classification failed");
    };
    Info vs{};
    HwFragmentRuntimeInfo fs{};
    Profile profile{};
    check(vs, fs, profile, true);
    for (u32 location = 0; location < IR::NumParams; ++location) {
        vs.stores = {};
        vs.stores.Set(IR::Attribute::Param0 + location);
        check(vs, fs, profile, location < MotionVectors::SafeCurrentLocation);
    }
    vs.stores = {};
    // A low fragment attribute index can refer to a high Vulkan location.
    fs.num_inputs = 1;
    for (u32 location = 0; location < 32; ++location) {
        fs.inputs[0].param_index = location;
        check(vs, fs, profile, location < MotionVectors::SafeCurrentLocation);
    }
    // High attribute indices can refer to low locations. The emitter also declares
    // unused non-default inputs, so no Info::loads exemption applies.
    fs.num_inputs = 32;
    fs.inputs = {};
    check(vs, fs, profile, true);
    fs.inputs[31].param_index = 24;
    check(vs, fs, profile, false);
    fs.inputs[31].is_default = true;
    check(vs, fs, profile, true);
    fs.inputs[31].is_flat = true; // passthrough is not a constant default
    check(vs, fs, profile, false);
    fs = {};
    profile.needs_clip_distance_emulation = true;
    vs.stores.Set(IR::Attribute::Param23);
    check(vs, fs, profile, true);
    vs.stores.Set(IR::Attribute::ClipDistance);
    check(vs, fs, profile, false);
    vs = {};
    fs.num_inputs = 2;
    fs.clip_distance_emulation = true;
    fs.inputs[0].param_index = 23;
    check(vs, fs, profile, false);
    fs.inputs[0].param_index = 22;
    fs.inputs[1].param_index = 31; // last input is the emulated clip vector
    check(vs, fs, profile, true);
    fs.num_inputs = 0;
    check(vs, fs, profile, false);
    fs.num_inputs = 33;
    check(vs, fs, profile, false);
    std::puts("PASS SSBO varying locations: remapping/default/clip/boundary cases");
}

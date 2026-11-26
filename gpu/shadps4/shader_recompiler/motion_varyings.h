// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "shader_recompiler/info.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader::MotionVaryings {

// Match DefineInputs/DefineOutputs in the SPIR-V backend. Fragment attribute
// indices are not Vulkan locations: PS_INPUT_CNTL can remap them, and emulated
// clip distances occupy location zero. Keep locations 24..31 free for motion
// and the builtin component budget on the bounded SSBO path.
inline bool SsboVaryingsAvailable(const Info& vs, const HwFragmentRuntimeInfo& runtime,
                                  const Profile& profile) {
    const u32 vertex_shift = profile.needs_clip_distance_emulation &&
                             vs.stores.GetAny(IR::Attribute::ClipDistance);
    for (u32 i = 0; i < IR::NumParams; ++i) {
        if (vs.stores.GetAny(IR::Attribute::Param0 + i) &&
            i + vertex_shift >= MotionVectors::SafeCurrentLocation) {
            return false;
        }
    }
    const u32 fragment_shift = runtime.clip_distance_emulation;
    if (runtime.num_inputs > runtime.inputs.size() || runtime.num_inputs < fragment_shift) {
        return false;
    }
    // The emitter declares every non-default input, including unused inputs.
    // Its final input is the emulated clip-distance vector when present.
    const u32 num_inputs = runtime.num_inputs - fragment_shift;
    for (u32 i = 0; i < num_inputs; ++i) {
        if (!runtime.inputs[i].IsDefault() &&
            runtime.inputs[i].param_index + fragment_shift >= MotionVectors::SafeCurrentLocation) {
            return false;
        }
    }
    return true;
}

} // namespace Shader::MotionVaryings

// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the float render-target guard of the shader translator (Profile::rt_nan_guard,
// export.cpp, BB_NAN_GUARD). A pixel shader that copies an interpolated value to MRT0 is
// translated for several target formats, guard on and off, and the SPIR-V is checked: float
// targets get one NaN test and one clamp per exported component with the format's own limit,
// normalized targets and the guard switched off get none. No GPU.
#include <cstdio>
#include <cstring>
#include <set>
#include <vector>
#include <spirv/unified1/GLSL.std.450.h>
#include <spirv/unified1/spirv.hpp>
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/runtime_info.h"

namespace {

int failures = 0;

void Expect(bool ok, const char* what, const char* format, bool guard) {
    if (!ok) {
        std::fprintf(stderr, "rt-guard-test: %s (%s, guard %s)\n", what, format, guard ? "on" : "off");
        ++failures;
    }
}

u32 Bits(float value) {
    u32 bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

// What the checks look at in a SPIR-V module.
struct Summary {
    u32 is_nan = 0;
    u32 fclamp = 0;
    std::set<u32> constants; // the words of every one-word OpConstant
};

Summary Summarize(const std::vector<u32>& spv) {
    Summary s;
    u32 glsl = ~0u;
    for (size_t i = 5; i < spv.size();) {
        const u32 count = spv[i] >> 16, op = spv[i] & 0xFFFFu;
        if (count == 0 || i + count > spv.size()) {
            break;
        }
        if (op == u32(spv::Op::OpExtInstImport) &&
            std::strcmp(reinterpret_cast<const char*>(&spv[i + 2]), "GLSL.std.450") == 0) {
            glsl = spv[i + 1];
        } else if (op == u32(spv::Op::OpExtInst) && spv[i + 3] == glsl &&
                   spv[i + 4] == GLSLstd450FClamp) {
            ++s.fclamp;
        } else if (op == u32(spv::Op::OpIsNan)) {
            ++s.is_nan;
        } else if (op == u32(spv::Op::OpConstant) && count == 4) {
            s.constants.insert(spv[i + 3]);
        }
        i += count;
    }
    return s;
}

// s_mov_b32 vcc_hi, 0; v_mov_b32 v4, v0 (FragCoord.x); exp mrt0 v4, v4, v4, v4 done vm;
// s_endpgm. Sea Islands encodings.
std::vector<u32> CopyToMrt0() {
    return {
        0xBEEB03FFu, 0u,
        (0x3Fu << 25) | (4u << 17) | (1u << 9) | (256u + 0u),
        (0x3Eu << 26) | (1u << 12) | (1u << 11) | 0xFu,
        0x04040404u,
        0xBF810000u,
    };
}

Summary Translate(AmdGpu::DataFormat data, AmdGpu::NumberFormat number, bool guard) {
    using namespace Shader;
    Info info{};
    info.hw_stage = HwStage::Fragment;
    info.sw_stage = SwStage::Fragment;
    RuntimeInfo runtime{};
    runtime.Initialize(info.hw_stage, info.sw_stage);
    auto& fs = runtime.hw.fs;
    fs.addr_flags.pos_x_float_ena = 1;
    fs.en_flags = fs.addr_flags;
    auto& target = fs.color_buffers[0];
    target.data_format = data;
    target.num_format = number;
    target.export_format = AmdGpu::ShaderExportFormat::ABGR_32;
    target.swizzle = AmdGpu::IdentityMapping;
    Profile profile{};
    profile.supported_spirv = 0x00010600;
    profile.support_int64 = true;
    profile.support_float64 = true;
    profile.gcn_float_rules = true;
    profile.rt_nan_guard = guard;
    Pools pools;
    auto program = TranslateProgram(CopyToMrt0(), pools, info, runtime, profile);
    Backend::Bindings bindings{};
    return Summarize(Backend::SPIRV::EmitSPIRV(profile, runtime, program, bindings));
}

// A float target: four more NaN tests and clamps with the guard, its limit as a constant.
void FloatTarget(const char* name, AmdGpu::DataFormat data, float limit) {
    const Summary on = Translate(data, AmdGpu::NumberFormat::Float, true);
    const Summary off = Translate(data, AmdGpu::NumberFormat::Float, false);
    Expect(on.is_nan == off.is_nan + 4, "one NaN test per component", name, true);
    Expect(on.fclamp == off.fclamp + 4, "one clamp per component", name, true);
    Expect(on.constants.contains(Bits(limit)) && on.constants.contains(Bits(-limit)),
           "the format's limits", name, true);
    Expect(!off.constants.contains(Bits(limit)), "no limit without the guard", name, false);
}

} // namespace

int main() {
    FloatTarget("RGBA16F", AmdGpu::DataFormat::Format16_16_16_16, 65504.0f);
    FloatTarget("R16F", AmdGpu::DataFormat::Format16, 65504.0f);
    FloatTarget("R11G11B10F", AmdGpu::DataFormat::Format11_11_10, 64512.0f);
    FloatTarget("RGBA32F", AmdGpu::DataFormat::Format32_32_32_32, 3.40282347e38f);
    // Normalized targets: the guard changes nothing.
    const Summary on = Translate(AmdGpu::DataFormat::Format8_8_8_8, AmdGpu::NumberFormat::Unorm, true);
    const Summary off = Translate(AmdGpu::DataFormat::Format8_8_8_8, AmdGpu::NumberFormat::Unorm, false);
    Expect(on.is_nan == off.is_nan && on.fclamp == off.fclamp && on.constants == off.constants,
           "a normalized target is left alone", "RGBA8", true);
    if (failures) {
        std::fprintf(stderr, "rt-guard-test: %d check(s) failed\n", failures);
        return 1;
    }
    std::puts("rt-guard-test: ok");
    return 0;
}

#ifdef _WIN32
#include <cstdlib>
// The Windows build links the runtime (gpu/CMakeLists.txt), which leaves this to the program.
extern "C" void runtime_restart() { std::abort(); }
#endif

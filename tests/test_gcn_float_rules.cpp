// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the GCN float rules of the shader translator (Profile::gcn_float_rules, BB_GCN_FLOAT).
// Small pixel shaders, each one instruction on FragCoord.x (v0) and FragCoord.y (v1) whose
// result goes to MRT0, are translated with the rules on and off, and the SPIR-V is checked for
// the instructions the rules stand for: NMin/NMax instead of FMin/FMax, a NaN test in med3,
// NClamp for the clamp modifier, unordered compares for V_CMP_N*, the legacy multiply in
// V_MAD_LEGACY/V_MAC_LEGACY and the clamp of V_RCP_CLAMP. No GPU.
#include <cstdio>
#include <cstring>
#include <map>
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

void Expect(bool ok, const char* what, const char* shader) {
    if (!ok) {
        std::fprintf(stderr, "gcn-float-test: %s: %s\n", shader, what);
        ++failures;
    }
}

// How often each instruction and each GLSL.std.450 instruction occurs, and the one-word constants.
struct Counts {
    std::map<u32, u32> ops;
    std::map<u32, u32> glsl;
    std::set<u32> constants;

    u32 Op(spv::Op op) const {
        const auto it = ops.find(u32(op));
        return it == ops.end() ? 0 : it->second;
    }
    u32 Glsl(u32 inst) const {
        const auto it = glsl.find(inst);
        return it == glsl.end() ? 0 : it->second;
    }
};

Counts Count(const std::vector<u32>& spv) {
    Counts counts;
    u32 glsl = ~0u;
    for (size_t i = 5; i < spv.size();) {
        const u32 words = spv[i] >> 16, op = spv[i] & 0xFFFFu;
        if (words == 0 || i + words > spv.size()) {
            break;
        }
        ++counts.ops[op];
        if (op == u32(spv::Op::OpExtInstImport) &&
            std::strcmp(reinterpret_cast<const char*>(&spv[i + 2]), "GLSL.std.450") == 0) {
            glsl = spv[i + 1];
        } else if (op == u32(spv::Op::OpExtInst) && spv[i + 3] == glsl) {
            ++counts.glsl[spv[i + 4]];
        } else if (op == u32(spv::Op::OpConstant) && words == 4) {
            counts.constants.insert(spv[i + 3]);
        }
        i += words;
    }
    return counts;
}

// Sea Islands encodings. Sources: v0 and v1 (256 + n); the result in v4.
constexpr u32 V0 = 256, V1 = 257;

u32 Vop1(u32 op, u32 src0) {
    return (0x3Fu << 25) | (4u << 17) | (op << 9) | src0;
}
u32 Vop2(u32 op, u32 src0, u32 vsrc1) {
    return (op << 25) | (4u << 17) | (vsrc1 << 9) | src0;
}
u32 Vopc(u32 op, u32 src0, u32 vsrc1) {
    return (0x3Eu << 25) | (op << 17) | (vsrc1 << 9) | src0;
}
std::vector<u32> Vop3(u32 op, u32 src0, u32 src1, u32 src2, bool clamp = false) {
    return {(0x34u << 26) | (op << 17) | (u32(clamp) << 11) | 4u,
            src0 | (src1 << 9) | (src2 << 18)};
}

// s_mov_b32 vcc_hi, 0; <body>; exp mrt0 v4, v4, v4, v4 done vm; s_endpgm.
std::vector<u32> Shader(const std::vector<u32>& body) {
    std::vector<u32> code{0xBEEB03FFu, 0u};
    code.insert(code.end(), body.begin(), body.end());
    code.insert(code.end(), {(0x3Eu << 26) | (1u << 12) | (1u << 11) | 0xFu, 0x04040404u,
                             0xBF810000u});
    return code;
}

Counts Translate(const std::vector<u32>& code, bool rules) {
    using namespace Shader;
    Info info{};
    info.hw_stage = HwStage::Fragment;
    info.sw_stage = SwStage::Fragment;
    RuntimeInfo runtime{};
    runtime.Initialize(info.hw_stage, info.sw_stage);
    auto& fs = runtime.hw.fs;
    fs.addr_flags.pos_x_float_ena = 1;
    fs.addr_flags.pos_y_float_ena = 1;
    fs.en_flags = fs.addr_flags;
    auto& target = fs.color_buffers[0];
    target.data_format = AmdGpu::DataFormat::Format32_32_32_32;
    target.num_format = AmdGpu::NumberFormat::Float;
    target.export_format = AmdGpu::ShaderExportFormat::ABGR_32;
    target.swizzle = AmdGpu::IdentityMapping;
    Profile profile{};
    profile.supported_spirv = 0x00010600;
    profile.support_int64 = true;
    profile.support_float64 = true;
    profile.gcn_float_rules = rules;
    Pools pools;
    auto program = TranslateProgram(code, pools, info, runtime, profile);
    Backend::Bindings bindings{};
    return Count(Backend::SPIRV::EmitSPIRV(profile, runtime, program, bindings));
}

struct Pair {
    Counts on, off;
};

Pair Both(const std::vector<u32>& body) {
    const auto code = Shader(body);
    return {Translate(code, true), Translate(code, false)};
}

} // namespace

int main() {
    using spv::Op;
    {
        const auto [on, off] = Both({Vop2(15, V0, 1)}); // v_min_f32 v4, v0, v1
        Expect(on.Glsl(GLSLstd450NMin) > 0 && on.Glsl(GLSLstd450FMin) == 0, "NMin", "v_min_f32");
        Expect(off.Glsl(GLSLstd450FMin) > 0 && off.Glsl(GLSLstd450NMin) == 0, "FMin off",
               "v_min_f32");
    }
    {
        const auto [on, off] = Both({Vop2(16, V0, 1)}); // v_max_f32 v4, v0, v1
        Expect(on.Glsl(GLSLstd450NMax) > 0 && on.Glsl(GLSLstd450FMax) == 0, "NMax", "v_max_f32");
        Expect(off.Glsl(GLSLstd450FMax) > 0 && off.Glsl(GLSLstd450NMax) == 0, "FMax off",
               "v_max_f32");
    }
    {
        const auto [on, off] = Both(Vop3(343, V0, V1, V0)); // v_med3_f32 v4, v0, v1, v0
        Expect(on.Op(Op::OpAny) == 1 && on.Op(Op::OpIsNan) == off.Op(Op::OpIsNan) + 1,
               "one NaN test of the three sources", "v_med3_f32");
        Expect(on.Glsl(GLSLstd450NMin) >= 2, "MIN3 for NaN", "v_med3_f32");
        Expect(off.Op(Op::OpAny) == 0 && off.Glsl(GLSLstd450NMin) == 0, "no NaN test off",
               "v_med3_f32");
    }
    {
        const auto [on, off] = Both(Vop3(256 + 3, V0, V1, 0, true)); // v_add_f32 v4, v0, v1 clamp
        Expect(on.Glsl(GLSLstd450NClamp) == 1 && on.Glsl(GLSLstd450FClamp) == 0, "NClamp",
               "v_add_f32 clamp");
        Expect(off.Glsl(GLSLstd450FClamp) == 1 && off.Glsl(GLSLstd450NClamp) == 0, "FClamp off",
               "v_add_f32 clamp");
    }
    {
        // v_cmp_nge_f32 vcc, v0, v1; v_cndmask_b32 v4, v0, v1, vcc
        const auto [on, off] = Both({Vopc(9, V0, 1), Vop2(0, V0, 1)});
        Expect(on.Op(Op::OpFUnordLessThan) == 1 && on.Op(Op::OpFOrdLessThan) == 0,
               "unordered a < b", "v_cmp_nge_f32");
        Expect(off.Op(Op::OpFOrdLessThan) == 1 && off.Op(Op::OpFUnordLessThan) == 0,
               "ordered off", "v_cmp_nge_f32");
    }
    {
        // v_cmp_nlg_f32 vcc, v0, v1 (had no entry in the table); v_cndmask_b32 v4, v0, v1, vcc
        const auto [on, off] = Both({Vopc(10, V0, 1), Vop2(0, V0, 1)});
        Expect(on.Op(Op::OpFUnordEqual) == 1, "unordered a == b", "v_cmp_nlg_f32");
        Expect(off.Op(Op::OpFUnordEqual) == 0 && off.Op(Op::OpFOrdEqual) >= 1, "ordered off",
               "v_cmp_nlg_f32");
    }
    {
        // v_cmp_lt_f32 vcc, v0, v1: an ordered compare stays ordered with the rules.
        const auto [on, off] = Both({Vopc(1, V0, 1), Vop2(0, V0, 1)});
        Expect(on.Op(Op::OpFOrdLessThan) == 1 && on.Op(Op::OpFUnordLessThan) == 0,
               "still ordered", "v_cmp_lt_f32");
    }
    {
        const auto [on, off] = Both(Vop3(320, V0, V1, V0)); // v_mad_legacy_f32 v4, v0, v1, v0
        Expect(on.Op(Op::OpFOrdEqual) == off.Op(Op::OpFOrdEqual) + 2 &&
                   on.Op(Op::OpSelect) == off.Op(Op::OpSelect) + 1,
               "zero tests of both factors", "v_mad_legacy_f32");
    }
    {
        // v_mov_b32 v4, v0; v_mac_legacy_f32 v4, v0, v1 (accumulates into v4)
        const auto [on, off] = Both({Vop1(1, V0), Vop2(6, V0, 1)});
        Expect(on.Op(Op::OpFOrdEqual) == off.Op(Op::OpFOrdEqual) + 2, "zero tests of both factors",
               "v_mac_legacy_f32");
    }
    {
        constexpr u32 flt_max = 0x7F7FFFFFu;
        const auto [on, off] = Both({Vop1(40, V0)}); // v_rcp_clamp_f32 v4, v0
        Expect(on.Op(Op::OpIsInf) == off.Op(Op::OpIsInf) + 1 && on.constants.contains(flt_max),
               "infinity to FLT_MAX", "v_rcp_clamp_f32");
        Expect(!off.constants.contains(flt_max), "plain rcp off", "v_rcp_clamp_f32");
    }
    {
        const auto [on, off] = Both({Vop1(44, V0)}); // v_rsq_clamp_f32 v4, v0
        Expect(on.Op(Op::OpIsInf) == off.Op(Op::OpIsInf) + 1, "infinity to FLT_MAX",
               "v_rsq_clamp_f32");
    }
    if (failures) {
        std::fprintf(stderr, "gcn-float-test: %d check(s) failed\n", failures);
        return 1;
    }
    std::puts("gcn-float-test: ok");
    return 0;
}

#ifdef _WIN32
#include <cstdlib>
// The Windows build links the runtime (gpu/CMakeLists.txt), which leaves this to the program.
extern "C" void runtime_restart() { std::abort(); }
#endif

// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise the actual SPIR-V backend without opening a window or loading the game.
// Output is validated by spirv-val (see docs/upscaler.md).
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <setjmp.h>
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "test_motion_varyings.h"

int MotionGpuTest(const std::filesystem::path& dir);

void WriteMotionShaders(const std::filesystem::path& dir, uint64_t params_address,
                        uint64_t positions_address) {
    using namespace Shader;
    std::filesystem::create_directories(dir);
    MotionVectors::params_address = params_address;
    MotionVectors::positions_address = positions_address;
    for (bool vertex : {true, false}) {
        for (bool motion : {false, true}) {
            Info info{};
            info.hw_stage = vertex ? HwStage::Vertex : HwStage::Fragment;
            info.sw_stage = vertex ? SwStage::Vertex : SwStage::Fragment;
            RuntimeInfo runtime{};
            runtime.Initialize(info.hw_stage, info.sw_stage);
            if (vertex) {
                runtime.hw.vs.motion_vectors = motion;
            } else {
                runtime.hw.fs.motion_vectors = motion;
                runtime.hw.fs.color_buffers[0].num_format = AmdGpu::NumberFormat::Float;
            }
            Common::ObjectPool<IR::Inst> pool;
            IR::Block block(pool);
            IR::IREmitter ir(block);
            const auto output = vertex ? IR::Attribute::Position0 : IR::Attribute::RenderTarget0;
            for (u32 component = 0; component < 4; ++component) {
                info.stores.Set(output, component);
                if (vertex && component < 2) {
                    info.loads.Set(IR::Attribute::VertexId, 0);
                    const auto id = ir.GetAttributeU32(IR::Attribute::VertexId);
                    const IR::U32 bit = component == 0
                                            ? IR::U32{ir.BitwiseAnd(id, ir.Imm32(1u))}
                                            : IR::U32{ir.ShiftRightLogical(id, ir.Imm32(1u))};
                    const IR::F32 coordinate{ir.ConvertUToF(32, 32, bit)};
                    ir.SetAttribute(output,
                                    ir.FPSub(ir.FPMul(coordinate, ir.Imm32(4.0f)), ir.Imm32(1.0f)),
                                    component);
                } else if (!vertex && component < 2) {
                    info.loads.Set(IR::Attribute::FragCoord, component);
                    ir.SetAttribute(output, ir.GetAttribute(IR::Attribute::FragCoord, component), component);
                } else {
                    ir.SetAttribute(output, ir.Imm32(component == 3 ? 1.0f : 0.0f), component);
                }
            }
            ir.Epilogue();
            IR::Program program(info);
            program.blocks.push_back(&block);
            program.syntax_list.push_back({.data = {.block = &block},
                                           .type = IR::AbstractSyntaxNode::Type::Block});
            program.syntax_list.push_back({.type = IR::AbstractSyntaxNode::Type::Return});
            Profile profile{};
            profile.supported_spirv = 0x00010600;
            profile.support_int64 = true;
            Backend::Bindings bindings{};
            const auto code = Backend::SPIRV::EmitSPIRV(profile, runtime, program, bindings);
            const auto path = dir / (std::string(vertex ? "vertex" : "fragment") +
                                      (motion ? "-motion.spv" : "-plain.spv"));
            std::ofstream out(path, std::ios::binary);
            out.write(reinterpret_cast<const char*>(code.data()), code.size() * sizeof(u32));
            if (!out) {
                throw std::runtime_error("shader output failed");
            }
        }
    }
}

int main(int argc, char** argv) {
    TestMotionVaryings();
    const std::filesystem::path dir = argc > 1 ? argv[1] : ".";
    if (argc > 2 && std::string(argv[2]) == "--gpu")
        return MotionGpuTest(dir);
    Shader::MotionVectors::storage_buffers = std::getenv("BB_OMV_TEST_BDA") == nullptr;
    WriteMotionShaders(dir, 0x10000, 0x20000);
}

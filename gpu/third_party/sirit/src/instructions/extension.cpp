/* This file is part of the sirit project.
 * Copyright (c) 2019 sirit
 * This software may be used and distributed according to the terms of the
 * 3-Clause BSD License
 */

#include <iterator>
#include <spirv/unified1/AMD_gcn_shader.h>
#include <spirv/unified1/AMD_shader_trinary_minmax.h>
#include <spirv/unified1/AMD_shader_explicit_vertex_parameter.h>
#include <spirv/unified1/GLSL.std.450.h>
#include <spirv/unified1/NonSemanticDebugPrintf.h>

#include "sirit/sirit.h"

#include "stream.h"

namespace Sirit {

Id Module::OpExtInst(Id result_type, Id set, u32 instruction, std::span<const Id> operands) {
    code->Reserve(5 + operands.size());
    return *code << OpId{spv::Op::OpExtInst, result_type} << set << instruction << operands
                 << EndOp{};
}

#define DEFINE_NULLARY(ext, funcname, opcode)                                                      \
    Id Module::funcname(Id result_type) {                                                          \
        return OpExtInst(result_type, ext, opcode);                                                \
    }

#define DEFINE_UNARY(ext, funcname, opcode)                                                        \
    Id Module::funcname(Id result_type, Id operand) {                                              \
        return OpExtInst(result_type, ext, opcode, operand);                                       \
    }

#define DEFINE_BINARY(ext, funcname, opcode)                                                       \
    Id Module::funcname(Id result_type, Id operand_1, Id operand_2) {                              \
        return OpExtInst(result_type, ext, opcode, operand_1, operand_2);                          \
    }

#define DEFINE_TRINARY(ext, funcname, opcode)                                                      \
    Id Module::funcname(Id result_type, Id operand_1, Id operand_2, Id operand_3) {                \
        return OpExtInst(result_type, ext, opcode, operand_1, operand_2, operand_3);               \
    }

DEFINE_UNARY(GetGLSLstd450(), OpFAbs, GLSLstd450FAbs)
DEFINE_UNARY(GetGLSLstd450(), OpSAbs, GLSLstd450SAbs)
DEFINE_UNARY(GetGLSLstd450(), OpRound, GLSLstd450Round)
DEFINE_UNARY(GetGLSLstd450(), OpRoundEven, GLSLstd450RoundEven)
DEFINE_UNARY(GetGLSLstd450(), OpTrunc, GLSLstd450Trunc)
DEFINE_UNARY(GetGLSLstd450(), OpFSign, GLSLstd450FSign)
DEFINE_UNARY(GetGLSLstd450(), OpSSign, GLSLstd450SSign)
DEFINE_UNARY(GetGLSLstd450(), OpFloor, GLSLstd450Floor)
DEFINE_UNARY(GetGLSLstd450(), OpCeil, GLSLstd450Ceil)
DEFINE_UNARY(GetGLSLstd450(), OpFract, GLSLstd450Fract)
DEFINE_UNARY(GetGLSLstd450(), OpSin, GLSLstd450Sin)
DEFINE_UNARY(GetGLSLstd450(), OpCos, GLSLstd450Cos)
DEFINE_UNARY(GetGLSLstd450(), OpAsin, GLSLstd450Asin)
DEFINE_UNARY(GetGLSLstd450(), OpAcos, GLSLstd450Acos)
DEFINE_BINARY(GetGLSLstd450(), OpPow, GLSLstd450Pow)
DEFINE_UNARY(GetGLSLstd450(), OpExp, GLSLstd450Exp)
DEFINE_UNARY(GetGLSLstd450(), OpLog, GLSLstd450Log)
DEFINE_UNARY(GetGLSLstd450(), OpExp2, GLSLstd450Exp2)
DEFINE_UNARY(GetGLSLstd450(), OpLog2, GLSLstd450Log2)
DEFINE_UNARY(GetGLSLstd450(), OpSqrt, GLSLstd450Sqrt)
DEFINE_UNARY(GetGLSLstd450(), OpInverseSqrt, GLSLstd450InverseSqrt)
DEFINE_BINARY(GetGLSLstd450(), OpLdexp, GLSLstd450Ldexp)
DEFINE_BINARY(GetGLSLstd450(), OpFMin, GLSLstd450FMin)
DEFINE_BINARY(GetGLSLstd450(), OpUMin, GLSLstd450UMin)
DEFINE_BINARY(GetGLSLstd450(), OpSMin, GLSLstd450SMin)
DEFINE_BINARY(GetGLSLstd450(), OpNMin, GLSLstd450NMin)
DEFINE_BINARY(GetGLSLstd450(), OpFMax, GLSLstd450FMax)
DEFINE_BINARY(GetGLSLstd450(), OpUMax, GLSLstd450UMax)
DEFINE_BINARY(GetGLSLstd450(), OpSMax, GLSLstd450SMax)
DEFINE_BINARY(GetGLSLstd450(), OpNMax, GLSLstd450NMax)
DEFINE_TRINARY(GetGLSLstd450(), OpFClamp, GLSLstd450FClamp)
DEFINE_TRINARY(GetGLSLstd450(), OpNClamp, GLSLstd450NClamp)
DEFINE_TRINARY(GetGLSLstd450(), OpUClamp, GLSLstd450UClamp)
DEFINE_TRINARY(GetGLSLstd450(), OpSClamp, GLSLstd450SClamp)
DEFINE_TRINARY(GetGLSLstd450(), OpFma, GLSLstd450Fma)
DEFINE_UNARY(GetGLSLstd450(), OpFrexpStruct, GLSLstd450FrexpStruct)
DEFINE_UNARY(GetGLSLstd450(), OpPackHalf2x16, GLSLstd450PackHalf2x16)
DEFINE_UNARY(GetGLSLstd450(), OpUnpackHalf2x16, GLSLstd450UnpackHalf2x16)
DEFINE_UNARY(GetGLSLstd450(), OpPackUnorm2x16, GLSLstd450PackUnorm2x16)
DEFINE_UNARY(GetGLSLstd450(), OpUnpackUnorm2x16, GLSLstd450UnpackUnorm2x16)
DEFINE_UNARY(GetGLSLstd450(), OpPackSnorm2x16, GLSLstd450PackSnorm2x16)
DEFINE_UNARY(GetGLSLstd450(), OpUnpackSnorm2x16, GLSLstd450UnpackSnorm2x16)
DEFINE_UNARY(GetGLSLstd450(), OpPackUnorm4x8, GLSLstd450PackUnorm4x8)
DEFINE_UNARY(GetGLSLstd450(), OpUnpackUnorm4x8, GLSLstd450UnpackUnorm4x8)
DEFINE_UNARY(GetGLSLstd450(), OpPackSnorm4x8, GLSLstd450PackSnorm4x8)
DEFINE_UNARY(GetGLSLstd450(), OpUnpackSnorm4x8, GLSLstd450UnpackSnorm4x8)
DEFINE_UNARY(GetGLSLstd450(), OpFindILsb, GLSLstd450FindILsb)
DEFINE_UNARY(GetGLSLstd450(), OpFindSMsb, GLSLstd450FindSMsb)
DEFINE_UNARY(GetGLSLstd450(), OpFindUMsb, GLSLstd450FindUMsb)
DEFINE_UNARY(GetGLSLstd450(), OpInterpolateAtCentroid, GLSLstd450InterpolateAtCentroid)
DEFINE_BINARY(GetGLSLstd450(), OpInterpolateAtSample, GLSLstd450InterpolateAtSample)
DEFINE_BINARY(GetGLSLstd450(), OpInterpolateAtOffset, GLSLstd450InterpolateAtOffset)
DEFINE_UNARY(GetGLSLstd450(), OpNormalize, GLSLstd450Normalize)
DEFINE_BINARY(GetGLSLstd450(), OpCross, GLSLstd450Cross)
DEFINE_UNARY(GetGLSLstd450(), OpLength, GLSLstd450Length)
DEFINE_TRINARY(GetGLSLstd450(), OpFMix, GLSLstd450FMix)

Id Module::OpDebugPrintf(Id fmt, std::span<const Id> fmt_args) {
    std::vector<Id> operands;
    operands.push_back(fmt);
    std::copy(fmt_args.begin(), fmt_args.end(), std::back_inserter(operands));
    return OpExtInst(TypeVoid(), GetNonSemanticDebugPrintf(), NonSemanticDebugPrintfDebugPrintf,
                     operands);
}

DEFINE_UNARY(GetAmdGcnShader(), OpCubeFaceCoordAMD, AMD_gcn_shaderCubeFaceCoordAMD)
DEFINE_UNARY(GetAmdGcnShader(), OpCubeFaceIndexAMD, AMD_gcn_shaderCubeFaceIndexAMD)
DEFINE_NULLARY(GetAmdGcnShader(), OpTimeAMD, AMD_gcn_shaderTimeAMD)

DEFINE_BINARY(GetAmdExplicitVertexParameter(), OpInterpolateAtVertexAMD, AMD_shader_explicit_vertex_parameterInterpolateAtVertexAMD)

DEFINE_TRINARY(GetAmdShaderTrinaryMinMax(), OpFMin3AMD, AMD_shader_trinary_minmaxFMin3AMD)
DEFINE_TRINARY(GetAmdShaderTrinaryMinMax(), OpUMin3AMD, AMD_shader_trinary_minmaxUMin3AMD)
DEFINE_TRINARY(GetAmdShaderTrinaryMinMax(), OpSMin3AMD, AMD_shader_trinary_minmaxSMin3AMD)
DEFINE_TRINARY(GetAmdShaderTrinaryMinMax(), OpFMax3AMD, AMD_shader_trinary_minmaxFMax3AMD)
DEFINE_TRINARY(GetAmdShaderTrinaryMinMax(), OpUMax3AMD, AMD_shader_trinary_minmaxUMax3AMD)
DEFINE_TRINARY(GetAmdShaderTrinaryMinMax(), OpSMax3AMD, AMD_shader_trinary_minmaxSMax3AMD)
DEFINE_TRINARY(GetAmdShaderTrinaryMinMax(), OpFMid3AMD, AMD_shader_trinary_minmaxFMid3AMD)
DEFINE_TRINARY(GetAmdShaderTrinaryMinMax(), OpUMid3AMD, AMD_shader_trinary_minmaxUMid3AMD)
DEFINE_TRINARY(GetAmdShaderTrinaryMinMax(), OpSMid3AMD, AMD_shader_trinary_minmaxSMid3AMD)


} // namespace Sirit

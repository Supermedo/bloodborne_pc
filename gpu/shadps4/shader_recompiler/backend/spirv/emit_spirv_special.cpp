// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "shader_recompiler/ir/debug_print.h"
#include "shader_recompiler/ir/microinstruction.h"

namespace Shader::Backend::SPIRV {

void EmitPrologue(EmitContext& ctx) {
    if (ctx.hw_stage == HwStage::Fragment) {
        ctx.DefineAmdPerVertexAttribs();
    }
    if (ctx.info.loads.Get(IR::Attribute::WorkgroupIndex)) {
        ctx.DefineWorkgroupIndex();
    }
    ctx.DefineBufferProperties();
}

void ConvertDepthMode(EmitContext& ctx) {
    const Id type{ctx.F32[1]};
    const Id position{ctx.OpLoad(ctx.F32[4], ctx.output_position)};
    const Id z{ctx.OpCompositeExtract(type, position, 2u)};
    const Id w{ctx.OpCompositeExtract(type, position, 3u)};
    const Id screen_depth{ctx.OpFMul(type, ctx.OpFAdd(type, z, w), ctx.Constant(type, 0.5f))};
    const Id vector{ctx.OpCompositeInsert(ctx.F32[4], screen_depth, position, 2u)};
    ctx.OpStore(ctx.output_position, vector);
}

void ConvertPositionToClipSpace(EmitContext& ctx) {
    ASSERT_MSG(!ctx.info.stores.GetAny(IR::Attribute::ViewportIndex),
               "Multi-viewport with shader clip space conversion not yet implemented.");

    const Id type{ctx.F32[1]};
    Id position{ctx.OpLoad(ctx.F32[4], ctx.output_position)};
    const Id x{ctx.OpCompositeExtract(type, position, 0u)};
    const Id y{ctx.OpCompositeExtract(type, position, 1u)};
    const Id z{ctx.OpCompositeExtract(type, position, 2u)};
    const Id w{ctx.OpCompositeExtract(type, position, 3u)};
    const Id xoffset_ptr{ctx.OpAccessChain(ctx.TypePointer(spv::StorageClass::PushConstant, type),
                                           ctx.push_data_block,
                                           ctx.ConstU32(PushData::XOffsetIndex))};
    const Id xoffset{ctx.OpLoad(type, xoffset_ptr)};
    const Id yoffset_ptr{ctx.OpAccessChain(ctx.TypePointer(spv::StorageClass::PushConstant, type),
                                           ctx.push_data_block,
                                           ctx.ConstU32(PushData::YOffsetIndex))};
    const Id yoffset{ctx.OpLoad(type, yoffset_ptr)};
    const Id xscale_ptr{ctx.OpAccessChain(ctx.TypePointer(spv::StorageClass::PushConstant, type),
                                          ctx.push_data_block,
                                          ctx.ConstU32(PushData::XScaleIndex))};
    const Id xscale{ctx.OpLoad(type, xscale_ptr)};
    const Id yscale_ptr{ctx.OpAccessChain(ctx.TypePointer(spv::StorageClass::PushConstant, type),
                                          ctx.push_data_block,
                                          ctx.ConstU32(PushData::YScaleIndex))};
    const Id yscale{ctx.OpLoad(type, yscale_ptr)};
    const Id vport_w =
        ctx.Constant(type, float(std::min<u32>(ctx.profile.max_viewport_width / 2, 8_KB)));
    const Id wnd_x = ctx.OpFAdd(type, ctx.OpFMul(type, x, xscale), xoffset);
    const Id ndc_x = ctx.OpFSub(type, ctx.OpFDiv(type, wnd_x, vport_w), ctx.Constant(type, 1.f));
    const Id vport_h =
        ctx.Constant(type, float(std::min<u32>(ctx.profile.max_viewport_height / 2, 8_KB)));
    const Id wnd_y = ctx.OpFAdd(type, ctx.OpFMul(type, y, yscale), yoffset);
    const Id ndc_y = ctx.OpFSub(type, ctx.OpFDiv(type, wnd_y, vport_h), ctx.Constant(type, 1.f));
    const Id vector{ctx.OpCompositeConstruct(ctx.F32[4], std::array<Id, 4>({ndc_x, ndc_y, z, w}))};
    ctx.OpStore(ctx.output_position, vector);
}

// RX6000: bounded ordinary StorageBuffers avoid raw GPU pointers for parameters/history.
// Atomic component stores handle duplicate indexed vertex invocations. The previous-frame
// half is read only after the native owner's frame barrier. Other GPUs retain the BDA path.
static Id EmitSsboHistory(EmitContext& ctx, Id position, Id store_base, Id load_base, Id slot,
                          Id do_store, Id do_load) {
    const Id scalar = ctx.U32[1], boolean = ctx.U1[1];
    const auto local_index = [&](Id base) {
        const Id global =
            ctx.OpUConvert(scalar, ctx.OpIAdd(ctx.U64, ctx.OpUConvert(ctx.U64, base), slot));
        const Id half = ctx.ConstU32(MotionVectors::PositionsPerHalf);
        const Id second = ctx.OpUGreaterThan(boolean, global, half);
        const Id local = ctx.OpSelect(scalar, second, ctx.OpISub(scalar, global, half), global);
        return std::pair{second, local};
    };
    const Id store_label = ctx.OpLabel(), store_merge = ctx.OpLabel();
    ctx.OpSelectionMerge(store_merge, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(do_store, store_label, store_merge);
    ctx.AddLabel(store_label);
    const auto [second_store, store_index] = local_index(store_base);
    const Id s0 = ctx.OpLabel(), s1 = ctx.OpLabel(), sj = ctx.OpLabel();
    ctx.OpSelectionMerge(sj, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(second_store, s1, s0);
    const Id pointer = ctx.TypePointer(spv::StorageClass::StorageBuffer, scalar);
    for (u32 half = 0; half < 2; ++half) {
        ctx.AddLabel(half ? s1 : s0);
        for (u32 component = 0; component < 4; ++component) {
            const Id address =
                ctx.OpAccessChain(pointer, ctx.motion_history_ssbo[half], ctx.u32_zero_value,
                                  store_index, ctx.ConstU32(component));
            const Id value =
                ctx.OpBitcast(scalar, ctx.OpCompositeExtract(ctx.F32[1], position, component));
            ctx.OpAtomicExchange(scalar, address, ctx.ConstU32(u32(spv::Scope::Device)),
                                 ctx.u32_zero_value, value);
        }
        ctx.OpBranch(sj);
    }
    ctx.AddLabel(sj);
    ctx.OpBranch(store_merge);
    ctx.AddLabel(store_merge);
    const Id load_label = ctx.OpLabel(), load_merge = ctx.OpLabel();
    ctx.OpSelectionMerge(load_merge, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(do_load, load_label, load_merge);
    ctx.AddLabel(load_label);
    const auto [second_load, load_index] = local_index(load_base);
    const Id l0 = ctx.OpLabel(), l1 = ctx.OpLabel(), lj = ctx.OpLabel();
    ctx.OpSelectionMerge(lj, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(second_load, l1, l0);
    std::array<Id, 2> loaded{};
    const Id vector_pointer = ctx.TypePointer(spv::StorageClass::StorageBuffer, ctx.U32[4]);
    for (u32 half = 0; half < 2; ++half) {
        ctx.AddLabel(half ? l1 : l0);
        const Id address = ctx.OpAccessChain(vector_pointer, ctx.motion_history_ssbo[half],
                                             ctx.u32_zero_value, load_index);
        loaded[half] = ctx.OpBitcast(ctx.F32[4], ctx.OpLoad(ctx.U32[4], address));
        ctx.OpBranch(lj);
    }
    ctx.AddLabel(lj);
    const Id value = ctx.OpPhi(ctx.F32[4], loaded[0], l0, loaded[1], l1);
    ctx.OpBranch(load_merge);
    ctx.AddLabel(load_merge);
    return ctx.OpPhi(ctx.F32[4], position, store_merge, value, lj);
}

static void EmitVertexMotionSsbo(EmitContext& ctx) {
    const Id position = ctx.OpLoad(ctx.F32[4], ctx.output_position);
    const Id u32_type = ctx.U32[1], bool_type = ctx.U1[1];
    const auto wide = [&](Id v) { return ctx.OpUConvert(ctx.U64, v); };
    const Id param_ptr =
        ctx.OpAccessChain(ctx.TypePointer(spv::StorageClass::PushConstant, u32_type),
                          ctx.push_data_block, ctx.ConstU32(PushData::MotionParamIndex));
    const Id raw_index = ctx.OpLoad(u32_type, param_ptr);
    const Id index = ctx.OpSelect(
        u32_type, ctx.OpULessThan(bool_type, raw_index, ctx.ConstU32(MotionVectors::ParamSlots)),
        raw_index, ctx.u32_zero_value);
    Id params, offsets;

    const Id pointer = ctx.TypePointer(spv::StorageClass::StorageBuffer, ctx.U32[4]);
    const Id record = ctx.OpIMul(u32_type, index, ctx.ConstU32(2u));
    params = ctx.OpLoad(
        ctx.U32[4], ctx.OpAccessChain(pointer, ctx.motion_params_ssbo, ctx.u32_zero_value, record));
    offsets = ctx.OpLoad(ctx.U32[4],
                         ctx.OpAccessChain(pointer, ctx.motion_params_ssbo, ctx.u32_zero_value,
                                           ctx.OpIAdd(u32_type, record, ctx.ConstU32(1u))));
    const auto field = [&](Id v, u32 i) { return ctx.OpCompositeExtract(u32_type, v, i); };
    const Id store_base = field(params, 0), load_base = field(params, 1);
    const Id vertices = field(params, 2), flags = field(params, 3);
    const Id vertex =
        ctx.OpISub(u32_type, ctx.OpLoad(u32_type, ctx.vertex_index), field(offsets, 0));
    const Id instance =
        ctx.OpISub(u32_type, ctx.OpLoad(u32_type, ctx.instance_id), field(offsets, 1));
    const Id in_range = ctx.OpLogicalAnd(bool_type, ctx.OpULessThan(bool_type, vertex, vertices),
                                         ctx.OpULessThan(bool_type, instance, field(offsets, 2)));
    const Id slot =
        ctx.OpIAdd(ctx.U64, wide(vertex), ctx.OpIMul(ctx.U64, wide(instance), wide(vertices)));
    const auto enabled = [&](u32 bit, Id base) {
        const Id flag = ctx.OpINotEqual(
            bool_type, ctx.OpBitwiseAnd(u32_type, flags, ctx.ConstU32(bit)), ctx.u32_zero_value);
        const Id nonzero = ctx.OpINotEqual(bool_type, base, ctx.u32_zero_value);
        const Id bounded =
            ctx.OpULessThan(bool_type, ctx.OpIAdd(ctx.U64, wide(base), slot),
                            ctx.Constant(ctx.U64, u64(MotionVectors::PositionSlots)));
        return ctx.OpLogicalAnd(bool_type, ctx.OpLogicalAnd(bool_type, in_range, flag),
                                ctx.OpLogicalAnd(bool_type, nonzero, bounded));
    };
    const Id do_store = enabled(MotionVectors::FlagStore, store_base);
    const Id do_load = enabled(MotionVectors::FlagLoad, load_base);
    Id previous = EmitSsboHistory(ctx, position, store_base, load_base, slot, do_store, do_load);
    const Id valid = ctx.OpSelect(ctx.F32[1], do_load, ctx.Constant(ctx.F32[1], 1.0f),
                                  ctx.Constant(ctx.F32[1], 0.0f));
    previous = ctx.OpCompositeInsert(ctx.F32[4], valid, previous, 2u);
    ctx.OpStore(ctx.motion_out_cur, position);
    ctx.OpStore(ctx.motion_out_prev, previous);
}

static void EmitVertexMotion(EmitContext& ctx) {
    if (MotionVectors::storage_buffers)
        return EmitVertexMotionSsbo(ctx);
    // Existing BDA path: parameters and history are addressed through GPU pointers.
    // RX 6000 uses bounded SSBO views above; keep BDA shader generation unchanged.
    // Disabled history loads/stores are skipped by the branches below.
    // Duplicate invocations can target the same slot; SSBO uses atomic stores above.
    const Id u32_type = ctx.U32[1];
    const Id position = ctx.OpLoad(ctx.F32[4], ctx.output_position);
    const Id param_ptr = ctx.OpAccessChain(ctx.TypePointer(spv::StorageClass::PushConstant, u32_type),
                                           ctx.push_data_block,
                                           ctx.ConstU32(PushData::MotionParamIndex));
    const Id param_index = ctx.OpLoad(u32_type, param_ptr);
    const auto address = [&](u64 base, Id index, u32 stride) {
        return ctx.OpIAdd(ctx.U64, ctx.Constant(ctx.U64, base),
                          ctx.OpIMul(ctx.U64, ctx.OpUConvert(ctx.U64, index),
                                     ctx.Constant(ctx.U64, u64(stride))));
    };
    const Id u32x4_ptr = ctx.TypePointer(spv::StorageClass::PhysicalStorageBuffer, ctx.U32[4]);
    const Id f32x4_ptr = ctx.TypePointer(spv::StorageClass::PhysicalStorageBuffer, ctx.F32[4]);
    const Id params = ctx.OpLoad(
        ctx.U32[4],
        ctx.OpConvertUToPtr(u32x4_ptr, address(MotionVectors::params_address, param_index, 32)),
        spv::MemoryAccessMask::Aligned, 16u);
    const Id store_base = ctx.OpCompositeExtract(u32_type, params, 0u);
    const Id load_base = ctx.OpCompositeExtract(u32_type, params, 1u);
    const Id vertices = ctx.OpCompositeExtract(u32_type, params, 2u);
    const Id flags = ctx.OpCompositeExtract(u32_type, params, 3u);
    const Id offsets = ctx.OpLoad(
        ctx.U32[4], ctx.OpConvertUToPtr(u32x4_ptr,
            address(MotionVectors::params_address + 16, param_index, 32)),
        spv::MemoryAccessMask::Aligned, 16u);
    const Id first_vertex = ctx.OpCompositeExtract(u32_type, offsets, 0u);
    const Id first_instance = ctx.OpCompositeExtract(u32_type, offsets, 1u);
    const Id instances = ctx.OpCompositeExtract(u32_type, offsets, 2u);
    const Id vertex = ctx.OpISub(u32_type, ctx.OpLoad(u32_type, ctx.vertex_index), first_vertex);
    const Id instance = ctx.OpISub(u32_type, ctx.OpLoad(u32_type, ctx.instance_id), first_instance);
    const Id bool_type = ctx.U1[1];
    const Id in_range = ctx.OpLogicalAnd(bool_type, ctx.OpULessThan(bool_type, vertex, vertices),
                                         ctx.OpULessThan(bool_type, instance, instances));
    const Id slot = ctx.OpIAdd(u32_type, vertex, ctx.OpIMul(u32_type, instance, vertices));
    const auto flag = [&](u32 bit) {
        return ctx.OpLogicalAnd(
            bool_type, in_range,
            ctx.OpINotEqual(bool_type, ctx.OpBitwiseAnd(u32_type, flags, ctx.ConstU32(bit)),
                            ctx.u32_zero_value));
    };
    const Id do_store = flag(MotionVectors::FlagStore);
    const Id do_load = flag(MotionVectors::FlagLoad);
    const Id store_label = ctx.OpLabel();
    const Id store_merge = ctx.OpLabel();
    ctx.OpSelectionMerge(store_merge, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(do_store, store_label, store_merge);
    ctx.AddLabel(store_label);
    const Id store_address = address(MotionVectors::positions_address,
                                     ctx.OpIAdd(u32_type, store_base, slot), 16);
    ctx.OpStore(ctx.OpConvertUToPtr(f32x4_ptr, store_address), position,
                spv::MemoryAccessMask::Aligned, 16u);
    ctx.OpBranch(store_merge);
    ctx.AddLabel(store_merge);

    const Id load_label = ctx.OpLabel();
    const Id load_merge = ctx.OpLabel();
    ctx.OpSelectionMerge(load_merge, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(do_load, load_label, load_merge);
    ctx.AddLabel(load_label);
    const Id loaded = ctx.OpLoad(
        ctx.F32[4], ctx.OpConvertUToPtr(f32x4_ptr, address(MotionVectors::positions_address,
            ctx.OpIAdd(u32_type, load_base, slot), 16)), spv::MemoryAccessMask::Aligned, 16u);
    ctx.OpBranch(load_merge);
    ctx.AddLabel(load_merge);
    Id previous = ctx.OpPhi(ctx.F32[4], position, store_merge, loaded, load_label);
    const Id valid = ctx.OpSelect(ctx.F32[1], do_load, ctx.Constant(ctx.F32[1], 1.0f),
                                  ctx.Constant(ctx.F32[1], 0.0f));
    previous = ctx.OpCompositeInsert(ctx.F32[4], valid, previous, 2u);
    ctx.OpStore(ctx.motion_out_cur, position);
    ctx.OpStore(ctx.motion_out_prev, previous);
}

// Screen-space motion (previous minus current, pixels) through the viewport scale; z = valid.
static void EmitFragmentMotion(EmitContext& ctx) {
    const Id f32_type = ctx.F32[1];
    const Id current = ctx.OpLoad(ctx.F32[4], ctx.motion_in_cur);
    const Id previous = ctx.OpLoad(ctx.F32[4], ctx.motion_in_prev);
    const auto push = [&](u32 index) {
        return ctx.OpLoad(f32_type, ctx.OpAccessChain(ctx.TypePointer(spv::StorageClass::PushConstant,
                                                                 f32_type),
                                                 ctx.push_data_block, ctx.ConstU32(index)));
    };
    const Id xscale = push(PushData::XScaleIndex);
    const Id yscale = push(PushData::YScaleIndex);
    const auto ndc = [&](Id clip, u32 component) {
        return ctx.OpFDiv(f32_type, ctx.OpCompositeExtract(f32_type, clip, component),
                          ctx.OpCompositeExtract(f32_type, clip, 3u));
    };
    const Id dx = ctx.OpFMul(f32_type, ctx.OpFSub(f32_type, ndc(previous, 0u), ndc(current, 0u)), xscale);
    const Id dy = ctx.OpFMul(f32_type, ctx.OpFSub(f32_type, ndc(previous, 1u), ndc(current, 1u)), yscale);
    const Id zero = ctx.Constant(f32_type, 0.0f);
    const Id epsilon = ctx.Constant(f32_type, 1e-5f);
    const Id front = ctx.OpLogicalAnd(ctx.U1[1],
        ctx.OpFOrdGreaterThan(ctx.U1[1], ctx.OpCompositeExtract(f32_type, previous, 3u), epsilon),
        ctx.OpFOrdGreaterThan(ctx.U1[1], ctx.OpCompositeExtract(f32_type, current, 3u), epsilon));
    const Id valid = ctx.OpSelect(f32_type, front,
                                  ctx.OpCompositeExtract(f32_type, previous, 2u), zero);
    // Uninstrumented draws can overwrite the final scene depth after this pixel was
    // written. Preserve depth so the compose pass can reject an occluded vector.
    const Id depth = ctx.OpCompositeExtract(f32_type,
                                            ctx.OpLoad(ctx.F32[4], ctx.frag_coord), 2u);
    ctx.OpStore(ctx.motion_frag_out,
                ctx.OpCompositeConstruct(ctx.F32[4], std::array<Id, 4>{
                    ctx.OpSelect(f32_type, front, dx, zero),
                    ctx.OpSelect(f32_type, front, dy, zero), valid, depth}));
}

// Apply projection jitter after recording unjittered positions for object motion.
// The viewport remains fixed, so its clipping bounds cannot expose an edge row.
static void EmitProjectionJitter(EmitContext& ctx) {
    const Id type = ctx.F32[1];
    const Id position = ctx.OpLoad(ctx.F32[4], ctx.output_position);
    const Id w = ctx.OpCompositeExtract(type, position, 3u);
    Id result = position;
    for (u32 axis = 0; axis < 2; ++axis) {
        const Id pointer = ctx.OpAccessChain(
            ctx.TypePointer(spv::StorageClass::PushConstant, type),
            ctx.push_data_block, ctx.ConstU32(axis == 0 ? PushData::XOffsetIndex
                                                      : PushData::YOffsetIndex));
        const Id jitter = ctx.OpLoad(type, pointer);
        const Id coordinate = ctx.OpCompositeExtract(type, position, axis);
        const Id shifted = ctx.OpFAdd(type, coordinate, ctx.OpFMul(type, jitter, w));
        result = ctx.OpCompositeInsert(ctx.F32[4], shifted, result, axis);
    }
    ctx.OpStore(ctx.output_position, result);
}

void EmitEpilogue(EmitContext& ctx) {
    if (Sirit::ValidId(ctx.motion_out_cur)) {
        EmitVertexMotion(ctx);
    }
    if (Sirit::ValidId(ctx.motion_frag_out)) {
        EmitFragmentMotion(ctx);
    }
    if (ctx.hw_stage == HwStage::Vertex &&
        ctx.runtime_info.hw.vs.emulate_depth_negative_one_to_one) {
        ConvertDepthMode(ctx);
    }
    if (ctx.hw_stage == HwStage::Vertex && ctx.runtime_info.hw.vs.clip_disable) {
        ConvertPositionToClipSpace(ctx);
    } else if (ctx.hw_stage == HwStage::Vertex && Sirit::ValidId(ctx.output_position)) {
        EmitProjectionJitter(ctx);
    }
}

void EmitDiscard(EmitContext& ctx) {
    ctx.OpDemoteToHelperInvocationEXT();
}

void EmitDiscardCond(EmitContext& ctx, Id condition) {
    const Id kill_label{ctx.OpLabel()};
    const Id merge_label{ctx.OpLabel()};
    ctx.OpSelectionMerge(merge_label, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(condition, kill_label, merge_label);
    ctx.AddLabel(kill_label);
    ctx.OpDemoteToHelperInvocationEXT();
    ctx.OpBranch(merge_label);
    ctx.AddLabel(merge_label);
}

void EmitEmitVertex(EmitContext& ctx) {
    ctx.OpEmitVertex();
}

void EmitEmitPrimitive(EmitContext& ctx) {
    ctx.OpEndPrimitive();
}

void EmitEmitVertex(EmitContext& ctx, const IR::Value& stream) {
    UNREACHABLE_MSG("Geometry streams");
}

void EmitEndPrimitive(EmitContext& ctx, const IR::Value& stream) {
    UNREACHABLE_MSG("Geometry streams");
}

void EmitDebugPrint(EmitContext& ctx, IR::Inst* inst, Id fmt, Id arg0, Id arg1, Id arg2, Id arg3) {
    IR::DebugPrintFlags flags = inst->Flags<IR::DebugPrintFlags>();
    std::array<Id, IR::DEBUGPRINT_NUM_FORMAT_ARGS> fmt_args = {arg0, arg1, arg2, arg3};
    auto fmt_args_span = std::span<Id>(fmt_args.begin(), fmt_args.begin() + flags.num_args);
    ctx.OpDebugPrintf(fmt, fmt_args_span);
}

Id EmitMemtime(EmitContext& ctx) {
    if (ctx.profile.supports_shader_subgroup_clock) {
        return ctx.OpReadClockKHR(ctx.U64, ctx.ConstU32(std::to_underlying(spv::Scope::Subgroup)));
    } else {
        return ctx.Constant(ctx.U64, 1U);
    }
}

} // namespace Shader::Backend::SPIRV

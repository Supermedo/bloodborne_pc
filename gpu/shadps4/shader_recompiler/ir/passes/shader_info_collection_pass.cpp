// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <boost/container/small_vector.hpp>

#include "core/emulator_settings.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/profile.h"
#include "video_core/buffer_cache/buffer_cache.h"

namespace Shader::Optimization {

void Visit(Info& info, const IR::Inst& inst) {
    switch (inst.GetOpcode()) {
    case IR::Opcode::GetAttribute:
    case IR::Opcode::GetAttributeU1:
    case IR::Opcode::GetAttributeU32:
        info.loads.Set(inst.Arg(0).Attribute(), inst.Arg(1).U32());
        break;
    case IR::Opcode::SetAttribute:
        info.stores.Set(inst.Arg(0).Attribute(), inst.Arg(2).U32());
        break;
    case IR::Opcode::GetUserData:
        info.ud_mask.Set(inst.Arg(0).ScalarReg());
        break;
    case IR::Opcode::SetPatch: {
        const auto patch = inst.Arg(0).Patch();
        if (patch <= IR::Patch::TessellationLodBottom) {
            info.stores_tess_level_outer = true;
        } else if (patch <= IR::Patch::TessellationLodInteriorV) {
            info.stores_tess_level_inner = true;
        } else {
            info.uses_patches |= 1U << IR::GenericPatchIndex(patch);
        }
        break;
    }
    case IR::Opcode::GetPatch: {
        const auto patch = inst.Arg(0).Patch();
        info.uses_patches |= 1U << IR::GenericPatchIndex(patch);
        break;
    }
    case IR::Opcode::LoadSharedU16:
    case IR::Opcode::WriteSharedU16:
        info.shared_types |= IR::Type::U16;
        break;
    case IR::Opcode::LoadSharedU32:
    case IR::Opcode::WriteSharedU32:
    case IR::Opcode::SharedAtomicIAdd32:
    case IR::Opcode::SharedAtomicISub32:
    case IR::Opcode::SharedAtomicSMin32:
    case IR::Opcode::SharedAtomicUMin32:
    case IR::Opcode::SharedAtomicSMax32:
    case IR::Opcode::SharedAtomicUMax32:
    case IR::Opcode::SharedAtomicInc32:
    case IR::Opcode::SharedAtomicDec32:
    case IR::Opcode::SharedAtomicAnd32:
    case IR::Opcode::SharedAtomicOr32:
    case IR::Opcode::SharedAtomicXor32:
    case IR::Opcode::SharedAtomicCmpSwap32:
        info.shared_types |= IR::Type::U32;
        break;
    case IR::Opcode::SharedAtomicIAdd64:
    case IR::Opcode::SharedAtomicISub64:
    case IR::Opcode::SharedAtomicSMin64:
    case IR::Opcode::SharedAtomicUMin64:
    case IR::Opcode::SharedAtomicSMax64:
    case IR::Opcode::SharedAtomicUMax64:
    case IR::Opcode::SharedAtomicInc64:
    case IR::Opcode::SharedAtomicDec64:
    case IR::Opcode::SharedAtomicAnd64:
    case IR::Opcode::SharedAtomicOr64:
    case IR::Opcode::SharedAtomicXor64:
    case IR::Opcode::SharedAtomicCmpSwap64:
        info.uses_shared_int64_atomics = true;
        [[fallthrough]];
    case IR::Opcode::LoadSharedU64:
    case IR::Opcode::WriteSharedU64:
        info.shared_types |= IR::Type::U64;
        break;
    case IR::Opcode::ConvertF16F32:
    case IR::Opcode::ConvertF32F16:
    case IR::Opcode::BitCastU16F16:
    case IR::Opcode::BitCastF16U16:
        info.uses_fp16 = true;
        break;
    case IR::Opcode::PackDouble2x32:
    case IR::Opcode::UnpackDouble2x32:
        info.uses_fp64 = true;
        break;
    case IR::Opcode::ImageWrite:
        info.has_storage_images = true;
        break;
    case IR::Opcode::QuadBroadcast:
        info.uses_group_quad = true;
        break;
    case IR::Opcode::Shuffle:
    case IR::Opcode::ShuffleXor:
        info.uses_group_shuffle = true;
        break;
    case IR::Opcode::ReadLane:
    case IR::Opcode::ReadFirstLane:
    case IR::Opcode::WriteLane:
        info.uses_group_ballot = true;
        break;
    case IR::Opcode::Discard:
    case IR::Opcode::DiscardCond:
        info.has_discard = true;
        break;
    case IR::Opcode::BitwiseXor32:
        info.has_bitwise_xor = true;
        break;
    case IR::Opcode::ImageGather:
    case IR::Opcode::ImageGatherDref:
        info.has_image_gather = true;
        break;
    case IR::Opcode::ImageQueryDimensions:
    case IR::Opcode::ImageQueryLod:
        info.has_image_query = true;
        break;
    case IR::Opcode::ImageAtomicFMax32:
    case IR::Opcode::ImageAtomicFMin32:
        info.uses_image_atomic_float_min_max = true;
        break;
    case IR::Opcode::BufferAtomicFMax32:
    case IR::Opcode::BufferAtomicFMin32:
        info.uses_buffer_atomic_float_min_max = true;
        break;
    case IR::Opcode::BufferAtomicIAdd64:
    case IR::Opcode::BufferAtomicSMax64:
    case IR::Opcode::BufferAtomicSMin64:
    case IR::Opcode::BufferAtomicUMax64:
    case IR::Opcode::BufferAtomicUMin64:
        info.uses_buffer_int64_atomics = true;
        break;
    case IR::Opcode::DataAppend:
    case IR::Opcode::DataConsume:
    case IR::Opcode::Ballot:
    case IR::Opcode::InverseBallot:
    case IR::Opcode::BallotFindLsb:
        info.uses_group_ballot = true;
        [[fallthrough]];
    case IR::Opcode::LaneId:
        info.uses_lane_id = true;
        break;
    case IR::Opcode::Memtime:
        info.uses_shader_clock = true;
        break;
    case IR::Opcode::ReadConst:
        if (!info.has_readconst) {
            info.buffers.push_back({
                .used_types = IR::Type::U32,
                .buffer_type = BufferType::Flatbuf,
            });
            info.has_readconst = true;
        }
        if (inst.Flags<u32>() == 0) {
            info.readconst_types |= Info::ReadConstType::Immediate;
            info.readconst_types |= Info::ReadConstType::Dynamic;
            info.uses_dma = true;
        }
        break;
    case IR::Opcode::PackUfloat10_11_11:
        info.uses_pack_10_11_11 = true;
        break;
    case IR::Opcode::UnpackUfloat10_11_11:
        info.uses_unpack_10_11_11 = true;
        break;
    default:
        break;
    }
}

static bool IsSample(IR::Opcode op) {
    return op == IR::Opcode::ImageSampleImplicitLod || op == IR::Opcode::ImageSampleExplicitLod ||
           op == IR::Opcode::ImageSampleRaw;
}

static const IR::Inst* FindUpstream(const IR::Value& root, auto&& match) {
    boost::container::small_vector<const IR::Inst*, 64> queue;
    boost::container::small_vector<const IR::Inst*, 64> seen;
    if (root.IsImmediate() || !root.Inst()) {
        return nullptr;
    }
    queue.push_back(root.Inst());
    size_t head = 0;
    while (head < queue.size() && queue.size() < 96) {
        const IR::Inst* inst = queue[head++];
        if (std::find(seen.begin(), seen.end(), inst) != seen.end()) {
            continue;
        }
        seen.push_back(inst);
        if (match(*inst)) {
            return inst;
        }
        for (size_t i = 0; i < inst->NumArgs(); ++i) {
            const IR::Value arg = inst->Arg(i);
            if (!arg.IsImmediate() && arg.Inst()) {
                queue.push_back(arg.Inst());
            }
        }
    }
    return nullptr;
}

static void FindAlbedo(IR::Program& program) {
    Info& info = program.info;
    if (info.sw_stage != SwStage::Fragment) {
        return;
    }
    for (const IR::Attribute target : {IR::Attribute::RenderTarget2, IR::Attribute::RenderTarget0,
                                       IR::Attribute::RenderTarget1, IR::Attribute::RenderTarget3}) {
        for (IR::Block* const block : program.post_order_blocks) {
            for (const IR::Inst& inst : block->Instructions()) {
                if (inst.GetOpcode() != IR::Opcode::SetAttribute ||
                    inst.Arg(0).Attribute() != target) {
                    continue;
                }
                const IR::Inst* sample = FindUpstream(inst.Arg(1), [](const IR::Inst& i) {
                    return IsSample(i.GetOpcode()) && i.Arg(0).IsImmediate();
                });
                if (!sample) {
                    continue;
                }
                const IR::Inst* attr = FindUpstream(sample->Arg(1), [](const IR::Inst& i) {
                    return i.GetOpcode() == IR::Opcode::GetAttribute &&
                           IR::IsParam(i.Arg(0).Attribute());
                });
                if (!attr) {
                    continue;
                }
                const u32 input = u32(attr->Arg(0).Attribute()) - u32(IR::Attribute::Param0);
                const u32 component = attr->Arg(1).U32() & 3u;
                info.rt_albedo_image = s32(sample->Arg(0).U32() & 0xFFFFu);
                info.rt_albedo_input = s32(input);
                info.rt_albedo_components = u8(component | ((component + 1u) & 3u) << 4);
                return;
            }
        }
    }
}

static void FindAlphaSource(IR::Program& program) {
    Info& info = program.info;
    if (info.sw_stage != SwStage::Fragment || !info.has_discard) {
        return;
    }
    for (IR::Block* const block : program.post_order_blocks) {
        for (const IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() != IR::Opcode::DiscardCond) {
                continue;
            }
            const IR::Inst* extract = FindUpstream(inst.Arg(0), [](const IR::Inst& i) {
                if (i.GetOpcode() != IR::Opcode::CompositeExtractF32x4 || !i.Arg(1).IsImmediate()) {
                    return false;
                }
                const IR::Value v = i.Arg(0);
                return !v.IsImmediate() && v.Inst() && IsSample(v.Inst()->GetOpcode()) &&
                       v.Inst()->Arg(0).IsImmediate();
            });
            if (!extract) {
                continue;
            }
            const IR::Inst* sample = extract->Arg(0).Inst();
            const IR::Inst* attr = FindUpstream(sample->Arg(1), [](const IR::Inst& i) {
                return i.GetOpcode() == IR::Opcode::GetAttribute && IR::IsParam(i.Arg(0).Attribute());
            });
            if (!attr) {
                continue;
            }
            const u32 component = attr->Arg(1).U32() & 3u;
            info.rt_alpha_image = s32(sample->Arg(0).U32() & 0xFFFFu);
            info.rt_alpha_input = s32(u32(attr->Arg(0).Attribute()) - u32(IR::Attribute::Param0));
            info.rt_alpha_components = u8(component | ((component + 1u) & 3u) << 4);
            info.rt_alpha_channel = u8(extract->Arg(1).U32() & 3u);
            return;
        }
    }
}

static void FindNormalMap(IR::Program& program) {
    Info& info = program.info;
    if (info.sw_stage != SwStage::Fragment) {
        return;
    }
    for (IR::Block* const block : program.post_order_blocks) {
        for (const IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() != IR::Opcode::SetAttribute ||
                inst.Arg(0).Attribute() != IR::Attribute::RenderTarget0) {
                continue;
            }
            const IR::Inst* sample = FindUpstream(inst.Arg(1), [](const IR::Inst& i) {
                return IsSample(i.GetOpcode()) && i.Arg(0).IsImmediate();
            });
            boost::container::small_vector<const IR::Inst*, 64> queue;
            boost::container::small_vector<const IR::Inst*, 64> seen;
            boost::container::small_vector<u32, 4> params;
            if (!inst.Arg(1).IsImmediate() && inst.Arg(1).Inst()) {
                queue.push_back(inst.Arg(1).Inst());
            }
            size_t head = 0;
            while (head < queue.size() && queue.size() < 128) {
                const IR::Inst* i = queue[head++];
                if (std::find(seen.begin(), seen.end(), i) != seen.end()) {
                    continue;
                }
                seen.push_back(i);
                if (IsSample(i->GetOpcode())) {
                    continue;
                }
                if (i->GetOpcode() == IR::Opcode::GetAttribute && IR::IsParam(i->Arg(0).Attribute())) {
                    const u32 p = u32(i->Arg(0).Attribute()) - u32(IR::Attribute::Param0);
                    if (std::find(params.begin(), params.end(), p) == params.end() && params.size() < 4) {
                        params.push_back(p);
                    }
                    continue;
                }
                for (size_t a = 0; a < i->NumArgs(); ++a) {
                    const IR::Value arg = i->Arg(a);
                    if (!arg.IsImmediate() && arg.Inst()) {
                        queue.push_back(arg.Inst());
                    }
                }
            }
            if (params.empty()) {
                continue;
            }
            info.rt_normal_image = sample ? s32(sample->Arg(0).U32() & 0xFFFFu) : -1;
            info.rt_normal_inputs = {s32(params[0]), params.size() > 1 ? s32(params[1]) : -1};
            return;
        }
    }
}

void CollectShaderInfoPass(IR::Program& program, const Profile& profile) {
    Info& info = program.info;
    for (IR::Block* const block : program.post_order_blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            Visit(info, inst);
        }
    }
    FindAlbedo(program);
    FindNormalMap(program);
    FindAlphaSource(program);

    if (!EmulatorSettings.IsDirectMemoryAccessEnabled()) {
        info.uses_dma = false;
        info.readconst_types = Info::ReadConstType::None;
    }

    if (info.uses_dma) {
        info.buffers.push_back({
            .used_types = IR::Type::U64,
            .buffer_type = BufferType::BdaPagetable,
            .is_written = true,
        });
        info.buffers.push_back({
            .used_types = IR::Type::U32,
            .buffer_type = BufferType::FaultBuffer,
            .is_written = true,
        });
        LOG_ERROR(Render, "Enabling DMA for shader {:#x}", info.pgm_hash);
    }
}

} // namespace Shader::Optimization

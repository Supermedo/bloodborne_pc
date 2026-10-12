// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <ranges>
#include <shared_mutex>
#include <string>
#include <unordered_set>
#include <vector>
#include <boost/container/static_vector.hpp>
#include <xxhash.h>

#include "common/hash.h"
#include "common/io_file.h"
#include "common/path_util.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/motion_history.h"
#include "video_core/renderer_vulkan/vk_compile_stats.h"
#include "video_core/renderer_vulkan/vk_draw_prep.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_serialization.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

#include "bbport_toggles.h"
#include "common/io_file.h"
#include "common/elf_info.h"

namespace Vulkan {

using Shader::HwStage;
using Shader::Output;
using Shader::SwStage;

constexpr static auto SpirvVersion1_6 = 0x00010600U;

constexpr static std::array DescriptorHeapSizes = {
    vk::DescriptorPoolSize{vk::DescriptorType::eUniformBuffer, 512},
    vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, 65536},
    vk::DescriptorPoolSize{vk::DescriptorType::eSampledImage, 32768},
    vk::DescriptorPoolSize{vk::DescriptorType::eStorageImage, 4096},
    vk::DescriptorPoolSize{vk::DescriptorType::eSampler, 16384},
};
// Every graphics draw takes one pre-rasterization set from the heap.
constexpr u32 DescriptorHeapSets = 8192;

static u32 MapOutputs(std::span<Shader::OutputMap, 3> outputs, const AmdGpu::VsOutputControl& ctl) {
    u32 num_outputs = 0;

    if (ctl.vs_out_misc_enable) {
        auto& misc_vec = outputs[num_outputs++];
        misc_vec[0] = ctl.use_vtx_point_size ? Output::PointSize : Output::None;
        misc_vec[1] = ctl.use_vtx_edge_flag
                          ? Output::EdgeFlag
                          : (ctl.use_vtx_gs_cut_flag ? Output::GsCutFlag : Output::None);
        misc_vec[2] =
            ctl.use_vtx_kill_flag
                ? Output::KillFlag
                : (ctl.use_vtx_render_target_idx ? Output::RenderTargetIndex : Output::None);
        misc_vec[3] = ctl.use_vtx_viewport_idx ? Output::ViewportIndex : Output::None;
    }

    if (ctl.vs_out_ccdist0_enable) {
        auto& ccdist0 = outputs[num_outputs++];
        ccdist0[0] = ctl.IsClipDistEnabled(0)
                         ? Output::ClipDist0
                         : (ctl.IsCullDistEnabled(0) ? Output::CullDist0 : Output::None);
        ccdist0[1] = ctl.IsClipDistEnabled(1)
                         ? Output::ClipDist1
                         : (ctl.IsCullDistEnabled(1) ? Output::CullDist1 : Output::None);
        ccdist0[2] = ctl.IsClipDistEnabled(2)
                         ? Output::ClipDist2
                         : (ctl.IsCullDistEnabled(2) ? Output::CullDist2 : Output::None);
        ccdist0[3] = ctl.IsClipDistEnabled(3)
                         ? Output::ClipDist3
                         : (ctl.IsCullDistEnabled(3) ? Output::CullDist3 : Output::None);
    }

    if (ctl.vs_out_ccdist1_enable) {
        auto& ccdist1 = outputs[num_outputs++];
        ccdist1[0] = ctl.IsClipDistEnabled(4)
                         ? Output::ClipDist4
                         : (ctl.IsCullDistEnabled(4) ? Output::CullDist4 : Output::None);
        ccdist1[1] = ctl.IsClipDistEnabled(5)
                         ? Output::ClipDist5
                         : (ctl.IsCullDistEnabled(5) ? Output::CullDist5 : Output::None);
        ccdist1[2] = ctl.IsClipDistEnabled(6)
                         ? Output::ClipDist6
                         : (ctl.IsCullDistEnabled(6) ? Output::CullDist6 : Output::None);
        ccdist1[3] = ctl.IsClipDistEnabled(7)
                         ? Output::ClipDist7
                         : (ctl.IsCullDistEnabled(7) ? Output::CullDist7 : Output::None);
    }

    return num_outputs;
}

const Shader::RuntimeInfo& PipelineCache::BuildRuntimeInfo(PipelineSelection& sel, HwStage stage,
                                                             SwStage l_stage) {
    auto& info = sel.runtime_infos[u32(l_stage)];
    const auto& regs = (*sel.regs);
    const auto BuildCommon = [&](const auto& program) {
        info.props.num_user_data = program.settings.num_user_regs;
        info.props.num_input_vgprs = program.settings.vgpr_comp_cnt;
        info.props.num_allocated_vgprs = program.NumVgprs();
        info.props.fp_denorm_mode32 = program.settings.fp_denorm_mode32;
        info.props.fp_denorm_mode16_64 = program.settings.fp_denorm_mode64;
        info.props.fp_round_mode32 = program.settings.fp_round_mode32;
        info.props.fp_round_mode16_64 = program.settings.fp_round_mode64;
    };
    info.Initialize(stage, l_stage);
    switch (stage) {
    case HwStage::Local: {
        BuildCommon(regs.ls_program);
        Shader::TessellationDataConstantBuffer tess_constants{};
        const auto* hull_info = sel.infos[u32(SwStage::TessellationControl)];
        hull_info->ReadTessConstantBuffer(tess_constants);
        info.hw.ls.ls_stride = tess_constants.ls_stride;
        break;
    }
    case HwStage::Hull:
        BuildCommon(regs.hs_program);
        break;
    case HwStage::Export:
        BuildCommon(regs.es_program);
        info.hw.es.vertex_data_size = regs.vgt_esgs_ring_itemsize;
        break;
    case HwStage::Geometry: {
        BuildCommon(regs.gs_program);
        info.hw.gs.num_outputs = MapOutputs(info.hw.gs.outputs, regs.vs_output_control);
        info.hw.gs.output_vertices = regs.vgt_gs_max_vert_out;
        info.hw.gs.num_invocations =
            regs.vgt_gs_instance_cnt.IsEnabled() ? regs.vgt_gs_instance_cnt.count : 1;
        if (regs.stage_enable.raw == AmdGpu::ShaderStageEnable::LsHsEsGs) {
            info.hw.gs.in_primitive = [&]() {
                switch (regs.tess_config.topology) {
                case AmdGpu::TessellationTopology::Point:
                    return AmdGpu::PrimitiveType::PointList;
                case AmdGpu::TessellationTopology::Line:
                    return AmdGpu::PrimitiveType::LineList;
                case AmdGpu::TessellationTopology::TriangleCw:
                case AmdGpu::TessellationTopology::TriangleCcw:
                    return AmdGpu::PrimitiveType::TriangleList;
                default:
                    UNREACHABLE();
                }
            }();
        } else {
            info.hw.gs.in_primitive = regs.primitive_type;
        }
        for (u32 stream_id = 0; stream_id < Shader::GsMaxOutputStreams; ++stream_id) {
            info.hw.gs.out_primitive[stream_id] =
                regs.vgt_gs_out_prim_type.GetPrimitiveType(stream_id);
        }
        info.hw.gs.in_vertex_data_size = regs.vgt_esgs_ring_itemsize;
        info.hw.gs.out_vertex_data_size = regs.vgt_gs_vert_itemsize[0];
        info.hw.gs.mode = regs.vgt_gs_mode.mode;
        const auto params_vc = AmdGpu::GetParams(regs.vs_program);
        info.hw.gs.vs_copy = params_vc.code;
        info.hw.gs.vs_copy_hash = params_vc.hash;
        DumpShader(info.hw.gs.vs_copy, info.hw.gs.vs_copy_hash, Shader::HwStage::Vertex, 0,
                   "copy.bin");
        break;
    }
    case HwStage::Vertex: {
        BuildCommon(regs.vs_program);
        info.hw.vs.user_clip_plane_mask = regs.clipper_control.user_clip_plane_enable;
        info.hw.vs.num_outputs = MapOutputs(info.hw.vs.outputs, regs.vs_output_control);
        info.hw.vs.emulate_depth_negative_one_to_one =
            !instance.IsDepthClipControlSupported() &&
            regs.clipper_control.clip_space == AmdGpu::ClipSpace::MinusWToW;
        info.hw.vs.clip_disable = regs.IsClipDisabled();
        info.hw.vs.motion_vectors = sel.motion;
        info.hw.vs.remix_capture = sel.remix_capture;
        info.hw.vs.remix_uv = sel.remix_uv;
        break;
    }
    case HwStage::Fragment: {
        BuildCommon(regs.ps_program);
        info.hw.fs.en_flags = regs.ps_input_ena;
        info.hw.fs.addr_flags = regs.ps_input_addr;
        info.hw.fs.num_inputs = regs.num_interp;
        info.hw.fs.front_face_all_bits = regs.barycentric_control.front_face_all_bits;
        info.hw.fs.num_samples =
            regs.ps_input_addr.sample_coverage_ena && regs.ps_input_ena.sample_coverage_ena
                ? regs.aa_config.NumSamples()
                : 1;
        info.hw.fs.z_export_format = regs.z_export_format;
        info.hw.fs.motion_vectors = sel.motion;
        u8 stencil_ref_export_enable = regs.depth_shader_control.stencil_op_val_export_enable |
                                       regs.depth_shader_control.stencil_test_val_export_enable;
        info.hw.fs.mrtz_mask = regs.depth_shader_control.z_export_enable |
                               (stencil_ref_export_enable << 1) |
                               (regs.depth_shader_control.mask_export_enable << 2) |
                               (regs.depth_shader_control.coverage_to_mask_enable << 3);
        const auto& cb0_blend = regs.blend_control[0];
        if (cb0_blend.enable) {
            info.hw.fs.dual_source_blending =
                LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.color_dst_factor) ||
                LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.color_src_factor);
            if (cb0_blend.separate_alpha_blend) {
                info.hw.fs.dual_source_blending |=
                    LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.alpha_dst_factor) ||
                    LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.alpha_src_factor);
            }
        } else {
            info.hw.fs.dual_source_blending = false;
        }
        const auto& ps_inputs = regs.ps_inputs;
        for (u32 i = 0; i < regs.num_interp; i++) {
            info.hw.fs.inputs[i] = {
                .param_index = u8(ps_inputs[i].input_offset),
                .is_default = bool(ps_inputs[i].use_default),
                .is_flat = bool(ps_inputs[i].flat_shade),
                .default_value = u8(ps_inputs[i].default_value),
            };
        }
        for (u32 i = 0; i < Shader::MaxColorBuffers; i++) {
            info.hw.fs.color_buffers[i] = sel.graphics_key.color_buffers[i];
        }
        // Lowered user clip planes ride the same emulation path as guest-exported distances, so
        // the fragment side arms whenever the hardware vertex stage lowers them, keeping its input
        // locations in sync with the shifted vertex outputs.
        const bool lowers_user_clip_planes =
            regs.clipper_control.user_clip_plane_enable &&
            !regs.stage_enable.IsStageEnabled(static_cast<u32>(HwStage::Geometry));
        info.hw.fs.clip_distance_emulation =
            ((regs.vs_output_control.clip_distance_enable &&
              !regs.stage_enable.IsStageEnabled(static_cast<u32>(HwStage::Local))) ||
             lowers_user_clip_planes) &&
            profile.needs_clip_distance_emulation;
        break;
    }
    case HwStage::Compute: {
        const auto& cs_pgm = liverpool->GetCsRegs();
        info.props.num_user_data = cs_pgm.settings.num_user_regs;
        info.props.num_allocated_vgprs = cs_pgm.settings.num_vgprs * 4;
        info.props.fp_denorm_mode32 = cs_pgm.settings.fp_denorm_mode32;
        info.props.fp_denorm_mode16_64 = cs_pgm.settings.fp_denorm_mode64;
        info.props.fp_round_mode32 = cs_pgm.settings.fp_round_mode32;
        info.props.fp_round_mode16_64 = cs_pgm.settings.fp_round_mode64;
        info.hw.cs.workgroup_size = {cs_pgm.num_thread_x.full, cs_pgm.num_thread_y.full,
                                     cs_pgm.num_thread_z.full};
        info.hw.cs.tgid_enable = {cs_pgm.IsTgidEnabled(0), cs_pgm.IsTgidEnabled(1),
                                  cs_pgm.IsTgidEnabled(2)};
        info.hw.cs.shared_memory_size = cs_pgm.SharedMemSize();
        break;
    }
    default:
        break;
    }
    switch (l_stage) {
    case SwStage::Vertex:
        info.sw.vs.step_rate_0 = regs.vgt_instance_step_rate_0;
        info.sw.vs.step_rate_1 = regs.vgt_instance_step_rate_1;
        info.sw.vs.vertex_sgpr_offset = sel.draw_indirect_params.vertex_sgpr_offset;
        info.sw.vs.instance_sgpr_offset = sel.draw_indirect_params.instance_sgpr_offset;
        info.sw.vs.tess_emulated_primitive =
            regs.primitive_type == AmdGpu::PrimitiveType::RectList ||
            regs.primitive_type == AmdGpu::PrimitiveType::QuadList;
        break;
    case SwStage::TessellationControl: {
        info.sw.tcs.num_input_control_points = regs.ls_hs_config.hs_input_control_points;
        info.sw.tcs.num_threads = regs.ls_hs_config.hs_output_control_points;
        info.sw.tcs.tess_type = regs.tess_config.type;
        info.sw.tcs.offchip_lds_enable = regs.hs_program.settings.oc_lds_en;
        break;
    }
    case SwStage::TessellationEval: {
        info.sw.tes.tess_type = regs.tess_config.type;
        info.sw.tes.tess_topology = regs.tess_config.topology;
        info.sw.tes.tess_partitioning = regs.tess_config.partitioning;
        break;
    }
    default:
        break;
    }
    return info;
}

PipelineCache::PipelineCache(const Instance& instance_, Scheduler& scheduler_,
                             AmdGpu::Liverpool* liverpool_, u32 sparse_page_shift)
    : instance{instance_}, scheduler{scheduler_}, liverpool{liverpool_},
      desc_heap{instance, scheduler.GetWorkSemaphore(), DescriptorHeapSizes, DescriptorHeapSets} {
    sel.regs = &liverpool->regs;
    const auto& vk12_props = instance.GetVk12Properties();
    profile = Shader::Profile{
        .max_viewport_width = instance.GetMaxViewportWidth(),
        .max_viewport_height = instance.GetMaxViewportHeight(),
        .max_shared_memory_size = instance.MaxComputeSharedMemorySize(),
        .supported_spirv = SpirvVersion1_6,
        .subgroup_size = instance.SubgroupSize(),
        .sparse_page_shift = sparse_page_shift,
        .support_int8 = instance.IsShaderInt8Supported(),
        .support_int16 = instance.IsShaderInt16Supported(),
        .support_int64 = instance.IsShaderInt64Supported(),
        .support_float16 = instance.IsShaderFloat16Supported(),
        .support_float64 = instance.IsShaderFloat64Supported(),
        .supports_denorm_behavior_independence =
            vk12_props.denormBehaviorIndependence != vk::ShaderFloatControlsIndependence::eNone,
        .supports_rounding_mode_independence =
            vk12_props.roundingModeIndependence != vk::ShaderFloatControlsIndependence::eNone,
        .support_fp16_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat16),
        .support_fp16_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat16),
        .support_fp16_round_to_zero = bool(vk12_props.shaderRoundingModeRTZFloat16),
        .support_fp32_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat32),
        .support_fp32_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat32),
        .support_fp32_round_to_zero = bool(vk12_props.shaderRoundingModeRTZFloat32),
        .support_fp64_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat64),
        .support_fp64_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat64),
        .support_fp64_round_to_zero = bool(vk12_props.shaderRoundingModeRTZFloat64),
        .support_fp16_signed_zero_inf_nan_preserve =
            bool(vk12_props.shaderSignedZeroInfNanPreserveFloat16),
        .support_fp32_signed_zero_inf_nan_preserve =
            bool(vk12_props.shaderSignedZeroInfNanPreserveFloat32),
        .support_fp64_signed_zero_inf_nan_preserve =
            bool(vk12_props.shaderSignedZeroInfNanPreserveFloat64),
        .supports_image_load_store_lod = instance_.IsImageLoadStoreLodSupported(),
        .supports_native_cube_calc = instance_.IsAmdGcnShaderSupported(),
        .supports_trinary_minmax = instance_.IsAmdShaderTrinaryMinMaxSupported(),
        .supports_buffer_fp32_atomic_min_max =
            instance_.IsShaderAtomicFloatBuffer32MinMaxSupported(),
        .supports_image_fp32_atomic_min_max = instance_.IsShaderAtomicFloatImage32MinMaxSupported(),
        .supports_buffer_int64_atomics = instance_.IsBufferInt64AtomicsSupported(),
        .supports_shared_int64_atomics = instance_.IsSharedInt64AtomicsSupported(),
        .supports_workgroup_explicit_memory_layout =
            instance_.IsWorkgroupMemoryExplicitLayoutSupported(),
        .supports_amd_shader_explicit_vertex_parameter =
            instance_.IsAmdShaderExplicitVertexParameterSupported(),
        .supports_fragment_shader_barycentric = instance_.IsFragmentShaderBarycentricSupported(),
        .supports_shader_subgroup_clock = instance_.IsShaderSubgroupClockSupported(),
        .needs_manual_interpolation = instance.IsFragmentShaderBarycentricSupported() &&
                                      instance.GetDriverID() == vk::DriverId::eNvidiaProprietary,
        // bbport: older NVIDIA (Pascal) has no barycentrics; BB_INTERP_INT_FIX=0/1 overrides.
        .needs_integer_interpolation_fix = [&] {
            if (const char* env = std::getenv("BB_INTERP_INT_FIX")) {
                return env[0] == '1';
            }
            return !instance.IsFragmentShaderBarycentricSupported() &&
                   instance.GetDriverID() == vk::DriverId::eNvidiaProprietary;
        }(),
        .needs_lds_barriers = instance.GetDriverID() == vk::DriverId::eNvidiaProprietary ||
                              instance.GetDriverID() == vk::DriverId::eMesaKosmickrisp,
        .needs_buffer_offsets = instance.StorageMinAlignment() > 4,
        .needs_unorm_fixup = instance.GetDriverID() == vk::DriverId::eMesaKosmickrisp,
        .needs_clip_distance_emulation = instance.GetDriverID() == vk::DriverId::eNvidiaProprietary,
        .supports_shader_stencil_export = instance_.IsShaderStencilExportSupported(),
    };
    LoadPipelineCache();
    // Preloaded pipelines must use the same driver cache as runtime pipelines.
    WarmUp();
}

PipelineCache::~PipelineCache() {
    compile_queue.WaitIdle();
    SavePipelineCache();
}

bool PipelineCache::PrepareGraphicsPipeline(PipelineSelection& worker_sel) {
    // Tessellation stages read constant buffers from memory at selection time: not prepared.
    if (worker_sel.regs->stage_enable.hs_en) {
        return false;
    }
    if (!RefreshGraphicsKey(worker_sel) || worker_sel.worker->failed) {
        return false;
    }
    RequestGraphicsPipeline(worker_sel);
    return true;
}

const GraphicsPipeline* PipelineCache::TryPreparedPipeline(const PreparedDraw& prepared) {
    if (prepared.state.load(std::memory_order_acquire) != PreparedDraw::Ready ||
        prepared.reg_checksum != liverpool->gfx_reg_checksum) {
        return nullptr;
    }
    // Same registers; the stage programs and their flattened user data (which also covers
    // everything read from guest memory for the specialization) must match as well. This is
    // the per-stage work GetProgram does on the regular path.
    const auto& regs = liverpool->regs;
    for (u32 i = 0; i < prepared.num_stages; ++i) {
        const auto& stage = prepared.stages[i];
        const auto* pgm = regs.ProgramForStage(static_cast<u32>(stage.hw_stage));
        if (!pgm || !pgm->Address<u32*>()) {
            return nullptr;
        }
        const auto params = AmdGpu::GetParams(*pgm);
        if (params.hash != stage.hash) {
            return nullptr;
        }
        auto& info = const_cast<Program*>(stage.program)->info;
        info.pgm_base = params.Base();
        info.user_data = params.user_data;
        info.RefreshFlatBuf();
        if (info.pgm_base != stage.pgm_base || info.flattened_ud_buf.size() != stage.flat_size ||
            std::memcmp(info.flattened_ud_buf.data(), stage.flat,
                        stage.flat_size * sizeof(u32)) != 0) {
            return nullptr;
        }
    }
    std::shared_lock lk{pipelines_mutex};
    const auto it = graphics_pipelines.find(prepared.key);
    return it != graphics_pipelines.end() ? it->second.get() : nullptr;
}

const GraphicsPipeline* PipelineCache::GetGraphicsPipeline(const DrawIndirectParams params,
                                                           const PreparedDraw* prepared) {
    used_prepared = nullptr;
    if (prepared) {
        if (const auto* pipeline = TryPreparedPipeline(*prepared)) {
            used_prepared = prepared;
            return pipeline;
        }
    }
    sel.draw_indirect_params = params;
    if (!RefreshGraphicsKey(sel)) {
        return nullptr;
    }
    const u64 pipeline_hash = std::hash<GraphicsPipelineKey>{}(sel.graphics_key);
    {
        std::shared_lock lk{pipelines_mutex};
        const auto found = graphics_pipelines.find(sel.graphics_key);
        if (found != graphics_pipelines.end()) {
            return found->second.get();
        }
    }
    if (compile_queue.HasPending(pipeline_hash)) {
        GpuCompileWaitTimer wait;
        compile_queue.Wait(pipeline_hash);
    }
    {
        std::shared_lock lk{pipelines_mutex};
        const auto found = graphics_pipelines.find(sel.graphics_key);
        if (found != graphics_pipelines.end()) {
            return found->second.get();
        }
    }
    return CreateGraphicsPipeline(sel, false);
}

const ComputePipeline* PipelineCache::GetComputePipeline() {
    if (!RefreshComputeKey()) {
        return nullptr;
    }
    const auto pipeline_hash = std::hash<ComputePipelineKey>{}(compute_key);
    {
        std::shared_lock lk{pipelines_mutex};
        const auto found = compute_pipelines.find(compute_key);
        if (found != compute_pipelines.end()) {
            return found->second.get();
        }
    }
    if (!compile_queue.HasPending(pipeline_hash)) {
        RequestComputePipeline();
    }
    if (compile_queue.HasPending(pipeline_hash)) {
        GpuCompileWaitTimer wait;
        compile_queue.Wait(pipeline_hash);
    }
    {
        std::shared_lock lk{pipelines_mutex};
        const auto found = compute_pipelines.find(compute_key);
        if (found != compute_pipelines.end()) {
            return found->second.get();
        }
    }
    return CreateComputePipeline(compute_key, sel.infos[0], sel.modules[0]);
}

bool PipelineCache::RefreshGraphicsKey(PipelineSelection& sel) {
    std::memset(&sel.graphics_key, 0, sizeof(GraphicsPipelineKey));
    const auto& regs = (*sel.regs);
    auto& key = sel.graphics_key;

    const bool db_enabled = regs.depth_buffer.DepthValid() || regs.depth_buffer.StencilValid();

    key.z_format = regs.depth_buffer.DepthValid() ? regs.depth_buffer.z_info.format
                                                  : AmdGpu::DepthBuffer::ZFormat::Invalid;
    key.stencil_format = regs.depth_buffer.StencilValid()
                             ? regs.depth_buffer.stencil_info.format
                             : AmdGpu::DepthBuffer::StencilFormat::Invalid;
    key.depth_clamp_enable = !regs.depth_render_override.disable_viewport_clamp;
    key.depth_clip_enable = regs.clipper_control.ZclipEnable();
    key.clip_space = regs.clipper_control.clip_space;
    key.provoking_vtx_last = regs.polygon_control.provoking_vtx_last;
    key.prim_type = regs.primitive_type;
    key.polygon_mode = regs.polygon_control.PolyMode();
    key.patch_control_points =
        regs.stage_enable.hs_en ? regs.ls_hs_config.hs_input_control_points : 0;
    key.logic_op = regs.color_control.rop3;
    key.depth_samples = db_enabled ? regs.depth_buffer.NumSamples() : 1;
    key.num_samples = key.depth_samples;
    key.cb_shader_mask = regs.color_shader_mask;

    const bool skip_cb_binding =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;

    // Only potentially animated G-buffer draws may use the extra motion attachment. The
    // ordinary stage variant is needed first to inspect the vertex shader's resources.
    bool motion_possible = false;
    sel.remix_capture = false;
    sel.remix_uv = 0xffff;
    {
        u32 bound = 0;
        for (s32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS && !skip_cb_binding; ++cb) {
            bound += (regs.color_buffers[cb] && regs.color_target_mask.GetMask(cb)) ? 1 : 0;
        }
        motion_possible = Shader::MotionVectors::positions_address != 0 && bound >= 5 &&
                          !regs.color_buffers[Shader::MotionVectors::Output] &&
                          regs.depth_buffer.DepthValid() &&
                          regs.depth_buffer.NumSamples() == 1 && !regs.IsClipDisabled() &&
                          regs.stage_enable.raw == AmdGpu::ShaderStageEnable::VgtStages::Vs;
    }
    sel.motion = false;

    // First pass to fill render target information needed by shader recompiler
    for (s32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS && !skip_cb_binding; ++cb) {
        const auto& col_buf = regs.color_buffers[cb];
        if (!col_buf || !regs.color_target_mask.GetMask(cb)) {
            // No attachment bound or writing to it is disabled.
            continue;
        }

        // Fill color target information
        auto& color_buffer = key.color_buffers[cb];
        color_buffer.data_format = col_buf.GetDataFmt();
        color_buffer.num_format = col_buf.GetNumberFmt();
        color_buffer.num_conversion = col_buf.GetNumberConversion();
        color_buffer.export_format = regs.color_export_format.GetFormat(cb);
        color_buffer.swizzle = col_buf.Swizzle();

        const auto& bc = regs.blend_control[cb];
        color_buffer.blend_self_scale =
            bc.enable && !col_buf.info.blend_bypass &&
            (bc.color_func == AmdGpu::BlendControl::BlendFunc::Min ||
             bc.color_func == AmdGpu::BlendControl::BlendFunc::Max) &&
            bc.color_src_factor == AmdGpu::BlendControl::BlendFactor::SrcColor &&
            bc.color_dst_factor == AmdGpu::BlendControl::BlendFactor::DstColor;
    }

    // Compile and bind shader stages
    if (!RefreshGraphicsStages(sel)) {
        return false;
    }

    if (Shader::RemixCapture::enabled && regs.depth_buffer.DepthValid() &&
        !regs.IsClipDisabled() && std::popcount(key.mrt_mask) >= 5 &&
        regs.stage_enable.raw == AmdGpu::ShaderStageEnable::VgtStages::Vs) {
        const auto* vs = sel.infos[u32(Shader::SwStage::Vertex)];
        const auto* ps = sel.infos[u32(Shader::SwStage::Fragment)];
        if (vs && ps && !vs->stores.GetAny(Shader::IR::Attribute::ClipDistance) &&
            !vs->stores.GetAny(Shader::IR::Attribute::Param0 + Shader::RemixCapture::UvLocation)) {
            sel.remix_capture = true;
            // Prefer actual sRGB albedo sampling, and retain the vertex varying it uses.
            for (const auto& image : ps->images) {
                if (image.remix_uv == 0xffff || image.is_depth || image.is_written) continue;
                const auto sharp = image.GetSharp(*ps);
                if (sharp.GetNumberFmt() != AmdGpu::NumberFormat::Srgb ||
                    sharp.GetType() != AmdGpu::ImageType::Color2D) continue;
                const u32 input = image.remix_uv / 4;
                if (input >= regs.num_interp || regs.ps_inputs[input].use_default) continue;
                sel.remix_uv = u16(regs.ps_inputs[input].input_offset * 4 + image.remix_uv % 4);
                break;
            }
            if (!RefreshGraphicsStages(sel)) return false;
        }
    }
    key.remix_capture = sel.remix_capture;
    key.remix_uv = sel.remix_uv;
    if (motion_possible) {
        const auto* vs = sel.infos[static_cast<u32>(Shader::SwStage::Vertex)];
        if (vs) {
            // Shaders with a bone palette (motion_history.h). Small skeletons (weapons,
            // props) also include static world pieces: the rasterizer gives those
            // history only while their constants change.
            for (const auto& resource : vs->buffers) {
                if (resource.IsSpecial()) continue;
                const auto buffer = resource.GetSharp(*vs);
                if (buffer.Valid() && buffer.GetStride() == 16 &&
                    Motion::ClassifyBuffer(buffer.GetSize()) != Motion::BufferRole::Other) {
                    sel.motion = true;
                    break;
                }
            }
        }
        // BB_MOTION_SELECT_LOG=1: each G-buffer vertex shader once, with its buffer sizes
        // and the selection, to find animated models that the size rule leaves out.
        static const bool select_log = [] {
            const char* value = std::getenv("BB_MOTION_SELECT_LOG");
            return value && value[0] == '1';
        }();
        if (select_log && vs) {
            std::string sizes;
            for (const auto& resource : vs->buffers) {
                if (resource.IsSpecial()) continue;
                const auto buffer = resource.GetSharp(*vs);
                sizes += fmt::format(" {}/{}", buffer.GetSize(), buffer.GetStride());
            }
            static std::mutex log_mutex;
            static std::unordered_set<u64> logged;
            std::scoped_lock lock{log_mutex};
            if (logged.insert(vs->pgm_hash ^ std::hash<std::string>{}(sizes)).second) {
                std::printf("Motion select: vs %016llx %s, buffers (size/stride):%s\n",
                            static_cast<unsigned long long>(vs->pgm_hash),
                            sel.motion ? "ON " : "off", sizes.c_str());
            }
        }
        // Keep the old broad path available for visual A/B tests.
        static const bool all_motion = [] {
            const char* value = std::getenv("BB_OBJECT_MOTION_ALL");
            return value && value[0] == '1';
        }();
        if (all_motion) {
            sel.motion = true;
        }
        if (sel.motion && !RefreshGraphicsStages(sel)) {
            return false;
        }
    }

    // Second pass to mask out render targets not written by shader and fill remaining info
    u8 color_samples = 0;
    bool all_color_samples_same = true;
    for (s32 cb = 0; cb < key.num_color_attachments && !skip_cb_binding; ++cb) {
        const auto& col_buf = regs.color_buffers[cb];
        const u32 target_mask = regs.color_target_mask.GetMask(cb);
        if (!col_buf || !target_mask) {
            continue;
        }
        if ((key.mrt_mask & (1u << cb)) == 0) {
            std::memset(&key.color_buffers[cb], 0, sizeof(Shader::PsColorBuffer));
            continue;
        }

        // Fill color blending information
        if (regs.blend_control[cb].enable && !col_buf.info.blend_bypass) {
            key.blend_controls[cb] = regs.blend_control[cb];
        }

        // Apply swizzle to target mask
        key.write_masks[cb] =
            vk::ColorComponentFlags{key.color_buffers[cb].swizzle.ApplyMask(target_mask)};

        // Fill color samples
        const u8 prev_color_samples = std::exchange(color_samples, col_buf.NumSamples());
        all_color_samples_same &= color_samples == prev_color_samples || prev_color_samples == 0;
        key.color_samples[cb] = color_samples;
        key.num_samples = std::max(key.num_samples, color_samples);
    }

    if (sel.motion) {
        constexpr u32 mv = Shader::MotionVectors::Output;
        key.motion_vectors = 1;
        key.mrt_mask |= 1u << mv;
        key.num_color_attachments = mv + 1;
        auto& color_buffer = key.color_buffers[mv];
        color_buffer.data_format = AmdGpu::DataFormat::Format32_32_32_32;
        color_buffer.num_format = AmdGpu::NumberFormat::Float;
        color_buffer.swizzle = AmdGpu::IdentityMapping;
        key.write_masks[mv] = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                              vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
        key.color_samples[mv] = 1;
    }

    // Force all color samples to match depth samples to avoid unsupported MSAA configuration
    if (color_samples != 0) {
        const bool depth_mismatch = db_enabled && color_samples != key.depth_samples;
        if (!all_color_samples_same && !instance.IsMixedAnySamplesSupported() ||
            all_color_samples_same && depth_mismatch && !instance.IsMixedDepthSamplesSupported()) {
            key.color_samples.fill(key.depth_samples);
            key.num_samples = key.depth_samples;
        }
    }

    return true;
}

bool PipelineCache::RefreshGraphicsStages(PipelineSelection& sel) {
    const auto& regs = (*sel.regs);
    auto& key = sel.graphics_key;
    sel.fetch_shader = std::nullopt;

    Shader::Backend::Bindings binding{};
    const auto bind_stage = [&](HwStage stage_in, SwStage stage_out) -> bool {
        const auto stage_in_idx = static_cast<u32>(stage_in);
        const auto stage_out_idx = static_cast<u32>(stage_out);
        if (!regs.stage_enable.IsStageEnabled(stage_in_idx)) {
            key.stage_hashes[stage_out_idx] = 0;
            sel.infos[stage_out_idx] = nullptr;
            return false;
        }

        const auto* pgm = regs.ProgramForStage(stage_in_idx);
        if (!pgm || !pgm->Address<u32*>()) {
            key.stage_hashes[stage_out_idx] = 0;
            sel.infos[stage_out_idx] = nullptr;
            return false;
        }

        const auto params = AmdGpu::GetParams(*pgm);
        binding.set = stage_out == SwStage::Fragment ? Shader::Backend::FragmentSet
                                                     : Shader::Backend::PreRasterSet;
        std::optional<Shader::Gcn::FetchShaderData> fetch_shader_;
        std::tie(sel.infos[stage_out_idx], sel.modules[stage_out_idx], fetch_shader_,
                 key.stage_hashes[stage_out_idx]) =
            GetProgram(sel, stage_in, stage_out, params, binding);
        if (fetch_shader_) {
            sel.fetch_shader = fetch_shader_;
        }
        return true;
    };

    sel.infos.fill(nullptr);
    sel.modules.fill(nullptr);

    bind_stage(HwStage::Fragment, SwStage::Fragment);

    const auto* fs_info = sel.infos[static_cast<u32>(SwStage::Fragment)];
    key.mrt_mask = fs_info ? fs_info->mrt_mask : 0u;
    key.num_color_attachments = std::bit_width(key.mrt_mask);

    switch (regs.stage_enable.raw) {
    case AmdGpu::ShaderStageEnable::VgtStages::EsGs:
        if (!instance.IsGeometryStageSupported()) {
            LOG_WARNING(Render_Vulkan, "Geometry shader stage unsupported, skipping");
            return false;
        }
        if (regs.vgt_gs_mode.onchip || regs.vgt_strmout_config.raw) {
            LOG_WARNING(Render_Vulkan, "Geometry shader features unsupported, skipping");
            return false;
        }
        if (!bind_stage(HwStage::Export, SwStage::Vertex)) {
            return false;
        }
        if (!bind_stage(HwStage::Geometry, SwStage::Geometry)) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::LsHs:
        if (!instance.IsTessellationSupported()) {
            return false;
        }
        if (!bind_stage(HwStage::Hull, SwStage::TessellationControl)) {
            return false;
        }
        if (!bind_stage(HwStage::Vertex, SwStage::TessellationEval)) {
            return false;
        }
        if (!bind_stage(HwStage::Local, SwStage::Vertex)) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::LsHsEsGs:
        if (!instance.IsTessellationSupported()) {
            return false;
        }
        if (!instance.IsGeometryStageSupported()) {
            LOG_WARNING(Render_Vulkan, "Geometry shader stage unsupported, skipping");
            return false;
        }
        if (regs.vgt_gs_mode.onchip || regs.vgt_strmout_config.raw) {
            LOG_WARNING(Render_Vulkan, "Geometry shader features unsupported, skipping");
            return false;
        }
        if (!bind_stage(HwStage::Hull, SwStage::TessellationControl)) {
            return false;
        }
        if (!bind_stage(HwStage::Export, SwStage::TessellationEval)) {
            return false;
        }
        if (!bind_stage(HwStage::Local, SwStage::Vertex)) {
            return false;
        }
        if (!bind_stage(HwStage::Geometry, SwStage::Geometry)) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::Vs:
        bind_stage(HwStage::Vertex, SwStage::Vertex);
        break;
    default:
        LOG_WARNING(Render_Vulkan, "unimplemented shader stage {}", (u32)regs.stage_enable.raw);
        return false;
    }

    const auto* vs_info = sel.infos[static_cast<u32>(SwStage::Vertex)];
    if (vs_info && sel.fetch_shader && !instance.IsVertexInputDynamicState()) {
        // Without vertex input dynamic state, the pipeline needs to specialize on format.
        // Stride will still be handled outside the pipeline using dynamic state.
        u32 vertex_binding = 0;
        for (const auto& attrib : sel.fetch_shader->attributes) {
            const auto& buffer = attrib.GetSharp(*vs_info);
            ASSERT_MSG(vertex_binding < MaxVertexBufferCount,
                       "Vertex attribute binding count exceeded limit: {} >= {}", vertex_binding,
                       MaxVertexBufferCount);
            key.vertex_buffer_formats[vertex_binding++] =
                Vulkan::LiverpoolToVK::SurfaceFormat(buffer.GetDataFmt(), buffer.GetNumberFmt());
        }
    }

    return true;
}

bool PipelineCache::RefreshComputeKey() {
    Shader::Backend::Bindings binding{};
    binding.set = 0;
    const auto& cs_pgm = liverpool->GetCsRegs();
    const auto cs_params = AmdGpu::GetParams(cs_pgm);
    std::tie(sel.infos[0], sel.modules[0], sel.fetch_shader, compute_key.value) =
        GetProgram(sel, HwStage::Compute, SwStage::Compute, cs_params, binding);
    return true;
}

vk::ShaderModule PipelineCache::CompileModule(Shader::Info& info, Shader::RuntimeInfo& runtime_info,
                                              const std::span<const u32>& code, size_t perm_idx,
                                              Shader::Backend::Bindings& binding,
                                              std::vector<u32>& spv_out) {
    LOG_INFO(Render_Vulkan, "Compiling {} shader {:#x} {}", info.hw_stage, info.pgm_hash,
             perm_idx != 0 ? "(permutation)" : "");
    DumpShader(code, info.pgm_hash, info.hw_stage, perm_idx, "bin");
    CompileTimer timer{CompileKind::Translate};
    if (info.hw_stage == HwStage::Vertex && runtime_info.hw.vs.motion_vectors) {
        ++g_bb_motion_compiles;
    }

    thread_local Shader::Pools tls_pools;
    tls_pools.ReleaseContents();
    const auto ir_program = Shader::TranslateProgram(code, tls_pools, info, runtime_info, profile);
    auto spv = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, ir_program, binding);
    DumpShader(spv, info.pgm_hash, info.hw_stage, perm_idx, "spv");

    vk::ShaderModule module;

    auto patch = GetShaderPatch(info.pgm_hash, info.hw_stage, perm_idx, "spv");
    const bool is_patched = patch && EmulatorSettings.IsPatchShaders();
    if (is_patched) {
        LOG_INFO(Loader, "Loaded patch for {} shader {:#x}", info.hw_stage, info.pgm_hash);
        module = CompileSPV(*patch, instance.GetDevice());
    } else {
        module = CompileSPV(spv, instance.GetDevice());
    }

    const auto name = GetShaderName(info.hw_stage, info.pgm_hash, perm_idx);
    Vulkan::SetObjectName(instance.GetDevice(), module, name);
    if (EmulatorSettings.IsShaderCollect()) {
        DebugState.CollectShader(name, info.sw_stage, module, spv, code,
                                 patch ? *patch : std::span<const u32>{}, is_patched);
    }
    spv_out = std::move(spv);
    return module;
}

std::pair<size_t, vk::ShaderModule> PipelineCache::PublishPermut(
    Program& program, const Shader::Info& meta_info, vk::ShaderModule module,
    Shader::StageSpecialization&& spec, std::vector<u32>&& spv) {
    spec.info = &program.info;
    std::unique_lock lk{programs_mutex};
    const auto [idx, inserted] = program.AddPermut(module, std::move(spec));
    if (!inserted) {
        lk.unlock();
        instance.GetDevice().destroyShaderModule(module);
        std::shared_lock rlk{programs_mutex};
        return {idx, program.modules[idx].module};
    }
    const auto& added = program.modules[idx];
    RegisterShaderMeta(meta_info, added.spec.fetch_shader_data, added.spec,
                       HashCombine(program.info.pgm_hash, idx), idx);
    RegisterShaderBinary(std::move(spv), program.info.pgm_hash, idx);
    return {idx, module};
}

std::pair<Program*, bool> PipelineCache::PublishProgram(std::unique_ptr<Program>& program,
                                                        std::vector<u32>&& spv) {
    const u64 hash = program->info.pgm_hash;
    std::unique_lock lk{programs_mutex};
    if (auto it = program_cache.find(hash); it != program_cache.end()) {
        return {it.value().get(), false};
    }
    program->info_template = std::make_unique<Shader::Info>(program->info);
    const auto& first = program->modules[0];
    RegisterShaderMeta(program->info, first.spec.fetch_shader_data, first.spec,
                       HashCombine(hash, 0), 0);
    RegisterShaderBinary(std::move(spv), hash, 0);
    Program* raw = program.get();
    program_cache.emplace(hash, std::move(program));
    return {raw, true};
}

PipelineCache::Result PipelineCache::GetProgram(PipelineSelection& sel, HwStage hw_stage,
                                                SwStage sw_stage,
                                                const Shader::ShaderParams& params,
                                                Shader::Backend::Bindings& binding) {
    auto runtime_info = BuildRuntimeInfo(sel, hw_stage, sw_stage);
    if (sel.worker) {
        // bbport: draw-preparation worker: look up only, with the worker's own Info copy.
        auto& worker = *sel.worker;
        const Program* program = nullptr;
        {
            std::shared_lock lk{programs_mutex};
            const auto found_program = program_cache.find(params.hash);
            if (found_program != program_cache.end() && found_program->second->info_template) {
                program = found_program->second.get();
            }
        }
        if (!program) {
            sigjmp_buf recover;
            if (sigsetjmp(recover, 0)) {
                runtime_fault_recover = nullptr;
                worker.failed = true;
                return {};
            }
            runtime_fault_recover = &recover;
            RequestProgramCompile(sel, hw_stage, sw_stage, params, binding, params.hash);
            runtime_fault_recover = nullptr;
            // Workers run ahead of the GPU thread: waiting for the translation here lets this
            // draw still queue its pipeline, created in parallel before the GPU thread needs it
            // instead of inline (one by one) when it gets there.
            if (!compile_queue.WaitFor(params.hash, WorkerCompileWait)) {
                worker.failed = true;
                return {};
            }
            std::shared_lock lk{programs_mutex};
            const auto found_program = program_cache.find(params.hash);
            if (found_program == program_cache.end() || !found_program->second->info_template) {
                worker.failed = true;
                return {};
            }
            program = found_program->second.get();
        }
        auto [it_info, new_info] = worker.infos.try_emplace(program, *program->info_template);
        auto& info = it_info->second;
        info.pgm_base = params.Base();
        info.user_data = params.user_data;
        // The walk of resource tables and the fetch shader parse read guest memory through
        // pointers in the registers; ahead of the GPU thread that memory may already be
        // reused. A fault returns here (runtime_fault_recover) and the draw is left to the GPU
        // thread. A jump out of the specialization leaks its partial allocations (rare).
        sigjmp_buf recover;
        if (sigsetjmp(recover, 0)) {
            worker.failed = true;
            return {};
        }
        runtime_fault_recover = &recover;
        info.RefreshFlatBuf();
        auto spec = Shader::StageSpecialization(info, runtime_info, profile, binding);
        runtime_fault_recover = nullptr;
        std::optional<size_t> found;
        vk::ShaderModule found_module{};
        std::optional<Shader::Gcn::FetchShaderData> found_fetch;
        {
            std::shared_lock lk{programs_mutex};
            found = program->FindPermut(spec);
            if (found) {
                found_module = program->modules[*found].module;
                found_fetch = program->modules[*found].spec.fetch_shader_data;
            }
        }
        if (!found) {
            const u64 perm_key =
                HashCombine(params.hash, XXH3_64bits(params.user_data.data(),
                                                     params.user_data.size_bytes()));
            RequestProgramCompile(sel, hw_stage, sw_stage, params, binding, perm_key);
            if (compile_queue.WaitFor(perm_key, WorkerCompileWait)) {
                std::shared_lock lk{programs_mutex};
                found = program->FindPermut(spec);
                if (found) {
                    found_module = program->modules[*found].module;
                    found_fetch = program->modules[*found].spec.fetch_shader_data;
                }
            }
        }
        if (!found) {
            worker.failed = true;
            return {};
        }
        info.AddBindings(binding);
        worker.stages.push_back(
            {program, params.hash, hw_stage, info.pgm_base, &info.flattened_ud_buf});
        return std::make_tuple(&info, found_module, std::move(found_fetch),
                               HashCombine(params.hash, *found));
    }

    const u64 perm_key =
        HashCombine(params.hash, XXH3_64bits(params.user_data.data(), params.user_data.size_bytes()));
    if (compile_queue.HasPending(params.hash)) {
        GpuCompileWaitTimer wait;
        compile_queue.Wait(params.hash);
    }
    if (compile_queue.HasPending(perm_key)) {
        GpuCompileWaitTimer wait;
        compile_queue.Wait(perm_key);
    }
    Program* program = nullptr;
    {
        std::shared_lock lk{programs_mutex};
        if (auto it = program_cache.find(params.hash); it != program_cache.end()) {
            program = it.value().get();
        }
    }
    if (!program) {
        auto new_program = std::make_unique<Program>(hw_stage, sw_stage, params);
        auto start = binding;
        std::vector<u32> spv;
        const auto module =
            CompileModule(new_program->info, runtime_info, params.code, 0, binding, spv);
        auto spec = Shader::StageSpecialization(new_program->info, runtime_info, profile, start);
        auto fetch = spec.fetch_shader_data;
        new_program->AddPermut(module, std::move(spec));
        const auto [published, inserted] = PublishProgram(new_program, std::move(spv));
        if (inserted) {
            return std::make_tuple(&published->info, module, std::move(fetch),
                                   HashCombine(params.hash, 0));
        }
        // A compile worker inserted this program meanwhile: ours becomes one of its
        // permutations (or is dropped if equal) and the lookup below picks it up.
        auto& mine = new_program->modules[0];
        PublishPermut(*published, new_program->info, mine.module, std::move(mine.spec),
                      std::move(spv));
        program = published;
        binding = start;
    }

    if (!program->info_template) {
        // Programs loaded by the pipeline cache warm-up get their template on first use.
        std::unique_lock lk{programs_mutex};
        if (!program->info_template) {
            program->info_template = std::make_unique<Shader::Info>(program->info);
        }
    }
    auto& info = program->info;
    info.pgm_base = params.Base(); // Needs to be actualized for inline cbuffer address fixup
    info.user_data = params.user_data;
    info.RefreshFlatBuf();
    auto spec = Shader::StageSpecialization(info, runtime_info, profile, binding);

    std::optional<size_t> perm_idx;
    vk::ShaderModule module{};
    std::optional<Shader::Gcn::FetchShaderData> fetch;
    size_t num_modules = 0;
    {
        std::shared_lock lk{programs_mutex};
        // bbport: consecutive draws of a program almost always use the same permutation.
        const size_t last = program->last_used;
        if (last < program->modules.size() && program->modules[last].module &&
            program->modules[last].spec == spec) {
            perm_idx = last;
        } else {
            perm_idx = program->FindPermut(spec);
        }
        if (perm_idx) {
            module = program->modules[*perm_idx].module;
            fetch = program->modules[*perm_idx].spec.fetch_shader_data;
        }
        num_modules = program->modules.size();
    }
    if (perm_idx) {
        info.AddBindings(binding);
    } else {
        auto new_info = Shader::Info(hw_stage, sw_stage, params);
        std::vector<u32> spv;
        module = CompileModule(new_info, runtime_info, params.code, num_modules, binding, spv);
        fetch = spec.fetch_shader_data;
        std::tie(perm_idx, module) =
            PublishPermut(*program, info, module, std::move(spec), std::move(spv));
    }
    program->last_used = *perm_idx;
    return std::make_tuple(&program->info, module, std::move(fetch),
                           HashCombine(params.hash, *perm_idx));
}

std::optional<vk::ShaderModule> PipelineCache::ReplaceShader(vk::ShaderModule module,
                                                             std::span<const u32> spv_code) {
    std::optional<vk::ShaderModule> new_module{};
    for (const auto& [_, program] : program_cache) {
        for (auto& m : program->modules) {
            if (m.module == module) {
                const auto& d = instance.GetDevice();
                d.destroyShaderModule(m.module);
                m.module = CompileSPV(spv_code, d);
                new_module = m.module;
            }
        }
    }
    if (module_related_pipelines.contains(module)) {
        auto& pipeline_keys = module_related_pipelines[module];
        for (auto& key : pipeline_keys) {
            if (std::holds_alternative<GraphicsPipelineKey>(key)) {
                auto& graphics_key = std::get<GraphicsPipelineKey>(key);
                graphics_pipelines.erase(graphics_key);
            } else if (std::holds_alternative<ComputePipelineKey>(key)) {
                auto& compute_key = std::get<ComputePipelineKey>(key);
                compute_pipelines.erase(compute_key);
            }
        }
    }
    return new_module;
}

std::string PipelineCache::GetShaderName(Shader::HwStage stage, u64 hash,
                                         std::optional<size_t> perm) {
    if (perm) {
        return fmt::format("{}_{:#018x}_{}", stage, hash, *perm);
    }
    return fmt::format("{}_{:#018x}", stage, hash);
}

void PipelineCache::DumpShader(std::span<const u32> code, u64 hash, Shader::HwStage stage,
                               size_t perm_idx, std::string_view ext) {
    if (!EmulatorSettings.IsDumpShaders()) {
        return;
    }

    using namespace Common::FS;
    const auto dump_dir = GetUserPath(PathType::ShaderDir) / "dumps";
    if (!std::filesystem::exists(dump_dir)) {
        std::filesystem::create_directories(dump_dir);
    }
    const auto filename = fmt::format("{}.{}", GetShaderName(stage, hash, perm_idx), ext);
    const auto file = IOFile{dump_dir / filename, FileAccessMode::Create};
    file.WriteSpan(code);
}

std::optional<std::vector<u32>> PipelineCache::GetShaderPatch(u64 hash, Shader::HwStage stage,
                                                              size_t perm_idx,
                                                              std::string_view ext) {

    using namespace Common::FS;
    const auto patch_dir = GetUserPath(PathType::ShaderDir) / "patch";
    if (!std::filesystem::exists(patch_dir)) {
        std::filesystem::create_directories(patch_dir);
    }
    const auto filename = fmt::format("{}.{}", GetShaderName(stage, hash, perm_idx), ext);
    const auto filepath = patch_dir / filename;
    if (!std::filesystem::exists(filepath)) {
        return {};
    }
    const auto file = IOFile{patch_dir / filename, FileAccessMode::Read};
    std::vector<u32> code(file.GetSize() / sizeof(u32));
    file.Read(code);
    return code;
}

void PipelineCache::RequestProgramCompile(PipelineSelection& sel, Shader::HwStage hw_stage,
                                          Shader::SwStage sw_stage,
                                          const Shader::ShaderParams& params,
                                          Shader::Backend::Bindings binding, u64 job_key) {
    if (compile_queue.HasPending(job_key)) {
        return;
    }
    std::vector<u32> code(params.code.begin(), params.code.end());
    std::array<u32, Shader::ShaderParams::NumShaderUserData> user_data{};
    std::copy_n(params.user_data.begin(), user_data.size(), user_data.begin());
    auto runtime_info = BuildRuntimeInfo(sel, hw_stage, sw_stage);
    compile_queue.Request(job_key, [this, hw_stage, sw_stage, hash = params.hash,
                                    code = std::move(code), user_data, runtime_info,
                                    binding]() mutable {
        Shader::ShaderParams job_params{
            .user_data = user_data,
            .code = code,
            .hash = hash,
        };
        Program* program = nullptr;
        {
            std::shared_lock lk{programs_mutex};
            if (auto it = program_cache.find(hash); it != program_cache.end()) {
                program = it->second.get();
            }
        }
        if (!program) {
            auto new_program = std::make_unique<Program>(hw_stage, sw_stage, job_params);
            auto start = binding;
            std::vector<u32> spv;
            const auto module = CompileModule(new_program->info, runtime_info, job_params.code,
                                              0, binding, spv);
            auto spec =
                Shader::StageSpecialization(new_program->info, runtime_info, profile, start);
            new_program->AddPermut(module, std::move(spec));
            const auto [published, inserted] = PublishProgram(new_program, std::move(spv));
            if (inserted) {
                return;
            }
            auto& mine = new_program->modules[0];
            PublishPermut(*published, new_program->info, mine.module, std::move(mine.spec),
                          std::move(spv));
            return;
        }
        // The specialization reads the resource lists of the translated program, exactly as
        // GetProgram does with Program::info; an untranslated Info has none, and a spec without
        // resources compares equal to any permutation.
        std::optional<Shader::Info> base;
        {
            std::shared_lock lk{programs_mutex};
            if (program->info_template) {
                base.emplace(*program->info_template);
            }
        }
        if (!base) {
            return;
        }
        base->pgm_base = job_params.Base();
        base->user_data = job_params.user_data;
        auto start = binding;
        std::optional<Shader::StageSpecialization> spec;
        {
            sigjmp_buf recover;
            if (sigsetjmp(recover, 0)) {
                runtime_fault_recover = nullptr;
                return;
            }
            runtime_fault_recover = &recover;
            base->RefreshFlatBuf();
            spec.emplace(*base, runtime_info, profile, start);
            runtime_fault_recover = nullptr;
        }
        size_t num_modules = 0;
        {
            std::shared_lock lk{programs_mutex};
            if (program->FindPermut(*spec)) {
                return;
            }
            num_modules = program->modules.size();
        }
        Shader::Info info{hw_stage, sw_stage, job_params};
        std::vector<u32> spv;
        const auto module =
            CompileModule(info, runtime_info, job_params.code, num_modules, binding, spv);
        PublishPermut(*program, *base, module, std::move(*spec), std::move(spv));
    });
}

void PipelineCache::RequestGraphicsPipeline(PipelineSelection& sel) {
    const u64 pipeline_hash = std::hash<GraphicsPipelineKey>{}(sel.graphics_key);
    {
        std::shared_lock lk{pipelines_mutex};
        if (graphics_pipelines.contains(sel.graphics_key)) {
            return;
        }
    }
    if (compile_queue.HasPending(pipeline_hash)) {
        return;
    }
    GraphicsPipelineKey key = sel.graphics_key;
    auto modules = sel.modules;
    auto runtime_infos = sel.runtime_infos;
    auto fetch = sel.fetch_shader;
    std::array<const Shader::Info*, MaxShaderStages> infos{};
    // Pipeline objects keep Program::info pointers; that shared Info has no userdata until
    // the GPU thread refreshes it. Copy the worker's flattened tables so GetSharp on the
    // compile thread does not read a null flat buffer (Fault at offset 0x148).
    struct StageUd {
        const Shader::Info* info{};
        std::vector<u32> user_data;
        std::vector<u32> flat;
        VAddr pgm_base{};
    };
    boost::container::static_vector<StageUd, MaxShaderStages> ud_copies;
    if (sel.worker) {
        for (const auto& stage : sel.worker->stages) {
            const u32 sw = u32(stage.program->info.sw_stage);
            infos[sw] = &stage.program->info;
            StageUd copy;
            copy.info = infos[sw];
            copy.pgm_base = stage.pgm_base;
            if (const Shader::Info* src = sel.infos[sw]) {
                const auto ud = src->UserData();
                const auto flat = src->FlatUserData();
                copy.user_data.assign(ud.begin(), ud.end());
                copy.flat.assign(flat.begin(), flat.end());
                copy.pgm_base = src->ProgramBase();
            } else if (stage.flat) {
                copy.flat = *stage.flat;
            }
            ud_copies.push_back(std::move(copy));
        }
    } else {
        infos = sel.infos;
    }
    compile_queue.Request(pipeline_hash, [this, key, modules, runtime_infos, fetch, infos,
                                          ud_copies = std::move(ud_copies)]() {
        struct RestoreUdSnapshots {
            u32 saved;
            ~RestoreUdSnapshots() {
                Shader::Info::num_ud_snapshots = saved;
            }
        } restore{Shader::Info::num_ud_snapshots};
        const u32 n = static_cast<u32>(ud_copies.size());
        Shader::Info::num_ud_snapshots = n;
        for (u32 i = 0; i < n; ++i) {
            const auto& u = ud_copies[i];
            Shader::Info::ud_snapshots[i] = {
                u.info,
                u.user_data.data(),
                static_cast<u32>(u.user_data.size()),
                u.flat.data(),
                static_cast<u32>(u.flat.size()),
                u.pgm_base,
            };
        }
        PipelineSelection local{};
        local.graphics_key = key;
        local.modules = modules;
        local.runtime_infos = runtime_infos;
        local.fetch_shader = fetch;
        local.infos = infos;
        CreateGraphicsPipeline(local, false);
    });
}

void PipelineCache::RequestComputePipeline() {
    const u64 pipeline_hash = std::hash<ComputePipelineKey>{}(compute_key);
    {
        std::shared_lock lk{pipelines_mutex};
        if (compute_pipelines.contains(compute_key)) {
            return;
        }
    }
    if (compile_queue.HasPending(pipeline_hash)) {
        return;
    }
    const auto key = compute_key;
    const Shader::Info* info = sel.infos[0];
    const vk::ShaderModule module = sel.modules[0];
    std::vector<u32> user_data;
    std::vector<u32> flat;
    VAddr pgm_base = 0;
    if (info) {
        const auto ud = info->UserData();
        const auto f = info->FlatUserData();
        user_data.assign(ud.begin(), ud.end());
        flat.assign(f.begin(), f.end());
        pgm_base = info->ProgramBase();
    }
    compile_queue.Request(pipeline_hash, [this, key, info, module, user_data = std::move(user_data),
                                          flat = std::move(flat), pgm_base]() {
        struct RestoreUdSnapshots {
            u32 saved;
            ~RestoreUdSnapshots() {
                Shader::Info::num_ud_snapshots = saved;
            }
        } restore{Shader::Info::num_ud_snapshots};
        if (info) {
            Shader::Info::num_ud_snapshots = 1;
            Shader::Info::ud_snapshots[0] = {
                info,
                user_data.data(),
                static_cast<u32>(user_data.size()),
                flat.data(),
                static_cast<u32>(flat.size()),
                pgm_base,
            };
        }
        CreateComputePipeline(key, info, module);
    });
}

const ComputePipeline* PipelineCache::CreateComputePipeline(const ComputePipelineKey& key,
                                                            const Shader::Info* info,
                                                            vk::ShaderModule module) {
    {
        std::shared_lock lk{pipelines_mutex};
        if (auto it = compute_pipelines.find(key); it != compute_pipelines.end()) {
            return it->second.get();
        }
    }
    const auto pipeline_hash = std::hash<ComputePipelineKey>{}(key);
    LOG_INFO(Render_Vulkan, "Compiling compute pipeline {:#x}", pipeline_hash);
    CompileTimer timer{CompileKind::Pipeline};
    ComputePipeline::SerializationSupport sdata{};
    auto pipe = std::make_unique<ComputePipeline>(instance, scheduler, desc_heap, profile,
                                                  *pipeline_cache, key, *info, module, sdata,
                                                  false);
    if (!pipe->IsValid()) {
        return nullptr;
    }
    const ComputePipeline* result = nullptr;
    {
        std::unique_lock lk{pipelines_mutex};
        const auto [it, is_new] = compute_pipelines.try_emplace(key);
        if (is_new) {
            it.value() = std::move(pipe);
            RegisterPipelineData(key, sdata);
            ++num_new_pipelines;
            if (EmulatorSettings.IsShaderCollect()) {
                module_related_pipelines[module].emplace_back(key);
            }
        }
        result = it.value().get();
    }
    return result;
}

const GraphicsPipeline* PipelineCache::CreateGraphicsPipeline(PipelineSelection& target,
                                                              bool preloading) {
    {
        std::shared_lock lk{pipelines_mutex};
        if (auto it = graphics_pipelines.find(target.graphics_key); it != graphics_pipelines.end()) {
            return it->second.get();
        }
    }
    const auto pipeline_hash = std::hash<GraphicsPipelineKey>{}(target.graphics_key);
    LOG_INFO(Render_Vulkan, "Compiling graphics pipeline {:#x}", pipeline_hash);

    GraphicsPipeline::SerializationSupport sdata{};
    std::unique_ptr<GraphicsPipeline> pipe;
    {
        CompileTimer timer{CompileKind::Pipeline};
        pipe = std::make_unique<GraphicsPipeline>(
            instance, scheduler, desc_heap, profile, target.graphics_key, *pipeline_cache,
            target.infos, target.runtime_infos, target.fetch_shader, target.modules, sdata,
            preloading);
    }
    if (!pipe->IsValid()) {
        target.fetch_shader.reset();
        return nullptr;
    }

    const GraphicsPipeline* result = nullptr;
    {
        std::unique_lock lk{pipelines_mutex};
        const auto [it, is_new] = graphics_pipelines.try_emplace(target.graphics_key);
        if (is_new) {
            it.value() = std::move(pipe);
            RegisterPipelineData(target.graphics_key, pipeline_hash, sdata);
            ++num_new_pipelines;
        }
        result = it.value().get();
    }
    target.fetch_shader.reset();
    if (num_new_pipelines % 32 == 0) {
        SavePipelineCache();
    }
    return result;
}

std::filesystem::path PipelineCache::PipelineCacheFile() const {
    using namespace Common::FS;
    const auto& game = Common::ElfInfo::Instance();
    return GetUserPath(PathType::CacheDir) / std::string{game.GameSerial()} /
           "vk_pipeline_cache.bin";
}

void PipelineCache::LoadPipelineCache() {
    vk::PipelineCacheCreateInfo info{};
    std::vector<u8> blob;
    const auto path = PipelineCacheFile();
    struct Header {
        char magic[8];
        u32 vendor;
        u32 device;
        u8 uuid[VK_UUID_SIZE];
    } header{};
    if (EmulatorSettings.IsPipelineCacheEnabled() && std::filesystem::exists(path)) {
        using namespace Common::FS;
        const auto file = IOFile{path, FileAccessMode::Read};
        if (file.IsOpen() && file.GetSize() > sizeof(Header)) {
            file.Read(header);
            const auto& uuid = instance.GetPipelineCacheUUID();
            if (std::memcmp(header.magic, "BBVKC01", 7) == 0 &&
                header.vendor == instance.GetVendorID() &&
                header.device == instance.GetDeviceID() &&
                std::memcmp(header.uuid, uuid.data(), VK_UUID_SIZE) == 0) {
                blob.resize(file.GetSize() - sizeof(Header));
                file.Read(blob);
                info.initialDataSize = blob.size();
                info.pInitialData = blob.data();
            }
        }
    }
    auto [cache_result, cache] = instance.GetDevice().createPipelineCacheUnique(info);
    ASSERT_MSG(cache_result == vk::Result::eSuccess, "Failed to create pipeline cache: {}",
               vk::to_string(cache_result));
    pipeline_cache = std::move(cache);
}

void PipelineCache::SavePipelineCache() {
    if (!pipeline_cache || !EmulatorSettings.IsPipelineCacheEnabled()) {
        return;
    }
    auto [result, data] = instance.GetDevice().getPipelineCacheData(*pipeline_cache);
    if (result != vk::Result::eSuccess || data.empty()) {
        return;
    }
    const auto path = PipelineCacheFile();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    struct Header {
        char magic[8];
        u32 vendor;
        u32 device;
        u8 uuid[VK_UUID_SIZE];
    } header{};
    std::memcpy(header.magic, "BBVKC01", 8);
    header.vendor = instance.GetVendorID();
    header.device = instance.GetDeviceID();
    std::memcpy(header.uuid, instance.GetPipelineCacheUUID().data(), VK_UUID_SIZE);
    using namespace Common::FS;
    auto temp = path;
    temp += ".tmp";
    {
        const auto file = IOFile{temp, FileAccessMode::Create};
        if (!file.IsOpen() || file.WriteObject(header) != 1 ||
            file.WriteSpan(std::span<const u8>{data}) != data.size()) {
            return;
        }
    }
    std::filesystem::rename(temp, path, ec);
}

} // namespace Vulkan

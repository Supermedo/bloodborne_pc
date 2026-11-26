// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>
#include <string>

#include "shader_recompiler/info.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_shader_hle.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/host_shaders/buffer_multi_copy_comp.h"
#include "bbport_copy_shader.h"
#include "bbport_toggles.h"
#include "core/memory.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"

extern std::unique_ptr<AmdGpu::Liverpool> liverpool;

namespace Vulkan {

static constexpr u64 COPY_SHADER_HASH = 0xfefebf9f;

// bbport: the copy shader runs ~57 times per frame with ~1024 small ranges each. As one
// vkCmdCopyBuffer with that many regions it cost ~37 ns of GPU time per range (~2.1 ms per
// frame) plus the recording; buffer_multi_copy.comp copies all ranges in one dispatch.
static bool MultiCopy(Rasterizer& rasterizer, const VideoCore::Buffer* src,
                      const VideoCore::Buffer* dst, std::span<const vk::BufferCopy> copies) {
    if (copies.size() < 8 || BbToggle::Disabled(BbToggle::MultiCopyShader)) {
        return false;
    }
    u64 src_min = ~0ull, src_max = 0, dst_min = ~0ull, dst_max = 0;
    for (const auto& copy : copies) {
        if ((copy.srcOffset | copy.dstOffset | copy.size) & 3) {
            return false; // dword copies only
        }
        src_min = std::min(src_min, copy.srcOffset);
        src_max = std::max(src_max, copy.srcOffset + copy.size);
        dst_min = std::min(dst_min, copy.dstOffset);
        dst_max = std::max(dst_max, copy.dstOffset + copy.size);
    }
    auto& runtime = rasterizer.GetRuntime();
    auto& scheduler = runtime.GetScheduler();
    const auto& instance = runtime.GetInstance();
    const u64 align = instance.StorageMinAlignment();
    src_min = Common::AlignDown(src_min, align);
    dst_min = Common::AlignDown(dst_min, align);

    struct Pipeline {
        vk::UniqueDescriptorSetLayout set_layout;
        vk::UniquePipelineLayout layout;
        vk::UniquePipeline pipeline;
    };
    static Pipeline pipe = [&] {
        const auto device = instance.GetDevice();
        Pipeline p;
        std::array<vk::DescriptorSetLayoutBinding, 3> bindings{};
        for (u32 i = 0; i < 3; ++i) {
            bindings[i] = {.binding = i,
                           .descriptorType = vk::DescriptorType::eStorageBuffer,
                           .descriptorCount = 1,
                           .stageFlags = vk::ShaderStageFlagBits::eCompute};
        }
        p.set_layout = Check(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = static_cast<u32>(bindings.size()),
            .pBindings = bindings.data(),
        }));
        const vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                          .offset = 0,
                                          .size = sizeof(u32)};
        p.layout = Check(device.createPipelineLayoutUnique({
            .setLayoutCount = 1,
            .pSetLayouts = &*p.set_layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &range,
        }));
        const auto module = CompileSPV(BUFFER_MULTI_COPY_COMP, device);
        p.pipeline = Check(device.createComputePipelineUnique(
            {}, vk::ComputePipelineCreateInfo{
                    .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                              .module = module,
                              .pName = "main"},
                    .layout = *p.layout,
                }));
        device.destroyShaderModule(module);
        return p;
    }();

    // The region table goes into the stream buffer (host writes, visible at submission).
    thread_local std::vector<u32> regions;
    regions.resize(copies.size() * 4);
    for (size_t i = 0; i < copies.size(); ++i) {
        regions[i * 4 + 0] = static_cast<u32>((copies[i].srcOffset - src_min) / 4);
        regions[i * 4 + 1] = static_cast<u32>((copies[i].dstOffset - dst_min) / 4);
        regions[i * 4 + 2] = static_cast<u32>(copies[i].size / 4);
        regions[i * 4 + 3] = 0;
    }
    auto& stream = rasterizer.GetBufferCache().GetStreamBuffer();
    const u64 table_size = regions.size() * sizeof(u32);
    const u64 table_offset = stream.Copy(regions.data(), table_size, align);

    if (auto* profiler = GpuProfiler::Get()) {
        profiler->Mark(0xC0B1ull, [] { return std::string{"copy shader HLE: barrier + dispatch"}; });
    }
    scheduler.EndRendering();
    const u64 src_size = src_max - src_min, dst_size = dst_max - dst_min;
    if (runtime.IsBufferAccessed(src, src_min, src_size) ||
        runtime.IsBufferAccessed(dst, dst_min, dst_size, true)) {
        runtime.FlushBarriers();
    }
    const u32 count = static_cast<u32>(copies.size());
    scheduler.Record([src = src->Handle(), dst = dst->Handle(), table = stream.Handle(), src_min,
                      src_size, dst_min, dst_size, table_offset, table_size,
                      count](vk::CommandBuffer cmdbuf) {
        const std::array<vk::DescriptorBufferInfo, 3> infos{{
            {src, src_min, src_size},
            {dst, dst_min, dst_size},
            {table, table_offset, table_size},
        }};
        std::array<vk::WriteDescriptorSet, 3> writes{};
        for (u32 i = 0; i < 3; ++i) {
            writes[i] = {.dstBinding = i,
                         .descriptorCount = 1,
                         .descriptorType = vk::DescriptorType::eStorageBuffer,
                         .pBufferInfo = &infos[i]};
        }
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *pipe.pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *pipe.layout, 0, writes);
        cmdbuf.pushConstants(*pipe.layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(count),
                             &count);
        cmdbuf.dispatch(count, 1, 1);
    });
    runtime.AccessBuffer(src, src_min, src_size, vk::PipelineStageFlagBits2::eComputeShader,
                         vk::AccessFlagBits2::eShaderRead);
    runtime.AccessBuffer(dst, dst_min, dst_size, vk::PipelineStageFlagBits2::eComputeShader,
                         vk::AccessFlagBits2::eShaderWrite);
    return true;
}

// bbport: what the shortcut did not run as given (bbport_copy_shader.h).
enum CopyProblem : u32 {
    CopyForeignBuffers,
    CopyControlNotMapped,
    CopyRecordsCut,
    CopyMemoryNotMapped,
    NumCopyProblems,
};

// One log line, with what the next look at it needs. Each kind counts for itself, so that many lines
// of one do not keep the first of another out. The user data holds every buffer of the dispatch.
static void ReportCopyShader(CopyProblem problem, const Shader::Info& info,
                             const AmdGpu::ComputeProgram& cs_program, const Rasterizer& rasterizer,
                             const AmdGpu::Buffer& ctl_buf_sharp,
                             const AmdGpu::Buffer& src_buf_sharp,
                             const AmdGpu::Buffer& dst_buf_sharp, u32 records_cut) {
    static constexpr std::array<const char*, NumCopyProblems> what = {
        "dispatch dropped, the buffers are not this shader's",
        "dispatch dropped, the control buffer is not mapped",
        "records past the limits",
        "a copy dropped, its memory is not mapped",
    };
    static std::array<std::atomic<u64>, NumCopyProblems> reports{};
    const u64 count = reports[problem].fetch_add(1, std::memory_order_relaxed) + 1;
    if (!BbCopyShader::ShouldReport(count)) {
        return;
    }
    std::string user_data;
    for (const u32 word : info.UserData()) {
        user_data += fmt::format(" {:08x}", word);
    }
    LOG_WARNING(Render_Vulkan,
                "Copy shader: {} (number {} of this run), {} records cut; queue {}, program {:#x}, "
                "{} threads, {}x{}x{} groups; control at {:#x} stride {} records {}, source at {:#x} "
                "stride {} records {}, destination at {:#x} stride {} records {}; user data{}",
                what[problem], count, records_cut, rasterizer.CsQueue(),
                cs_program.Address<u64>(), cs_program.num_thread_x.full, cs_program.dim_x,
                cs_program.dim_y, cs_program.dim_z, u64(ctl_buf_sharp.base_address),
                ctl_buf_sharp.GetStride(), u32(ctl_buf_sharp.num_records),
                u64(src_buf_sharp.base_address), src_buf_sharp.GetStride(),
                u32(src_buf_sharp.num_records), u64(dst_buf_sharp.base_address),
                dst_buf_sharp.GetStride(), u32(dst_buf_sharp.num_records), user_data);
}

static bool ExecuteCopyShaderHLE(const Shader::Info& info, const AmdGpu::ComputeProgram& cs_program,
                                 Rasterizer& rasterizer) {
    auto& runtime = rasterizer.GetRuntime();
    auto& buffer_cache = rasterizer.GetBufferCache();

    // Copy shader defines three formatted buffers as inputs: control, source, and destination.
    const auto ctl_buf_sharp = info.buffers[0].GetSharp(info);
    const auto src_buf_sharp = info.buffers[1].GetSharp(info);
    const auto dst_buf_sharp = info.buffers[2].GetSharp(info);
    const auto buf_stride = src_buf_sharp.GetStride();

    // bbport: these were two ASSERTs that ended the game. Buffers that are not this shader's mean
    // this dispatch is the wrong one, and returning true drops only it (bbport_copy_shader.h).
    const auto elements = [](const AmdGpu::Buffer& sharp) {
        return BbCopyShader::Elements{sharp.GetStride(), u32(sharp.data_format),
                                      u32(sharp.num_format)};
    };
    if (!BbCopyShader::ShapeFits(elements(ctl_buf_sharp), elements(src_buf_sharp),
                                 elements(dst_buf_sharp))) {
        ReportCopyShader(CopyForeignBuffers, info, cs_program, rasterizer, ctl_buf_sharp,
                         src_buf_sharp, dst_buf_sharp, 0);
        return true;
    }
    using CopyShaderControl = BbCopyShader::Control;
    const u32 src_records = src_buf_sharp.num_records;
    const u32 dst_records = dst_buf_sharp.num_records;
    // Records past the buffer's end read as zero on the GPU; the memory behind may not be mapped.
    const u32 num_controls = std::min<u32>(cs_program.dim_x, ctl_buf_sharp.num_records);
    auto* memory = Core::Memory::Instance();
    const auto mapped = [memory](VAddr address, u64 size) {
        // What the normal buffer binding asks (BindBuffers), and with its lookup cache: this runs
        // for every batch of about 57 dispatches a frame.
        return memory->ClampRangeSize(address, size) == size;
    };
    if (!mapped(ctl_buf_sharp.base_address, u64{num_controls} * sizeof(CopyShaderControl))) {
        ReportCopyShader(CopyControlNotMapped, info, cs_program, rasterizer, ctl_buf_sharp,
                         src_buf_sharp, dst_buf_sharp, 0);
        return true;
    }
    const auto ctl_buf = reinterpret_cast<const CopyShaderControl*>(ctl_buf_sharp.base_address);

    static std::vector<vk::BufferCopy> copies;
    copies.clear();
    copies.reserve(num_controls);

    u32 records_cut = cs_program.dim_x - num_controls;
    for (u32 i = 0; i < num_controls; i++) {
        const auto range = BbCopyShader::Range(ctl_buf[i], buf_stride, src_records, dst_records);
        records_cut += range.cut ? 1 : 0;
        // A copy of no bytes is not allowed in Vulkan.
        if (range.size != 0) {
            copies.emplace_back(range.src_offset, range.dst_offset, range.size);
        }
    }
    if (records_cut != 0) {
        ReportCopyShader(CopyRecordsCut, info, cs_program, rasterizer, ctl_buf_sharp,
                         src_buf_sharp, dst_buf_sharp, records_cut);
    }

    // bbport: 64 KiB instead of 64 MiB. The copies are a few KiB spread over up to 57 MiB, and
    // each batch synchronizes (uploads, marks GPU-modified) its whole range: GPU time of the copy
    // shader 1.5 -> 0.6 ms/frame, GPU busy 85% -> 77%, frame rate no lower.
    // BB_COPY_MERGE_KB overrides it.
    static const vk::DeviceSize MaxDistanceForMerge = [] {
        const char* env = std::getenv("BB_COPY_MERGE_KB");
        return env ? vk::DeviceSize(std::strtoull(env, nullptr, 10)) * 1024 : vk::DeviceSize(64_KB);
    }();
    u32 batch_start = 0;
    u32 batch_end = 0;

    while (batch_end < copies.size()) {
        // Place first copy into the current batch
        const auto& copy = copies[batch_start];
        auto src_offset_min = copy.srcOffset;
        auto src_offset_max = copy.srcOffset + copy.size;
        auto dst_offset_min = copy.dstOffset;
        auto dst_offset_max = copy.dstOffset + copy.size;

        for (++batch_end; batch_end < copies.size(); batch_end++) {
            // Compute new src and dst bounds if we were to batch this copy
            const auto& [src_offset, dst_offset, size] = copies[batch_end];
            auto new_src_offset_min = std::min(src_offset_min, src_offset);
            auto new_src_offset_max = std::max(src_offset_max, src_offset + size);
            if (new_src_offset_max - new_src_offset_min > MaxDistanceForMerge) {
                break;
            }

            auto new_dst_offset_min = std::min(dst_offset_min, dst_offset);
            auto new_dst_offset_max = std::max(dst_offset_max, dst_offset + size);
            if (new_dst_offset_max - new_dst_offset_min > MaxDistanceForMerge) {
                break;
            }

            // We can batch this copy
            src_offset_min = new_src_offset_min;
            src_offset_max = new_src_offset_max;
            dst_offset_min = new_dst_offset_min;
            dst_offset_max = new_dst_offset_max;
        }

        // bbport: buffers of a stale dispatch may point at nothing, and the checks above only see
        // the buffers' own sizes. The game's own copies are always inside mapped memory.
        if (!mapped(src_buf_sharp.base_address + src_offset_min, src_offset_max - src_offset_min) ||
            !mapped(dst_buf_sharp.base_address + dst_offset_min, dst_offset_max - dst_offset_min)) {
            ReportCopyShader(CopyMemoryNotMapped, info, cs_program, rasterizer, ctl_buf_sharp,
                             src_buf_sharp, dst_buf_sharp, 0);
            batch_start = batch_end;
            continue;
        }

        // Obtain buffers for the total source and destination ranges.
        const auto [src_buf, src_buf_offset] = buffer_cache.ObtainBuffer(
            src_buf_sharp.base_address + src_offset_min, src_offset_max - src_offset_min, false);
        const auto [dst_buf, dst_buf_offset] = buffer_cache.ObtainBuffer(
            dst_buf_sharp.base_address + dst_offset_min, dst_offset_max - dst_offset_min, true);

        // Apply found buffer base.
        const auto vk_copies = std::span{copies}.subspan(batch_start, batch_end - batch_start);
        for (auto& copy : vk_copies) {
            copy.srcOffset = copy.srcOffset - src_offset_min + src_buf_offset;
            copy.dstOffset = copy.dstOffset - dst_offset_min + dst_buf_offset;
        }

        // Execute buffer copies.
        LOG_TRACE(Render_Vulkan, "HLE buffer copy: src_size = {}, dst_size = {}",
                  src_offset_max - src_offset_min, dst_offset_max - dst_offset_min);
        if (!MultiCopy(rasterizer, src_buf, dst_buf, vk_copies)) {
            runtime.CopyBuffer(src_buf, dst_buf, vk_copies);
        }
        batch_start = batch_end;
    }

    return true;
}

bool IsShaderHLE(u64 pgm_hash) {
    return pgm_hash == COPY_SHADER_HASH;
}

bool ExecuteShaderHLE(const Shader::Info& info, const AmdGpu::Regs& regs,
                      const AmdGpu::ComputeProgram& cs_program, Rasterizer& rasterizer) {
    switch (info.pgm_hash) {
    case COPY_SHADER_HASH:
        return ExecuteCopyShaderHLE(info, cs_program, rasterizer);
    default:
        return false;
    }
}

} // namespace Vulkan

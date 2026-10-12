// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <ranges>
#include <shared_mutex>
#include <string>
#include <unordered_set>
#include <vector>
#include <xxhash.h>
#include <fmt/format.h>
#include "common/logging/log.h"
#include "common/serdes.h"
#include "core/emulator_settings.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Serialization {
/* You should increment versions below once corresponding serialization scheme is changed. */
static constexpr u32 ShaderBinaryVersion = 14u; // permutation index must match program_cache slot
static constexpr u32 ShaderMetaVersion = 13u;   // permutation index must match program_cache slot
static constexpr u32 PipelineKeyVersion = 12u;  // permutation index must match program_cache slot
} // namespace Serialization

namespace Vulkan {

void RegisterPipelineData(const ComputePipelineKey& key,
                          ComputePipeline::SerializationSupport& sdata) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar{};
    Serialization::Writer pldata{ar};

    pldata.Write(Serialization::PipelineKeyVersion);
    pldata.Write(u32{1}); // compute

    key.Serialize(ar);
    sdata.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::PipelineKey,
                                       fmt::format("c_{:#018x}", key.value), ar.TakeOff());
}

void RegisterPipelineData(const GraphicsPipelineKey& key, u64 hash,
                          GraphicsPipeline::SerializationSupport& sdata) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar{};
    Serialization::Writer pldata{ar};

    pldata.Write(Serialization::PipelineKeyVersion);
    pldata.Write(u32{0}); // graphics

    key.Serialize(ar);
    sdata.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::PipelineKey,
                                       fmt::format("g_{:#018x}", hash), ar.TakeOff());
}

void RegisterShaderMeta(const Shader::Info& info,
                        const std::optional<Shader::Gcn::FetchShaderData>& fetch_shader_data,
                        const Shader::StageSpecialization& spec, size_t perm_hash,
                        size_t perm_idx) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar;
    Serialization::Writer meta{ar};

    meta.Write(Serialization::ShaderMetaVersion);
    meta.Write(Serialization::ShaderBinaryVersion);

    meta.Write(perm_hash);
    meta.Write(perm_idx);

    spec.Serialize(ar);
    info.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderMeta,
                                       fmt::format("{:#018x}", perm_hash), ar.TakeOff());
}

void RegisterShaderBinary(std::vector<u32>&& spv, u64 pgm_hash, size_t perm_idx) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderBinary,
                                       fmt::format("{:#018x}_{}", pgm_hash, perm_idx),
                                       std::move(spv));
}

bool LoadShaderMeta(Serialization::Archive& ar, Shader::Info& info,
                    std::optional<Shader::Gcn::FetchShaderData>& fetch_shader_data,
                    Shader::StageSpecialization& spec, size_t& perm_idx) {
    Serialization::Reader meta{ar};

    u32 meta_version{};
    meta.Read(meta_version);
    if (meta_version != Serialization::ShaderMetaVersion) {
        return false;
    }

    u32 binary_version{};
    meta.Read(binary_version);
    if (binary_version != Serialization::ShaderBinaryVersion) {
        return false;
    }

    u64 perm_hash_ar{};
    meta.Read(perm_hash_ar);
    meta.Read(perm_idx);

    spec.Deserialize(ar);
    if (!info.Deserialize(ar)) {
        LOG_WARNING(Render_Vulkan, "Cached shader {:#x} has an invalid SRT walker, skipping it",
                    info.pgm_hash);
        return false;
    }

    fetch_shader_data = spec.fetch_shader_data;
    return true;
}

void ComputePipelineKey::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer key{ar};
    key.Write(value);
}

bool ComputePipelineKey::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader key{ar};
    key.Read(value);
    return true;
}

void ComputePipeline::SerializationSupport::Serialize(Serialization::Archive& ar) const {
    // Nothing here yet
    return;
}

bool ComputePipeline::SerializationSupport::Deserialize(Serialization::Archive& ar) {
    // Nothing here yet
    return true;
}

bool PipelineCache::LoadComputePipeline(Serialization::Archive& ar) {
    ComputePipelineKey key{};
    key.Deserialize(ar);

    ComputePipeline::SerializationSupport sdata{};
    sdata.Deserialize(ar);

    std::vector<u8> meta_blob;
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta,
                                       fmt::format("{:#018x}", key.value), meta_blob);
    if (meta_blob.empty()) {
        return false;
    }

    Serialization::Archive meta_ar{std::move(meta_blob)};
    PipelineSelection local{};
    if (!LoadPipelineStage(meta_ar, 0, local)) {
        return false;
    }

    // Built before it is inserted: a pipeline the driver rejects while preloading throws
    // and leaves no empty entry behind. The local selection is not the shared GPU-thread one.
    auto pipe = std::make_unique<ComputePipeline>(instance, scheduler, desc_heap, profile,
                                                  *pipeline_cache, key, *local.infos[0],
                                                  local.modules[0], sdata, true);
    if (!pipe->IsValid()) {
        return false;
    }
    std::unique_lock lk{pipelines_mutex};
    const auto [it, is_new] = compute_pipelines.try_emplace(key);
    if (is_new) {
        it.value() = std::move(pipe);
    }
    return true;
}

void GraphicsPipelineKey::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer key{ar};

    key.Write(this, sizeof(*this));
}

bool GraphicsPipelineKey::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader key{ar};

    key.Read(this, sizeof(*this));
    return true;
}

void GraphicsPipeline::SerializationSupport::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer sdata{ar};

    sdata.Write(&vertex_attributes, sizeof(vertex_attributes));
    sdata.Write(&vertex_bindings, sizeof(vertex_bindings));
    sdata.Write(&divisors, sizeof(divisors));
    sdata.Write(multisampling);
    sdata.Write(tcs);
    sdata.Write(tes);
}

bool GraphicsPipeline::SerializationSupport::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader sdata{ar};

    sdata.Read(&vertex_attributes, sizeof(vertex_attributes));
    sdata.Read(&vertex_bindings, sizeof(vertex_bindings));
    sdata.Read(&divisors, sizeof(divisors));
    sdata.Read(multisampling);
    sdata.Read(tcs);
    sdata.Read(tes);
    return true;
}

bool PipelineCache::LoadGraphicsPipeline(Serialization::Archive& ar) {
    PipelineSelection local{};
    local.graphics_key.Deserialize(ar);

    // bbport: motion-vector pipelines are never preloaded. Their vertex shader embeds session-
    // local buffer addresses (see LoadShaderMeta), and the shader metadata is stored once per
    // program, not per permutation: the meta found for a motion pipeline's stages could be the
    // plain permutation, so LoadShaderMeta's motion check missed it and the preload built the
    // 8-attachment motion key with plain shaders. The AMD driver refused some of these
    // (ErrorUnknown: the startup crash), and the ones it accepted were registered under the
    // motion key without motion output. Built at first use instead, like the vertex shaders.
    if (local.graphics_key.motion_vectors) {
        return false;
    }

    GraphicsPipeline::SerializationSupport sdata{};
    sdata.Deserialize(ar);

    for (int stage_idx = 0; stage_idx < MaxShaderStages; ++stage_idx) {
        const auto& hash = local.graphics_key.stage_hashes[stage_idx];
        if (!hash) {
            continue;
        }

        std::vector<u8> meta_blob;
        Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta,
                                           fmt::format("{:#018x}", hash), meta_blob);
        if (meta_blob.empty()) {
            return false;
        }

        Serialization::Archive meta_ar{std::move(meta_blob)};

        if (!LoadPipelineStage(meta_ar, stage_idx, local)) {
            return false;
        }
    }

    auto pipe = std::make_unique<GraphicsPipeline>(
        instance, scheduler, desc_heap, profile, local.graphics_key, *pipeline_cache, local.infos,
        local.runtime_infos, local.fetch_shader, local.modules, sdata, true);
    if (!pipe->IsValid()) {
        return false;
    }
    std::unique_lock lk{pipelines_mutex};
    const auto [it, is_new] = graphics_pipelines.try_emplace(local.graphics_key);
    if (is_new) {
        it.value() = std::move(pipe);
    }
    return true;
}

bool PipelineCache::LoadPipelineStage(Serialization::Archive& ar, size_t stage,
                                      PipelineSelection& dest) {
    auto program = std::make_unique<Program>();
    Shader::StageSpecialization spec{};
    spec.info = &program->info;
    size_t perm_idx{};
    if (!LoadShaderMeta(ar, program->info, dest.fetch_shader, spec, perm_idx)) {
        return false;
    }

    std::vector<u32> spv{};
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderBinary,
                                       fmt::format("{:#018x}_{}", program->info.pgm_hash, perm_idx),
                                       spv);
    if (spv.empty()) {
        return false;
    }

    vk::ShaderModule module{};
    {
        std::shared_lock lk{programs_mutex};
        auto found = program_cache.find(program->info.pgm_hash);
        if (found != program_cache.end()) {
            const auto& it =
                std::ranges::find(found.value()->modules, spec, &Program::Module::spec);
            if (it != found.value()->modules.end()) {
                const auto idx = std::distance(found.value()->modules.begin(), it);
                if (perm_idx != idx) {
                    // The same specialization was stored under two indices (a hole from loading a
                    // higher index first, or two saves that now compare equal). The module already
                    // in the program is the one later lookups will find. Skip this pipeline only;
                    // erasing the program would drop every other pipeline that shares the shader.
                    LOG_WARNING(Render_Vulkan,
                                "Cached permutation {} of {}_{:x} conflicts with index {}, skipping "
                                "preload",
                                perm_idx, program->info.hw_stage, program->info.pgm_hash, idx);
                    return false;
                }
                dest.infos[stage] = &found.value()->info;
                dest.modules[stage] = it->module;
                return true;
            }
        }
    }

    // A module the driver rejects is a damaged cache entry, not a fatal error as in CompileSPV.
    if (spv.size() < 5 || spv[0] != 0x07230203u) {
        throw Serialization::CorruptData{"damaged SPIR-V in the shader cache"};
    }
    auto [module_result, created] = instance.GetDevice().createShaderModule(
        {.codeSize = spv.size() * sizeof(u32), .pCode = spv.data()});
    if (module_result != vk::Result::eSuccess) {
        throw Serialization::CorruptData{"cached SPIR-V rejected by the driver"};
    }
    module = created;
    std::unique_lock lk{programs_mutex};
    auto [it_pgm, new_program] = program_cache.try_emplace(program->info.pgm_hash);
    if (new_program) {
        it_pgm.value() = std::move(program);
    }
    spec.info = &it_pgm.value()->info;
    const bool placed = it_pgm.value()->InsertPermut(module, std::move(spec), perm_idx);
    const vk::ShaderModule slot = it_pgm.value()->modules[perm_idx].module;
    dest.infos[stage] = &it_pgm.value()->info;
    dest.modules[stage] = slot;
    lk.unlock();
    if (slot != module) {
        instance.GetDevice().destroyShaderModule(module);
    }
    return placed;
}

void PipelineCache::WarmUp() {
    if (!EmulatorSettings.IsPipelineCacheEnabled()) {
        return;
    }

    Storage::DataBase::Instance().Open();

    // The profile blob also stamps the serialization versions: a cache written by another
    // build is removed as a whole instead of piling up entries no build can load.
    struct CacheStamp {
        Shader::Profile profile;
        u32 binary_version;
        u32 meta_version;
        u32 key_version;
    };
    CacheStamp stamp{};
    std::memset(&stamp, 0, sizeof(stamp));
    std::memcpy(&stamp.profile, &profile, sizeof(profile));
    stamp.binary_version = Serialization::ShaderBinaryVersion;
    stamp.meta_version = Serialization::ShaderMetaVersion;
    stamp.key_version = Serialization::PipelineKeyVersion;

    std::vector<u8> profile_data{};
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderProfile, "profile", profile_data);
    const bool stamp_ok = profile_data.size() == sizeof(CacheStamp) &&
                          std::memcmp(profile_data.data(), &stamp, sizeof(stamp)) == 0;
    if (!stamp_ok) {
        if (!profile_data.empty()) {
            LOG_WARNING(Render, "Pipeline cache was written by another build or system: "
                                "rebuilding it");
        }
        Storage::DataBase::Instance().Clear();
        profile_data.resize(sizeof(stamp));
        std::memcpy(profile_data.data(), &stamp, sizeof(stamp));
        Storage::DataBase::Instance().Save(Storage::BlobType::ShaderProfile, "profile",
                                           std::move(profile_data));
    }

    const auto rebuild_cache = [&] {
        LOG_WARNING(Render, "Pipeline cache is too stale or damaged: rebuilding it");
        {
            std::unique_lock pipelines_lk{pipelines_mutex};
            std::unique_lock programs_lk{programs_mutex};
            graphics_pipelines.clear();
            compute_pipelines.clear();
            std::unordered_set<VkShaderModule> modules;
            for (const auto& [_, program] : program_cache) {
                if (!program) {
                    continue;
                }
                for (const auto& permutation : program->modules) {
                    if (permutation.module) {
                        modules.insert(VkShaderModule(permutation.module));
                    }
                }
            }
            for (const VkShaderModule module : modules) {
                instance.GetDevice().destroyShaderModule(vk::ShaderModule{module});
            }
            program_cache.clear();
        }
        Storage::DataBase::Instance().Clear();
        Storage::DataBase::Instance().FinishPreload();
        profile_data.resize(sizeof(stamp));
        std::memcpy(profile_data.data(), &stamp, sizeof(stamp));
        Storage::DataBase::Instance().Save(Storage::BlobType::ShaderProfile, "profile",
                                           std::move(profile_data));
    };

    std::vector<std::vector<u8>> blobs;
    Storage::DataBase::Instance().ForEachBlob(
        Storage::BlobType::PipelineKey, [&](std::vector<u8>&& data) { blobs.push_back(std::move(data)); });

    u32 num_pipelines = 0;
    u32 stale_version = 0;
    u32 stale_load = 0;
    u32 num_damaged = 0;
    const u32 num_total_pipelines = static_cast<u32>(blobs.size());

    // Preload on this thread only: parallel VkPipeline creation during WarmUp has hung some
    // drivers when combined with a partially migrated cache.
    const auto load_blob = [this, &num_pipelines, &stale_version, &stale_load,
                            &num_damaged](std::vector<u8> data) {
        try {
            Serialization::Archive ar{std::move(data)};
            Serialization::Reader pldata{ar};

            u32 version{};
            pldata.Read(version);
            if (version != Serialization::PipelineKeyVersion) {
                ++stale_version;
                return;
            }

            u32 is_compute{};
            pldata.Read(is_compute);
            const bool result =
                is_compute ? LoadComputePipeline(ar) : LoadGraphicsPipeline(ar);
            if (result) {
                ++num_pipelines;
            } else {
                ++stale_load;
            }
        } catch (const std::exception& e) {
            if (num_damaged++ == 0) {
                LOG_WARNING(Render, "Pipeline cache: damaged entry ({})", e.what());
            }
        }
    };
    for (auto& data : blobs) {
        load_blob(std::move(data));
    }

    const u32 loaded = num_pipelines;
    // A few entries that fail to preload are skipped. The ones that loaded stay: wiping the
    // directory made the next session recompile every area from scratch.
    if (num_damaged || (num_total_pipelines > 0 && loaded == 0)) {
        if (num_damaged) {
            LOG_WARNING(Render, "Pipeline cache: {} damaged entries", num_damaged);
        } else {
            LOG_WARNING(Render, "Pipeline cache: only {} of {} entries loaded ({} stale version, "
                                "{} load failed)",
                        loaded, num_total_pipelines, stale_version, stale_load);
        }
        rebuild_cache();
        return;
    }

    LOG_INFO(Render, "Preloaded {} pipelines", loaded);
    std::printf("Pipeline cache: preloaded %u of %u; stale version %u, load failed %u\n", loaded,
                num_total_pipelines, stale_version, stale_load);

    Storage::DataBase::Instance().FinishPreload();
}

void PipelineCache::Sync() {
    compile_queue.WaitIdle();
    SavePipelineCache();
    Storage::DataBase::Instance().Close();
}

} // namespace Vulkan

namespace Shader {

void Info::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer info{ar};

    info.Write(this, sizeof(InfoPersistent));
    info.Write(flattened_ud_buf);
    srt_info.Serialize(ar);
}

bool Info::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader info{ar};

    info.Read(this, sizeof(Shader::InfoPersistent));
    info.Read(flattened_ud_buf);

    return srt_info.Deserialize(ar);
}

void Gcn::FetchShaderData::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer fetch{ar};
    ar.Grow(6 + attributes.size() * sizeof(VertexAttribute));

    fetch.Write(size);
    fetch.Write(vertex_offset_sgpr);
    fetch.Write(instance_offset_sgpr);
    fetch.Write(attributes);
}

bool Gcn::FetchShaderData::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader fetch{ar};

    fetch.Read(size);
    fetch.Read(vertex_offset_sgpr);
    fetch.Read(instance_offset_sgpr);
    fetch.Read(attributes);

    return true;
}

void PersistentSrtInfo::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer srt{ar};

    srt.Write(this, sizeof(*this));
    if (walker_func_size) {
        srt.Write(reinterpret_cast<void*>(walker_func), walker_func_size);
    }
}

bool PersistentSrtInfo::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader srt{ar};

    srt.Read(this, sizeof(*this));

    walker_func = nullptr;
    if (walker_func_size) {
        // The size is checked before the code is registered (it becomes executable):
        // a cut-short file must not have bytes past its end run as the walker.
        const u8* code = ar.CurrPtr();
        if (!ValidateWalkerCode(code, walker_func_size, flattened_bufsize_dw)) {
            walker_func_size = 0;
            return false;
        }
        walker_func = RegisterWalkerCode(code, walker_func_size);
        ar.Advance(walker_func_size);
    }

    return true;
}

void StageSpecialization::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer spec{ar};

    spec.Write(start);
    spec.Write(runtime_info);

    spec.Write(bitset.to_string());

    if (fetch_shader_data) {
        spec.Write(sizeof(*fetch_shader_data));
        fetch_shader_data->Serialize(ar);
    } else {
        spec.Write(size_t{0});
    }

    spec.Write(vs_attribs);
    spec.Write(buffers);
    spec.Write(images);
    spec.Write(fmasks);
    spec.Write(samplers);
}

bool StageSpecialization::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader spec{ar};

    spec.Read(start);
    spec.Read(runtime_info);

    std::string bits{};
    spec.Read(bits);
    bitset = std::bitset<MaxStageResources>(bits);

    u64 fetch_data_size{};
    spec.Read(fetch_data_size);

    if (fetch_data_size) {
        Gcn::FetchShaderData fetch_data;
        fetch_data.Deserialize(ar);
        fetch_shader_data = fetch_data;
    }

    spec.Read(vs_attribs);
    spec.Read(buffers);
    spec.Read(images);
    spec.Read(fmasks);
    spec.Read(samplers);

    return true;
}

} // namespace Shader

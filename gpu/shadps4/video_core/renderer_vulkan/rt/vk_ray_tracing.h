#pragma once

#include <array>
#include <chrono>
#include <cstdio>
#include <functional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/rt/rt_adaptive.h"
#include "video_core/renderer_vulkan/rt/rt_far.h"
#include "video_core/renderer_vulkan/rt/rt_blas_cache.h"
#include "video_core/renderer_vulkan/rt/rt_breadcrumb.h"
#include "video_core/renderer_vulkan/rt/rt_sharc.h"
#include "video_core/renderer_vulkan/rt/rt_direct.h"
#include "video_core/renderer_vulkan/rt/rt_fx.h"
#include "video_core/renderer_vulkan/rt/rt_geometry.h"
#include "video_core/renderer_vulkan/rt/rt_gi.h"
#include "video_core/renderer_vulkan/rt/rt_gi_pipeline.h"
#include "video_core/renderer_vulkan/rt/rt_hot.h"
#include "video_core/renderer_vulkan/rt/rt_lights.h"
#include "video_core/renderer_vulkan/rt/rt_live.h"
#include "video_core/renderer_vulkan/rt/rt_moon_cache.h"
#include "video_core/renderer_vulkan/rt/rt_sky_cache.h"
#include "video_core/renderer_vulkan/rt/rt_restir_gi.h"
#include "video_core/renderer_vulkan/rt/rt_surface.h"
#include "video_core/renderer_vulkan/rt/rt_toggle.h"
#include "video_core/renderer_vulkan/rt/vk_ray_tracing_ptlas.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

namespace VideoCore {
class BufferCache;
class TextureCache;
}

namespace Vulkan {

class Instance;
class Scheduler;
class Runtime;
class SceneTargets;
class ObjectMotion;

class RayTracing {
public:
    /// The game's object motion vectors (the moon history follows moving objects with them).
    void SetObjectMotion(ObjectMotion* motion) noexcept {
        object_motion = motion;
    }

    struct SceneInputs {
        VideoCore::ImageId depth{};
        std::array<float, 4> projection{};
        Rt::Affine inv_view{};
        bool camera_valid{};
        std::array<float, 2> jitter{};
        SceneTargets* scene_targets{};
        bool reduced{};
    };

    RayTracing(const Instance& instance, Scheduler& scheduler, Runtime& runtime,
               VideoCore::TextureCache& texture_cache, VideoCore::BufferCache& buffer_cache);
    ~RayTracing();

    [[nodiscard]] bool Enabled() const noexcept {
        return enabled;
    }

    static bool Wanted();
    static bool FxSmokeWanted();
    [[nodiscard]] bool FxSmoke() const noexcept {
        return enabled && fx_smoke;
    }
    [[nodiscard]] bool FxSheets() const noexcept {
        return fx_sheets;
    }
    [[nodiscard]] static bool Paused() noexcept {
        return Rt::effects_paused.load(std::memory_order_relaxed);
    }
    [[nodiscard]] bool WaterSheet(u64 ps) const {
        return enabled && water_on && !Paused() &&
               ps == (water_mark_layer ? Rt::WaterLayerShader : Rt::FxMistShader);
    }
    [[nodiscard]] bool SheetHidden(u64 ps) const {
        return ps == Rt::WaterLayerShader ? !water_layer : !fx_sheets;
    }
    [[nodiscard]] bool FogSheet(u64 ps) const {
        return enabled && water_on && !Paused() && mist_sheets && water_mist > 0.0f && ps == Rt::FxSheetShader;
    }
    [[nodiscard]] bool HazeLit(u64 ps) const {
        return enabled && haze_lit && !Paused() && (fx_sheets || stock_haze) && !FogSheet(ps) &&
               ps == Rt::FxSheetShader;
    }
    [[nodiscard]] u32 HazeColor(const std::array<float, 3>& world, u32 rgba) const;
    [[nodiscard]] float MistSheetScale() const noexcept {
        return mist_sheet_scale;
    }

    void AddDraw(const Rt::DrawInput& draw);
    void AddSmoke(std::span<const Rt::FxParticle> particles, const Rt::DrawTag& tag);
    [[nodiscard]] bool FlameShader(u64 ps) const {
        return enabled && water_on && Rt::HashListed(flame_ps, ps, 0);
    }
    void AddFlames(std::span<const Rt::FxParticle> world_particles);
    void AddFog(std::span<const Rt::FxParticle> world_particles);
    [[nodiscard]] bool GlowShader(u64 ps) const {
        return enabled && !glow_ps.empty() && Rt::HashListed(glow_ps, ps, 0);
    }
    void AddGlows(std::span<const std::array<float, 6>> vertices);
    void OnDrawBound(vk::ImageView albedo_view, VideoCore::ImageId image, u64 uid,
                     vk::ImageView normal_view = {}, VideoCore::ImageId normal_image = {},
                     vk::ImageView alpha_view = {}, VideoCore::ImageId alpha_image = {},
                     u64 alpha_uid = 0, u32 alpha_channel = 3);
    void OnSkyDome(vk::ImageView view, VideoCore::ImageId image, u64 uid);
    u32 SurfaceSlot(vk::ImageView view, VideoCore::ImageId image);
    void AppendKeptInstances(const Rt::Affine& current_inv_view);
    std::function<void(u32, u64, u64)> HoldFn();

    void OnGBufferPass(std::span<const VideoCore::ImageId> targets);
    void OnSceneColor(VideoCore::ImageId color);
    void OnDispatch(u64 cs_hash, const SceneInputs& inputs);
    void OnAfterComposite(const SceneInputs& inputs);
    bool SkipPs(u64 ps_hash) const {
        if (ps_hash == Rt::DeferredAoShader && SkipGameAo()) {
            return true;
        }
        if (ps_hash == Rt::VolumeLightShader && SkipGameVolumes()) {
            return true;
        }
        const bool paused = Paused();
        return (!fx_sheets && !paused &&
                ((ps_hash == Rt::FxSheetShader && !stock_haze) || ps_hash == Rt::FxMistShader)) ||
               (water_on && !water_layer && !paused && ps_hash == Rt::WaterLayerShader) ||
               (!skip_ps.empty() && std::find(skip_ps.begin(), skip_ps.end(), ps_hash) != skip_ps.end());
    }
    void InvalidateHistory() {
        history_valid = false;
    }

    void OnDisplayPass(const SceneInputs& inputs);

    [[nodiscard]] bool WantsLightBuffers() const noexcept {
        return enabled && light_capture;
    }
    void OnLightDraw(u64 ps_hash);
    void OnLightStencil(u32 stencil);
    void OnLightState(const Rt::LightDrawState& state);
    void OnLightVtxParam(u32 size, const void* data);
    void LogGatedLights(const Rt::LightTargets& main);
    void OnLightBuffer(u32 size, const void* data);
    void OnCompositeBuffer(u32 index, VAddr address, u32 size);
    [[nodiscard]] std::span<const Rt::Light> ShadowLights() const noexcept {
        return shadow_lights;
    }
    [[nodiscard]] std::span<const Rt::Light> FrameLights() const noexcept {
        return frame_lights;
    }
    static constexpr u64 LightCompositeShader = Rt::LightCompositeShader;
    [[nodiscard]] bool WantsDirect() const noexcept {
        return enabled && (direct_enabled || reflect_enabled);
    }
    void OnLightTargets(VideoCore::ImageId diffuse, VideoCore::ImageId specular);
    void OnAoInput(VideoCore::ImageId image);
    [[nodiscard]] bool WantsAo() const noexcept {
        return enabled && ao_enabled && ao_live;
    }
    void OnAoDraw(const SceneInputs& inputs);
    // The game's GXLightDeferredAO draw has no job once our traced light replaces the stock
    // accumulation (BB_RT_GAME_AO=1 keeps it for A/B).
    bool SkipGameAo() const;
    // BB_RT_SKIP_SHADOWS: the game's shadow-map draws are skipped while our direct pass
    // replaces the game's light accumulation (nothing else that reaches the screen samples them).
    bool SkipShadowMaps() const;
    // BB_RT_SKIP_VOLUMES: the game's volumetric light shafts are skipped while our traced fog
    // runs (it scatters light only from traced sources: moon, lamps, emitters, sky).
    bool SkipGameVolumes() const;
    bool SkipHalfResLight(VideoCore::ImageId target, bool blended, u64 ps_hash) const;
    [[nodiscard]] bool WantsDirShadowMask(u64 ps_hash) const noexcept {
        return direct_enabled && Rt::DirShadowMaskDraw(ps_hash);
    }
    void OnDirShadowInput(VideoCore::ImageId image);
    void OnLightSetBuffer(VAddr address, u32 size);
    void OnLightSetWrite(VAddr dst, u32 bytes, VAddr src, bool host_copy);
    void DebugLine(const char* text);
    void OnLightSetStorageWrite(VAddr address, u32 bytes, u64 pgm_hash, u32 stage);
    u32 sets_storage_logged = 0;
    void OnAccTargetDraw(VideoCore::ImageId target, u64 vs_hash, u64 ps_hash, u32 mrt_mask,
                         bool light_draw);
    std::unordered_set<u64> acc_draw_logged;
    void OnCompositeInput(VideoCore::ImageId image, bool proxy);
    void OnColorTarget(VideoCore::ImageId image, bool clear, u64 ps_hash);
    void OnFastClearEliminate(VideoCore::ImageId image);
    void OnCompositeDraw(VideoCore::ImageId target, VideoCore::ImageId depth);
    [[nodiscard]] const char* HookName() const noexcept {
        return hook_ps == Rt::LightCmbAlbedoShader ? "cmb-albedo" : "composite";
    }
    [[nodiscard]] bool DirectTrace() const noexcept {
        return direct_enabled && direct_debug == 4 && trace_until != ~0ull && frame <= trace_until;
    }
    [[nodiscard]] bool SkipLightDraw(VideoCore::ImageId diffuse, VideoCore::ImageId specular,
                                     bool composite, bool blended, bool writes_depth,
                                     bool writes_stencil);
    void OnLightComposite(const SceneInputs& inputs, u64 ps_hash);

    struct DenoiserImage {
        vk::Image image{};
        vk::ImageView view{};
        vk::Format format{};
    };
    struct DenoiserInputs {
        DenoiserImage radiance;
        DenoiserImage diffuse_albedo;
        DenoiserImage specular_albedo;
        DenoiserImage normal_roughness;
        DenoiserImage hit_distance;
        DenoiserImage specular_hit_distance;
        u64 specular_frame{~0ull};
        u32 width{}, height{};
        u64 frame{~0ull};
        bool valid{};
    };
    [[nodiscard]] DenoiserInputs Denoiser() const noexcept {
        DenoiserInputs d{};
        if (!gi_enabled || !gi_width || !raw.written || !aux_hit.written) {
            return d;
        }
        const auto pick = [](const OwnImage& own, vk::Format format) {
            return DenoiserImage{vk::Image(own.image), *own.view, format};
        };
        d.radiance = pick(raw, vk::Format::eR16G16B16A16Sfloat);
        d.diffuse_albedo = pick(aux_albedo, vk::Format::eR16G16B16A16Sfloat);
        d.specular_albedo = pick(aux_specular, vk::Format::eR16G16B16A16Sfloat);
        d.normal_roughness = pick(aux_normal, vk::Format::eR16G16B16A16Sfloat);
        d.hit_distance = pick(aux_hit, vk::Format::eR16Sfloat);
        if (aux_spec_hit.written) {
            d.specular_hit_distance = pick(aux_spec_hit, vk::Format::eR16Sfloat);
            d.specular_frame = reflect_frame;
        }
        d.width = gi_width;
        d.height = gi_height;
        d.frame = aux_frame;
        d.valid = true;
        return d;
    }
    struct Bound {
        vk::Image image{};
        vk::ImageView view{};
        u32 width{}, height{};
    };
    struct RrSource {
        std::span<const VideoCore::ImageId> gbuffer;
        Rt::NormalSource normal;
        int albedo_target;
        int roughness_target;
        int specular_target;
        bool guides_ready;
        Bound diffuse_albedo, specular_albedo, normal_roughness, specular_hit;
    };
    [[nodiscard]] bool RayReconstructionWanted() const noexcept {
        return enabled && toggles.rr;
    }
    [[nodiscard]] RrSource RayReconstructionSource() const noexcept {
        return {std::span<const VideoCore::ImageId>{gbuffer_targets.data(), gbuffer_count},
                normal_source,
                albedo_target,
                direct_g1_target,
                shadow_spec_target,
                false,
                {},
                {},
                {},
                reflect_enabled && aux_spec_hit.written && reflect_frame == frame && gi_width
                    ? Bound{vk::Image(aux_spec_hit.image), *aux_spec_hit.view, gi_width,
                            gi_height}
                    : Bound{}};
    }

private:
    void DumpLights();
    void EndLightFrame();
    struct HostRing {
        vk::Buffer buffer{};
        VmaAllocation allocation{};
        u8* mapped{};
        u64 address{};
        u32 slot_bytes{};
    };
    struct DeviceBuffer {
        vk::Buffer buffer{};
        VmaAllocation allocation{};
        u64 address{};
        u64 size{};
    };
    struct OwnImage {
        VideoCore::UniqueImage image;
        vk::UniqueImageView view;
        bool written = false;
    };
    struct PendingBuild {
        u32 entry_id{};
        u32 triangle_base{};
        u32 triangles{};
        u64 scratch_bytes{};
        bool update{};
        bool updatable{};
        bool omm{};
        u32 instance_index{};
        u64 omm_scratch_bytes{};
        u32 record{};
        bool anchored{};
    };
    struct FrameInstance {
        u32 entry_id{};
        Rt::Affine inv_view{};
        bool fresh{};
        u32 texture{};
        bool alpha_test{};
        float alpha_ref{0.5f};
        u64 view{}, uid{};
        u32 normal_map{};
        u32 image{};
        u64 normal_view{}, normal_uid{};
        u32 normal_image{};
        bool blended{};
        u32 alpha_texture{};
        u32 alpha_channel{3};
        u64 alpha_view{}, alpha_uid{};
        u32 alpha_image{};
        Rt::DrawTag tag{};
        u32 mode{};
        u64 skin_key{};
        u32 skin_step{};
        bool anchored{};
        bool kept{};
        u32 glass_mask{};
    };
    struct SkinState {
        u32 entry{}, mode{}, step{}, mask{}, flags{}, slot{}, triangles{};
        bool present{}, alpha{}, textured{};
        u64 ps{}, vs{}, view{}, last_frame{};
    };
    struct PickInstance {
        u32 entry_id{}, mode{}, texture{}, alpha_texture{}, mask{}, flags{}, triangles{};
        bool alpha_test{}, blended{}, uv{}, omm{};
        Rt::DrawTag tag{};
    };
    void ReadPick();
    void LogPick(const std::array<u32, 96>& words, std::span<const PickInstance> snap);
    void LogPickCamera(const std::array<u32, 96>& words, std::span<const PickInstance> snap);
    struct ProbeRecord {
        std::array<u32, 4> slots{};
        u32 entry{};
        u32 mode{};
        u32 pad0{}, pad1{};
        std::array<std::array<float, 4>, 3> rows{};
    };
    static_assert(sizeof(ProbeRecord) == 80);
    using RetiredBlock = Rt::RetiredBlock;
    struct Hitch {
        double add{}, alloc{}, build{}, gi{}, images{}, debug{}, display{}, wait{}, readback{},
            evict{}, stats{};
        u32 adds{}, rebuilds{}, refits{}, deferred{}, held{}, misses{}, dirty{}, skeleton{};
        u32 shifted{};
        std::array<u32, 5> miss_kind{};
        u32 superset{};
        std::array<u32, 7> reasons{};
        u64 rebuilt_triangles{}, refit_triangles{};
        u32 anchored{};
        float anchor_turn{};
    };

    void FinishFrame(const SceneInputs& inputs);
    void PrintStats(u32 triangles);
    void PrintGpuLine();
    u32 WriteGiLights(u32 slot, const Rt::Affine& view);
    void ReportHitch();

    bool CreateResources();
    void DestroyResources();
    bool CreatePipelines(const Rt::LiveToggles& f);
    bool CreateFeatureRings(const Rt::LiveToggles& f);
    bool MakeFeatures(const Rt::LiveToggles& need);
    void LogLiveState();
    bool CreateGiImages(u32 width, u32 height);
    void DestroyGiImages();
    bool MakeOwnImage(OwnImage& own, vk::Format format, u32 width, u32 height,
                      vk::ImageUsageFlags extra_usage = {});
    bool CreateReflectImages(u32 width, u32 height);
    bool CreateAoImages(u32 width, u32 height);
    void DestroyAoImages();
    void RecordAo(const SceneInputs& inputs);
    void ReadAoStats();
    void BeginSlot();
    void ReclaimBlocks();
    bool CreateSurfaceResources();
    void DestroySurfaceResources();
    void WriteSurfaces(u32 slot);
    void ReleaseEntry(const Rt::CacheEntry& entry);
    bool AllocateEntry(u32 triangles, bool updatable, bool omm, Rt::CacheEntry& out,
                       u64& scratch_bytes, u64& omm_scratch_bytes);
    u64 OmmScratchBytes(u32 triangles) const;
    u64 ScratchBytes(u32 triangles, bool updatable, bool update, u32 omm_entry = ~0u) const;
    bool Build(const SceneInputs& inputs);
    void RecordDebug(const SceneInputs& inputs);
    bool RecordGi(const SceneInputs& inputs);
    void ApplyFillVis();
    void ReadFillVis();
    void RecordSkyLayer(vk::CommandBuffer cmdbuf, std::span<const vk::WriteDescriptorSet> gi_writes);
    void ReadMoonRing();
    void SkyImageIo(vk::CommandBuffer cmdbuf, u64 key);
    void RecordFillVis(vk::CommandBuffer cmdbuf, std::span<const vk::WriteDescriptorSet> gi_writes,
                       const Rt::Affine& view, u32 slot);
    void RecordShadows(const SceneInputs& inputs);
    void RecordFog(const SceneInputs& inputs);
    void RecordSky(const SceneInputs& inputs);
    void RecordExposure(const SceneInputs& inputs, bool apply);
    bool SkyDomeLive() const;
    bool CreateFogImages(u32 width, u32 height);
    void DestroyFogImages();
    void RecordDirect(const SceneInputs& inputs);
    struct GpuSets {
        vk::Buffer buffer{};
        u64 offset{};
        u32 bytes{};
    };
    GpuSets ObtainGpuLightSets(u64 max_age);
    // The direct pass's moon set vote, written into GiParams.sharc.w of a GI/reflection params slot.
    void RecordWorldSet(vk::CommandBuffer cmdbuf, vk::ImageView depth_view, vk::ImageView g3_view,
                        const vk::DescriptorBufferInfo& sets, u32 set_count, u32 width, u32 height,
                        vk::Buffer params, u64 params_offset);
    void RecordMoonDir(vk::CommandBuffer cmdbuf, u64 sets_offset, u32 sets_bytes);
    void LogSetSource(const char* pass, const GpuSets& s, u32 sets, u32 lights);
    void ApplySetReadback();
    void LogDirSets();
    struct HotShader {
        const char* name{};
        vk::UniquePipelineLayout* layout{};
        vk::UniquePipeline* pipeline{};
        Rt::FileStamp stamp{};
    };
    void ReadLiveKnobs();
    void OpenLightDump();
    void PollHot();
    void ApplyToggles(const Rt::LiveToggles& wanted);
    bool PreparePtlas();
    void FlipTlasMode();
    Rt::LiveToggles toggle_avail{};
    Rt::LiveToggles toggles{};
    Rt::LiveToggles made = Rt::NoFeatures();
    Rt::LiveToggles last_wanted = Rt::NoFeatures();
    bool surface_avail = false;
    bool surface_made = false;
    u32 gpu_line_frames = 600;
    bool RebuildHotShader(HotShader& hs, std::span<const u32> code);
    std::string shader_dir;
    std::vector<HotShader> hot_shaders;
    enum class DirSetsMode : u8 { Snapshot, Gpu, Host };
    DirSetsMode dir_sets_mode = DirSetsMode::Snapshot;
    u64 sets_snapshot_frame = ~0ull;
    u32 sets_snapshot_bytes = 0;
    u64 sets_snapshot_frames = 0;
    u64 sets_bind_frame = ~0ull;
    u32 sets_bind_count = 0;
    u32 sets_bind_lines = 0;
    u32 sets_bind_logged = 0;
    u32 sets_write_logged = 0;
    bool dir_sets_bind_first = false;
    bool ao_live = true;
    __attribute__((format(printf, 2, 3))) void Trace(const char* format, ...);
    void TraceImage(const char* what, VideoCore::ImageId id);
    bool BindScene(VideoCore::ImageId id, const SceneInputs& inputs, vk::AccessFlags2 access,
                   Bound& out);
    bool BindMask(VideoCore::ImageId id, const SceneInputs& inputs, const Bound& depth, Bound& out);
    bool BindEmissive(const SceneInputs& inputs, u32 width, u32 height, Bound& out);

    const Instance& instance;
    Scheduler& scheduler;
    Runtime& runtime;
    VideoCore::TextureCache& texture_cache;
    VideoCore::BufferCache& buffer_cache;
    bool enabled = false;
    u32 debug_mode = 1;
    float range = 50.0f;
    float ao_radius = 1.0f;
    bool ao_enabled = false;
    u32 ao_rays = 2;
    u32 ao_max_history = 16;
    u32 ao_debug = 0;
    u32 ao_scale = 2;
    HostRing ao_stats_ring;
    HostRing halo_ring;
    std::array<u64, 3> ao_stats_tick{};
    std::array<u64, 3> ao_stats_frame{~0ull, ~0ull, ~0ull};
    double ao_stats_stock = 0.0, ao_stats_ours = 0.0;
    u64 ao_stats_frames = 0;
    vk::UniqueDescriptorSetLayout ao_layout;
    vk::UniquePipelineLayout ao_pipeline_layout;
    vk::UniquePipeline ao_pipeline;
    HostRing ao_params_ring;
    std::array<OwnImage, 2> ao_history;
    u32 ao_width = 0, ao_height = 0;
    u32 ao_history_index = 0;
    bool ao_history_valid = false;
    u64 ao_history_frame = ~0ull;
    Rt::Affine ao_prev_inv_view{};
    std::array<float, 4> ao_prev_projection{};
    std::array<float, 2> ao_prev_jitter{};
    std::array<VideoCore::ImageId, 8> ao_inputs{};
    u32 ao_input_count = 0;
    u64 ao_input_frame = ~0ull;
    VideoCore::ImageId ao_mask{};
    u64 ao_frame = ~0ull;
    u32 ao_main_width = 0;
    bool ao_falloff = true;
    bool ao_occlude_y = true;
    std::array<VideoCore::ImageId, 8> ao_gbuffer{};
    u32 ao_gbuffer_count = 0, ao_gbuffer_width = 0;
    u64 ao_frames = 0, ao_skipped = 0, ao_mask_mismatch = 0;

    static constexpr u32 Slots = 3;
    bool pick_enabled = false;
    u32 pick_every = 60;
    bool miss_log = false;
    bool gi_fog = true;
    bool gi_at_composite = true;
    u64 gi_composite_frame = ~0ull;
    u64 gi_fog_frame = ~0ull;
    u32 gi_fog_bytes = 0;
    u32 gi_fog_log_lines = 0;
    u32 miss_log_lines = 0;
    u32 rebuild_log_lines = 0;
    u32 superset_log_lines = 0;
    std::array<float, 2> pick_at{};
    HostRing pick_ring;
    std::array<u64, Slots> pick_tick{};
    std::array<u64, Slots> pick_frame{~0ull, ~0ull, ~0ull};
    std::array<u32, Slots> pick_built{};
    std::array<u64, Slots> pick_snap_frame{};
    std::array<std::vector<PickInstance>, Slots> pick_snap;
    // Kept-instance probe (BB_RT_KEPT_PROBE, words 128..255 of the pick ring): camera rays on a
    // coarse grid that hit a kept off-screen instance in front of the drawn surface. The game no
    // longer draws it there (a far LOD twin over the near floor shaded the floor black), so it is
    // not kept again until the game draws it.
    bool kept_probe = true;
    std::array<std::vector<u32>, Slots> kept_snap; // TLAS index -> kept entry id, ~0u otherwise
    std::array<u64, Slots> kept_snap_frame{};
    std::array<u64, Slots> kept_probe_tick{};
    std::array<u64, Slots> kept_probe_frame{~0ull, ~0ull, ~0ull};
    std::array<u32, Slots> kept_probe_built{};
    u64 kept_probe_hidden = 0, kept_probe_reads = 0, kept_probe_hits = 0;
    u32 kept_probe_log_lines = 0;
    void ReadKeptProbe();
    std::vector<Rt::DrawTag> entry_tags;
    u64 pick_logged_frame = 0;
    u32 frame_draws = 0;
    bool alpha_skip_unresolved = false;
    u64 alpha_unresolved_last = 0, alpha_unresolved_total = 0;
    std::vector<u8> instance_state_prev;
    std::array<u64, 4> flips_by_mode{}, unresolved_by_mode{};
    u64 flips_last = 0, flips_total = 0;
    std::unordered_map<u64, u64> flips_by_ps, unresolved_by_ps;
    void NoteInstanceState(u32 entry_id, u32 mode, u64 ps, u8 state, bool unresolved);
    std::vector<u64> exclude_hashes;
    u64 excluded_draws = 0;
    static constexpr u64 UnusedFrames = 120;
    Rt::Capacity capacity{};
    Rt::FrameGeometry geometry;
    Rt::BlasCache cache;
    Rt::BlockPool pool;
    Rt::RebuildBudget rebuild_budget;
    Rt::RefitBudget refit_budget;
    bool hitch_log = true;
    double hitch_threshold_ms = 8.0;
    Hitch hitch{};
    double hitch_worst = 0.0, hitch_worst_wait = 0.0;
    u64 hitch_frames = 0;
    std::vector<RetiredBlock> retired;
    std::vector<RetiredBlock> retired_uv;
    Rt::BlockPool uv_pool;
    DeviceBuffer uv_buffer;
    HostRing surfaces_ring;
    Rt::TextureTable textures;
    Rt::SurfaceHolds surface_holds{Slots};
    vk::UniqueDescriptorSetLayout surface_layout;
    vk::UniqueDescriptorPool surface_descriptor_pool;
    struct TextureRef {
        VideoCore::ImageId id{};
        u64 uid{};
        vk::ImageView view{};
        bool own{};
    };
    std::array<std::vector<TextureRef>, Slots> slot_textures;
    static constexpr u32 SurfaceSetRing = 24;
    struct SurfaceSetSlot {
        vk::DescriptorSet set{};
        u64 tick{};
    };
    std::array<SurfaceSetSlot, SurfaceSetRing> surface_ring{};
    u32 surface_ring_next{};
    OwnImage surface_fallback;
    u64 surface_dead{};
    vk::DescriptorSet SurfaceSet(vk::CommandBuffer cmdbuf, u32 slot);
    // The set SurfaceSet returned last: a pass in the same submission (tick) and slot reuses it
    // instead of taking another ring entry.
    struct SurfaceLast {
        vk::DescriptorSet set{};
        u64 tick = 0;
        u32 slot = ~0u;
    } surface_last;
    vk::UniqueSampler surface_sampler;
    u32 surface_written = 1;
    u32 built_slot = 0;
    bool surface_enabled = false;
    bool last_add_accepted = false;
    u64 uvs_address = 0;
    u64 normals_address = 0;
    u64 surface_normal_mapped = 0;
    u64 surface_draws = 0, surface_textured = 0, surface_alpha = 0, surface_uv_full = 0;
    u64 surface_alpha_split = 0, alpha_offered = 0, alpha_accepted = 0;
    std::vector<vk::UniqueAccelerationStructureKHR> entry_handles;
    std::vector<u64> entry_addresses;
    std::vector<u32> free_ids;
    std::vector<PendingBuild> pending;
    std::vector<FrameInstance> instances;
    std::vector<ProbeRecord> probes;
    std::array<std::vector<u32>, Slots> slot_probe_entries;
    std::array<u64, Slots> slot_probe_frames{};
    // Per probe: the entry's view and the instance mode, to place rt_probe.comp's sample point.
    std::array<std::vector<Rt::Affine>, Slots> slot_probe_views;
    std::array<std::vector<u8>, Slots> slot_probe_modes;
    // rt_probe.comp writes five words per probe (mismatch, sample point xyz, sample index).
    static constexpr u32 ProbeFlagWords = 5;
    // BB_RT_SHELF: metres a static mesh may jump before it counts as another placement of the
    // same draw (0 = off: rebuild in place, which drops the old placement from the TLAS).
    float shelf_metres = 1.0f;
    u64 shelved_total = 0, unshelved_total = 0;
    u32 shelf_log_lines = 0;
    u64 masked_instances = 0, camera_masked = 0, last_masked = 0;
    u64 kept_instances = 0, last_kept = 0, kept_untextured = 0;
    u64 kept_tris_last = 0, kept_alpha_last = 0, kept_over_budget_last = 0, kept_too_big_last = 0;
    u64 kept_shared_tex_last = 0, kept_dropped_total = 0;
    bool keep_gi = false;
    Rt::KeepRules keep_rules{};
    struct OffSwitches {
        bool lights = true, cone = true, instances = true, evict = true, texresolve = true,
             holds = true;
    } off_switches;
    std::array<float, 2> light_keep_proj{};
    // Scene camera at the end of the light frame (BB_RT_LIGHT_KEEP_SCENE): culls kept lights and
    // glow lamps instead of the camera in the game's light draws.
    Rt::Affine light_keep_inv_view{};
    bool light_keep_cam = false;
    bool light_keep_scene = true;
    u64 light_keep_scene_frames = 0;
    float light_keep_cam_gap = 0.0f;
    void SetLightKeepCamera(const SceneInputs& inputs) noexcept;
    u64 pool_full_draws = 0;
    u64 retired_bytes = 0;
    u64 scratch_alignment = 256;
    HostRing records_ring, indices_ring, instances_ring, params_ring, probes_ring, flags_ring,
        shadow_params_ring, direct_params_ring, direct_lights_ring, direct_sets_ring,
        gi_lights_ring, gi_sets_ring, gi_fog_ring, restir_remap_ring, sets_readback_ring;
    std::array<Rt::SetReadback, Slots> sets_readback{};
    DeviceBuffer soup, scratch, pool_buffer, tlas_buffer, fingerprints, material, material_accum;
    std::vector<u32> material_resets;
    bool material_cleared = false;
    vk::UniqueAccelerationStructureKHR tlas;
    u64 tlas_address = 0;
    u64 tlas_frame = 0;
    u64 tlas_tick = 0;
    PartitionedTlas ptlas;
    bool ptlas_on = false;
    bool ptlas_made = false;
    bool ptlas_pending = false;
    u64 khr_tlas_address = 0;
    bool tlas_space_written = false;
    double tlas_upload_ms_sum = 0.0;
    u64 tlas_upload_frames = 0;
    u64 tlas_writes_sum = 0;
    u64 tlas_refit_writes_sum = 0;
    u32 tlas_hidden_last = 0;
    u64 positions_address = 0;
    std::array<u64, Slots> slot_ticks{};
    static constexpr u32 TlasPass = 8;
    static constexpr u32 TimePasses = 9;
    struct CompactQuery {
        u32 entry_id{};
        u64 built_frame{};
    };
    void CrumbBegin(vk::CommandBuffer cmdbuf, Rt::Crumb what);
    void CrumbEnd(vk::CommandBuffer cmdbuf, Rt::Crumb what);
    void PrintCrumbs() const;
    DeviceBuffer crumbs;
    Rt::CrumbWords* crumbs_mapped{};
    u32 crumb_serial{};
    void TimeBegin(vk::CommandBuffer cmdbuf, u32 pass);
    void TimeEnd(vk::CommandBuffer cmdbuf, u32 pass);
    void ReadTimes(u32 slot);
    void ApplyCompaction(u32 slot);
    vk::UniqueQueryPool time_pool, compact_pool;
    std::array<double, TimePasses> time_sum_ms{};
    std::array<u64, TimePasses> time_count{};
    std::array<u32, Slots> time_written{};
    std::array<double, TimePasses> time_last_ms{};
    std::array<std::vector<CompactQuery>, Slots> slot_compact;
    double timestamp_period_ns = 1.0;
    bool time_enabled = false;
    bool compact_enabled = false;
    bool omm_enabled = false;
    u32 omm_level = 3;
    bool omm_log = false;
    u32 omm_budget = 0;
    u64 omm_over_budget = 0;
    u32 omm_used = 0;
    u64 omm_input_slot_bytes = 0;
    DeviceBuffer omm_pool_buffer, omm_input, omm_index_buffer;
    Rt::BlockPool omm_pool, omm_index_pool;
    std::vector<vk::UniqueMicromapEXT> omm_handles;
    std::vector<RetiredBlock> retired_omm, retired_omm_index;
    bool omm_skinned = true;
    u32 gi_tmax_m = 0;
    u64 omm_skinned_built = 0, omm_skinned_refits = 0;
    vk::UniqueDescriptorSetLayout omm_layout;
    vk::UniquePipelineLayout omm_pipeline_layout;
    vk::UniquePipeline omm_pipeline;
    u64 frame = 0;
    u64 built_frames = 0, empty_frames = 0, skipped_frames = 0;
    u32 max_triangles_seen = 0, max_rebuilt_seen = 0, last_rebuilt = 0, last_instances = 0;
    bool built_this_frame = false;
    bool frame_camera_valid = false;
    Rt::Affine frame_inv_view{};

    vk::UniqueDescriptorSetLayout gather_layout, debug_layout, probe_layout, gi_layout, apply_layout,
        shadow_layout, direct_layout, fog_layout, fog_apply_layout, sky_layout, world_set_layout;
    vk::UniquePipelineLayout gather_pipeline_layout, debug_pipeline_layout, probe_pipeline_layout,
        gi_pipeline_layout, apply_pipeline_layout, shadow_pipeline_layout, direct_pipeline_layout,
        fog_pipeline_layout, fog_apply_pipeline_layout, sky_pipeline_layout,
        world_set_pipeline_layout;
    vk::UniquePipeline gather_pipeline, debug_pipeline, probe_pipeline, gi_pipeline, apply_pipeline,
        shadow_pipeline, direct_pipeline, fog_pipeline, fog_apply_pipeline, sky_pipeline,
        world_set_pipeline;
    u64 world_set_frames = 0;
    bool gi_world_set = true;
    // rt_moon_dir.comp: light 1 of the set copy points at the moon the sky draws.
    vk::UniqueDescriptorSetLayout moon_dir_layout;
    vk::UniquePipelineLayout moon_dir_pipeline_layout;
    vk::UniquePipeline moon_dir_pipeline;
    // rt_exposure.comp: eye adaptation of the traced image (BB_RT_EXPOSURE).
    static constexpr float ExposureKeyDefault = -5.5f;
    vk::UniqueDescriptorSetLayout exposure_layout;
    vk::UniquePipelineLayout exposure_pipeline_layout;
    vk::UniquePipeline exposure_pipeline;
    HostRing exposure_ring;
    bool exposure_on = true;
    bool exposure_reset = true;
    float exposure_key = ExposureKeyDefault;
    float exposure_max = 1.5f;
    float exposure_rate = 0.03f;
    float exposure_white = 3.0f;
    float thin_transmit = 0.4f; // BB_RT_THIN_TRANSMIT: moonlight through thin cards
    u64 exposure_frames = 0;
    u64 moon_dir_frames = 0;
    bool moon_from_sky = true;
    // rt_gi.comp RT_SKY_LAYER: the moon and cloud layer grid and the moon search.
    vk::UniqueDescriptorSetLayout sky_layer_layout;
    vk::UniquePipelineLayout sky_layer_pipeline_layout;
    vk::UniquePipeline sky_layer_pipeline;
    float moon_min = 0.2f;
    u64 sky_layer_frames = 0;
    // The moon per sky model (rt_moon_cache.h): keyed by the layer draws, read back from the
    // sky layer pass, seeded into it while the game culls the disc.
    Rt::SkyKey sky_key;
    Rt::MoonCache moon_cache;
    std::string moon_cache_path;
    u64 moon_generation = 0;
    bool moon_found_run = false; // a confirmed find this layer run
    bool moon_last_found = false;
    // Logged source of light 1 (0 game sets, 1 sky moon, 2 sky moon fading out).
    int moon_source_logged = -1;
    u64 moon_stats_logged = 0;
    u64 moon_screen_logged = 0;
    bool moon_layer_find = false;  // BB_RT_MOON_LAYER_FIND
    std::array<float, 2> frame_proj_scale{}; // projection x/y scale of the last built frame
    std::array<float, 3> moon_last{}, moon_confirmed{};
    static constexpr u32 MoonRingSlotBytes = 80;
    std::array<Rt::Affine, 3> moon_ring_view{};
    HostRing moon_ring;
    std::array<u64, 3> moon_ring_tick{};
    std::array<u64, 3> moon_ring_frame{~0ull, ~0ull, ~0ull};
    std::array<u64, 3> moon_ring_key{};
    std::array<u64, 3> moon_ring_gen{};
    // The learned sky image per sky model on disk (rt_sky_cache.h, BB_RT_SKY_CACHE): loaded when
    // a sky appears, read back every SkyIoEvery frames and saved when it has grown.
    static constexpr u64 SkyIoEvery = 240;
    enum class SkyIo { Idle, Upload, Readback };
    SkyIo sky_io = SkyIo::Idle;
    bool sky_io_failed = false;
    vk::Buffer sky_io_buffer{};
    VmaAllocation sky_io_allocation{};
    float* sky_io_mapped = nullptr;
    u64 sky_io_tick = 0;
    u64 sky_io_key = 0;
    u64 sky_io_generation = 0;
    u64 sky_io_next = 0;
    u64 sky_seed_key = 0;
    u64 sky_seed_generation = ~0ull;
    std::string sky_cache_dir;
    std::unordered_map<u64, size_t> sky_saved_seen;

    bool gi_enabled = false;
    u32 gi_spp = 1;
    u32 gi_debug = 0;
    u32 gi_max_history = 16;
    u32 gi_denoise = 2;
    float gi_debug_threshold = 0.0f;
    float gi_strength = 1.0f;
    float gi_fill_sky = 1.0f;
    float gi_sky = 0.0f;
    float gi_clamp = 4.0f;
    float gi_bias = 0.02f;
    u32 gi_bounces = 2;
    u32 gi_bounces_used = 2;
    float far_start = 0.0f;
    float far_band = Rt::FarBandDefaultM;
    float adaptive_ms = 12.0f;
    double adaptive_frame_ms = 0.0;
    Rt::AdaptiveBounces adaptive{};
    u32 gi_bounce_rays = 1;
    float gi_emissive_luma = 2.0f;
    u32 gi_sparse = Rt::GiSparseDefault;
    bool gi_last_bounce_cache = false;
    bool gi_sharc = false;
    bool sharc_cleared = false;
    bool sharc_failed = false;
    DeviceBuffer sharc_grid;
    vk::UniqueDescriptorSetLayout sharc_layout;
    vk::UniquePipelineLayout sharc_pipeline_layout;
    vk::UniquePipeline sharc_pipeline;
    bool gi_light_grid = true;
    bool skin_fast_trace = true;
    bool skin_log = false;
    bool moving_refit = true;
    bool refit_anchor = true;
    u64 anchored_refits = 0;
    float anchor_turn_max = 0.0f;
    bool probe_keep = true;
    bool skin_alpha = true;
    u32 moving_tris = 65536;
    u64 moving_marked = 0, probe_kept = 0;
    std::unordered_map<u64, SkinState> skin_prev, skin_cur;
    std::unordered_map<u64, u8> skin_watch;
    std::unordered_map<u32, u64> skin_entry_keys;
    std::array<std::vector<u64>, Slots> slot_skin_keys;
    std::array<std::vector<u8>, Slots> slot_skin_keep;
    u64 skin_sec_frames = 0, skin_sec_max = 0, skin_sec_absent = 0, skin_sec_flips = 0,
        skin_sec_probe = 0, skin_sec_probe_masked = 0, skin_sec_suppressed = 0;
    double skin_sec_start = 0.0;
    void SkinNote(u64 key, const SkinState& state);
    void SkinFrame();

    u64 gi_grid_frames = 0, gi_grid_fallbacks = 0;
    u64 gi_lights_frame = ~0ull;
    u32 gi_lights_count = 0;
    std::vector<Rt::DirectLightGpu> gi_lights_scratch;
    std::vector<u32> gi_grid_scratch;
    bool direct_lights_wanted = false;
    u64 aux_frame = ~0ull;
    u64 trigger_hash = 0x9a9cf8a9;
    Rt::NormalSource normal_source{Rt::GameNormalSource};
    int albedo_target = -1;
    int debug_target = -1;

    std::array<VideoCore::ImageId, 8> gbuffer_targets{};
    VideoCore::ImageId emissive_target{};
    int emissive_target_index = Rt::EmissiveTargetDefault;
    u32 gbuffer_count = 0;
    u32 gbuffer_bound = 0;
    u64 gbuffer_layouts_ignored = 0;
    u64 direct_depth_normal_frames = 0;
    bool gbuffer_printed = false;
    VideoCore::ImageId scene_color{};
    u64 gi_frames = 0, gi_skipped = 0, gi_resets = 0, gi_early_composites = 0;
    u64 last_gi_frame = ~0ull;
    u64 last_post_frame = ~0ull;
    bool history_valid = false;
    bool history_prev_frame_ok = false;
    bool effects_were_paused = false;
    Rt::Affine previous_inv_view{};
    std::array<float, 4> previous_projection{};
    std::array<float, 2> previous_jitter{};
    u32 gi_width = 0, gi_height = 0;
    u32 history_index = 0;
    std::array<OwnImage, 2> history, history_z;
    OwnImage raw;
    OwnImage aux_albedo, aux_specular, aux_normal, aux_hit, aux_spec_hit;
    // See-through glass: per-pixel diffuse keep factor written by the reflection pass.
    OwnImage glass_keep;
    u64 glass_keep_frame = ~0ull;
    struct ReflectImages {
        OwnImage raw, aux_albedo, aux_specular, aux_normal, aux_hit, spec_hit, glass_keep;
        u32 width = 0, height = 0;
    } reflect_own;
    bool reflect_enabled = false;
    float reflect_max_rough = Rt::ReflectMaxRoughDefault;
    float reflect_strength = 1.0f;
    u32 reflect_debug = 0;
    u64 reflect_frame = ~0ull;
    u64 reflect_frames = 0, reflect_skipped = 0, reflect_pre_ao_frames = 0;
    HostRing reflect_params_ring;
    vk::UniqueDescriptorSetLayout reflect_layout;
    vk::UniquePipelineLayout reflect_pipeline_layout;
    vk::UniquePipeline reflect_pipeline;
    Rt::PreAoPair<VideoCore::ImageId> pre_ao_pair;
    void RecordReflect(const SceneInputs& inputs);

    bool restir_gi_enabled = false;
    u32 restir_gi_m_clamp = Rt::RestirGiMClampDefault;
    u32 restir_gi_max_age = Rt::RestirGiMaxAgeDefault;
    u32 restir_gi_spatial = Rt::RestirGiSpatialDefault;
    u32 restir_gi_neighbours = Rt::RestirGiNeighboursDefault;
    float restir_gi_radius = Rt::RestirGiRadiusDefault;
    u32 restir_gi_history = 0;
    std::array<std::array<OwnImage, 4>, Rt::RestirGiSets> restir_gi_sets;
    HostRing restir_gi_params_ring;
    vk::UniqueDescriptorSetLayout restir_gi_init_layout, restir_gi_resolve_layout,
        restir_gi_temporal_layout, restir_gi_spatial_layout;
    vk::UniquePipelineLayout restir_gi_init_pipeline_layout, restir_gi_resolve_pipeline_layout,
        restir_gi_temporal_pipeline_layout, restir_gi_spatial_pipeline_layout;
    vk::UniquePipeline restir_gi_init_pipeline, restir_gi_resolve_pipeline,
        restir_gi_temporal_pipeline, restir_gi_spatial_pipeline;
    void RecordRestirGi(const SceneInputs& inputs, const Bound& depth, u32 slot, u32 flags,
                        std::span<const vk::WriteDescriptorSet> gi_writes, u32 groups_x,
                        u32 groups_y);
    bool gi_rt_pipeline = false;
    bool gi_rt_ser = false;
    vk::UniqueDescriptorSetLayout restir_gi_rt_layout;
    vk::UniquePipelineLayout restir_gi_rt_pipeline_layout;
    std::array<vk::UniquePipeline, 2> restir_gi_rt_pipelines;
    DeviceBuffer gi_sbt_buffer;
    u8* gi_sbt_mapped = nullptr;
    Rt::GiSbtLayout gi_sbt{};
    bool CreateGiRtPipeline(std::span<const vk::DescriptorSetLayoutBinding> compute_bindings,
                            u32 variant);

    using LightDrawRecord = Rt::LightDrawRecord;
    bool light_capture = false;
    u32 light_max_local = 4;
    std::FILE* light_dump = nullptr;
    u64 light_dump_frames = 0;
    u64 light_frame = ~0ull;
    const Rt::LightShader* light_draw_shader = nullptr;
    std::vector<LightDrawRecord> light_draws;
    Rt::LightDedup light_dedup;
    size_t light_draw_first = 0;
    std::vector<Rt::Light> frame_lights;
    std::vector<Rt::Light> pending_lights;
    std::vector<Rt::Light> shadow_lights;
    std::vector<Rt::ShadowTrack> shadow_tracks;
    bool shadows_enabled = false;
    u32 shadow_debug = 0;
    float shadow_strength = 0.75f;
    int shadow_spec_target = 3;
    u64 light_cpu_ns = 0;
    u64 light_cpu_frame = 0;
    float shadow_bias = 0.02f;
    float shadow_tmin = 0.0f;
    float shadow_gap = 0.3f;
    u64 shadow_frames = 0, shadow_skipped = 0, shadow_light_frames = 0;
    u64 light_draws_total = 0, light_lights_total = 0, light_unknown_buffers = 0;
    bool direct_enabled = false;
    bool direct_novis = false;
    u32 direct_debug = 0;
    float direct_light_log = 0.0f;
    bool light_gate = true;
    bool fill_light_off = false;
    bool light_depth = true;
    Rt::CameraFixedCount fixed_count{};
    u64 depth_packed_frames = 0;
    u32 depth_packed_last = 0;
    std::vector<Rt::FixedLightTrack> fixed_tracks;
    std::vector<Rt::Light> gated_lights;
    u64 gate_scaled = 0, gate_removed = 0, gate_fixed_frames = 0, gate_log_frame = 0;
    u64 direct_light_log_frame = 0;
    // Fill-light visibility (BB_RT_FILL_VIS, rt_gi.comp RT_LIGHT_VIS): per light, the fraction of
    // fixed rays that reach the sky and whether it sits at an emitter, cached by world cell.
    struct FillVisEntry {
        std::array<float, 3> world{};
        float radius = 0.0f;
        const char* shader_name = nullptr;
        Rt::FillVisResult last{}; // latest result, trusted or not
        float sky = 0.0f;         // from the latest trusted result
        float applied = 1.0f, logged_scale = -1.0f;
        // measured: a trusted result exists (light on screen and not hidden when traced, so this
        // frame's TLAS held the geometry around it). Until then the light keeps full strength
        // and is retried every FillVisRetryFrames.
        bool measured = false, emitter = false, queued = false, logged_waiting = false;
        bool inherited = false; // seeded from a neighbour cell (a moving light), not traced yet
        bool lamp = false;      // latched: enclosed >= BB_RT_LAMP_ENCLOSED (a glass-housed lamp)
        bool fire_pit = false;  // latched: as enclosed but open above (Rt::FillVisFirePit)
        bool pulse = false;     // latched: its colour pulses (BB_RT_FILL_FLICKER), an animated source
        std::array<float, 3> game_color{};
        // Flicker (BB_RT_FILL_FLICKER): the game's colour sum last frame, the sign of its last
        // change, and how many times the change reversed direction.
        float flicker_last = -1.0f;
        float flicker_radius = -1.0f; // two lights in one cell would fake reversals
        int flicker_sign = 0;
        u32 flicker_turns = 0;
        u64 measured_frame = 0, tried_frame = 0, seen_frame = 0, step_frame = ~0ull;
    };
    std::unordered_map<u64, FillVisEntry> fill_vis;
    std::vector<u64> fill_vis_queue;
    std::array<std::array<u64, Rt::FillVisBatch>, Slots> fill_vis_slot_keys{};
    std::array<u32, Slots> fill_vis_slot_count{};
    std::array<u64, Slots> fill_vis_tick{};
    std::array<u64, Slots> fill_vis_frame{~0ull, ~0ull, ~0ull};
    HostRing fill_vis_ring;
    vk::UniqueDescriptorSetLayout fill_vis_layout;
    vk::UniquePipelineLayout fill_vis_pipeline_layout;
    vk::UniquePipeline fill_vis_pipeline;
    bool fill_vis_on = true;
    u32 fill_vis_rays = 256;
    u32 fill_vis_refresh = 120;
    float emitter_range = 4.0f;
    float emitter_bright = 2.0f;
    float emitter_big_flicker = 0.5f;
    float emitter_big_fast = 0.5f;
    float emitter_big_gain = 1.5f;
    float emitter_source = 0.25f;
    float emitter_lift = 0.0f;
    float emitter_peak = 1.0f;
    float emitter_flame = 1.0f;
    float emitter_flicker = 2.5f;
    float emitter_flicker_avg = 0.02f;
    float emitter_flicker_smooth = 0.1f;
    float emitter_sway = 0.15f;
    float emitter_steady_flicker = 0.35f;
    float emitter_steady_dev = 0.05f;
    float fixed_sway = 0.015f;
    float candle_rmax = 3.0f;
    float candle_source = 0.85f;
    float glow_flame = 1.0f;
    float glow_range = 1.8f;
    float candle_range = 2.5f;
    float halo_strength = 0.03f;
    float halo_radius = 0.12f;
    float halo_core = 0.1f;
    bool reflect_sky_dome = true;
    bool sky_image = true;
    float halo_match = 2.0f;
    std::vector<std::array<float, 3>> halo_spots, halo_view_scratch;
    u32 halo_count = 0;
    std::chrono::steady_clock::time_point flicker_t0 = std::chrono::steady_clock::now();
    std::unordered_map<u64, Rt::FlickerState> flicker_mean, flicker_mean_next;
    u32 emitter_falloff_last = 0;
    float fill_vis_sky_full = 0.12f;
    bool fill_vis_sky_keep{};
    bool glass_pass = true;
    u64 fill_vis_carried_log{};
    u64 direct_list_log{};
    float lamp_source = 0.6f, lamp_range = 6.0f, lamp_rmin = 1.0f, lamp_rmax = 6.0f;
    float lamp_enclosed = 0.4f;
    float lamp_up = 0.5f;
    float fire_lift = 0.5f;
    u32 fire_lift_last = 0;
    float ambient_cut = 1.0f;
    float ambient_spec_cut = 1.0f;
    u64 spec_cut_frame = ~0ull;
    float ambient_keep_lo = 0.1f, ambient_keep_hi = 0.3f, ambient_keep_lit = 0.25f;
    std::array<float, 3> ambient_cool{0.39f, 0.68f, 1.0f};
    u32 lamp_falloff_last = 0;
    float fill_vis_floor = 0.0f;
    bool fill_vis_offscreen = true;
    float fill_vis_emit_dist = 0.6f;
    float fill_vis_emit_luma = 0.02f;
    float fill_vis_enclosed = 0.2f;
    float fill_vis_cluster = 2.0f;
    float fill_vis_flicker = 0.01f;
    bool fill_vis_warm = true;
    float fill_vis_warm_hue = Rt::WarmLampBlue;
    float fill_vis_lamp_min = Rt::LampMinColour;
    u32 fill_vis_cluster_last = 0;
    std::vector<std::array<float, 3>> fill_vis_emitter_pos;
    u64 fill_vis_dispatches = 0, fill_vis_results = 0, fill_vis_scaled = 0, fill_vis_frames = 0;
    u32 fill_vis_scaled_last = 0, fill_vis_off_last = 0;
    u32 direct_rays = 8;
    u32 direct_keep = 2;
    u32 direct_keep_offscreen = 1800;
    int direct_g1_target = 1;
    u32 light_dump_first = 3;
    Rt::DirectTracks direct_tracks;
    std::vector<Rt::Light> direct_lights;
    std::vector<float> direct_sets;
    u32 direct_set_count = 0;
    Rt::GpuSetSource direct_sets_source{};
    u64 direct_sets_gpu_frames = 0;
    bool moon_mask = false;
    float moon_cone_cos = 1.0f;
    float moon_min_elevation = 0.0f; // BB_RT_MOON_MIN_EL, degrees
    bool moon_static = true;
    float restir_gi_jacobian = Rt::RestirGiJacobianDefault;
    u32 moon_half = 2;
    // BB_RT_MOON_WORLD (live, default 1): every surface uses the moon of the directional set most
    // of the screen uses, instead of its own material group's moon (characters got 1/7 of it).
    bool moon_world = true;
    // Pick pixel for the fog pass readout (RecordDirect sets it, RecordFog passes it on).
    std::array<float, 4> fog_pick_px{-1.0f, -1.0f, 0.0f, 0.0f};
    bool dir_log = false;
    // BB_RT_DIR_DUMP=1 (live, needs BB_RT_DIR_LOG=1): every non-zero set with its four
    // directional lights, once a second.
    bool dir_dump = false;
    u64 dir_dump_frame = 0;
    u64 dir_log_frame = ~0ull;
    std::vector<float> dir_log_prev;
    Rt::SetReadback dir_log_last{};
    std::chrono::steady_clock::time_point dir_log_tick{};
    struct DirLogStats {
        u64 frames{}, sets{}, shadow{}, address{}, flags{}, gaps{}, zero{}, lines{};
        u64 direct{}, state{}, no_sets{}, host{}, pass{}, no_mask{};
        float color_delta{}, dir_delta{};
    } dir_log_stats{};
    u32 dir_log_state = ~0u;
    void LogDirFrame(u32 state);
    bool depth_only_draws = false;
    bool fx_smoke = false;
    HostRing smoke_ring;
    u32 smoke_triangle_base = 0;
    u32 smoke_draws = 0;
    u32 smoke_triangles = 0;
    u32 smoke_vertex_cursor = 0;
    u64 fx_smoke_draws = 0;
    u64 fx_smoke_quads = 0;
    u64 fx_smoke_capped = 0;
    u64 fx_smoke_no_uv = 0;
    bool fog_surfaces = false;
    float fx_smoke_opacity = 0.6f;
    u32 fx_debug = 0;
    u64 fx_blended_draws = 0;
    u64 fx_blended_capped = 0;
    u32 fx_blended_last = 0;
    u32 fx_frame_blended = 0;
    u32 fx_cap = 256;
    std::array<VideoCore::ImageId, 8> dir_shadow_inputs{};
    u32 dir_shadow_input_count = 0;
    u64 dir_shadow_frame = ~0ull;
    std::array<float, 3> shadow_color{};
    u64 direct_mask_frames = 0;
    u64 gi_sets_gpu_frames = 0;
    u64 sets_readback_frames = 0;
    VideoCore::ImageId acc_diffuse{}, acc_specular{};
    u64 acc_frame = ~0ull;
    u64 direct_frames = 0, direct_skipped = 0, direct_sun_frames = 0, direct_duplicates = 0;
    VideoCore::ImageId direct_written_diffuse{}, direct_written_specular{};
    u64 direct_written_frame = ~0ull;
    bool direct_wrote_proxy = false;
    u32 composite_logged = 0;
    u64 trace_seq = 0;
    u64 trace_until = ~0ull;
    u64 light_draw_ps = 0;
    u64 hook_ps = 0;
    u32 frame_light_draws = 0, frame_light_draws_skipped = 0, frame_direct_calls = 0;
    u64 light_draws_skipped = 0;
    u64 direct_pass_through = 0;
    Rt::DirectWrite direct_mode = Rt::DirectWrite::Overwrite;
    bool light_draws_skipped_this_frame = false;
    bool restir_enabled = false;
    u32 restir_candidates = 8;
    u32 restir_history = 20;
    u32 restir_spatial = 5;
    std::array<DeviceBuffer, 3> reservoirs{};
    u32 reservoir_width = 0, reservoir_height = 0;
    u32 reservoir_index = 0;
    u64 restir_last_frame = ~0ull;
    u64 restir_frames = 0, restir_resets = 0;
    bool fog_enabled = false;
    bool fog_stock = false;
    bool skip_volumes = true;
    std::vector<u64> sky_ps;
    bool direct_base = false;
    bool game_ao = false;
    std::vector<u64> skip_ps;
    // See-through glass (BB_RT_GLASS*, live): glossy pixels of BB_RT_GLASS_PS draws, in the
    // reflection pass.
    bool glass_on = true;
    std::vector<u64> glass_ps;
    std::vector<u32> glass_caps; // per glass_ps entry ("hash:cap"), 0 = BB_RT_GLASS_MAX_TRIS
    float glass_gloss = 0.55f;
    // BB_RT_GLASS_MAX_TRIS: a BB_RT_GLASS_PS draw is glass only when its mesh has at most this
    // many triangles: a guard in case a listed shader also draws large building meshes.
    u32 glass_max_tris = 2000;
    bool GlassDraw(u64 ps, u32 triangles) const {
        if (!glass_on || !ps) {
            return false;
        }
        for (size_t i = 0; i < glass_ps.size(); ++i) {
            if (glass_ps[i] == ps) {
                const u32 cap = glass_caps[i] ? glass_caps[i] : glass_max_tris;
                return cap == 0 || triangles <= cap;
            }
        }
        return false;
    }
    // Masked glass: a BB_RT_GLASS_PS draw (also one over the triangle cap: the workshop bottle
    // shelves mix glass and metal in one mesh) is glass only where a hand-painted mask of its
    // albedo is white (all black: no glass, the workshop mirror): <dir of BB_RT_CFG>/glass_masks/<XXH3 of the guest texture, 16 hex>.pgm (P5, 8 bit,
    // any size). The record then carries the mask's texture slot as -alpha_ref (GlassAt in the
    // shaders). BB_RT_GLASS_MASK_DUMP=1 writes each such albedo (mip 0, raw) to
    // glass_masks/dump/<hash>_<w>x<h>_<vk format>.raw for painting. Any rt.cfg change rescans.
    std::unordered_map<u64, s32> glass_mask_by_uid;  // albedo image uid -> mask image, -1 none
    std::unordered_map<u64, s32> glass_mask_by_hash; // texture hash -> mask image, -1 none
    std::vector<OwnImage> glass_mask_images;
    std::unordered_map<u64, std::pair<s64, s32>> glass_mask_files; // hash -> file time, image
    std::unordered_set<u64> glass_mask_dumped;
    bool glass_mask_dump = false;
    std::string glass_mask_dir;
    void GlassMaskSlots(vk::CommandBuffer cmdbuf);
    s32 GlassMaskLoad(vk::CommandBuffer cmdbuf, u64 hash);
    void GlassMaskDump(vk::CommandBuffer cmdbuf, const VideoCore::Image& image, u64 hash);
    // Painted windows as portals (BB_RT_PORTAL*, live): shadow rays that reach a pane of a
    // BB_RT_PORTAL_PS draw first count as reaching the outside, so the moon and lamps outside
    // shine in through the fake window. Same triangle cap as glass.
    bool portal_on = true;
    std::vector<u64> portal_ps;
    u32 portal_instances = 0;
    // Water draws in this frame's TLAS (sheets, G-buffer puddle floors): each kind's per-pixel
    // probe ray runs only while some are drawn (forest: reflect 3.0 -> 1.7 ms without them).
    u32 water_sheets = 0;
    u32 water_puddles = 0;
    bool water_on = true;
    bool water_layer = false;
    bool water_mark_layer = true;
    std::vector<u64> water_ps;
    float water_rough = 0.05f;
    float water_f0 = 0.02f;
    float water_sheet_depth = 0.03f;
    float water_murk = 5.0f;
    float water_raise = 0.1f;
    float water_body = 0.05f;
    float water_sheen = 0.0f;
    float water_sheen_far = 1.0f;
    float water_puddle_edge = 0.25f;
    float water_puddle_wet = 0.6f;
    float water_ripple = 0.1f;
    float water_sheet_f0 = 0.5f;
    float water_mist = 0.0f;
    float water_mist_height = 5.0f;
    float water_mist_albedo = 1.0f;
    u32 water_mist_steps = 8;
    float water_mist_patch = 0.5f;
    float water_mist_glow = 0.25f;
    float water_mist_glow_dark = 0.6f;
    float water_mist_fog = 5.0f;
    bool mist_sheets = true;
    float mist_sheet_scale = 0.5f;
    std::vector<Rt::FxParticle> fog_filling;
    std::vector<Rt::FxParticle> fog_done;
    u64 fog_frame = 0;
    bool water_direct_keep = true;
    float water_scatter = 0.04f;
    u32 water_debug = 0;
    bool water_at_floor = false;
    bool WaterDraw(u64 ps) const {
        return water_on && (Rt::HashListed(water_ps, ps, 0) || ps == Rt::FxMistShader);
    }
    std::vector<u64> flame_ps;
    std::vector<std::array<float, 3>> flames_filling;
    std::vector<std::array<float, 3>> flames_done;
    std::vector<std::array<float, 3>> wisps_filling;
    std::vector<std::array<float, 3>> wisps_done;
    u64 flames_frame = 0;
    [[nodiscard]] const std::vector<std::array<float, 3>>& RecentFlames() const;
    [[nodiscard]] const std::vector<std::array<float, 3>>& RecentWisps() const;
    struct GlowLamp {
        std::array<float, 3> world{};
        std::array<float, 3> color{};
        u64 seen = 0;
        u32 hits = 0;
        float drift = 0.0f; // running mean distance of a capture from the average (m)
    };
    std::vector<u64> glow_ps;
    std::vector<GlowLamp> glow_lamps;
    float glow_strength = 0.6f, glow_radius = 2.0f, glow_skip = 1.5f, glow_dist = 40.0f;
    float glow_spec_power = 16.0f;
    float glow_merge = 2.5f;
    u32 glow_max_lights = 8;
    std::array<float, 3> glow_tint{1.0f, 0.55f, 0.2f};
    std::array<float, 3> lamp_hue{1.0f, 0.61f, 0.27f};
    float lamp_warm_max = 0.6f;
    bool lamp_hue_on = true;
    u32 lamp_tinted_last = 0;
    void TintLampLights();
    u64 glow_keep = 1800;
    u32 glow_min_hits = 30;
    float glow_max_drift = 0.1f, glow_warm_max = 0.8f;
    u32 glow_added_last = 0;
    void AddGlowLights(const Rt::Affine* view);
    bool PortalDraw(u64 ps, u32 triangles) const {
        return portal_on && Rt::HashListed(portal_ps, ps, 0) &&
               (glass_max_tris == 0 || triangles <= glass_max_tris);
    }
    float glass_tint = 1.0f;
    u32 glass_debug = 0;
    bool fx_sheets = false;
    bool stock_haze = true;
    int pause_knob = -1;
    bool haze_lit = true;
    float haze_floor = 0.3f;
    float haze_gain = 1.0f;
    float haze_tint = 0.5f;
    bool skip_shadows = false;
    // Window shaft haze per metre (BB_RT_FX_FOG_SHAFT, live): rt_fog.comp ShaftDensity.
    float fog_shaft = 0.15f;
    float fog_height = 0.1f;
    float fog_strength = 1.0f;
    float fog_distance = 60.0f;
    float fog_g = 0.0f;
    // Fog albedo over sky + sun (not sky alone); 0 restores the double-counted sun for A/B.
    bool fog_sun_cal = true;
    float fog_sun_share = 0.5f;
    u32 fog_steps = 4;
    float fog_alpha_distance = 5.0f;
    u32 fog_debug = 0;
    bool sky_enabled = false;
    u32 sky_debug = 0;
    float sky_u_offset = 0.313f;
    float sky_u_repeat = 1.0f;
    float sky_v_divisor = 2048.0f;
    vk::ImageView sky_dome_view{};
    VideoCore::ImageId sky_dome_image{};
    u64 sky_dome_uid = 0;
    u64 sky_dome_frame = ~0ull;
    u64 sky_frames = 0, sky_skipped = 0;
    struct HeldDraw {
        Rt::DrawInput draw{};
        std::vector<u8> indices;
    };
    std::vector<HeldDraw> fx_held;
    struct LateDraw {
        Rt::DrawInput draw{};
        Rt::Affine inv_view{};
        bool camera_valid{};
        // The moon and cloud layer keeps its texture over to the replay (OnDrawBound).
        vk::ImageView albedo_view{};
        VideoCore::ImageId albedo_image{};
        u64 albedo_uid{};
    };
    bool last_add_late = false;
    std::vector<LateDraw> late_held;
    std::unordered_set<u32> late_entries;
    u32 late_mode = 1;
    u32 late_last = 0;
    u64 late_total = 0, late_duplicates = 0, late_dropped = 0;
    u64 fx_blended_held = 0;
    u32 fog_width = 0, fog_height_px = 0, fog_index = 0;
    u64 fog_last_frame = ~0ull;
    u64 fog_frames = 0, fog_skipped = 0;
    u64 fog_ran_frame = ~0ull;
    std::array<OwnImage, 2> fog_images;
    HostRing fog_params_ring;
    Rt::Affine fog_prev_inv_view{};
    std::array<float, 4> fog_prev_projection{};
    std::array<float, 2> fog_prev_jitter{};
    vk::DescriptorBufferInfo fog_sets_info{};
    u32 fog_set_count = 0;
    u64 fog_sets_frame = ~0ull;
    Rt::Affine restir_prev_inv_view{};
    std::array<float, 4> restir_prev_projection{};
    std::array<float, 2> restir_prev_jitter{};
    std::vector<u64> direct_keys, restir_prev_keys;
    std::vector<u32> restir_remap;
    vk::UniqueDescriptorSetLayout restir_temporal_layout, restir_spatial_layout;
    vk::UniquePipelineLayout restir_temporal_pipeline_layout, restir_spatial_pipeline_layout;
    vk::UniquePipeline restir_temporal_pipeline, restir_spatial_pipeline;
    bool CreateReservoirs(u32 width, u32 height);
    void DestroyReservoirs();
    // Moon visibility history (BB_RT_MOON_HISTORY, rt_direct.comp MoonHistory): ping-pong
    // vec2 (visibility, view z) per pixel, reprojected with the last direct frame's camera.
    std::array<DeviceBuffer, 2> moon_hist{};
    u32 moon_hist_width = 0, moon_hist_height = 0, moon_hist_index = 0;
    u64 moon_hist_frame = ~0ull, moon_hist_resets = 0, moon_hist_frames = 0;
    bool moon_history = true;
    bool moon_object_motion = true; // BB_RT_MOON_OBJECT_MOTION (live)
    u64 moon_object_frames = 0;
    ObjectMotion* object_motion = nullptr;
    Rt::Affine moon_prev_inv_view{};
    std::array<float, 4> moon_prev_projection{};
    std::array<float, 2> moon_prev_jitter{};
    bool CreateMoonHistory(u32 width, u32 height);
    void DestroyMoonHistory();
};

}

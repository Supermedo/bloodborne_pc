// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include "common/recursive_lock.h"
#include "common/shared_first_mutex.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/vk_bind_helper.h"
#include "video_core/renderer_vulkan/vk_camera_motion.h"
#include "video_core/renderer_vulkan/vk_constant_ring.h"
#include "video_core/renderer_vulkan/vk_scene_resolution.h"
#include "video_core/renderer_vulkan/vk_object_motion.h"
#include "video_core/renderer_vulkan/vk_draw_pipe.h"
#include "video_core/renderer_vulkan/vk_draw_prep.h"
#include "video_core/renderer_vulkan/vk_temporal_upscaler.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

namespace AmdGpu {
struct Liverpool;
}

namespace Core {
class MemoryManager;
}

namespace Vulkan {

class GraphicsPipeline;
class Runtime;

class Rasterizer {
public:
    explicit Rasterizer(const Instance& instance, Scheduler& scheduler, Runtime& runtime,
                        AmdGpu::Liverpool* liverpool);
    ~Rasterizer();

    [[nodiscard]] Runtime& GetRuntime() noexcept {
        return runtime;
    }

    [[nodiscard]] VideoCore::BufferCache& GetBufferCache() noexcept {
        return buffer_cache;
    }

    [[nodiscard]] TemporalUpscaler& GetUpscaler() noexcept {
        return *upscaler;
    }

    [[nodiscard]] VideoCore::TextureCache& GetTextureCache() noexcept {
        return texture_cache;
    }

    void Draw(bool is_indexed, u32 index_offset = 0, const PreparedDraw* prepared = nullptr);

    /// bbport: draw preparation workers (vk_draw_prep.h), fed and consumed by Liverpool.
    DrawPreparation& GetDrawPreparation() {
        return *draw_prep;
    }
    void DrawIndirect(bool is_indexed, VAddr arg_address, u32 offset, u32 size, u32 max_count,
                      VAddr count_address, u16 vertex_sgpr_offset, u16 instance_sgpr_offset);

    void DispatchDirect();
    void DispatchIndirect(VAddr address, u32 offset, u32 size);

    void ScopeMarker(fmt::string_view fmt, fmt::format_args args, auto&& func) {
        if (host_markers_enabled) {
            ScopeMarkerBegin(fmt::vformat(fmt, args));
            func();
            ScopeMarkerEnd();
        } else {
            func();
        }
    }

    void ScopeMarkerBegin(const std::string_view& str, bool from_guest = false);
    void ScopeMarkerEnd(bool from_guest = false);
    void ScopedMarkerInsert(const std::string_view& str, bool from_guest = false);
    void ScopedMarkerInsertColor(const std::string_view& str, const u32 color,
                                 bool from_guest = false);

    void FillBuffer(VAddr address, u32 num_bytes, u32 value, bool is_gds);
    void CopyBuffer(VAddr dst, VAddr src, u32 num_bytes, bool dst_gds, bool src_gds);
    /// bbport: whether FillBuffer/CopyBuffer into guest memory at `dst` may write it on the CPU
    /// now (otherwise the GPU writes it, in stream order).
    [[nodiscard]] bool DmaMayWriteOnCpu(VAddr dst, u32 num_bytes);
    u32 ReadDataFromGds(u32 gsd_offset);
    bool InvalidateMemory(VAddr addr, u64 size, bool assume_locks = false);
    /// bbport: data written into GPU memory by a path we hear of (file reads, the game's resource
    /// loaders, DMA on the CPU): invalidated, and an asset for the buffer cache.
    void NoteAssetWrite(VAddr addr, u64 size);
    /// bbport: the CPU has written [addr, addr + size): invalidated, its bytes kept over GPU data that
    /// is read back first.
    void InvalidateAfterWrite(VAddr addr, u64 size);
    /// bbport (any thread): the command processor wrote these bytes outside the command stream
    /// (labels, timestamps, occlusion results): VRAM copies of them get them too.
    void NoteLateCommandWrite(VAddr addr, const void* data, u64 size) {
        buffer_cache.NoteLateCommandWrite(addr, data, size);
    }
    /// bbport: the command processor wrote `size` bytes of `data` at addr with the CPU in decode order
    /// (WriteData, a constant RAM dump; the decoder reads them there). Where the GPU's own data there
    /// is in VRAM the bytes go into it in stream order, else the caches are invalidated (no write
    /// tracking). `recording_thread`: the draw recording thread, which owns the caches (no drain).
    void NoteCommandWrite(VAddr addr, const void* data, u64 size, bool recording_thread);
    /// bbport (stage A, BB_CONSTRAM_PIPE, default on): before the command processor writes guest
    /// memory with the CPU (a constant RAM dump), waits until the recording thread has run the
    /// queued packets that read those bytes (their buffer bindings, DMA), not all of them.
    void WaitForPendingReads(VAddr address, u64 size);
    /// NoteCommandWrite for a write made on stage A: handed to the recording thread in order (a copy
    /// of the bytes) instead of draining it first.
    void NoteCommandWriteInOrder(VAddr addr, const void* data, u64 size);
    /// bbport: a guest thread wrote [addr, addr + size) with a libc import (memcpy, memset, memmove)
    /// over pages the caches watch (no write tracking): what a write fault did.
    void OnCpuWrite(VAddr addr, u64 size);
    /// GPU thread, before a write the guest can observe (see Scheduler::WaitHostCopies).
    void WaitHostCopies() {
        DrainDrawPipe();
        scheduler.WaitHostCopies();
    }
    /// Before a write into [address, address + size): waits only when a host copy may still read
    /// it (Scheduler::WaitHostCopiesFor). BB_HOST_COPY_WAITS=all: always (the old behaviour).
    void WaitHostCopiesFor(VAddr address, u64 size) {
        DrainDrawPipe();
        static const bool all = [] {
            const char* env = std::getenv("BB_HOST_COPY_WAITS");
            return env && std::string_view{env} == "all";
        }();
        if (all) {
            scheduler.WaitHostCopies();
        } else {
            scheduler.WaitHostCopiesFor(address, size);
        }
    }
    /// Notes the guest memory a host copy issued now reads (see Scheduler::NoteHostCopySource).
    void NoteHostCopySource(VAddr address, u64 size) {
        scheduler.NoteHostCopySource(address, size);
    }

    /// bbport: GPU command thread: waits until the draw recording thread has recorded every
    /// draw handed to it (vk_draw_pipe.h); no-op on other threads.
    /// `line`/`function`: the caller, for the statistics of where stage A waits.
    void DrainDrawPipe(u32 reason = DrawPipe::ReasonRasterizer, u32 line = __builtin_LINE(),
                       const char* function = __builtin_FUNCTION());
    /// Runs `task(rasterizer, copy of data)` in order with the draws: on the draw recording
    /// thread while the draw pipeline is in use (PipelinedTasks), else here after a drain.
    using OrderedTask = void (*)(Rasterizer& rasterizer, const u8* data);
    /// Returns true when the task was handed to the recording thread (not run yet).
    /// `reads_guest_memory`: the task may read guest memory (a CPU write on stage A over memory
    /// waits for it, WaitForPendingReads); false for fences, flips and signals.
    bool RunInOrder(OrderedTask task, const void* data, u32 size,
                    u64 toggle = BbToggle::PipelinedTasks, bool reads_guest_memory = true);
    /// Stage A: the draw pipe position after the last handed-over packet, and whether the
    /// recording thread has run everything before a position.
    [[nodiscard]] u64 DrawPipeHead() const {
        return draw_pipe ? draw_pipe->Head() : 0;
    }
    [[nodiscard]] bool DrawPipeReached(u64 position) const {
        return !draw_pipe || draw_pipe->Reached(position);
    }
    /// Stage A: no draws waiting for the recording thread.
    [[nodiscard]] bool DrawPipeIdle() const {
        return !draw_pipe || draw_pipe->Idle();
    }
    /// Stage A, end of a submission: its prepared draws stay alive until the recording thread
    /// has recorded them (instead of a drain).
    void RetireSubmission();
    /// Stage A: guest memory the recording thread will write for work handed to it (storage
    /// buffers, DMA, WriteData, fences); constants overlapping it are bound there, not copied here.
    void NotePendingGpuWrite(VAddr address, u64 size);
    /// Stage A: guest memory the recording thread will read for the next packet (buffer bindings).
    void NotePendingRead(VAddr address, u64 size);
    /// Before a guest-visible write: fences deferred earlier are written first.
    void WaitDeferredSignals() {
        scheduler.WaitDeferredSignals();
    }
    /// Runs `signal` after the guest memory copies issued so far, without waiting here.
    void SignalAfterHostCopies(std::function<void()> signal) {
        scheduler.SignalAfterHostCopies(std::move(signal));
    }
    /// bbport BB_HONEST_LABELS=1: runs `signal` once the GPU has finished every command recorded
    /// before this call (the submission of the current tick), in call order, on a thread that
    /// waits on the work semaphore: fences the guest sees when the work is done, not when it is
    /// recorded. `label` and `value`: the fence it writes (PendingSignalValue).
    void SignalAfterGpu(std::function<void()> signal, VAddr label = 0, u64 value = 0);
    /// Whether signals wait for work not submitted yet (the GPU command thread submits it when
    /// it runs out of work, or the guest could wait for them forever).
    bool HasUnsubmittedSignals();
    /// bbport BB_GUEST_IN_PLACE (draw recording thread): `data` written into the game's memory by the
    /// GPU in stream order, like the command processor's writes; false when the range is not
    /// bound in place (the caller writes it with the CPU).
    bool WriteGuestMemory(VAddr address, const void* data, u32 size);
    /// bbport BB_GUEST_IN_PLACE (in stream order: the recording thread): a WRITE_DATA performed by
    /// the GPU, as the command processor does, instead of a CPU store now (ahead of the GPU work
    /// recorded before it). Values of up to 8 bytes are registered for WAIT_REG_MEM until the GPU
    /// has written them. False: not in place (or BB_GPU_COMMAND_WRITES=0), the caller stores it.
    bool WriteDataOnGpu(VAddr address, const void* data, u32 size);
    /// End of a guest submission: submits the work recorded so far when signals wait for it and
    /// the last submission is BB_HONEST_FLUSH_US (1000) old: the GPU starts on it as the hardware
    /// would, instead of at the end of the frame, and the guest's mid-frame waits end sooner.
    void SubmitForSignals();
    /// bbport BB_PIPE_SIGNALS (default on with honest labels; 0: off): on the GPU command thread with
    /// the draw pipe, fences, the GPU idle and frame signals and the submissions for them are
    /// handed to the draw recording thread as ordered tasks instead of that thread being drained
    /// first (the GPU command thread waited for it ~15 times a frame and the GPU ran dry meanwhile).
    bool PipeSignals() const;
    /// Stage A: the fences the recording thread ran up to `position` are in the signal queue (or
    /// written): a fence it ran but holds still (PublishSignals) is not visible there yet.
    bool SignalsPublished(u64 position) const;
    /// Stage A (BB_PIPE_SIGNALS): the recording thread hands over its signals and submits the work
    /// they wait for, in order, when it gets there (one request in flight at a time).
    void RequestSignalFlush();
    u64 CurrentTick() const {
        return scheduler.CurrentTick();
    }
    /// The newest queued value of the fence at `address` (a WaitRegMem in the same stream is met
    /// by it in stream order).
    bool PendingSignalValue(VAddr address, u64& value);
    static bool HonestLabels();
    /// The GPU command thread or the draw recording thread (fault handling runs inline there).
    bool IsGpuSideThread() const;
    bool IsGpuSideThreadId(u32 tid) const;
    /// A guest write hit a protected page.
    /// `guest_rip`: the guest instruction that wrote (0: unknown), see BufferCache::NoteCpuWrite.
    bool OnWriteFault(VAddr addr, bool assume_locks, u64 guest_rip = 0);
    bool ReadMemory(VAddr addr, u64 size, bool assume_locks = false);
    void ProcessDownloadImages();
    bool IsMapped(VAddr addr, u64 size);
    void MapMemory(VAddr addr, u64 size);
    void RegisterMemory(VAddr addr, u64 size);
    void UnmapMemory(VAddr addr, u64 size);

    u64 Flush();
    void Finish();
    void OnSubmit();

    PipelineCache& GetPipelineCache() {
        return pipeline_cache;
    }

    template <typename Func>
    void ForEachMappedRangeInRange(VAddr addr, u64 size, Func&& func) {
        const auto range = decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);
        Common::RecursiveSharedLock lock{mapped_ranges_mutex};
        for (const auto& mapped_range : (mapped_ranges & range)) {
            func(mapped_range);
        }
    }

    std::thread::id GetGpuCommandProcessorThread();
#if defined(__linux__) || defined(_WIN32)
    u32 GetGpuCommandProcessorThreadId();
#endif

private:
    void PrepareRenderState(const GraphicsPipeline* pipeline);
    RenderState BeginRendering(const GraphicsPipeline* pipeline);
    RenderState BeginRenderingFull(const GraphicsPipeline* pipeline);
    /// bbport: a draw continuing the open render pass with the same inputs gets the same render
    /// state (RenderStateMemo); nothing can have broken the pass in between (barriers, copies and
    /// dispatches end it).
    struct BeginSignature {
        std::array<VideoCore::ImageId, AmdGpu::NUM_COLOR_BUFFERS + 1> ids{};
        std::array<VideoCore::ImageViewInfo, AmdGpu::NUM_COLOR_BUFFERS + 1> views{};
        u32 mrt_mask = 0;
        u32 num_samples = 0;
        std::array<u8, AmdGpu::NUM_COLOR_BUFFERS> color_samples{};
        bool motion = false;
        bool scene_started = false;
        bool raster_scaling = false;
        u64 upscaler_state = 0;
        u64 generation = 0;
        u32 depth_control = 0;
        bool depth_valid = false;
        bool stencil_valid = false;
        bool operator==(const BeginSignature&) const = default;
    };
    BeginSignature MakeBeginSignature(const GraphicsPipeline* pipeline) const;
    struct BeginMemo {
        bool valid = false;
        BeginSignature signature;
        RenderState state;
        u32 scene_size = 0;
        std::array<float, 2> target_scale{};
    } begin_memo;
    u64 begin_memo_hits = 0, begin_memo_misses = 0;
    void Resolve();
    void DepthStencilCopy(bool is_depth, bool is_stencil);
    void EliminateFastClear();

    void UpdateDynamicState(const GraphicsPipeline* pipeline, bool is_indexed) const;
    void UpdateViewportScissorState() const;
    void UpdateDepthStencilState() const;
    void UpdatePrimitiveState(bool is_indexed) const;
    void UpdateRasterizationState() const;
    void UpdateColorBlendingState(const GraphicsPipeline* pipeline) const;

    bool FilterDraw();
    bool FilterDrawPasses() const;
    /// Everything of a direct draw after the pipeline selection (GPU thread or stage B).
    void DrawRecord(const GraphicsPipeline* pipeline, const PreparedDraw* used_prepared,
                    bool is_indexed, u32 index_offset);
    static bool DrawPipeWanted();
    bool UseDrawPipe() const;
    bool OnStageA() const;
    /// Arguments of an indirect draw: `args` holds `max_count` commands `stride` apart.
    struct IndirectDraw {
        VAddr args;
        VAddr count; ///< the draw count, or 0 for max_count draws
        u32 stride;
        u32 max_count;
    };
    /// Everything of an indirect draw after the pipeline selection (GPU thread or stage B).
    void DrawIndirectRecord(const GraphicsPipeline* pipeline, bool is_indexed,
                            const IndirectDraw& indirect);
    /// Everything of an indirect dispatch after the pipeline selection.
    void DispatchIndirectRecord(const ComputePipeline* pipeline, VAddr args, u32 size);
    /// Hands a draw (or, with `cs`, a dispatch) to the recording thread.
    void PostDraw(const Pipeline* pipeline, const PreparedDraw* used_prepared, bool is_indexed,
                  u32 index_offset, const AmdGpu::ComputeProgram* cs = nullptr,
                  const IndirectDraw* indirect = nullptr);
    /// Everything of a direct dispatch after the pipeline selection (GPU thread or stage B).
    void DispatchRecord(const ComputePipeline* pipeline);
    /// The compute registers of the dispatch being recorded.
    const AmdGpu::ComputeProgram& CsRegs() const;
    static void RunDrawPacket(void* rasterizer, const u8* packet, u32 size);
    struct RingBinding;
    void CollectRingBindings(const Shader::Info& stage, const PreparedDraw* prepared,
                             boost::container::static_vector<RingBinding, Shader::NUM_BUFFERS>& out);
    void PrintPipeStats();
    /// The registers of the draw being recorded: stage B's copy there, else Liverpool's.
    const AmdGpu::Regs& Regs() const;
    AmdGpu::CbDbExtent CbExtent(u32 index) const;
    AmdGpu::CbDbExtent DbExtent() const;

    void BindBuffers(const Shader::Info& stage, const PreparedStage* prepared,
                     Shader::Backend::Bindings& binding,
                     Shader::PushData& push_data, u32& write_index);
    /// `on_helper`: run by the texture binding helper; out-of-date images are not refreshed.
    void BindTextures(const Shader::Info& stage, const PreparedStage* prepared,
                      Shader::Backend::Bindings& binding, u32& write_index, bool& barrier,
                      bool on_helper = false);
    bool BindResources(const Pipeline* pipeline);
    void ForceAnisotropy(AmdGpu::Sampler& sharp, bool is_depth) const;
    void BindSamplers(const Shader::Info& stage, const PreparedStage* prepared,
                      Shader::Backend::Bindings& binding, u32& write_index);
    /// bbport: a stage's resolved textures remembered by its prepared T# hashes
    /// (TextureSetMemo): a hit only redoes the per-draw effects (binding flags, transitions).
    struct TextureSetEntry {
        VideoCore::ImageId id{}; ///< after the depth redirect; null descriptor when invalid
        vk::ImageView view;
        const void* backing = nullptr;
        VideoCore::SubresourceRange range;
        bool proxy = false; ///< sampled from the scene proxy (view: SampleProxy on each use)
    };
    struct TextureSet {
        u64 key = 0;
        const Shader::Info* stage = nullptr;
        u64 generation = ~0ull;
        u64 scene_generation = 0; ///< SceneTargets::Generation of the proxy views
        u32 count = 0;
        static constexpr u32 MaxImages = 16; ///< larger sets are not memoized
        std::array<u64, MaxImages> hashes{};
        std::array<TextureSetEntry, MaxImages> entries{};
    };
    std::array<TextureSet, 32768> texture_sets{};
    u64 texture_set_hits = 0, texture_set_misses = 0;
    std::array<u64, 4> texture_set_why{}; ///< misses: other key, generation, image check, new
    /// Returns true when the stage's images were bound from the memo.
    bool BindTexturesFromSet(const Shader::Info& stage, const PreparedStage* prepared,
                             u32 first_image_idx, bool& barrier, TextureSet*& slot);
    /// The prepared sharps of `stage` in the current prepared draw, if they fit it.
    const PreparedStage* FindPreparedStage(const Shader::Info& stage) const;
    /// Whether the helper may bind textures for this pipeline at all (fixed descriptor layout,
    /// no storage writes that invalidate cached images).
    bool HelperEligible(const Pipeline* pipeline) const;
    /// Whether every texture of `stage` takes the memoized path without refreshing, creating or
    /// redirecting anything that records commands or touches the buffer cache.
    bool TexturesBindableOnHelper(const Shader::Info& stage, const PreparedStage* prepared);
    static void RunTextureTask(void* rasterizer);
    static void JoinBindHelper(void* rasterizer);

    /// Display pass: counts the frame (BbStats::gpu_frames) and, with BB_BUFFER_STATS, the lag.
    void NoteFrameStart();
    /// BB_GPU_PROFILE: a timestamp where a render pass starts (vk_gpu_profiler.h).
    void MarkPass(const GraphicsPipeline* pipeline, const RenderState& state);
    void BindVertexBuffers(const GraphicsPipeline* pipeline,
                           const PreparedDraw* prepared = nullptr);
    void BindIndexBuffer(u32 index_offset = 0);
    /// bbport: BindVertexBuffers/BindIndexBuffer in two halves: obtaining the buffers
    /// (vertex_binds, index_bind) and recording the binds.
    void ResolveVertexBuffers(const GraphicsPipeline* pipeline, const PreparedDraw* prepared);
    void EmitVertexBuffers();
    void ResolveIndexBuffer(u32 index_offset);
    void EmitIndexBuffer();

    void ResetBindings(bool is_compute);

    bool IsComputeMetaClear(const Pipeline* pipeline);
    bool IsComputeImageCopy(const Pipeline* pipeline);
    bool IsComputeImageClear(const Pipeline* pipeline);

private:
    friend class VideoCore::BufferCache;

    const Instance& instance;
    Scheduler& scheduler;
    Runtime& runtime;
    VideoCore::PageManager page_manager;
    VideoCore::BufferCache buffer_cache;
    VideoCore::TextureCache texture_cache;
    AmdGpu::Liverpool* liverpool;
    Core::MemoryManager* memory;
    boost::icl::interval_set<VAddr> mapped_ranges;
    Common::SharedFirstMutex mapped_ranges_mutex;
    PipelineCache pipeline_cache;
    std::unique_ptr<DrawPreparation> draw_prep;
    std::unique_ptr<CameraMotion> camera_motion; // bbport: motion vectors (docs/upscaler.md)
    std::unique_ptr<SceneTargets> scene_targets;
    bool scene_started = false;
    std::unique_ptr<ObjectMotion> object_motion;
    bool motion_draw = false;
    u64 motion_geometry{};    ///< vertex-stream identity of the current direct draw
    bool gbuffer_draw = false;
    std::unique_ptr<TemporalUpscaler> upscaler; // bbport: FSR (docs/upscaler.md)
    std::array<float, 2> draw_jitter{};         ///< viewport offset of the current draw, pixels
    std::array<float, 2> target_scale{1.0f, 1.0f}; ///< pass drawn into the upscaler's output-size images
    const bool host_markers_enabled;
    const bool guest_markers_enabled;

    using RenderTargetInfo = std::pair<VideoCore::ImageId, VideoCore::TextureCache::ImageDesc>;
    std::array<RenderTargetInfo, AmdGpu::NUM_COLOR_BUFFERS> cb_descs;
    std::pair<VideoCore::ImageId, VideoCore::TextureCache::ImageDesc> db_desc;
    boost::container::static_vector<vk::DescriptorImageInfo, Shader::NUM_IMAGES> image_infos;
    boost::container::static_vector<vk::DescriptorBufferInfo, Shader::NUM_BUFFERS> buffer_infos;
    boost::container::static_vector<VideoCore::ImageId, Shader::NUM_IMAGES> bound_images;
    struct BoundBuffer {
        const VideoCore::Buffer* buffer;
        u64 offset;
        u32 size;
        bool is_written;
    };
    boost::container::static_vector<BoundBuffer, Shader::NUM_BUFFERS> bound_buffers;

    u32 set_write_index{};
    Pipeline::DescriptorWrites set_writes;
    Shader::PushData push_data;

    // bbport: bindings point at their description instead of copying it (a hot spot): into
    // image_desc_cache for memoized lookups (pinned for the current BindTextures call), else
    // into image_desc_storage.
    using ImageBindingInfo = std::pair<VideoCore::ImageId, const VideoCore::TextureCache::ImageDesc*>;
    // bbport: texture descriptions depend only on the T# and three resource flags; building
    // them (mip layout sizes) for every binding of every draw was a hot spot.
    struct ImageDescCacheEntry {
        std::array<u64, 4> sharp{};
        u32 flags = ~0u;
        VideoCore::TextureCache::ImageDesc desc;
        // Memoized FindImage for bindings without mip overrides (TextureBindingMemo).
        u64 found_generation = ~0ULL;
        VideoCore::ImageId found_id{};
        VideoCore::TextureCache::ImageDesc found_desc;
        u64 pinned = 0; ///< bind_epoch of the BindTextures call referencing found_desc
        u64 last_use = 0;
        // Memoized FindView for found_id (after the depth redirect): valid while the entry's
        // FindImage memo holds and the image keeps this backing (TextureViewMemo).
        VideoCore::TextureCache::ViewMemo view_memo;
    };
    std::array<ImageDescCacheEntry, 4096> image_desc_cache{};
    u64 desc_use_counter = 0;
    /// Entries replacing pinned cache slots during one BindTextures call.
    boost::container::static_vector<ImageDescCacheEntry, Shader::NUM_IMAGES> image_desc_overflow;
    boost::container::static_vector<VideoCore::TextureCache::ImageDesc, Shader::NUM_IMAGES * 2>
        image_desc_storage;
    u64 bind_epoch = 0;
    // bbport: render/depth target lookups memoized by their raw register bytes while image
    // registrations are unchanged (the descriptions depend only on those registers).
    struct TargetMemo {
        std::array<u8, 256> key{};
        u32 key_size = 0;
        u64 generation = ~0ULL;
        VideoCore::ImageId image_id{};
        VideoCore::TextureCache::ImageDesc desc;
    };
    std::array<TargetMemo, 64> target_memo{};
    // bbport: consecutive draws mostly keep their targets; the slot's description (cb_descs,
    // db_desc) is then still the right one and is neither looked up nor copied.
    struct LastTarget {
        std::array<u8, 256> key{};
        u32 key_size = 0;
        u64 generation = ~0ULL;
        VideoCore::ImageId image_id{};
    };
    std::array<LastTarget, AmdGpu::NUM_COLOR_BUFFERS + 1> last_targets{}; ///< CBs, then DB
    template <typename... Parts>
    VideoCore::ImageId FindTargetMemoized(VideoCore::TextureCache::ImageDesc& desc,
                                          LastTarget& last, auto&& make_desc,
                                          const Parts&... parts);
    /// The prepared draw whose pipeline the current Draw uses (its sharps are valid), or null.
    const PreparedDraw* bind_prepared = nullptr;
    /// `hash` is ImageDescHash(sharp, res), computed by a draw-preparation worker or here.
    ImageDescCacheEntry& CachedImageDescEntry(const AmdGpu::Image& sharp,
                                              const Shader::ImageResource& res, u64 hash);
    ImageDescCacheEntry& CachedImageDescEntry(const AmdGpu::Image& sharp,
                                              const Shader::ImageResource& res) {
        return CachedImageDescEntry(sharp, res, ImageDescHash(sharp, res));
    }
    const VideoCore::TextureCache::ImageDesc& CachedImageDesc(const AmdGpu::Image& sharp,
                                                              const Shader::ImageResource& res) {
        return CachedImageDescEntry(sharp, res).desc;
    }
    boost::container::static_vector<ImageBindingInfo, Shader::NUM_IMAGES> image_bindings;
    struct VertexBinds {
        VertexInputs<vk::VertexInputAttributeDescription2EXT> attributes;
        VertexInputs<vk::VertexInputBindingDescription2EXT> bindings;
        VertexInputs<vk::Buffer> host_buffers;
        VertexInputs<vk::DeviceSize> host_offsets;
        VertexInputs<vk::DeviceSize> host_sizes;
        VertexInputs<vk::DeviceSize> host_strides;
        u32 num_buffers = 0;
    } vertex_binds;
    struct IndexBind {
        vk::Buffer handle;
        u64 offset;
        vk::IndexType type;
    } index_bind{};
    /// The direct draw whose vertex/index buffers BindResources may resolve early.
    struct DrawInputs {
        const GraphicsPipeline* pipeline;
        const PreparedDraw* prepared;
        u32 index_offset;
        bool is_indexed;
        bool pending;
        bool resolved;
    } draw_inputs{};
    // bbport: textures of a draw bound on the helper thread while this thread binds buffers.
    static bool BindHelperWanted();
    BindHelper bind_helper{BindHelperWanted()};
    struct TextureTask {
        struct Stage {
            const Shader::Info* info;
            const PreparedStage* prepared;
            Shader::Backend::Bindings binding;
            u32 write_index;
        };
        std::array<Stage, Shader::MaxStageTypes> stages;
        u32 count = 0;
        u32 completed = 0; ///< stages the helper bound; the rest are bound after the join
        bool barrier = false;
    } texture_task;
    /// Draws whose textures the helper bound completely, partly or not at all (BB_FRAME_STATS).
    u64 helper_full = 0, helper_partial = 0, helper_serial = 0;
    /// The memoized cache entry of each image binding (FindImage memo taken), else null.
    boost::container::static_vector<ImageDescCacheEntry*, Shader::NUM_IMAGES> image_binding_entries;
    bool fault_process_pending{};
    bool attachment_feedback_loop{};
    bool needs_barrier{};
    // bbport: two-stage draw pipeline (stage B state; the thread is the last member so it
    // stops first).
    static inline thread_local const AmdGpu::Regs* stage_regs = nullptr;
    static inline thread_local const AmdGpu::ComputeProgram* stage_cs = nullptr;
    AmdGpu::Regs pipe_regs{};
    bool pipe_synced = false;
    std::array<AmdGpu::CbDbExtent, AmdGpu::NUM_COLOR_BUFFERS> pipe_cb_extent{};
    AmdGpu::CbDbExtent pipe_db_extent{};
    /// Stage A: small read-only guest buffers copied into the constant ring for a packet.
    struct RingBinding {
        u32 index;  ///< buffer resource of the stage
        u32 size;
        u64 offset; ///< in the ring
        VAddr address;
    };
    /// Stage A: the newest draw pipe packet that reads guest memory, hashed per granule: 256 bytes
    /// for bindings up to 4 KiB, 16 KiB up to 256 KiB, 1 MiB above (a note costs a few granules,
    /// a lookup checks all three). A collision only makes a write wait longer.
    struct PendingReads {
        static constexpr u64 Shifts[3] = {8, 14, 20};
        static constexpr u64 Limits[2] = {4_KB, 256_KB};
        static constexpr u32 Bits = 16;
        std::array<std::vector<u64>, 3> tables{std::vector<u64>(1u << Bits),
                                               std::vector<u64>(1u << Bits),
                                               std::vector<u64>(1u << Bits)};
        static u64 Slot(u64 granule, u32 level) {
            return ((granule * 4 + level) * 0x9E3779B97F4A7C15ull) >> (64 - Bits);
        }
        void Note(VAddr address, u64 size, u64 packet) {
            const u32 level = size <= Limits[0] ? 0 : size <= Limits[1] ? 1 : 2;
            auto& table = tables[level];
            for (u64 g = address >> Shifts[level]; g <= (address + size - 1) >> Shifts[level];
                 ++g) {
                u64& slot = table[Slot(g, level)];
                slot = std::max(slot, packet);
            }
        }
        u64 Newest(VAddr address, u64 size) const {
            u64 newest = 0;
            for (u32 level = 0; level < 3; ++level) {
                for (u64 g = address >> Shifts[level]; g <= (address + size - 1) >> Shifts[level];
                     ++g) {
                    newest = std::max(newest, tables[level][Slot(g, level)]);
                }
            }
            return newest;
        }
    };
    PendingReads pending_reads;
    PendingReads pending_write_table; ///< the same per granule for GPU writes queued (NotePendingGpuWrite)
    u64 untracked_reads_packet = 0; ///< newest packet reading guest memory it does not list
    u64 constram_waits_skipped = 0; ///< statistics: CPU writes that did not have to wait
    u64 proxy_samples = 0; ///< texture bindings that read a scene proxy (statistics)
    float sampler_lod_bias = 0.0f; ///< bbport: extra bias of this draw's samplers
    bool pipeline_is_compute = false; ///< bbport: the bound pipeline of BindResources
    bool scene_debug_frame = false; ///< BB_SCENE_DEBUG: this frame's passes are printed
    u32 preupload_frame = ~0u; ///< bbport: frame of the last background pre-upload
    bool PendingWriteOverlaps(VAddr address, u64 size);
    /// Stage B: the ring bindings of the stages of the packet being recorded.
    struct RingStage {
        const Shader::Info* info;
        const RingBinding* bindings;
        u32 count;
    };
    std::array<RingStage, Shader::MaxStageTypes> ring_stages{};
    u32 num_ring_stages = 0;
    /// bbport: the vertex stream V#s of the packet being recorded (stage B; empty elsewhere).
    std::span<const AmdGpu::Buffer> packet_vsharps;
    const RingBinding* FindRingBinding(const Shader::Info& stage, u32 index) const {
        for (u32 i = 0; i < num_ring_stages; ++i) {
            if (ring_stages[i].info == &stage) {
                for (u32 j = 0; j < ring_stages[i].count; ++j) {
                    if (ring_stages[i].bindings[j].index == index) {
                        return &ring_stages[i].bindings[j];
                    }
                }
                return nullptr;
            }
        }
        return nullptr;
    }
    std::unique_ptr<ConstantRing> constant_ring;
    /// Submissions (prepared draws) kept alive until stage B reaches the position.
    std::deque<std::pair<u64, std::shared_ptr<const void>>> pipe_keepalive;
    std::unique_ptr<DrawPipe> draw_pipe;

    /// BB_HONEST_LABELS (SignalAfterGpu). Last: its thread stops before the scheduler goes.
    struct GpuSignal {
        u64 tick;
        std::function<void()> signal;
        VAddr label;
        u64 value;
    };
    void GpuSignalLoop(std::stop_token token);
    /// bbport: GpuSignalLoop has waited 10 s for `tick`: prints whether the GPU or the port is stuck.
    void GpuWatchdog(u64 tick);
    std::mutex gpu_signals_mutex;
    std::condition_variable_any gpu_signals_cv;
    std::deque<GpuSignal> gpu_signals;
    /// bbport: signals registered on the draw recording thread since it last handed them over
    /// (PublishSignals: at the end of each guest submission and at every flush), one lock per
    /// batch instead of per fence (~450 fences a frame).
    std::vector<GpuSignal> local_signals;
    std::atomic<u32> local_signal_count{0};
    void PublishSignals();
    /// Draw pipe position up to which the recording thread's signals are handed over: fences
    /// handed to it before are in gpu_signals (or written).
    std::atomic<u64> signals_published_upto{0};
    std::atomic<bool> signal_flush_requested{false};
    std::jthread gpu_signal_thread;
    void SubmitForSignalsNow();
    /// steady_clock nanoseconds of the last submission (written by both pipeline stages).
    std::atomic<s64> last_flush_ns{0};
};

} // namespace Vulkan

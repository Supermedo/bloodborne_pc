// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <utility>
#include <vector>
#include <mutex>
#include <thread>
#include <queue>

#include "bbport_toggles.h"
#include "common/assert.h"
#include "common/interval_set.h"
#include "common/unique_function.h"
#include "video_core/amdgpu/regs_color.h"
#include "video_core/amdgpu/regs_primitive.h"
#include "video_core/renderer_vulkan/vk_breadcrumbs.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"

namespace tracy {
class VkCtxScope;
}

namespace Vulkan {

class Instance;

struct RenderAttachment {
    vk::ImageView image_view;
    vk::ImageLayout image_layout;
    std::array<u32, 4> clear_value;
    union {
        u32 is_clear;
        struct {
            bool has_depth;
            bool depth_clear;
            bool has_stencil;
            bool stencil_clear;
        };
    };
};
static_assert(std::has_unique_object_representations_v<RenderAttachment>);

struct RenderState {
    std::array<RenderAttachment, 8> color_attachments;
    RenderAttachment depth_stencil_attachment;
    u16 width;
    u16 height;
    u16 num_layers;
    u16 num_color_attachments;

    bool operator==(const RenderState& other) const noexcept {
        return std::memcmp(this, &other, sizeof(RenderState)) == 0;
    }
};
static_assert(std::has_unique_object_representations_v<RenderState>);

struct SubmitInfo {
    std::array<vk::Semaphore, 4> wait_semas;
    std::array<u64, 4> wait_ticks;
    std::array<vk::Semaphore, 4> signal_semas;
    std::array<u64, 4> signal_ticks;
    vk::Fence fence;
    u32 num_wait_semas;
    u32 num_signal_semas;
    /// bbport: queue operations that go out right before the submission, in queue order (sparse
    /// binds): with BB_ASYNC_SUBMIT the submission itself goes out later, on a recording thread.
    std::vector<std::function<void()>> before_submit;

    void AddWait(vk::Semaphore semaphore, u64 tick = 1) {
        wait_semas[num_wait_semas] = semaphore;
        wait_ticks[num_wait_semas++] = tick;
    }

    void AddSignal(vk::Semaphore semaphore, u64 tick = 1) {
        signal_semas[num_signal_semas] = semaphore;
        signal_ticks[num_signal_semas++] = tick;
    }

    void AddSignal(vk::Fence fence) {
        this->fence = fence;
    }
};

using Viewports = boost::container::static_vector<vk::Viewport, AmdGpu::NUM_VIEWPORTS>;
using Scissors = boost::container::static_vector<vk::Rect2D, AmdGpu::NUM_VIEWPORTS>;
using ColorWriteMasks = std::array<vk::ColorComponentFlags, AmdGpu::NUM_COLOR_BUFFERS>;
struct StencilOps {
    vk::StencilOp fail_op{};
    vk::StencilOp pass_op{};
    vk::StencilOp depth_fail_op{};
    vk::CompareOp compare_op{};

    bool operator==(const StencilOps& other) const {
        return fail_op == other.fail_op && pass_op == other.pass_op &&
               depth_fail_op == other.depth_fail_op && compare_op == other.compare_op;
    }
};
struct DynamicState {
    struct {
        bool viewports : 1;
        bool scissors : 1;

        bool depth_test_enabled : 1;
        bool depth_write_enabled : 1;
        bool depth_compare_op : 1;

        bool depth_bounds_test_enabled : 1;
        bool depth_bounds : 1;

        bool depth_bias_enabled : 1;
        bool depth_bias : 1;

        bool stencil_test_enabled : 1;
        bool stencil_front_ops : 1;
        bool stencil_front_reference : 1;
        bool stencil_front_write_mask : 1;
        bool stencil_front_compare_mask : 1;
        bool stencil_back_ops : 1;
        bool stencil_back_reference : 1;
        bool stencil_back_write_mask : 1;
        bool stencil_back_compare_mask : 1;

        bool primitive_restart_enable : 1;
        bool rasterizer_discard_enable : 1;
        bool cull_mode : 1;
        bool front_face : 1;

        bool blend_constants : 1;
        bool color_write_masks : 1;
        bool line_width : 1;
        bool feedback_loop_enabled : 1;
    } dirty_state{};

    Viewports viewports{};
    Scissors scissors{};

    bool depth_test_enabled{};
    bool depth_write_enabled{};
    vk::CompareOp depth_compare_op{};

    bool depth_bounds_test_enabled{};
    float depth_bounds_min{};
    float depth_bounds_max{};

    bool depth_bias_enabled{};
    float depth_bias_constant{};
    float depth_bias_clamp{};
    float depth_bias_slope{};

    bool stencil_test_enabled{};
    StencilOps stencil_front_ops{};
    u32 stencil_front_reference{};
    u32 stencil_front_write_mask{};
    u32 stencil_front_compare_mask{};
    StencilOps stencil_back_ops{};
    u32 stencil_back_reference{};
    u32 stencil_back_write_mask{};
    u32 stencil_back_compare_mask{};

    bool primitive_restart_enable{};
    bool rasterizer_discard_enable{};
    vk::CullModeFlags cull_mode{};
    vk::FrontFace front_face{};

    std::array<float, 4> blend_constants{};
    ColorWriteMasks color_write_masks{};
    float line_width{};
    bool feedback_loop_enabled{};

    /// Commits the dynamic state to the provided command buffer (a null one only clears the
    /// flags that committing clears).
    void Commit(const Instance& instance, const vk::CommandBuffer& cmdbuf);

    /// bbport: passes each dirty state change to `emit` as a small command closure holding only
    /// its own values (recording a copy of the whole state per draw was a hot spot), and clears
    /// the flags it emitted. Flags whose test is disabled stay dirty, as in Commit.
    template <typename Emit>
    void CommitWith(bool depth_bounds_supported, bool color_write_mask_supported,
                    bool feedback_loop_supported, Emit&& emit) {
        using Face = vk::StencilFaceFlagBits;
        if (dirty_state.viewports) {
            dirty_state.viewports = false;
            emit([v = viewports](vk::CommandBuffer c) { c.setViewportWithCount(v); });
        }
        if (dirty_state.scissors) {
            dirty_state.scissors = false;
            emit([v = scissors](vk::CommandBuffer c) { c.setScissorWithCount(v); });
        }
        if (dirty_state.depth_test_enabled) {
            dirty_state.depth_test_enabled = false;
            emit([v = depth_test_enabled](vk::CommandBuffer c) { c.setDepthTestEnable(v); });
        }
        if (dirty_state.depth_write_enabled) {
            // Must be set in a command buffer even if depth test is disabled.
            dirty_state.depth_write_enabled = false;
            emit([v = depth_write_enabled](vk::CommandBuffer c) { c.setDepthWriteEnable(v); });
        }
        if (depth_test_enabled && dirty_state.depth_compare_op) {
            dirty_state.depth_compare_op = false;
            emit([v = depth_compare_op](vk::CommandBuffer c) { c.setDepthCompareOp(v); });
        }
        if (dirty_state.depth_bounds_test_enabled) {
            dirty_state.depth_bounds_test_enabled = false;
            if (depth_bounds_supported) {
                emit([v = depth_bounds_test_enabled](vk::CommandBuffer c) {
                    c.setDepthBoundsTestEnable(v);
                });
            }
        }
        if (depth_bounds_test_enabled && dirty_state.depth_bounds) {
            dirty_state.depth_bounds = false;
            if (depth_bounds_supported) {
                emit([lo = depth_bounds_min, hi = depth_bounds_max](vk::CommandBuffer c) {
                    c.setDepthBounds(lo, hi);
                });
            }
        }
        if (dirty_state.depth_bias_enabled) {
            dirty_state.depth_bias_enabled = false;
            emit([v = depth_bias_enabled](vk::CommandBuffer c) { c.setDepthBiasEnable(v); });
        }
        if (depth_bias_enabled && dirty_state.depth_bias) {
            dirty_state.depth_bias = false;
            emit([k = depth_bias_constant, clamp = depth_bias_clamp,
                  slope = depth_bias_slope](vk::CommandBuffer c) { c.setDepthBias(k, clamp, slope); });
        }
        if (dirty_state.stencil_test_enabled) {
            dirty_state.stencil_test_enabled = false;
            emit([v = stencil_test_enabled](vk::CommandBuffer c) { c.setStencilTestEnable(v); });
        }
        if (stencil_test_enabled) {
            const auto ops = [&](Face face, const StencilOps& o) {
                emit([face, o](vk::CommandBuffer c) {
                    c.setStencilOp(face, o.fail_op, o.pass_op, o.depth_fail_op, o.compare_op);
                });
            };
            if (dirty_state.stencil_front_ops && dirty_state.stencil_back_ops &&
                stencil_front_ops == stencil_back_ops) {
                ops(Face::eFrontAndBack, stencil_front_ops);
            } else {
                if (dirty_state.stencil_front_ops) {
                    ops(Face::eFront, stencil_front_ops);
                }
                if (dirty_state.stencil_back_ops) {
                    ops(Face::eBack, stencil_back_ops);
                }
            }
            dirty_state.stencil_front_ops = dirty_state.stencil_back_ops = false;

            const auto reference = [&](Face face, u32 v) {
                emit([face, v](vk::CommandBuffer c) { c.setStencilReference(face, v); });
            };
            if (dirty_state.stencil_front_reference && dirty_state.stencil_back_reference &&
                stencil_front_reference == stencil_back_reference) {
                reference(Face::eFrontAndBack, stencil_front_reference);
            } else {
                if (dirty_state.stencil_front_reference) {
                    reference(Face::eFront, stencil_front_reference);
                }
                if (dirty_state.stencil_back_reference) {
                    reference(Face::eBack, stencil_back_reference);
                }
            }
            dirty_state.stencil_front_reference = dirty_state.stencil_back_reference = false;

            const auto write_mask = [&](Face face, u32 v) {
                emit([face, v](vk::CommandBuffer c) { c.setStencilWriteMask(face, v); });
            };
            if (dirty_state.stencil_front_write_mask && dirty_state.stencil_back_write_mask &&
                stencil_front_write_mask == stencil_back_write_mask) {
                write_mask(Face::eFrontAndBack, stencil_front_write_mask);
            } else {
                if (dirty_state.stencil_front_write_mask) {
                    write_mask(Face::eFront, stencil_front_write_mask);
                }
                if (dirty_state.stencil_back_write_mask) {
                    write_mask(Face::eBack, stencil_back_write_mask);
                }
            }
            dirty_state.stencil_front_write_mask = dirty_state.stencil_back_write_mask = false;

            const auto compare_mask = [&](Face face, u32 v) {
                emit([face, v](vk::CommandBuffer c) { c.setStencilCompareMask(face, v); });
            };
            if (dirty_state.stencil_front_compare_mask && dirty_state.stencil_back_compare_mask &&
                stencil_front_compare_mask == stencil_back_compare_mask) {
                compare_mask(Face::eFrontAndBack, stencil_front_compare_mask);
            } else {
                if (dirty_state.stencil_front_compare_mask) {
                    compare_mask(Face::eFront, stencil_front_compare_mask);
                }
                if (dirty_state.stencil_back_compare_mask) {
                    compare_mask(Face::eBack, stencil_back_compare_mask);
                }
            }
            dirty_state.stencil_front_compare_mask = dirty_state.stencil_back_compare_mask = false;
        }
        if (dirty_state.primitive_restart_enable) {
            dirty_state.primitive_restart_enable = false;
            emit([v = primitive_restart_enable](vk::CommandBuffer c) {
                c.setPrimitiveRestartEnable(v);
            });
        }
        if (dirty_state.rasterizer_discard_enable) {
            dirty_state.rasterizer_discard_enable = false;
            emit([v = rasterizer_discard_enable](vk::CommandBuffer c) {
                c.setRasterizerDiscardEnable(v);
            });
        }
        if (dirty_state.cull_mode) {
            dirty_state.cull_mode = false;
            emit([v = cull_mode](vk::CommandBuffer c) { c.setCullMode(v); });
        }
        if (dirty_state.front_face) {
            dirty_state.front_face = false;
            emit([v = front_face](vk::CommandBuffer c) { c.setFrontFace(v); });
        }
        if (dirty_state.blend_constants) {
            dirty_state.blend_constants = false;
            emit([v = blend_constants](vk::CommandBuffer c) { c.setBlendConstants(v.data()); });
        }
        if (dirty_state.color_write_masks) {
            dirty_state.color_write_masks = false;
            if (color_write_mask_supported) {
                emit([v = color_write_masks](vk::CommandBuffer c) { c.setColorWriteMaskEXT(0, v); });
            }
        }
        if (dirty_state.line_width) {
            dirty_state.line_width = false;
            emit([v = line_width](vk::CommandBuffer c) { c.setLineWidth(v); });
        }
        if (dirty_state.feedback_loop_enabled && feedback_loop_supported) {
            dirty_state.feedback_loop_enabled = false;
            emit([v = feedback_loop_enabled](vk::CommandBuffer c) {
                c.setAttachmentFeedbackLoopEnableEXT(v ? vk::ImageAspectFlagBits::eColor
                                                       : vk::ImageAspectFlagBits::eNone);
            });
        }
    }

    /// bbport: true when Commit() would record anything. Flags that Commit() defers (their
    /// test is disabled) do not count.
    [[nodiscard]] bool AnyDirty() const noexcept {
        auto pending = dirty_state;
        if (!depth_test_enabled) {
            pending.depth_compare_op = false;
        }
        if (!depth_bounds_test_enabled) {
            pending.depth_bounds = false;
        }
        if (!depth_bias_enabled) {
            pending.depth_bias = false;
        }
        if (!stencil_test_enabled) {
            pending.stencil_front_ops = pending.stencil_back_ops = false;
            pending.stencil_front_reference = pending.stencil_back_reference = false;
            pending.stencil_front_write_mask = pending.stencil_back_write_mask = false;
            pending.stencil_front_compare_mask = pending.stencil_back_compare_mask = false;
        }
        static constexpr decltype(dirty_state) clean{};
        return std::memcmp(&pending, &clean, sizeof(pending)) != 0;
    }

    /// Invalidates all dynamic state to be flushed into the next command buffer.
    void Invalidate() {
        // bbport: named flags only; padding bits set by a memset made AnyDirty() always true.
        dirty_state.viewports = true;
        dirty_state.scissors = true;
        dirty_state.depth_test_enabled = true;
        dirty_state.depth_write_enabled = true;
        dirty_state.depth_compare_op = true;
        dirty_state.depth_bounds_test_enabled = true;
        dirty_state.depth_bounds = true;
        dirty_state.depth_bias_enabled = true;
        dirty_state.depth_bias = true;
        dirty_state.stencil_test_enabled = true;
        dirty_state.stencil_front_ops = true;
        dirty_state.stencil_front_reference = true;
        dirty_state.stencil_front_write_mask = true;
        dirty_state.stencil_front_compare_mask = true;
        dirty_state.stencil_back_ops = true;
        dirty_state.stencil_back_reference = true;
        dirty_state.stencil_back_write_mask = true;
        dirty_state.stencil_back_compare_mask = true;
        dirty_state.primitive_restart_enable = true;
        dirty_state.rasterizer_discard_enable = true;
        dirty_state.cull_mode = true;
        dirty_state.front_face = true;
        dirty_state.blend_constants = true;
        dirty_state.color_write_masks = true;
        dirty_state.line_width = true;
        dirty_state.feedback_loop_enabled = true;
    }

    void SetViewports(const Viewports& viewports_) {
        if (!std::ranges::equal(viewports, viewports_)) {
            viewports = viewports_;
            dirty_state.viewports = true;
        }
    }

    void SetScissors(const Scissors& scissors_) {
        if (!std::ranges::equal(scissors, scissors_)) {
            scissors = scissors_;
            dirty_state.scissors = true;
        }
    }

    void SetDepthTestEnabled(const bool enabled) {
        if (depth_test_enabled != enabled) {
            depth_test_enabled = enabled;
            dirty_state.depth_test_enabled = true;
        }
    }

    void SetDepthWriteEnabled(const bool enabled) {
        if (depth_write_enabled != enabled) {
            depth_write_enabled = enabled;
            dirty_state.depth_write_enabled = true;
        }
    }

    void SetDepthCompareOp(const vk::CompareOp compare_op) {
        if (depth_compare_op != compare_op) {
            depth_compare_op = compare_op;
            dirty_state.depth_compare_op = true;
        }
    }

    void SetDepthBoundsTestEnabled(const bool enabled) {
        if (depth_bounds_test_enabled != enabled) {
            depth_bounds_test_enabled = enabled;
            dirty_state.depth_bounds_test_enabled = true;
        }
    }

    void SetDepthBounds(const float min, const float max) {
        if (depth_bounds_min != min || depth_bounds_max != max) {
            depth_bounds_min = min;
            depth_bounds_max = max;
            dirty_state.depth_bounds = true;
        }
    }

    void SetDepthBiasEnabled(const bool enabled) {
        if (depth_bias_enabled != enabled) {
            depth_bias_enabled = enabled;
            dirty_state.depth_bias_enabled = true;
        }
    }

    void SetDepthBias(const float constant, const float clamp, const float slope) {
        if (depth_bias_constant != constant || depth_bias_clamp != clamp ||
            depth_bias_slope != slope) {
            depth_bias_constant = constant;
            depth_bias_clamp = clamp;
            depth_bias_slope = slope;
            dirty_state.depth_bias = true;
        }
    }

    void SetStencilTestEnabled(const bool enabled) {
        if (stencil_test_enabled != enabled) {
            stencil_test_enabled = enabled;
            dirty_state.stencil_test_enabled = true;
        }
    }

    void SetStencilOps(const StencilOps& front_ops, const StencilOps& back_ops) {
        if (stencil_front_ops != front_ops) {
            stencil_front_ops = front_ops;
            dirty_state.stencil_front_ops = true;
        }
        if (stencil_back_ops != back_ops) {
            stencil_back_ops = back_ops;
            dirty_state.stencil_back_ops = true;
        }
    }

    void SetStencilReferences(const u32 front_reference, const u32 back_reference) {
        if (stencil_front_reference != front_reference) {
            stencil_front_reference = front_reference;
            dirty_state.stencil_front_reference = true;
        }
        if (stencil_back_reference != back_reference) {
            stencil_back_reference = back_reference;
            dirty_state.stencil_back_reference = true;
        }
    }

    void SetStencilWriteMasks(const u32 front_write_mask, const u32 back_write_mask) {
        if (stencil_front_write_mask != front_write_mask) {
            stencil_front_write_mask = front_write_mask;
            dirty_state.stencil_front_write_mask = true;
        }
        if (stencil_back_write_mask != back_write_mask) {
            stencil_back_write_mask = back_write_mask;
            dirty_state.stencil_back_write_mask = true;
        }
    }

    void SetStencilCompareMasks(const u32 front_compare_mask, const u32 back_compare_mask) {
        if (stencil_front_compare_mask != front_compare_mask) {
            stencil_front_compare_mask = front_compare_mask;
            dirty_state.stencil_front_compare_mask = true;
        }
        if (stencil_back_compare_mask != back_compare_mask) {
            stencil_back_compare_mask = back_compare_mask;
            dirty_state.stencil_back_compare_mask = true;
        }
    }

    void SetPrimitiveRestartEnabled(const bool enabled) {
        if (primitive_restart_enable != enabled) {
            primitive_restart_enable = enabled;
            dirty_state.primitive_restart_enable = true;
        }
    }

    void SetCullMode(const vk::CullModeFlags cull_mode_) {
        if (cull_mode != cull_mode_) {
            cull_mode = cull_mode_;
            dirty_state.cull_mode = true;
        }
    }

    void SetFrontFace(const vk::FrontFace front_face_) {
        if (front_face != front_face_) {
            front_face = front_face_;
            dirty_state.front_face = true;
        }
    }

    void SetBlendConstants(const std::array<float, 4> blend_constants_) {
        if (blend_constants != blend_constants_) {
            blend_constants = blend_constants_;
            dirty_state.blend_constants = true;
        }
    }

    void SetRasterizerDiscardEnabled(const bool enabled) {
        if (rasterizer_discard_enable != enabled) {
            rasterizer_discard_enable = enabled;
            dirty_state.rasterizer_discard_enable = true;
        }
    }

    void SetColorWriteMasks(const ColorWriteMasks& color_write_masks_) {
        if (!std::ranges::equal(color_write_masks, color_write_masks_)) {
            color_write_masks = color_write_masks_;
            dirty_state.color_write_masks = true;
        }
    }

    void SetLineWidth(const float width) {
        if (line_width != width) {
            line_width = width;
            dirty_state.line_width = true;
        }
    }

    void SetAttachmentFeedbackLoopEnabled(const bool enabled) {
        if (feedback_loop_enabled != enabled) {
            feedback_loop_enabled = enabled;
            dirty_state.feedback_loop_enabled = true;
        }
    }
};

using SubmitFunc = Common::UniqueFunction<void, SubmitInfo&>;

/// bbport: the command buffer segments of one submission and their chunk counters (guarded by the
/// scheduler's recorder_mutex). With BB_ASYNC_SUBMIT several are in flight: a submission goes
/// out from a recording thread once its segments are recorded.
struct SubmissionBatch {
    static constexpr u32 MaxSegments = 64;
    std::array<vk::CommandBuffer, MaxSegments> segments{}; ///< null until begun
    std::array<u32, MaxSegments> handed{}, recorded{};
    void Reset() {
        segments.fill({});
        handed.fill(0);
        recorded.fill(0);
    }
};

/// bbport: a block of deferred Vulkan commands. Commands are closures placed in
/// fixed storage (no allocation per command) and run in order on the recording thread.
class RecordChunk {
public:
    static constexpr size_t Capacity = 128 * 1024;

    /// Returns false (and leaves `func` untouched) when the chunk has no room.
    template <typename Func>
    bool Push(Func&& func) {
        using Command = TypedCommand<std::decay_t<Func>>;
        static_assert(sizeof(Command) <= Capacity, "recorded command is too large");
        const size_t offset = (used + alignof(Command) - 1) & ~(alignof(Command) - 1);
        if (offset + sizeof(Command) > Capacity) {
            return false;
        }
        auto* command = new (storage + offset) Command(std::forward<Func>(func));
        if (last) {
            last->next = command;
        } else {
            first = command;
        }
        last = command;
        used = offset + sizeof(Command);
        PrefetchAhead();
        return true;
    }

    /// Raw storage in the chunk for a command's variable-length data; null when full.
    void* Allocate(size_t bytes, size_t align) {
        const size_t offset = (used + align - 1) & ~(align - 1);
        if (offset + bytes > Capacity) {
            return nullptr;
        }
        used = offset + bytes;
        PrefetchAhead();
        return storage + offset;
    }

    void Execute(vk::CommandBuffer cmdbuf) {
        for (CommandBase* command = first; command;) {
            CommandBase* const next = command->next;
            command->Execute(cmdbuf);
            command->~CommandBase();
            command = next;
        }
        first = last = nullptr;
        used = 0;
    }

    [[nodiscard]] bool Empty() const noexcept {
        return first == nullptr;
    }

    [[nodiscard]] size_t Size() const noexcept {
        return used;
    }

    u32 segment = 0; ///< the command buffer segment the chunk is recorded into (Scheduler)
    SubmissionBatch* batch = nullptr; ///< the submission it belongs to (Scheduler)
    /// Ordered chunks: the command chunks of `segment` (and all of the segments before it)
    /// that must be recorded before this chunk runs.
    u32 after = 0;

private:
    /// The recording thread last read these cache lines: the first write to each has to take
    /// it back from that core. Requesting ownership a few lines ahead overlaps those
    /// transfers instead of stalling on each (RecordPrefetch).
    void PrefetchAhead() const {
        if (!BbToggle::Disabled(BbToggle::RecordPrefetch)) {
            __builtin_prefetch(storage + used + 384, 1, 3);
            __builtin_prefetch(storage + used + 448, 1, 3);
        }
    }

    struct CommandBase {
        virtual ~CommandBase() = default;
        virtual void Execute(vk::CommandBuffer cmdbuf) = 0;
        CommandBase* next{};
    };
    template <typename Func>
    struct TypedCommand final : CommandBase {
        explicit TypedCommand(Func&& func_) : func{std::move(func_)} {}
        explicit TypedCommand(const Func& func_) : func{func_} {}
        void Execute(vk::CommandBuffer cmdbuf) override {
            func(cmdbuf);
        }
        Func func;
    };

    alignas(64) std::byte storage[Capacity];
    size_t used = 0;
    CommandBase* first{};
    CommandBase* last{};
};

class Scheduler {
public:
    /// `threaded_recording`: Vulkan commands passed to Record() are recorded by a worker thread.
    explicit Scheduler(const Instance& instance, bool threaded_recording = false);
    ~Scheduler();

    /// Sends the current execution context to the GPU
    /// and increments the scheduler timeline semaphore.
    void Flush(SubmitInfo& info);

    /// Sends the current execution context to the GPU
    /// and increments the scheduler timeline semaphore.
    void Flush();

    /// Sends the current execution context to the GPU and waits for it to complete.
    void Finish();

    /// bbport: waits for the GPU to complete the submissions before the current one, which is
    /// not sent: for callers that keep recording into it (the upscaler between CommandBuffer()
    /// and the end of its pass) and free resources only earlier submissions used.
    void WaitSubmittedWork() {
        const u64 tick = CurrentTick();
        if (tick > 1) {
            Wait(tick - 1);
        }
    }
    // Compatibility for the Windows DLSS bridge's resource retirement.
    void WaitSubmitted() { WaitSubmittedWork(); }

    /// Waits for the given tick to trigger on the GPU.
    void Wait(u64 tick);

    /// Attempts to execute operations whose tick the GPU has caught up with.
    void PopPendingOperations();

    /// Starts a new rendering scope with provided state.
    void BeginRendering(const RenderState& new_state);

    /// Ends current rendering scope.
    void EndRendering();

    /// Sets a function to be called on every scheduler submission.
    void SetSubmitCallback(SubmitFunc&& on_submit) {
        this->on_submit = std::move(on_submit);
    }

    /// Returns the current render state.
    const RenderState& GetRenderState() const {
        return render_state;
    }

    /// Returns the current pipeline dynamic state tracking.
    DynamicState& GetDynamicState() {
        return dynamic_state;
    }

    /// Returns the current command buffer for recording on the calling thread. With threaded
    /// recording this first waits until all commands passed to Record() are recorded, then
    /// records directly (Record() included, so callers may mix both) until the next
    /// KickRecording() or submission.
    [[gnu::noinline]] vk::CommandBuffer CommandBuffer() {
        ProducerScope producer{*this, "CommandBuffer"};
        if (!workers.empty() && !direct_mode) {
            EnterDirectMode();
            direct_recordings.fetch_add(1, std::memory_order_relaxed);
            TraceDirectRecording(__builtin_return_address(0));
        }
        return current_cmdbuf;
    }

    /// BB_RECORDER_TRACE=1: prints the most frequent CommandBuffer() callers every 2000 calls.
    static void TraceDirectRecording(void* caller);

    /// bbport: the recording side (current chunks, segments) is used by one thread at a time.
    /// A second thread inside it at once — or HandOver() entered from inside Record*() on the
    /// same thread (a fault handler) — is reported with both threads' names and this thread's
    /// stack: a null record_chunk/ordered_chunk crashed HandOver and SmallGuestCopy.
    class ProducerScope {
    public:
        ProducerScope(Scheduler& scheduler_, const char* where) noexcept : scheduler{scheduler_} {
            if (ProducerCheck()) {
                Enter(where);
            }
        }
        ~ProducerScope() noexcept {
            if (ProducerCheck()) {
                Leave();
            }
        }
        /// BB_PRODUCER_CHECK=1 (the launcher's crash diagnostics); off otherwise, the check
        /// costs an atomic exchange per recorded command.
        static bool ProducerCheck() noexcept {
            static const bool enabled = [] {
                const char* env = std::getenv("BB_PRODUCER_CHECK");
                return env && env[0] == '1';
            }();
            return enabled;
        }
        ProducerScope(const ProducerScope&) = delete;
        ProducerScope& operator=(const ProducerScope&) = delete;

    private:
        void Enter(const char* where) noexcept;
        void Leave() noexcept;
        Scheduler& scheduler;
        const char* previous_where = nullptr;
        bool outer = false;
    };

    /// Records `func(vk::CommandBuffer)` in order with other commands. The closure must own
    /// everything it uses (capture by value): it may run later on the recording thread.
    template <typename Func>
    void Record(Func&& func) {
        ProducerScope producer{*this, "Record"};
        if (workers.empty() || direct_mode) {
            func(current_cmdbuf);
            return;
        }
        if (BbToggle::Disabled(BbToggle::ThreadedRecording)) {
            EnterDirectMode();
            func(current_cmdbuf);
            return;
        }
        if (!record_chunk->Push(std::forward<Func>(func))) {
            RetireChunk();
            const bool pushed = record_chunk->Push(std::forward<Func>(func));
            ASSERT(pushed);
        }
    }

    /// bbport: Record() for a draw, dispatch or pass that runs shaders, between GPU breadcrumbs
    /// (vk_breadcrumbs.h): on a GPU hang they say which one never finished.
    template <typename Func>
    void RecordCrumb(const Breadcrumbs::Crumb& crumb, Func&& func) {
        if (!Breadcrumbs::Enabled()) {
            Record(std::forward<Func>(func));
            return;
        }
        const u32 id = Breadcrumbs::Note(crumb_stream, crumb);
        Record([func = std::forward<Func>(func), stream = crumb_stream,
                id](vk::CommandBuffer cmdbuf) mutable {
            Breadcrumbs::Mark(cmdbuf, stream, id, false);
            func(cmdbuf);
            Breadcrumbs::Mark(cmdbuf, stream, id, true);
        });
    }

    /// bbport: waits until the submission signalling `tick` is in the queue. Another thread's
    /// submission waiting for the tick on the same queue must not go first (BB_ASYNC_SUBMIT
    /// submits from the recording threads): the queue would wait for a later submission.
    void WaitSubmitted(u64 tick) {
        u64 submitted = submitted_tick.load(std::memory_order_acquire);
        while (submitted < tick) {
            submitted_tick.wait(submitted, std::memory_order_acquire);
            submitted = submitted_tick.load(std::memory_order_acquire);
        }
    }

    /// bbport: the breadcrumb stream of this scheduler, for passes recorded on CommandBuffer().
    [[nodiscard]] u32 CrumbStream() const noexcept {
        return crumb_stream;
    }

    /// True when Record() defers commands (and RecordData() copies into chunks).
    [[nodiscard]] bool IsRecordingDeferred() const noexcept {
        return !workers.empty() && !direct_mode &&
               !BbToggle::Disabled(BbToggle::ThreadedRecording);
    }

    /// Makes room for `bytes` of RecordData() plus the command that uses them in the current
    /// chunk: data and command must share a chunk, which is recycled once executed.
    void ReserveRecordData(size_t bytes) {
        if (!IsRecordingDeferred()) {
            return;
        }
        ASSERT(bytes + 1024 <= RecordChunk::Capacity);
        if (RecordChunk::Capacity - record_chunk->Size() < bytes + 1024) {
            RetireChunk();
        }
    }

    /// Copies `data` into recording storage that lives until the command that uses it has
    /// been recorded (the same chunk). With threaded recording off, returns `data` itself.
    template <typename T>
    std::span<const T> RecordData(std::span<const T> data) {
        ProducerScope producer{*this, "RecordData"};
        if (!IsRecordingDeferred() || data.empty()) {
            return data;
        }
        // Room for the data and the command that follows it, so both stay in one chunk.
        const size_t bytes = data.size_bytes();
        ReserveRecordData(bytes + alignof(T));
        auto* dst = static_cast<T*>(record_chunk->Allocate(bytes, alignof(T)));
        std::memcpy(dst, data.data(), bytes);
        return {dst, data.size()};
    }

    /// Hands recorded commands to the recording threads. Called at points where no caller holds
    /// the raw command buffer; small batches are kept. Without `force` (the end of a draw or
    /// dispatch: nothing but the dynamic state and the render pass carries over to the next
    /// command) a long enough segment is cut there, and the next one goes to another thread.
    void KickRecording(bool force = false);

    /// Waits until every recorded command is in the command buffer.
    void SyncRecording();

    /// bbport: guest memory copies on the recording thread (small ones, RecordHostCopy) or the
    /// copy threads (BbCopy::Async) must be done before the guest learns the GPU is past them
    /// (it may then rewrite the memory, e.g. UI vertices: flickering) and before the
    /// submission that reads them. Waits for all.
    void WaitHostCopies();

    /// bbport: guest memory [address, address + size) a host copy issued now will read (the
    /// source of a deferred upload or rename). WaitHostCopiesFor waits only for these.
    void NoteHostCopySource(u64 address, u64 size);
    /// Waits for the host copies when one of them may still read [address, address + size): a
    /// write there (the command processor's WRITE_DATA/DMA) must not change what they copy.
    /// Writes elsewhere do not wait (the hardware's command processor writes do not wait for
    /// earlier draws either).
    void WaitHostCopiesFor(u64 address, u64 size);

    /// Runs `copy` on a recording thread in order with the commands, the other host copies and
    /// the signals of SignalAfterHostCopies (the threads spin for work anyway, so small copies cost no wakeup);
    /// WaitHostCopies() covers it.
    template <typename Func>
    void RecordHostCopy(Func&& copy) {
        RecordOrdered([copy = std::forward<Func>(copy), this, seq = ++host_copies_issued] {
            copy();
            host_copies_done.store(seq, std::memory_order_release);
        });
    }

    /// bbport: runs `signal` once the guest memory copies issued so far are done, without
    /// waiting here: the recording thread reaches it after the copies queued before it and hands
    /// it to the copy threads' completion (BbCopy::AfterCopies).
    void SignalAfterHostCopies(std::function<void()> signal);

    /// bbport: before a guest-visible write that is not deferred (WriteData, end-of-shader
    /// fences, flip): waits until every signal handed to SignalAfterHostCopies ran, so that
    /// the write cannot overtake an earlier fence. Otherwise the guest, seeing the later value,
    /// may free memory the earlier fence then writes into (corrupted heap, guest fault).
    void WaitDeferredSignals();

    /// Whether a render pass with exactly this state is open.
    [[nodiscard]] bool IsRenderingWith(const RenderState& state) const {
        return is_rendering && render_state == state;
    }

    /// Whether BeginRendering(state) would start a new render pass.
    [[nodiscard]] bool WillBeginRendering(const RenderState& state) const {
        return !(is_rendering && render_state == state);
    }

    /// CommandBuffer() calls that waited for a recording thread (BB_FRAME_STATS).
    static inline std::atomic<u64> direct_recordings{0};
    /// Command buffer segments submitted (BB_FRAME_STATS: segments per submission).
    static inline std::atomic<u64> recorded_segments{0};
    static inline std::atomic<u64> recorded_submissions{0};

    /// Returns the current command buffer tick.
    [[nodiscard]] u64 CurrentTick() const noexcept {
        return work_semaphore.CurrentTick();
    }

    /// Returns true when a tick has been triggered by the GPU.
    [[nodiscard]] bool IsFree(u64 tick) noexcept {
        if (work_semaphore.IsFree(tick)) {
            return true;
        }
        work_semaphore.Refresh();
        return work_semaphore.IsFree(tick);
    }

    /// Returns the scheduler timeline semaphore.
    [[nodiscard]] Semaphore* GetWorkSemaphore() noexcept {
        return &work_semaphore;
    }

    /// Defers an operation until the gpu has reached the current cpu tick.
    /// Will be run when submitting or calling PopPendingOperations.
    void DeferOperation(Common::UniqueFunction<void>&& func) {
        std::unique_lock lk(pending_ops_mutex);
        pending_ops.emplace(std::move(func), CurrentTick());
        num_pending_ops.fetch_add(1, std::memory_order_release);
    }

    /// Defers an operation until the gpu has reached the current cpu tick.
    /// Runs as soon as possible in another thread.
    void DeferPriorityOperation(Common::UniqueFunction<void>&& func) {
        {
            std::unique_lock lk(priority_pending_ops_mutex);
            priority_pending_ops.emplace(std::move(func), CurrentTick());
        }
        priority_pending_ops_cv.notify_one();
    }

    static std::mutex submit_mutex;

private:
    static constexpr u32 MaxSegments = SubmissionBatch::MaxSegments;

    /// A recording thread. It owns a command pool (worker 0: the scheduler's) and records the
    /// segments i with i % workers.size() == its index.
    struct Worker {
        CommandPool* pool{};
        std::unique_ptr<CommandPool> own_pool;
        std::deque<std::unique_ptr<RecordChunk>> queue; ///< guarded by recorder_mutex
        std::atomic<size_t> queued{0};                  ///< queue.size() for lock-free polling
        bool busy = false;     ///< running a chunk (guarded by recorder_mutex)
        bool sleeping = false; ///< waiting on cv (guarded by recorder_mutex)
        std::condition_variable_any cv;
        std::jthread thread;
    };

    void AllocateWorkerCommandBuffers();
    SubmissionBatch* AcquireBatch();
    /// BB_ASYNC_SUBMIT: hands the submission to the recording threads (it goes out once its
    /// segments are recorded) instead of waiting for them here.
    void SubmitAsync(SubmitInfo& info);

    std::unique_ptr<RecordChunk> AcquireChunk();

    /// Moves the full current chunk aside; a new one takes its place.
    void RetireChunk() {
        ProducerScope producer{*this, "RetireChunk"};
        segment_bytes += record_chunk->Size();
        full_chunks.push_back(std::move(record_chunk));
        record_chunk = AcquireChunk();
    }

    /// Waits for the recording threads, then records on this thread into the current segment's
    /// command buffer until the next KickRecording(). bbport: with BB_ASYNC_SUBMIT, into a
    /// segment of its own instead, without waiting (direct_segment).
    void EnterDirectMode();

    /// Ends the direct segment (this thread's command buffer); the next commands go to a new
    /// segment of the recording threads.
    void LeaveDirectSegment();

    /// Whether ordered tasks run on the calling thread right away: no recording threads, or
    /// direct mode after waiting for them (not a direct segment: earlier tasks may be queued).
    [[nodiscard]] bool OrderedInline() const noexcept {
        return workers.empty() || (direct_mode && !direct_segment);
    }

    /// Cuts the command stream after the current segment when it is long enough.
    void MaybeSplit();

    /// Hands the current segment's chunks and the ordered chunks to the recording threads.
    void HandOver();

    /// Runs `func()` on a recording thread, in order with the other ordered tasks (guest memory
    /// copies and the fence signals after them) and after every command recorded before it, in
    /// all segments: the guest sees its fences no earlier than in 0.2. Fences signalled ahead of
    /// the queued recording coincided with frequent guest heap corruption (guest offset
    /// 0x263b8e7: 3 of 4 soak runs instead of none).
    template <typename Func>
    void RecordOrdered(Func&& func) {
        ProducerScope producer{*this, "RecordOrdered"};
        if (OrderedInline()) {
            func();
            return;
        }
        if (BbToggle::Disabled(BbToggle::ThreadedRecording)) {
            EnterDirectMode();
            func();
            return;
        }
        auto command = [func = std::forward<Func>(func)](vk::CommandBuffer) mutable { func(); };
        if (!ordered_chunk->Push(std::move(command))) {
            ordered_full.push_back(std::move(ordered_chunk));
            ordered_chunk = AcquireChunk();
            const bool pushed = ordered_chunk->Push(std::move(command));
            ASSERT(pushed);
        }
    }

    [[nodiscard]] bool RecordingIdle() const; ///< with recorder_mutex held

    /// Whether the oldest ordered chunk may run: the commands handed over before it are
    /// recorded (with recorder_mutex held).
    [[nodiscard]] bool OrderedReady() const;

    void RecorderThread(std::stop_token stoken, u32 index);

    void SubmitExecution(SubmitInfo& info);

    void PriorityPendingOpsThread(std::stop_token stoken);

private:
    const Instance& instance;
    u32 crumb_stream = 0; ///< bbport: GPU breadcrumbs (vk_breadcrumbs.h)
    Semaphore work_semaphore;
    CommandPool command_pool;
    DynamicState dynamic_state;
    SubmitFunc on_submit{};
    vk::CommandBuffer current_cmdbuf;
    std::condition_variable_any event_cv;
    struct PendingOp {
        Common::UniqueFunction<void> callback;
        u64 gpu_tick;
    };
    std::queue<PendingOp> pending_ops;
    std::recursive_mutex pending_ops_mutex;
    u32 pending_polls = 0; // bbport
    std::atomic<u32> num_pending_ops{0}; ///< bbport: pending_ops.size(), checked without the lock
    std::queue<PendingOp> priority_pending_ops;
    std::mutex priority_pending_ops_mutex;
    std::condition_variable_any priority_pending_ops_cv;
    std::jthread priority_pending_ops_thread;
    RenderState render_state;
    bool is_rendering = false;
    // bbport: threaded recording. Commands go into chunks of the current segment; segments are
    // recorded in parallel, each into its own command buffer, and a submission runs their
    // command buffers in order. Guest memory copies and fence signals go into ordered chunks,
    // which one recording thread at a time runs in order, each once the commands handed over
    // before it are recorded.
    std::vector<std::unique_ptr<Worker>> workers; ///< empty: recording on the calling thread
    /// The current submission's segments and the chunks of each handed to the recording threads
    /// and recorded (guarded by recorder_mutex); with BB_ASYNC_SUBMIT earlier ones may still be
    /// in flight (free_batches: those submitted).
    SubmissionBatch* batch = nullptr;
    std::vector<std::unique_ptr<SubmissionBatch>> all_batches;
    std::vector<SubmissionBatch*> free_batches; ///< guarded by recorder_mutex
    bool async_submit = false; ///< BB_ASYNC_SUBMIT (default on)
    std::atomic<u64> submitted_tick{0}; ///< the last tick handed to the queue (WaitSubmitted)
    u32 current_segment = 0;
    std::atomic<u32> active_worker{0}; ///< the worker of current_segment (it spins for work)
    size_t segment_bytes = 0;          ///< closures of the current segment handed over or retired
    size_t split_bytes = 0;            ///< segment length at which the stream is cut
    bool resume_rendering = false;     ///< a cut closed the render pass with render_state
    std::unique_ptr<RecordChunk> record_chunk;
    std::vector<std::unique_ptr<RecordChunk>> full_chunks;
    std::unique_ptr<RecordChunk> ordered_chunk;
    std::vector<std::unique_ptr<RecordChunk>> ordered_full;
    std::mutex recorder_mutex;
    std::condition_variable_any recorder_idle_cv;
    std::deque<std::unique_ptr<RecordChunk>> ordered_queue; ///< guarded by recorder_mutex
    std::atomic<size_t> ordered_queued{0};
    bool ordered_running = false; ///< a thread runs ordered chunks (guarded by recorder_mutex)
    std::vector<std::unique_ptr<RecordChunk>> free_chunks;
    bool direct_mode = false; ///< the command buffer is recorded on the caller's thread
    /// bbport: direct_mode in a segment of its own (direct_pool), the recording threads going on
    /// with the segments before it.
    bool direct_segment = false;
    std::unique_ptr<CommandPool> direct_pool; ///< used by the producer thread only
    u64 host_copies_issued = 0;
    std::atomic<u64> host_copies_done{0};
    /// Host copy sources by 64 KiB granule in a direct-mapped table: a slot is pending while its
    /// note is newer than the last completed WaitHostCopies; a slot two granules share is a
    /// wildcard (any granule mapping there counts as pending).
    struct HostCopySlot {
        u64 granule = 0;
        u64 seq = 0;
    };
    static constexpr u32 HostCopySlots = 4096;
    static constexpr u64 HostCopyWildcard = ~u64(0);
    std::mutex host_copy_sources_mutex;
    std::array<HostCopySlot, HostCopySlots> host_copy_slots{};
    u64 host_copy_source_seq = 0;
    u64 host_copy_sources_done = 0;  ///< notes up to this one are copied
    u64 host_copy_big_seq = 0;       ///< last note too large for the table (counts as everywhere)
    std::atomic<u64> deferred_signals_issued{0}; ///< by the thread recording (A or B)
    std::atomic<u32> producer_tid{0};                 ///< ProducerScope: the thread inside
    std::atomic<const char*> producer_where{nullptr}; ///< and where
    std::shared_ptr<std::atomic<u64>> deferred_signals_done =
        std::make_shared<std::atomic<u64>>(0);
    tracy::VkCtxScope* profiler_scope{};
};

} // namespace Vulkan

// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <thread>
#include <pthread.h>
#ifndef _WIN32
#include <sys/resource.h>
#include <sys/uio.h>
#endif
#include <unistd.h>
#include <array>
#include <time.h>
#include "bbport_threads.h"
#include "bbport_ce_stats.h"
#include "bbport_timeline.h"
#include "bbport_copy.h"
#include "bbport_toggles.h"
#include <cstdio>
#include <boost/preprocessor/stringize.hpp>

#include "common/assert.h"
#include "common/debug.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "core/libraries/kernel/process.h"
#include "core/libraries/videoout/driver.h"
#include "core/memory.h"
#include "core/libraries/gnmdriver/gnmdriver.h"
#include "core/platform.h"
#include "video_core/amdgpu/liverpool.h"
#include "bbport_write_log.h"
#include "bbport_free_check.h"
#include "video_core/amdgpu/pm4_cmds.h"
#include "video_core/renderdoc.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

extern "C" int runtime_memory_vma_info(uintptr_t address, int* prot, int* type, uintptr_t* end);

namespace AmdGpu {


static const char* dcb_task_name{"DCB_TASK"};
static const char* ccb_task_name{"CCB_TASK"};

#define MAX_NAMES 56
static_assert(Liverpool::NumComputeRings <= MAX_NAMES);

#define NAME_NUM(z, n, name) BOOST_PP_STRINGIZE(name) BOOST_PP_STRINGIZE(n),
#define NAME_ARRAY(name, num) {BOOST_PP_REPEAT(num, NAME_NUM, name)}

static const char* acb_task_name[] = NAME_ARRAY(ACB_TASK, MAX_NAMES);

#define YIELD(name)                                                                                \
    FIBER_EXIT;                                                                                    \
    co_yield {};                                                                                   \
    FIBER_ENTER(name);

#define YIELD_CE() YIELD(ccb_task_name)
#define YIELD_GFX() YIELD(dcb_task_name)
#define YIELD_ASC(id) YIELD(acb_task_name[id])

#define RESUME(task, name)                                                                         \
    FIBER_EXIT;                                                                                    \
    task.handle.resume();                                                                          \
    FIBER_ENTER(name);

#define RESUME_CE(task) RESUME(task, ccb_task_name)
#define RESUME_GFX(task) RESUME(task, dcb_task_name)
#define RESUME_ASC(task, id) RESUME(task, acb_task_name[id])

std::array<u8, 48_KB> Liverpool::ConstantEngine::constants_heap;

static std::span<const u32> NextPacket(std::span<const u32> span, size_t offset) {
    if (offset > span.size()) {
        LOG_ERROR(
            Lib_GnmDriver,
            ": packet length exceeds remaining submission size. Packet dword count={}, remaining "
            "submission dwords={}",
            offset, span.size());
        // Return empty subspan so check for next packet bails out
        return {};
    }

    return span.subspan(offset);
}

Liverpool::Liverpool() : guest_markers_enabled{EmulatorSettings.IsVkGuestMarkersEnabled()} {
    num_counter_pairs = Libraries::Kernel::sceKernelIsNeoMode() ? 16 : 8;
    process_thread = std::jthread{std::bind_front(&Liverpool::Process, this)};
}

Liverpool::~Liverpool() {
    process_thread.request_stop();
    process_thread.join();
}

void Liverpool::ProcessCommands() {
    // Process incoming commands with high priority
    while (num_commands) {
        // bbport: commands touch the caches and record into the scheduler (readbacks:
        // DownloadMemory copies and Finish()es), so the draw recording thread must be idle.
        // Drained per command: draining only when the first check saw one let a command that
        // arrived between the two checks run beside the recording thread — two threads in the
        // scheduler's chunks (null chunk crashes in HandOver/SmallGuestCopy, lost chunks, hangs).
        if (rasterizer) {
            rasterizer->DrainDrawPipe(Vulkan::DrawPipe::ReasonCommands);
        }
        Common::UniqueFunction<void> callback{};
        {
            std::scoped_lock lk{submit_mutex};
            callback = std::move(command_queue.front());
            command_queue.pop();
            --num_commands;
        }
        callback();
    }
}

void Liverpool::Process(std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:GpuCommandProcessor");
#ifndef _WIN32
    if (clockid_t clock; pthread_getcpuclockid(pthread_self(), &clock) == 0) {
        BbStats::gpu_thread_clock.store(static_cast<int>(clock));
    }
#endif
    gpu_id = std::this_thread::get_id();
#if defined(__linux__) || defined(_WIN32)
    gpu_tid = gettid();
#endif

    while (!stoken.stop_requested()) {
        // BB_HONEST_LABELS: fences (and GPU idle) wait for work not submitted yet. Submitted once
        // this thread has had nothing to do for 0.2 ms: the guest may be waiting for them.
        // BB_PIPE_SIGNALS: signals may still be on their way to the recording thread (tasks).
        if (rasterizer && Vulkan::Rasterizer::HonestLabels() &&
            (rasterizer->HasUnsubmittedSignals() || !rasterizer->DrawPipeIdle())) {
            std::unique_lock lk{submit_mutex};
            static const auto idle_wait = std::chrono::microseconds([] {
                const char* env = std::getenv("BB_HONEST_IDLE_US");
                return env ? std::strtoll(env, nullptr, 10) : 200ll;
            }());
            const bool work = submit_cv.wait_for(lk, idle_wait, [this] {
                return num_commands || num_submits || submit_done;
            });
            if (!work) {
                lk.unlock();
                if (rasterizer->PipeSignals()) {
                    // The recording thread submits once it gets there: waiting here for it to
                    // run dry (tens of ms in heavy scenes) held up the guest's next submission.
                    rasterizer->RequestSignalFlush();
                } else {
                    rasterizer->Flush();
                }
            }
        }
        {
            const auto idle_start = std::chrono::steady_clock::now();
            BbTimeline::Note(BbTimeline::DecoderIdleBegin);
            // bbport: the game submits a frame in ~20 parts: waking from the condition variable for
            // each took tens of microseconds on the way to the GPU. Spins first, with cores to spare
            // (BB_GPU_SPIN_US, default 200 with 12+ hardware threads, else 0).
            static const auto spin_time = std::chrono::microseconds([] {
                if (const char* env = std::getenv("BB_GPU_SPIN_US")) {
                    return std::max(0, std::atoi(env));
                }
                return BbThreads::Available() >= 12 ? 200 : 0;
            }());
            if (spin_time.count() > 0) {
                const auto spin_until = idle_start + spin_time;
                for (u32 spins = 1; !(num_commands || num_submits || submit_done) &&
                                    !stoken.stop_requested();
                     ++spins) {
                    __builtin_ia32_pause();
                    if (!(spins & 255) && std::chrono::steady_clock::now() >= spin_until) {
                        break;
                    }
                }
            }
            std::unique_lock lk{submit_mutex};
            Common::CondvarWait(submit_cv, lk, stoken,
                                [this] { return num_commands || num_submits || submit_done; });
            BbStats::gpu_idle_ns.fetch_add(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - idle_start).count(),
                std::memory_order_relaxed);
            BbTimeline::Note(BbTimeline::DecoderIdleEnd);
        }
        if (stoken.stop_requested()) {
            break;
        }

        VideoCore::StartCapture();

        curr_qid = -1;

        while (num_submits || num_commands) {
            ProcessCommands();

            curr_qid = (curr_qid + 1) % num_mapped_queues;

            auto& queue = mapped_queues[curr_qid];

            Task::Handle task{};
            {
                std::scoped_lock lock{queue.m_access};
                if (queue.submits.empty()) {
                    continue;
                }
                task = queue.submits.front();
            }
            if (task.address() != last_decoded) {
                last_decoded = task.address();
                BbTimeline::Note(BbTimeline::DecodeBegin, curr_qid, decoded_total);
            }
            task.resume();

            if (task.done()) {
                task.destroy();
                BbTimeline::Note(BbTimeline::DecodeEnd, curr_qid, decoded_total + 1);

                bool frame_end = false;
                {
                    std::scoped_lock lock{queue.m_access};
                    queue.submits.pop();

                    --num_submits;
                    std::scoped_lock lock2{submit_mutex};
                    ++decoded_total;
                    if (curr_qid == GfxQueueId) {
                        ++gfx_decoded;
                    }
                    frame_end = !frame_ends.empty() && frame_ends.front().first <= decoded_total;
                    submit_cv.notify_all();
                }
                // BB_SUBMIT_LOCK=frame: the last submission of a frame (any queue) is decoded.
                if (frame_end) {
                    SignalDecodedFrames();
                }
            }
        }

        if (submit_done) {
            VideoCore::EndCapture();
            if (rasterizer && rasterizer->PipeSignals()) {
                // BB_PIPE_SIGNALS: the frame's cache work runs on the recording thread after the
                // frame (this thread decodes the next one meanwhile instead of waiting for it).
                static constexpr u8 none = 0;
                rasterizer->RunInOrder(
                    [](Vulkan::Rasterizer& r, const u8*) {
                        r.OnSubmit();
                        r.Flush();
                    },
                    &none, 0);
            } else if (rasterizer) {
                rasterizer->OnSubmit();
                rasterizer->Flush();
            }
            submit_done = false;
        }
        // BB_SUBMIT_LOCK=decode: a frame ended after its submissions were all decoded.
        SignalDecodedFrames();

        // bbport: the guest takes GPU idle (sceGnmSubmitDone) as its work being done and frees
        // the objects that hold its fence labels. The labels must be written before: the draw
        // recording thread and the fences it deferred to the Vulkan recording thread
        // (RecorderFences). Else a late fence write lands in freed memory (a corrupted guest
        // heap free list after minutes of play).
        // The same holds when sceGnmSubmitDone finds the GPU idle and does not block at all:
        // decoded (num_submits 0) is not done until then (work_retired).
        // BB_PIPE_SIGNALS: the recording thread does both in order (a task below) instead.
        const bool pipe_signals = rasterizer && rasterizer->PipeSignals();
        if (rasterizer && !pipe_signals) {
            rasterizer->DrainDrawPipe(Vulkan::DrawPipe::ReasonSubmissionEnd);
            rasterizer->WaitDeferredSignals();
        }
        // GPU idle only if nothing was submitted meanwhile: the drain and the deferred fences take
        // milliseconds (Steam Deck), and a submission plus sceGnmSubmitDone in that time set the
        // submission lock for work not even decoded yet. Releasing it then let the guest free
        // that frame's labels and command buffers: late fences in its heap (guest fault
        // 0x263b8e7) and reused command buffers decoded as garbage ("PM4 type 0"). The loop
        // runs again for the new submissions and signals once they are done.
        // BB_HONEST_LABELS: idle once the GPU has finished what was recorded (its fences written)
        // and nothing was submitted since; the work is submitted below if this thread stays idle.
        if (rasterizer && Vulkan::Rasterizer::HonestLabels()) {
            u64 generation;
            {
                std::scoped_lock lk{submit_mutex};
                generation = submissions_total;
            }
            // BB_SUBMIT_LOCK=decode: everything is decoded, so the next submission need not wait
            // for the GPU to finish (the interrupt below still waits for it).
            if (Libraries::GnmDriver::SubmitLockOnDecode()) {
                bool decoded;
                {
                    std::scoped_lock lk{submit_mutex};
                    decoded = num_submits == 0 && submissions_total == generation;
                }
                if (decoded) {
                    Libraries::GnmDriver::ReleaseSubmissionLock();
                }
            }
            if (pipe_signals) {
                struct IdleSignal {
                    Liverpool* self;
                    u64 generation;
                } const signal{this, generation};
                rasterizer->RunInOrder(
                    [](Vulkan::Rasterizer& r, const u8* data) {
                        IdleSignal idle;
                        std::memcpy(&idle, data, sizeof(idle));
                        r.WaitDeferredSignals();
                        r.SignalAfterGpu([idle] { idle.self->SignalGpuIdle(idle.generation); });
                        r.SubmitForSignals(); // at once if the GPU has run dry
                    },
                    &signal, sizeof(signal), BbToggle::PipelinedTasks, false);
                continue;
            }
            rasterizer->SignalAfterGpu([this, generation] { SignalGpuIdle(generation); });
            continue;
        }
        bool idle;
        {
            std::scoped_lock lk{submit_mutex};
            idle = num_submits == 0;
            if (idle) {
                work_retired = true;
            }
        }
        if (idle) {
            Platform::IrqC::Instance()->Signal(Platform::InterruptId::GpuIdle);
        }
    }
}

namespace {
/// bbport: packets the GPU command thread handles while the draw recording thread may still
/// work on earlier draws (register state, draws it hands over, nested buffers). Every other
/// packet first waits for that thread to run dry (Rasterizer::DrainDrawPipe).
bool PipelinedOpcode(PM4ItOpcode opcode) {
    switch (opcode) {
    case PM4ItOpcode::Nop:
    case PM4ItOpcode::ContextControl:
    case PM4ItOpcode::ClearState:
    case PM4ItOpcode::SetConfigReg:
    case PM4ItOpcode::SetContextReg:
    case PM4ItOpcode::SetShReg:
    case PM4ItOpcode::SetUconfigReg:
    case PM4ItOpcode::IndexType:
    case PM4ItOpcode::NumInstances:
    case PM4ItOpcode::IndexBase:
    case PM4ItOpcode::IndexBufferSize:
    case PM4ItOpcode::SetBase:
    case PM4ItOpcode::DrawIndex2:
    case PM4ItOpcode::DrawIndexOffset2:
    case PM4ItOpcode::DrawIndexAuto:
    case PM4ItOpcode::IndirectBuffer:
    case PM4ItOpcode::AcquireMem:
    case PM4ItOpcode::PfpSyncMe:
    case PM4ItOpcode::IncrementDeCounter:
    case PM4ItOpcode::WaitOnCeCounter:
    case PM4ItOpcode::EventWrite:     // occlusion results only, no rasterizer state
    case PM4ItOpcode::EventWriteEop:  // runs in order on the recording thread (RunInOrder)
    case PM4ItOpcode::EventWriteEos:
    case PM4ItOpcode::WaitRegMem:     // waits for the recording thread only while unmet
    case PM4ItOpcode::DispatchDirect: // handed over like draws (PipelinedDispatch)
    case PM4ItOpcode::DrawIndirect:   // likewise (PipelinedIndirectDraws; else drains itself)
    case PM4ItOpcode::DrawIndirectMulti:
    case PM4ItOpcode::DrawIndexIndirect:
    case PM4ItOpcode::DrawIndexIndirectMulti:
    case PM4ItOpcode::DrawIndexIndirectCountMulti:
    case PM4ItOpcode::DispatchIndirect:
    case PM4ItOpcode::WriteData:     // run in order on the recording thread (RunInOrder)
    case PM4ItOpcode::DmaData:
        return true;
    default:
        return false;
    }
}
} // namespace

namespace {
// bbport: a label written straight into guest memory (no backing view): one store. A memcpy of a
// variable 4 or 8 bytes is two overlapping stores in glibc; a thread preempted between them
// wrote the label again after the guest had freed its block (runtime_memory_write_backing).
void StoreLabel(void* address, u64 value, u32 num_bytes) {
    if (num_bytes == 8 && (reinterpret_cast<uintptr_t>(address) & 7) == 0) {
        __atomic_store_n(static_cast<u64*>(address), value, __ATOMIC_RELEASE);
    } else if (num_bytes == 4 && (reinterpret_cast<uintptr_t>(address) & 3) == 0) {
        __atomic_store_n(static_cast<u32*>(address), u32(value), __ATOMIC_RELEASE);
    } else {
        auto* dst = static_cast<u8*>(address);
        for (u32 i = 0; i < num_bytes; ++i) {
            __atomic_store_n(dst + i, u8(value >> (8 * i)), __ATOMIC_RELEASE);
        }
    }
}

void SignalEop(const PM4CmdEventWriteEop& eop, u64 seq) {
    eop.SignalFence(
        [seq](void* address, u64 data, u32 num_bytes) {
            auto* memory = Core::Memory::Instance();
            // bbport: BB_FENCE_DELAY_TEST=1 (experiment) holds every 200th fence back 80 ms: if
            // the guest frees label objects after a timeout, late labels corrupt its heap.
            static const bool delay_test = [] {
                const char* env = std::getenv("BB_FENCE_DELAY_TEST");
                return env && env[0] == '1';
            }();
            if (delay_test && seq != 0 && seq % 200 == 0) { // seq: BB_FREE_CHECK=1
                std::this_thread::sleep_for(std::chrono::milliseconds(80));
            }
            BbFreeCheck::Check(reinterpret_cast<u64>(address), num_bytes, &data, BbFreeCheck::Eop,
                               seq);
            BbFreeCheck::NoteFenceWriting(reinterpret_cast<u64>(address));
            BbWriteLog::Note(reinterpret_cast<u64>(address), &data, num_bytes, BbWriteLog::Fence);
            const auto write_start = std::chrono::steady_clock::now();
            if (!memory->TryWriteBacking(address, &data, num_bytes)) {
                StoreLabel(address, data, num_bytes);
            }
            BbStats::eop_written.fetch_add(1, std::memory_order_relaxed);
            // bbport: a VRAM copy of the label gets it too (the backing mapping is not watched).
            if (auto* rasterizer = memory->GetRasterizer()) {
                rasterizer->NoteLateCommandWrite(reinterpret_cast<VAddr>(address), &data, num_bytes);
            }
            BbFreeCheck::NoteFenceWritten(
                reinterpret_cast<u64>(address),
                u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - write_start)
                        .count()));
        },
        [] { Platform::IrqC::Instance()->Signal(Platform::InterruptId::GfxEop); });
}

// bbport: end-of-pipe/-shader events run in order with the draws (Rasterizer::RunInOrder: on
// the draw recording thread when the draw pipeline is in use).
void RunEventWriteEop(Vulkan::Rasterizer& rasterizer, const u8* data) {
    const auto eop = *reinterpret_cast<const PM4CmdEventWriteEop*>(data);
    if (eop.data_sel.Value() != DataSelect::None) {
        BbStats::eop_decoded.fetch_add(1, std::memory_order_relaxed);
    }
    // BB_FREE_CHECK: the order of the fences in the command stream, checked where they land.
    const u64 seq = BbFreeCheck::NextFenceSeq();
    rasterizer.ProcessDownloadImages();
    // BB_HONEST_LABELS: written once the GPU has finished the work before it, as the hardware
    // does (an end-of-pipe event), instead of when that work is recorded.
    if (Vulkan::Rasterizer::HonestLabels()) {
        const bool writes = eop.data_sel.Value() != DataSelect::None;
        const u64 value = eop.data_sel.Value() == DataSelect::Data32Low ? eop.DataDWord()
                                                                        : eop.DataQWord();
        rasterizer.SignalAfterGpu([eop, seq] { SignalEop(eop, seq); },
                                  writes ? reinterpret_cast<VAddr>(eop.Address<u8>()) : 0, value);
        return;
    }
    // Guest memory copies on the copy threads precede the fence the guest sees.
    // BB_ASYNC_FENCES=1 writes it once they are done instead of waiting for them; measured
    // slower (79 vs 83 FPS): the guest waits on these fences, and later fences stall it more
    // than the wait costs.
    static const bool async_fences = [] {
        const char* env = std::getenv("BB_ASYNC_FENCES");
        return env && env[0] == '1';
    }();
    if (async_fences && !BbToggle::Disabled(BbToggle::AsyncFences)) {
        BbCopy::AfterCopies([eop, seq] { SignalEop(eop, seq); });
    } else if (Vulkan::DrawPipe::OnStageB() && !BbToggle::Disabled(BbToggle::RecorderFences)) {
        // The draw recording thread does not wait: the Vulkan recording thread signals the
        // fence after the copies queued before it.
        rasterizer.SignalAfterHostCopies([eop, seq] { SignalEop(eop, seq); });
    } else {
        // Labels are written in order: not before the ones deferred to the recording thread.
        rasterizer.WaitHostCopies();
        rasterizer.WaitDeferredSignals();
        SignalEop(eop, seq);
    }
}

void RunDmaData(Vulkan::Rasterizer& rasterizer, const u8* data) {
    const auto* dma_data = reinterpret_cast<const PM4DmaData*>(data);
    ASSERT(dma_data->command.das == 0);
    // bbport: a DMA into guest memory is written by this thread (FillBuffer/CopyBuffer) when the
    // GPU has not modified the range. Like WriteData, it must not overtake the copies and the
    // fences deferred to the recording thread: the guest, seeing it, frees objects whose labels
    // an earlier fence still writes (a corrupted guest heap free list, guest offset 0x263b8e7).
    // With the new memory model, recorded as a GPU fill or copy (memory used in place, or
    // written by the GPU) it lands after the submission goes out, which is after those copies
    // and signals: no wait (~16 us each, ~900 a second of 4-byte counter copies).
    if ((dma_data->dst_sel == DmaDataDst::Memory || dma_data->dst_sel == DmaDataDst::MemoryUsingL2) &&
        rasterizer.DmaMayWriteOnCpu(dma_data->DstAddress<VAddr>(), dma_data->NumBytes())) {
        rasterizer.WaitHostCopiesFor(dma_data->DstAddress<VAddr>(), dma_data->NumBytes());
        rasterizer.WaitDeferredSignals();
    }
    if (dma_data->src_sel == DmaDataSrc::Data && dma_data->dst_sel == DmaDataDst::Gds) {
        rasterizer.FillBuffer(dma_data->dst_addr_lo, dma_data->NumBytes(),
                               dma_data->data, true);
    } else if ((dma_data->src_sel == DmaDataSrc::Memory ||
                dma_data->src_sel == DmaDataSrc::MemoryUsingL2) &&
               dma_data->dst_sel == DmaDataDst::Gds) {
        rasterizer.CopyBuffer(dma_data->dst_addr_lo, dma_data->SrcAddress<VAddr>(),
                               dma_data->NumBytes(), true, false);
    } else if (dma_data->src_sel == DmaDataSrc::Data &&
               (dma_data->dst_sel == DmaDataDst::Memory ||
                dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
        rasterizer.FillBuffer(dma_data->DstAddress<VAddr>(), dma_data->NumBytes(),
                               dma_data->data, false);
    } else if (dma_data->src_sel == DmaDataSrc::Gds &&
               (dma_data->dst_sel == DmaDataDst::Memory ||
                dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
        rasterizer.CopyBuffer(dma_data->DstAddress<VAddr>(), dma_data->src_addr_lo,
                               dma_data->NumBytes(), false, true);
    } else if ((dma_data->src_sel == DmaDataSrc::Memory ||
                dma_data->src_sel == DmaDataSrc::MemoryUsingL2) &&
               (dma_data->dst_sel == DmaDataDst::Memory ||
                dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
        rasterizer.CopyBuffer(dma_data->DstAddress<VAddr>(),
                               dma_data->SrcAddress<VAddr>(), dma_data->NumBytes(),
                               false, false);
    } else {
        UNREACHABLE_MSG("WriteData src_sel = {}, dst_sel = {}", u32(dma_data->src_sel),
                        u32(dma_data->dst_sel));
    }
}

void SignalFlip(Vulkan::Rasterizer& rasterizer, const u8*) {
    rasterizer.WaitDeferredSignals();
    Platform::IrqC::Instance()->Signal(Platform::InterruptId::GfxFlip);
}

void RunWriteData(Vulkan::Rasterizer& rasterizer, const u8* data) {
    const auto* header = reinterpret_cast<const PM4Header*>(data);
    const auto* write_data = reinterpret_cast<const PM4CmdWriteData*>(data);
    // bbport BB_GUEST_IN_PLACE: the GPU writes it in stream order, as the command processor
    // does. A CPU store here lands before the GPU has run the work recorded before it, which may
    // still read the old value (the game orders such writes after its own fences and
    // WAIT_REG_MEMs, which this thread takes as met in stream order).
    if (rasterizer.WriteDataOnGpu(write_data->Address<VAddr>(), write_data->data,
                                  (header->type3.count.Value() - 2) * sizeof(u32))) {
        return;
    }
    // Copies deferred to the recording thread that read this range precede the write (the
    // command processor's writes do not wait for earlier draws: the game cannot take one for
    // a sign that the GPU is past them); fences deferred to it (RecorderFences) precede writes
    // the guest sees.
    rasterizer.WaitHostCopiesFor(write_data->Address<VAddr>(),
                                 (header->type3.count.Value() - 2) * sizeof(u32));
    rasterizer.WaitDeferredSignals();
    BbFreeCheck::Check(write_data->Address<u64>(), (header->type3.count.Value() - 2) * sizeof(u32),
                       write_data->data, BbFreeCheck::WriteData);
    BbWriteLog::Note(write_data->Address<u64>(), write_data->data,
                     (header->type3.count.Value() - 2) * sizeof(u32), BbWriteLog::WriteData);
    std::memcpy(write_data->Address<u64*>(), write_data->data,
                (header->type3.count.Value() - 2) * sizeof(u32));
    rasterizer.NoteCommandWrite(write_data->Address<VAddr>(), write_data->data,
                                (header->type3.count.Value() - 2) * sizeof(u32), true);
}

void WriteEosLabel(void* address, u64 value, u32 num_bytes) {
    auto* memory = Core::Memory::Instance();
    BbFreeCheck::Check(reinterpret_cast<u64>(address), num_bytes, &value, BbFreeCheck::Eos);
    BbFreeCheck::NoteFenceWriting(reinterpret_cast<u64>(address));
    BbWriteLog::Note(reinterpret_cast<u64>(address), &value, num_bytes, BbWriteLog::Fence);
    if (!memory->TryWriteBacking(address, &value, num_bytes)) {
        StoreLabel(address, value, num_bytes);
    }
    // bbport: a VRAM copy of the label gets it too (the backing mapping is not watched).
    if (auto* rasterizer = memory->GetRasterizer()) {
        rasterizer->NoteLateCommandWrite(reinterpret_cast<VAddr>(address), &value, num_bytes);
    }
    BbFreeCheck::NoteFenceWritten(reinterpret_cast<u64>(address));
}

void RunEventWriteEos(Vulkan::Rasterizer& rasterizer, const u8* data) {
    const auto& event_eos = *reinterpret_cast<const PM4CmdEventWriteEos*>(data);
    // BB_HONEST_LABELS: end of shader work, so written once the GPU has finished it.
    if (Vulkan::Rasterizer::HonestLabels() &&
        event_eos.command == PM4CmdEventWriteEos::Command::SignalFence) {
        rasterizer.ProcessDownloadImages();
        const PM4CmdEventWriteEos eos = event_eos;
        rasterizer.SignalAfterGpu([eos] { eos.SignalFence(&WriteEosLabel); },
                                  reinterpret_cast<VAddr>(eos.Address()), eos.DataDWord());
        return;
    }
    // Copies deferred to the recording thread precede writes the guest sees.
    rasterizer.WaitHostCopies();
    rasterizer.WaitDeferredSignals();
    rasterizer.ProcessDownloadImages();
    event_eos.SignalFence(&WriteEosLabel);
    if (event_eos.command == PM4CmdEventWriteEos::Command::GdsStore) {
        ASSERT(event_eos.size == 1);
        rasterizer.Finish();
        const u32 value = rasterizer.ReadDataFromGds(event_eos.gds_index);
        BbFreeCheck::Check(reinterpret_cast<u64>(event_eos.Address()), sizeof(u32), &value,
                           BbFreeCheck::EosGds);
        BbFreeCheck::NoteFenceWriting(reinterpret_cast<u64>(event_eos.Address()));
        *event_eos.Address() = value;
        BbFreeCheck::NoteFenceWritten(reinterpret_cast<u64>(event_eos.Address()));
    }
}
} // namespace

void Liverpool::NotePendingFences(const auto& event) {
    const u64 position = rasterizer->DrawPipeHead();
    while (!pending_fences.empty() && rasterizer->DrawPipeReached(pending_fences.front().position) &&
           rasterizer->SignalsPublished(pending_fences.front().position)) {
        pending_fences.pop_front();
    }
    constexpr bool is_eop = requires { event.SignalFence([](void*, u64, u32) {}, [] {}); };
    const auto note = [&](void* address, u64 data, u32 num_bytes) {
        BbWriteLog::NoteIntent(reinterpret_cast<u64>(address), &data, num_bytes,
                               is_eop ? BbWriteLog::FenceIntent : BbWriteLog::EosIntent);
        pending_fences.push_back({reinterpret_cast<VAddr>(address), data, num_bytes, position});
        rasterizer->NotePendingGpuWrite(reinterpret_cast<VAddr>(address), num_bytes);
    };
    if constexpr (requires { event.SignalFence(note, [] {}); }) {
        event.SignalFence(note, [] {});
    } else {
        event.SignalFence(note);
    }
}

void Liverpool::NotePendingWrite(const PM4CmdWriteData& write_data, u32 num_bytes) {
    if (num_bytes > sizeof(u64)) {
        return; // labels are small; larger writes are not waited on
    }
    u64 data = 0;
    std::memcpy(&data, write_data.data, num_bytes);
    pending_fences.push_back(
        {write_data.Address<VAddr>(), data, num_bytes, rasterizer->DrawPipeHead()});
}

bool Liverpool::PendingFenceValue(VAddr address, u32& value) {
    for (auto it = pending_fences.rbegin(); it != pending_fences.rend(); ++it) {
        if (rasterizer->DrawPipeReached(it->position) && rasterizer->SignalsPublished(it->position)) {
            break; // written already, or a queued GPU signal (PendingSignalValue below)
        }
        if (address >= it->address && address + sizeof(u32) <= it->address + it->num_bytes) {
            u64 data = it->data;
            value = static_cast<u32>(data >> ((address - it->address) * 8));
            return true;
        }
    }
    // BB_HONEST_LABELS: past the recording thread, a fence waits for the GPU to finish the work
    // before it; in stream order it is met already.
    if (Vulkan::Rasterizer::HonestLabels() && rasterizer) {
        u64 data = 0;
        if (rasterizer->PendingSignalValue(address, data)) {
            value = static_cast<u32>(data);
            return true;
        }
        if ((address & 7) == 4 && rasterizer->PendingSignalValue(address - 4, data)) {
            value = static_cast<u32>(data >> 32); // the high half of a 64-bit fence
            return true;
        }
    }
    return false;
}

Liverpool::Task Liverpool::ProcessCeUpdate(std::span<const u32> ccb) {
    FIBER_ENTER(ccb_task_name);

    while (!ccb.empty()) {
        ProcessCommands();

        const auto* header = reinterpret_cast<const PM4Header*>(ccb.data());
        const u32 type = header->type;
        if (type != 3) {
            // No other types of packets were spotted so far
            UNREACHABLE_MSG("Invalid PM4 type {}", type);
        }

        const PM4ItOpcode opcode = header->type3.opcode;
        const auto* it_body = reinterpret_cast<const u32*>(header) + 1;
        switch (opcode) {
        case PM4ItOpcode::Nop: {
            // const auto* nop = reinterpret_cast<const PM4CmdNop*>(header);
            break;
        }
        case PM4ItOpcode::WriteConstRam: {
            const auto* write_const = reinterpret_cast<const PM4WriteConstRam*>(header);
            memcpy(cblock.constants_heap.data() + write_const->Offset(), &write_const->data,
                   write_const->Size());
            break;
        }
        case PM4ItOpcode::DumpConstRam: {
            const auto* dump_const = reinterpret_cast<const PM4DumpConstRam*>(header);
            if (BbCeStats::Enabled()) {
                BbCeStats::NoteDump(dump_const->Address<VAddr>(), dump_const->Size());
            }
            // bbport: earlier draws on the recording thread may still read constants here: only
            // those that read these bytes are waited for, not all of them.
            if (rasterizer) {
                rasterizer->WaitForPendingReads(dump_const->Address<VAddr>(), dump_const->Size());
            }
            BbFreeCheck::Check(dump_const->Address<u64>(), dump_const->Size(),
                               cblock.constants_heap.data() + dump_const->Offset(),
                               BbFreeCheck::ConstRam);
            memcpy(dump_const->Address<void*>(),
                   cblock.constants_heap.data() + dump_const->Offset(), dump_const->Size());
            // bbport: no write tracking (BB_GUEST_IN_PLACE): the caches hear of it (in order, on the
            // recording thread).
            if (rasterizer && !VideoCore::WriteTracking()) {
                rasterizer->NoteCommandWriteInOrder(dump_const->Address<VAddr>(),
                                                    cblock.constants_heap.data() + dump_const->Offset(),
                                                    dump_const->Size());
            }
            break;
        }
        case PM4ItOpcode::IncrementCeCounter: {
            ++cblock.ce_count;
            break;
        }
        case PM4ItOpcode::WaitOnDeCounterDiff: {
            const auto diff = it_body[0];
            while ((cblock.de_count - cblock.ce_count) >= diff) {
                YIELD_CE();
            }
            break;
        }
        case PM4ItOpcode::IndirectBufferConst: {
            const auto* indirect_buffer = reinterpret_cast<const PM4CmdIndirectBuffer*>(header);
            auto task =
                ProcessCeUpdate({indirect_buffer->Address<const u32>(), indirect_buffer->ib_size});
            RESUME_CE(task);

            while (!task.handle.done()) {
                YIELD_CE();
                RESUME_CE(task);
            }
            break;
        }
        default:
            const u32 count = header->type3.NumWords();
            UNREACHABLE_MSG("Unknown PM4 type 3 opcode {:#x} with count {}",
                            static_cast<u32>(opcode), count);
        }
        ccb = NextPacket(ccb, header->type3.NumWords() + 1);
    }

    FIBER_EXIT;
}

// bbport: graphics-register effects of a type-3 packet. The GPU thread and the draw-preparation
// workers both go through this function, and both fold every register-writing packet into a
// running checksum: equal checksums at a draw mean equal register files.
u64 Liverpool::HashRegisterPacket(u64 checksum, const u32* words, u32 count) {
    for (u32 i = 0; i < count; ++i) {
        checksum = (checksum ^ words[i]) * 0x9E3779B97F4A7C15ull;
        checksum ^= checksum >> 29;
    }
    return checksum;
}

void Liverpool::ApplyGraphicsRegisterPacket(Regs& regs, const PM4Header* header, u64& checksum,
                                             RegDirty* dirty) {
    const u32 count = header->type3.NumWords();
    const auto* payload = reinterpret_cast<const u32*>(header + 2);
    const auto mark = [&](u32 word, u32 words) {
        if (dirty) {
            dirty->Mark(word, words);
        }
    };
    const auto mark_field = [&](const auto& field) {
        if (dirty) {
            dirty->MarkField(regs, field);
        }
    };
    switch (header->type3.opcode) {
    case PM4ItOpcode::ClearState:
        regs.SetDefaults();
        if (dirty) {
            dirty->Clear();
            dirty->reset = true;
        }
        break;
    case PM4ItOpcode::SetConfigReg: {
        const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
        std::memcpy(&regs.reg_array[Regs::ConfigRegWordOffset + set_data->reg_offset], payload,
                    (count - 1) * sizeof(u32));
        mark(Regs::ConfigRegWordOffset + set_data->reg_offset, count - 1);
        break;
    }
    case PM4ItOpcode::SetContextReg: {
        const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
        std::memcpy(&regs.reg_array[Regs::ContextRegWordOffset + set_data->reg_offset], payload,
                    (count - 1) * sizeof(u32));
        mark(Regs::ContextRegWordOffset + set_data->reg_offset, count - 1);
        break;
    }
    case PM4ItOpcode::SetShReg: {
        const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
        // The compute program range goes to the queue's cs_state, not to regs.
        if (!(set_data->reg_offset >= 0x200 &&
              set_data->reg_offset <= (0x200 + sizeof(ComputeProgram) / 4))) {
            std::memcpy(&regs.reg_array[Regs::ShRegWordOffset + set_data->reg_offset], payload,
                        (count - 1) * sizeof(u32));
            mark(Regs::ShRegWordOffset + set_data->reg_offset, count - 1);
        }
        break;
    }
    case PM4ItOpcode::SetUconfigReg: {
        const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
        std::memcpy(&regs.reg_array[Regs::UconfigRegWordOffset + set_data->reg_offset], payload,
                    (count - 1) * sizeof(u32));
        mark(Regs::UconfigRegWordOffset + set_data->reg_offset, count - 1);
        break;
    }
    case PM4ItOpcode::IndexType:
        regs.index_buffer_type.raw = reinterpret_cast<const PM4CmdDrawIndexType*>(header)->raw;
        mark_field(regs.index_buffer_type);
        break;
    case PM4ItOpcode::DrawIndex2: {
        const auto* draw_index = reinterpret_cast<const PM4CmdDrawIndex2*>(header);
        regs.max_index_size = draw_index->max_size;
        regs.index_base_address.base_addr_lo = draw_index->index_base_lo;
        regs.index_base_address.base_addr_hi = draw_index->index_base_hi;
        regs.num_indices = draw_index->index_count;
        mark_field(regs.max_index_size);
        mark_field(regs.index_base_address);
        mark_field(regs.num_indices);
        regs.draw_initiator = draw_index->draw_initiator;
        mark_field(regs.draw_initiator);
        break;
    }
    case PM4ItOpcode::DrawIndexOffset2: {
        const auto* draw_index_off = reinterpret_cast<const PM4CmdDrawIndexOffset2*>(header);
        regs.max_index_size = draw_index_off->max_size;
        regs.num_indices = draw_index_off->index_count;
        regs.draw_initiator = draw_index_off->draw_initiator;
        mark_field(regs.max_index_size);
        mark_field(regs.num_indices);
        mark_field(regs.draw_initiator);
        break;
    }
    case PM4ItOpcode::DrawIndexAuto: {
        const auto* draw_index = reinterpret_cast<const PM4CmdDrawIndexAuto*>(header);
        mark_field(regs.num_indices);
        mark_field(regs.draw_initiator);
        regs.num_indices = draw_index->index_count;
        regs.draw_initiator = draw_index->draw_initiator;
        break;
    }
    case PM4ItOpcode::NumInstances:
        regs.num_instances.num_instances =
            reinterpret_cast<const PM4CmdDrawNumInstances*>(header)->num_instances;
        mark_field(regs.num_instances);
        break;
    case PM4ItOpcode::IndexBase: {
        const auto* index_base = reinterpret_cast<const PM4CmdDrawIndexBase*>(header);
        regs.index_base_address.base_addr_lo = index_base->addr_lo;
        regs.index_base_address.base_addr_hi = index_base->addr_hi;
        mark_field(regs.index_base_address);
        break;
    }
    case PM4ItOpcode::IndexBufferSize:
        regs.num_indices = reinterpret_cast<const PM4CmdDrawIndexBufferSize*>(header)->num_indices;
        mark_field(regs.num_indices);
        break;
    case PM4ItOpcode::EventWrite: {
        const auto* event = reinterpret_cast<const PM4CmdEventWrite*>(header);
        if (event->event_type.Value() != EventType::SoVgtStreamoutFlush) {
            return;
        }
        // TODO: handle proper synchronization, for now signal that update is done immediately
        regs.cp_strmout_cntl.offset_update_done = 1;
        mark_field(regs.cp_strmout_cntl);
        break;
    }
    default:
        return;
    }
    checksum = HashRegisterPacket(checksum, reinterpret_cast<const u32*>(header), count + 1);
}

// bbport: BB_DCB_STATS=1 — structure of graphics command buffers (read-only scan, printed every
// 5 s): how many buffers, draws per buffer, state set before the first draw. Input for
// processing command buffers on several threads.
namespace {
struct DcbStats {
    u64 buffers[2]{}, with_draws[2]{}, draws[2]{}, dispatches[2]{}, max_draws[2]{};
    u64 clear_first[2]{}, ctx_before[2]{}, sh_before[2]{}, ibs[2]{}, dwords[2]{};
    u64 hist[2][6]{}; // draws per buffer: 0, 1-9, 10-99, 100-499, 500-1999, 2000+
    std::chrono::steady_clock::time_point window = std::chrono::steady_clock::now();
};
DcbStats g_dcb_stats;
int g_dcb_depth;

bool IsDrawOpcode(PM4ItOpcode op) {
    switch (op) {
    case PM4ItOpcode::DrawIndex2:
    case PM4ItOpcode::DrawIndexOffset2:
    case PM4ItOpcode::DrawIndexAuto:
    case PM4ItOpcode::DrawIndirect:
    case PM4ItOpcode::DrawIndirectMulti:
    case PM4ItOpcode::DrawIndexIndirect:
    case PM4ItOpcode::DrawIndexIndirectMulti:
    case PM4ItOpcode::DrawIndexIndirectCountMulti:
        return true;
    default:
        return false;
    }
}

void ScanDcb(std::span<const u32> dcb, int depth) {
    auto& st = g_dcb_stats;
    const int d = depth > 0 ? 1 : 0;
    u64 draws = 0, dispatches = 0, ctx = 0, sh = 0, ibs = 0;
    bool clear_first = false;
    st.dwords[d] += dcb.size();
    for (size_t at = 0; at < dcb.size();) {
        const auto* header = reinterpret_cast<const PM4Header*>(dcb.data() + at);
        if (header->type == 2) {
            ++at;
            continue;
        }
        if (header->type != 3) {
            break;
        }
        const PM4ItOpcode op = header->type3.opcode;
        if (IsDrawOpcode(op)) {
            ++draws;
        } else if (op == PM4ItOpcode::DispatchDirect || op == PM4ItOpcode::DispatchIndirect) {
            ++dispatches;
        } else if (op == PM4ItOpcode::IndirectBuffer) {
            ++ibs;
        } else if (draws == 0) {
            if (op == PM4ItOpcode::ClearState) {
                clear_first = true;
            } else if (op == PM4ItOpcode::SetContextReg) {
                ctx += header->type3.NumWords() - 1;
            } else if (op == PM4ItOpcode::SetShReg) {
                sh += header->type3.NumWords() - 1;
            }
        }
        at += header->type3.NumWords() + 1;
    }
    ++st.buffers[d];
    st.draws[d] += draws;
    st.dispatches[d] += dispatches;
    st.ibs[d] += ibs;
    st.max_draws[d] = std::max(st.max_draws[d], draws);
    if (draws) {
        ++st.with_draws[d];
        st.clear_first[d] += clear_first;
        st.ctx_before[d] += ctx;
        st.sh_before[d] += sh;
    }
    const int bucket = draws == 0 ? 0 : draws < 10 ? 1 : draws < 100 ? 2 : draws < 500 ? 3
                     : draws < 2000 ? 4 : 5;
    ++st.hist[d][bucket];

    const auto now = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(now - st.window).count();
    if (depth == 0 && secs >= 5.0) {
        for (int k = 0; k < 2; ++k) {
            const double with = std::max<u64>(st.with_draws[k], 1);
            std::printf("DCB stats %s: %.0f buffers/s (%.0f with draws), %.0f draws/s, %.0f "
                        "dispatches/s, max %llu draws in one buffer, %.0f IBs/s, %.0f kdw/s; "
                        "draw buffers: %.0f%% start with ClearState, %.0f ctx + %.0f sh reg "
                        "writes before first draw; draws/buffer hist 0:%llu 1-9:%llu 10-99:%llu "
                        "100-499:%llu 500-1999:%llu 2000+:%llu\n",
                        k ? "indirect" : "submitted", st.buffers[k] / secs, st.with_draws[k] / secs,
                        st.draws[k] / secs, st.dispatches[k] / secs,
                        static_cast<unsigned long long>(st.max_draws[k]), st.ibs[k] / secs,
                        st.dwords[k] / secs / 1000.0, 100.0 * st.clear_first[k] / with,
                        st.ctx_before[k] / with, st.sh_before[k] / with,
                        static_cast<unsigned long long>(st.hist[k][0]),
                        static_cast<unsigned long long>(st.hist[k][1]),
                        static_cast<unsigned long long>(st.hist[k][2]),
                        static_cast<unsigned long long>(st.hist[k][3]),
                        static_cast<unsigned long long>(st.hist[k][4]),
                        static_cast<unsigned long long>(st.hist[k][5]));
        }
        st = DcbStats{};
    }
}
// bbport: an invalid packet header (Steam Deck: "PM4 type 0" with dword 0, 8 or 0x10, mid-game): where
// in which buffer, what surrounds it, and which logged writes of ours landed in the buffer.
void ReportBadPacket(uintptr_t base, std::size_t dwords, const u32* at, u64 seq, int depth) {
    const uintptr_t address = reinterpret_cast<uintptr_t>(at);
    int prot = 0, type = -1;
    uintptr_t end = 0;
    runtime_memory_vma_info(address, &prot, &type, &end);
    std::fprintf(stderr,
                 "Bad PM4 packet at %#llx: dword %#x, %#llx bytes into a %#llx-byte buffer at %#llx "
                 "(IB depth %d, submission %llu; memory type %d, prot %#x)\n",
                 (unsigned long long)address, *at, (unsigned long long)(address - base),
                 (unsigned long long)(dwords * 4), (unsigned long long)base, depth,
                 (unsigned long long)seq, type, prot);
    const u32* first = reinterpret_cast<const u32*>(std::max(base, address - 64));
    const u32* last = std::min(reinterpret_cast<const u32*>(base) + dwords, at + 16);
    for (const u32* p = first; p < last; p += 8) {
        std::fprintf(stderr, "  %#llx:", (unsigned long long)reinterpret_cast<uintptr_t>(p));
        for (const u32* q = p; q < std::min(p + 8, last); ++q) {
            std::fprintf(stderr, q == at ? " [%08x]" : " %08x", *q);
        }
        std::fprintf(stderr, "\n");
    }
    BbWriteLog::DumpRange(base, dwords * 4);
    std::fflush(stderr);
}
} // namespace

Liverpool::Task Liverpool::ProcessGraphics(std::span<const u32> dcb, std::span<const u32> ccb,
                                           u64 seq, std::shared_ptr<SubmittedCopy> copy) {
    FIBER_ENTER(dcb_task_name);
    // Top-level buffers enqueued for the draw preparation workers (nested IBs are not).
    Vulkan::DrawPreparation* draw_prep =
        seq != NoSeq && rasterizer ? &rasterizer->GetDrawPreparation() : nullptr;
    if (draw_prep) {
        draw_prep->BeginSubmission(seq, regs, gfx_reg_checksum);
    }
    static const bool dcb_stats = EmulatorSettingsImpl::Flag("BB_DCB_STATS", false);
    const int dcb_depth = g_dcb_depth;
    if (dcb_stats) {
        ScanDcb(dcb, dcb_depth);
    }

    cblock.Reset();

    // TODO: potentially, ASCs also can depend on CE and in this case the
    // CE task should be moved into more global scope
    Task ce_task{};

    if (!ccb.empty()) {
        // In case of CCB provided kick off CE asap to have the constant heap ready to use
        ce_task = ProcessCeUpdate(ccb);
        RESUME_GFX(ce_task);
    }

    const auto base_addr = reinterpret_cast<uintptr_t>(dcb.data());
    const std::size_t dcb_dwords = dcb.size();
    while (!dcb.empty()) {
        ProcessCommands();

        const auto* header = reinterpret_cast<const PM4Header*>(dcb.data());
        const u32 type = header->type;

        switch (type) {
        default:
            ReportBadPacket(base_addr, dcb_dwords, dcb.data(), seq, dcb_depth);
            UNREACHABLE_MSG("Wrong PM4 type {}", type);
            break;
        case 0:
            ReportBadPacket(base_addr, dcb_dwords, dcb.data(), seq, dcb_depth);
            UNREACHABLE_MSG("Unimplemented PM4 type 0, base reg: {}, size: {}",
                            header->type0.base.Value(), header->type0.NumWords());
            break;
        case 2:
            // Type-2 packet are used for padding purposes
            dcb = NextPacket(dcb, 1);
            continue;
        case 3:
            const u32 count = header->type3.NumWords();
            const PM4ItOpcode opcode = header->type3.opcode;
            ApplyGraphicsRegisterPacket(regs, header, gfx_reg_checksum, &pipe_dirty);
            // DmaData to 0x3022C does nothing here (skipped below): no need to wait.
            if (rasterizer && !PipelinedOpcode(opcode) &&
                !(opcode == PM4ItOpcode::DmaData &&
                  reinterpret_cast<const PM4DmaData*>(header)->dst_addr_lo == 0x3022C)) {
                rasterizer->DrainDrawPipe(static_cast<u32>(opcode));
                // They may write guest memory: not before fences deferred earlier.
                rasterizer->WaitDeferredSignals();
            }
            switch (opcode) {
            case PM4ItOpcode::Nop: {
                const auto* nop = reinterpret_cast<const PM4CmdNop*>(header);
                if (nop->header.count.Value() == 0) {
                    break;
                }

                switch (nop->data_block[0]) {
                case PM4CmdNop::PayloadType::PatchedFlip: {
                    // There is no evidence that GPU CP drives flip events by parsing
                    // special NOP packets. For convenience lets assume that it does.
                    // bbport: after the writes before it (the buffer label: WriteData, which
                    // may still be queued on the draw recording thread).
                    if (rasterizer) {
                        rasterizer->RunInOrder(&SignalFlip, header, sizeof(u32),
                                               BbToggle::PipelinedMemoryWrites, false);
                    } else {
                        Platform::IrqC::Instance()->Signal(Platform::InterruptId::GfxFlip);
                    }
                    break;
                }
                case PM4CmdNop::PayloadType::DebugMarkerPush: {
                    if (guest_markers_enabled) {
                        const auto marker_sz = nop->header.count.Value() * 2;
                        const std::string_view label{
                            reinterpret_cast<const char*>(&nop->data_block[1]), marker_sz};
                        rasterizer->ScopeMarkerBegin(label, true);
                    }
                    break;
                }
                case PM4CmdNop::PayloadType::DebugColorMarkerPush: {
                    if (guest_markers_enabled) {
                        const auto marker_sz = nop->header.count.Value() * 2;
                        const std::string_view label{
                            reinterpret_cast<const char*>(&nop->data_block[1]), marker_sz};
                        const u32 color = *reinterpret_cast<const u32*>(
                            reinterpret_cast<const u8*>(&nop->data_block[1]) + marker_sz);
                        rasterizer->ScopedMarkerInsertColor(label, color, true);
                    }
                    break;
                }
                case PM4CmdNop::PayloadType::DebugMarkerPop: {
                    if (guest_markers_enabled) {
                        rasterizer->ScopeMarkerEnd(true);
                    }
                    break;
                }
                default:
                    break;
                }
                break;
            }
            case PM4ItOpcode::ContextControl: {
                break;
            }
            case PM4ItOpcode::ClearState:
            case PM4ItOpcode::SetConfigReg: {
                break; // registers: ApplyGraphicsRegisterPacket
            }
            case PM4ItOpcode::SetContextReg: {
                const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
                const auto reg_addr = Regs::ContextRegWordOffset + set_data->reg_offset;
                const auto* payload = reinterpret_cast<const u32*>(header + 2);

                // In the case of HW, render target memory has alignment as color block operates on
                // tiles. There is no information of actual resource extents stored in CB context
                // regs, so any deduction of it from slices/pitch will lead to a larger surface
                // created. The same applies to the depth targets. Fortunately, the guest always
                // sends a trailing NOP packet right after the context regs setup, so we can use the
                // heuristic below and extract the hint to determine actual resource dims.

                switch (reg_addr) {
                case ContextRegs::CbColor0Base:
                case ContextRegs::CbColor1Base:
                case ContextRegs::CbColor2Base:
                case ContextRegs::CbColor3Base:
                case ContextRegs::CbColor4Base:
                case ContextRegs::CbColor5Base:
                case ContextRegs::CbColor6Base:
                case ContextRegs::CbColor7Base: {
                    const auto col_buf_id = (reg_addr - ContextRegs::CbColor0Base) /
                                            (ContextRegs::CbColor1Base - ContextRegs::CbColor0Base);
                    ASSERT(col_buf_id < NUM_COLOR_BUFFERS);

                    const auto nop_offset = header->type3.count;
                    if (nop_offset == 0x0e || nop_offset == 0x0d || nop_offset == 0x0b) {
                        ASSERT_MSG(payload[nop_offset] == 0xc0001000,
                                   "NOP hint is missing in CB setup sequence");
                        last_cb_extent[col_buf_id].raw = payload[nop_offset + 1];
                    } else {
                        last_cb_extent[col_buf_id].raw = 0;
                    }
                    break;
                }
                case ContextRegs::CbColor0Cmask:
                case ContextRegs::CbColor1Cmask:
                case ContextRegs::CbColor2Cmask:
                case ContextRegs::CbColor3Cmask:
                case ContextRegs::CbColor4Cmask:
                case ContextRegs::CbColor5Cmask:
                case ContextRegs::CbColor6Cmask:
                case ContextRegs::CbColor7Cmask: {
                    const auto col_buf_id =
                        (reg_addr - ContextRegs::CbColor0Cmask) /
                        (ContextRegs::CbColor1Cmask - ContextRegs::CbColor0Cmask);
                    ASSERT(col_buf_id < NUM_COLOR_BUFFERS);

                    const auto nop_offset = header->type3.count;
                    if (nop_offset == 0x04) {
                        ASSERT_MSG(payload[nop_offset] == 0xc0001000,
                                   "NOP hint is missing in CB setup sequence");
                        last_cb_extent[col_buf_id].raw = payload[nop_offset + 1];
                    }
                    break;
                }
                case ContextRegs::DbZInfo: {
                    if (header->type3.count == 8) {
                        ASSERT_MSG(payload[20] == 0xc0001000,
                                   "NOP hint is missing in DB setup sequence");
                        last_db_extent.raw = payload[21];
                    } else {
                        last_db_extent.raw = 0;
                    }
                    break;
                }
                default:
                    break;
                }
                break;
            }
            case PM4ItOpcode::SetShReg: {
                const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
                const auto set_size = (count - 1) * sizeof(u32);

                if (set_data->reg_offset >= 0x200 &&
                    set_data->reg_offset <= (0x200 + sizeof(ComputeProgram) / 4)) {
                    ASSERT(set_size <= sizeof(ComputeProgram));
                    auto* addr = reinterpret_cast<u32*>(&mapped_queues[GfxQueueId].cs_state) +
                                 (set_data->reg_offset - 0x200);
                    std::memcpy(addr, header + 2, set_size);
                } // other SH registers: ApplyGraphicsRegisterPacket
                break;
            }
            case PM4ItOpcode::SetUconfigReg: {
                break; // registers: ApplyGraphicsRegisterPacket
            }
            case PM4ItOpcode::SetPredication: {
                LOG_WARNING(Render, "Unimplemented IT_SET_PREDICATION");
                break;
            }
            case PM4ItOpcode::IndexType: {
                break; // registers: ApplyGraphicsRegisterPacket
            }
            case PM4ItOpcode::DrawIndex2: {
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                const auto* prepared = draw_prep ? draw_prep->NextDraw() : nullptr;
                rasterizer->ScopeMarker("gfx:{}:DrawIndex2", fmt::make_format_args(cmd_address),
                                        [&] { rasterizer->Draw(true, 0, prepared); });
                break;
            }
            case PM4ItOpcode::DrawIndexOffset2: {
                const auto* draw_index_off =
                    reinterpret_cast<const PM4CmdDrawIndexOffset2*>(header);
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                const auto* prepared = draw_prep ? draw_prep->NextDraw() : nullptr;
                rasterizer->ScopeMarker(
                    "gfx:{}:DrawIndexOffset2", fmt::make_format_args(cmd_address),
                    [&] { rasterizer->Draw(true, draw_index_off->index_offset, prepared); });
                break;
            }
            case PM4ItOpcode::DrawIndexAuto: {
                const auto* draw_index = reinterpret_cast<const PM4CmdDrawIndexAuto*>(header);
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                const auto* prepared = draw_prep ? draw_prep->NextDraw() : nullptr;
                rasterizer->ScopeMarker("gfx:{}:DrawIndexAuto", fmt::make_format_args(cmd_address),
                                        [&] { rasterizer->Draw(false, 0, prepared); });
                break;
            }
            case PM4ItOpcode::DrawIndirect: {
                const auto* draw_indirect = reinterpret_cast<const PM4CmdDrawIndirect*>(header);
                const auto offset = draw_indirect->data_offset;
                const auto stride = sizeof(DrawIndirectArgs);
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker(
                    "gfx:{}:DrawIndirect", fmt::make_format_args(cmd_address), [&] {
                        rasterizer->DrawIndirect(false, indirect_args_addr, offset, stride, 1, 0,
                                                 draw_indirect->base_vtx_loc,
                                                 draw_indirect->start_inst_loc);
                    });
                break;
            }
            case PM4ItOpcode::DrawIndirectMulti: {
                const auto* draw_indirect =
                    reinterpret_cast<const PM4CmdDrawIndirectMulti*>(header);
                const auto offset = draw_indirect->data_offset;
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker(
                    "gfx:{}:DrawIndirectMulti", fmt::make_format_args(cmd_address), [&] {
                        rasterizer->DrawIndirect(false, indirect_args_addr, offset,
                                                 draw_indirect->stride, draw_indirect->count, 0,
                                                 draw_indirect->base_vtx_loc,
                                                 draw_indirect->start_inst_loc);
                    });
                break;
            }
            case PM4ItOpcode::DrawIndexIndirect: {
                const auto* draw_index_indirect =
                    reinterpret_cast<const PM4CmdDrawIndexIndirect*>(header);
                const auto offset = draw_index_indirect->data_offset;
                const auto stride = sizeof(DrawIndexedIndirectArgs);
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker(
                    "gfx:{}:DrawIndexIndirect", fmt::make_format_args(cmd_address), [&] {
                        rasterizer->DrawIndirect(true, indirect_args_addr, offset, stride, 1, 0,
                                                 draw_index_indirect->base_vtx_loc,
                                                 draw_index_indirect->start_inst_loc);
                    });
                break;
            }
            case PM4ItOpcode::DrawIndexIndirectMulti: {
                const auto* draw_index_indirect =
                    reinterpret_cast<const PM4CmdDrawIndexIndirectMulti*>(header);
                const auto offset = draw_index_indirect->data_offset;
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker(
                    "gfx:{}:DrawIndexIndirectMulti", fmt::make_format_args(cmd_address), [&] {
                        rasterizer->DrawIndirect(
                            true, indirect_args_addr, offset, draw_index_indirect->stride,
                            draw_index_indirect->count, 0, draw_index_indirect->base_vtx_loc,
                            draw_index_indirect->start_inst_loc);
                    });
                break;
            }
            case PM4ItOpcode::DrawIndexIndirectCountMulti: {
                const auto* draw_index_indirect =
                    reinterpret_cast<const PM4CmdDrawIndexIndirectCountMulti*>(header);
                const auto offset = draw_index_indirect->data_offset;
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker(
                    "gfx:{}:DrawIndexIndirectCountMulti", fmt::make_format_args(cmd_address), [&] {
                        rasterizer->DrawIndirect(
                            true, indirect_args_addr, offset, draw_index_indirect->stride,
                            draw_index_indirect->count,
                            draw_index_indirect->count_indirect_enable.Value()
                                ? draw_index_indirect->count_addr
                                : 0,
                            draw_index_indirect->base_vtx_loc, draw_index_indirect->start_inst_loc);
                    });
                break;
            }
            case PM4ItOpcode::DispatchDirect: {
                const auto* dispatch_direct = reinterpret_cast<const PM4CmdDispatchDirect*>(header);
                auto& cs_program = GetCsRegs();
                cs_program.dim_x = dispatch_direct->dim_x;
                cs_program.dim_y = dispatch_direct->dim_y;
                cs_program.dim_z = dispatch_direct->dim_z;
                cs_program.dispatch_initiator = dispatch_direct->dispatch_initiator;
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDumpCompute(base_addr, reinterpret_cast<uintptr_t>(header),
                                                   cs_program);
                }
                if (!rasterizer || (cs_program.dispatch_initiator & 1) == 0) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker("gfx:{}:DispatchDirect", fmt::make_format_args(cmd_address),
                                        [&] { rasterizer->DispatchDirect(); });
                break;
            }
            case PM4ItOpcode::DispatchIndirect: {
                const auto* dispatch_indirect =
                    reinterpret_cast<const PM4CmdDispatchIndirect*>(header);
                auto& cs_program = GetCsRegs();
                const auto offset = dispatch_indirect->data_offset;
                const auto size = sizeof(PM4CmdDispatchIndirect::GroupDimensions);
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDumpCompute(base_addr, reinterpret_cast<uintptr_t>(header),
                                                   cs_program);
                }
                if (!rasterizer || (cs_program.dispatch_initiator & 1) == 0) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker(
                    "gfx:{}:DispatchIndirect", fmt::make_format_args(cmd_address),
                    [&] { rasterizer->DispatchIndirect(indirect_args_addr, offset, size); });
                break;
            }
            case PM4ItOpcode::NumInstances:
            case PM4ItOpcode::IndexBase:
            case PM4ItOpcode::IndexBufferSize: {
                break; // registers: ApplyGraphicsRegisterPacket
            }
            case PM4ItOpcode::SetBase: {
                const auto* set_base = reinterpret_cast<const PM4CmdSetBase*>(header);
                ASSERT(set_base->base_index == PM4CmdSetBase::BaseIndex::DrawIndexIndirPatchTable);
                indirect_args_addr = set_base->Address<u64>();
                break;
            }
            case PM4ItOpcode::EventWrite: {
                const auto* event = reinterpret_cast<const PM4CmdEventWrite*>(header);
                LOG_DEBUG(Render, "Encountered EventWrite: event_type = {}, event_index = {}",
                          magic_enum::enum_name(event->event_type.Value()),
                          magic_enum::enum_name(event->event_index.Value()));
                if (event->event_type.Value() == EventType::SoVgtStreamoutFlush) {
                    // registers: ApplyGraphicsRegisterPacket
                } else if (event->event_index.Value() == EventIndex::ZpassDone) {
                    if (event->event_type.Value() == EventType::PixelPipeStatDump) {
                        static constexpr u64 OcclusionCounterValidMask = 0x8000000000000000ULL;
                        static constexpr u64 OcclusionCounterStep = 0x2FFFFFFULL;
                        u64* results = event->Address<u64*>();
                        for (s32 i = 0; i < num_counter_pairs; ++i, results += 2) {
                            *results = pixel_counter | OcclusionCounterValidMask;
                            if (rasterizer) {
                                rasterizer->NoteLateCommandWrite(reinterpret_cast<VAddr>(results),
                                                                 results, sizeof(*results));
                            }
                        }
                        pixel_counter += OcclusionCounterStep;
                    }
                }
                break;
            }
            case PM4ItOpcode::EventWriteEos: {
                const auto* event_eos = reinterpret_cast<const PM4CmdEventWriteEos*>(header);
                if (BbFreeCheck::Enabled()) {
                    BbFreeCheck::NoteFenceDecoded(reinterpret_cast<u64>(event_eos->Address()),
                                                  event_eos->DataDWord(), header, dcb.data(), seq);
                }
                if (rasterizer &&
                    rasterizer->RunInOrder(&RunEventWriteEos, event_eos, sizeof(*event_eos),
                                           BbToggle::PipelinedTasks, false)) {
                    NotePendingFences(*event_eos);
                }
                break;
            }
            case PM4ItOpcode::EventWriteEop: {
                const auto* event_eop = reinterpret_cast<const PM4CmdEventWriteEop*>(header);
                if (BbFreeCheck::Enabled()) {
                    BbFreeCheck::NoteFenceDecoded(
                        reinterpret_cast<u64>(event_eop->Address<u8>()),
                        event_eop->data_sel.Value() == DataSelect::Data32Low ? event_eop->DataDWord()
                                                                             : event_eop->DataQWord(),
                        header, dcb.data(), seq);
                }
                if (rasterizer) {
                    if (rasterizer->RunInOrder(&RunEventWriteEop, event_eop, sizeof(*event_eop),
                                               BbToggle::PipelinedTasks, false)) {
                        NotePendingFences(*event_eop);
                    }
                } else {
                    SignalEop(*event_eop, 0);
                }
                break;
            }
            case PM4ItOpcode::DmaData: {
                const auto* dma_data = reinterpret_cast<const PM4DmaData*>(header);
                if (dma_data->dst_addr_lo == 0x3022C || !rasterizer) {
                    break;
                }
                // bbport: what the copy reads on the recording thread is known here: a later
                // DumpConstRam waits only if it overlaps it (unlisted, every dump waited for the
                // copy; ~150 of those waits a second).
                const bool reads_memory = dma_data->src_sel == DmaDataSrc::Memory ||
                                          dma_data->src_sel == DmaDataSrc::MemoryUsingL2;
                if (BbCeStats::Enabled() && (dma_data->dst_sel == DmaDataDst::Memory ||
                                             dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
                    BbCeStats::NoteWrite(BbCeStats::Dma, dma_data->DstAddress<VAddr>(),
                                         dma_data->NumBytes());
                }
                if (reads_memory) {
                    rasterizer->NotePendingRead(dma_data->SrcAddress<VAddr>(),
                                                dma_data->NumBytes());
                }
                if (rasterizer->RunInOrder(&RunDmaData, dma_data, sizeof(PM4DmaData),
                                           BbToggle::PipelinedMemoryWrites, false) &&
                    (dma_data->dst_sel == DmaDataDst::Memory ||
                     dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
                    rasterizer->NotePendingGpuWrite(dma_data->DstAddress<VAddr>(),
                                                    dma_data->NumBytes());
                }
                break;
            }
            case PM4ItOpcode::WriteData: {
                const auto* write_data = reinterpret_cast<const PM4CmdWriteData*>(header);
                ASSERT(write_data->dst_sel.Value() == 2 || write_data->dst_sel.Value() == 5);
                ASSERT(!write_data->wr_one_addr.Value());
                if (rasterizer) {
                    // In order with the draws (on the draw recording thread when in use).
                    if (BbCeStats::Enabled()) {
                        BbCeStats::NoteWrite(BbCeStats::WriteData, write_data->Address<VAddr>(),
                                             (count - 2) * sizeof(u32));
                    }
                    BbWriteLog::NoteIntent(write_data->Address<u64>(), write_data->data,
                                           (count - 2) * sizeof(u32),
                                           BbWriteLog::WriteDataIntent);
                    if (rasterizer->RunInOrder(&RunWriteData, header, (count + 1) * sizeof(u32),
                                               BbToggle::PipelinedMemoryWrites, false)) {
                        NotePendingWrite(*write_data, (count - 2) * sizeof(u32));
                        rasterizer->NotePendingGpuWrite(write_data->Address<VAddr>(),
                                                        (count - 2) * sizeof(u32));
                    }
                } else {
                    BbFreeCheck::Check(write_data->Address<u64>(), (count - 2) * 4, write_data->data,
                                       BbFreeCheck::WriteData);
                    std::memcpy(write_data->Address<u64*>(), write_data->data, (count - 2) * 4);
                }
                break;
            }
            case PM4ItOpcode::CopyData: {
                const auto* copy_data = reinterpret_cast<const PM4CmdCopyData*>(header);
                LOG_WARNING(Render,
                            "unhandled IT_COPY_DATA src_sel = {}, dst_sel = {}, "
                            "count_sel = {}, wr_confirm = {}, engine_sel = {}",
                            u32(copy_data->src_sel.Value()), u32(copy_data->dst_sel.Value()),
                            copy_data->count_sel.Value(), copy_data->wr_confirm.Value(),
                            u32(copy_data->engine_sel.Value()));
                break;
            }
            case PM4ItOpcode::MemSemaphore: {
                const auto* mem_semaphore = reinterpret_cast<const PM4CmdMemSemaphore*>(header);
                if (mem_semaphore->IsSignaling()) {
                    mem_semaphore->Signal();
                } else {
                    while (!mem_semaphore->Signaled()) {
                        YIELD_GFX();
                    }
                    mem_semaphore->Decrement();
                }
                break;
            }
            case PM4ItOpcode::AcquireMem: {
                // const auto* acquire_mem = reinterpret_cast<PM4CmdAcquireMem*>(header);
                break;
            }
            case PM4ItOpcode::Rewind: {
                if (!rasterizer) {
                    break;
                }
                const PM4CmdRewind* rewind = reinterpret_cast<const PM4CmdRewind*>(header);
                while (!rewind->Valid()) {
                    YIELD_GFX();
                }
                break;
            }
            case PM4ItOpcode::WaitRegMem: {
                const auto* wait_reg_mem = reinterpret_cast<const PM4CmdWaitRegMem*>(header);
                // ASSERT(wait_reg_mem->engine.Value() == PM4CmdWaitRegMem::Engine::Me);
                // Optimization: VO label waits are special because the emulator
                // will write to the label when presentation is finished. So if
                // there are no other submits to yield to we can sleep the thread
                // instead and allow other tasks to run.
                const u64* wait_addr = wait_reg_mem->Address<u64*>();
                if (vo_port->IsVoLabel(wait_addr) &&
                    num_submits == mapped_queues[GfxQueueId].submits.size()) {
                    if (rasterizer) {
                        rasterizer->DrainDrawPipe(static_cast<u32>(opcode));
                    }
                    vo_port->WaitVoLabel([&] { return wait_reg_mem->Test(regs.reg_array); });
                    break;
                }
                // bbport: a fence handed to the draw recording thread counts as written: that
                // thread runs everything in stream order.
                if (u32 value; wait_reg_mem->mem_space.Value() ==
                                   PM4CmdWaitRegMem::MemSpace::Memory &&
                               !BbToggle::Disabled(BbToggle::PendingFenceWaits) &&
                               PendingFenceValue(wait_reg_mem->Address<VAddr>(), value) &&
                               wait_reg_mem->TestValue(value)) {
                    break;
                }
                // Else the value may come from a fence that thread has yet to write: let it
                // catch up before yielding to other queues.
                while (!wait_reg_mem->Test(regs.reg_array)) {
                    if (rasterizer && !rasterizer->DrawPipeIdle()) {
                        rasterizer->DrainDrawPipe(static_cast<u32>(opcode));
                        continue;
                    }
                    // BB_HONEST_LABELS: a fence from another queue may wait for work not
                    // submitted yet; this thread would spin on it forever.
                    if (rasterizer && rasterizer->HasUnsubmittedSignals()) {
                        rasterizer->Flush();
                    }
                    YIELD_GFX();
                }
                break;
            }
            case PM4ItOpcode::IndirectBuffer: {
                const auto* indirect_buffer = reinterpret_cast<const PM4CmdIndirectBuffer*>(header);
                auto task = ProcessGraphics(
                    {indirect_buffer->Address<const u32>(), indirect_buffer->ib_size}, {});
                // Tasks start suspended: the nested body (and its scan) runs on this resume.
                g_dcb_depth = dcb_depth + 1;
                RESUME_GFX(task);
                g_dcb_depth = dcb_depth;

                while (!task.handle.done()) {
                    YIELD_GFX();
                    RESUME_GFX(task);
                }
                break;
            }
            case PM4ItOpcode::IncrementDeCounter: {
                ++cblock.de_count;
                break;
            }
            case PM4ItOpcode::WaitOnCeCounter: {
                while (cblock.ce_count <= cblock.de_count && !ce_task.handle.done()) {
                    RESUME_GFX(ce_task);
                }
                break;
            }
            case PM4ItOpcode::PfpSyncMe: {
                break;
            }
            case PM4ItOpcode::StrmoutBufferUpdate: {
                const auto* strmout = reinterpret_cast<const PM4CmdStrmoutBufferUpdate*>(header);
                LOG_WARNING(Render_Vulkan,
                            "Unimplemented IT_STRMOUT_BUFFER_UPDATE, update_memory = {}, "
                            "source_select = {}, buffer_select = {}",
                            strmout->update_memory.Value(),
                            magic_enum::enum_name(strmout->source_select.Value()),
                            strmout->buffer_select.Value());
                break;
            }
            case PM4ItOpcode::GetLodStats: {
                LOG_WARNING(Render_Vulkan, "Unimplemented IT_GET_LOD_STATS");
                break;
            }
            case PM4ItOpcode::CondExec: {
                const auto* cond_exec = reinterpret_cast<const PM4CmdCondExec*>(header);
                if (cond_exec->command.Value() != 0) {
                    LOG_WARNING(Render, "IT_COND_EXEC used a reserved command");
                }
                const auto skip = *cond_exec->Address() == false;
                if (skip) {
                    dcb = NextPacket(dcb,
                                     header->type3.NumWords() + 1 + cond_exec->exec_count.Value());
                    continue;
                }
                break;
            }
            default:
                UNREACHABLE_MSG("Unknown PM4 type 3 opcode {:#x} with count {}",
                                static_cast<u32>(opcode), count);
            }
            dcb = NextPacket(dcb, header->type3.NumWords() + 1);
            break;
        }
    }

    if (ce_task.handle) {
        while (!ce_task.handle.done()) {
            RESUME_GFX(ce_task);
        }
        ce_task.handle.destroy();
    }

    if (rasterizer && seq != NoSeq) {
        rasterizer->RetireSubmission(); // prepared draws live until the recording thread is past
    }
    if (draw_prep) {
        draw_prep->EndSubmission();
    }
    if (rasterizer && seq != NoSeq) {
        rasterizer->SubmitForSignals();
    }
    if (seq != NoSeq && BbStats::enabled) {
        BbStats::submissions.fetch_add(1, std::memory_order_relaxed);
#ifndef _WIN32 // bbport: per-thread rusage statistics are Linux-only
        if (rusage usage{}; getrusage(RUSAGE_THREAD, &usage) == 0) {
            BbStats::gpu_user_us.store(u64(usage.ru_utime.tv_sec) * 1000000 + usage.ru_utime.tv_usec,
                                       std::memory_order_relaxed);
            BbStats::gpu_sys_us.store(u64(usage.ru_stime.tv_sec) * 1000000 + usage.ru_stime.tv_usec,
                                      std::memory_order_relaxed);
            BbStats::gpu_invol_switches.store(usage.ru_nivcsw, std::memory_order_relaxed);
            BbStats::gpu_vol_switches.store(usage.ru_nvcsw, std::memory_order_relaxed);
            BbStats::gpu_minor_faults.store(usage.ru_minflt, std::memory_order_relaxed);
        }
#endif
    }

    if (copy && BbFreeCheck::Enabled()) {
        CheckSubmittedCopy(*copy, seq); // diagnostic
    }

    FIBER_EXIT;
}

template <bool is_indirect>
Liverpool::Task Liverpool::ProcessCompute(std::span<const u32> acb, u32 vqid) {
    FIBER_ENTER(acb_task_name[vqid]);
    auto& queue = asc_queues[{vqid}];

    struct IndirectPatch {
        const PM4Header* header;
        VAddr indirect_addr;
    };
    boost::container::small_vector<IndirectPatch, 4> indirect_patches;

    auto base_addr = reinterpret_cast<VAddr>(acb.data());
    size_t acb_size = acb.size_bytes();
    while (!acb.empty()) {
        ProcessCommands();
        if (rasterizer) {
            rasterizer->DrainDrawPipe(Vulkan::DrawPipe::ReasonCompute); // bbport: compute work uses the caches
        }

        auto* header = reinterpret_cast<const PM4Header*>(acb.data());
        u32 next_dw_off = header->type3.NumWords() + 1;

        // If we have a buffered packet, use it.
        if (queue.tmp_dwords > 0) [[unlikely]] {
            header = reinterpret_cast<const PM4Header*>(queue.tmp_packet.data());
            next_dw_off = header->type3.NumWords() + 1 - queue.tmp_dwords;
            std::memcpy(queue.tmp_packet.data() + queue.tmp_dwords, acb.data(),
                        next_dw_off * sizeof(u32));
            queue.tmp_dwords = 0;
        }

        // If the packet is split across ring boundary, buffer until next submission
        if (next_dw_off > acb.size()) [[unlikely]] {
            std::memcpy(queue.tmp_packet.data(), acb.data(), acb.size_bytes());
            queue.tmp_dwords = acb.size();
            if constexpr (!is_indirect) {
                *queue.read_addr += acb.size();
                *queue.read_addr %= queue.ring_size_dw;
            }
            break;
        }

        if (header->type == 2) {
            // Type-2 packet are used for padding purposes
            next_dw_off = 1;
            acb = NextPacket(acb, next_dw_off);
            if constexpr (!is_indirect) {
                *queue.read_addr += next_dw_off;
                *queue.read_addr %= queue.ring_size_dw;
            }
            continue;
        }

        if (header->type != 3) {
            // No other types of packets were spotted so far
            UNREACHABLE_MSG("Invalid PM4 type {}", header->type.Value());
        }

        const PM4ItOpcode opcode = header->type3.opcode;

        const auto* it_body = reinterpret_cast<const u32*>(header) + 1;
        switch (opcode) {
        case PM4ItOpcode::Nop: {
            const auto* nop = reinterpret_cast<const PM4CmdNop*>(header);
            break;
        }
        case PM4ItOpcode::IndirectBuffer: {
            const auto* indirect_buffer = reinterpret_cast<const PM4CmdIndirectBuffer*>(header);
            auto task = ProcessCompute<true>(
                {indirect_buffer->Address<const u32>(), indirect_buffer->ib_size}, vqid);
            RESUME_ASC(task, vqid);

            while (!task.handle.done()) {
                YIELD_ASC(vqid);
                RESUME_ASC(task, vqid);
            }
            break;
        }
        case PM4ItOpcode::DmaData: {
            const auto* dma_data = reinterpret_cast<const PM4DmaData*>(header);
            if (dma_data->dst_addr_lo == 0x3022C || !rasterizer) {
                break;
            }
            ASSERT(dma_data->command.das == 0);
            if (dma_data->src_sel == DmaDataSrc::Data && dma_data->dst_sel == DmaDataDst::Gds) {
                rasterizer->FillBuffer(dma_data->dst_addr_lo, dma_data->NumBytes(), dma_data->data,
                                       true);
            } else if ((dma_data->src_sel == DmaDataSrc::Memory ||
                        dma_data->src_sel == DmaDataSrc::MemoryUsingL2) &&
                       dma_data->dst_sel == DmaDataDst::Gds) {
                rasterizer->CopyBuffer(dma_data->dst_addr_lo, dma_data->SrcAddress<VAddr>(),
                                       dma_data->NumBytes(), true, false);
            } else if (dma_data->src_sel == DmaDataSrc::Data &&
                       (dma_data->dst_sel == DmaDataDst::Memory ||
                        dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
                rasterizer->FillBuffer(dma_data->DstAddress<VAddr>(), dma_data->NumBytes(),
                                       dma_data->data, false);
            } else if (dma_data->src_sel == DmaDataSrc::Gds &&
                       (dma_data->dst_sel == DmaDataDst::Memory ||
                        dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
                rasterizer->CopyBuffer(dma_data->DstAddress<VAddr>(), dma_data->src_addr_lo,
                                       dma_data->NumBytes(), false, true);
            } else if ((dma_data->src_sel == DmaDataSrc::Memory ||
                        dma_data->src_sel == DmaDataSrc::MemoryUsingL2) &&
                       (dma_data->dst_sel == DmaDataDst::Memory ||
                        dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
                const u32 num_bytes = dma_data->NumBytes();
                const VAddr src_addr = dma_data->SrcAddress<VAddr>();
                const VAddr dst_addr = dma_data->DstAddress<VAddr>();
                const PM4Header* header =
                    reinterpret_cast<const PM4Header*>(dst_addr - sizeof(PM4Header));
                if (dst_addr >= base_addr && dst_addr < base_addr + acb_size &&
                    num_bytes == sizeof(PM4CmdDispatchIndirect::GroupDimensions) &&
                    header->type == 3 && header->type3.opcode == PM4ItOpcode::DispatchDirect) {
                    indirect_patches.emplace_back(header, src_addr);
                } else {
                    rasterizer->CopyBuffer(dst_addr, src_addr, num_bytes, false, false);
                }
            } else {
                UNREACHABLE_MSG("WriteData src_sel = {}, dst_sel = {}", u32(dma_data->src_sel),
                                u32(dma_data->dst_sel));
            }
            break;
        }
        case PM4ItOpcode::AcquireMem: {
            break;
        }
        case PM4ItOpcode::Rewind: {
            if (!rasterizer) {
                break;
            }
            const PM4CmdRewind* rewind = reinterpret_cast<const PM4CmdRewind*>(header);
            while (!rewind->Valid()) {
                YIELD_ASC(vqid);
            }
            break;
        }
        case PM4ItOpcode::SetShReg: {
            const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
            const auto set_size = (header->type3.NumWords() - 1) * sizeof(u32);

            if (set_data->reg_offset >= 0x200 &&
                set_data->reg_offset <= (0x200 + sizeof(ComputeProgram) / 4)) {
                ASSERT(set_size <= sizeof(ComputeProgram));
                auto* addr = reinterpret_cast<u32*>(&mapped_queues[vqid + 1].cs_state) +
                             (set_data->reg_offset - 0x200);
                std::memcpy(addr, header + 2, set_size);
            } else {
                std::memcpy(&regs.reg_array[Regs::ShRegWordOffset + set_data->reg_offset],
                            header + 2, set_size);
                // bbport: interleaves with the graphics stream at an unpredictable point, so
                // draw-preparation workers cannot reproduce it: make their checksums differ.
                gfx_reg_checksum = HashRegisterPacket(gfx_reg_checksum ^ 0xA5C0A5C0A5C0ull,
                                                      reinterpret_cast<const u32*>(header),
                                                      header->type3.NumWords() + 1);
            }
            break;
        }
        case PM4ItOpcode::SetQueueReg: {
            const auto* set_data = reinterpret_cast<const PM4CmdSetQueueReg*>(header);
            LOG_WARNING(Render, "Encountered compute SetQueueReg: vqid = {}, reg_offset = {:#x}",
                        set_data->vqid.Value(), set_data->reg_offset.Value());
            break;
        }
        case PM4ItOpcode::DispatchDirect: {
            const auto* dispatch_direct = reinterpret_cast<const PM4CmdDispatchDirect*>(header);
            if (auto it = std::ranges::find(indirect_patches, header, &IndirectPatch::header);
                it != indirect_patches.end()) {
                const auto size = sizeof(PM4CmdDispatchIndirect::GroupDimensions);
                rasterizer->DispatchIndirect(it->indirect_addr, 0, size);
                break;
            }
            auto& cs_program = GetCsRegs();
            cs_program.dim_x = dispatch_direct->dim_x;
            cs_program.dim_y = dispatch_direct->dim_y;
            cs_program.dim_z = dispatch_direct->dim_z;
            cs_program.dispatch_initiator = dispatch_direct->dispatch_initiator;
            if (DebugState.DumpingCurrentReg()) {
                DebugState.PushRegsDumpCompute(base_addr, reinterpret_cast<uintptr_t>(header),
                                               cs_program);
            }
            if (!rasterizer || (cs_program.dispatch_initiator & 1) == 0) {
                break;
            }
            const auto cmd_address = reinterpret_cast<const void*>(header);
            rasterizer->ScopeMarker("asc[{}]:{}:DispatchDirect",
                                    fmt::make_format_args(vqid, cmd_address),
                                    [&] { rasterizer->DispatchDirect(); });
            break;
        }
        case PM4ItOpcode::DispatchIndirect: {
            const auto* dispatch_indirect =
                reinterpret_cast<const PM4CmdDispatchIndirectMec*>(header);
            auto& cs_program = GetCsRegs();
            const auto ib_address = dispatch_indirect->Address<VAddr>();
            const auto size = sizeof(PM4CmdDispatchIndirect::GroupDimensions);
            if (DebugState.DumpingCurrentReg()) {
                DebugState.PushRegsDumpCompute(base_addr, reinterpret_cast<uintptr_t>(header),
                                               cs_program);
            }
            if (!rasterizer || (cs_program.dispatch_initiator & 1) == 0) {
                break;
            }
            const auto cmd_address = reinterpret_cast<const void*>(header);
            rasterizer->ScopeMarker("asc[{}]:{}:DispatchIndirect",
                                    fmt::make_format_args(vqid, cmd_address),
                                    [&] { rasterizer->DispatchIndirect(ib_address, 0, size); });
            break;
        }
        case PM4ItOpcode::WriteData: {
            // bbport: copies deferred to the recording thread precede writes the guest sees, and
            // so do graphics fences deferred to it (the pipe was drained before this packet).
            if (rasterizer) {
                rasterizer->WaitHostCopies();
                rasterizer->WaitDeferredSignals();
            }
            const auto* write_data = reinterpret_cast<const PM4CmdWriteData*>(header);
            ASSERT(write_data->dst_sel.Value() == 2 || write_data->dst_sel.Value() == 5);
            const u32 data_size = (header->type3.count.Value() - 2) * 4;
            if (!write_data->wr_one_addr.Value()) {
                BbFreeCheck::Check(write_data->Address<u64>(), data_size, write_data->data,
                                   BbFreeCheck::ComputeWriteData);
                std::memcpy(write_data->Address<void*>(), write_data->data, data_size);
                if (rasterizer) {
                    rasterizer->NoteCommandWrite(write_data->Address<VAddr>(), write_data->data,
                                                 data_size, true);
                }
            } else {
                UNREACHABLE();
            }
            break;
        }
        case PM4ItOpcode::MemSemaphore: {
            const auto* mem_semaphore = reinterpret_cast<const PM4CmdMemSemaphore*>(header);
            if (mem_semaphore->IsSignaling()) {
                mem_semaphore->Signal();
            } else {
                while (!mem_semaphore->Signaled()) {
                    YIELD_ASC(vqid);
                }
                mem_semaphore->Decrement();
            }
            break;
        }
        case PM4ItOpcode::WaitRegMem: {
            const auto* wait_reg_mem = reinterpret_cast<const PM4CmdWaitRegMem*>(header);
            ASSERT(wait_reg_mem->engine.Value() == PM4CmdWaitRegMem::Engine::Me);
            while (!wait_reg_mem->Test(regs.reg_array)) {
                // BB_HONEST_LABELS: the value may come from a fence waiting for work not
                // submitted yet; this thread would spin on it forever.
                if (rasterizer && rasterizer->HasUnsubmittedSignals()) {
                    rasterizer->Flush();
                }
                YIELD_ASC(vqid);
            }
            break;
        }
        case PM4ItOpcode::ReleaseMem: {
            // BB_HONEST_LABELS: written once the GPU has finished the work before it (not a GDS store,
            // which needs the GPU copy recorded here).
            if (const auto* rm = reinterpret_cast<const PM4CmdReleaseMem*>(header);
                rasterizer && Vulkan::Rasterizer::HonestLabels() &&
                rm->data_sel.Value() != DataSelect::GdsMemStore) {
                rasterizer->ProcessDownloadImages();
                const PM4CmdReleaseMem release = *rm;
                const u32 pipe_id = queue.pipe_id;
                const bool writes = release.data_sel.Value() != DataSelect::None;
                rasterizer->SignalAfterGpu(
                    [release, pipe_id] {
                        release.SignalFence(
                            [pipe_id] {
                                Platform::IrqC::Instance()->Signal(
                                    static_cast<Platform::InterruptId>(pipe_id));
                            },
                            [](VAddr, u16, u16) {});
                    },
                    writes ? release.Address<VAddr>() : 0, release.DataQWord());
                break;
            }
            // bbport: copies deferred to the recording thread precede writes the guest sees, and
            // so do graphics fences deferred to it (the pipe was drained before this packet).
            if (rasterizer) {
                rasterizer->WaitHostCopies();
                rasterizer->WaitDeferredSignals();
            }
            const auto* release_mem = reinterpret_cast<const PM4CmdReleaseMem*>(header);
            if (const auto sel = release_mem->data_sel.Value();
                sel != DataSelect::None && sel != DataSelect::GdsMemStore) {
                const u64 value = release_mem->DataQWord();
                BbFreeCheck::Check(release_mem->Address<u64>(),
                                   sel == DataSelect::Data32Low ? 4 : 8, &value,
                                   BbFreeCheck::ReleaseMem);
            }
            if (rasterizer) {
                rasterizer->ProcessDownloadImages();
            }
            release_mem->SignalFence(
                [pipe_id = queue.pipe_id] {
                    Platform::IrqC::Instance()->Signal(static_cast<Platform::InterruptId>(pipe_id));
                },
                [this](VAddr dst, u16 gds_index, u16 num_dwords) {
                    rasterizer->CopyBuffer(dst, gds_index, num_dwords * sizeof(u32), false, true);
                });
            break;
        }
        case PM4ItOpcode::EventWrite: {
            // const auto* event = reinterpret_cast<const PM4CmdEventWrite*>(header);
            break;
        }
        default:
            UNREACHABLE_MSG("Unknown PM4 type 3 opcode {:#x} with count {}",
                            static_cast<u32>(opcode), header->type3.NumWords());
        }

        acb = NextPacket(acb, next_dw_off);

        if constexpr (!is_indirect) {
            *queue.read_addr += next_dw_off;
            *queue.read_addr %= queue.ring_size_dw;
        }
    }

    FIBER_EXIT;
}

Liverpool::CmdBuffer Liverpool::CopyCmdBuffers(std::span<const u32> dcb, std::span<const u32> ccb) {
    auto& queue = mapped_queues[GfxQueueId];
    ASSERT_MSG(queue.dcb_buffer.capacity() >= queue.dcb_buffer_offset + dcb.size(),
               "dcb copy buffer out of reserved space");
    ASSERT_MSG(queue.ccb_buffer.capacity() >= queue.ccb_buffer_offset + ccb.size(),
               "ccb copy buffer out of reserved space");

    queue.dcb_buffer.resize(
        std::max(queue.dcb_buffer.size(), queue.dcb_buffer_offset + dcb.size()));
    queue.ccb_buffer.resize(
        std::max(queue.ccb_buffer.size(), queue.ccb_buffer_offset + ccb.size()));

    const u32 prev_dcb_buffer_offset = queue.dcb_buffer_offset;
    const u32 prev_ccb_buffer_offset = queue.ccb_buffer_offset;
    if (!dcb.empty()) {
        std::memcpy(queue.dcb_buffer.data() + queue.dcb_buffer_offset, dcb.data(),
                    dcb.size_bytes());
        queue.dcb_buffer_offset += dcb.size();
        dcb = std::span<const u32>{queue.dcb_buffer.begin() + prev_dcb_buffer_offset,
                                   queue.dcb_buffer.begin() + queue.dcb_buffer_offset};
    }

    if (!ccb.empty()) {
        std::memcpy(queue.ccb_buffer.data() + queue.ccb_buffer_offset, ccb.data(),
                    ccb.size_bytes());
        queue.ccb_buffer_offset += ccb.size();
        ccb = std::span<const u32>{queue.ccb_buffer.begin() + prev_ccb_buffer_offset,
                                   queue.ccb_buffer.begin() + queue.ccb_buffer_offset};
    }

    return std::make_pair(dcb, ccb);
}

void Liverpool::SubmitGfx(std::span<const u32> dcb, std::span<const u32> ccb) {
    auto& queue = mapped_queues[GfxQueueId];

    // bbport: one copy per submission, alive until it is decoded (CopyCmdBuffers' shared buffer
    // is reset at sceGnmSubmitDone, while this thread may still be decoding earlier frames).
    std::shared_ptr<SubmittedCopy> copy;
    if (EmulatorSettings.IsCopyGpuBuffers()) {
        copy = std::make_shared<SubmittedCopy>();
        copy->data.resize(dcb.size() + ccb.size());
        std::memcpy(copy->data.data(), dcb.data(), dcb.size_bytes());
        std::memcpy(copy->data.data() + dcb.size(), ccb.data(), ccb.size_bytes());
        copy->guest_dcb = dcb.data();
        copy->dcb_dwords = dcb.size();
        copy->guest_ccb = ccb.data();
        copy->ccb_dwords = ccb.size();
        copy->submitted = std::chrono::steady_clock::now();
        dcb = std::span<const u32>{copy->data.data(), copy->dcb_dwords};
        ccb = std::span<const u32>{copy->data.data() + copy->dcb_dwords, copy->ccb_dwords};
    }

    // The copy for the draw preparation workers is made before taking the queue lock, which
    // the GPU thread needs to pick up work.
    auto* draw_prep = rasterizer ? &rasterizer->GetDrawPreparation() : nullptr;
    auto prep_submission = draw_prep ? draw_prep->Build(dcb) : nullptr;
    {
        // Numbering, enqueueing and queueing under one lock keep the three orders identical.
        std::scoped_lock lock{queue.m_access};
        u64 seq = NoSeq;
        if (draw_prep) {
            seq = gfx_submit_seq++;
            draw_prep->Enqueue(seq, std::move(prep_submission));
        }
        BbFreeCheck::NoteSubmit(seq, copy ? copy->guest_dcb : dcb.data(), dcb.size_bytes());
        auto task = ProcessGraphics(dcb, ccb, seq, std::move(copy));
        queue.submits.emplace(task.handle);
    }

    std::scoped_lock lk{submit_mutex};
    ++num_submits;
    ++submissions_total;
    ++gfx_submitted;
    BbTimeline::Note(BbTimeline::GuestSubmit, 0, submissions_total);
    work_retired = false;
    submit_cv.notify_one();
}

void Liverpool::SignalDecodedFrames() {
    u64 frame = 0;
    {
        std::scoped_lock lk{submit_mutex};
        while (!frame_ends.empty() && frame_ends.front().first <= decoded_total) {
            frame = frame_ends.front().second;
            frame_ends.pop_front();
        }
    }
    if (frame == 0 || !rasterizer) {
        if (frame != 0) {
            Libraries::GnmDriver::NoteFramesRetired(frame);
        }
        return;
    }
    // Everything of the frame recorded (the draw pipe drained), then submitted: finished once the
    // GPU has executed it, whatever is decoded after it.
    if (rasterizer->PipeSignals()) {
        rasterizer->RunInOrder(
            [](Vulkan::Rasterizer& self, const u8* data) {
                u64 retired;
                std::memcpy(&retired, data, sizeof(retired));
                BbTimeline::Note(BbTimeline::PipeTask, 7, self.CurrentTick());
                self.SignalAfterGpu(
                    [retired] { Libraries::GnmDriver::NoteFramesRetired(retired); });
                self.Flush();
            },
            &frame, sizeof(frame), BbToggle::PipelinedTasks, false);
        return;
    }
    rasterizer->DrainDrawPipe(Vulkan::DrawPipe::ReasonSubmissionEnd);
    rasterizer->SignalAfterGpu([frame] { Libraries::GnmDriver::NoteFramesRetired(frame); });
    rasterizer->Flush();
}

void Liverpool::SignalGpuIdle(u64 generation) {
    bool idle;
    {
        std::scoped_lock lk{submit_mutex};
        idle = num_submits == 0 && submissions_total == generation;
        if (idle) {
            work_retired = true;
        }
    }
    if (idle) {
        Platform::IrqC::Instance()->Signal(Platform::InterruptId::GpuIdle);
    }
}

void Liverpool::SubmitAsc(u32 gnm_vqid, std::span<const u32> acb) {
    ASSERT_MSG(gnm_vqid > 0 && gnm_vqid < NumTotalQueues, "Invalid virtual ASC queue index");
    BbStats::asc_submits.fetch_add(1, std::memory_order_relaxed);
    auto& queue = mapped_queues[gnm_vqid];

    const auto vqid = gnm_vqid - 1;
    const auto& task = ProcessCompute(acb, vqid);
    {
        std::scoped_lock lock{queue.m_access};
        queue.submits.emplace(task.handle);
    }

    std::scoped_lock lk{submit_mutex};
    num_mapped_queues = std::max(num_mapped_queues, gnm_vqid + 1);
    ++num_submits;
    ++submissions_total;
    work_retired = false;
    submit_cv.notify_one();
}


void Liverpool::CheckSubmittedCopy(const SubmittedCopy& copy, u64 seq) {
    // The guest memory may be unmapped by now: read it without faulting.
    static std::atomic<u64> changed{0}, checked{0};
    const auto first_change = [](const u32* guest, const u32* kept, std::size_t dwords) -> s64 {
        std::array<u32, 1024> buf;
        for (std::size_t at = 0; at < dwords; at += buf.size()) {
            const std::size_t n = std::min(buf.size(), dwords - at);
            iovec local{buf.data(), n * 4}, remote{const_cast<u32*>(guest + at), n * 4};
            if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) != ssize_t(n * 4)) {
                // bbport BB_GUEST_IN_PLACE: dma-buf guest memory is not readable that way; still
                // mapped, it is read directly.
                int prot = 0, type = -1;
                uintptr_t end = 0;
                const auto from = reinterpret_cast<uintptr_t>(guest + at);
                if (!runtime_memory_vma_info(from, &prot, &type, &end) || end < from + n * 4) {
                    return -2 - s64(at); // unmapped
                }
                std::memcpy(buf.data(), guest + at, n * 4);
            }
            for (std::size_t i = 0; i < n; ++i) {
                if (buf[i] != kept[at + i]) {
                    return s64(at + i);
                }
            }
        }
        return -1;
    };
    checked.fetch_add(1, std::memory_order_relaxed);
    const s64 dcb_change = first_change(copy.guest_dcb, copy.data.data(), copy.dcb_dwords);
    const s64 ccb_change =
        first_change(copy.guest_ccb, copy.data.data() + copy.dcb_dwords, copy.ccb_dwords);
    if (dcb_change == -1 && ccb_change == -1) {
        return;
    }
    const u64 n = changed.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n > 16 && n % 100 != 0) {
        return;
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                                copy.submitted)
                          .count();
    const s64 at = dcb_change != -1 ? dcb_change : ccb_change;
    std::printf("Command buffers: the guest changed submission %llu's %s at %p +%#llx (%s) "
                "%.3f ms after submitting it, before it was decoded; %llu of %llu so far "
                "(decoded from the copy)\n",
                (unsigned long long)seq, dcb_change != -1 ? "dcb" : "ccb",
                static_cast<const void*>(dcb_change != -1 ? copy.guest_dcb : copy.guest_ccb),
                (unsigned long long)((at < -1 ? -2 - at : at) * 4),
                at < -1 ? "unmapped" : "rewritten", ms, (unsigned long long)n,
                (unsigned long long)checked.load());
    if (n > 4 || at < 0) {
        return;
    }
    // What changed, and the packets before it (a WaitRegMem the guest releases after patching,
    // or a write of ours into its own command buffer).
    const u32* kept = dcb_change != -1 ? copy.data.data() : copy.data.data() + copy.dcb_dwords;
    const u32* guest = dcb_change != -1 ? copy.guest_dcb : copy.guest_ccb;
    const std::size_t dwords = dcb_change != -1 ? copy.dcb_dwords : copy.ccb_dwords;
    const std::size_t from = at >= 8 ? at - 8 : 0, to = std::min<std::size_t>(dwords, at + 8);
    std::array<u32, 16> now{};
    iovec local{now.data(), (to - from) * 4}, remote{const_cast<u32*>(guest + from), (to - from) * 4};
    const bool readable = process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == ssize_t((to - from) * 4);
    std::printf("  submitted:");
    for (std::size_t i = from; i < to; ++i) {
        std::printf(i == std::size_t(at) ? " [%08x]" : " %08x", kept[i]);
    }
    std::printf("\n  now:      ");
    for (std::size_t i = from; readable && i < to; ++i) {
        std::printf(i == std::size_t(at) ? " [%08x]" : " %08x", now[i - from]);
    }
    std::printf("\n  packets before it (dword offset:opcode):");
    std::array<std::pair<u32, u32>, 12> last{};
    std::size_t count = 0;
    for (std::size_t p = 0; p < std::size_t(at) && p < dwords;) {
        const u32 header = kept[p];
        if ((header >> 30) == 3) {
            last[count++ % last.size()] = {u32(p), (header >> 8) & 0xff};
            p += ((header >> 16) & 0x3fff) + 2;
        } else if ((header >> 30) == 2) {
            ++p;
        } else {
            break;
        }
    }
    for (std::size_t i = count > last.size() ? count - last.size() : 0; i < count; ++i) {
        std::printf(" %#x:%#x", last[i % last.size()].first, last[i % last.size()].second);
    }
    std::printf("\n");
}

} // namespace AmdGpu

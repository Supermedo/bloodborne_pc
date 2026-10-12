// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: BB_FREE_CHECK=1 — a diagnostic for the guest heap corruption at guest offset
// 0x263b8e7. The game keeps its GPU label objects (48 bytes) in a small-object pool: 4 KiB
// pages, blocks from the page start, a header at page + 0xfc0 (list links, free list head,
// free count), the free list linked through the first qword of each free block. The crash is
// a free list whose head became 0x0000005300000000: something wrote into a freed block.
//
// - Check(): every write the GPU emulation makes to guest memory (fences, WriteData, DMA,
//   buffer and image downloads) is tested, before it lands, against the free list of the
//   pool page it targets. A write into a free block or into the page header is reported.
// - A watcher thread walks the free lists of the pool pages those writes touched every
//   millisecond and reports a corrupted list (a next pointer outside the page) as soon as it
//   appears, with the recent emulator writes into that page. A corruption with no emulator
//   write around it was written by the game itself.
#pragma once

#include <cstdint>

namespace BbFreeCheck {
enum Source : std::uint32_t {
    Eop,
    Eos,
    EosGds,
    WriteData,
    ComputeWriteData,
    ReleaseMem,
    ConstRam,
    DmaFill,
    DmaCopy,
    BufferDownload,
    ImageDownload,
};
bool Enabled();
/// Before a write of `size` bytes at `address`; `data` (may be null) is what will be written.
/// `seq`: the fence's place in the graphics command stream (NextFenceSeq), 0 for none: a
/// fence written after one that follows it in the stream is reported.
void Check(std::uint64_t address, std::uint64_t size, const void* data, Source source,
           std::uint64_t seq = 0);
/// Numbers the graphics fences in command stream order (0 when the check is off).
std::uint64_t NextFenceSeq();
/// Where the GPU command thread decoded a fence: its label and value, the packet and its command
/// buffer in guest memory, and the submission (Liverpool::SubmitGfx order).
void NoteFenceDecoded(std::uint64_t label, std::uint64_t value, const void* packet,
                      const void* buffer, std::uint64_t submit);
/// BB_LABEL_TRAP: a decoded fence's write starts, and has landed (`write_ns` it took; slow
/// ones are reported). Until then its label is pending.
void NoteFenceWriting(std::uint64_t label);
void NoteFenceWritten(std::uint64_t label, std::uint64_t write_ns = 0);
/// BB_LABEL_TRAP=1 (with BB_FREE_CHECK=1): a pool page holding labels whose fences are decoded
/// but not yet written is read-only; a guest write into such a label block is reported with
/// the guest code and call chain that made it (who frees a label before its fence). True when
/// the fault was the trap's (the page is writable again).
bool OnTrapFault(void* ucontext, std::uint64_t address);
/// After OnTrapFault and GPU page tracking declined a fault: true when it raced with a trapped
/// page's release (the write is retried).
bool OnStaleTrapFault(std::uint64_t address);
/// A graphics submission by the guest (sceGnmSubmitCommandBuffers).
void NoteSubmit(std::uint64_t submit, const void* buffer, std::uint64_t size);
/// At a guest fault: the free check's view of the page the guest was in (`rax`, `r14`).
void DumpAtFault(std::uint64_t rax, std::uint64_t r14);
} // namespace BbFreeCheck

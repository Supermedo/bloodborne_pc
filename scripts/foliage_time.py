#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Plants sway with the frame's time, not a step a frame (game version 01.09; BB_FOLIAGE_TIME=0 turns it off).

The swaying plants (the white flowers of the Hunter's Dream, Hemwick's lavender) are moved by
clocks the game keeps for its wind effect (0x2313870 fills the effect's shader constants: 4
entries of 0xC0 bytes, a clock at +0x610 of each, added to once a frame at 0x231581A/0x2315AA5,
and fade timers at +0x560 counted down at 0x2316043). Their step is not the frame's time: it is
1/60 when the game runs its 60 FPS mode ([[0x55406C8]+0x100] == 1, which the FPS patches set) and
1/30 otherwise. So with Uncap FPS++ the plants swayed as fast as the frame rate (twice as fast at
120 FPS as on PS4), and frames of uneven length (30 to 50 FPS) made them jolt: each frame moved
them as far, 20 ms or 35 ms. Measured on a still camera in the Hunter's Dream (optical flow of the
flowers): 22 px/s at 120 FPS, 11.5 at 60, 8.3 at 30.

The step is now the cloth's steady time (cloth_time.py: the frame's time, at most 5% further from
one frame to the next, never more than 1/30 s), read from its shared slot when it is of this frame
or the last (the plants may be updated before the first cloth of a frame). Without it (no cloth in
the frame, or Steady cloth time off) the frame's own time, at most 1/30 s; without a frame timer the
game's step. Two sites, where the step is stored ([rbp-0x170], 0x2313DBF) or kept in xmm4 (the
path at 0x2315D79), jump to small detours that call one routine; it changes xmm0 only.

The code lies in the cloth cave's free middle: between cloth.py's code (it ends before
cloth.CODE_END) and its constants at the page's end. Our own code; the facts above are measured
(BB_HEAPSCAN watchpoints) and read from the game's code.
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import cloth  # noqa: E402
import cloth_time  # noqa: E402
import quit_menu  # noqa: E402

STORE = 0x2313DBF             # vmovaps [rbp-0x170], xmm0: the step for the loop
STORE_BYTES = bytes.fromhex('c5f8298590feffff')
KEPT = 0x2315D79              # vmovss xmm13, [0x4929E6C]: the first instruction once xmm4 holds the step
KEPT_BYTES = bytes.fromhex('c57a102deb406102')
KEPT_CONSTANT = 0x4929E6C
CAVE = cloth.CODE_END
CAVE_END = quit_menu.CAVE_END - 4 * len(cloth.CONSTANTS)   # cloth.py's constants follow


def rip(code, opcode, target):
    """An instruction ending in a rip-relative disp32 to `target`."""
    end = code.here + len(opcode) + 4
    code.data += opcode + struct.pack('<i', target - end)


def code_writes():
    cave = quit_menu.Code(CAVE)
    longest = CAVE_END - 4                     # its own 1/30 (cloth_time.py's code may be off)
    labels, jumps = {}, []

    def jump(opcode, name):    # rel8, fixed up at the end
        cave.raw(opcode + '00')
        jumps.append((len(cave.data) - 1, name))

    # The routine: xmm0 in, the game's step; out, the plants' step. Only xmm0 changes.
    routine = cave.here
    cave.raw('505152')                         # push rax; push rcx; push rdx
    rip(cave, bytes.fromhex('488b05'), cloth_time.FRAME_TIMER)   # mov rax, [frame timer]
    cave.raw('4885c0')                         # test rax, rax
    jump('74', 'done')                         # no timer yet: the game's step
    cave.raw('8b88' + struct.pack('<I', cloth_time.FRAME_COUNT).hex())   # mov ecx, [rax+0x260]  (frame index)
    rip(cave, bytes.fromhex('488b15'), cloth_time.STEADY)              # mov rdx, [steady]  (index | time << 32)
    cave.raw('29d1')                           # sub ecx, edx
    cave.raw('83e11f')                         # and ecx, 31            (the index counts round 32 frames)
    cave.raw('83f901')                         # cmp ecx, 1
    jump('77', 'raw')                          # older than the last frame: not this frame's
    cave.raw('48c1ea20')                       # shr rdx, 32
    jump('74', 'raw')                          # no time in it yet
    cave.raw('c5f96ec2')                       # vmovd xmm0, edx         the steady time
    jump('eb', 'done')
    labels['raw'] = cave.here
    cave.raw('8b90' + struct.pack('<I', cloth_time.FRAME_TIME).hex())    # mov edx, [rax+0x264]  (frame time)
    cave.raw('85d2')                           # test edx, edx
    jump('7e', 'done')                         # none (or not above 0): the game's step
    cave.raw('c5f96ec2')                       # vmovd xmm0, edx
    rip(cave, bytes.fromhex('c5fa5d05'), longest)                      # vminss xmm0, xmm0, [1/30]
    labels['done'] = cave.here
    cave.raw('5a5958c3')                       # pop rdx; pop rcx; pop rax; ret
    for at, name in jumps:
        cave.data[at] = (labels[name] - (cave.offset + at + 1)) & 0xFF
    # The stored step.
    store = cave.here
    cave.call(routine)
    cave.raw(STORE_BYTES.hex())                # vmovaps [rbp-0x170], xmm0
    cave.jmp(STORE + len(STORE_BYTES))
    # The step in xmm4 (xmm0 is set anew right after the site).
    kept = cave.here
    cave.raw('c5f828c4')                       # vmovaps xmm0, xmm4
    cave.call(routine)
    cave.raw('c5f828e0')                       # vmovaps xmm4, xmm0
    rip(cave, bytes.fromhex('c57a102d'), KEPT_CONSTANT)                # vmovss xmm13, [0x4929E6C]
    cave.jmp(KEPT + len(KEPT_BYTES))
    if cave.here > longest:
        raise ValueError('plant sway time code does not fit')
    cave.data += b'\xcc' * (longest - cave.here) + struct.pack('<f', cloth_time.LONGEST)
    writes = [(cave.offset, bytes(cave.data))]
    for at, original, target in ((STORE, STORE_BYTES, store), (KEPT, KEPT_BYTES, kept)):
        site = quit_menu.Code(at)
        site.jmp(target)
        site.data += b'\x90' * (len(original) - len(site.data))
        writes.append((site.offset, bytes(site.data)))
    return writes


def check_original(image_bytes):
    for at, original in ((STORE, STORE_BYTES), (KEPT, KEPT_BYTES)):
        if image_bytes(at, at + len(original)) != original:
            raise ValueError(f'Plant sway time: unexpected code at {at + quit_menu.EBOOT_BASE:#x}')

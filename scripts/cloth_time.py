#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""A steady time step for the cloth at any frame rate (game version 01.09; BB_CLOTH_TIME=0 turns it off).

The game's cloth (Havok) gets the frame's time (0x76E370, its update, saves it at [rbp-0x78]) and
splits it into a fixed number of steps (2 for the player's cape). With Uncap FPS++ that time is the
real one, and frames are not all alike: standing still at 120 FPS (8.3 ms a frame), every 20 s or
so a stutter takes 15 to 35 ms and the frames after it catch up at 4.2 ms. The cloth took those as
steps 4 times as long, then half as long, as the ones before: capes and dresses spazzed out even
standing still. Measured (the fastest particle of each cloth, 7 minutes standing still in the
Hunter's Dream): 16 of 20 stutters kicked a cloth (11 the player's), 3 of 511 calm moments did.
At 30 FPS the cloth stays calm.

So the cloth's time follows the frame's at most 5% further from one frame to the next (x 0.95 to
x 1.05), and is never more than 1/30 s (the PS4's frame). A stutter is then a few frames of cloth a
little slower or faster, not a kick; a lasting change of frame rate (120 to 60 FPS) is followed in
about 15 frames. Measured as above: 5 of 33 stutters kicked a cloth (1 the player's), and with
Cloth as on PS4 (cloth.py, its time correction on this time) none of 30. The time
of the frame is worked out once per frame (the frame timer's ring index, +0x260, changes) by the
first cloth to see it; [STEADY] holds that frame's index (low half) and its time (high half), set
with one lock cmpxchg so every cloth of a frame gets the same time. A cloth given a scaled time
(the game's time scale) keeps its scale: its time is multiplied by steady time / frame time.
Our own code; the frame timer is the game's (0x55404F8: +0x260 frames, +0x264 the last frame's
time in seconds). cloth.py (Cloth as on PS4) reads the same steady time.
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import quit_menu  # noqa: E402

SITE = 0x76E384               # vmovss [rbp-0x78], xmm0: the cloth update saves the frame's time
SITE_BYTES = bytes.fromhex('c5fa114588')
RETURN = SITE + len(SITE_BYTES)
FRAME_TIMER = 0x55404F8       # the frame timer (a pointer): +0x260 frame count, +0x264 frame time, s
FRAME_COUNT, FRAME_TIME = 0x260, 0x264
# Two gaps in the code segment's last page: after Quit to Desktop's code (it ends at 0x50D9826, crash
# guards start at 0x50D98C0) and after Rebirth's (it ends at 0x50D9A6D, PC Settings at 0x50D9B00).
CAVE, CAVE_END = 0x50D9830, 0x50D9894   # effect_time.py has the rest, up to 0x50D98C0
MORE, MORE_END = 0x50D9A70, 0x50D9B00
STEADY = 0x56D3F98            # the data segment's unused tail (cloth.py's state ends at +0x18): 8 bytes
STEADY_TIME = STEADY + 4
SLOWER, FASTER, LONGEST = 0.95, 1.05, 1 / 30
CONSTANTS = (0.0, SLOWER, FASTER, LONGEST)


def rip(code, opcode, target):
    """An instruction ending in a rip-relative disp32 to `target`."""
    end = code.here + len(opcode) + 4
    code.data += opcode + struct.pack('<i', target - end)


def code_writes():
    cave, more = quit_menu.Code(CAVE), quit_menu.Code(MORE)
    constants = MORE_END - 4 * len(CONSTANTS)
    zero, slower, faster, longest = (constants + 4 * i for i in range(len(CONSTANTS)))
    labels, jumps = {}, []

    def jump(code, opcode, name):   # rel32 jumps, fixed up at the end (labels may be in either cave)
        code.data += bytes.fromhex('e9' if opcode == 'eb' else '0f' + format(int(opcode, 16) + 0x10, '02x'))
        jumps.append((code, len(code.data), name))
        code.data += bytes(4)

    def label(code, name):
        labels[name] = code.here

    cave.raw('c5fa114588')                     # vmovss [rbp-0x78], xmm0  (the site's: the time as given)
    cave.raw('5152')                           # push rcx; push rdx
    rip(cave, bytes.fromhex('488b05'), FRAME_TIMER)   # mov rax, [frame timer]
    cave.raw('4885c0')                         # test rax, rax
    jump(cave, '74', 'done')                   # no timer yet: as given
    cave.raw('c5fa1088' + struct.pack('<I', FRAME_TIME).hex())   # vmovss xmm1, [rax+0x264]  (frame time)
    rip(cave, bytes.fromhex('c5f82f0d'), zero)        # vcomiss xmm1, [0]
    jump(cave, '76', 'done')                   # no frame time: as given
    cave.raw('8b88' + struct.pack('<I', FRAME_COUNT).hex())      # mov ecx, [rax+0x260]  (frame index)
    label(cave, 'again')
    rip(cave, bytes.fromhex('488b05'), STEADY)        # mov rax, [steady]  (index | time << 32)
    cave.raw('39c8')                           # cmp eax, ecx
    jump(cave, '74', 'have')                   # this frame's time is set
    cave.raw('4889c2')                         # mov rdx, rax
    cave.raw('48c1ea20')                       # shr rdx, 32
    cave.raw('c5f96ed2')                       # vmovd xmm2, edx         the time before
    rip(cave, bytes.fromhex('c5f82f15'), zero)        # vcomiss xmm2, [0]
    jump(cave, '77', 'limit')
    cave.raw('c5f828d1')                       # vmovaps xmm2, xmm1      the first: the frame's time
    label(cave, 'limit')
    jump(cave, 'eb', 'more')
    # The rest after Rebirth's code.
    label(more, 'more')
    rip(more, bytes.fromhex('c5ea591d'), slower)      # vmulss xmm3, xmm2, [0.8]
    more.raw('c5e25fd9')                       # vmaxss xmm3, xmm3, xmm1
    rip(more, bytes.fromhex('c5ea5915'), faster)      # vmulss xmm2, xmm2, [1.25]
    more.raw('c5e25dda')                       # vminss xmm3, xmm3, xmm2   the frame's time, held
    rip(more, bytes.fromhex('c5e25d1d'), longest)     # vminss xmm3, xmm3, [1/30]
    more.raw('c5f97eda')                       # vmovd edx, xmm3
    more.raw('48c1e220')                       # shl rdx, 32
    more.raw('4809ca')                         # or rdx, rcx              this frame's index | time
    rip(more, bytes.fromhex('f0480fb115'), STEADY)    # lock cmpxchg [steady], rdx
    jump(more, '75', 'again')                  # another cloth set it first: take that one
    more.raw('4889d0')                         # mov rax, rdx
    label(more, 'have')
    more.raw('48c1e820')                       # shr rax, 32
    more.raw('c5f96ed0')                       # vmovd xmm2, eax          the steady time
    more.raw('c5ea5ed1')                       # vdivss xmm2, xmm2, xmm1  / the frame's
    more.raw('c5fa59c2')                       # vmulss xmm0, xmm0, xmm2
    more.raw('c5fa114588')                     # vmovss [rbp-0x78], xmm0
    label(more, 'done')
    more.raw('5a59')                           # pop rdx; pop rcx
    more.jmp(RETURN)
    for code, at, name in jumps:
        code.data[at:at + 4] = struct.pack('<i', labels[name] - (code.offset + at + 4))
    if cave.here > CAVE_END or more.here > constants:
        raise ValueError('steady cloth time code does not fit')
    more.data += b'\xcc' * (constants - more.here) + struct.pack(f'<{len(CONSTANTS)}f', *CONSTANTS)
    cave.data += b'\xcc' * (CAVE_END - cave.here)
    site = quit_menu.Code(SITE)
    site.jmp(CAVE)
    return [(cave.offset, bytes(cave.data)), (more.offset, bytes(more.data)), (site.offset, bytes(site.data))]


def check_original(image_bytes):
    if image_bytes(SITE, SITE + len(SITE_BYTES)) != SITE_BYTES:
        raise ValueError(f'Steady cloth time: unexpected code at {SITE + quit_menu.EBOOT_BASE:#x}')

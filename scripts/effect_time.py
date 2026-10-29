#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Effects step in whole 1/60 s at any frame rate (game version 01.09; with Uncap FPS++ and 90 FPS++;
BB_EFFECT_TIME=0 turns it off).

Once a frame the game updates its effects (0x1F77BD0 calls the effect system's update, vtable +0x40 of
[[0x5540370]+0x70], 0x22FF340) with a time step: 1/30 s on the PS4 (the constant at 0x4929170), 0 when
[+0x8c] asks for a frame without time. 30/60/90 FPS++ change that constant; Uncap FPS++ passes the
frame's measured time instead ([0x55404F8]+0x264) and drops the 0 frames. Below 1/60 s a step, the
effects' particles stop expiring: they pile up until the frame rate falls and the scene's resource
tables run short (Ebrietas: the "ground rumbling" particles around her; players capped at 60 FPS to
get through the fight). Measured with the boss bot (stage/bot/fight.py, 2.5 min of the fight), draws
per frame at the peak: 60 FPS++ 805; Uncap FPS++ 4948 uncapped and 5949 at 60 FPS (the measured time
jitters around 1/60); a constant 1/144 a frame 9374, a constant 1/60 at 144 FPS 735; 90 FPS++ (1/90, 90 times a
second) 10787, at 30 FPS.

So the measured time goes into an accumulator here, and the update gets its whole 1/60 s steps
(0, 1/60, 2/60, ...; the rest stays for the next frame): the effects run at the real time's pace, in
steps they handle. With 90 FPS++ the time is its fixed step (1/90, the constant above), as its game logic
steps. The 0 frames the game asks for get 0 again. Our own code, at Uncap FPS++'s own
sites in 0x1F77BD0 (the assert block it already uses, and its jump at 0x1F77D48), so the frame rate
patches' fingerprint is unchanged; the rest in cloth_time.py's first cave, past its code.
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import quit_menu  # noqa: E402

SITE = 0x1F77CEA              # jne past the assert block (Uncap FPS++: jmp); the block 0x1F77CEC..0x1F77D10
SITE_END = 0x1F77D10
BRANCH = 0x1F77D48            # jne 0x1F77D52 (the 0-frame flag); Uncap FPS++: jmp to the block
CALL = 0x1F77D52              # call rax: the update, with the step in xmm0
AFTER = 0x1F77D54             # past the call: the flag cleared
SITE_BYTES = bytes.fromhex('7524488d3d76369c02488d15c3369c02488d0de0349c02beb100000031c0e8a3d81300498b07')
BRANCH_BYTES = bytes.fromhex('7508c5fa10051e149b02')   # and the PS4 constant's load up to the call
FIXED_STEP = 0x4929170        # the step the PS4 code passes: 1/30 s, 1/90 with 90 FPS++
FRAME_TIMER = 0x55404F8       # the frame timer (a pointer): +0x264 the last frame's time, s
FRAME_TIME = 0x264
CAVE = 0x50D9894              # cloth_time.py's first cave, past its code (cloth_time.CODE_END)
CAVE_END = 0x50D98C0
ACCUMULATED = 0x56D3FA0       # the data segment's unused tail (cloth_time.py's STEADY ends here): 4 bytes
STEP = 1 / 60


def rip(code, opcode, target):
    end = code.here + len(opcode) + 4
    code.data += opcode + struct.pack('<i', target - end)


def code_writes(measured=True):
    """measured: the frame's measured time (Uncap FPS++); else the preset's fixed step, the constant
    the PS4 code loads (90 FPS++: 1/90, its game logic steps that way too)."""
    step = CAVE_END - 4
    # At the site: the flag's frame keeps its 0 (xmm0 is cleared before the branch), else
    # xmm1 = accumulated + the frame's time.
    site = quit_menu.Code(SITE)
    site.raw('eb24')                                      # jmp 0x1F77D10 (past the block, as Uncap)
    site.raw('75' + struct.pack('<b', CALL - (site.here + 2)).hex())  # jne CALL: the 0 frame
    if measured:
        rip(site, bytes.fromhex('488b0d'), FRAME_TIMER)   # mov rcx, [frame timer]
        site.raw('c5fa1089' + struct.pack('<i', FRAME_TIME).hex())  # vmovss xmm1, [rcx+0x264]
    else:
        rip(site, bytes.fromhex('c5fa100d'), FIXED_STEP)  # vmovss xmm1, [the preset's step]
    rip(site, bytes.fromhex('c5f2580d'), ACCUMULATED)     # vaddss xmm1, xmm1, [accumulated]
    site.jmp(CAVE)
    if site.here > SITE_END:
        raise ValueError('effect time site code does not fit')
    site.data += b'\x90' * (SITE_END - site.here)
    # In the cave: xmm0 = floor(xmm1 / step) x step goes to the update, the rest is kept.
    cave = quit_menu.Code(CAVE)
    rip(cave, bytes.fromhex('c5f25e05'), step)            # vdivss xmm0, xmm1, [step]
    cave.raw('c4e3790ac009')                              # vroundss xmm0, xmm0, xmm0, 9 (down)
    rip(cave, bytes.fromhex('c5fa5905'), step)            # vmulss xmm0, xmm0, [step]
    cave.raw('c5f25cc8')                                  # vsubss xmm1, xmm1, xmm0
    rip(cave, bytes.fromhex('c5fa110d'), ACCUMULATED)     # vmovss [accumulated], xmm1
    cave.jmp(BRANCH + 2)
    if cave.here > step:
        raise ValueError('effect time code does not fit')
    cave.data += b'\xcc' * (step - cave.here) + struct.pack('<f', STEP)
    # Back before the call (the PS4 constant's load there is not used any more): no whole step yet, no
    # update this frame.
    branch = quit_menu.Code(BRANCH)
    branch.raw('eb' + struct.pack('<b', SITE + 2 - (BRANCH + 2)).hex())  # jmp to the site's jne
    branch.raw('c5f97ec1')                                # vmovd ecx, xmm0
    branch.raw('85c9')                                    # test ecx, ecx
    branch.raw('74' + struct.pack('<b', AFTER - (branch.here + 2)).hex())  # jz past the call
    if branch.here != CALL:
        raise ValueError('effect time branch does not fit')
    return [(site.offset, bytes(site.data)), (cave.offset, bytes(cave.data)), (branch.offset, bytes(branch.data))]


def check_original(image_bytes):
    if image_bytes(SITE, SITE_END) != SITE_BYTES or image_bytes(BRANCH, CALL) != BRANCH_BYTES:
        raise ValueError(f'Effect time: unexpected code at {SITE + quit_menu.EBOOT_BASE:#x}')

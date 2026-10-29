#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Jumps as at 30 FPS at any frame rate (game version 01.09; BB_AIR_DRAG=0 turns it off).

In the air, the game slows a character's horizontal speed by a fixed factor every step: x 0.95 (the
constant at 0x490B5B0, used by the vmulps at 0x1A4ABCA), tuned for its 30 steps a second. The FPS
presets replace that constant with one value each, right for one frame rate only: Uncap FPS++ the
value for 60 FPS, 60/90 FPS++ the one for 90. Uncapped, a jump off the ledge behind Central
Yharnam's first lamp kept 48% of its horizontal speed after a second at 30 FPS, 20% at 60 and 4%
at 120 (21.5% on the PS4), and landed 5.9, 4.5 and 3.1 m from where it left the ground: at 120
FPS some jumps could not be made at all.

The multiply now uses 0.95^(30 x the step's time): the drag per second of 30 FPS at any frame
rate. The step's time is the function's own ([rbp-0x988], stored from the update at 0x1A4A851 and
checked above zero there); 30 x it is held to 0..4 (a long stall drags as 4 steps of 30 FPS).
Our own code; the idea of scaling the drag by the frame time is also in Lantern Engine (Awaken).
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import quit_menu  # noqa: E402

SITE = 0x1A4ABCA              # vmulps xmm0, xmm0, [0x490B5B0]  (0.95 in four lanes)
DRAG_CONSTANT = 0x490B5B0
SITE_BYTES = bytes.fromhex('c5f85905') + struct.pack('<i', DRAG_CONSTANT - (SITE + 8))
STEP = -0x988                 # [rbp-0x988]: the step's time, s
CAVE = 0x50D9C60              # past PC Settings' page table (it ends at 0x50D9C48), up to the cloth cave
CAVE_END = 0x50D9D00
CONSTANTS = (0.95, 30.0, 0.0, 4.0)


def rip(code, opcode, target):
    end = code.here + len(opcode) + 4
    code.data += opcode + struct.pack('<i', target - end)


def code_writes():
    cave = quit_menu.Code(CAVE)
    constants = CAVE_END - 4 * len(CONSTANTS)
    drag, thirty, zero, most = (constants + 4 * i for i in range(len(CONSTANTS)))
    cave.raw('4883ec10')                                  # sub rsp, 16
    cave.raw('c5fa1095' + struct.pack('<i', STEP).hex())  # vmovss xmm2, [rbp-0x988]  (step, s)
    rip(cave, bytes.fromhex('c5ea5915'), thirty)          # vmulss xmm2, xmm2, [30]
    rip(cave, bytes.fromhex('c5ea5f15'), zero)            # vmaxss xmm2, xmm2, [0]
    rip(cave, bytes.fromhex('c5ea5d15'), most)            # vminss xmm2, xmm2, [4]
    cave.raw('c5fa111424')                                # vmovss [rsp], xmm2        (k)
    rip(cave, bytes.fromhex('c5fa1015'), drag)            # vmovss xmm2, [0.95]
    cave.raw('c5fa11542404')                              # vmovss [rsp+4], xmm2      (D)
    cave.raw('d90424')                                    # fld dword [rsp]           st0 = k
    cave.raw('d9442404')                                  # fld dword [rsp+4]         st0 = D, st1 = k
    cave.raw('d9f1')                                      # fyl2x                     st0 = y = k log2 D
    cave.raw('d9c0')                                      # fld st0
    cave.raw('d9fc')                                      # frndint                   st0 = int(y)
    cave.raw('d9c9')                                      # fxch st1                  st0 = y, st1 = int(y)
    cave.raw('d8e1')                                      # fsub st0, st1             st0 = frac(y)
    cave.raw('d9f0')                                      # f2xm1
    cave.raw('d9e8')                                      # fld1
    cave.raw('dec1')                                      # faddp st1, st0            st0 = 2^frac
    cave.raw('d9fd')                                      # fscale                    st0 = D^k
    cave.raw('ddd9')                                      # fstp st1
    cave.raw('d95c2404')                                  # fstp dword [rsp+4]        (x87 empty again)
    cave.raw('c4e27918542404')                            # vbroadcastss xmm2, [rsp+4]
    cave.raw('c5f859c2')                                  # vmulps xmm0, xmm0, xmm2   (the site's)
    cave.raw('4883c410')                                  # add rsp, 16
    cave.jmp(SITE + len(SITE_BYTES))                      # xmm2 is set again right there
    if cave.here > constants:
        raise ValueError('air drag code does not fit')
    cave.data += b'\xcc' * (constants - cave.here) + struct.pack(f'<{len(CONSTANTS)}f', *CONSTANTS)
    site = quit_menu.Code(SITE)
    site.jmp(CAVE)
    site.data += b'\x90' * (len(SITE_BYTES) - len(site.data))
    return [(cave.offset, bytes(cave.data)), (site.offset, bytes(site.data))]


def check_original(image_bytes):
    if image_bytes(SITE, SITE + len(SITE_BYTES)) != SITE_BYTES:
        raise ValueError(f'Air drag: unexpected code at {SITE + quit_menu.EBOOT_BASE:#x}')

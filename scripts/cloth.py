#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Cloth as on the PS4 at any frame rate (bbport.ini steady_cloth, game version 01.09).

The game's cloth (Havok) was tuned at the PS4's 30 FPS; it takes a fixed number of steps a frame
(2 for the player's cape), so above 30 FPS it takes more steps a second, and at 60 and 120 FPS
capes float and flare out (measured in the Hunter's Dream: a cape's hem 342 px wide at 30 FPS,
357 at 60, 398 at 120). Each step moves each particle with Verlet integration (0x773DF0):

    new = position + (position - previous) * D + acceleration * dt^2

then pulls groups of 4 particles back toward the cloth's modelled shape by a fraction k of the way
(bend constraints, 0x7386E0, batched in 0x7384F0). Three things in that only hold at 30 FPS:

- Bends: gravity moves a particle g dt^2 a step and the bend takes back (1 - k) of the rest: the
  droop of a step is (1 - k) / k x g dt^2, 16x less at 120 FPS: the cloth keeps its modelled,
  flared shape. k' = k / (1 + (r - 1)(1 - k)) with r = (frame time at 30 FPS / frame time)^2 would
  keep 30 FPS's droop exactly, but in the beta long coats and skirts were "too relaxed" with it,
  going wild in motion, and a kilt stuck out (a cape folded up at 240 FPS). Half of it is used:
  r = frame time at 30 FPS / frame time, at most 4 (as at 120 FPS). A rigid k = 1 stays rigid.
- D is applied per step: D^min(1, 60 x frame time) keeps the damping per second of 60 FPS (with 30,
  less damping at 60: long garments were "jumpy").
- (position - previous) is the motion over the previous step; when frames differ in length
  (uncapped they differ 0.8 to 2x from one to the next) the cloth's speed jumps with them.
  Time-corrected Verlet: x frame time / previous frame time (0.5..2), on a frame's first step.

At 30 FPS and below all three are unchanged; damping is also unchanged at 60. Measured (hem
width, the widest row): 342 px at 30 FPS; stock 357 at 60 and 398 at 120; with it 344 at 60, and
at 120 the hem hangs below that row. The per-frame values (ratio, r - 1) are worked out once per
frame (the frame timer's ring index, +0x260, changes) by the first cloth job to see the new frame. The frame's
time is the cloth's steady one (cloth_time.py: it follows the frame's at most 5% a frame, so a stutter is
no kick), the time the cloth itself steps with; this patch is only applied with that one. Our own code; the
frame timer is the game's (0x55404F8: +0x260 frames).
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import cloth_time  # noqa: E402
import quit_menu  # noqa: E402

SITE = 0x773E77               # vbroadcastss xmm0, [r13+0x10c] in the Verlet step (0x773DF0)
SITE_BYTES = bytes.fromhex('c4c27918850c010000')
RETURN = SITE + len(SITE_BYTES)
FRAME_TIMER = 0x55404F8       # the frame timer (a pointer): +0x260 frame count, +0x264 frame time, s
FRAME_COUNT, FRAME_TIME = 0x260, 0x264
# In the code segment's last page, past the port's other code there: Quit to Desktop 0x50D9700,
# crash guards 0x50D98C0, Rebirth 0x50D9900, PC Settings 0x50D9B00 (pc_menu.py; its page table up to
# 0x50D9C48 is rewritten by the port when the game loads: a cave at 0x50D9C00 had its first
# instructions overwritten with addresses, and the first cloth step crashed). The constants end the page;
# between the code (up to CODE_END) and them lies foliage_time.py's code.
CAVE = 0x50D9D00
CODE_END = 0x50D9F50
STATE = 0x56D3F80             # the data segment's unused tail (past its size, in its last page):
LAST_FRAME, LAST_TIME, RATIO, SOFTEN = STATE, STATE + 4, STATE + 8, STATE + 12   # 0 until set
CALLERS = ((0x76EC70, -0x18), (0x76EE10, -0x20))   # return address, its step number's save slot
BEND = 0x7387F4               # vbroadcastss xmm0, [rcx-0xe]; vmulps xmm0, xmm8, xmm0: a bend
BEND_BYTES = bytes.fromhex('c4e2791841f2c5b859c0')  # constraint's stiffness x the operator's strength
BATCH = 0x738645              # vmovss xmm6, [rcx-0xe]; vmulss xmm6, xmm6, [r8]: the same in the
BATCH_BYTES = bytes.fromhex('c5fa1071f2c4c14a5930')  # batched solver (0x7384F0) bend operators use
REFERENCE = 30.0             # the PS4's frame rate, the cloth's tuning
DAMPING_REFERENCE = 60.0      # damping per second as at 60 FPS (as at 30, long coats and skirts swung on)
CONSTANTS = (0.0, 0.5, 1.0, 2.0, REFERENCE, 3.0, DAMPING_REFERENCE)


def rip(code, opcode, target):
    """An instruction ending in a rip-relative disp32 to `target`."""
    end = code.here + len(opcode) + 4
    code.data += opcode + struct.pack('<i', target - end)


def code_writes():
    cave = quit_menu.Code(CAVE)
    constants = quit_menu.CAVE_END - 4 * len(CONSTANTS)
    zero, half, one, two, reference, most, damping = (constants + 4 * i for i in range(len(CONSTANTS)))
    labels, jumps = {}, []

    def jump(opcode, name):   # the rel32 form of a short jump's opcode, fixed up at the end
        cave.data += bytes.fromhex('e9' if opcode == 'eb' else '0f' + format(int(opcode, 16) + 0x10, '02x'))
        jumps.append((len(cave.data), name))
        cave.data += bytes(4)

    def label(name):
        labels[name] = len(cave.data)

    cave.raw('9c505152')                       # pushfq; push rax; push rcx; push rdx
    cave.raw('4883ec30')                       # sub rsp, 48
    cave.raw('c5f8110c24')                     # vmovups [rsp], xmm1
    cave.raw('c5f811542410')                   # vmovups [rsp+16], xmm2
    cave.raw('c4c17a10850c010000')             # vmovss xmm0, [r13+0x10c]  (D)
    rip(cave, bytes.fromhex('488b05'), FRAME_TIMER)   # mov rax, [frame timer]
    cave.raw('4885c0')                         # test rax, rax
    jump('74', 'broadcast')                    # no timer yet: unchanged
    # Damping per second as at 60 FPS: D^k, k = min(1, 60 x frame time), for 0 < D < 1 (the frame's,
    # not this step's: a cloth with two steps a frame keeps its damping per frame). Unchanged at 60 FPS
    # and below: as at 30 FPS (less damping at 60), long coats and skirts were "jumpy" in the beta.
    rip(cave, bytes.fromhex('c5f82f05'), one)         # vcomiss xmm0, [1]
    jump('73', 'ratio')                        # D >= 1: nothing to scale
    rip(cave, bytes.fromhex('c5f82f05'), zero)        # vcomiss xmm0, [0]
    jump('76', 'ratio')                        # D <= 0
    rip(cave, bytes.fromhex('c5fa100d'), cloth_time.STEADY_TIME)   # vmovss xmm1, [steady time]
    rip(cave, bytes.fromhex('c5f2590d'), damping)     # vmulss xmm1, xmm1, [60]
    rip(cave, bytes.fromhex('c5f25d0d'), one)         # vminss xmm1, xmm1, [1]
    rip(cave, bytes.fromhex('c5f82f0d'), zero)        # vcomiss xmm1, [0]
    jump('76', 'ratio')                        # no frame time: unchanged
    cave.raw('c5fa114c2420')                   # vmovss [rsp+32], xmm1  (k)
    cave.raw('c5fa11442424')                   # vmovss [rsp+36], xmm0  (D)
    cave.raw('d9442420')                       # fld dword [rsp+32]        st0 = k
    cave.raw('d9442424')                       # fld dword [rsp+36]        st0 = D, st1 = k
    cave.raw('d9f1')                           # fyl2x                     st0 = y = k log2 D
    cave.raw('d9c0')                           # fld st0
    cave.raw('d9fc')                           # frndint                   st0 = int(y), st1 = y
    cave.raw('d9c9')                           # fxch st1                  st0 = y, st1 = int(y)
    cave.raw('d8e1')                           # fsub st0, st1             st0 = frac(y)
    cave.raw('d9f0')                           # f2xm1                     st0 = 2^frac - 1
    cave.raw('d9e8')                           # fld1
    cave.raw('dec1')                           # faddp st1, st0            st0 = 2^frac
    cave.raw('d9fd')                           # fscale                    st0 = 2^frac 2^int = D^k
    cave.raw('ddd9')                           # fstp st1
    cave.raw('d95c2424')                       # fstp dword [rsp+36]       (x87 stack empty again)
    cave.raw('c5fa10442424')                   # vmovss xmm0, [rsp+36]
    label('ratio')
    # Once per frame: RATIO = frame time / previous frame time (0.5..2; 1 for the first).
    cave.raw('8b88' + struct.pack('<I', FRAME_COUNT).hex())   # mov ecx, [rax+0x260]
    rip(cave, bytes.fromhex('3b0d'), LAST_FRAME)      # cmp ecx, [last frame]
    jump('74', 'apply')
    cave.raw('50')                             # push rax
    rip(cave, bytes.fromhex('8b05'), LAST_FRAME)      # mov eax, [last frame]
    rip(cave, bytes.fromhex('f00fb10d'), LAST_FRAME)  # lock cmpxchg [last frame], ecx
    cave.raw('58')                             # pop rax (flags kept)
    jump('75', 'apply')                        # another cloth job took this frame
    rip(cave, bytes.fromhex('c5fa100d'), cloth_time.STEADY_TIME)   # vmovss xmm1, [steady time]
    rip(cave, bytes.fromhex('c5fa1015'), LAST_TIME)   # vmovss xmm2, [last time]
    rip(cave, bytes.fromhex('c5fa110d'), LAST_TIME)   # vmovss [last time], xmm1
    rip(cave, bytes.fromhex('c5f82f15'), zero)        # vcomiss xmm2, [0]
    jump('76', 'first')
    cave.raw('c5f25eca')                       # vdivss xmm1, xmm1, xmm2
    rip(cave, bytes.fromhex('c5f25f0d'), half)        # vmaxss xmm1, xmm1, [0.5]
    rip(cave, bytes.fromhex('c5f25d0d'), two)         # vminss xmm1, xmm1, [2]
    jump('eb', 'store')
    label('first')
    rip(cave, bytes.fromhex('c5fa100d'), one)         # vmovss xmm1, [1]
    label('store')
    rip(cave, bytes.fromhex('c5fa110d'), RATIO)       # vmovss [ratio], xmm1
    # And SOFTEN = r - 1, r = frame time at 30 FPS / frame time: 0 at 30 FPS and below, at most 3 (as at
    # 120 FPS). Half the full correction (its square root): with the square, long coats and skirts were
    # "too relaxed" in the beta, going wild in motion, and a kilt stuck out; a cape folded up at 240.
    rip(cave, bytes.fromhex('c5fa100d'), cloth_time.STEADY_TIME)   # vmovss xmm1, [steady time]
    rip(cave, bytes.fromhex('c5f2590d'), reference)   # vmulss xmm1, xmm1, [30]
    rip(cave, bytes.fromhex('c5fa1015'), one)         # vmovss xmm2, [1]
    cave.raw('c5ea5ed1')                       # vdivss xmm2, xmm2, xmm1
    rip(cave, bytes.fromhex('c5ea5c15'), one)         # vsubss xmm2, xmm2, [1]
    rip(cave, bytes.fromhex('c5ea5f15'), zero)        # vmaxss xmm2, xmm2, [0]
    rip(cave, bytes.fromhex('c5ea5d15'), most)        # vminss xmm2, xmm2, [15]
    rip(cave, bytes.fromhex('c5fa1115'), SOFTEN)      # vmovss [soften], xmm2
    label('apply')
    # Only a frame's first step follows a step of another length: the step's callers keep their
    # step number in r13 (0x76EC6B) or r12 (0x76EE0B), saved by this function at [rbp-0x18] and
    # [rbp-0x20].
    for caller, saved in CALLERS:
        rip(cave, bytes.fromhex('488d0d'), caller)    # lea rcx, [return address]
        cave.raw('483b4d08')                   # cmp rcx, [rbp+8]
        jump('75', f'not {caller:x}')
        cave.raw('8b4d' + struct.pack('<b', saved).hex())   # mov ecx, [rbp+saved]  (step number)
        cave.raw('85c9')                       # test ecx, ecx
        jump('75', 'broadcast')                # a later step of the frame: as it was
        jump('eb', 'first step')
        label(f'not {caller:x}')
    label('first step')
    rip(cave, bytes.fromhex('c5fa100d'), RATIO)       # vmovss xmm1, [ratio]
    rip(cave, bytes.fromhex('c5f82f0d'), zero)        # vcomiss xmm1, [0]
    jump('76', 'broadcast')                    # not set yet
    cave.raw('c5fa59c1')                       # vmulss xmm0, xmm0, xmm1
    label('broadcast')
    cave.raw('c5f8c6c000')                     # vshufps xmm0, xmm0, xmm0, 0  (the site's broadcast)
    cave.raw('c5f8100c24')                     # vmovups xmm1, [rsp]
    cave.raw('c5f810542410')                   # vmovups xmm2, [rsp+16]
    cave.raw('4883c430')                       # add rsp, 48
    cave.raw('5a59589d')                       # pop rdx; pop rcx; pop rax; popfq
    cave.jmp(RETURN)
    # Bend constraints (0x7386E0): each step pulls its 4 particles a fraction k of the way back to
    # the cloth's modelled shape, while gravity moves them g dt^2. More steps a second, the same k:
    # the cloth keeps its modelled shape (capes flare out) instead of hanging as at 30 FPS. The
    # droop of a step, (1 - k) / k x g dt^2, stays as at 30 FPS with k' = k / (1 + (r - 1)(1 - k));
    # a rigid constraint (k = 1) stays rigid. No flags or registers but xmm9 and xmm10 (unused there).
    bend = cave.here
    cave.data += BEND_BYTES                    # k = stiffness x strength (the site's)
    rip(cave, bytes.fromhex('c46279180d'), one)       # vbroadcastss xmm9, [1]
    cave.raw('c5305cc8')                       # vsubps xmm9, xmm9, xmm0         1 - k
    cave.raw('c4412857d2')                     # vxorps xmm10, xmm10, xmm10
    cave.raw('c441305fca')                     # vmaxps xmm9, xmm9, xmm10        (not below 0)
    rip(cave, bytes.fromhex('c462791815'), SOFTEN)    # vbroadcastss xmm10, [soften]
    cave.raw('c4413059ca')                     # vmulps xmm9, xmm9, xmm10        (r - 1)(1 - k)
    rip(cave, bytes.fromhex('c462791815'), one)       # vbroadcastss xmm10, [1]
    cave.raw('c4413058ca')                     # vaddps xmm9, xmm9, xmm10
    cave.raw('c4c1785ec1')                     # vdivps xmm0, xmm0, xmm9         k'
    cave.jmp(BEND + len(BEND_BYTES))
    # The same for the batched bend solver (0x7384F0; scalar k in xmm6, xmm14 and xmm15 unused there).
    batch = cave.here
    cave.data += BATCH_BYTES                   # k = stiffness x strength (the site's)
    rip(cave, bytes.fromhex('c57a1035'), one)         # vmovss xmm14, [1]
    cave.raw('c50a5cf6')                       # vsubss xmm14, xmm14, xmm6       1 - k
    cave.raw('c4410057ff')                     # vxorps xmm15, xmm15, xmm15
    cave.raw('c4410a5ff7')                     # vmaxss xmm14, xmm14, xmm15      (not below 0)
    rip(cave, bytes.fromhex('c50a5935'), SOFTEN)      # vmulss xmm14, xmm14, [soften]
    rip(cave, bytes.fromhex('c50a5835'), one)         # vaddss xmm14, xmm14, [1]
    cave.raw('c4c14a5ef6')                     # vdivss xmm6, xmm6, xmm14        k'
    cave.jmp(BATCH + len(BATCH_BYTES))
    for at, name in jumps:
        cave.data[at:at + 4] = struct.pack('<i', labels[name] - (at + 4))
    if cave.here > CODE_END:
        raise ValueError('steady cloth code does not fit')
    writes = [(cave.offset, bytes(cave.data)), (constants, struct.pack(f'<{len(CONSTANTS)}f', *CONSTANTS))]
    for at, original, target in ((SITE, SITE_BYTES, CAVE), (BEND, BEND_BYTES, bend), (BATCH, BATCH_BYTES, batch)):
        site = quit_menu.Code(at)
        site.jmp(target)
        site.data += b'\x90' * (len(original) - len(site.data))
        writes.append((site.offset, bytes(site.data)))
    return writes


def check_original(image_bytes):
    for at, original in ((SITE, SITE_BYTES), (BEND, BEND_BYTES), (BATCH, BATCH_BYTES)):
        if image_bytes(at, at + len(original)) != original:
            raise ValueError(f'Steady cloth: unexpected code at {at + quit_menu.EBOOT_BASE:#x}')

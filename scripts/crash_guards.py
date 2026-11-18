#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Crash guards: the game's own code, where it reads through a pointer it never checks (game version 01.09).

- Online event without its data (0x1EB3D10, case 0). The game describes an event (r13: the event, its
  type the switch's value; r12: its data) and returns whether it could, the low byte of r12; an event
  type it does not know goes to 0x1EB3DD0: r12 = 0, "could not". Case 0 copies a string from r12 + 0x80
  without checking r12: a player online crashed there ("read access to 00000000000000a0
  at guest offset 0x1eb56b7") right after the server's ss.info request timed out. Case 0 with no data now
  goes the same way as an unknown event.

- Texture surface-wrapper getters (0x219F860, 0x219F870, 0x219F880). Sibling getters (0x219F850,
  0x219F8B0) already return 0 when [rdi+0x40] is NULL, while these three read [rax+0x4a], [rax+0x44]
  or [rax+0x46] unconditionally ("read access to 0000000000000044 at guest offset 0x219f874"). Each now
  returns 0 when [rdi+0x40] == NULL using the alignment padding between 0x219F860 and 0x219F8B0.

- Compositor sub-resource format query (0x21C2336). Queries [rcx+0x30] after loading rcx = [rcx]
  without checking rcx ("read access to 0000000000000030 at guest offset 0x21c2339"). Returning 0 on
  NULL preserves valid 64-bit pointers in rcx (unlike zeroing ecx unconditionally, which forced format 0
  on every call to sub_21C2100 and starved SSR mipmap allocations).

- CopyGnmSurfaceMip (sub_2168880, 0x2168880 / crash at 0x21688A2). Copies mips between two Texture
  wrappers; callers such as sub_21C2100 pass [tex+0x40] in rdx and on stack ([rsp+0x10] at entry)
  without checking for NULL, faulting at 0x21688A2 ([r14+0x3b]) or 0x21688D9 ([r15+0x3b]). All 14 direct
  callers treat it as void, so returning before prologue pushes safely skips NULL surface copies.

- SSR mipmap compositor loop abort (0x21C2461). With effect_ssr=1, if mip allocation sub_e31ed0 returns
  r14 == NULL inside sub_21C2100, continuing through 0x21C28D0 sets r15 = 0 for the next mip and faults
  at 0x21C24DC ([r15+0x48]), 0x21C268B ([r14]) or 0x21C2745 ([r14+0x48]). Restoring the retained source
  texture r15 = [rbp-0x118] and branching to the function's own loop-exit cleanup (0x21C2937) releases
  references cleanly and returns true.

Our own code lives in the 32-byte gap before Quit to Desktop's cave (0x50D96E0..0x50D9700, after
CODE_END = 0x50D96DC), the free tail of Quit to Desktop's cave (0x50D98C0..0x50D9900), and the gap
between PC Settings' runtime page table (0x50D9C48) and air_drag (0x50D9C60).
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import quit_menu  # noqa: E402

CAVE = quit_menu.CAVE + 0x1C0            # 0x50D98C0, up to Rebirth's cave at 0x50D9900
CAVE_END = quit_menu.CAVE + 0x200        # 0x50D9900
SSR_CAVE = CAVE + 0x2C                   # 0x50D98EC; after cave1 (25B) + cave2 (16B) + 3B slack
GNM_COPY_CAVE = 0x50D96E0                # 32B free gap after CODE_END (0x50D96DC) before quit_menu.CAVE
GNM_COPY_CAVE_END = quit_menu.CAVE       # 0x50D9700

EVENT_CASE = 0x1EB3DE8                   # sub r12, -0x80; lea rdi, [rbp-0xa0]
EVENT_CASE_BYTES = bytes.fromhex('4983ec80488dbd60ffffff')
EVENT_UNKNOWN = 0x1EB3DD0                # xor r12d, r12d; jmp (the function's end): "could not"

TEX_QUERY_SITE = 0x219F860               # three adjacent [rdi+0x40] getters up to 0x219F8B0
TEX_QUERY_ORIGINAL = bytes.fromhex(
    '488b47400fb6404ac30f1f8000000000'
    '488b47400fb740444088f1d3e8c36690'
    '488b47400fb740464088f1d3e8c36690'
    'b801000000c3662e0f1f840000000000'
    '8b4758c36666662e0f1f840000000000'
)
TEX_QUERY_WRITES = (
    (0x219F860, bytes.fromhex('488b47404885c074050fb6404ac3c390')),
    (0x219F870, bytes.fromhex('488b47404885c07402eb1bc30f1f4000')),
    (0x219F880, bytes.fromhex('488b47404885c07402eb19c30f1f4000')),
    (0x219F896, bytes.fromhex('0fb740444088f1d3e8c3')),
    (0x219F8A4, bytes.fromhex('0fb740464088f1d3e8c36690')),
)

VISCERAL_SITE = 0x21C2336                # mov rcx, [rcx]; mov ecx, [rcx + 0x30]
VISCERAL_SITE_BYTES = bytes.fromhex('488b098b4930')
VISCERAL_RESUME = 0x21C233C              # mov [rbp - 0x144], ecx

GNM_COPY_SITE = 0x2168880                # push rbp; mov rbp, rsp; push r15
GNM_COPY_BYTES = bytes.fromhex('554889e54157')
GNM_COPY_RESUME = GNM_COPY_SITE + len(GNM_COPY_BYTES)  # 0x2168886; push r14; push r13; push r12

SSR_SITE = 0x21C2461                     # cmp qword ptr [r14+0x40], 0
SSR_SITE_BYTES = bytes.fromhex('49837e4000')
SSR_RESUME = SSR_SITE + len(SSR_SITE_BYTES)
SSR_ORIGINAL = SSR_SITE_BYTES + bytes.fromhex('be000000007433')
SSR_LOOP_EXIT = 0x21C2937                # test r14, r14; ... release r14 & r15; canary; ret

# pc_menu's serialized patch ends at 0x50D9C28 and fills a 7-entry page-name table through
# 0x50D9C48 at runtime; air_drag starts at 0x50D9C60, leaving 0x50D9C48..0x50D9C60 free.
SSR_ABORT_CAVE = quit_menu.CAVE + 0x548      # 0x50D9C48
SSR_ABORT_CAVE_END = quit_menu.CAVE + 0x560  # 0x50D9C60


def code_writes():
    writes = []

    # Guard 1: Online event without its data
    cave1 = quit_menu.Code(CAVE)
    cave1.raw('4d85e4')                                   # test r12, r12
    cave1.data += b'\x0f\x84' + struct.pack('<i', EVENT_UNKNOWN - (cave1.here + 6))   # jz: as unknown
    cave1.data += EVENT_CASE_BYTES                        # the case's own first instructions
    cave1.jmp(EVENT_CASE + len(EVENT_CASE_BYTES))

    site1 = quit_menu.Code(EVENT_CASE)
    site1.jmp(CAVE)
    site1.data += b'\x90' * (len(EVENT_CASE_BYTES) - len(site1.data))

    writes.extend([(cave1.offset, bytes(cave1.data)), (site1.offset, bytes(site1.data))])

    # Guard 2: Texture surface-wrapper getters (0x219F860, 0x219F870, 0x219F880)
    writes.extend(TEX_QUERY_WRITES)

    # Guard 3: Compositor sub-resource format query (preserve non-NULL 64-bit rcx!)
    cave2 = quit_menu.Code(cave1.here)
    cave2.raw('488b09')                                   # mov rcx, [rcx]
    cave2.raw('4885c9')                                   # test rcx, rcx (if NULL, rcx/ecx is already 0)
    cave2.raw('7403')                                     # jz +3 (skip mov ecx, [rcx + 0x30])
    cave2.raw('8b4930')                                   # mov ecx, [rcx + 0x30]
    cave2.jmp(VISCERAL_RESUME)

    if cave2.here > SSR_CAVE:
        raise ValueError('crash guards do not fit')

    site2 = quit_menu.Code(VISCERAL_SITE)
    site2.jmp(cave2.offset)
    site2.data += b'\x90' * (len(VISCERAL_SITE_BYTES) - len(site2.data))

    writes.extend([(cave2.offset, bytes(cave2.data)), (site2.offset, bytes(site2.data))])

    # Guard 4: CopyGnmSurfaceMip (sub_2168880) NULL surface-wrapper guard
    cave3 = quit_menu.Code(GNM_COPY_CAVE)
    cave3.raw('4885ff')                                   # test rdi, rdi
    cave3.raw('740d')                                     # jz +0x0d -> ret
    cave3.raw('4885d2')                                   # test rdx, rdx
    cave3.raw('7408')                                     # jz +0x08 -> ret
    cave3.raw('48837c241000')                             # cmp qword ptr [rsp + 0x10], 0
    cave3.raw('7501')                                     # jnz +1 -> prologue
    cave3.raw('c3')                                       # ret
    cave3.data += GNM_COPY_BYTES                          # push rbp; mov rbp, rsp; push r15
    cave3.jmp(GNM_COPY_RESUME)

    if cave3.offset < quit_menu.CODE_END or cave3.here > GNM_COPY_CAVE_END:
        raise ValueError('GNM copy crash guard does not fit before quit_menu.CAVE')

    site3 = quit_menu.Code(GNM_COPY_SITE)
    site3.jmp(cave3.offset)
    site3.data += b'\x90' * (len(GNM_COPY_BYTES) - len(site3.data))

    writes.extend([(cave3.offset, bytes(cave3.data)), (site3.offset, bytes(site3.data))])
    return writes


def check_original(image_bytes):
    checks = (
        (EVENT_CASE, EVENT_CASE_BYTES),
        (TEX_QUERY_SITE, TEX_QUERY_ORIGINAL),
        (VISCERAL_SITE, VISCERAL_SITE_BYTES),
        (GNM_COPY_SITE, GNM_COPY_BYTES + bytes.fromhex('415641554154')),
        (0x21688A2, bytes.fromhex('410fb65e3b')),
        (GNM_COPY_CAVE, bytes(GNM_COPY_CAVE_END - GNM_COPY_CAVE)),
    )
    for address, original in checks:
        if image_bytes(address, address + len(original)) != original:
            raise ValueError(f'Crash guards: unexpected code at {address + quit_menu.EBOOT_BASE:#x}')


def ssr_code_writes():
    """Abort the sub_21C2100 mipmap loop cleanly when r14 == NULL at 0x21C2461.

    Any previous mip texture in r14 has already been released at 0x21C2432..0x21C244D while
    [rbp-0x118] holds the retained source texture r15. Restoring r15 and branching to 0x21C2937
    runs the function's native loop-exit cleanup instead of looping back through 0x21C28D0 with r15 = 0.
    """
    cave = quit_menu.Code(SSR_CAVE)
    cave.raw('4d85f6')                # test r14, r14
    cave.data += b'\x0f\x84' + struct.pack('<i', SSR_ABORT_CAVE - (cave.here + 6))  # jz SSR_ABORT_CAVE
    cave.data += SSR_SITE_BYTES       # cmp qword ptr [r14+0x40], 0
    cave.jmp(SSR_RESUME)              # resume at mov esi, 0; je 0x21C24A0
    if cave.here > CAVE_END:
        raise ValueError('SSR crash guard does not fit before Rebirth cave')

    abort = quit_menu.Code(SSR_ABORT_CAVE)
    abort.raw('4c8bbde8feffff')       # mov r15, qword ptr [rbp - 0x118]
    abort.jmp(SSR_LOOP_EXIT)          # jmp 0x21C2937 (releases r15 and returns 1)
    if abort.offset < 0x50D9C48 or abort.here > SSR_ABORT_CAVE_END:
        raise ValueError('SSR abort cave does not fit between PC Settings runtime table and air drag')

    site = quit_menu.Code(SSR_SITE)
    site.jmp(SSR_CAVE)
    if len(site.data) != len(SSR_SITE_BYTES):
        raise ValueError('SSR crash guard does not match its 5-byte site')
    return [
        (cave.offset, bytes(cave.data)),
        (abort.offset, bytes(abort.data)),
        (site.offset, bytes(site.data)),
    ]


def check_ssr_original(image_bytes):
    """Fail closed if any instruction relied upon by the SSR loop-abort invariant differs."""
    expected = (
        (0x21C2128, bytes.fromhex('4d85ff7445')),             # test r15, r15; je 0x21C2172
        (0x21C23E0, bytes.fromhex('4c89bde8feffff')),         # mov [rbp-0x118], r15
        (0x21C2430, bytes.fromhex('742f')),                   # je 0x21C2461 (r14 == r15)
        (0x21C2458, bytes.fromhex('7407')),                   # je 0x21C2461 (r15 == NULL)
        (SSR_SITE, SSR_ORIGINAL),                             # cmp [r14+0x40],0; mov esi,0; je 0x21C24A0
        (0x21C24D5, bytes.fromhex('4c8bbde8feffff498b4748')), # mov r15,[rbp-0x118]; mov rax,[r15+0x48]
        (0x21C26E3, bytes.fromhex('0f84e7010000')),           # je 0x21C28D0
        (SSR_LOOP_EXIT, bytes.fromhex('4d85f67421')),         # test r14, r14; je 0x21C295D
        (0x21C295D, bytes.fromhex('b3014d85ff7418')),         # mov bl, 1; test r15, r15; je 0x21C297c
    )
    for address, original in expected:
        if image_bytes(address, address + len(original)) != original:
            raise ValueError(f'SSR crash guard: unexpected code at {address + quit_menu.EBOOT_BASE:#x}')

"""Independent arithmetic oracle and bounded Windows x64 execution harness.

Only the translated baseline body is executed. BMI/LZCNT/TZCNT input bytes are
never called. Intel SDM vol. 2 instruction entries define the checked flag mask:
https://cdrdv2-public.intel.com/774492/325383-sdm-vol-2abcd.pdf
The harness preserves the Win64 nonvolatile GPRs, uses a private guest stack,
and snapshots every GPR plus flags before restoring the host stack.
"""
import ctypes as ct
import os
import struct

REGISTERS = ('rax', 'rbx', 'rcx', 'rdx', 'rsi', 'rdi', 'rbp', 'r8', 'r9',
             'r10', 'r11', 'r12', 'r13', 'r14', 'r15', 'rsp')
CF, ZF, SF, OF = 1, 64, 128, 2048
MASK64 = (1 << 64) - 1
ARITHMETIC_FLAGS = CF | ZF | SF | OF | 4 | 16  # PF and AF


def movbe_reference(width, source):
    """MOVBE reverses byte order at its width; its flags are unchanged."""
    if width not in (16, 32, 64):
        raise ValueError('MOVBE width must be 16, 32 or 64')
    data = (source & ((1 << width) - 1)).to_bytes(width // 8, 'little')
    return int.from_bytes(data, 'big')


def reference(mnemonic, width, source, other=0):
    """Return (value, defined flags, defined mask), without using x86 code."""
    mask = (1 << width) - 1
    source &= mask
    other &= mask
    carry = False
    if mnemonic == 'andn':
        result = (~source & other) & mask
    elif mnemonic == 'blsr':
        result, carry = source & (source - 1), source == 0
    elif mnemonic == 'blsi':
        result, carry = source & -source, source != 0
    elif mnemonic == 'blsmsk':
        result, carry = (source ^ (source - 1)) & mask, source == 0
    elif mnemonic == 'bextr':
        start, length = other & 255, (other >> 8) & 255
        result = ((source >> start) & ((1 << length) - 1)) if start < width else 0
    elif mnemonic == 'lzcnt':
        result, carry = width - source.bit_length(), source == 0
    elif mnemonic == 'tzcnt':
        result = (source & -source).bit_length() - 1 if source else width
        carry = source == 0
    else:
        raise ValueError(mnemonic)
    flags = (CF if carry else 0) | (ZF if result == 0 else 0)
    if mnemonic in ('lzcnt', 'tzcnt'):
        return result, flags, CF | ZF
    if mnemonic == 'bextr':
        # SF/AF/PF are undefined; OF and CF are cleared.
        return result, flags, CF | ZF | OF
    flags |= SF if result & (1 << (width - 1)) else 0
    return result, flags, CF | ZF | SF | OF


def register_name(register, width):
    if width == 64:
        return register
    names = {'rax': ('eax', 'ax'), 'rbx': ('ebx', 'bx'), 'rcx': ('ecx', 'cx'),
             'rdx': ('edx', 'dx'), 'rsi': ('esi', 'si'), 'rdi': ('edi', 'di'),
             'rbp': ('ebp', 'bp')}
    return names[register][width == 16] if register in names else register + ('w' if width == 16 else 'd')


def audit_baseline(body, capstone):
    """Fail before execution on unknown opcodes or branches outside the body."""
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    decoder.detail = True
    instructions = list(decoder.disasm(body, 0))
    if sum(x.size for x in instructions) != len(body) or not body or len(body) > 4096:
        raise ValueError('Body must fully decode within 4096 bytes')
    allowed = {'lea', 'push', 'pop', 'mov', 'movabs', 'not', 'and', 'or', 'xor',
               'test', 'cmp', 'neg', 'sub', 'add', 'shl', 'shr', 'bsf', 'bsr',
               'stc', 'clc', 'jmp', 'je', 'jne', 'jae', 'ja', 'jb', 'jbe', 'nop',
               'bswap', 'ror', 'pushfq', 'popfq'}
    boundaries = {x.address for x in instructions} | {len(body)}
    for instruction in instructions:
        if instruction.mnemonic not in allowed:
            raise ValueError('Non-baseline instruction: ' + instruction.mnemonic)
        if instruction.mnemonic.startswith('j'):
            if (len(instruction.operands) != 1 or
                    instruction.operands[0].type != capstone.x86.X86_OP_IMM or
                    instruction.operands[0].imm not in boundaries):
                raise ValueError('Branch leaves translated body')
    return instructions


class NativeHarness:
    """Reusable for one body; never executes source instruction encodings."""

    def __init__(self, body, keystone, capstone, *, rip_data=None, rip_offset=0):
        if os.name != 'nt' or ct.sizeof(ct.c_void_p) != 8:
            raise RuntimeError('Native CPU harness requires Windows x64')
        instructions = audit_baseline(body, capstone)
        self.context = (ct.c_uint64 * 52)()
        self.stack = (ct.c_ubyte * 16384)()
        self.guest_rsp = (ct.addressof(self.stack) + 12000) & ~15
        self.context[1] = self.guest_rsp
        saved = ('rbx', 'rbp', 'rsi', 'rdi', 'r12', 'r13', 'r14', 'r15')
        before = ['push ' + reg for reg in saved]
        before += ['mov rax, rcx', 'mov [rax], rsp', 'mov rsp, [rax+8]',
                   'lea rsp, [rsp-256]', 'push qword ptr [rax+16]', 'popfq',
                   'lea rsp, [rsp+256]']
        before += [f'mov {reg}, [rax+{24 + i * 8}]' for i, reg in enumerate(REGISTERS[:-1]) if reg != 'rax']
        before += ['mov rax, [rax+24]']
        # Saving below the 128-byte red zone keeps the region under test intact.
        after = ['lea rsp, [rsp-128]', 'push rax', f'mov rax, {ct.addressof(self.context)}']
        after += [f'mov [rax+{160 + i * 8}], {reg}' for i, reg in enumerate(REGISTERS[:-1]) if reg != 'rax']
        after += ['mov r11, [rsp]', 'mov [rax+160], r11', 'lea r11, [rsp+136]',
                  'mov [rax+280], r11', 'pushfq', 'pop r11', 'mov [rax+288], r11',
                  'mov rsp, [rax]']
        after += ['pop ' + reg for reg in reversed(saved)] + ['ret']
        assembler = keystone.Ks(keystone.KS_ARCH_X86, keystone.KS_MODE_64)
        prefix = bytes(assembler.asm('; '.join(before))[0])
        suffix = bytes(assembler.asm('; '.join(after))[0])
        code = bytearray(prefix + body + suffix)
        code_pages = (len(code) + 4095) & ~4095
        if rip_data is not None and (not 0 <= rip_offset < len(rip_data) <= 4096):
            raise ValueError('RIP fixture must fit in one private data page')
        rip_instructions = [item for item in instructions if any(
            operand.type == capstone.x86.X86_OP_MEM and operand.mem.base == capstone.x86.X86_REG_RIP
            for operand in item.operands)]
        if rip_instructions and rip_data is None:
            raise ValueError('RIP operands need an explicit private fixture')
        kernel = ct.WinDLL('kernel32', use_last_error=True)
        kernel.VirtualAlloc.argtypes = [ct.c_void_p, ct.c_size_t, ct.c_uint32, ct.c_uint32]
        kernel.VirtualAlloc.restype = ct.c_void_p
        kernel.VirtualProtect.argtypes = [ct.c_void_p, ct.c_size_t, ct.c_uint32, ct.POINTER(ct.c_uint32)]
        kernel.VirtualProtect.restype = ct.c_int
        kernel.VirtualFree.argtypes = [ct.c_void_p, ct.c_size_t, ct.c_uint32]
        kernel.VirtualFree.restype = ct.c_int
        kernel.GetCurrentProcess.restype = ct.c_void_p
        kernel.FlushInstructionCache.argtypes = [ct.c_void_p, ct.c_void_p, ct.c_size_t]
        kernel.FlushInstructionCache.restype = ct.c_int
        self.kernel, self.address = kernel, kernel.VirtualAlloc(None, code_pages + (4096 if rip_data is not None else 0), 0x3000, 4)
        if not self.address:
            raise ct.WinError(ct.get_last_error())
        self.rip_data_address = self.address + code_pages if rip_data is not None else None
        if rip_data is not None:
            ct.memmove(self.rip_data_address, bytes(rip_data), len(rip_data))
            for item in rip_instructions:
                target = self.rip_data_address + rip_offset
                relative = target - (self.address + len(prefix) + item.address + item.size)
                struct.pack_into('<i', code, len(prefix) + item.address + item.disp_offset, relative)
        ct.memmove(self.address, bytes(code), len(code))
        previous = ct.c_uint32()
        if not kernel.VirtualProtect(self.address, code_pages, 0x20, ct.byref(previous)):
            self.close()
            raise ct.WinError(ct.get_last_error())
        kernel.FlushInstructionCache(kernel.GetCurrentProcess(), self.address, len(code))
        self.function = ct.WINFUNCTYPE(None, ct.c_void_p)(self.address)

    def run(self, registers, flags=0x8d7, stack_values=None):
        ct.memset(ct.addressof(self.stack), 0xA5, ct.sizeof(self.stack))
        for offset, value in (stack_values or {}).items():
            ct.c_uint64.from_address(self.guest_rsp + offset).value = value
        before_zone = ct.string_at(self.guest_rsp - 128, 128)
        self.stack_window_before = ct.string_at(self.guest_rsp - 128, 256)
        self.context[2] = flags
        for i, reg in enumerate(REGISTERS[:-1]):
            self.context[3 + i] = registers[reg] & MASK64
        self.function(ct.addressof(self.context))
        self.stack_window_after = ct.string_at(self.guest_rsp - 128, 256)
        output = {reg: self.context[20 + i] for i, reg in enumerate(REGISTERS)}
        return output, self.context[36], before_zone, ct.string_at(self.guest_rsp - 128, 128)

    def close(self):
        if getattr(self, 'address', None):
            self.kernel.VirtualFree(self.address, 0, 0x8000)
            self.address = None

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()

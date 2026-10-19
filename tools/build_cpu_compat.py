"""Prepare guarded legacy CPU translations for this local game image.

Only decode functions described by the game's unwind tables or exported symbols.
The plan is applied to generated images; original PKGs and game files stay intact.
"""
import collections
import argparse
import bisect
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import sys
import time

PORT = Path(__file__).resolve().parent.parent
sys.path.insert(0, os.environ.get('BB_CPU_DEPS_DIR', str(PORT / 'tools/python-deps')))
sys.path.insert(0, str(PORT / 'scripts'))
import capstone
import keystone
from link_modules import module
from cpu_compat import (atomic_output, COUNT_SPANS, COUNT_SPAN_MODULE_SHA256,
                        COUNT_SPAN_FUNCTION, COUNT_SPAN_EVIDENCE_REFERENCE, COUNT_SPAN_EVIDENCE_SHA256)

SUPPORTED = {'andn', 'bextr', 'blsr', 'blsi', 'blsmsk', 'tzcnt', 'lzcnt', 'movbe'}
ASSEMBLER = keystone.Ks(keystone.KS_ARCH_X86, keystone.KS_MODE_64)
CACHE = {}


def canonical(register):
    names = {'eax': 'rax', 'ebx': 'rbx', 'ecx': 'rcx', 'edx': 'rdx', 'esi': 'rsi',
             'edi': 'rdi', 'ebp': 'rbp', 'esp': 'rsp', 'ax': 'rax', 'bx': 'rbx',
             'cx': 'rcx', 'dx': 'rdx', 'si': 'rsi', 'di': 'rdi', 'bp': 'rbp', 'sp': 'rsp'}
    if register in names:
        return names[register]
    if register.startswith('r') and register.endswith(('d', 'w')):
        return register[:-1]
    return register


def word(register, width):
    if width == 64:
        return register
    if width == 16:
        return {'rax': 'ax', 'rbx': 'bx', 'rcx': 'cx', 'rdx': 'dx', 'rsi': 'si',
                'rdi': 'di', 'rbp': 'bp', 'rsp': 'sp'}.get(register, register + 'w')
    return {'rax': 'eax', 'rbx': 'ebx', 'rcx': 'ecx', 'rdx': 'edx', 'rsi': 'esi',
            'rdi': 'edi', 'rbp': 'ebp', 'rsp': 'esp'}.get(register, register + 'd')


def movbe_body(operands):
    """MOVBE load/store: reverse bytes while preserving every arithmetic flag.

    MOV+BSWAP do not affect flags. The 16-bit form uses ROR surrounded by
    PUSHFQ/POPFQ, below the preserved red zone. No original MOVBE is executed.
    """
    args = operands.split(', ')
    if len(args) != 2 or sum('[' in arg for arg in args) != 1:
        raise ValueError('MOVBE requires one register and one memory operand.')
    memory_index = 0 if '[' in args[0] else 1
    memory = re.fullmatch(r'(word|dword|qword) ptr \[([a-z0-9x +*\-]+)\]', args[memory_index])
    if not memory:
        raise ValueError('Unsupported MOVBE memory operand.')
    width = {'word': 16, 'dword': 32, 'qword': 64}[memory[1]]
    register = canonical(args[1 - memory_index])
    allowed = {'rax', 'rbx', 'rcx', 'rdx', 'rsi', 'rdi', 'rbp', *('r' + str(i) for i in range(8, 16))}
    if register not in allowed or args[1 - memory_index] != word(register, width):
        raise ValueError('Unsupported MOVBE register operand/width.')
    address_registers = re.findall(r'\b(?:r(?:1[0-5]|[8-9])|r(?:ax|bx|cx|dx|si|di|bp|sp|ip))\b', memory[2])
    residue = memory[2]
    for item in address_registers:
        residue = re.sub(r'\b' + item + r'\b', '0', residue)
    if not re.fullmatch(r'[0-9a-fx +*\-]+', residue):
        raise ValueError('Unsupported MOVBE memory address expression.')
    scratch = next(reg for reg in ('r10', 'r11', 'r8', 'r9', 'rax', 'rdx', 'rsi')
                   if reg != register and reg not in address_registers)
    temporary = word(scratch, width)
    depth = 128 + 8 + (8 if width == 16 else 0)
    adjusted = list(args)
    adjusted[memory_index] = re.sub(r'\brsp\b', f'rsp + {depth}', args[memory_index])
    lines = ['lea rsp, [rsp-128]', 'push ' + scratch]
    if width == 16:
        lines.append('pushfq')
    lines += ['mov ' + temporary + ', ' + adjusted[1]]
    lines += ['ror ' + temporary + ', 8'] if width == 16 else ['bswap ' + temporary]
    lines += ['mov ' + adjusted[0] + ', ' + temporary]
    if width == 16:
        lines.append('popfq')
    lines += ['pop ' + scratch, 'lea rsp, [rsp+128]']
    return bytes(ASSEMBLER.asm('; '.join(lines))[0])


def body(mnemonic, operands):
    key = mnemonic, operands
    if key in CACHE:
        return CACHE[key]
    if mnemonic not in SUPPORTED:
        raise ValueError('Unsupported instruction.')
    if mnemonic == 'movbe':
        CACHE[key] = movbe_body(operands)
        return CACHE[key]
    args = operands.split(', ')
    if len(args) != (3 if mnemonic in ('andn', 'bextr') else 2):
        raise ValueError('Unexpected operand count.')
    allowed = {'rax', 'rbx', 'rcx', 'rdx', 'rsi', 'rdi', 'rbp', *('r' + str(i) for i in range(8, 16))}
    destination = canonical(args[0])
    if destination not in allowed:
        raise ValueError('Destination register is not supported.')
    width = next(width for width in (16, 32, 64) if args[0] == word(destination, width))
    if width == 16 and mnemonic not in ('lzcnt', 'tzcnt'):
        raise ValueError('Only count instructions support 16-bit operands.')
    original = []
    memory_index = 2 if mnemonic == 'andn' else 1
    for index, operand in enumerate(args):
        register = canonical(operand)
        if register in allowed and operand == word(register, width):
            original.append(register)
            continue
        memory = re.fullmatch(r'(word|dword|qword) ptr \[([a-z0-9x +*\-]+)\]', operand)
        if index != memory_index or not memory or memory[1] != {16: 'word', 32: 'dword', 64: 'qword'}[width]:
            raise ValueError('Unsupported operand form or width.')
        registers = re.findall(r'\b(?:r(?:1[0-5]|[8-9])|r(?:ax|bx|cx|dx|si|di|bp|sp|ip))\b', memory[2])
        residue = memory[2]
        for register in registers:
            residue = re.sub(r'\b' + register + r'\b', '0', residue)
        if not re.fullmatch(r'[0-9a-fx +*\-]+', residue):
            raise ValueError('Unsupported memory address expression.')
        original.extend(registers)
    temporary = [reg for reg in ('r10', 'r11', 'r8', 'r9', 'rax', 'rdx', 'rsi', 'rdi', 'rbx', 'r12')
                 if reg not in original and reg != 'rcx'][:3]
    first, control, mask = [word(reg, width) for reg in temporary]
    saved = temporary + (['rcx'] if mnemonic == 'bextr' and destination != 'rcx' else [])
    # Compensate for the exact prologue depth when the original operand addresses
    # the stack. Scratch selection also excludes all memory address registers.
    adjusted = [re.sub(r'\brsp\b', f'rsp + {128 + 8 * len(saved)}', arg)
                if '[' in arg else arg for arg in args]
    # Leave the entire SysV red zone intact before saving scratch registers.
    lines = ['lea rsp, [rsp-128]'] + ['push ' + reg for reg in saved]
    lines += ['mov ' + first + ', ' + adjusted[1]]
    if mnemonic == 'andn':
        lines += ['not ' + first, 'and ' + first + ', ' + adjusted[2]]
    elif mnemonic in ('blsr', 'blsi', 'blsmsk'):
        lines += ['test ' + first + ', ' + first, 'jz zero']
        if mnemonic == 'blsr':
            lines += ['lea ' + mask + ', [' + temporary[0] + '-1]', 'and ' + first + ', ' + mask]
        elif mnemonic == 'blsi':
            lines += ['mov ' + mask + ', ' + first, 'neg ' + first, 'and ' + first + ', ' + mask, 'stc']
        else:
            lines += ['lea ' + mask + ', [' + temporary[0] + '-1]', 'xor ' + first + ', ' + mask]
        lines += ['jmp done', 'zero:']
        if mnemonic == 'blsmsk':
            lines += ['mov ' + first + ', -1', 'test ' + first + ', ' + first, 'stc']
        else:
            lines += ['xor ' + first + ', ' + first] + (['stc'] if mnemonic == 'blsr' else [])
    elif mnemonic == 'bextr':
        lines += ['mov ' + word(temporary[1], 32) + ', ' + word(canonical(args[2]), 32),
                  'mov ecx, ' + word(temporary[1], 32), 'and ecx, 255', 'cmp ecx, ' + str(width),
                  'jae zero', 'shr ' + first + ', cl', 'mov ecx, ' + word(temporary[1], 32),
                  'shr ecx, 8', 'and ecx, 255', 'cmp ecx, ' + str(width), 'jae nomask',
                  'mov ' + mask + ', -1', 'shl ' + mask + ', cl', 'not ' + mask,
                  'and ' + first + ', ' + mask, 'jmp done', 'nomask:',
                  'test ' + first + ', ' + first, 'jmp done', 'zero:', 'xor ' + first + ', ' + first]
    elif mnemonic in ('tzcnt', 'lzcnt'):
        lines += [('bsf ' if mnemonic == 'tzcnt' else 'bsr ') + mask + ', ' + first, 'jz zero']
        if mnemonic == 'tzcnt':
            lines += ['mov ' + first + ', ' + mask]
        else:
            lines += ['mov ' + first + ', ' + str(width - 1), 'sub ' + first + ', ' + mask]
        lines += ['test ' + first + ', ' + first, 'jmp done', 'zero:',
                  'mov ' + first + ', ' + str(width), 'test ' + first + ', ' + first, 'stc']
    lines += ['done:', 'mov ' + args[0] + ', ' + first]
    lines += ['pop ' + reg for reg in reversed(saved)] + ['lea rsp, [rsp+128]']
    assembled, _ = ASSEMBLER.asm('; '.join(lines))
    CACHE[key] = bytes(assembled)
    return CACHE[key]


def file_offset(mod, address, length=1, executable=False):
    part = next(part for part in mod['ph'] if part['type'] == 1
                and (not executable or part['flags'] & 1)
                and part['vaddr'] <= address and address + length <= part['vaddr'] + part['filesz'])
    return part['offset'] + address - part['vaddr']


def function_ranges(mod):
    functions = set()
    header = next((part for part in mod['ph'] if part['type'] == 0x6474e550), None)
    if header:
        offset, address = header['offset'], header['vaddr']
        if bytes(mod['elf'][offset:offset+4]) != bytes.fromhex('011b033b'):
            raise ValueError('Unrecognized unwind table encoding.')
        count = struct.unpack_from('<I', mod['elf'], offset + 8)[0]
        if count > 250000 or 12 + 8 * count > header['filesz']:
            raise ValueError('Unwind table bounds are invalid.')
        for index in range(count):
            start, fde = struct.unpack_from('<ii', mod['elf'], offset + 12 + index * 8)
            start, fde = address + start, address + fde
            record = file_offset(mod, fde, 16)
            length, cie, displacement, size = struct.unpack_from('<IIiI', mod['elf'], record)
            if length < 12 or fde + 8 + displacement != start or not 0 < size < 4 * 1024**2:
                raise ValueError('Unwind function record did not match its index.')
            file_offset(mod, start, size, executable=True)
            functions.add((start, size))
    for symbol in mod['symbols']:
        if symbol['type'] == 2 and symbol['section'] and symbol['size']:
            try:
                file_offset(mod, symbol['value'], symbol['size'], executable=True)
                functions.add((symbol['value'], symbol['size']))
            except StopIteration:
                pass
    return sorted(functions)


def rip_fixups(instruction, replacement):
    """Locate displacement fields; do not bake the future trampoline address in."""
    targets = [instruction.address + instruction.size + operand.mem.disp
               for operand in instruction.operands
               if operand.type == capstone.x86.X86_OP_MEM and operand.mem.base == capstone.x86.X86_REG_RIP]
    if not targets:
        return []
    if len(targets) != 1:
        raise ValueError('Unexpected number of RIP-relative source operands.')
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    decoder.detail = True
    fixups = []
    for item in decoder.disasm(replacement, 0):
        if any(operand.type == capstone.x86.X86_OP_MEM and operand.mem.base == capstone.x86.X86_REG_RIP
               for operand in item.operands):
            # RIP addressing in 64-bit mode has a disp32 after ModRM mod=00,
            # r/m=101. Capstone 5.0.7 incorrectly reports disp_size=2 for the
            # operand-size-prefixed MOV16 used by MOVBE16; verify its actual
            # encoding instead of trusting that metadata field. Our generated
            # RIP loads/stores have no immediate after the displacement.
            if (not 0 < item.modrm_offset < item.disp_offset or item.modrm & 0xc7 != 5
                    or item.disp_offset + 4 != item.size
                    or struct.unpack_from('<i', item.bytes, item.disp_offset)[0] != item.disp):
                raise ValueError('Unexpected RIP disp32 encoding.')
            fixups.append({'offset': item.address + item.disp_offset,
                           'next_offset': item.address + item.size, 'target': targets[0]})
    if len(fixups) != 1:
        raise ValueError('RIP-relative source was not retained exactly once.')
    return fixups


def audit_count_span_entries(mod):
    """Audit the closed D015 spans; preserve every unresolved candidate.

    Raw byte searches find possible relative transfers even outside the ranges
    decoded by the normal instruction scanner. A candidate may be discarded
    only when its bytes belong to another decoded instruction. This is not a
    global proof about indirect branches, jump tables or overlapping code.
    """
    function_start, function_end, function_hash = COUNT_SPAN_FUNCTION
    offset = file_offset(mod, function_start, function_end - function_start, True)
    context = bytes(mod['elf'][offset:offset + function_end - function_start])
    if mod['sha256'] != COUNT_SPAN_MODULE_SHA256 or hashlib.sha256(context).hexdigest() != function_hash:
        raise ValueError('D015 source/function identity changed; count spans require fresh review.')
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    context_rows = list(decoder.disasm_lite(context, function_start))
    if not context_rows or context_rows[-1][0] + context_rows[-1][1] != function_end:
        raise ValueError('D015 allocator did not decode completely.')
    functions = function_ranges(mod)
    starts = [a for a, _ in functions]
    cache = {function_start: context_rows}
    interiors = {a for start, spec in COUNT_SPANS.items() for a in range(start + 1, start + len(bytes.fromhex(spec[0])))}
    known_entries = []
    unresolved = []
    rejected_candidates = []

    def known(kind, address, source=None):
        if address in interiors:
            record = {'kind': kind, 'address': address}
            if source is not None:
                record['source'] = source
            known_entries.append(record)

    for address, _ in functions:
        known('function', address)
    for symbol in mod['symbols']:
        if symbol['section']:
            known('symbol', symbol['value'])
    resolved_relocations = 0
    for source, kind, symid, addend in mod['relocs']:
        if kind == 8:
            known('relocation', addend, source)
            resolved_relocations += 1
        elif kind in (1, 6, 7) and mod['symbols'][symid]['section']:
            known('relocation', mod['symbols'][symid]['value'] + addend, source)
            resolved_relocations += 1

    def candidate(source, target, size):
        index = bisect.bisect_right(starts, source) - 1
        owner = functions[index] if index >= 0 else None
        containing_instruction = None
        if owner and owner[0] <= source < owner[0] + owner[1]:
            start, length = owner
            if start not in cache:
                at = file_offset(mod, start, length, True)
                cache[start] = list(decoder.disasm_lite(bytes(mod['elf'][at:at + length]), start))
            containing_instruction = next((row for row in cache[start] if row[0] <= source < row[0] + row[1]), None)
        if containing_instruction:
            address, length, mnemonic, operands = containing_instruction
            transfer = mnemonic == 'call' or mnemonic.startswith(('j', 'loop'))
            # Also recognize a prefix before the raw candidate's first byte.
            if transfer and operands == hex(target):
                known('direct_branch', target, address)
                return
            if address < source and source + size <= address + length:
                reason = 'contained_in_other_decoded_instruction'
            elif address < source and source < address + length:
                reason = 'starts_inside_other_decoded_instruction'
            else:
                reason = None
            if reason:
                at = file_offset(mod, address, length, True)
                rejected_candidates.append({'source': source, 'target': target, 'reason': reason,
                                            'containing_address': address, 'containing_bytes': bytes(mod['elf'][at:at + length]).hex(),
                                            'containing_instruction': mnemonic + ' ' + operands})
                return
        unresolved.append({'source': source, 'target': target, 'size': size})

    executable_bytes = 0
    for ph in mod['ph']:
        if ph['type'] != 1 or not ph['flags'] & 1:
            continue
        raw = bytes(mod['elf'][ph['offset']:ph['offset'] + ph['filesz']])
        base = ph['vaddr']
        executable_bytes += len(raw)
        for prefix in (b'\xe8', b'\xe9', *(bytes((15, x)) for x in range(0x80, 0x90))):
            pos = raw.find(prefix)
            while pos >= 0:
                length = len(prefix) + 4
                if pos + length <= len(raw):
                    target = base + pos + length + struct.unpack_from('<i', raw, pos + len(prefix))[0]
                    if target in interiors:
                        candidate(base + pos, target, length)
                pos = raw.find(prefix, pos + 1)
    low, high = min(interiors) - 129, max(interiors) + 129
    offset = file_offset(mod, low, high - low, True)
    raw = bytes(mod['elf'][offset:offset + high - low])
    for pos, opcode in enumerate(raw[:-1]):
        if 0x70 <= opcode <= 0x7f or 0xe0 <= opcode <= 0xe3 or opcode == 0xeb:
            source = low + pos
            target = source + 2 + struct.unpack_from('<b', raw, pos + 1)[0]
            if target in interiors:
                candidate(source, target, 2)
    return {'schema_version': 1, 'method': 'allocator_count_spans_v1',
            'source_sha256': mod['sha256'], 'function_start': function_start, 'function_end': function_end,
            'function_sha256': function_hash, 'global_indirect_proof': False,
            'spans': [{'address': a, 'end': a + len(bytes.fromhex(s[0])), 'bytes': s[0]} for a, s in COUNT_SPANS.items()],
            'coverage': {'executable_bytes_scanned_rel32': executable_bytes, 'rel8_window': [low, high],
                         'function_entries': len(functions), 'symbols': len(mod['symbols']),
                         'resolved_relocation_destinations': resolved_relocations,
                         'jump_tables': 'not_globally_resolved', 'landing_pads': 'not_globally_resolved',
                         'overlapping_code': 'not_globally_proven_absent'},
            'known_interior_entries': known_entries, 'unresolved_candidates': unresolved,
            'discarded_raw_candidates_with_instruction_proof': rejected_candidates}


def count_span_evidence(report):
    serialized = (json.dumps(report, sort_keys=True, indent=2) + '\n').encode('utf-8')
    digest = hashlib.sha256(serialized).hexdigest()
    if report['known_interior_entries'] or report['unresolved_candidates']:
        raise ValueError('D015 count spans have interior or unresolved entry candidates.')
    if digest != COUNT_SPAN_EVIDENCE_SHA256:
        raise ValueError('D015 entry audit changed; review its evidence before generating spans.')
    return {'reference': COUNT_SPAN_EVIDENCE_REFERENCE, 'sha256': digest,
            'method': 'allocator_count_spans_v1', 'global_indirect_proof': False,
            'known_entries': [], 'unresolved_candidates': []}


def count_span_site(mod, address, entry_evidence):
    """Construct one exact two-instruction sequence; body contains only count."""
    original, instruction, body_hash = COUNT_SPANS[address]
    expected = bytes.fromhex(original)
    offset = file_offset(mod, address, len(expected), True)
    if mod['sha256'] != COUNT_SPAN_MODULE_SHA256 or bytes(mod['elf'][offset:offset + len(expected)]) != expected:
        raise ValueError('D015 count span bytes/source changed.')
    rows = list(capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64).disasm_lite(expected, address))
    if len(rows) != 2 or [row[1] for row in rows] != [4, len(expected) - 4]:
        raise ValueError('D015 count span instruction boundaries changed.')
    mnemonic, operands = instruction.split(' ', 1)
    replacement = body(mnemonic, operands)
    if hashlib.sha256(replacement).hexdigest() != body_hash:
        raise ValueError('D015 count-only body changed; update its reviewed whitelist deliberately.')
    start, end, digest = COUNT_SPAN_FUNCTION
    context_offset = file_offset(mod, start, end - start, True)
    if hashlib.sha256(bytes(mod['elf'][context_offset:context_offset + end - start])).hexdigest() != digest:
        raise ValueError('D015 count span function context changed.')
    return {'kind': 'count_span', 'module': 'eboot.bin', 'address': address, 'bytes': original,
            'instruction': instruction, 'body': replacement.hex(), 'rip_fixups': [],
            'span': {'count_length': 4, 'tail_bytes': expected[4:].hex(),
                     'instruction_lengths': [4, len(expected) - 4], 'function_start': start,
                     'function_end': end, 'function_sha256': digest, 'entry_evidence': entry_evidence}}


SHORT_SPAN_MAX_TAIL = 2
SHORT_SPAN_METHOD = 'short_span_entries_v1'


def short_reason(mnemonic, error):
    effect = ('may execute with incorrect BSF/BSR semantics.' if mnemonic in ('lzcnt', 'tzcnt')
              else 'remains unsupported and can raise an illegal-instruction fault.')
    return f'Short instruction: {error}; {effect}'


def movable(row, last):
    """An instruction that behaves the same at any address (`last`: may end the span)."""
    _, _, mnemonic, operands = row
    if 'rip' in operands or mnemonic in SUPPORTED:
        return False
    if mnemonic in ('ret', 'retq') and not operands:
        return last
    return not (mnemonic == 'call' or mnemonic.startswith(('j', 'loop', 'ret', 'iret'))
                or mnemonic in ('syscall', 'sysenter', 'int', 'int3', 'ud2', 'hlt', 'enter', 'leave'))


def short_span(rows, index, backward=False):
    """(start, end, pieces) around the short instruction rows[index]: enough instructions for a
    five-byte jump. Pieces are ('translate', row) or ('move', row), in order. Forward: the
    following instructions (other short count/MOVBE ones translated too); backward: the
    previous ones."""
    pieces = [('translate', rows[index])]
    total = rows[index][1]
    if backward:
        at = index - 1
        while total < 5:
            if at < 0 or len(pieces) > SHORT_SPAN_MAX_TAIL or not movable(rows[at], False):
                raise ValueError('no movable instructions before it')
            pieces.insert(0, ('move', rows[at]))
            total += rows[at][1]
            at -= 1
        return pieces[0][1][0], rows[index][0] + rows[index][1], pieces
    at = index + 1
    while total < 5:
        if at >= len(rows) or len(pieces) > SHORT_SPAN_MAX_TAIL:
            raise ValueError('no room for a jump in the following instructions')
        row = rows[at]
        if row[2] in SUPPORTED:
            # Translated as well (a long one then leaves its own site to this span).
            pieces.append(('translate', row))
        elif movable(row, True):
            pieces.append(('move', row))
            if row[2] in ('ret', 'retq'):
                total += row[1]
                break
        else:
            raise ValueError(f'the next instruction cannot move ({row[2]} {row[3]})')
        total += row[1]
        at += 1
    if total < 5:
        raise ValueError('no room for a jump in the following instructions')
    last = pieces[-1][1]
    return rows[index][0], last[0] + last[1], pieces


def audit_short_span_entries(mod, spans):
    """Entries into the moved instructions of `spans` ({start: end}): the D015 audit for many.

    Function starts, symbols, relocation targets, every raw rel32 call/jump/jcc in the
    executable segments and every rel8 jump near a span are checked; a raw candidate is
    discarded only when it lies inside another decoded instruction. Jump tables and landing
    pads are not globally resolved (as for D015). Returns {span start: [entries]}.
    """
    interiors = {}
    for start, end in spans.items():
        for a in range(start + 1, end):
            interiors[a] = start
    found = collections.defaultdict(list)
    functions = function_ranges(mod)
    starts = [a for a, _ in functions]
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    cache = {}

    def entry(kind, address, source=None):
        if address in interiors:
            found[interiors[address]].append({'kind': kind, 'address': address, 'source': source})

    for address, _ in functions:
        entry('function', address)
    for symbol in mod['symbols']:
        if symbol['section']:
            entry('symbol', symbol['value'])
    for source, kind, symid, addend in mod['relocs']:
        if kind == 8:
            entry('relocation', addend, source)
        elif kind in (1, 6, 7) and mod['symbols'][symid]['section']:
            entry('relocation', mod['symbols'][symid]['value'] + addend, source)

    def candidate(source, target):
        index = bisect.bisect_right(starts, source) - 1
        owner = functions[index] if index >= 0 else None
        if owner and owner[0] <= source < owner[0] + owner[1]:
            start, length = owner
            if start not in cache:
                at = file_offset(mod, start, length, True)
                cache[start] = list(decoder.disasm_lite(bytes(mod['elf'][at:at + length]), start))
            row = next((row for row in cache[start] if row[0] <= source < row[0] + row[1]), None)
            if row:
                address, length, mnemonic, operands = row
                if (mnemonic == 'call' or mnemonic.startswith(('j', 'loop'))) and operands == hex(target):
                    entry('direct_branch', target, address)
                    return
                if address < source < address + length:
                    return  # the raw bytes are inside another decoded instruction
        entry('unresolved', target, source)

    for ph in mod['ph']:
        if ph['type'] != 1 or not ph['flags'] & 1:
            continue
        raw = bytes(mod['elf'][ph['offset']:ph['offset'] + ph['filesz']])
        base = ph['vaddr']
        for prefix in (b'\xe8', b'\xe9', *(bytes((15, x)) for x in range(0x80, 0x90))):
            pos = raw.find(prefix)
            while pos >= 0:
                length = len(prefix) + 4
                if pos + length <= len(raw):
                    target = base + pos + length + struct.unpack_from('<i', raw, pos + len(prefix))[0]
                    if target in interiors:
                        candidate(base + pos, target)
                pos = raw.find(prefix, pos + 1)
        for start, end in spans.items():
            low, high = max(base, start - 129), min(base + len(raw), end + 129)
            for source in range(low, high - 1):
                opcode = raw[source - base]
                if 0x70 <= opcode <= 0x7f or 0xe0 <= opcode <= 0xe3 or opcode == 0xeb:
                    target = source + 2 + struct.unpack_from('<b', raw, source - base + 1)[0]
                    if target in interiors:
                        candidate(source, target)
    return found


def add_short_spans(mod, name, candidates, plan):
    """Plan sites for short instructions whose span nothing enters: forward spans first, then
    backward ones for the rest; the others stay unhandled."""
    done = set()
    sites = []
    failures = {}
    for backward in (False, True):
        spans = {}
        for rows, index, function in candidates:
            address = rows[index][0]
            if address in done:
                continue
            try:
                start, end, pieces = short_span(rows, index, backward)
            except ValueError as error:
                failures.setdefault(address, str(error))
                continue
            if any(piece[0] == 'translate' and piece[1][0] in done for piece in pieces):
                continue
            spans[start] = (end, pieces, rows[index], function)
        entries = audit_short_span_entries(mod, {a: v[0] for a, v in spans.items()}) if spans else {}
        for start, (end, pieces, row, function) in sorted(spans.items()):
            if entries.get(start):
                kinds = sorted({item['kind'] for item in entries[start]})
                failures[row[0]] = 'an instruction it moves is entered (' + ', '.join(kinds) + ')'
                continue
            translated = [piece[1][0] for piece in pieces if piece[0] == 'translate']
            if any(a in done for a in translated) or any(s0 < end and start < e0 for s0, e0 in
                                                        ((x['image_span'][0], x['image_span'][1]) for x in sites)):
                continue
            try:
                code = b''.join(body(r[2], r[3]) if kind == 'translate'
                                else bytes(mod['elf'][file_offset(mod, r[0], r[1], True):][:r[1]])
                                for kind, r in pieces)
            except ValueError as error:
                failures[row[0]] = str(error)
                continue
            at = file_offset(mod, start, end - start, executable=True)
            original = bytes(mod['elf'][at:at + end - start])
            done.update(translated)
            sites.append({'kind': 'short_span', 'module': name, 'address': start,
                          'bytes': original.hex(), 'body': code.hex(),
                          'instruction': row[2] + ' ' + row[3], 'rip_fixups': [],
                          'image_span': (start, end),
                          'span': {'pieces': [[kind, r[0], r[1], r[2] + ' ' + r[3]] for kind, r in pieces],
                                   'function': [function[0], function[1]],
                                   'entry_audit': {'method': SHORT_SPAN_METHOD,
                                                   'global_indirect_proof': False,
                                                   'known_entries': [], 'unresolved_candidates': []}}})
    # A long instruction a span translates loses its own site.
    plan['sites'][:] = [site for site in plan['sites']
                        if site['module'] != name or site.get('kind') == 'count_span'
                        or site['address'] not in done]
    for site in sites:
        del site['image_span']
        plan['sites'].append(site)
    for rows, index, _ in candidates:
        address, _, mnemonic, operands = rows[index]
        if address not in done:
            plan['unhandled'].append({'module': name, 'address': hex(address),
                                      'instruction': mnemonic + ' ' + operands,
                                      'reason': short_reason(mnemonic, failures.get(address, 'no span'))})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game', type=Path, help='Plaintext game directory, read only')
    parser.add_argument('--out', type=Path, default=PORT / 'tools/legacy-cpu-plan.json')
    args = parser.parse_args()
    game = args.game
    plan = {'version': 4, 'modules': {}, 'sites': [], 'unhandled': [], 'coverage': {}}
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    detailed = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    detailed.detail = True
    seen = set()
    for name, path in [('eboot.bin', game / 'eboot.bin'), ('libc.prx', game / 'sce_module/libc.prx'),
                       ('libSceFios2.prx', game / 'sce_module/libSceFios2.prx')]:
        mod = module(path)
        plan['modules'][name] = mod['sha256']
        entry_evidence = None
        if name == 'eboot.bin':
            entry_report = audit_count_span_entries(mod)
            evidence_path = PORT / COUNT_SPAN_EVIDENCE_REFERENCE
            evidence_path.parent.mkdir(parents=True, exist_ok=True)
            with atomic_output(evidence_path) as stream:
                stream.write((json.dumps(entry_report, sort_keys=True, indent=2) + '\n').encode('utf-8'))
            entry_evidence = count_span_evidence(entry_report)
        functions = function_ranges(mod)
        short_candidates = []
        coverage = {'declared_functions': len(functions), 'fully_decoded_functions': 0,
                    'partial_functions': 0, 'undecoded_bytes': 0, 'partial_samples': [],
                    'claim': 'Only unwind/export function ranges; no complete control-flow coverage claim.'}
        print(json.dumps({'stage': 'analyzing_cpu_instructions', 'module': name, 'functions': len(functions)}), flush=True)
        report_time = time.monotonic()
        for index, (start, size) in enumerate(functions):
            offset = file_offset(mod, start, size, executable=True)
            decoded_end = start
            rows = list(decoder.disasm_lite(bytes(mod['elf'][offset:offset+size]), start))
            for row_index, (address, length, mnemonic, operands) in enumerate(rows):
                decoded_end = address + length
                if mnemonic not in SUPPORTED or (name, address) in seen:
                    continue
                seen.add((name, address))
                # strlen already has its guarded whole-function replacement.
                if name == 'libc.prx' and 0x2f970 <= address < 0x2fa50:
                    continue
                if name == 'eboot.bin' and address in COUNT_SPANS:
                    plan['sites'].append(count_span_site(mod, address, entry_evidence))
                    continue
                if length < 5:
                    # Short spans: the instruction and the next ones up to the jump's five bytes,
                    # kept only if the entry audit finds no way into the moved ones.
                    short_candidates.append((rows, row_index, (start, start + size)))
                    continue
                try:
                    replacement = body(mnemonic, operands)
                    at = file_offset(mod, address, length, executable=True)
                    instruction = next(detailed.disasm(bytes(mod['elf'][at:at+length]), address))
                    fixups = rip_fixups(instruction, replacement)
                except ValueError as error:
                    plan['unhandled'].append({'module': name, 'address': hex(address), 'instruction': mnemonic + ' ' + operands,
                                              'reason': str(error)})
                    continue
                plan['sites'].append({'module': name, 'address': address, 'bytes': bytes(mod['elf'][at:at+length]).hex(),
                                      'body': replacement.hex(), 'instruction': mnemonic + ' ' + operands,
                                      'rip_fixups': fixups})
            remaining = start + size - decoded_end
            coverage['fully_decoded_functions' if not remaining else 'partial_functions'] += 1
            coverage['undecoded_bytes'] += remaining
            if remaining and len(coverage['partial_samples']) < 20:
                coverage['partial_samples'].append({'address': hex(start), 'size': size, 'undecoded_bytes': remaining})
            if time.monotonic() - report_time > 15:
                print(json.dumps({'stage': 'analyzing_cpu_instructions', 'module': name,
                                  'functions_done': index + 1, 'functions_total': len(functions), 'sites': len(plan['sites'])}), flush=True)
                report_time = time.monotonic()
        plan['coverage'][name] = coverage
        add_short_spans(mod, name, short_candidates, plan)
    target = args.out
    with atomic_output(target) as stream:
        stream.write(json.dumps(plan, separators=(',', ':')).encode('utf-8'))
    print(json.dumps({'stage': 'cpu_plan_ready', 'sites': len(plan['sites']), 'unhandled': len(plan['unhandled']),
                      'counts': dict(collections.Counter(site['instruction'].split()[0] for site in plan['sites'])),
                      'plan': str(target)}), flush=True)


if __name__ == '__main__':
    main()

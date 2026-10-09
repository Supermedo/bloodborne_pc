"""Guarded local Ivy Bridge translations for generated guest images.

Original game files are never changed. The profile replaces the known strlen
implementation and applies a versioned, source-hash-bound translation plan to a
copy of the linked image. It turns on by itself when the CPU lacks BMI1, LZCNT
or MOVBE (Intel 3rd generation and older); legacy_cpu=0/1 in bbport.ini or
BB_LEGACY_CPU=0/1 override that. Analysis dependencies are not needed here.
"""
import bisect
import collections
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import tempfile
from prepare import nid


# SysV ABI: RDI points at a NUL-terminated byte string; RAX returns its length.
# xor eax,eax; loop: cmp byte [rdi+rax],0; je done; inc rax; jmp loop; done: ret.
STRLEN_BASELINE = bytes.fromhex('31c0803c0700740548ffc0ebf5c3')
LIBC_SHA256 = '4378b47f46f1d856824a6f971db1a0c3f41833b77f49d2fba9538f667a139166'
MAX_IMAGE_SIZE = 512 * 1024 * 1024
SUPPORTED = frozenset(('andn', 'bextr', 'blsr', 'blsi', 'blsmsk', 'tzcnt', 'lzcnt', 'movbe'))
# D015 is a closed exception, never a general instruction-stealing facility.
COUNT_SPAN_MODULE_SHA256 = 'd65f0b4f01d59166aed16f8604196d8b7dd805abbf0758b356e8f1354c9429f9'
COUNT_SPAN_FUNCTION = (0x26addc0, 0x26adf49,
                       '16729bb23b2f814ef311ef0a3f3aac2d91f16110a206885eae694f2b17f38133')
COUNT_SPAN_EVIDENCE_REFERENCE = 'docs/evidence/TASK-001/allocator-span-entries.json'
COUNT_SPAN_EVIDENCE_SHA256 = 'c665159c757c6638e1edbf4fc20269720ca9431213a4ba05f7619f4738d30b7b'
COUNT_SPANS = {
    0x26adde5: ('f30fbdc0b920000000', 'lzcnt eax, eax',
                '071663082920f0142ded5530ea5f039c945cf16e24f8c464b51b042db5e50284'),
    0x26addfe: ('f30fbcc883f910', 'tzcnt ecx, eax',
                'e977e8446d3c70387ad8b12424546ebc5c9a0c1d84037ab57f69fb64e150634e'),
    0x26adebe: ('f30fbdfaba1f000000', 'lzcnt edi, edx',
                '165fa7548268a439b5ff621df11cff11578f1ae8fe76ea2d0087c08193f40f53'),
    0x26adf12: ('f30fbdf1b91f000000', 'lzcnt esi, ecx',
                '922a7f4f61bdd53b9cab783faebbd114691319f3c223cc857fe54f920f531745'),
}


def _unique_fields(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f'Legacy CPU duplicate JSON field: {key}.')
        result[key] = value
    return result


def _count_span_tail(site, original, body, image, main, locations, mapped, relocations):
    """Validate the exact reviewed sequence/context; return its literal tail."""
    if set(site) != {'kind', 'module', 'address', 'bytes', 'body', 'instruction', 'rip_fixups', 'span'}:
        raise ValueError('Legacy CPU count span site has missing or unknown fields; return targets are not configurable.')
    address = site['address']
    spec = COUNT_SPANS.get(address)
    if (site['module'] != 'eboot.bin' or not spec or original.hex() != spec[0]
            or site['instruction'] != spec[1]
            or locations['eboot.bin'][2] != COUNT_SPAN_MODULE_SHA256):
        raise ValueError('Legacy CPU count span is outside the closed whitelist.')
    if hashlib.sha256(body).hexdigest() != spec[2]:
        raise ValueError('Legacy CPU count span body is not the reviewed count-only translation.')
    span = site.get('span')
    fields = {'count_length', 'tail_bytes', 'instruction_lengths', 'function_start',
              'function_end', 'function_sha256', 'entry_evidence'}
    if not isinstance(span, dict) or set(span) != fields:
        raise ValueError('Legacy CPU count span metadata is incomplete or has unknown fields.')
    function_start, function_end, function_hash = COUNT_SPAN_FUNCTION
    if (type(span['count_length']) is not int or span['count_length'] != 4
            or type(span['function_start']) is not int or span['function_start'] != function_start
            or type(span['function_end']) is not int or span['function_end'] != function_end
            or span['function_sha256'] != function_hash
            or span['tail_bytes'] != original[4:].hex()
            or type(span['instruction_lengths']) is not list
            or any(type(n) is not int for n in span['instruction_lengths'])
            or span['instruction_lengths'] != [4, len(original) - 4]):
        raise ValueError('Legacy CPU count span sequence/context metadata changed.')
    if (not function_start <= address < address + len(original) <= function_end
            or not mapped(function_start, function_end - function_start, executable=True)
            or hashlib.sha256(image[function_start:function_end]).hexdigest() != function_hash):
        raise ValueError('Legacy CPU count span original function signature does not match.')
    if site.get('rip_fixups', []) != []:
        raise ValueError('Legacy CPU count spans cannot contain RIP fixups.')
    evidence = span['entry_evidence']
    required = {'reference', 'sha256', 'method', 'global_indirect_proof', 'known_entries', 'unresolved_candidates'}
    if (not isinstance(evidence, dict) or set(evidence) != required
            or evidence['reference'] != COUNT_SPAN_EVIDENCE_REFERENCE
            or evidence['sha256'] != COUNT_SPAN_EVIDENCE_SHA256
            or evidence['method'] != 'allocator_count_spans_v1'
            or evidence['global_indirect_proof'] is not False
            or not isinstance(evidence['known_entries'], list)
            or not isinstance(evidence['unresolved_candidates'], list)):
        raise ValueError('Legacy CPU count span entry evidence does not match its reviewed manifest.')
    if evidence['unresolved_candidates']:
        raise ValueError('Legacy CPU count span has unresolved entry candidates.')

    def entry(value):
        target = _uint(value, 'known entry address')
        if target > MAX_IMAGE_SIZE:
            raise ValueError('Legacy CPU count span entry is outside image address limits.')
        if address < target < address + len(original):
            raise ValueError('Legacy CPU count span has a known interior entry.')

    for known in evidence['known_entries']:
        if (not isinstance(known, dict) or set(known) != {'kind', 'address'}
                or known['kind'] not in ('symbol', 'function', 'direct_branch', 'relocation', 'jump_table', 'landing_pad')):
            raise ValueError('Legacy CPU count span known-entry record is invalid.')
        entry(known['address'])
    # Inspect the actual inputs as well as the pinned offline evidence. Linker
    # kind 0 encodes a final guest address in field 2 (not a raw ELF addend).
    for relocation in relocations:
        if relocation[1] == 0:
            if type(relocation[2]) is not int or type(relocation[3]) is not int:
                raise ValueError('Legacy CPU count span resolved relocation address is invalid.')
            entry(relocation[2] + relocation[3])
    for symbol in main.get('symbols', []):
        if symbol.get('section'):
            entry(symbol.get('value'))
    for _, kind, symid, addend in main.get('relocs', []):
        if kind == 8:
            entry(addend)
        elif kind in (1, 6, 7):
            symbols = main.get('symbols', [])
            if not 0 <= symid < len(symbols):
                raise ValueError('Legacy CPU count span ELF relocation symbol is invalid.')
            symbol = symbols[symid]
            if symbol.get('section'):
                entry(symbol['value'] + addend)
    return original[4:], {key: evidence[key] for key in ('reference', 'sha256', 'method', 'global_indirect_proof')}


@contextmanager
def atomic_output(path):
    """Publish a complete generated artifact, preserving any older file on error."""
    path = Path(path)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode='wb', prefix=path.name + '.', suffix='.tmp',
                                         dir=path.parent, delete=False) as stream:
            temporary = Path(stream.name)
            yield stream
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


# CPUID (leaf, register index in eax/ebx/ecx/edx, bit) of the extensions the plan translates.
HOST_EXTENSIONS = {'BMI1': (7, 1, 3), 'LZCNT': (0x80000001, 2, 5), 'MOVBE': (1, 2, 22)}
# push rbx; mov eax,ecx; mov ecx,edx; cpuid; mov [r8],eax; mov [r8+4],ebx; mov [r8+8],ecx;
# mov [r8+12],edx; pop rbx; ret  (Windows x64: leaf in ecx, subleaf in edx, output in r8)
CPUID_CODE = bytes.fromhex('53' '89c8' '89d1' '0fa2' '418900' '41895804' '41894808' '4189500c' '5b' 'c3')


def _cpuid_windows():
    """A cpuid(leaf, subleaf) -> (eax, ebx, ecx, edx) callable and its cleanup, or None."""
    import ctypes
    k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    k32.VirtualAlloc.restype = ctypes.c_void_p
    k32.VirtualAlloc.argtypes = (ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32, ctypes.c_uint32)
    k32.VirtualFree.argtypes = (ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32)
    memory = k32.VirtualAlloc(None, len(CPUID_CODE), 0x3000, 0x40)  # MEM_COMMIT|MEM_RESERVE, RWX
    if not memory:
        return None
    ctypes.memmove(memory, CPUID_CODE, len(CPUID_CODE))
    function = ctypes.CFUNCTYPE(None, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p)(memory)

    def cpuid(leaf, subleaf=0):
        registers = (ctypes.c_uint32 * 4)()
        function(leaf, subleaf, ctypes.addressof(registers))
        return tuple(registers)
    return cpuid, lambda: k32.VirtualFree(memory, 0, 0x8000)


def missing_host_extensions(cpuid=None):
    """Names of HOST_EXTENSIONS this CPU lacks, or None when CPUID cannot be read here."""
    cleanup = None
    if cpuid is None:
        if os.name != 'nt' or struct.calcsize('P') != 8:
            return None
        try:
            found = _cpuid_windows()
        except (OSError, AttributeError, ValueError):
            return None
        if not found:
            return None
        cpuid, cleanup = found
    try:
        top = cpuid(0)[0]
        extended = cpuid(0x80000000)[0]
        missing = []
        for name, (leaf, register, bit) in HOST_EXTENSIONS.items():
            available = top if leaf < 0x80000000 else extended
            if leaf > available or not cpuid(leaf)[register] >> bit & 1:
                missing.append(name)
        return missing
    finally:
        if cleanup:
            cleanup()


def enabled():
    override = os.environ.get('BB_LEGACY_CPU')
    if override is not None:
        return override == '1'
    config = Path(os.environ.get('BB_CONFIG', Path(__file__).resolve().parent.parent / 'bbport.ini'))
    try:
        for line in config.read_text(encoding='utf-8').splitlines():
            if line.strip() in ('legacy_cpu=0', 'legacy_cpu=1'):
                return line.strip() == 'legacy_cpu=1'
    except OSError:
        pass
    # Unknown CPU (no CPUID): translate. The translations are equivalent on any x86-64 CPU,
    # while a CPU without the extensions closes the game on the first such instruction.
    missing = missing_host_extensions()
    return missing is None or bool(missing)


def partial_allowed():
    """The incomplete profile runs by default (an Ivy Bridge CPU cannot start the game without
    it); BB_LEGACY_CPU_ALLOW_PARTIAL=0 restores the refusal."""
    return os.environ.get('BB_LEGACY_CPU_ALLOW_PARTIAL', '1') == '1'


def patch_libc(module):
    if not enabled():
        return []
    if module['sha256'] != LIBC_SHA256:
        raise ValueError('Legacy CPU profile expects the inspected libc dump; its SHA256 changed.')
    matches = [symbol for symbol in module['symbols'] if symbol['section'] and symbol['type'] == 2
               and symbol['name'].split('#')[0] == nid('strlen')]
    if len(matches) != 1:
        raise ValueError('Legacy CPU profile could not uniquely identify strlen.')
    symbol = matches[0]
    segment = next(part for part in module['ph'] if part['type'] == 1 and part['flags'] & 1
                   and part['vaddr'] <= symbol['value']
                   and symbol['value'] + symbol['size'] <= part['vaddr'] + part['filesz'])
    offset = segment['offset'] + symbol['value'] - segment['vaddr']
    original = bytes(module['elf'][offset:offset + symbol['size']])
    if symbol['value'] != 0x2f970 or symbol['size'] != 224 or original[24:29] != bytes.fromhex('c4e2f8f2c6'):
        raise ValueError('Legacy CPU strlen signature does not match the inspected implementation.')
    module['elf'][offset:offset + symbol['size']] = STRLEN_BASELINE + b'\x90' * (symbol['size'] - len(STRLEN_BASELINE))
    report = {'function': 'strlen', 'address': hex(symbol['value']), 'original_sha256': hashlib.sha256(original).hexdigest(),
              'replacement': 'baseline x86-64, no BMI1'}
    print('CPU compatibility: replaced BMI1 strlen in the generated libc image')
    return [report]


def _uint(value, label):
    if type(value) is not int or value < 0:
        raise ValueError(f'Legacy CPU {label} must be a nonnegative integer.')
    return value


def _hex_bytes(value, label, minimum, maximum):
    if not isinstance(value, str) or len(value) % 2 or not re.fullmatch('[0-9a-fA-F]+', value):
        raise ValueError(f'Legacy CPU {label} must contain hexadecimal bytes.')
    data = bytes.fromhex(value)
    if not minimum <= len(data) <= maximum:
        raise ValueError(f'Legacy CPU {label} has an invalid length.')
    return data


def _relative(target, next_address, label):
    displacement = target - next_address
    if not -(1 << 31) <= displacement < (1 << 31):
        raise ValueError(f'Legacy CPU {label}: relative displacement is out of range.')
    return struct.pack('<i', displacement)


def patch_image(image, segments, main, modules, relocations, *, plan=None, active=None):
    """Return a translated copy of a linked image; never mutate caller inputs.

    Plan v1 contains register-only bodies. V2 additionally allows RIP-relative
    operand fixups, each giving its displacement offset, next instruction offset
    and module-relative target. V3 permits only the four D015 count spans.
    No disassembler/assembler is needed at runtime.
    All active-profile guards are checked before publishing anything. Disabling
    the profile does not load a plan or change image bytes/segment metadata.
    """
    is_active = enabled() if active is None else active
    if type(is_active) is not bool:
        raise ValueError('Legacy CPU active must be a boolean.')
    report = dict(schema_version=1, enabled=is_active, source_hashes={}, plan_sha256=None,
                  patched_sites=0, counts_by_instruction={}, trampoline_bytes=0,
                  unhandled_count=0, unhandled_by_reason={}, unhandled_by_instruction={}, libc=[],
                  complete_coverage=False, experimental=is_active, semantic_risk_sites=0,
                  plan_version=None, span_patches=0, span_sites=[], spans_experimental=False,
                  span_entry_safety='not_applicable', span_entry_evidence=[], span_execution_allowed=False)
    if not is_active:
        return bytearray(image), list(segments), report
    if not isinstance(image, (bytes, bytearray)) or not 0 < len(image) <= MAX_IMAGE_SIZE:
        raise ValueError('Legacy CPU input image has an invalid size/type.')
    try:
        if plan is None:
            path = Path(__file__).resolve().parent.parent / 'tools/legacy-cpu-plan.json'
            raw_plan = path.read_bytes()
            plan = json.loads(raw_plan, object_pairs_hook=_unique_fields)
        else:
            raw_plan = json.dumps(plan, sort_keys=True, separators=(',', ':')).encode('utf-8')
    except (OSError, UnicodeError, TypeError, json.JSONDecodeError) as error:
        raise ValueError(f'Legacy CPU plan could not be loaded: {error}') from error
    if not isinstance(plan, dict) or type(plan.get('version')) is not int or plan['version'] not in (1, 2, 3):
        raise ValueError('Legacy CPU plan version must be 1, 2 or 3.')
    report['plan_version'] = plan['version']
    if not isinstance(plan.get('modules'), dict) or not isinstance(plan.get('sites'), list):
        raise ValueError('Legacy CPU plan requires modules and sites.')
    if not isinstance(plan.get('unhandled'), list):
        raise ValueError('Legacy CPU plan requires an explicit unhandled list.')

    parts = []
    for segment in segments:
        if not isinstance(segment, (list, tuple)) or len(segment) != 3:
            raise ValueError('Legacy CPU invalid segment record.')
        start, size, flags = (_uint(value, 'segment field') for value in segment)
        if not size or start + size > len(image):
            raise ValueError('Legacy CPU segment is outside the image.')
        parts.append((start, size, flags))
    for left, right in zip(sorted(parts), sorted(parts)[1:]):
        if left[0] + left[1] > right[0]:
            raise ValueError('Legacy CPU input segments overlap.')

    def mapped(start, size, executable=False):
        return any(p <= start and start + size <= p + n and (not executable or flags & 1)
                   for p, n, flags in parts)

    locations = {}
    for item in modules:
        if not isinstance(item, dict) or not isinstance(item.get('file'), str):
            raise ValueError('Legacy CPU invalid module metadata.')
        name = item['file']
        if name == 'eboot.bin' or name in locations or '/' in name or '\\' in name:
            raise ValueError('Legacy CPU duplicate/invalid module name.')
        start, size = _uint(item.get('base'), 'module base'), _uint(item.get('size'), 'module size')
        if not size or start + size > len(image):
            raise ValueError(f'Legacy CPU {name}: module is outside the image.')
        locations[name] = (start, size, item.get('sha256'))
    # The original eboot ends before the first appended module (alignment gap is
    # harmless: site validation below also requires an executable load segment).
    main_size = min((item[0] for item in locations.values()), default=len(image))
    locations['eboot.bin'] = (0, main_size, main.get('sha256'))
    occupied = sorted((base, base + size) for base, size, _ in locations.values())
    if any(a[1] > b[0] for a, b in zip(occupied, occupied[1:])):
        raise ValueError('Legacy CPU input modules overlap.')
    if set(plan['modules']) != set(locations):
        raise ValueError('Legacy CPU plan module set does not match the linked image.')
    for name, (_, _, digest) in locations.items():
        if not isinstance(digest, str) or not re.fullmatch('[0-9a-f]{64}', digest):
            raise ValueError(f'Legacy CPU {name}: source SHA256 is invalid.')
        if plan['modules'][name] != digest:
            raise ValueError(f'Legacy CPU {name}: source SHA256 does not match the plan.')
    report['source_hashes'] = {name: values[2] for name, values in locations.items()}
    report['plan_sha256'] = hashlib.sha256(raw_plan).hexdigest()

    relocation_targets = []
    for relocation in relocations:
        if not isinstance(relocation, (list, tuple)) or len(relocation) != 4:
            raise ValueError('Legacy CPU invalid relocation record.')
        target = _uint(relocation[0], 'relocation target')
        if target + 8 > len(image):
            raise ValueError('Legacy CPU relocation is outside the image.')
        relocation_targets.append(target)
    relocation_targets.sort()

    arena = (len(image) + 65535) & ~65535
    cursor = arena
    operations = []
    span_reports = []
    span_evidence = []
    counts = collections.Counter()
    for site in plan['sites']:
        if not isinstance(site, dict) or site.get('module') not in locations:
            raise ValueError('Legacy CPU site references an unknown module.')
        name = site['module']
        address = _uint(site.get('address'), f'{name} site address')
        label = f'{name}+{address:#x}'
        original = _hex_bytes(site.get('bytes'), label + ' original bytes', 5, 15)
        body = bytearray(_hex_bytes(site.get('body'), label + ' body', 1, 4096))
        instruction = site.get('instruction')
        if not isinstance(instruction, str) or instruction.split(' ', 1)[0] not in SUPPORTED:
            raise ValueError(f'Legacy CPU {label}: unsupported instruction description.')
        kind = site.get('kind', 'instruction')
        if kind not in ('instruction', 'count_span'):
            raise ValueError(f'Legacy CPU {label}: unsupported site kind.')
        is_span = kind == 'count_span'
        if is_span and plan['version'] != 3:
            raise ValueError(f'Legacy CPU {label}: count spans require plan version 3.')
        # F3 0F BC/BD register count without REX is exactly four bytes. A
        # longer record cannot be smuggled in as a legacy single instruction.
        short_count_prefix = (original[:3] in (b'\xf3\x0f\xbc', b'\xf3\x0f\xbd')
                              and original[3] & 0xc0 == 0xc0)
        if not is_span and ('span' in site or short_count_prefix):
            raise ValueError(f'Legacy CPU {label}: count span cannot be treated as a single instruction.')
        base, size, _ = locations[name]
        start = base + address
        end = start + len(original)
        if address + len(original) > size or not mapped(start, len(original), executable=True):
            raise ValueError(f'Legacy CPU {label}: site is outside its executable module segment.')
        if image[start:end] != original:
            raise ValueError(f'Legacy CPU {label}: original instruction bytes do not match.')
        index = bisect.bisect_left(relocation_targets, start - 7)
        if index < len(relocation_targets) and relocation_targets[index] < end:
            raise ValueError(f'Legacy CPU {label}: site overlaps a relocation.')
        if is_span:
            tail, evidence = _count_span_tail(site, original, body, image, main, locations, mapped, relocations)
            body.extend(tail)
            if evidence not in span_evidence:
                span_evidence.append(evidence)
        cursor = (cursor + 15) & ~15
        fixups = site.get('rip_fixups', [])
        if not isinstance(fixups, list) or (fixups and plan['version'] < 2):
            raise ValueError(f'Legacy CPU {label}: RIP fixups require plan version 2.')
        fixed_bytes = set()
        for fixup in fixups:
            if not isinstance(fixup, dict):
                raise ValueError(f'Legacy CPU {label}: invalid RIP fixup.')
            offset = _uint(fixup.get('offset'), 'RIP displacement offset')
            following = _uint(fixup.get('next_offset'), 'RIP next instruction offset')
            target = _uint(fixup.get('target'), 'RIP target')
            if not offset + 4 <= following <= len(body) or offset + 4 > len(body):
                raise ValueError(f'Legacy CPU {label}: RIP fixup is outside the body.')
            if target >= size or not mapped(base + target, 1):
                raise ValueError(f'Legacy CPU {label}: RIP target is outside mapped module data.')
            if fixed_bytes.intersection(range(offset, offset + 4)):
                raise ValueError(f'Legacy CPU {label}: RIP fixups overlap.')
            fixed_bytes.update(range(offset, offset + 4))
            body[offset:offset + 4] = _relative(base + target, cursor + following, label)
        go = b'\xe9' + _relative(cursor, start + 5, label)
        back = b'\xe9' + _relative(end, cursor + len(body) + 5, label)
        operations.append((start, end, cursor, bytes(body) + back, go + b'\x90' * (len(original) - 5)))
        if is_span:
            span_reports.append(dict(module=name, address=address, image_start=start, image_end=end,
                                     replacement_bytes=operations[-1][4].hex(), trampoline_start=cursor,
                                     trampoline_size=len(body) + 5,
                                     trampoline_sha256=hashlib.sha256(operations[-1][3]).hexdigest()))
        cursor += len(body) + 5
        if cursor > MAX_IMAGE_SIZE:
            raise ValueError('Legacy CPU translated image exceeds the probe limit.')
        counts[instruction.split(' ', 1)[0]] += 1
    by_address = sorted(operations)
    if any(left[1] > right[0] for left, right in zip(by_address, by_address[1:])):
        raise ValueError('Legacy CPU instruction sites overlap or are duplicated.')
    for item in plan['unhandled']:
        if not isinstance(item, dict) or item.get('module') not in locations or not isinstance(item.get('reason'), str):
            raise ValueError('Legacy CPU invalid unhandled instruction report.')
        if not isinstance(item.get('instruction'), str):
            raise ValueError('Legacy CPU unhandled site has no instruction description.')
    semantic_risk = sum(item['instruction'].split(' ', 1)[0] in ('lzcnt', 'tzcnt')
                        for item in plan['unhandled'])
    partial = partial_allowed()
    if span_reports and not partial:
        raise ValueError('Legacy CPU count spans are experimental: global indirect entries are unproven. '
                         'BB_LEGACY_CPU_ALLOW_PARTIAL=1 (the default) runs them.')
    if semantic_risk and not partial:
        raise ValueError(f'Legacy CPU profile is incomplete: {semantic_risk} LZCNT/TZCNT sites remain; '
                         'this CPU may execute them with incorrect BSR/BSF semantics. '
                         'BB_LEGACY_CPU_ALLOW_PARTIAL=1 (the default) runs them.')
    report.update(patched_sites=len(operations), counts_by_instruction=dict(counts),
                  trampoline_bytes=cursor - arena, unhandled_count=len(plan['unhandled']),
                  unhandled_by_reason=dict(collections.Counter(x['reason'] for x in plan['unhandled'])),
                  unhandled_by_instruction=dict(collections.Counter(x['instruction'].split(' ', 1)[0]
                                                                        for x in plan['unhandled'])),
                  coverage=plan.get('coverage', {}), semantic_risk_sites=semantic_risk,
                  partial_execution_allowed=partial,
                  span_patches=len(span_reports), span_sites=span_reports, spans_experimental=bool(span_reports),
                  span_entry_safety='known_entries_checked_global_indirect_unproven' if span_reports else 'not_applicable',
                  span_entry_evidence=span_evidence,
                  span_execution_allowed=bool(span_reports) and partial)
    result = bytearray(image)
    new_segments = list(segments)
    if operations:
        padded_end = (cursor + 4095) & ~4095
        if padded_end > MAX_IMAGE_SIZE:
            raise ValueError('Legacy CPU aligned image exceeds the probe limit.')
        result.extend(bytes(padded_end - len(result)))
        for start, end, target, body, jump in operations:
            result[start:end] = jump
            result[target:target + len(body)] = body
        new_segments.append((arena, padded_end - arena, 5))
    return result, new_segments, report

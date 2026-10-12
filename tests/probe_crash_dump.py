"""Verify a real access violation retains its exception and modules in a Windows minidump."""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from test_probe import package


def dump_streams(data):
    signature, _, count, directory_rva = struct.unpack_from('<4I', data)
    assert signature == 0x504d444d, hex(signature)
    return {kind: (size, rva) for kind, size, rva in
        (struct.unpack_from('<3I', data, directory_rva + i*12) for i in range(count))}


def memory_at(data, streams, address, length):
    _, rva = streams[5]
    count = struct.unpack_from('<I', data, rva)[0]
    for i in range(count):
        start, size, offset = struct.unpack_from('<QII', data, rva+4+i*16)
        if start <= address and address+length <= start+size:
            return data[offset+address-start:offset+address-start+length]
    return None


with tempfile.TemporaryDirectory(prefix='bbport краш проверка ') as directory:
    work = Path(directory)
    image = work / 'boot.bin'
    # xor eax,eax; mov dword ptr [rax],1 (guest access violation, no unwind metadata).
    image.write_bytes(package(bytes.fromhex('31c0c70001000000c3'), [], []))
    env = {**os.environ, 'BB_CRASH_DIR': str(work), 'BB_CRASH_DUMP': '1'}
    result = subprocess.run([sys.argv[1], str(image), '--cpu-only'], env=env,
        capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=30)
    print(result.stdout)
    print(result.stderr, file=sys.stderr)
    assert result.returncode == 3, result.returncode
    assert 'Runtime build: PE timestamp=' in result.stdout
    dumps = list(work.glob('bb-crash-*.dmp'))
    assert len(dumps) == 1, dumps
    data = dumps[0].read_bytes()
    streams = dump_streams(data)
    assert 3 in streams and 4 in streams and 6 in streams  # threads, modules, exception
    _, exception_rva = streams[6]
    code = struct.unpack_from('<I', data, exception_rva + 8)[0]
    address = struct.unpack_from('<Q', data, exception_rva + 24)[0]
    assert code == 0xc0000005, hex(code)
    assert address == 0x800000002, hex(address)
    assert 'Crash dump:' in result.stderr
    print('PASS: original exception, threads, modules and Unicode dump path')
    # Diagnostics must preserve exit 3 even when disabled or unable to create the file.
    for extra, expected in [({'BB_CRASH_DUMP': '0'}, None),
            ({'BB_CRASH_DIR': str(work / 'missing' / 'child')}, 'Crash dump: failed')]:
        retry = subprocess.run([sys.argv[1], str(image), '--cpu-only'], env={**env, **extra},
            capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=20)
        assert retry.returncode == 3, retry.returncode
        assert len(list(work.rglob('*.dmp'))) == 1
        if expected:
            assert expected in retry.stderr, retry.stderr
        else:
            assert 'Crash dump:' not in retry.stderr
    print('PASS: disabled and failed dump writes retain the original exit code')

    # Reproduce the reported failure's exact access: null float array, index 2.
    # Keep its guest caller far outside DbgHelp's automatic 256-byte PC window,
    # and a pointed-to object in a separate RW segment, with an indirect field
    # in the middle of its page. No real game files are needed for this check.
    entry = bytes.fromhex('55 4889e5 488d1d f5bf0000 e8 f05f0000 0f0b')
    fault = bytes.fromhex('55 4889e5 31c9 b802000000 c5f857c0 c5f82e0481')
    marker = b'BBPORT_INDIRECT_OBJECT_TEST'
    guest = bytearray(0x10000)
    guest[:len(entry)] = entry
    guest[0x6000:0x6000+len(fault)] = fault
    guest[0xe080:0xe080+len(marker)] = marker
    # The root object's field points to the second object. Relocation makes
    # this independent of the guest mapping address selected by the loader.
    boot = (struct.pack('<8s5Q', b'BBPROBE1', len(guest), 0, 2, 1, 0)
        + struct.pack('<6Q', 0, 0x7000, 5, 0xc000, 0x4000, 6)
        + struct.pack('<4Q', 0xc330, 0, 0xe080, 0) + guest)
    image.write_bytes(boot)
    result = subprocess.run([sys.argv[1], str(image), '--cpu-only'], env=env,
        capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=30)
    assert result.returncode == 3, result.stdout+result.stderr
    current = [p for p in work.glob('bb-crash-*.dmp') if p != dumps[0]]
    assert len(current) == 1, current
    data = current[0].read_bytes()
    streams = dump_streams(data)
    _, rva = streams[6]
    pc = struct.unpack_from('<Q', data, rva+24)[0]
    base = pc-(0x6000+len(fault)-5)
    assert struct.unpack_from('<Q', data, rva+48)[0] == 8  # attempted read address
    assert memory_at(data, streams, base, len(entry)) == entry
    assert memory_at(data, streams, pc-15, len(fault)) == fault
    assert memory_at(data, streams, base+0xc330, 8) == struct.pack('<Q', base+0xe080)
    assert memory_at(data, streams, base+0xe080, len(marker)) == marker
    assert 16 in streams  # VirtualQuery memory map, not a full-memory dump
    size, rva = streams[10]  # CommentStreamA: guest base, size and hard budget
    description = data[rva:rva+size].decode('ascii').rstrip('\0')
    assert description.startswith('BBPORT_GUEST_CONTEXT_V1\n'), description
    details = dict(line.split('=', 1) for line in description.splitlines()[1:])
    assert int(details['image_base'], 16) == base
    assert int(details['image_size'], 16) == len(guest)
    assert int(details['extra_pages'])*int(details['page_size']) <= int(details['extra_limit']) == 2*1024*1024
    assert 0 < int(details['caller_frames']) <= 32
    assert len(data) < 4*1024*1024, len(data)
    assert memory_at(data, streams, base+0xb000, 1) is None  # inaccessible gap stays omitted
    print('PASS: null-array fault retains distant guest caller, root/indirect objects and bounded memory map')

    # A corrupt frame pointer and a root page containing hundreds of valid
    # pointers must still produce a bounded dump, without following the heap
    # recursively or touching inaccessible gaps.
    large = bytearray(0x211000)
    large[:len(entry)] = entry
    corrupt_frame = fault[:-5]+bytes.fromhex('48c7c5 f0ffffff')+fault[-5:]
    large[0x6000:0x6000+len(corrupt_frame)] = corrupt_frame
    relocs = b''.join(struct.pack('<4Q', 0xc000+i*8, 0, 0x10000+i*4096, 0) for i in range(512))
    image.write_bytes(struct.pack('<8s5Q', b'BBPROBE1', len(large), 0, 2, 512, 0)
        + struct.pack('<6Q', 0, 0x7000, 5, 0xc000, len(large)-0xc000, 6) + relocs + large)
    previous = set(work.glob('*.dmp'))
    result = subprocess.run([sys.argv[1], str(image), '--cpu-only'], env=env,
        capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=30)
    assert result.returncode == 3, result.stdout+result.stderr
    current = set(work.glob('*.dmp'))-previous
    assert len(current) == 1, current
    data = current.pop().read_bytes()
    streams = dump_streams(data)
    size, rva = streams[10]
    description = data[rva:rva+size].decode('ascii').rstrip('\0')
    details = dict(line.split('=', 1) for line in description.splitlines()[1:])
    assert int(details['extra_pages'])*int(details['page_size']) == int(details['extra_limit']) == 2*1024*1024
    assert len(data) < 4*1024*1024, len(data)
    print('PASS: corrupt frame chain and dense pointer page retain exception within the 2 MiB extra budget')

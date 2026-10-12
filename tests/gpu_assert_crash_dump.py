"""GPU fatal assertions retain exit 23 and write an assertion-context minidump."""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix='bbport GPU assertion краш ') as directory:
    work = Path(directory)
    env = {**os.environ, 'BB_CRASH_DIR': str(work), 'BB_CRASH_DUMP': '1'}
    for mode in ('main', 'worker', 'concurrent'):
        previous = set(work.glob('*.dmp'))
        result = subprocess.run([sys.argv[1], mode], env=env, capture_output=True,
                                text=True, encoding='utf-8', errors='replace', timeout=25)
        assert result.returncode == 23, result.stdout + result.stderr
        assert 'STOP: GPU library assertion failed' in result.stderr
        current = set(work.glob('*.dmp')) - previous
        assert len(current) == 1, (mode, current, result.stderr)
        data = current.pop().read_bytes()
        signature, _, count, directory_rva = struct.unpack_from('<4I', data)
        assert signature == 0x504d444d
        streams = {kind: (size, rva) for kind, size, rva in
                   (struct.unpack_from('<3I', data, directory_rva + i*12) for i in range(count))}
        assert 3 in streams and 4 in streams and 6 in streams
        _, exception_rva = streams[6]
        thread_id = struct.unpack_from('<I', data, exception_rva)[0]
        code = struct.unpack_from('<I', data, exception_rva+8)[0]
        address = struct.unpack_from('<Q', data, exception_rva+24)[0]
        assert code == 0xe0424201, hex(code)  # explicit port assertion marker, not a CPU fault
        assert address and thread_id
        _, thread_rva = streams[3]
        thread_count = struct.unpack_from('<I', data, thread_rva)[0]
        thread_ids = [struct.unpack_from('<I', data, thread_rva+4+i*48)[0] for i in range(thread_count)]
        assert thread_id in thread_ids
        size, rva = streams[10]
        assert b'reason=GPU library assertion failed\n' in data[rva:rva+size]
        assert 'Crash dump:' in result.stderr and 'timed out' not in result.stderr
        print(f'PASS: {mode} GPU assertion, original exit 23, one valid minidump, fault thread and reason')
    previous = set(work.glob('*.dmp'))
    for extra, expected in [({'BB_CRASH_DUMP': '0'}, None),
                            ({'BB_CRASH_DIR': str(work/'missing'/'child')}, 'Crash dump: failed')]:
        result = subprocess.run([sys.argv[1], 'worker'], env={**env, **extra}, capture_output=True,
                                text=True, encoding='utf-8', errors='replace', timeout=25)
        assert result.returncode == 23, result.stdout + result.stderr
        assert set(work.glob('*.dmp')) == previous
        if expected:
            assert expected in result.stderr, result.stderr
        else:
            assert 'Crash dump:' not in result.stderr
    print('PASS: disabled/unwritable assertion dumps retain exit 23')

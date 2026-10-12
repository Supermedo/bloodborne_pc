"""Real fail-fast/foreign faults: one external dump with the original Windows status."""
import os
import shutil
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix='bbport monitor краш ') as directory:
    work = Path(directory)
    env = {**os.environ, 'BB_CRASH_DIR': str(work), 'BB_CRASH_DUMP': '1'}
    monitor, target = sys.argv[1:3]
    local_target = work / 'клиент с пробелами.exe'
    shutil.copy2(target, local_target)
    target = str(local_target)
    for mode, expected in [('failfast', 0xc0000409), ('worker', 0xc0000409),
                           ('fastfail-intrinsic', 0xc0000409), ('av', 0xc0000005)]:
        previous = set(work.glob('*.dmp'))
        result = subprocess.run([monitor, target, mode], env=env, capture_output=True,
                                text=True, encoding='utf-8', errors='replace', timeout=30)
        assert result.returncode == expected, (mode, result.returncode, result.stdout, result.stderr)
        current = set(work.glob('*.dmp')) - previous
        assert len(current) == 1, (mode, current, result.stderr)
        data = current.pop().read_bytes()
        signature, _, count, rva = struct.unpack_from('<4I', data)
        assert signature == 0x504d444d
        streams = {kind: (size, offset) for kind, size, offset in
                   (struct.unpack_from('<3I', data, rva+i*12) for i in range(count))}
        assert 3 in streams and 4 in streams and 6 in streams and 16 in streams
        _, rva = streams[6]
        tid, code, address = struct.unpack_from('<I', data, rva)[0], struct.unpack_from('<I', data, rva+8)[0], struct.unpack_from('<Q', data, rva+24)[0]
        assert tid and code == expected and address
        _, thread_rva = streams[3]
        threads = struct.unpack_from('<I', data, thread_rva)[0]
        assert tid in [struct.unpack_from('<I', data, thread_rva+4+i*48)[0] for i in range(threads)]
        _, module_rva = streams[4]
        modules = struct.unpack_from('<I', data, module_rva)[0]
        module_names = []
        for i in range(modules):
            name_rva = struct.unpack_from('<I', data, module_rva+4+i*108+20)[0]
            name_size = struct.unpack_from('<I', data, name_rva)[0]
            module_names.append(data[name_rva+4:name_rva+4+name_size].decode('utf-16-le'))
        assert any(name.endswith(local_target.name) for name in module_names), module_names
        assert not any(name.endswith(Path(monitor).name) for name in module_names)
        size, rva = streams[10]
        assert b'reason=External crash monitor: unhandled Windows exception' in data[rva:rva+size]
        assert len(data) < 4*1024*1024 and 'Crash dump:' in result.stderr
        print('PASS: external', mode, hex(code), 'original thread/context and bounded dump')
    previous = set(work.glob('*.dmp'))
    for mode in ('handled', 'clean'):
        result = subprocess.run([monitor, target, mode], env=env, capture_output=True, timeout=20)
        assert result.returncode == 7, (result.returncode, result.stdout, result.stderr)
        assert set(work.glob('*.dmp')) == previous
    args = ['', 'кириллица space', 'C:\\folder name\\', 'quote"slash\\"']
    result = subprocess.run([monitor, target, 'argv', *args], env=env, capture_output=True,
                            text=True, encoding='utf-8', errors='replace', timeout=20)
    assert result.returncode == 0, result.stderr
    assert [line[4:] for line in result.stdout.splitlines() if line.startswith('arg=')] == args
    for extra, message in [({'BB_CRASH_DUMP': '0'}, None),
                           ({'BB_CRASH_DIR': str(work/'missing'/'child')}, 'Crash dump: failed')]:
        result = subprocess.run([monitor, target, 'failfast'], env={**env, **extra}, capture_output=True,
                                text=True, encoding='utf-8', errors='replace', timeout=25)
        assert result.returncode == 0xc0000409
        assert set(work.glob('*.dmp')) == previous
        assert message in result.stderr if message else 'Crash dump:' not in result.stderr
    print('PASS: handled exceptions, clean exit, Unicode/empty/quoted arguments, disabled/failed dump writes')

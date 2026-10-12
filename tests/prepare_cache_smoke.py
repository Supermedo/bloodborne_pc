"""Run real preparation/linking and reuse inside Python or packaged Bloodborne.exe.

Uses generated SELF/SFO fixtures only; no game files. Optional --benchmark creates
a large sparse guest image to measure the launch preparation component.
"""
from pathlib import Path
import json
import os
import struct
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))
from prepare_cache import OUTPUTS, digest, prepare_game
from patches import eboot_segments


def self_fixture(library, *, image_bytes=64):
    data = bytearray(0x500)
    data[:4] = b'O\x15=\x1d'
    struct.pack_into('<H', data, 24, 2)
    struct.pack_into('<4Q', data, 32, 0x800, 0x200, 16, 16)
    struct.pack_into('<4Q', data, 64, 0x100800, 0x300, 512, 512)
    struct.pack_into('<16sHHIQQQIHHHHHH', data, 96,
                     b'\x7fELF\x02\x01\x01\x09', 0xfe18, 62, 1, 0, 64, 0, 0,
                     64, 56, 5, 0, 0, 0)
    for i, ph in enumerate(((1, 5, 0x1000, 0, 0, 16, image_bytes, 0x1000),
                            (0x61000000, 4, 0x2000, 0, 0, 512, 0, 16),
                            (2, 4, 0x2100, 0, 0, 192, 192, 8),
                            (0x61000001, 4, 0, 16, 0, 0, 0, 8),
                            (7, 4, 0x1008, 8, 0, 4, 16, 16))):
        struct.pack_into('<II6Q', data, 160 + 56 * i, *ph)
    data[0x200] = 0xc3
    names = b'\0bzQExy189ZI#C#A\0' + library.encode() + b'\0'
    liboff = len(b'\0bzQExy189ZI#C#A\0')
    data[0x300:0x300 + len(names)] = names
    struct.pack_into('<IBBHQQ', data, 0x340 + 24, 1, 0x12, 0, 1, 0, 1)
    tags = ((0x61000035, 0), (0x61000037, 64), (0x61000039, 64),
            (0x6100003f, 48), (0x61000029, 128), (0x6100002d, 0),
            (0x6100002f, 128), (0x61000031, 0),
            (0x61000013, (2 << 48) | (1 << 32) | liboff),
            (0x6100000d, (1 << 32) | liboff), (12, 0), (0, 0))
    for i, tag in enumerate(tags):
        struct.pack_into('<2Q', data, 0x400 + 16 * i, *tag)
    return data


def write_fixture(game, image_bytes=64):
    (game / 'sce_module').mkdir(parents=True)
    (game / 'sce_sys').mkdir()
    for name, library, size in (('eboot.bin', 'main', image_bytes),
                                ('sce_module/libc.prx', 'libc', 64),
                                ('sce_module/libSceFios2.prx', 'libSceFios2', 64)):
        (game / name).write_bytes(self_fixture(library, image_bytes=size))
    (game / 'sce_sys/param.sfo').write_bytes(struct.pack('<4s4I', b'\0PSF', 0x101, 20, 20, 0))


def main():
    benchmark = '--benchmark' in sys.argv
    with tempfile.TemporaryDirectory(prefix='bb preparation smoke ') as directory:
        base = Path(directory)
        game, out = base / 'game', base / 'out'
        write_fixture(game, 91 * 1024 * 1024 if benchmark else 64)
        env = dict(os.environ, BB_SKIP_GAME_CHECK='1')  # synthetic executable only
        calls = []

        def runner(name, *args):
            calls.append(name)
            command = [sys.executable]
            if getattr(sys, 'frozen', False):
                command.append('--script')
            command.extend([str(ROOT / 'scripts' / name), *map(str, args)])
            result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=60)
            assert result.returncode == 0, (command, result.stdout, result.stderr)

        if benchmark:
            old, cold, warm = [], [], []
            for _ in range(5):
                start = time.perf_counter()
                runner('prepare.py', game, '--out', out)
                runner('link_libc.py', game, '--out', out)
                runner('link_modules.py', game, '--out', out)
                old.append((time.perf_counter() - start) * 1000)
                (out / 'prepare-cache.json').unlink(missing_ok=True)
                start = time.perf_counter()
                prepare_game(game, out, runner, env=env)
                cold.append((time.perf_counter() - start) * 1000)
            prepare_game(game, out, runner, env=env)
            for _ in range(5):
                start = time.perf_counter()
                assert prepare_game(game, out, runner, env=env)
                warm.append((time.perf_counter() - start) * 1000)
            print(json.dumps(dict(synthetic_image_MiB=91, old_ms=old, rebuild_ms=cold,
                                  hit_ms=warm, medians_ms=[sorted(values)[2] for values in (old, cold, warm)])))
            return
        assert not prepare_game(game, out, runner, env=env)
        assert calls == ['prepare.py', 'link_modules.py']
        assert (out / 'boot.bin').read_bytes()[:8] == b'BBPROBE2'
        assert (out / 'boot-linked.bin').read_bytes()[:8] == b'BBPROBE5'
        assert eboot_segments((out / 'eboot-headers.bin').read_bytes()) == eboot_segments((out / 'eboot.elf').read_bytes())
        assert not json.loads((out / 'analysis.json').read_text())['resource_inventory']
        before = {name: digest(out / name) for name in OUTPUTS}
        before_boot = digest(out / 'boot.bin')
        calls.clear()
        assert prepare_game(game, out, runner, env=env) and not calls
        # Same binary output as the original complete preparation, including libc linking.
        runner('prepare.py', game, '--out', out)
        runner('link_libc.py', game, '--out', out)
        runner('link_modules.py', game, '--out', out)
        assert digest(out / 'boot-linked.bin') == before['boot-linked.bin']
        assert digest(out / 'boot.bin') == before_boot
        assert prepare_game(game, out, runner, env=env)  # diagnostic inventory is not runtime input
        (out / 'boot.bin').unlink()  # not consumed by a verified image hit
        (out / 'eboot.elf').unlink()
        assert prepare_game(game, out, runner, env=env)
        runner('patches.py', '--out', out, '--game-dir', game, '--fps', '30',
               '--settings', base / 'missing.ini')
        assert (out / 'patches.bin').read_bytes()[:8] == b'BBPATCH2'
        # A synthetic image cannot be reused to bypass normal game validation.
        strict = dict(env)
        strict.pop('BB_SKIP_GAME_CHECK')
        env.pop('BB_SKIP_GAME_CHECK')
        try:
            prepare_game(game, out, runner, env=strict)
            raise AssertionError('unsupported fixture was accepted')
        except AssertionError as error:
            assert 'Unsupported game files' in str(error), error
        assert not (out / 'prepare-cache.json').exists()
    print('Preparation cache smoke passed: real SELF preparation/linking, exact binary, compact patch headers, verified reuse and strict rejection')


if __name__ == '__main__':
    main()

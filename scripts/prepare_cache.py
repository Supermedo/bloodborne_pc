#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Reuse a prepared Windows image only while every relevant byte still matches."""
import hashlib
import json
import os
from pathlib import Path
import time

ROOT = Path(__file__).resolve().parent.parent
SCHEMA = 1
GAME_INPUTS = ('eboot.bin', 'sce_sys/param.sfo', 'sce_module/libc.prx',
               'sce_module/libSceFios2.prx')
SCRIPT_INPUTS = ('prepare.py', 'game_check.py', 'link_libc.py', 'link_modules.py',
                 'prepare_cache.py')
# The runtime image and patch compiler's segment headers are consumed on a hit.
# boot.bin/full ELF/reports are intermediates; avoid large redundant reads.
OUTPUTS = ('boot-linked.bin', 'eboot-headers.bin')


def digest(path):
    sha = hashlib.sha256()
    with Path(path).open('rb') as file:
        while chunk := file.read(1024 * 1024):
            sha.update(chunk)
    return sha.hexdigest()


def fingerprint(game, root, env):
    # Relative labels allow identical mod views to share an image. Hash contents,
    # not paths, sizes or timestamps: replacing a dump must invalidate the cache.
    inputs = {f'game/{name}': digest(game / name) for name in GAME_INPUTS}
    inputs.update({f'scripts/{name}': digest(root / 'scripts' / name) for name in SCRIPT_INPUTS})
    inputs['skip_game_check'] = env.get('BB_SKIP_GAME_CHECK') == '1'
    return inputs


def prepare_game(game, out, runner, *, root=ROOT, env=None):
    """Return True on a verified hit; runner performs the normal CLI steps on a miss.

    Content profiles and patches remain outside this cache and run every launch.
    A failed rebuild leaves no reusable manifest. Cache I/O failure is nonfatal;
    missing/invalid game files still fail through the normal preparation scripts.
    """
    game, out, root = Path(game), Path(out), Path(root)
    env = os.environ if env is None else env
    manifest = out / 'prepare-cache.json'
    started = time.perf_counter()
    enabled = env.get('BB_PREPARE_CACHE') != '0'
    inputs = None
    if enabled:
        try:
            inputs = fingerprint(game, root, env)
            saved = json.loads(manifest.read_text(encoding='utf-8'))
            if (isinstance(saved, dict) and saved.get('schema') == SCHEMA and
                    saved.get('inputs') == inputs and
                    saved.get('outputs') == {name: digest(out / name) for name in OUTPUTS}):
                print(f'Prepared image: verified cache hit ({time.perf_counter() - started:.3f}s)')
                return True
        except (OSError, ValueError):
            pass

    # Invalidate before writing anything, including when caching is disabled.
    # Never carry an old success record across a failed/interrupted rebuild.
    try:
        manifest.unlink(missing_ok=True)
    except OSError:
        # Invalidating its contents is enough if removing this file is denied.
        # If neither succeeds, stop before changing the prepared outputs.
        manifest.write_text('{}\n', encoding='utf-8')
    runner('prepare.py', game, '--out', out, '--no-resource-inventory')
    # The multi-module linker already includes libc. boot-libc.bin from the old
    # single-module linker is not consumed by the Windows runtime.
    runner('link_modules.py', game, '--out', out)
    if enabled and inputs is not None:
        temp = manifest.with_suffix('.json.tmp')
        try:
            if fingerprint(game, root, env) != inputs:
                print('Prepared image: inputs changed during preparation; cache not saved')
                return False
            saved = dict(schema=SCHEMA, inputs=inputs,
                         outputs={name: digest(out / name) for name in OUTPUTS})
            temp.write_text(json.dumps(saved, indent=2) + '\n', encoding='utf-8')
            temp.replace(manifest)
        except OSError as error:
            print(f'Prepared image cache unavailable: {error}')
        finally:
            try:
                temp.unlink(missing_ok=True)
            except OSError:
                pass
    print(f'Prepared image: rebuilt ({time.perf_counter() - started:.3f}s)')
    return False

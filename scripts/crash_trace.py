#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the configured game with the external crash monitor and collect its report."""
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import zipfile


def collect(directory, status, root):
    dumps = sorted(directory.glob('bb-crash-*.dmp'))
    binaries = {}
    for name in ('Bloodborne.exe', 'bin/bb-probe.exe', 'bin/bb-crash-monitor.exe'):
        path = root / name
        if path.is_file():
            binaries[name] = hashlib.sha256(path.read_bytes()).hexdigest()
    metadata = {
        'format': 'BBPORT_CRASH_TRACE_V1',
        'launch_status': status & 0xffffffff,
        'launch_status_hex': f'0x{status & 0xffffffff:08x}',
        'dump_count': len(dumps),
        'binary_sha256': binaries,
        'mode': 'External Windows debugger; diagnostic launches only.',
        'contents': 'This session log, crash dumps and binary fingerprints. No game, save or settings files are copied.',
    }
    archive = directory.with_suffix('.zip')
    with zipfile.ZipFile(archive, 'x', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as report:
        report.writestr('metadata.json', json.dumps(metadata, indent=2) + '\n')
        report.write(directory / 'session.log', 'session.log')
        for path in dumps:
            report.write(path, path.name)
    return archive


def main():
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding='utf-8', errors='replace', line_buffering=True)
        except (AttributeError, ValueError):
            pass
    root = Path(__file__).resolve().parent.parent
    data = Path(os.environ.get('BB_DATA_DIR', root)).resolve()
    stamp = datetime.now().strftime('%Y%m%d-%H%M%S-%f')
    directory = data / 'logs' / f'crash-trace-{stamp}'
    directory.mkdir(parents=True)
    print('Crash diagnostics: reproduce the crash in this launch. The report ZIP will appear in logs.\n'
          'Debugger exception processing can reduce performance in this diagnostic mode.', flush=True)
    env = {**os.environ, 'BB_CRASH_MONITOR': '1', 'BB_CRASH_DUMP': '1', 'BB_CRASH_DIR': str(directory)}
    command = [str(root / 'Bloodborne.exe'), '--play']
    with (directory / 'session.log').open('w', encoding='utf-8') as log:
        with subprocess.Popen(command, cwd=root, env=env, stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding='utf-8',
                errors='replace', creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0)) as process:
            for line in process.stdout:
                log.write(line)
                log.flush()
                print(line, end='', flush=True)
            status = process.wait()
        log.write(f'Launch exit: {status & 0xffffffff} (0x{status & 0xffffffff:08x})\n')
    print(f'Report: {collect(directory, status, root)}', flush=True)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, zipfile.BadZipFile) as error:
        print(f'Crash diagnostics: {error}', file=sys.stderr)
        raise SystemExit(1)

#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The port's program files of one version. packaging/windows/package.sh writes bbport-files.json beside
Bloodborne.exe: every file of bin\\, run.py and scripts\\ with its SHA-256. run.py and the launcher compare the
installation with it, because a test build unpacked over an older version, or a package copied only in part,
mixes the versions; the game then stops at the start with "bad patch entry" (a black screen).

    package_files.py --write PORT_DIR VERSION
"""
import hashlib
import json
import sys
from pathlib import Path

MANIFEST = 'bbport-files.json'
SKIP = ('nvngx',)  # NVIDIA's DLSS runtime: players may put a newer one in
QUICK = ('bin/bb-probe.exe', 'run.py')  # with scripts\: what has to match at every start


def program_files(port):
    port = Path(port)
    files = [port / 'run.py', *sorted((port / 'bin').glob('*')),
             *sorted(p for p in (port / 'scripts').rglob('*.py') if '__pycache__' not in p.parts)]
    return [p for p in files if p.is_file() and not p.name.lower().startswith(SKIP)]


def digest(path):
    hashed = hashlib.sha256()
    with open(path, 'rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            hashed.update(block)
    return hashed.hexdigest()


def write(port, version):
    """The list for a package, into PORT_DIR; returns the number of files."""
    port = Path(port)
    files = {p.relative_to(port).as_posix(): digest(p) for p in program_files(port)}
    (port / MANIFEST).write_text(json.dumps({'version': version, 'files': files}, indent=1) + '\n', encoding='utf-8')
    return len(files)


def check(port, quick=False):
    """(version, [files missing or changed]) against the package's list; (None, []) without one (a build from
    the sources). quick: only what every start needs (bb-probe.exe, run.py, the scripts)."""
    port = Path(port)
    try:
        manifest = json.loads((port / MANIFEST).read_text(encoding='utf-8'))
        version, files = str(manifest['version']), dict(manifest['files'])
    except (OSError, ValueError, KeyError, TypeError):
        return None, []
    differ = []
    for name, expected in sorted(files.items()):
        if quick and name not in QUICK and not name.startswith('scripts/'):
            continue
        try:
            if digest(port / name) != expected:
                differ.append(name)
        except OSError:
            differ.append(name)
    return version, differ


def main(argv):
    if len(argv) == 4 and argv[1] == '--write':
        print(f'{MANIFEST}: {write(argv[2], argv[3])} files')
        return 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))

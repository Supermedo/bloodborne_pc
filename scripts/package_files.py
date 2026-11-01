#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The port's program files of one version. packaging/windows/package.sh writes bbport-files.json beside
Bloodborne.exe: every file of bin\\, run.py and scripts\\ with its size and SHA-256. run.py and the launcher compare the
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
CACHE = '.bbport-files-cache.json'  # the hashes of the last start, by size and time: a start reads only what changed


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
    found = program_files(port)
    files = {p.relative_to(port).as_posix(): digest(p) for p in found}
    sizes = {p.relative_to(port).as_posix(): p.stat().st_size for p in found}
    (port / MANIFEST).write_text(json.dumps({'version': version, 'files': files, 'sizes': sizes}, indent=1) + '\n',
                                 encoding='utf-8')
    return len(files)


def check(port, quick=False):
    """(version, [files missing or changed]) against the package's list; (None, []) without one (a build from
    the sources). quick (every start): all listed files too, by size first and then by the hash of the last start
    for a file whose size and time did not change, so only a changed file is read again. The DLSS bridge
    (bin\\bbport_dlss.dll) of another version made DLSS fail without a word while the start check passed."""
    port = Path(port)
    try:
        manifest = json.loads((port / MANIFEST).read_text(encoding='utf-8'))
        version, files = str(manifest['version']), dict(manifest['files'])
        sizes = dict(manifest.get('sizes') or {})
    except (OSError, ValueError, KeyError, TypeError, AttributeError):
        return None, []
    cache = {}
    if quick:
        try:
            cache = dict(json.loads((port / CACHE).read_text(encoding='utf-8')))
        except (OSError, ValueError, TypeError):
            cache = {}
    known, differ = {}, []
    for name, expected in sorted(files.items()):
        try:
            info = (port / name).stat()
            if name in sizes and info.st_size != sizes[name]:
                differ.append(name)
                continue
            stamp = [info.st_size, info.st_mtime_ns]
            seen = cache.get(name)
            hashed = seen[1] if isinstance(seen, list) and len(seen) == 2 and seen[0] == stamp else digest(port / name)
            known[name] = [stamp, hashed]
            if hashed != expected:
                differ.append(name)
        except OSError:
            differ.append(name)
    if quick and known != cache:
        try:
            (port / CACHE).write_text(json.dumps(known), encoding='utf-8')
        except OSError:
            pass  # a read-only installation: the next start reads the files again
    return version, differ


def main(argv):
    if len(argv) == 4 and argv[1] == '--write':
        print(f'{MANIFEST}: {write(argv[2], argv[3])} files')
        return 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))

#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The files of the clean game, version 1.09 (CUSA03173), by size and checksum: a copy or an unpack that went wrong,
or a mod copied into the game folder, shows as files that differ. Only checksums are kept here, no game data.

    python game_files.py GAME_DIR            checks a game folder against the list
    python game_files.py --make GAME_DIR     writes the list from a clean game folder

The list (game_files_0109.txt.gz) has a line per file: size, BLAKE2b-128 in hex, the path inside the game folder.
eboot.bin is not in it (the launcher checks it on its own), nor sce_sys, whose files differ between dump tools."""
import gzip
import hashlib
import os
import sys
from pathlib import Path

# From the port's folder, as patches.py finds patches/: the packaged launcher has this module in _internal.
LIST = Path(__file__).resolve().parent.parent / 'scripts' / 'game_files_0109.txt.gz'
FOLDERS = ('dvdroot_ps4', 'sce_module')
CHUNK = 1 << 20


def checksum(path):
    digest = hashlib.blake2b(digest_size=16)
    with open(path, 'rb') as stream:
        while block := stream.read(CHUNK):
            digest.update(block)
    return digest.hexdigest()


def game_files(game_dir):
    """{path inside the game folder ('/' between folders): size} of FOLDERS, without following links."""
    root, found = Path(game_dir), {}
    for folder in FOLDERS:
        stack = [root / folder]
        while stack:
            try:
                entries = list(os.scandir(stack.pop()))
            except OSError:
                continue
            for entry in entries:
                try:
                    if entry.is_symlink():
                        continue
                    if entry.is_dir():
                        stack.append(entry.path)
                    elif entry.is_file():
                        found[Path(entry.path).relative_to(root).as_posix()] = entry.stat().st_size
                except (OSError, ValueError):
                    pass
    return found


def read_list(path=LIST):
    """{path: (size, checksum)} of the clean game."""
    files = {}
    with gzip.open(path, 'rt', encoding='utf-8') as stream:
        for line in stream:
            size, digest, name = line.rstrip('\n').split('\t', 2)
            files[name] = (int(size), digest)
    return files


def make(game_dir, out=LIST):
    files = game_files(game_dir)
    lines = [f'{size}\t{checksum(Path(game_dir) / name)}\t{name}\n' for name, size in sorted(files.items())]
    with gzip.open(out, 'wt', encoding='utf-8', compresslevel=9) as stream:
        stream.writelines(lines)
    return len(lines)


def check(game_dir, progress=None, clean=None):
    """{'checked', 'differ': [paths], 'missing': [paths]} of a game folder against the clean game's list.
    progress(done_bytes, all_bytes) is called as files are read; the folders' own sizes rule out a file first,
    without reading it."""
    clean = read_list() if clean is None else clean
    have = game_files(game_dir)
    missing = sorted(name for name in clean if name not in have)
    differ = sorted(name for name, (size, _digest) in clean.items() if name in have and have[name] != size)
    to_read = [name for name in sorted(clean) if name in have and have[name] == clean[name][0]]
    total, done = sum(clean[name][0] for name in to_read), 0
    for name in to_read:
        try:
            if checksum(Path(game_dir) / name) != clean[name][1]:
                differ.append(name)
        except OSError:
            differ.append(name)
        done += clean[name][0]
        if progress:
            progress(done, total)
    return {'checked': len(clean), 'differ': sorted(differ), 'missing': missing}


def write_result(result, path):
    lines = [f"{result['checked']} files: {len(result['differ'])} differ, {len(result['missing'])} missing\n"]
    lines += [f'differ\t{name}\n' for name in result['differ']]
    lines += [f'missing\t{name}\n' for name in result['missing']]
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_text(''.join(lines), encoding='utf-8')


def main(argv):
    if argv[:1] == ['--make'] and len(argv) == 2:
        print(f'{make(argv[1])} files written to {LIST}')
        return 0
    if len(argv) != 1:
        print(__doc__)
        return 1
    result = check(argv[0])
    print(f"{result['checked']} files: {len(result['differ'])} differ, {len(result['missing'])} missing")
    for name in result['differ'][:20] + result['missing'][:20]:
        print(' ', name)
    return 0 if not result['differ'] and not result['missing'] else 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))

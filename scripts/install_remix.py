#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Install the pinned, official NVIDIA RTX Remix runtime for the SDK backend."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil
import struct
import tempfile
import urllib.request
import zipfile

VERSION = '1.5.2'
API_VERSION = '0.6.4'
COMMIT = 'b81a7b566b1eeb9edb4dc2b3c9d3972e0f253ad4'
URL = ('https://github.com/NVIDIAGameWorks/rtx-remix/releases/download/'
       'remix-1.5.2/remix-1.5.2-release.zip')
SHA256 = 'cc424be4dd1a0c6fd922bc6a7f8e5f6582baea7043a38afa6686d8b6faabad01'


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def check_x64(path):
    with path.open('rb') as stream:
        header = stream.read(64)
        if len(header) != 64 or header[:2] != b'MZ':
            raise ValueError('Runtime has no valid PE header')
        offset, = struct.unpack_from('<I', header, 60)
        if offset > path.stat().st_size - 24:
            raise ValueError('Runtime PE header offset is invalid')
        stream.seek(offset)
        pe = stream.read(24)
        if pe[:4] != b'PE\0\0' or struct.unpack_from('<H', pe, 4)[0] != 0x8664:
            raise ValueError('SDK needs the x64 .trex/d3d9.dll; the x86 bridge is incompatible')


def extract_runtime(archive, target):
    """Only .trex and its licenses; reject traversal, links and unexpected layouts."""
    with zipfile.ZipFile(archive) as zipped:
        members = []
        licenses = {'LICENSE.txt', 'ThirdPartyLicenses-dxvk.txt'}
        for info in zipped.infolist():
            name = info.filename.replace('\\', '/')
            parts = PurePosixPath(name)
            if parts.is_absolute() or any(p in ('..', '.') or ':' in p for p in parts.parts):
                raise ValueError(f'Unsafe runtime archive member: {info.filename}')
            if (info.external_attr >> 16) & 0o170000 == 0o120000:
                raise ValueError(f'Symbolic link in runtime archive: {info.filename}')
            if name in licenses or (parts.parts and parts.parts[0] == '.trex'):
                members.append(info)
        if not any(i.filename.replace('\\', '/') == '.trex/d3d9.dll' for i in members):
            raise ValueError('Official x64 .trex/d3d9.dll missing from archive')
        if sum(i.file_size for i in members) > 2 * 1024**3:
            raise ValueError('Runtime archive exceeds expected uncompressed size')
        for info in members:
            path = target.joinpath(*PurePosixPath(info.filename.replace('\\', '/')).parts)
            if info.is_dir():
                path.mkdir(parents=True, exist_ok=True)
            else:
                path.parent.mkdir(parents=True, exist_ok=True)
                with zipped.open(info) as source, path.open('xb') as destination:
                    shutil.copyfileobj(source, destination)
    check_x64(target / '.trex' / 'd3d9.dll')


def install(target, archive=None):
    target = target.resolve()
    if target.exists():
        manifest = target / 'sdk-runtime.json'
        if manifest.is_file():
            data = json.loads(manifest.read_text(encoding='utf-8'))
            dll = target / '.trex' / 'd3d9.dll'
            if (data.get('archive_sha256') == SHA256 and dll.is_file()
                    and data.get('dll_sha256') == digest(dll)):
                check_x64(dll)
                print(f'Official RTX Remix {VERSION} already installed: {target}')
                return target
        raise ValueError(f'Destination already exists; choose a new empty destination: {target}')
    target.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.remix-install-', dir=target.parent) as temp:
        work = Path(temp)
        package = Path(archive).resolve() if archive else work / 'runtime.zip'
        if archive is None:
            print(f'Downloading official RTX Remix {VERSION}...', flush=True)
            request = urllib.request.Request(URL, headers={'User-Agent': 'Bloodborne-Windows-v2'})
            with urllib.request.urlopen(request, timeout=60) as source, package.open('xb') as destination:
                shutil.copyfileobj(source, destination)
        if digest(package) != SHA256:
            raise ValueError('Runtime SHA-256 mismatch; installation aborted')
        staging = work / 'runtime'
        staging.mkdir()
        extract_runtime(package, staging)
        metadata = {'runtime_version': VERSION, 'sdk_api': API_VERSION, 'source_commit': COMMIT,
                    'source_url': URL, 'archive_sha256': SHA256,
                    'dll_sha256': digest(staging / '.trex' / 'd3d9.dll')}
        (staging / 'sdk-runtime.json').write_text(json.dumps(metadata, indent=2) + '\n', encoding='utf-8')
        staging.rename(target)
    print(f'Official RTX Remix {VERSION} installed: {target}', flush=True)
    print('Select Graphics > Renderer > RTX Remix (experimental) in the launcher, then restart the game.', flush=True)
    return target


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive', type=Path, help='Use the unmodified official release ZIP offline')
    parser.add_argument('--destination', type=Path,
                        default=Path(__file__).resolve().parents[1] / 'remix-runtime-1.5.2')
    args = parser.parse_args()
    install(args.destination, args.archive)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, zipfile.BadZipFile) as error:
        raise SystemExit(f'RTX Remix installation failed: {error}')

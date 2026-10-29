#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Adapted from 0xCydral/bloodborne_pc at 66576efbbd272d0c97e04a9bdb5631455d530eae.
"""Downloads the fsr4vk provider (experimental FSR 4.1.1 on Windows) into a folder.

    scripts/fetch_fsr4vk.py [--dir DIR] [--check]

fsr4vk (https://github.com/dvj5411/fsr4vk, GPLv3; its AMD shader and model data are MIT) is a Vulkan
FFX provider for the INT8 FSR 4.1.1 model. Windows packages include it and load its DLL at run time
(vk_fsr4vk.cpp). For repair or source builds, this script fetches the pinned zip,
checks its SHA-256, extracts the provider DLL and licenses, and checks the DLL's SHA-256.
The zip is fsr4vk v0.4.3 plus one patch for AMD's Windows
driver (upstream's DLL fails there), hosted in 0xCydral's pinned GitHub release. A folder that holds
the original upstream DLL does not verify, so `fetch` replaces it. Resumable (HTTP Range into a .part
file), stdlib only, so the launcher can import `fetch` or run it as a process.

DIR defaults to BB_FSR4VK_DIR, else <BB_DATA_DIR or port>/fsr4vk. run.py
passes that persistent folder to the renderer, and launcher updates preserve it.
Exit status 0: the folder holds the verified files. 1: failed (the reason is the last line on stderr).
"""
import argparse
import hashlib
import http.client
import os
import sys
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

RELEASE = 'v0.4.3 with the AMD fix'
SOURCE_URL = 'https://github.com/dvj5411/fsr4vk/tree/7c04e511195bf4420a060d64df84d625a37457e0'
ISSUE_URL = 'https://github.com/dvj5411/fsr4vk/issues/1'
# Deliberately the windows-v1.7.0-beta.2 release, where the asset is hosted: this URL does not follow later
# version bumps.
RELEASE_URL = 'https://github.com/0xCydral/bloodborne_pc/releases/download/windows-v1.7.0-beta.2'
ZIP_NAME = 'fsr4vk-v0.4.3-amdfix.zip'
ZIP_URL = f'{RELEASE_URL}/{ZIP_NAME}'
SOURCE_ZIP_URL = f'{RELEASE_URL}/fsr4vk-v0.4.3-amdfix-src.zip'
ZIP_SIZE = 14142136
ZIP_SHA256 = '049e81577dd0836fd6fb24a2d7fb07d1276951987519d11251905b9e60456ebb'

# (name inside the zip, name in DIR, size, SHA-256)
FILES = (
    ('amd_fidelityfx_upscaler_vk.dll', 'amd_fidelityfx_upscaler_vk.dll', 17603072,
     '60a90b24f6789cd52c467471d5a66a0fa6d04e1b8af41095242f50367904f8b2'),
    ('LICENSES/GPL-3.0.txt', 'LICENSES/GPL-3.0.txt', 35149,
     '3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986'),
    ('LICENSES/AMD-FidelityFX-SDK-MIT.md', 'LICENSES/AMD-FidelityFX-SDK-MIT.md', 1119,
     'be05d8cd5489deff924dbe91e0c5b8c0ff034d43cd11749bf9a67a0c38252b7b'),
    ('LICENSES/Zstandard-BSD.txt', 'LICENSES/Zstandard-BSD.txt', 1549,
     '7055266497633c9025b777c78eb7235af13922117480ed5c674677adc381c9d8'),
)
EXTRA = ('PATCH.md',)  # unpacked when the zip holds it; files_ok does not look at it
SOURCE_NOTE = (
    'fsr4vk ' + RELEASE + ' (GPLv3 provider, MIT AMD FidelityFX data).\n'
    'This is fsr4vk v0.4.3 (upstream tag v0.4.3, commit 7c04e511195bf4420a060d64df84d625a37457e0) with '
    'one patch: it drops the variable descriptor count flag '
    '(VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT) from its three heap bindings, which AMD\'s '
    'Windows driver does not accept. PATCH.md describes it.\n'
    'Downloaded from ' + ZIP_URL + '\n'
    'Source code of the patched build: ' + SOURCE_ZIP_URL + '\n'
    'Source code of upstream v0.4.3: ' + SOURCE_URL + '\n'
    'Upstream issue about the AMD failure: ' + ISSUE_URL + '\n'
    'Licenses: LICENSES/ (GPLv3 for the provider). The DLL is loaded at run time; its source '
    'and patched-build provenance are linked above.\n')
CHUNK = 1 << 20


class FetchError(Exception):
    pass


PORT = Path(__file__).resolve().parent.parent
DEFAULT_DIR_TEXT = 'BB_FSR4VK_DIR, else <BB_DATA_DIR or port>/fsr4vk'


def default_dir(port: Path = PORT) -> Path:
    """Persistent optional assets, outside bin/ so application updates keep them."""
    override = os.environ.get('BB_FSR4VK_DIR')
    return Path(override) if override else Path(os.environ.get('BB_DATA_DIR', port)) / 'fsr4vk'


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, 'rb') as stream:
        for block in iter(lambda: stream.read(CHUNK), b''):
            digest.update(block)
    return digest.hexdigest()


def verified(path: Path, size: int, sha256: str) -> bool:
    try:
        return path.stat().st_size == size and sha256_of(path) == sha256
    except OSError:
        return False


def files_ok(target: Path) -> bool:
    """True when every pinned file is in `target` with its pinned size and hash."""
    return all(verified(target / name, size, sha) for _, name, size, sha in FILES)


def _urlopen(request, timeout):
    """The launcher's HTTPS (launcher/bbport_tls.py: no expired certificates of the Windows stores, Windows'
    trust when OpenSSL refuses a server) where it is there: in Bloodborne.exe and when the launcher imports
    this script. urllib's own when the script runs by itself from a source tree."""
    try:
        import bbport_tls
    except ImportError:
        return urllib.request.urlopen(request, timeout=timeout)
    return bbport_tls.urlopen(request, timeout=timeout)


def _download(part: Path, progress) -> None:
    """Fills `part` with the zip, resuming what an earlier run left; the caller checks the hash."""
    for attempt in range(4):
        have = part.stat().st_size if part.exists() else 0
        if have > ZIP_SIZE:
            part.unlink()
            have = 0
        if have == ZIP_SIZE:
            return
        request = urllib.request.Request(ZIP_URL, headers={'User-Agent': 'bbport-fetch-fsr4vk'})
        if have:
            request.add_header('Range', f'bytes={have}-')
        try:
            with _urlopen(request, timeout=60) as response:
                if have and response.status != 206:
                    have = 0  # the server ignored the range: start again
                with open(part, 'ab' if have else 'wb') as out:
                    for block in iter(lambda: response.read(CHUNK), b''):
                        out.write(block)
                        have += len(block)
                        if progress:
                            progress(have, ZIP_SIZE)
        except urllib.error.HTTPError as error:
            if error.code == 416:  # the part is longer than the file: drop it
                part.unlink(missing_ok=True)
            elif attempt == 3:
                raise FetchError(f'download failed: HTTP {error.code} from {ZIP_URL}')
        except (urllib.error.URLError, http.client.HTTPException, OSError, TimeoutError) as error:
            if attempt == 3:
                raise FetchError(f'download failed: {error}')
        if part.exists() and part.stat().st_size == ZIP_SIZE:
            return
    raise FetchError('download failed: the file stayed incomplete')


def fetch(target: Path, progress=None) -> None:
    """Puts the pinned, verified provider files into `target`. `progress(done, total)` is optional.
    Raises FetchError. Does nothing when `target` already holds them."""
    target = Path(target)
    if files_ok(target):
        return
    target.mkdir(parents=True, exist_ok=True)
    work = target / '.download'
    work.mkdir(exist_ok=True)
    part = work / (ZIP_NAME + '.part')
    _download(part, progress)
    if part.stat().st_size != ZIP_SIZE or sha256_of(part) != ZIP_SHA256:
        part.unlink(missing_ok=True)
        raise FetchError('the downloaded zip does not match the pinned SHA-256; it was deleted')
    try:
        with zipfile.ZipFile(part) as archive:
            for member, name, size, sha in FILES:
                data = archive.read(member)
                if len(data) != size or hashlib.sha256(data).hexdigest() != sha:
                    raise FetchError(f'{member} in the zip does not match its pinned SHA-256')
                destination = target / name
                destination.parent.mkdir(parents=True, exist_ok=True)
                temporary = destination.with_name(destination.name + '.tmp')
                try:
                    temporary.write_bytes(data)
                    os.replace(temporary, destination)
                except OSError:  # e.g. the game has the DLL loaded: no half-written copy stays
                    temporary.unlink(missing_ok=True)
                    raise
            for name in EXTRA:
                if name in archive.namelist():
                    (target / name).write_bytes(archive.read(name))
    except zipfile.BadZipFile as error:
        raise FetchError(f'the zip is damaged: {error}')
    (target / 'SOURCE.txt').write_text(SOURCE_NOTE, encoding='utf-8')
    part.unlink(missing_ok=True)
    try:
        work.rmdir()
    except OSError:
        pass


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--dir', type=Path, default=None, help='target folder (default: ' + DEFAULT_DIR_TEXT + ')')
    parser.add_argument('--check', action='store_true', help='only verify the folder; download nothing')
    args = parser.parse_args(argv)
    target = args.dir or default_dir()
    if args.check:
        ok = files_ok(target)
        print(f'fsr4vk {RELEASE}: {"files verified" if ok else "files missing or changed"} in {target}')
        return 0 if ok else 1
    last = [-1]

    def progress(done, total):
        percent = done * 100 // total
        if percent != last[0] and percent % 5 == 0:
            last[0] = percent
            print(f'PROGRESS {done} {total}', flush=True)
    try:
        fetch(target, progress)
    except FetchError as error:
        print(f'fetch_fsr4vk: {error}', file=sys.stderr)
        return 1
    print(f'fsr4vk {RELEASE}: files verified in {target}')
    return 0


if __name__ == '__main__':
    sys.exit(main())

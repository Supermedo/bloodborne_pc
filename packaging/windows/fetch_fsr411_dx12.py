#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prepare the pinned AMD FSR 4.1.1 runtime for the Windows release package.

Only the unmodified upscaler DLL is extracted. package.sh ships it with AMD's
license and third-party notices from packaging/windows/licenses/.
"""
import argparse
import hashlib
from pathlib import Path
import os
import shutil
import tempfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[2]
ARCHIVE_NAME = 'FidelityFX-Samples-v2.3.0-prebuilt.zip'
URL = ('https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/releases/download/'
       'v2.3.0/' + ARCHIVE_NAME)
ARCHIVE_SIZE = 127451548
ARCHIVE_SHA256 = 'f90890b9323bb2f4f2404ac4cdc9395e8495ecdac6f7aa0bcdf1ad1848422273'
DLL_NAME = 'amd_fidelityfx_upscaler_dx12.dll'
DLL_MEMBER = 'Samples/Upscalers/FidelityFX_FSR/dx12/x64/Release/' + DLL_NAME
DLL_SIZE = 28761864
DLL_SHA256 = 'd0dcccc74a43c44ba435b7a369b456e0970d8a4464e4bd683119b374f2c9fb46'


def verified(path, size, digest):
    try:
        with path.open('rb') as stream:
            if path.stat().st_size != size:
                return False
            actual = hashlib.sha256()
            for block in iter(lambda: stream.read(1 << 20), b''):
                actual.update(block)
            return actual.hexdigest() == digest
    except OSError:
        return False


def prepare(target):
    """Cache downloads under out/; reject modified archives and DLLs before installation."""
    dll = target / DLL_NAME
    if verified(dll, DLL_SIZE, DLL_SHA256):
        return dll
    target.mkdir(parents=True, exist_ok=True)
    archive = target / ARCHIVE_NAME
    if not verified(archive, ARCHIVE_SIZE, ARCHIVE_SHA256):
        request = urllib.request.Request(URL, headers={'User-Agent': 'bbport-fsr411-package'})
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(dir=target, suffix='.zip.part', delete=False) as out:
                temporary = Path(out.name)
                with urllib.request.urlopen(request, timeout=60) as response:
                    shutil.copyfileobj(response, out)
            if not verified(temporary, ARCHIVE_SIZE, ARCHIVE_SHA256):
                raise ValueError('AMD SDK archive size or SHA-256 does not match the pinned release')
            os.replace(temporary, archive)
        finally:
            if temporary:
                temporary.unlink(missing_ok=True)
    # Read this exact member, never extract arbitrary archive paths.
    with zipfile.ZipFile(archive) as package:
        if package.getinfo(DLL_MEMBER).file_size != DLL_SIZE:
            raise ValueError('AMD upscaler DLL size does not match the pinned release')
        data = package.read(DLL_MEMBER)
    if hashlib.sha256(data).hexdigest() != DLL_SHA256:
        raise ValueError('AMD upscaler DLL SHA-256 does not match the pinned release')
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=target, suffix='.dll.part', delete=False) as out:
            temporary = Path(out.name)
            out.write(data)
        os.replace(temporary, dll)
    finally:
        if temporary:
            temporary.unlink(missing_ok=True)
    return dll


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dir', type=Path, default=ROOT / 'out/fsr411-dx12')
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    if args.check:
        return 0 if verified(args.dir / DLL_NAME, DLL_SIZE, DLL_SHA256) else 1
    try:
        print('AMD FSR 4.1.1 runtime verified:', prepare(args.dir))
    except (OSError, ValueError, KeyError, zipfile.BadZipFile) as error:
        parser.exit(1, f'fetch_fsr411_dx12: {error}\n')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())

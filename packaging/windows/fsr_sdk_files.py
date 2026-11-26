# SPDX-License-Identifier: GPL-2.0-or-later
"""Verify and atomically fetch hash-pinned AMD SDK effect files."""
import hashlib
import os
from pathlib import Path
import tempfile
import urllib.request


def verified(path, row):
    try:
        with Path(path).open('rb') as stream:
            return Path(path).stat().st_size == row['bytes'] and hashlib.file_digest(stream, 'sha256').hexdigest() == row['sha256']
    except OSError:
        return False


def prepare(target, pin, check=False, user_agent='bbport-fsr-sdk'):
    target = Path(target)
    for row in pin['files']:
        path = target / Path(row['path']).name
        if verified(path, row):
            continue
        if check:
            raise ValueError('SDK 2.4 file missing or changed: ' + path.name)
        target.mkdir(parents=True, exist_ok=True)
        url = f"https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/{pin['commit']}/{row['path']}"
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(dir=target, suffix='.part', delete=False) as stream:
                temporary = Path(stream.name)
                request = urllib.request.Request(url, headers={'User-Agent': user_agent})
                with urllib.request.urlopen(request, timeout=120) as response:
                    while block := response.read(1 << 20):
                        stream.write(block)
            if not verified(temporary, row):
                raise ValueError('SDK 2.4 file size or SHA256 mismatch: ' + path.name)
            os.replace(temporary, path)
        finally:
            if temporary:
                temporary.unlink(missing_ok=True)
    return target

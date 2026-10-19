#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Field of view (bbport.ini fov, degrees): the game's camera table with every camera's field of
view scaled, as a mod layer (mods.py --layer).

The cameras come from LockCamParam in param/gameparam/gameparam.parambnd.dcx, the same
LOCK_CAM_PARAM_ST as Dark Souls III's: each row is camDistTarget, rotRangeMinX,
lockRotXShiftRatio, chrOrgOffset_Y, chrLockRangeMaxRadius, then camFovY, the vertical field of
view in degrees, at +0x14. The normal camera has 43; a few areas set their own (48). Every row is
scaled by fov / 43, so those keep their difference. The game reads the table at start: a change
needs a restart. A mod that replaces gameparam.parambnd.dcx is layered above this one and wins.
"""
import argparse
import hashlib
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from quit_menu import bnd4_files, pack_dcx, rebuild_bnd4, unpack_dcx  # noqa: E402

GAME_FOV = 43.0     # camFovY of the game's normal camera
FOV_RANGE = (30, 90)
ARCHIVE = Path('dvdroot_ps4') / 'param' / 'gameparam' / 'gameparam.parambnd.dcx'
TABLE = 'LockCamParam.param'
CAM_FOV_Y = 0x14


def bnd4_name(data, entry):
    """The file name of a BND4 entry, without its folders."""
    offset, = struct.unpack_from('<I', data, entry + 0x20)
    if data[0x30]:  # UTF-16 names
        end = offset
        while data[end:end + 2] != b'\0\0':
            end += 2
        name = data[offset:end].decode('utf-16-le')
    else:
        name = data[offset:data.index(b'\0', offset)].decode('shift-jis')
    return name.replace('\\', '/').rsplit('/', 1)[-1]


def param_rows(param):
    """Data offsets of a PARAM's rows (64-bit row table at 0x40, as in this game's params)."""
    if param[0x0C:0x0C + 17] != b'LOCK_CAM_PARAM_ST':
        raise ValueError('LockCamParam is not a LOCK_CAM_PARAM_ST table')
    count, = struct.unpack_from('<H', param, 0x0A)
    rows = [struct.unpack_from('<Q', param, 0x40 + i * 24 + 8)[0] for i in range(count)]
    if not rows or any(r + CAM_FOV_Y + 4 > len(param) for r in rows):
        raise ValueError('LockCamParam rows out of range')
    return rows


def scaled_table(param, fov):
    """The table with every camFovY scaled by fov / GAME_FOV, and how many rows changed."""
    out = bytearray(param)
    changed = 0
    for row in param_rows(param):
        value, = struct.unpack_from('<f', param, row + CAM_FOV_Y)
        if not 10.0 <= value <= 120.0:
            raise ValueError(f'LockCamParam camFovY {value} is not a field of view')
        struct.pack_into('<f', out, row + CAM_FOV_Y, min(150.0, value * fov / GAME_FOV))
        changed += 1
    return bytes(out), changed


def scaled_archive(archive, fov):
    bnd = unpack_dcx(archive)
    for entry, offset, length, file_id in bnd4_files(bnd):
        if bnd4_name(bnd, entry) == TABLE:
            table, changed = scaled_table(bnd[offset:offset + length], fov)
            return pack_dcx(archive, rebuild_bnd4(bnd, {file_id: table})), changed
    raise ValueError(f'{TABLE} not found')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('game', type=Path, help='game folder (with dvdroot_ps4)')
    p.add_argument('--out', type=Path, required=True, help='mod layer folder to write')
    p.add_argument('--fov', type=float, required=True, help=f'degrees (the game: {GAME_FOV:g})')
    a = p.parse_args()
    fov = min(max(a.fov, FOV_RANGE[0]), FOV_RANGE[1])
    source = a.game / ARCHIVE
    data = source.read_bytes()
    target = a.out / ARCHIVE
    # Repacking takes a few seconds: the layer is kept while the setting and the game file match.
    stamp = a.out / 'fov.stamp'
    key = f'{fov:g} {hashlib.sha256(data).hexdigest()}'
    if target.is_file() and stamp.is_file() and stamp.read_text() == key:
        print(f'Field of view: {fov:g} degrees (kept)')
        return
    archive, changed = scaled_archive(data, fov)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(archive)
    stamp.write_text(key)
    print(f'Field of view: {fov:g} degrees ({changed} cameras)')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError, struct.error) as error:
        print(f'Field of view: {error}; the game keeps its own', file=sys.stderr)
        sys.exit(1)

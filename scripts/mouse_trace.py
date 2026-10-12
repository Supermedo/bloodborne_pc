"""Collect the bounded, opt-in Windows mouse-camera trace without a Python install."""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import time
import zipfile


def decode(data):
    if len(data) < 4096 or data[:8] not in (b'BBMOUSE1', b'BBMOUSE2'):
        raise ValueError('Not a mouse-camera trace')
    version, header, record_size, capacity, frequency, before_count, after_count = struct.unpack_from('<4IQ2I', data, 8)
    layouts = {1: (b'BBMOUSE1', 336, 4096), 2: (b'BBMOUSE2', 552, 16384)}
    if (version not in layouts or (data[:8], record_size, capacity) != layouts[version] or
            (header, before_count, after_count) != (4096, 64, 9)):
        raise ValueError('Unsupported mouse-camera trace layout')
    if not frequency or len(data) != header + record_size * capacity:
        raise ValueError('Truncated mouse-camera trace')
    offsets = struct.unpack_from('<73I', data, 64)
    if len(set(offsets[:64])) != 64 or any(n > 0x294 or n % 4 for n in offsets):
        raise ValueError('Invalid camera field offsets')
    names = [f'before_{n:03x}' for n in offsets[:64]] + [f'after_{n:03x}' for n in offsets[64:]]
    if version == 2:
        if struct.unpack_from('<2I', data, 40) != (51, 8):
            raise ValueError('Invalid movement trace extension')
        extra = struct.unpack_from('<51I', data, 356)
        native_flags = struct.unpack_from('<8I', data, 560)
        if (len(set(offsets[:64] + extra)) != 115 or any(n > 0x294 or n % 4 for n in extra) or
                len(set(native_flags)) != 8 or any(n > 0x294 for n in native_flags)):
            raise ValueError('Invalid movement camera field offsets')
        names += ['pad_valid', 'pad_buttons', 'left_x', 'left_y', 'right_x', 'right_y']
        names += [f'before_{n:03x}' for n in extra] + [f'native_flag_{n:03x}' for n in native_flags]
    rows, origin, previous = [], None, None
    for index in range(capacity):
        values = struct.unpack_from('<3Q2ifI73f', data, header + record_size * index)
        sequence, ticks, camera, dx, dy, sensitivity, flags, *fields = values
        if not sequence:  # unfilled or interrupted record
            continue
        if sequence != index + 1 or flags & ~(31 if version == 2 else 15):
            raise ValueError('Invalid mouse-camera record')
        if origin is None:
            origin = previous = ticks
        row = [sequence, (ticks - origin) / frequency, (ticks - previous) / frequency,
                     hex(camera), dx / 256, dy / 256, sensitivity,
                     int(bool(flags & 1)), int(bool(flags & 2)), int(bool(flags & 4)), int(bool(flags & 8)), *fields]
        if version == 2:
            pad, *extension = struct.unpack_from('<Q51fI', data, header + record_size * index + 336)
            row += [int(bool(flags & 16)), hex(pad & 0xffffffff), *[(pad >> shift) & 255 for shift in (32, 40, 48, 56)]]
            row += extension[:-1] + [(extension[-1] >> bit) & 1 for bit in range(8)]
        rows.append(row)
        previous = ticks
    columns = ['sequence', 'seconds', 'update_seconds', 'camera', 'raw_dx', 'raw_dy', 'sensitivity',
               'capture', 'mouse_owner', 'stick_moving', 'invert_y', *names]
    return columns, rows


def movement_summary(columns, rows):
    if 'pad_valid' not in columns:
        return {'available': False, 'reason': 'V1 reports do not record pad input'}
    valid, capture, owner, x, y = [columns.index(n) for n in ('pad_valid', 'capture', 'mouse_owner', 'left_x', 'left_y')]
    result = {'available': True, 'pad_samples': 0, 'diagonal_capture_on': 0,
              'diagonal_capture_off': 0, 'diagonal_mouse_owned': 0, 'cardinal_samples': 0}
    for row in rows:
        if not row[valid]:
            continue
        result['pad_samples'] += 1
        horizontal, vertical = abs(row[x] - 128) > 8, abs(row[y] - 128) > 8
        if horizontal and vertical:
            result['diagonal_capture_on' if row[capture] else 'diagonal_capture_off'] += 1
            result['diagonal_mouse_owned'] += bool(row[owner])
        elif horizontal or vertical:
            result['cardinal_samples'] += 1
    return result


def collect(directory):
    state = (directory / 'state.bin').read_bytes()
    columns, rows = decode(state)
    if not rows:
        raise ValueError('No mouse samples: enable F4 and move the mouse after loading a character')
    # All cells are numeric or hex identifiers, so CSV quoting is unnecessary.
    csv = '\n'.join([','.join(columns), *[','.join(map(str, row)) for row in rows]]) + '\n'
    metadata = {
        'format': 'BBPORT_MOUSE_REPORT_V2' if 'pad_valid' in columns else 'BBPORT_MOUSE_REPORT_V1', 'samples': len(rows),
        'seconds': rows[-1][1], 'capacity': struct.unpack_from('<I', state, 20)[0],
        'meaning': 'Before fields are sampled at camera-update entry; after fields follow native mouse writes. '
                   'Camera position and view basis at entry reflect the preceding game update. '
                   'Pad axes/buttons are the last complete state returned to the game, including remaps and replay. '
                   'Different camera addresses are separate objects.',
        'movement': movement_summary(columns, rows),
        'limits': 'This report diagnoses a gameplay issue; collecting it does not fix camera behavior. '
                  'Code contains the unmodified camera routine, bounded direct callees and RIP-relative constants.',
        'privacy': 'No game assets, saves, player names, launcher settings or game logs are collected.',
    }
    target = directory.with_suffix('.zip')
    code = (directory / 'code.txt').read_bytes()
    # Avoid replacing a previously collected report, even when called twice.
    with zipfile.ZipFile(target, 'x', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        archive.writestr('state.bin', state)
        archive.writestr('state.csv', csv)
        archive.writestr('metadata.json', json.dumps(metadata, indent=2) + '\n')
        archive.writestr('code.txt', code)
    return target


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path, nargs='?', help='A logs/mouse-camera-* directory')
    parser.add_argument('--play', action='store_true', help='Run the configured game, then collect this session')
    parser.add_argument('--movement', action='store_true', help='Show the keyboard/camera comparison procedure')
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    if args.play:
        if args.directory:
            parser.error('Use --play or a trace directory')
        started = time.time()
        if args.movement:
            print('After loading a character, enable F4 and move the mouse once to start recording.\n'
                  'In an open area: hold W for 3 seconds, W+A for 5 seconds, then W+D for 5 seconds.\n'
                  'Also start with A or D, then add W or S and hold each combination for 3 seconds.\n'
                  'Release the first key while holding the second: W -> W+D -> D, then D -> D+W -> W.\n'
                  'Hold each step for 3 seconds; repeat with A and S.\n'
                  'Repeat with the mouse moving. Disable F4 and repeat W, W+A and W+D.\n'
                  'For a controller comparison, enable F4 and move the mouse once, then repeat the directions with the left stick; keep the right stick neutral.\n'
                  'Close the game; the report ZIP will appear in logs. Recording is limited to 16384 camera updates.\n'
                  'Tracing is optional and does not alter input or camera behavior.', flush=True)
        else:
            print('Load a character, enable F4 and record left/right reversals, up/down sweeps and stops for about 15 seconds. '
                  'Then close the game. Recording stops after 16384 camera updates.', flush=True)
        subprocess.run([str(root / 'Play Bloodborne.exe')], cwd=root,
                       env={**os.environ, 'BB_MOUSE_TRACE': '1'}, check=False)
        directories = sorted(p.parent for p in (root / 'logs').glob('mouse-camera-*/state.bin')
                             if p.stat().st_mtime >= started)
    else:
        if not args.directory:
            parser.error('Provide a trace directory or use --play')
        directories = [args.directory]
    if not directories:
        raise ValueError('No trace was created. Use the diagnostic bb-probe build and a supported camera hook.')
    for directory in directories:
        print(f'Report: {collect(directory)}', flush=True)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, zipfile.BadZipFile) as error:
        print(f'Mouse trace: {error}', file=sys.stderr)
        raise SystemExit(1)

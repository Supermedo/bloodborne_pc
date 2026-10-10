#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Textures of the game's menu archives (menu/*.tpf.dcx), read and replaced in place.

A .tpf.dcx is a zlib-compressed (DCX "DFLT") TPF: a list of textures, here PS4 ones. Their
data is the GPU's tiled layout: BC blocks in 8x8-block tiles, Morton order inside a tile.
Replacing a texture keeps its size, so the TPF's offsets stay as they are.

Encoding is BC7 mode 6 (one RGBA line per 4x4 block, 16 levels), plenty for the button prompts
this is used for; plain Python, so packaged builds need nothing more. Decoding (tools, tests)
goes through Pillow's BC7 decoder."""
import io
import struct
import zlib

BC7 = 98  # DXGI_FORMAT_BC7_UNORM
BLOCK = 16  # bytes per BC7 block
DDS_HEADER = 148  # DDS magic, header and DX10 header before the blocks


def read_dcx(data):
    """(TPF bytes, DCX header) of a DCX "DFLT" archive."""
    if data[:4] != b'DCX\0' or data[0x28:0x2c] != b'DFLT':
        raise ValueError('not a DCX DFLT archive')
    size, = struct.unpack_from('>I', data, 0x1c)
    tpf = zlib.decompress(data[0x4c:])
    if len(tpf) != size:
        raise ValueError('DCX size mismatch')
    return tpf, data[:0x4c]


def write_dcx(tpf, header):
    packed = zlib.compress(tpf, 9)
    header = bytearray(header)
    struct.pack_into('>II', header, 0x1c, len(tpf), len(packed))
    return bytes(header) + packed


def textures(tpf):
    """name -> dict(offset, size, width, height, dxgi) of a PS4 TPF."""
    if tpf[:4] != b'TPF\0' or tpf[12] != 4:
        raise ValueError('not a PS4 TPF')
    count, = struct.unpack_from('<I', tpf, 8)
    utf16 = tpf[14] == 1
    out = {}
    for i in range(count):
        offset, size, _fmt, _type, _mips, _flags, width, height, _u1, _u2, name_at, _f2, dxgi = \
            struct.unpack_from('<IIBBBBHHiiiii', tpf, 16 + i * 0x24)
        if utf16:
            end = name_at
            while tpf[end:end + 2] != b'\0\0':
                end += 2
            name = tpf[name_at:end].decode('utf-16-le')
        else:
            name = tpf[name_at:tpf.index(b'\0', name_at)].decode('shift_jis')
        out[name] = dict(offset=offset, size=size, width=width, height=height, dxgi=dxgi)
    return out


def tile_order(blocks_x, blocks_y):
    """For each block of the tiled data, in order, its row-major index (None: padding)."""
    order = []
    for ty in range((blocks_y + 7) // 8):
        for tx in range((blocks_x + 7) // 8):
            for t in range(64):
                x = tx * 8 + ((t & 1) | (t >> 1 & 2) | (t >> 2 & 4))
                y = ty * 8 + ((t >> 1 & 1) | (t >> 2 & 2) | (t >> 3 & 4))
                order.append(y * blocks_x + x if x < blocks_x and y < blocks_y else None)
    return order


def _check(texture):
    if texture['dxgi'] != BC7:
        raise ValueError(f"DXGI format {texture['dxgi']}, expected BC7")
    return texture['width'] // 4, texture['height'] // 4


def blocks(tpf, texture):
    """The texture's BC7 blocks, row-major: a list of 16-byte strings."""
    bx, by = _check(texture)
    out = [b''] * (bx * by)
    at = texture['offset']
    for slot, index in enumerate(tile_order(bx, by)):
        if index is not None:
            out[index] = bytes(tpf[at + slot * BLOCK:at + (slot + 1) * BLOCK])
    return out


def put_blocks(tpf, texture, new):
    """Writes BC7 blocks into a bytearray TPF: `new` maps row-major indices to 16 bytes."""
    bx, by = _check(texture)
    at = texture['offset']
    for slot, index in enumerate(tile_order(bx, by)):
        if index in new:
            tpf[at + slot * BLOCK:at + (slot + 1) * BLOCK] = new[index]


def dds(width, height, row_blocks):
    """A DDS file (DX10 header, BC7) of row-major blocks."""
    data = b''.join(row_blocks)
    header = struct.pack('<4sIIIIIII44sIII20sIIIII', b'DDS ', 124, 0x81007, height, width,
                         len(data), 0, 1, b'\0' * 44, 32, 4, 0x30315844, b'\0' * 20, 0x1000,
                         0, 0, 0, 0)
    return header + struct.pack('<IIIII', BC7, 3, 0, 1, 0) + data


def dds_blocks(data):
    """The row-major blocks of a DDS file written by dds()."""
    data = data[DDS_HEADER:]
    return [data[i:i + BLOCK] for i in range(0, len(data), BLOCK)]


def decode(width, height, row_blocks):
    """RGBA Pillow image of row-major BC7 blocks."""
    from PIL import Image
    return Image.open(io.BytesIO(dds(width, height, row_blocks))).convert('RGBA')


_WEIGHTS = (0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64)


def _principal_axis(pixels, mean):
    cov = [[0.0] * 4 for _ in range(4)]
    for p in pixels:
        d = [p[c] - mean[c] for c in range(4)]
        for i in range(4):
            for j in range(4):
                cov[i][j] += d[i] * d[j]
    axis = [1.0, 1.0, 1.0, 1.0]
    for _ in range(8):
        axis = [sum(cov[i][j] * axis[j] for j in range(4)) for i in range(4)]
        norm = sum(a * a for a in axis) ** 0.5
        if norm < 1e-9:
            return [0.0, 0.0, 0.0, 0.0]
        axis = [a / norm for a in axis]
    return axis


def encode_block(pixels):
    """One BC7 mode 6 block of 16 RGBA tuples (row-major)."""
    # The colour of a see-through pixel does not show: it takes the block's visible colour, and
    # colour errors count as much as the pixel is opaque.
    visible = sum(p[3] for p in pixels)
    shown = [sum(p[c] * p[3] for p in pixels) / visible if visible else 0 for c in range(3)]
    pixels = [p if p[3] else (round(shown[0]), round(shown[1]), round(shown[2]), 0) for p in pixels]
    # Transparency counts more than colour: a see-through pixel turned faintly visible shows.
    weights = [(p[3] / 255, p[3] / 255, p[3] / 255, 4.0) for p in pixels]
    mean = [sum(p[c] for p in pixels) / 16 for c in range(4)]
    axis = _principal_axis(pixels, mean)
    project = [sum((p[c] - mean[c]) * axis[c] for c in range(4)) for p in pixels]
    # Endpoint candidates: the extremes along the principal axis, and the darkest/lightest
    # visible pixel with the alpha range.
    ends = [([mean[c] + min(project) * axis[c] for c in range(4)],
             [mean[c] + max(project) * axis[c] for c in range(4)])]
    shade = sorted(pixels, key=lambda p: (p[3] > 0, sum(p[:3])))
    alphas = [p[3] for p in pixels]
    ends.append((list(shade[0][:3]) + [min(alphas)], list(shade[-1][:3]) + [max(alphas)]))
    ends.append((list(shade[-1][:3]) + [min(alphas)], list(shade[0][:3]) + [max(alphas)]))
    def fit(lo, hi):
        """The best p-bits for these endpoints: (error, q0, q1, p0, p1, indices)."""
        best = None
        for p0 in (0, 1):
            for p1 in (0, 1):
                q0 = [min(127, max(0, round((v - p0) / 2))) for v in lo]
                q1 = [min(127, max(0, round((v - p1) / 2))) for v in hi]
                e0 = [q * 2 + p0 for q in q0]
                e1 = [q * 2 + p1 for q in q1]
                palette = [[((64 - w) * e0[c] + w * e1[c] + 32) >> 6 for c in range(4)] for w in _WEIGHTS]
                error, index = 0.0, []
                for p, wt in zip(pixels, weights):
                    dists = [sum((p[c] - col[c]) ** 2 * wt[c] for c in range(4)) for col in palette]
                    k = min(range(16), key=dists.__getitem__)
                    index.append(k)
                    error += dists[k]
                if best is None or error < best[0]:
                    best = (error, q0, q1, p0, p1, index)
        return best

    best = min((fit(lo, hi) for lo, hi in ends), key=lambda b: b[0])
    # Least squares: with the indices fixed, the endpoints that fit the pixels best.
    for _ in range(2):
        t = [_WEIGHTS[k] / 64 for k in best[5]]
        a00 = sum((1 - x) ** 2 for x in t)
        a01 = sum((1 - x) * x for x in t)
        a11 = sum(x * x for x in t)
        det = a00 * a11 - a01 * a01
        if abs(det) < 1e-9:
            break
        lo, hi = [], []
        for c in range(4):
            b0 = sum((1 - x) * p[c] for x, p in zip(t, pixels))
            b1 = sum(x * p[c] for x, p in zip(t, pixels))
            lo.append(min(255, max(0, (a11 * b0 - a01 * b1) / det)))
            hi.append(min(255, max(0, (a00 * b1 - a01 * b0) / det)))
        refined = fit(lo, hi)
        if refined[0] >= best[0]:
            break
        best = refined
    _, q0, q1, p0, p1, index = best
    # The first pixel's index has no top bit: swap the endpoints when it would need one.
    if index[0] >= 8:
        q0, q1, p0, p1 = q1, q0, p1, p0
        index = [15 - k for k in index]
    bits, shift = 1 << 6, 7
    for c in range(4):
        bits |= q0[c] << shift | q1[c] << (shift + 7)
        shift += 14
    bits |= p0 << shift | p1 << (shift + 1)
    shift += 2
    for k, value in enumerate(index):
        bits |= value << shift
        shift += 3 if k == 0 else 4
    return bits.to_bytes(16, 'little')


def encode(image):
    """Row-major BC7 blocks of an RGBA Pillow image whose sides are multiples of 4."""
    width, height = image.size
    raw = image.convert('RGBA').tobytes()
    data = [tuple(raw[i:i + 4]) for i in range(0, len(raw), 4)]
    return [encode_block([data[(by * 4 + y) * width + bx * 4 + x] for y in range(4) for x in range(4)])
            for by in range(height // 4) for bx in range(width // 4)]


def rect_blocks(texture, x, y, width, height):
    """Row-major indices of the blocks a pixel rectangle touches, and the block-aligned
    rectangle around it (x, y, width, height)."""
    bx = texture['width'] // 4
    x0, y0 = x // 4, y // 4
    x1, y1 = (x + width + 3) // 4, (y + height + 3) // 4
    which = [row * bx + col for row in range(y0, y1) for col in range(x0, x1)]
    return which, (x0 * 4, y0 * 4, (x1 - x0) * 4, (y1 - y0) * 4)

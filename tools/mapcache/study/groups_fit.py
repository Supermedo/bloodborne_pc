"""Which rule over draw groups explains the pieces the game drew in a frame?
Input: a geometry cache file (build_cache.py, version 3) and "RT geo drawn frame ..." log lines
(BB_RT_GEO_LOG=1). For every logged frame: the group bits every drawn piece has, how many
pieces each collision's display groups fail to show, and the smallest union of collision sets
that shows them all.
usage: groups_fit.py <map>.bbgeo <game log>"""
import re
import struct
import sys

path, log = sys.argv[1], sys.argv[2]
with open(path, 'rb') as f:
    magic, version, n_models, n_meshes, n_places, n_cols, _pad, v_at, i_at, size = struct.unpack('<8sIIIIII QQQ', f.read(56))
    f.seek(56 + 16 * n_models + 56 * n_meshes)
    places = [struct.unpack('<II12f6f8I', f.read(112)) for _ in range(n_places)]
    cols = [struct.unpack('<8I', f.read(32)) for _ in range(n_cols)]


def bits(g):
    v = 0
    for i, x in enumerate(g):
        v |= x << (32 * i)
    return v


def show(v):
    return ','.join(str(i) for i in range(256) if v >> i & 1) or '-'


pg = [bits(p[20:28]) for p in places]
cg = [bits(c) for c in cols]
name = path.replace('\\', '/').split('/')[-1]
frames = []
for line in open(log, encoding='utf-8', errors='replace'):
    m = re.match(r'RT geo drawn frame (\d+) (\S+): (.*)', line)
    if m and m.group(2) == name:
        frames.append((int(m.group(1)), sorted({int(x) for x in m.group(3).split()})))
print(len(frames), 'frames logged for', name, '-', n_places, 'placements,', n_cols, 'collision sets')
for frame, drawn in frames[-4:]:
    every = (1 << 256) - 1
    any_bit = 0
    for p in drawn:
        every &= pg[p]
        any_bit |= pg[p]
    empty = sum(1 for p in drawn if pg[p] == 0)
    print(f'frame {frame}: {len(drawn)} placements drawn; bits all have [{show(every)}]; {empty} have no draw group')
    miss = sorted((sum(1 for p in drawn if not pg[p] & c), k) for k, c in enumerate(cg))
    for n, k in miss[:6]:
        print(f'   collision set {k} [{show(cg[k])[:60]}] fails to show {n} of {len(drawn)}')
    # Greedy union of collision sets that shows every drawn piece.
    left = set(drawn)
    chosen = []
    while left and len(chosen) < 8:
        best = max(range(len(cg)), key=lambda k: sum(1 for p in left if pg[p] & cg[k]) if bin(cg[k]).count('1') <= 100 else -1)
        got = {p for p in left if pg[p] & cg[best]}
        if not got:
            break
        chosen.append(best)
        left -= got
    union = 0
    for k in chosen:
        union |= cg[k]
    shown = sum(1 for g in pg if g & union)
    print(f'   greedy union of sets {chosen} [{show(union)}] leaves {len(left)} unexplained and would show {shown} of {n_places} placements')
    # One single bit that every drawn piece has?
    single = [b for b in range(256) if every >> b & 1]
    for b in single[:8]:
        print(f'   bit {b}: on {sum(1 for g in pg if g >> b & 1)} placements of the map')

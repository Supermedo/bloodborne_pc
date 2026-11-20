"""Memory planning from out_near.txt: BLAS = 36 B x unique-model tris, TLAS = 64 B x instances (parts in radius).
Flat variant (no instancing, world-space BLAS) = 36 B x placed tris. MB = 10^6 bytes.

    python -B memory.py out_near.txt
"""
import re
import sys

mix_names = None
for line in open(sys.argv[1], encoding='utf-8'):
    line = line.rstrip('\n')
    if line.startswith('## near'):
        print('\n' + line)
        continue
    if line.startswith('radius |'):
        mix_names = line.split(' | ')[3:]
        print('radius | parts | ' + ' | '.join('%s inst. MB (flat MB)' % n for n in mix_names))
        continue
    if mix_names and re.match(r'^(\d+|all) \|', line):
        f = line.split(' | ')
        parts = int(f[1])
        cells = []
        for c in f[3:]:
            pl, un, _nb = [float(x) for x in c.split(' / ')]
            cells.append('%.0f (%.0f)' % (un * 36 + parts * 64 / 1e6, pl * 36))
        print('%s | %d | %s' % (f[0], parts, ' | '.join(cells)))

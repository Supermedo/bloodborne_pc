#!/usr/bin/env python3
"""Per-function PM4 signatures in an objdump -M intel listing (the eboot's libGnm region).

Usage: gnm_sigs.py region.asm callgraph.txt > sigs.txt
callgraph.txt: tools/re/callgraph.py output (function starts, sizes, callers).
For every function: the packets it writes as immediates (opcode, count, first register).
"""
import re, sys, collections
sys.path.insert(0, __file__.rsplit('/', 1)[0])
from pm4_scan_names import OPCODES, reg_name

store = re.compile(r'^\s*([0-9a-f]+):\s+(?:[0-9a-f]{2} )+\s*mov\s+DWORD PTR \[(\w+)(?:\+0x([0-9a-f]+))?\],0x([0-9a-f]+)$')

def main():
    funcs = []
    for line in open(sys.argv[2]):
        p = line.split()
        funcs.append((int(p[0], 16), int(p[2]), int(p[4]), int(p[6])))
    lines = collections.defaultdict(list)
    starts = sorted(f[0] for f in funcs)
    import bisect
    info = {f[0]: f for f in funcs}
    for line in open(sys.argv[1]):
        m = store.match(line)
        if not m: continue
        va = int(m.group(1), 16)
        k = bisect.bisect_right(starts, va) - 1
        if k < 0: continue
        fn = starts[k]
        if va >= fn + info[fn][1]: continue
        lines[fn].append((m.group(2), int(m.group(3) or '0', 16), int(m.group(4), 16)))
    for fn in starts:
        st = lines.get(fn, [])
        by = {(b, o): v for b, o, v in st}
        sig = []
        for b, o, v in st:
            if (v >> 30) == 3 and ((v >> 8) & 0xff) in OPCODES and (v & 0xfd) == 0:
                op = (v >> 8) & 0xff; cnt = (v >> 16) & 0x3fff
                name = OPCODES[op]
                nxt = by.get((b, o + 4))
                if name.startswith('SET_') and nxt is not None:
                    sig.append(f'{name}({reg_name(name, nxt)}x{cnt})')
                else:
                    sig.append(f'{name}/{cnt}')
        f = info[fn]
        if sig or f[3]:
            print(f'0x{fn:08x} size {f[1]:5d} sites {f[2]:4d} ext {f[3]:3d}: ' + ' '.join(sig))

main()

#!/usr/bin/env python3
"""Direct call graph (E8/E9 rel32 to function starts from .eh_frame_hdr) for a target address range.

Usage: callgraph.py out/eboot.elf LO HI [out.txt]
Prints, for each function in [LO, HI), its size, the number of call sites and of calling functions,
split into callers inside and outside the range.
"""
import struct, sys, bisect, collections

def load(path):
    f = open(path, 'rb').read()
    phoff, = struct.unpack_from('<Q', f, 0x20); phnum, = struct.unpack_from('<H', f, 0x38)
    segs = [struct.unpack_from('<IIQQQQQQ', f, phoff + 56 * i) for i in range(phnum)]
    text = next(s for s in segs if s[0] == 1 and s[1] & 1)
    ehh = next(s for s in segs if s[0] == 0x6474e550)
    toff, tva, tsz = text[2], text[3], text[5]
    h, hva = ehh[2], ehh[3]
    count, = struct.unpack_from('<i', f, h + 8)
    funcs = []
    for i in range(count):
        loc, fde = struct.unpack_from('<ii', f, h + 12 + 8 * i)
        fde_off = hva + fde - tva + toff
        size, = struct.unpack_from('<i', f, fde_off + 12)
        funcs.append((hva + loc, size))
    funcs.sort()
    return f[toff:toff + tsz], tva, funcs

def main():
    code, tva, funcs = load(sys.argv[1])
    lo, hi = int(sys.argv[2], 0), int(sys.argv[3], 0)
    starts = [s for s, _ in funcs]
    startset = set(starts)
    def owner(va):
        k = bisect.bisect_right(starts, va) - 1
        return starts[k] if k >= 0 and va < funcs[k][0] + funcs[k][1] else None
    sites = collections.defaultdict(list)
    for i in range(len(code) - 5):
        op = code[i]
        if op != 0xe8 and op != 0xe9:
            continue
        rel, = struct.unpack_from('<i', code, i + 1)
        tgt = tva + i + 5 + rel
        if not (lo <= tgt < hi) or tgt not in startset:
            continue
        src = tva + i
        fn = owner(src)
        if fn is None:
            continue
        sites[tgt].append((src, fn, op))
    sizes = dict(funcs)
    out = open(sys.argv[4], 'w') if len(sys.argv) > 4 else None
    inner = [s for s in starts if lo <= s < hi]
    called_outside = 0
    for s in inner:
        l = sites.get(s, [])
        ext = {fn for _, fn, _ in l if not (lo <= fn < hi)}
        inn = {fn for _, fn, _ in l if lo <= fn < hi}
        if ext: called_outside += 1
        line = f'0x{s:08x} size {sizes[s]:5d} sites {len(l):5d} ext_fns {len(ext):5d} int_fns {len(inn):3d}'
        if out: out.write(line + ' ext ' + ' '.join(f'0x{x:x}' for x in sorted(ext)[:20]) + '\n')
    allext = {fn for s in inner for _, fn, _ in sites.get(s, []) if not (lo <= fn < hi)}
    print(f'functions in range: {len(inner)}, called from outside: {called_outside}, '
          f'distinct outside callers: {len(allext)}')
    if out:
        with open(sys.argv[4] + '.callers', 'w') as o:
            for fn in sorted(allext):
                o.write(f'0x{fn:08x} {sizes[fn]}\n')

main()

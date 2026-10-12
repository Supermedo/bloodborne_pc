#!/usr/bin/env python3
"""Find PM4 type-3 packet headers built as immediates in the eboot's code, grouped by function.

Function bounds come from .eh_frame_hdr (the eboot is stripped). Usage: pm4_scan.py out/eboot.elf
"""
import struct, sys, collections

OPCODES = {0x10:'NOP',0x12:'CLEAR_STATE',0x15:'DISPATCH_DIRECT',0x16:'DISPATCH_INDIRECT',0x22:'COND_EXEC',
 0x26:'INDEX_BASE',0x27:'DRAW_INDEX_2',0x28:'CONTEXT_CONTROL',0x2a:'INDEX_TYPE',0x2c:'DRAW_INDIRECT_MULTI',
 0x2d:'DRAW_INDEX_AUTO',0x2f:'NUM_INSTANCES',0x33:'INDIRECT_BUFFER_CONST',0x34:'STRMOUT_BUFFER_UPDATE',
 0x35:'DRAW_INDEX_OFFSET_2',0x37:'WRITE_DATA',0x38:'DRAW_INDEX_INDIRECT_MULTI',0x39:'MEM_SEMAPHORE',
 0x3c:'WAIT_REG_MEM',0x3f:'INDIRECT_BUFFER',0x40:'COPY_DATA',0x42:'PFP_SYNC_ME',0x46:'EVENT_WRITE',
 0x47:'EVENT_WRITE_EOP',0x48:'EVENT_WRITE_EOS',0x49:'RELEASE_MEM',0x4a:'PREAMBLE_CNTL',0x50:'DMA_DATA',
 0x58:'ACQUIRE_MEM',0x59:'REWIND',0x68:'SET_CONFIG_REG',0x69:'SET_CONTEXT_REG',0x76:'SET_SH_REG',
 0x79:'SET_UCONFIG_REG',0x11:'SET_BASE',0x20:'SET_PREDICATION',0x23:'PRED_EXEC',0x24:'DRAW_INDIRECT',
 0x25:'DRAW_INDEX_INDIRECT',0x13:'INDEX_BUFFER_SIZE',0x81:'WRITE_CONST_RAM',0x83:'DUMP_CONST_RAM',
 0x84:'INCREMENT_CE_COUNTER',0x85:'INCREMENT_DE_COUNTER',0x86:'WAIT_ON_CE_COUNTER',0x88:'WAIT_ON_DE_COUNTER_DIFF',
 0x80:'LOAD_CONST_RAM',0x9d:'DRAW_INDEX_INDIRECT_COUNT_MULTI'}

def main(path):
    f = open(path,'rb').read()
    # program headers
    phoff, = struct.unpack_from('<Q', f, 0x20); phnum, = struct.unpack_from('<H', f, 0x38)
    segs = []
    for i in range(phnum):
        p_type,p_flags,p_off,p_vaddr,_,p_filesz,_,_ = struct.unpack_from('<IIQQQQQQ', f, phoff+56*i)
        segs.append((p_type,p_flags,p_off,p_vaddr,p_filesz))
    text = next(s for s in segs if s[0]==1 and s[1]&1)
    ehh = next(s for s in segs if s[0]==0x6474e550)
    toff, tva, tsz = text[2], text[3], text[4]
    def va2off(va): return va - tva + toff
    # eh_frame_hdr: version, eh_frame_ptr_enc, fde_count_enc, table_enc
    h = ehh[2]; hva = ehh[3]
    ver, pe, ce, te = f[h:h+4]
    assert ver == 1 and te == 0x3b, (ver, pe, ce, te)  # datarel sdata4
    pos = h+4
    pos += 4  # eh_frame_ptr (sdata4 pcrel)
    count, = struct.unpack_from('<i', f, pos); pos += 4
    funcs = []
    for i in range(count):
        loc, fde = struct.unpack_from('<ii', f, pos+8*i)
        start = hva + loc; fde_off = va2off(hva + fde)
        # FDE: length, CIE ptr, pc_begin (pcrel sdata4 assumed), pc_range
        pc_range, = struct.unpack_from('<i', f, fde_off+12)
        funcs.append((start, pc_range))
    funcs.sort()
    starts = [s for s,_ in funcs]
    import bisect
    code = f[toff:toff+tsz]
    hits = collections.defaultdict(collections.Counter)
    total = collections.Counter()
    i = code.find(b'\xc0', 3)
    # scan every 4-byte window whose top byte is 0xC0 (type 3, predicate 0, shader type 0/1 in bit 1)
    for m in range(3, len(code)):
        b3 = code[m]
        if b3 != 0xc0 and b3 != 0xc1: continue
        b0 = code[m-3]
        if b0 not in (0, 2): continue          # bit 1 = shader type (compute)
        op = code[m-2]
        if op not in OPCODES: continue
        cnt = code[m-1]
        if cnt > 0x3f and op not in (0x10,): continue
        s = m - 3
        ok = False
        if s >= 1 and 0xb8 <= code[s-1] <= 0xbf: ok = True
        if s >= 2 and code[s-2] == 0xc7 and (code[s-1] >> 6) == 0 and (code[s-1] & 7) not in (4, 5): ok = True
        if s >= 3 and code[s-3] == 0xc7 and (code[s-2] >> 6) == 1 and (code[s-2] & 7) != 4: ok = True
        if s >= 4 and code[s-4] == 0xc7 and (code[s-3] >> 6) == 1 and (code[s-3] & 7) == 4: ok = True
        if s >= 2 and code[s-2] == 0x81 and (code[s-1] & 0xf8) == 0xc8: ok = True
        if s >= 1 and code[s-1] == 0x0d: ok = True
        if not ok: continue
        va = tva + m - 3
        k = bisect.bisect_right(starts, va) - 1
        if k < 0 or va >= funcs[k][0] + funcs[k][1]: continue
        hits[funcs[k][0]][OPCODES[op]] += 1
        total[OPCODES[op]] += 1
    print(f'functions (eh_frame): {len(funcs)}; functions with PM4 header immediates: {len(hits)}')
    print('by opcode:', ', '.join(f'{k} {v}' for k,v in total.most_common()))
    # per-opcode: how many functions
    per = collections.Counter()
    for fn,c in hits.items():
        for k in c: per[k] += 1
    print('functions per opcode:', ', '.join(f'{k} {v}' for k,v in per.most_common()))
    big = sorted(hits.items(), key=lambda kv: -sum(kv[1].values()))[:40]
    sizes = dict(funcs)
    for fn,c in big:
        print(f'0x{fn:08x} size {sizes[fn]:6d}: ' + ', '.join(f'{k} {v}' for k,v in c.most_common(8)))
    with open(sys.argv[2] if len(sys.argv)>2 else '/dev/null','w') as o:
        for fn,c in sorted(hits.items()):
            o.write(f'0x{fn:08x} {sizes[fn]} ' + ' '.join(f'{k}={v}' for k,v in sorted(c.items())) + '\n')

main(sys.argv[1])

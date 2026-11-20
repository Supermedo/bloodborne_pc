"""Bloodborne EMEVD ('EVD\\0', version 0xCC, 64-bit) reader with event-parameter substitution.

read_emevd(bytes) -> {event id: {'instr': [(bank, id, args bytes)], 'params': [(instr idx, target, source, size)]}}
resolved_calls(events) -> list of (caller event, target event, bank, id, args bytes) where the args of
    every instruction of an event started by 2000[00] (or 2000[06]) have the parameters filled in.
Instructions of events that are not initialised with parameters are reported as-is (event id = their own).
"""
import struct


def read_emevd(b):
    if b[:4] != b'EVD\0':
        raise ValueError('not EMEVD')
    h = struct.unpack_from('<16q', b, 0x10)
    ev_cnt, ev_off, _ic, in_off = h[0], h[1], h[2], h[3]
    par_cnt, par_off = h[8], h[9]
    args_off = h[13]
    psize = 0x20
    events = {}
    for e in range(ev_cnt):
        eid, icnt, ioff, pcnt, poff, rest, _pad = struct.unpack_from('<qqqqqii', b, ev_off + e * 0x30)
        ins = []
        for k in range(icnt):
            bank, iid, alen, aoff = struct.unpack_from('<iiqq', b, in_off + ioff + k * 0x20)
            ins.append((bank, iid, bytes(b[args_off + aoff:args_off + aoff + alen])))
        pars = []
        for k in range(pcnt):
            ii, tgt, src, size = struct.unpack_from('<qqqi', b, par_off + poff + k * psize)
            pars.append((ii, tgt, src, size))
        events[eid] = {'instr': ins, 'params': pars, 'rest': rest}
    return events


def instances(events, extra=None):
    """Yield (event id, slot or None, init args or None, [(bank, id, args)]) for each running instance:
    events with no params run as written; events with params once per 2000[00]/2000[06] call
    (in this file or `extra` files' events)."""
    pools = [events] + (extra or [])
    inits = []   # (target event, param bytes)
    for ev in pools:
        for eid, e in ev.items():
            for bank, iid, a in e['instr']:
                if bank == 2000 and iid == 0 and len(a) >= 8:
                    _slot, tgt = struct.unpack_from('<iI', a, 0)
                    inits.append((tgt, a[8:], eid))
                elif bank == 2000 and iid == 6 and len(a) >= 8:
                    _unk, tgt = struct.unpack_from('<iI', a, 0)
                    inits.append((tgt, a[8:], eid))
    for eid, e in events.items():
        if not e['params']:
            yield eid, None, e['instr']
    for tgt, pb, caller in inits:
        for ev in pools:
            e = ev.get(tgt)
            if e is None or not e['params']:
                continue
            ins = [list(x) for x in e['instr']]
            for ii, t, s, size in e['params']:
                if ii < len(ins):
                    a = bytearray(ins[ii][2])
                    a[t:t + size] = pb[s:s + size].ljust(size, b'\0')
                    ins[ii][2] = bytes(a)
            yield tgt, caller, [tuple(x) for x in ins]
            break

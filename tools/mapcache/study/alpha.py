"""Alpha class of a FLVER material from its MTD (mtd/allmaterialbnd.mtdbnd.dcx).

test  = cut-out: g_AlphaRef.x > 0 and g_BlendMode 0 (normal) or 1 (TexEdge)
blend = g_BlendMode 2 (alpha blend; its AlphaRef is 0 or 1, i.e. no real clip)
add   = g_BlendMode >= 3 (water / add / sub ...)
opaque otherwise. MTDs not found in the archive fall back to the name: _e / Edge -> test, _al / _Alp -> blend.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mtd_params  # noqa: E402

_CACHE = {}


def mtd_alpha(dv):
    if not _CACHE:
        for k, v in mtd_params.mtd_table(dv).items():
            _CACHE[k.lower()] = v
    return _CACHE


def alpha_class(mtd_path, amap):
    base = mtd_path.replace('\\', '/').split('/')[-1].lower()
    v = amap.get(base)
    if v is not None and v['blend'] is not None:
        bm = v['blend'][0]
        ref = v['alpharef'][0] if v['alpharef'] else 0
        if bm in (0, 1) and (ref > 0 or bm == 1):
            return 'test'
        if bm == 2:
            return 'blend'
        if bm >= 3:
            return 'add/water'
        return 'opaque'
    if re.search(r'_e(_|\.|$)|edge', base):
        return 'test?'
    if re.search(r'_al(_|\.|$)|_alp', base):
        return 'blend?'
    return 'opaque?' if v is None else 'opaque'


def is_alpha_test(mtd_path, amap):
    return alpha_class(mtd_path, amap).startswith('test')

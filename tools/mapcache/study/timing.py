"""Pure-Python parse time + peak working set for one map: (a) DCX inflate only, (b) header stats (flstat),
(c) full mapcache.load_map (positions + LOD-0 indices). Run each mode in its own process for a clean peak.

    python -B timing.py <dvdroot> <map> inflate|stat|full
"""
import ctypes
import ctypes.wintypes as wt
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..'))
import mapcache as mc  # noqa: E402
import flstat  # noqa: E402


class PMC(ctypes.Structure):
    _fields_ = [('cb', wt.DWORD), ('PageFaultCount', wt.DWORD), ('PeakWorkingSetSize', ctypes.c_size_t),
                ('WorkingSetSize', ctypes.c_size_t), ('QuotaPeakPagedPoolUsage', ctypes.c_size_t),
                ('QuotaPagedPoolUsage', ctypes.c_size_t), ('QuotaPeakNonPagedPoolUsage', ctypes.c_size_t),
                ('QuotaNonPagedPoolUsage', ctypes.c_size_t), ('PagefileUsage', ctypes.c_size_t),
                ('PeakPagefileUsage', ctypes.c_size_t)]


def peak_mb():
    c = PMC()
    c.cb = ctypes.sizeof(PMC)
    k = ctypes.WinDLL('kernel32')
    k.GetCurrentProcess.restype = wt.HANDLE
    psapi = ctypes.WinDLL('psapi')
    psapi.GetProcessMemoryInfo.argtypes = [wt.HANDLE, ctypes.POINTER(PMC), wt.DWORD]
    psapi.GetProcessMemoryInfo(k.GetCurrentProcess(), ctypes.byref(c), c.cb)
    return c.PeakWorkingSetSize / 2**20, c.PeakPagefileUsage / 2**20


dv, mp, mode = sys.argv[1], sys.argv[2], sys.argv[3]
base = peak_mb()
d = os.path.join(dv, 'map', mp)
files = [f for f in os.listdir(d) if f.endswith('.flver.dcx')]
size = sum(os.path.getsize(os.path.join(d, f)) for f in files)
t = time.perf_counter()
if mode == 'inflate':
    n = sum(len(mc.dcx_read(os.path.join(d, f))) for f in files)
elif mode == 'stat':
    n = 0
    keep = {}
    for f in files:
        keep[f] = flstat.stat_flver(mc.dcx_read(os.path.join(d, f)), f)
        n += 1
elif mode == 'full':
    msb, pieces, flvers, fails = mc.load_map(dv, mp)
    n = len(flvers)
dt = time.perf_counter() - t
print('%s %s: %d files, %.1f MB dcx, %.2f s, %.1f MB/s, peak working set %.0f MB (python baseline %.0f), peak private %.0f MB, result %d' % (
    mp, mode, len(files), size / 2**20, dt, size / 2**20 / dt, peak_mb()[0], base[0], peak_mb()[1], n))

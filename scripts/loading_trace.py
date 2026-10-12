#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compare Continue and respawn loading without changing game timing or settings."""
import argparse
import csv
import ctypes
from ctypes import wintypes as wt
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import time
import zipfile


class ProcessEntry(ctypes.Structure):
    _fields_ = [('size', wt.DWORD), ('usage', wt.DWORD), ('pid', wt.DWORD),
                ('heap', ctypes.c_size_t), ('module', wt.DWORD), ('threads', wt.DWORD),
                ('parent', wt.DWORD), ('priority', wt.LONG), ('flags', wt.DWORD),
                ('exe', wt.WCHAR * 260)]


class ThreadEntry(ctypes.Structure):
    _fields_ = [('size', wt.DWORD), ('usage', wt.DWORD), ('tid', wt.DWORD),
                ('pid', wt.DWORD), ('priority', wt.LONG), ('delta', wt.LONG),
                ('flags', wt.DWORD)]


class IoCounters(ctypes.Structure):
    _fields_ = [(name, ctypes.c_ulonglong) for name in
                ('read_ops', 'write_ops', 'other_ops', 'read_bytes', 'write_bytes', 'other_bytes')]


def descendants(root_pid, entries):
    """Only this launch's process tree; leave other running games out of the report."""
    found = {root_pid}
    pending = {pid: parent for pid, parent, _name in entries}
    while True:
        added = {pid for pid, parent in pending.items() if parent in found and pid not in found}
        if not added:
            return found
        found.update(added)


class WindowsSampler:
    """Read-only process/thread counters. No suspension, memory reads or input injection."""
    INVENTORY_INTERVAL = .5
    NAME_RETRY_INTERVAL = 1.0
    NAME_RETRIES = 3

    def __init__(self):
        if sys.platform != 'win32':
            raise OSError('Loading diagnostics require Windows.')
        self.k = ctypes.WinDLL('kernel32', use_last_error=True)
        self.u = ctypes.WinDLL('user32', use_last_error=True)
        signatures = {
            'CreateToolhelp32Snapshot': (wt.HANDLE, [wt.DWORD, wt.DWORD]),
            'Process32FirstW': (wt.BOOL, [wt.HANDLE, ctypes.POINTER(ProcessEntry)]),
            'Process32NextW': (wt.BOOL, [wt.HANDLE, ctypes.POINTER(ProcessEntry)]),
            'Thread32First': (wt.BOOL, [wt.HANDLE, ctypes.POINTER(ThreadEntry)]),
            'Thread32Next': (wt.BOOL, [wt.HANDLE, ctypes.POINTER(ThreadEntry)]),
            'OpenProcess': (wt.HANDLE, [wt.DWORD, wt.BOOL, wt.DWORD]),
            'OpenThread': (wt.HANDLE, [wt.DWORD, wt.BOOL, wt.DWORD]),
            'GetProcessIdOfThread': (wt.DWORD, [wt.HANDLE]),
            'CloseHandle': (wt.BOOL, [wt.HANDLE]),
            'LocalFree': (wt.HANDLE, [wt.HANDLE]),
            'GetProcessIoCounters': (wt.BOOL, [wt.HANDLE, ctypes.POINTER(IoCounters)]),
        }
        for name, (result, args) in signatures.items():
            fn = getattr(self.k, name)
            fn.restype, fn.argtypes = result, args
        for name in ('GetProcessTimes', 'GetThreadTimes'):
            fn = getattr(self.k, name)
            fn.restype = wt.BOOL
            fn.argtypes = [wt.HANDLE, *([ctypes.POINTER(wt.FILETIME)] * 4)]
        self.k.GetThreadDescription.restype = wt.LONG
        self.k.GetThreadDescription.argtypes = [wt.HANDLE, ctypes.POINTER(ctypes.c_void_p)]
        self.u.GetAsyncKeyState.argtypes = [ctypes.c_int]
        self.u.GetAsyncKeyState.restype = wt.SHORT
        self.u.GetForegroundWindow.restype = wt.HWND
        self.u.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
        self.processes, self.threads = {}, {}
        self._inventory = None
        self._inventory_at = 0
        self._name_retries = {}
        self.f8_down = False

    def inventory(self):
        snapshot = self.k.CreateToolhelp32Snapshot(2 | 4, 0)
        if snapshot == ctypes.c_void_p(-1).value:
            raise ctypes.WinError(ctypes.get_last_error())
        processes, threads = [], []
        try:
            p = ProcessEntry(size=ctypes.sizeof(ProcessEntry))
            ok = self.k.Process32FirstW(snapshot, ctypes.byref(p))
            while ok:
                processes.append((p.pid, p.parent, p.exe))
                ok = self.k.Process32NextW(snapshot, ctypes.byref(p))
            t = ThreadEntry(size=ctypes.sizeof(ThreadEntry))
            ok = self.k.Thread32First(snapshot, ctypes.byref(t))
            while ok:
                threads.append((t.tid, t.pid))
                ok = self.k.Thread32Next(snapshot, ctypes.byref(t))
        finally:
            self.k.CloseHandle(snapshot)
        return processes, threads

    def times(self, handle, thread=False):
        values = [wt.FILETIME() for _ in range(4)]
        fn = self.k.GetThreadTimes if thread else self.k.GetProcessTimes
        if not fn(handle, *(ctypes.byref(value) for value in values)):
            return None
        ticks = [(v.dwHighDateTime << 32) | v.dwLowDateTime for v in values]
        if ticks[1]:
            return None  # a cached handle to an exited process/thread must not hide ID reuse
        return ticks[0], ticks[2] / 10000, ticks[3] / 10000  # identity, kernel ms, user ms

    def thread_name(self, handle):
        text = ctypes.c_void_p()
        if self.k.GetThreadDescription(handle, ctypes.byref(text)) < 0 or not text.value:
            return ''
        try:
            return ctypes.wstring_at(text)
        finally:
            self.k.LocalFree(text)

    def sample(self, root_pid, names=('bb-probe.exe',)):
        now = time.perf_counter()
        if self._inventory is None or now - self._inventory_at >= self.INVENTORY_INTERVAL:
            self._inventory = self.inventory()
            self._inventory_at = now
        processes, threads = self._inventory
        related = descendants(root_pid, processes)
        active = {pid for pid, _parent, name in processes if pid in related and name.lower() in names}
        process_rows, thread_rows = [], []
        for pid in active:
            handle = self.processes.get(pid)
            if not handle:
                handle = self.k.OpenProcess(0x1000 | 0x400, False, pid)
                if not handle:
                    continue  # process exited between the snapshot and OpenProcess
                self.processes[pid] = handle
            cpu = self.times(handle)
            io = IoCounters()
            if cpu:
                counters = ([getattr(io, name) for name, _ in io._fields_]
                            if self.k.GetProcessIoCounters(handle, ctypes.byref(io)) else [''] * 6)
                process_rows.append([pid, *cpu, *counters])
            else:
                self.k.CloseHandle(self.processes.pop(pid))
        live_threads = {tid for tid, pid in threads if pid in active}
        for tid, pid in threads:
            if tid not in live_threads:
                continue
            cached = self.threads.get(tid)
            if not cached:
                handle = self.k.OpenThread(0x800, False, tid)
                if not handle:
                    continue
                if self.k.GetProcessIdOfThread(handle) != pid:
                    self.k.CloseHandle(handle)
                    continue  # ID reused since the last inventory, by another process
                cached = (handle, self.thread_name(handle))
                self.threads[tid] = cached
                self._name_retries[tid] = (now + self.NAME_RETRY_INTERVAL, self.NAME_RETRIES)
            elif not cached[1]:
                retry_at, remaining = self._name_retries[tid]
                if remaining and now >= retry_at:
                    cached = (cached[0], self.thread_name(cached[0]))
                    self.threads[tid] = cached
                    self._name_retries[tid] = (now + self.NAME_RETRY_INTERVAL, remaining - 1)
            cpu = self.times(cached[0], thread=True)
            if cpu:
                thread_rows.append([pid, tid, cached[1], *cpu])
            else:
                self.k.CloseHandle(self.threads.pop(tid)[0])
                self._name_retries.pop(tid, None)
        for pid in set(self.processes) - active:
            self.k.CloseHandle(self.processes.pop(pid))
        for tid in set(self.threads) - live_threads:
            self.k.CloseHandle(self.threads.pop(tid)[0])
            self._name_retries.pop(tid, None)
        return process_rows, thread_rows

    def marker(self, active):
        down = bool(self.u.GetAsyncKeyState(0x77) & 0x8000)
        foreground = wt.DWORD()
        self.u.GetWindowThreadProcessId(self.u.GetForegroundWindow(), ctypes.byref(foreground))
        marker = down and not self.f8_down and foreground.value in active
        self.f8_down = down
        return marker

    def close(self):
        for handle in self.processes.values():
            self.k.CloseHandle(handle)
        for handle, _name in self.threads.values():
            self.k.CloseHandle(handle)
        self.processes.clear()
        self.threads.clear()
        self._name_retries.clear()
        self._inventory = None


REPORT_FILES = ('metadata.json', 'markers.csv', 'process.csv', 'threads.csv', 'game.log',
                'frames.csv', 'waits.log', 'readbacks.csv')


def collect(directory):
    directory = Path(directory)
    target = directory.with_suffix('.zip')
    with zipfile.ZipFile(target, 'x', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for name in REPORT_FILES:
            path = directory / name
            if path.is_file():
                archive.write(path, name)
    return target


def capture(root, command=None, names=('bb-probe.exe',), interval=.1):
    """Use existing frame/wait diagnostics and sample this launch's native process tree."""
    root = Path(root).resolve()
    sampler = WindowsSampler()
    directory = Path(os.environ.get('BB_DATA_DIR', root)) / 'logs' / (
        'loading-' + datetime.now().strftime('%Y%m%d-%H%M%S-%f'))
    directory.mkdir(parents=True, exist_ok=False)
    directory = directory.resolve()
    env = {**os.environ, 'BB_FRAME_STATS': '1', 'BB_WAIT_TRACE': '1',
           'BB_FRAME_LOG': str(directory / 'frames.csv'),
           'BB_WAIT_LOG': str(directory / 'waits.log'),
           'BB_READBACK_LOG': str(directory / 'readbacks.csv')}
    metadata = {'schema': 2, 'started': datetime.now().astimezone().isoformat(),
                'sample_interval_s': interval, 'marker_poll_interval_s': .01,
                'inventory_interval_s': sampler.INVENTORY_INTERVAL,
                'empty_name_retry_interval_s': sampler.NAME_RETRY_INTERVAL,
                'empty_name_retries': sampler.NAME_RETRIES,
                'markers': [], 'processes': [],
                'clock': 'process.csv, threads.csv, markers.csv and game.log: seconds since capture start',
                'native_clocks': 'frames.csv: since first flip; readbacks.csv: since logger creation; waits.log: 5-second report windows',
                'io_counters': 'Logical process I/O, including cached file access and pipes; not physical disk throughput.',
                'purpose': 'Compare loading paths. No respawn fix or game timing change is applied.'}
    metadata['collector_sha256'] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    if (root / 'BUILD-INFO.txt').is_file():
        metadata['build_info'] = (root / 'BUILD-INFO.txt').read_text(encoding='utf-8')[:8192]
    start = time.perf_counter()
    log_bytes = 0
    sample_wall_s = 0
    sample_max_wall_s = 0
    sample_count = 0

    def drain(process):
        nonlocal log_bytes
        log = None
        try:
            try:
                log = (directory / 'game.log').open('w', encoding='utf-8', buffering=1)
            except OSError as error:
                metadata['game_log_error'] = str(error)
            for raw in iter(process.stdout.readline, b''):
                log_bytes += len(raw)
                if log is not None and log_bytes <= 16 * 1024 * 1024:
                    try:
                        log.write(f'{time.perf_counter() - start:.4f} ' + raw.decode('utf-8', errors='replace'))
                    except OSError as error:
                        metadata['game_log_error'] = str(error)
                        # Keep draining the pipe even when the log volume fills up.
                        log_bytes = 16 * 1024 * 1024 + 1
        finally:
            if log is not None:
                log.close()

    try:
        with (directory / 'process.csv').open('w', encoding='utf-8', newline='') as pfile, \
             (directory / 'threads.csv').open('w', encoding='utf-8', newline='') as tfile, \
             (directory / 'markers.csv').open('w', encoding='utf-8', newline='') as mfile:
            pw, tw, mw = csv.writer(pfile), csv.writer(tfile), csv.writer(mfile)
            pw.writerow(['t_s', 'pid', 'creation_ticks', 'kernel_ms', 'user_ms',
                         *[name for name, _ in IoCounters._fields_]])
            tw.writerow(['t_s', 'pid', 'tid', 'name', 'creation_ticks', 'kernel_ms', 'user_ms'])
            mw.writerow(['t_s', 'index'])
            process = subprocess.Popen(command or [str(root / 'Bloodborne.exe'), '--play'],
                                       cwd=root, env=env, stdin=subprocess.DEVNULL,
                                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                       creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
            reader = threading.Thread(target=drain, args=(process,), daemon=True)
            reader.start()
            try:
                next_sample, active = 0, set()
                while process.poll() is None:
                    if time.perf_counter() >= next_sample:
                        sampled = time.perf_counter()
                        prows, trows = sampler.sample(process.pid, names)
                        sample_wall = time.perf_counter() - sampled
                        sample_wall_s += sample_wall
                        sample_max_wall_s = max(sample_max_wall_s, sample_wall)
                        sample_count += 1
                        stamp = round(sampled - start, 4)
                        pw.writerows([[stamp, *row] for row in prows])
                        tw.writerows([[stamp, *row] for row in trows])
                        active = {row[0] for row in prows}
                        metadata['processes'] = sorted(set(metadata['processes']) | active)
                        next_sample = sampled + interval
                    if sampler.marker(active):
                        stamp = round(time.perf_counter() - start, 4)
                        metadata['markers'].append(stamp)
                        mw.writerow([stamp, len(metadata['markers'])])
                        mfile.flush()
                        print(f'Marker {len(metadata["markers"])}: {stamp:.3f} s', flush=True)
                    time.sleep(.01)
            except OSError as error:
                metadata['sampling_error'] = str(error)
                print(f'Sampling stopped: {error}. Close the game to collect the available logs.', flush=True)
            process.wait()
            reader.join()
            process.stdout.close()
            metadata.update(exit_code=process.returncode, duration_s=time.perf_counter() - start,
                            game_log_truncated=log_bytes > 16 * 1024 * 1024,
                            sample_count=sample_count, sample_wall_s=sample_wall_s,
                            sample_max_wall_s=sample_max_wall_s)
    finally:
        sampler.close()
    (directory / 'metadata.json').write_text(json.dumps(metadata, indent=2) + '\n', encoding='utf-8')
    if not metadata['processes']:
        print('No native game process was sampled. The report contains the launch log only.', flush=True)
    return collect(directory)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.parse_args()
    print('Loading comparison (diagnostics only):\n'
          '1. With the game focused, press F8 before choosing Continue.\n'
          '2. Press F8 when you can control the character.\n'
          '3. After a death, press F8 as soon as the loading screen appears,\n'
          '   then F8 when you can control the respawned character.\n'
          '4. Repeat step 3 for two deaths, then close the game normally.\n'
          'The report ZIP will appear in logs. Keep this session short (about 2 minutes).\n'
          'F8 only marks observations while the game is focused; no input is blocked.\n'
          'No saves, settings or game assets are added to the report. Logs can contain paths.\n', flush=True)
    root = Path(__file__).resolve().parent.parent
    print(f'Report: {capture(root)}', flush=True)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError) as error:
        print(f'Loading trace: {error}', file=sys.stderr)
        raise SystemExit(1)

# SPDX-License-Identifier: GPL-2.0-or-later
"""The launcher's game tests: the game started with given settings, driven the same way every
time by a scenario, and measured while it runs.

Three kinds, one engine:
  performance  several runs; the settings that keep a frame rate goal with the best picture
               (Plan: the recommendation first, then one step up or down at a time)
  compare      one run per variant of a setting, each with a screenshot at the same moment
               (Variants), for a side-by-side picture and frame rate comparison
  stability    one long run that repeats the scenario's loop (Soak): crashes, freezes, memory
               that keeps growing, stutter over time; for beta builds

A scenario is a JSON file (assets/tests/*.json built in, more in the launcher's tests/scenarios
folder): which save to start from, settings on top
of the player's, and steps. Steps press pad buttons through the pad file (BB_PAD_FILE), play a
recorded route (BB_PAD_REPLAY; the game records one with BB_PAD_RECORD, F9 stops), take
screenshots and mark the measured seconds. Every run works on a copy of the save and the shader
cache in a folder of its own (BB_USER_DIR) with a settings file of its own (BB_CONFIG): the
player's saves and bbport.ini are never written to.

While a run lasts, a monitor samples every second: the game's frame statistics from its log
(BB_FRAME_STATS: FPS, frame pacing, stutters, shader compiles, draws), the process's memory and
CPU time, its graphics memory and GPU load (Windows' GPU performance counters), the memory free
for programs, and whether the window still answers. bbport_report turns it into the report.
"""
import ctypes
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import threading
import time
from ctypes import wintypes
from pathlib import Path

NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
ROUTE_SECONDS = 24
FREEZE_SECONDS = 25  # no frame statistics for this long while the game runs: a freeze
ASSETS = Path(getattr(sys, '_MEIPASS', Path(__file__).resolve().parent)) / 'assets'
BUILTIN_SCENARIOS = ASSETS / 'tests'
FIXED_INI = {'cheat_no_death': True, 'cheat_stealth': True, 'skip_intro': True, 'show_fps': False}


# ---- routes and scenarios -------------------------------------------------------------------------
def pan_route(seconds=ROUTE_SECONDS):
    """The built-in camera route: still for a moment, then a steady turn to the right and back to
    the left (the right stick; 128 is the centre), the hunter standing where the save left him.
    Lines: ms buttons lx ly rx ry l2 r2 (the format BB_PAD_RECORD writes)."""
    lines, ms = [], 0
    for rx, part in ((128, 1.0), (196, seconds / 2 - 1), (60, seconds / 2), (128, 0.5)):
        lines.append(f'{ms} 0 128 128 {rx} 128 0 0')
        ms += int(part * 1000)
    lines.append(f'{ms} 0 128 128 128 128 0 0')
    return '\n'.join(lines) + '\n'


def route_seconds(text):
    """The length of a recorded route in seconds."""
    times = [int(line.split()[0]) for line in text.splitlines() if line.strip() and line.split()[0].isdigit()]
    return (max(times) / 1000) if times else 0


class Scenario:
    """A scenario file: .name, .description, .kinds, .save ('own', 'ask' or a folder), .save_from
    (for 'ask': the mod that has the save, {name, url, file, folder}), .settings, .steps, .loop,
    .folder (for its route and save files)."""

    def __init__(self, path, recorded=None):
        self.path = Path(path)
        self.folder = self.path.parent
        self.recorded = Path(recorded) if recorded else None  # routes the player recorded for it
        data = json.loads(self.path.read_text(encoding='utf-8'))
        self.id = data.get('id') or self.path.stem
        self.name = data.get('name', self.id)
        self.description = data.get('description', '')
        self.kinds = data.get('kinds', ['performance', 'compare', 'stability'])
        self.save = data.get('save', 'own')
        self.save_from = data.get('save_from') or None
        self.settings = data.get('settings', {})
        self.steps = data.get('steps', [])
        self.loop = data.get('loop', [])
        self.port_details = bool(data.get('port_details'))  # the texture cache, VRAM details (bug reports)
        self.builtin = BUILTIN_SCENARIOS in self.path.parents

    def route(self, name):
        """The text of a route: 'pan' (built in, 'pan:30' for 30 s), one the player recorded for this
        scenario, or a file next to it."""
        if name.split(':')[0] == 'pan':
            return pan_route(float(name.split(':')[1]) if ':' in name else ROUTE_SECONDS)
        return self.route_path(name).read_text(encoding='utf-8')

    def route_path(self, name):
        own = self.recorded / self.id / name if self.recorded else None
        return own if own and own.is_file() else self.folder / name

    def missing_routes(self):
        """The route files this scenario needs that nobody has recorded yet."""
        names = {step.get('route', 'pan') for step in self.steps + self.loop if step.get('do') in ('route', 'measure')}
        return sorted(name for name in names if name.split(':')[0] != 'pan' and not self.route_path(name).is_file())

    def save_folder(self, chosen=None):
        """The SPRJ0005 folder to start from for 'ask' and folder saves (None: the player's own)."""
        if self.save == 'own':
            return None
        if self.save == 'ask':
            return Path(chosen) if chosen else None
        return self.folder / self.save


def scenarios(user_folder):
    """Every scenario: the built-in ones, then the player's (tests/scenarios/<name>/scenario.json
    or tests/scenarios/<name>.json). Routes the player recorded are in tests/routes/<scenario id>/."""
    found = []
    paths = sorted(BUILTIN_SCENARIOS.glob('*.json'))
    user = Path(user_folder)
    if user.is_dir():
        paths += sorted(user.glob('*.json')) + sorted(user.glob('*/scenario.json'))
    for path in paths:
        try:
            found.append(Scenario(path, user.parent / 'routes'))
        except (OSError, ValueError, AttributeError):
            pass
    return found


def words(text):
    """Lower-case words only: 'BOSS 2 - Father Gascoigne' -> 'boss 2 father gascoigne'."""
    return ' '.join(re.findall(r'[a-z0-9]+', str(text).lower()))


def mod_save(root, folder, archive=''):
    """A save mod's save for one scenario: the save folder (userdata0000 …) at or under `root`
    whose path names `folder` ('Cleric Beast' finds 'BOSS 1 - Cleric Beast'), or None; one inside
    the mod's `archive` folder first (the mod's other archive may name the same boss). The mod's
    files stay where the player unpacked them; the test copies them."""
    root, key, prefer = Path(root), words(folder), words(archive)
    if not root.is_dir() or not key:
        return None
    candidates = [root] + sorted((p for p in root.rglob('*') if p.is_dir()), key=lambda p: (len(p.parts), str(p)))
    found = []
    for path in candidates[:20000]:
        named = f" {words(' '.join(path.relative_to(root.parent).parts))} "
        if (path / 'userdata0000').is_file() and f' {key} ' in named:
            found.append((not prefer or f' {prefer} ' not in named, path))
    return min(found, key=lambda item: item[0])[1] if found else None


def game_files(folder, names):
    """shutil.copytree's ignore: only the game's own files of a save from elsewhere (a mod's save
    folder also holds a picture of the boss and a sce_sys folder of another layout)."""
    return [name for name in names if not name.lower().startswith(('userdata', 'backup'))]


# ---- reading the game's log -------------------------------------------------------------------------
FRAME_STATS = re.compile(r'^Frame stats: ([\d.]+) FPS, worst frame ([\d.]+) ms.*?; (\d+) shader/pipeline compiles, '
                         r'([\d.]+) ms.*? ([\d.]+) draws/frame', re.M)
FRAME_PACING = re.compile(r'^Frame pacing: median ([\d.]+) ms, stddev ([\d.]+) ms, min [\d.]+ ms, p1 [\d.]+ ms, '
                          r'p99 ([\d.]+) ms, (\d+) frames over 1\.5x median', re.M)
UPSCALER = re.compile(r'^Upscaler: ([^\n]+)|^(DLSS[^\n]*)|^(FSR 3[^\n]*)', re.M)
EVENT_LINES = re.compile(r'^(?:.*(?:error|Error|ERROR|failed|Failed|assert|Assert|STOP:|device lost|memory pressure|'
                         r'Exception|Traceback)[^\n]*)$', re.M)


TEXTURE_CACHE = re.compile(r'^Texture cache: memory pressure.*?(\d+) images evicted, (\d+) written back.*?(\d+) dropped',
                           re.M)
PORT_VRAM = re.compile(r'^Memory: VRAM (\d+) MiB', re.M)
STALE_CACHE = re.compile(r'WarmUp: (\d+) stale pipelines')
SETUP_LINES = re.compile(r'^(?:GPU:|DLSS:|Upscaler:|Object motion:|Output resolution|Patches: FPS|Frame generation:|'
                         r'VideoOut:)[^\n]*$', re.M)


def port_details(text):
    """The port's own view of a run (the scenarios with port_details): the texture cache under
    memory pressure, the VRAM the renderer counts, its "video memory full" messages, shader cache
    entries from another build, pipelines that failed, and the setup lines from the log's start."""
    cache = [tuple(int(v) for v in m) for m in TEXTURE_CACHE.findall(text)]
    vram = [int(v) for v in PORT_VRAM.findall(text)]
    return {'evicted': sum(c[0] for c in cache), 'written_back': sum(c[1] for c in cache),
            'dropped': sum(c[2] for c in cache), 'port_vram_mib': max(vram) if vram else None,
            'vram_full': text.count('video memory full'),
            'stale_cache': sum(int(v) for v in STALE_CACHE.findall(text)),
            'failed_pipelines': text.count('Failed to create graphics pipeline'),
            'device_lost': text.lower().count('device lost'),
            'setup': [line.strip() for line in SETUP_LINES.findall(text)
                      if not re.match(r'(Frame generation pacing|Object motion: \d)', line)
                      and 'video memory full' not in line][:20]}


def stats(text):
    """[(fps, draws per frame)] and [p99 ms] of a game log (the short form the plan uses)."""
    return ([(float(m[0]), float(m[4])) for m in FRAME_STATS.findall(text)],
            [float(m[2]) for m in FRAME_PACING.findall(text)])


def ini_text(base, changes):
    """bbport.ini with `changes` ({key: value}) over `base` (its text)."""
    seen, lines = set(), []
    fmt = lambda value: ('1' if value else '0') if isinstance(value, bool) else str(value)
    for line in base.splitlines():
        key = line.split('=', 1)[0].strip()
        if key in changes and '=' in line:
            lines.append(f'{key}={fmt(changes[key])}')
            seen.add(key)
        else:
            lines.append(line)
    lines += [f'{key}={fmt(value)}' for key, value in changes.items() if key not in seen]
    return '\n'.join(lines) + '\n'


# ---- watching the game process ----------------------------------------------------------------------
class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [('dwSize', wintypes.DWORD), ('cntUsage', wintypes.DWORD), ('th32ProcessID', wintypes.DWORD),
                ('th32DefaultHeapID', ctypes.c_size_t), ('th32ModuleID', wintypes.DWORD),
                ('cntThreads', wintypes.DWORD), ('th32ParentProcessID', wintypes.DWORD),
                ('pcPriClassBase', ctypes.c_long), ('dwFlags', wintypes.DWORD), ('szExeFile', ctypes.c_wchar * 260)]


class PROCESS_MEMORY_COUNTERS_EX(ctypes.Structure):
    _fields_ = [('cb', wintypes.DWORD), ('PageFaultCount', wintypes.DWORD)] + [
        (name, ctypes.c_size_t) for name in ('PeakWorkingSetSize', 'WorkingSetSize', 'QuotaPeakPagedPoolUsage',
                                             'QuotaPagedPoolUsage', 'QuotaPeakNonPagedPoolUsage',
                                             'QuotaNonPagedPoolUsage', 'PagefileUsage', 'PeakPagefileUsage',
                                             'PrivateUsage')]


def game_pids(name='bb-probe.exe'):
    """The process ids of the game (by its executable's name)."""
    kernel32 = ctypes.windll.kernel32
    kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    snapshot = kernel32.CreateToolhelp32Snapshot(0x2, 0)
    entry = PROCESSENTRY32W()
    entry.dwSize = ctypes.sizeof(entry)
    pids = []
    try:
        ok = kernel32.Process32FirstW(snapshot, ctypes.byref(entry))
        while ok:
            if entry.szExeFile.lower() == name:
                pids.append(entry.th32ProcessID)
            ok = kernel32.Process32NextW(snapshot, ctypes.byref(entry))
    finally:
        kernel32.CloseHandle(snapshot)
    return pids


def game_window(pid):
    """The visible top-level window of a process, or None."""
    user32 = ctypes.windll.user32
    found = []

    def each(hwnd, _l):
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(hwnd):
            found.append(hwnd)
        return True
    user32.EnumWindows(ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)(each), 0)
    return found[0] if found else None


class GpuCounters:
    """Windows' GPU performance counters: a process's dedicated graphics memory and 3D load."""

    def __init__(self):
        self.query = None
        try:
            pdh = ctypes.windll.pdh
            self.pdh = pdh
            query = wintypes.HANDLE()
            if pdh.PdhOpenQueryW(None, 0, ctypes.byref(query)):
                return
            self.memory, self.engine = wintypes.HANDLE(), wintypes.HANDLE()
            if (pdh.PdhAddEnglishCounterW(query, r'\GPU Process Memory(*)\Dedicated Usage', 0, ctypes.byref(self.memory))
                    or pdh.PdhAddEnglishCounterW(query, r'\GPU Engine(*)\Utilization Percentage', 0,
                                                 ctypes.byref(self.engine))):
                return
            self.query = query
            pdh.PdhCollectQueryData(query)
        except (AttributeError, OSError):
            self.query = None

    def values(self, handle):
        class FMT(ctypes.Structure):
            _fields_ = [('CStatus', wintypes.DWORD), ('pad', wintypes.DWORD), ('value', ctypes.c_double)]

        class ITEM(ctypes.Structure):
            _fields_ = [('name', wintypes.LPWSTR), ('fmt', FMT)]
        size, count = wintypes.DWORD(0), wintypes.DWORD(0)
        self.pdh.PdhGetFormattedCounterArrayW(handle, 0x200, ctypes.byref(size), ctypes.byref(count), None)
        if not size.value:
            return []
        buffer = ctypes.create_string_buffer(size.value)
        if self.pdh.PdhGetFormattedCounterArrayW(handle, 0x200, ctypes.byref(size), ctypes.byref(count), buffer):
            return []
        items = ctypes.cast(buffer, ctypes.POINTER(ITEM))
        return [(items[i].name, items[i].fmt.value) for i in range(count.value)]

    def sample(self, pid):
        """(dedicated graphics memory in GB, 3D engine load in %) of a process, or (None, None)."""
        if not self.query or not pid:
            return None, None
        try:
            self.pdh.PdhCollectQueryData(self.query)
            mark = f'pid_{pid}_'
            memory = sum(v for n, v in self.values(self.memory) if n.startswith(mark))
            load = sum(v for n, v in self.values(self.engine) if n.startswith(mark) and n.endswith('engtype_3D'))
            return memory / 1024 ** 3, min(100.0, load)
        except (OSError, ValueError):
            return None, None

    def close(self):
        if self.query:
            self.pdh.PdhCloseQuery(self.query)
            self.query = None


class Monitor(threading.Thread):
    """Samples the running game every second until stopped; reads its log as it grows."""

    def __init__(self, log_path):
        super().__init__(daemon=True)
        self.log_path, self.start_time = log_path, time.time()
        self.stop_event = threading.Event()
        self.lock = threading.Lock()
        self.position, self.text = 0, ''
        self.frames, self.pacing, self.telemetry, self.events = [], [], [], []
        self.pid, self.hwnd, self.last_frames = None, None, None
        self.hung_since = None

    def now(self):
        return round(time.time() - self.start_time, 1)

    def read_log(self):
        try:
            with open(self.log_path, 'r', encoding='utf-8', errors='replace') as log:
                log.seek(self.position)
                new = log.read()
                self.position = log.tell()
        except OSError:
            return
        if not new:
            return
        t = self.now()
        with self.lock:
            self.text += new
            for m in FRAME_STATS.finditer(new):
                self.frames.append({'t': t, 'fps': float(m[1]), 'worst_ms': float(m[2]), 'compiles': int(m[3]),
                                    'compile_ms': float(m[4]), 'draws': float(m[5])})
                self.last_frames = t
            for m in FRAME_PACING.finditer(new):
                self.pacing.append({'t': t, 'median_ms': float(m[1]), 'stddev_ms': float(m[2]), 'p99_ms': float(m[3]),
                                    'stutters': int(m[4])})
            for m in EVENT_LINES.finditer(new):
                if len(self.events) < 400:
                    self.events.append({'t': t, 'text': m[0][:300]})

    def run(self):
        kernel32, user32 = ctypes.windll.kernel32, ctypes.windll.user32
        kernel32.OpenProcess.restype = wintypes.HANDLE
        psapi = ctypes.windll.psapi
        gpu, handle, last_cpu = GpuCounters(), None, None
        cpus = os.cpu_count() or 1
        try:
            while not self.stop_event.wait(1.0):
                try:
                    self.read_log()
                    if not self.pid:
                        pids = game_pids()
                        if pids:
                            self.pid = pids[-1]
                            handle = kernel32.OpenProcess(0x1000 | 0x0010, False, self.pid)  # query limited + vm read
                    if not self.pid:
                        continue
                    if not self.hwnd:
                        self.hwnd = game_window(self.pid)
                    sample = {'t': self.now()}
                    if handle:
                        counters = PROCESS_MEMORY_COUNTERS_EX()
                        counters.cb = ctypes.sizeof(counters)
                        if psapi.GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.cb):
                            sample['ram_gb'] = counters.PrivateUsage / 1024 ** 3
                            sample['working_set_gb'] = counters.WorkingSetSize / 1024 ** 3
                        times = [wintypes.FILETIME() for _i in range(4)]
                        if kernel32.GetProcessTimes(handle, *[ctypes.byref(t) for t in times]):
                            busy = sum((t.dwHighDateTime << 32 | t.dwLowDateTime) for t in times[2:]) / 1e7
                            if last_cpu:
                                sample['cpu'] = min(100.0, (busy - last_cpu[0]) / max(0.01, time.time() - last_cpu[1])
                                                    / cpus * 100)
                            last_cpu = (busy, time.time())
                    vram, load = gpu.sample(self.pid)
                    if vram is not None:
                        sample['vram_gb'], sample['gpu'] = vram, load
                    try:
                        from bbport_help import memory_status
                        status = memory_status()
                        if status:
                            sample['free_gb'] = status[0]
                    except ImportError:
                        pass
                    hung = bool(self.hwnd and user32.IsHungAppWindow(self.hwnd))
                    sample['hung'] = hung
                    if hung and not self.hung_since:
                        self.hung_since = sample['t']
                    elif not hung:
                        self.hung_since = None
                    with self.lock:
                        self.telemetry.append(sample)
                except Exception:
                    # One failed sample, and the next comes: a monitor that stopped read no more frame statistics,
                    # and the test took the game for frozen (frozen).
                    pass
            self.read_log()
        finally:
            gpu.close()
            if handle:
                kernel32.CloseHandle(handle)

    def stop(self):
        self.stop_event.set()
        self.join(5)

    def frozen(self):
        """Seconds without new frame statistics (the game's frames stopped), or 0."""
        if self.last_frames is None:
            return 0
        return max(0, self.now() - self.last_frames - 5) if self.now() - self.last_frames > FREEZE_SECONDS else 0


# ---- one run ------------------------------------------------------------------------------------------
class XINPUT_GAMEPAD(ctypes.Structure):
    _fields_ = [('buttons', wintypes.WORD), ('left_trigger', ctypes.c_ubyte), ('right_trigger', ctypes.c_ubyte),
                ('lx', ctypes.c_short), ('ly', ctypes.c_short), ('rx', ctypes.c_short), ('ry', ctypes.c_short)]


class XINPUT_STATE(ctypes.Structure):
    _fields_ = [('packet', wintypes.DWORD), ('pad', XINPUT_GAMEPAD)]


def pad_in_use(pad):
    """An XInput pad's state is a player's input: a button, a trigger, or a stick past XInput's
    own dead zones (a resting stick jitters inside them)."""
    return bool(pad.buttons or pad.left_trigger > 30 or pad.right_trigger > 30 or
                max(abs(pad.lx), abs(pad.ly)) > 7849 or max(abs(pad.rx), abs(pad.ry)) > 8689)


class InputGuard:
    """The player's own input during a run, which would change what is measured: a key or the
    mouse (Windows' time of the last input) and Xbox-type controllers (XInput). The launcher's
    input goes through the pad file, which Windows never sees, so anything here is the player's.
    PlayStation controllers that the game reads itself (not through Steam Input) are not seen."""

    def __init__(self):
        self.xinput = None
        for name in ('xinput1_4', 'xinput9_1_0'):
            try:
                self.xinput = getattr(ctypes.windll, name)
                break
            except OSError:
                continue
        self.start = self.last_input()

    @staticmethod
    def last_input():
        class LASTINPUTINFO(ctypes.Structure):
            _fields_ = [('size', wintypes.UINT), ('time', wintypes.DWORD)]
        info = LASTINPUTINFO(ctypes.sizeof(LASTINPUTINFO), 0)
        return info.time if ctypes.windll.user32.GetLastInputInfo(ctypes.byref(info)) else None

    def pads_in_use(self):
        if not self.xinput:
            return False
        state = XINPUT_STATE()
        for index in range(4):
            if self.xinput.XInputGetState(index, ctypes.byref(state)) == 0 and pad_in_use(state.pad):
                return True
        return False

    def touched(self):
        """None, or what the player used."""
        if self.start is not None and self.last_input() != self.start:
            return 'keyboard or mouse'
        if self.pads_in_use():
            return 'controller'
        return None


class Stop(Exception):
    """The run ends early: the reason (crash, freeze, cancelled, input, …) in .status."""

    def __init__(self, status, text=''):
        super().__init__(text or status)
        self.status, self.text = status, text


class Run:
    """One start of the game with one set of settings, driven by the scenario's steps."""

    def __init__(self, test, index, name, settings):
        self.test, self.index, self.name, self.settings = test, index, name, settings
        self.windows, self.captures, self.process = [], {}, None
        self.loaded = False
        self.guard = None  # armed when the game starts

    # pad file
    def pad(self, text=''):
        (self.test.work / 'pad').write_text(text, encoding='utf-8')

    def press(self, token, seconds=0.3):
        self.pad(token)
        self.sleep(seconds)
        self.pad('')

    def sleep(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            self.check()
            time.sleep(min(0.5, max(0.0, end - time.time())))

    def check(self):
        """Raises Stop when the game has ended, froze or the test was cancelled."""
        if self.test.cancelled:
            raise Stop('cancelled')
        touched = self.guard.touched() if self.guard else None
        if touched:  # the player's input would change the route, the camera and what is measured
            raise Stop('input', f'{touched} used during the test')
        code = self.process.poll()
        if code is not None:
            raise Stop('crash' if self.loaded else 'no start', f'the game ended (code {code})')
        if self.monitor.frozen():
            raise Stop('freeze', f'no new frames for {FREEZE_SECONDS} s')
        if self.monitor.hung_since is not None and self.monitor.now() - self.monitor.hung_since > FREEZE_SECONDS:
            raise Stop('freeze', f'the window stopped answering for {FREEZE_SECONDS} s')

    def wait_for(self, done, seconds, failure):
        end = time.time() + seconds
        while not done():
            if time.time() > end:
                raise Stop('no start' if not self.loaded else 'error', failure)
            self.sleep(1)

    def go(self):
        t = self.test
        (t.work / 'bbport.ini').write_text(ini_text(t.base_ini, {**FIXED_INI, **t.scenario.settings, **self.settings}),
                                           encoding='utf-8')
        self.pad('')
        log_path = t.folder / f'run-{self.index}.log'
        self.monitor = Monitor(log_path)
        status, text, code = 'ok', '', None
        env = t.environment()
        if t.scenario.port_details:  # the allocator's statistics: VRAM by purpose
            env['BB_VMA_DUMP'] = str(t.folder / f'run-{self.index}-vram.json')
        with open(log_path, 'w', encoding='utf-8', errors='replace') as log:
            self.process = subprocess.Popen(t.command, cwd=str(t.port_dir), env=env, stdin=subprocess.DEVNULL,
                                            stdout=log, stderr=subprocess.STDOUT, creationflags=NO_WINDOW)
            self.monitor.start_time = time.time()
            self.monitor.start()
            self.guard = InputGuard()
            try:
                for step in t.scenario.steps:
                    self.step(step)
                if t.minutes and t.scenario.loop:
                    end = time.time() + t.minutes * 60
                    rounds = 0
                    self.windows.append(['loop', self.monitor.now(), None])
                    while time.time() < end:
                        for step in t.scenario.loop:
                            self.step(step)
                        rounds += 1
                        t.emit('progress', {'run': self.index, 'round': rounds,
                                            'left': max(0, round((end - time.time()) / 60, 1))})
                    self.windows[-1][2] = self.monitor.now()
            except Stop as stop:
                status, text = stop.status, stop.text
            except OSError as error:
                status, text = 'error', str(error)
            finally:
                code = self.process.poll()
                self.close()
                self.monitor.stop()
        return self.result(status, text, code)

    def step(self, step):
        do = step.get('do')
        t = self.test
        if do == 'title':  # the title menus: Online/Offline and Continue, until the world is there
            m = self.monitor
            self.wait_for(lambda: 'Overlay: menu ready' in m.text, 240, 'the game did not reach its menu')
            self.sleep(step.get('settle', 8))
            presses, last = 0, 0.0
            while True:
                frames = m.frames
                if frames and frames[-1]['draws'] > step.get('world_draws', 300) and m.now() > 5:
                    break
                if m.now() > 300:
                    raise Stop('no start', 'the save did not load')
                if presses < 2 or (presses < 5 and time.time() - last > 25):
                    self.press(step.get('press', 'cross'))
                    presses, last = presses + 1, time.time()
                self.sleep(5)
            self.loaded = True
            t.emit('loaded', {'run': self.index})
        elif do == 'wait':
            self.sleep(float(step.get('seconds', 1)))
        elif do == 'press':
            self.press(step.get('button', 'cross'), float(step.get('seconds', 0.3)))
        elif do == 'hold':  # buttons and sticks: "cross lx=128 ly=0"
            self.pad(step.get('pad', ''))
            self.sleep(float(step.get('seconds', 1)))
            self.pad('')
        elif do in ('route', 'measure'):
            text = t.scenario.route(step.get('route', 'pan'))
            (t.work / 'route.txt').write_text(text, encoding='utf-8')
            seconds = float(step.get('seconds') or route_seconds(text) or ROUTE_SECONDS)
            start, begun = self.monitor.now(), time.time()
            self.press('replay', 0.5)
            for mark in sorted(float(s) for s in step.get('captures', []) if float(s) < seconds):
                self.sleep(begun + 0.5 + mark - time.time())  # pictures along the route: the same places each run
                self.capture(f"{step.get('name', 'route')}-{mark:g}s")
            self.sleep(begun + 0.5 + seconds + 1 - time.time())
            if do == 'measure':
                self.windows.append([step.get('name', 'measure'), start, self.monitor.now()])
        elif do == 'capture':
            self.capture(step.get('name', f'shot{len(self.captures) + 1}'))
        else:
            raise Stop('error', f'unknown step {do!r} in the scenario')

    def capture(self, name):
        """A screenshot of the game's window (its full client area) as PNG."""
        hwnd = self.monitor.hwnd
        if not hwnd:
            return
        try:
            from PIL import Image
        except ImportError:
            return
        user32, gdi32 = ctypes.windll.user32, ctypes.windll.gdi32
        rect = wintypes.RECT()
        user32.GetClientRect(hwnd, ctypes.byref(rect))
        w, h = rect.right, rect.bottom
        if w < 16 or h < 16:
            return
        hdc = user32.GetDC(hwnd)
        mem = gdi32.CreateCompatibleDC(hdc)
        bmp = gdi32.CreateCompatibleBitmap(hdc, w, h)
        gdi32.SelectObject(mem, bmp)
        try:
            user32.PrintWindow(hwnd, mem, 3)  # the client area, rendered content included

            class BITMAPINFOHEADER(ctypes.Structure):
                _fields_ = [('size', wintypes.DWORD), ('w', ctypes.c_long), ('h', ctypes.c_long),
                            ('planes', wintypes.WORD), ('bits', wintypes.WORD), ('compression', wintypes.DWORD),
                            ('image', wintypes.DWORD), ('x', ctypes.c_long), ('y', ctypes.c_long),
                            ('used', wintypes.DWORD), ('important', wintypes.DWORD)]
            buffer = ctypes.create_string_buffer(w * h * 4)
            header = BITMAPINFOHEADER(ctypes.sizeof(BITMAPINFOHEADER), w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
            gdi32.GetDIBits(mem, bmp, 0, h, buffer, ctypes.byref(header), 0)
            file = f'run-{self.index}-{re.sub(r"[^A-Za-z0-9_-]", "_", name)}.png'
            Image.frombuffer('RGBA', (w, h), buffer, 'raw', 'BGRA', 0, 1).convert('RGB').save(
                self.test.folder / file, optimize=False, compress_level=3)
            self.captures[name] = file
        finally:
            gdi32.DeleteObject(bmp)
            gdi32.DeleteDC(mem)
            user32.ReleaseDC(hwnd, hdc)

    def close(self):
        """The game closed the normal way (WM_CLOSE), else ended."""
        if self.process.poll() is None:
            hwnd = self.monitor.hwnd or (game_window(self.monitor.pid) if self.monitor.pid else None)
            if hwnd:
                ctypes.windll.user32.PostMessageW(hwnd, 0x0010, 0, 0)
            for _i in range(40):
                if self.process.poll() is not None:
                    break
                time.sleep(0.5)
        if self.process.poll() is None:
            subprocess.run(['taskkill', '/PID', str(self.process.pid), '/T', '/F'], capture_output=True,
                           creationflags=NO_WINDOW)
        for pid in game_pids():  # never leave a game behind
            if pid == self.monitor.pid:
                subprocess.run(['taskkill', '/PID', str(pid), '/F'], capture_output=True, creationflags=NO_WINDOW)

    def result(self, status, text, code):
        m = self.monitor
        windows = [w for w in self.windows if w[2] is not None]

        def inside(rows):
            if not windows:
                return rows[1:] if len(rows) > 1 else rows
            return [r for r in rows if any(a + 4 < r['t'] <= b + 1 for _n, a, b in windows)]
        frames, pacing = inside(m.frames), inside(m.pacing)
        out = {'index': self.index, 'name': self.name, 'settings': self.settings, 'status': status,
               'exit_code': code if status != 'ok' else None, 'message': text, 'windows': windows,
               'frames': m.frames, 'pacing': m.pacing, 'telemetry': m.telemetry, 'events': m.events,
               'captures': self.captures, 'log': f'run-{self.index}.log', 'seconds': m.now()}
        upscaler = [next(g for g in groups if g) for groups in UPSCALER.findall(m.text)]
        out['upscaler'] = upscaler[-1] if upscaler else ''
        if frames:
            out['fps'] = round(statistics.mean(f['fps'] for f in frames), 1)
            out['worst_ms'] = max(f['worst_ms'] for f in frames)
            out['compiles'] = sum(f['compiles'] for f in frames)
        if pacing:
            out['p99_ms'] = round(statistics.mean(p['p99_ms'] for p in pacing), 2)
            out['low1'] = round(1000 / out['p99_ms'], 1) if out['p99_ms'] else None
            out['stutters'] = sum(p['stutters'] for p in pacing)
        # each measured part on its own (a scenario with several: standing, camera turn, walk)
        sections = {}
        for name, a, b in windows:
            sections.setdefault(name, []).append((a, b))
        if len(sections) > 1:
            out['sections'] = []
            for name, spans in sections.items():
                part = lambda rows: [r for r in rows if any(a + 4 < r['t'] <= b + 1 for a, b in spans)]
                f, p = part(m.frames), part(m.pacing)
                p99 = statistics.mean(x['p99_ms'] for x in p) if p else None
                out['sections'].append({'name': name, 'fps': round(statistics.mean(x['fps'] for x in f), 1) if f else None,
                                        'low1': round(1000 / p99, 1) if p99 else None,
                                        'worst_ms': max(x['worst_ms'] for x in f) if f else None})
        if self.test.scenario.port_details:
            out['port'] = port_details(m.text)
            vram_file = self.test.folder / f'run-{self.index}-vram.json'
            out['port']['vram_file'] = vram_file.name if vram_file.is_file() else None
        ram = [s['ram_gb'] for s in m.telemetry if 'ram_gb' in s]
        if len(ram) > 20:  # memory that keeps growing: the last quarter against the second
            quarter = len(ram) // 4
            out['ram_growth_gb'] = round(statistics.mean(ram[-quarter:]) - statistics.mean(ram[quarter:2 * quarter]), 2)
        if status != 'ok':
            out['error'] = text or status
        elif 'fps' not in out:
            out['error'] = 'no frame statistics'
        try:
            from bbport_help import hints_in_log
            out['hints'] = hints_in_log(m.text)
        except ImportError:
            out['hints'] = []
        return out


# ---- a whole test ---------------------------------------------------------------------------------------
class Test:
    """A test in a helper thread. on_event(kind, data) is called from it (the launcher puts the
    calls on its Tk queue). Kinds: 'start', 'run' (index, name), 'loaded', 'progress', 'result'
    (a run's result), 'done' (plan, results, folder), 'error' (text)."""

    def __init__(self, port_dir, command, environment, user_dir, game_dir, base_ini, scenario, plan, folder,
                 on_event, kind='performance', minutes=0, save=None, info=None):
        self.port_dir, self.command, self.base_env = Path(port_dir), command, environment
        self.user_dir, self.game_dir, self.base_ini = Path(user_dir), game_dir, base_ini
        self.scenario, self.plan, self.on_event = scenario, plan, on_event
        self.kind, self.minutes, self.save, self.info = kind, minutes, save, info or {}
        self.folder = Path(folder)  # results: logs, screenshots, results.json, the report
        self.work = Path(os.environ.get('TEMP', self.user_dir)) / 'bbport-test'  # the game's copy of the saves
        self.cancelled = False

    def emit(self, kind, data=None):
        try:
            self.on_event(kind, data)
        except Exception:  # the launcher may be gone; the test still ends cleanly
            pass

    def environment(self):
        env = dict(self.base_env)
        for key in [k for k in env if k.startswith(('BB_SHADNET', 'BB_THD', 'BB_COMMUNITY', 'BB_PAD_RECORD'))] + [
                'BB_ONLINE', 'BB_NET_BACKEND']:
            env.pop(key, None)
        env.update(BB_USER_DIR=str(self.work / 'user'), BB_CONFIG=str(self.work / 'bbport.ini'),
                   BB_PAD_FILE=str(self.work / 'pad'), BB_PAD_REPLAY=str(self.work / 'route.txt'),
                   BB_FRAME_STATS='1', BB_FPS='uncap', BB_FPS_LIMIT='0', BB_FULLSCREEN='1')
        return env

    def prepare(self, save_dir):
        """The work folder: the save to start from (the game's files and their metadata), the
        shader cache. A scenario's own save takes the player's save slot."""
        shutil.rmtree(self.work, ignore_errors=True)
        user = self.work / 'user'
        target = user / Path(save_dir).relative_to(self.user_dir)
        own = self.scenario.save_folder(self.save) is None
        source = Path(save_dir) if own else self.scenario.save_folder(self.save)
        if not source.is_dir():
            raise OSError(f'the save for this scenario is missing: {source}')
        shutil.copytree(source, target, ignore=None if own else game_files)
        meta = Path(str(source) + '.sce_sys')
        if meta.is_dir():
            shutil.copytree(meta, Path(str(target) + '.sce_sys'))
        elif Path(str(save_dir) + '.sce_sys').is_dir():  # the player's metadata fits any save of the game
            shutil.copytree(Path(str(save_dir) + '.sce_sys'), Path(str(target) + '.sce_sys'))
        if (self.user_dir / 'cache').is_dir():
            shutil.copytree(self.user_dir / 'cache', user / 'cache')
        (self.work / 'route.txt').write_text(pan_route(), encoding='utf-8')

    def start(self, save_dir):
        threading.Thread(target=self.work_thread, args=(save_dir,), daemon=True).start()

    def record(self, save_dir, route):
        """The player records a route for this scenario: the game from the scenario's save with the
        scenario's settings (the hunter unkillable and unseen), F9 starts and stops the recording
        (BB_PAD_RECORD). Once the game is closed the recording is kept as `route`."""
        threading.Thread(target=self.record_thread, args=(save_dir, Path(route)), daemon=True).start()

    def record_thread(self, save_dir, route):
        try:
            if game_pids():
                raise OSError('the game is already running')
            self.prepare(save_dir)
            (self.work / 'bbport.ini').write_text(ini_text(self.base_ini, {**FIXED_INI, **self.scenario.settings}),
                                                  encoding='utf-8')
            recording = self.work / 'recording.txt'
            env = self.environment()
            for key in ('BB_PAD_FILE', 'BB_PAD_REPLAY', 'BB_FRAME_STATS'):
                env.pop(key, None)
            env['BB_PAD_RECORD'] = str(recording)
            self.emit('recording')
            with open(self.work / 'recording.log', 'w', encoding='utf-8', errors='replace') as log:
                subprocess.Popen(self.command, cwd=str(self.port_dir), env=env, stdin=subprocess.DEVNULL, stdout=log,
                                 stderr=subprocess.STDOUT, creationflags=NO_WINDOW).wait()
            text = recording.read_text(encoding='utf-8') if recording.is_file() else ''
            if route_seconds(text) < 5:  # F9 not pressed, or pressed twice at once
                self.emit('recorded', None)
                return
            route.parent.mkdir(parents=True, exist_ok=True)
            route.write_text(text, encoding='utf-8')
            self.emit('recorded', str(route))
        except Exception as error:  # a recording must never take the launcher down
            self.emit('error', str(error))
        finally:
            shutil.rmtree(self.work, ignore_errors=True)

    def work_thread(self, save_dir):
        results = []
        try:
            if game_pids():
                raise OSError('the game is already running')
            self.folder.mkdir(parents=True, exist_ok=True)
            self.prepare(save_dir)
            self.emit('start')
            for index, (name, settings) in enumerate(self.plan.steps(results), 1):
                if self.cancelled:
                    break
                self.emit('run', (index, name))
                result = Run(self, index, name, settings).go()
                if isinstance(self.plan, Variants):
                    result['display'] = name  # the value's own text
                results.append(result)
                self.save_results(results)
                self.emit('result', result)
                if result['status'] in ('cancelled', 'input'):  # input: the rest would not be comparable
                    break
            self.save_results(results)
            try:
                import bbport_report
                bbport_report.write(self.folder, self.summary(results), results)
            except Exception as error:  # the measurements are kept even when the report fails
                (self.folder / 'report-error.txt').write_text(str(error), encoding='utf-8')
            self.emit('done', (self.plan, results, self.folder))
        except Exception as error:  # a test must never take the launcher down
            self.save_results(results)
            self.emit('error', str(error))
        finally:
            shutil.rmtree(self.work, ignore_errors=True)

    def summary(self, results):
        return {'kind': self.kind, 'scenario': self.scenario.name, 'scenario_id': self.scenario.id,
                'minutes': self.minutes, 'goal': getattr(self.plan, 'goal', None),
                'start': getattr(self.plan, 'start', {}),
                'choice': (self.plan.choice or {}).get('index') if getattr(self.plan, 'choice', None) else None,
                'floor': getattr(self.plan, 'floor', None), 'date': time.strftime('%Y-%m-%d %H:%M'),
                **self.info}

    def save_results(self, results):
        try:
            self.folder.mkdir(parents=True, exist_ok=True)
            (self.folder / 'results.json').write_text(json.dumps({'test': self.summary(results), 'runs': results},
                                                                 indent=1), encoding='utf-8')
        except OSError:
            pass


# ---- what to run --------------------------------------------------------------------------------------
# Going down: what costs the least picture first (measured on an RX 9070 XT, 2026-10: object
# motion vectors ~5 %, SSAO and dynamic shadows ~7 %, then the upscaler's quality steps and the
# model detail).
DOWN = [('object_motion', False), ('effect_ssao+effect_dynamic_shadows', False), ('preset', '+1'),
        ('model_lod', '0'), ('preset', '+1'), ('model_lod', '1')]
# Going up: the biggest gain first (Native AA cost ~3.5 % there).
UP = [('preset', '-1'), ('preset', '-1'), ('object_motion', True)]
PRESET_ORDER = ['0', '1', '2', '3', '4']  # Native AA .. Ultra Performance
LOWEST = {'preset': '3', 'object_motion': False, 'effect_ssao': False, 'effect_dynamic_shadows': False,
          'model_lod': '0'}


def step(settings, change):
    """settings with one step of DOWN/UP applied, or None when it changes nothing."""
    key, value = change
    out = dict(settings)
    if key == 'preset':
        if out.get('upscaler') in ('off', 'taa'):
            return None
        index = PRESET_ORDER.index(str(out.get('preset', '1'))) + int(value)
        if not 0 <= index < len(PRESET_ORDER):
            return None
        out['preset'] = PRESET_ORDER[index]
    else:
        for part in key.split('+'):
            out[part] = value
    return out if out != settings else None


class Plan:
    """Performance: what to try, decided run by run. The recommendation first. When it misses the
    goal, the lowest settings show what the PC can reach at all (where the processor or the engine
    sets the limit, lower graphics settings gain nothing): the goal becomes at most 97 % of that.
    Then one step down at a time until the goal is kept, or one step up while it still is.
    .choice is the result to offer at the end (None: nothing kept the goal)."""

    def __init__(self, start, goal, max_runs=7):
        self.start, self.goal, self.max_runs = dict(start), goal, max_runs
        self.choice = None

    def keeps(self, result):
        budget = 1000 / self.goal
        return ('error' not in result and result.get('fps', 0) >= self.goal * 1.03
                and (not result.get('p99_ms') or result['p99_ms'] <= budget * 1.6))

    def steps(self, results):
        current = dict(self.start)
        yield 'recommended', current
        first = results[-1]
        if 'error' in first:
            return
        runs = 1
        if self.keeps(first):
            self.choice, moves, going_up = first, UP, True
        else:
            lowest = {**current, **{k: v for k, v in LOWEST.items() if current.get('upscaler') not in ('off', 'taa')
                                    or k != 'preset'}}
            yield 'lowest', lowest
            runs += 1
            floor = results[-1]
            if 'error' in floor:
                return
            self.goal = min(self.goal, floor['fps'] * 0.97)
            self.floor = floor['fps']
            if self.keeps(first):
                self.choice, moves, going_up = first, UP, True
            else:
                moves, going_up = DOWN, False
        for change in moves:
            if runs >= self.max_runs:
                return
            candidate = step(current, change)
            if candidate is None:
                continue
            yield f'{change[0]} {change[1]}', candidate
            runs += 1
            last = results[-1]
            if 'error' in last:
                return
            current = candidate
            if going_up:
                if not self.keeps(last):
                    return  # one step too far: the previous result stays the choice
                self.choice = last
            elif self.keeps(last):
                self.choice = last
                return


class Variants:
    """Compare: one run per variant ({name: settings} in order), each on top of `base`; `repeats`
    runs of each, interleaved so that drift hits all alike."""

    def __init__(self, base, variants, repeats=1):
        self.start, self.variants, self.repeats = dict(base), list(variants), repeats
        self.choice = None

    def steps(self, _results):
        for _r in range(self.repeats):
            for name, settings in self.variants:
                yield name, {**self.start, **settings}


class Soak:
    """Stability: one run with the given settings; the scenario's loop repeats for the minutes."""

    def __init__(self, settings):
        self.start, self.choice = dict(settings), None

    def steps(self, _results):
        yield 'stability', dict(self.start)

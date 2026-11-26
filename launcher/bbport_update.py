# SPDX-License-Identifier: GPL-2.0-or-later
"""The launcher's own update (the side bar's Update, Advanced > Other versions): the part that must never leave a
broken installation. Up to 1.9.6 the new version copied itself file by file over the installation; a file in use
stayed old, and old and new program files mixed (1.9.5 froze with Python 3.12, 1.9.6 with 3.14: "Failed to import
encodings module", R74). A second start during the update deleted the files it was copying from.

Now the download is checked before anything changes (check_package). The installer (a copy of the running launcher,
Bloodborne.exe --install-update) waits for every program started from the installation, copies the new version
beside it on the same drive (TARGET/.update-new, stage) and swaps every top-level entry by renaming (swap): the old
one into TARGET/.update-old, the new one into its place; _internal and Bloodborne.exe, the launcher's two halves,
last and one right after the other. A rename that fails undoes all before it, so the installation is the old version
or the new one, never a mix. The swap notes its plan in .update-old first (JOURNAL): when the installer is stopped
halfway (Task Manager, a power cut), the next start finishes or undoes it from there (finish) instead of deleting the
only copy of a program folder. While the installer runs it holds the update mutex: a second start neither deletes
anything nor opens a launcher, and the leftovers are cleared only by a start without it (clear_leftovers)."""
import ctypes
import hashlib
import json
import os
import re
import shutil
import time
import zipfile
import zlib
from ctypes import wintypes
from pathlib import Path

from bbport_benchmark import PROCESSENTRY32W

L = None  # bbport_launcher_win, set by bind()


def bind(module):
    global L
    L = module


def _(*texts):
    return L._(*texts)


UPDATE_MUTEX = 'Local\\bbport-update'
UPDATE_FRONT = 'Local\\bbport-update-front'  # a start during an update brings the installer's window to the front
NEW, OLD = '.update-new', '.update-old'  # in the installation: the new version staged, the old one swapped out
JOURNAL = '.swap.json'  # in .update-old: what swap renames, for finish after an interruption
FIRST, LAST = '_internal', 'Bloodborne.exe'  # the frozen Python goes in right before the program that loads it
REQUIRED = ('Bloodborne.exe', '_internal/base_library.zip')  # without them the launcher cannot start
PYTHON_DLL = re.compile(r'python3\d+\.dll', re.I)
WAIT_SECONDS = 300  # for the old launcher and the programs started from the installation
COPY_SECONDS = 1800  # for the copy into .update-new: a stalled drive gives up before anything is renamed
TRIES, PAUSE = 20, 0.5  # per rename: an antivirus scanning a file it has just seen holds it for a moment
SYNCHRONIZE, DELETE = 0x00100000, 0x00010000
REPARSE_POINT = 0x400  # FILE_ATTRIBUTE_REPARSE_POINT: a symbolic link or a junction


class UpdateFailed(Exception):
    """Why the update stopped, for the player; the installation is unchanged."""


class RollbackFailed(UpdateFailed):
    """The update stopped (reason), and putting the old version back failed too; old is the .update-old with the
    journal (finish)."""

    def __init__(self, text, old=None, reason=None):
        super().__init__(text)
        self.old, self.reason = old, reason


class BadDownload(ValueError):
    """What is wrong with a downloaded package, for the player."""


def kernel32():
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.CreateMutexW.restype = kernel.OpenMutexW.restype = kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.CreateToolhelp32Snapshot.restype = kernel.CreateFileW.restype = kernel.OpenEventW.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = kernel.SetEvent.argtypes = [wintypes.HANDLE]
    kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel.Process32FirstW.argtypes = kernel.Process32NextW.argtypes = [wintypes.HANDLE,
                                                                        ctypes.POINTER(PROCESSENTRY32W)]
    kernel.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR,
                                                  ctypes.POINTER(wintypes.DWORD)]
    kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD,
                                   wintypes.DWORD, wintypes.HANDLE]
    return kernel


INVALID_HANDLE = ctypes.c_void_p(-1).value


# ---- the update mutex ---------------------------------------------------------------------------------------------
_MUTEX = None


def hold_mutex(name=UPDATE_MUTEX):
    """The installer's hold on the update mutex, until release_mutex or its exit; False when another installer has
    it."""
    global _MUTEX
    kernel = kernel32()
    handle = kernel.CreateMutexW(None, False, name)
    if handle and ctypes.get_last_error() == 183:  # ERROR_ALREADY_EXISTS
        kernel.CloseHandle(handle)
        return False
    _MUTEX = handle  # None when Windows gave none: the install goes on without it, as before there was one
    return True


def release_mutex():
    """Before the installer starts the launcher: that start clears the leftovers only without the mutex."""
    global _MUTEX
    if _MUTEX:
        kernel32().CloseHandle(_MUTEX)
        _MUTEX = None


def underway(name=UPDATE_MUTEX):
    """Whether an installer runs: its files are being swapped, so nothing may delete or start from them."""
    kernel = kernel32()
    handle = kernel.OpenMutexW(SYNCHRONIZE, False, name)
    if handle:
        kernel.CloseHandle(handle)
        return True
    return False


def to_front(name=UPDATE_FRONT):
    """A start during an update: the installer's window comes to the front (it waits for this event), and the start
    can exit at once instead of holding the installation's files open behind a message. False when no installer
    window waits for it."""
    kernel = kernel32()
    handle = kernel.OpenEventW(0x0002, False, name)  # EVENT_MODIFY_STATE
    if not handle:
        return False
    ctypes.windll.user32.AllowSetForegroundWindow(-1)  # ASFW_ANY: this start may hand the foreground on
    kernel.SetEvent(handle)
    kernel.CloseHandle(handle)
    return True


def wait_for_installer(process, name=UPDATE_MUTEX, seconds=60):
    """None once the installer just started holds the update mutex (the launcher may close then: a second start
    can no longer clear its files); else why it did not: its exit code, or 'timeout' (it is stopped then; it does
    nothing before this launcher has closed)."""
    import subprocess
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if underway(name):
            return None
        code = process.poll()
        if code is not None:
            return f'{code:#x}' if code > 0xFFFF else str(code)
        time.sleep(0.1)
    process.kill()
    try:
        process.wait(10)  # gone before its folder is cleared
    except subprocess.TimeoutExpired:
        pass
    return 'timeout'


def notice(text, error=False, seconds=None):
    """A message box without a launcher window (the installer, a start during an update); with seconds it closes by
    itself then, for a program that must not stay open (MessageBoxTimeoutW)."""
    user32, flags = ctypes.windll.user32, 0x10 if error else 0x40
    if seconds:
        user32.MessageBoxTimeoutW(None, text, 'Bloodborne', flags, 0, int(seconds * 1000))
    else:
        user32.MessageBoxW(None, text, 'Bloodborne', flags)


# ---- the download -------------------------------------------------------------------------------------------------
def check_package(archive, digest=None):
    """The folder inside a downloaded package that holds Bloodborne.exe ('' or 'bbport-windows/'). Raises
    BadDownload when the zip is damaged, differs from what GitHub lists (the asset's digest, "sha256:…") or misses
    what the launcher needs to start: then nothing is replaced."""
    algorithm, _sep, expected = (digest or '').partition(':')
    if algorithm.lower() == 'sha256' and expected.strip():
        hashed = hashlib.sha256()
        with open(archive, 'rb') as stream:
            for block in iter(lambda: stream.read(1 << 20), b''):
                hashed.update(block)
        if hashed.hexdigest() != expected.strip().lower():
            raise BadDownload(_('the download differs from the release (its checksum). Try again.'))
    try:
        with zipfile.ZipFile(archive) as package:
            damaged = package.testzip()
            names = package.namelist()
    except (zipfile.BadZipFile, zlib.error, EOFError) as failure:
        raise BadDownload(_('the download is damaged ({}). Try again.').format(failure)) from failure
    if damaged:
        raise BadDownload(_('the download is damaged ({}). Try again.').format(damaged))
    launchers = sorted((n for n in names if n.rsplit('/', 1)[-1] == LAST), key=lambda n: n.count('/'))
    inside = launchers[0][:-len(LAST)] if launchers else ''
    missing = [name for name in REQUIRED if inside + name not in names]
    if not any(PYTHON_DLL.fullmatch(n[len(inside + FIRST) + 1:]) for n in names if n.startswith(inside + FIRST + '/')):
        missing.append(f'{FIRST}/python3*.dll')
    if missing:
        raise BadDownload(_('the download is not a complete port package: {} is missing.').format(
            missing[0].replace('/', '\\')))
    return inside


def listed_files(package, inside):
    """{path: size} of every file of a checked package (an open ZipFile) under its folder `inside`: what the copy
    in the installation must hold (stage), whatever an antivirus or a cleanup took from the unpacked files."""
    return {info.filename[len(inside):]: info.file_size for info in package.infolist()
            if not info.is_dir() and info.filename.startswith(inside) and len(info.filename) > len(inside)}


def read_listed(path):
    """listed_files as the launcher wrote it for the installer (BB_UPDATE_FILES); None without a usable list (an
    installer started by 1.9.6 or older)."""
    try:
        listed = json.loads(Path(path).read_text(encoding='utf-8'))
        return {str(name): int(size) for name, size in listed.items()}
    except (OSError, TypeError, ValueError, AttributeError):
        return None


# ---- waiting for the programs of the installation -----------------------------------------------------------------
def open_process(pid):
    """A handle to wait for a process with (None when it has gone)."""
    try:
        return kernel32().OpenProcess(SYNCHRONIZE, False, int(pid)) or None
    except (ValueError, OverflowError):
        return None


def inside(path, folder):
    folder = os.path.normcase(os.path.abspath(folder)).rstrip('\\/') + os.sep
    return os.path.normcase(os.path.abspath(path)).startswith(folder)


def programs_under(folder, skip=None):
    """[(process id, program file)] of the running programs whose file lies in folder (this process left out):
    the old launcher, a game or a game test, their helpers. Programs Windows does not let us look at are missed;
    the swap then fails on their files and is undone."""
    kernel = kernel32()
    skip = {os.getpid()} if skip is None else set(skip)
    snapshot = kernel.CreateToolhelp32Snapshot(0x2, 0)  # TH32CS_SNAPPROCESS
    if not snapshot or snapshot == INVALID_HANDLE:
        return []
    entry = PROCESSENTRY32W()
    entry.dwSize = ctypes.sizeof(entry)
    found = []
    try:
        ok = kernel.Process32FirstW(snapshot, ctypes.byref(entry))
        while ok:
            pid = entry.th32ProcessID
            if pid and pid not in skip:
                handle = kernel.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
                if handle:
                    size = wintypes.DWORD(32768)
                    path = ctypes.create_unicode_buffer(size.value)
                    if kernel.QueryFullProcessImageNameW(handle, 0, path, ctypes.byref(size)) and \
                            inside(path.value, folder):
                        found.append((pid, path.value))
                    kernel.CloseHandle(handle)
            ok = kernel.Process32NextW(snapshot, ctypes.byref(entry))
    finally:
        kernel.CloseHandle(snapshot)
    return found


def wait_for_programs(target, launcher, launcher_pid, say, seconds=WAIT_SECONDS, pause=1.0):
    """Waits for the launcher being replaced (a handle from open_process), then for every program started from
    target, up to `seconds` in all; the window names what it waits for. Raises UpdateFailed naming what still
    runs."""
    deadline = time.monotonic() + seconds
    kernel = kernel32()
    if launcher:
        say(_('Waiting for the launcher to close…'))
        kernel.WaitForSingleObject(launcher, int(max(0.0, deadline - time.monotonic()) * 1000))
        kernel.CloseHandle(launcher)
    while True:
        running = programs_under(target)
        if not running:
            return
        names = ', '.join(sorted({Path(path).name for _pid, path in running}, key=str.lower))
        if time.monotonic() >= deadline:
            raise UpdateFailed(_('{} is still running').format(names))
        if {pid for pid, _path in running} == {launcher_pid}:
            say(_('Waiting for the launcher to close…'))
        else:
            say(_('Waiting for {} to close…').format(names))
        time.sleep(pause)


# ---- staging and the swap -----------------------------------------------------------------------------------------
def is_link(path):
    """A symbolic link or a junction: os.path.islink misses junctions, and os.walk and copytree follow them."""
    try:
        return os.path.islink(path) or bool(getattr(os.lstat(path), 'st_file_attributes', 0) & REPARSE_POINT)
    except OSError:
        return False


def remove(path):
    """A file or folder gone as far as Windows lets it (what is left goes at a later start). A link goes, never
    what it points to."""
    path = Path(path)
    try:
        if is_link(path):
            try:
                os.rmdir(path)  # a junction or a link to a folder: only the link
            except OSError:
                os.unlink(path)
        elif path.is_dir():
            shutil.rmtree(path, ignore_errors=True)
        else:
            path.unlink()
    except OSError:
        pass


def fresh(path):
    """path, emptied; when a leftover there cannot be removed, path-2, path-3 … Never one with a journal: an
    unfinished swap needs it (finish)."""
    path = Path(path)
    for number in range(1, 100):
        candidate = path if number == 1 else path.with_name(f'{path.name}-{number}')
        if (candidate / JOURNAL).exists():
            continue
        remove(candidate)
        if not os.path.lexists(candidate):
            return candidate
    raise OSError(f'{path} cannot be emptied')


def link_again(link, there, notes):
    """A link (a junction, a symbolic link to a folder) the player put in a program folder, made again in the new
    version as a junction to the same folder: copied, a large linked folder filled the disk."""
    target = os.path.realpath(link)
    try:
        import _winapi
        there.parent.mkdir(parents=True, exist_ok=True)
        _winapi.CreateJunction(target, str(there))
    except (ImportError, OSError) as failure:
        notes.append(f'Update: the link {link} (to {target}) could not be made again in the new version ({failure}); '
                     'the folder it points to is unchanged')


VIEW_MARK = '.yharnam-view'  # bbport_yharnam.VIEW_MARK


def carry_over(old, new, notes):
    """The files an older version's folder holds and the new one's lacks, copied into the new one: a player's own
    patches in patches\\, FSR 4 shaders the launcher downloaded (an update copied over them before, so they stayed).
    Links stay links (link_again); a seamless co-op game view (VIEW_MARK: links and hard links into the game) stays
    behind, made again from the game at its next start."""
    old, new = Path(old), Path(new)
    for folder, dirs, files in os.walk(old):
        folder = Path(folder)
        here = new / folder.relative_to(old)
        for name in list(dirs):
            if name == '__pycache__' or is_link(folder / name):
                dirs.remove(name)
                if name != '__pycache__' and not os.path.lexists(here / name):
                    link_again(folder / name, here / name, notes)
            elif (folder / name / VIEW_MARK).is_file():
                dirs.remove(name)
        for name in files:
            if not os.path.lexists(here / name):
                here.mkdir(parents=True, exist_ok=True)
                shutil.copy2(folder / name, here / name)


def stage(source, target, user_files, listed=None, notes=None, seconds=COPY_SECONDS):
    """The new version (source) copied beside the installation, on its drive, so that it can be renamed into place:
    TARGET/.update-new. The player's files (user_files) stay out. Raises OSError, or UpdateFailed when the copy is
    not complete: against listed (the package's files and sizes, listed_files) and bbport-files.json (every program
    file with its checksum). On a failure the copy is removed again."""
    source, target = Path(source), Path(target)
    notes = [] if notes is None else notes
    staging = fresh(target / NEW)
    try:
        skip = {name.lower() for name in user_files} | {NEW, OLD}
        top = os.path.normcase(os.path.abspath(source))
        deadline = time.monotonic() + seconds

        def copy(here, there):
            if time.monotonic() > deadline:  # a stalled drive: nothing in the installation has been renamed yet
                raise UpdateFailed(_('copying the new files did not finish in {} minutes').format(round(seconds / 60)))
            return shutil.copy2(here, there)
        shutil.copytree(source, staging, copy_function=copy, ignore=lambda folder, names: [
            n for n in names if os.path.normcase(os.path.abspath(folder)) == top and n.lower() in skip])
        # Every folder but _internal keeps what only the old one had; _internal must be exactly the new version's
        # Python. An old folder that is a link stays as it is where it points to; the new folder takes its place.
        for entry in staging.iterdir():
            old = target / entry.name
            if entry.name == FIRST or not entry.is_dir():
                continue
            if is_link(old):
                notes.append(f'Update: {old} was a link to {os.path.realpath(old)}; the new version\'s folder takes '
                             'its place, the folder it points to is unchanged')
            elif old.is_dir():
                carry_over(old, entry, notes)
        missing = [name for name in REQUIRED if not (staging / name).is_file()]
        if not any(PYTHON_DLL.fullmatch(p.name) for p in (staging / FIRST).glob('python3*.dll')):
            missing.append(f'{FIRST}/python3*.dll')
        for name, size in sorted((listed or {}).items()):
            if name.split('/', 1)[0].lower() not in skip and not same_size(staging / name, size):
                missing.append(name)
        try:
            from package_files import check
            missing += check(staging)[1]
        except ImportError:
            pass
        if missing:
            names = ', '.join(name.replace('/', '\\') for name in missing[:3]) + (' …' if len(missing) > 3 else '')
            raise UpdateFailed(_('the copied files are incomplete ({})').format(names))
    except BaseException:
        remove(staging)  # this copy's own folder, also when it is .update-new-2
        raise
    return staging


def same_size(path, size):
    try:
        return path.is_file() and path.stat().st_size == size
    except OSError:
        return False


def locked_file(path):
    """The first file or folder under path (path itself included) that another program holds open without letting
    it be renamed or deleted; None when there is none (then path itself is named)."""
    path = Path(path)
    kernel = kernel32()

    def held(candidate):
        handle = kernel.CreateFileW(str(candidate), DELETE, 7, None, 3, 0x02000000, None)  # OPEN_EXISTING, BACKUP
        if handle == INVALID_HANDLE or not handle:
            return ctypes.get_last_error() == 32  # ERROR_SHARING_VIOLATION
        kernel.CloseHandle(handle)
        return False
    if held(path):
        return path
    for folder, dirs, files in os.walk(path):
        dirs[:] = [name for name in dirs if not is_link(Path(folder) / name)]
        for name in dirs + files:
            if held(Path(folder) / name):
                return Path(folder) / name
    return None


def rename(source, destination, tries, pause):
    for attempt in range(tries):
        try:
            os.rename(source, destination)
            return
        except OSError:
            if attempt == tries - 1:
                raise
            time.sleep(pause)


def order(names):
    """The swap's order: everything else first, then _internal and Bloodborne.exe one right after the other. A stop
    in between all others leaves the old launcher whole, and it can finish the rest at its next start; the program
    everything starts from changes only once the Python it loads is in place."""
    return sorted(names, key=lambda name: (name in (FIRST, LAST), name == LAST, name.lower()))


def write_journal(old, staging, names, existed):
    """The swap's plan, before its first rename: with what lies where, it says how far it got (finish)."""
    temporary = old / (JOURNAL + '.tmp')
    temporary.write_text(json.dumps({'staging': staging.name, 'names': names, 'existed': existed}), encoding='utf-8')
    os.replace(temporary, old / JOURNAL)


def read_journal(old):
    try:
        journal = json.loads((Path(old) / JOURNAL).read_text(encoding='utf-8'))
        return {'staging': str(journal['staging']), 'names': [str(n) for n in journal['names']],
                'existed': [str(n) for n in journal['existed']]}
    except (OSError, ValueError, KeyError, TypeError):
        return None


def swap(staging, target, tries=None, pause=None):
    """Every entry of staging put in place of target's by renaming, the old one into TARGET/.update-old first (in
    the order of order()). A rename that fails (a file in use by another program) undoes every rename before it, in
    reverse, and raises UpdateFailed naming the file; so does any other stop that leaves this function. When undoing
    fails too: RollbackFailed, and the journal stays for finish. Returns the folder with the old version."""
    staging, target = Path(staging), Path(target)
    tries, pause = tries or TRIES, PAUSE if pause is None else pause
    old = fresh(target / OLD)
    old.mkdir()
    names = order(p.name for p in staging.iterdir())
    write_journal(old, staging, names, [name for name in names if os.path.lexists(target / name)])
    done = []
    try:
        for name in names:
            moves = [(staging / name, target / name)]
            if os.path.lexists(target / name):
                moves.insert(0, (target / name, old / name))
            for here, there in moves:
                try:
                    rename(here, there, tries, pause)
                except OSError as failure:
                    try:
                        blocked = locked_file(here) or here
                    except Exception:  # only the message would lack the file's name: the undo comes first
                        blocked = here
                    raise UpdateFailed(_('{} is in use by another program (an antivirus program, for example)').format(
                        blocked)) from failure
                done.append((here, there))
    except BaseException as stopped:
        try:
            undo(done, tries, pause)
        except RollbackFailed as failed:
            failed.old, failed.reason = old, stopped
            raise
        remove(old)  # empty again, with the journal
        raise
    try:
        (old / JOURNAL).unlink()  # everything is in place: nothing to finish any more
    except OSError:
        pass
    return old


def undo(done, tries, pause):
    """The renames done, undone in reverse. It stops at the first one that fails: what is left is the start of the
    swap's plan, as after an interruption, which finish completes or undoes (going on past it could pair the new
    Bloodborne.exe with the old _internal)."""
    for here, there in reversed(done):
        try:
            rename(there, here, tries, pause)
        except OSError as failure:
            raise RollbackFailed(str(there)) from failure


def finish(target, old, tries=None, pause=None):
    """An unfinished swap put right from the journal in old (the installer was stopped halfway, or its undo failed):
    once the new _internal is in place only Bloodborne.exe can be left, and it goes in too (the launcher's two halves
    must match; a running Bloodborne.exe can be renamed); else every rename is undone, the new entries back into the
    staging folder. Without a readable journal, what the installation lacks goes back (put_back). Returns (whether
    the new version is in place, a line for the log); raises OSError when a file is still held (the journal stays,
    the next start tries again)."""
    target, old = Path(target), Path(old)
    tries, pause = tries or TRIES, PAUSE if pause is None else pause
    journal = read_journal(old)
    if journal is None:
        back = put_back(old, target)
        return False, f'Update: {", ".join(back) or "nothing"} put back from {old.name}, which had no readable plan'
    staging, names, existed = target / journal['staging'], journal['names'], set(journal['existed'])

    def out(name):  # the old entry is in .update-old
        return name in existed and os.path.lexists(old / name)

    def placed(name):  # the new entry is in the installation
        return os.path.lexists(target / name) and (name not in existed or out(name))
    forward = FIRST in names and placed(FIRST) and all(placed(n) or os.path.lexists(staging / n) for n in names)
    if forward:
        for name in names:
            if not placed(name):
                if name in existed and not out(name):
                    rename(target / name, old / name, tries, pause)
                rename(staging / name, target / name, tries, pause)
    else:
        staging.mkdir(exist_ok=True)
        for name in reversed(names):
            if placed(name):
                rename(target / name, staging / name, tries, pause)
            if out(name):
                rename(old / name, target / name, tries, pause)
    (old / JOURNAL).unlink()
    return forward, (f'Update: the update that stopped halfway was {"completed" if forward else "undone"}; the '
                     f'{"new" if forward else "old"} version is in place ({target})')


def put_back(old, target):
    """The entries of a .update-old without a usable journal that the installation lacks, renamed back into it:
    the only copy of a program folder is never deleted. Returns their names; raises OSError when one is held."""
    back = []
    for entry in sorted(Path(old).iterdir()):
        if not entry.name.startswith(JOURNAL) and not os.path.lexists(Path(target) / entry.name):
            os.rename(entry, Path(target) / entry.name)
            back.append(entry.name)
    return back


def settle(target):
    """Every unfinished swap in target put right (finish): before an update, whose own swap needs the folders.
    Returns lines for the log; raises OSError when one cannot be."""
    return [finish(target, old)[1] for old in sorted(Path(target).glob(OLD + '*')) if (old / JOURNAL).is_file()]


def install(source, target, launcher, launcher_pid, say, user_files, seconds=WAIT_SECONDS, listed=None):
    """The installer's work: waits for the programs of target, stages source beside it (checked against listed)
    and swaps it in; the leftovers go at once when Windows lets them. Returns lines for the next start's log. Raises
    UpdateFailed (target is unchanged then) or RollbackFailed (after one more try to put it right, finish)."""
    source, target = Path(source), Path(target)
    notes = []
    wait_for_programs(target, launcher, launcher_pid, say, seconds)
    try:
        notes += settle(target)
    except OSError as failure:
        raise UpdateFailed(_('{} is in use by another program (an antivirus program, for example)').format(
            failure.filename or target)) from failure
    say(_('Copying the new files…'))
    try:
        staging = stage(source, target, user_files, listed, notes)
    except OSError as failure:
        raise UpdateFailed(_('the new files could not be copied ({})').format(failure)) from failure
    try:
        # A program started while the files were copied (a shortcut, Steam) would hold its files during the swap.
        wait_for_programs(target, None, None, say, seconds)
        say(_('Putting the new version in place…'))
        old = swap(staging, target)
    except RollbackFailed as failure:
        reason = str(failure.reason or '') or str(failure)  # why the swap stopped, for the player
        completed, line = put_right(failure, target)
        notes.append(line)
        old = failure.old
        if not completed:
            remove(old)
            remove(staging)
            raise UpdateFailed(reason) from failure
    except BaseException:
        remove(staging)
        raise
    remove(old)
    remove(staging)
    return notes


def put_right(failure, target):
    """After RollbackFailed, one more try from its journal a moment later: the antivirus that held a file may have
    let go. Returns finish's (completed, line); raises the RollbackFailed again when it fails too (the journal
    stays, the next start tries again)."""
    time.sleep(PAUSE)
    try:
        return finish(target, failure.old)
    except (OSError, TypeError):
        raise failure


def clear_leftovers(port_dir, update_dir=None, name=UPDATE_MUTEX):
    """What finished or failed updates left behind: an unfinished swap is finished or undone first (finish), then
    the installation's .update-old and .update-new go, and the downloads (update_dir, unless the port runs from
    it). Only while no installer runs: it copies from there and swaps into there. Returns lines for the log; None
    while one runs."""
    if underway(name):
        return None
    port_dir = Path(port_dir)
    notes, keep = [], set()
    for old in sorted(port_dir.glob(OLD + '*')):
        try:
            if (old / JOURNAL).is_file():
                notes.append(finish(port_dir, old)[1])
            else:
                back = put_back(old, port_dir)
                if back:
                    notes.append(f'Update: {", ".join(back)} put back from {old.name} (the installation lacked them)')
        except OSError as failure:
            journal = read_journal(old)
            keep.update({old} | ({port_dir / journal['staging']} if journal else set()))
            notes.append(f'Update: {old.name} kept for the next start, it could not be put right yet ({failure})')
    for entry in sorted(port_dir.glob('.update-*')):
        if entry.name.startswith((NEW, OLD)) and entry not in keep:
            remove(entry)
            notes.append(f'Update: {entry.name}, left behind by an update, '
                         f'{"could not be removed completely yet" if os.path.lexists(entry) else "removed"}')
    if update_dir and Path(update_dir) not in port_dir.parents and Path(update_dir) != port_dir:
        remove(update_dir)
    return notes

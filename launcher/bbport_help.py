# SPDX-License-Identifier: GPL-2.0-or-later
"""Help for the problems any installation can run into, whatever the release: the game files
(the clean 1.09 eboot.bin, a complete unpack, the right folder), Windows (the page file, an
antivirus, synced folders) and the graphics card. The launcher checks what it can on its own,
reads the log after a failed start for the known messages, and shows each fix step by step on
its Help page; the same steps the FAQ of the community Discord gives.

Logic first (no Tk, tested in tests/test_launcher_help.py), the Help page after it (HelpMixin).
The launcher module binds itself (bind()) for its helpers and texts.
"""
import ctypes
import hashlib
import os
import re
import threading
import webbrowser
from pathlib import Path

import bbport_theme as theme

L = None  # bbport_launcher_win, set by bind()
DISCORD = 'https://discord.gg/yTMG8c4Bqm'  # the community Discord (bbport.de), its help forum
EBOOT_SIZE = 93_825_704  # the clean 1.09 eboot.bin as the update PKG unpacks it
# The 1.09 image with Lance McDonald's 60 fps patch built in (run.py LANCE_60FPS_IMAGE).
LANCE_60FPS_IMAGE = 'bbc91f4dff6bc3118b039464289e39878a8fdfc802a07fab9d5fe84d935a676f'
GUEST_MEMORY_GB = {'1080': 6, 'high': 10}  # what the game reserves at its start, with the page file


def bind(module):
    global L
    L = module


def _(*texts):
    """Translated; a page named by an older name (Game & language) by its name now."""
    return L.renamed(L._(*texts)) if L else texts[0]


def number(value, digits=0):
    """A number as the launcher's language writes it: 93,825,704 / 93.825.704; 29.3 / 29,3."""
    text = f'{value:,.{digits}f}'
    language = str(getattr(L, 'LANG', 'en') or 'en')[:2]
    if language in ('de', 'es', 'it', 'pt'):
        text = text.replace(',', '\0').replace('.', ',').replace('\0', '.')
    elif language in ('fr', 'ru'):
        text = text.replace(',', '\u202f').replace('.', ',')
    return text


# ---- what can go wrong, and the fix ---------------------------------------------------------------
# key: (title, what it means, [steps]). Only problems every release can have.
TOPICS = {
    'eboot_modified': (
        'eboot.bin is not the clean 1.09 one',
        'Re-uploaded 1.09 updates often have a 60 FPS patch or other changes built into eboot.bin. The port '
        'brings its own 60 FPS and patches only the clean file: with a changed one the game runs at 30 FPS, the '
        'effect options do nothing, and some patches (Lance McDonald\'s 60 FPS) crash the gestures menu '
        'or the cutscene after Rom.',
        ["Use the eboot.bin of an unmodified 1.09 update, in a new, empty folder. Don't use a loose "
         'eboot.bin someone shared.',
         'Copy eboot.bin from it into your game folder (Game & language > Game folder) and replace the old one. '
         'Make sure it is that folder, not another copy of the game.',
         'Right-click eboot.bin > Properties: the clean file is exactly 93,825,704 bytes.',
         'Mods must not bring their own eboot.bin: the check at the top of this page names any that do.']),
    'eboot_truncated': (
        'eboot.bin is cut off',
        'It broke while unpacking (the log says "prepare failed: out of bounds").',
        ['Put the base game and the 1.09 update into two new, empty folders again.',
         'Check the size: eboot.bin of the update is exactly 93,825,704 bytes (89.4 MB).',
         'Copy everything from the update folder into the base game folder and replace the files.']),
    'eboot_unreadable': (
        'eboot.bin cannot be read',
        'It is not the original file (a PS4 SELF file): another tool has converted it, or it is not from '
        'Bloodborne.',
        ['Use the eboot.bin of an unmodified 1.09 update (exactly 93,825,704 bytes).']),
    'version': (
        'The game is not version 1.09',
        'The community patches (60 FPS and more, the effect options) exist for 1.09 only; other versions run at '
        '30 FPS.',
        ['Put the 1.09 update into a new, empty folder again.',
         'Open it, press Ctrl + A, then Ctrl + C.',
         'Open the base game folder (the one with dvdroot_ps4), press Ctrl + V and choose Replace the files in '
         'the destination.']),
    'update_only': (
        'Only the update folder was chosen',
        'The folder has eboot.bin but not the whole game (the log says gxshader.shaderbnd.dcx not found).',
        ['Copy everything from the update folder into the base game folder (replace the files).',
         'Choose the base game folder in Game & language. Its dvdroot_ps4 has about 29 GB in about 28,800 files.']),
    'empty_files': (
        'Files of the game are empty',
        'Unpacking broke: some files have 0 bytes.',
        ['Put the base game and the 1.09 update into new, empty folders again, then copy the update into the '
         'base game again.']),
    'page_file': (
        'Windows has too little memory for the start',
        'The game reserves about 6 GB at its start (10 GB at 1440p and 4K) in one block, from the RAM and the '
        'page file together. When Windows refuses, the game closes at once with code 3 (the log says "cannot '
        'create … MiB of guest memory").',
        ['Press Win + R, type sysdm.cpl, press Enter.',
         'Advanced > Performance: Settings… > Advanced > Virtual memory: Change…',
         'Tick Automatically manage paging file size for all drives. Still crashing: untick it, select a drive '
         'with free space, Custom size: initial 16384, maximum 32768, then Set.',
         'OK, then restart the PC: the change works only after a restart.',
         'Close browsers and other big programs before you press PLAY.']),
    'antivirus': (
        'An antivirus blocks the port\'s files',
        'The launcher rebuilds a few files at every start; an antivirus that scans them at that moment blocks '
        'them (the log says PermissionError [WinError 32] or WinError 5).',
        ['Start menu > Windows Security > Virus & threat protection > Manage settings.',
         'Exclusions > Add or remove exclusions > Add an exclusion > Folder: the port folder (the one with '
         'Bloodborne.exe).',
         'Another antivirus (Avast, Norton, …): add the port folder to its exceptions there.',
         'Still there: move the port to a plain folder like C:\\Games\\BBPort.']),
    'location': (
        'The port is in a synced or protected folder',
        'OneDrive (Desktop and Documents often are synced) and Program Files lock or move files while the port '
        'writes them.',
        ['Move the whole port folder to a plain folder like C:\\Games\\BBPort and start Bloodborne.exe from there.']),
    'other_install': (
        'A folder of another installation is in use',
        "The mods or cache folder in the launcher's settings lies in another port folder (an earlier version or a "
        "test build): both installations share it. Another version's shader cache can crash the game, and its mods "
        'apply here too.',
        ['Click the button below: this installation uses its own mods and cache folders again. The saves folder '
         'stays, and with it your characters and achievements.']),
    'vram': (
        'The graphics card\'s memory runs full',
        'Cards with 6 GB or less, or the log says ErrorOutOfDeviceMemory or "memory pressure": textures no '
        'longer fit, the game stutters, shows broken faces or crashes.',
        ['Graphics: preset Balanced or Performance (DLSS on GeForce RTX cards).',
         'Model detail (LOD) "As in the game", object motion vectors off.',
         'Close your browser and other programs while playing.']),
    'driver': (
        'The graphics driver stopped (device lost)',
        'The game needs Vulkan 1.3 and a current driver; an old or crashed driver ends the game with '
        '"device lost".',
        ['Update the driver: NVIDIA App, AMD Software (Adrenalin) or Intel Driver & Support Assistant.',
         'Restart the PC after installing it.',
         'Overclocked graphics card or memory: try the standard clocks.']),
    'black_screen': (
        'Only a black screen',
        'The game starts but shows nothing, or the picture breaks after a driver update: its shader cache no longer '
        'fits the driver.',
        ['Clear the shader cache (the button below). The next start stutters for a few minutes while it is '
         'built again.',
         'Still black: update the graphics driver and restart the PC.']),
    'shader_cache': (
        'Stutter in the first minutes',
        'Normal: the port builds its shader cache the first time it sees each effect. It is kept for the next '
        'starts.',
        ['Play on: it gets smooth once an area has been seen. After a driver update the cache is built again.']),
    'save_lost': (
        'A character is gone or the save is broken',
        'The Load Game menu is empty, a character is missing, or the game says the save is damaged. Before every '
        'start the launcher backs up your saves, so you can go back to how they were.',
        ['Close the game.',
         'Saves page, Backups: put back the newest backup from before the problem ("Before playing" with the '
         'date of your last good session).',
         'Your current saves are backed up first: trying an older backup loses nothing.',
         'Only one character is affected: under Your characters, open its earlier states and put back just that '
         'one.',
         'The character came from another version and there is no backup yet: import it again under '
         '"From other versions".']),
    'controller': (
        'The controller does nothing',
        'The game reads controllers the same way the controller test on the Controls page does: what lights up '
        'there reaches the game.',
        ['Open the controller test and press the buttons.',
         'Nothing lights up: connect the controller again (a cable is the surest), and close tools that take it '
         'over, like DS4Windows.',
         'Started from Steam: in the game\'s Steam properties, try Steam Input on or off.']),
    'smartscreen': (
        '"Windows protected your PC"',
        'Windows shows this for programs it does not know yet.',
        ['Click More info, then Run anyway.']),
}
TOPIC_ORDER = ['eboot_modified', 'eboot_truncated', 'eboot_unreadable', 'version', 'update_only', 'empty_files',
               'page_file', 'antivirus', 'location', 'save_lost', 'controller', 'black_screen', 'vram', 'driver',
               'shader_cache', 'smartscreen']
# Buttons under a topic's steps: (text, launcher method). They do the step for the player.
TOPIC_ACTIONS = {'black_screen': [('Clear the shader cache', 'clear_cache')],
                 'page_file': [('Open the memory settings', 'open_memory_settings')],
                 'antivirus': [('Open Windows Security', 'open_windows_security')],
                 'location': [('Open the port folder', 'open_port_folder')],
                 'other_install': [("Use this installation's own folders", 'use_own_folders')],
                 'eboot_modified': [('Open the game folder', 'open_game_folder')],
                 'eboot_truncated': [('Open the game folder', 'open_game_folder')],
                 'update_only': [('Choose the game folder', 'choose_game_page')],
                 'save_lost': [('Open the backups', 'open_backups')],
                 'controller': [('Open the controller test', 'open_controller_test')]}
TOPIC_ORDER += [key for key in TOPICS if key not in TOPIC_ORDER]  # topics added later: at the end

# The game's and run.py's messages that name the cause of a failed start or crash.
LOG_HINTS = [
    (r'of guest memory|page file is too small|commit limit', 'page_file'),
    (r'WinError (?:32|5)\b|PermissionError', 'antivirus'),
    (r'out of bounds: 0x', 'eboot_truncated'),
    (r'gxshader\.shaderbnd\.dcx', 'update_only'),
    (r'not the 01\.09 build|Lance McDonald', 'eboot_modified'),
    (r'expected PS4 SELF', 'eboot_unreadable'),
    (r'ErrorOutOfDeviceMemory|Texture cache: memory pressure', 'vram'),
    (r'ErrorDeviceLost|device lost', 'driver'),
]


def hints_in_log(text):
    """The topics whose messages the log has, in the order of LOG_HINTS."""
    return [topic for pattern, topic in LOG_HINTS if re.search(pattern, text or '')]


# The folders of the launcher's settings that belong to an installation: (setting, its name on its page).
OWN_FOLDERS = (('user_dir', ('Saves folder', 'Папка сохранений')), ('mods_dir', ('Mods folder', 'Папка модов')),
               ('cache_dir', ('Cache folder',)))
# The ones an installation must not share with another (Help's check and button): another version's mods apply here
# too, and its shaders can break the picture. A shared saves folder is fine (the characters in every version, backed
# up before every start): testers who set it back found their characters and achievements gone in both versions.
SEPARATE_FOLDERS = ('mods_dir', 'cache_dir')


def installation_of(path):
    """The port installation folder `path` lies in (run.py beside bin\\bb-probe.exe or Bloodborne.exe), or None."""
    try:
        path = Path(path).resolve()
    except (OSError, ValueError):
        return None
    for folder in (path, *path.parents):
        if (folder / 'run.py').is_file() and ((folder / 'bin' / 'bb-probe.exe').is_file()
                                              or (folder / 'Bloodborne.exe').is_file()):
            return folder
    return None


def other_installations(settings, port_dir, data_dir):
    """[(setting, its name, the other installation)] for the folders of the settings that lie in another
    installation of the port. The launcher's settings are shared by every installation (%APPDATA%), so a
    test build beside the old version takes its saves, shader cache and mods."""
    own = set()
    for folder in (port_dir, data_dir):
        try:
            own.add(Path(folder).resolve())
        except (OSError, ValueError):
            pass
    found = []
    for key, name in OWN_FOLDERS:
        value = str(settings.get(key) or '').strip()
        other = installation_of(value) if value else None
        if other and other not in own:
            found.append((key, name, other))
    return found


# ---- the checks ----------------------------------------------------------------------------------
def eboot_state(folder):
    """'clean', 'lance' (Lance McDonald's 60 fps patch), 'modified' (another build, or changed),
    'truncated', 'unreadable' or 'missing'; with the file size."""
    path = Path(folder or '.') / 'eboot.bin'
    try:
        data = path.read_bytes()
    except OSError:
        return 'missing', 0
    try:
        from prepare import parse_self, span
        from patches import PATCH_GROUPS, eboot_bytes, matching_groups_of
        elf, _header, ph, _segments, _missing = parse_self(data)
    except ValueError as error:
        if 'expected PS4 SELF' in str(error):  # another format (a converted ELF, another file)
            return 'unreadable', len(data)
        return ('truncated' if 'out of bounds' in str(error) or len(data) < EBOOT_SIZE else 'unreadable'), len(data)
    except (ImportError, KeyError, IndexError, OSError):
        return 'unreadable', len(data)
    try:
        loads = [p for p in ph if p['type'] in (1, 0x61000010)]
        image = bytearray(max(p['vaddr'] + p['memsz'] for p in loads))
        for p in loads:
            image[p['vaddr']:p['vaddr'] + p['filesz']] = span(elf, p['offset'], p['filesz'])
        if hashlib.sha256(image).hexdigest() == LANCE_60FPS_IMAGE:
            return 'lance', len(data)
        # Every patch group's original bytes (patches.py fingerprints each group on its own).
        clean = matching_groups_of(lambda start, end: eboot_bytes(elf, start, end)) == set(PATCH_GROUPS)
    except (ValueError, KeyError, IndexError, OSError):
        return 'truncated', len(data)
    return ('clean' if clean else 'modified'), len(data)


def scan_game(folder, limit=60000):
    """{'files', 'bytes', 'empty': [first few empty files], 'shaders': bool} of a game folder."""
    root = Path(folder)
    files = size = 0
    empty = []
    stack = [root]
    while stack and files < limit:
        try:
            entries = list(os.scandir(stack.pop()))
        except OSError:
            continue
        for entry in entries:
            try:
                if entry.is_dir(follow_symlinks=False):
                    stack.append(entry.path)
                elif entry.is_file(follow_symlinks=False):
                    files += 1
                    length = entry.stat().st_size
                    size += length
                    if not length and len(empty) < 5:
                        empty.append(os.path.relpath(entry.path, root))
            except OSError:
                pass
    return {'files': files, 'bytes': size, 'empty': empty,
            'shaders': (root / 'dvdroot_ps4' / 'shader' / 'gxshader.shaderbnd.dcx').is_file()}


def mods_with_eboot(mods_dir):
    """The mods (their folders) that bring an eboot.bin of their own."""
    root = Path(mods_dir or '')
    found = []
    if root.is_dir():
        for mod in sorted(p for p in root.iterdir() if p.is_dir()):
            if any(True for _p in mod.rglob('eboot.bin')):
                found.append(mod.name)
    return found


class MEMORYSTATUSEX(ctypes.Structure):
    _fields_ = [('dwLength', ctypes.c_ulong), ('dwMemoryLoad', ctypes.c_ulong),
                ('ullTotalPhys', ctypes.c_ulonglong), ('ullAvailPhys', ctypes.c_ulonglong),
                ('ullTotalPageFile', ctypes.c_ulonglong), ('ullAvailPageFile', ctypes.c_ulonglong),
                ('ullTotalVirtual', ctypes.c_ulonglong), ('ullAvailVirtual', ctypes.c_ulonglong),
                ('ullAvailExtendedVirtual', ctypes.c_ulonglong)]


def memory_status():
    """(free for programs now, limit, RAM) in GB: Windows' commit charge, RAM and page file together."""
    status = MEMORYSTATUSEX()
    status.dwLength = ctypes.sizeof(status)
    try:
        ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status))
    except (AttributeError, OSError):
        return None
    gb = 1024 ** 3
    return status.ullAvailPageFile / gb, status.ullTotalPageFile / gb, status.ullTotalPhys / gb


def memory_needed(output_res):
    """GB the game reserves at its start for an output resolution ('2560x1440')."""
    try:
        height = int(str(output_res).lower().split('x')[1])
    except (IndexError, ValueError):
        height = 1080
    return GUEST_MEMORY_GB['high' if height > 1080 else '1080']


def memory_problem(output_res, status=None):
    """None, or (free GB, needed GB, page file off) when the start would likely fail."""
    status = status or memory_status()
    if not status:
        return None
    free, limit, ram = status
    need = memory_needed(output_res)
    page_file_off = limit - ram < 1
    return (free, need, page_file_off) if free < need + 1 else None


def location_problem(port_dir):
    text = str(port_dir).lower()
    return 'location' if 'onedrive' in text or 'program files' in text else None


def run_checks(game_dir, mods_dir, port_dir, output_res, cached=None):
    """[(state, topic or None, text)] for this installation; `cached` is a previous eboot result
    {'key': …, 'state': …} that is used while eboot.bin is unchanged. Returns (checks, eboot)."""
    checks = []
    game = Path(game_dir) if game_dir else None
    eboot = None
    if not game or not (game / 'eboot.bin').is_file():
        checks.append(('bad', None, _('No eboot.bin in the game folder: choose the folder that has eboot.bin, '
                                      'sce_sys and dvdroot_ps4 directly inside (Game & language).')))
    else:
        stat = (game / 'eboot.bin').stat()
        # '2|': checked per patch group; an older launcher's answer is not reused.
        key = f'2|{(game / "eboot.bin").resolve()}|{stat.st_size}|{int(stat.st_mtime)}'
        state = cached.get('state') if cached and cached.get('key') == key else eboot_state(game)[0]
        eboot = {'key': key, 'state': state}
        size = number(stat.st_size)
        checks.append({
            'clean': ('ok', None, _('eboot.bin is the clean 1.09 file ({} bytes).').format(size)),
            'lance': ('bad', 'eboot_modified', _('eboot.bin has Lance McDonald\'s 60 FPS patch built in: the game '
                                                 'crashes with it. Use the clean one.')),
            'modified': ('warn', 'eboot_modified', _('eboot.bin is not the clean 1.09 file ({} bytes): the game runs '
                                                     'at 30 FPS without the effect options.').format(size)),
            'truncated': ('bad', 'eboot_truncated', _('eboot.bin is cut off ({} bytes, the clean one has {}).')
                          .format(size, number(EBOOT_SIZE))),
            'unreadable': ('bad', 'eboot_unreadable', _('eboot.bin cannot be read ({} bytes).').format(size)),
        }.get(state, ('bad', None, _('eboot.bin cannot be read.'))))
        values = {}
        try:
            from prepare import sfo
            values = sfo((game / 'sce_sys' / 'param.sfo').read_bytes())
        except (OSError, ValueError, ImportError):
            pass
        version = values.get('APP_VER')
        if version and version != '01.09':
            checks.append(('warn', 'version', _('Game version {}: it needs the 1.09 update.').format(version)))
        scan = scan_game(game)
        # The whole game: about 28,800 files and 29 GB (more with mods); the update alone is far less.
        if not scan['shaders'] or scan['files'] < 20000 or scan['bytes'] < 20 * 1024 ** 3:
            checks.append(('bad', 'update_only', _('dvdroot_ps4 is incomplete ({} files, {} GB): this looks like '
                                                   'the update folder alone.').format(
                number(scan['files']), number(scan['bytes'] / 1024 ** 3, 1))))
        elif scan['empty']:
            checks.append(('bad', 'empty_files', _('Empty files in the game folder, for example {}.').format(
                scan['empty'][0])))
        else:
            checks.append(('ok', None, _('The game folder is complete ({} files, {} GB).').format(
                number(scan['files']), number(scan['bytes'] / 1024 ** 3, 1))))
    mods = mods_with_eboot(mods_dir)
    if mods:
        checks.append(('warn', 'eboot_modified', _('These mods bring their own eboot.bin: {}. Turn them off or '
                                                   'remove that file from them.').format(', '.join(mods))))
    memory = memory_problem(output_res)
    status = memory_status()
    if memory:
        free, need, off = memory
        checks.append(('warn', 'page_file', (_('The page file is off: ') if off else '') + _(
            'only {} GB are free for the game\'s start right now, it reserves about {} GB.').format(number(free, 1), need)))
    elif status:
        checks.append(('ok', None, _('Memory for the start: {} GB free (RAM and page file), about {} GB '
                                     'needed.').format(number(status[0], 1), memory_needed(output_res))))
    if location_problem(port_dir):
        checks.append(('warn', 'location', _('The port is in {}.').format(port_dir)))
    return checks, eboot


# ---- the Help page -------------------------------------------------------------------------------
class HelpMixin:
    """The Help page, the check before PLAY and the hint after a failed start."""

    def build_help(self):
        tk, ttk, px = self.tk, self.ttk, self.px
        f = self.scrolled_page('help', _('Help'), _('The problems any installation can run into, checked for '
                                                    'this PC, and how to fix each of them.'))
        card = ttk.Frame(f, style='Card.TFrame', padding=(px(22), px(16)))
        card.grid(row=self.next_row(f), column=0, columnspan=2, sticky='we', pady=(px(10), 0))
        head = tk.Frame(card, bg=theme.CARD)
        head.pack(fill='x')
        tk.Label(head, text=_('Check this installation'), bg=theme.CARD, fg=theme.GOLD_LIGHT,
                 font=(theme.HEAD, 12)).pack(side='left')
        self.help_button = ttk.Button(head, text=_('Check again'), style='Card.TButton', command=self.help_check)
        self.help_button.pack(side='right')
        self.help_list = tk.Frame(card, bg=theme.CARD)
        self.help_list.pack(fill='x', pady=(px(10), 0))
        self.link_row(f, _('Set everything up again, step by step.'), None, _('Repeat the setup'), self.open_setup)

        self.section(f, _('Problems and their fixes'))
        self.help_topics = {}
        for key in TOPIC_ORDER:
            self.help_topics[key] = self.topic_card(f, key)
        self.section(f, _('Still stuck?'))
        last = ttk.Frame(f, style='Card.TFrame', padding=(px(22), px(16)))
        last.grid(row=self.next_row(f), column=0, columnspan=2, sticky='we', pady=(px(4), 0))
        self.piracy_notice(last, theme.CARD, wrap=700).pack(fill='x', pady=(0, px(12)))
        tk.Label(last, text=_('Ask in the help forum of the community Discord. Save a report and attach it: it has '
                              'the log, your settings and this PC\'s details (no passwords), so a helper sees the '
                              'cause at once.'), bg=theme.CARD, fg=theme.TEXT, font=(theme.BODY, 10), justify='left',
                 wraplength=px(720)).pack(anchor='w')
        row = tk.Frame(last, bg=theme.CARD)
        row.pack(anchor='w', pady=(px(10), 0))
        button = ttk.Button(row, text=_('Save a report'), style='Card.Accent.TButton', command=self.save_report)
        button.pack(side='left')
        self.index_entry(button, _('Save a report'), key='button:report')
        ttk.Button(row, text=_('Copy a summary'), style='Card.TButton', command=self.copy_diagnostics).pack(
            side='left', padx=(px(8), 0))
        ttk.Button(row, text=_('Open the Discord'), style='Card.TButton',
                   command=lambda: webbrowser.open(DISCORD)).pack(side='left', padx=(px(8), 0))
        more = tk.Frame(last, bg=theme.CARD)
        more.pack(anchor='w', pady=(px(8), 0))
        ttk.Button(more, text=_('Show the log'), style='Card.TButton', command=lambda: self.show('log')).pack(
            side='left')
        ttk.Button(more, text=_('Repeat the setup'), style='Card.TButton', command=self.open_setup).pack(
            side='left', padx=(px(8), 0))
        self.report_note = tk.Label(last, text='', bg=theme.CARD, fg=theme.MUTED, font=(theme.BODY, 9),
                                    justify='left', wraplength=px(720))
        self.report_note.pack(anchor='w', pady=(px(6), 0))
        self.help_results = []
        self.root.after(1500, self.help_check)

    def topic_card(self, parent, key):
        """A problem: its title, what it means; the steps fold out on a click."""
        tk, px = self.tk, self.px
        title, why, steps = TOPICS[key]
        card = tk.Frame(parent, bg=theme.CARD, cursor='hand2')
        card.grid(row=self.next_row(parent), column=0, columnspan=2, sticky='we', pady=(px(4), 0))
        top = tk.Frame(card, bg=theme.CARD)
        top.pack(fill='x', padx=px(18), pady=(px(10), px(10)))
        arrow = tk.Label(top, text='›', bg=theme.CARD, fg=theme.GOLD_LIGHT, font=(theme.BODY_SEMI, 13), width=2)
        arrow.pack(side='left')
        tk.Label(top, text=_(title), bg=theme.CARD, fg=theme.TEXT_STRONG, font=(theme.BODY_SEMI, 10),
                 anchor='w').pack(side='left', fill='x', expand=True)
        body = tk.Frame(card, bg=theme.CARD)
        tk.Label(body, text=_(why), bg=theme.CARD, fg=theme.MUTED, font=(theme.BODY, 9), justify='left',
                 wraplength=px(700)).pack(anchor='w', pady=(0, px(6)))
        for number, step in enumerate(steps, 1):
            line = tk.Frame(body, bg=theme.CARD)
            line.pack(fill='x', pady=px(2))
            tk.Label(line, text=f'{number}.', bg=theme.CARD, fg=theme.GOLD_LIGHT, font=(theme.BODY_SEMI, 10),
                     width=3, anchor='ne').pack(side='left', anchor='n')
            tk.Label(line, text=_(step), bg=theme.CARD, fg=theme.TEXT, font=(theme.BODY, 10), justify='left',
                     anchor='w', wraplength=px(660)).pack(side='left', fill='x', expand=True)

        actions = TOPIC_ACTIONS.get(key, [])
        if actions:
            row = tk.Frame(body, bg=theme.CARD)
            row.pack(anchor='w', pady=(px(8), 0), padx=(px(38), 0))
            for text, method in actions:
                self.ttk.Button(row, text=_(text), style='Card.TButton',
                                command=getattr(self, method)).pack(side='left', padx=(0, px(8)))

        def toggle(_event=None, show=None):
            show = not body.winfo_ismapped() if show is None else show
            if show:
                body.pack(fill='x', padx=(px(52), px(18)), pady=(0, px(12)))
            else:
                body.pack_forget()
            arrow.configure(text='⌄' if show else '›')
        for widget in (card, top, arrow) + tuple(top.winfo_children()):
            widget.bind('<Button-1>', toggle)
        card.toggle = toggle
        return card

    def open_topic(self, key):
        """The Help page with that problem's steps open, scrolled to it."""
        self.show('help')
        card = self.help_topics.get(key)
        if not card:
            return
        card.toggle(show=True)
        self.root.update_idletasks()
        canvas = card.master.master
        try:
            top = card.winfo_rooty() - card.master.winfo_rooty()
            canvas.yview_moveto(max(0, top - self.px(20)) / max(1, card.master.winfo_height()))
        except self.tk.TclError:
            pass

    def help_check(self):
        if getattr(self, 'help_running', False):
            return
        self.help_running = True
        self.help_button.configure(state='disabled')
        for child in self.help_list.winfo_children():
            child.destroy()
        self.state_line(self.help_list, 'info', _('Checking the game files…'), theme.CARD).pack(fill='x')
        get = lambda key, store: self.var(key, store).get()
        settings = {key: get(key, 'app') for key in SEPARATE_FOLDERS}
        args = (get('game_dir', 'app'), get('mods_dir', 'app') or str(L.DATA_DIR / 'mods'), L.PORT_DIR,
                get('output_res', 'ini'), self.app.get('eboot_check') or {})

        def work():
            try:
                result = run_checks(*args)
                elsewhere = other_installations(settings, L.PORT_DIR, L.DATA_DIR)
                result = (result[0] + [('warn', 'other_install', _('The {0} is in another installation of the port: '
                                                                   '{1}.').format(_(*name), folder))
                                       for _key, name, folder in elsewhere], result[1])
            except Exception as error:  # a check must never take the launcher down
                result = ([('warn', None, _('The check stopped: {}').format(error))], None)
            self.ui_calls.put(lambda: self.help_done(*result))
        threading.Thread(target=work, daemon=True).start()

    def help_done(self, checks, eboot):
        self.help_running = False
        self.help_button.configure(state='normal')
        if eboot:
            self.app['eboot_check'] = eboot
        self.help_results = checks
        for child in self.help_list.winfo_children():
            child.destroy()
        for state, topic, text in checks:
            line = self.state_line(self.help_list, state, text, theme.CARD, wrap=640)
            line.pack(fill='x', pady=self.px(3))
            if topic:
                self.link_to_topic(line, topic).pack(side='right', anchor='n')
        self.refresh_status()

    # the topics' buttons
    def open_memory_settings(self):
        import subprocess
        subprocess.Popen(['SystemPropertiesAdvanced.exe'])  # Advanced > Performance > Virtual memory

    def use_own_folders(self):
        """Help's button: the mods and cache folders that lie in another installation set back to this
        installation's own (empty: the default). The saves folder stays (SEPARATE_FOLDERS)."""
        settings = {key: self.var(key, 'app').get() for key in SEPARATE_FOLDERS}
        for key, _name, _folder in other_installations(settings, L.PORT_DIR, L.DATA_DIR):
            self.var(key, 'app').set('')
        self.collect()
        self.refresh_status()
        self.help_check()

    def open_windows_security(self):
        try:
            os.startfile('windowsdefender://threatsettings/')
        except OSError:
            webbrowser.open('https://support.microsoft.com/windows/add-an-exclusion-to-windows-security')

    def open_port_folder(self):
        self.open_path(L.PORT_DIR)

    def open_game_folder(self):
        self.open_path(self.var('game_dir', 'app').get())

    def choose_game_page(self):
        self.show('game')

    def open_backups(self):
        self.show('saves')
        self.root.update_idletasks()
        self.scroll_to(self.backups_view, above=self.px(40))

    def open_controller_test(self):
        self.show('controls')
        self.root.update_idletasks()
        self.scroll_to(self.pad_card, above=self.px(40))

    def link_to_topic(self, parent, topic):
        label = self.tk.Label(parent, text=_('How to fix') + '  ›', bg=theme.CARD, fg=theme.GOLD_LIGHT,
                              cursor='hand2', font=(theme.BODY_MEDIUM, 9))
        label.bind('<Button-1>', lambda _e: self.open_topic(topic))
        return label

    def eboot_note(self):
        """('✗ …' or '⚠ …') for the Play page while eboot.bin is known not to be clean, else None."""
        state = (self.app.get('eboot_check') or {}).get('state')
        return {'lance': '✗ ' + _('eboot.bin has a 60 FPS patch built in: the game crashes with it (Help)'),
                'modified': '⚠ ' + _('eboot.bin is not the clean 1.09 one: 30 FPS, no effect options (Help)'),
                'truncated': '✗ ' + _('eboot.bin is cut off: unpack the game again (Help)'),
                'unreadable': '✗ ' + _('eboot.bin cannot be read (Help)')}.get(state)

    def preflight(self):
        """Before PLAY: False when the start would fail for a reason the player can fix first."""
        problem = memory_problem(self.var('output_res', 'ini').get())
        if problem:
            free, need, off = problem
            text = (_('Windows has only {} GB free for programs right now; the game reserves about {} GB at its '
                      'start and would close at once (code 3).').format(number(free, 1), need) + '\n\n' +
                    (_('The page file is off. ') if off else '') +
                    _('Close big programs (browsers), or let Windows manage the page file (Help shows how).') +
                    '\n\n' + _('Start anyway?'))
            if not self.messagebox.askyesno('Bloodborne', text, icon='warning'):
                self.open_topic('page_file')
                return False
        return True

    def after_exit(self, code):
        """After the game ended with an error: the known causes its log names, with the fix."""
        if not code:
            return
        try:
            log = Path(self.app.get('user_dir') or L.DATA_DIR / 'user') / 'last_run.log'
            text = log.read_text(encoding='utf-8', errors='replace')[-400_000:]
        except OSError:
            return
        topics = hints_in_log(text)
        if not topics:
            return
        title, why, _steps = TOPICS[topics[0]]
        if self.messagebox.askyesno('Bloodborne', _('The game ended with an error (code {}). Its log names the '
                                                    'cause:').format(code) + '\n\n' + _(title) + '\n' + _(why) +
                                    '\n\n' + _('Show the fix?')):
            self.open_topic(topics[0])

    def save_report(self):
        """A zip on the desktop for asking for help: the log, the settings without passwords, this PC
        and the checks."""
        import json
        import platform
        import time
        import zipfile
        desktop = Path(os.environ.get('USERPROFILE', '.')) / 'Desktop'
        target = (desktop if desktop.is_dir() else Path.home()) / time.strftime('bbport-report-%Y%m%d-%H%M%S.zip')
        user = Path(self.app.get('user_dir') or L.DATA_DIR / 'user')
        self.collect()
        try:
            with zipfile.ZipFile(target, 'w', zipfile.ZIP_DEFLATED) as z:
                z.writestr('summary.txt', self.diagnostics() + '\n\nChecks:\n' + '\n'.join(
                    f'[{state}] {text}' for state, _t, text in self.help_results) + f'\n\nPython {platform.python_version()}, '
                    f'{platform.platform()}\n')
                z.writestr('settings.json', json.dumps(private_free(self.app), indent=2, ensure_ascii=False))
                if L.ini_path().is_file():
                    z.write(L.ini_path(), 'bbport.ini')
                for name in ('last_run.log',):
                    if (user / name).is_file():
                        z.write(user / name, name)
            self.report_note.configure(text=_('Saved: {}').format(target), fg=theme.GOLD_LIGHT)
        except OSError as error:
            self.report_note.configure(text=_('Could not save it: {}').format(error), fg=theme.BLOOD_LIGHT)


SECRET = re.compile(r'password|token|secret|npid|online_id|user$', re.I)


def private_free(settings):
    """The launcher settings without passwords, tokens and account names."""
    return {key: ('(hidden)' if SECRET.search(key) and value else value) for key, value in settings.items()}

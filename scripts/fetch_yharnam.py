"""Puts BBSC: Yharnam Together 1.0.0 (BBSC's BBport edition) in out/seamless, which packaging/windows/package.sh
ships as the package's `seamless` folder: its game (engine, run.py, scripts, event tool), unchanged but for FIXES,
without its own launcher (BLauncher.exe and what only it uses): this launcher starts it (launcher/bbport_yharnam.py). Two of
BBSC's launcher modules that its button icons need (bbport_controls.py, bbport_icons.py) come from its source zip
into scripts/, where its run.py finds them.

Usage: python scripts/fetch_yharnam.py [zip [source zip]]   (the release's zips if they were downloaded already)

Each zip must be the release's own: its SHA-256 is the one BBSC published beside it. Nothing kept is changed or
run here; BBSC is GPL-2.0 (its source zip is on the same release page).
"""
import hashlib
from pathlib import Path, PurePosixPath
import shutil
import sys
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parent.parent
VERSION = '1.0.0'
NAME = f'BBSC-Yharnam-Together-{VERSION}-BBport.zip'
URL = f'https://github.com/CLSAMG22/BBSC/releases/download/v{VERSION}/{NAME}'
SHA256 = '3bdaf3e607e25d3dbef03ab2a3b2b306b319488dd0f8d1c5fe873d647c1c8183'
TOP = 'Yharnam Together/'  # the release zip's one folder
MARK = 'bbport-seamless.txt'  # what out/seamless holds, written last
SOURCE_NAME = f'BBSC-Yharnam-Together-{VERSION}-source.zip'
SOURCE_URL = f'https://github.com/CLSAMG22/BBSC/releases/download/v{VERSION}/{SOURCE_NAME}'
SOURCE_SHA256 = '77d16571731d943179a567b504470b0988440360e4f74855314a3fe458809aae'
SOURCE_TOP = f'BBSC-Yharnam-Together-{VERSION}-source/bbport/launcher/'
ICON_MODULES = ('bbport_controls.py', 'bbport_icons.py')
# BBSC's own launcher and what only it uses (its frozen Python, its window's art; portable.txt is about it).
LEFT_OUT = {'blauncher.exe', 'bloodborne.exe', '_internal', 'launcher', 'portable.txt'}
# This port's changes to BBSC's files (GPL-2.0: each is listed in MARK): (file, BBSC's text, this port's text).
FIXES = (
    # An event without instructions may give any instruction offset: mods' event files (written by event editors)
    # give -1, and the game reads nothing there. BBSC's check refused them ("event 10008890: its 0 instructions at
    # -1 are outside the instruction table"), so seamless co-op did not start with such a mod. Its parameter check
    # already allowed this for an event without parameters.
    ('tools/party_boss_events.py',
     "        if (count < 0 or first < 0 or first % INSTRUCTION_SIZE or\n"
     "                first + count * INSTRUCTION_SIZE > fields[2] * INSTRUCTION_SIZE):\n",
     "        if count < 0 or count and (first < 0 or first % INSTRUCTION_SIZE or\n"
     "                                   first + count * INSTRUCTION_SIZE > fields[2] * INSTRUCTION_SIZE):\n"),
)


def sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as file:
        for block in iter(lambda: file.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def download(target, url=URL):
    target.parent.mkdir(parents=True, exist_ok=True)
    part = target.with_suffix('.part')
    request = urllib.request.Request(url, headers={'User-Agent': 'bbport-package'})
    with urllib.request.urlopen(request, timeout=60) as answer, open(part, 'wb') as file:
        shutil.copyfileobj(answer, file, 1 << 20)
    part.replace(target)
    return target


def member_path(name):
    """The path below TOP a zip entry unpacks to, or None for the folder itself and anything that would land
    outside it."""
    if not name.startswith(TOP):
        return None
    rest = PurePosixPath(name[len(TOP):])
    if not rest.parts or rest.is_absolute() or '..' in rest.parts or ':' in rest.parts[0]:
        return None
    return Path(*rest.parts)


def unpack(zip_path, dest, expected=SHA256):
    """The release zip's folder into `dest` (replaced), without BBSC's own launcher (LEFT_OUT). Refuses a zip whose
    SHA-256 is not the release's."""
    found = sha256(zip_path)
    if found != expected:
        raise ValueError(f'{zip_path} is not BBSC {VERSION}\'s release zip (SHA-256 {found})')
    dest = Path(dest)
    if dest.exists():
        shutil.rmtree(dest)
    count = 0
    with zipfile.ZipFile(zip_path) as archive:
        for info in archive.infolist():
            path = member_path(info.filename)
            if path is None or path.parts[0].lower() in LEFT_OUT:
                continue
            target = dest / path
            if info.is_dir():
                target.mkdir(parents=True, exist_ok=True)
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(info) as source, open(target, 'wb') as file:
                shutil.copyfileobj(source, file, 1 << 20)
            count += 1
    apply_fixes(dest)
    (dest / MARK).write_text(f'BBSC: Yharnam Together {VERSION}\n{URL}\nSHA-256 {expected}\n'
                             'Without its own launcher (BLauncher.exe): the SuperMedo launcher starts it.\n'
                             'Changed by the SuperMedo port: '
                             + ', '.join(sorted({name for name, _, _ in FIXES})) +
                             ' (an event without instructions may give any instruction offset)\n'
                             f'Source: {SOURCE_URL}\n', encoding='utf-8')
    return count


def apply_fixes(dest):
    """FIXES into the unpacked files; each must find BBSC's text exactly once."""
    for name, old, new in FIXES:
        path = Path(dest) / name
        raw = path.read_bytes().decode('utf-8')
        eol = '\r\n' if '\r\n' in raw else '\n'  # BBSC's own line ends are kept
        text = raw.replace('\r\n', '\n')
        if text.count(old) != 1:
            raise ValueError(f'{name}: the text to change is not there once (BBSC {VERSION} changed?)')
        path.write_bytes(text.replace(old, new).replace('\n', eol).encode('utf-8'))


def add_icon_modules(source_zip, dest, expected=SOURCE_SHA256):
    """BBSC's button-icon modules from its source zip into dest/scripts (its run.py's button_icons imports them)."""
    found = sha256(source_zip)
    if found != expected:
        raise ValueError(f'{source_zip} is not BBSC {VERSION}\'s source zip (SHA-256 {found})')
    with zipfile.ZipFile(source_zip) as archive:
        for name in ICON_MODULES:
            (Path(dest) / 'scripts' / name).write_bytes(archive.read(SOURCE_TOP + name))
    return len(ICON_MODULES)


def main(argv):
    zip_path = Path(argv[1]) if len(argv) > 1 else ROOT / 'out' / 'downloads' / NAME
    if not zip_path.is_file():
        print(f'Downloading {URL}')
        download(zip_path)
    source_zip = Path(argv[2]) if len(argv) > 2 else ROOT / 'out' / 'downloads' / SOURCE_NAME
    if not source_zip.is_file():
        print(f'Downloading {SOURCE_URL}')
        download(source_zip, SOURCE_URL)
    count = unpack(zip_path, ROOT / 'out' / 'seamless')
    count += add_icon_modules(source_zip, ROOT / 'out' / 'seamless')
    print(f'BBSC: Yharnam Together {VERSION}: {count} files in out/seamless (without its launcher)')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))

# SPDX-License-Identifier: GPL-2.0-or-later
"""The launcher's settings files (bbport.ini, input.ini, settings.json): read the way a player who edited them by
hand meant them, and written whole or not at all.

Players open bbport.ini and input.ini in Notepad or another editor: some save them in ANSI (an umlaut in a
comment is then no UTF-8), with a BOM, or with a decimal comma (sharpness=0,5). Each of these kept the launcher
from opening. A file that needed repairing is rewritten clean, its old bytes kept beside it as <name>.bad, and
REPAIRS gets a line for the log of the next start (bbport_launcher_win writes them there).
"""
import os
import shutil
import time
from pathlib import Path

REPAIRS = []  # lines for the next start's log: what was repaired, and where the old file went
REPLACE_TRIES, REPLACE_WAIT = 4, 0.25


def read(path):
    """(text, clean) of a text file: UTF-8 (a BOM is fine), or another encoding with the bytes that are no UTF-8
    as '?' (clean False then). OSError as from open()."""
    data = Path(path).read_bytes()
    try:
        return data.decode('utf-8-sig'), True
    except UnicodeDecodeError:
        return data.decode('utf-8-sig', errors='replace').replace('�', '?'), False


def write(path, text):
    """The whole text or nothing: a temporary file beside it, then os.replace, so a full drive or a crash
    halfway leaves the old file as it was. A file another program has open for a moment (an antivirus, the
    game reading it) is tried again over about a second."""
    path = Path(path)
    temp = path.with_name(path.name + '.tmp')
    try:
        with open(temp, 'w', encoding='utf-8') as stream:
            stream.write(text)
            stream.flush()
            os.fsync(stream.fileno())
        for attempt in range(1, REPLACE_TRIES + 1):
            try:
                os.replace(temp, path)
                break
            except PermissionError:
                if attempt == REPLACE_TRIES:
                    raise
                time.sleep(REPLACE_WAIT)
    finally:
        try:
            temp.unlink()
        except OSError:
            pass


def keep_bad(path):
    """A copy of a file that needed repairing, as <name>.bad (.bad2, … when that is taken: never over an
    earlier one). Returns the copy, or None when it could not be made."""
    path = Path(path)
    copy, number = path.with_name(path.name + '.bad'), 1
    while copy.exists():
        number += 1
        copy = path.with_name(f'{path.name}.bad{number}')
    try:
        shutil.copyfile(path, copy)
    except OSError:
        return None
    return copy

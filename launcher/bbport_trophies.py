# SPDX-License-Identifier: GPL-2.0-or-later
"""Bloodborne trophies: the list (PS4 trophy ids of CUSA03173) and the unlocks the game made.

The game unlocks a trophy through sceNpTrophyUnlockTrophy; the runtime appends "id unix-time" to
<saves folder>/trophies.log and prints "Runtime: trophy <id> unlocked". The launcher records
what it reads in the game's output (or in last_run.log) in the same file, so older runtimes and
games started without the launcher are tracked too. The platinum is the console's job: it is
awarded here once every base-game trophy is unlocked (The Old Hunters does not count).
"""
import re
import time
from pathlib import Path

BASE, DLC = 'Gravity Rush 2', "Another Story: Raven's Choice"
PLATINUM, GOLD, SILVER, BRONZE = 'platinum', 'gold', 'silver', 'bronze'
GRADES = (PLATINUM, GOLD, SILVER, BRONZE)

# (id, name, description, grade, group)
TROPHIES = [
    (0, 'Ultimate Shifter', 'Collected every trophy.', PLATINUM, BASE),
    (1, 'Lifetime Service Award', 'Completed all side missions.', GOLD, BASE),
    (2, 'Gold Medalist', 'Completed all challenge missions with gold or better.', GOLD, BASE),
    (3, 'Gem Collector', 'Collected 60,000 precious gems.', GOLD, BASE),
    (4, 'Fin', 'Completed the Eto Chapter.', GOLD, BASE),
    (5, 'Kat Working Overtime', 'Completed 40 side missions.', SILVER, BASE),
    (6, 'Kat versus World', 'Defeated 1,000 enemies.', SILVER, BASE),
    (7, 'Silver Medalist', 'Completed all challenge missions with silver or better.', SILVER, BASE),
    (8, 'Power Kat', "Raised all of Kat's powers to the maximum.", SILVER, BASE),
    (9, 'Saghassi Maniac', "Found all of Saghassi's Paintings in Hekseville.", SILVER, BASE),
    (10, 'Tour Conductor', 'Discovered all of the sites in Hekseville.', SILVER, BASE),
    (11, 'Smile for the Camera', "Completed the women's portrait collection.", SILVER, BASE),
    (12, 'Mining Instructions', 'Reached the 3rd layer of the Delvool Mining Trench.', SILVER, BASE),
    (13, 'Hekseville Twilight', 'Completed the Hekseville Chapter.', SILVER, BASE),
    (14, 'Rift Hunter', 'Defeated 5 Giant Nevi.', SILVER, BASE),
    (15, 'Kat Rising', 'Returned to Banga Settlement.', BRONZE, BASE),
    (16, 'Gravity Test', 'Completed the gravity shifting tutorial.', BRONZE, BASE),
    (17, 'Banga Sets Sail', 'Completed the Banga Chapter.', BRONZE, BASE),
    (18, 'Jirga Para Lhao Revolution', 'Completed the Jirga Para Lhao Chapter.', BRONZE, BASE),
    (19, 'A Day in Kat\'s Life', 'Completed 1 side mission.', BRONZE, BASE),
    (20, 'Kat\'s Daily Routine', 'Completed 20 side missions.', BRONZE, BASE),
    (21, 'New Challenger', 'Played a challenge mission.', BRONZE, BASE),
    (22, 'Bronze Medalist', 'Completed all challenge missions with bronze or better.', BRONZE, BASE),
    (23, 'Saghassi Fan', "Found all of Saghassi's Paintings in Jirga Para Lhao.", BRONZE, BASE),
    (24, 'Tour Guide', 'Discovered all of the sites in Jirga Para Lhao.', BRONZE, BASE),
    (25, 'Camera Man', "Completed the men's portrait collection.", BRONZE, BASE),
    (26, 'Mining Expert', 'Collected 5 talisman stones.', BRONZE, BASE),
    (27, 'Talisman Crafter', 'Merged a talisman 3 times.', BRONZE, BASE),
    (28, 'Sacred Trio', 'Equipped all three types of talismans at once.', BRONZE, BASE),
    (29, 'Lunar Vortex', 'Grabbed 5 enemies with one Lunar Style Vortex Field.', BRONZE, BASE),
    (30, 'Ball of Destruction', 'Defeated 3 enemies with one Jupiter Style Debris Ball.', BRONZE, BASE),
    (31, 'Surge of Power', 'Defeated 5 enemies with one Jupiter Style Surge Kick.', BRONZE, BASE),
    (32, 'Cat Claw', 'Defeated 10 enemies with one Spiraling Claw.', BRONZE, BASE),
    (33, 'High Wind Alert', 'Defeated 10 enemies within 10 seconds using one Gravity Typhoon.', BRONZE, BASE),
    (34, 'Event Horizon', 'Defeated 10 enemies with one Micro Black Hole.', BRONZE, BASE),
    (35, 'Gravity Flat', 'Defeated 20 enemies using Gravity Kick.', BRONZE, BASE),
    (36, 'Pipe Cleaner', 'Defeated 10 enemies using Stasis Throw.', BRONZE, BASE),
    (37, 'Raven\'s Awakening', 'Completed the Ark Chapter.', GOLD, DLC),
]

BY_ID = {t[0]: t for t in TROPHIES}
PLATINUM_ID = 0
PLATINUM_NEEDS = frozenset(t[0] for t in TROPHIES if t[4] == BASE and t[0] != PLATINUM_ID)

UNLOCK_LINE = re.compile(r'Runtime: trophy (\d+) unlocked')
LOG_NAME = 'trophies.log'


def read_log(path):
    """{id: earliest unlock time} from a trophies.log; missing or broken lines are skipped."""
    unlocked = {}
    try:
        text = Path(path).read_text(encoding='utf-8', errors='replace')
    except OSError:
        return unlocked
    for line in text.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[0].isdigit() and parts[1].isdigit():
            trophy, when = int(parts[0]), int(parts[1])
            if trophy not in unlocked or when < unlocked[trophy]:
                unlocked[trophy] = when
    return unlocked


def append_log(path, trophy, when):
    try:
        Path(path).parent.mkdir(parents=True, exist_ok=True)
        with open(path, 'a', encoding='utf-8') as log:
            log.write(f'{trophy} {int(when)}\n')
    except OSError:
        pass


def unlocks_in(text):
    """Trophy ids the game's output reports as unlocked, in order."""
    return [int(m.group(1)) for m in UNLOCK_LINE.finditer(text)]


def platinum_due(unlocked):
    return PLATINUM_ID not in unlocked and PLATINUM_NEEDS <= unlocked.keys()


class Tracker:
    """The unlocks of one saves folder, kept up to date from trophies.log, last_run.log and the
    lines of a running game. poll() and observe() return the ids that are new since the last call."""

    def __init__(self, user_dir):
        self.user_dir = Path(user_dir)
        self.log = self.user_dir / LOG_NAME
        self.run_log = self.user_dir / 'last_run.log'
        self.unlocked = {}
        self.log_stamp = self.run_stamp = None
        self.run_offset, self.run_tail = 0, b''

    @staticmethod
    def stamp(path):
        try:
            info = path.stat()
            return info.st_size, info.st_mtime_ns
        except OSError:
            return None

    def merge(self, found):
        new = []
        for trophy, when in sorted(found.items(), key=lambda item: item[1]):
            if trophy not in self.unlocked:
                new.append(trophy)
            if trophy not in self.unlocked or when < self.unlocked[trophy]:
                self.unlocked[trophy] = when
        return new

    def record(self, trophies, when=None):
        """Unlocks seen in the game's output: written to trophies.log unless the runtime already did."""
        when = int(when or time.time())
        new = self.refresh_log()
        for trophy in trophies:
            if trophy not in self.unlocked:
                self.unlocked[trophy] = when
                append_log(self.log, trophy, when)
                new.append(trophy)
        return new + self.award_platinum()

    def award_platinum(self):
        if not platinum_due(self.unlocked):
            return []
        when = max(self.unlocked[t] for t in PLATINUM_NEEDS)
        self.unlocked[PLATINUM_ID] = when
        append_log(self.log, PLATINUM_ID, when)
        return [PLATINUM_ID]

    def refresh_log(self):
        stamp = self.stamp(self.log)
        if stamp == self.log_stamp:
            return []
        self.log_stamp = stamp
        return self.merge(read_log(self.log))

    def poll(self, run_log=True):
        """run_log=False while the launcher reads the game's output itself (observe())."""
        new = self.refresh_log()
        stamp = self.stamp(self.run_log) if run_log else None
        if stamp and stamp != self.run_stamp:
            self.run_stamp = stamp
            try:
                with open(self.run_log, 'rb') as log:
                    # A new run rewrites the file: then the bytes before the offset differ.
                    if self.run_offset:
                        log.seek(self.run_offset - len(self.run_tail))
                        if log.read(len(self.run_tail)) != self.run_tail:
                            self.run_offset = 0
                    log.seek(self.run_offset)
                    chunk = log.read()
                # Only whole lines: the rest is read again once the game has finished writing it.
                end = chunk.rfind(b'\n') + 1
                if end:
                    self.run_offset += end
                    self.run_tail = chunk[max(0, end - 64):end]
                found = unlocks_in(chunk[:end].decode('utf-8', errors='replace'))
            except OSError:
                found = []
            if found:
                new += self.record(found, stamp[1] // 1_000_000_000)
        return new + self.award_platinum()

    def observe(self, line):
        found = unlocks_in(line)
        return self.record(found) if found else []

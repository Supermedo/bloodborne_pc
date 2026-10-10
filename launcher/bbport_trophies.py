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

BASE, DLC = 'Bloodborne', 'The Old Hunters'
PLATINUM, GOLD, SILVER, BRONZE = 'platinum', 'gold', 'silver', 'bronze'
GRADES = (PLATINUM, GOLD, SILVER, BRONZE)

# (id, name, description, grade, group)
TROPHIES = [
    (0, 'Bloodborne', 'All trophies acquired. Hats off!', PLATINUM, BASE),
    (1, 'Yharnam Sunrise', 'You lived through the hunt, and saw another day.', GOLD, BASE),
    (2, 'Honoring Wishes', "Captivated by the moon presence, you pledge to watch over the hunter's dream.", GOLD, BASE),
    (3, "Childhood's Beginning", 'You became an infant Great One, lifting humanity into its next childhood.', GOLD, BASE),
    (4, 'Yharnam, Pthumerian Queen', 'Defeat Yharnam, Blood Queen of the Old Labyrinth.', GOLD, BASE),
    (5, "Hunter's Essence", 'Acquire all hunter weapons.', GOLD, BASE),
    (6, "Hunter's Craft", 'Acquire all special hunter tools.', GOLD, BASE),
    (7, 'Weapon Master', 'Acquire a weapon of the highest level.', SILVER, BASE),
    (8, 'Blood Gem Master', 'Acquire an extremely precious blood gem.', SILVER, BASE),
    (9, 'Rune Master', 'Acquire an extremely precious Caryll Rune.', SILVER, BASE),
    (10, 'Cainhurst', 'Gain entry to Cainhurst, the lost and ruined castle.', SILVER, BASE),
    (11, 'The Choir', 'Gain entry to the realm of the Choir, the high stratum of the Healing Church.', SILVER, BASE),
    (12, 'The Source of the Dream', "Discover the abandoned old workshop, the source of the hunter's dream.", SILVER, BASE),
    (13, 'Nightmare Lecture Building',
     'Gain entry into the Byrgenwerth lecture building, that drifts within the realm of nightmare.', SILVER, BASE),
    (14, 'Father Gascoigne', 'Defeat the beast that once was Father Gascoigne.', BRONZE, BASE),
    (15, 'Vicar Amelia', 'Defeat the beast that once was Vicar Amelia.', BRONZE, BASE),
    (16, 'Shadow of Yharnam', 'Defeat the Shadow of Yharnam.', BRONZE, BASE),
    (17, 'Rom, the Vacuous Spider', 'Defeat Great One: Rom, the Vacuous Spider.', BRONZE, BASE),
    (18, 'The One Reborn', 'Defeat the One Reborn.', BRONZE, BASE),
    (19, 'Micolash, Host of the Nightmare', 'Defeat Micolash, Host of the Nightmare.', BRONZE, BASE),
    (20, "Mergo's Wet Nurse", "Defeat Great One: Mergo's Wet Nurse.", BRONZE, BASE),
    (21, 'Cleric Beast', 'Defeat Cleric Beast.', BRONZE, BASE),
    (22, 'Blood-starved Beast', 'Defeat Blood-starved Beast.', BRONZE, BASE),
    (23, 'The Witch of Hemwick', 'Defeat the Witch of Hemwick.', BRONZE, BASE),
    (24, 'Darkbeast Paarl', 'Defeat Darkbeast Paarl.', BRONZE, BASE),
    (25, 'Amygdala', 'Defeat Great One: Amygdala.', BRONZE, BASE),
    (26, 'Martyr Logarius', 'Defeat Martyr Logarius.', BRONZE, BASE),
    (27, 'Celestial Emissary', 'Defeat Great One: Celestial Emissary.', BRONZE, BASE),
    (28, 'Ebrietas, Daughter of the Cosmos', 'Defeat Great One: Ebrietas, Daughter of the Cosmos.', BRONZE, BASE),
    (29, 'Blood Gem Contact', 'Acquire a blood gem that imbues hunter weapons with special strength.', BRONZE, BASE),
    (30, 'Rune Contact', 'Acquire a Caryll Rune that endows hunters with special strength.', BRONZE, BASE),
    (31, 'Chalice of Pthumeru',
     'Acquire the Chalice of Pthumeru that seals the catacombs that form a web deep below Yharnam.', BRONZE, BASE),
    (32, 'Chalice of Ailing Loran', 'Acquire the Chalice of Ailing Loran that seals the tragic land lost to the sands.',
     BRONZE, BASE),
    (33, 'Chalice of Isz', 'Acquire the Great Chalice of Isz that seals the home of the cosmic kin.', BRONZE, BASE),
    (34, "Old Hunter's Essence", 'Acquire all old hunter weapons.', GOLD, DLC),
    (35, 'Orphan of Kos', 'Defeat Great One: Orphan of Kos.', SILVER, DLC),
    (36, 'Ludwig, the Holy Blade', 'Defeat the beast that was once Ludwig, the Holy Blade.', BRONZE, DLC),
    (37, 'Lady Maria of the Astral Clocktower', 'Defeat Lady Maria of the Astral Clocktower.', BRONZE, DLC),
    (38, 'Living Failures', 'Defeat the failed attempts to become Great Ones.', BRONZE, DLC),
    (39, 'Laurence, the First Vicar', 'Defeat the beast that was once Laurence, the First Vicar.', BRONZE, DLC),
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

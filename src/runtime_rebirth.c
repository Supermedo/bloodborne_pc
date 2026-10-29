/* Rebirth at the Doll (scripts/rebirth.py, game 1.09): the Doll's new "Rebirth" choice puts a
 * character back to its origin's attributes and level for REBIRTH_INSIGHT Insight, and the levels
 * it had are then bought again for free in the level-up screen, as in Dark Souls III's rebirth.
 * An attribute stays at what the equipped weapons need (BB_REBIRTH_WEAPONS, from rebirth.py):
 * the game never lets a weapon be used below its requirements, and doing it broke its attack
 * animations (a T-pose on a hit, Chikage below its Strength). Those points stay spent.
 * Once a level is bought the rebirth is kept, and the levels not bought yet stay free for later.
 * runtime_rebirth_closed ("bb:e") undoes one no level was bought after: the Doll's list calls it
 * on every choice, which passes only once the level-up screen is closed (rebirth.py doll_script;
 * the 1.9.5 betas' earlier try called it as the screen opened and undid every rebirth there).
 * No Blood Echoes are given back: a level costs nothing only below the level the character had
 * (its target), and no further.
 *
 * The patched game calls in through sceSystemServiceLaunchWebBrowser (runtime_services.c):
 * "bb:b" when the Doll's script asks for the rebirth, "bb:f" with a level when the level-up
 * screen prices that level. The player's data is GameDataMan (a global of the game image)
 * -> +0x8; its layout is the save's (launcher/bbport_respec.py) 0x40 bytes further in. The
 * targets are kept per character (origin and name) in <user>/rebirth.txt, so levels not spent
 * yet stay free after quitting. A rebirth whose target could not be saved is refused (it would
 * take the levels with nothing to regain them by), and the target last read or written is kept
 * in memory, so pricing a level does not open the file (an antivirus holding it made levels
 * cost Blood Echoes). While a rebirth can still be undone, its line also holds the character as it
 * was ("key target | attributes level insight reborn-level old-target"), so a game closed in the
 * level-up screen undoes it at the next choice in the Doll's list. */
#include "runtime.h"
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

#define GAME_DATA_MAN 0x553B130u
enum { VITALITY = 0x40, INSIGHT = 0x84, LEVEL = 0x90, NAME = 0xA8, NAME_CHARS = 16, ORIGIN = 0xCE };
/* The equipped weapons' ids: left 1, right 1, left 2, right 2 (empty hands: 1000000, Unarmed). */
enum { WEAPONS = 0x2A4, WEAPON_SLOTS = 4 };
#define REBIRTH_INSIGHT 5
/* Each attribute's offset, in CharaInitParam's order: Vitality, Endurance, Strength, Skill,
 * Bloodtinge, Arcane (the slot at 0x50 is unused). */
static const int ATTRIBUTE[6] = {0x40, 0x48, 0x58, 0x60, 0x68, 0x70};
/* CharaInitParam rows 3000-3008: each origin's level and attributes. */
static const uint8_t ORIGINS[9][7] = {
    {10, 11, 10, 12, 10, 9, 8},  /* Milquetoast */
    {10, 14, 11, 11, 10, 7, 7},  /* Lone Survivor */
    {10, 9, 14, 9, 13, 6, 9},    /* Troubled Childhood */
    {10, 12, 11, 15, 9, 6, 7},   /* Violent Past */
    {10, 9, 12, 9, 15, 7, 8},    /* Professional */
    {10, 10, 10, 14, 13, 7, 6},  /* Military Veteran */
    {10, 7, 8, 9, 13, 14, 9},    /* Noble Scion */
    {10, 10, 12, 10, 9, 5, 14},  /* Cruel Fate */
    {4, 10, 9, 10, 9, 7, 9},     /* Waste of Skin */
};

static unsigned char *image;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
/* The target of the character last asked about (rebirth.txt's line, or 0). */
static struct { int valid; char key[96]; unsigned target; } known;
/* What the last level priced came to, for the log (one line when it changes). */
static struct { char key[96]; unsigned target; int free_level; } logged;

/* The character as it was before the rebirth no level was bought after yet (its level-up screen
 * open, or closed without a choice in the Doll's list since). */
typedef struct {
    int active;
    char key[96];
    uint32_t attributes[6], level, insight, reborn_level;
    unsigned target;
} Before;
static Before before;

/* Each weapon's requirements (Strength, Skill, Bloodtinge, Arcane) by id, read once. */
#define MAX_WEAPONS 2048
static struct { uint32_t id; uint8_t needs[4]; } weapons[MAX_WEAPONS];
static int weapon_count = -1;

static void load_weapons(void) {
    weapon_count = 0;
    const char *path = getenv("BB_REBIRTH_WEAPONS");
    FILE *f = path && *path ? fopen(path, "r") : NULL;
    if (!f) return;
    unsigned id, a, b, c, d;
    while (weapon_count < MAX_WEAPONS && fscanf(f, "%u %u %u %u %u", &id, &a, &b, &c, &d) == 5) {
        weapons[weapon_count].id = id;
        weapons[weapon_count].needs[0] = (uint8_t)a; weapons[weapon_count].needs[1] = (uint8_t)b;
        weapons[weapon_count].needs[2] = (uint8_t)c; weapons[weapon_count].needs[3] = (uint8_t)d;
        ++weapon_count;
    }
    fclose(f);
}

/* The requirements of weapon `id`: its own row, else its base weapon's (an id's last four digits
 * are its upgrade and variant). NULL: none. */
static const uint8_t *weapon_needs(uint32_t id) {
    if (weapon_count < 0) load_weapons();
    for (int pass = 0; pass < 2; ++pass, id -= id % 10000)
        for (int i = 0; i < weapon_count; ++i)
            if (weapons[i].id == id) return weapons[i].needs;
    return NULL;
}

void runtime_rebirth_set_image(unsigned char *base) { image = base; }

static uint32_t get32(const unsigned char *p, int at) { uint32_t v; memcpy(&v, p + at, 4); return v; }
static void put32(unsigned char *p, int at, uint32_t v) { memcpy(p + at, &v, 4); }

/* The player's data, or NULL outside a game. */
static unsigned char *player(void) {
    if (!image) return NULL;
    uintptr_t manager, data;
    memcpy(&manager, image + GAME_DATA_MAN, sizeof(manager));
    if (!manager) return NULL;
    memcpy(&data, (const void *)(manager + 8), sizeof(data));
    return (unsigned char *)data;
}

/* "<origin>:<name as UTF-16 hex>": who the target belongs to. */
static void character_key(const unsigned char *p, char *out, size_t size) {
    size_t used = (size_t)snprintf(out, size, "%u:", p[ORIGIN]);
    for (int i = 0; i < NAME_CHARS * 2 && used + 3 < size; i += 2) {
        if (!p[NAME + i] && !p[NAME + i + 1]) break;
        used += (size_t)snprintf(out + used, size - used, "%02x%02x", p[NAME + i], p[NAME + i + 1]);
    }
}

static void targets_path(char *out, size_t size) {
    snprintf(out, size, "%s/rebirth.txt", runtime_file_user_dir());
}

/* `key`'s line in the file at `path`: 1 and *target (and *undo, when given, from its " | ..." part;
 * active 0 without one) when there is one, 0 when not, -1 when the file cannot be read. */
static int read_line(const char *path, const char *key, unsigned *target, Before *undo) {
    char line[256];
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int found = 0;
    const size_t length = strlen(key);
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, key, length) && line[length] == ' ') {
            *target = (unsigned)strtoul(line + length + 1, NULL, 10);
            found = 1;
            if (!undo) continue;
            Before b = {0};
            const char *rest = strstr(line + length, " | ");
            if (rest && sscanf(rest, " | %" SCNu32 " %" SCNu32 " %" SCNu32 " %" SCNu32 " %" SCNu32 " %" SCNu32
                               " %" SCNu32 " %" SCNu32 " %" SCNu32 " %u",
                               &b.attributes[0], &b.attributes[1], &b.attributes[2], &b.attributes[3],
                               &b.attributes[4], &b.attributes[5], &b.level, &b.insight, &b.reborn_level,
                               &b.target) == 10) {
                b.active = 1;
                snprintf(b.key, sizeof(b.key), "%s", key);
            }
            *undo = b;
        }
    }
    fclose(f);
    return found;
}

/* The target level for `key` (0: none). Without rebirth.txt, the rebirth.txt.new that 1.9.5's
 * betas left when replacing it failed (they removed it first) is the latest. */
static unsigned read_target(const char *key) {
    if (known.valid && !strcmp(known.key, key)) return known.target;
    char path[4096], temp[4200];
    targets_path(path, sizeof(path));
    snprintf(temp, sizeof(temp), "%s.new", path);
    unsigned target = 0;
    if (read_line(path, key, &target, NULL) < 0) read_line(temp, key, &target, NULL);
    known.valid = 1;
    snprintf(known.key, sizeof(known.key), "%s", key);
    known.target = target;
    return target;
}

/* `temp` becomes `path`, replacing it; tried for a few seconds while another program (an
 * antivirus, a sync client) has either open. */
static int replace_file(const char *temp, const char *path) {
    for (int attempt = 0; attempt < 40; ++attempt) {
#ifdef _WIN32
        if (MoveFileExA(temp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return 1;
        Sleep(50);
#else
        if (!rename(temp, path)) return 1;
#endif
    }
    return 0;
}

/* The rebirth of `key` that can still be undone, from rebirth.txt (active 0: none). */
static Before read_undo(const char *key) {
    char path[4096], temp[4200];
    targets_path(path, sizeof(path));
    snprintf(temp, sizeof(temp), "%s.new", path);
    unsigned target = 0;
    Before undo = {0};
    if (read_line(path, key, &target, &undo) < 0) read_line(temp, key, &target, &undo);
    return undo;
}

/* Sets (or with 0 removes) `key`'s target, with `undo` (when given) the rebirth that can still be
 * undone; the other characters' lines are kept. 0: it could not be saved (the file is unchanged). */
static int write_line(const char *key, unsigned target, const Before *undo) {
    char path[4096], temp[4200], line[256];
    targets_path(path, sizeof(path));
    snprintf(temp, sizeof(temp), "%s.new", path);
    FILE *in = fopen(path, "r");
    char *lines = NULL;
    size_t used = 0;
    if (!in) in = fopen(temp, "r");
    const size_t length = strlen(key);
    while (in && fgets(line, sizeof(line), in)) {
        if (strncmp(line, key, length) == 0 && line[length] == ' ') continue;
        const size_t n = strlen(line);
        char *grown = realloc(lines, used + n + 1);
        if (!grown) { fclose(in); free(lines); return 0; }
        lines = grown;
        memcpy(lines + used, line, n + 1);
        used += n;
    }
    if (in) fclose(in);
    FILE *out = fopen(temp, "w");
    int ok = out != NULL;
    if (out) {
        if (used) ok &= fwrite(lines, 1, used, out) == used;
        if (target && undo)
            ok &= fprintf(out, "%s %u | %" PRIu32 " %" PRIu32 " %" PRIu32 " %" PRIu32 " %" PRIu32 " %" PRIu32
                          " %" PRIu32 " %" PRIu32 " %" PRIu32 " %u\n", key, target,
                          undo->attributes[0], undo->attributes[1], undo->attributes[2], undo->attributes[3],
                          undo->attributes[4], undo->attributes[5], undo->level, undo->insight,
                          undo->reborn_level, undo->target) > 0;
        else if (target)
            ok &= fprintf(out, "%s %u\n", key, target) > 0;
        ok &= fflush(out) == 0;
        ok &= fclose(out) == 0;
    }
    free(lines);
    ok = ok && replace_file(temp, path);
    if (!ok) {
        remove(temp);
        printf("Runtime: rebirth: %s could not be saved\n", path);
        return 0;
    }
    known.valid = 1;
    snprintf(known.key, sizeof(known.key), "%s", key);
    known.target = target;
    return 1;
}

static int write_target(const char *key, unsigned target) { return write_line(key, target, NULL); }

/* 1: open the level-up screen (reborn now, or levels still to regain from an earlier rebirth,
 * which costs nothing more); 2: nothing to reset (already the origin's level); 0: not enough
 * Insight. rebirth.py's patch turns bit 0 and bit 1 into the flags the Doll's script reads. */
int runtime_rebirth(void) {
    pthread_mutex_lock(&lock);
    unsigned char *p = player();
    int result = 0;
    if (p && p[ORIGIN] < 9) {
        const uint8_t *origin = ORIGINS[p[ORIGIN]];
        const uint32_t insight = get32(p, INSIGHT), level = get32(p, LEVEL);
        char key[96];
        character_key(p, key, sizeof(key));
        const unsigned target = read_target(key);
        if (level < target) {
            printf("Runtime: rebirth: levels %" PRIu32 " to %u still free to regain\n", level, target);
            result = 1;
        } else {
            /* The origin's attributes, but Strength..Arcane not below the equipped weapons' needs
             * (nor above what the character has). Each point kept is a level kept. */
            uint32_t reborn[6], new_level = origin[0];
            for (int i = 0; i < 6; ++i) {
                reborn[i] = origin[i + 1];
                const uint32_t had = get32(p, ATTRIBUTE[i]);
                for (int slot = 0; i >= 2 && slot < WEAPON_SLOTS; ++slot) {
                    const uint8_t *needs = weapon_needs(get32(p, WEAPONS + slot * 4));
                    if (needs && needs[i - 2] > reborn[i]) reborn[i] = needs[i - 2] < had ? needs[i - 2] : had;
                }
                if (reborn[i] < origin[i + 1]) reborn[i] = origin[i + 1];
                new_level += reborn[i] - origin[i + 1];
            }
            if (level <= new_level) {
                printf("Runtime: rebirth: already the origin's level %u (with the weapons' needs)\n", new_level);
                result = 2;
            } else if (insight < REBIRTH_INSIGHT) {
                printf("Runtime: rebirth refused: Insight %" PRIu32 " (needs %d)\n", insight, REBIRTH_INSIGHT);
            } else {
                Before undo = {.active = 1, .level = level, .insight = insight, .reborn_level = new_level,
                               .target = target};
                snprintf(undo.key, sizeof(undo.key), "%s", key);
                for (int i = 0; i < 6; ++i) undo.attributes[i] = get32(p, ATTRIBUTE[i]);
                /* Without its line the levels could not be regained, nor the rebirth undone after a
                 * restart: no rebirth then. */
                if (!write_line(key, target > level ? target : level, &undo)) {
                    printf("Runtime: rebirth refused: its free levels could not be saved\n");
                    pthread_mutex_unlock(&lock);
                    fflush(stdout);
                    return 0;
                }
                before = undo;
                for (int i = 0; i < 6; ++i) put32(p, ATTRIBUTE[i], reborn[i]);
                put32(p, LEVEL, new_level);
                put32(p, INSIGHT, insight - REBIRTH_INSIGHT);
                printf("Runtime: rebirth: level %" PRIu32 " -> %" PRIu32 " (origin %u, the rest for the equipped weapons), "
                       "levels up to %u free, Insight %" PRIu32 " -> %" PRIu32 "\n", level, new_level, origin[0],
                       target > level ? target : level, insight, insight - REBIRTH_INSIGHT);
                result = 1;
            }
        }
    }
    pthread_mutex_unlock(&lock);
    fflush(stdout);
    return result;
}

/* A choice in the Doll's list (the level-up screen a rebirth opened is closed by then): no level
 * bought means no rebirth, as when backing out of DS3's. The rebirth may be from before a restart
 * (rebirth.txt). */
void runtime_rebirth_closed(void) {
    pthread_mutex_lock(&lock);
    unsigned char *p = player();
    if (p) {
        char key[96];
        character_key(p, key, sizeof(key));
        if (!before.active || strcmp(key, before.key)) before = read_undo(key);
        const uint32_t level = get32(p, LEVEL);
        if (!before.active) {
            /* no rebirth to undo */
        } else if (level == before.reborn_level) {
            /* The Insight spent comes back (Insight gained or used since stays so). */
            const uint32_t insight = get32(p, INSIGHT) + REBIRTH_INSIGHT;
            for (int i = 0; i < 6; ++i) put32(p, ATTRIBUTE[i], before.attributes[i]);
            put32(p, LEVEL, before.level);
            put32(p, INSIGHT, insight);
            write_target(key, before.target);
            printf("Runtime: rebirth cancelled (no level bought): level %" PRIu32 " back, Insight %" PRIu32 "\n",
                   before.level, insight);
        } else {
            /* A level was bought (the rebirth stays; its free levels too), or the save is from
             * before the rebirth: nothing to undo any more. */
            write_target(key, read_target(key));
            printf("Runtime: rebirth kept (level %" PRIu32 ")\n", level);
        }
    }
    before.active = 0;
    pthread_mutex_unlock(&lock);
    fflush(stdout);
}

int runtime_rebirth_free_level(int level) {
    pthread_mutex_lock(&lock);
    unsigned char *p = player();
    int free_level = 0;
    if (p && p[ORIGIN] < 9 && level >= 0) {
        char key[96];
        character_key(p, key, sizeof(key));
        const unsigned target = read_target(key);
        free_level = (unsigned)level < target;
        if (target && (strcmp(logged.key, key) || logged.target != target || logged.free_level != free_level)) {
            printf("Runtime: rebirth: level %d %s (free up to level %u)\n", level + 1,
                   free_level ? "free" : "costs Blood Echoes", target);
            fflush(stdout);
            snprintf(logged.key, sizeof(logged.key), "%s", key);
            logged.target = target;
            logged.free_level = free_level;
        }
        /* Every level is bought again: the target is no longer needed (and not tried again while
         * the screen prices levels when the file can't be written now). */
        if (target && get32(p, LEVEL) >= target) {
            if (!write_target(key, 0)) known.target = 0;
            printf("Runtime: rebirth: every level regained (%u)\n", target);
            fflush(stdout);
        }
    }
    pthread_mutex_unlock(&lock);
    return free_level;
}

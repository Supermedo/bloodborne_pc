/* Rebirth: backing out without a level undoes it, in the same session or after the game closed in
 * the level-up screen (rebirth.txt); a level bought keeps it. */
#include "../src/runtime_rebirth.c"
#include <assert.h>
#include <sys/stat.h>

static char dir[512];
const char *runtime_file_user_dir(void) { return dir; }

static unsigned char game_image[GAME_DATA_MAN + 16];
static unsigned char manager[16];
static unsigned char data[0x400];

static void character(uint32_t level, uint32_t insight, const uint32_t attributes[6]) {
    for (int i = 0; i < 6; ++i) put32(data, ATTRIBUTE[i], attributes[i]);
    put32(data, LEVEL, level);
    put32(data, INSIGHT, insight);
}

static void restart(void) { memset(&before, 0, sizeof(before)); memset(&known, 0, sizeof(known)); }

int main(void) {
    snprintf(dir, sizeof(dir), "out/rebirth-undo-test.d");  /* its own folder: never a player's file */
    mkdir(dir, 0755);
    char path[600];
    snprintf(path, sizeof(path), "%s/rebirth.txt", dir);
    remove(path);
    uintptr_t m = (uintptr_t)manager, d = (uintptr_t)data;
    memcpy(game_image + GAME_DATA_MAN, &m, 8);
    memcpy(manager + 8, &d, 8);
    runtime_rebirth_set_image(game_image);
    data[ORIGIN] = 0;  /* Milquetoast: level 10, 11 10 12 10 9 8 */
    data[NAME] = 'A';
    for (int i = 0; i < WEAPON_SLOTS; ++i) put32(data, WEAPONS + i * 4, 1000000);
    const uint32_t had[6] = {30, 20, 25, 30, 9, 8};

    /* 1. Rebirth, back out in the same session: everything back. */
    character(70, 12, had);
    assert(runtime_rebirth() == 1);
    assert(get32(data, LEVEL) == 10 && get32(data, INSIGHT) == 7);
    runtime_rebirth_closed();
    assert(get32(data, LEVEL) == 70 && get32(data, INSIGHT) == 12 && get32(data, ATTRIBUTE[0]) == 30);
    restart();
    assert(read_target("0:4100") == 0);  /* no free levels left behind */

    /* 2. Rebirth, the game closes in the level-up screen (saved reborn), then a list choice. */
    restart();
    character(70, 12, had);
    assert(runtime_rebirth() == 1);
    restart();
    put32(data, INSIGHT, 9);  /* Insight changed since: +2 */
    runtime_rebirth_closed();
    assert(get32(data, LEVEL) == 70 && get32(data, INSIGHT) == 14 && get32(data, ATTRIBUTE[2]) == 25);
    restart();
    assert(read_undo("0:4100").active == 0);

    /* 3. Rebirth, one level bought: kept, the rest still free, no undo later. */
    restart();
    character(70, 12, had);
    assert(runtime_rebirth() == 1);
    put32(data, LEVEL, 11);
    put32(data, ATTRIBUTE[0], 12);
    runtime_rebirth_closed();
    assert(get32(data, LEVEL) == 11 && get32(data, INSIGHT) == 7);
    restart();
    assert(read_undo("0:4100").active == 0);
    assert(read_target("0:4100") == 70);
    assert(runtime_rebirth_free_level(11) == 1);
    runtime_rebirth_closed();
    assert(get32(data, LEVEL) == 11);

    /* 4. The save from before the rebirth (crash before saving): nothing undone, nothing broken. */
    restart();
    remove(path);
    character(70, 12, had);
    assert(runtime_rebirth() == 1);
    restart();
    character(70, 12, had);
    runtime_rebirth_closed();
    assert(get32(data, LEVEL) == 70 && get32(data, INSIGHT) == 12);
    assert(read_undo("0:4100").active == 0);
    puts("rebirth undo: all cases OK");
    return 0;
}

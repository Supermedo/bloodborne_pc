/* Discord status (bbport.ini-independent; the launcher's "Show what I'm doing on Discord",
 * BB_DISCORD=0 turns it off): "Playing Bloodborne - Cathedral Ward - Level 34", or the boss during
 * a boss fight, with the time since the game (or the fight) started. It talks to the Discord app's
 * local pipe (\\.\pipe\discord-ipc-N) itself, no Discord library: a frame is {u32 opcode, u32
 * length, JSON}; opcode 0 is the handshake with the application id, opcode 1 a command
 * (SET_ACTIVITY), opcode 2 Discord closing. No Discord running: nothing happens, another try every
 * 20 s. The way to Discord and where the game keeps its area and boss bar are BBSC's (CLS_AMG,
 * GPL-2.0-or-later, src/bbsc_discord.cpp and bloodborne_re.cpp's "Discord status").
 *
 * Names are the game's own text in its language, written by scripts/discord_names.py into
 * BB_DISCORD_NAMES: "p <id> <place>" (item.msgbnd FMG 19, id = area * 100 + block * 10), "b <id>
 * <boss>" (FMG 18, the id the boss bar shows) and "l <the word Level>". Game 1.09. */
#include "runtime.h"
#ifdef _WIN32
#include <windows.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DISCORD_APPLICATION_ID ""          /* "Bloodborne" (the port's Discord application) */
#define GAME_DATA_MAN 0x553B130u            /* -> +8 player data (runtime_rebirth.c) */
#define CURRENT_MAP_LIST 0x553B148u         /* -> +0x10 maps, +0x20 index; entries 0xA0 apart */
#define BOSS_GAUGE 0x5562878u               /* -> slots 0x14 apart: +0x1658 name id, +0x165C handle */
enum { LEVEL = 0x90, MAX_NAMES = 512 };

static unsigned char *image;
typedef struct { int32_t id; char text[96]; } Name;
static Name places[MAX_NAMES], bosses[MAX_NAMES];
static int place_count, boss_count;
static char level_word[48] = "Level";

/* Guest memory read only where it is mapped and readable (the game's structures come and go). */
static int readable(uintptr_t at, size_t size) {
    MEMORY_BASIC_INFORMATION mi;
    if (at < 0x10000 || !VirtualQuery((const void *)at, &mi, sizeof(mi)) || mi.State != MEM_COMMIT) return 0;
    if (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    return at + size <= (uintptr_t)mi.BaseAddress + mi.RegionSize;
}
static uint64_t read64(uintptr_t at) { uint64_t v = 0; if (readable(at, 8)) memcpy(&v, (void *)at, 8); return v; }
static int32_t read32(uintptr_t at) { int32_t v = -1; if (readable(at, 4)) memcpy(&v, (void *)at, 4); return v; }

static void load_names(void) {
    const char *path = getenv("BB_DISCORD_NAMES");
    FILE *f = path && *path ? fopen(path, "r") : NULL;
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char kind = line[0];
        if (kind == 'l' && line[1] == ' ') { snprintf(level_word, sizeof(level_word), "%s", line + 2); continue; }
        char *text = NULL;
        long id = strtol(line + 1, &text, 10);
        if ((kind != 'p' && kind != 'b') || !text || *text != ' ') continue;
        Name *list = kind == 'p' ? places : bosses;
        int *count = kind == 'p' ? &place_count : &boss_count;
        if (*count < MAX_NAMES) {
            list[*count].id = (int32_t)id;
            snprintf(list[*count].text, sizeof(list[*count].text), "%s", text + 1);
            ++*count;
        }
    }
    fclose(f);
}
static const char *find(const Name *list, int count, int32_t id) {
    for (int i = 0; i < count; ++i) if (list[i].id == id) return list[i].text;
    return NULL;
}

/* The area the player is in: m<area>_<block> as the game packs it (0: none, at the title). */
static uint32_t current_map(void) {
    const uint64_t list = read64((uintptr_t)image + CURRENT_MAP_LIST);
    const int32_t index = read32(list + 0x20);
    const uint64_t maps = read64(list + 0x10);
    if (!list || index < 0 || !maps || index >= read32(maps + 0x18)) return 0;
    const uint64_t entries = read64(maps + 0x20);
    uint32_t packed = 0;
    if (entries && readable(entries + (uint64_t)index * 0xA0 + 8, 4)) memcpy(&packed, (void *)(entries + (uint64_t)index * 0xA0 + 8), 4);
    return packed == UINT32_MAX ? 0 : packed;
}
static void area_name(uint32_t packed, char *out, size_t size) {
    const int32_t area = (int32_t)(packed >> 24 & 0xFF), block = (int32_t)(packed >> 16 & 0xFF);
    const char *name = find(places, place_count, area * 100 + block * 10);
    if (!name) name = find(places, place_count, area * 100);
    if (name) snprintf(out, size, "%s", name);
    else if (area == 29) snprintf(out, size, "Chalice Dungeon");
    else snprintf(out, size, "m%02d_%02d", (int)area, (int)block);
}
/* The boss whose bar is shown (-1: none). */
static int32_t current_boss(void) {
    const uint64_t gauge = read64((uintptr_t)image + BOSS_GAUGE);
    if (!gauge) return -1;
    for (int slot = 0; slot < 3; ++slot)
        if (read32(gauge + 0x165C + slot * 0x14) != -1) return read32(gauge + 0x1658 + slot * 0x14);
    return -1;
}
static int32_t player_level(void) {
    const uint64_t manager = read64((uintptr_t)image + GAME_DATA_MAN);
    const uint64_t player = manager ? read64(manager + 8) : 0;
    return player ? read32(player + LEVEL) : -1;
}

static void json_text(char *out, size_t size, const char *text) {
    size_t used = 0;
    if (size < 3) return;
    out[used++] = '"';
    for (const char *c = text; *c && used + 3 < size; ++c) {
        if (*c == '"' || *c == '\\') out[used++] = '\\';
        out[used++] = (unsigned char)*c < 0x20 ? ' ' : *c;
    }
    out[used++] = '"';
    out[used] = 0;
}

static HANDLE pipe = INVALID_HANDLE_VALUE;
static void pipe_close(void) { if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe); pipe = INVALID_HANDLE_VALUE; }
static int pipe_send(uint32_t opcode, const char *json) {
    const uint32_t length = (uint32_t)strlen(json);
    char *frame = malloc(8 + length);
    if (!frame) return 0;
    memcpy(frame, &opcode, 4); memcpy(frame + 4, &length, 4); memcpy(frame + 8, json, length);
    DWORD written = 0;
    const int ok = WriteFile(pipe, frame, 8 + length, &written, NULL) && written == 8 + length;
    free(frame);
    return ok;
}
/* Reads what Discord sent so the pipe never fills; wait: one frame (the handshake's answer).
 * 0: the pipe is gone or Discord closed it. */
static int pipe_drain(int wait) {
    for (;;) {
        DWORD available = 0, got = 0;
        if (!PeekNamedPipe(pipe, NULL, 0, NULL, &available, NULL)) { pipe_close(); return 0; }
        if (available < 8 && !wait) return 1;
        uint32_t header[2];
        if (!ReadFile(pipe, header, 8, &got, NULL) || got != 8) { pipe_close(); return 0; }
        char *body = malloc(header[1] + 1);
        if (!body || (header[1] && (!ReadFile(pipe, body, header[1], &got, NULL) || got != header[1]))) {
            free(body); pipe_close(); return 0;
        }
        body[header[1]] = 0;
        if (header[0] == 2) {
            printf("Discord status: Discord closed the connection: %s\n", body);
            free(body); pipe_close(); return 0;
        }
        free(body);
        wait = 0;
    }
}
static int pipe_connect(const char *application) {
    for (int i = 0; i < 10 && pipe == INVALID_HANDLE_VALUE; ++i) {
        char name[64];
        snprintf(name, sizeof(name), "\\\\.\\pipe\\discord-ipc-%d", i);
        pipe = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    }
    if (pipe == INVALID_HANDLE_VALUE) return 0;
    char hello[128];
    snprintf(hello, sizeof(hello), "{\"v\":1,\"client_id\":\"%s\"}", application);
    if (!pipe_send(0, hello)) { pipe_close(); return 0; }
    return pipe_drain(1);
}

static DWORD WINAPI discord_thread(LPVOID application_arg) {
    const char *application = application_arg;
    const int64_t game_start = (int64_t)time(NULL);
    int64_t boss_start = 0;
    int32_t last_boss = -1;
    char sent[1024] = "";
    uint64_t nonce = 0, next_try = 0;
    int said = 0;
    load_names();
    char logged[384] = "";
    for (;;) {
        Sleep(2000);
        /* What the player does now. */
        const uint32_t map = current_map();
        const int32_t boss = map ? current_boss() : -1;
        if (boss != last_boss) boss_start = (int64_t)time(NULL);
        last_boss = boss;
        char area[96] = "", details[256] = "", state[128] = "", large[160] = "";
        if (map) area_name(map, area, sizeof(area));
        const char *boss_name = boss != -1 ? find(bosses, boss_count, boss) : NULL;
        if (boss != -1) json_text(details, sizeof(details), boss_name ? boss_name : area);
        else if (map) json_text(details, sizeof(details), area);
        const int32_t level = map ? player_level() : -1;
        if (level > 0 && level < 1000) {
            char text[80];
            snprintf(text, sizeof(text), "%s %d", level_word, (int)level);
            json_text(state, sizeof(state), text);
        }
        json_text(large, sizeof(large), boss != -1 && map ? area : "Bloodborne");
        char activity[1024];
        int used = snprintf(activity, sizeof(activity), "{");
        if (details[0]) used += snprintf(activity + used, sizeof(activity) - used, "\"details\":%s,", details);
        if (state[0]) used += snprintf(activity + used, sizeof(activity) - used, "\"state\":%s,", state);
        snprintf(activity + used, sizeof(activity) - used,
                 "\"timestamps\":{\"start\":%" PRId64 "},\"assets\":{\"large_image\":\"bloodborne\",\"large_text\":%s}}",
                 boss != -1 ? boss_start : game_start, large);
        /* In the log when it changes (reports show what Discord was told). */
        char line[384];
        snprintf(line, sizeof(line), "%s | %s", details[0] ? details : "-", state[0] ? state : "-");
        if (strcmp(line, logged)) {
            printf("Discord status: %s\n", line);
            fflush(stdout);
            snprintf(logged, sizeof(logged), "%s", line);
        }
        if (pipe == INVALID_HANDLE_VALUE) {
            if (GetTickCount64() < next_try) continue;
            if (!pipe_connect(application)) { next_try = GetTickCount64() + 20000; continue; }
            if (!said) { printf("Discord status: connected to Discord\n"); fflush(stdout); said = 1; }
            sent[0] = 0;
        }
        if (!pipe_drain(0)) continue;
        if (!strcmp(activity, sent)) continue;
        char command[1400];
        snprintf(command, sizeof(command), "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%lu,\"activity\":%s},\"nonce\":\"%" PRIu64 "\"}",
                 GetCurrentProcessId(), activity, ++nonce);
        if (pipe_send(1, command)) snprintf(sent, sizeof(sent), "%s", activity);
        else pipe_close();
    }
    return 0;
}

void runtime_discord_start(unsigned char *base) {
    image = base;
    const char *off = getenv("BB_DISCORD");
    if (off && off[0] == '0') return;
    const char *id = getenv("BB_DISCORD_APP_ID");
    static char application[32];
    snprintf(application, sizeof(application), "%s", id && *id ? id : DISCORD_APPLICATION_ID);
    if (!application[0]) return; /* no Discord application yet */
    HANDLE thread = CreateThread(NULL, 0, discord_thread, application, 0, NULL);
    if (thread) {
        CloseHandle(thread);
        printf("Discord status: on (shown while the Discord app runs; BB_DISCORD=0 turns it off)\n");
    }
}
#else
void runtime_discord_start(unsigned char *base) { (void)base; }
#endif

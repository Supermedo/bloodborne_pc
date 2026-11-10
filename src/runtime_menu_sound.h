/* Decode the player's own Bloodborne UI sounds; no game audio is packaged. */
#ifndef BB_RUNTIME_MENU_SOUND_H
#define BB_RUNTIME_MENU_SOUND_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { int16_t *pcm; uint32_t frames; } BbMenuSound;
typedef struct { BbMenuSound move, select; } BbMenuSounds;
typedef struct { uint32_t decoded_move,decoded_select,queued_move,queued_select,mixed_move,mixed_select,muted; } BbMenuSoundStats;
int runtime_menu_sound_load(const char *bank_path, BbMenuSounds *out);
void runtime_menu_sound_free(BbMenuSounds *sounds);
/* 0 = cursor move, 1 = select. The audio runtime mixes into the game's main port. */
void runtime_audio_menu_event(int select);
void runtime_audio_menu_stats(BbMenuSoundStats *out);
#ifdef __cplusplus
}
#endif
#endif

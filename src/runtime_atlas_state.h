/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BB_RUNTIME_ATLAS_STATE_H
#define BB_RUNTIME_ATLAS_STATE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void runtime_atlas_state_bind(void *image, uint64_t size);
/* -1 means unreadable/unsupported, 0 means off, 1 means on. */
int runtime_atlas_flag(uint32_t id);
uintptr_t runtime_atlas_player_token(void);
/* Travel requests contain only a verified native destination and its eligibility. */
typedef struct {
    int32_t return_point;
    uint32_t unlock_flag;
    uint32_t on[8], off[8];
    uint8_t on_count, off_count;
    float arrival[3];
} BbAtlasTravelDestination;
int runtime_atlas_travel_request(const BbAtlasTravelDestination *destination);
/* 0 idle, 1 queued, 2 loading, 3 arrived, -1 rejected, -2 timed out. */
int runtime_atlas_travel_status(void);
int runtime_atlas_travel_supported(void);
#ifdef __cplusplus
}
#endif
#endif

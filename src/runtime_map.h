/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BB_RUNTIME_MAP_H
#define BB_RUNTIME_MAP_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { float x, y, z, yaw; uint32_t block; int valid; } BbMapLocation;
typedef struct { int supported, top_level, selected; } BbMapMenu;
void runtime_map_bind(void *image, uint64_t size);
int runtime_map_location(BbMapLocation *out);
int runtime_map_menu_open(void);
int runtime_map_menu_state(BbMapMenu *out);
uint32_t runtime_map_confirm_button(void);
#ifdef __cplusplus
}
#endif
#endif

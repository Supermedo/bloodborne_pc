/* SPDX-License-Identifier: GPL-2.0-or-later
 * Local observation only: no writes, game calls, hooks, or save access.
 * 1.09 layout facts cross-checked against bbhost's public engine documentation.
 */
#include "runtime_map.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#endif
static uintptr_t world_slot;
static uintptr_t menu_slot;
static uintptr_t menu_main_slot,image_base;

void runtime_map_bind(void *image, uint64_t size) {
    world_slot=0;
    menu_slot=0;
    menu_main_slot=0;image_base=0;
    const char *enabled=getenv("BB_ATLAS_ENABLED");
    if(!enabled || strcmp(enabled,"1"))return;
    /* Anchors from the installed CUSA00207 1.09 executable. Full anchors rather
     * than prologues prevent a future executable silently using stale offsets. */
    if (!image || size<0x553e880) return;
    /* MAP_BUILD_GUARD is an image byte fingerprint generated during packaging. */
#include "map_build_guard.inc"
    if (memcmp((unsigned char *)image+BB_MAP_GUARD_OFFSET,bb_map_guard,sizeof(bb_map_guard))) {
        puts("Map: executable layout mismatch; position tracking disabled");return;
    }
    world_slot=(uintptr_t)image+0x553e878;
    /* Game IsShowMenu reads this manager; +0x54 contributes the player-menu bit.
     * Fingerprint the exact predicate, and observe without calling or patching it. */
    static const unsigned char menu_guard[]={0x48,0x8b,0x05,0xd6,0x6f,0xdf,0x03,0x83,0x78,0x3c,0x00};
    /* The native grid's selected-index read, independently correlated with
     * actual 0/1/2 top-row selection and 8 below System in isolated runtime.
     * No writes or game function calls are used by this bridge. */
    static const unsigned char focus_guard[]={0x8b,0x8b,0xe8,0x0a,0,0,0x85,0xc9,0x79,0x06,0x8b,0x8b,0xe4,0x0a,0,0};
    if(size>0x5562880 && !memcmp((unsigned char*)image+0x176b89b,menu_guard,sizeof(menu_guard)) &&
       !memcmp((unsigned char*)image+0x1bdf835,focus_guard,sizeof(focus_guard))) {
        image_base=(uintptr_t)image;menu_slot=image_base+0x5562878;menu_main_slot=image_base+0x5562870;
    }
    puts("Map: read-only local position reader ready");
}

static int read_at(uintptr_t at,void *out,size_t bytes) {
    if (!at || at<0x10000) return 0;
#ifdef _WIN32
    SIZE_T done=0;
    return ReadProcessMemory(GetCurrentProcess(),(const void *)at,out,bytes,&done) && done==bytes;
#else
    (void)out;(void)bytes;return 0;
#endif
}
static uintptr_t pointer(uintptr_t at) {
    uintptr_t p=0;return read_at(at,&p,sizeof(p)) ? p : 0;
}
int runtime_map_menu_open(void) {
    uintptr_t menu=pointer(menu_slot);int value=0;
    if(!menu || !read_at(menu+0x54,&value,sizeof(value)))return 0;
    return value==2; /* Stable shown state, verified against the actual quick menu. */
}
uint32_t runtime_map_confirm_button(void) {
    /* The game uses its region predicate, not the platform enter-button setting.
     * Observe the exact applied 1.09 region/confirm patch; never execute or alter it.
     * Unlock Game Region returns false (Cross), Circle Confirm returns true.
     * An unknown predicate cannot safely establish menu input ownership. */
    static const unsigned char cross[]={0x31,0xc0,0xc3,0x90,0x90,0x90};
    static const unsigned char circle[]={0xb8,0x01,0x00,0x00,0x00,0xc3};
    unsigned char code[6];
    /* The manifest-supported US build's original predicate uses Cross. */
#include "map_confirm_guard.inc"
    unsigned char original[sizeof(bb_map_cross_region_guard)];
    if(!image_base || !read_at(image_base+0x1fb8730,code,sizeof(code)))return 0;
    if(!memcmp(code,cross,sizeof(cross)))return 0x4000u;
    if(!memcmp(code,circle,sizeof(circle)))return 0x2000u;
    if(read_at(image_base+0x1fb8730,original,sizeof(original)) &&
       !memcmp(original,bb_map_cross_region_guard,sizeof(original)))return 0x4000u;
    return 0;
}
int runtime_map_menu_state(BbMapMenu *out) {
    if(!out)return 0;
    memset(out,0,sizeof(*out));out->selected=-1;
    if(!runtime_map_confirm_button())return 0;
    uintptr_t menu=pointer(menu_slot),main=pointer(menu_main_slot);
    uintptr_t list=pointer(main?main+0x80:0);
    uint32_t status=0,shown=0;int shape[6]={0};
    if(!menu || !main || !list || pointer(list)!=image_base+0x539c680 ||
       !read_at(menu+0x54,&shown,sizeof(shown)) ||
       !read_at(list+0x98,&status,sizeof(status)) ||
       !read_at(list+0xae0,shape,sizeof(shape)))return 0;
    /* This native list is a four-row six-slot grid, with only three enabled
     * items on its first row. +0x98 is 1 while the list is interactive and 2
     * while hidden behind nested dialogs; manager+0x54 stays 2 in both. */
    if(shape[0]!=24 || shape[3]!=6 || shape[4]!=4 || shape[1]<0 || shape[1]>=24 || shape[2]!=-1)return 0;
    if(main!=pointer(menu_main_slot) || list!=pointer(main+0x80))return 0;
    out->supported=1;out->top_level=shown==2 && status==1;out->selected=shape[1];return 1;
}
int runtime_map_location(BbMapLocation *out) {
    if (!out) return 0;
    memset(out,0,sizeof(*out));
    if (!world_slot) return 0;
    uintptr_t world=pointer(world_slot),chr=pointer(world ? world+0x60 : 0);
    uintptr_t modules=pointer(chr ? chr+0x3b0 : 0);
    uintptr_t transform=pointer(modules ? modules+0x68 : 0);
    float v[3],yaw;uint32_t block;
    if (!transform || !pointer(chr+0x288) ||
        !read_at(transform+0x1e0,v,sizeof(v)) || !read_at(transform+0x1d4,&yaw,sizeof(yaw)) ||
        !read_at(chr+0x3f8,&block,sizeof(block))) return 0;
    if (chr!=pointer(world+0x60) || transform!=pointer(modules+0x68)) return 0;
    unsigned area=block>>24;
    if (area<21 || area>36 || block==0xffffffffu || !isfinite(yaw) ||
        !isfinite(v[0]) || !isfinite(v[1]) || !isfinite(v[2]) ||
        fabsf(v[0])>20000 || fabsf(v[1])>20000 || fabsf(v[2])>20000) return 0;
    out->x=v[0];out->y=v[1];out->z=v[2];out->yaw=yaw;out->block=block;out->valid=1;
    return 1;
}

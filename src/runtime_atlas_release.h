/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BB_ATLAS_RELEASE_H
#define BB_ATLAS_RELEASE_H
#include <stdint.h>
typedef struct {uint32_t buttons;unsigned axes,triggers;} BbAtlasRelease;
static inline int bb_atlas_axis_held(uint8_t value){return value<110 || value>146;}
static inline void bb_atlas_remember(BbAtlasRelease *s,uint32_t buttons,const uint8_t axes[4],const uint8_t triggers[2]){
    s->buttons|=buttons;
    for(unsigned i=0;i<4;++i)if(bb_atlas_axis_held(axes[i]))s->axes|=1u<<i;
    for(unsigned i=0;i<2;++i)if(triggers[i]>32)s->triggers|=1u<<i;
}
static inline void bb_atlas_release_filter(BbAtlasRelease *s,uint32_t raw_buttons,uint32_t *buttons,uint8_t axes[4],uint8_t triggers[2]){
    s->buttons&=raw_buttons;*buttons&=~s->buttons;
    for(unsigned i=0;i<4;++i)if(s->axes&(1u<<i)){if(!bb_atlas_axis_held(axes[i]))s->axes&=~(1u<<i);else axes[i]=128;}
    for(unsigned i=0;i<2;++i)if(s->triggers&(1u<<i)){if(triggers[i]<=32)s->triggers&=~(1u<<i);else triggers[i]=0;}
}
#endif

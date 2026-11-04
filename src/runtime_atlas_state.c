/* SPDX-License-Identifier: GPL-2.0-or-later
 * Atlas's guarded event-flag observation and explicitly enabled lamp travel.
 * Layouts and entry bytes verified against the supported local 1.09 image.
 * Flags are only read. Travel uses the game's normal return-point/loading calls.
 */
#include "runtime_atlas_state.h"
#include "runtime_map.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <stdatomic.h>
#ifdef _WIN32
#include <windows.h>
#endif
#define ATLAS_ABI __attribute__((sysv_abi))
static uintptr_t base;
static int state_ready, travel_ready;
static atomic_int travel_status;
static BbAtlasTravelDestination pending;
static uintptr_t pending_player;
static uint64_t request_time;
static uint32_t source_block;
static int observed_loading;
#ifdef _WIN32
static SRWLOCK request_lock = SRWLOCK_INIT;
static int64_t (ATLAS_ABI *original_frame)(void *);
#endif

static int read_mem(uintptr_t at, void *out, size_t size) {
#ifdef _WIN32
    SIZE_T done = 0;
    return at >= 0x10000 && ReadProcessMemory(GetCurrentProcess(), (const void*)at, out, size, &done) && done == size;
#else
    (void)at; (void)out; (void)size; return 0;
#endif
}
static uintptr_t ptr(uintptr_t at) { uintptr_t p=0; return read_mem(at,&p,sizeof(p))?p:0; }
uintptr_t runtime_atlas_player_token(void) {
    if(!state_ready) return 0;
    uintptr_t world=ptr(base+0x553e878);
    return world?ptr(world+0x60):0;
}
int runtime_atlas_flag(uint32_t id) {
    if(!state_ready || id>99999999u) return -1;
    uintptr_t man=ptr(base+0x553b100),head,node,found;
    uint32_t bits=0;
    if(!man || !read_mem(man+0x1c,&bits,4) || bits!=1000) return -1;
    head=ptr(man+0x38); node=head?ptr(head+8):0; found=head;
    const uint32_t group=id/bits, bit=id%bits;
    int stopped=0;
    for(int depth=0;depth<64;++depth) {
        unsigned char nil=1; uint32_t key=0;
        if(!node || !read_mem(node+0x19,&nil,1))return -1;
        if(nil){stopped=1;break;}
        if(!read_mem(node+0x20,&key,4))return -1;
        if(key<group)node=ptr(node+0x10);
        else {found=node;node=ptr(node);}
    }
    uint32_t key=0,kind=0;uintptr_t data=0;
    if(!stopped || found==head || !read_mem(found+0x20,&key,4) || key!=group || !read_mem(found+0x28,&kind,4))return -1;
    if(kind==2)data=ptr(found+0x30);
    else if(kind==1) {
        uint32_t stride=0,index=0;uintptr_t pool=ptr(man+0x28);
        if(!pool || !read_mem(man+0x20,&stride,4) || stride<125 || stride>4096 || !read_mem(found+0x30,&index,4))return -1;
        uint64_t offset=(uint64_t)stride*index;if(offset>UINT32_MAX)return -1;
        data=pool+(uintptr_t)offset;
    } else return -1;
    unsigned char value=0;
    if(!data || !read_mem(data+(bit>>3),&value,1) || man!=ptr(base+0x553b100))return -1;
    return (value&(0x80u>>(bit&7)))?1:0;
}
static int eligible(const BbAtlasTravelDestination *d) {
    if(d->return_point<=0 || d->on_count>8 || d->off_count>8)return 0;
    if(d->unlock_flag && runtime_atlas_flag(d->unlock_flag)!=1)return 0;
    for(unsigned i=0;i<d->on_count;++i)if(runtime_atlas_flag(d->on[i])!=1)return 0;
    for(unsigned i=0;i<d->off_count;++i)if(runtime_atlas_flag(d->off[i])!=0)return 0;
    return 1;
}
int runtime_atlas_travel_supported(void){return travel_ready;}
int runtime_atlas_travel_status(void){return atomic_load_explicit(&travel_status,memory_order_acquire);}
int runtime_atlas_travel_request(const BbAtlasTravelDestination *d) {
#ifdef _WIN32
    BbMapLocation loc={0};
    BbMapMenu menu={0};
    if(!travel_ready || !d || !runtime_map_location(&loc) || !runtime_map_menu_state(&menu) || !menu.top_level || !eligible(d))return 0;
    for(int i=0;i<3;++i)if(!isfinite(d->arrival[i])||fabsf(d->arrival[i])>20000)return 0;
    AcquireSRWLockExclusive(&request_lock);
    int status=atomic_load(&travel_status);
    if(status==1 || status==2){ReleaseSRWLockExclusive(&request_lock);return 0;}
    pending=*d;pending_player=runtime_atlas_player_token();request_time=GetTickCount64();source_block=loc.block;observed_loading=0;
    atomic_store_explicit(&travel_status,1,memory_order_release);
    ReleaseSRWLockExclusive(&request_lock);return 1;
#else
    (void)d;return 0;
#endif
}
#ifdef _WIN32
static void game_tick(void) {
    int status=atomic_load_explicit(&travel_status,memory_order_acquire);
    if(status!=1 && status!=2)return;
    BbMapLocation loc={0};const int valid=runtime_map_location(&loc);
    if(status==2) {
        if(!valid)observed_loading=1;
        /* Placement may keep the same block; require loading/player replacement
         * or enough time for the native stage change, then its target block. */
        const uint32_t block=((uint32_t)(pending.return_point/100000)<<24)|((uint32_t)((pending.return_point/10000)%10)<<16);
        if(valid && loc.block==block && GetTickCount64()-request_time>1500 &&
           (observed_loading || loc.block!=source_block || runtime_atlas_player_token()!=pending_player ||
            (fabsf(loc.x-pending.arrival[0])<20&&fabsf(loc.y-pending.arrival[1])<20&&fabsf(loc.z-pending.arrival[2])<20)))atomic_store(&travel_status,3);
        else if(GetTickCount64()-request_time>60000)atomic_store(&travel_status,-2);
        return;
    }
    AcquireSRWLockExclusive(&request_lock);
    if(atomic_load(&travel_status)!=1){ReleaseSRWLockExclusive(&request_lock);return;}
    BbMapMenu menu={0};
    uintptr_t data=ptr(base+0x553b130),record=data?ptr(data+8):0;int32_t hp=0;
    if(!valid || !record || !read_mem(record+0x14,&hp,4) || hp<=0 ||
       !runtime_map_menu_state(&menu) || !menu.top_level || runtime_atlas_player_token()!=pending_player ||
       GetTickCount64()-request_time>3000 || !eligible(&pending)) {
        atomic_store(&travel_status,-1);ReleaseSRWLockExclusive(&request_lock);return;
    }
    /* These three exact SysV entry points are fingerprinted before installation.
     * First choose the return point for arrival and the following respawn; then
     * request the game's normal stage change. No flag setter is called. */
    typedef int64_t (ATLAS_ABI *SetReturn)(int32_t);
    typedef int64_t (ATLAS_ABI *UseReturn)(int32_t);
    typedef int64_t (ATLAS_ABI *Warp)(uintptr_t,int32_t);
    const int32_t destination=pending.return_point;
    request_time=GetTickCount64();atomic_store_explicit(&travel_status,2,memory_order_release);
    ReleaseSRWLockExclusive(&request_lock);
    ((SetReturn)(base+0x156d110))(destination);
    ((UseReturn)(base+0x156d0e0))(1);
    ((Warp)(base+0x132e050))(0,destination);
    printf("Atlas: native lamp travel queued to %d\n",destination);
}
static int64_t ATLAS_ABI frame_bridge(void *self) {
    game_tick();return original_frame(self);
}
static int guard(uintptr_t rva,const unsigned char *bytes,size_t n,uint64_t size) {
    return rva<=size && n<=size-rva && !memcmp((const void*)(base+rva),bytes,n);
}
#endif
void runtime_atlas_state_bind(void *image,uint64_t size) {
    state_ready=travel_ready=0;base=(uintptr_t)image;
    const char *enabled=getenv("BB_ATLAS_ENABLED");
    if(!enabled || strcmp(enabled,"1"))return;
    if(!image || size<0x553e880)return;
#ifdef _WIN32
    static const unsigned char flag_guard[]={0x53,0x49,0x89,0xd0,0x8b,0x4f,0x1c,0x31,0xd2,0x89,0xf0,0xf7,0xf1,0x41,0x89,0xc2,0x44,0x0f,0xaf,0xd1,0x48,0x8b,0x5f,0x38};
#include "map_build_guard.inc"
    if(!guard(BB_MAP_GUARD_OFFSET,bb_map_guard,sizeof(bb_map_guard),size) ||
       !guard(0x13cfc00,flag_guard,sizeof(flag_guard),size)){
        puts("Atlas: executable fingerprint mismatch; flags and travel disabled");return;
    }
    state_ready=1;puts("Atlas: guarded read-only event flags ready");
    const char *opt=getenv("BB_MAP_TRAVEL_ENABLED");if(!opt || strcmp(opt,"1"))return;
    static const unsigned char frame[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x38};
    static const unsigned char warp[]={0x55,0x48,0x89,0xe5,0x48,0x63,0xc6,0x48,0x69,0xc8,0x89,0xb5,0xf8,0x14,0x48,0x89,0xca,0x48,0xc1,0xea,0x3f,0x48,0xc1,0xe9,0x2d,0x01,0xd1};
    static const unsigned char ret[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x53,0x48,0x83,0xec,0x28,0x89,0xfb,0x4c,0x8b,0x35,0xba,0x6a,0xe7,0x03};
    static const unsigned char use[]={0x48,0x8b,0x05,0x91,0x95,0xfe,0x03,0x40,0x88,0xb8,0x24,0x15,0,0,0xc3};
    if(!guard(0x2034770,frame,sizeof(frame),size)||!guard(0x132e050,warp,sizeof(warp),size)||
       !guard(0x156d110,ret,sizeof(ret),size)||!guard(0x156d0e0,use,sizeof(use),size)){
        puts("Atlas: travel unavailable; native entry fingerprint mismatch");return;
    }
    unsigned char *trampoline=VirtualAlloc(NULL,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    if(!trampoline)return;
    memcpy(trampoline,frame,sizeof(frame));trampoline[17]=0xff;trampoline[18]=0x25;memset(trampoline+19,0,4);
    uintptr_t resume=base+0x2034770+17;memcpy(trampoline+23,&resume,8);
    DWORD old=0;
    if(!VirtualProtect(trampoline,4096,PAGE_EXECUTE_READ,&old)){VirtualFree(trampoline,0,MEM_RELEASE);return;}
    original_frame=(int64_t (ATLAS_ABI *)(void*))trampoline;
    unsigned char jump[17]={0xff,0x25,0,0,0,0};uintptr_t target=(uintptr_t)frame_bridge;
    memcpy(jump+6,&target,8);memset(jump+14,0x90,3);
    void *site=(void*)(base+0x2034770);
    if(!VirtualProtect(site,17,PAGE_EXECUTE_READWRITE,&old)){VirtualFree(trampoline,0,MEM_RELEASE);original_frame=NULL;return;}
    memcpy(site,jump,17);FlushInstructionCache(GetCurrentProcess(),site,17);
    DWORD ignored=0;VirtualProtect(site,17,old,&ignored);
    travel_ready=1;puts("Atlas: opt-in native lamp travel ready on the main game thread");
#else
    (void)size;
#endif
}

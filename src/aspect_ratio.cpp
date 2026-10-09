// SPDX-License-Identifier: GPL-3.0-or-later
// Bloodborne 1.09 camera and floating UI corrections adapted from bbhost's
// src/engine/live_resolution.cpp and src/engine/graphics_patch.cpp (7c790536).
#include "aspect_thunk.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {
constexpr uint64_t Base = 0x400000;
constexpr float OriginalAspect = 16.0f / 9.0f;
float aspect = OriginalAspect;
constexpr uint64_t Blend = 0x18368b0;
constexpr uint8_t Prologue[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,
    0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x48};
struct Stage { uint64_t at; bool height, rcx = false; };
constexpr Stage Stages[] = {
    {0x19e83af,false},{0x1ffd491,false},{0x1ffd4d1,true},
    {0x1a44357,false},{0x1a44365,true},{0x1a44c55,false},{0x1a44c63,true},
    {0x1a452c7,false},{0x1a452d5,true},{0x1a54df6,false},{0x1a54e1c,true},
    {0x1a55611,true},{0x1a55628,false},{0x1ab3372,false,true},{0x1ab3398,true}
};
struct Bound { uint64_t at; uint8_t game[3], open[3], n; bool vertical; };
constexpr Bound Bounds[] = {
    {0x1ffd473,{0x77,0x30},{0x66,0x90},2,false},
    {0x1a54dd9,{0x77,0x2f},{0x66,0x90},2,false},
    {0x1ab3370,{0x77,0x5a},{0x66,0x90},2,false},
    {0x1a42520,{0x0f,0x96,0xc1},{0xb1,0x01,0x90},3,false},
    {0x1ffd4cf,{0x77,0x14},{0x66,0x90},2,true},
    {0x1a54e1a,{0x77,0x14},{0x66,0x90},2,true},
    {0x1ab3396,{0x77,0x34},{0x66,0x90},2,true},
    {0x1a4253c,{0x77,0x20},{0x66,0x90},2,true}
};
constexpr uint64_t Scales[] = {0x4cf9a00,0x4cf9a60,0x4cf9ad0};
void __attribute__((sysv_abi)) camera_hook(uint64_t, const uint64_t* saved) {
    auto* mgr = reinterpret_cast<uint8_t*>(saved[5]);
    if (!mgr) return;
    auto fix = [](uint8_t* cam) {
        if (!cam) return;
        float a; std::memcpy(&a,cam+0x54,4);
        if (std::fabs(a-OriginalAspect)<1e-4f) std::memcpy(cam+0x54,&aspect,4);
    };
    uint8_t *a, *b;
    std::memcpy(&a,mgr+0x60,8); std::memcpy(&b,mgr+0x68,8);
    fix(a); fix(b); fix(mgr);
}
// The upstream emitter uses the return value to decide whether to replay.
int64_t __attribute__((sysv_abi)) hook(uint64_t id,const uint64_t* saved) {
    camera_hook(id,saved); return 0;
}
void pinned(uint8_t* out, const Stage& s, uint32_t value) {
    const uint8_t code[] = {uint8_t(s.rcx?0xb9:0xb8),0,0,0,0,0x0f,0x1f,0x40,0};
    std::memcpy(out,code,9); std::memcpy(out+1,&value,4);
}
}

extern "C" void aspect_ratio_bind_image(void* image,uint64_t size) {
    uint32_t w=1920,h=1080;
    if (const char* res=std::getenv("BB_OUTPUT_RES")) std::sscanf(res,"%ux%u",&w,&h);
    if (!w || !h || uint64_t(w)*9==uint64_t(h)*16) return;
    const char* signature=std::getenv("BB_ASPECT_EBOOT_SEGMENTS");
    if (!signature || std::strcmp(signature,"cbe97cb9e7b830195c93e7ed139cbaaeea66302da82f1b39ca31f90a3ceccc52")) {
        std::puts("Aspect: requires verified Bloodborne 1.09; no hooks applied"); return;
    }
    auto* bytes=static_cast<uint8_t*>(image);
    auto at=[&](uint64_t address,size_t n)->uint8_t* {
        return address>=Base && address-Base+n<=size ? bytes+address-Base : nullptr;
    };
    auto equals=[&](uint64_t address,const void* expected,size_t n) {
        auto* p=at(address,n); return p && std::memcmp(p,expected,n)==0;
    };
    bool valid=equals(Blend,Prologue,sizeof(Prologue));
    const float scale0[]={0.5f,-0.5f}, right0=1920, bottom0=1080;
    for (auto a:Scales) valid &= equals(a,scale0,8);
    valid &= equals(0x4d26ea4,&right0,4) && equals(0x4d26ea8,&bottom0,4);
    for (const auto& b:Bounds) valid &= equals(b.at,b.game,b.n);
    for (const auto& s:Stages) {
        uint8_t original[]={0x48,0x8d,uint8_t(s.rcx?0x0d:0x05),0,0,0,0,0x8b,uint8_t(s.rcx?0x09:0)};
        const int32_t disp=int32_t((s.height?0x55289fc:0x55289f8)-(s.at+7));
        std::memcpy(original+3,&disp,4);
        uint8_t patched[9]; pinned(patched,s,s.height?1080:1920);
        valid &= equals(s.at,original,9) || equals(s.at,patched,9);
    }
    if (!valid) { std::puts("Aspect: conflicting camera/UI patch; no hooks applied"); return; }
    auto* stub=static_cast<uint8_t*>(VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    if (!stub) { std::puts("Aspect: cannot allocate camera trampoline"); return; }
    thunk_emit_prologue_stub(stub,0,reinterpret_cast<void*>(hook),
        reinterpret_cast<uint64_t>(at(Blend,sizeof(Prologue)))+sizeof(Prologue),Prologue,sizeof(Prologue),false);
    DWORD old=0;
    if (!VirtualProtect(stub,4096,PAGE_EXECUTE_READ,&old)) {
        VirtualFree(stub,0,MEM_RELEASE); std::puts("Aspect: cannot protect camera trampoline"); return;
    }
    FlushInstructionCache(GetCurrentProcess(),stub,4096);
    aspect=float(w)/float(h);
    const float rx=std::max(1.0f,aspect/OriginalAspect), ry=std::max(1.0f,OriginalAspect/aspect);
    const uint32_t right=1920+uint32_t(std::ceil(960*(rx-1))), bottom=1080+uint32_t(std::ceil(540*(ry-1)));
    const float scale[]={0.5f*rx,-0.5f*ry}, right_f=float(right), bottom_f=float(bottom);
    for (auto a:Scales) std::memcpy(at(a,8),scale,8);
    std::memcpy(at(0x4d26ea4,4),&right_f,4); std::memcpy(at(0x4d26ea8,4),&bottom_f,4);
    for (const auto& s:Stages) {
        uint32_t v=s.height?1080:1920;
        if (s.at==0x1ffd491 || s.at==0x1a54df6 || s.at==0x1ab3372) v=right;
        if (s.at==0x1ffd4d1 || s.at==0x1a54e1c || s.at==0x1ab3398) v=bottom;
        pinned(at(s.at,9),s,v);
    }
    for (const auto& b:Bounds) std::memcpy(at(b.at,b.n),
        (b.vertical?ry>1.0005f:rx>1.0005f)?b.open:b.game,b.n);
    uint8_t jump[sizeof(Prologue)]={0xff,0x25,0,0,0,0};
    const uint64_t address=reinterpret_cast<uint64_t>(stub);
    std::memcpy(jump+6,&address,8); std::fill(jump+14,jump+sizeof(jump),0x90);
    std::memcpy(at(Blend,sizeof(jump)),jump,sizeof(jump));
    std::printf("Aspect: %ux%u, camera %.6f (vertical FOV kept), centered 1920x1080 HUD; plates x -%u..%u y -%u..%u\n",
        w,h,double(aspect),right-1920,right,bottom-1080,bottom);
}

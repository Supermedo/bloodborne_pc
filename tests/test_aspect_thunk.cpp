// SPDX-License-Identifier: GPL-3.0-or-later
// Execute the camera trampoline's ABI bridge, including floating-point arguments.
#include "src/aspect_thunk.h"
#include <windows.h>
#include <cassert>
#include <cstdio>
using Fn = int64_t(__attribute__((sysv_abi)) *)(int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,float,double);
static bool called;
int64_t __attribute__((sysv_abi)) inspect(uint64_t id,const uint64_t* saved) {
    assert(id==42 && saved[5]==11 && int64_t(saved[4])==-12 && saved[3]==13 &&
           int64_t(saved[2])==-14 && saved[1]==15 && int64_t(saved[0])==-16);
    called=true; return 0;
}
int64_t __attribute__((sysv_abi)) resume(int64_t a,int64_t b,int64_t c,int64_t d,int64_t e,int64_t f,float x,double y) {
    assert(called && a==11 && b==-12 && c==13 && d==-14 && e==15 && f==-16);
    assert(x==1.25f && y==-2.5);
    return 0x12345678;
}
int main() {
    auto* page=static_cast<uint8_t*>(VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    assert(page);
    auto* end=thunk_emit_prologue_stub(page,42,reinterpret_cast<void*>(inspect),
        reinterpret_cast<uint64_t>(resume),nullptr,0,false);
    DWORD old=0; assert(VirtualProtect(page,4096,PAGE_EXECUTE_READ,&old));
    FlushInstructionCache(GetCurrentProcess(),page,end-page);
    assert(reinterpret_cast<Fn>(page)(11,-12,13,-14,15,-16,1.25f,-2.5)==0x12345678);
    VirtualFree(page,0,MEM_RELEASE);
    std::puts("Aspect camera trampoline: six SysV integer arguments, XMM float arguments, stack alignment and resume PASS");
}

// bbport co-op: the memory calls NpSignaling makes for its private heap (host memory here).
#pragma once
#include "common/types.h"

namespace Libraries::Kernel {
s32 PS4_SYSV_ABI sceKernelMapNamedFlexibleMemory(void** addr_in_out, u64 len, s32 prot, s32 flags,
                                                 const char* name);
s32 PS4_SYSV_ABI sceKernelMunmap(void* addr, u64 len);
} // namespace Libraries::Kernel

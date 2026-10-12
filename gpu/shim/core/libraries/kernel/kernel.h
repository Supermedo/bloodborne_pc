// bbport: the parts of shadPS4's kernel.h used by vendored libraries.
#pragma once
#include "common/types.h"
#include "core/libraries/kernel/orbis_error.h"
namespace Libraries::Kernel {
int* PS4_SYSV_ABI __Error();
s32 ErrnoToSceKernelError(s32 e);

struct SwVersionStruct {
    u64 struct_size;
    char text_representation[0x1c];
    u32 hex_representation;
};
s32 PS4_SYSV_ABI sceKernelGetSystemSwVersion(SwVersionStruct* ret);
} // namespace Libraries::Kernel

// bbport co-op: the stat structure from shadPS4's file_system.h (network sockets fill it).
#pragma once
#include "common/types.h"
#include "core/libraries/kernel/time.h"

namespace Libraries::Kernel {

struct OrbisKernelStat {
    u32 st_dev;
    u32 st_ino;
    u16 st_mode;
    u16 st_nlink;
    u32 st_uid;
    u32 st_gid;
    u32 st_rdev;
    OrbisKernelTimespec st_atim;
    OrbisKernelTimespec st_mtim;
    OrbisKernelTimespec st_ctim;
    s64 st_size;
    s64 st_blocks;
    u32 st_blksize;
    u32 st_flags;
    u32 st_gen;
    s32 st_lspare;
    OrbisKernelTimespec st_birthtim;
    unsigned int : (8 / 2) * (16 - static_cast<int>(sizeof(OrbisKernelTimespec)));
    unsigned int : (8 / 2) * (16 - static_cast<int>(sizeof(OrbisKernelTimespec)));
};

} // namespace Libraries::Kernel

// SPDX-License-Identifier: GPL-2.0-or-later
// bbport (Windows): small POSIX helpers the renderer uses for diagnostics and thread
// identity. Force-included into the GPU library on Windows only (CMakeLists.txt); kept free
// of <windows.h> so its macros do not reach every translation unit.
#pragma once
#ifdef _WIN32
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
__declspec(dllimport) unsigned long __stdcall GetCurrentThreadId(void);
__declspec(dllimport) void* __stdcall GetCurrentProcess(void);
__declspec(dllimport) int __stdcall ReadProcessMemory(void*, const void*, void*, size_t, size_t*);
static inline int gettid(void) { return (int)GetCurrentThreadId(); }
// The command-buffer diagnostics read one range of this process without faulting.
struct iovec { void* iov_base; size_t iov_len; };
static inline intptr_t process_vm_readv(int pid, const struct iovec* local, unsigned long nl,
                                       const struct iovec* remote, unsigned long nr,
                                       unsigned long flags) {
    (void)pid;
    if (nl != 1 || nr != 1 || flags || local->iov_len != remote->iov_len) return -1;
    size_t copied = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), remote->iov_base, local->iov_base,
                           local->iov_len, &copied)) return -1;
    return (intptr_t)copied;
}
#ifdef __cplusplus
}
#endif
#endif

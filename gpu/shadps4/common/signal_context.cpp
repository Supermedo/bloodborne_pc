// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/arch.h"
#include "common/signal_context.h"

#ifdef _WIN32
#include <windows.h>
#elif defined(__FreeBSD__)
#include <machine/npx.h>
#include <sys/ucontext.h>
#else
#include <sys/ucontext.h>
#endif

namespace Common {

void* GetRip(void* ctx) {
#if defined(_WIN32)
    return (void*)((EXCEPTION_POINTERS*)ctx)->ContextRecord->Rip;
#elif defined(__APPLE__) && defined(ARCH_X86_64)
    return (void*)((ucontext_t*)ctx)->uc_mcontext->__ss.__rip;
#elif defined(__APPLE__) && defined(ARCH_ARM64)
    return (void*)((ucontext_t*)ctx)->uc_mcontext->__ss.__pc;
#elif defined(__FreeBSD__)
    return (void*)((ucontext_t*)ctx)->uc_mcontext.mc_rip;
#elif defined(ARCH_X86_64)
    return (void*)((ucontext_t*)ctx)->uc_mcontext.gregs[REG_RIP];
#else
#error "Unsupported architecture"
#endif
}

void SetRip(void* ctx, void* rip) {
#if defined(_WIN32)
    ((EXCEPTION_POINTERS*)ctx)->ContextRecord->Rip = (DWORD64)rip;
#elif defined(__APPLE__) && defined(ARCH_X86_64)
    ((ucontext_t*)ctx)->uc_mcontext->__ss.__rip = (u64)rip;
#elif defined(__APPLE__) && defined(ARCH_ARM64)
    ((ucontext_t*)ctx)->uc_mcontext->__ss.__pc = (u64)rip;
#elif defined(__FreeBSD__)
    ((ucontext_t*)ctx)->uc_mcontext.mc_rip = (long)rip;
#elif defined(ARCH_X86_64)
    ((ucontext_t*)ctx)->uc_mcontext.gregs[REG_RIP] = (greg_t)rip;
#else
#error "Unsupported architecture"
#endif
}

void IncrementRip(void* ctx, u64 length) {
#if defined(_WIN32)
    ((EXCEPTION_POINTERS*)ctx)->ContextRecord->Rip += length;
#elif defined(__APPLE__) && defined(ARCH_X86_64)
    ((ucontext_t*)ctx)->uc_mcontext->__ss.__rip += length;
#elif defined(__APPLE__) && defined(ARCH_ARM64)
    ((ucontext_t*)ctx)->uc_mcontext->__ss.__pc += length;
#elif defined(__FreeBSD__)
    ((ucontext_t*)ctx)->uc_mcontext.mc_rip += length;
#elif defined(ARCH_X86_64)
    ((ucontext_t*)ctx)->uc_mcontext.gregs[REG_RIP] += length;
#else
#error "Unsupported architecture"
#endif
}

void ZeroHostGpr(void* ctx, u32 which) {
    if (which > 15) {
        return;
    }
#if defined(_WIN32)
    DWORD64* regs[16] = {
        &((EXCEPTION_POINTERS*)ctx)->ContextRecord->Rax,
        &((EXCEPTION_POINTERS*)ctx)->ContextRecord->Rcx,
        &((EXCEPTION_POINTERS*)ctx)->ContextRecord->Rdx,
        &((EXCEPTION_POINTERS*)ctx)->ContextRecord->Rbx,
        &((EXCEPTION_POINTERS*)ctx)->ContextRecord->Rsp,
        &((EXCEPTION_POINTERS*)ctx)->ContextRecord->Rbp,
        &((EXCEPTION_POINTERS*)ctx)->ContextRecord->Rsi,
        &((EXCEPTION_POINTERS*)ctx)->ContextRecord->Rdi,
        &((EXCEPTION_POINTERS*)ctx)->ContextRecord->R8,
        &((EXCEPTION_POINTERS*)ctx)->ContextRecord->R9,
        &((EXCEPTION_POINTERS*)ctx)->ContextRecord->R10,
        &((EXCEPTION_POINTERS*)ctx)->ContextRecord->R11,
        &((EXCEPTION_POINTERS*)ctx)->ContextRecord->R12,
        &((EXCEPTION_POINTERS*)ctx)->ContextRecord->R13,
        &((EXCEPTION_POINTERS*)ctx)->ContextRecord->R14,
        &((EXCEPTION_POINTERS*)ctx)->ContextRecord->R15,
    };
    if (which != 4) {
        *regs[which] = 0;
    }
#elif defined(__APPLE__) && defined(ARCH_X86_64)
    auto& ss = ((ucontext_t*)ctx)->uc_mcontext->__ss;
    u64* regs[16] = {&ss.__rax, &ss.__rcx, &ss.__rdx, &ss.__rbx, &ss.__rsp, &ss.__rbp,
                     &ss.__rsi, &ss.__rdi, &ss.__r8,  &ss.__r9,  &ss.__r10, &ss.__r11,
                     &ss.__r12, &ss.__r13, &ss.__r14, &ss.__r15};
    if (which != 4) {
        *regs[which] = 0;
    }
#elif defined(__APPLE__) && defined(ARCH_ARM64)
    (void)ctx;
    (void)which;
#elif defined(__FreeBSD__)
    auto& mc = ((ucontext_t*)ctx)->uc_mcontext;
    long* regs[16] = {&mc.mc_rax, &mc.mc_rcx, &mc.mc_rdx, &mc.mc_rbx, &mc.mc_rsp, &mc.mc_rbp,
                      &mc.mc_rsi, &mc.mc_rdi, &mc.mc_r8,  &mc.mc_r9,  &mc.mc_r10, &mc.mc_r11,
                      &mc.mc_r12, &mc.mc_r13, &mc.mc_r14, &mc.mc_r15};
    if (which != 4) {
        *regs[which] = 0;
    }
#elif defined(ARCH_X86_64)
    auto* g = ((ucontext_t*)ctx)->uc_mcontext.gregs;
    const int ids[16] = {REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP, REG_RSI, REG_RDI,
                         REG_R8,  REG_R9,  REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15};
    if (which != 4) {
        g[ids[which]] = 0;
    }
#else
#error "Unsupported architecture"
#endif
}

bool IsWriteError(void* ctx) {
#if defined(_WIN32)
    return ((EXCEPTION_POINTERS*)ctx)->ExceptionRecord->ExceptionInformation[0] == 1;
#elif defined(__APPLE__) && defined(ARCH_X86_64)
    return ((ucontext_t*)ctx)->uc_mcontext->__es.__err & 0x2;
#elif defined(__APPLE__) && defined(ARCH_ARM64)
    return ((ucontext_t*)ctx)->uc_mcontext->__es.__esr & 0x40;
#elif defined(__FreeBSD__) && defined(ARCH_X86_64)
    return ((ucontext_t*)ctx)->uc_mcontext.mc_err & 0x2;
#elif defined(ARCH_X86_64)
    return ((ucontext_t*)ctx)->uc_mcontext.gregs[REG_ERR] & 0x2;
#else
#error "Unsupported architecture"
#endif
}

} // namespace Common

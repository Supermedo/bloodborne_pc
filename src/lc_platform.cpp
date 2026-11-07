// SPDX-License-Identifier: GPL-2.0-or-later
#include "lc_platform.h"

#include <cstring>

#ifdef _WIN32
#include <windows.h>
#include <cwchar>
#include <cstdlib>

namespace lc_platform {
bool read(void* dst, std::uintptr_t src, std::size_t size) {
    SIZE_T n = 0;
    return src >= 65536 && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(src), dst, size, &n) &&
           n == size;
}
unsigned char* alloc_code(std::size_t size) {
    return static_cast<unsigned char*>(VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
}
bool seal_code(unsigned char* code, std::size_t size) {
    DWORD old = 0;
    if (!VirtualProtect(code, size, PAGE_EXECUTE_READ, &old)) return false;
    FlushInstructionCache(GetCurrentProcess(), code, size);
    return true;
}
void flush_code(void* code, std::size_t size) { FlushInstructionCache(GetCurrentProcess(), code, size); }
bool write_code(void* dst, const void* src, std::size_t size) {
    DWORD old = 0, tmp = 0;
    if (!VirtualProtect(dst, size, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(dst, src, size);
    VirtualProtect(dst, size, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), dst, size);
    return true;
}
std::FILE* open_catalog() {
    wchar_t path[MAX_PATH * 2];
    if (const wchar_t* env = _wgetenv(L"BB_LOCAL_CHALICE_CATALOG")) return _wfopen(env, L"rb");
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&open_catalog), &self))
        return nullptr;
    const DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return nullptr;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return nullptr;
    wcscpy(slash + 1, L"LocalChalice\\catalog.bin");
    return _wfopen(path, L"rb");
}
bool claim_process() {
    static int claimed = -1;
    if (claimed >= 0) return claimed != 0;
    wchar_t name[64];
    swprintf(name, 64, L"Local\\LocalChalice.%lu", GetCurrentProcessId());
    HANDLE m = CreateMutexW(nullptr, TRUE, name);
    claimed = m && GetLastError() != ERROR_ALREADY_EXISTS;
    if (m && !claimed) CloseHandle(m);  // the one that claimed it keeps it open
    return claimed != 0;
}
} // namespace lc_platform

#else
#include <dlfcn.h>
#include <cstdlib>
#include <string>
#include <sys/mman.h>
#include <sys/uio.h>
#include <unistd.h>

namespace lc_platform {
bool read(void* dst, std::uintptr_t src, std::size_t size) {
    if (src < 65536) return false;
    // process_vm_readv on ourselves fails cleanly (EFAULT) for unmapped memory instead of faulting.
    iovec local{dst, size}, remote{reinterpret_cast<void*>(src), size};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(size);
}
unsigned char* alloc_code(std::size_t size) {
    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? nullptr : static_cast<unsigned char*>(p);
}
bool seal_code(unsigned char* code, std::size_t size) {
    if (mprotect(code, size, PROT_READ | PROT_EXEC)) return false;
    __builtin___clear_cache(reinterpret_cast<char*>(code), reinterpret_cast<char*>(code) + size);
    return true;
}
void flush_code(void* code, std::size_t size) {
    __builtin___clear_cache(static_cast<char*>(code), static_cast<char*>(code) + size);
}
bool write_code(void* dst, const void* src, std::size_t size) {
    const std::uintptr_t page = static_cast<std::uintptr_t>(sysconf(_SC_PAGESIZE));
    const std::uintptr_t start = reinterpret_cast<std::uintptr_t>(dst) & ~(page - 1);
    const std::uintptr_t end = (reinterpret_cast<std::uintptr_t>(dst) + size + page - 1) & ~(page - 1);
    void* first = reinterpret_cast<void*>(start);
    if (mprotect(first, end - start, PROT_READ | PROT_WRITE | PROT_EXEC)) return false;
    std::memcpy(dst, src, size);
    // Game code is read-execute once loaded (the port sets its final protection after this when
    // called from its loader).
    mprotect(first, end - start, PROT_READ | PROT_EXEC);
    flush_code(dst, size);
    return true;
}
std::FILE* open_catalog() {
    if (const char* env = std::getenv("BB_LOCAL_CHALICE_CATALOG")) return std::fopen(env, "rb");
    Dl_info info{};
    if (!dladdr(reinterpret_cast<void*>(&open_catalog), &info) || !info.dli_fname) return nullptr;
    std::string path(info.dli_fname);
    const auto slash = path.rfind('/');
    path = (slash == std::string::npos ? std::string(".") : path.substr(0, slash)) + "/LocalChalice/catalog.bin";
    return std::fopen(path.c_str(), "rb");
}
bool claim_process() {
    // A process-wide mark every copy of this code sees: an environment variable naming this
    // process (a child process inherits it with another pid, so it does not count there).
    static int claimed = -1;
    if (claimed >= 0) return claimed != 0;
    const std::string me = std::to_string(getpid());
    const char* owner = std::getenv("LC_LOCALCHALICE_PROCESS");
    claimed = !(owner && me == owner);
    if (claimed) setenv("LC_LOCALCHALICE_PROCESS", me.c_str(), 1);
    return claimed != 0;
}
} // namespace lc_platform
#endif

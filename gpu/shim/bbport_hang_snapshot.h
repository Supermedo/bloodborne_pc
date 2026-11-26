// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// bbport: where every thread of the process is. Each thread is suspended in turn for its
// instruction pointer and its stack (up to 32 KiB from rsp), then resumed; nothing is allocated or
// locked while a thread is suspended, since it may hold the lock. Return addresses are the stack
// values that point just after a call instruction into the game's image or a loaded module.
// Windows only.

#ifdef _WIN32
#ifndef PSAPI_VERSION
#define PSAPI_VERSION 2 // EnumProcessModules is K32EnumProcessModules in kernel32
#endif
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace BbHangSnapshot {

struct Module {
    std::uintptr_t base, end;
    std::string name;
};

/// The loaded modules, by address.
inline std::vector<Module> Modules() {
    std::vector<Module> out;
    std::vector<HMODULE> handles(1024);
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), handles.data(),
                            DWORD(handles.size() * sizeof(HMODULE)), &needed)) {
        return out;
    }
    handles.resize(std::min<std::size_t>(handles.size(), needed / sizeof(HMODULE)));
    for (HMODULE module : handles) {
        MODULEINFO info{};
        char name[MAX_PATH] = {};
        if (!GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info))) {
            continue;
        }
        GetModuleBaseNameA(GetCurrentProcess(), module, name, sizeof(name));
        const auto base = reinterpret_cast<std::uintptr_t>(info.lpBaseOfDll);
        out.push_back({base, base + info.SizeOfImage, name});
    }
    std::sort(out.begin(), out.end(),
              [](const Module& a, const Module& b) { return a.base < b.base; });
    return out;
}

/// The module holding `address`, or nullptr.
inline const Module* Find(const std::vector<Module>& modules, std::uintptr_t address) {
    auto it = std::upper_bound(modules.begin(), modules.end(), address,
                               [](std::uintptr_t a, const Module& m) { return a < m.base; });
    if (it == modules.begin()) {
        return nullptr;
    }
    --it;
    return address < it->end ? &*it : nullptr;
}

/// The bytes before `address` end with a near call, so `address` is a return address.
inline bool AfterCall(std::uintptr_t address) {
    unsigned char b[8];
    SIZE_T got = 0;
    if (address < sizeof(b) ||
        !ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address - sizeof(b)),
                           b, sizeof(b), &got) ||
        got != sizeof(b)) {
        return false;
    }
    // b[7] is the byte just before `address`. ModRM r/m 4 takes a SIB byte; with mod 0, r/m 5 is
    // rip-relative.
    const auto reg = [](unsigned char modrm, unsigned char mod) {
        return (modrm & 0xf8) == mod && (modrm & 7) != 4;
    };
    return b[3] == 0xe8                                         // call rel32
           || (b[2] == 0xff && b[3] == 0x15)                    // call [rip+disp32]
           || (b[6] == 0xff && (b[7] & 0xf8) == 0xd0)           // call reg
           || (b[6] == 0xff && reg(b[7], 0x10) && (b[7] & 7) != 5) // call [reg]
           || (b[5] == 0xff && b[6] == 0x14)                    // call [sib]
           || (b[5] == 0xff && reg(b[6], 0x50))                 // call [reg+disp8]
           || (b[4] == 0xff && b[5] == 0x54)                    // call [sib+disp8]
           || (b[2] == 0xff && reg(b[3], 0x90))                 // call [reg+disp32]
           || (b[1] == 0xff && b[2] == 0x94);                   // call [sib+disp32]
}

class Taker {
public:
    /// Addresses in `image`..`image + image_size` (the game) are written as eboot+offset.
    Taker(std::uintptr_t image, std::uint64_t image_size)
        : image_{image}, image_size_{image_size}, stack_(StackWords) {}

    /// For every other thread of the process, a line with its id, name, instruction pointer and
    /// stack pointer, then one line per return address on its stack, innermost first.
    std::string Take() {
        const auto modules = Modules();
        std::string text;
        HANDLE threads = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (threads == INVALID_HANDLE_VALUE) {
            return "no thread list\n";
        }
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        const DWORD process = GetCurrentProcessId(), self = GetCurrentThreadId();
        for (BOOL ok = Thread32First(threads, &entry); ok; ok = Thread32Next(threads, &entry)) {
            if (entry.th32OwnerProcessID == process && entry.th32ThreadID != self) {
                Thread(entry.th32ThreadID, modules, text);
            }
        }
        CloseHandle(threads);
        return text;
    }

private:
    static constexpr std::size_t StackWords = 32 * 1024 / 8;
    static constexpr std::size_t MaxReturns = 48;

    std::string Where(std::uintptr_t address, const std::vector<Module>& modules) const {
        char text[160];
        if (image_ && address - image_ < image_size_) {
            std::snprintf(text, sizeof(text), "eboot+0x%llx",
                          static_cast<unsigned long long>(address - image_));
        } else if (const Module* module = Find(modules, address)) {
            std::snprintf(text, sizeof(text), "%s+0x%llx", module->name.c_str(),
                          static_cast<unsigned long long>(address - module->base));
        } else {
            std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(address));
        }
        return text;
    }

    bool Known(std::uintptr_t address, const std::vector<Module>& modules) const {
        return (image_ && address - image_ < image_size_) || Find(modules, address);
    }

    void Thread(DWORD id, const std::vector<Module>& modules, std::string& text) {
        HANDLE thread = OpenThread(
            THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, id);
        if (!thread) {
            return;
        }
        CONTEXT context{};
        context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        std::size_t words = 0;
        bool have = false;
        if (SuspendThread(thread) != DWORD(-1)) {
            have = GetThreadContext(thread, &context) != 0;
            if (have) {
                words = ReadStack(context.Rsp);
            }
            ResumeThread(thread);
        }
        char line[400];
        std::snprintf(line, sizeof(line), "thread %lu '%s'", static_cast<unsigned long>(id),
                      Name(thread).c_str());
        text += line;
        if (!have) {
            text += ": no context\n";
            CloseHandle(thread);
            return;
        }
        std::snprintf(line, sizeof(line), " rip %s rsp 0x%llx\n",
                      Where(context.Rip, modules).c_str(),
                      static_cast<unsigned long long>(context.Rsp));
        text += line;
        std::size_t found = 0;
        for (std::size_t i = 0; i < words && found < MaxReturns; ++i) {
            const auto value = static_cast<std::uintptr_t>(stack_[i]);
            if (Known(value, modules) && AfterCall(value)) {
                std::snprintf(line, sizeof(line), "  %s (rsp+0x%llx)\n",
                              Where(value, modules).c_str(), static_cast<unsigned long long>(i * 8));
                text += line;
                ++found;
            }
        }
        CloseHandle(thread);
    }

    /// The stack from `rsp` into stack_, a page at a time up to the first page that cannot be
    /// read; the number of words read.
    std::size_t ReadStack(std::uint64_t rsp) {
        auto* out = reinterpret_cast<unsigned char*>(stack_.data());
        const std::size_t limit = stack_.size() * 8;
        std::size_t bytes = 0;
        while (bytes < limit) {
            const std::uint64_t at = rsp + bytes;
            const std::size_t chunk = std::min<std::size_t>(limit - bytes, 4096 - (at & 4095));
            SIZE_T got = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(at),
                                   out + bytes, chunk, &got) ||
                got != chunk) {
                break;
            }
            bytes += chunk;
        }
        return bytes / 8;
    }

    /// The thread's description (SetThreadDescription), UTF-8.
    static std::string Name(HANDLE thread) {
        using Describe = HRESULT(WINAPI*)(HANDLE, PWSTR*);
        static const auto describe = reinterpret_cast<Describe>(reinterpret_cast<void*>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription")));
        PWSTR wide = nullptr;
        if (!describe || FAILED(describe(thread, &wide)) || !wide) {
            return "";
        }
        char name[256] = {};
        WideCharToMultiByte(CP_UTF8, 0, wide, -1, name, sizeof(name) - 1, nullptr, nullptr);
        LocalFree(wide);
        return name;
    }

    std::uintptr_t image_;
    std::uint64_t image_size_;
    std::vector<std::uint64_t> stack_;
};

} // namespace BbHangSnapshot
#endif

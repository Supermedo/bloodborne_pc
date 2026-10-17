// SPDX-License-Identifier: GPL-2.0-or-later
// An unknown bridge exposes only its ABI word; every later entry is inaccessible.
#include <cstdint>
#include <windows.h>

extern "C" __declspec(dllexport) const void* BbDlssGetApi() {
    static const void* table = []() -> const void* {
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        const auto page = info.dwPageSize;
        auto* memory = static_cast<unsigned char*>(VirtualAlloc(
            nullptr, page * 2, MEM_RESERVE, PAGE_NOACCESS));
        if (!memory || !VirtualAlloc(memory, page, MEM_COMMIT, PAGE_READWRITE)) return nullptr;
        auto* abi = reinterpret_cast<std::uint32_t*>(memory + page - sizeof(std::uint32_t));
        *abi = 0xffffffffu;
        return abi;
    }();
    return table;
}

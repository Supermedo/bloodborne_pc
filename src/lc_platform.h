// SPDX-License-Identifier: GPL-2.0-or-later
// LocalChalice offline: the few host services the game-level code needs. Everything else in
// local_chalice.cpp talks only to the 1.09 game code, so it is the same on every port that runs
// that code natively (Windows and Linux builds alike).
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace lc_platform {
// Copies size bytes from address src; false (and nothing copied) if any byte is unreadable.
bool read(void* dst, std::uintptr_t src, std::size_t size);
// Executable memory for trampolines: allocate read-write, then seal read-execute.
unsigned char* alloc_code(std::size_t size);
bool seal_code(unsigned char* code, std::size_t size);
// After patching code bytes.
void flush_code(void* code, std::size_t size);
// Writes into the game's code whatever its current protection (made writable for the copy,
// then restored) and flushes the instruction cache.
bool write_code(void* dst, const void* src, std::size_t size);
// The glyph catalog: BB_LOCAL_CHALICE_CATALOG if set, else LocalChalice/catalog.bin next to the
// module this code is in (the mod's DLL, or the port's program when built into it).
std::FILE* open_catalog();
// True for the first LocalChalice loader of this process to ask (pdh.dll, winmm.dll, a bbhost
// plugin, the Linux preload library...): only that one installs the mod.
bool claim_process();
} // namespace lc_platform

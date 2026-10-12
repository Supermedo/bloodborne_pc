// SPDX-License-Identifier: GPL-2.0-or-later
// Opt-in evidence collection. No file operations or allocation in the camera callback.
#pragma once
#ifdef _WIN32
#include <windows.h>
#include <Zydis/Zydis.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cstring>
#include <filesystem>
#include <set>

namespace BbMouse::Trace {
inline std::atomic<std::uint8_t> enabled{0};
inline constexpr std::array<std::uint32_t, 64> Fields{
    0x40,0x44,0x48, 0xc0,0xc4,0xc8, 0xd0,0xd4,0xd8, 0x130,
    0x140,0x144,0x148,0x14c,0x150,0x154,
    0x198,0x19c,0x1a0,0x1a4,0x1a8,0x1ac,0x1b0,0x1b4,0x1b8,0x1bc,
    0x1c0,0x1c4,0x1c8,0x1cc,0x1d0,0x1d4,0x1d8,0x1dc,0x1e0,0x1e4,
    0x1e8,0x1ec,0x1f0,0x1f4,0x1f8,0x1fc,0x200,0x204,0x208,0x20c,
    0x210,0x214,0x218,0x21c,0x220,0x224,0x228,0x22c,0x230,0x234,0x238,
    0x23c,0x240,0x244,0x248,0x26c,0x270,0x294};
inline constexpr std::array<std::uint32_t, 9> AfterFields{
    0x140,0x144,0x148,0x14c,0x150,0x26c,0x270,0x130,0x294};
// Final view basis, character/model basis and follow positions. The first
// report only described stationary orbit response; movement needs these too.
inline constexpr std::array<std::uint32_t, 51> ExtraFields{
    0x10,0x14,0x18, 0x20,0x24,0x28, 0x30,0x34,0x38,
    0x60,0x64,0x68, 0x70,0x74,0x78, 0x80,0x84,0x88,
    0x90,0x94,0x98, 0xa0,0xa4,0xa8, 0xb0,0xb4,0xb8,
    0xe0,0xe4,0xe8, 0xf0,0xf4,0xf8, 0x100,0x104,0x108,
    0x110,0x114,0x118,0x124,0x134,0x138,0x158,0x17c,0x180,
    0x184,0x188,0x278,0x27c,0x264,0x268};
inline constexpr std::array<std::uint32_t, 8> FlagFields{
    0x13c,0x15c,0x260,0x263,0x274,0x275,0x276,0x28c};
inline constexpr unsigned Capacity = 16384, HeaderSize = 4096;
struct Record {
    std::uint64_t sequence, ticks, camera;
    std::int32_t dx, dy; // raw counts multiplied by 256, not cursor pixels
    float sensitivity;
    std::uint32_t flags; // capture=1, mouse owner=2, stick moving=4, invert Y=8, pad valid=16
    float before[Fields.size()], after[AfterFields.size()];
    std::uint32_t reserved; // retain the complete 336-byte V1 prefix
    std::uint64_t pad; // buttons (low 32), left X/Y, right X/Y (high 32)
    float extra[ExtraFields.size()];
    std::uint32_t native_flags;
};
static_assert(sizeof(Record) == 552 && offsetof(Record, before) == 40 && offsetof(Record, pad) == 336);
inline unsigned char* mapping = nullptr;
inline std::atomic<unsigned> next{0};
inline std::atomic<bool> started{false};
inline std::atomic<std::uint64_t> pad_snapshot{0};
inline std::atomic<bool> pad_available{false};
static_assert(decltype(pad_snapshot)::is_always_lock_free);

inline void Pad(std::uint8_t lx, std::uint8_t ly, std::uint8_t rx, std::uint8_t ry,
                std::uint32_t buttons) {
    if (!enabled.load(std::memory_order_relaxed)) return;
    const auto axes = std::uint32_t(lx) | std::uint32_t(ly) << 8 |
                      std::uint32_t(rx) << 16 | std::uint32_t(ry) << 24;
    pad_snapshot.store(std::uint64_t(buttons) | std::uint64_t(axes) << 32, std::memory_order_relaxed);
    pad_available.store(true, std::memory_order_release);
}

// Dump only the verified routine, bounded direct callees and image constants.
// Never follow live pointers or include game resources, player names or saves.
inline bool Code(FILE* file, const unsigned char* image, std::uint64_t size,
                 std::size_t begin, std::size_t end) {
    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    ZydisFormatter formatter;
    ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL);
    std::set<std::uint64_t> calls, constants;
    const auto dump = [&](std::uint64_t address, std::uint64_t length, bool references) {
        std::fprintf(file, "\nimage+0x%llx, %llu bytes\n",
                     (unsigned long long)address, (unsigned long long)length);
        for (auto at = address; at < address + length;) {
            ZydisDecodedInstruction inst{};
            ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT]{};
            if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, image + at,
                    address + length - at, &inst, operands))) {
                std::fprintf(file, "%08llx: db %02x\n", (unsigned long long)at, image[at]);
                ++at;
                continue;
            }
            char text[256]{};
            ZydisFormatterFormatInstruction(&formatter, &inst, operands,
                inst.operand_count_visible, text, sizeof(text), at, ZYAN_NULL);
            std::fprintf(file, "%08llx: ", (unsigned long long)at);
            for (unsigned n = 0; n < inst.length; ++n) std::fprintf(file, "%02x", image[at + n]);
            std::fprintf(file, "  %s\n", text);
            for (unsigned n = 0; references && n < inst.operand_count_visible; ++n) {
                std::uint64_t target = 0;
                if (!ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&inst, &operands[n], at, &target)) ||
                    target >= size) continue;
                if (inst.meta.category == ZYDIS_CATEGORY_CALL &&
                    operands[n].type == ZYDIS_OPERAND_TYPE_IMMEDIATE && calls.size() < 64)
                    calls.insert(target);
                if (operands[n].type == ZYDIS_OPERAND_TYPE_MEMORY &&
                    operands[n].mem.base == ZYDIS_REGISTER_RIP && constants.size() < 256)
                    constants.insert(target);
            }
            at += inst.length;
        }
    };
    std::fputs("BBPORT_MOUSE_CODE_V1; original image bytes before native hooks\n", file);
    std::fprintf(file, "Function %zx..%zx; at most 64 direct callees (512 bytes each)\n"
                       "and 256 RIP-relative constants (32 bytes each). Callees may be truncated.\n", begin, end);
    dump(begin, end - begin, true);
    for (auto address : calls)
        if (address < begin || address >= end) dump(address, std::min<std::uint64_t>(512, size - address), false);
    std::fputs("\nRIP-relative image data\n", file);
    for (auto address : constants) {
        std::fprintf(file, "%08llx: ", (unsigned long long)address);
        for (unsigned n = 0; n < std::min<std::uint64_t>(32, size - address); ++n)
            std::fprintf(file, "%02x", image[address + n]);
        std::fputc('\n', file);
    }
    return !std::ferror(file);
}

inline void Start(const unsigned char* image, std::uint64_t size,
                  std::size_t begin, std::size_t end) {
    const char* value = std::getenv("BB_MOUSE_TRACE");
    if (!value || std::strcmp(value, "1") || mapping) return;
    try {
        SYSTEMTIME time{};
        GetSystemTime(&time);
        wchar_t name[128];
        std::swprintf(name, std::size(name), L"mouse-camera-%04u%02u%02u-%02u%02u%02u-%lu",
            time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond,GetCurrentProcessId());
        const auto directory = std::filesystem::path(L"logs") / name;
        if (!std::filesystem::create_directories(directory)) return;
        FILE* code = _wfopen((directory / L"code.txt").c_str(), L"wb");
        if (!code) return;
        const bool ok = Code(code, image, size, begin, end);
        const bool closed = !std::fclose(code);
        if (!ok || !closed) return;
        HANDLE file = CreateFileW((directory / L"state.bin").c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        const auto bytes = HeaderSize + sizeof(Record) * Capacity;
        HANDLE section = CreateFileMappingW(file, nullptr, PAGE_READWRITE, 0, DWORD(bytes), nullptr);
        if (section) mapping = static_cast<unsigned char*>(MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, bytes));
        if (section) CloseHandle(section);
        CloseHandle(file);
        if (!mapping) return;
        // Prefault the bounded file at startup; the callback only copies to RAM.
        std::memset(mapping, 0, bytes);
        std::memcpy(mapping, "BBMOUSE2", 8);
        const std::uint32_t header[]{2, HeaderSize, sizeof(Record), Capacity};
        std::memcpy(mapping + 8, header, sizeof(header));
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        std::memcpy(mapping + 24, &frequency.QuadPart, 8);
        const std::uint32_t counts[]{Fields.size(), AfterFields.size()};
        std::memcpy(mapping + 32, counts, sizeof(counts));
        const std::uint32_t extension[]{ExtraFields.size(), FlagFields.size()};
        std::memcpy(mapping + 40, extension, sizeof(extension));
        std::memcpy(mapping + 64, Fields.data(), sizeof(Fields));
        std::memcpy(mapping + 64 + sizeof(Fields), AfterFields.data(), sizeof(AfterFields));
        std::memcpy(mapping + 64 + sizeof(Fields) + sizeof(AfterFields), ExtraFields.data(), sizeof(ExtraFields));
        std::memcpy(mapping + 64 + sizeof(Fields) + sizeof(AfterFields) + sizeof(ExtraFields),
                    FlagFields.data(), sizeof(FlagFields));
        enabled.store(1, std::memory_order_release);
        std::printf("Mouse trace: logs/%ls; recording %u updates after first mouse motion; camera behavior unchanged\n",
                    name, Capacity);
    } catch (const std::exception&) {
        std::fputs("Mouse trace: cannot create diagnostic files; camera behavior unchanged\n", stderr);
    }
}

struct Sample {
    Record record;
    const unsigned char* camera = nullptr;
    unsigned index = Capacity;
    Sample(const void* pointer, std::uint64_t motion, float sensitivity, unsigned flags) {
        if (!enabled.load(std::memory_order_relaxed) || !pointer) return;
        if (!started.load(std::memory_order_relaxed)) {
            if (!motion || (flags & 3) != 3) return;
            started.store(true, std::memory_order_relaxed);
        }
        index = next.fetch_add(1, std::memory_order_relaxed);
        if (index >= Capacity) {
            enabled.store(0, std::memory_order_release);
            return;
        }
        std::memset(&record, 0, sizeof(record));
        camera = static_cast<const unsigned char*>(pointer);
        LARGE_INTEGER time{};
        QueryPerformanceCounter(&time);
        record.ticks = time.QuadPart;
        record.camera = reinterpret_cast<std::uintptr_t>(pointer);
        record.dx = std::int32_t(motion); record.dy = std::int32_t(motion >> 32);
        record.sensitivity = sensitivity; record.flags = flags;
        if (pad_available.load(std::memory_order_acquire)) {
            record.pad = pad_snapshot.load(std::memory_order_relaxed);
            record.flags |= 16;
        }
        for (unsigned n = 0; n < Fields.size(); ++n)
            std::memcpy(record.before + n, camera + Fields[n], 4);
        for (unsigned n = 0; n < ExtraFields.size(); ++n)
            std::memcpy(record.extra + n, camera + ExtraFields[n], 4);
        for (unsigned n = 0; n < FlagFields.size(); ++n)
            record.native_flags |= unsigned(camera[FlagFields[n]] != 0) << n;
    }
    ~Sample() {
        if (!camera) return;
        for (unsigned n = 0; n < AfterFields.size(); ++n)
            std::memcpy(record.after + n, camera + AfterFields[n], 4);
        auto* destination = mapping + HeaderSize + sizeof(Record) * index;
        std::memcpy(destination + 8, reinterpret_cast<const unsigned char*>(&record) + 8, sizeof(Record) - 8);
        // Publish only complete records, including on early returns/lock-on.
        InterlockedExchange64(reinterpret_cast<volatile LONG64*>(destination), index + 1);
    }
};
} // namespace BbMouse::Trace
#endif

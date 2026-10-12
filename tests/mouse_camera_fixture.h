// SPDX-License-Identifier: GPL-2.0-or-later
// Synthetic instruction fixtures, not executable bytes extracted from a game.
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace CameraFixture {
constexpr std::size_t ImageSize = 0x5540000;
constexpr std::size_t Main = 0x143ceaa, Begin = 0x143ac60, End = 0x143fad0;
constexpr std::size_t Orbit = 0x143e7e6;
// Independently assembled orbit-blend instructions, matching the validator's
// semantic anchor. Inputs/register state are supplied by the orbit replay test.
constexpr unsigned char OrbitCode[]{
    0xc4,0xc2,0x79,0x18,0x8d,0x30,1,0,0,
    0xc5,0xf8,0x59,0xc1,0xc5,0xd8,0x58,0xc0,
    0xc5,0xf8,0x28,0xa5,0xb0,0xfd,0xff,0xff,
    0xc5,0x90,0x5c,0xcc,0xc5,0xf0,0x59,0xc0,0xc5,0xd8,0x58,0xc0};
constexpr std::array<std::size_t,4> Anchors{0x143c6e8,0x143c984,0x143dde6,0x143c870};
constexpr std::array<std::uint32_t,5> Fields{0x130,0x140,0x144,0x150,0x294};
constexpr unsigned char Loads[]{0xc4,0xc1,0x7a,0x10,0x85,0x40,1,0,0,
                               0xc4,0xc1,0x7a,0x10,0x8d,0x50,1,0,0};
struct Store { std::size_t offset; std::uint32_t field; unsigned size = 9; float value = .875f; };

inline std::vector<Store> Populate(unsigned char* image,
        const std::array<std::uint32_t,4>& fields = {0x130,0x294,0x140,0x150}) {
    std::memset(image + Begin, 0x90, End - Begin);
    std::memcpy(image + Main, Loads, sizeof(Loads));
    image[Main + sizeof(Loads)] = 0xc3;
    std::memcpy(image + Orbit, OrbitCode, sizeof(OrbitCode));
    image[Orbit + sizeof(OrbitCode)] = 0xc3;
    // A genuine SysV-style function boundary. Yaw is read BEFORE the old
    // mid-routine pitch hook, exposing the asymmetric one-update delay.
    unsigned char entry[]{0x55,0x48,0x89,0xe5,0x41,0x55, // push rbp; mov rbp,rsp; push r13
        0x49,0x89,0xfd, // mov r13,rdi (this)
        0xc4,0xc1,0x7a,0x10,0x9d,0x44,1,0,0, // vmovss xmm3,[r13+144]
        0xe8,0,0,0,0, // call the later pitch-load path
        0x41,0x5d,0x5d,0xc3}; // pop r13; pop rbp; ret
    const auto call = std::int32_t(Main - (Begin + 23));
    std::memcpy(entry + 19, &call, 4);
    std::memcpy(image + Begin, entry, sizeof(entry));
    std::vector<Store> stores;
    unsigned signatures = 0;
    const auto add = [&](std::size_t offset, std::uint32_t field, bool high = false) {
        unsigned char code[]{0xc4,0xc1,0x7a,0x11,0x95,0,0,0,0}; // xmm2 -> [r13+field]
        if (high) { code[1] = 0x41; code[4] = 0x8d; } // xmm9
        std::memcpy(code + 5, &field, 4);
        std::memcpy(image + offset, code, sizeof(code));
        image[offset + sizeof(code)] = 0xc3;
        stores.push_back({offset, field});
        if (!high && std::find(Fields.begin(), Fields.end(), field) != Fields.end()) ++signatures;
    };
    for (unsigned i = 0; i < Anchors.size(); ++i) add(Anchors[i], fields[i]);
    for (unsigned i = 0; signatures < 29; ++i) add(Begin + 0x100 + i * 32, Fields[i % Fields.size()]);
    // The signature count uses the original low-register encoding, but semantic
    // discovery must also handle stores from higher XMM registers safely.
    add(Begin + 0x1000, 0x144, true);
    add(Begin + 0x1020, 0x26c);
    add(Begin + 0x1040, 0x270);
    add(Begin + 0x1060, 0x148);
    add(Begin + 0x1080, 0x14c);
    for (unsigned i = 0; i < 2; ++i) {
        const auto at = Begin + 0x10a0 + i * 32;
        const unsigned char extract[]{0xc4,0xc3,0x79,0x17,0x95,0x44,1,0,0,0}; // xmm2 lane 0 -> yaw
        std::memcpy(image + at, extract, sizeof(extract));
        image[at + sizeof(extract)] = 0xc3;
        stores.push_back({at,0x144,sizeof(extract)});
    }
    for (unsigned i = 0; i < 5; ++i) {
        const auto at = Begin + 0x1100 + i * 32;
        const std::uint32_t field = i == 0 ? 0x140 : i < 3 ? 0x26c : 0x294;
        unsigned char immediate[]{0x41,0xc7,0x85,0,0,0,0,0,0,0,0};
        std::memcpy(immediate + 3, &field, 4);
        std::memcpy(image + at, immediate, sizeof(immediate));
        image[at + sizeof(immediate)] = 0xc3;
        stores.push_back({at,field,sizeof(immediate),0.f});
    }
    return stores;
}
} // namespace CameraFixture

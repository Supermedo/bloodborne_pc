// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <filesystem>
#include <fstream>
#include <vector>
#include "bbport_fsr4vk_manifest.h"

namespace Fsr4VkPackage {
inline bool Verified(const std::filesystem::path& folder) {
    for (const auto& file : Files) {
        const auto path = folder / file.name;
        std::error_code error;
        if (std::filesystem::file_size(path, error) != file.size || error) return false;
        std::ifstream stream(path, std::ios::binary);
        if (!stream) return false;
        struct Hash {
            std::vector<unsigned char> object;
            BCRYPT_ALG_HANDLE algorithm{};
            BCRYPT_HASH_HANDLE handle{};
            ~Hash() {
                if (handle) BCryptDestroyHash(handle);
                if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
            }
        } hash;
        if (BCryptOpenAlgorithmProvider(&hash.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
            return false;
        DWORD size{}, written{};
        if (BCryptGetProperty(hash.algorithm, BCRYPT_OBJECT_LENGTH,
                reinterpret_cast<PUCHAR>(&size), sizeof(size), &written, 0) < 0) return false;
        hash.object.resize(size);
        if (BCryptCreateHash(hash.algorithm, &hash.handle, hash.object.data(), size, nullptr, 0, 0) < 0)
            return false;
        std::array<unsigned char, 65536> buffer{};
        while (stream.read(reinterpret_cast<char*>(buffer.data()), buffer.size()) || stream.gcount()) {
            if (BCryptHashData(hash.handle, buffer.data(), ULONG(stream.gcount()), 0) < 0) return false;
        }
        if (!stream.eof()) return false;
        std::array<unsigned char, 32> digest{};
        if (BCryptFinishHash(hash.handle, digest.data(), digest.size(), 0) < 0) return false;
        static constexpr char hex[] = "0123456789abcdef";
        for (size_t i = 0; i < digest.size(); ++i) {
            if (file.sha256[i * 2] != hex[digest[i] >> 4] ||
                file.sha256[i * 2 + 1] != hex[digest[i] & 15]) return false;
        }
    }
    return true;
}
}
#endif

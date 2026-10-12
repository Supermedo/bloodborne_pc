// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "remix_scene.h"
#include <array>
#include <cstring>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace BbRemix::Wire {
constexpr uint32_t Magic = 0x52584242, Version = 1;
constexpr uint32_t MaxMessage = 192u << 20, MaxMeshes = 32768, MaxVertices = 2u << 20;
enum class Operation : uint32_t { Initialize, Frame, Stop };
struct Header { uint32_t magic{Magic}, version{Version}, bytes{}; Operation operation{}; };
struct Initialize { uint32_t width{}, height{}; std::array<uint8_t,8> luid{}; };
struct Frame {
    uint64_t number{};
    uint32_t width{}, height{}, updates{}, instances{}, reset{};
    SceneCamera camera{};
};
struct Mesh {
    uint64_t id{}, revision{}, material{};
    uint32_t vertices{}, indices{}, alpha_cutout{};
    uint32_t albedo_chars{}, normal_chars{};
};
struct Reply {
    uint32_t magic{Magic}, version{Version}, success{}, ready{};
    uint64_t memory{}, generation{};
    uint32_t width{}, height{};
    std::array<uint8_t,8> luid{};
};
class Writer {
public:
    std::vector<uint8_t> data;
    template<class T> void Put(const T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        Bytes(&value, sizeof(value));
    }
    void Bytes(const void* source, size_t size) {
        if (size > MaxMessage || data.size() > MaxMessage-size) throw std::runtime_error("Remix scene exceeds IPC budget");
        if (!size) return;
        const auto* p=static_cast<const uint8_t*>(source); data.insert(data.end(),p,p+size);
    }
};
class Reader {
    std::span<const uint8_t> data;
    size_t offset{};
public:
    explicit Reader(std::span<const uint8_t> data) : data(data) {}
    template<class T> T Get() {
        static_assert(std::is_trivially_copyable_v<T>);
        T result{}; Bytes(&result,sizeof(result)); return result;
    }
    void Bytes(void* out, size_t size) {
        if (size > data.size()-offset) throw std::runtime_error("Truncated Remix scene message");
        if (size) std::memcpy(out,data.data()+offset,size); offset+=size;
    }
    std::wstring Path(uint32_t chars) {
        if (chars>32767) throw std::runtime_error("Invalid Remix texture path");
        std::wstring result(chars,L'\0'); Bytes(result.data(),size_t(chars)*sizeof(wchar_t));
        if (result.find(L'\0')!=std::wstring::npos) throw std::runtime_error("NUL in Remix texture path");
        return result;
    }
    bool End() const { return offset == data.size(); }
};
} // namespace BbRemix::Wire

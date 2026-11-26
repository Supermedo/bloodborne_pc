// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>

// Headless test only. Record the exact public vkCreateShaderModule input that
// the SDK submits through its supplied vkGetDeviceProcAddr, without editing it.
// Host pipelines use the dispatcher's existing vkCreateShaderModule directly.
namespace Fsr24ShaderCapture {
inline PFN_vkGetDeviceProcAddr downstream{};
inline std::filesystem::path directory;
inline std::atomic_uint32_t sequence{};
inline std::mutex log_mutex;

inline VKAPI_ATTR VkResult VKAPI_CALL CreateShaderModule(
    VkDevice device, const VkShaderModuleCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkShaderModule* result) {
    const auto id = sequence.fetch_add(1);
    const auto file = directory / ("shader-" + std::to_string(id) + ".spv");
    {
        std::ofstream stream(file, std::ios::binary);
        stream.write(reinterpret_cast<const char*>(info->pCode), info->codeSize);
    }
    HMODULE module{};
    wchar_t name[32768]{};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(__builtin_return_address(0)), &module)) {
        GetModuleFileNameW(module, name, DWORD(std::size(name)));
    }
    {
        std::scoped_lock lock{log_mutex};
        std::printf("SDK_SHADER_CAPTURE id=%u bytes=%zu caller=%ls\n", id, info->codeSize, name);
    }
    // Resolve and invoke the unmodified original implementation with the same pointers.
    const auto create = reinterpret_cast<PFN_vkCreateShaderModule>(downstream(device, "vkCreateShaderModule"));
    return create(device, info, allocator, result);
}

inline VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice device, const char* name) {
    return std::strcmp(name, "vkCreateShaderModule") == 0
               ? reinterpret_cast<PFN_vkVoidFunction>(&CreateShaderModule)
               : downstream(device, name);
}

inline void Enable(const char* path) {
    directory = std::filesystem::path(path);
    std::filesystem::create_directories(directory);
    downstream = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;
    if (!downstream) throw std::runtime_error("shader capture needs a device dispatcher");
    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr = &GetDeviceProcAddr;
}
} // namespace Fsr24ShaderCapture

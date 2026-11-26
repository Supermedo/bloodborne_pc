// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include "ffx_api_loader.h"
#include "ffx_upscale.h"

#ifndef FFX_API_EFFECT_COUNT
#error "sdk24_upscaling.h requires the SDK 2.4 headers in gpu/fsr24/sdk"
#endif
static_assert(FFX_API_BACKEND_ID_DX12 == 0x00000000u &&
              FFX_API_BACKEND_ID_VK == 0x01000000u &&
              FFX_API_BACKEND_ID_XBOX == 0x02000000u,
              "SDK 2.4 backend IDs required; check translation-unit include paths");

// Public FFX exports from the unmodified SDK 2.4 upscaler DLL. Load the effect
// explicitly: the central loader reuses an already loaded DLL with the same
// name, which could be our retained 4.1.1 runtime. Never use internal driver ABI.
namespace FsrSdk24 {
struct Error : std::runtime_error {
    const char* reason;
    Error(const char* why, const std::string& text) : std::runtime_error(text), reason(why) {}
};
struct Version { uint64_t id{}; std::string name; };
// Development-only input checks. Default rendering and benchmark paths stay unchanged.
inline bool DebugChecking() {
    const char* value = std::getenv("BB_FSR24_DEBUG");
    return value && std::string_view(value) == "1";
}
inline void Message(uint32_t type, const wchar_t* message) {
    if (message) std::printf("FSR SDK 2.4 message %u: %ls\n", type, message);
}
inline void ConfigureDiagnostics(ffxCreateContextDescUpscale& create) {
    create.fpMessage = &Message;
    if (DebugChecking()) create.flags |= FFX_UPSCALE_ENABLE_DEBUG_CHECKING;
}
inline bool Is412(const char* name) {
    return name && std::string_view(name).starts_with("4.1.2") &&
        (!name[5] || name[5] == ' ' || name[5] == '-' || name[5] == '*');
}
inline std::filesystem::path Directory() {
    if (const auto path = _wgetenv(L"BB_TEST_FSR24_DIR"); path && *path) return path;
    std::wstring path(32768, L'\0');
    const auto size = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
    if (!size || size == path.size()) throw Error("files", "Cannot locate the SDK 2.4 runtime folder");
    path.resize(size);
    return std::filesystem::path(path).parent_path() / L"fsr24";
}
inline bool VulkanRuntimePresent() noexcept {
    try {
        std::error_code error;
        return std::filesystem::is_regular_file(Directory() / L"amd_fidelityfx_upscaler_vk.dll", error);
    } catch (...) {
        return false;
    }
}
struct Api {
    HMODULE module{};
    ffxFunctions functions{};
    std::filesystem::path path;
};
inline const Api& Runtime(bool dx12) {
    static Api apis[2];
    static std::mutex mutex;
    std::scoped_lock lock{mutex};
    auto& api = apis[dx12 ? 1 : 0];
    if (api.module) return api;
    const auto path = std::filesystem::absolute(Directory() / (dx12 ? L"amd_fidelityfx_upscaler_dx12.dll" : L"amd_fidelityfx_upscaler_vk.dll"));
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) throw Error("files", "Missing SDK 2.4 file: " + path.filename().string());
    const auto module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module) throw Error("runtime", "Cannot load SDK 2.4 " + path.filename().string() + ": Windows error " + std::to_string(GetLastError()));
    std::wstring loaded(32768, L'\0');
    const auto length = GetModuleFileNameW(module, loaded.data(), DWORD(loaded.size()));
    loaded.resize(length);
    if (!length || _wcsicmp(std::filesystem::path(loaded).lexically_normal().c_str(), path.lexically_normal().c_str())) {
        FreeLibrary(module);
        throw Error("runtime", "SDK 2.4 load resolved to a different upscaler DLL");
    }
    ffxFunctions functions{};
    ffxLoadFunctions(&functions, module);
    if (!functions.CreateContext || !functions.DestroyContext || !functions.Configure || !functions.Query || !functions.Dispatch) {
        FreeLibrary(module);
        throw Error("runtime", "SDK 2.4 upscaler API exports missing");
    }
    api = {module, functions, path}; // Contexts may outlive callers; retain verified modules until exit.
    const auto override_path = _wgetenv(L"BB_TEST_FSR24_DIR");
    const auto utf8 = path.u8string();
    const std::string module_path(utf8.begin(), utf8.end());
    std::printf("FSR SDK 2.4 runtime: %s, source=%s, module=%s\n", dx12 ? "AMD DX12" : "AMD Vulkan",
        override_path && *override_path ? "BB_TEST_FSR24_DIR" : "package", module_path.c_str());
    return api;
}
inline Version Select(const Api& api, void* device) {
    uint64_t count{};
    ffxQueryDescGetVersions query{};
    query.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    query.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    query.device = device;
    query.outputCount = &count;
    if (api.functions.Query(nullptr, &query.header) != FFX_API_RETURN_OK || !count || count > 64)
        throw Error("provider", "SDK 2.4 upscaler enumeration unavailable on this GPU/driver");
    const auto capacity = count;
    std::vector<uint64_t> ids(capacity);
    std::vector<const char*> names(capacity);
    query.versionIds = ids.data(); query.versionNames = names.data();
    if (api.functions.Query(nullptr, &query.header) != FFX_API_RETURN_OK || count > capacity)
        throw Error("provider", "SDK 2.4 upscaler enumeration changed; retry the support check");
    for (size_t i = 0; i < count; ++i) {
        std::printf("SDK 2.4 upscaler provider: %s\n", names[i] ? names[i] : "(none)");
        if (Is412(names[i])) return {ids[i], names[i]};
    }
    throw Error("provider", "SDK 2.4: FSR 4.1.2 unavailable on this GPU/driver");
}
inline Version Verify(const Api& api, ffxContext* context, const Version& selected) {
    ffxQueryGetProviderVersion query{};
    query.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (api.functions.Query(context, &query.header) != FFX_API_RETURN_OK ||
        !Is412(query.versionName) || query.versionId != selected.id)
        throw Error("provider", "SDK 2.4 context did not select the requested FSR 4.1.2 provider");
    return {query.versionId, query.versionName};
}
// Publish only a verified, newly created context. No dispatch has used it yet,
// so failure cleanup here needs no wait on previously recorded GPU work.
inline Version CreateVerified(const Api& api, ffxContext& destination,
                              ffxCreateContextDescHeader* description,
                              const Version& selected) {
    if (destination) throw Error("runtime", "SDK 2.4 context is already initialized");
    ffxContext created{};
    try {
        const auto rc = api.functions.CreateContext(&created, description, nullptr);
        if (rc != FFX_API_RETURN_OK || !created)
            throw Error("runtime", "SDK 2.4 context creation returned " + std::to_string(rc));
        auto verified = Verify(api, &created, selected);
        destination = created;
        return verified;
    } catch (...) {
        if (created) api.functions.DestroyContext(&created, nullptr);
        throw;
    }
}
inline void LogMemory(const Api& api, ffxContext* context, const char* backend) {
    FfxApiEffectMemoryUsage memory{};
    ffxQueryDescUpscaleGetGPUMemoryUsage query{};
    query.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GPU_MEMORY_USAGE;
    query.gpuMemoryUsageUpscaler = &memory;
    const auto rc = api.functions.Query(context, &query.header);
    if (rc == FFX_API_RETURN_OK) {
        std::printf("FSR 4.1.2 %s SDK memory: total=%llu bytes, aliasable=%llu bytes\n",
            backend, static_cast<unsigned long long>(memory.totalUsageInBytes),
            static_cast<unsigned long long>(memory.aliasableUsageInBytes));
    } else {
        // Diagnostic query support is not a rendering or hardware requirement.
        std::printf("FSR 4.1.2 %s SDK memory query unavailable: code=%u\n", backend, unsigned(rc));
    }
}
} // namespace FsrSdk24

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
#include "ffx_framegeneration.h"

// Keep the legacy 4.0.1 DLL and SDK 2.4 effect in independent modules.
// This changes neither the bridge nor frame presentation.
namespace FgSdk24 {
struct Error : std::runtime_error {
    const char* reason;
    Error(const char* why, const std::string& text) : std::runtime_error(text), reason(why) {}
};
struct Version { uint64_t id{}; std::string name; };
inline bool Is402(const char* name) {
    return name && std::string_view(name).starts_with("4.0.2") &&
        (!name[5] || name[5] == ' ' || name[5] == '-' || name[5] == '*');
}
inline std::filesystem::path Directory() {
    if (const auto path = _wgetenv(L"BB_TEST_FG24_DIR"); path && *path) return path;
    std::wstring path(32768, L'\0');
    const auto size = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
    if (!size || size == path.size()) throw Error("files", "Cannot locate the SDK 2.4 FG runtime folder");
    path.resize(size);
    return std::filesystem::path(path).parent_path() / L"fsr24";
}
struct Api { HMODULE module{}; ffxFunctions functions{}; std::filesystem::path path; };
inline bool DebugChecking() {
    const char* value=std::getenv("BB_FSR24_DEBUG");
    return value && std::string_view(value)=="1";
}
inline void Message(uint32_t type, const wchar_t* message) {
    if (message) std::printf("FSR SDK 2.4 FG message %u: %ls\n",type,message);
}
inline void ConfigureDiagnostics(const Api& api, ffxCreateContextDescFrameGeneration& create) {
    if (DebugChecking()) create.flags |= FFX_FRAMEGENERATION_ENABLE_DEBUG_CHECKING;
    ffxConfigureDescGlobalDebug debug{};
    debug.header.type=FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG;
    debug.effectId=FFX_API_EFFECT_ID_FRAMEGENERATION;
    debug.fpMessage=&Message;
    debug.debugLevel=FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_ERRORS|FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_WARNINGS;
    const auto rc=api.functions.Configure(nullptr,&debug.header);
    if (rc!=FFX_API_RETURN_OK)
        std::printf("FSR SDK 2.4 FG diagnostics unavailable: code=%u\n",unsigned(rc));
}
inline void LogMemory(const Api& api, ffxContext* context) {
    FfxApiEffectMemoryUsage memory{};
    ffxQueryDescFrameGenerationGetGPUMemoryUsage query{};
    query.header.type=FFX_API_QUERY_DESC_TYPE_FRAMEGENERATION_GPU_MEMORY_USAGE;
    query.gpuMemoryUsageFrameGeneration=&memory;
    const auto rc=api.functions.Query(context,&query.header);
    if (rc==FFX_API_RETURN_OK)
        std::printf("ML-FG 4.0.2 SDK memory: total=%llu bytes, aliasable=%llu bytes\n",
            static_cast<unsigned long long>(memory.totalUsageInBytes),static_cast<unsigned long long>(memory.aliasableUsageInBytes));
    else std::printf("ML-FG 4.0.2 SDK memory query unavailable: code=%u\n",unsigned(rc));
}
inline const Api& Runtime() {
    static Api api;
    static std::mutex mutex;
    std::scoped_lock lock{mutex};
    if (api.module) return api;
    const auto path = std::filesystem::absolute(Directory() / L"amd_fidelityfx_framegeneration_dx12.dll");
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error))
        throw Error("files", "fsr24/amd_fidelityfx_framegeneration_dx12.dll");
    const auto module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module) throw Error("runtime", "SDK 2.4 FG: Windows error " + std::to_string(GetLastError()));
    std::wstring loaded(32768, L'\0');
    const auto size = GetModuleFileNameW(module, loaded.data(), DWORD(loaded.size()));
    loaded.resize(size);
    if (!size || _wcsicmp(std::filesystem::path(loaded).lexically_normal().c_str(), path.lexically_normal().c_str())) {
        FreeLibrary(module);
        throw Error("runtime", "SDK 2.4 FG resolved to a different DLL");
    }
    ffxFunctions functions{};
    ffxLoadFunctions(&functions, module);
    if (!functions.CreateContext || !functions.DestroyContext || !functions.Configure || !functions.Query || !functions.Dispatch) {
        FreeLibrary(module);
        throw Error("runtime", "SDK 2.4 FG API exports missing");
    }
    api = {module, functions, path}; // Retain while any FG context may exist.
    return api;
}
inline Version Select(const Api& api, void* device) {
    uint64_t count{};
    ffxQueryDescGetVersions query{};
    query.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    query.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    query.device = device; query.outputCount = &count;
    if (api.functions.Query(nullptr, &query.header) != FFX_API_RETURN_OK || !count || count > 64)
        throw Error("provider", "4.0.2");
    const auto capacity = count;
    std::vector<uint64_t> ids(capacity);
    std::vector<const char*> names(capacity);
    query.versionIds = ids.data(); query.versionNames = names.data();
    if (api.functions.Query(nullptr, &query.header) != FFX_API_RETURN_OK || count > capacity)
        throw Error("check", "SDK 2.4 FG enumeration changed; retry");
    for (size_t i = 0; i < count; ++i)
        if (Is402(names[i])) return {ids[i], names[i]};
    throw Error("provider", "4.0.2");
}
inline void Verify(const Api& api, ffxContext* context, const Version& selected) {
    ffxQueryGetProviderVersion query{};
    query.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (api.functions.Query(context, &query.header) != FFX_API_RETURN_OK ||
        !Is402(query.versionName) || query.versionId != selected.id)
        throw Error("provider", "SDK 2.4 FG context did not select the requested 4.0.2 provider");
}
} // namespace FgSdk24

// Local prototype. Provider ABI documented by OptiScaler, commit fa44a4a.
// ABI reference: OptiScaler contributors, GPL-3.0, commit fa44a4a.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <stdexcept>
#include <string>
#include "ffx_api.h"

namespace DriverFsr {
struct Provider {
    uint64_t versionId{};
    const char* versionName{};
    bool (*canProvide)(uint64_t){};
    PfnFfxCreateContext createContext{};
    PfnFfxDestroyContext destroyContext{};
    PfnFfxConfigure configure{};
    PfnFfxQuery query{};
    PfnFfxDispatch dispatch{};
};
struct External {
    uint32_t structVersion{3};
    uint64_t descType{};
    Provider provider{};
    bool (*isSupported)(const void*){};
    void* reserved{};
};
static_assert(offsetof(External,isSupported)==0x50 && sizeof(External)==0x60);
using Update = HRESULT (STDMETHODCALLTYPE*)(void*,uint32_t);

// This only changes the import in the private process's AMD module. Driver files
// remain signed and unchanged. The driver itself supplies a signed SDK stand-in.
inline std::atomic<HMODULE> vkModule{};
inline std::atomic<HMODULE> dxModule{};
inline thread_local bool directRequest{};
inline std::atomic<decltype(&GetModuleHandleW)> originalModuleHandle{};
inline HMODULE WINAPI ModuleHandle(LPCWSTR name) {
    const auto dx=dxModule.load();
    const auto vk=vkModule.load();
    const auto original=originalModuleHandle.load();
    if (directRequest && dx && name &&
        (!_wcsicmp(name,L"amd_fidelityfx_loader_dx12.dll") ||
         !_wcsicmp(name,L"amd_fidelityfx_upscaler_dx12.dll") ||
         !_wcsicmp(name,L"amd_fidelityfx_framegeneration_dx12.dll"))) return dx;
    HMODULE found = original ? original(name) : GetModuleHandleW(name);
    if (found || !name) return found;
    if (!_wcsicmp(name,L"amd_fidelityfx_loader_vk.dll") ||
        !_wcsicmp(name,L"amd_fidelityfx_upscaler_vk.dll") ||
        !_wcsicmp(name,L"amd_fidelityfx_framegeneration_vk.dll")) return vk;
    return nullptr;
}
inline void PatchSdkLookup(HMODULE module,bool dx=false) {
    auto base = reinterpret_cast<unsigned char*>(module);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base+dos->e_lfanew);
    auto dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) throw std::runtime_error("AMD driver import table missing");
    auto desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base+dir.VirtualAddress);
    for (;desc->Name;++desc) {
        if (!desc->OriginalFirstThunk) continue;
        auto names=reinterpret_cast<IMAGE_THUNK_DATA*>(base+desc->OriginalFirstThunk);
        auto slots=reinterpret_cast<IMAGE_THUNK_DATA*>(base+desc->FirstThunk);
        for (;names->u1.AddressOfData;++names,++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto name=reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base+names->u1.AddressOfData)->Name;
            if (std::strcmp(reinterpret_cast<char*>(name),"GetModuleHandleW")) continue;
            const auto imported=reinterpret_cast<decltype(&GetModuleHandleW)>(slots->u1.Function);
            if(imported==&ModuleHandle) {
                if(dx) dxModule=module; else vkModule=module;
                return;
            }
            DWORD old{};
            if (!VirtualProtect(&slots->u1.Function,sizeof(void*),PAGE_READWRITE,&old))
                throw std::runtime_error("Cannot install local AMD SDK lookup");
            originalModuleHandle=imported;
            if(dx) dxModule=module; else vkModule=module;
            slots->u1.Function=reinterpret_cast<ULONG_PTR>(&ModuleHandle);
            DWORD ignored{};
            VirtualProtect(&slots->u1.Function,sizeof(void*),old,&ignored);
            return;
        }
    }
    throw std::runtime_error("AMD SDK lookup import not found");
}
inline HMODULE Runtime(bool dx) {
    static HMODULE modules[2]{};
    static std::mutex mutex;
    const std::lock_guard lock(mutex);
    auto& module=modules[dx?1:0];
    if(module) return module;
    const wchar_t* path=_wgetenv(dx?L"BB_TEST_AMD_DX_RUNTIME":L"BB_TEST_AMD_VK_RUNTIME");
    if(!path || !*path) throw std::runtime_error("AMD driver runtime path missing");
    const auto candidate=LoadLibraryExW(path,nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if(!candidate) throw std::runtime_error("LoadLibraryEx: Windows error " + std::to_string(GetLastError()));
    try {
        PatchSdkLookup(candidate,dx);
    } catch (...) {
        FreeLibrary(candidate);
        throw;
    }
    module=candidate; // Publish only a fully initialized runtime; failures remain retryable.
    return module;
}
inline External Get(HMODULE module,uint64_t descType) {
    auto update=reinterpret_cast<Update>(GetProcAddress(module,"UpdateFfxApiProvider"));
    if (!update) throw std::runtime_error("AMD driver provider export missing");
    External out{}; out.descType=descType;
    struct Request {
        bool previous=directRequest;
        Request() { directRequest=true; }
        ~Request() { directRequest=previous; }
    } request;
    HRESULT hr=update(&out,sizeof(out));
    std::printf("Driver provider: HRESULT=%08lx id=%llx name=%s supported_query=%d\n",
        (unsigned long)hr,(unsigned long long)out.provider.versionId,
        out.provider.versionName?out.provider.versionName:"(none)",int(out.isSupported!=nullptr));
    if (FAILED(hr) || !out.provider.createContext || !out.provider.destroyContext ||
        !out.provider.dispatch || !out.provider.query) {
        char error[128]{};
        std::snprintf(error, sizeof(error), "UpdateFfxApiProvider: HRESULT=0x%08lx, provider API %s",
            (unsigned long)hr, out.provider.createContext && out.provider.destroyContext &&
                out.provider.dispatch && out.provider.query
                ? "present" : "missing");
        throw std::runtime_error(error);
    }
    return out;
}
inline bool Supported(const External& provider, const void* backend) {
    if (!provider.isSupported)
        throw std::runtime_error("AMD provider support query missing");
    return provider.isSupported(backend);
}
}

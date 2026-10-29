// SPDX-License-Identifier: GPL-2.0-or-later
#include "fg401_support.h"
#include "ffx_api.h"
#include "ffx_api_loader.h"
#include "ffx_framegeneration.h"
#include <cstring>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <filesystem>
#include <vector>
#include <windows.h>
#include <winternl.h>

bool Fg401WindowsSupported() {
    using RtlVersion = LONG(WINAPI *)(PRTL_OSVERSIONINFOW);
    const auto rtl = reinterpret_cast<RtlVersion>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    RTL_OSVERSIONINFOW v{};
    v.dwOSVersionInfoSize = sizeof(v);
    return rtl && !rtl(&v) && v.dwMajorVersion >= 10 && v.dwBuildNumber >= 22000;
}
Fg401Support ProbeFg401(const uint8_t bytes[8], uint32_t version) {
    struct Resources {
        IDXGIFactory4 *factory{};
        IDXGIAdapter1 *adapter{};
        ID3D12Device *device{};
        HMODULE module{};
        DWORD old_mode{};
        bool changed =
            SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX, &old_mode);
        ~Resources() {
            if (device)
                device->Release();
            if (adapter)
                adapter->Release();
            if (factory)
                factory->Release();
            if (module)
                FreeLibrary(module);
            if (changed)
                SetThreadErrorMode(old_mode, nullptr);
        }
    } r;
    if (!Fg401WindowsSupported())
        return {false, "windows11"};
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&r.factory))))
        return {false, "check", "DXGI"};
    LUID luid{};
    std::memcpy(&luid, bytes, 8);
    if (!(luid.LowPart || luid.HighPart) ||
        FAILED(r.factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&r.adapter))))
        return {false, "adapter"};
    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(r.adapter->GetDesc1(&desc)))
        return {false, "adapter"};
    char name[256]{};
    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
    if (!IsRx9000(desc.VendorId, name) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
        return {false, "rx9000"};
    std::wstring path(32768, L'\0');
    auto n = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
    path.resize(n);
    const auto directory = std::filesystem::path(path).parent_path();
    const auto dll = directory / L"amd_fidelityfx_framegeneration_dx12.dll";
    std::error_code error;
    if (!std::filesystem::is_regular_file(dll, error))
        return {false, "files", "amd_fidelityfx_framegeneration_dx12.dll"};
    if (GetProcAddress(GetModuleHandleW(nullptr), "D3D12SDKVersion") &&
        !std::filesystem::is_regular_file(directory / L"D3D12Core.dll", error))
        return {false, "files", "D3D12Core.dll"};
    if (FAILED(D3D12CreateDevice(r.adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&r.device))))
        return {false, "check", "D3D12CreateDevice"};
    D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_6};
    if (FAILED(r.device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))) ||
        sm.HighestShaderModel < D3D_SHADER_MODEL_6_6)
        return {false, "dx12", "Shader Model 6.6"};
    r.module = LoadLibraryExW(dll.c_str(), nullptr,
                              LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!r.module)
        return {false, "runtime", "Windows error " + std::to_string(GetLastError())};
    ffxFunctions ffx{};
    ffxLoadFunctions(&ffx, r.module);
    if (!ffx.Query || !ffx.CreateContext || !ffx.DestroyContext || !ffx.Dispatch || !ffx.Configure)
        return {false, "runtime", "FidelityFX API"};
    uint64_t count{};
    ffxQueryDescGetVersions q{};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    q.device = r.device;
    q.outputCount = &count;
    if (ffx.Query(nullptr, &q.header) != FFX_API_RETURN_OK || !count || count > 64)
        return {false, "provider"};
    std::vector<uint64_t> ids(count);
    std::vector<const char *> names(count);
    q.versionIds = ids.data();
    q.versionNames = names.data();
    if (ffx.Query(nullptr, &q.header) != FFX_API_RETURN_OK)
        return {false, "provider"};
    for (size_t i = 0; i < count && i < names.size(); ++i)
        if (Fg401ProviderMatches(ids[i], version))
            return {true, "ready"};
    return {false, "provider", Fg401VersionName(version)};
}

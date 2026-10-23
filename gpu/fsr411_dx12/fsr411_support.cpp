// SPDX-License-Identifier: GPL-2.0-or-later
#include "fsr411_support.h"
#include "ffx_api.h"
#include "ffx_api_loader.h"
#include "ffx_upscale.h"
#include "driver_provider.h"
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <cstring>
#include <filesystem>
#include <vector>

Fsr411Support ProbeFsrDx12Interop(VkPhysicalDevice device, FsrSupportTrace* trace) {
    const char* stage = "gpu";
    const auto fail = [&](const char* reason, std::string detail = {}) {
        FsrCheck(trace, stage, "fail", detail);
        return Fsr411Support{false, reason, std::move(detail)};
    };
    VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
    vkGetPhysicalDeviceProperties2(device, &props);
    if (props.properties.vendorID != 0x1002)
        return fail("hardware", "AMD GPU required");
    FsrCheck(trace, "gpu", "pass", "AMD");
    stage = "adapter";
    if (!id.deviceLUIDValid)
        return fail("adapter", "Vulkan/DX12 adapter LUID");
    FsrCheck(trace, "adapter", "pass", "Vulkan/DX12 LUID");
    stage = "interop";
    uint32_t count{};
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr) != VK_SUCCESS)
        return fail("check", "Vulkan extension query");
    std::vector<VkExtensionProperties> extensions(count);
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, extensions.data()) != VK_SUCCESS)
        return fail("check", "Vulkan extension query");
    bool memory = false, semaphore = false;
    for (const auto& extension : extensions) {
        memory |= !std::strcmp(extension.extensionName, "VK_KHR_external_memory_win32");
        semaphore |= !std::strcmp(extension.extensionName, "VK_KHR_external_semaphore_win32");
    }
    if (!memory || !semaphore)
        return fail("interop", "Vulkan/DX12 shared memory");
    VkSemaphoreTypeCreateInfo timeline{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    timeline.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkPhysicalDeviceExternalSemaphoreInfo info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
        &timeline, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT};
    VkExternalSemaphoreProperties fences{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
    vkGetPhysicalDeviceExternalSemaphoreProperties(device, &info, &fences);
    if (!(fences.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT))
        return fail("interop", "Vulkan/DX12 shared GPU fence");
    FsrCheck(trace, "interop", "pass", "external memory + D3D12 fence import");
    stage = "runtime_file";
    if (GetProcAddress(GetModuleHandleW(nullptr), "D3D12SDKVersion")) {
        std::wstring path(32768, L'\0');
        path.resize(GetModuleFileNameW(nullptr, path.data(), DWORD(path.size())));
        std::error_code error;
        if (!std::filesystem::is_regular_file(std::filesystem::path(path).parent_path() / L"D3D12Core.dll", error))
            return fail("files", "D3D12Core.dll");
    }
    FsrCheck(trace, "runtime_file", "pass", "D3D12Core.dll / system D3D12");
    return {true, "ready", "Vulkan/DX12 shared resources"};
}

Fsr411Support ProbeFsr411Dx12(const uint8_t bytes[8], FsrSupportTrace* trace) {
    const char* stage = "runtime_file";
    const auto fail = [&](const char* reason, std::string detail = {}) {
        FsrCheck(trace, stage, "fail", detail);
        return Fsr411Support{false, reason, std::move(detail)};
    };
    struct Resources {
        IDXGIFactory4* factory{};
        IDXGIAdapter1* adapter{};
        ID3D12Device* device{};
        HMODULE module{};
        DWORD old_mode{};
        bool changed = SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX, &old_mode);
        ~Resources() {
            if (device) device->Release();
            if (adapter) adapter->Release();
            if (factory) factory->Release();
            if (module) FreeLibrary(module);
            if (changed) SetThreadErrorMode(old_mode, nullptr);
        }
    } r;
    std::wstring path(32768, L'\0');
    path.resize(GetModuleFileNameW(nullptr, path.data(), DWORD(path.size())));
    const auto dll = std::filesystem::path(path).parent_path() / L"amd_fidelityfx_upscaler_dx12.dll";
    std::error_code error;
    if (!std::filesystem::is_regular_file(dll, error))
        return fail("files", "amd_fidelityfx_upscaler_dx12.dll");
    FsrCheck(trace, "runtime_file", "pass", "amd_fidelityfx_upscaler_dx12.dll");
    stage = "runtime_load";
    r.module = LoadLibraryExW(dll.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!r.module)
        return fail("runtime", "amd_fidelityfx_upscaler_dx12.dll: Windows error " +
            std::to_string(GetLastError()) + "; check runtime dependencies");
    FsrCheck(trace, "runtime_load", "pass", "amd_fidelityfx_upscaler_dx12.dll");
    stage = "exports";
    ffxFunctions ffx{};
    ffxLoadFunctions(&ffx, r.module);
    if (!ffx.Query || !ffx.CreateContext || !ffx.DestroyContext || !ffx.Dispatch)
        return fail("runtime", "FidelityFX API exports");
    FsrCheck(trace, "exports", "pass", "FidelityFX API");
    stage = "adapter";
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&r.factory))))
        return fail("driver", "DXGI factory");
    LUID luid{};
    std::memcpy(&luid, bytes, 8);
    if (!(luid.LowPart || luid.HighPart) ||
        FAILED(r.factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&r.adapter))))
        return fail("adapter", "Vulkan/DX12 adapter LUID");
    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(r.adapter->GetDesc1(&desc))) return fail("adapter", "DXGI adapter");
    if (desc.VendorId != 0x1002 || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
        return fail("hardware", "AMD GPU required");
    FsrCheck(trace, "adapter", "pass", "AMD / matching Vulkan LUID");
    stage = "device";
    if (FAILED(D3D12CreateDevice(r.adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&r.device))))
        return fail("driver", "D3D12 feature level 12_0");
    FsrCheck(trace, "device", "pass", "D3D12 feature level 12_0");
    stage = "provider_query";
    uint64_t count{};
    ffxQueryDescGetVersions q{};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    q.device = r.device;
    q.outputCount = &count;
    if (ffx.Query(nullptr, &q.header) != FFX_API_RETURN_OK || !count || count > 64)
        return fail("provider", "AMD FSR 4.1.1 provider enumeration");
    std::vector<uint64_t> ids(count);
    std::vector<const char*> names(count);
    q.versionIds = ids.data(); q.versionNames = names.data();
    if (ffx.Query(nullptr, &q.header) != FFX_API_RETURN_OK)
        return fail("provider", "AMD FSR 4.1.1 provider enumeration");
    FsrCheck(trace, "provider_query", "pass", "FidelityFX provider enumeration");
    for (const char* name : names)
        if (name) FsrCheck(trace, "provider", "info", name);
    stage = "provider_version";
    for (const char* name : names)
        if (name && std::strstr(name, "4.1.1") &&
            !std::strstr(name, "i8") && !std::strstr(name, "Int8"))
            { FsrCheck(trace, "provider_version", "pass", name); return {true, "ready"}; }
    return fail("provider", "AMD FSR 4.1.1 provider unavailable on this GPU/driver");
}

Fsr411Support ProbeFsr412Dx12(const uint8_t bytes[8], FsrSupportTrace* trace) {
    const char* stage = "adapter";
    const auto fail = [&](const char* reason, std::string detail = {}) {
        FsrCheck(trace, stage, "fail", detail);
        return Fsr411Support{false, reason, std::move(detail)};
    };
    struct Resources {
        IDXGIFactory4* factory{};
        IDXGIAdapter1* adapter{};
        ID3D12Device* device{};
        ~Resources() {
            if (device) device->Release();
            if (adapter) adapter->Release();
            if (factory) factory->Release();
        }
    } r;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&r.factory)))) return fail("driver", "DXGI factory");
    LUID luid{}; std::memcpy(&luid, bytes, 8);
    if (FAILED(r.factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&r.adapter))))
        return fail("adapter", "Vulkan/DX12 adapter LUID");
    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(r.adapter->GetDesc1(&desc))) return fail("check", "DXGI adapter");
    if (desc.VendorId != 0x1002) return fail("hardware", "AMD GPU required");
    FsrCheck(trace, "adapter", "pass", "AMD / matching Vulkan LUID");
    stage = "device";
    if (FAILED(D3D12CreateDevice(r.adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&r.device))))
        return fail("driver", "D3D12 feature level 12_0");
    FsrCheck(trace, "device", "pass", "D3D12 feature level 12_0");
    try {
        stage = "runtime_load";
        const auto module = DriverFsr::Runtime(true);
        FsrCheck(trace, stage, "pass", "amdxcffx64.dll");
        stage = "provider_query";
        const auto provider = DriverFsr::Get(module, FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE);
        char identity[64]{};
        std::snprintf(identity, sizeof(identity), "0x%llx", (unsigned long long)provider.provider.versionId);
        FsrCheck(trace, stage, "pass", std::string(identity) + " " +
            (provider.provider.versionName ? provider.provider.versionName : ""));
        stage = "provider_version";
        if ((uint32_t(provider.provider.versionId) & 0x7fffffffu) != ((4u<<22)|(1u<<12)|2u))
            return fail("driver", "FSR 4.1.2 provider unavailable");
        FsrCheck(trace, stage, "pass", "4.1.2");
        stage = "provider_support";
        if (!DriverFsr::Supported(provider, r.device))
            return fail("hardware", "FSR 4.1.2 unsupported by selected GPU");
        FsrCheck(trace, stage, "pass", "isSupported=true");
        return {true, "ready", "4.1.2"};
    } catch (const std::exception& error) {
        return fail(std::string_view(stage) == "provider_support" ? "check" : "driver", error.what());
    }
}

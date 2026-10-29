// SPDX-License-Identifier: GPL-3.0-or-later
// Test-only provider ABI. No GPU access and no invocation of the dummy context callbacks.
#include "driver_provider.h"
static void Dummy() {}
static bool Supported(const void*) { return GetEnvironmentVariableW(L"BB_TEST_PROVIDER_REJECT", nullptr, 0) == 0; }
extern "C" __declspec(dllexport) HRESULT WINAPI UpdateFfxApiProvider(void* data, uint32_t size) {
    if (!GetModuleHandleW(L"kernel32.dll") || size != sizeof(DriverFsr::External)) return E_FAIL;
    auto& out = *static_cast<DriverFsr::External*>(data);
    out.provider.versionId = (4u << 22) | (1u << 12) | 2u;
    out.provider.versionName = "4.1.2 fixture";
    out.provider.createContext = reinterpret_cast<PfnFfxCreateContext>(&Dummy);
    out.provider.destroyContext = reinterpret_cast<PfnFfxDestroyContext>(&Dummy);
    out.provider.query = reinterpret_cast<PfnFfxQuery>(&Dummy);
    out.provider.dispatch = reinterpret_cast<PfnFfxDispatch>(&Dummy);
    out.isSupported = &Supported;
    if (GetEnvironmentVariableW(L"BB_TEST_PROVIDER_NO_SUPPORT", nullptr, 0)) out.isSupported = nullptr;
    if (GetEnvironmentVariableW(L"BB_TEST_PROVIDER_NO_DESTROY", nullptr, 0)) out.provider.destroyContext = nullptr;
    return S_OK;
}

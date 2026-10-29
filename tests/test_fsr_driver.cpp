// SPDX-License-Identifier: GPL-3.0-or-later
#include "driver_provider.h"
#include "bbport_settings.h"
#include "fsr411_support.h"
#include "bbport_fsr4vk_package.h"
#include <cassert>
#include <filesystem>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    assert(argc == 4);
    const auto invalid = std::filesystem::absolute(argv[1]);
    const auto valid = std::filesystem::absolute(argv[2]);
    _wputenv_s(L"BB_TEST_AMD_DX_RUNTIME", invalid.c_str());
    for (int attempt = 0; attempt < 2; ++attempt) {
        bool rejected = false;
        try { DriverFsr::Runtime(true); } catch (const std::exception&) { rejected = true; }
        assert(rejected);
        assert(!GetModuleHandleW(invalid.filename().c_str()));
    }
    _wputenv_s(L"BB_TEST_AMD_DX_RUNTIME", valid.c_str());
    std::vector<std::thread> threads;
    HMODULE handles[8]{};
    for (int i = 0; i < 8; ++i) threads.emplace_back([&, i] { handles[i] = DriverFsr::Runtime(true); });
    for (auto& thread : threads) thread.join();
    for (const auto module : handles) assert(module && module == handles[0]);
    const auto module = handles[0];
    // Reusing a DLL for an explicit second API override must not hook our hook.
    _wputenv_s(L"BB_TEST_AMD_VK_RUNTIME", valid.c_str());
    assert(DriverFsr::Runtime(false) == module);
    assert(DriverFsr::ModuleHandle(L"kernel32.dll"));
    auto provider = DriverFsr::Get(module, 0);
    assert(DriverFsr::Supported(provider, nullptr));
    SetEnvironmentVariableW(L"BB_TEST_PROVIDER_REJECT", L"1");
    assert(!DriverFsr::Supported(DriverFsr::Get(module, 0), nullptr));
    SetEnvironmentVariableW(L"BB_TEST_PROVIDER_REJECT", nullptr);
    for (const auto variable : {L"BB_TEST_PROVIDER_NO_SUPPORT", L"BB_TEST_PROVIDER_NO_DESTROY"}) {
        SetEnvironmentVariableW(variable, L"1");
        bool rejected = false;
        try { DriverFsr::Supported(DriverFsr::Get(module, 0), nullptr); }
        catch (const std::exception&) { rejected = true; }
        assert(rejected);
        assert(!DriverFsr::directRequest);
        SetEnvironmentVariableW(variable, nullptr);
    }
    for (int upscaler = 0; upscaler < BbSettings::UpscalerCount; ++upscaler) {
        assert(!BbSettings::UsesHostSharpen(upscaler, false));
        assert(BbSettings::UsesHostSharpen(upscaler, true) ==
            (upscaler == BbSettings::UpscalerFsr411 || upscaler == BbSettings::UpscalerFsr412));
    }
    assert(FsrSupportCode({false,"check","support query missing"}) == 5);
    assert(FsrSupportCode({false,"hardware","AMD GPU required"}) == 1);
    assert(FsrSupportCode({false,"hardware","VK_EXT_descriptor_buffer"}) == 2);
    assert(FsrSupportCode({false,"files","D3D12Core.dll"}) == 4);
    const auto package = std::filesystem::absolute(argv[3]);
    assert(Fsr4VkPackage::Verified(package));
    const auto temp_base = std::filesystem::canonical(std::filesystem::temp_directory_path());
    const auto temp = temp_base /
        (L"fsr-package-\u00e4-" + std::to_wstring(GetCurrentProcessId()));
    assert(temp.is_absolute() && temp.lexically_normal().parent_path() == temp_base);
    assert(!std::filesystem::exists(temp));
    std::filesystem::copy(package, temp, std::filesystem::copy_options::recursive);
    assert(Fsr4VkPackage::Verified(temp));
    std::filesystem::remove(temp / Fsr4VkPackage::Files[1].name);
    assert(!Fsr4VkPackage::Verified(temp));
    std::filesystem::copy_file(package / Fsr4VkPackage::Files[1].name,
        temp / Fsr4VkPackage::Files[1].name);
    { std::fstream changed(temp / Fsr4VkPackage::Files[0].name, std::ios::binary | std::ios::in | std::ios::out);
      changed.put('!'); }
    assert(!Fsr4VkPackage::Verified(temp));
    assert(std::filesystem::canonical(temp).parent_path() == temp_base);
    std::filesystem::remove_all(temp); // Only the unique test folder created above.
    std::puts("PASS: retry cleanup, concurrent runtime loading, exact support proof, API validation and independent host sharpening");
}

// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_settings.h"
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <windows.h>
int main() {
    using namespace BbSettings;
    for (int provider=UpscalerOff;provider<UpscalerCount;++provider) {
        assert(WindowsAutoExposureFixed(provider,Fsr411Vulkan)==(provider==UpscalerFsr412));
        assert(WindowsAutoExposureFixed(provider,Fsr411Dx12)==(provider==UpscalerFsr412||provider==UpscalerFsr411));
    }
    wchar_t path[MAX_PATH]{};
    assert(GetTempFileNameW(std::filesystem::temp_directory_path().c_str(),L"bbu",0,path));
    _wputenv_s(L"BB_CONFIG",path);
    _putenv_s("BB_UPSCALER","");
    auto& s=Get();
    for (const int choice:{UpscalerFsr4,UpscalerFsr411,UpscalerFsr412}) {
        SelectUpscaler(choice);
        assert(s.ActiveUpscaler()==choice);
        UpscalerFailure(choice);
        assert(s.upscaler==choice&&s.ActiveUpscaler()==UpscalerFsr3);
        // Saving any other overlay setting must preserve the failed FSR choice.
        s.sharpness=.67f;s.object_motion_cache=true;Save();
        std::ifstream file{std::filesystem::path(path)};
        const std::string text((std::istreambuf_iterator<char>(file)),{});
        assert(text.find(std::string("upscaler=")+UpscalerName(choice)+"\n")!=std::string::npos);
        assert(text.find("sharpness=0.67\n")!=std::string::npos);
        const auto request=s.upscaler_request.load();
        SelectUpscaler(choice); // retry the same visible selection
        assert(s.ActiveUpscaler()==choice&&s.upscaler_request==request+1);
        SelectUpscaler(choice);
        assert(s.upscaler_request==request+1); // selecting a working route is not a reset
        UpscalerFailure(choice);Save();Load();
        assert(s.upscaler==choice&&s.ActiveUpscaler()==choice&&!s.UpscalerFallbackActive());
    }
    for (const int choice:{UpscalerFsr4,UpscalerFsr411}) {
        SelectUpscaler(choice);ConfigureUpscalerSupport(false,false,"CPU fixture unavailable");
        assert(s.upscaler==choice&&s.ActiveUpscaler()==UpscalerFsr3);
        Save();Load();assert(s.upscaler==choice&&s.ActiveUpscaler()==choice);
    }
    SelectUpscaler(UpscalerFsr412);UpscalerFailure(UpscalerFsr412);
    SelectUpscaler(UpscalerFsr411);UpscalerFailure(UpscalerFsr412);
    assert(s.ActiveUpscaler()==UpscalerFsr411); // late failure from another selection
    SelectUpscaler(UpscalerFsr3);UpscalerFailure(UpscalerFsr411);
    assert(s.ActiveUpscaler()==UpscalerFsr3&&!s.UpscalerFallbackActive());
    std::filesystem::remove(path);
    std::puts("PASS: selected/active FSR states, unrelated saves, retry, startup failure and restart");
}

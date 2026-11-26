// SPDX-License-Identifier: GPL-2.0-or-later
// Saved FSR 4 choices must be safe before the first rendered frame on older GPUs.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <string>
#ifdef _WIN32
#include <filesystem>
#include <windows.h>
#define setenv(key, value, overwrite) _putenv_s(key, value)
#define unsetenv(key) _putenv_s(key, "")
#else
#include <unistd.h>
#endif
#include "bbport_settings.h"

int main() {
    using namespace BbSettings;
#ifdef _WIN32
    wchar_t temporary[MAX_PATH]{};
    assert(GetTempFileNameW(std::filesystem::temp_directory_path().c_str(), L"bbu", 0, temporary));
    const auto temporary_path = std::filesystem::path(temporary).string();
    const char* path = temporary_path.c_str();
#else
    char path[] = "/tmp/bbport-upscaler-test-XXXXXX";
    const int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
#endif
    setenv("BB_CONFIG", path, 1);
    unsetenv("BB_UPSCALER");
    unsetenv("BB_UPSCALE_PRESET");
    unsetenv("BB_RENDER_RES");
    auto& s = Get();
    s.upscaler = UpscalerTaa;
    s.preset = Performance;
    assert(RenderPreset() == NativeAA && s.preset == Performance);
    Save();
    s.upscaler = UpscalerOff;
    Load();
    assert(s.upscaler == UpscalerTaa && s.preset == Performance);
    assert(RenderPreset() == NativeAA && !ResolutionNeedsRestart());
    s.upscaler = UpscalerFsr3;
    assert(RenderPreset() == Performance);
    for (bool fsr4 : {false, true}) {
        for (bool fsr411 : {false, true}) {
            for (int requested = 0; requested < UpscalerCount; ++requested) {
                FILE* config = std::fopen(path, "w");
                assert(config);
                std::fprintf(config, "upscaler=%s\npreset=3\noutput_res=2560x1440\n",
                             UpscalerName(requested));
                std::fclose(config);
                s.fsr4_problem = nullptr;
                Load();
                assert(s.upscaler == requested);
                ConfigureUpscalerSupport(fsr4, fsr411);
                const bool unsupported = (requested == UpscalerFsr4 && !fsr4) ||
                    (requested == UpscalerFsr411 && !fsr411);
                // The choice stays (Save() keeps it); this session runs FSR 3.1 instead.
                assert(s.upscaler == requested);
                assert(s.ActiveUpscaler() == (unsupported ? UpscalerFsr3 : requested));
                assert(s.fsr4_supported == fsr4);
                assert(s.fsr411_supported == fsr411);
                assert(bool(s.fsr4_problem.load()) == unsupported);
                assert(s.preset == Performance && s.output_res == 2);
                // Startup patch settings still describe the already applied guest patches.
                assert(s.startup_upscaler == requested);
                ConfigureUpscalerSupport(fsr4, fsr411);
                assert(s.upscaler == requested);
                assert(s.ActiveUpscaler() == (unsupported ? UpscalerFsr3 : requested));
            }
        }
    }
    for (const char* reason : {"Vulkan (fsr4vk): missing amd_fidelityfx_upscaler_vk.dll; download fsr4vk in Graphics",
                              "Vulkan (fsr4vk): GPU/driver lacks VK_EXT_descriptor_buffer",
                              "AMD DX12: runtime load failure - Windows error 126"}) {
        s.upscaler = UpscalerFsr411;
        s.fsr4_fallback_source = -1;
        ConfigureUpscalerSupport(true, false, reason);
        assert(s.upscaler == UpscalerFsr411 && s.ActiveUpscaler() == UpscalerFsr3);
        assert(std::string(s.fsr4_problem.load()).find(reason) == 0);
        assert(std::string(s.fsr411_problem.load()) == reason);
        assert(std::string(s.fsr4_problem.load()).find("using FSR 3.1") != std::string::npos);
    }
    ConfigureUpscalerSupport(true, true);
    assert(!s.fsr411_problem.load());
    s.startup_preset = Quality;
    s.startup_upscaler = UpscalerFsr3;
    s.upscaler = UpscalerFsr3;
    s.preset = Performance;
    assert(RenderPreset() == Performance && !ResolutionNeedsRestart());
    setenv("BB_RENDER_RES", "1706x960", 1);
    assert(RenderPreset() == Quality && ResolutionNeedsRestart());
    s.preset = Quality;
    assert(!ResolutionNeedsRestart());
    s.upscaler = UpscalerOff;
    assert(ResolutionNeedsRestart());
    s.upscaler = UpscalerFsr4;
    assert(!ResolutionNeedsRestart()); // provider change at the same patched size is live
    s.upscaler = UpscalerTaa;          // TAA needs native guest targets
    assert(ResolutionNeedsRestart());
    s.upscaler = UpscalerFsr4;
    const int output = s.startup_output_res;
    s.output_res = (output + 1) % OutputCount;
    assert(ResolutionNeedsRestart());
    s.output_res = output;
    setenv("BB_RENDER_RES", "", 1);
    assert(!FixedRenderSession() && !ResolutionNeedsRestart());
    unsetenv("BB_RENDER_RES");
    std::remove(path);
    std::puts("Upscaler support: saved choices and FSR 3.1 fallback PASS");
}

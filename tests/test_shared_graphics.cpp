// SPDX-License-Identifier: GPL-2.0-or-later
// Shared INI preferences, deferred activation, and direct-engine override compatibility.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include "bbport_settings.h"
#ifdef _WIN32
#include <windows.h>
#define setenv(key, value, overwrite) _putenv_s(key, value)
#else
#include <unistd.h>
#endif

void Config(const char* path, const char* text) {
    std::ofstream(path) << text;
}

int main(int argc, char** argv) {
    using namespace BbSettings;
    if (argc >= 3) {
        setenv("BB_CONFIG", argv[2], 1);
        setenv("BB_SHARED_GRAPHICS_CONFIG", "1", 1);
        Load();
        auto& s = Get();
        if (std::string(argv[1]) == "save") {
            assert(argc == 5);
            s.fsr411_backend = std::string(argv[3]) == "dx12" ? Fsr411Dx12 : Fsr411Vulkan;
            s.object_motion_cache = std::string(argv[4]) == "1";
            Save();
        }
        std::printf("{\"backend\":\"%s\",\"cache\":%d,\"restart\":%d,\"startup_backend\":%d}\n",
            s.fsr411_backend == Fsr411Dx12 ? "dx12" : "vk", int(s.object_motion_cache.load()),
            int(FsrSettingsNeedRestart()), s.startup_fsr411_backend);
        return 0;
    }
#ifdef _WIN32
    wchar_t temporary[MAX_PATH]{};
    assert(GetTempFileNameW(std::filesystem::temp_directory_path().c_str(), L"bbs", 0, temporary));
    const auto name = std::filesystem::path(temporary).string();
#else
    char temporary[] = "/tmp/bbport-shared-test-XXXXXX";
    const int fd = mkstemp(temporary);
    assert(fd >= 0);
    close(fd);
    const std::string name = temporary;
#endif
    const char* path = name.c_str();
    setenv("BB_CONFIG", path, 1);
    setenv("BB_SHARED_GRAPHICS_CONFIG", "1", 1);
    setenv("BB_FSR411_BACKEND", "vk", 1);
    setenv("BB_OBJECT_MOTION_CACHE", "0", 1);
    Config(path, "upscaler=fsr411\nobject_motion=1\nfsr411_backend=dx12\nobject_motion_cache=1\n");
    Load();
    auto& s = Get();
    assert(s.fsr411_backend == Fsr411Dx12 && s.object_motion_cache);
    assert(s.startup_fsr411_backend == Fsr411Dx12 && s.startup_object_motion_cache);
    assert(!FsrSettingsNeedRestart());
    s.fsr411_backend = Fsr411Vulkan;
    assert(!FsrSettingsNeedRestart()); // Backend changes are live; the cache is not.
    s.object_motion_cache = false;
    assert(FsrSettingsNeedRestart());
    assert(s.startup_fsr411_backend == Fsr411Dx12 && s.startup_object_motion_cache);
    Save();
    Load();
    assert(s.fsr411_backend == Fsr411Vulkan && !s.object_motion_cache);
    assert(!FsrSettingsNeedRestart());
    // An unavailable cache retains its saved preference, and is applied when FSR returns.
    Config(path, "upscaler=taa\nobject_motion=1\nfsr411_backend=dx12\nobject_motion_cache=1\n");
    Load();
    assert(s.object_motion_cache && !FsrSettingsNeedRestart());
    s.upscaler = UpscalerFsr3;
    assert(FsrSettingsNeedRestart());
    Save();
    Load();
    assert(!FsrSettingsNeedRestart());
    s.upscaler = UpscalerFsr4;
    assert(!FsrSettingsNeedRestart());
    s.upscaler = UpscalerFsr411;
    assert(!FsrSettingsNeedRestart());
    // Explicit script overrides remain available outside managed launcher starts.
    setenv("BB_SHARED_GRAPHICS_CONFIG", "0", 1);
    Load();
    assert(s.fsr411_backend == Fsr411Vulkan && !s.object_motion_cache);
    setenv("BB_SHARED_GRAPHICS_CONFIG", "1", 1);
    Config(path, "fsr411_backend=invalid\nobject_motion_cache=true\n");
    Load();
    assert(s.fsr411_backend == Fsr411Vulkan && !s.object_motion_cache);
    Config(path, "");
    Load();
    assert(s.fsr411_backend == Fsr411Vulkan && !s.object_motion_cache);
    std::remove(path);
    std::puts("Shared graphics: INI roundtrip, deferred activation and overrides PASS");
}

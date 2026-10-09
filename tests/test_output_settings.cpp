// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_settings.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <chrono>
#include <initializer_list>

int main() {
    using namespace BbSettings;
    const auto path = std::filesystem::temp_directory_path() /
        ("bb-output-settings-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) + ".ini");
#ifdef _WIN32
    _putenv_s("BB_CONFIG", path.string().c_str());
    _putenv_s("BB_RENDER_RES", "");
#else
    setenv("BB_CONFIG", path.string().c_str(), 1);
    unsetenv("BB_RENDER_RES");
#endif
    auto& settings = Get();
    for (const char* size : {"1920x1080", "3840x1820", "5000x2000", "1080x1920", "1280x800"}) {
        FILE* file = std::fopen(path.string().c_str(), "w");
        assert(file);
        std::fprintf(file, "output_res=%s\n", size);
        std::fclose(file);
        Load();
        int w = 0, h = 0;
        assert(std::sscanf(size, "%dx%d", &w, &h) == 2);
        assert(OutputWidths[settings.output_res] == w && OutputHeights[settings.output_res] == h);
        Save();
        settings.output_res = OutputDefault;
        Load();
        assert(OutputWidths[settings.output_res] == w && OutputHeights[settings.output_res] == h);
    }
    for (const char* bad : {"999999999999999999999x1080", "0x0", "1921x1080",
                           "hello", "1920x1080junk", "+1920x1080", "1920x1080x2"}) {
        FILE* file = std::fopen(path.string().c_str(), "w");
        assert(file);
        std::fprintf(file, "output_res=%s\n", bad);
        std::fclose(file);
        settings.output_res = OutputDefault;
        Load();
        assert(settings.output_res == OutputDefault);
    }
    std::filesystem::remove(path);
    std::puts("Custom output settings: save/load and malformed input rejection PASS");
}

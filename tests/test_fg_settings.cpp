// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_settings.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <chrono>

int main() {
    using namespace BbSettings;
    const auto path = std::filesystem::temp_directory_path() /
        ("bb-fg-settings-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) + ".ini");
#ifdef _WIN32
    _putenv_s("BB_CONFIG", path.string().c_str());
    _putenv_s("BB_FRAME_GENERATION", "");
    _putenv_s("BB_FG_BACKEND", "");
    _putenv_s("BB_REFLEX", "");
#else
    setenv("BB_CONFIG", path.string().c_str(), 1);
    unsetenv("BB_FRAME_GENERATION");
    unsetenv("BB_FG_BACKEND");
    unsetenv("BB_REFLEX");
#endif
    auto& settings = Get();

    // 1. Defaults verification on init
    {
        FILE* file = std::fopen(path.string().c_str(), "w");
        assert(file);
        std::fclose(file);
        Load();
        assert(settings.frame_generation == 0); // Off
        assert(settings.fg_backend == FgDlss);  // DLSS
        assert(settings.reflex == ReflexOn);    // Reflex On guarded
        assert(settings.reflex_supported.load() == true);
        assert(settings.startup_frame_generation == 0);
        assert(settings.startup_fg_backend == FgDlss);
        assert(settings.startup_reflex == ReflexOn);
        assert(settings.source_fps.load() == 0.0f);
        assert(settings.display_fps.load() == 0.0f);
    }

    // 2. Roundtrip ini: DLSS with 2x, 3x, 4x, Reflex off/on/boost
    for (const char* factor : {"off", "2x", "3x", "4x"}) {
        for (const char* reflex_mode : {"off", "on", "boost"}) {
            FILE* file = std::fopen(path.string().c_str(), "w");
            assert(file);
            std::fprintf(file, "fg_backend=dlss\nframe_generation=%s\nreflex=%s\n", factor, reflex_mode);
            std::fclose(file);
            Load();

            int expected_fg = 0;
            if (std::strcmp(factor, "2x") == 0) expected_fg = 2;
            else if (std::strcmp(factor, "3x") == 0) expected_fg = 3;
            else if (std::strcmp(factor, "4x") == 0) expected_fg = 4;
            assert(settings.frame_generation == expected_fg);
            assert(settings.fg_backend == FgDlss);

            int expected_reflex = ReflexOn;
            if (std::strcmp(reflex_mode, "off") == 0) expected_reflex = ReflexOff;
            else if (std::strcmp(reflex_mode, "boost") == 0) expected_reflex = ReflexBoost;
            assert(settings.reflex == expected_reflex);

            // Test Save and re-load
            Save();
            settings.frame_generation = 0;
            settings.fg_backend = FgFsr;
            settings.reflex = ReflexOff;
            Load();
            assert(settings.frame_generation == expected_fg);
            assert(settings.fg_backend == FgDlss);
            assert(settings.reflex == expected_reflex);
        }
    }

    // 3. FSR backend factor clamping: FSR supports Off and 2x only!
    // 3x and 4x on FSR must clamp down to 2x.
    {
        FILE* file = std::fopen(path.string().c_str(), "w");
        assert(file);
        std::fprintf(file, "fg_backend=fsr\nframe_generation=4x\nreflex=on\n");
        std::fclose(file);
        Load();
        assert(settings.fg_backend == FgFsr);
        assert(settings.frame_generation == 2); // clamped to 2x

        file = std::fopen(path.string().c_str(), "w");
        assert(file);
        std::fprintf(file, "fg_backend=fsr\nframe_generation=3x\nreflex=on\n");
        std::fclose(file);
        Load();
        assert(settings.fg_backend == FgFsr);
        assert(settings.frame_generation == 2); // clamped to 2x

        file = std::fopen(path.string().c_str(), "w");
        assert(file);
        std::fprintf(file, "fg_backend=fsr\nframe_generation=off\nreflex=on\n");
        std::fclose(file);
        Load();
        assert(settings.fg_backend == FgFsr);
        assert(settings.frame_generation == 0); // off preserved
    }

    // 4. Switching backend clamps active factor
    {
        FILE* file = std::fopen(path.string().c_str(), "w");
        assert(file);
        std::fprintf(file, "fg_backend=dlss\nframe_generation=4x\n");
        std::fclose(file);
        Load();
        assert(settings.frame_generation == 4);

        // Switch to FSR: must clamp 4x to 2x
        file = std::fopen(path.string().c_str(), "w");
        assert(file);
        std::fprintf(file, "frame_generation=4x\nfg_backend=fsr\n");
        std::fclose(file);
        Load();
        assert(settings.fg_backend == FgFsr);
        assert(settings.frame_generation == 2);
    }

    // 5. Malformed values clamping & aliases
    {
        FILE* file = std::fopen(path.string().c_str(), "w");
        assert(file);
        std::fprintf(file, "fg_backend=garbage\nframe_generation=100x\nreflex=superboost\n");
        std::fclose(file);
        Load();
        assert(settings.fg_backend == FgDlss);
        assert(settings.frame_generation == 0);
        assert(settings.reflex == ReflexOn);

        // Numeric environment / ini aliases: "1" for FSR, "2" for 2x, "0" for Reflex Off
        file = std::fopen(path.string().c_str(), "w");
        assert(file);
        std::fprintf(file, "fg_backend=1\nframe_generation=2\nreflex=0\n");
        std::fclose(file);
        Load();
        assert(settings.fg_backend == FgFsr);
        assert(settings.frame_generation == 2);
        assert(settings.reflex == ReflexOff);
    }

    // 6. Environment variable overrides
    {
#ifdef _WIN32
        _putenv_s("BB_FG_BACKEND", "fsr");
        _putenv_s("BB_FRAME_GENERATION", "2x");
        _putenv_s("BB_REFLEX", "boost");
#else
        setenv("BB_FG_BACKEND", "fsr", 1);
        setenv("BB_FRAME_GENERATION", "2x", 1);
        setenv("BB_REFLEX", "boost", 1);
#endif
        FILE* file = std::fopen(path.string().c_str(), "w");
        assert(file);
        std::fprintf(file, "fg_backend=dlss\nframe_generation=off\nreflex=off\n");
        std::fclose(file);
        Load();
        assert(settings.fg_backend == FgFsr);
        assert(settings.frame_generation == 2);
        assert(settings.reflex == ReflexBoost);

#ifdef _WIN32
        _putenv_s("BB_FG_BACKEND", "");
        _putenv_s("BB_FRAME_GENERATION", "");
        _putenv_s("BB_REFLEX", "");
#else
        unsetenv("BB_FG_BACKEND");
        unsetenv("BB_FRAME_GENERATION");
        unsetenv("BB_REFLEX");
#endif
    }

    // 7. Reflex runtime capability guarding
    {
        settings.reflex = ReflexOn;
        ConfigureReflexSupport(false);
        assert(settings.reflex == ReflexOff);
        assert(settings.reflex_supported == false);

        ConfigureReflexSupport(true);
        assert(settings.reflex_supported == true);
    }

    // 8. FrameGenNeedsRestart checks
    {
        FILE* file = std::fopen(path.string().c_str(), "w");
        assert(file);
        std::fprintf(file, "fg_backend=dlss\nframe_generation=off\nreflex=on\n");
        std::fclose(file);
        Load();
        // Since startup_* was set to 0, dlss, on during init, settings should match startup
        settings.frame_generation = settings.startup_frame_generation;
        settings.fg_backend = settings.startup_fg_backend;
        settings.reflex = settings.startup_reflex;
        assert(!FrameGenNeedsRestart());

        settings.frame_generation = 2;
        assert(FrameGenNeedsRestart());
        settings.frame_generation = settings.startup_frame_generation;
        assert(!FrameGenNeedsRestart());

        settings.fg_backend = FgFsr;
        assert(FrameGenNeedsRestart());
        settings.fg_backend = settings.startup_fg_backend;
        assert(!FrameGenNeedsRestart());

        settings.reflex = ReflexBoost;
        assert(FrameGenNeedsRestart());
        settings.reflex = settings.startup_reflex;
        assert(!FrameGenNeedsRestart());
    }

    // 9. Verify source_fps and display_fps atomics exist and can be updated by renderer/presenter
    {
        settings.source_fps = 48.5f;
        settings.display_fps = 96.2f;
        assert(settings.source_fps.load() > 48.0f);
        assert(settings.display_fps.load() > 96.0f);
    }

    std::filesystem::remove(path);
    std::puts("FG and Reflex settings: load/save, clamping, switching, and env overrides PASS");
    return 0;
}

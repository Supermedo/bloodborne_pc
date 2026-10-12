// SPDX-License-Identifier: GPL-2.0-or-later
// bbport_settings.cpp: the menu saves its keys into bbport.ini and keeps the launcher's (controls).
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include "test_platform.h"
#include "gpu/shim/bbport_settings.h"

static std::string Read(const char* path) {
    std::ifstream file(path);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

int main() {
    char path[4096];
    const int fd = bb_test_temp(path, sizeof(path), "bbport-settings-test");
    assert(fd >= 0);
    const char ini[] = "# launcher\nupscaler=fsr3\nkey.cross=X, Space\npad.circle=a\nshow_fps=0\n"
                       "fullscreen_hint=1\n";
    assert(write(fd, ini, sizeof(ini) - 1) == ssize_t(sizeof(ini) - 1));
    close(fd);
    setenv("BB_CONFIG", path, 1);
    unsetenv("BB_DLSS_PRESET");

    BbSettings::Load();
    auto& s = BbSettings::Get();
    assert(s.upscaler == BbSettings::UpscalerFsr3 && !s.show_fps && s.menu_x < 0.0f);
    s.show_fps = true;
    s.upscaler = BbSettings::UpscalerFsr411;
    s.frame_generation = BbSettings::FrameGenerationDlss;
    s.menu_x = 0.625f;
    s.menu_y = 0.125f;
    BbSettings::Save();

    const std::string saved = Read(path);
    // The launcher's controls, its other keys and comments stay; the menu's keys are replaced
    // in place, new ones appended.
    assert(saved.find("# launcher\nupscaler=fsr411\nkey.cross=X, Space\npad.circle=a\nshow_fps=1\n"
                      "fullscreen_hint=1\n") == 0);
    assert(saved.find("menu_pos=0.6250,0.1250\n") != std::string::npos);
    assert(saved.find("upscaler=fsr3") == std::string::npos);
    assert(saved.find("frame_generation=dlss\n") != std::string::npos);

    s.menu_x = -1.0f;
    s.menu_y = -1.0f;
    BbSettings::Load();
    assert(s.menu_x == 0.625f && s.menu_y == 0.125f && s.upscaler == BbSettings::UpscalerFsr411);
    assert(s.frame_generation == BbSettings::FrameGenerationDlss);
    const int quality = s.preset;
    for (int preset : BbSettings::DlssPresets) {
        s.dlss_preset = preset;
        BbSettings::Save();
        assert(Read(path).find(std::string("dlss_preset=") + BbSettings::DlssPresetName(preset) +
                               "\n") != std::string::npos);
        s.dlss_preset = -1;
        BbSettings::Load();
        assert(s.dlss_preset == preset && s.preset == quality);
    }
    setenv("BB_DLSS_PRESET", " k ", 1);
    BbSettings::Load();
    assert(s.dlss_preset == 11);
    setenv("BB_DLSS_PRESET", "13", 1);
    BbSettings::Load();
    assert(s.dlss_preset == 13);
    setenv("BB_DLSS_PRESET", "7", 1); // Reserved NGX hints must never be submitted.
    BbSettings::Load();
    assert(s.dlss_preset == 0);
    setenv("BB_DLSS_PRESET", "invalid", 1);
    BbSettings::Load();
    assert(s.dlss_preset == 0);
    unsetenv("BB_DLSS_PRESET");
    s.mouse_camera = true;
    s.mouse_invert_y = true;
    s.mouse_sensitivity = 125.5f;
    BbSettings::Save();
    s.mouse_camera = s.mouse_invert_y = false;
    s.mouse_sensitivity = 100.f;
    BbSettings::Load();
    assert(s.mouse_camera && s.mouse_invert_y && s.mouse_sensitivity == 125.5f);
    for (const auto& [text, expected] : {std::pair{"nan", 100.f}, {"inf", 100.f},
                                       {"bad", 100.f}, {"0", 1.f}, {"9999", 400.f}}) {
        setenv("BB_MOUSE_SENSITIVITY", text, 1);
        BbSettings::Load();
        assert(s.mouse_sensitivity == expected);
    }
    unsetenv("BB_MOUSE_SENSITIVITY");
    unlink(path);
    std::puts("PASS: settings persistence, DLSS model presets and environment validation");
}

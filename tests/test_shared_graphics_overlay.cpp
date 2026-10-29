// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise the actual overlay widgets with Dear ImGui's navigation activation.
// Including the implementation keeps internal menu helpers out of the public API.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include "imgui_internal.h"
#define runtime_restart overlay_test_restart
#include "../gpu/shim/bbport_overlay.cpp"
#undef runtime_restart
#include <windows.h>

static int restarts = 0;
// Keep the restart observable while linking the renderer's actual runtime dependencies.
extern "C" void overlay_test_restart() { ++restarts; }

static std::string Frame(ImGuiID activate = 0, ImGuiWindow* target = nullptr) {
    ImGui::GetIO().DeltaTime = 1.0f / 60;
    ImGui::NewFrame();
    auto& g = *ImGui::GetCurrentContext();
    if (activate) {
        g.NavWindow = target;
        g.NavInputSource = ImGuiInputSource_Keyboard;
        g.NavActivateId = g.NavActivateDownId = g.NavActivatePressedId = activate;
        g.NavActivateFlags = ImGuiActivateFlags_None;
    }
    ImGui::LogToBuffer();
    BbOverlay::Menu();
    const std::string text = g.LogBuffer.c_str();
    ImGui::LogFinish();
    ImGui::Render();
    return text;
}

int main() {
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    wchar_t temporary[MAX_PATH]{};
    assert(GetTempFileNameW(std::filesystem::temp_directory_path().c_str(), L"bbo", 0, temporary));
    const auto path = std::filesystem::path(temporary).string();
    _putenv_s("BB_CONFIG", path.c_str());
    _putenv_s("BB_SHARED_GRAPHICS_CONFIG", "1");
    std::ofstream(path) << "upscaler=fsr411\nobject_motion=1\nfsr411_backend=vk\nobject_motion_cache=0\nfsr_compatibility_options=1\n";
    BbSettings::Load();
    auto& s = BbSettings::Get();
    s.fsr411_supported = s.fsr4_supported = true;
    for (auto& version : s.fsr41_backend_supported)
        for (auto& supported : version) supported = true;
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {3840, 2160};
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    assert(pixels && width > 0 && height > 0);
    BbOverlay::SetOpen(true);
    Frame();
    auto text = Frame();
    assert(text.find("FSR 4.1 Settings") != std::string::npos);
    assert(text.find("Selected: FSR 4.1.1 - Vulkan (fsr4vk)") != std::string::npos);
    assert(text.find("Advanced FSR options") == std::string::npos);
    assert(text.find("FSR 4.1 backend") == std::string::npos);
    ImGuiWindow* window = ImGui::FindWindowByName("Bloodborne - Settings (Insert / L3+R3)");
    assert(window);
    const ImGuiID settings = window->GetID("FSR 4.1 Settings##FSR41SettingsButton");
    const ImGuiID cache = window->GetID("Motion-vector cache (experimental)");
    auto& g = *ImGui::GetCurrentContext();
    for (int other : {BbSettings::UpscalerFsr3, BbSettings::UpscalerTaa, BbSettings::UpscalerOff}) {
        s.upscaler = other;
        Frame(settings, window);
        assert(g.OpenPopupStack.empty());
    }
    s.upscaler = BbSettings::UpscalerFsr411;
    auto dialog = [&]() {
        Frame(settings, window);
        Frame();
        assert(!g.OpenPopupStack.empty() && g.OpenPopupStack.back().Window);
        return g.OpenPopupStack.back().Window;
    };
    auto* popup = dialog();
    text = Frame();
    assert(text.find("FSR 4.1.2") != std::string::npos && text.find("FSR 4.1.1") != std::string::npos);
    assert(text.find("(i)") != std::string::npos);
    for (const auto& route : BbOverlay::Fsr41Routes) {
        const std::string label=std::string(route.method)+"###"+route.title;
        Frame(ImHashStr(label.c_str(), 0, popup->GetID("FSR41Methods")), popup);
        assert(s.upscaler == route.version && s.fsr411_backend == route.backend);
        assert(!BbSettings::FsrSettingsNeedRestart());
        BbSettings::Save();
        BbSettings::Load();
        assert(s.upscaler == route.version && s.fsr411_backend == route.backend);
        assert(s.ActiveFsr411Backend() == route.backend);
    }
    // Hardware, software and file statuses are distinct; unavailable choices
    // cannot overwrite the user's saved working version/method.
    s.fsr41_backend_supported[1][0] = false;
    s.fsr41_status[1][0] = 1;
    s.fsr41_backend_supported[1][1] = false;
    s.fsr41_status[1][1] = 3;
    s.fsr41_backend_supported[0][0] = false;
    s.fsr41_status[0][0] = 4;
    Frame(ImHashStr("AMD Vulkan###FSR 4.1.2 - AMD Vulkan", 0, popup->GetID("FSR41Methods")), popup);
    assert(s.upscaler == BbSettings::UpscalerFsr411 && s.fsr411_backend == BbSettings::Fsr411Vulkan);
    text = Frame();
    assert(text.find("GPU not supported") != std::string::npos);
    assert(text.find("Driver support missing") != std::string::npos);
    assert(text.find("Files missing") != std::string::npos);
    // Help on an unavailable method remains keyboard-accessible and read-only.
    const auto& unavailable = BbOverlay::Fsr41Routes[0];
    g.NavWindow = popup;
    g.NavId = ImHashStr("(i)", 0, ImHashStr(unavailable.title, 0, popup->GetID("FSR41Methods")));
    text = Frame();
    assert(text.find(Fsr41Ui::Text(unavailable.hint)) != std::string::npos);
    assert(s.upscaler == BbSettings::UpscalerFsr411 && s.fsr411_backend == BbSettings::Fsr411Vulkan);
    g.NavId = 0;
    Frame(popup->GetID("Close"), popup);
    // All 13 languages use the same generated catalog as the launcher.
    for (int language = 0; language < 13; ++language) {
        _putenv_s("BB_UI_LANGUAGE", Fsr41Ui::languages[language]);
        const std::string button = std::string(Fsr41Ui::Text(Fsr41Ui::Settings))+"##FSR41SettingsButton";
        Frame(window->GetID(button.c_str()), window);
        Frame();
        popup = g.OpenPopupStack.back().Window;
        text = Frame();
        assert(text.find(Fsr41Ui::Text(Fsr41Ui::Files)) != std::string::npos);
        assert(text.find(Fsr41Ui::Text(Fsr41Ui::Driver)) != std::string::npos);
        Frame(popup->GetID(Fsr41Ui::Text(Fsr41Ui::Close)), popup);
    }
    _putenv_s("BB_UI_LANGUAGE", "en");
    for (auto& version : s.fsr41_backend_supported) for (auto& support : version) support = true;
    s.upscaler = BbSettings::UpscalerFsr3;
    Frame(window->GetID("Upscaler"), window);
    Frame();
    popup = g.OpenPopupStack.back().Window;
    Frame(popup->GetID("FSR 4.1 (experimental)"), popup);
    assert(s.upscaler == BbSettings::UpscalerFsr411 && s.fsr411_backend == BbSettings::Fsr411Vulkan);
    // A fresh remembered preference uses the primary Vulkan provider.
    s.upscaler = BbSettings::UpscalerFsr3;
    s.fsr41_version = BbSettings::UpscalerFsr412;
    BbSettings::Save(); BbSettings::Load();
    assert(s.fsr41_version == BbSettings::UpscalerFsr412);
    Frame(window->GetID("Upscaler"), window); Frame();
    popup = g.OpenPopupStack.back().Window;
    Frame(popup->GetID("FSR 4.1 (experimental)"), popup);
    assert(s.upscaler == BbSettings::UpscalerFsr412 && s.fsr411_backend == BbSettings::Fsr411Vulkan);
    Frame(cache, window);
    assert(s.object_motion_cache);
    assert(BbSettings::FsrSettingsNeedRestart());
    text = Frame();
    assert(text.find("Apply and restart") != std::string::npos);
    Frame(window->GetID("Apply and restart"), window);
    assert(restarts == 1);
    BbSettings::Load();
    assert(s.object_motion_cache && s.upscaler == BbSettings::UpscalerFsr412);
    assert(!BbSettings::FsrSettingsNeedRestart());
    // Saved ML-FG selection remains visible while the session runs its FSR 3.1 fallback.
    BbSettings::SelectFrameGen(BbSettings::FrameGenFsr401);
    BbSettings::FrameGenFailure(BbSettings::FrameGenFsr401, "ML-FG file missing: test.dll");
    text = Frame();
    assert(text.find("ML-FG file missing: test.dll") != std::string::npos);
    assert(text.find("FSR 3.1 FG active") != std::string::npos);
    assert(s.frame_gen == BbSettings::FrameGenFsr401);
    s.frame_gen_hdr = true;
    text = Frame();
    assert(text.find("ML-FG paused: HDR output.") != std::string::npos);
    assert(text.find("FSR 3.1 FG active") == std::string::npos);
    BbSettings::Save();
    BbSettings::Load();
    assert(s.frame_gen == BbSettings::FrameGenFsr401 && !s.frame_gen_hdr);
    BbOverlay::SetOpen(false);
    ImGui::DestroyContext();
    std::remove(path.c_str());
    std::puts("Overlay widgets: method selection, cache gating, save and restart activation PASS");
}

// SPDX-License-Identifier: GPL-2.0-or-later
// Compile the production overlay here to exercise its private widgets and SDL input
// without adding test-only entry points to the game. No Vulkan rendering is needed.
#include "../gpu/shim/bbport_overlay.cpp"
#include "imgui_internal.h"
#include "test_assert.h"
#include "test_platform.h"
#include "sdl_window.h"
#include "mouse_camera_fixture.h"
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unistd.h>

struct Item {
    ImRect rect;
    ImGuiWindow* window = nullptr;
    bool disabled = false;
    std::string label;
};
static std::unordered_map<ImGuiID, Item> items;
void ImGuiTestEngineHook_ItemAdd(ImGuiContext* ctx, ImGuiID id, const ImRect& rect,
                               const ImGuiLastItemData* data) {
    auto& item = items[id];
    item.rect = rect;
    item.window = ctx->CurrentWindow;
    item.disabled = data && (data->ItemFlags & ImGuiItemFlags_Disabled);
}
void ImGuiTestEngineHook_ItemInfo(ImGuiContext*, ImGuiID id, const char* label,
                                ImGuiItemStatusFlags) {
    items[id].label = label;
}
void ImGuiTestEngineHook_Log(ImGuiContext*, const char*, ...) {}
const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*, ImGuiID id) {
    const auto it = items.find(id);
    return it == items.end() ? nullptr : it->second.label.c_str();
}

static ImGuiWindow* MenuWindow() {
    return ImGui::FindWindowByID(ImHashStr("###bbport_settings"));
}

static Item Find(const char* label) {
    for (const auto& [id, item] : items) {
        if (item.label == label) return item;
    }
    // BeginCombo registers its ID/rectangle, but no ItemInfo label.
    if (auto* window = MenuWindow()) {
        const auto it = items.find(ImHashStr(label, 0, window->ID));
        if (it != items.end()) return it->second;
    }
    std::fprintf(stderr, "Missing menu item: %s\n", label);
    std::abort();
}

static ImRect checkbox_rect, slider_rect;
static std::atomic<bool> checked{false};
static std::atomic<float> amount{0.2f};
static ImVec2 display_size(1280, 720);

static void Frame(bool menu = false) {
    auto& io = ImGui::GetIO();
    io.DisplaySize = display_size;
    io.DeltaTime = 1.f / 60;
    items.clear();
    ImGui::NewFrame();
    if (menu) {
        BbOverlay::Menu();
    } else {
        ImGui::SetNextWindowPos(ImVec2(20, 20));
        ImGui::SetNextWindowSize(ImVec2(500, 180));
        ImGui::Begin("Input regression", nullptr, ImGuiWindowFlags_NoResize);
        BbOverlay::Checkbox("Checkbox", checked);
        checkbox_rect = GImGui->LastItemData.Rect;
        BbOverlay::Slider("Slider", amount, 0, 1);
        slider_rect = GImGui->LastItemData.Rect;
        ImGui::End();
    }
    ImGui::Render();
}

static void Move(ImVec2 at) {
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.x = at.x;
    event.motion.y = at.y;
    assert(BbOverlay::HandleEvent(event));
}

static void Button(bool down, ImVec2 at) {
    SDL_Event event{};
    event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.x = at.x;
    event.button.y = at.y;
    assert(BbOverlay::HandleEvent(event));
}

static void Click(ImVec2 at, bool menu = false, bool motion = true) {
    if (motion) Move(at);
    Frame(menu);
    Button(true, at);
    Frame(menu);
    Button(false, at);
    Frame(menu);
}

static void ClickItem(const char* label) {
    Item item = Find(label);
    if (item.window && !item.window->InnerRect.Contains(item.rect.GetCenter())) {
        ImGui::SetScrollY(item.window, item.window->Scroll.y + item.rect.GetCenter().y -
                                         item.window->InnerRect.GetCenter().y);
        Frame(true);
        Frame(true);
        item = Find(label);
    }
    assert(item.window->InnerRect.Contains(item.rect.GetCenter()));
    Click(ImVec2(item.rect.Min.x + 12, item.rect.GetCenter().y), true);
}

static void Select(const char* label, const char* choice) {
    ClickItem(label);
    Frame(true);
    assert(!Find(choice).disabled);
    ClickItem(choice);
    Frame(true);
}

static void Key(SDL_Keycode key, bool down, SDL_Keymod mod = SDL_KMOD_NONE) {
    SDL_Event event{};
    event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    event.key.key = key;
    event.key.mod = mod;
    assert(BbOverlay::HandleEvent(event));
}

static void ToastLayout() {
    auto& io = ImGui::GetIO();
    const ImVec2 previous_size = display_size;
    const float previous_scale = BbOverlay::base_scale;
    const ImGuiStyle previous_style = ImGui::GetStyle();
    for (const ImVec2 size : {ImVec2(320, 180), ImVec2(640, 360),
                             ImVec2(1280, 720), ImVec2(2560, 1440)}) {
        const float scale = std::max(size.y / 1080.f, .75f);
        ImGui::GetStyle() = previous_style;
        ImGui::GetStyle().ScaleAllSizes(scale / previous_scale);
        ImGui::GetStyle().FontScaleMain = scale;
        BbOverlay::base_scale = scale;
        for (int frame = 0; frame < 3; ++frame) {
            io.DisplaySize = size;
            io.DeltaTime = 1.f / 60;
            ImGui::NewFrame();
            // Check a nonzero work-area origin as well as ordinary resolutions.
            auto* viewport = ImGui::GetMainViewport();
            viewport->WorkPos = ImVec2(10, 10);
            viewport->WorkSize = ImVec2(size.x - 20, size.y - 20);
            BbOverlay::MouseCameraToast();
            ImGui::Render();
            const auto* toast = ImGui::FindWindowByName("##mouse_camera_toast");
            if (toast && (toast->Pos.x + toast->Size.x > size.x - 9.f ||
                          toast->Pos.y + toast->Size.y > size.y - 9.f))
                std::fprintf(stderr, "Toast %.0fx%.0f frame %d: pos %.0f,%.0f size %.0f,%.0f work %.0f,%.0f %.0f,%.0f\n",
                    size.x, size.y, frame, toast->Pos.x, toast->Pos.y,
                    toast->Size.x, toast->Size.y, viewport->WorkPos.x, viewport->WorkPos.y,
                    viewport->WorkSize.x, viewport->WorkSize.y);
            assert(toast && toast->Size.x > 250.f);
            assert(toast->Size.x < size.x && toast->Size.y < size.y - 20.f);
            assert(toast->Pos.x >= viewport->WorkPos.x);
            assert(toast->Pos.y >= viewport->WorkPos.y);
            assert(toast->Pos.x + toast->Size.x <= size.x - 10.f + 1.f);
            assert(toast->Pos.y + toast->Size.y <= size.y - 10.f + 1.f);
            assert((toast->Flags & ImGuiWindowFlags_NoInputs) == ImGuiWindowFlags_NoInputs);
        }
    }
    ImGui::GetStyle() = previous_style;
    BbOverlay::base_scale = previous_scale;
    display_size = previous_size;
}

int main() {
    char path[4096];
    const int fd = bb_test_temp(path, sizeof(path), "bbport-overlay-test");
    assert(fd >= 0);
    close(fd);
    setenv("BB_CONFIG", path, 1);
    BbSettings::Load();
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    GImGui->TestEngineHookItems = true;
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    ImFontConfig font_config;
    font_config.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(bb_font_ttf),
                                  int(bb_font_ttf_end - bb_font_ttf), 18.f, &font_config);
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    BbOverlay::initialized = true;
    Key(SDLK_INSERT, true);
    assert(BbOverlay::MenuOpen() && BbOverlay::CapturesInput());
    Key(SDLK_INSERT, false);
    Frame();
    Frame();

    Click(checkbox_rect.GetCenter());
    assert(checked && BbOverlay::dirty);
    Click(checkbox_rect.GetCenter());
    assert(!checked);
    Click(ImVec2(slider_rect.Min.x + 150, slider_rect.GetCenter().y));
    assert(amount > 0.4f);
    // Changes must survive the next frame, rather than only changing the drawn widget.
    Frame();
    assert(amount > 0.4f);

    // SDL button events also carry a position: no preceding motion is required.
    Move(ImVec2(1000, 650));
    Frame();
    Click(checkbox_rect.GetCenter(), false, false);
    assert(checked);

    // Ctrl+click exact entry uses SDL characters and keyboard shortcuts.
    Move(ImVec2(slider_rect.Min.x + 150, slider_rect.GetCenter().y));
    Frame();
    Key(SDLK_LCTRL, true, SDL_KMOD_CTRL);
    Click(ImVec2(slider_rect.Min.x + 150, slider_rect.GetCenter().y));
    Key(SDLK_A, true, SDL_KMOD_CTRL);
    Frame();
    Key(SDLK_A, false, SDL_KMOD_CTRL);
    Key(SDLK_LCTRL, false);
    SDL_Event typed{};
    typed.type = SDL_EVENT_TEXT_INPUT;
    typed.text.text = "0.75";
    assert(BbOverlay::HandleEvent(typed));
    Frame();
    Key(SDLK_RETURN, true);
    Frame();
    Key(SDLK_RETURN, false);
    Frame();
    assert(std::abs(amount.load() - 0.75f) < 0.001f);

#ifdef _WIN32
    // A verified synthetic image enables the actual camera controls in this menu test.
    auto* camera_image = static_cast<unsigned char*>(VirtualAlloc(nullptr, 0x5540000,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    assert(camera_image);
    CameraFixture::Populate(camera_image);
    camera_image[0x143c984] ^= 1;
    assert(!BbMouse::Install(camera_image, 0x5540000));
    ToastLayout(); // the actual long failure message, on its first visible frame
    camera_image[0x143c984] ^= 1;
    assert(BbMouse::Install(camera_image, 0x5540000));
#endif

    auto& s = BbSettings::Get();
    s.menu_language = BbSettings::MenuLanguage::English;
    BbOverlay::mouse_toast_enabled = true;
    ToastLayout();
    BbOverlay::mouse_toast_enabled = false;
    ToastLayout();
    s.menu_language = BbSettings::MenuLanguage::Russian;
    ToastLayout();
    s.menu_language = BbSettings::MenuLanguage::English;
    s.dlss_supported = s.fsr4_supported = s.fsr411_supported = true;
    s.fsr_fg_supported = s.dlss_fg_supported = true;
    Frame(true);
    Frame(true);
    Frame(true);
    ClickItem("Sharpening (RCAS)");
    assert(!s.sharpen);
    ClickItem("Sharpening (RCAS)");
    assert(s.sharpen);
    ClickItem("Subpixel jitter");
    assert(!s.jitter);
    ClickItem("Enable mask");
    assert(s.reactive);
    const float previous_threshold = s.reactive_threshold;
    ClickItem("Threshold");
    assert(s.reactive_threshold != previous_threshold);
    Select("Preset", "Performance (x2.0, render 960x540)");
    assert(s.preset == BbSettings::Performance);
    Select("Upscaler", "DLSS (NVIDIA RTX)");
    assert(s.upscaler == BbSettings::UpscalerDlss);
    assert(Find("Enable mask").disabled);
    Select("DLSS model preset", "K (Transformer)");
    assert(s.dlss_preset == 11);
    Select("Frame generation", "FSR 3.1.6 x2");
    assert(s.frame_generation == BbSettings::FrameGenerationFsr);
    Select("Output resolution", "2560 x 1440");
    assert(s.output_res == 2);
    Select("Model detail", "Highest (-2)");
    assert(s.model_lod == -2);
    for (int e = 0; e < BbSettings::EffectCount; ++e) {
        const bool old = s.effects[e];
        ClickItem(BbSettings::Effects[e].label);
        if (s.effects[e] == old) {
            const auto item = Find(BbSettings::Effects[e].label);
            std::fprintf(stderr, "Unchanged effect %s: rect %.0f,%.0f..%.0f,%.0f disabled=%d "
                                 "mouse %.0f,%.0f hovered=%x active=%x\n",
                         BbSettings::Effects[e].label, item.rect.Min.x, item.rect.Min.y,
                         item.rect.Max.x, item.rect.Max.y, item.disabled,
                         io.MousePos.x, io.MousePos.y, GImGui->HoveredId, GImGui->ActiveId);
        }
        assert(s.effects[e] != old);
    }
    assert(!Find("Apply and restart game").disabled);
    Select("Upscaler", "Off");
    assert(s.upscaler == BbSettings::UpscalerOff);
    assert(Find("Sharpening (RCAS)").disabled);
    ClickItem("Sharpening (RCAS)");
    assert(s.sharpen);
    Select("Upscaler", "FSR 3.1");
    assert(!Find("Sharpening (RCAS)").disabled);
    assert(!Find("Enable mask").disabled);
#ifdef _WIN32
    ClickItem("Mouse camera (F4)");
    assert(s.mouse_camera);
    ClickItem("Mouse sensitivity (%)");
    assert(s.mouse_sensitivity != 100.f);
    ClickItem("Invert mouse Y");
    assert(s.mouse_invert_y);
#endif

    // Escape dismisses an open dropdown before closing the whole menu.
    ClickItem("Upscaler");
    Frame(true);
    Key(SDLK_ESCAPE, true);
    Frame(true);
    Key(SDLK_ESCAPE, false);
    Frame(true);
    assert(BbOverlay::MenuOpen());
    assert(!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup));

    // Restored positions and a smaller game window cannot hide the lower controls.
    Key(SDLK_INSERT, true);
    s.menu_x = s.menu_y = 0.99f;
    Key(SDLK_INSERT, true);
    for (const ImVec2 size : {ImVec2(1280, 720), ImVec2(640, 360), ImVec2(2560, 1440)}) {
        display_size = size;
        Frame(true);
        Frame(true);
        const auto* window = MenuWindow();
        assert(window->Pos.x >= 0 && window->Pos.y >= 0);
        assert(window->Pos.x + window->Size.x <= size.x);
        assert(window->Pos.y + window->Size.y <= size.y);
        ClickItem("FPS counter in corner");
    }
    display_size = ImVec2(1280, 720);
    Frame(true);
    Frame(true);
    auto* window = MenuWindow();
    ImGui::SetScrollY(window, window->ScrollMax.y);
    Move(window->InnerRect.GetCenter());
    Frame(true);
    Frame(true);
    const float scroll = window->Scroll.y;
    SDL_Event wheel{};
    wheel.type = SDL_EVENT_MOUSE_WHEEL;
    wheel.wheel.y = 2;
    assert(BbOverlay::HandleEvent(wheel));
    Frame(true);
    Frame(true);
    assert(window->Scroll.y < scroll);
    ClickItem("Close");
    assert(!BbOverlay::MenuOpen());
    Key(SDLK_INSERT, true);

    // Losing focus and closing during a press must not leave a stuck mouse button.
    Button(true, checkbox_rect.GetCenter());
    Frame();
    SDL_Event focus{};
    focus.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    assert(!BbOverlay::HandleEvent(focus));
    Frame();
    assert(!io.MouseDown[0]);
    focus.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
    assert(!BbOverlay::HandleEvent(focus));
    Button(true, checkbox_rect.GetCenter());
    Frame();
    Key(SDLK_INSERT, true);
    Key(SDLK_INSERT, true);
    Frame();
    assert(!io.MouseDown[0]);

#ifdef _WIN32
    // Also exercise the real Win32 SDL window event pump, not only HandleEvent.
    setenv("BB_HIDDEN_WINDOW", "1", 1);
    {
        Frontend::WindowSDL window(1280, 720, "Overlay input regression");
        assert(window.PollEvents());
        Frame();
        Frame();
        const bool old = checked;
        const SDL_WindowID id = SDL_GetWindowID(window.GetSDLWindow());
        const float density = BbOverlay::PixelDensity(id);
        const ImVec2 at = checkbox_rect.GetCenter();
        SDL_Event event{};
        event.type = SDL_EVENT_MOUSE_MOTION;
        event.motion.windowID = id;
        event.motion.x = at.x / density;
        event.motion.y = at.y / density;
        assert(SDL_PushEvent(&event));
        assert(SDL_WaitEventTimeout(nullptr, 8)); // waiting must not consume the mouse event
        assert(window.PollEvents());
        Frame();
        event = {};
        event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        event.button.windowID = id;
        event.button.button = SDL_BUTTON_LEFT;
        event.button.x = at.x / density;
        event.button.y = at.y / density;
        assert(SDL_PushEvent(&event));
        assert(window.PollEvents());
        Frame();
        event.type = SDL_EVENT_MOUSE_BUTTON_UP;
        assert(SDL_PushEvent(&event));
        assert(window.PollEvents());
        Frame();
        assert(checked != old);

        Key(SDLK_INSERT, true); // F4 is available during gameplay, not menu value entry
        event = {};
        event.type = SDL_EVENT_KEY_DOWN;
        event.key.windowID = id;
        event.key.key = SDLK_F4;
        assert(SDL_PushEvent(&event)); assert(window.PollEvents());
        assert(!s.mouse_camera);
        event.key.mod = SDL_KMOD_ALT;
        assert(SDL_PushEvent(&event)); assert(window.PollEvents());
        assert(!s.mouse_camera); // Alt+F4 must not toggle the camera
        event.key.mod = SDL_KMOD_NONE;
        assert(SDL_PushEvent(&event)); assert(window.PollEvents());
        assert(s.mouse_camera);
        Key(SDLK_INSERT, true);
    }
    unsetenv("BB_HIDDEN_WINDOW");
#endif

    Key(SDLK_INSERT, true);
    assert(!BbOverlay::MenuOpen() && !BbOverlay::CapturesInput());
    std::ifstream file(path);
    std::stringstream saved;
    saved << file.rdbuf();
    assert(saved.str().find("dlss_preset=K\n") != std::string::npos);
    assert(saved.str().find("output_res=2560x1440\n") != std::string::npos);
#ifdef _WIN32
    assert(saved.str().find("mouse_camera=1\n") != std::string::npos);
    assert(saved.str().find("mouse_invert_y=1\n") != std::string::npos);
    VirtualFree(camera_image, 0, MEM_RELEASE);
#endif
    file.close();
    ImGui::DestroyContext();
    BbOverlay::initialized = false;
    unlink(path);
    std::puts("PASS: SDL mouse controls, exact value entry, full graphics menu, scrolling, "
              "resize, persistence, input reset and bounded F4 notifications");
}

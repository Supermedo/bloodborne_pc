// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_native_mod.h"
#include "pad_mailbox.h"

#include "../atlas2/texture_budget.h"
#include "../native/gpu/shim/bbport_atlas_menu.h"
#include "../native/gpu/shim/bbport_atlas_paths.h"
#include "../native/gpu/shim/bbport_map.h"
#include "../native/gpu/shim/bbport_map_art.h"
#include "../native/gpu/shim/bbport_map_static.h"

#include "imgui.h"
#include "backends/imgui_impl_vulkan.h"

#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

void BbRuntimeBridgeSetHost(const BbNativeHostV1* host);

namespace {
const BbNativeHostV1* host{};
std::mutex state_mutex;
std::atomic<bool> initialized{false}, visible{false}, capture{false}, release_mouse{false};
std::chrono::steady_clock::time_point last_frame;
float base_scale = 1.0f;
bool settings_open = false;
AtlasInput::PadMailbox pad_mailbox;
std::vector<AtlasInput::PadSample> pad_batch;
uint32_t last_routed_buttons = 0;

// Called only while state_mutex is held. The pad sampling thread only waits
// for the mailbox's short copy/swap operation, never for this render work.
void DrainPad() {
    pad_mailbox.Take(pad_batch);
    for (const auto& sample : pad_batch) {
        last_routed_buttons = settings_open ? 0 : BbAtlasMenu::RoutePad(
            sample.buttons, sample.lx, sample.ly, sample.rx, sample.ry);
    }
    pad_batch.clear();
}

void Log(uint32_t level, const char* text) {
    if (host && host->log) host->log(level, "hunters-atlas", text);
}

uint32_t SdlType(uint32_t kind) {
    switch (kind) {
    case BB_NATIVE_EVENT_KEY_DOWN: return SDL_EVENT_KEY_DOWN;
    case BB_NATIVE_EVENT_KEY_UP: return SDL_EVENT_KEY_UP;
    case BB_NATIVE_EVENT_TEXT_INPUT: return SDL_EVENT_TEXT_INPUT;
    case BB_NATIVE_EVENT_MOUSE_MOTION: return SDL_EVENT_MOUSE_MOTION;
    case BB_NATIVE_EVENT_MOUSE_BUTTON_DOWN: return SDL_EVENT_MOUSE_BUTTON_DOWN;
    case BB_NATIVE_EVENT_MOUSE_BUTTON_UP: return SDL_EVENT_MOUSE_BUTTON_UP;
    case BB_NATIVE_EVENT_MOUSE_WHEEL: return SDL_EVENT_MOUSE_WHEEL;
    case BB_NATIVE_EVENT_WINDOW_FOCUS_GAINED: return SDL_EVENT_WINDOW_FOCUS_GAINED;
    case BB_NATIVE_EVENT_WINDOW_FOCUS_LOST: return SDL_EVENT_WINDOW_FOCUS_LOST;
    case BB_NATIVE_EVENT_GAMEPAD_BUTTON_DOWN: return SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    case BB_NATIVE_EVENT_GAMEPAD_BUTTON_UP: return SDL_EVENT_GAMEPAD_BUTTON_UP;
    case BB_NATIVE_EVENT_GAMEPAD_AXIS: return SDL_EVENT_GAMEPAD_AXIS_MOTION;
    case BB_NATIVE_EVENT_GAMEPAD_ADDED: return SDL_EVENT_GAMEPAD_ADDED;
    case BB_NATIVE_EVENT_GAMEPAD_REMOVED: return SDL_EVENT_GAMEPAD_REMOVED;
    default: return 0;
    }
}

SDL_Event ToSdl(const BbNativeEventV1& value) {
    SDL_Event event{};
    event.type = SdlType(value.kind);
    event.common.timestamp = value.timestamp_ns;
    switch (value.kind) {
    case BB_NATIVE_EVENT_KEY_DOWN:
    case BB_NATIVE_EVENT_KEY_UP:
        event.key.windowID = value.window_id;
        event.key.key = static_cast<SDL_Keycode>(value.key);
        event.key.scancode = static_cast<SDL_Scancode>(value.scancode);
        event.key.mod = static_cast<SDL_Keymod>(value.modifiers);
        event.key.repeat = value.repeat != 0;
        event.key.down = value.kind == BB_NATIVE_EVENT_KEY_DOWN;
        break;
    case BB_NATIVE_EVENT_TEXT_INPUT:
        event.text.windowID = value.window_id;
        // SDL3 stores a pointer here. The event envelope outlives this call
        // (HandleEvent feeds ImGui before returning), so the pointer remains
        // valid for the only consumer of the translated event.
        event.text.text = value.text;
        break;
    case BB_NATIVE_EVENT_MOUSE_MOTION:
        event.motion.windowID = value.window_id; event.motion.which = value.device_id;
        event.motion.x = value.x; event.motion.y = value.y;
        event.motion.xrel = value.xrel; event.motion.yrel = value.yrel;
        break;
    case BB_NATIVE_EVENT_MOUSE_BUTTON_DOWN:
    case BB_NATIVE_EVENT_MOUSE_BUTTON_UP:
        event.button.windowID = value.window_id; event.button.which = value.device_id;
        event.button.button = static_cast<Uint8>(value.button);
        event.button.x = value.x; event.button.y = value.y;
        event.button.down = value.kind == BB_NATIVE_EVENT_MOUSE_BUTTON_DOWN;
        break;
    case BB_NATIVE_EVENT_MOUSE_WHEEL:
        event.wheel.windowID = value.window_id; event.wheel.which = value.device_id;
        event.wheel.x = value.wheel_x; event.wheel.y = value.wheel_y;
        break;
    case BB_NATIVE_EVENT_GAMEPAD_BUTTON_DOWN:
    case BB_NATIVE_EVENT_GAMEPAD_BUTTON_UP:
        event.gbutton.which = value.device_id;
        event.gbutton.button = static_cast<Uint8>(value.button);
        event.gbutton.down = value.kind == BB_NATIVE_EVENT_GAMEPAD_BUTTON_DOWN;
        break;
    case BB_NATIVE_EVENT_GAMEPAD_AXIS:
        event.gaxis.which = value.device_id; event.gaxis.axis = static_cast<Uint8>(value.axis);
        event.gaxis.value = static_cast<Sint16>(value.axis_value);
        break;
    case BB_NATIVE_EVENT_GAMEPAD_ADDED:
    case BB_NATIVE_EVENT_GAMEPAD_REMOVED:
        event.gdevice.which = value.device_id;
        break;
    default: break;
    }
    return event;
}

void RefreshState() {
    const bool is_visible = BbMap::IsOpen() || BbAtlasMenu::Visible() || settings_open;
    visible.store(is_visible, std::memory_order_release);
    // The game's quick menu remains interactive while Atlas contributes its
    // Map tile. Capturing input merely because that menu is open traps the
    // player's controls in the host's neutral-input gate, so only the Atlas
    // map/settings surfaces own game input.
    const bool owns_input = BbMap::CapturesInput() || settings_open;
    capture.store(owns_input, std::memory_order_release);
    // The map and the quick-menu tile need the pointer; the game needs the
    // captured (relative) mouse while they are closed.
    release_mouse.store(BbMap::IsOpen() || settings_open || BbAtlasMenu::Visible(),
                        std::memory_order_release);
}

void FeedImGui(const SDL_Event& event) {
    ImGuiIO& io = ImGui::GetIO();
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        io.AddKeyEvent(ImGuiMod_Ctrl, (event.key.mod & SDL_KMOD_CTRL) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (event.key.mod & SDL_KMOD_SHIFT) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (event.key.mod & SDL_KMOD_ALT) != 0);
        break;
    case SDL_EVENT_TEXT_INPUT: io.AddInputCharactersUTF8(event.text.text); break;
    case SDL_EVENT_MOUSE_MOTION: {
        SDL_Window* window = SDL_GetWindowFromID(event.motion.windowID);
        const float density = window ? SDL_GetWindowPixelDensity(window) : 1.0f;
        io.AddMousePosEvent(event.motion.x * density, event.motion.y * density);
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        const int button = event.button.button == SDL_BUTTON_LEFT ? 0 :
                           event.button.button == SDL_BUTTON_RIGHT ? 1 :
                           event.button.button == SDL_BUTTON_MIDDLE ? 2 : -1;
        if (button >= 0) io.AddMouseButtonEvent(button, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL: io.AddMouseWheelEvent(event.wheel.x, event.wheel.y); break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED: io.AddFocusEvent(true); break;
    case SDL_EVENT_WINDOW_FOCUS_LOST: io.AddFocusEvent(false); break;
    default: break;
    }
}

int Initialize(const BbNativeGraphicsV1* graphics) {
    std::scoped_lock lock{state_mutex};
    if (!graphics || graphics->struct_size < sizeof(*graphics) || !graphics->instance ||
        !graphics->physical_device || !graphics->device || !graphics->queue) return 0;
    if (const char* enabled = std::getenv("BB_ATLAS_ENABLED"); !enabled || std::strcmp(enabled, "1")) {
        Log(BB_NATIVE_LOG_ERROR, "manifest did not enable Atlas"); return 0;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.BackendPlatformName = "bbport-native-mod";
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f; style.FrameRounding = 4.0f; style.GrabRounding = 4.0f;

    const auto map_font = BbAtlasPath("BB_MAP_FONT",
        BbAtlasPath("BB_MAP_DIR", "navigation-map/maps").parent_path() / "fonts/bloodborne-native.ttf");
    if (std::filesystem::is_regular_file(map_font)) {
        if (auto* font = io.Fonts->AddFontFromFileTTF(map_font.string().c_str(), 24.0f))
            BbMap::SetTitleFont(font);
    }

    HMODULE vulkan = GetModuleHandleW(L"vulkan-1.dll");
    auto get_instance_proc = vulkan ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        GetProcAddress(vulkan, "vkGetInstanceProcAddr")) : nullptr;
    struct Loader { PFN_vkGetInstanceProcAddr get; VkInstance instance; } loader{
        get_instance_proc, reinterpret_cast<VkInstance>(graphics->instance)};
    if (!get_instance_proc || !ImGui_ImplVulkan_LoadFunctions(graphics->vulkan_api_version,
        [](const char* name, void* opaque) -> PFN_vkVoidFunction {
            auto& state = *static_cast<Loader*>(opaque);
            return state.get(state.instance, name);
        }, &loader)) {
        Log(BB_NATIVE_LOG_ERROR, "could not load Vulkan functions");
        ImGui::DestroyContext(); return 0;
    }
    const VkFormat color_format = static_cast<VkFormat>(graphics->color_format);
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = graphics->vulkan_api_version;
    info.Instance = reinterpret_cast<VkInstance>(graphics->instance);
    info.PhysicalDevice = reinterpret_cast<VkPhysicalDevice>(graphics->physical_device);
    info.Device = reinterpret_cast<VkDevice>(graphics->device);
    info.QueueFamily = graphics->queue_family;
    info.Queue = reinterpret_cast<VkQueue>(graphics->queue);
    info.DescriptorPoolSize = Atlas2::RendererImageDescriptors;
    info.MinImageCount = std::max(graphics->image_count, 2u);
    info.ImageCount = std::max(graphics->image_count, 2u);
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color_format,
    };
    if (!ImGui_ImplVulkan_Init(&info)) {
        Log(BB_NATIVE_LOG_ERROR, "ImGui Vulkan backend initialization failed");
        ImGui::DestroyContext(); return 0;
    }
    BbMapArt::Init();
    BbMap::Init();
    BbMap::Close();
    BbAtlasMenu::Reset();
    pad_mailbox.Clear();
    pad_batch.reserve(32);
    last_routed_buttons = 0;
    settings_open = false;
    last_frame = std::chrono::steady_clock::now();
    initialized.store(true, std::memory_order_release);
    RefreshState();
    Log(BB_NATIVE_LOG_INFO, "Atlas 3.1 renderer initialized");
    return 1;
}

void Shutdown() {
    std::scoped_lock lock{state_mutex};
    if (!initialized.exchange(false)) return;
    pad_mailbox.Clear();
    pad_batch.clear();
    BbMap::Close();
    BbAtlasMenu::Reset();
    settings_open = false;
    BbMapStatic::Shutdown();
    ImGui_ImplVulkan_Shutdown();
    BbMapArt::Shutdown();
    ImGui::DestroyContext();
    visible = capture = release_mouse = false;
}

void Tick(int host_overlay_open) {
    if (!initialized.load(std::memory_order_acquire)) return;
    std::scoped_lock lock{state_mutex};
    BbAtlasMenu::Suppress(host_overlay_open != 0);
    if (host_overlay_open) {
        pad_mailbox.Clear();
        if (BbMap::IsOpen()) BbMap::Close();
        settings_open = false;
    } else {
        DrainPad();
        BbMap::PollTestCommands();
        BbMap::Tick();
        BbAtlasMenu::Tick();
    }
    RefreshState();
}

int Render(const BbNativeRenderV1* render) {
    if (!render || render->struct_size < sizeof(*render) || !render->command_buffer ||
        !render->image_view || !render->width || !render->height) return 0;
    std::scoped_lock lock{state_mutex};
    if (!initialized.load()) return 1;
    DrainPad();
    RefreshState();
    if (!visible.load()) return 1;
    const auto now = std::chrono::steady_clock::now();
    float delta = std::chrono::duration<float>(now - last_frame).count();
    last_frame = now;
    if (!(delta > 0.0f && delta < 1.0f)) delta = 1.0f / 60.0f;
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(render->width), float(render->height));
    io.DeltaTime = delta;
    const float scale = std::max(float(render->height) / 1080.0f, 0.75f);
    if (std::abs(scale - base_scale) > 0.01f) {
        ImGui::GetStyle().ScaleAllSizes(scale / base_scale);
        ImGui::GetStyle().FontScaleMain = scale;
        base_scale = scale;
    }
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    if (BbMap::IsOpen()) BbMap::Draw();
    if (!BbMap::IsOpen()) BbAtlasMenu::Draw();
    if (settings_open) {
        ImGui::SetNextWindowSize(ImVec2(520 * scale, 0), ImGuiCond_Appearing);
        if (ImGui::Begin("Hunter's Atlas settings", &settings_open,
                         ImGuiWindowFlags_AlwaysAutoResize)) BbMap::Settings();
        ImGui::End();
    }
    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(),
        reinterpret_cast<VkCommandBuffer>(render->command_buffer));
    RefreshState();
    return 1;
}

int HandleEvent(const BbNativeEventV1* value) {
    if (!value || value->struct_size < sizeof(*value) || !initialized.load()) return 0;
    std::scoped_lock lock{state_mutex};
    SDL_Event event = ToSdl(*value);
    if (!event.type) return 0;
    if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST || event.type == SDL_EVENT_GAMEPAD_REMOVED) {
        // Queued presses from the old focus/device cannot reopen the map.
        pad_mailbox.Clear();
        settings_open = false;
    }
    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
        event.key.key == SDLK_F1 && BbMap::IsOpen()) {
        settings_open = !settings_open; RefreshState(); return 1;
    }
    FeedImGui(event);
    if (BbAtlasMenu::HandleMouse(event)) { RefreshState(); return 1; }
    const bool consumed = BbMap::HandleEvent(event);
    RefreshState();
    return consumed ? 1 : 0;
}

uint32_t RoutePad(uint32_t buttons, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry) {
    if (!initialized.load()) return buttons;
    pad_mailbox.Push({buttons, lx, ly, rx, ry});
    if (!state_mutex.try_lock()) return capture.load() ? 0 : buttons;
    std::unique_lock lock{state_mutex, std::adopt_lock};
    if (!initialized.load()) return buttons;
    DrainPad();
    RefreshState();
    return last_routed_buttons;
}

const BbNativeModV1 module_api{
    sizeof(BbNativeModV1), BBPORT_NATIVE_MOD_API_V1,
    "hunters-atlas", "Hunter's Atlas",
    Initialize, Shutdown, Tick,
    [] { return visible.load(std::memory_order_acquire) ? 1 : 0; },
    Render, HandleEvent, RoutePad,
    [] { return capture.load(std::memory_order_acquire) ? 1 : 0; },
    [] { return release_mouse.load(std::memory_order_acquire) ? 1 : 0; },
};
} // namespace

extern "C" BBPORT_NATIVE_MOD_EXPORT const BbNativeModV1* bbport_native_mod_query(
    uint32_t host_api, const BbNativeHostV1* value) {
    if (host_api != BBPORT_NATIVE_MOD_API_V1 || !value ||
        value->struct_size < sizeof(BbNativeHostV1) ||
        value->api_version != BBPORT_NATIVE_MOD_API_V1) return nullptr;
    host = value;
    BbRuntimeBridgeSetHost(value);
    return &module_api;
}

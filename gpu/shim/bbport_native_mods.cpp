// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_native_mods.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>

#include "../../sdk/bbport_native_mod.h"
#include "../../src/runtime_atlas_state.h"
#include "../../src/runtime_map.h"
#include "../../src/runtime_menu_sound.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace BbNativeMods {
namespace {

struct LoadedMod {
#ifdef _WIN32
    HMODULE library{};
#endif
    const BbNativeModV1* api{};
    std::string configured_id;
};

std::vector<LoadedMod> mods;
std::atomic<bool> ready{false};

void HostLog(uint32_t level, const char* mod_id, const char* message) {
    const char* kind = level == BB_NATIVE_LOG_ERROR ? "error" :
                       level == BB_NATIVE_LOG_WARNING ? "warning" : "info";
    std::printf("Native mod%s%s: %s: %s\n", mod_id ? " " : "", mod_id ? mod_id : "",
                kind, message ? message : "");
}

int HostLocation(BbNativeLocationV1* out) {
    if (!out) return 0;
    BbMapLocation value{};
    if (!runtime_map_location(&value)) return 0;
    *out = {value.x, value.y, value.z, value.yaw, value.block, value.valid};
    return 1;
}
int HostMenuOpen() { return runtime_map_menu_open(); }
int HostMenuState(BbNativeMenuV1* out) {
    if (!out) return 0;
    BbMapMenu value{};
    if (!runtime_map_menu_state(&value)) return 0;
    *out = {value.supported, value.top_level, value.selected};
    return 1;
}
uint32_t HostConfirmButton() { return runtime_map_confirm_button(); }
int HostEventFlag(uint32_t id) { return runtime_atlas_flag(id); }
uintptr_t HostPlayerToken() { return runtime_atlas_player_token(); }
int HostTravelRequest(const BbNativeTravelDestinationV1* value) {
    if (!value) return 0;
    BbAtlasTravelDestination destination{};
    destination.return_point = value->return_point;
    destination.unlock_flag = value->unlock_flag;
    std::copy(std::begin(value->on), std::end(value->on), std::begin(destination.on));
    std::copy(std::begin(value->off), std::end(value->off), std::begin(destination.off));
    destination.on_count = value->on_count;
    destination.off_count = value->off_count;
    std::copy(std::begin(value->arrival), std::end(value->arrival), std::begin(destination.arrival));
    return runtime_atlas_travel_request(&destination);
}
int HostTravelStatus() { return runtime_atlas_travel_status(); }
int HostTravelSupported() { return runtime_atlas_travel_supported(); }
void HostMenuAudioEvent(int select) { runtime_audio_menu_event(select); }
void HostMenuAudioStats(BbNativeMenuSoundStatsV1* out) {
    if (!out) return;
    BbMenuSoundStats value{};
    runtime_audio_menu_stats(&value);
    *out = {value.decoded_move, value.decoded_select, value.queued_move,
            value.queued_select, value.mixed_move, value.mixed_select, value.muted};
}

const BbNativeHostV1 host_api{
    sizeof(BbNativeHostV1), BBPORT_NATIVE_MOD_API_V1, HostLog, HostLocation,
    HostMenuOpen, HostMenuState, HostConfirmButton, HostEventFlag, HostPlayerToken,
    HostTravelRequest, HostTravelStatus, HostTravelSupported, HostMenuAudioEvent,
    HostMenuAudioStats,
};

#ifdef _WIN32
std::wstring Utf16(const std::filesystem::path& path) { return path.wstring(); }

void LoadConfigured(const BbNativeGraphicsV1& graphics) {
    const char* file_name = std::getenv("BB_NATIVE_MODS_FILE");
    if (!file_name || !*file_name) return;
    try {
        std::ifstream file(std::filesystem::u8path(file_name));
        if (!file) throw std::runtime_error("cannot open BB_NATIVE_MODS_FILE");
        nlohmann::json config;
        file >> config;
        if (!config.is_object() || config.value("schema", 0) != 1 ||
            !config.contains("mods") || !config["mods"].is_array())
            throw std::runtime_error("invalid native-mod launch file");
        for (const auto& item : config["mods"]) {
            const std::string id = item.at("id").get<std::string>();
            const auto path = std::filesystem::u8path(item.at("module").get<std::string>());
            if (id.empty() || !path.is_absolute() || !std::filesystem::is_regular_file(path)) {
                HostLog(BB_NATIVE_LOG_ERROR, id.c_str(), "module path is missing or not absolute");
                continue;
            }
            HMODULE library = LoadLibraryExW(Utf16(path).c_str(), nullptr,
                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            if (!library) {
                HostLog(BB_NATIVE_LOG_ERROR, id.c_str(), "LoadLibraryEx failed");
                continue;
            }
            auto query = reinterpret_cast<BbNativeModQueryV1>(
                GetProcAddress(library, "bbport_native_mod_query"));
            const BbNativeModV1* api = query ? query(BBPORT_NATIVE_MOD_API_V1, &host_api) : nullptr;
            if (!api || api->struct_size < sizeof(BbNativeModV1) ||
                api->api_version != BBPORT_NATIVE_MOD_API_V1 || !api->id || id != api->id ||
                !api->initialize || !api->tick || !api->visible || !api->render ||
                !api->handle_event || !api->route_pad || !api->captures_input) {
                HostLog(BB_NATIVE_LOG_ERROR, id.c_str(), "module does not implement native-mod ABI v1");
                FreeLibrary(library);
                continue;
            }
            if (!api->initialize(&graphics)) {
                HostLog(BB_NATIVE_LOG_ERROR, id.c_str(), "graphics initialization failed");
                FreeLibrary(library);
                continue;
            }
            mods.push_back({library, api, id});
            HostLog(BB_NATIVE_LOG_INFO, id.c_str(), "loaded");
        }
    } catch (const std::exception& error) {
        HostLog(BB_NATIVE_LOG_ERROR, nullptr, error.what());
    }
}
#else
void LoadConfigured(const BbNativeGraphicsV1&) {
    if (std::getenv("BB_NATIVE_MODS_FILE"))
        HostLog(BB_NATIVE_LOG_WARNING, nullptr, "native modules are currently supported on Windows only");
}
#endif

uint32_t EventKind(uint32_t type) {
    switch (type) {
    case SDL_EVENT_KEY_DOWN: return BB_NATIVE_EVENT_KEY_DOWN;
    case SDL_EVENT_KEY_UP: return BB_NATIVE_EVENT_KEY_UP;
    case SDL_EVENT_TEXT_INPUT: return BB_NATIVE_EVENT_TEXT_INPUT;
    case SDL_EVENT_MOUSE_MOTION: return BB_NATIVE_EVENT_MOUSE_MOTION;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: return BB_NATIVE_EVENT_MOUSE_BUTTON_DOWN;
    case SDL_EVENT_MOUSE_BUTTON_UP: return BB_NATIVE_EVENT_MOUSE_BUTTON_UP;
    case SDL_EVENT_MOUSE_WHEEL: return BB_NATIVE_EVENT_MOUSE_WHEEL;
    case SDL_EVENT_WINDOW_FOCUS_GAINED: return BB_NATIVE_EVENT_WINDOW_FOCUS_GAINED;
    case SDL_EVENT_WINDOW_FOCUS_LOST: return BB_NATIVE_EVENT_WINDOW_FOCUS_LOST;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: return BB_NATIVE_EVENT_GAMEPAD_BUTTON_DOWN;
    case SDL_EVENT_GAMEPAD_BUTTON_UP: return BB_NATIVE_EVENT_GAMEPAD_BUTTON_UP;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: return BB_NATIVE_EVENT_GAMEPAD_AXIS;
    case SDL_EVENT_GAMEPAD_ADDED: return BB_NATIVE_EVENT_GAMEPAD_ADDED;
    case SDL_EVENT_GAMEPAD_REMOVED: return BB_NATIVE_EVENT_GAMEPAD_REMOVED;
    default: return BB_NATIVE_EVENT_NONE;
    }
}

BbNativeEventV1 Translate(const SDL_Event& event) {
    BbNativeEventV1 out{};
    out.struct_size = sizeof(out);
    out.kind = EventKind(event.type);
    out.timestamp_ns = event.common.timestamp;
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        out.window_id = event.key.windowID;
        out.key = static_cast<int32_t>(event.key.key);
        out.scancode = static_cast<int32_t>(event.key.scancode);
        out.repeat = event.key.repeat;
        out.modifiers = static_cast<uint32_t>(event.key.mod);
        break;
    case SDL_EVENT_TEXT_INPUT:
        out.window_id = event.text.windowID;
        std::strncpy(out.text, event.text.text, sizeof(out.text) - 1);
        break;
    case SDL_EVENT_MOUSE_MOTION:
        out.window_id = event.motion.windowID; out.device_id = event.motion.which;
        out.x = event.motion.x; out.y = event.motion.y;
        out.xrel = event.motion.xrel; out.yrel = event.motion.yrel;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        out.window_id = event.button.windowID; out.device_id = event.button.which;
        out.button = event.button.button; out.x = event.button.x; out.y = event.button.y;
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        out.window_id = event.wheel.windowID; out.device_id = event.wheel.which;
        out.wheel_x = event.wheel.x; out.wheel_y = event.wheel.y;
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        out.device_id = event.gbutton.which; out.button = event.gbutton.button;
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        out.device_id = event.gaxis.which; out.axis = event.gaxis.axis;
        out.axis_value = event.gaxis.value;
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED:
        out.device_id = event.gdevice.which;
        break;
    default: break;
    }
    return out;
}

} // namespace

void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count) {
    BbNativeGraphicsV1 graphics{
        sizeof(BbNativeGraphicsV1), instance.ApiVersion(),
        reinterpret_cast<uintptr_t>(VkInstance(instance.GetInstance())),
        reinterpret_cast<uintptr_t>(VkPhysicalDevice(instance.GetPhysicalDevice())),
        reinterpret_cast<uintptr_t>(VkDevice(instance.GetDevice())),
        reinterpret_cast<uintptr_t>(VkQueue(instance.GetGraphicsQueue())),
        instance.GetGraphicsQueueFamilyIndex(), static_cast<uint32_t>(format), image_count, 0,
    };
    LoadConfigured(graphics);
    ready.store(true, std::memory_order_release);
}

bool HandleEvent(const SDL_Event& event) {
    if (!ready.load(std::memory_order_acquire)) return false;
    const auto translated = Translate(event);
    if (translated.kind == BB_NATIVE_EVENT_NONE) return false;
    for (auto& mod : mods) if (mod.api->handle_event(&translated)) return true;
    return false;
}

bool TickAndVisible(bool host_overlay_open) {
    if (!ready.load(std::memory_order_acquire)) return false;
    bool visible = false;
    for (auto& mod : mods) {
        mod.api->tick(host_overlay_open ? 1 : 0);
        visible = mod.api->visible() || visible;
    }
    return visible;
}

void Render(vk::CommandBuffer command_buffer, vk::ImageView image_view, vk::Extent2D extent) {
    if (!ready.load(std::memory_order_acquire)) return;
    const BbNativeRenderV1 render{sizeof(BbNativeRenderV1), extent.width, extent.height,
        reinterpret_cast<uintptr_t>(static_cast<VkCommandBuffer>(command_buffer)),
        reinterpret_cast<uintptr_t>(static_cast<VkImageView>(image_view))};
    bool any_visible = false;
    for (const auto& mod : mods) any_visible = mod.api->visible() || any_visible;
    if (!any_visible) return;
    const vk::RenderingAttachmentInfo attachment{
        .imageView = image_view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    command_buffer.beginRendering(vk::RenderingInfo{
        .renderArea = {{0, 0}, extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    });
    {
        std::scoped_lock submit_lock{Vulkan::Scheduler::submit_mutex};
        for (auto& mod : mods) if (mod.api->visible() && !mod.api->render(&render))
            HostLog(BB_NATIVE_LOG_WARNING, mod.configured_id.c_str(), "render call failed");
    }
    command_buffer.endRendering();
}

u32 RoutePad(u32 buttons, u8 lx, u8 ly, u8 rx, u8 ry) {
    if (!ready.load(std::memory_order_acquire)) return buttons;
    for (auto& mod : mods) buttons = mod.api->route_pad(buttons, lx, ly, rx, ry);
    return buttons;
}
bool CapturesInput() {
    if (!ready.load(std::memory_order_acquire)) return false;
    for (const auto& mod : mods) if (mod.api->captures_input()) return true;
    return false;
}
bool ReleasesMouse() {
    if (!ready.load(std::memory_order_acquire)) return false;
    for (const auto& mod : mods)
        if (mod.api->releases_mouse && mod.api->releases_mouse()) return true;
    return false;
}
} // namespace BbNativeMods

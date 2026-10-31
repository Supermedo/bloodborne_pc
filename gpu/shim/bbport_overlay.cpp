// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_overlay.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <string>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <cwchar>
#include <thread>
#include <vector>
#endif

#include <SDL3/SDL.h>
#include "bbport_process_memory.h"
#include "bbport_settings.h"
#include "bbport_fsr41_ui.h"
#include <filesystem>
#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

// DejaVu Sans (Cyrillic), embedded (third_party/fonts, Bitstream Vera license).
#ifdef _WIN32
asm(".section .rdata,\"dr\"\n"
    ".balign 16\n"
    ".global bb_font_ttf\n"
    "bb_font_ttf:\n"
    ".incbin \"" BB_FONT_PATH "\"\n"
    ".global bb_font_ttf_end\n"
    "bb_font_ttf_end:\n"
    ".text\n");
#else
asm(".section .rodata\n"
    ".balign 16\n"
    ".hidden bb_font_ttf\n"
    ".global bb_font_ttf\n"
    "bb_font_ttf:\n"
    ".incbin \"" BB_FONT_PATH "\"\n"
    ".hidden bb_font_ttf_end\n"
    ".global bb_font_ttf_end\n"
    "bb_font_ttf_end:\n"
    ".previous\n");
#endif
extern "C" const unsigned char bb_font_ttf[];
extern "C" const unsigned char bb_font_ttf_end[];

extern "C" void runtime_restart(void); // bb-probe (probe.c)

namespace BbOverlay {

namespace {

std::mutex imgui_mutex; // the ImGui context: window thread (input) and present thread
bool initialized = false;
std::atomic<bool> menu_open{false};
bool l3_down = false, r3_down = false, back_down = false, start_down = false;

// What opens the menu (the launcher's Controls page): BB_MENU_KEY, a key's name ("Insert", the
// default, "F10", ...) or "none"; BB_MENU_PAD, "l3+r3" (the default), "back+start" or "off".
SDL_Keycode MenuKey() {
    static const SDL_Keycode key = [] {
        const char* name = std::getenv("BB_MENU_KEY");
        if (!name || !*name) return SDLK_INSERT;
        if (!SDL_strcasecmp(name, "none")) return SDLK_UNKNOWN;
        const SDL_Keycode found = SDL_GetKeyFromName(name);
        return found != SDLK_UNKNOWN ? found : SDLK_INSERT;
    }();
    return key;
}
enum class MenuPad { Off, L3R3, BackStart };
MenuPad MenuPadCombo() {
    static const MenuPad combo = [] {
        const char* name = std::getenv("BB_MENU_PAD");
        if (name && !SDL_strcasecmp(name, "off")) return MenuPad::Off;
        if (name && !SDL_strcasecmp(name, "back+start")) return MenuPad::BackStart;
        return MenuPad::L3R3;
    }();
    return combo;
}
// "Insert / L3+R3", as the window title and the log name it.
std::string MenuOpeners() {
    std::string text = MenuKey() != SDLK_UNKNOWN ? SDL_GetKeyName(MenuKey()) : "";
    const MenuPad pad = MenuPadCombo();
    if (pad != MenuPad::Off) {
        text += text.empty() ? "" : " / ";
        text += pad == MenuPad::L3R3 ? "L3+R3" : "Select+Start";
    }
    return text.empty() ? "the launcher only" : text;
}
bool dirty = false; // settings changed while open: saved on close
// The game's text dialog (SetTextEntry), guarded by imgui_mutex.
bool text_entry_active = false;
std::string text_entry_prompt, text_entry_text;
// Video memory warning (BbOverlay::Warn): text from the GPU thread, read by the present thread.
std::mutex warn_mutex;
std::string warn_headline;
std::string warn_detail;
float warn_fraction = -1.0f; ///< memory in use over the limit, or negative for no bar
std::chrono::steady_clock::time_point warn_until{};

// Video memory readout (SetVramUsage): written by the GPU thread's collector, read by the present
// thread.
std::atomic<u64> vram_used{0};
std::atomic<u64> vram_limit{0};
std::atomic<u64> vram_pressure{0};
std::atomic<u64> vram_critical{0};
std::atomic<u64> vram_card_free{0};

// CPU, GPU and RAM for the counter, sampled at a few Hz (the readings are syscalls, and none of
// the numbers moves between two frames). Negative: not known (yet, or not available).
struct SystemLoad {
    std::atomic<float> cpu{-1.0f}; ///< this process, % of the machine (all cores = 100 %)
    std::atomic<float> gpu{-1.0f}; ///< this process, % of the busiest 3D engine
    std::atomic<u64> ram{0};       ///< this process's working set
    std::atomic<u64> ram_free{0};  ///< the machine's available physical memory, 0: not known
};
// Leaked on purpose: the GPU sampler thread reads it while the process is shutting down.
SystemLoad& Load() {
    static SystemLoad* load = new SystemLoad;
    return *load;
}

#ifdef _WIN32
// This process's CPU time over wall time, as a share of the whole machine.
void SampleCpu() {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        return;
    }
    const u64 ticks = (u64(kernel.dwHighDateTime) << 32 | kernel.dwLowDateTime) +
                      (u64(user.dwHighDateTime) << 32 | user.dwLowDateTime);
    static u64 last_ticks = 0;
    static std::chrono::steady_clock::time_point last_time{};
    const auto now = std::chrono::steady_clock::now();
    if (last_ticks != 0 && now > last_time) {
        const double wall_us = std::chrono::duration<double, std::micro>(now - last_time).count();
        const double cpu_us = double(ticks - last_ticks) / 10.0; // FILETIME: 100 ns ticks
        const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
        Load().cpu.store(float(100.0 * cpu_us / (wall_us * cores)), std::memory_order_relaxed);
    }
    last_ticks = ticks;
    last_time = now;
}

// Windows' per-process GPU Engine counters (the number Task Manager shows), 3D engine only: the
// one a game loads. Opening the query and enumerating the instances is not free, so this runs on
// its own thread once a second, never on the present thread.
float QueryGpuLoad() {
    static PDH_HQUERY query{};
    static PDH_HCOUNTER counter{};
    static const bool ready = [&] {
        return PdhOpenQueryW(nullptr, 0, &query) == ERROR_SUCCESS &&
               PdhAddEnglishCounterW(query, L"\\GPU Engine(*)\\Utilization Percentage", 0,
                                     &counter) == ERROR_SUCCESS;
    }();
    if (!ready || PdhCollectQueryData(query) != ERROR_SUCCESS) {
        return -1.0f;
    }
    constexpr DWORD format = PDH_FMT_DOUBLE | PDH_FMT_NOCAP100;
    DWORD bytes = 0, count = 0;
    if (PdhGetFormattedCounterArrayW(counter, format, &bytes, &count, nullptr) != PDH_MORE_DATA) {
        return -1.0f;
    }
    static std::vector<BYTE> buffer;
    buffer.resize(bytes);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
    if (PdhGetFormattedCounterArrayW(counter, format, &bytes, &count, items) != ERROR_SUCCESS) {
        return -1.0f;
    }
    const std::wstring mine = L"pid_" + std::to_wstring(GetCurrentProcessId()) + L"_";
    double load = 0.0;
    for (DWORD i = 0; i < count; ++i) {
        if (!items[i].szName || !std::wcsstr(items[i].szName, L"engtype_3D") ||
            std::wcsncmp(items[i].szName, mine.c_str(), mine.size()) != 0) {
            continue;
        }
        load += items[i].FmtValue.doubleValue;
    }
    return float(std::min(load, 100.0));
}

// The machine's available physical memory: what Task Manager's "Available" shows (free plus
// standby). The RAM row's process figure alone reads as a leak.
u64 AvailableMemory() {
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    return GlobalMemoryStatusEx(&status) ? status.ullAvailPhys : 0;
}

void StartGpuSampler() {
    static std::once_flag once;
    std::call_once(once, [] {
        // shortcut: runs until the process exits (nothing joins it), upgrade if the overlay ever
        // needs an orderly shutdown. Idle while the readout is switched off, so it costs nothing
        // for players who never enable it.
        std::thread([] {
            for (;;) {
                if (BbSettings::Get().show_gpu) {
                    Load().gpu.store(QueryGpuLoad(), std::memory_order_relaxed);
                }
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }).detach();
    });
}
#else
void SampleCpu() {}
u64 AvailableMemory() {
    return 0;
}
void StartGpuSampler() {}
#endif

// The frame-time graph (NotePresent writes, FpsCounter reads, both on the present thread).
constexpr size_t FrameHistorySize = 240; ///< about four seconds at 60 Hz
std::array<float, FrameHistorySize> frame_history{};
size_t frame_history_count = 0;
size_t frame_history_next = 0;

// The counter's values in plain grey when the player turned its colours off (counter_colors=0).
constexpr ImVec4 PlainValue{0.80f, 0.80f, 0.80f, 1.0f};
bool PlainCounter() {
    return !BbSettings::Get().counter_colors.load(std::memory_order_relaxed);
}

// Green while the frame rate can keep up with a display, amber down to the game's 30 FPS floor,
// red below it. The numbers mean the same thing however the game is capped.
ImVec4 PaceColor(float frame_ms) {
    if (PlainCounter()) {
        return PlainValue;
    }
    if (frame_ms <= 20.0f) {
        return ImVec4(0.55f, 0.95f, 0.55f, 1.0f);
    }
    if (frame_ms <= 33.5f) {
        return ImVec4(1.00f, 0.82f, 0.45f, 1.0f);
    }
    return ImVec4(1.00f, 0.45f, 0.40f, 1.0f);
}

// A load in percent: amber from `amber`, red from `red`.
ImVec4 LoadColor(float percent, float amber, float red) {
    if (PlainCounter()) {
        return PlainValue;
    }
    if (percent >= red) {
        return ImVec4(1.00f, 0.45f, 0.40f, 1.0f);
    }
    if (percent >= amber) {
        return ImVec4(1.00f, 0.82f, 0.45f, 1.0f);
    }
    return ImVec4(0.55f, 0.95f, 0.55f, 1.0f);
}

// Video memory against the collector's own marks: the colours then mean what the collector does
// (green: room, amber: evicting, red: evicting aggressively).
ImVec4 VramColor(u64 used, u64 pressure, u64 critical) {
    if (PlainCounter()) {
        return PlainValue;
    }
    if (critical != 0 && used >= critical) {
        return ImVec4(1.00f, 0.45f, 0.40f, 1.0f);
    }
    if (pressure != 0 && used >= pressure) {
        return ImVec4(1.00f, 0.82f, 0.45f, 1.0f);
    }
    return ImVec4(0.55f, 0.95f, 0.55f, 1.0f);
}
float base_scale = 1.0f;

// Present rate for the FPS counter.
std::chrono::steady_clock::time_point last_present{};
float frame_ms_avg = 0.0f, last_present_ms = 0.0f;

void SetOpen(bool value) {
    if (menu_open.exchange(value) == value) {
        return;
    }
    ImGui::GetIO().MouseDrawCursor = value;
    if (!value && dirty) {
        dirty = false;
        BbSettings::Save();
    }
}

ImGuiKey KeyFromSdl(SDL_Keycode key) {
    switch (key) {
    case SDLK_TAB: return ImGuiKey_Tab;
    case SDLK_LEFT: return ImGuiKey_LeftArrow;
    case SDLK_RIGHT: return ImGuiKey_RightArrow;
    case SDLK_UP: return ImGuiKey_UpArrow;
    case SDLK_DOWN: return ImGuiKey_DownArrow;
    case SDLK_PAGEUP: return ImGuiKey_PageUp;
    case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
    case SDLK_HOME: return ImGuiKey_Home;
    case SDLK_END: return ImGuiKey_End;
    case SDLK_DELETE: return ImGuiKey_Delete;
    case SDLK_BACKSPACE: return ImGuiKey_Backspace;
    case SDLK_SPACE: return ImGuiKey_Space;
    case SDLK_RETURN: return ImGuiKey_Enter;
    case SDLK_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDLK_ESCAPE: return ImGuiKey_Escape;
    case SDLK_LCTRL: return ImGuiKey_LeftCtrl;
    case SDLK_RCTRL: return ImGuiKey_RightCtrl;
    case SDLK_LSHIFT: return ImGuiKey_LeftShift;
    case SDLK_RSHIFT: return ImGuiKey_RightShift;
    case SDLK_LALT: return ImGuiKey_LeftAlt;
    case SDLK_RALT: return ImGuiKey_RightAlt;
    default: return ImGuiKey_None;
    }
}

ImGuiKey KeyFromGamepad(u8 button) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return ImGuiKey_GamepadFaceDown;
    case SDL_GAMEPAD_BUTTON_EAST: return ImGuiKey_GamepadFaceRight;
    case SDL_GAMEPAD_BUTTON_WEST: return ImGuiKey_GamepadFaceLeft;
    case SDL_GAMEPAD_BUTTON_NORTH: return ImGuiKey_GamepadFaceUp;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ImGuiKey_GamepadL1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ImGuiKey_GamepadR1;
    case SDL_GAMEPAD_BUTTON_START: return ImGuiKey_GamepadStart;
    case SDL_GAMEPAD_BUTTON_BACK: return ImGuiKey_GamepadBack;
    default: return ImGuiKey_None;
    }
}

float PixelDensity(SDL_WindowID id) {
    SDL_Window* window = SDL_GetWindowFromID(id);
    const float density = window ? SDL_GetWindowPixelDensity(window) : 1.0f;
    return density > 0.0f ? density : 1.0f;
}

// Marks the settings dirty when a widget changed them.
template <typename T>
void Store(std::atomic<T>& target, T value, bool changed) {
    if (changed) {
        target = value;
        dirty = true;
    }
}

void Checkbox(const char* label, std::atomic<bool>& value) {
    bool v = value;
    const bool changed = ImGui::Checkbox(label, &v);
    Store(value, v, changed);
}

void Slider(const char* label, std::atomic<float>& value, float lo, float hi) {
    float v = value;
    const bool changed = ImGui::SliderFloat(label, &v, lo, hi, "%.2f");
    Store(value, v, changed);
}

void Hint(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

/// The presented frame rate; with frame generation adding frames also the game's own rate,
/// which is what the GPU's load (resolution, upscaler) changes. The total follows the game's
/// FPS cap times the multiplier while the GPU keeps up.
std::string FpsText() {
    const auto& s = BbSettings::Get();
    const float shown = frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f;
    char text[96];
    const int generated = s.frame_gen != BbSettings::FrameGenOff ? s.frame_gen_now.load() : 0;
    if (generated > 0 && s.game_fps > 0.0f) {
        std::snprintf(text, sizeof(text), "%.0f FPS (game %.0f, frame gen %dx)", shown,
                      s.game_fps.load(), int(shown / s.game_fps + 0.5f));
    } else {
        std::snprintf(text, sizeof(text), "%.0f FPS", shown);
    }
    return text;
}

bool IsFsr41(int upscaler) {
    return upscaler == BbSettings::UpscalerFsr411 || upscaler == BbSettings::UpscalerFsr412;
}
struct Fsr41Route { int version, backend; const char* title; const char* method; int hint; };
static constexpr Fsr41Route Fsr41Routes[] = {
    {BbSettings::UpscalerFsr412, 0, "FSR 4.1.2 - AMD Vulkan", "AMD Vulkan", Fsr41Ui::Hint412Vk},
    {BbSettings::UpscalerFsr412, 1, "FSR 4.1.2 - AMD DX12", "AMD DX12", Fsr41Ui::Hint412Dx},
    {BbSettings::UpscalerFsr411, 1, "FSR 4.1.1 - AMD DX12", "AMD DX12", Fsr41Ui::Hint411Dx},
    {BbSettings::UpscalerFsr411, 0, "FSR 4.1.1 - Vulkan (fsr4vk)", "Vulkan (fsr4vk)", Fsr41Ui::Hint411Vk},
};
bool AnyFsr41() {
    const auto& s=BbSettings::Get();
    for(const auto& route:Fsr41Routes) if(s.FsrMethodSupported(route.version,route.backend)) return true;
    return false;
}
void Fsr41Settings() {
    auto& s=BbSettings::Get();
    const float max_width=std::max(300.f, ImGui::GetMainViewport()->WorkSize.x-60.f*base_scale);
    ImGui::SetNextWindowSize(ImVec2(std::min(420.f*base_scale,max_width),0), ImGuiCond_Appearing);
    const std::string title=std::string(Fsr41Ui::Text(Fsr41Ui::Settings))+"###FSR41Settings";
    if(!ImGui::BeginPopupModal(title.c_str(),nullptr,ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoTitleBar)) return;
    ImGui::TextUnformatted(Fsr41Ui::Text(Fsr41Ui::Settings));
    ImGui::Spacing();
    if(ImGui::BeginTable("FSR41Methods",2,ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Method",ImGuiTableColumnFlags_WidthStretch,1.1f);
        ImGui::TableSetupColumn("Support",ImGuiTableColumnFlags_WidthStretch,1.f);
        for(const auto& route:Fsr41Routes) {
            if ((route.backend==0 && route.version==BbSettings::UpscalerFsr412) ||
                (route.backend==1 && route.version==BbSettings::UpscalerFsr411)) {
                ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("FSR %s",route.version==BbSettings::UpscalerFsr412 ? "4.1.2" : "4.1.1");
            }
            const bool ready=s.FsrMethodSupported(route.version,route.backend);
            const bool selected=s.upscaler==route.version && s.fsr411_backend==route.backend;
            int status=ready ? 0 : s.fsr41_status[route.version==BbSettings::UpscalerFsr412][route.backend].load();
            if(status<0 || status>8 || (!ready && status==0)) status=5;
            ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
            if(selected) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,IM_COL32(45,42,34,255));
            ImGui::BeginDisabled(!ready);
            const std::string label=std::string(route.method)+"###"+route.title;
            if(ImGui::RadioButton(label.c_str(),selected)) {
                Store(s.fsr41_version,route.version,true);
                Store(s.fsr411_backend,route.backend,true);
                Store(s.upscaler,route.version,true);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::PushID(route.title);
            ImGui::SmallButton("(i)");
            if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) || ImGui::IsItemFocused()) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(ImGui::GetFontSize()*30.f);
                ImGui::TextUnformatted(Fsr41Ui::Text(route.hint));
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
            ImGui::PopID();
            ImGui::TableSetColumnIndex(1);
            const ImVec4 color=status==0 ? ImVec4(.47f,.74f,.57f,1) :
                status==1 ? ImVec4(.88f,.47f,.44f,1) :
                status==5 || status==6 || status==8 ? ImVec4(.54f,.55f,.56f,1) : ImVec4(.85f,.64f,.25f,1);
            ImGui::PushTextWrapPos();
            ImGui::TextColored(color,"%s",Fsr41Ui::Text(Fsr41Ui::Supported+status));
            ImGui::PopTextWrapPos();
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    if(ImGui::Button(Fsr41Ui::Text(Fsr41Ui::Close))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void Menu() {
    auto& s = BbSettings::Get();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + 40.0f * base_scale,
                                   viewport->WorkPos.y + 40.0f * base_scale),
                            ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(620.0f * base_scale, 0.0f), ImGuiCond_Appearing);
    // A menu taller than the screen would leave its last row (Close) off screen: the window
    // stays within the screen (below its 40 px offset) and scrolls instead.
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(0.0f, 0.0f),
        ImVec2(FLT_MAX, std::max(viewport->WorkSize.y - 60.0f * base_scale, 200.0f * base_scale)));
    bool keep_open = true;
    static const std::string title = "Bloodborne - Settings (" + MenuOpeners() + ")###settings";
    if (!ImGui::Begin(title.c_str(), &keep_open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    ImGui::Text("%s", FpsText().c_str());

    ImGui::SeparatorText("Temporal upscaling");
    static const char* upscalers[] = {"Off", "FSR 3.1", "FSR 4 (INT8)", "FSR 4.1.1 (experimental)",
                                     "TAA (native anti-aliasing)", "DLSS (NVIDIA RTX)", "FSR 4.1.2 (experimental)"};
    static const char* later[] = {"XeSS"};
    int upscaler = s.upscaler;
    if (ImGui::BeginCombo("Upscaler", IsFsr41(upscaler) ? Fsr41Ui::Text(Fsr41Ui::Family) : upscalers[upscaler])) {
        static const int order[] = {BbSettings::UpscalerOff, BbSettings::UpscalerFsr3,
            BbSettings::UpscalerFsr4,
#ifdef _WIN32
            BbSettings::UpscalerFsr412,
#else
            BbSettings::UpscalerFsr411,
#endif
            BbSettings::UpscalerTaa, BbSettings::UpscalerDlss};
        static_assert(sizeof(order) / sizeof(*order) == BbSettings::UpscalerCount - 1);
        for (const int i : order) {
            const bool supported = i == BbSettings::UpscalerFsr4 ? s.fsr4_supported.load()
                : i == BbSettings::UpscalerFsr411 ? s.fsr411_supported.load()
                : i == BbSettings::UpscalerFsr412 ? AnyFsr41()
                : i == BbSettings::UpscalerDlss   ? s.dlss_supported.load() : true;
            ImGui::BeginDisabled(!supported);
            const bool family = i == BbSettings::UpscalerFsr412;
            if (ImGui::Selectable(family ? Fsr41Ui::Text(Fsr41Ui::Family) : upscalers[i],
                                  family ? IsFsr41(upscaler) : i == upscaler)) {
                if (family) {
                    if (!IsFsr41(upscaler)) {
                        bool restored=false;
                        if(s.FsrMethodSupported(s.fsr41_version,s.fsr411_backend)) {
                            Store(s.upscaler,s.fsr41_version.load(),true);
                            restored=true;
                        }
                        if(!restored) for (const auto& route : Fsr41Routes) {
                            if (!s.FsrMethodSupported(route.version, route.backend)) continue;
                            Store(s.fsr41_version, route.version, true);
                            Store(s.fsr411_backend, route.backend, true);
                            Store(s.upscaler, route.version, true);
                            break;
                        }
                    }
                } else Store(s.upscaler, i, true);
            }
            ImGui::EndDisabled();
            if (!supported) {
                ImGui::SameLine();
                if (family) {
                    ImGui::TextDisabled("- %s", Fsr41Ui::Text(Fsr41Ui::Unavailable));
                } else if (i == BbSettings::UpscalerFsr411) {
                    ImGui::TextDisabled("- unavailable");
                    if (const char* problem = s.fsr411_problem.load()) Hint(problem);
                } else {
                    ImGui::TextDisabled("- unsupported by this GPU");
                }
            }
        }
        for (const char* name : later) {
            ImGui::BeginDisabled();
            ImGui::Selectable(name, false);
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("- in development");
        }
        ImGui::EndCombo();
    }
#ifdef _WIN32
    const bool fsr411_selected = IsFsr41(s.upscaler);
    const std::string settings_button = std::string(Fsr41Ui::Text(Fsr41Ui::Settings))+"##FSR41SettingsButton";
    ImGui::BeginDisabled(!fsr411_selected);
    if (ImGui::Button(settings_button.c_str())) ImGui::OpenPopup("###FSR41Settings");
    ImGui::EndDisabled();
    if (fsr411_selected) {
        ImGui::SameLine();
        for (const auto& route : Fsr41Routes) if (route.version==s.upscaler && route.backend==s.fsr411_backend)
            ImGui::TextDisabled("%s: %s", Fsr41Ui::Text(Fsr41Ui::Selected), route.title);
    } else if (!AnyFsr41()) ImGui::TextDisabled("%s", Fsr41Ui::Text(Fsr41Ui::Unavailable));
    Fsr41Settings();
    if (const char* problem = s.fsr4_method_problem.load(); problem && fsr411_selected) {
        ImGui::PushTextWrapPos();
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "%s", problem);
        ImGui::PopTextWrapPos();
    }
#endif
    if (const char* problem = s.dlss_problem.load(); problem && upscaler == BbSettings::UpscalerDlss) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "DLSS: %s", problem);
    }
    if (const char* problem = s.fsr4_problem.load()) {
        ImGui::PushTextWrapPos();
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "FSR 4 unavailable: %s", problem);
        if (!BbSettings::IsFsr4(s.upscaler))
            ImGui::TextUnformatted("The selected mode is active. You can select FSR 4 again.");
        ImGui::PopTextWrapPos();
    }
    if (BbSettings::IsFsr4(s.upscaler)) {
        if (s.upscaler == BbSettings::UpscalerFsr412) {
            Hint("FSR 4.1.2 uses the original AMD Vulkan driver or AMD DX12 bridge. Both FSR 4.1.1 methods remain available.");
        } else if (s.upscaler == BbSettings::UpscalerFsr411) {
#ifdef _WIN32
            Hint("FSR 4.1.1 temporal upscaling uses the method selected above. AMD DX12 is included in the Windows package; Vulkan assets can be downloaded in the launcher. Quality presets and sharpening apply to both methods.");
#else
            Hint("FSR 4.1.1 uses the Vulkan model passes. One model covers Native through Performance, with another for Ultra Performance. See tools/fsr4cap/build_assets.sh for asset preparation.");
#endif
        } else {
            Hint("FSR 4 INT8 uses the v07 model from the AMD FidelityFX SDK. Higher quality than FSR 3.1, with more GPU work. Changing presets rebuilds the model and may pause briefly. Assets: tools/fetch_fsr4_assets.sh.");
        }
        Checkbox("FSR 4: auto exposure", s.fsr4_auto_exposure);
        Checkbox("FSR 4: invert jitter", s.fsr4_invert_jitter);
        Hint("Ghosting diagnostics: FSR 4 uses exposure to normalize color and decide when to discard history. These options apply immediately.");
    }
    const bool upscaler_on = s.upscaler != BbSettings::UpscalerOff;
    const bool taa = s.upscaler == BbSettings::UpscalerTaa;
    ImGui::BeginDisabled(!upscaler_on);
    ImGui::BeginDisabled(taa);
    int preset = taa ? BbSettings::NativeAA : s.preset.load();
    char preset_label[64];
    std::snprintf(preset_label, sizeof(preset_label), "%s (x%.1f)", BbSettings::PresetName(preset),
                  BbSettings::PresetScale(preset));
    if (ImGui::BeginCombo("Preset", preset_label)) {
        for (int i = 0; i < BbSettings::PresetCount; ++i) {
            char label[64];
            const float scale = BbSettings::PresetScale(i);
            const int output = s.output_res;
            std::snprintf(label, sizeof(label), "%s (x%.1f, render %dx%d)",
                          BbSettings::PresetName(i), scale,
                          int(std::lround(BbSettings::OutputWidths[output] / scale / 2) * 2),
                          int(std::lround(BbSettings::OutputHeights[output] / scale / 2) * 2));
            if (ImGui::Selectable(label, i == preset)) {
                Store(s.preset, i, true);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (taa) {
        ImGui::TextWrapped("TAA smooths the scene at output resolution without FSR upscaling. The saved upscaling preset returns when you select an upscaler.");
    }
    ImGui::Text("Active scene resolution: %d x %d", s.active_render_width.load(),
                s.active_render_height.load());
    if (BbSettings::FixedRenderSession()) {
        ImGui::Text("Startup preset: %s", BbSettings::PresetName(s.startup_preset));
        if (const char* automatic = std::getenv("BB_AUTO_RENDER_RES");
            automatic && automatic[0] == '1') {
            Hint("At outputs other than 1080p, the scene resolution is patched at startup. Changes to the preset or output require a restart. This saves GPU work on older GPUs. Non-16:9 outputs always use this path to keep camera and HUD geometry synchronized.");
        } else {
            Hint("BB_RENDER_RES fixes the scene resolution at startup. Remove this explicit override to allow live resolution and preset changes. Changes to aspect ratio still require a restart.");
        }
    } else {
        Hint("Native AA uses the upscaler for anti-aliasing. Other presets reduce scene resolution relative to the output. The HUD is drawn at output resolution. Preset changes apply on the next frame in a live session.");
    }
    Checkbox("Sharpening (RCAS)", s.sharpen);
    ImGui::BeginDisabled(!s.sharpen);
    Slider("Sharpness", s.sharpness, 0.0f, 2.0f);
    Hint("Up to 1 uses the upscaler sharpening pass (RCAS). Values above 1 add another RCAS pass. Ctrl+click the slider to enter an exact value.");
    ImGui::EndDisabled();
    Checkbox("Subpixel jitter", s.jitter);
    Hint("The camera shifts by a fraction of a pixel each frame so the upscaler can recover detail across multiple frames. Without jitter, it only smooths using frame history.");

    ImGui::SeparatorText("Reactive mask");
    ImGui::BeginDisabled(taa);
    Checkbox("Enable mask", s.reactive);
    Hint("Marks transparent effects such as particles and fog so the upscaler relies less on history. Reduces trails, but can increase shimmer behind those effects.");
    ImGui::BeginDisabled(!s.reactive);
    Slider("Scale", s.reactive_scale, 0.0f, 4.0f);
    Slider("Threshold", s.reactive_threshold, 0.0f, 1.0f);
    Slider("Maximum", s.reactive_max, 0.0f, 1.0f);
    bool show_mask = s.debug_view == BbSettings::DebugReactive;
    if (ImGui::Checkbox("Show mask (debug)", &show_mask)) {
        s.debug_view = show_mask ? BbSettings::DebugReactive : BbSettings::DebugNone;
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    // Off on this GPU whatever the setting (vk_object_motion.cpp): greyed out, with the reason.
    const char* motion_unavailable = s.object_motion_unavailable.load();
    ImGui::BeginDisabled(motion_unavailable != nullptr);
    Checkbox("Object motion vectors", s.object_motion);
    ImGui::EndDisabled();
    if (motion_unavailable) {
        ImGui::TextDisabled("%s", motion_unavailable);
    }
    Hint("Accurate vectors for animated objects reduce breakup on clothing and weapons during movement. Static geometry receives no extra pass. Changes require a restart.");
#ifdef _WIN32
    const bool cache_available = !motion_unavailable && s.object_motion &&
        (s.upscaler == BbSettings::UpscalerFsr3 || BbSettings::IsFsr4(s.upscaler));
    ImGui::BeginDisabled(!cache_available);
    Checkbox(Fsr41Ui::Text(Fsr41Ui::MotionCacheLabel), s.object_motion_cache);
    ImGui::EndDisabled();
    Hint(Fsr41Ui::Text(Fsr41Ui::MotionCacheHelp));
    if (cache_available && BbSettings::FsrSettingsNeedRestart()) {
        const bool active = s.startup_object_motion_cache && s.startup_object_motion &&
            (s.startup_upscaler == BbSettings::UpscalerFsr3 || BbSettings::IsFsr4(s.startup_upscaler));
        ImGui::TextDisabled("Startup cache mode: %s; changes apply after restart", active ? "On" : "Off");
    }
#endif
    bool show_motion = s.debug_view == BbSettings::DebugMotion;
    if (ImGui::Checkbox("Show motion vectors (debug)", &show_motion)) {
        s.debug_view = show_motion ? BbSettings::DebugMotion : BbSettings::DebugNone;
    }
    Hint("Red/green: horizontal/vertical motion (8 pixels = full brightness). Blue: the pixel has accurate object motion, beyond camera motion. A moving object with no blue or red/green is treated as stationary and may leave trails.");
    ImGui::EndDisabled(); // upscaler off

    ImGui::SeparatorText("Frame generation");
    static const char* frame_gens[] = {"Off", "FSR 3.1 (any GPU)", "DLSS (NVIDIA RTX 40/50)",
        "AMD ML-FG 4.0.1 (RX 9000, 2x SDR, experimental)",
        "AMD ML-FG 4.0.2 (RX 9000, 2x SDR, experimental)"};
    static_assert(sizeof(frame_gens) / sizeof(*frame_gens) == BbSettings::FrameGenCount);
    const int frame_gen = s.frame_gen;
    if (ImGui::BeginCombo("Frame generation", frame_gens[frame_gen])) {
        for (int i = 0; i < BbSettings::FrameGenCount; ++i) {
            const bool supported = (i != BbSettings::FrameGenDlss || s.dlssg_max_multiplier > 1) &&
                (!BbSettings::IsMlFrameGen(i) || s.MlFgSupported(i));
            ImGui::BeginDisabled(!supported);
            if (ImGui::Selectable(frame_gens[i], i == frame_gen)) {
                BbSettings::SelectFrameGen(i);
                dirty = true;
            }
            ImGui::EndDisabled();
            if (!supported) {
                ImGui::SameLine();
                ImGui::TextDisabled(BbSettings::IsMlFrameGen(i) ? "- unavailable" : "- unsupported by this GPU");
                if (BbSettings::IsMlFrameGen(i))
                    if (const char* problem = s.MlFgProblem(i)) Hint(problem);
            }
        }
        ImGui::EndCombo();
    }
    Hint("Generates frames between two of the game's frames: 60 FPS on screen becomes 120. Needs a display with that refresh rate (120 Hz or more at 60 FPS); it pauses when the display cannot show the extra frames, and 3x/4x drop to what it can. Adds a little input latency. Menus and movies are not generated.");
    const int max_multiplier =
        s.frame_gen == BbSettings::FrameGenDlss ? s.dlssg_max_multiplier.load() : 2;
    ImGui::BeginDisabled(s.frame_gen == BbSettings::FrameGenOff || max_multiplier <= 2);
    int multiplier = std::min(s.frame_gen_multiplier.load(), max_multiplier);
    if (ImGui::SliderInt("Multiplier", &multiplier, 2, std::max(max_multiplier, 2), "%dx")) {
        Store(s.frame_gen_multiplier, multiplier, true);
    }
    ImGui::EndDisabled();
    ImGui::BeginDisabled(s.frame_gen == BbSettings::FrameGenOff);
    Checkbox("Fit to display refresh", s.frame_gen_fit_display);
    ImGui::EndDisabled();
    Hint("On: no more frames than the display shows (an uncapped game gets fewer generated frames, and the GPU does less). Off: the multiplier as chosen; past the refresh rate the display shows the newest frames.");
    // What runs now (fitted: at most the frames the display shows between two of the game's).
    if (s.ActiveFrameGen() != BbSettings::FrameGenOff && !s.frame_gen_hdr && s.frame_gen_display_hz > 0) {
        const int now = std::min(s.frame_gen_now.load(), multiplier - 1);
        if (now > 0 && s.frame_gen_flip_limit > 0) {
            ImGui::TextDisabled("Now %dx on a %d Hz display; V-Sync (FIFO) caps the game at %d FPS",
                                now + 1, s.frame_gen_display_hz.load(),
                                s.frame_gen_flip_limit.load());
        } else if (now > 0) {
            ImGui::TextDisabled("Now %dx on a %d Hz display", now + 1,
                                s.frame_gen_display_hz.load());
        } else if (s.frame_gen_fit_display) {
            ImGui::TextDisabled("Paused: no room on a %d Hz display at this frame rate",
                                s.frame_gen_display_hz.load());
        } else {
            ImGui::TextDisabled("Paused: presenting fell behind (V-Sync on a slower display?)");
        }
    }
    if (BbSettings::IsMlFrameGen(s.frame_gen)) {
        if (const char* problem = s.MlFgProblem(s.frame_gen)) {
            ImGui::PushTextWrapPos();
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "%s", problem);
            ImGui::PopTextWrapPos();
        }
        if (s.frame_gen_hdr)
            ImGui::TextDisabled("ML-FG paused: HDR output.");
    }
    if (const char* problem = s.frame_gen_problem.load(); problem && !s.frame_gen_hdr) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "%s", problem);
    }

    ImGui::SeparatorText("Output resolution");
    static const char* outputs[] = {"1280 x 720", "1920 x 1080", "2560 x 1440", "3840 x 2160",
                                    "2560 x 1080 (21:9)", "3440 x 1440 (21:9)",
                                    "3840 x 1600 (21:9)", "5120 x 2160 (21:9)",
                                    "3840 x 1080 (32:9)", "5120 x 1440 (32:9)"};
    static_assert(sizeof(outputs) / sizeof(*outputs) == BbSettings::OutputCount);
    int output = s.output_res;
    if (ImGui::BeginCombo("Output resolution", outputs[output])) {
        for (int i = 0; i < BbSettings::OutputCount; ++i) {
            if (ImGui::Selectable(outputs[i], i == output)) {
                Store(s.output_res, i, true);
            }
        }
        ImGui::EndCombo();
    }
    if (BbSettings::Ultrawide(s.output_res)) {
        int hud = s.hud_position;
        Store(s.hud_position, hud,
              ImGui::SliderInt("HUD position", &hud, 0, 100, hud == 0 ? "Centred (16:9)" :
                                                         hud == 100 ? "Screen edges" : "%d%%"));
        Hint("Ultrawide (21:9, 32:9): moves the health, items and Blood Echoes from the centred 16:9 area toward the screen edges. Changes require a restart.");
    }
    if (BbSettings::FixedRenderSession()) {
        Hint("Final image and HUD resolution. The preset controls scene resolution relative to the output. 4K Performance is approximately 1920x1080. Changes require a restart.");
    } else {
        Hint("Output and HUD resolution changes at the next frame boundary. The preset controls scene resolution relative to the output. Resizing resets upscaler history and may pause briefly.");
    }
    static const char* live_modes[] = {"Auto (based on GPU)", "Off (faster)", "On"};
    int live = s.live_resolution + 1;
    if (ImGui::BeginCombo("Live resolution changes", live_modes[live])) {
        for (int i = 0; i < 3; ++i) {
            if (ImGui::Selectable(live_modes[i], i == live)) {
                Store(s.live_resolution, i - 1, true);
            }
        }
        ImGui::EndCombo();
    }
    Hint("On: output and presets change without restarting, while game post-processing stays at 1080p. Off: the scene is patched to the preset resolution at startup. Auto enables the live path on capable discrete GPUs, except for outputs above 1080p with depth of field on (the live path blurs those scenes). Changes apply after restarting the game.");
    ImGui::SeparatorText("Game effects (restart required)");
    static const char* lods[] = {"Highest (-2)", "Original", "Lower (1)", "Lowest (2)"};
    static constexpr int lod_values[] = {-2, 0, 1, 2};
    int lod_index = 1;
    for (int i = 0; i < 4; ++i) {
        if (lod_values[i] == s.model_lod) lod_index = i;
    }
    if (ImGui::BeginCombo("Model detail", lods[lod_index])) {
        for (int i = 0; i < 4; ++i) {
            if (ImGui::Selectable(lods[i], i == lod_index)) {
                Store(s.model_lod, lod_values[i], true);
            }
        }
        ImGui::EndCombo();
    }
    Hint("Model LOD game patch. Highest (-2) asks for a detail level some armor parts don't have: capes vanish and some armor looks wrong.");
    if (s.model_lod == -2) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Highest can break armor and capes");
    }
    int fov = s.fov;
    Store(s.fov, fov, ImGui::SliderInt("Field of view", &fov, BbSettings::FovMin, BbSettings::FovMax,
                                       fov == BbSettings::FovDefault ? "%d (game)" : "%d"));
    Hint("Vertical field of view in degrees; the game's camera has 43. Every camera is scaled by the same amount (scripts/fov.py).");
    for (int e = 0; e < BbSettings::EffectCount; ++e) {
        Checkbox(BbSettings::Effects[e].label, s.effects[e]);
    }
    Hint("Effects are applied through game patches at startup (patches/Bloodborne.xml). Motion blur and dynamic shadows can increase GPU load.");
    Hint("Free camera: hold Cross and press L3 (keyboard: Space + Z). Debug menu: left touchpad / Tab. Requires DbgFont14h.ccm and DbgFont14h.tpf in dvdroot_ps4/font from the debug-font mod. Right touchpad: Backspace.");

    bool restart = BbSettings::FsrSettingsNeedRestart() || s.object_motion != s.startup_object_motion ||
                   s.model_lod != s.startup_model_lod || s.fov != s.startup_fov ||
                   s.live_resolution != s.startup_live_resolution ||
                   s.fps_mode != s.startup_fps_mode || s.frame_cap != s.startup_frame_cap ||
                   BbSettings::ResolutionNeedsRestart();
    for (int e = 0; e < BbSettings::EffectCount; ++e) {
        restart |= s.effects[e] != s.startup_effects[e];
    }
    if (restart) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                           "Changes will apply after restarting the game");
        if (ImGui::Button("Apply and restart")) {
            BbSettings::Save();
            runtime_restart();
        }
    }

    ImGui::SeparatorText("Other");
    ImGui::TextDisabled("Counter (top right), one switch per line it shows:");
    Checkbox("FPS", s.show_fps);
    ImGui::SameLine();
    Checkbox("Frame times", s.show_frame_times);
    ImGui::SameLine();
    Checkbox("Render latency", s.show_latency);
    ImGui::SameLine();
    Checkbox("VRAM", s.show_vram);
    Checkbox("GPU usage", s.show_gpu);
    ImGui::SameLine();
    Checkbox("CPU usage", s.show_cpu);
    ImGui::SameLine();
    Checkbox("RAM usage", s.show_ram);
    Checkbox("Frame time graph", s.show_fps_graph);
    ImGui::SameLine();
    Checkbox("Colour-coded values", s.counter_colors);
    Checkbox("Mute when the game is in the background", s.mute_unfocused);
    static const char* prompt_modes[] = {"Follow the device in use", "PlayStation", "Keyboard and mouse", "Xbox",
                                         "Nintendo Switch"};
    int prompts = s.button_prompts;
    if (ImGui::BeginCombo("Button prompts", prompt_modes[prompts])) {
        for (int i = 0; i < BbSettings::PromptsCount; ++i) {
            if (ImGui::Selectable(prompt_modes[i], i == prompts)) Store(s.button_prompts, i, true);
        }
        ImGui::EndCombo();
    }
    Hint("The game's button icons: keyboard and mouse keys from input.ini while you play with them, the controller's (PlayStation, Xbox or Switch) while you use it.");

    ImGui::Spacing();
    if (ImGui::Button("Close")) {
        keep_open = false;
    }
    // Issues #74, #116: leaving the game from the pad (this menu opens with L3+R3). A second
    // press confirms; the window thread then closes like the window's own close button.
    // (From bbport 0.51.)
    static bool quit_armed;
    ImGui::SameLine();
    // One ID for both labels ("###bb_quit"): with a new ID the button lost the pad's or keyboard's
    // focus at the first press, which disarmed it at once.
    if (ImGui::Button(quit_armed ? "Press again to quit###bb_quit" : "Quit game###bb_quit")) {
        if (quit_armed) {
            SDL_Event quit{};
            quit.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&quit);
        }
        quit_armed = !quit_armed;
    }
    if (quit_armed && !ImGui::IsItemFocused() && !ImGui::IsItemHovered()) {
        quit_armed = false;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Settings are saved to bbport.ini");
    ImGui::End();
    if (!keep_open) {
        SetOpen(false);
    }
}

// The game's text dialog: what is typed, and how to finish (keyboard or controller).
void TextEntryBox() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.42f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(viewport->WorkSize.x * 0.32f, 0.0f),
                                        ImVec2(viewport->WorkSize.x * 0.8f, FLT_MAX));
    ImGui::SetNextWindowBgAlpha(0.92f);
    ImGui::Begin("##text_entry", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextColored(ImVec4(0.85f, 0.72f, 0.45f, 1.0f), "%s", text_entry_prompt.c_str());
    ImGui::Separator();
    ImGui::SetWindowFontScale(1.4f);
    ImGui::Text("%s_", text_entry_text.c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Separator();
    ImGui::TextDisabled("Keyboard: type, Backspace to erase, Enter = OK, Esc = cancel");
    ImGui::TextDisabled("Controller: Cross (A) = OK, Circle (B) = cancel");
    ImGui::End();
}

const char* UpscalerLabel(int upscaler) {
    switch (upscaler) {
    case BbSettings::UpscalerFsr3: return "FSR 3.1";
    case BbSettings::UpscalerFsr4: return "FSR 4";
    case BbSettings::UpscalerFsr411: return "FSR 4.1.1";
    case BbSettings::UpscalerFsr412: return "FSR 4.1.2";
    case BbSettings::UpscalerTaa: return "TAA";
    case BbSettings::UpscalerDlss: return "DLSS";
    default: return "";
    }
}

// The upscaler's vendor colour, as the vendors brand them.
// TODO(port): Intel XeSS (blue, #0068B5) when it is implemented: add the enum value in
// bbport_settings.h and a case here.
ImVec4 UpscalerColor(int upscaler) {
    switch (upscaler) {
    case BbSettings::UpscalerDlss:
        return ImVec4(0.55f, 0.90f, 0.30f, 1.0f); // NVIDIA green
    case BbSettings::UpscalerFsr3:
    case BbSettings::UpscalerFsr4:
    case BbSettings::UpscalerFsr411:
    case BbSettings::UpscalerFsr412:
        return ImVec4(0.95f, 0.40f, 0.35f, 1.0f); // AMD red
    default:
        return ImVec4(0.75f, 0.75f, 0.75f, 1.0f); // TAA, off: no vendor
    }
}

/// The processor name Windows shows in System settings, read once (a reference: the counter asks
/// for it every frame).
const std::string& CpuName() {
#if defined(_WIN32)
    static const std::string name = [] {
        wchar_t buffer[256]{};
        DWORD bytes = sizeof(buffer);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                         L"ProcessorNameString", RRF_RT_REG_SZ, nullptr, buffer, &bytes) !=
            ERROR_SUCCESS) {
            return std::string{};
        }
        char utf8[512]{};
        const int length =
            WideCharToMultiByte(CP_UTF8, 0, buffer, -1, utf8, sizeof(utf8), nullptr, nullptr);
        return std::string(utf8, length > 1 ? size_t(length - 1) : 0);
    }();
    return name;
#else
    static const std::string name;
    return name;
#endif
}

std::string gpu_name; ///< the Vulkan device's model name, set in Init
u32 output_width = 0, output_height = 0; ///< the presented size, set in Render

// A dim label, with every value starting in the same column (the reference overlay's look). The
// column is measured, not a constant: a longer label or a larger UI scale would otherwise crowd a
// value or clip it.
void Label(const char* text, float column) {
    ImGui::TextDisabled("%s", text);
    ImGui::SameLine(column);
}

// Frame-time graph: the last four seconds as a filled curve, milliseconds on the left, the same
// heights read as FPS on the right. Auto-ranged like a scope, no fixed reference lines: the axes
// carry the scale. Each axis gets a gutter sized from its own widest tick, and the plot is inset by
// half a line top and bottom, so no number sits on the curve or gets cut by the box.
void FrameTimeGraph(float average_ms) {
    char text[32];
    const float font = ImGui::GetFontSize();
    const float unit = 4.0f * base_scale;
    // The samples in ring order, oldest first. frame_history_next is the next slot to write, so the
    // oldest stored one is frame_history_count back from it (not next itself: it is unwritten until
    // the ring has filled once).
    const size_t oldest =
        (frame_history_next + FrameHistorySize - frame_history_count) % FrameHistorySize;
    float lo = frame_history[oldest];
    float hi = lo;
    for (size_t i = 1; i < frame_history_count; ++i) {
        const float ms = frame_history[(oldest + i) % FrameHistorySize];
        lo = std::min(lo, ms);
        hi = std::max(hi, ms);
    }
    // At least 4 ms of span, so a steady scene does not zoom into noise.
    if (hi - lo < 4.0f) {
        const float mid = 0.5f * (lo + hi);
        lo = mid - 2.0f;
        hi = mid + 2.0f;
    }
    lo = std::max(lo - 0.4f, 0.0f);
    hi += 0.4f;

    const float ticks[3] = {lo, 0.5f * (lo + hi), hi};
    float gutter_l = 0.0f, gutter_r = 0.0f;
    for (const float ms : ticks) {
        std::snprintf(text, sizeof(text), "%.1f", double(ms));
        gutter_l = std::max(gutter_l, ImGui::CalcTextSize(text).x);
        std::snprintf(text, sizeof(text), "%.0f", double(ms > 0.0f ? 1000.0f / ms : 0.0f));
        gutter_r = std::max(gutter_r, ImGui::CalcTextSize(text).x);
    }
    gutter_l += unit * 2.0f;
    gutter_r += unit * 2.0f;

    const float plot_w = 210.0f * base_scale;
    const float vpad = font * 0.5f + unit; // half a line above and below the plot
    const float w = gutter_l + plot_w + gutter_r;
    const float h = 60.0f * base_scale + 2.0f * vpad;

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 end{origin.x + w, origin.y + h};
    ImGui::Dummy(ImVec2(w, h));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 plot_min{origin.x + gutter_l, origin.y + vpad};
    const ImVec2 plot_max{end.x - gutter_r, end.y - vpad};
    IM_ASSERT(plot_max.x > plot_min.x && plot_max.y > plot_min.y); // gutters fit in the box
    const float plot_h = plot_max.y - plot_min.y;
    const ImVec4 tone = PaceColor(average_ms);
    const ImU32 line = ImGui::ColorConvertFloat4ToU32(tone);
    const ImU32 fill_top = ImGui::ColorConvertFloat4ToU32(ImVec4(tone.x, tone.y, tone.z, 0.35f));
    const ImU32 fill_bottom = ImGui::ColorConvertFloat4ToU32(ImVec4(tone.x, tone.y, tone.z, 0.02f));
    const ImU32 grid = ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 1.0f, 1.0f, 0.08f));
    const bool plain = PlainCounter();
    const ImU32 ms_text = ImGui::ColorConvertFloat4ToU32(plain ? PlainValue : ImVec4(0.62f, 0.80f, 0.92f, 1.0f));
    const ImU32 fps_text = ImGui::ColorConvertFloat4ToU32(plain ? PlainValue : ImVec4(0.55f, 0.95f, 0.55f, 1.0f));

    draw->AddRectFilled(origin, end, ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, 0.42f)),
                        4.0f * base_scale);
    const auto y_for = [&](float ms) {
        return plot_max.y - plot_h * (ms - lo) / (hi - lo);
    };
    for (int tick = 2; tick >= 0; --tick) {
        const float y = y_for(ticks[tick]);
        draw->AddLine(ImVec2(plot_min.x, y), ImVec2(plot_max.x, y), grid, 1.0f);
        std::snprintf(text, sizeof(text), "%.1f", double(ticks[tick]));
        draw->AddText(ImVec2(origin.x + unit, y - font * 0.5f), ms_text, text);
        std::snprintf(text, sizeof(text), "%.0f",
                      double(ticks[tick] > 0.0f ? 1000.0f / ticks[tick] : 0.0f));
        const ImVec2 size = ImGui::CalcTextSize(text);
        draw->AddText(ImVec2(end.x - unit - size.x, y - font * 0.5f), fps_text, text);
    }

    const float step = (plot_max.x - plot_min.x) / float(FrameHistorySize);
    ImVec2 points[FrameHistorySize];
    for (size_t i = 0; i < frame_history_count; ++i) {
        const size_t sample = (oldest + i) % FrameHistorySize; // oldest first
        const float x = plot_min.x + step * float(i + 1);
        points[i] = ImVec2(x, std::clamp(y_for(frame_history[sample]), plot_min.y, plot_max.y));
        draw->AddRectFilledMultiColor(ImVec2(x - step, points[i].y), ImVec2(x, plot_max.y),
                                      fill_top, fill_top, fill_bottom, fill_bottom);
    }
    draw->AddPolyline(points, int(frame_history_count), line, ImDrawFlags_None, 1.5f * base_scale);
    draw->AddCircleFilled(points[frame_history_count - 1], 2.2f * base_scale, line);
}

void FpsCounter() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - pad,
                                   viewport->WorkPos.y + pad),
                            ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.5f);
    ImGui::Begin("##fps", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    const auto& s = BbSettings::Get();
    const ImVec4 pace = PaceColor(frame_ms_avg);
    const float rate = frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f;
    constexpr double gb = 1024.0 * 1024.0 * 1024.0;
    const float model_gap = 12.0f * base_scale;
    // One value column for the whole panel, as wide as the longest label it can show plus a gap.
    // Measured and constant, so values line up and never move when a switch is flipped.
    float column = 0.0f;
    for (const char* label : {"FPS:", "Frame:", "Latency:", "VRAM:", "GPU:", "CPU:", "RAM:"}) {
        column = std::max(column, ImGui::CalcTextSize(label).x);
    }
    column += 10.0f * base_scale;
    if (s.show_fps) {
        Label("FPS:", column);
        ImGui::PushStyleColor(ImGuiCol_Text, pace);
        ImGui::Text("%.1f", double(rate));
        ImGui::PopStyleColor();
        ImGui::SameLine(0.0f, model_gap);
        const int out = s.output_res;
        ImGui::TextDisabled("|  %ux%u", output_width ? output_width : u32(BbSettings::OutputWidths[out]),
                            output_height ? output_height : u32(BbSettings::OutputHeights[out]));
        if (UpscalerLabel(s.upscaler)[0] != '\0') {
            ImGui::SameLine(0.0f, 6.0f * base_scale);
            ImGui::TextColored(PlainCounter() ? PlainValue : UpscalerColor(s.upscaler), "%s",
                               UpscalerLabel(s.upscaler));
        }
        const int generated = s.frame_gen != BbSettings::FrameGenOff ? s.frame_gen_now.load() : 0;
        if (generated > 0 && s.game_fps > 0.0f) {
            ImGui::SameLine(0.0f, 6.0f * base_scale);
            ImGui::TextDisabled("(game %.0f, %dx)", double(s.game_fps.load()), generated);
        }
    }
    if (s.show_frame_times) {
        Label("Frame:", column);
        ImGui::PushStyleColor(ImGuiCol_Text, pace);
        ImGui::Text("%.1f ms", frame_ms_avg);
        ImGui::PopStyleColor();
    }
    // Render latency, its own line (it sat under FPS, where players took it for the frame time):
    // from the game starting a frame to its present (vk_low_latency.cpp), and the low latency mode.
    if (s.show_latency) {
        static const char* modes[] = {"Low latency off", "Reflex", "Reflex + Boost", "Anti-Lag 2",
                                      "Low latency"};
        const int mode = std::clamp(s.latency_mode.load(), 0, 4);
        Label("Latency:", column);
        if (const float latency = s.latency_ms.load(std::memory_order_relaxed); latency > 0.0f) {
            ImGui::Text("%.1f ms", double(latency));
        } else {
            ImGui::TextDisabled("--");
        }
        ImGui::SameLine(0.0f, model_gap);
        ImGui::TextDisabled("|  render latency, %s", modes[mode]);
    }
    // Video memory in use, against the limit the collector holds itself to: the same numbers the
    // low-memory warning reports. 0, so nothing shown, until the first collector pass.
    const u64 used = vram_used.load(std::memory_order_relaxed);
    if (s.show_vram && used != 0) {
        const u64 limit = vram_limit.load(std::memory_order_relaxed);
        Label("VRAM:", column);
        ImGui::PushStyleColor(ImGuiCol_Text,
                             VramColor(used, vram_pressure.load(std::memory_order_relaxed),
                                       vram_critical.load(std::memory_order_relaxed)));
        if (limit != 0) {
            ImGui::Text("%.1f / %.1f GB", double(used) / gb, double(limit) / gb);
        } else {
            ImGui::Text("%.1f GB", double(used) / gb);
        }
        ImGui::PopStyleColor();
        // The card's own headroom: the limit above is the port's conservative budget, so without
        // this "4.6 of 4.6" reads as "the card is full" when it is not.
        ImGui::SameLine(0.0f, 8.0f * base_scale);
        ImGui::TextDisabled("(card: %.1f GB free)",
                            double(vram_card_free.load(std::memory_order_relaxed)) / gb);
    }
    if (s.show_gpu) {
        Label("GPU:", column);
        const float gpu = Load().gpu.load(std::memory_order_relaxed);
        if (gpu < 0.0f) {
            ImGui::TextDisabled("--");
        } else {
            ImGui::TextColored(LoadColor(gpu, 90.0f, 97.0f), "%.0f%%", double(gpu));
        }
        if (!gpu_name.empty()) {
            ImGui::SameLine(0.0f, model_gap);
            ImGui::TextDisabled("%s", gpu_name.c_str());
        }
    }
    if (s.show_cpu) {
        Label("CPU:", column);
        const float cpu = Load().cpu.load(std::memory_order_relaxed);
        if (cpu < 0.0f) {
            ImGui::TextDisabled("--");
        } else {
            ImGui::TextColored(LoadColor(cpu, 70.0f, 90.0f), "%.0f%%", double(cpu));
        }
        const std::string& model = CpuName();
        if (!model.empty()) {
            ImGui::SameLine(0.0f, model_gap);
            ImGui::TextDisabled("%s", model.c_str());
        }
    }
    if (s.show_ram) {
        Label("RAM:", column);
        const u64 ram = Load().ram.load(std::memory_order_relaxed);
        if (ram == 0) {
            ImGui::TextDisabled("--");
        } else {
            ImGui::Text("%.1f GB", double(ram) / gb);
            // The machine's headroom, like the VRAM row's card figure: the game's working set on
            // its own looks like it is eating all the memory.
            const u64 free = Load().ram_free.load(std::memory_order_relaxed);
            if (free != 0) {
                ImGui::SameLine(0.0f, 8.0f * base_scale);
                ImGui::TextDisabled("(%.1f GB free)", double(free) / gb);
            }
        }
    }
    if (s.show_fps_graph && frame_history_count > 1) {
        FrameTimeGraph(frame_ms_avg);
        // Legend under the graph, the current values of what the line shows.
        const float square = 9.0f * base_scale;
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(square, square));
        ImGui::GetWindowDrawList()->AddRectFilled(at, ImVec2(at.x + square, at.y + square),
                                                  ImGui::ColorConvertFloat4ToU32(pace),
                                                  1.5f * base_scale);
        ImGui::SameLine(0.0f, 6.0f * base_scale);
        ImGui::Text("Frame: %.1f ms", frame_ms_avg);
        ImGui::SameLine(0.0f, 14.0f * base_scale);
        ImGui::TextDisabled("%.0f FPS", double(rate));
    }
    ImGui::End();
}

bool WarningActive() {
    std::scoped_lock lock{warn_mutex};
    return !warn_headline.empty() && std::chrono::steady_clock::now() < warn_until;
}

// Amber banner at the top of the screen: the player should save before the game runs out of
// video memory (nothing is blocked; the game still crashes if it has to). The headline is
// brighter than the detail so the point of the banner reads first.
void WarningBanner() {
    std::string headline, detail;
    float fraction = -1.0f;
    {
        std::scoped_lock lock{warn_mutex};
        headline = warn_headline;
        detail = warn_detail;
        fraction = warn_fraction;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + 24.0f * base_scale),
                            ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.92f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.42f, 0.20f, 0.02f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.95f, 0.60f, 0.20f, 1.0f));
    if (ImGui::Begin("##vram-warning", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                         ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing)) {
        ImGui::PushTextWrapPos(560.0f * base_scale);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 0.82f, 0.45f, 1.00f));
        ImGui::TextUnformatted(headline.c_str());
        ImGui::PopStyleColor();
        if (fraction >= 0.0f) {
            // How much of the memory in use: the bar says in one glance what the numbers say.
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram,
                                  fraction > 0.90f ? ImVec4(1.00f, 0.45f, 0.40f, 1.0f)
                                                   : ImVec4(1.00f, 0.72f, 0.30f, 1.0f));
            ImGui::ProgressBar(std::min(fraction, 1.0f), ImVec2(-FLT_MIN, 5.0f * base_scale), "");
            ImGui::PopStyleColor();
        }
        if (!detail.empty()) {
            ImGui::Spacing();
            ImGui::TextUnformatted(detail.c_str());
        }
        ImGui::PopTextWrapPos();
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
}

} // namespace

void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count) {
    std::scoped_lock lock{imgui_mutex};
    if (initialized) {
        return;
    }
    gpu_name = std::string(instance.GetModelName());
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // window positions are not kept
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.BackendPlatformName = "bbport";

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.Colors[ImGuiCol_WindowBg].w = 0.92f;

    static ImVector<ImWchar> picker_glyphs;
    ImFontGlyphRangesBuilder ranges;
    ranges.AddRanges(io.Fonts->GetGlyphRangesDefault());
    ranges.AddRanges(io.Fonts->GetGlyphRangesCyrillic());
    for (const char* text : Fsr41Ui::texts[Fsr41Ui::Language()]) ranges.AddText(text);
    ranges.BuildRanges(&picker_glyphs);
    ImFontConfig font_config;
    font_config.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(bb_font_ttf),
                                   int(bb_font_ttf_end - bb_font_ttf), 18.0f, &font_config, picker_glyphs.Data);

#ifdef _WIN32
    if (const char* windows=std::getenv("WINDIR")) {
        const std::string language=Fsr41Ui::languages[Fsr41Ui::Language()];
        const char* candidates[] = {"msyh.ttc","YuGothM.ttc","msgothic.ttc","malgun.ttf"};
        if (language=="zh" || language=="ja" || language=="ko") for(const char* file:candidates) {
            const auto path=std::filesystem::path(windows)/"Fonts"/file;
            if(!std::filesystem::is_regular_file(path)) continue;
            ImFontConfig merge; merge.MergeMode=true;
            io.Fonts->AddFontFromFileTTF(path.string().c_str(),18.f,&merge,picker_glyphs.Data);
        }
    }
#endif
    const vk::Instance vk_instance = instance.GetInstance();
    ImGui_ImplVulkan_LoadFunctions(
        instance.ApiVersion(),
        [](const char* name, void* user) {
            return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr(
                *static_cast<const vk::Instance*>(user), name);
        },
        const_cast<vk::Instance*>(&vk_instance));

    const VkFormat color_format = static_cast<VkFormat>(format);
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = instance.ApiVersion();
    info.Instance = vk_instance;
    info.PhysicalDevice = instance.GetPhysicalDevice();
    info.Device = instance.GetDevice();
    info.QueueFamily = instance.GetGraphicsQueueFamilyIndex();
    info.Queue = instance.GetGraphicsQueue();
    info.DescriptorPoolSize = 16;
    info.MinImageCount = std::max(image_count, 2u);
    info.ImageCount = std::max(image_count, 2u);
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color_format,
    };
    if (!ImGui_ImplVulkan_Init(&info)) {
        std::printf("Overlay: ImGui Vulkan backend init failed\n");
        ImGui::DestroyContext();
        return;
    }
    initialized = true;
    std::printf("Overlay: menu ready (%s)\n", MenuOpeners().c_str());
}

void UpdateTextInput(SDL_Window* window) {
    bool want = false;
    {
        std::scoped_lock lock{imgui_mutex};
        want = initialized && menu_open && ImGui::GetIO().WantTextInput;
    }
    if (want != SDL_TextInputActive(window)) {
        if (want) {
            SDL_StartTextInput(window);
        } else {
            SDL_StopTextInput(window);
        }
    }
}

bool HandleEvent(const SDL_Event& event) {
    std::scoped_lock lock{imgui_mutex};
    if (!initialized) {
        return false;
    }
    ImGuiIO& io = ImGui::GetIO();
    const bool is_open = menu_open;
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        const bool menu_key = MenuKey() != SDLK_UNKNOWN && event.key.key == MenuKey();
        if (down && !event.key.repeat && (menu_key || (is_open && event.key.key == SDLK_ESCAPE))) {
            SetOpen(menu_key ? !is_open : false);
            return true;
        }
        if (!is_open) {
            return false;
        }
        io.AddKeyEvent(ImGuiMod_Ctrl, (event.key.mod & SDL_KMOD_CTRL) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (event.key.mod & SDL_KMOD_SHIFT) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (event.key.mod & SDL_KMOD_ALT) != 0);
        if (const ImGuiKey key = KeyFromSdl(event.key.key); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        const bool down = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        const u8 button = event.gbutton.button;
        if (button == SDL_GAMEPAD_BUTTON_LEFT_STICK) {
            l3_down = down;
        } else if (button == SDL_GAMEPAD_BUTTON_RIGHT_STICK) {
            r3_down = down;
        } else if (button == SDL_GAMEPAD_BUTTON_BACK) {
            back_down = down;
        } else if (button == SDL_GAMEPAD_BUTTON_START) {
            start_down = down;
        }
        const MenuPad pad = MenuPadCombo();
        if (down && ((pad == MenuPad::L3R3 && l3_down && r3_down) ||
                     (pad == MenuPad::BackStart && back_down && start_down))) {
            SetOpen(!is_open);
            return true;
        }
        if (!is_open) {
            return false;
        }
        if (const ImGuiKey key = KeyFromGamepad(button); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_TEXT_INPUT: {
        // Typed characters (Ctrl+click on a slider, a text field): key events alone erase but
        // do not type. SDL sends them while text input is on (UpdateTextInput).
        if (!is_open) {
            return false;
        }
        io.AddInputCharactersUTF8(event.text.text);
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        if (!is_open) {
            return false;
        }
        const float density = PixelDensity(event.motion.windowID);
        io.AddMousePosEvent(event.motion.x * density, event.motion.y * density);
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (!is_open) {
            return false;
        }
        const int button = event.button.button == SDL_BUTTON_LEFT    ? 0
                           : event.button.button == SDL_BUTTON_RIGHT  ? 1
                           : event.button.button == SDL_BUTTON_MIDDLE ? 2
                                                                      : -1;
        if (button >= 0) {
            io.AddMouseButtonEvent(button, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        }
        return true;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (!is_open) {
            return false;
        }
        io.AddMouseWheelEvent(event.wheel.x, event.wheel.y);
        return true;
    default:
        return false;
    }
}

// Any of the counter's readouts asked for (the graph lives in the same window).
bool ReadoutsOn() {
    const auto& s = BbSettings::Get();
    return s.show_fps || s.show_frame_times || s.show_latency || s.show_vram || s.show_gpu ||
           s.show_cpu || s.show_ram || s.show_fps_graph;
}

bool Visible() {
    return initialized &&
           (menu_open || text_entry_active || WarningActive() || ReadoutsOn());
}

bool CapturesInput() {
    return menu_open || text_entry_active;
}

void SetTextEntry(bool active, const std::string& prompt, const std::string& text) {
    std::scoped_lock lock{imgui_mutex};
    text_entry_active = active;
    text_entry_prompt = prompt;
    text_entry_text = text;
}

void Warn(const std::string& headline, const std::string& detail, float used_fraction) {
    std::scoped_lock lock{warn_mutex};
    warn_headline = headline;
    warn_detail = detail;
    warn_fraction = used_fraction;
    warn_until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
}

void SetVramUsage(u64 used_bytes, u64 limit_bytes, u64 pressure_bytes, u64 critical_bytes,
                  u64 card_free_bytes) {
    vram_used.store(used_bytes, std::memory_order_relaxed);
    vram_limit.store(limit_bytes, std::memory_order_relaxed);
    vram_pressure.store(pressure_bytes, std::memory_order_relaxed);
    vram_critical.store(critical_bytes, std::memory_order_relaxed);
    vram_card_free.store(card_free_bytes, std::memory_order_relaxed);
}

// Called for every presented image: the interval for the FPS readout and BB_FPS_LOG.
void NotePresent() {
    const auto now = std::chrono::steady_clock::now();
    const float ms = std::chrono::duration<float, std::milli>(now - last_present).count();
    last_present = now;
    last_present_ms = ms;
    if (ms > 0.0f && ms < 1000.0f) {
        frame_ms_avg = frame_ms_avg == 0.0f ? ms : frame_ms_avg * 0.95f + ms * 0.05f;
        frame_history[frame_history_next] = ms;
        frame_history_next = (frame_history_next + 1) % FrameHistorySize;
        frame_history_count = std::min(frame_history_count + 1, FrameHistorySize);
    }
    // CPU and RAM at 4 Hz (syscalls, and neither changes between two frames); the GPU reading
    // has its own thread, started the first time the readout is asked for.
    static auto last_sample = now;
    if (now - last_sample >= std::chrono::milliseconds(250)) {
        last_sample = now;
        SampleCpu();
        Load().ram.store(BbProcessMemory::Get().working_set, std::memory_order_relaxed);
        Load().ram_free.store(AvailableMemory(), std::memory_order_relaxed);
        if (BbSettings::Get().show_gpu) {
            StartGpuSampler();
        }
    }
    // BB_FPS_LOG=1: presents per second and the slowest frame, every 5 seconds (test runs).
    static const bool fps_log = [] {
        const char* env = std::getenv("BB_FPS_LOG");
        return env && env[0] == '1';
    }();
    if (fps_log) {
        static auto window_start = now;
        static int frames = 0;
        static float worst = 0.0f;
        ++frames;
        worst = std::max(worst, ms < 1000.0f ? ms : 0.0f);
        const float elapsed = std::chrono::duration<float>(now - window_start).count();
        if (elapsed >= 5.0f) {
            std::printf("Frame rate: %.1f FPS, slowest frame %.1f ms\n", frames / elapsed, worst);
            window_start = now;
            frames = 0;
            worst = 0.0f;
        }
    }
}

void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent) {
    const float ms = last_present_ms;
    // What is actually presented: follows a window resize, unlike the configured output.
    output_width = extent.width;
    output_height = extent.height;
    if (!Visible()) {
        return;
    }
    std::scoped_lock lock{imgui_mutex};
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(extent.width), float(extent.height));
    io.DeltaTime = ms > 0.0f && ms < 1000.0f ? ms / 1000.0f : 1.0f / 60.0f;
    // UI scale follows the display height (1080p = 1).
    const float scale = std::max(float(extent.height) / 1080.0f, 0.75f);
    if (std::abs(scale - base_scale) > 0.01f) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(scale / base_scale);
        style.FontScaleMain = scale;
        base_scale = scale;
    }

    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    if (menu_open) {
        Menu();
    }
    if (ReadoutsOn() && !menu_open) {
        FpsCounter();
    }
    if (text_entry_active) {
        TextEntryBox();
    }
    if (WarningActive()) {
        WarningBanner();
    }
    ImGui::Render();

    const vk::RenderingAttachmentInfo attachment{
        .imageView = view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    cmdbuf.beginRendering(vk::RenderingInfo{
        .renderArea = {{0, 0}, extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    });
    {
        // Font atlas uploads submit to the graphics queue themselves.
        std::scoped_lock submit_lock{Vulkan::Scheduler::submit_mutex};
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmdbuf);
    }
    cmdbuf.endRendering();
}

} // namespace BbOverlay

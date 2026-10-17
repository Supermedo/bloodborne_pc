// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: user settings changed at run time from the in-game menu (bbport_overlay.h) and kept
// in bbport.ini (BB_CONFIG overrides the path). Environment variables override the file at
// start. Readers load the atomics every frame; writers are the menu and Load().

#pragma once

#include <atomic>

namespace BbSettings {

enum Upscaler : int { UpscalerOff = 0, UpscalerFsr3 = 1, UpscalerFsr4 = 2, UpscalerFsr411 = 3,
                      UpscalerTaa = 4, UpscalerDlss = 5, UpscalerCount };
/// FSR 4 v07 or FSR 4.1.1: the same inputs, settings and placement in the frame.
inline bool IsFsr4(int upscaler) {
    return upscaler == UpscalerFsr4 || upscaler == UpscalerFsr411;
}
enum FgBackend : int { FgDlss = 0, FgFsr = 1, FgBackendCount };
enum ReflexMode : int { ReflexOff = 0, ReflexOn = 1, ReflexBoost = 2, ReflexModeCount };

const char* FgBackendName(int backend);
const char* ReflexModeName(int mode);
const char* FrameGenName(int factor);
enum Preset : int { NativeAA = 0, Quality, Balanced, Performance, UltraPerformance, PresetCount };
enum DebugView : int { DebugNone = 0, DebugReactive = 1, DebugMotion = 2, DebugViewCount };

/// Game effects switched by the community patches at start (patches.py EFFECTS): ini key,
/// menu label, default (the game's own behaviour).
struct Effect {
    const char* key;
    const char* label;
    bool default_on;
};
inline constexpr Effect Effects[] = {
    {"effect_chromatic_aberration", "Chromatic aberration", true},
    {"effect_dof", "Depth of field (DoF)", true},
    {"effect_motion_blur", "Motion blur", true},
    {"effect_ssao", "Ambient occlusion (SSAO)", true},
    {"effect_game_aa", "Game anti-aliasing", true},
    {"effect_dynamic_shadows", "Dynamic shadows", true},
    {"effect_ssr", "Screen-space reflections (SSR)", false},
    {"skip_intro", "Skip intro logos and movie", false},
    {"debug_camera", "Free camera (Cross + L3)", false},
    {"debug_menu", "Debug menu (requires font files)", false},
    {"cheat_no_death", "Cheat: never die (minimum 1 HP)", false},
    {"cheat_stealth", "Cheat: enemies cannot see you", false},
    {"cheat_silent", "Cheat: enemies cannot hear you", false},
    {"cheat_rally_no_decay", "Cheat: Rally never decays", false},
    {"cheat_enemy_control", "Cheat: control targeted enemy (R3 / L3)", false},
    {"tweak_no_rally", "Disable Rally (health recovery)", false},
    {"tweak_camera_distance", "Increase camera distance", false},
    {"tweak_no_camera_rotation", "Disable automatic camera rotation", false},
    {"tweak_easy_run", "Run with less stick tilt", false},
    {"tweak_ragdoll", "Dark Souls-style ragdoll physics", false},
    {"tweak_circle_confirm", "Japanese/Asian buttons (Circle confirms)", false},
    {"quit_to_desktop", "Quit to Desktop in the title and System menus", true},
};
inline constexpr int EffectCount = int(sizeof(Effects) / sizeof(Effects[0]));
/// Live output resolutions: the upscaler's output and the UI host targets, including the named
/// 21:9/32:9 presets. Wider-than-16:9 outputs keep the HUD centred and widen the camera with the
/// startup patch (patches.py, run.py sets BB_RENDER_RES/BB_OUTPUT_RES), so they change only on a
/// restart; 16:9 and narrower ones change live.
// Last slot retains a validated custom resolution loaded from the launcher/ini.
inline int OutputWidths[] = {1280,1920,2560,3840,1280,2560,3440,5120,5120,1080,3840,3840,1920};
inline int OutputHeights[] = {720,1080,1440,2160,800,1080,1440,1440,2160,1920,1600,1080,1080};
inline constexpr int OutputCount = sizeof(OutputWidths) / sizeof(OutputWidths[0]);
inline constexpr int OutputCustom = OutputCount - 1;
inline constexpr int OutputDefault = 1; ///< 1920x1080, the game's own size
/// Wider than 16:9: the camera and the menu/HUD layer are patched at startup.
inline bool Ultrawide(int output) {
    return OutputWidths[output] * 9 > OutputHeights[output] * 16;
}

struct Values {
    std::atomic<int> upscaler{UpscalerFsr3};
    std::atomic<int> frame_generation{0}; ///< 0 Off, 2 (2x), 3 (3x), 4 (4x)
    std::atomic<int> fg_backend{FgDlss};  ///< FgDlss (0), FgFsr (1)
    std::atomic<int> reflex{ReflexOn};    ///< ReflexOff (0), ReflexOn (1), ReflexBoost (2)
    std::atomic<bool> reflex_supported{true}; ///< Guarded by runtime capability
    std::atomic<int> simulation_fps{0};   ///< Active simulation FPS / selected cap
    std::atomic<float> source_fps{0.0f};  ///< Measured real game frames per second
    std::atomic<float> display_fps{0.0f}; ///< Measured presents per second
    std::atomic<int> preset{NativeAA};
    std::atomic<bool> sharpen{true};
    std::atomic<float> sharpness{0.3f};
    std::atomic<bool> jitter{true};
    std::atomic<bool> reactive{false};
    std::atomic<bool> object_motion{true};
    std::atomic<float> reactive_scale{1.0f};
    std::atomic<float> reactive_threshold{0.2f};
    std::atomic<float> reactive_max{0.9f};
    std::atomic<int> debug_view{DebugNone};
    std::atomic<bool> show_fps{false};
    // FSR 4 checks (menu): the provider's auto exposure, the jitter sign it is given.
    std::atomic<bool> fsr4_auto_exposure{true};
    std::atomic<bool> fsr4_invert_jitter{false};
    std::atomic<int> active_render_width{1920}, active_render_height{1080};
    /// Applied at start (patches.py); the menu shows when a restart is needed.
    std::atomic<bool> effects[EffectCount]{};
    std::atomic<int> model_lod{0}; ///< -2 highest .. 2 lowest, 0 the game's
    std::atomic<int> output_res{OutputDefault}; ///< index into OutputWidths
    /// Ultrawide outputs: the HUD's corner groups, 0 centred in 16:9 .. 100 at the screen edges
    /// (hud_layout.py at start).
    std::atomic<int> hud_position{0};
    /// Live resolution and preset changes (run.sh): 0 off by default (startup patch, fastest
    /// on the Steam Deck and older GPUs), -1 auto (strong discrete GPUs), 1 on. On restart.
    std::atomic<int> live_resolution{0};
    /// Why FSR 4 cannot run (assets, device features), or null. Set by the renderer.
    std::atomic<const char*> fsr4_problem{nullptr};
    std::atomic<bool> fsr4_supported{false}, fsr411_supported{false};
    /// DLSS (bbport_dlss.dll, NVIDIA RTX) is ready, or why not (null before the device exists).
    std::atomic<bool> dlss_supported{false};
    std::atomic<const char*> dlss_problem{nullptr};

    /// Startup settings for the explicit BB_RENDER_RES compatibility patch only.
    int startup_preset = NativeAA;
    int startup_upscaler = UpscalerFsr3;
    int startup_frame_generation = 0;
    int startup_fg_backend = FgDlss;
    int startup_reflex = ReflexOn;
    bool startup_object_motion = true;
    bool startup_effects[EffectCount]{};
    int startup_model_lod = 0;
    int startup_output_res = OutputDefault;
    int startup_hud_position = 0;
    int startup_live_resolution = 0;
};

Values& Get();

/// Reads the file, then the environment overrides. Called once at start.
void Load();
/// Checks the loaded choice before the first frame; unsupported FSR 4 uses FSR 3.1.
void ConfigureUpscalerSupport(bool fsr4, bool fsr411);
/// After device creation: DLSS availability; a DLSS setting falls back to FSR 3.1 without it.
void ConfigureDlssSupport(bool available, const char* problem);
/// Runtime reflex capability check: guards reflex support.
void ConfigureReflexSupport(bool supported);
/// Startup-patched scene dimensions cannot change until run.sh prepares a new image.
bool FixedRenderSession();
int RenderPreset();
bool ResolutionNeedsRestart();
/// Camera and HUD aspect changes require new guest patches on restart.
bool AspectNeedsRestart();
/// FG backend, factor or reflex mode changes take effect on restart.
bool FrameGenNeedsRestart();
/// Writes the file (menu changes).
void Save();

/// Render resolution divisor of a preset (1.0 native, 1.5 quality, ...).
float PresetScale(int preset);
const char* PresetName(int preset);
const char* UpscalerName(int upscaler);

} // namespace BbSettings

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
enum Preset : int { NativeAA = 0, Quality, Balanced, Performance, UltraPerformance, PresetCount };
/// In-game menu language (menu_language in bbport.ini: en, pt, ru).
enum MenuLanguage : int { MenuEnglish = 0, MenuPortuguese = 1, MenuRussian = 2, MenuLanguageCount };
enum DebugView : int { DebugNone = 0, DebugReactive = 1, DebugMotion = 2, DebugViewCount };

/// Game effects switched by the community patches at start (patches.py EFFECTS): ini key,
/// menu label, default (the game's own behaviour).
struct Effect {
    const char* key;
    const char* label;    ///< English
    const char* label_pt; ///< Portuguese
    const char* label_ru; ///< Russian
    bool default_on;
};
inline constexpr Effect Effects[] = {
    {"effect_chromatic_aberration", "Chromatic aberration", "Aberração cromática", "Хроматическая аберрация", true},
    {"effect_dof", "Depth of field (DoF)", "Profundidade de campo (DoF)", "Глубина резкости (DoF)", true},
    {"effect_motion_blur", "Motion blur", "Desfoque de movimento", "Размытие в движении", true},
    {"effect_ssao", "Ambient occlusion (SSAO)", "Oclusão de ambiente (SSAO)", "Затенение SSAO", true},
    {"effect_game_aa", "Game's own anti-aliasing", "Antisserrilhado do próprio jogo", "Собственное сглаживание игры", true},
    {"effect_dynamic_shadows", "Shadows from dynamic lights", "Sombras de luzes dinâmicas", "Тени от динамических источников", true},
    {"effect_ssr", "Screen-space reflections (not in original game)", "Reflexos SSR (não existiam no jogo)", "Отражения SSR (не было в игре)", false},
    {"skip_intro", "Skip startup intros", "Pular vídeos de abertura", "Пропуск заставок при запуске", false},
    {"debug_camera", "Free camera (Cross + L3)", "Câmera livre (Cross + L3)", "Свободная камера (Cross + L3)", false},
    {"debug_menu", "Debug menu (requires font files)", "Debug menu (precisa dos arquivos de fonte)", "Debug menu (нужны файлы шрифтов)", false},
    {"cheat_no_death", "Cheat: immortality (never below 1 HP)", "Trapaça: imortalidade (nunca abaixo de 1 HP)", "Чит: бессмертие (не ниже 1 HP)", false},
    {"cheat_stealth", "Cheat: enemies don't notice you", "Trapaça: inimigos não percebem você", "Чит: враги не замечают", false},
    {"cheat_silent", "Cheat: enemies don't hear you", "Trapaça: inimigos não ouvem você", "Чит: враги не слышат", false},
    {"cheat_rally_no_decay", "Cheat: Rally never fades", "Trapaça: Rally não diminui", "Чит: Rally не угасает", false},
    {"cheat_enemy_control", "Cheat: control the enemy (R3 / L3)", "Trapaça: controlar o inimigo (R3 / L3)", "Чит: управление врагом (R3 / L3)", false},
    {"tweak_no_rally", "No Rally (HP regain)", "Sem Rally (recuperar HP)", "Без Rally (возврата HP)", false},
    {"tweak_camera_distance", "Camera further away", "Câmera mais afastada", "Камера дальше", false},
    {"tweak_no_camera_rotation", "No camera auto-rotation", "Sem rotação automática da câmera", "Без автоповорота камеры", false},
    {"tweak_easy_run", "Run with less stick tilt", "Correr com menos inclinação do analógico", "Бег с меньшим наклоном стика", false},
    {"tweak_ragdoll", "Body physics as in Dark Souls", "Física de corpos como no Dark Souls", "Физика тел как в Dark Souls", false},
};
inline constexpr int EffectCount = int(sizeof(Effects) / sizeof(Effects[0]));
/// Live output resolutions: the upscaler's output and the UI host targets.
inline constexpr int OutputWidths[] = {1280, 1920, 2560, 3840};
inline constexpr int OutputHeights[] = {720, 1080, 1440, 2160};
inline constexpr int OutputCount = 4;
inline constexpr int OutputDefault = 1; ///< 1920x1080, the game's own size

struct Values {
    std::atomic<int> menu_language{MenuEnglish};
    std::atomic<int> upscaler{UpscalerFsr3};
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
    bool startup_object_motion = true;
    bool startup_effects[EffectCount]{};
    int startup_model_lod = 0;
    int startup_output_res = OutputDefault;
    int startup_live_resolution = 0;
};

Values& Get();

/// Reads the file, then the environment overrides. Called once at start.
void Load();
/// Checks the loaded choice before the first frame; unsupported FSR 4 uses FSR 3.1.
void ConfigureUpscalerSupport(bool fsr4, bool fsr411);
/// After device creation: DLSS availability; a DLSS setting falls back to FSR 3.1 without it.
void ConfigureDlssSupport(bool available, const char* problem);
/// Startup-patched scene dimensions cannot change until run.sh prepares a new image.
bool FixedRenderSession();
int RenderPreset();
bool ResolutionNeedsRestart();
/// Writes the file (menu changes).
void Save();

/// Render resolution divisor of a preset (1.0 native, 1.5 quality, ...).
float PresetScale(int preset);
const char* PresetName(int preset);
const char* UpscalerName(int upscaler);
/// `english`, `portuguese` or `russian`, by the menu language.
const char* MenuText(const char* english, const char* portuguese, const char* russian);
/// An effect's menu label in the menu language.
const char* EffectLabel(int effect);

} // namespace BbSettings

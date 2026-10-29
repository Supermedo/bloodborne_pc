// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_settings.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace BbSettings {

namespace {

const char* Path() {
    const char* env = std::getenv("BB_CONFIG");
    return env && env[0] ? env : "bbport.ini";
}

float Clamp(float v, float lo, float hi) {
    return std::clamp(v, lo, hi);
}

/// The FPS overlay's readouts: key=0|1 each, in the order the menu lists them.
struct Readout {
    const char* key;
    std::atomic<bool> Values::*member;
};
constexpr Readout Readouts[] = {
    {"show_fps", &Values::show_fps},
    {"show_frame_times", &Values::show_frame_times},
    {"show_latency", &Values::show_latency},
    {"show_vram", &Values::show_vram},
    {"show_gpu", &Values::show_gpu},
    {"show_cpu", &Values::show_cpu},
    {"show_ram", &Values::show_ram},
    {"show_fps_graph", &Values::show_fps_graph},
    {"counter_colors", &Values::counter_colors},
};

void Set(Values& v, const std::string& key, const std::string& value) {
    const float f = float(std::atof(value.c_str()));
    const int i = std::atoi(value.c_str());
    for (const Readout& readout : Readouts) {
        if (key == readout.key) {
            v.*readout.member = value != "0";
            return;
        }
    }
    if (key == "upscaler") {
        for (int u = 0; u < UpscalerCount; ++u) {
            if (value == UpscalerName(u)) {
                v.upscaler = u;
            }
        }
    } else if (key == "frame_gen") {
        for (int g = 0; g < FrameGenCount; ++g) {
            if (value == FrameGenName(g)) {
                v.frame_gen = g;
            }
        }
    } else if (key == "frame_gen_multiplier") {
        v.frame_gen_multiplier = std::clamp(i, 2, 4);
    } else if (key == "frame_gen_fit_display") {
        v.frame_gen_fit_display = i != 0;
    } else if (key == "preset") {
        v.preset = std::clamp(i, 0, PresetCount - 1);
    } else if (key == "sharpen") {
        v.sharpen = i != 0;
    } else if (key == "sharpness") {
        v.sharpness = Clamp(f, 0.0f, 2.0f);
    } else if (key == "jitter") {
        v.jitter = i != 0;
    } else if (key == "reactive") {
        v.reactive = i != 0;
    } else if (key == "object_motion") {
        v.object_motion = i != 0;
    } else if (key == "fsr411_backend") {
        v.fsr411_backend = value == "dx12" ? Fsr411Dx12 : Fsr411Vulkan;
    } else if (key == "fsr41_version") {
        v.fsr41_version = value == "fsr411" ? UpscalerFsr411 : UpscalerFsr412;
    } else if (key == "object_motion_cache") {
        v.object_motion_cache = value == "1";
    } else if (key == "fsr_compatibility_options") {
        v.fsr_compatibility_options = value == "1";
    } else if (key == "reactive_scale") {
        v.reactive_scale = Clamp(f, 0.0f, 16.0f);
    } else if (key == "reactive_threshold") {
        v.reactive_threshold = Clamp(f, 0.0f, 1.0f);
    } else if (key == "reactive_max") {
        v.reactive_max = Clamp(f, 0.0f, 1.0f);
    } else if (key == "debug_view") {
        v.debug_view = std::clamp(i, 0, DebugViewCount - 1);
    } else if (key == "mute_unfocused") {
        v.mute_unfocused = i != 0;
    } else if (key == "button_prompts") {
        v.button_prompts = value == "playstation" ? PromptsPlayStation
                         : value == "keyboard"    ? PromptsKeyboard
                         : value == "xbox"        ? PromptsXbox
                         : value == "switch"      ? PromptsSwitch
                                                  : PromptsAuto;
    } else if (key == "fsr4_auto_exposure") {
        v.fsr4_auto_exposure = i != 0;
    } else if (key == "fsr4_invert_jitter") {
        v.fsr4_invert_jitter = i != 0;
    } else if (key == "model_lod") {
        v.model_lod = std::clamp(i, -2, 2);
    } else if (key == "live_resolution") {
        v.live_resolution = value == "auto" ? -1 : std::clamp(i, 0, 1);
    } else if (key == "hud_position") {
        v.hud_position = std::clamp(i, 0, 100);
    } else if (key == "sharp_ui") {
        v.sharp_ui = i != 0;
    } else if (key == "fov") {
        v.fov = std::clamp(i, FovMin, FovMax);
    } else if (key == "fps_mode") {
        for (int m = 0; m < FpsModeCount; ++m) {
            if (value == FpsModeNames[m]) v.fps_mode = m;
        }
    } else if (key == "frame_cap") {
        v.frame_cap = value.empty() ? FrameCapAuto : std::max(i, 0);
    } else if (key == "output_res") {
        for (int r = 0; r < OutputCount; ++r) {
            if (value == std::to_string(OutputWidths[r]) + "x" + std::to_string(OutputHeights[r])) {
                v.output_res = r;
            }
        }
    } else {
        for (int e = 0; e < EffectCount; ++e) {
            if (key == Effects[e].key) {
                v.effects[e] = i != 0;
            }
        }
    }
}

} // namespace

Values& Get() {
    static Values values;
    return values;
}

void Load() {
    auto& v = Get();
    v.fg401_fallback = -1;
    v.fg401_fallback_source = -1;
    v.frame_gen_hdr = false;
    v.frame_gen_problem = nullptr;
    v.fg401_problem = nullptr;
    v.fg402_problem = nullptr;
    v.fsr411_backend = Fsr411Vulkan;
    v.fsr41_version = UpscalerFsr412;
    v.fsr4_method_problem = nullptr;
    v.object_motion_cache = false;
    v.fsr_compatibility_options = false;
    for (int e = 0; e < EffectCount; ++e) {
        v.effects[e] = Effects[e].default_on;
    }
    // Before the file: what the launcher started the game with (older bbport.ini files have no
    // fps_mode / frame_cap yet).
    if (const char* fps = std::getenv("BB_FPS")) Set(v, "fps_mode", fps);
    if (const char* cap = std::getenv("BB_FPS_LIMIT")) Set(v, "frame_cap", cap);
    if (FILE* file = std::fopen(Path(), "r")) {
        char line[256];
        while (std::fgets(line, sizeof(line), file)) {
            std::string text{line};
            text.erase(text.find_last_not_of(" \t\r\n") + 1);
            const auto eq = text.find('=');
            if (text.empty() || text[0] == '#' || eq == std::string::npos) {
                continue;
            }
            Set(v, text.substr(0, eq), text.substr(eq + 1));
        }
        std::fclose(file);
        std::printf("Settings: %s\n", Path());
    }
    // Environment overrides (scripts, A/B tests).
    if (const char* env = std::getenv("BB_UPSCALER")) {
        v.upscaler = UpscalerOff;
        for (int u = 0; u < UpscalerCount; ++u) {
            if (std::strcmp(env, UpscalerName(u)) == 0) v.upscaler = u;
        }
    }
    const std::pair<const char*, const char*> env_keys[] = {
        {"BB_FSR_SHARPNESS", "sharpness"},        {"BB_JITTER", "jitter"},
        {"BB_REACTIVE", "reactive"},              {"BB_REACTIVE_SCALE", "reactive_scale"},
        {"BB_REACTIVE_THRESHOLD", "reactive_threshold"}, {"BB_REACTIVE_MAX", "reactive_max"},
        {"BB_UPSCALE_PRESET", "preset"},            {"BB_OBJECT_MOTION", "object_motion"},
        {"BB_FRAME_GEN", "frame_gen"},              {"BB_FRAME_GEN_MULTIPLIER", "frame_gen_multiplier"},
        {"BB_FRAME_GEN_FIT_DISPLAY", "frame_gen_fit_display"},
    };
    for (const auto& [env, key] : env_keys) {
        if (const char* value = std::getenv(env)) {
            Set(v, key, value);
        }
    }
    // Direct engine launches retain the explicit A/B overrides. Launcher starts refresh
    // their derived environment from this file in run.py, including after an overlay restart.
    const char* shared = std::getenv("BB_SHARED_GRAPHICS_CONFIG");
    if (!shared || std::strcmp(shared, "1") != 0) {
        if (const char* native = std::getenv("BB_FSR411_NATIVE")) {
            v.fsr411_backend = native[0] == '1' ? Fsr411Dx12 : Fsr411Vulkan;
        }
        if (const char* backend = std::getenv("BB_FSR411_BACKEND")) {
            Set(v, "fsr411_backend", backend);
        }
        if (const char* cache = std::getenv("BB_OBJECT_MOTION_CACHE")) {
            Set(v, "object_motion_cache", cache);
        }
    }
    v.startup_preset = v.preset;
    if (v.upscaler == UpscalerFsr411 || v.upscaler == UpscalerFsr412) v.fsr41_version = v.upscaler.load();
    v.startup_upscaler = v.upscaler;
    v.startup_object_motion = v.object_motion;
    v.startup_fsr411_backend = v.fsr411_backend;
    v.active_fsr411_backend = v.fsr411_backend.load();
    v.startup_object_motion_cache = v.object_motion_cache;
    for (int e = 0; e < EffectCount; ++e) {
        v.startup_effects[e] = v.effects[e];
    }
    v.startup_model_lod = v.model_lod;
    v.startup_output_res = v.output_res;
    v.startup_hud_position = v.hud_position;
    v.startup_fov = v.fov;
    v.startup_fps_mode = v.fps_mode;
    v.startup_frame_cap = v.frame_cap;
    v.startup_live_resolution = v.live_resolution;
}

void ConfigureUpscalerSupport(bool fsr4, bool fsr411, const char* fsr411_problem) {
    auto& v = Get();
    v.fsr4_supported = fsr4;
    v.fsr411_supported = fsr411;
    static std::string method_problem;
    method_problem = fsr411 ? "" : fsr411_problem ? fsr411_problem :
        "GPU/driver does not support the selected FSR 4.1.1 method";
    v.fsr411_problem = method_problem.empty() ? nullptr : method_problem.c_str();
    const int requested = v.upscaler;
    if ((requested == UpscalerFsr4 && !v.fsr4_supported) ||
        (requested == UpscalerFsr411 && !v.fsr411_supported)) {
        static std::string kept;
        kept = requested == UpscalerFsr411 && fsr411_problem ? fsr411_problem :
            "GPU/driver does not support the selected FSR 4 shaders";
        std::printf("Upscaler: %s unavailable: %s; falling back to FSR 3.1 before the first frame\n",
                    UpscalerName(requested), kept.c_str());
        kept += "; using FSR 3.1";
        v.fsr4_problem = kept.c_str();
        v.upscaler = UpscalerFsr3;
    }
}

void ConfigureFg401Support(bool available, const char* problem, int provider) {
    auto& v = Get();
    auto& supported = provider == FrameGenFsr402 ? v.fg402_supported : v.fg401_supported;
    auto& message = provider == FrameGenFsr402 ? v.fg402_problem : v.fg401_problem;
    supported = available;
    static std::string kept[2];
    auto& text = kept[provider == FrameGenFsr402];
    text = problem ? problem : "";
    message = available || text.empty() ? nullptr : text.c_str();
}

void SelectFrameGen(int provider) {
    auto& v = Get();
    v.frame_gen = provider;
    v.fg401_fallback = -1;
    v.fg401_fallback_source = -1;
    v.frame_gen_problem = nullptr;
}

void FrameGenFailure(int provider, const char* problem) {
    auto& v = Get();
    if (IsMlFrameGen(provider)) {
        ConfigureFg401Support(false, problem, provider);
        v.fg401_fallback_source = provider;
        v.fg401_fallback = FrameGenFsr3;
        v.frame_gen_problem = "ML-FG failed; FSR 3.1 FG active.";
    } else if (IsMlFrameGen(v.frame_gen)) {
        v.fg401_fallback_source = v.frame_gen.load();
        v.fg401_fallback = FrameGenOff;
        v.frame_gen_problem = "Frame generation unavailable; FG off.";
    } else {
        // Existing DLSS / FSR 3.1 fallback policy.
        static std::string kept;
        kept = problem ? problem : "Frame generation failed.";
        v.frame_gen_problem = kept.c_str();
        v.frame_gen = provider == FrameGenDlss ? FrameGenFsr3 : FrameGenOff;
    }
}

void ConfigureDlssSupport(bool available, const char* problem) {
    auto& v = Get();
    v.dlss_supported = available;
    static std::string kept;
    kept = problem ? problem : "";
    v.dlss_problem = available || kept.empty() ? nullptr : kept.c_str();
    if (v.upscaler == UpscalerDlss && !available) {
        std::printf("Upscaler: DLSS unavailable (%s); falling back to FSR 3.1\n",
                    kept.empty() ? "bbport_dlss.dll or nvngx_dlss.dll missing" : kept.c_str());
        v.upscaler = UpscalerFsr3;
    }
}

bool FixedRenderSession() {
    const char* size = std::getenv("BB_RENDER_RES");
    return size && size[0];
}

int RenderPreset() {
    const auto& v = Get();
    return FixedRenderSession() ? v.startup_preset :
        v.upscaler == UpscalerTaa ? NativeAA : v.preset.load();
}

bool ResolutionNeedsRestart() {
    const auto& v = Get();
    // TAA needs the live path (native guest targets): run.sh selects it on restart. Ultrawide
    // outputs patch the camera and HUD at startup.
    if (Ultrawide(v.output_res) != Ultrawide(v.startup_output_res)) return true;
    if (Ultrawide(v.output_res) && v.hud_position != v.startup_hud_position) return true;
    return FixedRenderSession() &&
        (v.preset != v.startup_preset || v.output_res != v.startup_output_res ||
         (v.upscaler == UpscalerOff) != (v.startup_upscaler == UpscalerOff) ||
         (v.upscaler == UpscalerTaa) != (v.startup_upscaler == UpscalerTaa));
}

bool FsrSettingsNeedRestart() {
    const auto& v = Get();
    const bool fsr_now = v.upscaler == UpscalerFsr3 || IsFsr4(v.upscaler);
    const bool fsr_start = v.startup_upscaler == UpscalerFsr3 || IsFsr4(v.startup_upscaler);
    return v.object_motion_cache != v.startup_object_motion_cache ||
        (v.object_motion_cache &&
         (fsr_now && v.object_motion) != (fsr_start && v.startup_object_motion));
}

void Save() {
    const auto& v = Get();
    FILE* file = std::fopen(Path(), "w");
    if (!file) {
        std::printf("Settings: cannot write %s\n", Path());
        return;
    }
    std::fprintf(file,
                 "# bbport settings (in-game menu: Insert / L3+R3)\n"
                 "upscaler=%s\npreset=%d\nsharpen=%d\nsharpness=%.2f\njitter=%d\n"
                 "reactive=%d\nobject_motion=%d\nreactive_scale=%.2f\nreactive_threshold=%.2f\nreactive_max=%.2f\n"
                 "debug_view=%d\nfsr4_auto_exposure=%d\nfsr4_invert_jitter=%d\n",
                 UpscalerName(v.upscaler), v.preset.load(), int(v.sharpen.load()),
                 v.sharpness.load(), int(v.jitter.load()), int(v.reactive.load()),
                 int(v.object_motion.load()),
                 v.reactive_scale.load(), v.reactive_threshold.load(), v.reactive_max.load(),
                 v.debug_view.load(), int(v.fsr4_auto_exposure.load()),
                 int(v.fsr4_invert_jitter.load()));
    for (const Readout& readout : Readouts) {
        std::fprintf(file, "%s=%d\n", readout.key, int((v.*readout.member).load()));
    }
    std::fprintf(file, "fsr411_backend=%s\nobject_motion_cache=%d\n",
                 v.fsr411_backend == Fsr411Dx12 ? "dx12" : "vk",
                 int(v.object_motion_cache.load()));
    std::fprintf(file, "fsr_compatibility_options=%d\n", int(v.fsr_compatibility_options.load()));
    std::fprintf(file, "fsr41_version=%s\n", v.fsr41_version == UpscalerFsr411 ? "fsr411" : "fsr412");
    // Read by patches.py at start.
    for (int e = 0; e < EffectCount; ++e) {
        std::fprintf(file, "%s=%d\n", Effects[e].key, int(v.effects[e].load()));
    }
    std::fprintf(file, "frame_gen=%s\nframe_gen_multiplier=%d\nframe_gen_fit_display=%d\n",
                 FrameGenName(v.frame_gen), v.frame_gen_multiplier.load(),
                 int(v.frame_gen_fit_display.load()));
    std::fprintf(file, "model_lod=%d\noutput_res=%dx%d\n", v.model_lod.load(),
                 OutputWidths[v.output_res], OutputHeights[v.output_res]);
    std::fprintf(file, "mute_unfocused=%d\n", int(v.mute_unfocused.load()));
    static constexpr const char* prompts[PromptsCount] = {"auto", "playstation", "keyboard", "xbox", "switch"};
    std::fprintf(file, "button_prompts=%s\n", prompts[std::clamp(v.button_prompts.load(), 0, PromptsCount - 1)]);
    // Read by run.py at start (hud_layout.py).
    std::fprintf(file, "hud_position=%d\n", v.hud_position.load());
    // Read by run.py and patches.py at start (ui_viewport).
    std::fprintf(file, "sharp_ui=%d\n", int(v.sharp_ui.load()));
    // Read by run.py at start (fov.py).
    std::fprintf(file, "fov=%d\n", v.fov.load());
    // Read by run.py at start (patches.py FPS preset, BB_FPS_LIMIT); the launcher shares them.
    std::fprintf(file, "fps_mode=%s\n", FpsModeNames[std::clamp(v.fps_mode.load(), 0, FpsModeCount - 1)]);
    if (v.frame_cap < 0) {
        std::fprintf(file, "frame_cap=\n");
    } else {
        std::fprintf(file, "frame_cap=%d\n", v.frame_cap.load());
    }
    // Read by run.sh at start.
    std::fprintf(file, "live_resolution=%s\n", v.live_resolution < 0 ? "auto"
                                                  : v.live_resolution ? "1" : "0");
    std::fclose(file);
}

float PresetScale(int preset) {
    static constexpr float scales[PresetCount] = {1.0f, 1.5f, 1.7f, 2.0f, 3.0f};
    return scales[std::clamp(preset, 0, PresetCount - 1)];
}

const char* PresetName(int preset) {
    static constexpr const char* names[PresetCount] = {"Native AA", "Quality", "Balanced",
                                                       "Performance", "Ultra Performance"};
    return names[std::clamp(preset, 0, PresetCount - 1)];
}

const char* FrameGenName(int frame_gen) {
    static constexpr const char* names[FrameGenCount] = {"off", "fsr3", "dlss", "fsr401", "fsr402"};
    return names[std::clamp(frame_gen, 0, FrameGenCount - 1)];
}

const char* UpscalerName(int upscaler) {
    static constexpr const char* names[UpscalerCount] = {"off", "fsr3", "fsr4", "fsr411", "taa", "dlss", "fsr412"};
    return names[std::clamp(upscaler, 0, UpscalerCount - 1)];
}

} // namespace BbSettings

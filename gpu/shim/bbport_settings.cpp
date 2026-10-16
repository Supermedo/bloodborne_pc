// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_settings.h"
#include "bbport_neural_settings.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <string_view>

namespace BbSettings {
const char* FgBackendName(int backend) {
    static constexpr const char* names[FgBackendCount] = {"dlss", "fsr"};
    return names[std::clamp(backend, 0, FgBackendCount - 1)];
}

const char* ReflexModeName(int mode) {
    static constexpr const char* names[ReflexModeCount] = {"off", "on", "boost"};
    return names[std::clamp(mode, 0, ReflexModeCount - 1)];
}

const char* FrameGenName(int factor) {
    switch (factor) {
    case 2: return "2x";
    case 3: return "3x";
    case 4: return "4x";
    default: return "off";
    }
}

int ParseFrameGen(const std::string& value) {
    if (value == "2x" || value == "2") return 2;
    if (value == "3x" || value == "3") return 3;
    if (value == "4x" || value == "4") return 4;
    return 0; // default/off
}

int ParseFgBackend(const std::string& value) {
    if (value == "fsr" || value == "1") return FgFsr;
    return FgDlss; // default/0
}

int ParseReflex(const std::string& value) {
    if (value == "off" || value == "0") return ReflexOff;
    if (value == "boost" || value == "2") return ReflexBoost;
    return ReflexOn; // default/1/"on"
}


namespace {

const char* Path() {
    const char* env = std::getenv("BB_CONFIG");
    return env && env[0] ? env : "bbport.ini";
}

float Clamp(float v, float lo, float hi) {
    return std::clamp(v, lo, hi);
}

void Set(Values& v, const std::string& key, const std::string& value) {
    const float f = float(std::atof(value.c_str()));
    const int i = std::atoi(value.c_str());
    if (key == "upscaler") {
        for (int u = 0; u < UpscalerCount; ++u) {
            if (value == UpscalerName(u)) {
                v.upscaler = u;
            }
        }
    } else if (key == "frame_generation") {
        int fg = ParseFrameGen(value);
        if (v.fg_backend == FgFsr && fg > 2) {
            fg = 2; // FSR FG supports Off / 2x only
        }
        v.frame_generation = fg;
    } else if (key == "fg_backend") {
        int b = ParseFgBackend(value);
        v.fg_backend = b;
        if (b == FgFsr && v.frame_generation > 2) {
            v.frame_generation = 2; // clamp when switching to FSR
        }
    } else if (key == "reflex") {
        v.reflex = ParseReflex(value);
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
    } else if (key == "reactive_scale") {
        v.reactive_scale = Clamp(f, 0.0f, 16.0f);
    } else if (key == "reactive_threshold") {
        v.reactive_threshold = Clamp(f, 0.0f, 1.0f);
    } else if (key == "reactive_max") {
        v.reactive_max = Clamp(f, 0.0f, 1.0f);
    } else if (key == "debug_view") {
        v.debug_view = std::clamp(i, 0, DebugViewCount - 1);
    } else if (key == "show_fps") {
        v.show_fps = i != 0;
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
    } else if (key == "output_res") {
        for (int r = 0; r < OutputCustom; ++r) {
            if (value == std::to_string(OutputWidths[r]) + "x" + std::to_string(OutputHeights[r])) {
                v.output_res = r;
                return;
            }
        }
        const auto separator = value.find('x');
        if (separator == std::string::npos) return;
        int w = 0, h = 0;
        const char* begin = value.data();
        const char* split = begin + separator;
        const char* end = begin + value.size();
        const auto width = std::from_chars(begin, split, w);
        const auto height = std::from_chars(split + 1, end, h);
        if (width.ec == std::errc{} && width.ptr == split &&
            height.ec == std::errc{} && height.ptr == end &&
            w >= 256 && w <= 7680 && h >= 144 && h <= 4320 && !(w & 1) && !(h & 1)) {
            OutputWidths[OutputCustom] = w;
            OutputHeights[OutputCustom] = h;
            v.output_res = OutputCustom;
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
    static bool initialized = false;
    v.frame_generation = 0;
    v.fg_backend = FgDlss;
    v.reflex = ReflexOn;
    for (int e = 0; e < EffectCount; ++e) {
        v.effects[e] = Effects[e].default_on;
    }
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
    // Neural rendering keeps its own nr_* keys in the same file (bbport_neural_settings.cpp).
    BbNeural::Load(Path());
    // Environment overrides (scripts, A/B tests).
    if (const char* env = std::getenv("BB_UPSCALER")) {
        v.upscaler = UpscalerOff;
        for (int u = 0; u < UpscalerCount; ++u) {
            if (std::strcmp(env, UpscalerName(u)) == 0) v.upscaler = u;
        }
    }
    if (const char* env = std::getenv("BB_FG_BACKEND")) {
        Set(v, "fg_backend", env);
    }
    if (const char* env = std::getenv("BB_FRAME_GENERATION")) {
        Set(v, "frame_generation", env);
    }
    if (const char* env = std::getenv("BB_REFLEX")) {
        Set(v, "reflex", env);
    }
    const std::pair<const char*, const char*> env_keys[] = {
        {"BB_FSR_SHARPNESS", "sharpness"},        {"BB_JITTER", "jitter"},
        {"BB_REACTIVE", "reactive"},              {"BB_REACTIVE_SCALE", "reactive_scale"},
        {"BB_REACTIVE_THRESHOLD", "reactive_threshold"}, {"BB_REACTIVE_MAX", "reactive_max"},
        {"BB_UPSCALE_PRESET", "preset"},            {"BB_OBJECT_MOTION", "object_motion"},
        {"BB_OUTPUT_RES", "output_res"},
    };
    for (const auto& [env, key] : env_keys) {
        if (const char* value = std::getenv(env)) {
            Set(v, key, value);
        }
    }
    if (!initialized) {
        v.startup_frame_generation = v.frame_generation;
        v.startup_fg_backend = v.fg_backend;
        v.startup_reflex = v.reflex;
        initialized = true;
    }
    v.startup_preset = v.preset;
    v.startup_upscaler = v.upscaler;
    v.startup_object_motion = v.object_motion;
    for (int e = 0; e < EffectCount; ++e) {
        v.startup_effects[e] = v.effects[e];
    }
    v.startup_model_lod = v.model_lod;
    v.startup_output_res = v.output_res;
    v.startup_hud_position = v.hud_position;
    v.startup_live_resolution = v.live_resolution;
}

void ConfigureUpscalerSupport(bool fsr4, bool fsr411) {
    auto& v = Get();
    v.fsr4_supported = fsr4;
    v.fsr411_supported = fsr4 && fsr411;
    const int requested = v.upscaler;
    if ((requested == UpscalerFsr4 && !v.fsr4_supported) ||
        (requested == UpscalerFsr411 && !v.fsr411_supported)) {
        v.fsr4_problem = "GPU does not support the selected FSR 4 shaders; using FSR 3.1";
        std::printf("Upscaler: %s unsupported on this GPU; falling back to FSR 3.1 before the first frame\n",
                    UpscalerName(requested));
        v.upscaler = UpscalerFsr3;
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

void ConfigureReflexSupport(bool supported) {
    auto& v = Get();
    v.reflex_supported = supported;
    if (!supported && v.reflex != ReflexOff) {
        v.reflex = ReflexOff;
    }
}

bool FrameGenNeedsRestart() {
    const auto& v = Get();
    return v.frame_generation != v.startup_frame_generation ||
           v.fg_backend != v.startup_fg_backend ||
           v.reflex != v.startup_reflex;
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

bool AspectNeedsRestart() {
    const auto& v = Get();
    const int a = v.output_res, b = v.startup_output_res;
    return int64_t(OutputWidths[a]) * OutputHeights[b] !=
           int64_t(OutputWidths[b]) * OutputHeights[a];
}

bool ResolutionNeedsRestart() {
    const auto& v = Get();
    // Wide outputs patch the camera and the HUD layer (patches.py, hud_layout.py) at startup, and
    // two wide outputs can share an aspect ratio, so compare the outputs themselves.
    if ((Ultrawide(v.output_res) || Ultrawide(v.startup_output_res)) &&
        (v.output_res != v.startup_output_res || v.hud_position != v.startup_hud_position)) {
        return true;
    }
    // TAA needs the live path (native guest targets): run.sh selects it on restart.
    // An aspect change needs the camera and UI viewport patches rebuilt on restart.
    return AspectNeedsRestart() || (FixedRenderSession() &&
        (v.preset != v.startup_preset || v.output_res != v.startup_output_res ||
         (v.upscaler == UpscalerOff) != (v.startup_upscaler == UpscalerOff) ||
         (v.upscaler == UpscalerTaa) != (v.startup_upscaler == UpscalerTaa)));
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
                 "upscaler=%s\nframe_generation=%s\nfg_backend=%s\nreflex=%s\n"
                 "preset=%d\nsharpen=%d\nsharpness=%.2f\njitter=%d\n"
                 "reactive=%d\nobject_motion=%d\nreactive_scale=%.2f\nreactive_threshold=%.2f\nreactive_max=%.2f\n"
                 "debug_view=%d\nshow_fps=%d\nfsr4_auto_exposure=%d\nfsr4_invert_jitter=%d\n",
                 UpscalerName(v.upscaler), FrameGenName(v.frame_generation.load()),
                 FgBackendName(v.fg_backend.load()), ReflexModeName(v.reflex.load()),
                 v.preset.load(), int(v.sharpen.load()),
                 v.sharpness.load(), int(v.jitter.load()), int(v.reactive.load()),
                 int(v.object_motion.load()),
                 v.reactive_scale.load(), v.reactive_threshold.load(), v.reactive_max.load(),
                 v.debug_view.load(), int(v.show_fps.load()),
                 int(v.fsr4_auto_exposure.load()), int(v.fsr4_invert_jitter.load()));
    // Read by patches.py at start.
    for (int e = 0; e < EffectCount; ++e) {
        std::fprintf(file, "%s=%d\n", Effects[e].key, int(v.effects[e].load()));
    }
    std::fprintf(file, "model_lod=%d\noutput_res=%dx%d\n", v.model_lod.load(),
                 OutputWidths[v.output_res], OutputHeights[v.output_res]);
    // Read by run.py at start (hud_layout.py).
    std::fprintf(file, "hud_position=%d\n", v.hud_position.load());
    // Read by run.sh at start.
    std::fprintf(file, "live_resolution=%s\n", v.live_resolution < 0 ? "auto"
                                                  : v.live_resolution ? "1" : "0");
    // The neural-rendering keys of the same file: Save() rewrote it, so append them again.
    BbNeural::Append(file);
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

const char* UpscalerName(int upscaler) {
    static constexpr const char* names[UpscalerCount] = {"off", "fsr3", "fsr4", "fsr411", "taa", "dlss"};
    return names[std::clamp(upscaler, 0, UpscalerCount - 1)];
}

} // namespace BbSettings

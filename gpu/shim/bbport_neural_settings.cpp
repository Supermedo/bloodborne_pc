// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_neural_settings.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace BbNeural {

namespace {

std::mutex settings_mutex;
Config settings; // guarded by settings_mutex
std::mutex status_mutex;
std::string status_text = "Disabled"; // guarded by status_mutex

/// The ini parser is case-insensitive and trims: the launcher, the menu and a hand-edited file all
/// write the same tokens, but "Ratio" or " nr_resolve = Ratio " must not silently fall back.
std::string Trim(std::string text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::string Lower(std::string text) {
    for (char &c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

/// NaN and infinities pass through std::clamp; they would reach the shader dimensions as garbage,
/// so a non-finite value falls back to the field's default before clamping.
float Finite(float value, float fallback, float lo, float hi) {
    return std::isfinite(value) ? std::clamp(value, lo, hi) : fallback;
}

bool ParseBool(const std::string &value) {
    const std::string text = Lower(value);
    return text == "1" || text == "true" || text == "on" || text == "yes";
}

int ParseResolve(const std::string &value) {
    const std::string text = Lower(value);
    if (text == "residual" || text == "1")
        return ResolveResidual;
    if (text == "ratio" || text == "2")
        return ResolveRatio;
    return ResolveAuto;
}

int ParseStage(const std::string &value) {
    const std::string text = Lower(value);
    if (text == "pre" || text == "pre-upscale" || text == "0")
        return PreUpscale;
    if (text == "present" || text == "2")
        return Present;
    return PostUpscale;
}

bool ApplyPassKey(Pass &pass, const std::string &field, const std::string &value) {
    const float f = float(std::atof(value.c_str()));
    const int i = std::atoi(value.c_str());
    if (field == "enabled")
        pass.enabled = ParseBool(value);
    else if (field == "stage")
        pass.stage = ParseStage(value);
    else if (field == "style")
        pass.style = i;
    else if (field == "intensity")
        pass.intensity = f;
    else if (field == "structure")
        pass.structure = f;
    else if (field == "tone")
        pass.tone = f;
    else if (field == "auto_skin")
        pass.auto_skin = ParseBool(value);
    else if (field == "skin_structure")
        pass.skin_structure = f;
    else
        return false;
    return true;
}

/// Applies one ini key; false when the key is not a neural-rendering key.
bool ApplyKey(Config &config, const std::string &key, const std::string &value) {
    const float f = float(std::atof(value.c_str()));
    const int i = std::atoi(value.c_str());
    if (key == "nr_enabled")
        config.enabled = ParseBool(value);
    else if (key == "nr_preset")
        config.preset = i;
    else if (key == "nr_input_scale")
        config.input_scale = f;
    else if (key == "nr_resolve")
        config.resolve = ParseResolve(value);
    else if (key == "nr_ui_correction")
        config.ui_correction = ParseBool(value);
    else if (key == "nr_hotkey")
        config.hotkey = i;
    else if (key == "nr_passes")
        config.pass_count = i;
    else if (key == "nr_transfer_strength")
        config.transfer_strength = f;
    else if (key == "nr_colour_strength")
        config.colour_strength = f;
    else if (key == "nr_max_ratio")
        config.max_ratio = f;
    else if (key == "nr_white_point")
        config.white_point = f;
    else if (key.size() > 7 && key.compare(0, 7, "nr_pass") == 0 &&
             std::isdigit(static_cast<unsigned char>(key[7]))) {
        const auto separator = key.find('_', 7);
        if (separator == std::string::npos)
            return false;
        const int index = std::atoi(key.substr(7, separator - 7).c_str());
        if (index < 1 || index > MaxPasses)
            return false;
        return ApplyPassKey(config.passes[index - 1], key.substr(separator + 1), value);
    } else {
        return false;
    }
    return true;
}

} // namespace

bool RuntimeSupportsOptimized() {
#ifdef _WIN32
    std::wstring executable(32768, L'\0');
    const auto count = GetModuleFileNameW(nullptr, executable.data(), DWORD(executable.size()));
    if (!count || count >= executable.size()) return false;
    executable.resize(count);
    auto directory = std::filesystem::path(executable).parent_path();
    if (const wchar_t *env = _wgetenv(L"BB_NR_RUNTIME"); env && *env) {
        directory = std::filesystem::path(env);
        if (_wcsicmp(directory.extension().c_str(), L".dll") == 0) directory = directory.parent_path();
    }
    const auto path = directory / L"nvngx_dlssnr.dll";
    HMODULE library = LoadLibraryExW(L"version.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!library) return false;
    const auto supported = [&] {
        const auto size_fn = reinterpret_cast<decltype(&GetFileVersionInfoSizeW)>(
            GetProcAddress(library, "GetFileVersionInfoSizeW"));
        const auto info_fn = reinterpret_cast<decltype(&GetFileVersionInfoW)>(
            GetProcAddress(library, "GetFileVersionInfoW"));
        const auto query_fn = reinterpret_cast<decltype(&VerQueryValueW)>(
            GetProcAddress(library, "VerQueryValueW"));
        if (!size_fn || !info_fn || !query_fn) return false;
        DWORD ignored{};
        const DWORD size = size_fn(path.c_str(), &ignored);
        if (!size) return false;
        std::vector<DWORD> buffer((size + sizeof(DWORD) - 1) / sizeof(DWORD));
        if (!info_fn(path.c_str(), 0, size, buffer.data())) return false;
        VS_FIXEDFILEINFO *info{};
        UINT length{};
        if (!query_fn(buffer.data(), L"\\", reinterpret_cast<void **>(&info), &length) ||
            !info || length < sizeof(*info) || info->dwSignature != 0xfeef04bd) return false;
        return SupportsOptimizedVersion(HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                                        HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
    }();
    FreeLibrary(library);
    return supported;
#else
    return false;
#endif
}

int EffectivePreset(int preset, bool optimized_available) {
    preset = std::clamp(preset, 0, PresetCount - 1);
    return preset == Optimized && !optimized_available ? Original : preset;
}

Config Defaults() { return Config{}; }

Config Sanitize(Config config) {
    config.preset = std::clamp(config.preset, 0, PresetCount - 1);
    config.input_scale = Finite(config.input_scale, 1.0f, InputScaleMin, 1.0f);
    config.resolve = std::clamp(config.resolve, 0, ResolveCount - 1);
    config.pass_count = std::clamp(config.pass_count, 1, MaxPasses);
    config.transfer_strength = Finite(config.transfer_strength, 1.0f, 0.0f, TransferStrengthMax);
    config.colour_strength = Finite(config.colour_strength, 1.0f, 0.0f, ColourStrengthMax);
    config.max_ratio = Finite(config.max_ratio, 2.0f, MaxRatioMin, MaxRatioMax);
    config.white_point = Finite(config.white_point, 1.0f, WhitePointMin, WhitePointMax);
    if (config.hotkey < 0)
        config.hotkey = 0;
    for (auto &pass : config.passes) {
        pass.stage = std::clamp(pass.stage, 0, StageCount - 1);
        pass.style = std::clamp(pass.style, 0, StyleCount - 1);
        pass.intensity = Finite(pass.intensity, 1.0f, 0.0f, StyleParamMax);
        pass.structure = Finite(pass.structure, 1.0f, 0.0f, StyleParamMax);
        pass.tone = Finite(pass.tone, 1.0f, 0.0f, StyleParamMax);
        pass.skin_structure = Finite(pass.skin_structure, 1.0f, SkinStructureMin, StyleParamMax);
    }
    return config;
}

Config Snapshot() {
    const std::lock_guard<std::mutex> lock(settings_mutex);
    return settings;
}

void Set(const Config &config) {
    const std::lock_guard<std::mutex> lock(settings_mutex);
    settings = Sanitize(config);
}

bool Toggle() {
    const std::lock_guard<std::mutex> lock(settings_mutex);
    settings.enabled = !settings.enabled;
    return settings.enabled;
}

void Load(const char *path) {
    Config config = Defaults();
    if (path && path[0]) {
        if (std::FILE *file = std::fopen(path, "r")) {
            char line[512];
            while (std::fgets(line, sizeof(line), file)) {
                std::string text{line};
                const auto equals = text.find('=');
                if (equals == std::string::npos)
                    continue;
                const std::string key = Trim(text.substr(0, equals));
                if (key.empty() || key[0] == '#')
                    continue;
                ApplyKey(config, key, Trim(text.substr(equals + 1)));
            }
            std::fclose(file);
        }
    }
    if (const char *env = std::getenv("BB_NEURAL")) {
        config.enabled = ParseBool(env);
    }
    Set(config);
}

void Append(std::FILE *file) {
    if (!file)
        return;
    const Config config = Snapshot();
    std::fprintf(
        file,
        "# Neural rendering (DLSS NR): in-game menu (Insert / L3+R3), Graphics page of the "
        "launcher\n"
        "nr_enabled=%d\nnr_preset=%d\nnr_input_scale=%.2f\nnr_resolve=%s\nnr_ui_correction=%d\n"
        "nr_hotkey=%d\nnr_passes=%d\n"
        "nr_transfer_strength=%.2f\nnr_colour_strength=%.2f\nnr_max_ratio=%.2f\nnr_white_point=%."
        "2f\n",
        int(config.enabled), config.preset, config.input_scale, ResolveName(config.resolve),
        int(config.ui_correction), config.hotkey, config.pass_count, config.transfer_strength,
        config.colour_strength, config.max_ratio, config.white_point);
    for (int i = 0; i < MaxPasses; ++i) {
        const Pass &pass = config.passes[i];
        std::fprintf(file,
                     "nr_pass%d_enabled=%d\nnr_pass%d_stage=%s\nnr_pass%d_style=%d\n"
                     "nr_pass%d_intensity=%.2f\nnr_pass%d_structure=%.2f\nnr_pass%d_tone=%.2f\n"
                     "nr_pass%d_auto_skin=%d\nnr_pass%d_skin_structure=%.2f\n",
                     i + 1, int(pass.enabled), i + 1, StageName(pass.stage), i + 1, pass.style,
                     i + 1, pass.intensity, i + 1, pass.structure, i + 1, pass.tone, i + 1,
                     int(pass.auto_skin), i + 1, pass.skin_structure);
    }
}

void SetStatus(const char *text) {
    const std::lock_guard<std::mutex> lock(status_mutex);
    status_text = text ? text : "";
}

std::string Status() {
    const std::lock_guard<std::mutex> lock(status_mutex);
    return status_text;
}

const char *StageName(int stage) {
    static constexpr const char *names[StageCount] = {"pre-upscale", "post-upscale", "present"};
    return names[std::clamp(stage, 0, StageCount - 1)];
}

const char *ResolveName(int resolve) {
    static constexpr const char *names[ResolveCount] = {"auto", "residual", "ratio"};
    return names[std::clamp(resolve, 0, ResolveCount - 1)];
}

const char *PresetName(int preset) {
    static constexpr const char *names[PresetCount] = {"Automatic", "Original", "Optimized"};
    return names[std::clamp(preset, 0, PresetCount - 1)];
}

const char *StyleName(int style) {
    static constexpr const char *names[StyleCount] = {"style 0", "style 1", "style 2"};
    return names[std::clamp(style, 0, StyleCount - 1)];
}

} // namespace BbNeural

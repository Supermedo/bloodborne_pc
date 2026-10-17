// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: neural rendering (DLSS NR) settings, kept apart from BbSettings so the feature can be
// added or removed without touching the upscaler and frame-generation code paths. The in-game
// menu (bbport_overlay.cpp) and the launcher write the same nr_* keys of bbport.ini; BbSettings::
// Load and Save call BbNeural::Load and Append so the port still uses a single settings file.
//
// The header deliberately includes no SDL: the settings tests compile it with only gpu/shim on the
// include path, so Config::hotkey is the numeric SDL scancode (DefaultHotkey) and the overlay maps
// it through SDL_SCANCODE_*.

#pragma once

#include <array>
#include <cstdio>
#include <string>

namespace BbNeural {

/// Pass chain: one entry per neural-rendering pass, executed in order within its stage.
constexpr int MaxPasses = 4;
enum Stage : int { PreUpscale = 0, PostUpscale = 1, Present = 2, StageCount };
enum Resolve : int { ResolveAuto = 0, ResolveResidual = 1, ResolveRatio = 2, ResolveCount };
enum Preset : int { Automatic = 0, Original = 1, Optimized = 2, PresetCount };
enum Style : int { Style0 = 0, Style1 = 1, Style2 = 2, StyleCount };

/// SDL_SCANCODE_KP_MINUS (SDL3 SDL_scancode.h:194). The overlay compares it with SDL_SCANCODE_*.
constexpr int DefaultHotkey = 86;

/// Slider ranges follow the reference controls and the compositor's effective range.
constexpr float StyleParamMax = 1.0f;
constexpr float InputScaleMin = 0.25f;
constexpr float SkinStructureMin = -1.0f;
constexpr float TransferStrengthMax = 1.0f;
constexpr float ColourStrengthMax = 1.0f;
constexpr float MaxRatioMin = 1.0f, MaxRatioMax = 8.0f;
constexpr float WhitePointMin = 0.01f, WhitePointMax = 100.0f;

struct Pass {
    bool enabled = true;
    int stage = PostUpscale;
    int style = Style0;
    float intensity = 1.0f; ///< 0..StyleParamMax
    float structure = 1.0f; ///< 0..StyleParamMax
    float tone = 1.0f;      ///< 0..StyleParamMax
    bool auto_skin = false;
    float skin_structure = 1.0f; ///< SkinStructureMin..StyleParamMax
};

struct Config {
    bool enabled = false;
    int preset = Automatic;   ///< Automatic / Original / Optimized model
    float input_scale = 1.0f; ///< InputScaleMin..1, 1 is no downsample
    int resolve = ResolveAuto;
    bool ui_correction = true;
    int hotkey = DefaultHotkey; ///< SDL scancode, 0 disables the toggle
    int pass_count = 1;         ///< 1..MaxPasses
    /// Advanced Ratio resolve (ResolveRatio): luma-ratio transfer, colour transfer, the ratio cap
    /// and the white point of the tone mapping. Ignored by Auto and Residual.
    float transfer_strength = 1.0f; ///< 0..TransferStrengthMax
    float colour_strength = 1.0f;   ///< 0..ColourStrengthMax
    float max_ratio = 2.0f;         ///< MaxRatioMin..MaxRatioMax
    float white_point = 1.0f;       ///< WhitePointMin..WhitePointMax
    std::array<Pass, MaxPasses> passes{};
};

/// Optimized is available from model DLL 310.9.16.0. Unknown versions are conservative.
constexpr bool SupportsOptimizedVersion(unsigned major, unsigned minor, unsigned patch, unsigned build) {
    return std::array<unsigned, 4>{major, minor, patch, build} >=
           std::array<unsigned, 4>{310, 9, 16, 0};
}
/// Uses BB_NR_RUNTIME, or nvngx_dlssnr.dll beside the renderer. Reads metadata without loading it.
bool RuntimeSupportsOptimized();
/// A saved Optimized choice on an old/unknown runtime uses Original without overwriting the setting.
int EffectivePreset(int preset, bool optimized_available);

Config Defaults();
/// Every field clamped into its documented range.
Config Sanitize(Config config);

/// Current settings. Safe to call from the render thread while the menu edits them.
Config Snapshot();
void Set(const Config &config);
/// Flips Config::enabled (the toggle hotkey); returns the new value.
bool Toggle();

/// Reads the nr_* keys of an ini file; called once at start from BbSettings::Load.
void Load(const char *path);
/// Writes the nr_* keys to an open ini file; called from BbSettings::Save before it closes.
void Append(std::FILE *file);

/// Human-readable state for the menu (set by the renderer): "Disabled" until told otherwise.
void SetStatus(const char *text);
std::string Status();

const char *StageName(int stage);
const char *ResolveName(int resolve);
const char *PresetName(int preset);
const char *StyleName(int style);

} // namespace BbNeural

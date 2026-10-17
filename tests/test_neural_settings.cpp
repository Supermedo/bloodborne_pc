// SPDX-License-Identifier: GPL-2.0-or-later
// Neural-rendering settings (BbNeural): defaults, ini roundtrip, pass chain, clamping and the
// status line. Runs without Vulkan or ImGui; build target neural-settings-test.
#include "bbport_neural_settings.h"

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <string>

namespace {

bool Near(float a, float b) { return std::fabs(a - b) < 0.005f; }

std::string ReadFile(const std::filesystem::path &path) {
    std::string text;
    if (FILE *file = std::fopen(path.string().c_str(), "rb")) {
        char buffer[512];
        size_t read = 0;
        while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
            text.append(buffer, read);
        }
        std::fclose(file);
    }
    return text;
}

} // namespace

int main() {
    using namespace BbNeural;
    assert(!SupportsOptimizedVersion(310, 8, 0, 0));
    assert(!SupportsOptimizedVersion(310, 9, 15, 99));
    assert(SupportsOptimizedVersion(310, 9, 16, 0));
    assert(SupportsOptimizedVersion(311, 0, 0, 0));
    assert(EffectivePreset(Automatic, false) == Automatic);
    assert(EffectivePreset(Original, false) == Original);
    assert(EffectivePreset(Optimized, false) == Original);
    assert(EffectivePreset(Optimized, true) == Optimized);
    assert(std::string(PresetName(Automatic)) == "Automatic");
    assert(std::string(PresetName(Original)) == "Original");
    assert(std::string(PresetName(Optimized)) == "Optimized");
    if (const char *expected = std::getenv("BB_NR_EXPECT_OPTIMIZED"))
        assert(RuntimeSupportsOptimized() == (expected[0] == '1'));
    const auto path =
        std::filesystem::temp_directory_path() /
        ("bb-neural-settings-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".ini");
#ifdef _WIN32
    _putenv_s("BB_NEURAL", "");
#else
    unsetenv("BB_NEURAL");
#endif

    // 1. Defaults of a fresh file: off, one post-upscale pass, keypad-minus hotkey.
    {
        FILE *file = std::fopen(path.string().c_str(), "w");
        assert(file);
        std::fclose(file);
        Load(path.string().c_str());
        const Config config = Snapshot();
        assert(!config.enabled);
        assert(config.preset == 0);
        assert(Near(config.input_scale, 1.0f));
        assert(config.resolve == ResolveAuto);
        assert(config.ui_correction);
        assert(config.hotkey == DefaultHotkey);
        assert(config.pass_count == 1);
        assert(Near(config.transfer_strength, 1.0f) && Near(config.colour_strength, 1.0f));
        assert(Near(config.max_ratio, 2.0f) && Near(config.white_point, 1.0f));
        assert(config.passes[0].enabled);
        assert(config.passes[0].stage == PostUpscale);
        assert(config.passes[0].style == Style0);
        assert(Near(config.passes[0].intensity, 1.0f));
        assert(!config.passes[0].auto_skin);
        assert(Status() == "Disabled");
    }

    // 2. Roundtrip: every nr_* key is read, and Append writes them all back.
    {
        FILE *file = std::fopen(path.string().c_str(), "w");
        assert(file);
        std::fprintf(file, "nr_enabled=1\nnr_preset=1\nnr_input_scale=0.50\nnr_resolve=ratio\n"
                           "nr_ui_correction=0\nnr_hotkey=87\nnr_passes=3\n"
                           "nr_transfer_strength=0.75\nnr_colour_strength=0.75\n"
                           "nr_max_ratio=4.00\nnr_white_point=2.50\n"
                           "nr_pass2_enabled=0\nnr_pass2_stage=present\nnr_pass2_style=1\n"
                           "nr_pass2_intensity=0.50\nnr_pass2_structure=0.50\nnr_pass2_tone=0.25\n"
                           "nr_pass2_auto_skin=1\nnr_pass2_skin_structure=-1.00\n");
        std::fclose(file);
        Load(path.string().c_str());
        Config config = Snapshot();
        assert(config.enabled);
        assert(config.preset == 1);
        assert(Near(config.input_scale, 0.50f));
        assert(config.resolve == ResolveRatio);
        assert(!config.ui_correction);
        assert(config.hotkey == 87);
        assert(config.pass_count == 3);
        assert(Near(config.transfer_strength, 0.75f) && Near(config.colour_strength, 0.75f));
        assert(Near(config.max_ratio, 4.0f) && Near(config.white_point, 2.50f));
        assert(!config.passes[1].enabled);
        assert(config.passes[1].stage == Present);
        assert(config.passes[1].style == Style1);
        assert(Near(config.passes[1].intensity, 0.50f));
        assert(Near(config.passes[1].structure, 0.50f));
        assert(Near(config.passes[1].tone, 0.25f));
        assert(config.passes[1].auto_skin);
        assert(Near(config.passes[1].skin_structure, -1.0f));

        // Save truncates the file in BbSettings::Save, so Append must rewrite every key.
        FILE *out = std::fopen(path.string().c_str(), "w");
        assert(out);
        Append(out);
        std::fclose(out);
        Set(Defaults());
        Load(path.string().c_str());
        config = Snapshot();
        assert(config.enabled && config.preset == 1 && config.pass_count == 3);
        assert(config.resolve == ResolveRatio && config.hotkey == 87);
        assert(!config.passes[1].enabled && config.passes[1].stage == Present);
        assert(config.passes[1].auto_skin && Near(config.passes[1].skin_structure, -1.0f));
    }

    // 2b. Every pass field of every pass survives Append + Load, for each resolve and stage token,
    // and the file keeps the canonical lower-case tokens the parser reads back.
    {
        const int resolve_values[] = {ResolveAuto, ResolveResidual, ResolveRatio};
        const char *resolve_tokens[] = {"auto", "residual", "ratio"};
        const int stage_values[] = {PreUpscale, PostUpscale, Present};
        const char *stage_tokens[] = {"pre-upscale", "post-upscale", "present"};
        for (int r = 0; r < ResolveCount; ++r) {
            for (int s = 0; s < StageCount; ++s) {
                Config written = Defaults();
                written.enabled = true;
                written.preset = r;
                written.input_scale = 0.35f;
                written.resolve = resolve_values[r];
                written.ui_correction = false;
                written.hotkey = 87;
                written.pass_count = MaxPasses;
                written.transfer_strength = 0.75f;
                written.colour_strength = 0.25f;
                written.max_ratio = 6.5f;
                written.white_point = 12.5f;
                for (int i = 0; i < MaxPasses; ++i) {
                    Pass &pass = written.passes[i];
                    pass.enabled = (i % 2) == 0;
                    pass.stage = stage_values[s];
                    pass.style = i % StyleCount;
                    pass.intensity = 0.25f + 0.25f * float(i);
                    pass.structure = 0.75f - 0.25f * float(i);
                    pass.tone = 0.5f + 0.1f * float(i);
                    pass.auto_skin = (i % 2) != 0;
                    pass.skin_structure = -0.5f + 0.25f * float(i);
                }
                Set(written);
                FILE *out = std::fopen(path.string().c_str(), "w");
                assert(out);
                Append(out);
                std::fclose(out);
                const std::string text = ReadFile(path);
                assert(text.find(std::string("nr_resolve=") + resolve_tokens[r]) !=
                       std::string::npos);
                assert(text.find(std::string("nr_pass1_stage=") + stage_tokens[s]) !=
                       std::string::npos);
                assert(text.find("nr_pass4_stage=") != std::string::npos);
                Set(Defaults());
                Load(path.string().c_str());
                const Config read = Snapshot();
                assert(read.enabled && read.preset == r);
                assert(Near(read.input_scale, 0.35f));
                assert(read.resolve == resolve_values[r]);
                assert(!read.ui_correction);
                assert(read.hotkey == 87);
                assert(read.pass_count == MaxPasses);
                assert(Near(read.transfer_strength, 0.75f) && Near(read.colour_strength, 0.25f));
                assert(Near(read.max_ratio, 6.5f) && Near(read.white_point, 12.5f));
                for (int i = 0; i < MaxPasses; ++i) {
                    const Pass &pass = read.passes[i];
                    assert(pass.enabled == ((i % 2) == 0));
                    assert(pass.stage == stage_values[s]);
                    assert(pass.style == i % StyleCount);
                    assert(Near(pass.intensity, 0.25f + 0.25f * float(i)));
                    assert(Near(pass.structure, 0.75f - 0.25f * float(i)));
                    assert(Near(pass.tone, 0.5f + 0.1f * float(i)));
                    assert(pass.auto_skin == ((i % 2) != 0));
                    assert(Near(pass.skin_structure, -0.5f + 0.25f * float(i)));
                }
            }
        }
    }

    // 3. Clamping: the menu and the ini cannot move a field out of its documented range.
    {
        Config config = Defaults();
        config.preset = 7;
        config.input_scale = 5.0f;
        config.resolve = 9;
        config.pass_count = 9;
        config.transfer_strength = 9.0f;
        config.colour_strength = -2.0f;
        config.max_ratio = 0.0f;
        config.white_point = 1000.0f;
        config.hotkey = -3;
        for (auto &pass : config.passes) {
            pass.stage = 7;
            pass.style = 9;
            pass.intensity = 9.0f;
            pass.structure = -4.0f;
            pass.tone = 3.0f;
            pass.skin_structure = -9.0f;
        }
        config = Sanitize(config);
        assert(config.preset == Optimized);
        assert(Near(config.input_scale, 1.0f));
        assert(config.resolve == ResolveCount - 1);
        assert(config.pass_count == MaxPasses);
        assert(Near(config.transfer_strength, TransferStrengthMax));
        assert(Near(config.colour_strength, 0.0f));
        assert(Near(config.max_ratio, MaxRatioMin));
        assert(Near(config.white_point, WhitePointMax));
        assert(config.hotkey == 0);
        for (const auto &pass : config.passes) {
            assert(pass.stage == StageCount - 1);
            assert(pass.style == StyleCount - 1);
            assert(Near(pass.intensity, StyleParamMax));
            assert(Near(pass.structure, 0.0f));
            assert(Near(pass.tone, StyleParamMax));
            assert(Near(pass.skin_structure, SkinStructureMin));
        }
    }

    // 3b. NaN and infinities fall back to the defaults instead of passing through std::clamp, so a
    // broken value can never reach the renderer's shader dimensions.
    {
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float inf = std::numeric_limits<float>::infinity();
        Config config = Defaults();
        config.input_scale = nan;
        config.transfer_strength = inf;
        config.colour_strength = -inf;
        config.max_ratio = nan;
        config.white_point = inf;
        config.passes[0].intensity = nan;
        config.passes[0].structure = inf;
        config.passes[0].tone = -inf;
        config.passes[0].skin_structure = nan;
        config.passes[1].intensity = 0.5f;
        config = Sanitize(config);
        assert(Near(config.input_scale, 1.0f));
        assert(Near(config.transfer_strength, 1.0f));
        assert(Near(config.colour_strength, 1.0f));
        assert(Near(config.max_ratio, 2.0f));
        assert(Near(config.white_point, 1.0f));
        assert(Near(config.passes[0].intensity, 1.0f));
        assert(Near(config.passes[0].structure, 1.0f));
        assert(Near(config.passes[0].tone, 1.0f));
        assert(Near(config.passes[0].skin_structure, 1.0f));
        assert(Near(config.passes[1].intensity, 0.5f));
        // Set() sanitizes as well: nothing non-finite survives a round trip through the settings.
        config.passes[2].tone = nan;
        Set(config);
        assert(std::isfinite(Snapshot().passes[2].tone));
        assert(Near(Snapshot().passes[2].tone, 1.0f));
    }

    // 3c. The parser trims whitespace and reads the enum tokens case-insensitively, so a
    // hand-edited file, the launcher and the in-game menu all agree.
    {
        FILE *file = std::fopen(path.string().c_str(), "w");
        assert(file);
        std::fprintf(file, " nr_enabled = TRUE \nnr_resolve = Ratio \n"
                           "nr_pass1_stage = Pre-Upscale \nnr_pass2_enabled=Yes\n"
                           "nr_pass2_stage=PRESENT\nnr_pass3_style = 1 \n");
        std::fclose(file);
        Load(path.string().c_str());
        const Config config = Snapshot();
        assert(config.enabled);
        assert(config.resolve == ResolveRatio);
        assert(config.passes[0].stage == PreUpscale);
        assert(config.passes[1].enabled);
        assert(config.passes[1].stage == Present);
        assert(config.passes[2].style == Style1);
    }

    // 4. Set/Toggle and the status line.
    {
        Config config = Defaults();
        config.enabled = false;
        Set(config);
        assert(!Snapshot().enabled);
        assert(Toggle());
        assert(Snapshot().enabled);
        assert(!Toggle());
        SetStatus("Enabled - 2 passes");
        assert(Status() == "Enabled - 2 passes");
        SetStatus(nullptr);
        assert(Status().empty());
        SetStatus("Disabled");
    }

    // 5. BB_NEURAL overrides the file at start.
    {
        FILE *file = std::fopen(path.string().c_str(), "w");
        assert(file);
        std::fprintf(file, "nr_enabled=0\n");
        std::fclose(file);
#ifdef _WIN32
        _putenv_s("BB_NEURAL", "1");
#else
        setenv("BB_NEURAL", "1", 1);
#endif
        Load(path.string().c_str());
        assert(Snapshot().enabled);
#ifdef _WIN32
        _putenv_s("BB_NEURAL", "0");
#else
        setenv("BB_NEURAL", "0", 1);
#endif
        Load(path.string().c_str());
        assert(!Snapshot().enabled);
#ifdef _WIN32
        _putenv_s("BB_NEURAL", "");
#else
        unsetenv("BB_NEURAL");
#endif
    }

    // 6. Unrelated keys and a malformed pass index are ignored, not fatal.
    {
        FILE *file = std::fopen(path.string().c_str(), "w");
        assert(file);
        std::fprintf(
            file, "upscaler=dlss\nnr_bogus=1\nnr_pass9_style=2\nnr_passx_style=2\nnr_enabled=1\n");
        std::fclose(file);
        Load(path.string().c_str());
        const Config config = Snapshot();
        assert(config.enabled);
        assert(config.pass_count == 1);
        assert(config.passes[0].style == Style0);
    }

    std::printf("neural settings: ok\n");
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    return 0;
}

// SPDX-License-Identifier: GPL-2.0-or-later
// Saved FSR 4 choices must be safe before the first rendered frame on older GPUs.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <unistd.h>
#include "bbport_settings.h"

int main() {
    using namespace BbSettings;
    char path[] = "/tmp/bbport-upscaler-test-XXXXXX";
    const int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    setenv("BB_CONFIG", path, 1);
    unsetenv("BB_UPSCALER");
    unsetenv("BB_UPSCALE_PRESET");
    unsetenv("BB_RENDER_RES");
    auto& s = Get();
    s.upscaler = UpscalerTaa;
    s.preset = Performance;
    assert(RenderPreset() == NativeAA && s.preset == Performance);
    Save();
    s.upscaler = UpscalerOff;
    Load();
    assert(s.upscaler == UpscalerTaa && s.preset == Performance);
    assert(RenderPreset() == NativeAA && !ResolutionNeedsRestart());
    s.upscaler = UpscalerFsr3;
    assert(RenderPreset() == Performance);
    for (bool fsr4 : {false, true}) {
        for (bool fsr411 : {false, true}) {
            for (int requested = 0; requested < UpscalerCount; ++requested) {
                FILE* config = std::fopen(path, "w");
                assert(config);
                std::fprintf(config, "upscaler=%s\npreset=3\noutput_res=2560x1440\n",
                             UpscalerName(requested));
                std::fclose(config);
                s.fsr4_problem = nullptr;
                Load();
                assert(s.upscaler == requested);
                ConfigureUpscalerSupport(fsr4, fsr411);
                const bool unsupported = (requested == UpscalerFsr4 && !fsr4) ||
                    (requested == UpscalerFsr411 && !(fsr4 && fsr411));
                assert(s.upscaler == (unsupported ? UpscalerFsr3 : requested));
                assert(s.fsr4_supported == fsr4);
                assert(s.fsr411_supported == (fsr4 && fsr411));
                assert(bool(s.fsr4_problem.load()) == unsupported);
                assert(s.preset == Performance && s.output_res == 2);
                // Startup patch settings still describe the already applied guest patches.
                assert(s.startup_upscaler == requested);
                ConfigureUpscalerSupport(fsr4, fsr411);
                assert(s.upscaler == (unsupported ? UpscalerFsr3 : requested));
            }
        }
    }
    // A live session (no BB_RENDER_RES): a selection the startup patch would apply cannot
    // change live, so the menu offers a restart for it.
    unsetenv("BB_RENDER_RES");
    unsetenv("BB_PATCH_AVAILABLE");
    s.upscaler = UpscalerFsr3;
    s.preset = NativeAA;
    s.output_res = OutputDefault;
    s.live_resolution = 0;
    assert(!FixedRenderSession() && !ResolutionNeedsRestart()); // nothing to reduce at 1080p
    s.preset = Performance;
    assert(ResolutionNeedsRestart()); // now a patch would apply: restart required
    s.live_resolution = 1;
    assert(!ResolutionNeedsRestart()); // live changes are the explicit opt-out
    s.live_resolution = 0;
    s.preset = NativeAA;
    assert(!StartupPatchApplies());
    s.preset = Performance;
    assert(StartupPatchApplies());
    s.upscaler = UpscalerTaa;
    assert(!StartupPatchApplies());
    s.upscaler = UpscalerOff;
    assert(!StartupPatchApplies());
    s.upscaler = UpscalerFsr4;
    setenv("BB_PATCH_AVAILABLE", "0", 1); // a game copy without the 01.09 patches
    assert(!StartupPatchApplies() && !ResolutionNeedsRestart());
    unsetenv("BB_PATCH_AVAILABLE");
    // Fixed session: the running guest was patched at start; size changes need the restart.
    s.output_res = 2;
    s.live_resolution = 0;
    setenv("BB_RENDER_RES", "1706x960", 1);
    s.startup_preset = Quality;
    s.preset = Performance;
    assert(RenderPreset() == Quality && ResolutionNeedsRestart());
    s.preset = Quality;
    assert(!ResolutionNeedsRestart());
    s.upscaler = UpscalerOff;
    assert(ResolutionNeedsRestart());
    s.upscaler = UpscalerFsr4;
    assert(!ResolutionNeedsRestart()); // provider change at the same patched size is live
    s.upscaler = UpscalerTaa;          // TAA needs native guest targets
    assert(ResolutionNeedsRestart());
    s.upscaler = UpscalerFsr4;
    const int output = s.startup_output_res;
    s.output_res = (output + 1) % OutputCount;
    assert(ResolutionNeedsRestart());
    s.output_res = output;
    s.live_resolution = 1; // the next launch would be live: restart to apply
    assert(ResolutionNeedsRestart());
    s.live_resolution = 0;
    s.output_res = OutputDefault;
    s.preset = NativeAA;
    setenv("BB_RENDER_RES", "", 1);
    assert(!FixedRenderSession() && !ResolutionNeedsRestart());
    unsetenv("BB_RENDER_RES");
    std::remove(path);
    std::puts("Upscaler support: saved choices and FSR 3.1 fallback PASS");
}

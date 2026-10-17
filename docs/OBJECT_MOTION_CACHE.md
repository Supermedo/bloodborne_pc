# Experimental motion-vector cache

In the Windows launcher's **Graphics** page, **Motion-vector cache
(experimental)** enables reuse of object-motion render targets. It is **off by
default**, including for existing profiles. With it off, the renderer uses the
existing single-target allocation path.

The checkbox is available with **FSR 3.1, FSR 4 or FSR 4.1.1** and **Object
motion vectors** enabled. It works with either FSR 4.1.1 method. Its saved choice
is retained while another upscaler or disabled object motion makes it unavailable.
Both normal PLAY and shortcuts apply the choice at the **next game start**.
Changing the in-game upscaler retains that process's startup cache mode.

The option and hint are translated into all 13 launcher languages. The boolean
`object_motion_cache` lives in the launcher's `settings.json`, alongside its
other startup preferences. The launcher explicitly sets
`BB_OBJECT_MOTION_CACHE=0` or `1` after processing inherited/extra variables, so
the checkbox consistently controls launcher and shortcut starts.

## Implementation and tradeoff

Some scenes alternate full- and half-resolution object-motion targets within a
frame. The tested sewer view alternated 2560x1440 and 1280x720. The original
single-target path finishes the GPU and reallocates on each size change.

The cache keeps an active target and up to three inactive sizes, with each
image, view, dimensions and layout. A revisited size reuses its target with the
existing clearing and validity rules. Eviction and teardown retain completion
waits. The motion-vector calculations and selected image-quality settings stay
the same. The renderer is shared by upscalers that use object motion; the
Windows checkbox exposes the experiment for FSR.

Resident targets consume additional VRAM. The option remains experimental and
off by default while moving gameplay, area transitions, frame pacing and
memory-pressure comparisons are collected. For comparisons, restart at the same
saved position with identical settings, follow the same route/camera movements,
and record per-frame timing and VRAM along with FPS.

## Evidence and reproduction

The [FSR Windows report](FSR411_WINDOWS.md#cache-only-ab-in-the-sewer-separate-run)
contains the static-scene cache-only A/B and a separate four-method comparison
with the cache enabled. Those recorded numbers describe the experimental
enabled configuration. They come from the 16 October 2026 test integrations;
the checkbox build receives separate correctness and packaging checks.

For direct engine launches, `BB_OBJECT_MOTION_CACHE=1` enables bounded reuse.
Unset, `0` and other values select the existing allocation path. The setting is
read once when the object-motion renderer starts, and its startup log prints
`Object motion: target cache on` or `off`. Use the checkbox when starting through
the Windows launcher.

In a configured CLANG64 build:

```bash
ninja -C out/gpu bb-probe object-motion-cache-test motion-history-test
out/gpu/motion-history-test.exe
for mode in 0 1; do
    for scene in tiny game churn; do
        BB_OBJECT_MOTION_CACHE=$mode out/gpu/object-motion-cache-test.exe "$scene"
    done
done
python -m unittest discover -s tests -p test_object_motion_cache_launcher.py -v
```

The GPU test verifies every readback pixel and validity flag across repeated,
full/half-size and seven-size eviction sequences. VMA allocation counts verify
one live target with the cache off and up to four with it on. Repeat with the
variable unset and with an invalid value to check the default path.

## Published test.2

[test.2](https://github.com/rattensuppe/bloodborne_pc/releases/tag/v1.8.2-fsr411-test.2)
is the preserved earlier build with automatic caching. Its tag, downloads and
recorded results remain fixed. The published
[test.3](https://github.com/rattensuppe/bloodborne_pc/releases/tag/v1.8.2-fsr411-test.3)
contains the default-off comparison control on Windows 1.8.2. The current PR
incorporates upstream Windows 1.9 and keeps this control alongside the upstream
frame-generation settings.


Crash guard (amb3: device lost, write fault at address 0 after 11.4k frames on 48ac8c3; 8e8e28c ran
clean at the same spot): the only new GPU binding was the mask image at binding 14, bound through
`BindScene`, which in reduced-resolution mode goes through `SceneTargets::Read`: that path
*creates* a proxy entry for any image that has none and copies into it. `Rt::BindMask` now binds
the mask only when the texture-cache slot is still allocated (`TryGetImage`), the image is a
2D colour image with a backing and the depth's size, and, with live resolution on, only when the
image already has a current proxy (`ProxyCurrent`); otherwise the frame falls back to the traced
moon without SSAO (log `traced, no SSAO (no mask this frame)`). Not reproduced here: if amb4
still faults, bisect with `BB_RT_MOON=trace` plus a build that skips `OnDirShadowInput`.
((0.5 n.L + 0.5)^2) * (1 - g2)` (`0x758..0x778`), scaled by
   `m3 = 1 + f.z (m1 - 1)` and `m4 = 1 + f.w (m1 - 1)` (`0x21c..0x22c`); their specular is the
   same Blinn term as lights 1 and 2 but without the `clamp(16 n.L)` factor. These are the
   hemisphere/fill lights of the set: on the forest ground they are most of what the moon does
   not reach, so dropping them is the "near black mid-ground".
2. The moon specular is shadowed too: `v1 = shadow.r * spec1.r` at `0x6c8` and
   `v36 = v1 * F1 + spec2` at `0x6e0`, so `spec1 *= m1 * shadow` just like the diffuse.
   48ac8c3 applied `m1` only, so the moon highlight showed through every shadow: the sparkles.

Change (default `BB_RT_MOON=trace`, `BB_RT_MOON=mask` keeps 48ac8c3):

- The `DDirLightShadowed1` mask is bound in both modes; `.x` (SSAO) always goes into
  `m1 = 1 + f.x (x - 1)`. In trace mode the cascade term `.z * mix(1, .w, FC)` is replaced by
  `mix(1, V, FC_ShadowColor)` with `V` the traced moon visibility (`Rt::MoonShadowTraced`, same
  numbers as `MoonShadowFromMask` for `z = 1, w = V`), so the shadowed ground gets the stock fill
  colour instead of black. Both diffuse 1 and specular 1 take `m1 * shadow`.
- Soft shadow: `MoonVisibility` picks one direction per pixel per frame inside the moon cone
  (`ConeSample`, PCG seeded by pixel and frame; `BB_RT_MOON_ANGLE` in tenths of a degree,
  default 15 = 1.5 degrees half-angle, carried as `cos` in `DirectParams::shadow_color.w`).
  DLSS RR / the temporal filter integrate it; raise the angle for a wider penumbra.
- Lights 3 and 4 shaded as above (`HalfLambertSq`, `FillSpecular`). Also in the GI bounce.
- GI bounce (`rt_gi.comp`): the moon at hits uses the same `mix(1, V, FC_ShadowColor)` fill
  (`GiParams::shadow_color`, 272 bytes, `ParamsSlotBytes` 512) plus the fill lights' diffuse.
- TLAS audit: `AddDraw` took every triangle-list draw with a motion param, including the
  depth-only shadow-map pass draws (`mrt_mask == 0`). Their captured positions are in the light's
  clip space, so they entered the TLAS as mis-placed opaque proxies: a source of the angular
  shards. They are skipped now (`Rt::DepthOnlyDraw`, `DrawInput::depth_only`); `BB_RT_DEPTH_DRAWS=1`
  keeps them for comparison. The BLAS geometry is `eOpaque` but alpha-tested instances get
  `FORCE_NO_OPAQUE` (or an OMM when `BB_RT_OMM` is on), so the canopy cutouts run
  `AlphaTestPasses` in the ray query; the OMM is 4-state with unknown-opaque for mixed
  micro-triangles, which falls back to the any-hit test.

Not changed: the off-screen keep (rt-fix-offscreen) is independent of this; lights 3 and 4 at
GI hits have no specular.

Live check (supervisor), forest spot, `BB_RT=1 BB_RT_DIRECT=1 BB_RT_GI=1`:

1. RT vs F9: ground path, mid-ground and rock pile with similar luma, no sparkles, soft moon
   shadows without angular shards, no change with the camera pitch. Log: `direct frame` says
   `moon shadow traced, SSAO from the game mask`, `depth-only draws skipped`.
2. `BB_RT_MOON=mask`: the 48ac8c3 look (sharper cascades, now also with lights 3/4 and the
   shadowed specular).
3. `BB_RT_MOON_ANGLE=40` for a visibly wider penumbra; `BB_RT_DEPTH_DRAWS=1` brings the shards
   back if the shadow-pass proxies were the cause.
4. Hunter's Dream flowers, moon, torch and fire unchanged; torch shadows still traced.

Files: `host_shaders/rt_direct.comp`, `host_shaders/rt_gi.comp`, `rt/rt_direct.h`, `rt/rt_gi.h`,
`rt/rt_geometry.h`, `rt/vk_ray_tracing.{h,cpp}`, `vk_rasterizer.cpp`, `tests/test_rt_direct.cpp`
(`TestMoonTraced`). Evidence: `out/rt-fix-ambient/build3.log`.
## Environment variables

| Variable | Default | Meaning |
| --- | --- | --- |
| `BB_RT=1` | unset (off) | Enables the extensions, the per-frame BLAS/TLAS and the debug view. Implies `BB_OBJECT_MOTION_ALL=1`. |
| `BB_RT_DEBUG` | `0` | `0` build only, `1` hit distance (grey `exp(-depth / range)`, misses dark blue), `2` ray-traced AO (8 cosine rays, normal from neighbouring primary hits), `3` distance blended 50 % over the frame, `4` AO multiplied onto the frame. |
| `BB_RT_RANGE` | `50` | Depth scale of the distance view, view-space units. |
| `BB_RT_AO_RADIUS` | `1` | AO ray length, view-space units (the debug view and RTAO). |
| `BB_RT_AO` | `1` (with `BB_RT_DIRECT=1`) | `1` ray-traced AO (one `BB_RT_AO_SCALE`=2 block per traced pixel, hit-distance falloff): `rt_ao.comp` writes its result into the SSAO channel (`.x`) of the game's shadow mask right before the `GXLightDeferredAO` draw, so the stock SSAO strength `f.x` still applies to the ambient base, the directional sets and (as stock does) the local lights; `0` keeps the game's SSAO. `BB_RT_AO_RAYS` (2, max 16) cosine rays per pixel, `BB_RT_AO_HISTORY` (16) frames of reprojected history, `BB_RT_AO_DEBUG=1` no history. Views: `BB_RT_DIRECT_DEBUG=6` the mask AO alone, `7` the spot cone mask. |
| `BB_RT_TRIANGLES` | `2097152` | Triangle capacity per frame (soup, BLAS and scratch are sized from it; the index ring holds 6 bytes per triangle, so 32-bit-index draws use it twice as fast). Draws past the capacity are dropped and counted. |

Requirements: an NVIDIA/AMD/Intel GPU with `VK_KHR_acceleration_structure`,
`VK_KHR_ray_query`, `VK_KHR_deferred_host_operations` and `bufferDeviceAddress`; object motion
vectors on (the menu checkbox, default on) and an upscaler other than Off (object motion is gated
on it). Without any of these the renderer prints why (`Ray tracing: off (...)`) and runs as
before.

## Viewing the debug image

Start the game with `BB_RT=1` (and optionally `BB_RT_DEBUG=<mode>`) in the environment of
`bb-probe.exe`, for example in PowerShell:

```powershell
$env:BB_RT = "1"; $env:BB_RT_DEBUG = "1"; .\out\bb-probe.exe
```

The whole frame is replaced (modes 1, 2) or blended (3, 4). Mode 3 is the one for checking the
geometry: edges of the traced distance image must sit exactly on the rasterised edges. Every
600 frames the log prints `Ray tracing: N frames built, N empty, N without camera; last frame N
triangles in N draws ...; draws N, N accepted, N over capacity, N invalid`.

## Live test steps (supervisor)

1. Baseline: run without `BB_RT`. Expect no `Ray tracing:` line in the log and the usual frame.
2. `BB_RT=1 BB_RT_DEBUG=0`: expect `Ray tracing: on, debug view 0 ...` at startup, the normal
   image, no validation errors (run with the validation layer if available), stable VRAM across
   a few minutes of play (the startup line prints the fixed allocation sizes), stats lines with
   `built` growing and `over capacity` staying 0 (raise `BB_RT_TRIANGLES` otherwise).
3. `BB_RT_DEBUG=1`: grey depth image of the G-buffer geometry, dark blue where nothing is hit
   (sky, and anything outside the capture, see Limitations). Move the camera: the image must
   follow with no lag relative to the G-buffer.
4. `BB_RT_DEBUG=3`: edges of the traced image must coincide with the rasterised edges at full
   and half scene resolution (`live_resolution`, FSR presets), after a resolution change, through
   a loading screen (the view disappears while no G-buffer is drawn and returns afterwards) and
   in the pause menu.
5. `BB_RT_DEBUG=2` or `4`: contact darkening in corners and under characters; no device loss over
   a few minutes.
6. Check the log for `debug view skipped` (frame image not RGBA8 storage: builds still run,
   only the view is off) and `Ray tracing: off` lines.

## Limitations (known, not fixed here)

- Geometry outside the view frustum is missing: only what the G-buffer draws rasterises is
  captured, and the capture is per frame. Rays leaving the frustum (AO, later bounces) hit
  nothing there.
- Only triangle-list draws of motion-capable G-buffer pipelines (5+ colour targets) are
  gathered; strips, quads and indirect draws are not, nor are transparent or forward passes.
- Vertex positions are post-projection: the geometry lives in the current frame's view space,
  not world space, so a world-space accumulation needs `CameraMotion::WorldCamera` later.
- No materials, normals or lighting; AO normals are estimated from neighbouring primary hits.
- The capture needs object motion vectors on, which requires an upscaler to be selected.

## Files

- `gpu/shadps4/video_core/renderer_vulkan/rt/rt_geometry.h`: CPU packing, clip-to-view and ray
  maths, BLAS input sizing, CPU reference of the gather.
- `gpu/shadps4/video_core/renderer_vulkan/rt/vk_ray_tracing.{h,cpp}`: resources, pipelines,
  per-frame builds and the debug pass.
- `gpu/shadps4/video_core/host_shaders/rt_gather.comp`, `rt_debug.comp`.
- `gpu/shadps4/video_core/renderer_vulkan/vk_instance.{h,cpp}`: extensions and features, only
  with `BB_RT=1`.
- `vk_rasterizer.{h,cpp}`, `vk_object_motion.h` (`StoreBase`), `vk_camera_motion.h`
  (`CurrentProjection`): hooks.
- `tests/test_rt_geometry.cpp` (`rt-geometry-test`): clip-to-view and ray consistency, BLAS
  input sizing, record packing and capacities, the gather reference against hand-built index
  lists (16/32-bit, non-indexed, instanced, restart, out-of-range, non-finite).

## Build and check

```bash
C:\msys64\usr\bin\bash.exe -lc "cd /d/modding/bbport-rt && bash build.sh && source ./msys2-env.sh && ninja -C out/gpu rt-geometry-test && out/gpu/rt-geometry-test.exe"
```

Shader validation: `glslang --target-env vulkan1.3 -o x.spv rt_*.comp && spirv-val --target-env vulkan1.3 x.spv`.
Evidence of this slice: `out/rt-slice1/` (build log, test output, spirv-val output).

# Hybrid path-traced indirect diffuse (slice 2)

`BB_RT=1 BB_RT_GI=1`: one cosine-weighted bounce per G-buffer pixel through the slice 1 TLAS,
shaded from the current frame's lit scene colour (screen-space radiance cache), accumulated over
time with camera reprojection, and added to the scene colour before post-processing as
`scene += albedo * gi * BB_RT_GI_STRENGTH`. Without `BB_RT_GI` nothing of this exists: no images,
no pipelines, no hooks fire (`OnGBufferPass`, `OnSceneColor`, `OnDispatch` return at once).

## Hook point

The GI runs at the dispatch of the post-processing combine shader (`cs 9a9cf8a9`, the same
`BB_UPSCALE_BEFORE_CS` trigger the temporal upscaler uses, `Rasterizer::DispatchRecord`), right
before `TemporalUpscaler::OnDispatch`. Why there:

- It is the one point already proven to be "scene finished, post not started": the scene colour
  (full-size RGBA16F, HDR, linear) holds the lit opaques, transparents and the fog composite; the
  combine pass that follows reads it for bloom and tonemapping, so GI added here goes through the
  game's own post and the upscaler like any other light.
- Every G-buffer draw of the frame has been recorded (`AddDraw`), so the BLAS/TLAS of the same
  frame can be built here. `RayTracing::Build` is now a separate step: the GI pass builds first,
  the display pass reuses it (`built_this_frame`) for the slice 1 debug view, and without GI the
  display pass builds as before.
- The scene depth and the G-buffer targets of the frame are still intact (the game keeps them
  until the next frame; the half-resolution effects use other targets).
- The camera of the frame is complete (`CameraMotion::WorldCamera`, `CurrentProjection`) and the
  frame's jitter is known (`TemporalUpscaler::Jitter`, zero once the upscale has run).

Since the fog change (below) the GI pass runs right after the `GXLightComposite` draw instead
(`Rasterizer::Draw` -> `OnAfterComposite`, `BB_RT_GI_AT_COMPOSITE=0` restores the post
trigger): the TLAS is already built by the direct pass, the G-buffer and depth are complete, and
the transparents, particles (smoke, mist) and the post chain then draw over a scene that already
holds the GI, so the GI no longer shows through smoke. The fog is inside the composite, so the
GI apply still multiplies the indirect term by the fog transmittance (next section).

Reduced scene presets: with a live preset the scene is rasterised into `SceneTargets` proxies.
`RayTracing::BindScene` reads the proxies (`SceneTargets::Read`, with write access for the scene
colour) at `SceneTargets::Size()`, otherwise the native images through `Runtime::Transit`.
Every input must come out at one size (scene colour, depth, albedo, normal target), else the
frame is skipped (`GI skipped (...)`, printed up to 8 times). The GI images (two history slots
RGBA16F + R32F view z, one raw RGBA16F) follow that size: a change recreates them after a
`scheduler.Finish()` and resets the history. The TLAS is in view space and needs no resizing.

## Passes

1. `rt_gi.comp` (8x8 groups over the scene size), per pixel:
   - depth -> view position, like `camera_motion.comp` (de-jittered; the TLAS geometry comes from
     the unjittered clip positions, the depth from the jittered viewport);
   - normal: from the depth buffer by default (the smaller finite difference per axis, so
     silhouettes do not bleed), or from a G-buffer target (`BB_RT_GBUF_NORMAL`), flipped to face
     the camera;
   - `BB_RT_SPP` cosine-weighted rays from `p + n * (BB_RT_GI_BIAS + 2e-4 z)`; a hit at view
     position `h` is projected to the (jittered) screen; if on screen and the depth there agrees
     with `h.z` (3 % + 0.05) its scene colour is the radiance (clamped to `BB_RT_GI_CLAMP`,
     NaN/Inf -> 0); occluded or off-screen hits and misses fall back to `BB_RT_SKY`; a miss whose
     direction lands on a sky pixel (depth 1) takes that pixel's colour (screen-space sky);
   - temporal accumulation: the view position is reprojected into the previous frame with
     `previous.view * current.inv_view` (`rt_gi.h`, built from the previous frame's inverse view
     kept by the RT class), bilinear 2x2 taps, each accepted only if the stored view z agrees
     (5 % + 0.02) and is finite; `new = mix(history, raw, 1 / (min(count, BB_RT_GI_HISTORY) + 1))`;
     writes history (rgb, count) and view z, plus the raw sample for the debug view.
   - history rejection: no history (first frame, `InvalidateHistory` from the upscaler's frame
     start, a frame without GI such as a loading screen or menu, image recreation), or a camera
     cut (`CameraCut`: more than 3 units of translation or more than ~25 degrees of forward
     rotation between frames) -> the frame starts from the raw sample (count 1).
2. `rt_gi_apply.comp`: `scene = scene + clamp(albedo) * max(gi, 0) * strength`, NaN/Inf guarded
   (a non-finite result keeps the original pixel); the debug modes replace the scene colour.

Barriers: the slice 1 build barriers; own image barriers for the history in/out and raw images
(undefined -> general on first use), a compute->compute memory barrier between the two passes
(the trace pass samples the scene colour the apply pass writes), and a trailing compute-write ->
all-commands barrier before the game's combine pass. GPU cost: `BB_GPU_PROFILE=1` prints
`ray tracing build` and `ray tracing GI` ms per frame every 5 s with the other labels.

## G-buffer mapping (unproven offline)

`docs/upscaler.md` lists the G-buffer as 6 targets 1920x1080 (RGBA8 x3, sRGB albedo, B10G11R11,
RGBA16F) + D32S8 depth, plus the seventh target of the object motion vectors. The order and the
normal encoding are not written anywhere in this repo, `bbhost-recon/docs/rendering.md` or the
Ghidra project, so:

- Albedo: `BB_RT_GBUF_ALBEDO=auto` (default) takes the first G-buffer target with an sRGB
  format; the texel fetch through an sRGB view gives linear albedo. Confidence: medium (one sRGB
  target in the pass, named "albedo" in the frame table). `BB_RT_GBUF_ALBEDO=<i>` overrides;
  unbound or wrong-size -> albedo 1 (white), printed once.
- Normal: default `0.rgb.uw` (G-buffer 0, world space; slice 9 fix 2). `BB_RT_GBUF_NORMAL=depth` derives the normal from the depth buffer, which is
  always right geometrically (flat shading on smooth surfaces, no normal maps). A G-buffer normal
  is `BB_RT_GBUF_NORMAL=<target>.<swizzle>[.<enc>]`: `<swizzle>` two or three of `rgba`/`xyzw`
  (two -> z reconstructed, camera-facing), `<enc>` letters `u` unorm `n*2-1` (default), `s`
  signed as stored, `w` world space (rotated by the frame's view matrix). Example: `1.rgb`,
  `2.rg.s`, `1.xyz.uw`. A non-finite or zero normal falls back to the depth normal per pixel.
- The startup log prints `Ray tracing: G-buffer targets: 0 <format> WxH, 1 ...` at the first
  G-buffer pass and `GI albedo target N (bound), normal depth|G-buffer, WxH` at the first GI
  frame.

## Environment variables (slice 2)

| Variable | Default | Meaning |
| --- | --- | --- |
| `BB_RT_GI=1` | unset (off) | Enables the GI passes (needs `BB_RT=1`). |
| `BB_RT_SPP` | `1` | Bounce rays per pixel per frame (1..16). |
| `BB_RT_GI_STRENGTH` | `1` | Multiplier of the indirect term added to the scene colour. |
| `BB_RT_SKY` | `0` | Radiance (grey, linear HDR) for misses, off-screen and occluded hits. |
| `BB_RT_MOON_FROM_SKY` | `1` | Live. Moonlight comes from the moon the sky draws, not the game's light 1 direction (Cathedral Ward lights the bridge from behind the camera while the moon hangs in front). The game draws the moon and its red clouds as one blended band mesh around the camera (ps `765450df`); that mesh is in the TLAS under its own mask bit (`MaskSky` 0x80, every other ray leaves it out) and the sky layer pass (`rt_gi.comp` `RT_SKY_LAYER`, every 4th frame, cost within noise) traces it from the camera over a 512x128 world azimuth x elevation grid (-20..90 degrees), then takes the whitest spot above the horizon, scored by min(r, g, b) of the texture, as the moon when it is a disc: at least 4x the mean score on a ring about 5 degrees out (a pale cloud is wide, so its ring is as pale; at dusk in the open fields the layer has clouds only, peak 0.04, and a cloud picked by the plain peak pointed the moonlight away and left the view black). `rt_moon_dir.comp` points light 1 of every set in the per-frame set copy at it, so direct, GI, reflections and fog agree. Nothing on screen (fire, cloud gaps, exposure) can pass for the moon. The game draws the moon disc (a 96-index draw of the same model) only while it is in view, while the 5 cloud segments draw every frame: once found, the moon is kept as long as the layer draws, and out of view it comes from the moon cache (`BB_RT_MOON_CACHE`), so a load facing away is lit from the moon without a look at the sky. Without the layer for 600 frames (an interior, a sky with no moon) and with 0, the game's direction. `BB_RT_SKY_DEBUG=4` shows the grid as a strip along the top and the layer over the sky; every sky debug view draws a magenta ring around the moon; the strip's top rows show the moon slot (red held, blue empty), the peak score (green) and 4x the ring mean (yellow), full width 2.0. |
| `BB_RT_MOON_CACHE` | `rt_moon.txt` | Load time. The moon of each sky model, one `key x y z` line per sky: the key hashes the index and vertex counts of the cloud segments (every layer draw present in each frame since the layer appeared, so the moon draw itself is left out), and x y z is the moon's world position. The moon is a mesh fixed in the world about 756 m out (Hemwick graveyard), not at infinity: across one map its direction moves several degrees, so the light points from the camera at the cached position. A find confirmed by two searches within 1 degree stores the camera plus the disc's distance along its direction, rewritten only when it moves over 20 m (about 1.5 degrees; the hit lands off the disc centre). The log line `moon of sky ... found N degrees from its cached position` gives the seed error at the first find of each run (0.43 measured away from where it was cached). A real look only refreshes the cache. |
| `BB_RT_SKY_CACHE` | `rt_sky` | Load time. A folder with the learned sky image (the 1024x1024 octahedral image mirrors read, clouds included) per sky model, `<key>.bin`, same key as the moon cache. The image only holds directions that were on screen, so a fresh load had grey dome in mirrors and GI wherever the player had not looked. When a sky appears its file is copied over the image (no file: the image is cleared, so the last sky does not linger); every 240 frames the image is read back and saved once it grew by 0.5 %. RGB9E5 per cell, unseen runs as counts: 1.1 MB for a quarter of the image (Hemwick), 2-8 ms per save on the render thread while the sky is still filling, then none. GI probe cells off screen take the image mean (16 directions, at least half seen) before dome x gain. Measured: a cold load with the cache has the red clouds in the puddles; without it, the grey dome. A fit of the layer texture against the screen sky (gain x dome x (1 - coverage) + gain x layer) was tried first and dropped: error 8.6 vs 5.7 for the dome alone, and yellow-green clouds, because the layer shader recolours its texture with per-draw constants and vertex colours. |
| `BB_RT_MOON_MIN` | `0.2` | Live. The min(r, g, b) score a layer texel needs to count as the moon. |
| `BB_RT_GI_FILL_SKY` | `1` | Live. The world set's fill lights 3 and 4 (the game's sky light) added as sky radiance on GI misses, `((1 + dir.L) / 2)^2 * colour * 24/17`, so open ground gets them and geometry occludes them. 0 = off. |
| `BB_RT_GI_HISTORY` | `16` | Maximum accumulated sample count (temporal weight floor `1/17`). |
| `BB_RT_GI_CLAMP` | `4` | Radiance clamp per hit (fireflies). |
| `BB_RT_GI_BIAS` | `0.02` | Ray origin offset along the normal, view units. |
| `BB_RT_GI_DEBUG` | `0` | `1` raw GI, `2` accumulated GI, `3` albedo as used, `4` normal as used (`n*0.5+0.5`), `5` history count / max, `10+i` G-buffer target i (rgb), `20+i` its alpha. Replaces the scene colour before post (so it is tonemapped). |
| `BB_RT_GBUF_ALBEDO` | `auto` | G-buffer target index of the albedo, or `auto` = first sRGB target. |
| `BB_RT_GBUF_NORMAL` | `0.rgb.uw` | Normal source, see above (`depth` for the depth-derived normal). |
| `BB_UPSCALE_BEFORE_CS` | `9a9cf8a9` | The trigger compute shader (shared with the upscaler). |

## Live test steps (supervisor), slice 2

The game must run with an upscaler other than Off and object motion on (slice 1 requirements);
`BB_RT=1 BB_RT_DEBUG=0 BB_RT_GI=1` for all steps below.

1. G-buffer mapping first. Read `Ray tracing: G-buffer targets: ...` from the log. Then
   `BB_RT_GI_DEBUG=10`, `11`, ... one at a time: the sRGB target must look like unlit albedo
   (textures without lighting), a normal target like a normal map (flat surfaces one colour,
   changing with the camera if view space, not if world space). `BB_RT_GI_DEBUG=4` shows the
   depth-derived normal for comparison. If a target is the normal, set `BB_RT_GBUF_NORMAL=<i>.rgb`
   (or `.rg`, `.s`, `.w` as it looks) and check mode 4 matches the depth normal's colours on walls
   and floor. Record the final mapping in this file.
2. Slice 1 TLAS view: `BB_RT_DEBUG=3` as in the slice 1 steps; edges must still coincide (the
   build now runs at the post trigger instead of the display pass when GI is on).
3. GI: `BB_RT_GI_DEBUG=1` (raw: noisy, brighter near lit walls/floors, black on sky) then `2`
   (accumulated: the noise settles within ~16 frames when standing still, follows the camera when
   moving without smearing; a camera cut, the pause menu and a loading screen restart from the raw
   image: no ghost of the old view). `5` shows the count ramp. Then `BB_RT_GI_DEBUG=0` with
   `BB_RT_GI_STRENGTH=1` and `3`: corners and areas under lit surfaces brighten, nothing
   flickers, no black or white pixels (NaN guards), the HUD unchanged. Try `BB_RT_SKY=0.2` outdoors.
4. Resolution: change the preset (reduced proxies) and the output size while GI is on: the log
   prints `GI images WxH` once per size, no validation errors, VRAM flat afterwards; the
   `Ray tracing:` stats line (every 600 frames) shows `GI N frames, N skipped, N history resets`
   with skipped not growing during play.
5. `BB_GPU_PROFILE=1`: `ray tracing build` and `ray tracing GI` ms per frame; expect the GI pass
   in the low milliseconds at 1080p with 1 spp.

## Limitations (slice 2)

- Rays leaving the frustum or hitting surfaces not visible on screen have no radiance: they get
  `BB_RT_SKY`. The radiance cache is the current frame's lit colour, which already contains the
  game's ambient term, so the indirect light is over-estimated by that term (strength knob).
- No spatial filter: 1 spp plus temporal accumulation is noisy during fast camera motion.
- No materials at the hit: the hit's colour is whatever is on screen there (specular highlights
  included).
- The fog is composited before the GI is added (see Hook point).

## Files (slice 2)

- `rt/rt_gi.h`: cosine sampling and tangent frames, pixel <-> view <-> screen with jitter,
  rigid inverse and reprojection, camera-cut and history rules, normal-source parsing, the
  `GiParams` layout (std140, 208 bytes).
- `rt/vk_ray_tracing.{h,cpp}`: `Build` split out, `OnGBufferPass`, `OnSceneColor`, `OnDispatch`,
  `RecordGi`, `BindScene`, GI images and pipelines.
- `host_shaders/rt_gi.comp`, `rt_gi_apply.comp`.
- `vk_rasterizer.cpp`: four hooks (G-buffer targets, scene colour, the dispatch trigger, history
  invalidation with the upscaler's).
- `tests/test_rt_gi.cpp` (`rt-gi-test`): cosine sampling distribution (mean cos 2/3, quadrant
  balance, pdf), pixel/view round trips with jitter and the depth formula, reprojection against
  the world-space path, blend weights, history acceptance, camera cuts, normal-source parsing.

## Build and check (slice 2)

```bash
C:\msys64\usr\bin\bash.exe -lc "cd /d/modding/bbport-rt && bash build.sh && source ./msys2-env.sh && ninja -C out/gpu rt-geometry-test rt-gi-test && out/gpu/rt-geometry-test.exe && out/gpu/rt-gi-test.exe"
```

Evidence: `out/rt-slice2/` (build log, check log, spirv-val output).

# Cached BLAS, debug view at the post trigger (slice 3)

Three changes from the first live test (2026-11-04): the slice 1 debug views now draw where the
slice 2 views already draw, the per-frame BLAS build is replaced by a cache of per-draw BLASes
plus a per-frame TLAS of instances, and the default triangle capacity is 4M.

## Debug view

`BB_RT_DEBUG` 1..4 wrote into the display pass frame image, which never reached the screen (the
hook fires before the display-copy draw is recorded, so that draw overwrites the image; the
`BB_DEBUG_MOTION` overlay in `CameraMotion::Overlay` uses the same image at the same hook and is
not visible for the same reason, left as is). `RayTracing::RecordDebug` now runs at the post
trigger (`OnDispatch`, the same `cs 9a9cf8a9` dispatch as the GI), after the GI passes, and
writes `rt_debug.comp` into the scene colour (RGBA16F storage, `BindScene`), the path proven by
`BB_RT_GI_DEBUG`. The ray grid is the scene size (2296x960 live); the result goes through the
game's post, so mode 1/2 greys are tonemapped. `OnSceneColor` and `OnDispatch` are active when
either GI or a debug view is on; `OnDisplayPass` no longer touches the frame image and only
builds as a fallback when no post trigger ran.

## BLAS cache

Geometry keeps arriving per frame in clip space, but static world pieces only change because
the camera moves. Each accepted draw gets a `Rt::DrawKey` (vertex shader hash, vertex streams
hash, index address, index topology hash, index count, instances, first instance, vertex range,
occurrence in the frame), the identity the object-motion history already uses, and the BLAS
built for it is kept in a fixed pool (`rt_blas_cache.h`: `BlasCache`, `BlockPool`).

Per frame (`AddDraw`):

- Cache hit: no gather, no index copy, no build. The draw becomes one TLAS instance whose
  transform is `view_now * inv_view_built` (`InstanceTransform`, from `CameraMotion::WorldCamera`,
  the game's own view matrix), so the BLAS stays in the view space of the frame that built it and
  the TLAS stays in the current view space: the GI and debug shaders are unchanged.
- Rebuild (miss, skinned draw with a character-size skeleton buffer, small-skeleton palette hash
  changed, triangle count changed, or probe-dirty): the draw is gathered into the per-frame soup
  as before and its BLAS is built into its own pool block; builds of one frame are batched into
  one `vkCmdBuildAccelerationStructuresKHR` (split with a barrier when the scratch fills).
- Probe (`rt_probe.comp`, one thread per instance): four sample vertices of every draw (picked
  on the CPU from the index list, `ProbeSlots`) are read from this frame's captured positions,
  transformed back into the BLAS's built view space and compared to the fingerprint stored when
  the BLAS was built (tolerance `2e-3 + 1e-3 |z|`). A mismatch flags the entry dirty three
  frames later (the readback is read when the host ring slot is reused) and it is rebuilt. This
  catches everything the key cannot: non-skinned objects that move (doors, lifts, props),
  vertex-animated geometry, draw-order swaps between equal keys, buffers rewritten in place.
  Skinned characters never wait for the probe: they rebuild when their bones change (fix 1).
- No world camera this frame (`WorldCamera` invalid): cached instances are masked (fix 1), new ones use the identity transform and
  those are marked dirty, so the next frame with a camera rebuilds them.

Memory is fixed at startup: the pool is `BB_RT_BLAS_MB` MiB (default since 2026-11-15: the
larger of 1.5 x the single-BLAS size of `BB_RT_TRIANGLES` and an eighth of the video memory
clamped to 512 MiB..2 GiB; printed at startup), one `VkAccelerationStructureKHR` per entry on the pool
buffer, at most 8192 entries, fingerprints 512 KiB, probe/flag rings per slot. Eviction at frame
start: entries unused for 120 frames (loading screens, area changes), then least recently used
entries until the pool is under 75 %. Freed blocks are reused only after the GPU passed the tick
of the frame that released them (`retired`, `ReclaimBlocks`); handles are destroyed through
`Scheduler::DeferOperation`. A draw whose BLAS does not fit (pool full, no free entry) is dropped
for the frame and counted (`pool full` in the stats line); it is retried the next frame.

Expected build cost: the gather and BLAS builds only cover rebuilt draws (characters, moving
props, new geometry after a camera turn), the TLAS build is ~230 instances, the probe is one
small dispatch. Live before: 6.6 ms per frame for 2.87M triangles in 227 draws, one full rebuild.
Expected after: well under 1.5 ms while standing or walking in the Hunter's Dream (a few hundred
thousand skinned triangles rebuilt per frame), with one-off spikes to the old cost on the first
frame of an area. Measure with `BB_GPU_PROFILE=1` (`ray tracing build`).

Default capacity: `BB_RT_TRIANGLES` is now 4194304 (4M); 2M dropped 27 % of the draws live with
a 2.87M peak. Soup 144 MiB, index ring 24 MiB x 3.

## Stats line (every 600 frames)

`Ray tracing: N frames built, N empty, N without camera; last frame N triangles in N instances,
N BLAS rebuilt (N triangles gathered, N KiB of indices), max N of N per frame, max rebuilt N;
draws N, N accepted, N over capacity, N invalid, N pool full; cache N entries, N hits, N misses,
N rebuilt, N evicted, N probe dirty, pool N of N MiB (N retired); GI ...`. Healthy while standing
still: `BLAS rebuilt` equals the number of skinned draws, `hits` grows by ~200 per frame,
`probe dirty` stays near 0, `pool full` 0, `pool` flat.

## Live test steps (supervisor), slice 3

All with the slice 1 requirements (upscaler on, object motion on) and `BB_RT=1`.

1. Debug view: `BB_RT_DEBUG=1 BB_RT_GI=0`. Expect `Ray tracing: debug view 1 drawn into the
   scene colour WxH at the post trigger` once in the log and the grey distance image on screen
   (dark blue sky). `BB_RT_DEBUG=3`: traced edges on the rasterised edges, following the camera
   with no lag. `BB_RT_DEBUG=4`: AO darkening in corners and under the character. The HUD is
   untouched (drawn after post).
2. Cache correctness, `BB_RT_DEBUG=3`: walk through the Hunter's Dream and turn the camera: no
   edge offsets, no doubled or missing geometry. Watch the doll and the messengers animate: their
   outlines must follow every frame (skinned, rebuilt per frame). Open a door or ride the
   lantern/lift if one is near: the moving piece must follow within a few frames (probe). Load
   another area: the old geometry is gone at once (new keys), `cache N entries` drops within
   120 frames, `pool` MiB goes down, no validation errors.
3. Build cost: `BB_GPU_PROFILE=1 BB_RT_DEBUG=0 BB_RT_GI=1`. Compare `ray tracing build` ms with
   the 6.6 ms baseline standing still and while walking; note the first-frame spike after a load.
   Expect the stats line `BLAS rebuilt` to be a small number while standing (the skinned draws).
4. Capacity: the stats line shows `max N of 4194304 per frame` and `over capacity 0`,
   `pool full 0` through the whole session; `pool N of M MiB` stays flat (no VRAM growth).
5. Resolution and loading: change the preset and the output size with `BB_RT_DEBUG=3`: the
   view keeps matching (nothing in the cache depends on the size); through a loading screen the
   view disappears and returns, `history resets` and `empty` frames grow only there.

## Files (slice 3)

- `rt/rt_blas_cache.h`: `BlockPool` (first-fit, merging free list), `BlasCache` (key lookup with
  occurrence, rebuild rules, dirty marks, unused/LRU eviction), `InstanceTransform`, `ProbeSlots`.
- `rt/rt_geometry.h`: `DrawKey`, `DrawInput` key/palette/skinned, `FrameGeometry::Valid`,
  `TriangleCount`, `PopLast`.
- `rt/vk_ray_tracing.{h,cpp}`: pool, entries, pending builds, instances, probes, `RecordDebug`,
  `OnDisplayPass(SceneInputs)`.
- `host_shaders/rt_probe.comp` (new), `rt_debug.comp` (RGBA16F target).
- `vk_rasterizer.cpp`: draw key, skeleton flag and palette passed to `AddDraw`; display pass
  inputs with the world camera.
- `tests/test_rt_geometry.cpp`: block pool (alloc, fail, merge), cache (miss, occurrence, hit,
  skinned, palette, size change, probe dirty, unused eviction, LRU eviction, clear), instance
  transform round trip, probe slots, `PopLast`.

## Build and check (slice 3)

Same command as slice 2. Evidence: `out/rt-slice3/` (check log, spirv-val output).

## Slice 3 fix 1 (review defects and live 2026-11-04)

Live before (`out/rt-live/s3a.log`, Hunter's Dream standing still, `BB_RT_DEBUG=3 BB_RT_GI=1`):
227 instances, ~111 BLAS rebuilt per frame (1.86M of 2.87M triangles), `ray tracing build`
4.05 ms, `ray tracing GI` 3.2 ms (0.77 ms with the single BLAS and `BB_RT_DEBUG=0`).

- Wrong placement is masked, not traced. The probe now runs before the TLAS build and clears
  the instance mask (`instance ring`, storage usage, barrier to the build) of every cached
  instance whose four sample vertices do not match its BLAS. This covers repeated props that
  shift occurrence when one copy is culled and rigid objects moved through view-relative
  constants: they are invisible to rays until the rebuild lands 3 frames later, never at the
  old place. Readback flags from a frame older than the entry's `built_frame` are ignored
  (`stale probe flags`), so one detection is one rebuild.
- No camera this frame: cached instances get mask 0 (their built view stays valid for the next
  frame); only BLASes built this frame are traced (identity, they are in this frame's space).
- Pool full recovers. When an allocation fails, `EvictForSpace` picks the cheapest contiguous
  window between entries used this frame (free, retired and older entries only) and evicts the
  entries in it; the blocks are reused after the GPU passed them and the draw is retried the
  next frame. With no free entry id the least recent entry is evicted. `pool full` only stays
  up when the draws of one frame do not fit.
- Static draws hit. The skeleton rule was "a character-size bone buffer rebuilds every frame";
  now every bone buffer (small and character size) is content-hashed into the palette, so a
  draw rebuilds only when its bones change (`skeleton changed` / `palette changed`); a bone
  buffer that cannot be read still rebuilds every frame (`unreadable bones`). Deforming draws
  are built with `ALLOW_UPDATE | PREFER_FAST_BUILD` and refit in place (`refits`), with a full
  rebuild every 60 refits; everything else is built once with `PREFER_FAST_TRACE`.
- GI cost: the debug view (`BB_RT_DEBUG=3`, one ray per pixel) ran in the `ray tracing GI`
  segment since it moved to the post trigger; it now has its own `ray tracing debug` label.
  The TLAS is built with `PREFER_FAST_TRACE` and cached BLASes too. The rest of the gap to the
  single BLAS is the overlap of 227 view-space instance boxes (a ray enters every box it
  crosses); it is not removable without merging static draws into one BLAS.

New second stats line (every 600 frames, after the first):
`Ray tracing: rebuild reasons N miss, N size, N dirty, N unreadable bones, N skeleton changed,
N palette changed; N refits, N space evictions, N stale probe flags; masked N by the probe (last
frame N), N without camera`. Healthy standing still: only `skeleton changed` grows (animated
characters, ~a few per frame), `refits` grows with it, `dirty` and `masked` near 0, `pool full`
0, `cache N entries` and `pool` flat.

## Live test steps (supervisor), slice 3 fix 1

1. `BB_GPU_PROFILE=1 BB_RT_GI=1 BB_RT_DEBUG=0`, Hunter's Dream standing still 30 s: `ray tracing
   build` < 1.5 ms; first stats line `BLAS rebuilt` small; second line shows which reason
   grows. If `skeleton changed` or `palette changed` grows by ~100 per frame while standing,
   the bone buffers change every frame for static pieces (report the numbers).
2. Same run: `ray tracing GI` against 0.77 ms (single BLAS). With `BB_RT_DEBUG=3` the debug
   cost shows under `ray tracing debug`, not in GI.
3. `BB_RT_DEBUG=3`: walk past repeated props (lamps, gravestones) and turn so one copy leaves
   the view: no traced edges at a wrong place; a copy may vanish from the traced view for up to
   3 frames. `masked N by the probe` grows a little there, `stale probe flags` grows with it.
4. Load another area and come back several times: `pool full` stays 0 or returns to flat,
   `space evictions` may grow, `pool N of M MiB` stays bounded.
5. Off: run without `BB_RT`: no `Ray tracing:` line, same frame time as before.

Fix 1 evidence: `out/rt-slice3/fix1-check.log`, `out/rt-slice3/spirv-val-fix1.log`. New tests in
`tests/test_rt_geometry.cpp`: `TestRebuildReasons` (static skinned draw hits for 10 frames,
skeleton/unreadable/size/palette reasons, refit and its limit), `TestOccurrenceShift` (a culled
copy shifts the others onto its entry, the probe mismatch masks them, one rebuild, later flags
ignored), `TestEvictForSpace` (fragmented pool under 75 %, failing allocation, one window
eviction, allocation succeeds after reclaim; no window when every entry is in use).

# Hitches

Live before (2026-11-04, `out/rt-live/stutA.log` RT off, `stutB.log` `BB_RT=1 BB_RT_GI=1`,
Hunter's Dream walk, `BB_FPS_LOG=1`): RT off 117 FPS, slowest frame 89-103 ms per 5 s window;
RT+GI 94-104 FPS, slowest frame 497-569 ms in 7 of 8 windows. The same ~440 ms hitch was already
there on the title screen and menus with RT on and nothing built (`0 frames built, N empty`), and
the one window without a `Ray tracing:` stats line (`stutB.log` line 252) had 126 ms: the hitch
followed the 600-frame stats print, not the builds.

## Stalls found

| Where (before, f9207f4) | Why it blocked | Change |
| --- | --- | --- |
| `vk_ray_tracing.cpp:1409` stats print (`frame % 600`), two `std::printf` lines of ~1.4 KB | `bb-probe` sets stdout unbuffered (`src/probe.c:490`); the UCRT `printf` then issues one write per character, and the live runs redirect stdout to a file through `Start-Process -RedirectStandardOutput` (`out/rt-live/run.ps1`), where each write costs ~0.7 ms. Measured with the same toolchain and redirect: the two lines take 995-1067 ms with `printf`, 0.6-1.3 ms as one `fwrite`. | Every RT print goes through `Log` (`vsnprintf` into a stack buffer, one `fwrite`); the stats lines moved to `PrintStats`. |
| `vk_ray_tracing.cpp:678` `scheduler.Wait` on the ring slot of 3 frames ago, every frame | Blocking whenever the GPU is more than 2 frames behind, also on frames that built nothing (menus, loading: the slot tick was set unconditionally at line 1464). | The tick is only set when the frame built (`slot_ticks[slot]` at `FinishFrame`); the wait only runs when `IsFree` says the tick is not done, and is timed (`wait`). Kept 3 slots: the hitch log shows whether a 4th is needed. |
| `rt_blas_cache.h:196` + `vk_ray_tracing.cpp:747` refit counter | Every deforming entry created in the same frame (area entry, first frame) reached 60 refits on the same frame, so all skinned BLASes did a full `PREFER_FAST_BUILD` rebuild together every 61 frames. | First build of an entry starts its counter at `RefitPhase(id) = id % 60`; after a refit-limit rebuild it restarts at 0, so the full rebuilds stay spread over 60 frames (`TestRefitStagger`: 120 entries, at most 2 full rebuilds per frame instead of 120). |
| `AddDraw` full rebuild bursts (first frame of an area, 2.85M triangles in one frame) | Gather, index copy and BLAS builds of every miss landed on one frame. | `BB_RT_REBUILD_BUDGET` (default 1048576 triangles of full builds per frame, 0 unlimited; refits are not counted). `Rt::PlanStep`: a miss / size change / dirty entry over the budget is deferred: no TLAS instance this frame, the old entry stays in the cache (still dirty, never placed from stale data) and is retried next frame. The first full build of a frame always fits, so one draw larger than the budget still builds. A refit-limit rebuild over the budget refits once more instead; after a frame that deferred anything, refit-limit rebuilds take no budget (no starvation of misses). |
| `DestroyGiImages` `scheduler.Finish()` (`vk_ray_tracing.cpp:349`) and `CreateGiImages` | Only on a scene size change (one `GI images WxH` line per size in the live logs): not steady state. | Unchanged, timed (`GI images`). |
| `BindScene` `createImageView` per bound image per frame, `AllocateEntry` / `ScratchBytes` size queries and handle creation | Driver calls of microseconds, no wait; views and handles are destroyed through `DeferOperation`. | Unchanged, timed (`alloc`, inside `GI`). |

Not RT, noted only: the pre-existing ~90 ms hitch with RT off is the same cause. `BB_FPS_LOG`'s
`Frame rate:` line, the `Frame generation pacing:` line (`gpu/shim/bbport_overlay.cpp:1539`) and
the texture cache / memory reports are `printf` calls on the unbuffered stdout, ~180 characters
every 5 s, ~0.5 ms per character under the `Start-Process` redirect. Making stdout line-buffered
in `probe.c`, or formatting each line before one write, would remove it.

## Hitch log

`BB_RT_HITCH_LOG=1` times each RT entry point on the CPU (steady clock, nothing when unset) and
prints, for any frame whose RT CPU time is over 8 ms or whose ring wait is over 2 ms:

`Ray tracing hitch: frame N, RT CPU X ms (draws X in N calls, alloc X, build X, GI X, GI images X,
debug X, display X: wait X, readback X, evict X, stats X); N BLAS rebuilt (N triangles), N refit
(N triangles), N deferred, budget N`

RT CPU = draws (`AddDraw`) + GI (`RecordGi`, includes `Build` when GI is on) + debug
(`RecordDebug`) + display (`OnDisplayPass`: fallback `Build`, stats print, next slot's wait,
probe readback, reclaim and eviction). `alloc`, `build`, `GI images`, `wait`, `readback`,
`evict`, `stats` are parts of those. Every 600 frames, after the stats lines:
`Ray tracing hitch: worst RT CPU X ms, worst wait X ms, N hitch frames (600 frames); N deferred
by the rebuild budget, N held stale, N budget refits`. A draw whose BLAS exists but needs a
rebuild the budget cannot pay keeps its stale BLAS in the TLAS that frame (`Step::Hold`,
counted in the `rebuild reasons` line); only draws with no BLAS at all are deferred. GPU time stays with `BB_GPU_PROFILE=1`.

Expected live (walk.ps1 with `BB_RT_HITCH_LOG=1`): `stats` well under 2 ms, no hitch lines in
the steady walk except the first frames after the area load (budget-capped rebuilds, a few
deferred draws for 2-3 frames), `worst wait` under 2 ms; the slowest frame per window back to
the RT-off value (~90-120 ms with the same `BB_FPS_LOG` prints). If hitch lines still show
`wait`, the GPU runs more than 2 frames behind and the rings need a 4th slot.

## Files and check

- `rt/rt_blas_cache.h`: `RefitPhase`, `Lookup::can_refit` / `limit`, `RebuildBudget`,
  `Step`, `PlanStep`, `RefitsAfter`.
- `rt/vk_ray_tracing.{h,cpp}`: `Log`, `CpuTimer`, `Hitch`, `PrintStats`, `ReportHitch`,
  `FinishFrame`, budget in `AddDraw`, slot tick only for built frames.
- `tests/test_rt_geometry.cpp`: `TestRefitStagger` (120 deforming entries: at most 2 full
  rebuilds per frame, never more than 60 refits in a row, lockstep counters give 120 in one
  frame), `TestRebuildBudget` (250-triangle budget: 2 of 10 built per frame, deferred draws
  have no entry, all built on frame 5; one draw over the budget still builds; 0 is unlimited;
  a refit-limit entry over the budget refits and takes no budget after a deferral; a dirty
  entry over the budget stays dirty and is rebuilt next frame).
- No shader changed (spirv-val not rerun).

Evidence: `out/rt-hitch/check.log`.

# Light data for ray-traced shadows (slice 4, step 1)

Goal: find the game's lights (sun/moon directional light, local point and spot lights) so a
shadow pass can trace one ray per pixel per light. This slice proves where they live and
captures them; the shadow pass itself is step 2 (not in this commit).

## What was proven, and how

The game's deferred lighting is a light pre-pass: after the G-buffer, `GXLightAcc.vpo`
(vertex shader hash `8747a367`) draws full-screen quads (depth-bounds culled by the light's
depth range, `bbhost-recon/src/host/render.cpp:3210`) with one pixel shader per light batch
into two B10G11R11 accumulation targets (diffuse `mrt0`, specular `mrt1`), then
`GXLightComposite.ppo` (`32e47788`) combines them with the albedo into the scene colour.

Source of the proof: the shipped shader bundle
`CUSA03173/dvdroot_ps4/shader/gxrenderershader.shaderbnd.dcx` (DCX/BND4, 111 Sony shader
containers). Every container carries the compiler's reflection after its `OrbShdr` footer
(constant buffer names, sizes, API slots, members; format from
`bbhost-recon/src/gcn/names.cpp:38-123`), and the `shader_hash` at footer + 16
(`regs_shader.h:34`) is the `pgm_hash` the port logs (`cs 9a9cf8a9` etc.), so the bundle's
shaders map 1:1 onto live draws by hash. `out/rt-slice4/shader_reflect.py` parses the bundle
(`gxrenderershader-reflection.txt` all entries, `lightacc-reflection-full.txt` the light
shaders with inputs/outputs); `out/rt-slice4/*.dis` are GCN disassemblies (bbhost's `gcndis`,
built from `bbhost-recon/tools/gcndis.cpp` + `src/gcn/{decode,container,wave}.cpp`; LLVM's
`llvm-mc` in msys2 refuses gfx7 disassembly).

Light accumulation pixel shaders (all with `GXLightAcc.vpo`), `rt/rt_lights.h`
`LightShaders` (86 entries, hash -> kind, count, shadowed):

| Family | Counts | `cbLightAccLightParam` (slot 1) | per light |
| --- | --- | --- | --- |
| `[Legacy][D]PointLight<N>` | 1..12 | `g_aPointLight PointLight[N]`, 64 N bytes | 64 |
| `[Legacy][D]PointLightShadowed<N>` | 1..4 | 96 N bytes, plus `g_LightShadowTexture` (cube, slot 7) | 96 |
| `[D]SpotLight<N>` | 1..4 | `g_aSpotLight SpotLight[N]`, 128 N bytes | 128 |
| `[D]SpotLightShadowed<N>` | 1..4 | 144 N bytes, plus `g_LightShadowTexture` (2D, slot 7) | 144 |
| `DLineLight<N>` | 1..4 | not analysed | - |
| `DDirLight1`, `DDirLightShadowed1` | 1 | `cbLight` (slot 5), 576 bytes `FC_DirLight LightSetParam` | 288 x 2 sets |

Every light shader also binds `cbLightAccParam` (slot 0, 208 bytes: `g_InvProjMatrix` 64,
`g_ViewMatrix` 48 (3x4), `g_ScreenSize` 16, `g_CameraMatrix` 64, `g_specPowerParam` 16) and
samples `g_LightPrepassAttrTexture` (slot 0), `g_DepthTexture` (1), `g_GBuffer1Texture` (3),
`g_GBuffer3Texture` (5), the shadowed/dir ones `g_ShadowTexture` (32), the local ones
`g_SSAOTexture` (33). The directional shadowed variant additionally binds `cbSceneParam`
(slot 8, the 864-byte scene constants the camera code already reads: `FC_MatrixView` at 32,
`FC_ProjSpaceToShadowMatrix[4]` at 448, `FC_CascadeSelectDist` at 704, `FC_MatrixInvView` at
720).

Member layout, from the GCN code (`DPointLight1.dis`, `DSpotLight1.dis`, `DDirLight1.dis`;
SMRD `s_buffer_load` offsets are in dwords):

- `PointLight` (16 floats): `[0..2]` position in **view space** (the shader subtracts it from
  the view position it reconstructs with `g_InvProjMatrix`, with no further transform:
  `v_mad_f32 v13, -v15, v4, s30` ...), `[3]` **1 / radius^2** (`v6 = clamp(s31 * d2)`,
  `v7 = max(0, 1/d2 - s31)`), `[4..6]` diffuse colour (multiplied by the clamped N.L),
  `[7]` specular power term, `[8..10]` specular colour, `[12..13]` wrap/subsurface
  parameters in a branch. The G-buffer normal is world space: the shader rotates it with
  `g_ViewMatrix` rows (`cbLightAccParam` dwords 16..27) before the dot products.
- `SpotLight` (32 floats): `[0..2]` position (view space, same pattern), `[3]` **radius**
  (`rcp(s7 * s7)`), `[4..6]` colour (same use as the point light's), `[12..15]` mode flags
  (`s15 > 0` selects the falloff branch), `[16..31]` a 4x4 projection of the cone (the
  `x^2 + y^2 <= w^2` test gives the cone falloff). The direction is implicit in that matrix;
  not needed for a shadow ray, which only needs the position.
- `LightSetParam` (72 floats, two sets per `cbLight`): the shader indexes it with
  `int(gbuffer.a * 255) * 18` float4s, so each pixel picks a set by a G-buffer alpha
  (light-set id). Float4 `[0]` is the light direction, dotted with the untransformed
  G-buffer normal (**world space**), `[1]` its colour, `[4]`, `[5]` a second direction and
  colour (fill/ambient), `[17]` a vector of four scale factors. The directional *shadow* is
  not in these constants: `GXScreenSpaceCascadedShadow_{PCF4,PCF9,Soft}.ppo` write a
  screen-space mask from the 4096^2 cascade map and `cbPerScene`
  (`g_ProjSpaceToShadowMatrix[4]`, `g_CascadeSelectDist`), `GXRend_CompositeShadowMask.ppo`
  combines it with the SSAO (`FC_Mult`, `FC_Add`), and `DDirLightShadowed1` samples the
  result as `g_ShadowTexture`.

Not proven offline (checked live by the dump below): that the live `cbLightAccLightParam`
stride matches the table for the families the Hunter's Dream uses, the exact colour scale
(linear HDR vs. a premultiplied intensity), and which of the two `LightSetParam` sets is the
sun/moon.

CPU side (asked for by the brief, not found): the decomp in `bbhost-recon/src/decomp/gx`
covers the command list, constant engine, present and the light blink only; `blink.cpp:4-14`
gives the GX light objects (point light vtable `0x57b7670`, spot `0x57b76d0`, blink at
`+0x130` / `+0x1ac`, the factor at `+0x2c` scaling the colour) but not the constant-buffer
writer. The eboot has no `cbLightAcc*`/`g_aPointLight` strings (GX binds constant buffers by
slot) and only UTF-16 `LightAcc` shader names (28); no Ghidra install is on this machine
(`analyzeHeadless` absent) and REA's Ghidra provider only accepts PE on Windows, so the
writer function was not decompiled. A Game Crawler probe of the running game was tried: its
address expressions resolve bare hex as RVAs of `bb-probe.exe` (guest addresses were not
reachable) and the whole-memory `i64` scan for the point-light vtable timed out at 30 s;
nothing was written to the game. The shader-side proof above does not depend on the writer:
the shaders read exactly those offsets.

## Capture (this commit)

`BB_RT_LIGHT_DUMP=<path>` (or `=1` for `rt_lights.txt` in the working directory) and
`BB_RT_SHADOWS=1` turn the capture on (`RayTracing::WantsLightBuffers`). In
`Rasterizer::BeginRendering`, a draw whose vertex shader is `8747a367` calls
`RayTracing::OnLightDraw(ps_hash)`; `BindBuffers` then hands every fragment-stage constant
buffer of that draw to `OnLightBuffer(size, data)`, from both of its paths: buffers the GPU
command thread copied into the constant ring (`CollectRingBindings`: small, read-only, the path
these cbuffers take, read from the guest address since fix 2, the ring copy as fallback) and buffers bound from their V# (fix 1: the first
live dump had 0 bytes everywhere because only the V# path called the hook).
`Rt::RecordLightBuffer` recognises them by size (208
`cbLightAccParam`, 576 `cbLight`, `count x stride` the light array) and parses the lights with
`rt_lights.h` (`ParseLocalLights`, `ParseDirectionalLight`: NaN/Inf, zero or negative radius
rejected). At the post trigger (`OnDispatch`) the frame's list is closed: `SelectStrongest`
keeps the directional lights and the `BB_RT_SHADOW_LIGHTS` (default 4) local lights with the
largest luminance x radius; `FrameLights()` exposes them to the shadow pass of step 2.

The dump writes, for the first three frames with light draws and then every 600th frame, one
block per frame into the file (buffered `fprintf`, one `fflush` per dumped frame; no stdout
prints, so no hitch): every light draw with its shader name and hash, the raw floats of
`cbLightAccParam` (4 per line), of the light array (one light per line, stride columns) and of
`cbLight`, then the parsed lights (`pos`, `dir`, `radius`, `color`). Off (`BB_RT_LIGHT_DUMP`
and `BB_RT_SHADOWS` unset): `WantsLightBuffers()` is false, the rasterizer does not look at the
draws, nothing is allocated.

## Live steps (supervisor)

The running `bb-probe.exe` was built before this commit (the copy in `build.sh` failed
because the game had the file open); `out/gpu/bb-probe.exe` is the new one, copy it to
`out/bb-probe.exe` once the game is closed (or rerun `bash build.sh`).

1. Hunter's Dream with the torch out, `BB_RT=1 BB_RT_GI=0 BB_RT_DEBUG=0
   BB_RT_LIGHT_DUMP=out\lights-live.txt`. Expect
   `Ray tracing: light dump to ... (open)` at startup and the file to hold three frames at once
   and one more every 600 frames.
2. In the file: a `DDirLight*` draw with 208 + 576 bytes, `DPointLight<N>` / `PointLight<N>`
   draws with 208 + 64 N bytes (shadowed 96 N), spot draws 128 N / 144 N. Each draw line ends
   with `N other` and up to six sizes of the fragment buffers no rule matched (864, the scene
   constants, is expected). A draw with `lights 0 bytes` and an unexpected size there means the
   live layout differs from the table: report the shader name and those sizes.
3. Torch check: one parsed point light must sit about 1-2 units in front of and below the
   camera (view space: small |x|, y negative, z 1..3) with a warm colour, and move with the
   player between the frames 600 apart; the Dream's lamps are the other points with larger
   radii. The sun/moon: `cbLight` float4 `[0]` a unit-length direction, `[1]` its colour.
4. Report the live strides and the position/colour ranges; they fix the "not proven" items
   above before step 2 traces anything.

## Live layout (step 2, from `out/rt-slice4/lights-live.txt`, Hunter's Dream, torch out)

The first live dump (frames 4928..8400, ~308 light draws and ~659 parsed lights per frame)
corrected two assumptions of step 1:

- **Local light buffers are `count x 144` bytes live, the lights inside are packed at the
  table stride.** `DPointLight1` 144, `DPointLight2` 288, `LegacyDPointLight6` 864,
  `LegacyDPointLight12` 1728, `LegacyDPointLightShadowed1` 144: always 144 per light whatever
  the family, but the floats sit at 64-byte stride (96 shadowed) from the start, the rest is
  zero padding (`LegacyDPointLight6`: six lights in floats `[0..95]`, `[96..215]` zero;
  `DPointLight2`: light 1 at float 16, `LegacyDPointLightShadowed1`: 24 floats then zero).
  Step 1's `LightStride` divided the buffer size by the count (144) and so parsed every light
  after the first from the padding (the dump's `pos 0.467 0.467 0.467` lights). Now
  `LightDataStride` is fixed per family (point 64/96, spot 128/144) and a buffer is a light
  array when it is a multiple of 16 holding at least `count x stride` bytes.
- **`cbLight` is 64800 bytes live, not 576**: 225 `LightSetParam` sets of 288 bytes (the
  pixel shader indexes the set with `int(gbuffer.a * 255)`, so the game allocates one set per
  light-set id). The 864-byte buffer on the same draw is `cbSceneParam`. `IsDirLightSetBuffer`
  accepts any multiple of 288 except 864 on a directional draw, `ParseDirectionalSets` parses
  every set with a finite non-zero direction and a non-black colour (first
  `MaxDirLightSets` = 8), each tagged with its set id in `Light::index`.

Field meaning confirmed from the dump (`LegacyDPointLight6`, light 1): `[0..2]` view-space
position `(0.038, -0.530, 4.564)` (the torch: in front of and below the camera), `[3]`
`1/r^2 = 0.01406` (r = 8.4), `[4..6]` warm diffuse colour `(2.35, 1.32, 0.59)`, `[7]` 0.5
(specular power term, 20 on the lamps), `[8..11]` specular colour, `[12..15]` `1 1 0 0` flags.
The `cbLightAccParam` `g_CameraMatrix` (floats 32..47, row-major 3x4 + `0 0 0 1`) is the
inverse of `g_ViewMatrix` (checked: `-R^T t` of the view rows gives its translation), so
`ApplyCameraMatrix` turns every parsed local light into world space with its own draw's matrix,
and the shadow pass brings it back into the RT view space with the port's own
`inputs.inv_view`; a mismatch between the two view conventions would show up in the first
three `Ray tracing: shadows frame ...` lines (game view vs rt view of the strongest light).

## Step 2: the shadow pass (this commit)

`BB_RT_SHADOWS=1` (needs `BB_RT=1`): `rt_shadow.comp` runs at the post trigger after the GI
(`RayTracing::RecordShadows`, profiler label `ray tracing shadows`). Per pixel: view position
from the depth and the surface normal exactly as `rt_gi.comp` (same `PixelToView`,
`SurfaceNormal`, same `BB_RT_GBUF_NORMAL` source and bias), then for each of the frame's
selected lights one `rayQueryEXT` from `p + n * bias` towards the light with
`TerminateOnFirstHit`, `tMin = BB_RT_SHADOW_TMIN` (0) and `tMax = |light - origin| -
BB_RT_SHADOW_GAP` (0.3, keeps the ray out of the lamp's own mesh). Lights behind the surface
(`N.L <= 0`) or out of range (`d^2 / r^2 >= 1`) cost nothing. Range, attenuation and `N.L` use
the unbiased `light - p` like the game; only the ray starts at the biased origin (fix 2).

Light selection (`SelectShadowLights`): only **unshadowed point lights** with a world position
are candidates; the `BB_RT_SHADOW_LIGHTS` (default 4, max 8) with the largest luminance x
radius are traced, after a dedup (fix 2): the game draws the same lamp in many light batches
(frame 57600 of `lights-live.txt`: one lamp at world `10.800 -0.441 -1.267` is light 0 of 80
draws, the torch at `0.024 -0.512 4.568` of 64), so a plain top-N held N copies of one lamp and
subtracted it N times. `SameLight` treats two lights as one when the world positions are within
0.01 per axis and radius and colour within 1 %; the selection walks the strength-sorted list and
keeps a light only if it matches none already kept (at most 8 compares per light). Excluded on
purpose: every `*Shadowed*` family (the game already shadows
them with its cube/2D maps: tracing them again would double-darken), spot lights (their cone
is a 4x4 projection in floats `[16..31]` that the subtraction below would need; not stored
yet), line lights (not analysed) and the sun. The sun is skipped: the Hunter's Dream only
draws `DDirLightShadowed1`, whose shadow is the screen-space cascade mask the game composites
before the light pass, and the pass has no clean access to that mask (it would need a new
rasterizer hook on the `GXScreenSpaceCascadedShadow` / `CompositeShadowMask` pixel shaders,
out of this slice) nor to the per-pixel light-set id that picks the sun's set out of the 225;
without both it would darken already-shadowed pixels twice. The directional sets are only
captured and parsed while `BB_RT_LIGHT_DUMP` is on (see the CPU cost below).

Combination without double-darkening: the game has already added this light's full
contribution to the scene colour (these families have no shadow term), so where the ray is
blocked the pass subtracts what the game added: `scene -= albedo * (1 - g^2 / 2) * colour *
max(0, 1/d^2 - 1/r^2) * N.L * BB_RT_SHADOW_STRENGTH`, clamped to `[0, 65504]` (no fp16
overflow, no negative colour), with the albedo from the GI's sRGB G-buffer target (white when
not bound) and the attenuation mirroring the GCN light shader (`max(0, 1/d2 - s31)`).

The game's diffuse term (`DPointLight1.dis`, fix 2). The table at `s[2:3]` holds slot 1 depth,
3 `g_GBuffer1Texture`, 5 `g_GBuffer3Texture`, 33 `g_SSAOTexture` (8 dwords each from the
table start, matching the reflection's `@r16..r40`). `0x28c..0x294` squares the slot 5 sample
(`v3, v10, v11 = g^2`), `0x3ac..0x3cc` gives `diffuse = c * (1 - g^2 / 2)` per channel,
`0x3d4..0x3e8` multiplies diffuse and specular by the SSAO sample `v2` (`0x280`), `mrt0` is the
diffuse, `mrt1` the specular `g^2 * ssao * spec`. What the pass does with each factor:

- `(1 - g^2 / 2)`: applied from G-buffer target `BB_RT_SHADOW_SPEC` (default 3, the only
  `R8G8B8A8Unorm` target left after albedo 2 in the live list `0 1 3 Unorm, 2 Srgb, 4
  B10G11R11, 5 RGBA16F`; `255` off). Assumption, not proven: that `g_GBuffer3Texture` is MRT 3
  of the G-buffer pass (the name matches, the format check rejects anything but RGBA8 unorm).
  A wrong target errs by at most a factor 2 (the factor is in `[0.5, 1]`). The first `shadows
  frame` lines print `specular bound|none`.
- SSAO (slot 33): not reachable, the port never captures the light pass's textures and the
  shadow pass runs at the post trigger. Leaving it out over-darkens occluded pixels (SSAO <= 1),
  so the default `BB_RT_SHADOW_STRENGTH` is now 0.75 instead of 1 as a conservative stand-in.
- The wrap/subsurface branch (`0x2dc..0x344`, taken when `g_GBuffer1Texture.y != 0`): not
  modelled; such pixels get the plain `N.L` term.
- Specular (`mrt1`): not removed. A blocked light keeps its highlight; removing it would need
  the specular power (`[7]`), `g_specPowerParam` and the view vector per light and is left for
  a later slice. This errs towards too little darkening, never double-darkening.

The exact colour scale of the game's accumulation (linear HDR vs a premultiplied intensity) is
still unproven; `BB_RT_SHADOW_STRENGTH` tunes it live. 1 ray per pixel per light, no temporal
accumulation yet (hard shadows from point lights need none for the first look).

`BB_RT_SHADOW_DEBUG=1` replaces the scene colour with the visibility: per pixel the fraction of
the in-range lights whose ray is not blocked (white lit, black blocked, the blue channel 0.5
where no selected light reaches the pixel, black on the sky).

Stable selection (fix 3). The user saw whole-light shadows switch on and off (Old Yharnam lamp,
two screenshots 14 s apart: a hard lamp + messengers shadow, then none). Root cause: the old
`SelectShadowLights` ranked by luminance x radius only, with no camera term and no history.
`lights-live.txt` frames 6000/6600/7200: of the top 4, three are lamps 42-72 m away (17.5, 7.6,
7.1 vs 6.9 at the cut), the torch 4.6 m away flickers 12.5-14.8, and the near lamps sit at 3.3
vs 3.3, so the 4th slot and the order among near-equal lights changed with flicker and draw
order, and each change switched a shadow fully on or off in one frame. Now:

- Score `ShadowScore = lum * radius / (1 + max(0, d - radius)^2)`, `d` the light's distance to
  the camera (its game view position): lights whose range reaches the camera keep the full
  strength, far lamps fall off.
- Hysteresis (`UpdateShadowTracks`, state `RayTracing::shadow_tracks`): a light is the same
  source as last frame when its world position is within 0.5 m and its radius within 25 %
  (`SameSource`; colour is ignored so torch flicker does not break the match). A light selected
  last frame scores x1.5, so a newcomer must be clearly stronger to take its slot.
- Fade: each tracked light has a weight that moves 0.1 per frame towards 1 (selected) or 0
  (not), and the light's colour is scaled by it (`ShadowDrawLights`), so a shadow fades in and
  out over 10 frames. A fading light is drawn only while the game still draws it this frame
  (its contribution is in the scene colour, so subtracting it cannot double-darken); a light
  the game stops drawing is kept invisible for 8 frames so a one-frame gap resumes at its old
  weight. At most `MaxShadowLights` (8) are drawn, selected first; the extra slots are only
  used during a fade.
- Only the 16 strongest deduplicated candidates are considered per frame.

`tests/test_rt_lights.cpp` `TestShadowSelectionStable`: 6 near-equal lights (0.5 % apart, 3 %
flicker, shuffled every frame) over 120 frames: the old selection changes 108 times, the new
one 0. `TestShadowFade` checks the 10-frame fade in/out, the far-lamp score and the missing
grace.

Lamp self-occlusion (fix 3): the lamp's light sits inside its own cage and glass, so with the
old 0.05 m end gap the ray to the light could hit the lantern mesh and the lamp cast a big black
blob. The default `BB_RT_SHADOW_GAP` is now 0.3 m (hits in the last 0.3 m before the light are
ignored).

F9 (fix 3 audit): `gpu/shim/window.cpp:237-240` consumes the key-down event, and
`src/runtime_input_config.c:252` (`IsReservedScancode`) refuses F9 in any input.ini binding, so
the pad sampler (`src/runtime_pad.c:184`, `SDL_GetKeyboardState`) has no output on F9; the
only other reader, `src/runtime_pad.c:816`, is the `BB_PAD_RECORD` recorder and injects no
button. F7/F8 take the same path. The Gestures menu is the touchpad's (left half: `g` or the
pad's Back, `src/runtime_pad.c:145-175`), not an F9 binding.

Off: `shadows_enabled` false, no pipeline, no params ring write, `OnDispatch` returns as
before; the light capture only runs with the dump or the shadows on. Cost on: one compute
dispatch of the scene size with up to 8 short rays per pixel; GPU time in the profiler under
`ray tracing shadows`. Logs go through the existing `Log` helper: three `shadows frame` lines,
then only the stats line every 600 frames, which ends with `light capture X ms/frame` (CPU time
of `OnLightDraw` + `OnLightBuffer` + `EndLightFrame` averaged since the previous line).

CPU cost (fix 2, supervisor live walk: 78-95 FPS with shadows vs 103-116 without, GPU pass
0.17 ms). The capture read every light cbuffer from the constant ring's mapped memory, which is
a `Stream` buffer (device-local, host sequential-write: write-combined, uncached for CPU
reads), about 250 KB per frame over ~308 draws plus the 64800-byte `cbLight`. Now: the ring
branch of `BindBuffers` hands the guest address of the same cbuffer (cached memory, the
address `camera_motion` already reads), the ring copy only when the address is not a valid GPU
mapping; without the dump only unshadowed point-light draws are recorded (no directional,
shadowed, spot or line draws, so no `cbLight` copy); the light array copy is `count x stride`
bytes into a fixed array in the record (no per-draw heap allocation, `light_draws` keeps its
capacity). Not measured offline: the before/after ms comes from the stats line live. Trade-off:
the guest address is read in stage B, after stage A copied it; a game that rewrote the cbuffer
in between would give that frame's lights the new values (the game keeps light constants until
the GPU consumed them, so this is not expected).

## Files and check (slice 4)

- `rt/rt_lights.h`: the shader table, strides (`LightDataStride`), parsers,
  `ParseDirectionalSets`, `ApplyCameraMatrix`, `SelectShadowLights`, `ShadowParams`,
  `PackShadowLight`, `SelectStrongest`.
- `rt/vk_ray_tracing.{h,cpp}`: `OnLightDraw`, `OnLightBuffer`, `EndLightFrame`, `DumpLights`,
  `RecordShadows`, the env variables.
- `host_shaders/rt_shadow.comp` (`gpu/CMakeLists.txt`).
- `vk_rasterizer.{h,cpp}`: `light_draw` and the two hooks (unchanged in step 2).
- `tests/test_rt_lights.cpp` (`rt-lights-test`): table uniqueness and lookups, strides for the
  live buffer sizes, point/shadowed/spot/directional parsing, the 225-set `cbLight` and the
  864-byte scene buffer on a directional draw, the live `LegacyDPointLight6` block, the camera
  matrix to world and back to RT view, rejects, strongest-light and shadow-light selection,
  the dedup with the frame-57600 layout (80 copies of the lamp, 64 of the torch).
- Evidence: `out/rt-slice4/` (`check.log`, `tests.log`, `step2-check.log`, `fix2-check.log`: spirv-val of
  `rt_shadow.comp`, the three RT tests, `build.sh`; `lights-live.txt`, the reflection dumps,
  disassemblies, `shader_reflect.py`).

# Live A/B hotkey (F9)

`F9` (key down, repeats ignored, handled in the HOT-003 block of `gpu/shim/window.cpp` before
any menu or dialog can consume it) flips `Vulkan::Rt::effects_paused` (`rt/rt_toggle.h`, a
process-wide atomic). `RayTracing::OnDispatch` returns before recording GI, shadows and the
debug view while paused; draws keep feeding the BLAS cache and the TLAS is still built on
demand, so turning the effects back on costs nothing and the GI history is reset once on
re-enable (`history_valid = false`) so no stale ghost shows. One `Ray tracing: effects on/off
(F9)` line per toggle through the `Log` helper. Nothing happens unless `BB_RT=1`: with RT off
the key is swallowed and `OnDispatch` returns at `enabled`.

# Slice 5: hit colour for off-screen and occluded bounce hits

Problem: a bounce ray that hits geometry outside the screen, or behind something in the
G-buffer, had no lit colour to fetch and fell back to the flat sky constant, so GI near the
screen edges and from everything behind the camera was wrong.

Ladder: (1) cannot be skipped, the fallback is visibly wrong; (2)-(6) nothing in the codebase,
the engine or Vulkan gives the lit colour of an unseen surface; the TLAS already carries one
stable id per cached BLAS (`CacheEntry::id`, now written as `instanceCustomIndex`), so the
simplest thing that works is (7a): one average lit radiance per BLAS, learned from the hits
that *are* on screen. (b) per primitive/cluster and (c) texture lookups at hit UVs were not
needed: (c) would also need the game's material binding per draw, which the RT path does not
capture.

Mechanism (`rt_gi.comp`, `rt_gi_apply.comp`, `rt_gi.h`, `vk_ray_tracing.cpp`):

- `material[id]` (vec4 per BLAS slot, `capacity.records` x 16 B = 128 KiB) holds the running
  mean lit radiance (rgb) and the number of resolved frames (w, 0 = unknown, capped at 255).
  `accum[id]` (uvec4) holds this frame's fixed-point sum (1/256 steps, radiance clamped to
  16) and count.
- Learning: every bounce hit that reprojects on screen and passes the depth test already
  fetches the scene colour; 1 in 32 pixels (hash of pixel and frame, first sample only)
  atomically adds that colour to `accum[id]`. No extra rays, no CPU work per draw. Worst
  case at 4K, all adds into one slot: 259k x 16 x 256 = 1.06e9 < 2^32 (`MaterialSumSafe`).
- Resolve: the apply pass (already after the GI pass) uses its first `capacity.records`
  invocations to fold `accum` into `material` (first frame: mean; after that EMA with weight
  1/8) and zero `accum`. The EMA is what removes flicker between frames.
- Use: a hit that is off screen or fails the depth test takes `material[id]` when `w > 0`,
  else the sky constant as before. A missed ray still returns the sky.
- Stale entries: `ReleaseEntry` (evict) and every BLAS rebuild push the id into
  `material_resets`; the next GI pass zeroes those 16-byte slots (both buffers) with
  `fillBuffer` before the dispatch, a full clear when more than 256 are pending or on the
  first GI frame. Refits keep the colour (same mesh, same material).
- Cost: two SSBO bindings, a predicated atomic on 1/32 of the pixels, ~8k resolve threads
  in the apply pass. Not measured live yet (supervisor): expected well under the 0.5 ms
  budget.
- Debug: `BB_RT_GI_DEBUG=8` shows the hit colour source per pixel (averaged over spp):
  green = reprojected scene colour, blue = material cache, red = sky fallback, grey = ray
  missed everything. Expect blue at the screen edges and on surfaces facing away from
  the view after a second of play, red only where no cached BLAS was ever seen on screen.

Shortcut: one colour per BLAS, so a large mesh with mixed materials (a whole building) gives
its average; upgrade to per-cluster (b) only if the live check shows visible colour bleeding
from that. The cache is radiance, not albedo: lighting changes (torch on/off) take ~8
frames of on-screen hits to follow.

## Files and check (slice 5)

- `rt/rt_gi.h`: `MaterialEncode`, `MaterialSampled`, `MaterialSumSafe`, `MaterialResolve`,
  `MaterialValid`, `HitSource`, `GiDebugSource`.
- `rt/vk_ray_tracing.{h,cpp}`: `material`, `material_accum`, `material_resets`, the fills
  and bindings 9/10 (GI) and 5/6 (apply), `ApplyConstants::records`, `instanceCustomIndex`.
- `host_shaders/rt_gi.comp`, `host_shaders/rt_gi_apply.comp`.
- `tests/test_rt_gi.cpp` (`TestMaterialCache`): encode clamps and NaN, sum overflow bound at
  4K, 1/32 sampling, first resolve = mean, EMA convergence, frame cap, zero count keeps the
  stored value.
- Evidence: `out/rt-slice5/check.log` (spirv-val of both shaders, `build.sh`, the three RT
  tests).

# Slice 6: path-traced direct lighting (`BB_RT_DIRECT=1`)

Goal: replace the game's light accumulation with our own, computed from every captured light
with ray-traced visibility, written into the same two B10G11R11 targets so the game's composite,
material, fog and post shade with it. `F9` A/Bs it live; `BB_RT_DIRECT=0` or paused = stock.

Ladder: (1) needed, this is the slice; (2) the light capture (`rt_lights.h`), the shadow pass's
normal/depth reconstruction and ray query (`rt_shadow.comp`) and the F9 toggle are reused; (3)-(5)
nothing in Vulkan or the engine gives a per-pixel light list; (6) one rule for the insertion: let
the game's light pass run and **overwrite** the two accumulation targets just before
`GXLightComposite` (`32e47788`). Justification: no draw suppression code, nothing in the stock
path changes (off/F9 = the game's own accumulation, untouched), debug 3 can read the game's
values from the same image, and the wasted light pass costs the game's own ~1 ms. Shortcut: the
game's light draws still run; skip them when the budget needs it.

Mechanism:

- `Rasterizer::PrepareRenderState`: every light draw (VS `8747a367`) reports its `cb0`/`cb1`
  (`OnLightTargets`); the draw whose pixel shader is `32e47788` calls `OnLightComposite` with
  the same `SceneInputs` the post trigger uses. `RecordDirect` builds the TLAS there (the
  G-buffer draws are all in by then; `Build` is idempotent per frame, the GI reuses it), binds
  depth, the two targets as `r11f_g11f_b10f` storage images, the normal source
  (`BB_RT_GBUF_NORMAL`), G-buffer target `BB_RT_SHADOW_SPEC` (3) as `g_GBuffer3Texture` (rgb
  = `g`, alpha = light-set id) and `BB_RT_DIRECT_G1` (1, 255 off) as `g_GBuffer1Texture` (x
  spec power source, y wrap), and dispatches `rt_direct.comp` in 16x16 tiles (profiler label
  `ray tracing direct`).
- Lights: with direct on, the capture records every light family (point, shadowed, spot,
  directional; `OnLightDraw` no longer filters), parses the full point/spot record
  (`spec_power` [7], `spec` [8..10], `wrap` [12..13], `spot_flag` [15], spot `cone` [16..31])
  and `MergeDirectLights` (`rt_direct.h`) tracks lights by identity: a light drawn this frame
  is a duplicate when it lies within 2 cm (same kind/legacy/radius) of a track already claimed
  this frame, else it moves the nearest unclaimed track of the same kind within 50 cm (radius
  within 25 %, colour ignored), else it opens a new track with the next id. A track lives
  `BB_RT_DIRECT_KEEP` (2) frames after its last draw and the list is emitted **sorted by track
  id** (ReSTIR keys are the ids). Fire fix (rt-fix-fire): the old quantized position key gave
  a moving torch a new key every frame while the stale keys stayed alive for KEEP frames, so
  the hand torch was summed 1-3 times depending on how far it moved (floor luma step 4.65 vs
  0.21 stock); `TestMovingTorchKeepsOneTrack` is red on the old merge. Up to 1024 lights
  (128 B each, host ring) and the frame's `cbLight` (up to 225 sets x 288 B) go to the GPU.
- `rt_direct.comp`: per 16x16 workgroup, the tile's |z| range (shared atomics) and the four
  tile frustum planes cull the light spheres into a shared bit mask that thread 0 compacts in
  light-index order (up to 1024 per tile, the whole light list, so no tile overflow drops lights; debug 2 blue would show one), so the tile list is the
  same every frame (the old `atomicAdd` slot order changed per frame and with it which lights
  got the 8 rays). Per pixel every listed light in range is shaded with the game's BRDF
  (below); every light contributing at least 1% of the strongest light at that pixel gets
  one shadow ray, in tile order, up to `BB_RT_DIRECT_RAYS` (32, max 64) rays (`TerminateOnFirstHit`, bias/tmin/gap from the shadow pass env,
  0.3 m end gap); the rest stay unshadowed. Deterministic: no noise, no temporal accumulation.
- Sun: the per-pixel set id `int(g3.a * 255)` picks the `LightSetParam`; light 1 (`[0]`) is
  traced along its captured world direction (rotated into view with the frame's view rows,
  `tMax` 10 km); the screen-space mask is replaced by `m1 = 1 + f.x (V - 1)`,
  `m2 = 1 + f.y (m1 - 1)` with `f = [17]`, the game's own use of `g_ShadowTexture.x`. If no
  directional draw was captured this frame or target 3 is not RGBA8, the directional sets are
  left out of our result (the whole target is overwritten, so such a frame has no sun at all);
  the first `direct frame` log line and the stats line (`with a directional set`) say so.
- Output clamped to `[0, 64512]` (B10G11R11 max), NaN -> 0, never negative. Sky pixels keep
  the game's values.

BRDF reproduced from the GCN code (`out/rt-slice4/DPointLight1.dis`, `DDirLight1.dis`,
`DSpotLight1.dis`), CPU mirror `PointLightTerms` in `rt_direct.h` checked against hand values
in `tests/test_rt_direct.cpp`:

- Point (`DPointLight1.dis`): `d = L - P` (view space), `atten = max(0, 1/d2 - 1/r2)`,
  `range = 1 - clamp(d2/r2)`, `sp = max(1, [7] * 2^(13 g1.x) / (0.01 * 2^(13 g1.x) + [7]))`,
  `H = normalize(L + V)`. Diffuse `c * clamp(N.L) * atten` when `g1.y == 0`, else the wrap
  branch `w = 10 g1.y`: `atten * (max(0, N.L + w)/(2w+1) + w * [12] * ((1+N.V)/2)^2 *
  clamp(-L.V)^(32 [13]))`. Specular `[8..10] * (N.H)^sp * clamp(N.L)^0.1 * range * (0.25 +
  0.125 sp)`. `mrt0 = diffuse * (1 - g2/2) * ssao`, `mrt1 = g2 * specular * ssao` with
  `g = g_GBuffer3Texture.rgb`.
- Directional (`DDirLight1.dis`): two lights per set, `[0]/[1]` world directions (w = spec
  power term), `[4]/[5]` colours, `[8]/[9]` specular colours, `[17]` mask factors. Diffuse
  `c_i * clamp(N.L_i) * (1 - g2) * m_i` (wrap branch as the point light's, with the
  `(-L.V)^32` back term), specular `s_i * (0.25 + 0.125 sp_i) * (N.H_i)^sp_i * clamp(16 N.L_i)
  * F_i * m_i` with Schlick `F_i = g2 + (1 - g2)(1 - V.H_i)^5`. The game's `g_ShadowTexture.z`
  factor on light 1 (unknown content, likely the SSAO) is taken as 1.
- Spot (`DSpotLight1.dis`, approximate): `q = cone * (P, 1)` with the 4 rows `[16..31]`;
  `q.z <= 0` or `q.w <= 0` = outside; `cone = clamp(1 - (q.x/q.w)^2 - (q.y/q.w)^2)` multiplies
  the point terms (the `[15] > 0` branch; the other branch and the exact falloff power are not
  reproduced). Shortcut: verify with a live spot light in debug 3; upgrade if the cone edge
  differs.

Known differences vs the game (what debug 3 shows even with visibility forced): the SSAO (slot
33, and inside the sun mask) is not captured, so our accumulation is brighter in creases; the
directional sets' extra lights `[2]/[3]` (flag-gated by `[16].xy`) and line lights are not
shaded; shadowed point/spot families are traced instead of using the game's maps (intended).
Which MRT is `g_GBuffer1Texture` is a guess (1); `BB_RT_DIRECT_G1=255` turns it off (spec
power from `[7]` only, no wrap).

Light churn check asked by the supervisor: whether the game drops/re-adds light draws frame to
frame could not be measured offline (the only dump has frames 600 apart and the running game
must not be touched); `BB_RT_LIGHT_DUMP_FRAMES=60` now dumps the first 60 consecutive frames so
it can be counted live (same light = same `pos`/`radius` across blocks). The keep-alive above
covers gaps of up to `BB_RT_DIRECT_KEEP` frames either way.

Environment: `BB_RT_DIRECT=1` (needs `BB_RT=1`), `BB_RT_DIRECT_DEBUG` 1 our diffuse into mrt0
(mrt1 black), 2 tile light count / 16 (blue = overflow), 3 `|game - ours|` per target;
`BB_RT_DIRECT_NOVIS=1` visibility 1 (the correctness proof with debug 3: expect near black
except SSAO creases); `BB_RT_DIRECT_RAYS` (32, max 64), `BB_RT_DIRECT_KEEP` (2),
`BB_RT_DIRECT_G1` (1), `BB_RT_LIGHT_DUMP_FRAMES` (3). Logs: one startup line, three `direct
frame` lines, the stats line every 600 frames (`direct N frames ...`), a skip line whenever the
skip reason changes (with the frame, at most 32).

Cost: not measured live (no game access from the worker). Budget 3 ms: about 1.3 ms for 8 rays
per pixel from the shadow pass numbers, plus the per-tile cull (every light tested once per
tile per 256 threads) and the BRDF per listed light.

Fix 1 (live: every frame skipped). Root cause: `PrepareRenderState` fills `cb_descs` only up to
the highest bound target, so the one-target composite draw (`32e47788`, same VS `8747a367`)
passed the light pass's stale `cb_descs[1]` and its own RGBA16F scene target to
`OnLightTargets`; the B10G11R11 format check then skipped every frame (the 8 logged skips were
all used up in the loading screen). Now only bound targets are passed and `OnLightTargets` keeps
a pair only when both are B10G11R11 (storage is supported: the 4070 Ti reports
`shaderStorageImageExtendedFormats`). "0 with a directional set" followed from the skip (it is
counted after it). Capture cost: about 1000 light draws per frame repeat the same ~26 lights, and
`ApplyCameraMatrix` ran over draws x lights. `LightDedup` hashes each light record (and each
`cbLight` set buffer) before parsing and skips one already seen this frame (first occurrence
kept, so the order is stable); a draw that added nothing reuses its record instead of appending
one (not with `BB_RT_LIGHT_DUMP`, which keeps every draw). The stats line counts the skipped
duplicates.

## Files and check (slice 6)

- `rt/rt_direct.h` (new): GPU layouts, `PackDirectLight`, `DirectKey`, `MergeDirectLights`,
  `PointLightTerms`, `SpecPower`, `SphereInTile`.
- `rt/rt_lights.h`: `Light` BRDF fields and their parsing.
- `rt/vk_ray_tracing.{h,cpp}`: `OnLightTargets`, `OnLightComposite`, `RecordDirect`, the three
  rings, the pipeline, env, logs and stats.
- `host_shaders/rt_direct.comp` (new), `gpu/CMakeLists.txt`.
- `vk_rasterizer.cpp`: the two hooks in `PrepareRenderState`.
- `tests/test_rt_direct.cpp` (`rt-direct-test`): point BRDF vs hand values from the `.dis`
  formula, the wrap branch, tile culling (in/out, z range, large sphere, tile hit count), the
  dedup/keep-alive/stable order, packing and format limits.
- `tests/test_rt_lights.cpp`: `TestDedupBeforeParse` (repeated and partly repeated draws, the
  dir set buffer, reset, clear).
- Evidence: `out/rt-slice6/check.log` (spirv-val, `build.sh`, the four RT tests), fix 1
  `out/rt-slice6/fix1-check.log`.
# Slice 7: hit surfaces (UV capture, albedo texture, alpha test)

Problem: a bounce hit knew only the per-BLAS average colour of slice 5, and alpha-tested
draws (grass, foliage, fences) were opaque walls in the BVH.

Ladder: (1) cannot skip, GI colour and foliage holes are visible; (2)-(6) the RT path captures
only clip positions, nothing in the engine or Vulkan gives the texture at an arbitrary hit;
(7) the smallest thing that works: capture the one VS output that feeds the PS's albedo
sample next to the position, keep the per-triangle UVs with the cached BLAS, bind the albedo
image views of this frame's draws in one partially bound descriptor array, and alpha test in
the ray query candidate loop only for instances flagged alpha tested.

Mechanism:

- `shader_recompiler/ir/passes/shader_info_collection_pass.cpp` `FindAlbedo`: for a PS,
  walk back from the export to MRT2 (then 0, 1, 3) to the first `ImageSample*` whose handle
  is already patched, then from its coordinates to the first `GetAttribute(ParamN, c)`.
  `Info::rt_albedo_image` (image index), `rt_albedo_input` (PS input N), `rt_albedo_components`
  (c | c+1 << 4). Nothing found: -1, the draw falls back to slice 5.
- `emit_spirv_special.cpp` `EmitVertexMotion`: when `MotionVectors::uvs_address` is set and
  the per-draw params word 7 has bit 31 (`Rt::UvSelect`), select the VS output param by a
  select chain over the existing outputs, pick the two components with
  `OpVectorExtractDynamic`, `PackHalf2x16`, store one u32 at `uvs_address + slot * 4`
  (same slot as the clip position). `vk_object_motion.cpp` owns the 32 MiB UV buffer
  (`(1 + 2 * PositionsPerFrame) * 4`), created only with `BB_RT=1`.
- `vk_rasterizer.cpp`: the draw maps the PS input to the VS param through
  `regs.ps_inputs[N].input_offset`, passes `uv_select` to `PrepareDraw` and `alpha_test =
  ps.has_discard` / `uv_captured` in `Rt::DrawInput`. `BindTextures` records the image view
  bound for the PS albedo image (`rt_albedo_view`, respecting descriptor arrays) and the
  draw hands it to `RayTracing::OnDrawBound` right after `BindResources`.
- `rt/rt_surface.h`: `UvSelect`, half packing, `InterpolateUv`, `SurfaceRecord`
  (texture slot, flags AlphaTest|HasUv|HasTexture, uv base, alpha ref), `TextureTable`
  (per-frame view -> slot, slot 0 = none, dedupe, capacity 1024).
- `rt/vk_ray_tracing.cpp` (separate functions `CreateSurfaceResources`,
  `DestroySurfaceResources`, `OnDrawBound`, `WriteSurfaces`): UV pool (`BB_RT_UV_MB`,
  default triangles x 12 B, min 16 MiB) with its own `BlockPool`, a region per cache entry
  (`CacheEntry::uv_base/uv_bytes`, freed through `retired_uv` on release). `DrawRecord` grew
  to 48 B with `uv_base`; `rt_gather.comp` copies the packed UV of each triangle vertex into
  the entry's region (binding 3, `uvs_address` push constant) in the same dispatch as the
  position soup, so a cached BLAS keeps its UVs across frames. `surfaces_ring` (host, one
  `SurfaceRecord` per entry id, rewritten per frame from this frame's instances) is binding
  11 of the GI pass, the UV pool binding 12, and set 1 binding 0 is the combined image
  sampler array (`Slots` sets, partially bound, one own sampler linear/repeat). Texture
  lifetime: a slot is written in `OnDrawBound` while the view is certainly alive and the
  table restarts every frame, so the set only ever holds views bound by draws of the same
  frame; nothing is kept across frames and an evicted image can only be referenced by a
  frame that already used it as a texture.
- `rt_gi.comp`: `HitSurface(instance, primitive, barycentrics) -> albedo, alpha, normal,
  valid` (normal left at 0, no cheap source without the position soup of cached entries).
  `Trace` now runs the candidate loop with `gl_RayFlagsNoneEXT`; instances are forced opaque
  unless alpha tested with a texture (`FORCE_NO_OPAQUE`), and a candidate whose albedo
  alpha at LOD 0 is below `alpha_ref` (0.5) is not confirmed. `CachedRadiance` with a
  valid surface returns `albedo * luma(cached) / max(luma(albedo), 0.05)`: texture detail
  with the brightness of the slice 5 average; without a cache entry `albedo * sky`.
- Device: `descriptorBindingPartiallyBound`, `runtimeDescriptorArray`,
  `shaderSampledImageArrayNonUniformIndexing` are enabled when the device has them;
  `Instance::IsRtSurfaceSupported`. GI now requires them (logged otherwise).
- Debug: `BB_RT_GI_DEBUG=9` albedo of the first bounce hit (magenta = no surface),
  `=10` alpha-test rejections per pixel (red). G-buffer target views moved to 11-19 / 21-29.
- Cost: per draw one hash lookup and at most one descriptor write; gather does 3 extra loads
  and stores per rebuilt triangle; GI: one SSBO read per hit, one texture fetch at LOD 2
  per off-screen hit, candidate loop only on alpha-tested instances. Not measured live.

Shortcuts and what was not checked: albedo identification is a heuristic (first sample
feeding MRT2), unproven on live shaders; alpha threshold is fixed at 0.5 (the PS compare
constant is not read); the texture LOD is fixed; `rt_albedo_view` is one member shared by
the draw and helper-thread bindings; GI without descriptor indexing is off instead of
falling back to slice 5; not run live in this slice.

## Files and check (slice 7)

- `rt/rt_surface.h` (new), `tests/test_rt_surface.cpp` (new, `rt-surface-test`): half
  pack round trip, select encoding, barycentric interpolation, table lifetime (dedupe,
  capacity, per-frame reset), record flags.
- `rt/rt_geometry.h`, `rt/rt_blas_cache.h`, `rt/vk_ray_tracing.{h,cpp}`,
  `host_shaders/rt_gather.comp`, `host_shaders/rt_gi.comp`, `vk_rasterizer.{h,cpp}`,
  `vk_pipeline_common.h` (`HasStage`), `vk_object_motion.{h,cpp}`, `vk_instance.{h,cpp}`,
  `shader_recompiler/info.h`, `runtime_info.h`, `shader_info_collection_pass.cpp`,
  `emit_spirv_special.cpp`.
- Evidence: `out/rt-slice7/check.log`.

# Slice 8: bounces, emissive surfaces, sky on miss (`BB_RT_BOUNCES`)

Problem: a bounce hit returned the game's lit colour (screen or per-BLAS cache) and stopped;
a missed ray returned the flat `BB_RT_SKY` constant. No second bounce, no light from glowing
surfaces, no environment.

Ladder: (1) cannot skip, the slice is the path; (2) the direct pass's light list, cone and
attenuation (`rt_direct.comp`), the UV pool, the material cache and the sky constant are
reused; (3)-(6) nothing in the engine gives the hit normal, the sky cubemap or an emissive
mask without new capture work; (7) the smallest set that works: one packed face normal per
triangle next to the UVs, a Lambert-only light loop at the hit from the same captured list,
the per-BLAS radiance cache as the emissive source above a luma threshold, one extra material
slot that learns the mean on-screen sky colour for off-screen misses.

Mechanism:

- Hit normals (`rt_gather.comp`, `rt_surface.h`): the per-triangle region is now 4 words
  (`TriangleWords`: uv0 uv1 uv2 normal); word 3 is the face normal `cross(v1-v0, v2-v0)` in
  the entry's build space, `packSnorm4x8`. The region exists for every cached entry
  (`SurfaceHasNormal`); `SurfaceHasUv` now also needs the draw's UV capture (`DrawRecord`
  flag 8 / `CacheEntry::uv_captured`, `MakeSurface(..., uv_captured)`), so an entry without
  UVs still has normals. `HitSurface` rotates the local normal with
  `rayQueryGetIntersectionObjectToWorldEXT` and flips it against the ray; without a normal
  the hit uses `-dir`.
- Path (`rt_gi.comp` main): `BB_RT_BOUNCES` (2, max 8) segments per sample. `0` keeps the
  slice 7 behaviour exactly (one segment, `HitRadiance` from screen/cache). With bounces: at a
  hit with a valid surface `sum += throughput * (albedo * DirectAtHit + Emission)`,
  `throughput *= albedo` (Lambert with cosine sampling, so BRDF cos / pdf = albedo, no 1/pi:
  the game's own accumulation has none either, so this matches the scene colour fetched on
  screen). A hit without a surface (no texture) falls back to `HitRadiance` and ends the path.
  Russian roulette from the second segment on: `p = clamp(max(throughput), 0.05, 1)`,
  survive with `u < p`, `throughput /= p` (`PathRoulette`, unbiased: the test checks the mean).
  The first sample's first hit still feeds the slice 5 material cache (1 in 32 pixels).
- `DirectAtHit(p, n, set)`: every captured point/spot light in range (the direct pass's
  `d2 * inv_r2 < 1`, cone test, `atten = 1/d2 - 1/r2`), Lambert `c * clamp(N.L) * atten *
  cone`; the first `BB_RT_BOUNCE_RAYS` (1, max 8) with a non-zero contribution get a shadow
  ray (`Opaque | TerminateOnFirstHit`, end gap `BB_RT_SHADOW_GAP`), the rest are unshadowed.
  The directional set is the primary pixel's (`g3.a * 255`, G-buffer target
  `BB_RT_SHADOW_SPEC`), light 1 shadow-traced 10 km with the game's mask blend `[17]`. Lights
  come from the same capture as the direct pass (`direct_lights_wanted = direct || bounces`,
  so `BB_RT_DIRECT` is not required), packed into the GI's own rings (`gi_lights_ring`,
  `gi_sets_ring`, bindings 16/17) with the frame's view.
- Emission (`Emission`): the game's emissive marking was not found in the time box (not in
  the PS albedo walk, no G-buffer bit identified); fallback as specified: a hit whose per-BLAS
  cached radiance (slice 5) has luma above `BB_RT_EMISSIVE_LUMA` (2.0, 0 off) adds that
  radiance (texture detail from the albedo as `CachedRadiance`) as emission, not multiplied by
  the hit albedo. A merely brightly lit BLAS also qualifies; raise the threshold if it blooms.
- Sky (`SkyRadiance`): on miss, the scene colour at the direction's screen pixel when that
  pixel is sky (depth 1), else the learned mean sky colour, else `BB_RT_SKY`. The mean lives
  in material slot `records` (`SkySlot`; both buffers are `records + 1` entries, the apply
  resolve runs `linear <= records`), fed by the sky pixels of the GI dispatch (1 in 32, same
  EMA as the BLAS cache). The game's sky cubemap draw was not located; the mean has no
  direction.
- Denoiser aux (slice 9 input): the GI pass writes, every GI frame at the GI size, four own
  images exposed by `RayTracing::Denoiser()` (`vk_ray_tracing.h`, `DenoiserInputs`: image,
  view, format each, width, height, frame, valid). `radiance` = the noisy per-frame `raw`
  (RGBA16F, rgb irradiance before the albedo multiply, this frame only, no history);
  `diffuse_albedo` (RGBA16F, the albedo G-buffer target or 1); `specular_albedo` (RGBA16F,
  `g3.rgb^2` or 0); `normal_roughness` (RGBA16F, view-space normal xyz as used by the trace,
  w = `1 - g1.x` approximate, 1 when G-buffer 1 is unbound); `hit_distance` (R16F, the first
  sample's first segment `t`, `1e4` on miss, 0 on sky pixels). All are left in `eGeneral`
  after the GI barrier; the consumer adds its own barrier from compute write.
- Env: `BB_RT_BOUNCES` (2), `BB_RT_BOUNCE_RAYS` (1), `BB_RT_EMISSIVE_LUMA` (2.0). Logs: one
  extra startup line, nothing per frame. F9 off and `BB_RT=0` are untouched (the GI pass does
  not run).

Cost (not measured, no game access): per pixel per sample `segments` rays plus up to
`BOUNCE_RAYS + 1` shadow rays per hit; default 2 segments, 1 spp, 1 light + sun = about 6
rays per pixel, 13 M rays at 2294x960, plus one `lights` loop per hit (about 26 lights live).
Measure with the GPU profiler label `ray tracing GI` (covers the dispatch and the apply pass)
at `BB_RT_BOUNCES=0` vs `2` and `BB_RT_BOUNCE_RAYS=0/1/2`; if over 4 ms, first drop
`BOUNCE_RAYS` to 0 (unshadowed bounce lights), then bounces to 1.

Shortcuts and not checked: no specular at hits (Lambert only); the hit normal is the face
normal (flat shading on bounces); the set id at a hit is the primary pixel's; shadow rays at
hits ignore alpha test; the emissive source is a heuristic on lit colour; the sky mean has no
direction; `BB_RT_BOUNCES=0` was not diffed live against slice 7; aux images are written at
bounces 0 too (needed by slice 9 either way); the UV pool grew from 12 to 16 B per triangle
(`BB_RT_UV_MB` default follows).

## Files and check (slice 8)

- `rt/rt_gi.h`: `GiParams` 256 B (`bounce`, `bounce_cfg`, `bounce_misc`), `BounceFlag*`,
  `PathState`/`PathHit`/`PathMiss`/`PathRoulette`, `RouletteProbability`, `LambertLight`,
  `CacheEmissive`, `Roughness`, `SkySlot`, `PathSegments`.
- `rt/rt_surface.h`: `TriangleWords`, `TriangleNormalWord`, `SurfaceHasNormal`,
  `PackNormal`/`UnpackNormal`, `MakeSurface(..., uv_captured)`; `rt/rt_geometry.h`
  `FlagHasUv`, `SetLastUvBase(base, captured)`; `rt/rt_blas_cache.h` `uv_captured`.
- `rt/vk_ray_tracing.{h,cpp}`: env, `direct_lights_wanted`, the two GI rings, aux images,
  bindings 13-21, `Denoiser()`.
- `host_shaders/rt_gi.comp` (path loop, `DirectAtHit`, `Emission`, `SkyRadiance`,
  `WriteAux`), `rt_gather.comp` (normal word), `rt_gi_apply.comp` (sky slot resolve).
- `tests/test_rt_gi.cpp` (`TestBouncePath`): path sums over two hits and a miss, emission not
  attenuated, roulette probability clamp, kill/survive and unbiased mean over 20k draws,
  Lambert terms, emissive threshold, roughness. `tests/test_rt_surface.cpp`: 16 B triangle
  region, normal pack round trip, normal-only records.
- Evidence: `out/rt-slice8/check.log`.
## Slice 9: DLSS Ray Reconstruction

Goal: NGX "DLSSD" (`nvngx_dlssd.dll`) denoises and upscales the path-traced lighting in one
pass, replacing DLSS Super Resolution when `BB_RT_DLSS_RR=1`. Unset, or whenever RR is not
possible, the DLSS SR path runs exactly as before.

Mechanism:

- Bridge (`gpu/dlss_bridge`, ABI 3): `RayReconstructionAvailable`, `RayReconstructionCreate`
  (feature `NVSDK_NGX_Feature_RayReconstruction`, `NGX_VULKAN_CREATE_DLSSD_EXT1`, HW depth,
  roughness packed in the normal's alpha, flags `MVLowRes | IsHDR | AutoExposure`),
  `RayReconstructionEvaluate` (`NVSDK_NGX_VK_DLSSD_Eval_Params`: colour, depth, motion
  vectors, output, diffuse albedo, specular albedo, normals (+roughness), optional specular hit
  distance) and `RayReconstructionRelease`. RR's instance/device extensions are merged like
  Frame Generation's; availability is `SuperSamplingDenoising.Available` from the capability
  parameters. ABI 1 and 2 bridges still load (RR off).
- Renderer (`vk_dlss.{h,cpp}`): `RayReconstructionAvailable`, `HasRrFeature`,
  `CreateRrFeature`, `EvaluateRr(RrFrame{Frame base; RrGuides guides})`, `ReleaseRrFeature`.
  At `DLSS: ready` time the missing `nvngx_dlssd.dll` (or an NGX refusal) is printed once.
- Guides (`vk_dlss_rr.{h,cpp}`, `host_shaders/dlss_rr_guides.comp`): one 16x16 compute
  dispatch at render size writes three images in General layout: diffuse albedo (RGBA8
  unorm, linear: the sRGB G-buffer target is fetched through its sRGB view, RR rejects sRGB
  formats), specular albedo (RGBA8, G-buffer 3 `g` colour, 0.04 when unbound) and
  world-space normal + linear roughness (RGBA16F; roughness from G-buffer 1 `x` through
  slice 6's `SpecPower` with term 1 then `sqrt(2 / (sp + 2))`, 0.5 when unbound). The normal
  is decoded exactly as `rt_direct.comp` does (`BB_RT_GBUF_NORMAL`, depth-derived by default)
  and rotated into world space with the frame's inverse view rows. Sky pixels (depth 0/1 or
  farther than 100 km) get albedo 0.5, specular 0, normal 0, roughness 1, as the RR guide
  recommends. Native G-buffer targets are transitioned and viewed like `BindScene`; reduced
  scene proxies go through `SceneTargets::Read`.
- Interface for slice 8: `RayTracing::RayReconstructionSource()` (`rt/vk_ray_tracing.h`,
  accessor only) returns the G-buffer target ids, the normal source, the albedo/roughness/
  specular target indices and a `guides_ready` flag with four `Bound` images. When slice 8
  sets `guides_ready` and fills `diffuse_albedo`, `specular_albedo`, `normal_roughness` (and
  `specular_hit`), `DlssGuides::Record` passes them through without the decode pass.
- Integration (`vk_temporal_upscaler.{h,cpp}`): `Rasterizer` hands the upscaler its
  `RayTracing`; `RecordDlss` tries `RecordRayReconstruction` first on both of its call paths
  when `UseRayReconstruction()`: `BB_RT_DLSS_RR=1`, ray tracing on, bridge reports RR, no
  earlier NGX failure. The RR feature is created at the same size/quality as SR would be, with
  the caller's `hdr` flag; the SR/RR switch sets `reset`. A guide failure (G-buffer not
  bindable at the render size this frame) falls back to SR for that frame without resetting
  SR history; an NGX failure prints once and keeps SR for the session.
- Fix 1 (live s9a.log showed SR only): at any output other than 1920x1080 (`Scaled()`,
  `vk_temporal_upscaler.cpp` `Scaled`), `OnDispatch` returns before `Run()` and the upscale
  runs in `RunScaled()` on the tonemapped R8G8B8A8 scene target (`ldr_target`), calling
  `RecordDlss(..., hdr = false)`; slice 9 tried RR only when `hdr` was true, so the 3440x1440
  session never reached it. RR now also runs there with `IsHDR` off. The RR guide asks for
  pre-tonemap colour, but in the scaled path the game's post-processing runs at render size
  between the HDR trigger and the LDR target, so upscaling at the HDR trigger would need every
  later pass at output size (the 1080p `Run()` design); the tonemapped input is the trade-off.
  The guides use `SceneTargets::Read` only when the scene is reduced by the proxy targets
  (`!scaled_session`); a `BB_RENDER_RES` session binds the native G-buffer.

Env: `BB_RT_DLSS_RR=1` (with `BB_RT=1` and the DLSS upscaler selected). Logs, once each:
`DLSS: Ray Reconstruction available` (bridge), `DLSS: Ray Reconstruction unavailable (...)`,
`DLSS: Ray Reconstruction guides WxH, albedo ..., normal ..., roughness ..., specular ...`,
`DLSS: Ray Reconstruction runs on the HDR scene colour before tonemapping (IsHDR on)` or
`... on the tonemapped scene colour of the scaled output path (IsHDR off)`, bridge
`Ray Reconstruction WxH -> WxH, quality q, preset p[, HDR input]`,
`DLSS: BB_RT_DLSS_RR=1 but ...; Super Resolution continues`, `DLSS: Ray Reconstruction
failed; Super Resolution continues`. No per-frame output.

Fetching the DLL and SDK (never committed): `git clone --depth 1 --branch v310.9.16
https://github.com/NVIDIA/DLSS <own empty directory>` (official repo, release tag), then `DLSS_SDK_ROOT=<that dir> CMAKE_EXE=<VS 2022 cmake> bash
packaging/windows/build_dlss.sh` copies `nvngx_dlss.dll`, `nvngx_dlssg.dll`,
`nvngx_dlssd.dll` and the rebuilt `bbport_dlss.dll` to `out/`, and builds
`out/bb-dlss-capabilities.exe` (`tools/dlss_capabilities.c`): `out/bb-dlss-capabilities.exe
<dir with the DLLs>` creates a Vulkan device with NGX's extensions and prints the SR / FG / RR
availability (offline check for slice 9, `out/rt-slice9/capabilities.log`).

Shortcuts and what was not checked: specular albedo skips the guide's `EnvBRDFApprox2`
(the raw specular colour is used); no specular hit distance, no world-to-view / view-to-clip
matrices (only needed with hit distance); roughness mapping from the Phong exponent is a
guess; the DLSS preset for RR is the driver default; not run live in this slice (supervisor).

### Fix 2 (rt-fix-rr): G-buffer normal, RR matrices, specular hit distance

Live all1/s9b (`BB_RT=1 BB_RT_GI=1 BB_RT_DIRECT=1 BB_RT_REFLECT=1 BB_RT_DLSS_RR=1`) showed
flat, white, polygon-faceted patches on props and the character, and the guide line said
`normal depth`.

- Root cause: `normal_source` defaulted to depth (`rt/vk_ray_tracing.h` member init,
  `rt_gi.h` `NormalSource{}` = `NormalFromDepth`) and the live runs did not set
  `BB_RT_GBUF_NORMAL`. So every RT pass (direct `rt_direct.comp` `SurfaceNormal`, GI and
  reflections `rt_gi.comp` `SurfaceNormal` at the primary pixel, the RR guide) used the
  depth-derived face normal of each triangle: one N.L per facet in the replaced direct
  lighting, one mirror direction per facet in the reflections, and a faceted RR guide. The
  reflection hit normal (`HitSurface`, `packSnorm4x8` face normal) only shades the secondary
  hit and is not the cause.
- The game's normal: every `GXLight*`/`D*Light*` shader reads it from
  `g_LightPrepassAttrTexture` (slot 0) as `rgb * 2 - 1`, normalised, and dots it with the
  world-space light direction of `LightSetParam` without a transform (`DDirLight1.dis`
  `0x9c..0xcc`, `0x1fc..0x208`), so it is a world-space unorm normal. `GXLightCmbAlbedo` reads
  `g_GBuffer2Texture` (the sRGB albedo, MRT 2) and the light shaders `g_GBuffer1Texture` /
  `g_GBuffer3Texture` (MRT 1 / 3), so `GBufferN` is MRT N and the attribute texture is MRT 0,
  the remaining RGBA8 unorm target (inference, not yet seen live: `BB_RT_GI_DEBUG=4` against
  `10` confirms it). New default `Rt::GameNormalSource` = `0.rgb.uw`;
  `BB_RT_GBUF_NORMAL=depth` restores the old behaviour. A zero or non-finite texel still falls
  back to the depth normal per pixel.
- RR evaluation now passes `pInWorldToViewMatrix` / `pInViewToClipMatrix` (row-major, row
  vectors: the inverse of the camera's rigid inverse view and `clip = (xs x, ys y, zs z + zo,
  z)`, as frame generation uses), which RR needs to read world-space normals. Bridge ABI 4
  (`BbDlssdEvaluate.has_matrices`, `world_to_view`, `view_to_clip`).
- Specular hit distance: `RayReconstructionSource().specular_hit` is slice 8c's
  `aux_spec_hit` (R16F, General) when reflections wrote it this frame; the guide pass forwards
  it when its size matches the RR input. The other `DenoiserInputs` images (albedo, specular,
  normal) carry the same G-buffer data as the guide pass in RGBA16F and are not used, so
  `guides_ready` stays false. Log once per state: `DLSS: Ray Reconstruction specular hit
  distance from the ray-traced reflections|not given`.
- Cost: the profiler label is now `upscaler Run[Scaled] (DLSS RR|DLSS|TAA|FSR)`. Live, DLSS
  SR's RunScaled was ~1.47 ms (s8a.log) and RR's ~4.5 ms (all1.log) at 2296x960 -> 3440x1440,
  quality 1, driver default preset; the guide pass is one 16x16 dispatch at render size. RR
  model choice: `BB_RT_DLSS_RR_PRESET=D|E|F` (default: driver) for a live A/B; whether any
  preset reaches 2.5 ms is unmeasured.

### Fix 3 (rt-fix-rr2): linear HDR input on the scaled output path

Fix 1 ran RR on the tonemapped `ldr_target` with `IsHDR` off; the RR guide wants linear
pre-tonemap colour. Moving RR to the HDR trigger would need every later post pass at output
size, so the scaled path now inverts the tonemap instead (the reversible-tonemap pattern):

- `host_shaders/dlss_rr_tonemap.comp`, two variants (`dlss_rr_linearize_comp`,
  `dlss_rr_tonemap_comp` with `RR_TONEMAP_FORWARD`). Linearize, at render size into an own
  RGBA16F image: sRGB decode, then `l / (1 - 0.99 max(l))` (peak 100). Tonemap, in place on
  the RGBA16F RR output before sharpening: `l / (1 + 0.99 max(l))`, sRGB encode. The pair is
  an exact inverse (max-channel Reinhard keeps hue), so an identity RR returns the input:
  `out/rt-fix-rr2/roundtrip.log` checks 496k RGB8 channels through an fp16 intermediate,
  0 off by one LSB. The final picture keeps the game's own tonemap, as SR shows it.
- `DlssGuides::Linearize` / `Tonemap` (`vk_dlss_rr.{h,cpp}`); `RecordRayReconstruction`
  linearizes when `hdr` is false and creates the RR feature with `IsHDR | AutoExposure`.
  A linearize failure (pipeline) falls back to Fix 1's LDR input.
- Env `BB_RT_DLSS_RR_HDR`: unset or `1` = linear HDR input (default), `0` = Fix 1's tonemapped
  input with `IsHDR` off. The 1080p `Run()` path already feeds the HDR scene colour and is
  unchanged. The Windows / swapchain HDR state does not select the path (only `Scaled()`
  does: output size or `BB_RENDER_RES`). Log once per path: `DLSS: Ray Reconstruction runs on
  the scaled output path's scene colour inverse-tonemapped to linear HDR, re-tonemapped after
  (IsHDR on), WxH -> WxH`.
- Cost: two 16x16 dispatches (render-size RGBA8 -> RGBA16F, output-size RGBA16F in place)
  inside the `upscaler RunScaled (DLSS RR)` mark. An offline NGX timing loop on blank images
  (RTX 4070 Ti SUPER, `out/rt-fix-rr2/bench-offline-unreliable.log`) was not usable: blank
  inputs and idle clocks gave 0.5 to 26 ms for the same feature, so that tool change was
  dropped; preset and quality cost is measured live with `BB_RT_GPU_TIME`.

## Files and check (slice 9)

- `gpu/dlss_bridge/bbport_dlss_bridge.{h,cpp}` (ABI 3), `packaging/windows/build_dlss.sh`,
  `packaging/windows/package.sh`, `packaging/windows/README.md`, `tools/dlss_capabilities.c`
  (new), `vk_dlss.{h,cpp}`, `vk_dlss_rr.{h,cpp}` (new), `host_shaders/dlss_rr_guides.comp`
  (new), `gpu/CMakeLists.txt`, `vk_temporal_upscaler.{h,cpp}`, `vk_rasterizer.cpp`,
  `rt/vk_ray_tracing.h` (accessor).
- Evidence: `out/rt-slice9/check.log` (bridge build, capabilities run, build.sh).

# Slice 8b: skip the game's light accumulation draws (`BB_RT_DIRECT=1`)

Goal: with direct on, the game's light pass (VS `8747a367` + `GXLightAcc_*` pixel shaders into
the two B10G11R11 targets) is wasted work, since `RecordDirect` overwrites both targets before
`GXLightComposite` (`32e47788`). Its draws are now dropped on the GPU; the CPU-side light capture
(constants read from the guest address at bind time) still runs for every draw.

Ladder: (1) needed, the slice 6 shortcut; (2) reuses the light-draw detection, `OnLightTargets`
and the F9 pause flag; (6) one rule, `Rt::SkipLightDraw` (`rt_direct.h`), a pure function of:
direct enabled, not paused, not the composite draw (same VS, never skipped), the draw blends
(additive accumulation; a non-blended light draw initialises its target and is kept), no depth
write, no stencil write (`stencil_enable` with a non-zero write mask), and `RecordDirect` wrote
these exact two targets **last frame** (`direct_written_frame + 1 == frame`, same image ids).

Consequences of the last-frame rule: F9 off, a `RecordDirect` skip (loading screen, no
geometry, AS not built) or new target images all draw normally from the next frame on, with at
most one frame where the draws were skipped and our pass did not overwrite (the game's own
clear stays); the first frame after F9 on draws normally (our pass overwrites it, then the draws
go). A mid-frame F9 off after draws were skipped still runs `RecordDirect` for that frame
(`light_draws_skipped_this_frame`), so no dark frame on toggle.

Mechanism: `Rasterizer::PrepareRenderState` asks `RayTracing::SkipLightDraw` after
`OnLightTargets` (blend/depth/stencil from the regs); `DrawRecord`/`DrawIndirectRecord` return
after `BindResources` (the capture point) with `ResetBindings`, before `BeginRendering`, so no
draw, vertex/index emit or barrier is recorded. Counter: `N light draws skipped` on the direct
stats line (every 600 frames; no per-frame output).

## Files and check (slice 8b)

- `rt/rt_direct.h`: `LightDrawSkip`, `SkipLightDraw`.
- `rt/vk_ray_tracing.{h,cpp}`: `SkipLightDraw`, `direct_written_*`, `light_draws_skipped`,
  stats line, the paused override in `OnLightComposite`.
- `vk_rasterizer.{h,cpp}`: `skip_light_draw` (light draw path only).
- `tests/test_rt_direct.cpp` (`TestLightDrawSkip`): skip when direct ran last frame; never
  when off, paused, first frame after on, never ran, same frame, composite, other or single
  targets, opaque, depth or stencil writes.
- Evidence: `out/rt-slice8b/check.log` (build exit 0, five rt tests exit 0).

# Slice 8c: ray-traced reflections (`BB_RT_REFLECT=1`)

Where the game applies its reflections (proven from the shipped bundle, disassembled with
bbhost's `gcndis`; the reflection entries are in
`out/rt-slice4/gxrenderershader-reflection.txt:1588-1650,1843-1860`):

- `Filter\GXSsr.ppo` (`6c2323c2`, `GXSsr.vpo` `5a619b89`) is the screen-space reflection pass:
  depth + `g_LightPrepassAttrTexture` (normal), ray-marches `g_PrevFrameTexture` with
  `g_ReprojMatrix` and writes `(colour * fade, fade)` (premultiplied, fade = edge/`g_fadeParam`).
  It is not among the profiled passes of the live logs (`out/rt-live/*.log`), so it is cheap or
  not drawn in the captured scenes.
- `Filter\GXLightDeferredAO.ppo` (`3fe9e964`, same VS `8747a367`, one draw per frame into a
  second B10G11R11 pair, no depth, live cost ~0.1 ms) is where the term enters: it reads the
  light accumulation pair (slots 6/7), `g_RLRTexture` (slot 44, the SSR result, mipmapped,
  `lod = min(6, 6.5 - 6.5 * g1.x)`), `g_ShadowTexture` (slot 32, `.x * .y` = AO, `.w` the
  shadow mask), `g_GBuffer1Texture.x`, `g_GBuffer3Texture.rgb` and `FC_ShadowColor`
  (`cbSceneParam + 304`) and writes `mrt0 = irr * ao`,
  `mrt1 = ao * mix(spec, g1.x^2 * (1 - g^2) * (1 - N.V)^5 * RLR.rgb, RLR.a)` with
  `ao = sx * sy * (1 - spec.a * shadow_colour * (1 - sw))`. So the game's reflection is a
  grazing-only Fresnel lerp into the specular accumulation, and `GXLightComposite`
  (`32e47788`) then reads that pair. `GXEnvDecal[_Deferred]` (`3029db84`/`3b7ae589`) are
  probe decals; neither hash appears in any live log.

Ladder: (1) cannot skip, the slice is the reflections; (2) the GI shader's `Trace`,
`HitSurface`, `DirectAtHit`, `Emission`, `SkyRadiance` and its whole descriptor layout are
reused by compiling `rt_gi.comp` a second time with `RT_REFLECT=1` (`rt_reflect_comp`; no
separate shader file, nothing duplicated); (3)-(6) the engine has no hook to replace the RLR
texture itself without intercepting the AO draw; (7) one compute dispatch at the composite hook
(after `RecordDirect`) rewrites the specular target.

Mechanism (`RecordReflect`, bindings 0-21 as the GI pass plus 22 the specular target as a
`r11f_g11f_b10f` storage image, 23 the irradiance target, 24/25 the pre-AO pair, 26 the specular
hit-distance image; `GiParams` reused: `strength_bias = {strength, bias, max_rough, 0}`,
`bounce_cfg.zw = reflect flags, debug`, `tlas_frame_flags.w = GiFlagNoScreen`):

- Per pixel with depth < 1: roughness `1 - g1.x` (`BB_RT_DIRECT_G1` target; without it the pass
  is skipped and logged, every pixel keeps the game's term). Roughness above
  `BB_RT_REFLECT_MAX_ROUGH` (0.6) or `N.V <= 0`: untouched, hit distance 0.
- One GGX half vector (`alpha = roughness^2`, `GgxSampleHalf`), `L = reflect(-V, H)`; if `L`
  goes below the surface the mirror direction is used instead. One `Trace` (alpha tested):
  miss -> `SkyRadiance` (learned sky mean, then `BB_RT_SKY`; the screen lookup is off through
  `GiFlagNoScreen` because the scene colour at composite time is last frame's), hit with a
  surface -> `albedo * DirectAtHit(hit, face normal) + Emission`, hit without one -> the slice 5
  material cache. Weight `F(F0, V.H) * G1(L) G1(V) V.H / (N.V N.H)` clamped to 1
  (`GgxWeight`), `F0 = g^2` from G-buffer 3 (0.04 when unbound), times
  `BB_RT_REFLECT_STRENGTH` (1).
- Replace, not add: with `BB_RT_DIRECT=1` the target holds our direct specular (no SSR in it),
  so `spec += reflection`. Without direct, `OnLightTargets` remembers the previous distinct
  B10G11R11 pair of the frame (the light pass pair A, before the AO draw wrote pair B) and the
  pass rebuilds `spec_B = ao * (spec_A + reflection)` with `ao = luma(irr_B) / luma(irr_A)`
  (`AoFromIrradiance`), dropping the game's lerp entirely; if no previous pair was seen it adds
  on top (the first three `reflections frame` log lines say which).
- Specular hit distance: `aux_spec_hit` (R16F, GI size, created with the GI images when
  reflections are on), `t` on hit, `1e4` on miss, 0 on sky/untraced pixels;
  `DenoiserInputs::specular_hit_distance` + `specular_frame` (slice 9).
- Env: `BB_RT_REFLECT` (needs `BB_RT_GI=1` for the hit surfaces, logged otherwise),
  `BB_RT_REFLECT_MAX_ROUGH` (0.6), `BB_RT_REFLECT_STRENGTH` (1), `BB_RT_REFLECT_DEBUG`
  (1 reflection only, 2 weight/roughness/miss, 3 hit radiance). Logs: one startup line, three
  `reflections frame` lines, a skip line when the reason changes (max 32), the stats line
  every 600 frames. F9 and `BB_RT_REFLECT` unset: the pass does not run (slice 8 behaviour).
  Profiler label `ray tracing reflections`.

Cost (not measured, no game access): one reflection ray plus up to `BB_RT_BOUNCE_RAYS` + 1
shadow rays at the hit per traced pixel; untraced (rough) pixels cost one texel fetch. Expect
about the slice 6 shadow-ray cost per ray (~0.5-0.8 ms at 2294x960 for the smooth pixels),
within the 1.5 ms target; if over, lower `BB_RT_REFLECT_MAX_ROUGH`.

Shortcuts and not checked: one sample, no temporal accumulation (slice 9 denoises with the hit
distance); the GGX sample uses the NDF, not visible normals; a below-horizon sample falls back
to the mirror (biased toward sharp); `ao` is a luma ratio of the irradiance pair, so the game's
spec-alpha shadow-colour term is folded in approximately; whether the AO draw's input pair is
the previous light pair is inferred from the draw order, not read from its descriptors; the
metal case uses physical `F0 = g^2` where the game uses `(1 - g^2)` grazing only; not run live.

## Files and check (slice 8c)

- `rt/rt_gi.h`: `ReflectTraced`, `GgxAlpha`, `GgxSampleHalf`, `GgxD`, `GgxPdf`, `GgxG1`,
  `SchlickFresnel`, `GgxWeight`, `AoFromIrradiance`, `ReflectFlag*`, `GiFlagNoScreen`.
- `host_shaders/rt_gi.comp` (`RT_REFLECT` entry point, `FlagNoScreen`), `gpu/CMakeLists.txt`
  (`rt_reflect_comp`).
- `rt/vk_ray_tracing.{h,cpp}`: env, `reflect_*`, `acc_prev_*`, `RecordReflect`, `aux_spec_hit`,
  `DenoiserInputs::specular_hit_distance`, `WantsDirect` covers reflections (no rasterizer
  change).
- `tests/test_rt_gi.cpp` (`TestReflect`): roughness cutoff incl. unbound G-buffer 1, GGX half
  vector unit/hemisphere, `1/pdf` estimator of the hemisphere area, `D cos` integrates to 1,
  pdf formula, Schlick ends, G1 monotone, weight bound/zero below horizon, AO ratio.
- Evidence: `out/rt-slice8c/check.log`.

# Slice 8d: ReSTIR DI (`BB_RT_RESTIR=1`)

Goal: replace the slice 6 per-pixel loop over every tile light with up to 8 shadow rays by
reservoir-based spatiotemporal importance resampling (RTXDI style): one shadow ray per pixel,
the same two B10G11R11 targets, so 8b/8c/9 keep working. `BB_RT_RESTIR` unset = slice 6
unchanged (same shader entry, the loop body only moved into `ShadeLight`); needs
`BB_RT_DIRECT=1`. Default off; noisy without slice 9's denoiser (one light sample per pixel).

Ladder: (1) needed when the tile light count makes slice 6 pay 8 rays plus every BRDF per
pixel; (2) reuses the tile cull, the BRDF, the directional path, the GI's reprojection
(`Reprojection`, `CameraCut`) and the stable sorted light list of slice 6; (3)-(5) nothing in
the engine samples lights; (6) one rule: candidates come from the tile list only (the tile
cull is the light grid), so no CPU alias table is needed; the per-tile power CDF is a 64-entry
prefix sum in shared memory.

Mechanism (`rt_direct.comp` compiled three times: slice 6, `RT_RESTIR_PASS=1`, `=2`):

- Pass 1 (temporal, `rt_restir_temporal_comp`): tile cull as slice 6, thread 0 builds the tile
  CDF with weight `luma(color) + luma(spec) + 1e-4`. Per pixel `BB_RT_RESTIR_M` (8, max 16)
  candidates: binary search in the CDF, `pdf = w_i / total`, target `p_hat = luma(unshadowed
  diffuse + specular)`, weighted reservoir update (`w = p_hat / pdf`). Then temporal reuse:
  the pixel reprojects into the previous frame (`reproject` rows, `prev_proj`, `prev_jitter`,
  nearest pixel), the stored reservoir passes when its |z| is within 5% + 2 cm and its
  normal (rotated into the previous view) dots > 0.9, its light id is remapped through
  `remap[]` (`NoLight` = the light left the list), its `M` is clamped to
  `BB_RT_RESTIR_HISTORY` (20, 0 = temporal off) and it merges with
  `w = p_hat_here * W_prev * M_prev`. Written to the temp reservoir buffer.
- Pass 2 (spatial + shade, `rt_restir_spatial_comp`): own reservoir rebuilt as
  `w_sum = W * M * p_hat`, `BB_RT_RESTIR_SPATIAL` (5, max 16) neighbours in a 16 px disc,
  same z/normal test on the stored values, merged with `p_hat` of the neighbour's light at
  this pixel. Final `W = w_sum / (M * p_hat)`; one shadow ray to the chosen light
  (`LightVisibility`, slice 6 bias/gap); output `f(light) * W * vis`, split diffuse /
  specular as slice 6; visibility reuse: a blocked light stores `W = 0` so neighbours and the
  next frame do not pick it. The directional set then runs the slice 6 path unchanged.
- Reservoir = 16 B: light id, `W`, half2 (`M`, `p_hat`), half |z| + 8+8 bit octahedral normal.
  Three device buffers of width x height (prev, next, temp; 105 MiB at 2294x960), created in
  `RecordDirect` when the depth size changes (`CreateReservoirs`, logs the size).
- Reset (`DirectFlagRestirReset`, pass 1 skips the history): first frame, buffer
  re-creation, a frame gap (`restir_last_frame + 1 != frame`: F9 pause, a skipped direct
  frame) or `CameraCut(3 m, cos 0.9)`. Counted in `restir_resets` (stats line `ReSTIR N
  frames, N reservoir resets, N light ids remapped`).
- Light identity: `MergeDirectLights` also returns the sorted `DirectKey`s; `RemapLightIds`
  (merge walk over two sorted key lists) maps last frame's index to this frame's, uploaded
  per frame in `restir_remap_ring` (binding 11).
- Bindings 9/10 are reservoir in/out (prev -> temp, then temp -> next), the first 9 are the
  slice 6 set; `DirectParams` grew to 224 B (`prev_proj`, `reproject`, `prev_jitter`,
  `restir` = candidates, history, spatial taps, previous light count). Profiler label `ray
  tracing restir` (`0xB1A5'0008`) instead of `ray tracing direct`.
- Debug: `BB_RT_DIRECT_DEBUG` 1 and 3 work on the ReSTIR result; 2 (tile count) is written
  by pass 1 and pass 2 leaves it.

Unbiasedness: the RIS estimate `f(y) W` with `W = w_sum / (M p_hat(y))` is unbiased for the
unshadowed sum over the tile list for any candidate pdf > 0 on lights with `f > 0`, and so is
the temporal merge at the same pixel (same target). The spatial merge and the visibility
reuse are the usual biased ReSTIR shortcuts (no Jacobian/MIS; darkening at shadow edges);
the normal/depth test keeps that bias local. The CPU mirror (`Reservoir`,
`ReservoirUpdate`, `ReservoirMerge`, `ReservoirWeight` in `rt_direct.h`) is checked in
`tests/test_rt_direct.cpp`: update selection/weights, `M` clamp and zero-weight merges,
mean of 200k RIS estimates on a 10-light synthetic set within 2% of the brute-force sum for
M = 1/4/8/16, merged pair within 2%, a 200k-frame temporal chain (clamp 20) within 5%,
light id remap incl. a light leaving the list between two `MergeDirectLights` calls.

Cost: per pixel 8 + 1 + 5 BRDF evaluations, 1 reservoir read per tap and 1 shadow ray
instead of up to 8; two dispatches with a compute-to-compute barrier. Not measured live (no
game access from the worker); the 1.0 ms target at 2294x960 is the supervisor's live check.

Shortcuts and not checked: candidates only from the tile list (a light that overflows the
64-entry tile list is never sampled; blue in debug 2 as before); temporal tap is nearest
pixel, no motion vectors beyond the camera reprojection (moving objects fall back to the
similarity test); no MIS/Jacobian in the spatial pass; `M` stored as half (exact up to
2048); sky pixels write an empty reservoir; not run live in this slice.

## Files and check (slice 8d)

- `host_shaders/rt_direct.comp`: `Surface`, `ShadeLight`, `LightVisibility`,
  `DirectionalSets`, `WriteResult`, `MakeSurface`; `RT_RESTIR_PASS` 1/2 code (`Res*`,
  `Pack`, `Similar`, `PackNormal`, `Pcg`), `gpu/CMakeLists.txt` (`rt_restir_temporal_comp`,
  `rt_restir_spatial_comp`).
- `rt/rt_direct.h`: `DirectFlagRestirReset`, `NoLight`, `Restir*` limits, `DirectParams` (224
  B), `Reservoir`, `ReservoirUpdate`, `ReservoirWeight`, `ReservoirMerge`, `RemapLightIds`,
  `MergeDirectLights(..., keys)`.
- `rt/vk_ray_tracing.{h,cpp}`: env (`BB_RT_RESTIR`, `_M`, `_HISTORY`, `_SPATIAL`), startup
  line, `restir_remap_ring`, `CreateReservoirs`/`DestroyReservoirs`, the two pipelines,
  `RecordDirect` reset/remap/params and the two dispatches, stats line.
- `tests/test_rt_direct.cpp`: `TestReservoirUpdate`, `TestReservoirMergeClamp`,
  `TestRisUnbiased`, `TestRemapLightIds`.
- Evidence: `out/rt-slice8d/check.log` (spirv-val of the three variants, `build.sh`, the five
  RT tests).

# Fix: direct lighting too dark, reflections never replace the SSR term (`rt-fix-light`)

Live (`out/rt-live/d0b_ab.png`, `d3b_*`, `all1_ab.png`, `all1.log`): player near-black with
`BB_RT_DIRECT=1` (also with `BB_RT_DIRECT_NOVIS=1`, so not visibility), floor spot near the
feet and puddle specular far brighter than stock, whole scene darker with all slices, and
`reflections ... 0 with the game's pre-AO pair`.

Root causes:

1. Legacy point lights shaded with the wrong BRDF. About half of the live light draws are
   `GXLightAcc_LegacyDPointLight*` (`out/rt-slice4/lights-live.txt`), parsed like the `D`
   family and shaded with its formula (`rt_direct.comp:277,292` at f3aa74b; `rt_lights.h:250`
   dropped the shader's `legacy` flag). `LegacyDPointLight1.dis` differs: attenuation
   `(1 - sqrt(clamp(d2 / r2)))^2` (`0x228..0x2b4`: `1 - sqrt(clamp([3] d2))`, squared), not
   `1/d2 - 1/r2`, and Phong specular `[8..10] (R.L)^(sp/4) (0.25 + 0.125 sp) * atten` with
   `R = reflect(-V, N)` (`0x18c..0x280`), no `N.L^0.1` / range factor. Far from a legacy
   light (character, scene) ours was several times too dark, within ~1 m (floor next to the
   lamp/player light, puddles) far too bright. Fix: `Light::legacy`, packed as
   `wrap_r_flag.w` (the unused spot flag), `LegacyAtten` / `LegacySpecular` in `rt_direct.h`
   and `rt_direct.comp`; the GI/reflection hit lighting (`rt_gi.comp` `DirectAtHit`) uses the
   legacy attenuation too. `DirectKey` keeps legacy and `D` lights apart.
2. The overwrite dropped the game's ambient. Draw order (proven by the logs, not assumed): the
   AO pass `GXLightDeferredAO` (`3fe9e964`) is the **first** light-VS draw of the frame and
   writes the light accumulation pair non-blended (`mrt0 = irr * ao` from
   `g_IrradianceTexture`, `mrt1 = ao * mix(spec, RLR term)`); the light draws then add onto that
   same pair (8b skipped 94% of the light draws by comparing against the pair `RecordDirect`
   wrote, which is only possible if the AO output pair and the light pair are the same), and the
   composite reads it. `RecordDirect` overwrote the pair (`rt_direct.comp:365` at f3aa74b), so
   the irradiance/ambient (SH probes for characters, the bulk of their light at night) and the
   game's environment/SSR specular were lost. Fix: `DirectWriteMode` (`rt_direct.h`): with the
   game's light draws skipped this frame (8b) our lights are **added** to the pair
   (`DirectFlagAdd`), so the target is AO base + our lights; on a frame where the game's draws ran
   (first frame after F9 on, or after a `RecordDirect` skip) nothing is written (stock frame) and
   the pair is marked so the next frame skips the draws; debug 1-3 still overwrite, and debug 3
   no longer skips the game's draws (its diff then includes the AO base).
3. Slice 8c looked for the pre-AO pair as "the previous distinct pair before the AO draw"
   (`vk_ray_tracing.cpp:1475-1480` at f3aa74b), but the AO draw comes first, and `pre_ao`
   required `!direct_enabled` (`:1753`). Fix: the rasterizer reports the AO draw's sampled
   B10G11R11 images (`OnAoInput`, both texture bind paths, `ao_draw` set in
   `PrepareRenderState` only for `3fe9e964`); `Rt::PreAoPair` keeps exactly two distinct ones per
   frame that are not the AO targets (usage order slot 6 then 7; swapped if only the second is a
   G-buffer target); anything else is not a pair. `ReflectUsesPreAo`: pair valid, direct on,
   the game's light draws skipped this frame, debug 0. Then `RecordReflect` runs **before**
   `RecordDirect` and rebuilds `spec = ao * (spec_A + reflection)` with
   `ao = luma(irr_B) / luma(irr_A)` (the pair holds only the AO output at that point), and the
   direct pass adds on top. Without direct (game lights in the pair) or without a pair the
   reflection is added on top as before (logged per frame in the first three lines).

Not checked offline: that every game light draw of a frame is skipped once one is (a non-skipped
blended draw would be counted twice in add mode); the AO texture order; `LegacyDPointLight2..12`
and `LegacyDPointLightShadowed*` assumed to use the same per-light formula as
`LegacyDPointLight1` (only that `.dis` was read). Logs: the `direct frame` lines name the write
mode, the stats line adds `N frames kept from the game`, the reflection lines say
`rebuilt from the AO pass inputs` or `added on top`.

## Files and check (rt-fix-light)

- `rt/rt_lights.h` (`Light::legacy`), `rt/rt_direct.h` (`LegacyAtten`, `LegacySpecular`,
  `DirectFlagAdd`, `DirectWriteMode`, `ReflectUsesPreAo`, `PreAoPair`, `DeferredAoShader`),
  `host_shaders/rt_direct.comp`, `host_shaders/rt_gi.comp` (`DirectAtHit` attenuation only),
  `rt/vk_ray_tracing.{h,cpp}` (`OnAoInput`, `pre_ao_pair`, write mode, reflect order),
  `vk_rasterizer.{h,cpp}` (`ao_draw`, two `OnAoInput` calls).
- `tests/test_rt_direct.cpp`: `TestLegacyBrdf` (hand values from `LegacyDPointLight1.dis`, far
  brighter / near darker than the `D` formula, key and pack), `TestDirectWriteMode`,
  `TestPreAoPairWithSkippedLightDraws` (pair from the AO inputs with 8b skipping, duplicates and
  the AO's own targets ignored, a third input or a new frame invalidates it).
- Evidence: `out/rt-fix-light/check.log`.
- Merged with 8d/8e: `ShadeLight` (slice 6 loop, ReSTIR pass 1 `TargetPdf` and pass 2 shading)
  reads the legacy flag from `wrap_r_flag.w`, so `BB_RT_RESTIR` set or unset uses the same BRDF;
  `DirectFlagAdd` moved to 32 (16 is `DirectFlagRestirReset`) and the add-on-AO-base write is in
  `WriteResult`, used by the slice 6 path and ReSTIR pass 2. `RestirTargetPdf` (CPU mirror of
  `TargetPdf`) is tested by `TestRestirTargetLegacy`. `rt_gi.comp` `DirectAtHit` (also in the ReSTIR GI
  init variant) uses the legacy attenuation.

# Slice 8e: ReSTIR GI (`BB_RT_RESTIR_GI=1`)

Ouyang et al. 2021 on top of the slice 8 bounce paths. Unset = slice 8 behaviour (the
`rt_gi_comp` pipeline, untouched); `BB_RT_RESTIR_GI=1` needs `BB_RT_GI=1` and swaps the one
GI dispatch for four:

1. `rt_restir_gi_init` (`rt_gi.comp` + `RT_RESTIR_GI RT_RESTIR_INIT`): the slice 8 path loop
   unchanged, plus the first sample's first hit goes into a reservoir: position (view space),
   hit normal (flipped towards the ray), outgoing radiance = the whole path sum from that
   hit (sky on a miss, stored at `MissHitDistance` along the ray with normal `-dir`), `M = 1`,
   `W = pi / cos(theta)` (the inverse cosine pdf, so `L * cos / pi * W = L`). Writes the aux
   images (albedo, specular, normal/roughness, first-hit distance) as before; raw/history are
   left to the resolve pass.
2. `rt_restir_gi_temporal` (`rt_restir_gi.comp`): reprojects the receiver with the GI
   reprojection affine, nearest tap, accepts when the stored receiver z is within 10 % and
   the stored receiver normal (rotated into the previous view) dots >= 0.9, the sample is
   younger than `BB_RT_RESTIR_GI_AGE` (32) frames and the reservoir is non-empty. The
   history sample is moved into the current view (`unproject` affine), its `M` clamped to
   `BB_RT_RESTIR_GI_M` (30), merged with `w = p_hat * W * M`, target `p_hat = luma(L) *
   max(cos at the receiver, 0)`. `FlagReset` (camera cut, history invalid, resolution change,
   F9) skips the history.
3. `rt_restir_gi_spatial` (`RT_RESTIR_SPATIAL`), `BB_RT_RESTIR_GI_SPATIAL` passes (1, 0-2) of
   `BB_RT_RESTIR_GI_NEIGHBOURS` (3) taps in a `BB_RT_RESTIR_GI_RADIUS` (16 px) disc, same
   depth/normal similarity. Neighbour samples are reweighted with the solid-angle Jacobian
   `J = (cos phi_r / cos phi_q) * (d_q^2 / d_r^2)` (angles at the sample point), `w = p_hat_r
   * J * W_q * M_q`; a candidate with a zero/negative Jacobian is skipped. One visibility ray
   (opaque, terminate on first hit, `BB_RT_SHADOW_GAP` end gap) per pixel for the selected
   sample when it came from a neighbour; blocked => `W = 0`.
4. `rt_restir_gi_resolve` (`rt_gi.comp` + `RT_RESTIR_RESOLVE`): no rays; `raw = L * cos / pi
   * W` clamped to `BB_RT_GI_CLAMP`, then the slice 5 temporal accumulation and history
   writes exactly as before, so `rt_gi_apply` and the slice 9 denoiser inputs (`radiance` =
   the resampled indirect, aux from pass 1) are unchanged.

Reservoir sets: three sets of four images (`rgba32f` position + M, `rgba16f` radiance + W,
`rgba16f` sample normal + age, `rgba16f` receiver normal + receiver z), 40 B/px/set
(`BB_RT_RESTIR_GI` log line prints the MiB). Per frame the sets rotate: `init = (h+1)%3`,
`temporal = (h+2)%3`, spatial passes ping-pong `temporal -> init -> temporal`, the final set
becomes the next history `h` (`RestirGiFinalSet`). They live with the GI images
(`CreateGiImages`/`DestroyGiImages`), so a resolution change recreates them. Profiler label
`ray tracing restir gi` covers the temporal, spatial and resolve dispatches (the init dispatch
stays under `ray tracing GI`).

Derivation check (the unit test caught it): the pdf of a neighbour's cosine sample expressed in
the receiver's solid angle is `p_q / J`, so the RIS weight is `p_hat * J / p_q`, a multiply by
`J`; dividing by `J` biases the 0.6 m offset synthetic scene by > 3 %.

## Files and check (slice 8e)

- `rt/rt_restir_gi.h` (new): `RestirGiParams` 208 B, `GiReservoir`, `GiInitialReservoir`,
  `GiTargetPdf`, `GiJacobian`, `GiReservoirMerge` (M clamp, weighted selection),
  `GiReservoirFinalize`, `GiResolve`, `GiTemporalAccept`, the set rotation helpers, defaults.
- `host_shaders/rt_gi.comp` (`RT_RESTIR_INIT` / `RT_RESTIR_RESOLVE` paths, bindings 22-25),
  `host_shaders/rt_restir_gi.comp` (new: temporal entry, `RT_RESTIR_SPATIAL` entry),
  `gpu/CMakeLists.txt` (four shader variants, `rt-restir-gi-test`).
- `rt/vk_ray_tracing.{h,cpp}`: env (`BB_RT_RESTIR_GI*`), `restir_gi_*`, reservoir sets in
  `CreateGiImages`/`DestroyGiImages`, 3-slot params ring, four pipelines, `RecordGi` branch,
  `RecordRestirGi`.
- `tests/test_rt_restir_gi.cpp` (new): initial reservoir resolves to its radiance; 8-frame
  temporal merge mean within 3 % of a 400k-sample brute force on a spatially varying ceiling;
  M clamp at 30; Jacobian unit cases; 4-neighbour spatial reuse with the Jacobian within 3 %
  of brute force while the same merge without the Jacobian is off by > 3 %; finalize edge
  cases, set rotation, temporal accept rules.
- Evidence: `out/rt-slice8e/check.log` (build exit 0, spirv-val on six shader variants,
  six rt tests exit 0).
- Not checked here: live frame time (budget <= 1.0 ms at 2294x960) and the visual result;
  supervisor runs live. The spatial pass traces at most one ray per pixel to stay inside that
  budget (Ouyang traces one per candidate).
# Slice 10: cheaper rays on the BVH side (OMM, compaction, GPU timing)

Problem: every alpha-tested instance (slice 7, `FORCE_NO_OPAQUE`) runs the ray query
candidate loop with a texture fetch per candidate; static BLAS keep their worst-case build
size in the pool; nothing measured the GPU cost of each RT pass.

Ladder: (1) cannot skip for foliage-heavy scenes; (2) the UV pool and the per-frame texture
set of slice 7 are reused as the OMM bake input; (7) the smallest set that works: one 4-state
opacity micromap per static alpha-tested BLAS baked from the albedo alpha, ALLOW_COMPACTION
plus a compacted-size query and a copy three frames later, and timestamp pairs around the six
RT passes. Cluster acceleration structures (`VK_NV_cluster_acceleration_structure`, exposed on
the 4070 Ti SUPER, driver 617.14) are not used: static BLAS are cached so their build cost is
already paid once, skinned BLAS are refit (CLAS has no refit), and CLAS would need
cluster-local primitive ids in every hit shader; the probe only reports the extension.

Mechanism:

- `rt/rt_omm.h`: the spec's barycentric -> space-filling-curve index (`OmmSpaceFillingIndex`),
  words per triangle per level, `OmmClassify` (all samples opaque -> opaque, all transparent
  -> transparent, mixed -> unknown opaque so the shader alpha test still decides),
  `OmmTriangleIndex` (uniform triangles use the special indices, no data lookup) and the CPU
  bake `OmmBakeTriangle` the unit test runs. `host_shaders/rt_omm_bake.comp` is the GPU
  version: one thread per triangle, `2^(level+2)` samples per edge of the albedo texture at
  LOD 0 through the slice 7 texture set (set 1), writes the micromap data, the
  `VkMicromapTriangleEXT` records and the index buffer into the slot's input region.
- `vk_instance.{h,cpp}`: `VK_EXT_opacity_micromap` + `micromap` feature when exposed and
  `BB_RT_OMM` is not `0` (`IsOpacityMicromapSupported`); `IsClusterAccelerationStructureExposed`.
  With the EXT enabled, `OpRayQueryProceedKHR` applies the micromaps without any shader change;
  the shaders are untouched.
- `vk_ray_tracing.cpp`: `AllocateEntry(..., omm, ...)` sizes and creates the micromap in the
  OMM pool (`BB_RT_OMM_MB`, 64), chains `VkAccelerationStructureTrianglesOpacityMicromapEXT`
  into the BLAS size query; `AddDraw` wants an OMM for alpha-tested, UV-captured, non-skinned
  draws within `BB_RT_OMM_TRIANGLES` (262144) per frame; `Build` runs the bake after the
  gather, `vkCmdBuildMicromapsEXT` (batched on the scratch buffer), then the BLAS builds with
  the chained index buffer and micromap; an OMM instance carries neither force flag (the
  micromap decides, ray-flag `Opaque` shadow rays still override). `ReleaseEntry` retires the
  micromap block with the entry. `CacheEntry::omm/omm_offset/omm_bytes/compacted`.
- An OMM BLAS can still build without its micromap (no UV slot when the UV pool is full: the
  bake is skipped). `AllocateEntry` and `ScratchBytes` therefore size the BLAS and scratch as
  the larger of the with- and without-micromap queries; sized for the OMM case only, such a
  build overran its scratch into the next BLAS of the batch and lost the device (BLAS build in
  flight right after a micromap build, 4 of 4 manual runs within 2-4 min before, 0 in 5 min
  after with 6 such builds logged). `BB_RT_OMM_LOG=1` logs each micromap build and each skip.
- Compaction (`BB_RT_COMPACT`, default on): `Rt::BlasBuildFlags(updatable, compaction)` keeps
  PREFER_FAST_TRACE for static and FAST_BUILD + ALLOW_UPDATE for skinned, adds
  ALLOW_COMPACTION to static; after the builds `vkCmdWriteAccelerationStructuresPropertiesKHR`
  writes the compacted sizes into a per-slot query pool; `ApplyCompaction(slot)` in
  `BeginSlot` (after the slot's fence) allocates the smaller block when
  `CompactionWorthIt` (<= 90 %), records `vkCmdCopyAccelerationStructureKHR` compact, swaps
  the handle/address and retires the old block.
- Timing (`BB_RT_GPU_TIME`, default on): `TimeBegin/TimeEnd` write timestamp pairs for build,
  GI, direct, ReSTIR, reflections and shadows into a per-slot pool; `ReadTimes` reads them
  with availability after the slot fence; the 600-frame stats print
  `Ray tracing GPU ms/frame: build, GI, direct, ReSTIR, reflect, shadows (...); compaction N
  BLAS, M MiB saved; OMM N BLAS, T triangles, O over budget`. No per-frame output.

Shortcuts and what was not checked: the bake classifies from `2^(level+2)` point samples per
edge, so an alpha feature smaller than the sample spacing inside a micro-triangle can flip a
micro-triangle to opaque/transparent where slice 7's per-hit LOD 0 test would differ (raise
`BB_RT_OMM_LEVEL`, max 5, or `BB_RT_OMM=0`); levels 4-5 use a slow per-micro-triangle loop in
the shader; a static BLAS rebuilt in place (dirty) rebuilds its micromap in place like the
BLAS; the alpha reference is the per-draw `alpha_ref`; not run live in this slice (the
supervisor runs the live PASS), so the bird-curve ordering is proven only against the spec
reference code, not against the hardware.

## Files and check (slice 10)

- `rt/rt_omm.h` (new), `host_shaders/rt_omm_bake.comp` (new), `tests/test_rt_omm.cpp` (new,
  `rt-omm-test`: sizes, curve bijection per level, classify, bake, build flags).
- `rt/rt_blas_cache.h` (`BlasBuildFlags`, `CompactionWorthIt`, entry fields, stats),
  `rt/vk_ray_tracing.{h,cpp}`, `vk_instance.{h,cpp}`, `tools/gpu_capabilities.c`
  (`GPU ray tracing: ray query, opacity micromaps, cluster acceleration structures` line),
  `gpu/CMakeLists.txt`.
- Evidence: `out/rt-slice10/check.log`.

# Fix: the pre-AO pair is never found live (`rt-fix-reflect`)

Live (`out/rt-live/fire2.log`, `s10a.log`, all RT on): `reflections 1570 frames (555 skipped, 0
with the game's pre-AO pair)`, and every reflection line says `added on top` even on frames where
the direct line says `light draws skipped`, so `ReflectUsesPreAo` saw direct on, the game's draws
skipped, debug 0, and the pair was not valid.

Root cause (`rt/vk_ray_tracing.cpp` `OnAoInput`, `:2128-2133` at 56c062f): the AO draw's inputs
were accepted only as `B10G11R11`. `GXLightDeferredAO` reads `g_SpecularAccTexture` (slot 7)
with its alpha (`ao = sx * sy * (1 - spec.a * shadow_colour * (1 - sw))`, slot 8c notes above),
so that input carries an alpha channel and cannot be `B10G11R11`; at most the irradiance input
passed, `count` stayed at 1 and `PreAoPair::Valid` was false every frame. The RT shader reads the
pair with `texelFetch` on `texture2D` (bindings 24/25), so it never needed one format.

Fix: `Rt::ClassifyAoInput` (`rt_direct.h`) accepts an AO-draw sampled image that is HDR float
(`B10G11R11`, or `R16G16B16A16` / `R32G32B32A32` float marked as having alpha), has one mip level
(drops the mipmapped `g_RLRTexture`) and the size of the light accumulation pair; unorm inputs
(shadow mask, G-buffer 1/3, normal) are rejected. `PreAoPair` keeps the alpha flag;
`PreAoPair::Ordered` puts the alpha input in the specular slot, else the old rule (the G-buffer
target is the irradiance). Diagnostics so the next live run proves the AO bindings: the first
24 AO-draw bindings are logged (`Ray tracing: AO draw frame F binds image N <format> WxH levels L
(acc pair d/s this frame|stale) -> own target|rejected|pre-AO input [(alpha)]`), and the first
three frames that want the pair but do not get it log `reflections frame F without the pre-AO
pair: N inputs from frame G [(more than two)], not a pair|not bindable, pair sizes ...`.

Not checked offline: the live formats of the AO inputs (the log above names them); a second HDR
float full-size single-mip input besides slots 6/7 would overflow the pair (logged as `more than
two`). F9 off and `BB_RT_REFLECT` unset: `OnAoInput` returns first, nothing changes.

## Files and check (rt-fix-reflect)

- `rt/rt_direct.h` (`AoInput`, `ClassifyAoInput`, `PreAoPair` alpha + `Ordered`),
  `rt/vk_ray_tracing.cpp` (`OnAoInput` filter and log, `RecordReflect` pair order and log).
- `tests/test_rt_direct.cpp`: `TestPreAoPairWithSkippedLightDraws` adds the live binding set of
  the AO draw (shadow, G-buffer 3, mipmapped RLR rejected, an alpha specular and a B10G11R11
  irradiance give a valid ordered pair; the old B10G11R11-only filter gives none).
- Evidence: `out/rt-fix-reflect/check.log`.
# Fix: GI cost (`rt-fix-giperf`)

Goal: GI pass (bounces 2 + ReSTIR GI) at or under 1.5 ms at 2296x960, from 2.93 ms.

- `BB_RT_GI_SPARSE` (default 4, ReSTIR GI only): the ReSTIR GI init pass runs one thread per
  2x2 block (2: per 2x1 block, 1: per pixel, the old pass). Each thread traces one pixel of
  its block, picked by a per-block hash plus the GI frame counter, so every pixel gets a fresh
  path every 4 (or 2) frames. The other pixels write an empty reservoir (M 0), their receiver
  normal/depth and the denoiser aux images; temporal and spatial reuse fill them, which keeps
  the estimator unbiased (`TestSparseTemporalUnbiased`). Pixels whose previous-frame depth does
  not match (disocclusion) and every pixel on camera cuts are always traced. With sparse on, the
  resolve pass writes the RR hit distance from the reservoir sample it shades. Threads trace in
  lockstep (the traced pixel is iteration 0 of the block loop), so the ray count really drops
  to a quarter instead of idling lanes. Expected: init pass about 4x cheaper.
- `BB_RT_GI_LAST_BOUNCE_CACHE` (default 0): bounces after the first end at the hit's
  screen/material cache colour (slice 5) instead of `DirectAtHit`, like SHaRC termination. Saves
  the light loop and its shadow/sun rays at bounce 2 (4 of 6 rays per traced path left).
- Tests: `rt-gi-test` `TestSparseTrace` (each block index traced once per cycle, dispatch
  covers 2296x960, config packing), `rt-restir-gi-test` `TestSparseTemporalUnbiased`.

## Files and check (rt-fix-giperf)

- `host_shaders/rt_gi.comp`, `rt/rt_gi.h`, `rt/vk_ray_tracing.{h,cpp}`, `tests/test_rt_gi.cpp`,
  `tests/test_rt_restir_gi.cpp`.
- Evidence: `out/rt-fix-giperf/check.log` (spirv-val of the four `rt_gi.comp` variants,
  `build.sh`, all RT tests).
# rt-fix-hitmat: specular at hits, alpha-tested shadow rays from hits

Problem: secondary hits (GI bounces, reflection hits) were Lambert only and their shadow rays
(`DirectAtHit`, ReSTIR GI spatial visibility) used `gl_RayFlagsOpaqueEXT`, so foliage cast
solid shadows and overrode the slice 10 micromaps.

Mechanism:

- `rt_gi.comp` `Blocked` (shared shadow-ray helper of `DirectAtHit` in all four `rt_gi.comp`
  variants): `TerminateOnFirstHit` without the `Opaque` ray flag, candidate loop with the
  slice 7 `AlphaTestPasses`; opaque-forced instances commit without a candidate, OMM
  instances resolve opaque/transparent micro-triangles in hardware and only unknown ones reach
  the shader test. `rt_restir_gi.comp` (temporal + spatial) gets the same helper with its own
  copy of `AlphaTestPasses`, the surface records and UV pool at bindings 15/16 (the GI's
  bindings 11/12 buffers) and the slice 7 texture set as set 1.
- Specular at hits: `DirectAtHit(p, n, set, v, albedo)` now returns
  `c * (albedo + HitSpecular)` per light and for the key directional light (fill light stays
  diffuse). `HitSpecular` is GGX D * Smith G * Schlick F * pi / (4 N.L N.V) (pi: the game's
  no-1/pi convention, as the Lambert term), clamped to 16, alpha = max(r^2, 0.02).
  `HitMaterial(p)`: when the hit projects onto a screen pixel with matching depth (same test
  as `HitRadiance`), F0 = `g3.rgb^2` and roughness `1 - g1.x` from that pixel's G-buffer, which
  is the hit material's own PS output; else F0 0.04, roughness 0.6. Callers pass `v = -dir`;
  path throughput stays `*= albedo` (bounce sampling is still cosine/Lambert).
- Hit normals: still the slice 8 flat face normal. A VS normal capture has no identifying
  signal (the UV capture keys off the PS sample coordinate; nothing marks a VS output as a
  normal), and a smooth normal from shared indices needs a per-vertex accumulator over the
  positions slot space (up to 4M slots per frame), a second gather dispatch and a 6-word
  triangle region shared with the OMM bake; not done in this time box.
- CPU mirror and tests: `rt_gi.h` `HitMaterialFrom`, `HitSpecAlpha`, `HitSpecular`;
  `tests/test_rt_gi.cpp` `TestHitSpecular` (fallbacks, zero below the horizon, cap, white
  furnace with F0 1 below 1.02 and falling with roughness, 0.92/0.69/0.31 at alpha 0.25/0.5/1 (single-scatter GGX loses energy when rough), Fresnel rises at grazing).

Cost (not measured live): one G-buffer projection + two texel fetches per hit, a GGX eval per
lit light; shadow rays from hits now run the candidate loop on alpha-tested, non-OMM instances
(one LOD 0 fetch per candidate).

Not checked: live look and frame time (supervisor); whether `g1.x` is gloss on every material.
F9 off and `BB_RT` unset are untouched (none of these passes run).

## Files and check (rt-fix-hitmat)

- `host_shaders/rt_gi.comp`, `host_shaders/rt_restir_gi.comp`, `rt/vk_ray_tracing.cpp`
  (ReSTIR GI pass layout 17 bindings + set 1), `rt/rt_gi.h`, `tests/test_rt_gi.cpp`.
- Evidence: `out/rt-fix-hitmat/check.log`.
# Emission and sky from game data (`rt-fix-emsky`)

Replaces slice 8's two guesses: emission from "per-BLAS cache luma > `BB_RT_EMISSIVE_LUMA`"
and the sky on miss from one learned mean colour.

## Where the game's emission is (proven offline)

- `Filter\GXLightComposite.ppo` (`32e47788`, the composite the port already hooks as
  `LightCompositeShader`) binds `g_EmissiveAccTexture` at slot 8
  (`out/rt-slice4/gxrenderershader-reflection.txt:1588-1606`). Its GCN code (bbhost `gcndis`)
  samples the irradiance (slot 6, `v[7:9]`), the specular accumulation (slot 7, `v[11:13]`)
  and the emissive texture (slot 8, `v[14:16]`) and writes `irr + spec + emissive`, then fog
  (`v_add_f32 v0, v8, v12` / `v_add_f32 v0, v15, v0` at `0x160..0x18c`). So emission is a
  plain additive HDR term in scene-colour units.
- The G-buffer pixel shaders of emissive materials (`gxflvershader.shaderbnd.dcx`: 102 of the
  292 six/seven-output G-buffer shaders, e.g. `GXFlver_ColDifSpcBumpEmiDOL_Gbuf.ppo` with `g_EmissiveTexture`
  slot 6, `FC_EmissiveColor`, `FC_EmissiveIntensity`) declare seven outputs
  `Color, Color1..4, Specular, Emissive` and export `mrt0..mrt6`; the other 190 export
  `mrt0..mrt5` only. So the game's emissive marker is **colour target 6 of the G-buffer
  pass**, bound only on emissive draws (the live logs print `6 unbound` for the first,
  non-emissive draw); the object-motion attachment is target 7 (`MotionVectors::Output`).

## What the trace does now

- `OnGBufferPass` keeps the last non-null colour target `BB_RT_EMISSIVE_TARGET` (default 6,
  `255` off) of the frame's G-buffer draws (reset every frame). `BindEmissive` binds it at
  binding 27 of `rt_gi.comp` (all four variants) when it is B10G11R11/RGBA16F at the GI size,
  and sets `BounceFlagEmissive` (32).
- `Emission(id, surface, hit)`: with the flag, a hit visible on screen (same depth test as
  `HitRadiance`) returns the game's emissive texel at that pixel, and 1 in 32 pixels add it to
  the per-BLAS emissive cache (`EmissiveSlot(records, id) = records + 1 + id`, same EMA as the
  slice 5 cache, resolved by `rt_gi_apply.comp`); an off-screen or occluded hit returns that
  cache (0 for BLAS never seen emitting). Without the flag (target missing, wrong size or
  `BB_RT_EMISSIVE_TARGET=255`) the slice 8 luma heuristic runs unchanged.
- `SkyRadiance(dir)`: screen sky pixel first (unchanged), then a 32x32 octahedral sky probe
  indexed by **world** direction (`view_rows` transposed), bilinear over learned cells, then
  the learned mean (slot `records`), then `BB_RT_SKY`. `SkyLearn` (the sky-pixel sample in
  `main`, 1 in 32) feeds the mean and the probe cell of the pixel's world direction. The game's
  sky draw / cubemap was not needed: the probe is built from the sky pixels over frames, so it
  follows camera turns and keeps directions that left the screen.
- Material/accum buffers are `MaterialSlots(records) = 2 records + 1 + 1024` entries
  (`[0, records)` BLAS radiance, `records` sky mean, then emissive per BLAS, then the probe);
  the apply resolve covered only `[0, records]` until 2026-11-05 (the emissive cache and the
  probe were accumulated but never resolved, so `SkyProbe` always failed and misses used the
  mean); it now covers every slot. A full material clear (first frame, or more than 256 BLAS
  resets in one frame) keeps the sky mean and the probe unless the frame is a camera cut;
  BLAS resets clear both the radiance and emissive slot.
- Logs: the GI startup line names the emissive target and the probe; the one-time
  `GI albedo target ...` line adds `emissive target 6 (bound|not bound: luma fallback)`.
  No per-frame output. `BB_RT` unset or F9 off: none of this runs.

Not checked: live (supervisor). The per-BLAS emissive cache is a mean over the BLAS, so an
off-screen lantern whose glass is a small part of its mesh glows dimmer than its glass; the
on-screen path is per pixel. Forward-drawn effects (fire particles, FFX) are not in target 6
and stay out of the trace. Shader-side emissive textures (`g_EmissiveTexture` per material via
`shader_info_collection_pass.cpp`) were not wired: the G-buffer target already carries the
final emissive (texture x colour x intensity) the composite uses.

## Files and check

- `host_shaders/rt_gi.comp` (`Emission`, `SkyRadiance`, `SkyLearn`, `SkyProbe`, binding 27),
  `rt/rt_gi.h` (`EmissiveSlot`, `SkyProbeSlot`, `MaterialSlots`, `OctEncode`,
  `SkyProbeCell`, `ViewToWorldDir`), `rt/vk_ray_tracing.{h,cpp}`, `tests/test_rt_gi.cpp`
  (`TestEmissiveSky`).
- Evidence: `out/rt-fix-emsky/check.log`.

## rt-fix-fire: offline replay of the pyre dump

`fire_lights.txt` frame 8400 (Central Yharnam pyre) replayed through the live capture path
(`RecordLightBuffer` with `LightDedup` and record reuse, `ApplyCameraMatrix`,
`MergeDirectLights`, `PackDirectLight`): 15 lights kept, the pyre light
(`LegacyDPointLight4`, view `1.021 -1.598 2.471`, `1/r^2 = 0.04`, colour `6.536 3.257 1.005`)
among them with `legacy = 1` and the view position restored exactly; the same frame's
`127.00 -63.68 30.85` lamp matches the world position the shadow log printed live. The tile cull
(`zsign = +1`, the light-pass projection) keeps it and `PointLightTerms` gives the
`LegacyDPointLight1` formula (`colour * N.L * (1 - sqrt(d^2/r^2))^2 * (1 - g2/2)`), about 4.6 on
red 1 m from the light (`TestFireLightFromDump`). None of these CPU stages zero the pyre
light. Note the light-pass projection (`1/0.3939` vertically): the pyre light at view `y -1.6`
is below the bottom of the screen; the visible floor starts about 4.8 m out, still inside its
5 m radius (about 1.5 on red there).
Not proven offline: the GPU inputs of `rt_direct.comp` at the pyre (depth, normal and
GBuffer1 values, the bound pair) and the GPU tile list.

### Attempt 5 (rt-fix-fire): debug 4 and the GPU-side handoff audit

Read every CPU-to-GPU handoff of the light list (`RecordDirect`, `vk_ray_tracing.cpp:1770-1934`):
`Params` uniform at binding 6 (std140, 224 bytes, slot `frame % 3` x 256, offsets checked by
`static_assert` in `TestPyreTileGridLikeGpu`), the light SSBO at binding 7 (std430 128-byte
`DirectLight`, slot x 131072, range = slot), `tlas_count_flags.z = light_count`, `cfg.w = debug`,
the descriptor layout (`vk_ray_tracing.cpp:886`), the three shader variants (`gpu/CMakeLists.txt:146`),
the post-dispatch barrier, and `inputs.projection` / `inputs.inv_view` (the G-buffer camera, the
same values the GI and shadow passes use). None is wrong by reading. The tile cull has not changed
since slice 6 (`git diff 0fdbff6 HEAD` on the cull block is empty).
`TestPyreTileGridLikeGpu` mirrors the GPU per-tile z range (floor plane at view y -1.65, the GPU's
`abs(z) * 64` quantisation) over the whole 143x60 tile grid: the pyre light stays in every
bottom-row tile and in over 2000 tiles, so the cull math cannot give the black debug 2 frame.
`BB_RT_DIRECT_DEBUG=4` (also in the ReSTIR temporal pass; the spatial pass clears its
reservoir) writes, on every surface pixel: blue 0.5 (the pass wrote and the composite shows it),
red 4.0 where the tile list has at least one light, and a green bar in the top 24 rows,
16 px per light the shader sees in `LightCount()`. Reading: no blue = the dispatch output never
reaches the composite; blue and no green = the uniform's light count is 0 on the GPU; green and no
red = the GPU cull rejects what the CPU mirror keeps (light buffer contents); red = the list is
right and the loss is in `ShadeLight`/`WriteResult` or the composite's scale.
No speculative fix was stacked on the two failed ones; the live debug 4 frame picks the branch.

### Attempt 6 (rt-fix-fire): the proxy write was never marked, the composite read the native image

The live debug 4 frame (`out/rt-live/fdbg4b.png`) had no blue: the "dispatch output never
reaches the composite" branch. Root cause in `vk_scene_resolution.cpp` `SceneTargets::Read`:
with the raster reduced (the live run renders 2296x960 for 3440x1440) every RT pass binds the
accumulation pair through `Read`, which fetched the proxy and transitioned it but left
`Coherence` untouched. `Get` had just copied the native image into the proxy
(`CopiedToProxy`: `dirty = false`), so after our `imageStore` the proxy was "clean". The
composite then bound the pair on the native path (`NativeAccess` in `Runtime::Transit`), which
resolves only when `dirty` is set, and sampled the native image: the game's cleared
accumulation (the light draws were skipped), hence a black deferred scene in every debug mode
and the "too dark" integration build. `Read` now calls `Coherence::ProxyAccess(writes)` =
`ProxyWrite()` for any write access (`TestProxyWriteByReadMarksDirty`), so a later native
access resolves our result and `SampleProxy` keeps serving the proxy. Unaffected with
`BB_RT_DIRECT` unset: `Read` is only used by the RT passes.
Second finding: `OnGBufferPass` only captured the G-buffer targets with `BB_RT_GI=1`, which is
why this branch logged "normal depth, GBuffer3 none, GBuffer1 none" while rt-int (GI on) had
them bound; it now captures them for direct and reflections too.
Evidence lines for the live run: "composite draw frame F samples image N (proxy|native)
WxH; direct wrote images A and B (proxy|native)" (first three direct frames,
`RayTracing::OnCompositeInput`, hooked on both texture binding paths of the
`GXLightComposite` draw).

### Attempt 7 (rt-fix-fire): per-frame order trace (`BB_RT_DIRECT_DEBUG=4`)

Attempt 6 failed live (`out/rt-live/fdbg6.png`: no blue; `fdbg6.log`: the composite samples
image 81 native, the direct pass wrote 80 and 81 native, same frame). What the code says about
the order inside one composite draw (`Rasterizer::DrawRecord`): `PrepareRenderState` ->
`OnLightComposite` -> `RecordDirect` (`scheduler.EndRendering`, `BindScene` transitions, the
dispatch, a compute->all barrier, all into `scheduler.CommandBuffer()`, which syncs the
threaded recorder first) -> `BindResources` (the composite's textures, `OnCompositeInput`) ->
`BeginRendering` -> draw. On paper the dispatch precedes the draw. Two facts in `fdbg6.log` do
not fit that picture: the composite's fragment stage reported exactly one B10G11R11 image per
frame (81) although `GXLightComposite` samples `g_IrradianceTexture`, `g_SpecularAccTexture`
and `g_EmissiveAccTexture` (slots 6/7/8, `out/rt-slice4/gxrenderershader-reflection.txt:1588`),
and `direct 443 frames (354 skipped)` over 354 frames means `RecordDirect` is reached more than
once per frame (a second composite hook per frame, one of them skipped for "no light
accumulation targets, depth or camera"). Which image 81 is (the AO pass's `mrt0` or `mrt1`) and
what the composite binds for the other two slots decides the branch: if 81 is the specular
target (debug 4 writes it black) and the irradiance slot is another image, the dispatch writes
the wrong pair.

No speculative fix (rule: two failed fixes on one problem, this is the third attempt on it).
Instead a debug-only order trace, on with `BB_RT_DIRECT_DEBUG=4` for the first two frames
after the first composite hook, one line per event with a monotonically increasing sequence
number (`RayTracing::Trace`, "Ray tracing: trace seq N frame F: ..."):

- every light-VS draw whose target pair differs from the previous one, and the AO draw
  (`3fe9e964`) with both targets' id, format, size, guest address, usage (`OnLightTargets`);
- the composite hook with the light-VS draw count, the skipped count, which hook of the frame it
  is, the depth/camera validity and the accumulation pair + scene depth images
  (`OnLightComposite`);
- `RecordDirect`: each skip reason, the pass-through, or the dispatch (groups, lights, mode,
  images, native/proxy);
- every image the composite's fragment stage binds, any format, native or proxy, with guest
  address (`OnCompositeInput`), then "composite draw recorded" with its colour target and depth
  (`OnCompositeDraw`, after `BeginRendering`);
- any render pass that binds one of the accumulation images as a colour attachment, with its
  pixel shader and load op (`OnColorTarget` from `BeginRenderingFull`), and any
  `EliminateFastClear` on them (`OnFastClearEliminate`);
- the frame end with the per-frame counts.

Reading the trace: the dispatch line must come before "composite draw recorded" with no
"load CLEAR" or "fast clear eliminate" line on images 80/81 in between; the composite's
sampled images must include both images the dispatch wrote; a second composite hook in the
same frame shows which pair the game composites last. Unchanged with `BB_RT_DIRECT` unset or
debug != 4 (`DirectTrace()` is false; `OnColorTarget`/`OnFastClearEliminate` are only called
with `WantsDirect()`).

Live run (supervisor):
`BB_RT=1 BB_RT_DIRECT=1 BB_RT_DIRECT_DEBUG=4` (as fdbg6), then
`rg -n "trace seq" <log>`; for the whole pass order of one frame add `BB_CAPTURE_DIR=<dir>`
and create the file named by `BB_CAPTURE_TRIGGER` once the scene is up (`frame_<time>.txt`
lists every pass with its targets, shaders and sampled images).

### Attempt 8 (rt-fix-fire): the composite never reads the irradiance image, GXLightCmbAlbedo does

Live trace of attempt 7 (`out/rt-live/ftrace.log`, build c053c79, frames 2755-2756, camera
invalid so no dispatch): the AO pass `3fe9e964` writes `mrt0` = image 81 and `mrt1` = image 82
(both B10G11R11, 2296x960), the shadowed directional light `d92266b9`
(`Filter\GXLightAcc_DDirLightShadowed1.ppo`, `out/rt-slice4/gxrenderershader-reflection.txt:157`)
then blends into the same pair, and `GXLightComposite` (`32e47788`) samples, in its
fragment-binding order, images 83 R8G8B8A8Unorm, 77 R32Sfloat, 87 R8G8B8A8Srgb,
86 R16G16B16A16Sfloat, 82 B10G11R11, 90 R16G16B16A16Sfloat. Its SPIR-V
(`user/cache/CUSA03173/0x0000000032e47788_0_v13.spv`, `spirv-dis`) loads `fs_img0..5` in
that order and uses `.w` of img0, `.x` of img1, `.w` of img2 and `.xyz` of img3/4/5, which with
the reflection's slots (`gxrenderershader-reflection.txt:1588`) gives:

| binding | image | format           | slot | name                 |
|---------|-------|------------------|------|----------------------|
| img0    | 83    | R8G8B8A8Unorm    | 4    | g_GBuffer2Texture    |
| img1    | 77    | R32Sfloat        | 1    | g_DepthTexture       |
| img2    | 87    | R8G8B8A8Srgb     | 5    | g_GBuffer3Texture    |
| img3    | 86    | R16G16B16A16F    | 6    | g_IrradianceTexture  |
| img4    | 82    | B10G11R11        | 7    | g_SpecularAccTexture |
| img5    | 90    | R16G16B16A16F    | 8    | g_EmissiveAccTexture |

Root cause: the composite's irradiance input is not the accumulation image 81 but image 86,
written by `Filter\GXLightCmbAlbedo.ppo` (`8650418f`, reflection line 1577: samples
`g_GBuffer2Texture` slot 4 and `g_IrradianceTexture` slot 6, one colour output; its SPIR-V
multiplies the two, i.e. albedo * irradiance into an RGBA16F target). That draw runs between the
last light draw and the composite. Our hook fired at the composite draw
(`vk_rasterizer.cpp` `PrepareRenderState`, `pgm_hash == LightCompositeShader`), so the direct
pass wrote 81 after `GXLightCmbAlbedo` had already consumed it: the diffuse never reached the
composite, only the specular (82) did, which is why attempts 5-7 saw no blue in debug 4 and no
lighting change in mode 1. Image 90 (emissive) is not part of the pair.

Fix: the hook fires at the first of `GXLightCmbAlbedo` or `GXLightComposite`
(`Rt::DirectHookDraw`, `rt_direct.h`); `OnLightComposite` resets `acc_frame`, so the composite
hook later in the same frame is a no-op ("hook without light targets", traced). The dispatch now
writes 81 and 82 before `GXLightCmbAlbedo` reads 81, so diffuse flows through albedo into 86 and
specular stays in 82, exactly the images the composite samples. Nothing else changes: the
additive mode (`DirectWriteMode`, `SkipLightDraw` from the next frame), the ReSTIR path, the
reflection pre-AO pair (`OnAoInput` keeps collecting the AO pass's inputs, the AO pass precedes
both hooks) and `BB_RT_DIRECT` unset / F9 off (hook only with `WantsDirect()`). Where the game
has no `GXLightCmbAlbedo` draw the composite hook behaves as before.

Trace changes (`BB_RT_DIRECT_DEBUG=4`): the two traced frames start at the first hook where
targets, depth and a valid camera are present ("trace starts: ..." in `RecordDirect`), not at the
first hook, so load frames no longer eat the window; every render-pass colour target is traced
with its format and pixel shader (accumulation targets marked), which shows who writes 86 and 90;
hook lines carry the hook name (`cmb-albedo` / `composite`), as do the sampled-image lines.

Reading the next trace: "cmb-albedo hook ... camera valid" -> "direct dispatch recorded ... wrote
images 81 and 82" -> "cmb-albedo samples (native) image 81" and "image 83" -> "cmb-albedo draw
recorded" with target 86 -> "render target image 86 ... bound by ps 8650418f" -> later
"composite samples" lines including 86 and 82 -> "composite draw recorded" target 89.

Live run (supervisor): `BB_RT=1 BB_RT_DIRECT=1 BB_RT_DIRECT_DEBUG=4` then
`rg -n "trace seq" <log>`; expected on screen: blue diffuse (through albedo) and red specular.
Then `BB_RT=1 BB_RT_DIRECT=1` for the stock-like lit scene. Check: `out/rt-fix-fire/check5.log`.

# rt-fix-foliage: alpha-tested shadow rays in the direct pass

Problem: with `BB_RT=1 BB_RT_DIRECT=1` (no GI) the white flowers and foliage of Hunter's Dream
were dark. The direct pass shadow rays (`rt_direct.comp` `Blocked`, all three variants: slice 6,
ReSTIR DI temporal and spatial) used `gl_RayFlagsOpaqueEXT`, and the hit surfaces (slice 7) were
created only with `BB_RT_GI=1`, so every instance was `FORCE_OPAQUE`: each foliage card was a
solid quad and the key directional light (`DirectionalSets` sun ray) and the point lights were
blocked for anything under or behind a bush. With GI on, only the ray flag was wrong; with
GI off the TLAS had no alpha information at all.

Mechanism:

- `rt_direct.comp` `Blocked`: `TerminateOnFirstHit` without the `Opaque` ray flag, candidate
  loop with its own copy of the slice 7 `AlphaTestPasses` (surface records at binding 12, UV
  pool at binding 13, the slice 7 texture array as set 1). Opaque-forced instances commit
  without a candidate; OMM instances resolve in hardware and only unknown micro-triangles reach
  the shader test.
- `vk_ray_tracing.cpp`: hit surfaces (UV pool, surface records, texture set, `FORCE_NO_OPAQUE`
  for alpha-tested instances, OMM) are on when `BB_RT_GI=1` or `BB_RT_DIRECT=1`; the direct
  and ReSTIR DI pipeline layouts take the surface set layout as set 1, and `RecordDirect`
  binds `surface_sets[slot]` before each dispatch.

Cost (not measured live): the candidate loop runs only on alpha-tested, non-OMM instances
(one LOD 0 fetch per candidate); with direct only, the UV capture and OMM bake now run too.

Live check (supervisor): `BB_RT=1 BB_RT_DIRECT=1` in Hunter's Dream, same view as
`out/rt-live/iso_d.png`: flowers bright as in `iso_g.png` and stock. Log: "hit surfaces on".

## Attempt 2: the directional sets were all zero (root cause)

Live (`out/rt-live/fol_nv2.png` vs `fol_nv2_off.png`, visibility forced to 1): the white
flowers stay dark with the direct pass while the torch-lit ground matches stock, so the missing
term is the cool directional (moon) light, which the flowers show much more than the stone
because their material takes the wrap branch (`g1.y > 0`: `(N.L + 10 g1.y) / (20 g1.y + 1)`,
about half the moon colour at any orientation) while the stone's `clamp(N.L)` is near zero.

Proof offline: every `cbLight` block in `out/rt-live/fire_lights.txt` (the 64800-byte
`LightSetParam` buffer the `DDirLightShadowed1` draw binds, 225 sets x 288 B, dumped by
`BB_RT_LIGHT_DUMP` from the guest address at bind time) is 16200 zero floats in all 25 dumped
frames, while `cbLightAccParam` of the same draw has the camera. The game fills that buffer on
the GPU (`DMA_DATA` memory copies and compute writes go through `Rasterizer::CopyBuffer` /
the buffer cache when either side is GPU-modified, so the CPU page never holds the data), and
`CollectRingBindings` skips GPU-modified constants, so the draw binds the buffer-cache copy
while our capture read the untouched host page. `DirectionalSets` in `rt_direct.comp` then
added `c1 = c2 = 0` for every pixel: no moon in the direct pass at all, since slice 6. The
fire fix's torch tests never showed it (torch-dominated views).

Change:

- `vk_rasterizer.cpp`: the non-ring buffer path of a light draw also reports the guest address
  and size (`RayTracing::OnLightSetBuffer`).
- `vk_ray_tracing.{h,cpp}`: `OnLightSetBuffer` keeps the first `LightSetParam`-sized buffer
  (`IsDirLightSetBuffer`) of the frame as `direct_sets_source`; `RecordDirect` obtains that
  range from the buffer cache (`ObtainBuffer`, read-only: the live GPU data, uploaded from the
  host page when the game wrote it on the CPU instead) and binds it as the `sets` SSBO
  (binding 8) when the offset meets the storage alignment, with a transfer/compute write to
  compute read barrier before the dispatch; otherwise the host copy path stays as before. The
  `RayTracing` constructor takes the `BufferCache`. The `direct frame` lines say
  `bound from the game's GPU buffer` or `bound from the host copy`; the stats line counts
  `N from the game's GPU buffer`.
- `rt_direct.h`: `GpuSetSource`, `GpuLightSetsThisFrame` (same frame, set-sized, not the
  864-byte scene buffer), `GpuLightSetsAligned`; `tests/test_rt_direct.cpp`
  `TestGpuLightSets`.

Not changed (same zero data, out of scope): the CPU parse of the sets for the shadow pass
(`ParseDirectionalSets`) and the GI/reflection bounce `gi_sets_ring` copy of `direct_sets`,
and the `BB_RT_LIGHT_DUMP` `cbLight` block, which still prints the host page.

Live check (supervisor): `BB_RT=1 BB_RT_DIRECT=1`, Hunter's Dream, the `fol_nv2` view, RT
direct vs F9: flowers white as in stock, near and in the mist; the log's first `direct frame`
line must say `bound from the game's GPU buffer`. Second check: `BB_RT_DIRECT_DEBUG=3` with
`BB_RT_DIRECT_NOVIS=1` in the same view: flowers near black in the diff (before: bright).

## Attempt 3: moon for GI/reflection, the CPU parse and the dump; cheaper moon ray

Root cause 1: Attempt 2 bound the game's GPU-resident `LightSetParam` buffer only for the
`rt_direct` `sets` SSBO. The reflection and GI bounces still copied the zero host page
(`direct_sets` -> `gi_sets_ring`), so bounce hits had no moon; `ParseDirectionalSets` (the CPU
parse that feeds the light list and the stats) saw zeros, and `BB_RT_LIGHT_DUMP` printed the
zero page as `cbLight`.

Change 1 (`vk_ray_tracing.{h,cpp}`, `rt_direct.h`):

- `ObtainGpuLightSets(max_age)`: one place that obtains the buffer-cache range of
  `direct_sets_source` (`GpuLightSetsFresh`: same frame for direct, up to one frame old for
  GI/reflection since those passes can run before the frame's light draws). `RecordReflect`
  and `RecordGi` bind it as binding 17 with the same transfer/compute write to compute read
  barrier, and fall back to the host copy only when it is unavailable. Stats line:
  `N GI/reflection frames from it`.
- No stall readback: `RecordDirect` copies the bound range into `sets_readback_ring` (host
  visible, `Slots` deep) after the dispatch barrier and records `{tick, frame, bytes}`.
  `EndLightFrame` (`ApplySetReadback`) picks the newest slot whose tick the scheduler reports
  free (`NewestReadySetReadback`, never waits), overwrites the frame's `LightSetParam` record
  with it, drops the record's directional lights and re-parses them. The dump and the CPU
  parse therefore see the GPU data one to three frames late; the moon is static so this is
  invisible. Stats line: `N CPU set readbacks`.

Root cause 2: with real sets every pixel whose `N.L1 > 0` traced an alpha-tested moon
visibility ray (`Visibility(origin, L1, 1e4)`), so direct GPU time went from 0.69 to 1.075 ms
(the ray was never traced before because `c1` was zero).

Change 2 (`rt_direct.comp`, `rt_gi.comp`): `DirectionalSets` computes both lights' unshadowed
diffuse and specular terms first and traces the moon ray only when it can change the result:
`f.x > 0` (the shadow factor of light 1; `f.y` feeds light 2 through `m1`) and
`f.x * (Luma(dd1 + ds1) + f.y * Luma(dd2 + ds2)) > MoonRayEpsilon` (1e-3 in light accumulation
units, below one 8-bit step after the albedo multiply). The shading result is identical where
the ray is traced (`dd * m1` factorised). The GI bounce ray takes the `f.x > 0` gate. CPU mirror
`MoonRayNeeded` in `rt_direct.h`, `TestMoonRayGate` in `tests/test_rt_direct.cpp`.

Not changed: ray flags (the alpha test needs candidate intersections, so no opaque/cull
flags), ReSTIR passes (only the spatial pass shades, so the moon ray already runs once per
pixel).

Live check (supervisor), Hunter's Dream (flowers, torch, moon):

1. `BB_RT=1 BB_RT_DIRECT=1 BB_RT_GPU_TIME=1`: the image matches rt-int with direct on (flowers
   white, stone, torch, no flicker); the stats line `Ray tracing GPU ms/frame` direct entry is
   below 1.075 ms (target about 0.8), and the direct stats line says `N CPU set readbacks` with
   N growing.
2. `BB_RT=1 BB_RT_DIRECT=1 BB_RT_GI=1` (same view): the GI bounce shows moonlit surfaces (cool
   fill on the shadowed side of the flowers/stone), no new noise; the stats line says
   `N GI/reflection frames from it` with N growing. F9 off unchanged.

## Attempt 4: device lost after an hour with direct only (hit-surface slot mismatch)

Symptom (`out/rt-live/fol2.log`, rt-fix-foliage 53aea72, `BB_RT=1 BB_RT_DIRECT=1`): after about
60k frames `InstructionPointerFault` + `ReadInvalid`, then `ErrorDeviceLost`. Direct GPU time
was 1.07 ms in the Dream, a steady 1.77 ms standing in a 224-instance area and 2.5-4.2 ms while
walking through 2-2.9M-triangle areas with 12-32 lights: it follows the scene and the light
count, not the frame number (no list grows; the light and track counts stay at 12-32).

Root cause: the hit-surface resources are per build slot (`surfaces_ring` slot, the
`surface_sets` descriptor array, written by `OnDrawBound` during the frame and `WriteSurfaces`
in `Build` at the display pass), but the passes that run in the middle of frame N (direct and
ReSTIR in the light composite hook, GI/reflection/ReSTIR GI in the dispatch hook) bound slot
`N % 3`, which still holds the records written by `Build` of frame N-3, while the TLAS they
trace is the one built at the end of frame N-1, and `OnDrawBound` keeps rewriting the
descriptor array of that same set `N % 3` after the dispatch was recorded. A record from frame
N-3 names a texture slot that frame N has not (yet) written, so the dispatch samples the view
frame N-3 put there; when the texture cache has since freed that image (streaming, or the
2 s idle eviction) the view is gone and the read faults. GI and reflection had the same
mismatch since slice 5; `BB_RT_DIRECT` alone started tracing alpha-tested rays with these
resources in 669f44b, which is why the direct-only soak hit it.

The game's `LightSetParam` buffer bound as the `sets` SSBO (Attempt 2) is not it: the arena
chunk it lives in is noted as used by the game's own draw in the same tick (`NoteChunkUse`),
released chunks are unbound only after the tick is free (`FlushUnbinds`), and the stream path
is a per-tick ring; nothing frees it under a recorded dispatch.

Change (`vk_ray_tracing.{h,cpp}`, `rt_surface.h`): `Build` records `built_slot`
(`PassSurfaceSlot(frame, Slots)`); every mid-frame pass binds `surfaces_ring` and
`surface_sets` of `built_slot`, the slot whose records and texture views match the TLAS it
traces, and which nothing rewrites during the frame (`OnDrawBound` writes the current frame's
set, `WriteSurfaces` runs at the display pass into the current frame's slot). The display pass
also stamps `slot_ticks[built_slot]` with the frame's tick, so `BeginSlot` waits for the passes
that read a slot before the CPU rewrites it two frames later. The OMM bake in `Build` keeps the
current slot (same frame, recorded after the writes). Test: `TestPassSurfaceSlot` in
`tests/test_rt_surface.cpp` (the pass slot always holds the build's records; the frame's own
slot holds an older build once the ring wrapped).

Live soak (supervisor): `BB_RT=1 BB_RT_DIRECT=1 BB_RT_GPU_TIME=1`, walk between areas for
5 min: no device lost; the flowers, stone and torch unchanged; `direct` ms follows the area.

# rt-fix-offscreen: lighting and shadows no longer follow the camera angle

User report (Hunter's Dream / Yharnam): "the camera angle is making the shadows and light
change", and "because the angle, some textures gone". Root cause, from the code: everything the
RT passes see is view-dependent capture.

- Lights: `MergeDirectLights` dropped a track `BB_RT_DIRECT_KEEP` (2) frames after its last
  draw. The game frustum-culls its light volumes, so a torch that leaves the screen stopped
  lighting and shadowing the surfaces still on screen two frames later.
- Geometry: the TLAS held only the draws the game issued this frame (frustum and occlusion
  culled). Shadow casters behind the camera and off-screen bounce/reflection targets vanished
  with the turn, and the per-frame `TextureTable` only had slots for this frame's draws.

## Change

- Lights (`rt_direct.h`): a track older than `keep` frames stays alive while its sphere is
  outside the current frustum (`SphereInFrustum`, view from the light draw's `g_CameraMatrix`,
  scales from the frame's projection) and younger than `BB_RT_DIRECT_KEEP_OFF` (1800 frames).
  A light inside the frustum that the game stops drawing dies after `keep` frames as before
  (torch put away, extinguished: no ghost, the flicker fix and the sorted ids are untouched).
  A kept spot light's cone matrix is re-expressed in the current view (`ReprojectCone`).
- Geometry (`rt_blas_cache.h`, `AppendKeptInstances`): every entry drawn this frame records
  the camera position of the sighting and its hit-surface texture (view, image id, uid). At
  the TLAS build, entries not drawn this frame are appended as instances (`KeepInstance`,
  deterministic id order, probe mode 2 = no compare) while they are valid, not dirty, the
  camera is within `BB_RT_KEEP_DIST` (40 m) of the sighting and, for skinned/updatable
  entries, at most `BB_RT_KEEP_SKINNED` (30) frames old (last pose). Static entries have no
  age limit, so standing still never pops; they leave when the camera moves away, when the
  probe flags them dirty on a later draw, or under pool pressure (LRU, unchanged). Cache
  eviction of a kept static entry waits `BB_RT_KEEP_UNUSED` (3600) frames instead of 120.
- Textures: a kept instance re-resolves its albedo through `texture_cache.TryGetImage(id,
  uid)`; if the image was evicted it gets slot 0 (untextured, counted as `untextured`),
  never a stale view. `OnDrawBound` now receives the image id and uid from the rasterizer.
- Compaction and retire: kept instances read `entry_addresses` at build time like drawn ones,
  so the retire rule (free after a TLAS built at or after the retire frame) covers them.

Stats line (`rebuild reasons ...`): `kept N off-screen instances (last frame N, N untextured),
N off-screen lights`.

Kill switches (each `=0` restores the exact pre-keep behaviour of that piece; all default 1,
states printed on the first three `direct frame` lines as `off-screen switches lights N cone N
instances N evict N texresolve N holds N`):

- `BB_RT_OFF_LIGHTS=0`: `MergeDirectLights` gets no keep view (lights drop after
  `BB_RT_DIRECT_KEEP`, no frustum test, no cone reprojection).
- `BB_RT_OFF_CONE=0`: lights still kept, spot cones stay as captured (no reprojection).
- `BB_RT_OFF_INSTANCES=0`: `AppendKeptInstances` is never called (no entry sighting
  bookkeeping, no kept instances, plain 120-frame eviction).
- `BB_RT_OFF_EVICT=0`: instances kept but cache eviction uses the plain 120-frame limit.
- `BB_RT_OFF_TEXRESOLVE=0`: kept instances are appended untextured (slot 0, counted as
  untextured; alpha-tested kept cutouts become opaque).
- `BB_RT_OFF_HOLDS=0`: no `HoldImage` calls (texture cache erases as before 935071b).

## Live steps (supervisor)

`BB_RT=1 BB_RT_DIRECT=1` (the usual set), stand still in the Hunter's Dream near a torch or
lamp and record a slow 360 degree camera rotation, then a second one with `BB_RT_KEEP_DIST=0
BB_RT_DIRECT_KEEP_OFF=0` (old behaviour).

1. Fixed region luma: pick a floor region lit by the torch and keep it in frame while the
   torch leaves the frame. New build: the luma stays within noise across the whole turn. Old
   env: it drops when the torch leaves the frame.
2. Stats line: `kept N off-screen instances (last frame N ...)` grows during the turn and
   `N off-screen lights` is > 0 while the torch is off screen; `BLAS rebuilt` stays small.
3. Quality: walk away 50 m and the kept count returns to 0; enemies that die stay for at most
   30 frames; opening a door while looking at it updates at once (probe). FPS >= 30 with
   `BB_RT_COMPACT=1`.

Not checked live from the worker (no game access). Unit tests: `TestKeepInstances`
(`rt-geometry-test`: distance, dirty, seen, drawn-this-frame, skinned age, eviction limits
and `EvictUnusedBy`), `TestOffscreenLightsKept` (`rt-direct-test`: frustum test, kept behind
the camera, dropped in view, cone reprojection after a 90 degree turn, age cap, redrawn
track keeps its id).

# rt-fix-ambient: flat faceted rocks, white ground patches, black rock pile in the forest

Live (`out/rt-live/tex_gone.png` vs `tex_gone_off.png`, Forbidden Woods, `BB_RT=1
BB_RT_DIRECT=1 BB_RT_GI=1`): the big rock is lit per triangle (sharp flat facets, no normal-map
detail), the ground shows bright flat triangles, the far rock pile is black.

Root cause (from `out/rt-live/pop1.log` line 293, the forest run): the G-buffer layout captured
there was `0 R16G16B16A16Sfloat, 1 R8G8B8A8Unorm, 2 R8G8B8A8Srgb, 3 R8G8B8A8Unorm, 4
B10G11R11, 5 unbound`, while every Hunter's Dream run logged `0 R8G8B8A8Unorm ... 5
R16G16B16A16Sfloat`. `Rasterizer::PrepareRenderState` treats any draw with five or more colour
targets and the scene depth as a G-buffer draw and `OnGBufferPass` **overwrote** `gbuffer_targets`
on every such draw, so the last one before the light pass won. In the forest a later 5-target
draw binds the RGBA16F emissive/scene target as cb0 (cb1-cb4 are the real G-buffer 1-4), so the
normal source `0.rgb.uw` read the RGBA16F image: near-zero texels fall back to the
depth-derived face normal per pixel (`SurfaceNormal`), which is the faceting, and the facets
facing the lamp are the bright flat ground patches. The same wrong normal feeds the GI primary
hit (`rt_gi.comp`), so the GI fill is wrong too. The rock pile is lit in stock mostly by the
moon on surfaces whose face normals point away from it, so it went black.

Change: `Rt::GBufferLayoutReplaces` (`rt_gi.h`): within a frame a G-buffer draw replaces the
kept layout only when it binds **more** colour targets (`gbuffer_bound`, reset per frame with
`gbuffer_count`); the real 6-target G-buffer draws win over the later 5-target one, and the
emissive target is still taken from any draw that binds index 5. Stats line: `N frames with
depth normals` (direct frames where the normal source could not be bound, expect 0) and `N
smaller G-buffer layouts ignored` (expect > 0 in the forest, 0 in the Dream). Test
`TestGBufferLayoutKeepsLargest` (`rt-gi-test`).

Not changed: the moon ray length (10 km; the game's cascaded shadow map ends at
`FC_CascadeSelectDist`, beyond which stock is unshadowed) and `g_ShadowTexture.z`
(`GXRend_CompositeShadowMask`: `.x = SSAO`, `.yz = GBuffer1.z * FC_Mult + FC_Add`, taken as 1).
If the far ground under the canopy stays darker than stock after this fix, those two are next.

Live check (supervisor), same forest spot, RT vs F9:

1. `BB_RT=1 BB_RT_DIRECT=1 BB_RT_GI=1`: the big right rock shows its normal-map texture (no
   flat facets), no bright flat triangles on the ground, the centre rock pile visible; the
   stats line reads `0 frames with depth normals` and a growing `smaller G-buffer layouts
   ignored`.
2. Hunter's Dream (flowers, moon, torch): unchanged vs rt-int2; stats line `0 smaller G-buffer
   layouts ignored` there is fine either way.

## Files and check (rt-fix-ambient)

- `rt/rt_gi.h` (`GBufferLayoutReplaces`), `rt/vk_ray_tracing.{h,cpp}` (`OnGBufferPass`,
  `gbuffer_bound`, `gbuffer_layouts_ignored`, `direct_depth_normal_frames`, stats line),
  `tests/test_rt_gi.cpp`.
- Evidence: `out/rt-fix-ambient/check.log`.

## Attempt 2 (rt-fix-ambient): the moon shadow comes from the game's cascade mask

Live after 8e8e28c (`out/rt-live/amb1_on.png` vs `amb1_off.png`): the rock detail is back, but
RT on is much darker than F9 off (ground path and mid-ground near black, the centre rock pile
barely visible), the brightness changes with the camera angle ("some moon light comes" when
looking down) and the ground shows large hard angular lit/shadowed shards (`sq_full.png`,
`sq2_full.png`).

Root cause (`out/rt-slice4/DDirLightShadowed1.dis`): the stock shader does **not** shadow the
moon with `g_ShadowTexture.x`. It samples the mask with `dmask 0xd` (`.x`, `.z`, `.w`):
`.x` is the SSAO (`m1 = 1 + f.x (x - 1)`, `0x1a8..0x1bc`), and the cascade shadow is
`.z * mix(1, .w, FC_ShadowColor)` (`0x150..0x1b4`, `0x210..0x218`; `FC_ShadowColor` is
`cbSceneParam + 304`, the `s_buffer_load_dwordx4 ... 0x4c` at `0xac`), applied to the diffuse of
light 1 only (`0x2c4`, `0x36c`); light 2 and both speculars are unshadowed. `rt_direct.comp`
replaced the SSAO slot with a hard 10 km ray against the TLAS and dropped the shadow-colour
fill, so: (a) every canopy hit gave `1 - f.x` (black at `f.x = 1`) where stock gives at least
`z (1 - FC_ShadowColor)` of the moon plus the SSAO-only path, (b) the canopy is only in the TLAS
while on screen, so looking down removed it and the ground lit up, (c) the alpha-tested canopy
cards and their OMM state cast the angular shards. The game's cascade mask is filtered and
clipped at `FC_CascadeSelectDist`, which is the stock look.

Change: with `BB_RT_DIRECT=1` the moon term uses the game's mask. The rasterizer flags the
`DDirLightShadowed1` draw (`d92266b9`, `Rt::DirShadowMaskDraw`) and reports its sampled fragment
images (`OnDirShadowInput`, both bind paths; the draw still binds under the 8b skip).
`Rt::PickShadowMask` takes the first one that is neither the scene depth nor a G-buffer target
(the shader samples depth, GBuffer1, GBuffer3 and the mask), `RecordDirect` binds it at binding
14 at the G-buffer size and sets `DirectFlagShadowMask`; `FC_ShadowColor` is read from the
864-byte `cbSceneParam` of that draw (`OnLightBuffer`) into `DirectParams::shadow_color`
(240 bytes). `DirectionalSets` then does exactly the stock maths (`Rt::MoonShadowFromMask` is
the host copy, tested): `m1 = 1 + f.x (mask.x - 1)`, `diffuse += dd1 m1 mask.z mix(1, mask.w,
FC_ShadowColor) + dd2 m2`, speculars `m1`/`m2` only, no moon ray. `BB_RT_MOON=trace` keeps
the old traced moon (also the fallback on a frame without the mask). Torch and lamp shadows are
still traced. Log: the `direct frame` line says `moon shadow game mask (FC_ShadowColor r g b)`,
the stats line adds `N frames with the game's moon shadow mask` (expect = direct frames).

Not changed: the GI bounce (`rt_gi.comp`) still traces the moon at hits; the TLAS contents
(whether shadow-map/depth-only draws add opaque canopy proxies) were not audited: with the game
mask they no longer affect the moon, only torch shadows and GI.

Live check (supervisor), same forest spot, `BB_RT=1 BB_RT_DIRECT=1 BB_RT_GI=1`:

1. RT vs F9: ground path and rock pile visible with similar luma, no angular shards, no change
   with the camera pitch; stats line `N frames with the game's moon shadow mask` growing.
2. `BB_RT_MOON=trace`: the old dark/angular look returns (proves the mask is the difference).
3. Hunter's Dream flowers, moon, torch and fire: unchanged; torch shadows still traced.

Files: `rt/rt_direct.h`, `rt/vk_ray_tracing.{h,cpp}`, `vk_rasterizer.{h,cpp}`,
`host_shaders/rt_direct.comp`, `tests/test_rt_direct.cpp` (`TestMoonShadowMask`). Evidence:
`out/rt-fix-ambient/check2.log` (build exit 0, seven rt tests exit 0).
## Attempt 2: device lost at the forest spot (hit-surface views freed under the RT passes)

Live (`int2b/c/d.log`, Forbidden Woods, 230+ lights, merge rt-int2): `ReadInvalid` page
faults after 3 to 557 direct frames; `amb1.log` (same merge without this branch) ran 12k
frames. Root cause: a hit-surface descriptor slot points at an image view the texture cache
owns. `TextureCache::DeleteImage` erases the image (and its views) once the tick of the
deletion is free, but the RT passes that sample the slot run later: the `built_slot` set is
bound by every pass until the next TLAS build, and a kept instance re-resolved its texture
with `TryGetImage(id, uid)`, which still answers for an image whose erase is pending
(unregistered, `FreeImage` from the collector) and for one the collector will pick next,
because nothing drawn touched it for frames. With keep off the same window exists for a
texture deleted between its draw and the pass (one GC or overlap free in the wrong tick);
the kept path made it a certainty, since kept textures are the collector's LRU victims.

Change:

- `Image::rt_hold_tick` and `TextureCache::HoldImage(id, uid, tick)`: `~0` means held by a
  surface set still in use, any other value is the tick after which the view is free.
  `DeleteImage` of an image RT ever held goes to `held_erases` instead of the deferred
  erase; `EraseHeldImages` (each submit, before the collector) erases it once both the
  deletion tick and the hold are free. `ReclaimMemoryFor` skips held images. The deferred
  path for images RT never used is unchanged.
- `Rt::SurfaceHolds` (`rt_surface.h`): `OnDrawBound` and `AppendKeptInstances` mark each
  bound image held; `FinishFrame` moves this frame's draws to the frame tick, and the
  images of the built slot stay held until a later build replaces that slot (released at the
  tick of the frame that built the replacement, after that frame's passes were recorded).
  `DestroySurfaceResources` releases everything.
- A kept instance's texture is used only while the image is still `Registered`
  (`KeptTextureAlive`); a dying image gives slot 0 as before.
- A kept spot cone is reprojected only when the track recorded a valid seen view
  (`seen_valid`), so a track from a frame without a camera no longer gets a zero cone.

Unit tests: `TestSurfaceHolds` (`rt-surface-test`: release rules, de-duplication, slot
hand-over, same-slot rebuild releases the old list, `ReleaseAll`).

# rt-fix-ambient attempt 1: blocker debug view for boxed foliage shadows

Problem: fern and grass cards in the Old Yharnam courtyard (`out/rt-live/box.png`) cast solid
rectangular torch shadows although `Blocked` alpha-tests candidates (rt-fix-foliage). Offline
every link checked out: all `rt_direct.comp` ray queries (slice 6 loop, ReSTIR DI) run
`TerminateOnFirstHit` without `Opaque`; the old `rt_shadow.comp` opaque pass is off; `has_discard`
comes from `exp ... vm` (any kill); the UV capture selects the VS export param named by
`ps_inputs[rt_albedo_input].input_offset`; the OMM bake samples the same slot and alpha. What
stays unproven offline is per instance: whether the fern draws reach the surface record with
a texture and captured UVs, which texture the albedo heuristic (`RenderTarget2` sample first)
picked, and whether its `.a` is a cutout.

Change: `BB_RT_DIRECT_DEBUG=5` colours each pixel by what the shadow ray to its strongest tile
light meets: black lit; blue committed hit on a forced-opaque instance; cyan committed hit on an
OMM instance's opaque micro-triangle; red first candidate whose record cannot alpha test (no
texture or UV); green = the alpha sampled at the first candidate (blue tint 0.5 when it is an
OMM instance), red raised to 0.5 when the ray ended blocked. Surface records carry
`SurfaceOmm` (16) so the view can tell OMM from forced-opaque instances;
`tests/test_rt_surface.cpp` `TestRecord` covers the flag.

Reading the view at the wall pixels inside a box: blue = the draw is not flagged alpha-tested
or has no texture (detection); cyan = the OMM baked opaque (bake texture); red = UVs not
captured for this entry (uv_select / first build); bright yellow-green = the sampled alpha is
1, so the heuristic picked a texture whose alpha is not the cutout; dim green with leaf shapes
= the shader test works and the box comes from somewhere else (a second draw).

# rt-int3: hit normals from the vertex shader and the material normal map

The gather used to store one flat triangle normal per hit triangle (word 3 of the 4-word UV
record). Hits now carry smooth and mapped normals:

- Capture: the PS info pass (`FindNormalMap`) walks the `RenderTarget0` (world-space G-buffer
  normal) output: its texture sample is the normal map image (`rt_normal_image`), the `Param`
  inputs reached without passing through a sample are the TBN varyings (`rt_normal_inputs`, up
  to two). The rasterizer packs both VS export params into the UV select word
  (`Rt::NormalSelect`, bits 16-27) and the VS capture stores `packSnorm4x8` of each candidate
  per vertex slot in a second buffer (`MotionVectors::normals_address`, 8 bytes per slot).
- Gather (`rt_gather.comp`): the triangle record is 8 words: 3 UVs, flat normal, 3 vertex
  normals, tangent (`xyz` + handedness in `w`, from the UV derivatives of the view-space
  positions). Per vertex the candidate (raw or rotated into view space by the frame's view rows)
  with `|dot| > 0.5` against the flat normal is kept, sign-aligned to it; anything else is 0,
  which falls back to the flat normal at the hit. This self-selects normal vs tangent varyings
  and world vs view space without knowing the shader.
- Hit (`rt_gi.comp HitSurface`): the vertex normals are interpolated with the barycentrics and
  rotated with the instance; when the surface record names a normal map
  (`flags >> 16`, slot in the same hit-texture table as the albedo) it is sampled at the hit UV
  (lod 2), decoded `rg*2-1`, `z` from `b` when present else reconstructed (BC5), and applied in
  the tangent frame. Every stage falls back to the previous normal (flat -> vertex -> mapped) when
  data is missing or the result faces away from the geometric normal.
- Debug: `BB_RT_GI_DEBUG=6` shows the first-bounce hit normal (`n*0.5+0.5`).
- Kept (off-screen) instances keep their vertex normals (they live in the UV pool with the
  entry) but lose the normal map (slot 0) until redrawn.

Unit tests (`rt-surface-test`): `TestNormalSelect`, `TestNormalDecode`, `TestNormalInterpolate`,
`TestPickVertexNormal`, `TestTangent`.

## RTAO and exact spot cones (rt-feat-ao)

Fixlist item 5 ("spot cone approximate, SSAO not captured").

Spot lights, from `out/rt-slice4/DSpotLight1.dis` (`s[32:47]` = floats `[16..31]`, the cone
rows; `s15` = float `[15]`, the spot flag; `s7` = float `[3]`, the radius): `q = cone * (P, 1)`;
`q.z <= 0` is unlit (`0x23c`, the only rejection; `q.w` is not tested, `rcp(0)` gives 0). With
`[15] > 0` (`0x2d4..0x314`): `cone = clamp(1 - (q.x/q.w)^2 - (q.y/q.w)^2)`, `atten = max(0,
1/d2 - 1/r2)`, `range = 1 - clamp(d2/r2)`, the point BRDF times `cone`. With `[15] == 0`
(`0x318..0x350`): `cone = clamp(1 - x^2 - y^2)^2 * clamp(1 - q.w/r)^2` (axial distance, no
`1/d2` term, `range = 1`) and the legacy `(R.L)^(sp/4)` specular. `rt_direct.comp` and
`rt_gi.comp` (`SpotFactor`) do both branches; `PackDirectLight` writes the branch in
`spec_kind.w` (1 falloff, 2 axial) and, for the axial branch, a culling radius `r sqrt(1 +
tan^2 half)` (`SpotCullRadius`, half-angle from the cone rows) into `pos_inv_r2.w` so the tile
list and the GI range test keep the cone's far corners. Local lights are multiplied by the mask's
SSAO (`.x`) like the stock `g_SSAOTexture` sample at `0x3d4`. Host mirror `Rt::SpotFactor`,
test `TestSpotMatchesStock` (register-level transcription of the `.dis` vs the mirror at 7
angles x 7 distances x 2 branches), `TestSpotCullRadius`.

RTAO (`BB_RT_AO=1` default with direct): the rasterizer calls `OnAoDraw` at the
`GXLightDeferredAO` draw (`3fe9e964`, before its resources bind); `RecordAo` builds the TLAS if
needed and dispatches `rt_ao.comp` (`BB_RT_AO_RAYS` cosine rays of `BB_RT_AO_RADIUS` m, alpha
tested through the surface records like the shadow rays) into the `.x` channel of the shadow
mask, found as the R8G8B8A8 AO-draw input that is neither the depth nor a G-buffer target
(`OnAoInput` collects them, `PickAoMask`; the first frame uses the stock SSAO). History: two
RGBA16F images (ao, view z, frame count), bilinear reprojection with the GI camera cut and a 5 %
depth test, blend `1/(n+1)` up to `BB_RT_AO_HISTORY`. The game's SSAO strength `f.x`, the
ambient base and the directional sets are untouched. Stats: `RTAO N frames, N skipped, N mask
changes`, GPU timer `AO`. Debug: `BB_RT_DIRECT_DEBUG=6` (mask AO alone), `7` (spot cone mask),
`BB_RT_AO_DEBUG=1` (no history, raw noise). F9 (effects paused) leaves the stock SSAO.

### Attempt 2 (rt-feat-ao): over-occlusion and cost

Live after 140f3f9 (`out/rt-live/ao_on.png` vs `ao_zero.png`): the whole forest went much
darker (mid-distance rocks, path and trees near black, the ambient haze gone), 64 vs 84 FPS, GPU
timer `AO 1.9 ms`. Root cause: the AO term itself, not the channel. The mask `.x` feeds the
ambient directly (`GXLightDeferredAO`: `mrt0 = irr * sx * sy`, no `f.x` strength), and the pass
counted every hit inside 1 m as full occlusion with 2 binary rays per full-resolution pixel; in a
forest of opaque grass/leaf cards and roots that is 0.3-0.6 over most of the ground, so the
ambient (the haze) collapsed. Fix: hit-distance falloff `w = 1 - t/R` (`AoOcclusionWeight`,
`AoVisibility`), radius 0.5 m (`BB_RT_AO_RADIUS`), and one traced pixel per `BB_RT_AO_SCALE`
(2) block whose position rotates each frame (`AoBlockPixel`) so the history averages the block;
the result is written to every pixel of the block whose view z is within 2 % of the traced one
(`AoBlockKeepsStock`), the others keep the stock SSAO. The history images are block sized (4x
smaller). Calibration readback: the shader sums the stock `.x` (before the write) and ours on a
1/64 grid into a host ring (`AoStatsReadback`), the stats line reads `mask SSAO mean stock X
ours Y (N frames)`; aim for ours within a few % of stock. Test `TestAoEncoding` (visibility
range, `m1 = 1 + f.x (x - 1)` against `MoonShadowFromMask`, block pixel rotation, depth gate,
readback). Not checked offline: the live means and the AO GPU time (expect ~4x less than 1.9 ms).

# Slice FX: smoke and particle cards in the TLAS (`BB_RT_FX_SMOKE`)

Problem: alpha-blended effect draws (smoke, dust, fire sprites) were never captured (the motion
selection only admitted G-buffer draws), so smoke cast no shadow and absorbed no GI, while any
card that had slipped into the TLAS would have been an opaque occluder in reflections and
bounces.

Ladder: (1) cannot skip, the shadow rays have nothing to hit; (2) reuses the capture path, the
surface records and the alpha-test candidate loop; (3) one instance mask bit instead of a
second TLAS; (4) opacity = texture alpha x `BB_RT_FX_SMOKE_OPACITY`, no vertex alpha (not
captured); (5) GI uses a fixed 0.6 so the GI params stay untouched.

- `vk_pipeline_cache.cpp`: with `BB_RT_FX_SMOKE` on, a one-target blended draw into an
  RGBA16F target with the scene depth and no depth write is motion-selected too, so the
  rasterizer's `AddDraw` sees it (`DrawInput::blended`, set from the same blend/depth regs).
- `vk_ray_tracing.cpp`: blended instances get TLAS mask `MaskBlended` (0x02) and
  `FORCE_NO_OPAQUE`; solid instances `MaskSolid` (0x01). They are never kept between frames
  (`CacheEntry::blended`) and their surface record carries `SurfaceBlended` (32).
- Shaders: camera, bounce, reflection, ReSTIR and `rt_shadow` rays trace with mask 0x01, so a
  card is never a hit surface. `rt_direct.comp` `Transmittance` (was `Blocked`) and the
  `rt_gi.comp` light rays trace 0xFF and multiply `1 - alpha * opacity` per blended
  candidate, committing only below 2 % (`DirectParams::fx.x` opacity, `fx.y` debug).
  `BB_RT_DIRECT_DEBUG=4` paints a blended first blocker magenta.
- Env: `BB_RT_FX_SMOKE` (1, 0 off), `BB_RT_FX_SMOKE_OPACITY` (0.6), `BB_RT_FX_DEBUG`.
  Stats line: `N blended draws in the TLAS`.
- Live FAIL of the first cut (41 FPS at the forest, base 84): the capture reused the
  motion-vector pipeline variant, so every blended draw got the extra motion attachment and
  its own render pass. Fixed in 8a88b3e: `PipelineSelection::motion_store`
  (`key.motion_vectors == 2`) is a positions-only vertex variant, no fragment MRT and no
  render-signature change. `Rt::FxSmokeDraw` (`rt_toggle.h`) keeps only SrcAlpha /
  OneMinusSrcAlpha blends (additive = emissive, skipped) of triangle lists with at most 12288
  indices, depth test on, no depth write, RGBA16F target; the draw must sample a texture.
  `BB_RT_FX_CAP` (256) caps blended draws per frame. The old stats line printed stack garbage
  (its printf arguments had gone to the shadows line); it now prints last-frame, total and
  capped counts.
- Fog in-scatter (`BB_RT_FX_FOG`, 272aed2): `rt_fog.comp` marches the camera ray at quarter
  resolution (`BB_RT_FX_FOG_STEPS` 8 since the fx branch, `_ALPHA_DIST` 5 m alpha-tested segment, 4x4 sub-pixel phase per frame, jittered start) with one
  opaque shadow ray to the moon per step, height-falloff density (`_DENSITY` 0.02, `_HEIGHT`
  0.1), Henyey-Greenstein phase (`_G` 0.6), moon direction and colour from directional set 0
  bound by the direct pass (`_MOON` scale). Reprojected and blended 0.9 with the previous
  frame; the `RT_FOG_APPLY` variant upsamples depth-aware and adds `_STRENGTH` x in-scatter to
  the scene colour at the post trigger. `_DEBUG=1` shows the in-scatter alone. GPU timer slot
  `fog`. That first version was off by default (invented medium on top of the stock fog, no
  extinction, 23-28 vs 10.7-10.9 stock luma at the lamp courtyard). Since 2026-11-05 it is on
  by default and replaces the stock fog: see "Traced fog from the game's fog table" below.

## Stock fog: inside GXLightComposite, GI attenuated by its transmittance

Found with `BB_SCENE_DEBUG=<file>` (touch the file: the next frame's render passes and
dispatches go to the RT log as `Scene pass: ...` / `Scene dispatch cs ...`; the dump used to go
through `std::printf` and never reached the log, and it only listed passes after the scene
start, so a full frame was never seen). Frame order at 2296x960: shadow maps and probes, the
G-buffer passes (mrt 0xbf, 6 targets + depth), three `3d5ebf4e` dispatches (light culling),
half-res depth, AO (`3fe9e964`), the light accumulation draws (VS `8747a367`), `8650418f`
(albedo combine), sky (`8cbc58fa`), `GXLightComposite` `32e47788` into the RGBA16F scene colour
with depth bound, then the transparents (`4cd21a0f`/`41ae600f`/`c6cdba2b`), half-res particles
(`855f61b6`, the smoke), and the post chain from `cs 9a9cf8a9`. No separate fog pass exists in
the Dream or in the foggy areas.

`GXLightComposite` (`user/cache/CUSA03173/0x0000000032e47788_0_v13.spv`, `spirv-cross -V`)
binds `cbFog` (slot 6, `FC_FogParamArray FogParam`, declared 192 B, bound 21600 B = 225 sets x
96 B, the same per-pixel set index as the directional sets: `g_LightPrepassAttrTexture.a * 255`,
6 vec4 per set). It writes `scene = irr + spec + emissive`, then:

- `o = CameraMatrix * viewPos` (rotation only: camera-relative world offset, y up),
- `a1 = clamp(v2.x - v2.x * exp2(-(sqrt(o.x^2 + o.z^2) - v2.y) * v2.z), 0, 1)` with vec4 2 =
  (amount, start distance, rate), horizontal distance only;
- `a2 = clamp(v3.x * clamp(v3.z * (o.y + v3.w - v3.y), 0, 1), 0, 1)` with vec4 3 = (amount,
  base, scale, offset);
- `col1 = v1.rgb * (1 + v1.w * (luma(scene) - 1))`, `col2` the same from vec4 4;
- `out = lerp(lerp(scene, col1, a1), col2, a2)`.

So light `g` added before the fog comes out as `g * T + luma(g) * K` with
`T = (1 - a1) * (1 - a2)` and `K = a1 * (1 - a2) * v1.w * v1.rgb + a2 * v4.w * v4.rgb` (the
luma term is not small: `w` is 0.46 in the Dream, so dense fog still passes about half of the
GI's luma tinted in the fog colour). The RT reproduces exactly that: `Rasterizer` reports the
composite draw's (`32e47788`) fragment buffers (`OnCompositeBuffer`), the one of size >= 192
and a multiple of 96 is the fog table; it is GPU-written every frame (the CPU copy reads zeros),
so it is snapshotted with a GPU copy into `gi_fog_ring` (slot `frame % Slots`, 32 KiB) at the
composite draw. `rt_gi_apply.comp` binds depth (7), G-buffer 3 (8), the GI `Params` block (9,
the vec4 count travels in `bounce_cfg.z`) and the table (10), reconstructs the camera-relative
world offset per pixel (`PixelToView`, `WorldDir` as in `rt_gi.comp`) and adds
`gi * T + dot(gi, luma) * K`. `BB_RT_GI_FOG=0` (live) disables it, `BB_RT_GI_DEBUG=27` shows
`T` as grey. The log line `Ray tracing: fog sets buffer ...` prints set 0 of the snapshot every
600 frames. Hunter's Dream, set 0: colour (1, 0.647, 0.416) w 0.46, distance amount 0 (start
454, rate 0.0005), height amount 0.893 base -16.8 scale -0.017, offset -5.35/-3.93 (it moves
with the camera height), so the fog only starts ~11 m below the camera and T = 1 in the Dream,
as measured with the debug view; the Dream's visible mist is particles, not this fog.

## Traced fog from the game's fog table (2026-11-05)

`BB_RT_FX_FOG` defaults to on. `OnCompositeBuffer` still snapshots the composite's fog table
into `gi_fog_ring`, and when the traced fog will run this frame (`fog_enabled`, not
`BB_RT_FOG_STOCK=1`, fog pipeline present, TLAS instances, effects not paused) it zero-fills the
game's table right after the copy, so `GXLightComposite` applies no stock fog and
`rt_gi_apply.comp` gets `T = 1` (its `fog_bound` is only set when the stock fog still applies).
`rt_fog.comp` binds G-buffer 3 (9) and the table (10, `FogFlagTable`, vec4 count in
`prev_jitter.w`) and reads the pixel's set (`G-buffer3.a * 255`, sky pixels take the first
non-sky pixel below them): vec4 2 = (amount, start, rate) gives the transmittance
`T(s) = exp2(-max(s - start, 0) * rate)` along the view ray (horizontal distance in stock; the
ray length here), vec4 1 the fog colour `col`. The march (quarter res, 4x4 sub-pixel phase,
`BB_RT_FX_FOG_STEPS` 8 steps to `BB_RT_FX_FOG_DIST` 60 m) weights each step by
`T(s0) - T(s0 + ds)` and gathers three traced terms at the step point `x`:

- moon: one shadow ray to the pixel's directional set light 1 (per-pixel set id, never set 0:
  the game renumbers sets per frame and set 0 alternates between the warm moon and a blue set,
  which made the sky veil blink); `sum_m += w * vis * col`, times `4 pi HG(dir.L)` and
  `c1 / (4 pi ref)`;
- lamps (bindings 11 = the GI light buffer + grid, `BB_RT_FX_FOG_LIGHTS` scale): weighted
  reservoir sampling of one lamp from the grid cell of `x` by `luma(colour * atten * cone)`,
  shadow ray to the lamp, estimator `e * wsum / w * vis * 4 pi HG(dir.l)`; the fog pass writes
  the lights ring itself (`WriteGiLights`) when the GI did not this frame;
- sky (binding 12 = the GI material buffer, `BB_RT_FX_FOG_SKY` scale): one direction sampled
  from the HG phase around the view ray (`PhaseSample`, exact inversion), radiance from the
  sky probe / mean slot (world directions above the horizon only), visibility ray to 1e4 m,
  estimator `L(w) * vis` (no phase factor: the phase is the sampling pdf).

Units: the table colour is the fog's lit colour under the artists' ambient, so it is treated
as `albedo * reference` with `reference = luma(mean sky)` (`SkyReference`, 1 when the sky is
unknown): `moon = sum_m * phase * c1 / (4 pi ref)`, `lamps = col * sum_l / (4 pi ref)`,
`sky = col * sum_s / ref`; an unoccluded uniform sky reproduces `col` exactly. Beyond the
march distance the moon and the mean sky are added unshadowed. The apply pass:
`result = scene * (1 - amount * (1 - T)) + amount * scatter`, then the stock height-fog mix
(flat colour, still to do). `BB_RT_FX_FOG_DEBUG` 1 scatter, 2 lamps only, 3 sky only, 4 moon
only. Measured in the Yharnam tower square (hot45, sky region of 255): traced 49/38/52 (stable
over six rapid frames after the per-pixel set fix), stock 26/24/54; fog pass 0.63 ms (was
0.11 with the moon only). Known: the sky veil is warmer and about 2x the stock one because
the game's moon irradiance is ~16x its mean sky radiance; calibration is listed in
our plan notes with the other open items (height fog, remainder,
lamp term check at a lantern).

Other defaults changed the same day, all for the "no fake light" rule: the game's particle
fog sheets (ps `71c96598`, also the fire smoke; ps `a28d2a7a`, the flat mist sheet over flooded floors, added 2026-11-07) are skipped, as are (2026-11-07) all blended light draws into the half-res light buffer (1148x480, vs `8747a367`, e.g. ps `31b90cc5`, a hard-edged patch on a dark floor; `RayTracing::SkipHalfResLight`, `BB_RT_SKIP_VOLUMES=0` restores) (`BB_RT_FX_SHEETS=1` restores,
`BB_RT_SKIP_PS=<hex,hex>` skips any pixel shader; `Rasterizer::Draw` drops the draw after
`SkipOutputlessDraw`), the game's ambient base in the accumulation targets is no longer added
(`BB_RT_DIRECT_BASE=1` restores `DirectFlagAdd`), and specular-only lights (black diffuse,
e.g. the player-attached blue specular point light, radius 7 m) are removed with
`BB_RT_FILL_LIGHT=off`. The Yharnam street coat region went from 5.5/11/13 to 0.8/1.7/2.3.

## Moon flicker outdoors: directional set log and a fixed moon ray sample

Symptom: Central Yharnam under the big moon, four screenshots 150 ms apart differ in whole regions
(ground under the tree, wall and foliage), with DLSS on.

Likely cause: `MoonVisibility` in `rt_direct.comp` traced one cone ray per pixel with a seed that
changed every frame and no history. Under alpha-tested canopy every pixel is a penumbra, so the
moon term was new binary noise each frame; DLSS turns that into low-frequency blotches that crawl
across the ground and walls.

Change:

- `BB_RT_MOON_SEED=static|frame` (default `static`): the moon cone sample is fixed per pixel
  (`DirectFlagMoonStatic`, 256), so a still view traces the same ray every frame. `frame` restores
  the old per-frame sample. No extra rays, no GPU cost.
- `BB_RT_MOON_HALF=0|1|2` (live, default 2, logged in `Ray tracing live state`): the moon ray is
  the one long ray (1e4 m, alpha-tested canopy) every moonlit pixel of `rt_direct.comp` traces, the
  point lights only trace short rays to their few tile lights. 1 traces it on a checkerboard
  (`DirectFlagMoonHalf`, 2048, `MoonHalfTraces`); the other pixels average their in-tile 4-neighbours
  that traced (same plane within 0.5% of the depth + 2 cm, normals within 0.8), or trace themselves
  when none qualifies. 2 also swaps the checkerboard every frame (`DirectFlagMoonHalfAlt`, 4096).
  With painted-window portals in the frame it also skips the exact-direction centre ray unless a
  portal-only query (`PortalAhead`) finds a pane on it (same result, one long ray less). Off with
  `BB_RT_MOON=mask`, `BB_RT_MOON_WORLD=0`, any `BB_RT_DIRECT_DEBUG` mode and the ReSTIR pass.
- `BB_RT_DIR_LOG=1`: every completed `sets_readback` slot (the exact `LightSetParam` bytes the
  direct pass bound) is compared with the previous frame (`CompareDirSets`). A `RT dir frame`
  line (at most 8 a second) prints when the sets, FC_ShadowColor, the buffer address or the mode
  (add / game draws kept, game mask, moon mode, ray sample) changed: changed set count, first
  changed set's moon colour before and after, max colour and direction deltas, all-zero sets,
  set 0 moon/light 2 values, FC_ShadowColor and the address. `RT dir summary` once a second
  counts frames read, missed, and frames each value changed. Every direct frame (also pass-through and frames without sets) also feeds a `RT dir frame N state` line when the sets source (none / GPU buffer / host copy), the game mask or the write mode changes, and the summary counts direct frames, state changes, frames without sets, from the host copy, pass-through and without the mask.

Live check (supervisor), same spot, `BB_RT_DIR_LOG=1`: the summary should show `changed in 0
(sets)`, `0 (buffer address)` and few or no `FC_ShadowColor` changes while standing still; if so
the flicker is the moon ray and `BB_RT_MOON_SEED=static` (default) must remove it, while
`BB_RT_MOON_SEED=frame` brings it back. If the summary counts set or address changes every
frame, the lines name the set and value that flips.

## Player fill light and the stock light depth test (rt-fix-glow, attempt 3)

`GXLightAcc.vpo` (`8747a367`, disassembled with bbhost's `gcndis`) exports `pos0 = (x, y,
g_vtxParam.x, w)`: every light quad is drawn at one constant depth from `cbLightAccVtxParam`
(16 bytes, vertex stage), tested against the stored scene depth with the draw's depth func
(`LessEqual` live) and the depth bounds `[DB_DEPTH_BOUNDS_MIN, MAX]` (on for every light draw
live). The rasterizer now captures `g_vtxParam.x` (`OnLightVtxParam`), the bounds, depth write,
cull mode and the viewport `zscale/zoffset`; `Rt::LightDepthTest` gives the quad's window depth
`zoffset + zscale * g_vtxParam.x` (w taken as 1). `PackStockDepth` writes it into the unused cone
rows of point lights seen this frame (`age 0`), and `rt_direct.comp` `ShadeLight` drops a light
for a pixel whose depth fails the stock test, as the hardware does for the stock pass.
`BB_RT_LIGHT_DEPTH=0` turns it off. `BB_RT_DIRECT_DEBUG=9`: strongest light per pixel ignoring
the test, green where the stock test passes, red where it rejects, blue no captured state.

Player fill lights: `PlayerLightSignature` (unshadowed point, radius <= 10, grey colour and
specular, view |x| <= 1.5, |y| <= 5, z 1.5..8) marks every copy each frame, the old view-fixed
tracker stays as a second rule. |y| was 3 until 2026-11-07: in a narrow sewer shaft the camera sits
above the player and the light came in at view (0.47, 4.01, 4.18). It passed the signature and was
taken for a lamp (fill-vis emitter, radius 3.5 -> 8.75), so a grey light followed the player. Log (`BB_RT_DIRECT_LIGHT_LOG`): `player fill light x N this frame
(signature S, tracked T), gate reason: ...`; the near-camera list prints each light's depth func,
quad z, bounds, cull and whether the stock depth test was applied.

# See-through window and door glass (`BB_RT_GLASS`, rt-fix-turn, rt-fixlist item 30)

The door frame and its glass panes are one draw (pixel shader `9abe2f09`, which also draws
plain walls). Measured with `BB_RT_GI_DEBUG=11` + `BB_RT_DEBUG_THRESH`: panes have
G-buffer1.x (gloss) >= ~0.55, the wood, walls and stairs are below it; albedo alpha carries no
glass signal. Gloss alone is unsafe game-wide (wet stone, metal), so a pixel is glass only when
both hold:

- candidate: G-buffer1.x > `BB_RT_GLASS_GLOSS` (0.55);
- confirmed: a camera ray through the pixel, limited to `[d - w, d + w]` around the pixel's
  distance `d` (`w = 0.01 d + 0.03 m`), first hits an instance whose surface record has
  `SurfaceGlass` (bit 28 of `SurfaceRecord.flags`). `WriteSurfaces` sets that bit for every
  frame instance whose draw's `tag.ps` is in `BB_RT_GLASS_PS` (default `9abe2f09`; a cache
  entry shared by several draws keeps the bit if any of them matches; written even when
  the surface records are off). The instance custom index is the entry id, so the bit reaches the GPU
  through the existing Surfaces buffer (binding 11), no new buffer.

Where: a mode of the reflection pass (`RecordReflect`, `rt_gi.comp` `RT_REFLECT`), i.e. at the
`GXLightComposite` hook, after `RecordDirect`, writing the light accumulation pair before the
game's composite. On a confirmed pixel:

- transmitted `T`: the camera ray goes on straight (thin glass, no refraction offset) from
  `t_pane + 0.03 m / max(N.V, 0.1)` (past the pane's own back face, longer at grazing
  angles), alpha tested; miss -> `SkyRadiance`, hit ->
  the reflection hit shading (`albedo * DirectAtHit + HitEmission`, traced shadow rays). The
  pane always covers the hit on screen, so the shaded-screen shortcut never applies there.
- `F = Schlick(F0, N.V)`, `F0 = g3^2` (0.04 without G-buffer 3).
- decal cover `c = smoothstep(0.06, 0.25, max|albedo_gbuffer - albedo_pane|)`: the G-buffer
  albedo holds the crack/dirt decals (ps `a89e01e9` / `2140da2b`), the pane's own texture
  (traced `HitSurface` at the confirm hit, LOD 2) does not.
- tint `mix(1, pane / max(pane.r, pane.g, pane.b), BB_RT_GLASS_TINT * 0.5)` from the pane
  texture, so every channel is <= 1 (the pane filters, it never adds light).
- specular target `+= T * tint * (1 - F) * (1 - c)` on top of the pane's specular (light
  specular plus our F-weighted GGX reflection when roughness <= `BB_RT_REFLECT_MAX_ROUGH`);
  irradiance target `*= c` (binding 28, storage), so the pane's diffuse stays only under the
  decals and the cracks stay visible. Only when the irradiance target is a storage
  `B10G11R11_UFLOAT` image (binding 28 is `r11f_g11f_b10f`) and the direct pass does not run
  after this pass (reflect-first ordering, `BB_RT_DIRECT_BASE=1` with the add write mode, where
  `RecordDirect` would add the pane diffuse back after the scale); otherwise the pane diffuse
  stays and the log line says why.

Knobs (live): `BB_RT_GLASS` (on, `0` off), `BB_RT_GLASS_PS` (comma hex list, `9abe2f09`),
`BB_RT_GLASS_GLOSS` (0.55), `BB_RT_GLASS_TINT` (1, 0-2), `BB_RT_GLASS_DEBUG` (1 glass mask
magenta, 2 also rejected candidates green, 3 the diffuse keep factor as grey: black = clear
glass, white = full diffuse). Needs the reflection pass (`BB_RT_REFLECT`, on with
`BB_RT_DIRECT=1`). Log: `see-through glass` line when its state changes (max 4).

Cost (not measured): candidates cost one short confirm ray; confirmed pixels one transmitted ray
plus the hit's shadow rays, about one reflection pixel each.

Not done / known limits: with `BB_RT_DIRECT_BASE=1` the irradiance scale is off (see above), so
the composite's pane diffuse stays under the transmitted light (the GI keep factor below still
applies). Light from outside (moon, sky) does not pass through glass for shadow/GI rays; fog between
the pane and the hit is not applied (the composite fogs at the pane depth); every glossy pixel of
a `9abe2f09` wall counts as glass.

Diagnosis: `BB_RT_PICK=fx,fy` also logs the camera ray through the pick pixel (`RT pick   camera
ray`, then `cam hit 1..4`: t vs the G-buffer distance, TLAS instance, BLAS entry, ps/vs, alpha,
face, glass bit, alpha test, TLAS mask) and the confirm ray's verdict (`RT pick   glass confirm
ray ... => ACCEPTED as glass` or `rejected: <reasons>`). Live finding before it: with
`BB_RT_GLASS_DEBUG=2` the door frame, bars and a wall are accepted (magenta) and the cracked
panes are rejected candidates (green), and `BB_RT_SKIP_PS=9abe2f09` removes frame and bars but
not the pane image, so the pane is probably a different draw than `9abe2f09`.

Live result of that diagnosis (hot82): the cracked pane is ps `8aba0ecd` (vs `7620368b`), drawn as a
kept instance (TLAS mask `0x04`, MaskKept), so a confirm ray on the solid mask alone could never see
it. `BB_RT_GLASS_PS` now defaults to `8aba0ecd` and the confirm ray traces `MaskSolid | MaskKept`
(the transmitted ray stays on the solid mask). With `BB_RT_GLASS_DEBUG=2` the panes are accepted and
the frame is not. Open: the pane still looks frosted, because its brightness comes from the composite's
baked ambient (target 4 x albedo) and the GI apply pass (albedo x GI), which the irradiance scale does
not reach; the room behind this door is dark, so the transmitted light is near zero there.

## Every diffuse source of a pane (diffuse keep factor)

Where a pane pixel's light comes from, read from the shaders (`user/cache/CUSA03173/*.spv`,
`spirv-cross -V`) and the frame order (`hot49.log` `Scene pass` dump, `hot83.log` AO inputs):

- G-buffer pass (mrt 0xbf): mrt4 `B10G11R11` is the baked irradiance (lightmap/SH, the pane
  shader `8aba0ecd` writes it already times `1 - spec`), mrt5 `RGBA16F` the baked specular.
  `8aba0ecd` exports `mrt0..mrt5` only, so the emissive target (6, image 90) is 0 on a pane.
- `GXLightDeferredAO` (`3fe9e964`) samples them (images 85 / 84, the pre-AO pair) and writes the
  accumulation pair 81 / 82 (`irr * ao`, `ao * mix(spec, RLR)`); our direct pass adds onto 81/82
  (light draws skipped), then the reflection pass runs, both at the `GXLightCmbAlbedo` hook.
- `GXLightCmbAlbedo` (`8650418f`): `out.rgb = GBuffer2.rgb * irradiance(81)` -> image 86.
- `GXLightComposite` (`32e47788`): `scene = img3 (86) + img4 (82) + img5 (90)`, then fog. 86 is
  its only diffuse term, and it is the image `GlassIrr` scales (81, before `8650418f` reads it).
- `rt_gi_apply.comp` (after the composite): `scene += albedo(target 2) * GI * strength`.

So the baked ambient is not a separate composite term: it rides in 81 and was already scaled by
`c`. The live A/B (glass on vs off, pane region mean 4.4/6.0/9.3 in both) means the two terms the
glass code touched were about 0 there: `c ~ 0` and `T ~ 0` (dark room) make the specular add 0,
and the scaled 81 was already near 0 (`RT pick` on the pane: directional diffuse luma 0, the one
point light blocked; the baked irradiance is written times `1 - spec`). What is left on a clear
pane is the game's specular in 82 (kept on purpose: it is the pane's specular) and the GI apply
term `albedo * GI`, the only diffuse term that shows the pane's albedo (the frosted, cracked
texture); the GI ray sees the outside through the doorway, so it is the visible one.

Fix: the reflection pass writes a diffuse keep factor, R16F `glass_keep` (GI image size,
binding 29 of `RT_REFLECT`, flag `GlassKeep` 32): 1 on every pixel, `c` on a glass pixel, 0 on
the debug paint. `rt_gi_apply.comp` binds it at 11 and multiplies the GI add by it
(`ApplyConstants.glass_keep`: 0 none, 1 scale, 2 show it). It is used only when the reflection
pass wrote it this frame (`glass_keep_frame == frame`, reset when the GI images are rebuilt);
otherwise `raw` is bound and not read, so with `BB_RT_GLASS=0`, the reflection pass off or
skipped, nothing changes. Cost: one R16 store per pixel (two on glass pixels), one fetch in the
apply pass; 4.4 MB at 2296x960. Other `rt_gi.comp` variants (GI, ReSTIR GI init/resolve,
`RT_LIGHT_VIS`) do not declare binding 29 and keep their layouts.

Not verified live. Check: `BB_RT_GLASS_DEBUG=3` (panes black, decals grey, rest white), then the
pane A/B with `BB_RT_GLASS` 1/0. Still open: the denoiser guides (`aux_albedo`) keep the pane's
albedo; the game's specular in 82 is the pane's specular, so a pane with no light behind it still
shows its baked environment reflection.

# S1 (rt-perf-1): ReSTIR DI shadow-ratio mode, dropped

Plan step S1 tried `BB_RT_RESTIR=1` as a default "shadow ratio" mode (`ba3ce27` on rt-perf-1): the
slice 6 shading of every tile light, with the per-light shadow-ray loop replaced by one stratified
shadow ray per pixel and a per-pixel visibility ratio `r = sum w_i vis_i / sum w_i` kept in the 8d
reservoir buffers. It is not on this branch; `BB_RT_RESTIR` keeps its rt-perf-0 meaning (unset or
`0` = slice 6 full-ray direct, `1` = the slice 8d RIS passes, opt-in).

Why dropped (live, supervisor, Forbidden Woods, 3 x 600 frames per side):

- No gain: the `direct` column went 3.32 ms -> 3.44 ms. The one-ray path still runs the full tile
  light loop twice (unshadowed sum and BRDF), and the exact-ray fallback (no history,
  contradiction) fires often enough in the woods to eat the saved rays.
- Wrong by design (review): one scalar ratio per pixel is shared by all traced lights, so when one
  light is blocked and another is not, the blocked light gets the other light's visibility and
  leaks into its own shadow. With `BB_RT_RESTIR_HISTORY` 16 the ratio also trails moving casters
  for ~30 frames (ghost shadows), below the 0.75 contradiction threshold that would reset it.

Direct stays the deterministic slice 6 loop. A future direct speed-up needs per-light (or
per-light-group) visibility, not one ratio per pixel.

# S2 (rt-perf-2b): sparse ReSTIR GI on by default, DLSS Ray Reconstruction stays opt-in

Branch rt-perf-2b = rt-perf-0 + the S2 work of rt-perf-2 (`fbe5ab2`) without S1. Defaults now:
`BB_RT_RESTIR_GI` unset or `1` = on (`0` = the full-ray slice 8 GI pass), `BB_RT_GI_SPARSE` 4
(unchanged, ReSTIR GI only), `BB_RT_DLSS_RR` unset or `0` = off (`1` = Ray Reconstruction, as
before). ReSTIR GI still needs `BB_RT_GI=1`; RR still needs ray tracing on, the DLSS upscaler
selected and `nvngx_dlssd.dll`, otherwise Super Resolution runs.

Live results (supervisor, Forbidden Woods, 3 x 600 frames per side):

| Setting | GI ms | FPS | Look |
| --- | --- | --- | --- |
| rt-perf-0 (full-ray GI, SR) | 13.9 | 35.1 | reference |
| ReSTIR GI, sparse 4, SR | 8.6 | 42.6 | same as reference |
| ReSTIR GI, sparse 4, RR | - | 37.3 | glowing leaf clump, red specks |

RR on top of ReSTIR GI costs ~5 FPS (42.6 -> 37.3, it replaces SR at ~4.5 ms vs ~1.47 ms) and the
one RR + ReSTIR GI frame showed a glowing leaf clump and red specks, so `BB_RT_DLSS_RR` goes back
to default off. The RR artifacts with sparse reservoirs are not investigated yet.

Why ReSTIR GI was off: shipped "behind `BB_RT_RESTIR_GI=1`" in slice 8e and only run live once
(hot56, sparse 4: GI 2.49 ms). Every later fix went into the full-ray path only. The spatial pass
visibility ray (`rt_restir_gi.comp` `Blocked`) predated glass panes and painted windows: it
stopped at glass panes (the GI rays pass through them, `rt_gi.comp` `Trace`) and at the wall
behind a painted-window pane (the GI rays skip `PortalWall` past it), so every neighbour sample
seen through a window was thrown away (`W = 0`): darker indirect light next to windows. In the
synthetic scene with glass over half the hemisphere the spatial estimate came out at 45 % of the
reference.

Fix:

- `rt_restir_gi.comp` `Blocked`: first an opaque query on `MaskPortal`; if a pane is hit, test
  `[0, t_pane]` and `[t_pane + PortalWall, t_max]` (same rule as `rt_gi.comp` `Transmittance`),
  else `[0, t_max]`. `BlockedSpan` (mask `MaskSolid` 0x01) skips glass panes (`SurfaceGlass`)
  and keeps the alpha test. One extra cheap opaque query per spatial visibility ray.
- The "blocked => `W = 0`" rule itself stays: a real occluder must still reject the sample.
- `rt/rt_gi.h` `RestirGiFromEnv` (default on), `RayReconstructionFromEnv` (default off, `1` = on);
  `vk_ray_tracing.cpp` and `vk_temporal_upscaler.cpp` use them; the "needs BB_RT_GI=1" line only
  prints when `BB_RT_RESTIR_GI` is set explicitly.
- CPU port of the shader visibility: `rt/rt_restir_gi.h` `GiBlocked` / `GiBlockedSpan` over a
  list of `GiRayHit` (t, instance mask, surface flags, alpha result) follow `Blocked` /
  `BlockedSpan` line by line: closest `MaskPortal` hit as the opaque query, `MaskSolid` hits in
  `[t_min, t_max]`, glass skipped, alpha-failing hits ignored. The shaders here are plain GLSL
  with no shared headers, so this is a port, not shared code: any change to `Blocked` in
  `rt_restir_gi.comp` must be mirrored there. The constants (`RestirGiSurfaceGlass`,
  `RestirGiMaskSolid`, `RestirGiMaskPortal`, `RestirGiPortalWall` 1.5) are static-asserted
  against `rt_surface.h` / `rt_geometry.h`.
- Tests (`rt-restir-gi-test`): `TestDefaults` (ReSTIR GI on, RR off unless `1`, sparse 4),
  `TestBlockedGlassAndPortals` (glass, wall, alpha cutout, pane only, wall inside and past
  `PortalWall`, wall in front of the pane, two panes, pane past `t_max`),
  `TestSpatialSeesThroughWindows` (spatial reuse with `GiBlocked` on the ray to every
  neighbour sample on the +x half: glass and painted window within 3 % of the brute-force
  reference, a real wall clearly darker, so the test does go through the visibility code).

`BB_RT_GI_LAST_BOUNCE_CACHE` stays off. On: bounce 2 ends at the screen / material cache colour
instead of `DirectAtHit` + emission, which saves the light loop, its shadow rays and the sun ray
at bounce 2. With sparse 4 only a quarter of the pixels trace, so the saving shrinks to a quarter
(rough guess 0.3-0.6 ms, not measured). Cost: bounce-2 light is the camera's view of the hit, so
second-bounce colour can shift while turning. Only worth a live A/B if GI is still over budget.

Live checklist (supervisor):

- Exe `out\s2b\bb-probe.exe`; shader dir `out\shaders-s2b` (must hold
  the new `rt_restir_gi_spatial_comp.spv`), or unset `BB_RT_SHADER_DIR`.
- A/B: `run-hot.ps1` as is against `-Extra @{BB_RT_RESTIR_GI='0'}`; GPU lines: `GI` column
  drops, `restir gi` column appears, upscaler label stays `(DLSS)`.
- Logs: `ReSTIR GI on (BB_RT_RESTIR_GI=1, default; ...`, `ReSTIR GI traces 1 of 4 pixels per
  frame`, no `ReSTIR DI` line unless `BB_RT_RESTIR=1`, no `Ray Reconstruction runs` line.
- Look: rooms with glass and painted windows (indirect light next to them must match the
  `BB_RT_RESTIR_GI=0` shot), a lamp moving through a corridor, a fast 180 turn in the woods.

## Partitioned TLAS (optimize plan S4)

The KHR TLAS lives in the current view space (instance transform `view_now * inv_view_built`),
so every instance changes whenever the camera moves and a partial update gains nothing. The
partitioned TLAS (`VK_NV_partitioned_acceleration_structure`, `BB_RT_PTLAS`, default off since the live A/B below, `BB_RT_PTLAS=1` on when the
extension and the `partitionedAccelerationStructure` feature are there) is built in world space
instead: an instance's transform is its `inv_view_built`, constant for cached geometry.

- Ray space: the TLAS buffer starts with a 256-byte header holding the view-to-TLAS affine (3
  rows); the acceleration structure sits at +256, and `tlas_address` points at it. Every
  `rayQueryInitializeEXT` in `rt_gi/direct/fog/shadow/ao/restir_gi/debug.comp` traces
  `TlasPoint(origin)`, `TlasDir(dir)`; `rt_gi.comp` maps `ObjectToWorld` back with `TlasToView`.
  Hit `t`, custom index (`instanceID`), primitive, barycentrics, mask and flags are unchanged, so
  hit shaders index the same tables. KHR path: identity header written once, the build is as
  before. `rayQueryGetIntersectionInstanceIdEXT` (pick only) returns the PTLAS instance index;
  the pick snapshot is re-indexed by it.
- Instance index = cache entry id (second draw of one entry in a frame: entry + 8192; a third is
  left out). Partitions (`rt_ptlas.h`): 0 = static cached BLAS of the area, 1..16 = kept off-screen
  instances by 64 m cell, global = fresh, updatable or moving entries. The kept cell is keyed by
  the world-space centre of the BLAS bounds (`inv_view_built` applied to the centre of the probe's
  4 sampled vertices, written per entry by `rt_probe.comp` on a build into a host-visible centre
  buffer); with no centre yet it falls back to the build camera position.
- Per frame (`PtlasPlanner`, tested in `tests/test_rt_ptlas.cpp`): only instances whose BLAS,
  transform, mask, flags, custom index or partition changed get a `WRITE_INSTANCE`, plus every
  active instance whose entry was built, rebuilt in place or refit this frame (any `pending`
  build: refits, `ReuseAllocation` rebuilds, anchored refits, `BB_RT_MOVING_REFIT`), because a
  partition without ops keeps its old bounds. Entries gone from the frame are written disabled
  (BLAS address 0). The first build writes all 16384 indices. The instance ring copy is skipped;
  the ops go to a small per-slot host ring with one `WRITE_INSTANCE` command.
- The probe (`rt_probe.comp`, push constant `ptlas` = 1) uses only `WRITE_INSTANCE` (no
  `UPDATE_INSTANCE`: those need `ENABLE_EXPLICIT_BOUNDING_BOX`, VUID-...-10572). It keeps a
  persistent shadow per index (last full write record + hidden bit, GPU-side copy of this frame's
  CPU records). A mismatched instance is hidden by zeroing the BLAS address of this frame's
  record, or, when the CPU did not write it, by appending a copy of the shadow record with address
  0 and bumping the command's `argCount` on the GPU; once the probe matches again the shadow
  record is appended unchanged, restoring it. The Build-start compute-to-compute barrier orders
  last frame's shadow writes before this frame's probe.
- Stats: the 600-frame `Ray tracing GPU ms/frame` line ends with `TLAS <ms> ms GPU, instance
  upload <ms> ms CPU, <n> instance writes/frame, <r> refit writes/frame, <h> probe-hidden` on both
  paths (KHR: writes = instances, refit and hidden 0). `refit writes` = writes of instances whose
  BLAS was built or refit that frame; `probe-hidden` = instances the probe hid per frame. Startup:
  `Ray tracing: partitioned TLAS on (...)` or `... off (<reason>), KHR TLAS rebuilt every frame`.

Not verified live. A/B: `BB_RT_PTLAS=0` vs default; check the image is identical (no instances
missing or misplaced on camera turns, hidden-on-mismatch doors still vanish for the probe's
frames), then compare the TLAS ms and the writes/frame. With skinned enemies or moving doors in
view, `refit writes/frame` must be above 0 and no hits may lag behind the animated mesh.

# S6 (rt-perf-6): GI trace as a ray tracing pipeline with SER (spike)

Goal: the cheapest path to a real number for RT_OPTIMIZE_PLAN S6. With sparse ReSTIR GI on by
default (S2) the hot GI work is the ReSTIR GI initial-sample pass (`rt_restir_gi_init_comp`):
bounce rays, alpha-tested foliage, hit shading (`HitSurface`, `DirectAtHit`, emission, sky).
Only that pass moves; temporal, spatial, resolve and apply stay compute.

How it is built, sharing everything:

- `rt_gi.comp` compiles to three more stages with defines. `RT_GI_RGEN` (+ `RT_RESTIR_INIT`) is
  the same `Shade` / `main` as the compute pass, launched with `gl_LaunchIDEXT`; only `TraceSpan`
  changes. `RT_GI_AHIT` is the alpha test (`AlphaTestPasses`, glass skipped when the payload says
  so, rejected count kept for debug view 10), `RT_GI_CHIT` fills the payload (instance, primitive,
  barycentrics, object-to-world, t), `RT_GI_MISS` sets t = -1. The shadow / transmittance rays
  inside the shading (`SegmentTransmittance`, `Transmittance`, `DirectAtHit`) stay ray queries,
  which raygen allows.
- `RT_GI_SER` (second raygen variant): `hitObjectTraceRayNV` (any-hit still runs, closest-hit and
  miss are not invoked), then `reorderThreadNV(hit, hint, 16)` with hint = instance custom index
  (the surface record, so material) | 0x8000 for hits and 0 for misses, then the hit is read back
  from the hit object (`hitObjectGetRayTMaxNV` for t). Shading after the reorder runs on
  coherent warps.
- Host (`vk_ray_tracing.cpp`, `CreateGiRtPipeline`): the ReSTIR GI init bindings with raygen /
  any-hit / closest-hit / miss stage flags (push descriptors as before), the surface texture set
  gets the same stages, one pipeline (groups raygen, miss, triangle hit group chit + ahit,
  recursion 1), a host-visible SBT laid out by `Rt::GiSbt` (`rt_gi_pipeline.h`).
  `RecordRestirGi` binds it on `eRayTracingKHR`, writes the same descriptors, and calls
  `traceRaysKHR` over `Rt::GiInitLaunch` = the compute dispatch size, between an all-commands ->
  ray tracing barrier and a ray tracing -> compute barrier. Same outputs, same images; the GPU
  `GI` timer (pass 1) still brackets it.
- Device (`vk_instance.cpp`): with `BB_RT=1` and `BB_RT_GI_PIPELINE=1`,
  `VK_KHR_ray_tracing_pipeline` (+ `rayTracingPipeline`) and, unless `BB_RT_GI_SER=0`,
  `VK_NV_ray_tracing_invocation_reorder` (+ `rayTracingInvocationReorder`). Flags 0: no new
  extension, feature or stage flag; the compute path is the one from S2.

Flags: `BB_RT_GI_PIPELINE` (default 0 in the spike; 1 = pipeline), `BB_RT_GI_SER` (default 1
when the extension is exposed; 0 = pipeline without reorder). Needs ReSTIR GI on (default) and
the hit surfaces; otherwise the log says why and the compute pass runs. Hot-reloaded `.spv` files
(`BB_RT_SHADER_DIR`) only reach the compute pipelines; the ray tracing stages are embedded.

Checks done offline: all five SPIR-V variants pass `spirv-val`; a standalone program created both
pipelines (with and without SER) on the 4070 Ti SUPER, driver 617.14, with these layouts and got
the group handles. Driver exposes `VK_KHR_ray_tracing_pipeline` (handle 32 B), both
`VK_NV_ray_tracing_invocation_reorder` and `VK_EXT_ray_tracing_invocation_reorder` (reorder mode
REORDER). Tests: `rt-gi-test` `TestGiPipelineMode`, `TestGiSbt`.

Live checklist (supervisor):

- Exe `out\bb-probe.exe` of that worktree.
- A/B on the same spot (Forbidden Woods, 600-frame lines): base, `BB_RT_GI_PIPELINE=1`
  (SER on), `BB_RT_GI_PIPELINE=1 BB_RT_GI_SER=0` (pipeline alone, to split the pipeline cost from
  the reorder gain). Compare the `GI` column.
- Startup line: `Ray tracing: GI pipeline on (BB_RT_GI_PIPELINE=1): ReSTIR GI initial trace as
  raygen + any-hit alpha test (VK_KHR_ray_tracing_pipeline), SER on
  (VK_NV_ray_tracing_invocation_reorder exposed, BB_RT_GI_SER), SBT stride 32`.
- Look: the image must match the base (same samples, same alpha test); debug view 10 (alpha
  rejects) and 8 (sources) should look the same.
- Decision: keep only if the `GI` column beats the base by a clear margin (plan: 20-40 % on Ada).

With S4 (branch rt-perf-all): the raygen `TraceSpan` traces `TlasPoint(origin)` /
`TlasDir(dir)` in both variants (`traceRayEXT`, `hitObjectTraceRayNV`) like every ray query, and
the closest-hit / hit-object object-to-world goes through `TlasToView` in raygen (the closest-hit
stage stores the raw `gl_ObjectToWorldEXT`). Hit `t`, custom index, primitive and barycentrics
are unchanged by the rigid TLAS space, so any-hit and closest-hit need no other change. The
TLAS build barriers (`WriteSpace` header write, PTLAS build start, build -> readers) include
the ray tracing shader stage when the GI pipeline is on.

Default on (rt-perf-all): `BB_RT_GI_PIPELINE` unset now means 1 when `VK_KHR_ray_tracing_pipeline`
is exposed, except on AMD (vendor 0x1002), which keeps the compute pass unless
`BB_RT_GI_PIPELINE=1`; `0` = compute pass everywhere (`Rt::GiPipelineWanted`). `BB_RT_GI_SER`
unchanged (default 1 when the NV reorder extension is exposed). Startup: `Ray tracing: GI
pipeline on (BB_RT_GI_PIPELINE, default 1): ... SER on (...)` or `Ray tracing: GI pipeline off
(<reason>), compute GI kept`.

## RT optimization live results (2026-11-06, Forbidden Woods lamp, 3x600-frame GPU lines, ms)

| step | branch | result | kept |
|---|---|---|---|
| S0 one-write stdout, hitch log on CPU/wait | rt-perf-0 | worst frame ~450 -> ~30 ms, FPS 26-28 -> 35 | yes |
| S1 ReSTIR DI shadow-ratio mode | rt-perf-1 | direct 3.32 -> 3.44 | no |
| S1 RIS ReSTIR DI (`BB_RT_RESTIR=1`, M 4) | base flag | direct 2.91 -> 2.38, a few extra lit spots | opt-in |
| S2 sparse ReSTIR GI default | rt-perf-2b | GI 13.7 -> 8.5, total 20.2 -> 15.0 | yes |
| S2 DLSS RR default | rt-perf-2 | -5 FPS, glowing leaf clump once | opt-in |
| S3 CLAS skinned/moving | rt-perf-3 | net loss in every config (best +1.0 ms) | no |
| S4 partitioned TLAS | rt-perf-4 | alone -1.1 ms; on top of S2b+S6 +0.28 ms (GI -0.6, direct/fog +1.0) | opt-in |
| S5 OMM | default | -0.6 ms | yes |
| S5 compaction | flag | no gain | no |
| S5 sparse reflections | rt-perf-5 | reflect 2.0 -> 2.5 (slower) | no |
| S6 GI raygen pipeline + SER | rt-perf-6 | GI 7.44 -> 6.67 (OMM flag fixed) | yes |

## OMM on swaying foliage (P1, rt-perf-all)

Updatable (skinned) alpha-tested BLAS now get an opacity micromap. Alpha comes from the texture and
UVs, and neither changes with the pose, so the micromap is baked on each full rebuild and kept across
refits. The OMM index array goes to a persistent pool (`BB_RT_OMM_INDEX_MB`, default 32), copied
from the per-slot bake input, so every refit carries the same micromap and indices. Refit scratch is
sized with the OMM chain attached (without it the first refits overran scratch and lost the device
in the BLAS build). An instance drops `FORCE_NO_OPAQUE` only once its micromap is built (`omm_ready`).

Switch: `BB_RT_OMM_SKINNED` (default 1, live from `BB_RT_CFG`, applies at each BLAS's next full
rebuild, about every 20 refits; in the menu as "OMM on swaying foliage"). The GPU line adds
`skinned N built M refits`.

In-boot live A/B, 180-frame windows (`inab.ps1 -Key BB_RT_OMM_SKINNED`):

| Spot | Off total ms | On total ms | GI | Direct | Reflect |
|---|---|---|---|---|---|
| deep (3 rounds) | 16.27 | 14.02 | 9.16 -> 7.52 | 3.48 -> 3.00 | 1.75 -> 1.39 |
| woods (2 rounds) | 14.31 | 12.73 | 7.47 -> 6.40 | 3.13 -> 2.73 | 1.61 -> 1.28 |

Look: same frame on/off at deep shows only animation and foliage sway differences. A 5-minute walking
soak at deep with live off/on toggles survived.

## GI ray length cap (P4, rt-perf-all)

`BB_RT_GI_TMAX` (metres, 0 = no cap, default 0, live from `BB_RT_CFG`, menu "GI ray cap") caps
the GI bounce rays; a capped ray takes the sky radiance like a miss. It rides in bits 4-15 of the
GI trace config word (`GiTraceCfg`). Reflection rays are not capped.

In-boot live A/B at deep, 3 rounds x 180 frames:

| Cap | GI ms | RT total ms |
|---|---|---|
| none | 7.52 | 14.03 |
| 20 m | 6.71 | 13.29 |
| 50 m | 7.49 | 14.03 (none 14.09) |

The 20 m gain is the skipped shading of far hits, not traversal (50 m saves nothing). Look at deep
and garden: same frame, +1.7-2% mean luma (far bounce becomes sky), within the swaying-light noise.
Kept opt-in: in a hall wider than the cap the sky fallback would leak light, and no such save is in
the test set. A radiance cache (P6) is the right fallback for capped rays.

## Bounce-2 cache on by default, live bounce count (rt-perf-all)

`BB_RT_GI_LAST_BOUNCE_CACHE` and `BB_RT_BOUNCES` are now read live from `BB_RT_CFG` (live state
line and menu). The cache is now on by default: bounce 2 ends at the hit's screen/material cache
colour instead of `DirectAtHit` + emission.

In-boot live A/B at deep, 3 rounds x 180 frames:

| Config | GI ms | RT total ms |
|---|---|---|
| 2 bounces, cache off (old default) | 7.52 | 13.98 |
| 2 bounces, cache on (new default) | 6.84 | 13.30 |
| 1 bounce | 5.04 | 11.48 |

Look: cache on vs off is within same-state noise at deep, garden, and a pose turned about 90
degrees in the woods. One bounce vs two is also within noise at deep and garden (lifted crops show
only wind sway): bounce 2 is about 1 % of the frame there, while GI on/off moves mean luma about 10 %.
One bounce stays opt-in (menu "GI bounces"): lamp-lit interiors lean on bounce 2 and none is in the
test set. `run-hot.ps1` no longer pins the cache off.

Bounce count 0 (one segment ending at the screen/cache colour): 10.64 vs 13.20 ms, but about 3.5 %
darker and flatter at deep. Menu option only. ReSTIR DI on (`BB_RT_RESTIR`): 13.19 vs 13.39 ms, the
same look, within noise: not adopted.

## Fog march 4 steps by default (rt-perf-all)

`BB_RT_FX_FOG_STEPS` default 8 -> 4 (live, menu "Fog march steps" 4/8/16). In-boot live A/B at
deep, 3 rounds x 180 frames: fog 1.06 -> 0.65 ms, RT total 13.38 -> 12.94 ms. The march start is
jittered per pixel and frame and the fog has history, so half the steps shows no banding: look
within same-state noise at deep, garden and woods. A clean boot with no override measures fog 0.649 ms.

Checked and dropped in the same round (RTX Remix study):

- Shadow and visibility rays already use `TerminateOnFirstHit` in every pass; the three
  `gl_RayFlagsNoneEXT` queries in `rt_direct.comp` are the debug pick only.
- `BB_RT_DIRECT_RAYS` 16 vs 32: 13.31 vs 13.40 ms, noise (deep has few lights per tile).
- `BB_RT_OMM_LEVEL` 4: BLAS build 1.0 -> 80+ ms/frame (the bake runs on every swaying rebuild),
  GI unchanged. Level 5: device lost in `rt_omm_bake` (TDR) on two boots. Level 3 stays; do not
  raise it without baking skinned micromaps once per DrawKey.

## Adaptive GI bounces (rt-perf-all, P7)

`BB_RT_ADAPTIVE_MS` (live, default 12, 0 = off, menu "Adaptive budget"): the RT GPU total of each
frame (sum of the timed passes) feeds a smoothed average (`rt_adaptive.h`, weight 0.05). Above the
budget the GI bounce count drops by one (never below 1); below budget - 2.5 ms it climbs back to
`BB_RT_BOUNCES`. A change holds 120 frames. The 2.5 ms margin is wider than what one bounce saves
(~1.8 ms at deep), so the count does not flap. Each change logs `Ray tracing adaptive:`.
Sparse GI is already at its maximum 4, so the bounce count is the lever (P7 as planned is N/A).

In-boot live A/B at deep, 3 rounds x 180 frames: budget 12 -> 11.14 ms, off -> 12.91 ms (one drop
per enable, no flapping). Garden (8.5 ms) stays at 2 bounces; woods drops to 1 at 12.08 ms. Look:
1 vs 2 bounces was within noise at deep and garden (see the bounce-2 cache section).

## Lit-view test spot and moon half rays (rt-perf-all)

The heavy case is deep turned toward the orange-lit rocks: `inab.ps1 -Spot deep -Turn 0.6` (and
`look-ab.ps1 -Turn 0.6`) holds `rx=255` on the virtual pad for 0.6 s after load. A 0.3 s step sweep
put the RT total there at 26.9 ms (GI 12.2, direct 9.1, reflect 2.9, adaptive off) against 18.6 ms
at the saved heading.

In-boot live A/B in that view, 4 rounds x 180 frames:

| Knob | On | Off | Result |
| --- | --- | --- | --- |
| `BB_RT_MOON_HALF` 2 vs 0 | direct 8.14, total 19.22 ms | direct 8.89, total 19.88 ms | kept, default 2 |
| `BB_RT_SS_TRACE_GI` 1 vs 0 (branch `rt-p2-ssgi`) | GI 7.42, total 18.27 ms | GI 6.95, total 17.74 ms | dropped |

Moon half look: on/off mean-abs-diff 1.59 vs 1.36-1.40 noise, no visible change. The moon ray is
only about 1.5 ms of the 8.9 ms direct pass in this view, so the rest of the direct cost is the
point-light work. Screen-space GI (P2) was slower in every round: the 16-step march with 4 bisection
steps and G-buffer reads costs more than the one BVH segment it replaces with OMM and SER on.

Direct-pass split in the lit view (same A/B, 2 rounds): `BB_RT_DIRECT_RAYS` 0 vs 32 = direct 2.39
vs 5.80 ms, so the point-light visibility rays are ~3.4 ms; a cap of 4 rays only saves 0.2 ms (most
pixels already trace 4 lights or fewer). ReSTIR DI (`BB_RT_RESTIR=1`, one shadow ray per pixel):
total 16.32 vs 19.38 ms (-3.1 ms). Look: still diff 1.54-1.70 vs 1.36 noise, mean luma -1 % (lit
trunks a touch dimmer); mid-pan frames show no extra noise. Stays opt-in (menu), the user picks.

## ReSTIR GI Jacobian limit (grass blotches)

Orange blotches flickered on grass in the lit view (user report). They came from ReSTIR GI spatial
reuse: a neighbour's sample that hit a nearby blade has a huge reconnection Jacobian
(`(cos_r/cos_q)*(lq2/lr2)`), it wins the merge and its inflated weight spreads through temporal
reuse (M 30). Gone with `BB_RT_RESTIR_GI=0`. Fix: live `BB_RT_RESTIR_GI_JACOBIAN` (default 10,
0 = old behaviour) rejects spatial candidates with a Jacobian above it (`GiJacobianOk`). Rejecting
the low side as well was tried and only darkened. Live, lit view, 2 frames per case: blotches in
every frame at 0, none at 10 or 4; the bright blob on the bottom-left rock also goes, and the
frame then matches full-ray GI (`BB_RT_RESTIR_GI=0`) within ~1 luma. GI cost unchanged (~7.1 ms).

## World-space radiance cache, `BB_RT_SHARC` (P6, rt-p6-sharc)

Live knob, default 0 (off: params field zero, no buffer, no resolve pass, same output). On: the
first hit of a GI path reads a SHaRC-style hash grid and the path stops there, skipping
`DirectAtHit` (light loop, shadow and sun rays) and every later segment.

- Grid (`rt/rt_sharc.h`, mirrored in `rt_gi.comp` and `rt_sharc.comp`): 2^20 slots x 32 B =
  32 MiB, allocated the first frame the knob is on. Key = world cell (the view-space hit moved
  to world space by `view_rows`, so camera moves do not shift cells), 17 bits per axis, level of detail
  (cell = 0.25 m, doubled until it is at least 2 % of the camera distance), normal octant.
  Slot = hash of the key, checksum = second hash (0 = empty), 8 linear probes, CAS insert.
- Update: 1 in 8 traced paths (`SharcUpdatePath`) always trace the full path, and every path
  whose cell is missing or not ready (weight < 4) does too; both add the path's sample value
  (the outgoing radiance of the first hit, emission and bounce 2 included) to the cell's
  fixed-point sums (1/4096, at most 256 adds per cell per frame) and stamp the frame.
- Resolve (`rt_sharc.comp`, before the GI pass, inside the GI timer): folds the sums into the
  resolved half-float colour as a running mean up to weight 32, then an EMA (n/32 per frame),
  zeroes the sums, evicts slots not touched for 64 frames.
- Read only when the first-hit distance exceeds 1.5 cells (short rays in corners trace as
  before); a camera cut clears the grid. Debug: `BB_RT_GI_DEBUG=8` shows cache hits in cyan.
- Expected: GI init cost close to the measured 0-bounce figure (one segment) plus the update
  share; unlike `BB_RT_BOUNCES=0` the value is the traced radiance, not the screen colour.
- Risks: light leaking through walls thinner than a far cell (normal octant only separates
  sides facing away from each other), a lag of ~20 frames on moving lights or a torch switch
  (EMA weight 32), cell blockiness in far GI before the denoiser.
- Tests: `rt-sharc-test` (levels, key and hash distinctness, resolve mean/EMA/cap, overflow
  bound, stale check, update ratio).

## Distance fade to stock lighting (P3, opt-in)

`BB_RT_FAR_START` (live, metres of camera distance, default 0 = off) and `BB_RT_FAR_BAND` (live,
default 10). Weight w = clamp((START + BAND - d) / BAND, 0, 1) (`rt_far.h`, same math in the shaders,
band 0 = hard step). Beyond START + BAND (w = 0):

- GI: no trace, raw 0, and the ReSTIR init writes a zero receiver normal so the temporal and spatial
  ReSTIR passes skip the pixel (no reuse, no visibility ray). GI is added on top of the game's
  lighting, so 0 = the game's own lighting there.
- Direct: no point-light visibility rays; the lights stay unshadowed, as the game draws them (the
  game's stock light-volume depth test in `ShadeLight` still runs). ReSTIR DI uses vis = 1.
- Moon: only when the game's shadow mask is bound (`FlagShadowMask`): no moon ray; diffuse gets the
  stock `mask.z * mix(1, mask.w, FC_ShadowColor)`, specular is unshadowed (as `DDirLightShadowed1`).
  Without a mask the moon keeps tracing (unshadowed moon would light far
  buildings fully).

In the band the traces still run and the result is blended linearly: GI raw scaled by w, light
visibility mix(1, vis, w), moon mix(stock, vis, w). Deterministic, no extra noise, no seam; the cost
saved is only past the band. ReSTIR DI far pixels stay a one-light estimate with vis = 1 (noisier than
the exact sum). Not measured live yet.

## Live results: SHaRC (P6) and far fade (P3), rt-perf-all

In-boot live A/B, 3 rounds x 180 frames.

| Knob | Spot | On | Off | Look |
| --- | --- | --- | --- | --- |
| `BB_RT_SHARC` 1 vs 0 | deep lit view (`-Turn 0.6`) | GI 6.00, total 18.44 ms | GI 7.05, total 19.43 ms | diff 1.23 vs 1.40 noise, same luma |
| `BB_RT_SHARC` 1 vs 0 | garden | GI 3.41, total 7.04 ms | GI 3.68, total 7.30 ms | ~2 % brighter, no visible artefact |
| `BB_RT_FAR_START` 30 vs 0 | deep lit view | GI 6.46, direct 7.49, total 18.46 ms | GI 6.92, direct 7.98, total 19.38 ms | lit rocks behind the trees visibly dimmer, luma -1.3 % |

Both stay opt-in (menu: "GI radiance cache (SHaRC)", "Far fade start"). SHaRC is untested with a
moving light (cache lag, about 18 frames to 90 %) and indoors (leaks through thin walls); far fade
at 30 m drops bounce light the forest view still needs, so it is for open vistas.

## P5 far/tiny instance culling, dropped (branch rt-p5-cull)

`BB_RT_CULL_FAR` (mrad angular radius, rt-p5-cull 1011b12/fde9930, not merged). Lit view: 20 mrad
drops 2 of ~389 instances, 1000 mrad only 4 of 349: almost every instance is within reach of one of
the ~90-106 lights or inside the view cone, and those are protected. Total 23.85 vs 23.66 ms
(noise). That build also ran ~5 ms slower than rt-perf-all with the knob at 0 (23.7 vs 18.4 ms in
the same view), so its per-build bounds pass is not free. Not worth it for Bloodborne's dense,
light-heavy scenes.

## Sewer water: the motion-vector target stalled the GPU twice a frame (2026-11-07)

The sewer near the water ran at ~23 FPS with DLSS while the GPU sat idle ~14 ms a frame (GPU
profile "between submissions", the same with `BB_RT_PAUSE=1`). Frame stats said the GPU thread
was blocked on GPU ticks 72 % of the time. `Scheduler::Wait`, `Finish` and `WaitSubmitted` now
take a `std::source_location`, and the 5 s frame stats list the top wait sites
(`wait X ms/frame N/frame file:line`). The site was `vk_object_motion.cpp` `EnsureImage`: 29 ms a
frame, 2 calls. The scene alternates full and half size object-motion targets, and with the cache
off (`BB_OBJECT_MOTION_CACHE=0`, the launcher default and forced off with DLSS) each size change
called `scheduler.Finish()` before freeing the old target, so the CPU and GPU ran in lockstep.
The old target now goes to `scheduler.DeferOperation` and is freed once the GPU has passed it.

Sewer save, DLSS, 3 boots each, steady 5 s frame stats:

| Build | FPS |
| --- | --- |
| before, cache off | 22.7, 23.4 |
| cache on (`BB_OBJECT_MOTION_CACHE=1`) | 49.6, 49.6 |
| deferred release, cache off | 49.8, 50.2 |

Same image as the cache run. RTAO at the same spot: `BB_RT_AO_RAYS` 8 to 4 halves AO (3.5 to
1.9 ms); `BB_RT_AO_RADIUS` 20, 2 or 0.5 costs the same.

## RTAO glowing edges on dark objects (2026-11-07)

With the dark-interior AO preset (`BB_RT_AO_RADIUS` 20, `BB_RT_AO_FALLOFF` 0, 8 rays) the sewer
showed thin bright outlines on characters, coat flaps and railings. RTAO traces one pixel per
`BB_RT_AO_SCALE` block and copies its value only to block pixels at the same depth (2 %). The other
pixels (silhouettes, overlapping folds) kept the game's own AO, close to 1, next to RT AO close to
0, so they glowed. Toggling GI, reflections and direct light left the edges; `BB_RT_AO=0` removed
them (and brought back the blue ambient). `rt_ao.comp` now runs a second dispatch (push constant 1)
after the trace: each pixel whose own cell is at another depth takes the AO of the 3x3 neighbour
cell with the closest stored depth. Live A/B/A at the lamp spot (`BB_RT_AO_RESOLVE` 1/0/1): edges
gone / back / gone; AO GPU time 1.27-1.36 ms either way. `BB_RT_AO_RESOLVE=0` (live) turns it off.

### Sewer lamps without light (2026-11-07)

The "dark-interior" preset above is wrong for enclosed areas: with a 20 m radius and no falloff
every tunnel pixel is fully occluded, so the lamp-post area went black. Region luma (sRGB, statue /
player / floor / wall) at the lamp save, A/B/A: default AO 12.2 / 26.8 / 11.6 / 7.1, preset
2.9 / 2.3 / 1.1 / 1.1, default 12.1 / 26.9 / 11.5 / 7.1 (stock, RT off: 19.1 / 48.2 / 19 / 9.6).
Keep the AO defaults. The two lamp-post lights are on (lantern preset, emitters floored to lum
0.8). The rest of the gap to stock is the blue moon fill spots (`applied 0.00`, see "Warm lights
are lamps") and the removed player fill light, both by the no-fake-light rule.

Tried and dropped: finding lamps from the emissive G-buffer target 6 (a compute pass summing lit
tiles into point lights). At GI time target 6 holds no lantern glass (0 pixels above luma 1e-6
over the whole frame; the glass is drawn later), so nothing was found. The `GI_DEBUG=16` view
that seemed to show the glass was the scene-colour fallback.

### Baked ambient cut: no light from nowhere (2026-11-07)

The blue fill on sewer floors and walls with no lamp or moon in reach was the game's baked
ambient, not a traced light. Our direct pass adds onto the pair `GXLightDeferredAO` wrote (81 =
`irr * ao` from the baked irradiance target 4, 82 = `ao * mix(baked spec, RLR)`). Lamp range,
ReSTIR, GI, reflections, fog, volumes and the moon changed nothing there; the old 20 m AO preset
removed it (that is why it showed up again when the preset went). Every `BB_RT_DIRECT_DEBUG` view
overwrites the pair, so those views are black there too.

`rt_direct.comp` `CutAmbient` on the diffuse base: remove the largest multiple of the cool
ambient colour `BB_RT_AMBIENT_COOL` (default 0.39,0.68,1) that fits, then keep the remainder only
where it is a fair part of the pixel's baked light (`smoothstep(BB_RT_AMBIENT_KEEP_LO 0.1,
BB_RT_AMBIENT_KEEP_HI 0.3, remainder share)`), so warm baked lamp light stays and the yellow or
magenta leftovers of the blue fill go. `BB_RT_AMBIENT_CUT` (default 1) scales it. The baked
specular base is dropped (`BB_RT_AMBIENT_SPEC_CUT` 1): its hue-cut remainder lit weapons yellow,
and the traced reflections and lamp highlights give the real specular. All four are live.

With `BB_RT_DIRECT_BASE` on (the default) the reflection pass runs first and writes its result
into that same specular target. The direct pass then scaled the whole target by 1 - cut and
erased every traced reflection, so water, glass and metal lost them (flooded sewer floor, water
debug: the sheet was found, but nothing showed). Now the reflection pass applies the cut itself
(`GlassPush.base_cut`): to the game's base under its reflection and to the pixels it keeps. The
direct pass skips the spec cut in a frame where the reflection pass did it (`spec_cut_frame`).

Sewer spot, sRGB region mean, A/B/A: floor / right wall / left wall / whole frame, cut off 8.1 /
7.7 / 8.5 / 6.5, on 1.0 / 1.0 / 1.0 / 2.0 (repeatable). Diffuse cut alone left the walls at blue
4.6 with an olive tint; spec cut off brought the left wall back to 4.9. Dropped: a moon-specular
switch (no effect) and a warm-hue gate on lamp latching (no effect here).

Wall lamps with no game light (sewer, two lamps 7 m away, nearest warm game light 51 m): their
glow is only baked, mixed into the blue fill. With the gate at 0.3/0.6 it was cut (wall under the
lamp 2.4); ungated 5.6, 0.1/0.3 5.4, 0.05/0.2 5.6, while the lamp-less wall stays 1.0-1.3 in all;
cut off 8.2. Default 0.1/0.3.

The low gate let red blood decals keep a warm remainder with no light near them (blood region 5.4,
3.5, 2.0; our direct light there about 1.0, i.e. none). `BB_RT_AMBIENT_KEEP_LIT` (default 0.25, 0 =
off, live) also scales the kept remainder by `smoothstep(0, 1, ours / (lit x remainder))`, with
`ours` the traced direct diffuse at the pixel: baked warm light stays only where a real or glow
lamp reaches. Blood checked live as fixed. The wall lamps keep theirs because their glow lights
light the wall.

### Glow lamps: lights for lamps the game never lit (2026-11-07)

Some wall lamps have no game light at all, only a halo billboard. In the sewer the halo is ps
`5e224cb6` (vs `41ae600f`, additive, 8 indices); `898dabb3` is a whiter flare on the same lamps.
Found with `BB_LOG_PS=scene` and a `BB_RT_SKIP_PS` screenshot bisect (lamp pixels 51 -> 19 with the
halo skipped). The decompiled vs (`out\shader-decomp\glsl`) multiplies attr0 straight by the
view-projection, so the vertex buffer holds world positions; attr1 is RGBA8 colour read as `.zyx`
(`ffffa666` = 1.0, 0.65, 0.40).

`Rasterizer::CaptureGlow` reads each halo draw's vertices on the CPU, `RayTracing::AddGlows`
clusters them (0.6 m) into a cache of lamps (256, oldest evicted), and `AddGlowLights` adds a point
light for each cached lamp seen in the last `BB_RT_GLOW_KEEP` frames (1800) within
`BB_RT_GLOW_DIST` m (40) of the camera that has no game point light within `BB_RT_GLOW_SKIP` m
(1.5), nearest 32. Each is a legacy light of radius `BB_RT_GLOW_RADIUS` (2), colour = halo colour x
`BB_RT_GLOW_STRENGTH` (0.38, matches the converted game lamps), then the lamp `EmitterFalloff`. The
cache keeps lamps lit while their halo is off-screen. Knobs live: `BB_RT_GLOW=0` off,
`BB_RT_GLOW_PS`, `BB_RT_GLOW_SPEC_POWER` (16), `BB_RT_GLOW_LOG=1`.

Measured (sewer two-lamp spot, 2 of 6 cached lamps uncovered): right wall 5.4 -> 9.3, left wall
2.1 -> 3.8, floor 4.9 -> 7.2; whole frame 3.56 -> 5.41, A/B/A identical. Halo + flare averaged gave
a whiter colour (0.62, 0.47, 0.40), so the default list is the halo only.

The halo's vertex colour is not its look: its ps multiplies it by the halo texture, so the light
read grey-white next to the game's warm lamps (which also keep warm baked light). `BB_RT_GLOW_TINT`
(default 1,0.55,0.2) multiplies it. Added light on screen, R:G:B: tint 1,1,1 1 : 0.89 : 0.82;
1,0.7,0.35 1 : 0.72 : 0.49; 1,0.55,0.2 1 : 0.62 : 0.37; 1,0.45,0.12 1 : 0.55 : 0.30.
The tint lowered luminance, so `BB_RT_GLOW_STRENGTH` went 0.38 -> 0.6: light colour (0.240, 0.086,
0.019), Rec. 709 luminance 0.114 vs 0.118 for a converted game lamp (0.152, 0.114, 0.054).

About 30 cached lamps in a row of halos gave about 30 lights (12 m reach each) and a 20 ms direct
pass. Uncovered lamps now merge within `BB_RT_GLOW_MERGE` m (2.5) into one light (luminance-
weighted centre, brightest colour), at most `BB_RT_GLOW_MAX` (8) lights, nearest first. The same
spot then logged at most 1.6 ms direct over a 20 minute watch.
### Reflection G-buffer pass (2026-11-07)

The game draws a second, half-size G-buffer pass (1148x480, 5+ colour targets, e.g. vs
`9e496c3d` / ps `70bb42af`) for its reflection view, with that view's own camera. Before the
fix its draws went into the TLAS in that view's space but were placed with the main camera's
inverse view, so misplaced copies (the floor `dbb222eb` floating ~1.9 m up, the player model
beside the player) were kept as off-screen instances and blocked shadow rays: the hand torch
only lit what the camera faced. `Rasterizer::side_gbuffer_draw` (depth narrower than
`CameraMotion::RenderSize`) now keeps those draws out of `AddDraw` and out of the camera
depth/constants tracking. Measured: picks blamed a kept off-screen instance 1142 times in a
run before, 0 after; the floor around the torch is lit from every camera angle.

### Forward water sheet reflections (2026-11-07)

Flooded floors (e.g. the sewer under the torch test spot) are not the G-buffer water `e54449c7`
but a forward alpha-blended sheet, ps `a28d2a7a` (`Rt::FxMistShader`), drawn after the RT
lighting. RT skipped it and only looked for G-buffer water, so the floor showed through with no
reflection. Now (`BB_RT_WATER_SHEET`, default on with `BB_RT=1`):

- The sheet draw keeps its motion store (`EmitVertexMotion`) and is drawn with a zero-extent
  scissor unless `BB_RT_FX_SHEETS=1`: its VS still runs and gives the TLAS gather positions, but
  it puts no pixels on screen. Skipping the draw outright left the instance with no positions,
  so no ray ever hit it.
- It is captured without alpha test as a water-only TLAS layer, mask `MaskWater` 0x10. Every
  other ray mask is `0xEF`, so shadows, GI, fog and direct light never see it. The pick ray
  (`BB_RT_PICK`) still uses `0xFF` so it reports water hits.
- `WaterFind` traces the camera ray against mask 0x10 first (`BB_RT_WATER_DEBUG=1`: blue, red under decals); G-buffer
  water (cyan) is the fallback; the rest is black. The sheet counts only when it lies at least
  `BB_RT_WATER_SHEET_DEPTH` (default 0.03 m, along the camera ray) in front of the G-buffer
  point; a negative value restores the old test, which also took sheets up to the glass depth
  window behind it. The raised walkway beside the sewer channel sits 0.04 m above the sheet
  (pick: sheet +0.039 m behind the G-buffer at 5.45 m, window 0.085 m), so the whole walkway
  mirrored like the channel while stock shows it dry. Channel pixels have the sheet 0.4 to
  0.65 m in front. The sheet only marks the pixel as water: the reflection starts at
  the G-buffer point with the world up axis (view_rows column y) as the normal. The sheet is a
  late-held draw whose positions come from the previous frame's camera, so its own normal
  tilted while the camera turned (a debug paint of normal vs up showed half the sheet off by
  more than 25 degrees mid-turn, and some tilt even at rest), and the reflections swung with it.
- The flame itself is an FX sprite with no geometry, so a mirror ray cannot see it. Water mirror
  rays instead test a soft flame shape around each warm point light (`FlameImage`: radius
  `FlameRadius` 0.07 m, `FlameHeight` 2.2 times taller than wide along world up, centre lifted
  `FlameLift`, peak `FlameGain` 3 times the light colour with a whiter core). It is cut at the
  ray's own hit, so anything in front of the flame hides it. Rules:
  - Only warm lights count (blue below `FlameWarm` 0.6 of red); spots and grey lights do not.
  - In each `FlameMerge` 1.5 m cluster only the brightest light draws one, because the torch
    fixtures carry two lights about 0.8 m apart, which drew two flames.
  - Lights near a moving track (the player's torch, flag 8 in `wrap_r_flag.w`, set in
    `WriteGiLights`) draw none. The game puts that light at the player's body, not at the fire,
    so its image landed beside the player's feet.
  - This replaced the GGX glint (F0 0.02 plus shadow rays above `WaterGlintMin`). The glint was a
    round dot, and a grey light near the player (0.19, 0.19, 0.19) made the white dot that came
    and went.
- The mirror image uses `BB_RT_WATER_F0` (default 0.5, was 0.02). At the physical 0.02 the
  traced reflection changed water pixels by about 1/255 in that dark room: only the glint
  showed. At 0.5 the pillar and its panel mirror clearly. What lies under the water is scaled
  by 1 - F (the same irradiance target and GI keep factor the see-through glass uses), so the
  floor darkens where the water reflects. The irradiance scale needs the direct pass to run
  before the reflections (`BB_RT_DIRECT_BASE=0`); with `BB_RT_DIRECT_BASE=1` (the default,
  reflections first) the reflect pass stores the water factor negative in the keep image (glass
  stays positive) and the direct pass multiplies its whole diffuse (the game's base and ours) and
  its own specular by it (`DirectFlagWaterKeep`, binding 16, `BB_RT_WATER_DIRECT_KEEP=0` turns it
  off). Before, the floor under the sewer water kept its full stock and RT direct light, so the
  channel showed a lit brown floor through clear water where stock draws dark, murky water.
- Sheet water fades with depth like the stock layer's depth factor: clear = exp(-depth /
  `BB_RT_WATER_MURK`) (default 0.05 m, 0 = off; 0.15 left the floor near the camera showing),
  depth being the camera-ray distance from the sheet to the G-buffer point. The reflection is
  scaled by 1 - clear and the under-water factor is mix((1 - F) * clear, 1, clear), so the
  channel goes opaque while the shallow ramp edges stay floor. Stock water is dark blue, not
  black: the direct pass adds `BB_RT_WATER_SCATTER` times the game's own ambient base (its cool
  ambient, before the ambient cut) times 1 - under to the specular target, untinted by the floor
  albedo. Scattering our direct light instead drew the lamp pattern on the floor as brown blobs.
- Sheet water is calm and dull like the stock layer: its ripple slope is `BB_RT_WATER_RIPPLE`
  (default 0.1; G-buffer water keeps 0.5) and its F0 is `BB_RT_WATER_SHEET_F0` (default 0.1;
  G-buffer water keeps `BB_RT_WATER_F0`), scatter default 0.04. At 0.5/0.5/0.1 the sewer channel
  showed big wobbly brown reflections of the lit walls. Sewer A/B (play29, mean RGB of the
  channel, stock = `BB_RT_WATER=0 BB_RT_WATER_LAYER=1`): stock 4.4/4.4/6.2, new 4.3/5.0/7.2, old
  9.5/10.3/12.6. Stock's grey haze over the channel is the `a28d2a7a` mist sheet, which stays
  skipped (no fake light); compare against stock without it.
- Traced mist over sheet water replaces that sheet (`MistVolume` in the reflect pass of
  `rt_gi.comp`). Every non-glass depth pixel finds the water level: the sheet its camera ray
  crosses, else a `MaskWater` ray straight down (6 m) from the camera, from the ray's midpoint,
  then from just above the pixel. The camera ray's part inside the slab [level, level + 3 x
  `BB_RT_MIST_HEIGHT`] (60 m cap) is marched in `BB_RT_MIST_STEPS` jittered steps, density
  `BB_RT_MIST` x exp(-h / height) x a drifting, domain-warped value noise. One lamp sample with
  a shadow ray (`MistLamp`, light grid, same atten/cone as `DirectLit`) is taken at one step
  picked by its weight T(1 - step T), times `BB_RT_MIST_ALBEDO`. The pass writes `spec x T +
  in-scatter` and stores -T in the keep target, so the direct pass and the GI apply attenuate
  diffuse and GI behind the mist, also on walls, the walkway and the player, not only on water.
  Defaults 0.05 / 1.5 / 2.5 / 4 steps. Sewer A/B (play33, mean RGB left haze / floor / wall):
  stock with the sheet 13.5/13.8/14.0, 9.4/10.5/12.9, 14.7/14.1/15.1; ours 15.9/15.5/13.7,
  13.6/13.8/12.4, 15.9/15.1/13.8; mist off 9.9/9.2/9.4. Ours is warmer: only the lamps light
  it, stock's blue-grey is the sheet's own ambient. Reflect pass 1.94 -> 2.77 ms (the first
  version, a shadow ray per step, cost 2.3 ms). Density 0.3 at height 1.5 hid the whole tunnel.
- Mist without grain, in pieces (2026-11-07). The random lamp pick and random step pick made
  per-pixel noise that DLSS kept, worse in motion (frame-to-frame luma diff in the haze 2.21 vs
  0.20 with the mist off). Now deterministic: the march jitter is interleaved gradient noise,
  each half of the ray is lit at its in-scatter-weighted mean point, and `MistLamp` sums every
  lamp of the cell times the shadow ray to the strongest one. The noise is cut at
  `BB_RT_MIST_PATCH` (smoothstep from the patch to patch + 0.3), so the mist is separate wisps
  like the stock sheets instead of one block. Defaults density 0.08, patch 0.5, 8 steps (only
  noise per step, no rays); at 0.25 the lit corridor end was an opaque glowing wall that hid
  the beams the stock haze shows. Reflect pass about +0.9 to 1.3 ms with the mist on.
- Mist seen from above (walkway over the water): the level search reached 6 m, so pixels on
  things higher above the water found no level and had no mist, cut in hard lines along the
  walkway; reach is 30 m now. The march cap went from 60 to 300 m (the slab clip keeps the
  steps inside the mist), else distant mist vanished. From above the layer read as a flat
  pale plane: the top is now 4 x height with a quadratic fade, and the height scale varies
  0.3 to 1.7 x `BB_RT_MIST_HEIGHT` with a slow 2-D noise (6.7 m cells), so the pieces stand at
  different heights.
- Mist in-scatter now has the isotropic phase: `in = sum(T dt sigma) x E x albedo / (4 pi)`,
  `BB_RT_MIST_ALBEDO` default 1 (the physical maximum), density 0.2. Without the 1/(4 pi) and
  with albedo 2.5 the mist under a lantern above the walkway water was ~30x physical: the water
  read 55 (mist off 3.7), a flat pale sheet. Physical: 5.5 at density 0.08, 7.8 at 0.25; stock
  11.8/14.2/17.4 there, its blue haze being the sheet's own ambient. So the traced mist is a
  glow around lamps and a darkening elsewhere; away from lamps it cannot reach stock's haze
  without invented light.
- Mist glows on its own like stock's haze (2026-11-08): `BB_RT_MIST_GLOW` (default 0.25) adds
  the absorbed weight times a blue-grey tint, faded in from 1.5 to 8 m from the camera so it
  does not fog the player, and scaled toward `BB_RT_MIST_GLOW_DARK` (0.6) where no lamp lights
  the mist, so it is softer in darkness.
- The mist sits where stock's haze is. Stock's haze is the `71c96598` particle system
  (`Rt::FxSheetShader`): PatchPrimitive draws, mostly additive, drawn after the TLAS build into
  a depth buffer that is not the camera's. Each 96-byte vertex record holds an 8_8_8_8 colour
  at byte 12 whose alpha is the particle's fade (0.01 to 0.17 seen in the sewer), and a world
  3x4 matrix at byte 48 whose translation is the particle position and whose column-0 length
  is its size (2 to 20 m). The rasterizer captures those particles (`CaptureSmokePatch`,
  `AddFog`), hides the raster draw, and `WriteGiLights` stores the last finished frame's up to
  256 particles (view-space centre, radius = size x `BB_RT_MIST_SHEET_SCALE`, alpha) after the
  flame clusters in the lights buffer. `MistVolume` keeps the 8 strongest spheres the camera
  ray crosses, marches only between them, and the density is `BB_RT_MIST` x sum(alpha x (1 -
  d^2/r^2)^2) x noise. `BB_RT_MIST_SHEETS=0` goes back to the water-level slab. A first
  version put the particles in the TLAS as quads (`MaskFog`): the huge overlapping instances
  slowed every GI ray, GI 5.3 -> 9 ms even with the mist off, reflect 6.8 ms. As spheres in a
  buffer: GI 5.3 ms, reflect 1.0 (off) / 2.7 (slab) / 3.0 (sheets) ms, same fps as stock.
  Defaults scale 0.5, density 0.05: at scale 1 the haze covered the whole view, at density
  0.08 it hid the dark arches stock shows, at 0.03 it was gone. The slab's
  `BB_RT_MIST_HEIGHT` default is 5 (was 1.5; stock's haze stands higher than that).
- The traced mist was parked 2026-11-08 and is on again since 2026-11-14 (`BB_RT_MIST` 1,
  `BB_RT_MIST_FOG` 0.25, sky-lit glow; see "Traced mist on by default"). With the mist at 0 the
  stock haze (`71c96598`) draws (`BB_RT_FX_SHEETS=0` still hides it) and its particles are not
  captured. Stock's water mist sheet (`a28d2a7a`) stays hidden unless `BB_RT_FX_SHEETS=1`: over
  the RT water it is an opaque blue film (sewer water patch 6 6 7 without it, stock 4 7 11). Why: at an outdoor bridge stock's haze adds about +9 +12 +12 (8-bit)
  over the water, a grey-green that the traced mist could not reach at a fair cost. The particle
  colour now rides in the low 8 bits of the centre words (`FxParticle.rgb`, cap
  `FogMaxParticles` 1024), and the glow is `BB_RT_MIST_FOG` (default 5 then; sky-lit at 0.25 since 2026-11-14) x the per-area height
  fog colour (`fog_sets[set * 6 + 4]`, bound to the reflect pass at binding 30) x particle colour,
  replacing `BB_RT_MIST_GLOW` when the fog table is there. The main fog colour (+1) was too blue
  (+2 +6 +9); the height fog colour at 5 matched the hue and level. Moon in-scatter (dir set
  light) was tried and dropped: barely visible, +2.5 ms. Cost at the bridge, standing still:
  mist on 6.7-7.1 ms reflect vs 2.9-3.1 off; of the +3.8 ms, lamp shadow rays about 1 ms, 8 vs 4
  steps about 1 ms, noise warp none, the rest the per-pixel scan of 130 particles into 8
  dynamically indexed slots. Parked defaults: GI 6.3, reflect 1.8 ms.
- Stock's haze is lit by our lamps (2026-11-08, `BB_RT_HAZE_LIT`, default on): each `71c96598`
  puff's colour word (byte 12 of its 96-byte record) is rewritten on the CPU from the direct
  lights at its world position (same falloffs as the shaders): alpha x clamp(`BB_RT_HAZE_FLOOR`
  0.3 + lamp luma x `BB_RT_HAZE_GAIN` 1), rgb tinted toward the lamp colour by
  `BB_RT_HAZE_TINT` 0.5. Stock's puffs glow white in the dark; now they are bright only near
  lamps. The word goes to the GPU copy with `updateBuffer` (render pass ended, barriers around
  it); the haze ranges skip the stream buffer (`ObtainBuffer` `no_stream`) because the stream
  copy runs on a copy thread after the patch. The constant ring never held these ranges, and
  patching a synchronous stream copy found none (the haze reads the arena). Sewer alley, still:
  floor 0 + gain 0 removes the haze, off shows stock's glow; GPU-finished latency on 53.5-54.1
  vs off 52.9-53.4 ms (noise). FPS drops seen with `BB_RT_FX_SMOKE_LOG=1` come from the log, not
  from the haze.
- Game lamp lights take the flame's hue (2026-11-08, `BB_RT_LAMP_HUE`, default 1,0.61,0.27,
  `0` = off): every game lamp light here is `LegacyDPointLight*` colour (0.80 0.54 0.40), half
  again as blue as the flame (screen flame 134 107 74 = linear 1 : 0.61 : 0.27), so lamps cast
  white light. Every game point light (not camera-fixed, not held, not a GlowLamp) that is warm
  (r >= g >= b, blue under `BB_RT_LAMP_WARM` 0.6 of red) but bluer than the flame hue keeps its
  luma and takes the hue (colour and spec), each frame before the glow lights are added. Lights
  already orange (0.88 0.54 0.24, 1.23 0.44 0.10) stay as they are. A first version only tinted
  lights within 2 m of a seen glow sprite: at the spawn lamp the nearest glow was 43 m away and 0
  lights were tinted; now 9. Sewer alley lamp-lit wall: off 13 11 10, flame hue 13 10 7, the
  glow light's hue 1,0.36,0.08 15 9 6 (too red); spawn street wall under the lamp A/B/A 23 18 12
  / 22 18 15 / 23 18 12. Stock (F9) wall is as pale as off.
- Water is marked by the game's own water layer (2026-11-08, `BB_RT_WATER_MARK`, default 1; 0 =
  the old `a28d2a7a` mist sheet). The layer draw (`e3021062`) gets the motion store and goes into
  the TLAS as `MaskWater` like the sheet did, hidden by a zero scissor unless
  `BB_RT_WATER_LAYER=1`. Both quads lie at the same level (pick: 1.35 m along the ray above the
  street at the spawn), so the marker alone did not fix the street; the fade did.
- Water fades like stock (2026-11-08): the layer shader lerps toward its water colour by
  clamp(view depth below the surface x 0.2) (constants 87/88 of its buffer 1, both 0.2 here, so
  opaque at 5 m). Ours was exp(-depth / 0.05 m): the street at the spawn, 1.35 m under that
  plane, became black opaque water with a bright mirror, and the blood decals on it read as
  white splashes. Now clear = 1 - clamp(depth / `BB_RT_WATER_MURK`), murk default 5 m. Blood
  patch A/B/A: 15 12 11 / old 14 16 18 / 14 11 10, stock 15 14 16 (bluer moon fill). Stock shows
  only a faint film on that street. Not yet checked in the sewer channel, where the old 0.05 m
  fade was tuned against stock's murky water.
- Deep sheet water is a mirror like stock's (2026-11-08): stock's water colour is its planar
  reflection plus env maps at full strength, so where the lerp reaches it the floor is gone. Ours
  weighted the RT mirror by Fresnel with F0 0.1 and kept the floor under it, so the flooded
  courtyard past the spawn (water 2.37 m along the ray) looked dry. Now the floor keeps only
  clear (diffuse and specular x clear), the mirror gets 1 - clear, `BB_RT_WATER_SHEET_F0`
  default 1 and murk 2.5 m. Courtyard near floor: new 14 14 13, old 11 9 8, stock 14 15 16;
  far 15 16 15 / 10 8 7 / 11 13 15. Floor sheen kept under the water: no change (far 6 vs 5),
  dropped. Ambient water colour (`BB_RT_WATER_SCATTER`) 0.1 to 0.8 gave white blotches (it
  scales irradiance, not albedo), kept 0.04. Stock's own layer shown over RT matched the shore
  line but brings back the lit tunnel patch above, so it stays hidden.
- Murk back to 5 m, stock's own 0.2 (2026-11-08): at 2.5 m the street by the sandbags past the
  courtyard (pick: plane 5.10 m, floor 6.98 m along the ray, about a quarter of the camera
  height) was 75% mirror, so the sandbags read as sunk and the water as too high. Stock lerps
  only ~36% there and the floor shows through. 5 m keeps the floor and the full-strength mirror
  on top.
- Sheet water reflects from the water plane (2026-11-08): `WaterFind` kept `water_p` at the floor
  point, so the mirror ray and the mist sheet started at the floor and the water read lower than
  stock's. `water_p = dir * t_sheet` now. Sandbag alley, bottom floor mean: stock 12, plane 8,
  floor 5 (whole scene ~0.6x stock). `BB_RT_WATER_AT_FLOOR=1` brings the old origin back.
- Murk 1 m (2026-11-08): at 5 m the sandbag alley still read lower than stock, the floor and the
  sandbag bottoms showing through where stock's sheet hides them. The layer's constants there
  are 0.2 too (`BB_LOG_PS=e3021062`, buffer 1 dwords 86-88 = 1, 0.2, 0.2; vertex colour alpha
  255), yet stock's own layer drawn over RT (`BB_RT_WATER_LAYER=1`) is near opaque a few cm
  below the line, so its depth difference grows much faster than our metres along the ray. 1 m
  puts our waterline on stock's (plinth step, sandbag bottoms); 0.5 and 0.2 look the same, 2
  and 5 show the floor. Spawn street (the faint film above) not rechecked at 1 m.
- Raised plane and water body instead of a film (2026-11-08): stock's mist sheet (a28d2a7a) makes
  its water read higher, but over RT it is a blue film, and our own constant film (tried, reverted)
  sat on dry shallow streets too (spawn street: 0.41 m along the ray, ~0.13 m deep). Now the sheet
  plane is raised `BB_RT_WATER_RAISE` 0.1 m (the camera ray stops raise / |cos| before the sheet,
  so walls and sandbags up to 10 cm above it go under) and deep water gets a body colour, (1 -
  clear) x `BB_RT_WATER_BODY` 0.05 x the area's height-fog colour, so it no longer reads as a bare
  mirror. Sandbag alley floor, stock / body 0 / 0.05 / 0.1 / 0.2: 8 10 13 / 14 12 10 / 15 14 15 /
  18 18 19 / 19 22 24; 8-10 9-13 13-15 / 9 8 8 / 12 13 14 / 13 15 17 / 17 20 23.
  The fog-colour body glowed in dark corners (constant, not lit), so the body is now lit: (1 -
  clear) x body x the floor's irradiance. At 0.05 it adds little (dark side 3 4 6 vs 3 4 5 off,
  stock 10 13 17); judged good live as is.
- Sheet water is a physical volume (2026-11-08): the user saw "only reflections, no real water".
  Cause: sheet F0 1 made the surface a pure mirror, and the irradiance-lit body read the
  irradiance target before RT light is added (~0), so `BB_RT_WATER_BODY` 20 changed nothing
  (mean 7.9 vs 7.8). Now: surface Fresnel with water's F0 (`BB_RT_WATER_SHEET_F0` 0.02), floor
  transmission (see the clear-shallows entry below) x (1 - F), and the rest is the murky body,
  traced: with probability clamp(1 - F, 0.1, 0.9) the reflect ray takes a cosine direction and
  returns (1 - F)(1 - T) x `BB_RT_WATER_BODY` 1 x its radiance (no extra rays). The mirror shows
  at grazing only. The old mirror drew the wall's reflection where water should be, which read
  as a lower waterline; with the body, raise 0.1 puts the line at the plinth bottom like stock
  (0.3 drowns the sandbags).
- Shallow sheet water is clear (2026-11-08): with murk 0.15 m the shallow water over a street's
  cobbles read as "a solid mist block"; stock shows the floor there. Transmission is now linear
  like the game's water layer (water colour = clamp(depth x 0.2)): T = 1 - path / `BB_RT_WATER_MURK`
  5 m. Murk 1, 3, 5 all show the cobbles; 5 matches stock. The sheet's vertex alpha (attr2.w)
  is 1 over this water too, so it can't tell water from dry floor (tried, removed). Stock's blue
  tint here is its moonlight on everything (walls, character), not the water. With water's real
  F0 0.02 the clear water showed no reflections (user: "wheres the reflections"); F0 0.2 is
  still faint, 0.5 mirrors the plinth and corpse with the cobbles still under them, 0.8 is a
  mirror. `BB_RT_WATER_SHEET_F0` is 0.5 (same as `BB_RT_WATER_F0`).
- Blood under sheet water keeps its colour (2026-11-08): with sheet F0 0.5 a blood splat on the
  flooded floor turned grey (user: "now the blood splat is grey"; F0 0.02 kept it red). The
  blood is only in the G-buffer albedo (target 2), not geometry: the pick ray at the splat hit
  the sheet, then the 72527-triangle floor. `WaterDecalCover` marks it by hue: (r - max(g, b)) / r
  of the G-buffer albedo, ramp 0.4..0.65 (brown cobbles ~0.2, blood 0.75+); the debug
  (`BB_RT_WATER_DEBUG=1`, red = cover) follows the smears of `BB_RT_GI_DEBUG=12`, plain water
  blue. Covered pixels get no mirror or body, and the keep target carries -(2 + under) there so
  the direct pass skips the water scatter (0.04 x the game's light buffer, which is very bright
  on blood decals: white streaks, gone with `BB_RT_WATER_SCATTER=0`, A/B/A) and the GI apply
  pass still dims by under. Tried and dropped: comparing with the floor's own traced texture
  (layered materials trace one texture: a raised ledge read as half covered, streaks between
  the blood); under 1 on covered pixels (splats glowed against the water). The G-buffer albedo
  is bound whenever water is on, not only glass.
- Sheet water lets the floor through with real water's F0 0.02 (2026-11-08); only the mirror
  uses `BB_RT_WATER_SHEET_F0` 0.5. With 0.5 for both the floor under clear water went half
  dark. Pinkish blobs at the player's feet are sharp reflections of something bright overhead,
  cut up by the ripple (gone with F0 0, roughness 0.3 or 0.2; a 0.25 m near-hit skip did not
  change them). Roughness 0.15 softened them but the user found the reflections worse: kept 0.05.
- G-buffer water is wet only where glossy (2026-11-08): a cobble walkway whose floor shader is
  on the water list (`BB_RT_WATER_DEBUG=1` cyan) read as entirely flooded (user: "some parts are
  water splat, not the entire ground"). Its G-buffer gloss (`BB_RT_GI_DEBUG=11`,
  `BB_RT_DEBUG_THRESH`) is ~0.3..0.5 on the stone and above 0.65 on the puddle by the torch, so
  type-2 water needs gloss >= 0.6 (`WaterPuddleGloss`); the rest is plain floor. Debug after:
  cyan only on that patch, the walkway dry like stock.
- Puddles are calm: G-buffer water's ripple slope was a fixed 0.5 (user: "these are puddles,
  not moving water"); now 0.3 x `BB_RT_WATER_RIPPLE` (0.03). Frame-to-frame change on the
  puddle pixels 5.07 -> 2.07 (camera-only baseline ~0.7..1.0).
- Water sheen off by default (`BB_RT_WATER_SHEEN` 0): it mixes the area's fog colour into every
  water ray by its length, which laid a flat blue-grey fog over the water (user: "why the water
  seems foggy"). A/B/A on / off / on in one view: fog, dark clear water, fog.
- F9 (effects paused) brings stock's water back: the water layer (`WaterLayerShader`), the water
  mist sheet (`a28d2a7a`) and stock's haze colours no longer stay hidden or patched while paused
  (`RayTracing::Paused()` in `SkipPs`, `WaterSheet`, `FogSheet`, `HazeLit`). Sewer alley: paused
  water 4 6 9 = paused with the stock knobs 4 6 9 (RT 2 2 3). A live-config reload no longer
  undoes F9: `BB_RT_PAUSE` applies only when its value in rt.cfg changes. One device lost (write
  fault at address 0, like the amb3 crash above) came 3 s into a pause on play82; 60 s paused, 7
  toggles and reloads on play83 did not repeat it.
- Water reflection hits trace shadow rays like every other reflection hit (`DirectLit` with
  `BounceRays()`, 1 here). They used to trace none, so in an enclosed tunnel the walls the
  water mirrors were lit by every lamp through the stone, and the water glowed (patch luma
  4.97, vs 1.02 with shadows and 1.06 with `BB_RT_WATER=0`). Reflect pass 3.80 to 4.25 ms in
  that view. The flame image needs no extra rays: it is clipped by the mirror ray's own hit.
- The game's forward water layer, ps `e3021062` (vs `9253a878`, `Rt::WaterLayerShader`), is
  hidden while `BB_RT_WATER` is on (`BB_RT_WATER_LAYER=1` shows it again, live). It draws over
  the lit scene: lerp(scene, water colour, depth factor), and the water colour is the half-res
  planar reflection, two static 256x256 env maps and the light-set colour, none of which sees
  RT occlusion. In the arch tunnel past the Central Yharnam lamp it painted the far floor as
  one flat lit patch with a hard edge while the walls around were dark. A/B/A, same view:
  patch luma 1.01 hidden, 8.42 shown, 1.01 hidden (stock 5.51, with its walls lit too). The RT
  mirror and `WaterRipple` take over its reflection and wrinkles. The puddle shapes of the
  `a28d2a7a` sheet show the RT mirror with or without the layer.
- GI rays bounce off the water too (`BounceFlagWater`, on with `BB_RT_WATER`), so light the
  water mirrors reaches walls and the player. A first-segment GI ray that crosses the water
  sheet (`WaterSpan`, a `MaskWater` query up to its hit) reflects about world up with Fresnel
  probability (F0 `WaterGiF0` 0.5, Schlick), else it goes through as before. For ReSTIR the
  sample point is the mirrored hit (or the mirrored miss direction), so reuse stays valid. A
  debug paint confirmed the walls and objects beside the water take the mirror path. GI pass
  8.24 ms on vs 8.22 ms off. The sewer spot is so dark that the extra light is within noise.
  Looking up hides the sky patch on the water: the opening is straight overhead, so its mirror
  image only shows at steep down angles (true mirror behaviour, checked with
  `BB_RT_REFLECT_DEBUG=2`).
- The held torch's own fire is mirrored too. The game's light for it is often off here (fill
  visibility puts it at 0 until it is carried), so the flame image comes from the fire FX draw
  instead (`BB_RT_FLAME_PS`, default `d729c469`, needs `BB_RT_WATER`). Its particles are
  PatchPrimitive draws with a 96-byte vertex record: the position is local (0,0,0) and a 3x4
  world matrix sits at byte 48. The rasterizer reads those centres, the light writer averages
  them into up to 32 clusters (0.5 m) and stores them in view space after the light grid
  (`FlameBlockWords` at word 36873). `FlameImage` draws each cluster as a warm core of radius
  0.15 m. The mirror ray may hit the torch head first, so a cluster up to 0.3 m behind the hit
  still shows (`FlameFxSlack`). Game flame lights within `FlameMerge` of a cluster are skipped
  so the fire is not drawn twice.
- A game light within 1 m of a captured fire cluster counts as carried (an emitter). Before,
  a torch light only became an emitter once the player had walked with it, so a torch equipped
  while standing still stayed dark (fill state `applied 0.00`). Checked live: the re-equipped
  torch light reads `mobile 0 emitter 1 lum 1.75`. Such a light is marked `held`, and
  `KeepOffscreenLight` does not keep a held light as a lamp: a still emitter otherwise stays
  alive after the game stops drawing it, so a torch put away kept lighting the spot (seen live
  at age 440 frames, `lum 1.75`). With `held` the scene goes back to the unlit level (mean
  luma 13.95 lit, 4.2 off, 4.21 before equipping).
- Water ripples (`WaterRipple`): the water normal was flat world up, so the water read as a
  clear pane rather than water. Stock water shows the game's normal-mapped wrinkles. Now every
  level water pixel (sheet or G-buffer water) tilts its normal by the slope of six moving sine
  waves in world X/Z (wavelength 0.2 to 1.2 m, time from the frame counter at 60 fps), fading
  out by 40 m. Slope 0.12 was smoothed away by the denoiser, 0.3 still showed a smooth arc,
  and 0.5 (`WaterRippleSlope`) breaks the torch mirror into moving glints like stock. The
  pattern is anchored in world space, so it stays put when the camera moves. Reflect pass
  about 7.9 ms before and 8.1 ms after (camera not fixed, within noise). Stock's bluish sheen
  on the water comes from the blue moon fill lights, which fill visibility turns off.

## Warm lights are lamps (BB_RT_FILL_WARM)

In the sewer every game light was off (`applied 0.00`, 64 of 64): no lamp passed an emitter
test. The game's anchors sit about 1 m off the fixture (enclosed 0%), the emissive target was
not bound (emissive luma 0), and the sewer lamps do not flicker. The game's colours split in
two groups: lamps and fires are warm and bright (`2.000 1.356 0.997`, `1.000 0.678 0.498`,
`1.500 1.017 0.747`), the designer's moon fills are blue (`0.197 0.339 0.500`,
`0.315 0.543 0.800`) or dim warm bounce fills (`0.200 0.134 0.086`). `Rt::WarmEmitter`
latches a light as an emitter when red >= 1 and blue < 0.6 x red (the flame shader's
`FlameWarm`). Live: 20 of 56 lights at the spawn became emitters and the stair lamp lights
the stairs and shows in the water; the blue fills stay off (the moon and GI are traced).
`BB_RT_FILL_WARM=0` turns it off.

The held torch is warm too, so its light latches. `held` (no lamp keep, see above) is now
any light within 1 m of a captured fire that is within 6 m of the camera
(`HeldFlameDistance`, `HeldCameraDistance`), whatever else made it an emitter. Live: luma
6.41 before equipping, 13.2 with the torch, 6.46 eleven seconds after putting it away.

Some lamps are authored very dim because the blue fill does the work in stock. The sewer wall
lantern near the spawn (`LegacyDPointLight3` at world -117.2 -75.5 -31.6) has game colour
`0.200 0.150 0.072` and passes as an emitter by enclosure, but lit almost nothing once the
fill was off. `Rt::LampFloor` raises a static warm emitter (red > blue, not carried or held)
to a colour sum of at least `LampMinColour` 2.0, the dimmest regular lamp
(`1.000 0.678 0.498`); colour and spec scale together. Live knob `BB_RT_FILL_LAMP_MIN`
(0 = off). A/B/A at the lantern, wall luma: 9.96 on, 4.45 off, 9.05 on. The `dlist` log now
lists the nearest 64 lights first, with `cam` = distance to the camera.

All sewer wall lanterns share that preset, and most fail the enclosure test (0.4% and 0.8% for
the two by the ladder), so they stayed off. `WarmEmitter` has a second, lantern hue test:
green >= 0.7 x red and blue <= 0.4 x red (`LanternGreen`, `LanternBlue`), at any brightness.
It takes the lantern preset (`0.200 0.150 0.072`: 0.75, 0.36) but not the dim warm bounce fill
(`0.200 0.134 0.086`: 0.67, 0.43) or the other warm fills seen here (`0.953 0.579 0.323`,
`0.800 0.595 0.399`). Live: both ladder lanterns became emitters at the floored lum 0.80; no
other light changed state.

Sky gain was tried for the stock blue sheen (GI miss radiance x `BB_RT_SKY_GAIN`): 1, 4 and 50
gave the same luma (2.64, 2.65, 2.57) in four directions. The GI source view
(`BB_RT_GI_DEBUG=8`, ReSTIR GI off) was all yellow (traced), no grey misses: no GI ray at the
sewer spawn reaches the sky, so there is no real sky light to scale there. Removed.

That conclusion was wrong: the sky meshes were in the TLAS. A straight-up ray from the floor
under an open shaft hit the game's dome (ps `dda49e89`) at 1-3 km, and in the open sewer a
second 224-triangle sky mesh (ps `765450df`, two instances, back face) at 460 m, so every
miss became a hit. `BB_RT_SKY_PS` (live, default `dda49e89,765450df`) gives those instances
TLAS mask 0: still rasterized, never hit by a ray, so misses read the sky probe. Live in the
open sewer, A/B/A (`BB_RT_SKY_PS=0` restores the old TLAS): mean RGB 3.90 4.00 6.40 / 3.36
3.36 4.18 / 3.88 3.96 6.29, the water reflecting the blue sky. `BB_RT_SKIP_PS` is not the
tool for this: it stops the draw itself, which also stops the sky pass.

The moon and cloud layer (`765450df`) now gets mask `MaskSky` instead of 0 (see
`BB_RT_MOON_FROM_SKY`). In the open world it was never in the TLAS at all: it is not a smoke
draw, so it got no position store, and it draws after the TLAS build, so its held-over draw lost
its texture. It now stores positions like a water sheet, and a held draw of this shader keeps its
albedo texture for the replay (other late draws are unchanged). The screen-based moon search it
replaces (`rt_sky.comp` phase 3, min(r, g, b) of the learned sky image) needed the moon on screen
once and flipped to the fire core in Central Yharnam or a cloud gap whenever the moon left the
screen, so the moonlight changed direction until the player looked up again.

Measured at the torch spot (reflect pass GPU ms, 60-frame averages):

| Setup | Reflect pass |
|---|---|
| `BB_RT_WATER=0` | 0.7 ms |
| Water find only (debug) | 1.3 ms |
| Water with shadow rays at hits | 7.1 ms |
| Water, no shadow rays (shipped) | 5.1 ms |

With F0 0.5 and the floor scale, in a view with more water: 6.4 ms on, 1.4 ms off.

### Flames are small light sources (2026-11-08, `BB_RT_EMITTER_FLAME`)

A converted emitter's source size was `BB_RT_EMITTER_SOURCE` x game radius / sqrt(peak): 2.7 m for
the hand torch of the patrolling enemy (game r 8.47), 1.4 m for a wall lamp. Its light was a soft
blob metres wide, dimmer than stock at 3 m (0.18 vs 0.42 x colour) and with no hot spot, so torches
read as a generic glow instead of a light. `EmitterFalloff` now takes a flame size (default 0.5 m,
`0` = old): the far field (colour x source^2) is kept, the source shrinks to the flame and the
colour rises to match, so the light falls as 1/d^2 from the flame. Patrol A/B (alternating
1.1 s shots): the torch lights a pool on the wall around the enemy with its arm shadow; off it is
a flame sprite on a dark wall. 0.25 m clipped the wall to white when the enemy stood in an alcove;
0.4-0.9 m all keep a bright niche there (a flame in a niche), 0.5 is the middle. Live knob.
Test: `TestEmitterFlameKeepsFarField`.

### One reach for every emitter (2026-11-08)

Converted emitters reached `BB_RT_EMITTER_RANGE` 2.5 x the game radius (the patrol torch: 21 m,
stock 8.5 m) and lamps `BB_RT_LAMP_RANGE` 6 x (glow lamps 12 m). With the falloff colour x s2 / d2
the light at the cut was 0.0065 x colour for the torch and 0.01 for lamps, so torches lit vaults
and walls far past stock. Now every emitter ends where its light falls to 0.025 x colour:
reach = radius x source_frac / sqrt(0.025), which is `BB_RT_EMITTER_RANGE` 1.26 (torch 10.7 m) and
`BB_RT_LAMP_RANGE` 3.8 (lamp r 1.5: 5.7 m, glow lamps 7.6 m). Range 1.25 vs 1.5 vs 2.5 on the
patrol: same pool around the enemy, no light on the vault 4+ m above at 1.25. A range <= 1 turns
the conversion off (the old legacy curve), so 1.0 is not a valid test value. Lights that are not
emitters (blue fills, spots) already use the game radius.

`BB_RT_DIRECT_DEBUG=10` (needs `BB_RT_RESTIR=0`): red = light that reaches the pixel only because
the 0.3 m end gap (`BB_RT_SHADOW_GAP`) skipped a blocker next to the light, green = lit with a
2 cm gap too. Sewer patrol view: red is the torch alcove (the torch head around its light) and the
ladder lamp's cage pattern; no wall between a light and a lit surface found there. The gap cannot
tell a fixture from a wall within 0.3 m of the light; a size test per instance would need
instance bounds, which the GPU-gathered BLAS do not keep.

### Hand torch vanished looking down (2026-11-08)

With the player's torch out and the camera tilted down, the floor around the player got no torch
light. Pick at the floor next to the player: 3 lights in the tile, 0 shadow rays, i.e. every light
was dropped before tracing; `BB_RT_DIRECT_DEBUG=9` painted the whole floor red (stock depth test
fails) and `BB_RT_LIGHT_DEPTH=0` lit it. Numbers through the pick: floor depth 0.98959 (z 4.80 m),
the game's window for the torch quad 0.99568 / bounds max 0.99825, i.e. z ~11.6-28.6 m (near
~0.05 m), a volume ~20 m away, while the parsed light sat at z 4.62 m in the torch (the stale track
copy also said 24.6 m). The copied test belonged to another camera or frame, not this light.

`rt_direct.comp` `StockDepthOwnsLight`: the stock depth test is used only if it would pass the
light's own centre (`DepthOfZ(light z)` inside the bounds and through the compare, 1e-5 slack);
otherwise the light is shaded without it. A real light volume always holds its centre, so lights
whose test is consistent keep it. Live: debug 9 red -> green over the floor, normal view matches
`BB_RT_LIGHT_DEPTH=0`.

### Torch too hot near, flicker too flat (2026-11-08)

With `BB_RT_EMITTER_FLAME` the intensity is colour x (source_frac x r)^2 (peak cancels out), so a
hand torch (r 8.43, colour 2.97) gave ~6.7 at 1 m against the game's (1 - d/r)^2 x 2.97 = 2.3: the
coat burned white. Flame 0.5 / 1.0 / 1.5 m A/B on the sewer wall: mean luma 66 / 53 / 42; 1.0 keeps
the wall glow and the hot spot without the white coat (1.5 loses the wall). Default 1.0.

The game flickers the torch colour by ~8% rel. std (dlist lum over 22 samples) and part of it is
lost in the GI bounce, which is accumulated over frames. `BB_RT_EMITTER_FLICKER` (default 1)
scales each emitter's deviation from its own running mean (`Rt::FlickerScale`, per track id,
weight `BB_RT_EMITTER_FLICKER_AVG` per frame, clamped 0.25-2.5x): same rhythm as the game, larger
swings. With a 0.1 weight the mean followed the slow flicker: gain 3 gave only 8.4% -> 13.6%.
With weight 0.02 (about a second) the swing scales as asked: dlist lum rel. std 9.3 / 15.6 / 25.2%
at gain 1 / 2 / 3; sewer wall luma over 8 shots 3.6% at gain 1, 14.1% at 2.5. Default 2.5.

Follow-up, same day: the swing read as a fast strobe and as one uniform pulse. The game's colour
is now smoothed first (`BB_RT_EMITTER_FLICKER_SMOOTH` 0.1 per frame, ~10 frames) and the gain
applies to that, so the pace is a flame's, not a frame's. Every emitter but lamps also sways
(`BB_RT_EMITTER_SWAY` 0.15 m, `Rt::FlameSway`: two incommensurate 0.4-1.3 Hz sines per axis,
phases from the track id, vertical half), so the lit pattern and the shadows move instead of
only dimming in step.
Live, sewer wall, 12 shots: luma spread 2.6% at gain 1, 8.3% at 2.5 with the smoothing (14% raw).
Sway with the flicker off, mean difference between consecutive shots: 3.2 (sway 0, the noise
floor), 3.6 (0.05 m), 5.1 (0.15 m), 8.3 (0.3 m). 0.15: visible, still inside a torch flame's wander.

Fixed flames: the game keeps wall torches, candles and lamps steady (dlist lum constant over 40
samples), so the gain had nothing to scale. A light whose own swing stays under 1%
(`FlickerState::dev`) gets `Rt::FlameNoise` (three incommensurate sines 0.5-3.2 Hz, phases per
track id) x `BB_RT_EMITTER_STEADY_FLICKER` (0.3, ~13% rel. std). Fire and candles sway; the sway
scales with the game radius (r / 8.5, at least 0.2: a 2 m candle ~3.5 cm, the 8.5 m hand torch
15 cm). Lamps (flames behind glass) flicker and do not sway.
Correction: a sewer wall lamp (game r 5, 5 m from the player) swayed. Its open cage measures
1.2-9.8% enclosed, not the 40% lamp threshold, so it counted as fire. No signal tells a caged lamp
from a fixed fire by enclosure, so the sway goes to carried flames (mobile tracks: hand and enemy
torches) and candles (fixed, game r <= `BB_RT_CANDLE_RMAX` 3 m; candles are 2 m, wall lamps 5 m
and up). Every other fixed emitter only flickers.
Steady flicker, take two: the wall lamp's swing read too small, and at 0.9 (linear) the dips
went down to the 0.25 clamp and read as the lamp going out. Now the scale is
exp(`BB_RT_EMITTER_STEADY_FLICKER` x noise), default 0.35 (x0.70-x1.42, dips as gentle as
flares), and FlameNoise is slower (0.25-1.6 Hz), which the accumulated GI follows better.

### Workshop fireplace and candelabra flicker (2026-11-09)

User: the fireplace, candles and outdoor fire lacked the stock "glimmer and wobble".
- Measured with `tools/live_vision.py` (live DXGI capture, per-region luma sd and optical flow),
  RT vs F9 stock: mantel 0.8% vs 3.7%, floor 1.8% vs 3.9%.
- Per-frame light log (`BB_RT_DIRECT_LIST_LOG=2`): the game draws each light once a frame.
  Candelabra lights (DPointLight5, r 6) are constant. The fireplace (LegacyDPointLightShadowed1,
  r 3.75) sits at sum 2.000 and swells to 2.175 over ~20 frames every ~1.7 s (0.6 Hz, the stock
  surfaces' main frequency). That swell gave it `FlickerState::dev` 0.015, over the 0.01 steady
  threshold, so the fireplace, the biggest flame in the room, got no FlameNoise. Every candle got
  ~15%.
- Fix: the steady threshold is now `BB_RT_EMITTER_STEADY_DEV` 0.05 (game-flickered flames sit at
  0.06+). RT-only A/B, interleaved: hearth 0.7-0.9% (old) -> 3.6-6.9% (new), mantel 1.2 -> 2.5-3.9%.
  Stock is hearth 3.1%, mantel 2.6%, so this is a little above stock (LOOK: more dramatic).
- Sway: fixed flames (candles, candelabra, fireplace) now sway by `BB_RT_FIXED_SWAY`; carried
  flames keep the radius-scaled `BB_RT_EMITTER_SWAY`. Optical-flow motion, px/frame (floor /
  cabinet shadows): 0 m 0.023 / 0.016 (noise floor; stock 0.015-0.022), 0.015 m 0.024-0.027 /
  0.019-0.020, 0.05 m 0.043-0.045 / 0.031-0.032. The user called 0.05 "too much wobble for an
  inside and small fires". An indoor flame's light centre moves about 1 cm, so the default is
  0.015.
- RT capture runs at 11 fps (the copy waits behind the RT GPU work; the winrt backend gives the
  same), so its sd and p2p are valid but Hz above ~5 are not.

### Lamp halo (2026-11-08)

Fixed flames (flag 4, not near a mobile track) get a glow in the air around them, so the steady
flicker shows on the lamp itself, not only on the walls. `BuildHalos` (rt_direct.h) picks the 16
nearest, packs pos/radius plus colour x intensity x `BB_RT_HALO`, and the GI apply pass adds, per
pixel, the integral of a Gaussian (`BB_RT_HALO_RADIUS`) over the view ray up to the depth, softened
by `BB_RT_HALO_CORE`. It rides the light's flickered colour, so it flickers with it. GI must be on.
- First try used a hard sphere chord: read as a disc ("too fake"). Gaussian closest approach fixed it.
- Width A/B 0.35 / 0.2 / 0.12 m: 0.35 and 0.2 read too big; 0.12 hugs the lamp cage. Default 0.12.
- Colour: the light's own colour reads creamy white in the haze. The shader pushes it away from
  white keeping the peak channel (pow(c/peak, `HALO_SAT`)); A/B 1.0 / 1.8 / 2.6: 1.8 is a natural
  flame orange, 2.6 too dim and red.
- Lamp crop luma rel std: 2.6% with steady flicker off, 8.7% with halo + flicker 0.35.
Knobs: `BB_RT_HALO` 0.03, `BB_RT_HALO_RADIUS` 0.12, `BB_RT_HALO_CORE` 0.1.
- Halo colour, take two: pushing the light's own colour away from white (pow 1.8) suited the
  cream lamps but made already red lamps (game colour 0.200 0.150 0.072) deep red. Every halo
  now gets one flame tint at the light's peak channel, `HALO_TINT` (1, 0.5, 0.22); A/B
  (1, 0.62, 0.32) read pale, (1, 0.5, 0.22) a natural flame for both lamp kinds.
- Glow lamps flicker too. Some sewer lamps have no game light: the game only draws the lit
  glass, and `AddGlowLights` adds the light ("GlowLamp"). Those lights have no track, so they
  skipped the flicker (and are missing from `BB_RT_DIRECT_LIST_LOG`, which only lists tracked
  lights). Found by painting halos by index: the two lamps in view were halos 1 and 2, while
  every logged flame was 50 m+ away. Now they get the steady flicker seeded by
  `PositionFlickerId` (world position on a 2 m grid). Lamp crop rel std with
  `BB_RT_EMITTER_STEADY_FLICKER` 0 / 1.5: left 1.0% / 24.4%, right 0.4% / 15.1% (before the fix
  the right lamp stayed at 0.4-0.8% at any value); default 0.35: right 1.7% -> 5.4%.
- Lamps in the water: the mirror drew a lamp only as `FlameImage`'s 7 cm core, which the
  ripples broke up, so lamps barely showed in puddles. `MirrorHalo` (rt_gi.comp) adds the same
  halo integral along the mirror ray for every halo light in the hit's light cell; the knobs
  ride in the free words 1-3 of the flame block. Sewer puddles, A/B halo 0 / 0.03 / 0.3: no
  lamp image / warm glints where the lamps mirror / bright lamp images in both puddles.
- Halo vs the light it belongs to, 8 s video at 31 fps with steady flicker 1.0, both lamps in
  view: halo vs the wall it lights corr 0.99 at lag 0 (rel std 22.6% / 19.7% and 25.7% /
  26.5%), so the halo and the light flicker together.

### Halos only on lamp glass, loot is not fire (2026-11-08)

Killing an enemy left a floating warm orb and the corpse loot got a warm halo and an orange
pool (stock screenshot). Every warm emitter got a halo, and the loot
counted as fire.
- Glow lamps must be steady: `GlowLampSteady` keeps a glow capture only after
  `BB_RT_GLOW_MIN_HITS` 30 hits, with a position drift EMA of at most `BB_RT_GLOW_MAX_DRIFT`
  0.1 m and blue <= `BB_RT_GLOW_WARM_MAX` 0.8 x red. Real lamp glass logs drift 0.000; kill FX
  (blood echoes) move, so they no longer become lights or halos.
- Halos are anchored to the steady glow spots (the lit glass), not to every light. `BuildHalos`
  pairs each spot with the nearest fixed emitter within `BB_RT_HALO_MATCH` 2 m for colour and
  flicker. No spot = no halo, so open fires, loot and ambient lights get none. The halo list is
  written once after the fog block; the screen pass and `MirrorHalos` (water) read the same block.
- The loot was a "flame" three ways, all from its light: game colour (0.611 0.611 0.476) /
  (1.016 1.016 0.791), red = green. Its wisps are drawn with the fire FX shader, so the light
  latched as `held`; `TintLampLights` turned the pale yellow orange; and the water drew orange
  `FlameImage` cores at the wisp. `FlameColour` (green <= 0.9 x red) now gates held/carried/
  emitter and the lamp hue tint.
- Fire FX particles carry a colour (same record offset as fog). Loot wisp particles log
  `ffe6be`: `PaleParticle` (lowest channel >= 0.6 x highest) splits them out as wisps. A/B at a
  corpse in water, reflections 1 / 0: orange pool / gone; pale gate off / on: orange pool /
  gone, wisp kept. `BB_RT_FLAME_PALE=1` turns the split off, `BB_RT_FLAME_LOG=1` logs particles.
- With the loot no longer an emitter the fill-light fade took its light to 0 (dlist "applied
  0.00"): the loot lit nothing. A light within 1 m of a wisp now keeps the game's light as is.
- Wisps get their own image in water mirrors (flame block word 3 = 1): `WispFxColor`
  (1, 0.9, 0.75), 6 cm tall and 12 cm wide, gain 0.8. The fire-sized core (15 cm, gain 3) read
  as a white pillar ("ugly"); 6 cm round read small, 12 cm wide was approved.

### Post lamps, candles, shelf, sky in mirrors, puddle flicker (2026-11-08)

- Post lamp glass read black: the glow lamp lit its own iron housing brighter than the glass the
  game draws over it. Glow lamps get `housed` (light flag 16); `HousingFade` (rt_direct, rt_gi)
  gives no light within 0.3 m, full at 0.6 m. `BB_RT_HALO` 0.03 -> 0.005 (the halo outshone
  the glass after the glow lamp power raise). Glow lamps: radius 5, strength 1.2, flame 1 m
  (`BB_RT_GLOW_FLAME`), reach 1.8 x radius (`BB_RT_GLOW_RANGE`), tint (1, 0.8, 0.55).
- Candles (fixed, game r <= `BB_RT_CANDLE_RMAX`, not lamps or carried): source 0.85
  (`BB_RT_CANDLE_SOURCE`), reach 2.5 x r (`BB_RT_CANDLE_RANGE`). Reach 5 matched stock on the
  statues but four candles cost direct 3.2 -> 7.5 ms (35 FPS, "fps loss idle"); 2.5 lights them
  the same for 3.8 ms (41 FPS).
- Shelf (`BB_RT_SHELF` 1 m): a static mesh whose probe sample jumps metres is the same draw at
  another placement (reused buildings). rt_probe.comp writes 5 words per probe (worst error,
  sample point, index); the CPU keeps the old BLAS as a shelved kept instance (`ShelfBit` key)
  and takes a shelved one back when the draw returns there.
- Mirror rays that miss read the sky: the 1024x1024 octahedral sky image learned from the
  screen's sky pixels (rt_sky.comp phase 2, `BB_RT_SKY_IMAGE`), else the dome texture x fitted
  gain (`BB_RT_REFLECT_SKY_DOME`). The dome binding lives while its image uid lives, not only
  while the game draws it. 512 cells (~0.4 deg) left the clouds soft in puddles.
- Puddle flicker (mirror <-> dry while moving): `WaterFind` took the closest hit, and a puddle
  floor has coplanar copies (kept instances, second draws) without the water shader; which copy
  won changed with the view angle ("all puddles disappear at this angle", water mask blinking).
  It now walks every candidate in the depth window and counts water when a water surface lies
  within `WaterCopySlack` (0.2 % of distance) of the nearest one. Cost +0.05 ms (reflect 3.69
  -> 3.74 ms, same view). User: "Fixed".
- Puddle normals: the full cobble bump scattered each mirror ray over a wide patch of sky (flat
  violet, no clouds); flat water was "like mirrors". Normals within ~45 deg of up are
  `mix(up, n, PuddleBump 0.25)`; approved. Puddle ripple stays 0.3 x `BB_RT_WATER_RIPPLE`.

## Forest reflections: rough cutoff 0.4 and gated water probes (2026-11-09)

Forest lamp spot (worst FPS), DLSS, 4070 Ti SUPER, GPU 99 % busy. Checked the Cyberpunk
"RT Overdrive optimizations" mod ideas first: 1 ray per pixel is already ours, 1 bounce vs 2
measured equal (the adaptive budget already drops to 1 here), and ReSTIR GI off (its plain-RIS
idea) is slower (19-20 vs 25 FPS).

Live A/B, same boot (`BB_RT_CFG`), FPS:

| Knob | Off | On |
|---|---|---|
| `BB_RT_REFLECT_MAX_ROUGH` 0.6 -> 0.4 (now live) | 37.7, reflect 6.2 ms | 42.5, reflect 3.1 ms |
| `BB_RT_REFLECT_MAX_ROUGH` 0.6 -> 0.25 | 37.5 | 41.3 |
| no shadow rays on rough reflection hits (on top of 0.4) | 43.9 | 43.6, dropped |
| `BB_RT_GI_TMAX` 20 | 43.8 | 45.3 (opt-in, hall leak risk) |
| `BB_RT_GI_TMAX` 30 | 43.8 | 44.4 |
| `BB_RT_DIRECT_RAYS` 32 -> 8 / 4 | 43.7 | 43.8 / 42.5, no gain |
| `BB_RT_DIRECT_NOVIS` (cost of all direct shadow rays) | direct 5.1 ms | 0.8 ms |
| `BB_RT_GI_DENOISE` 2 -> 1 | 42.8 | 43.8, noisier, kept 2 |
| `BB_RT_WATER` 0 (before the gate) | 43.8, reflect 3.0 | 45.3, reflect 1.7 |

- `ReflectMaxRoughDefault` 0.6 -> 0.4: rougher pixels keep the game's specular. Screenshots at
  0.6 and 0.4 differ within the frame-to-frame noise (swaying lamp).
- Water probes gated: the reflection pass traced a puddle probe for every pixel (a NoOpaque
  window query over the floor, any-hit on every leaf). It now runs only while a puddle floor
  (`BB_RT_WATER_PS`) is in the TLAS and only on pixels glossy enough to be a puddle
  (`WaterPuddleGloss`, the same test that demoted them after the probe). The sheet probe runs
  only while a water sheet is in the TLAS. The log line `water probes frame N` gives the counts.
  The forest has 1 sheet and 0 puddle floors.

Rejected the same day, Yharnam street, standing still, 3 rounds each:
`BB_RT_GI_TMAX` 20 measured 63.2 vs 63.3 FPS (GI 4.4 vs 4.6 ms), so the forest +1.5 FPS does not
carry over. A "far-hit cache" variant (first-bounce hits beyond 20 m take the cache colour, the
ray still finds the far wall so no hall leak) measured 53.6 vs 54.8 FPS: the full-length
traversal is the cost, not the bounce after a far hit. Code removed; GI_TMAX stays opt-in.

Before/after, same view, separate boots: shipped rt-v1.9.7-pt1 43.7 FPS (reflect 4.0 ms, at
3172x1328 output) -> this build 51.0 FPS (reflect 0.7 ms, at 3440x1334).

## Forest fire pits: not lamps, lifted out of the log pile (2026-11-09)

User report: the forest bonfires were far too bright. `BB_RT_DIRECT=0` looked like stock, so
the direct pass was the source. `BB_RT_DIRECT_LIST_LOG` showed the fire pits (game r 5,
colour ~6) latched as **lamps**: reach 19 m (x3.8) and colour ~50 (lamp source 0.6), and no
stock depth test (`PackStockDepth` skips lamps). The lamp test was enclosure only
(`BB_RT_LAMP_ENCLOSED` 0.4), and the log pile plus the ground enclose the light 56-58%
(nearest solid 0.03 m), as much as a glass wall lamp.

- The fill-vis pass (`rt_gi.comp` `RT_LIGHT_VIS`) also reports `enclosed_up`, the enclosed
  share of the upward rays (world y > 0.5), in `vis_out[2].z`. Fire pits measure 24-29%; a
  lamp has its cap right above the flame.
- `Rt::FillVisLamp`: enclosed >= `BB_RT_LAMP_ENCLOSED` **and** up >= `BB_RT_LAMP_UP` (0.5).
  `Rt::FillVisFirePit`: as enclosed, open above. Both latch per light like before; a light's
  first latch logs `latched as a lamp` with its evidence.
- As plain emitters the fire pits lit almost nothing: every shadow ray from the ground ended
  in the sticks. Fixed fire pits move up `BB_RT_FIRE_LIFT` m (default 0.5) out of the pile.
  Mean luma of the ground at the fires (same view): stock 28.4, lift 0 22.3, 0.3 25.5,
  0.5 27.8, 0.7 27.3; 1.0 over-lit the ground next to one pit.
- The emitter line of the fill-vis log adds `N lamps, N fire pits lifted X m`. The light list
  (`BB_RT_DIRECT_LIST_LOG`) adds `up` and `near` for each light.
- Not checked live: the Yharnam glass wall lamps (save is in the forest). If one loses its
  lamp reach, its `up` value in the light list says why; `BB_RT_LAMP_UP=0` restores the old
  rule (needs a restart because the latch is sticky).
- Test: `tests/test_rt_lights.cpp` `TestFillVis` (fire pit 57.8% / 27.3% up is not a lamp,
  capped 65% / 80% is).

## Facing the moon: lily field brightness and eye adaptation (2026-11-09)

User report: in Hemwick's lily field, looking toward the moon, the image was far darker than
stock (lower-half screen luma, same view: RT 12.1, stock 53.3). Three causes:

- **ReSTIR GI lost energy.** The spatial pass zeroed the reservoir when the winning
  neighbour sample was blocked from this pixel. It now keeps the pixel's own reservoir
  (`rt_restir_gi.comp`), which gives back about a quarter of the GI in the field.
- **Lilies lost their translucency.** 79b602e dropped the wrap term with the fake rim and
  ambient. It is back for thin translucent materials only (g1.y > 0: `(nl + w) / (2w + 1)`,
  w = 10 g1.y), not as a general wrap. `MoonVisibility` flips the ray to the back face for those
  materials when the moon is behind them. Everything else stays plain Lambert.
- **Backlit by design.** The moon is behind the field's big tree (canopy blocker at 44 m),
  so the field is in real shadow. Stock is bright there because of its camera-side ambient and
  fill, which the no-fake-light rule removed.

Remedy (user choice): **eye adaptation**, `rt_exposure.comp`. It runs after the fog at the post
trigger, on the HDR scene colour before the game's bloom and tonemap.

- It measures the log-average luminance of a 64x36 grid of non-sky pixels.
- It moves the boost toward `key - mean` by `BB_RT_EXPOSURE_RATE` per frame (about 0.4 s at
  80 FPS).
- The boost never goes below 0 and never above `BB_RT_EXPOSURE_MAX` stops.
- The gain applies to geometry only. The sky keeps the game's look: a full-frame gain blew the
  sky and the distant fog to white.
- Highlights roll off: the gain is `g / (1 + (g - 1) L / W)` for L below a white level W
  (`BB_RT_EXPOSURE_WHITE` stops above the key), with no change above W. The curve is monotonic
  and continuous at W.
- No light is added.
- With F9 (stock) it only measures. The log line every 120 frames prints both values, which is
  how the key was set: stock reads -5.2 to -5.6 log2 at the spawn gate and in the field.

| Knob | Default | Meaning |
|---|---|---|
| `BB_RT_EXPOSURE` | `1` | Live. `0` turns the eye adaptation off (resets it). |
| `BB_RT_EXPOSURE_KEY` | `-5.5` | Live. Target log2 luminance of the lit geometry (stock's measure). |
| `BB_RT_EXPOSURE_MAX` | `1.5` | Live. Largest boost in stops. |
| `BB_RT_EXPOSURE_RATE` | `0.03` | Live. Share of the gap closed per frame. |
| `BB_RT_EXPOSURE_WHITE` | `3` | Live. Stops above the key where the boost has faded out. |

Measured live, lower-half screen luma, same view, one boot:

| Spot | Exposure off | Exposure on | Stock (F9) |
|---|---|---|---|
| Hemwick spawn gate | 27.6 | 38.9 | 45.4 |
| Lily field toward the moon | 26.0 | 39.6 | 44.6 |

The boost sits at the +1.5 EV cap in both views. The field stays a little darker than stock
because it is backlit. The on-screen FPS was the same with the pass on and off (83.9 / 84.2).

### Thin cards pass moonlight through (2026-11-09, `BB_RT_THIN_TRANSMIT`)

User report: still dark facing the moon. `BB_RT_DIRECT_DEBUG=11` (new; needs `BB_RT_RESTIR=0`
like the other direct views) colours the moon term per pixel: green lit, cyan lit through a thin
card, black shadowed, blue faces away, dark red no moon term. The field was almost all blue.
Nothing lost its shadow test: the visible side of every petal faces away from the moon, and the
game's own directional diffuse is zero there too. Stock is bright only from its fill and ambient.

Real petals and leaves are thin sheets that pass part of the light through, so a backlit field
glows. A pixel counts as a thin card when:

- the camera ray to its G-buffer point (rt_gi.comp GlassFind's confirm window) lands on an
  alpha-tested surface, or on the **back face** of an opaque one. A closed solid never shows the
  camera a back face. The near lilies are opaque double-sided petals (ps bb10e440, seen from the
  back face), the far ones are alpha-tested cards (ps dbeccd9c);
- it is not blended, and its spec colour is below 0.1, so iron stays solid.

Moonlight from behind (n.L < 0) then lights it with `-n.L * BB_RT_THIN_TRANSMIT`. The moon
shadow ray starts from the back side, like the g1.y wrap materials. One camera ray per pixel,
only where the moon is behind the surface; the answer is cached for the pixel.

| Knob | Default | Meaning |
|---|---|---|
| `BB_RT_THIN_TRANSMIT` | `0.4` | Live. Share of the moonlight a thin card passes; `0` off. |

Measured live, lily field facing the full moon, lower-half screen luma, same view, alternated
twice:

| Transmit 0 | Transmit 0.4 | Stock (F9) |
|---|---|---|
| 33.4 / 33.0 | 40.9 / 41.9 | 69.9 |

GPU direct pass: 1.66 ms (1.7 to 1.9 ms before, other views). Debug 11 shows the field cyan and
the stone pillars blue, so no solid geometry was taken for a card. The exposure boost now sits at
+0.5 EV here, under its cap: the key (-5.5) is the limit. Stock measures -4.5 in this view, 1 EV
brighter than at the spawn gate where the key was set.

Second report: "I moved the camera and it is black again", same spot. Debug 11 painted the
whole field black (shadowed), not blue. The moon direction did not move: `BB_RT_PICK` read the
same view direction to the moon at every pixel. With the moon low, a petal's moon ray crosses
the flower bed and hits the next petal 0.5 m away (blocker: instance 36, alpha-tested ps
dbeccd9c). The petals shaded each other like stone.

`ThinMoonVisibility`: within `ThinNear` (1.5 m) of the surface, a thin card on the moon ray
(alpha-tested, or an opaque sheet hit on its back face) passes `BB_RT_THIN_TRANSMIT` of the light
instead of blocking it. Past 1.5 m every blocker is solid as before, so tree canopies and walls
keep their shadows. The near segment runs with every geometry non-opaque (`gl_RayFlagsNoOpaqueEXT`)
so OMM-opaque petal parts reach the loop. Off with `BB_RT_THIN_TRANSMIT=0` and in rooms with
portals.

| View | Before | Transmit 0 | Transmit 0.4 |
|---|---|---|---|
| Field, moon at the top edge (lower-half luma) | 14.1 | 33.4 / 39.9 | 41.8 / 42.2 |

GPU direct pass: 1.66 -> 2.79 ms in this view (the near segment's non-opaque traversal).

### Kept instances keep their tag; the moon does not creep (2026-11-09)

Third report: "black again" after turning. A kept off-screen instance takes its TLAS mask from
`entry_tags`, and that was only recorded with `BB_RT_PICK` on. The moon mesh (SkyLayerPs, 32
triangles about 750 m out, drawn only while in view) stayed in the TLAS as a solid card in front of
the moon once the camera turned: every moon ray stopped on it (pick sweep: blocker at 740 m, mode 2
= kept) and whole fields went black. Mist, water sheets, panes and portals lost their masks the
same way. `entry_tags` is now always recorded; the sweep finds no 740 m blocker.

Fourth report: "when I started moving, the shadows started changing, like trying to get the real
one". All of it was the cached moon position moving, so the light turned:

- Each find lands up to a grid cell (~25 m at 750 m) off the disc centre, and from one spot every
  find lands off the same way (~10 m). The cache took the newest find, then a running mean capped
  at 64 finds; both followed the player, the mean in 2 m steps (1 degree) while standing still.
  `MoonCache` now keeps the plain mean of all finds (`MaxSamples` = `MaxVotes` 1000).
- Strays: from one spot the search kept finding a point 52 m (4 degrees) off, and from the spawn a
  point 123 degrees off at the horizon (~1750 m out), which the old "farther than 100 m is another
  moon" rule took as the moon and lit the field from. A find farther than `Gate` (30 m) from the
  moon goes to one candidate cluster instead; the candidate replaces the moon only when it has more
  votes. Votes are saved as a fifth column in the moon file (a line without them counts 8).
- The shader lit from the find when it disagreed with the cache by over 3 degrees, so strays lit
  the frame directly. With a cached moon the light now always follows it (`MoonSeedAgree` removed);
  finds only feed the cache.

Live (view11, user walking the field): the cached moon stayed within +-2 m (0.15 degrees) of
(-203 256 637) and the horizon stray at the spawn was outvoted 22 to 1. User: "seems good now,
none shadow flickering and light changing".

The mesh is fixed in the world, not on a camera-centred dome: with the camera logged per find
(`cached at ... camera ... direction` line), 70 m of camera movement left the finds within about
+-12 m of (-203 255 637) while the direction changed 4 degrees.

## Maximizing the window lost the device inside DLSS (2026-11-09)

Maximize/restore lost the GPU (checkpoint inside DLSS/NGX) on the 3rd resize of view13, and once
after 20 min on view12 when the user maximized. A resize recreates the swapchain on the present
thread, and `Swapchain::Destroy` calls `vkDeviceWaitIdle`, which needs every queue externally
synchronized, while the render thread kept submitting. The resize, acquire-failure and HDR
recreates now hold `Scheduler::submit_mutex` (the present-failure path already did). view14: 8
maximize/restore cycles (16 resizes), no device loss.

## Hunter's Dream look check at player distance (2026-11-09)

A free-camera walk of the isle (house front, graves, gate) against F9 stock, interleaved live
A/B with `tools/look_ab.ps1`. No knob change: every candidate failed the data.
- House front, dark next to the fire: the gap to stock (mid 25 vs 48) is the cool designer fills
  the fill gate removes (`BB_RT_FILL_VIS=0`: top 10% 86 -> 128, tint neutral). They are fake
  light and stay off. The fire lights stay on (emitter, applied 1.00). `BB_RT_EMITTER_SOURCE`
  0.25 + range 1.58 (the game curve at 4 m) moved mid by +2, inside noise. The fire cycle moves
  the area 15-25 luma over minutes; in its bright phase it is in the LOOK bands (top 10% 105-122,
  p20 15-19, p90/p20 about 6).
- Graves, open ground: p5 9-10 vs stock 21, but the `mask` shows the floor is canopy and trunks
  backlit by the sky; the moon shadow on the ground keeps detail. `BB_RT_GI_FILL_SKY` 1.5 / 2
  did not move p5 (adaptation pulls p90 down instead). Tint neutral (a -4, b -1).
- Edge mist: luma 75 vs stock 73, same tint; with `BB_RT_FX_FOG=0` it is 83.
- Workshop interior (player inside, fly camera at the altar, candelabras, desk lantern,
  workbench): the warmest and most dramatic area. Candles light the altar, curtains and statue
  under a moon shaft where stock is flat; contact shadows and near-white flame cores hold. Mean
  34-68 vs stock 23-54, top 10% b +25..+48 (lamp-lit, allowed). The deep shelf cavities under
  luma 6 are crushed in stock too. The white blob at the high window is the game's shaft sprite,
  same in stock. Open: the player's pale coat reads khaki; stock keeps it pale with its
  camera-fixed player light (`BB_RT_FILL_LIGHT`, off as fake light).
- Workshop wardrobe glass (user request): the wardrobe and desk-cabinet door panes are ps
  `ca7226ec` (pick: 85-triangle mesh, front and back faces 2 cm apart, gloss 0.75, cabinet back
  0.9 m behind). Added to the `BB_RT_GLASS_PS` default (`1c3953c6,ca7226ec`). With
  `BB_RT_GLASS_DEBUG=1` a turn of the workshop accepted only those panes. Live: the closed door
  shows the bottles inside with the pane's crack decal over them; the open door shows the room.
  Not yet checked in other areas (the 2000-triangle and gloss guards still apply).

## Workshop bottles: masked glass (2026-11-09)

The workshop bottles and jars are one 29352-triangle mesh of the glass shader ca7226ec, so
`BB_RT_GLASS_MAX_TRIS` (2000) kept them opaque. The mesh also holds metal parts (rings, caps,
pipes, nuts) that share its 1024x1024 BC1 atlas, so the whole mesh cannot be glass.

- **Masked glass:** a `BB_RT_GLASS_PS` draw over the triangle cap whose albedo has a mask file
  `<dir of BB_RT_CFG>/glass_masks/<XXH3 of the guest texture, 16 hex>.pgm` (P5, 8 bit, any size)
  is glass only where the mask is white. `RayTracing::GlassMaskSlots` uploads the mask as an R8
  texture, registers it as a surface slot and stores the slot as `-alpha_ref` (only on
  non-alpha-tested draws). The instance is non-opaque. `GlassAt(instance, primitive, bary)`
  in rt_gi, rt_direct and rt_restir_gi replaces the instance-only glass tests (any-hit,
  TraceSpan, transmittance, GlassFind, ThinMoonVisibility, BlockedSpan).
- **Tools:** `BB_RT_GLASS_MASK_DUMP=1` writes each candidate albedo (mip 0) to
  `glass_masks/dump/<hash>_<w>x<h>_<vk format>.raw`. `tools/glass_mask.py decode` turns it into a
  PNG, and `pgm` turns a painted PNG into the mask. Any rt.cfg change rescans; an edited mask file
  is uploaded again.
- **The mask** (`data/glass_masks`, copy into `rt-live/glass_masks`) has 10 bottle-body panels of
  atlas 9ba439fe72338d96. The streaming low-res copy 69623b0bcd1a30f5 gets the same file. Atlas
  pixel boxes: 620,190-820,352; 822,186-1022,348; 846,358-1022,472; 0,512-284,742;
  370,403-672,618; 0,742-206,904; 207,742-407,904; 536,743-822,988; 488,643-664,730;
  0,425-120,507. In each box only the largest non-background part counts. Rows darker than 0.6x
  the panel median are the liquid band and stay opaque. Then a 3 px erode. 29% of the atlas is
  glass.
- **Found by probe:** with `BB_RT_GLASS_DEBUG=1`, masking one atlas box at a time and counting
  magenta pixels showed which boxes cover the shelf jugs and floor bottles. The top-left strips
  are the jug necks and handles, and the octagons are the bases: they stay opaque.
- **Live:** the big jars show the shelf posts behind them, and light passes through the bottles
  onto the wall. Necks, caps and the liquid stay solid. 61 -> 60 fps.
- **Not shipped yet:** the launcher sets no `BB_RT_CFG`, so a deployed build needs the mask folder
  and a path to it.

### Altar cloth was taken for glass (2026-11-09)

The red cloth on the workshop altar looked washed out and see-through. A/B: unchanged without
the bottle mask and with `BB_RT_GLASS_PASS=0`, right with `BB_RT_GLASS=0` or with
`BB_RT_GLASS_PS=ca7226ec`. Pick: a 110-triangle alpha-tested mesh of ps 1c3953c6 (the shader of
the cracked door panes, 28-48 triangles), gloss 0.88, so the reflection pass accepted it as a pane.
`BB_RT_GLASS_PS` entries now take their own triangle cap (`hash:cap`); the default is
`1c3953c6:64,ca7226ec`. Live: the cloth matches glass-off, the pick reports "rejected".

### Workshop mirror was taken for glass (2026-11-09)

The standing mirror in the workshop turned into clear glass. Pick: ps ca7226ec, 8 triangles,
gloss 0.98, with its solid backing board (bb10e440) 4.1 cm behind. `GlassFind` (rt_gi.comp) now
traces on past an accepted pane: an opaque hit from **another draw** within 5 cm behind it (the
gap is measured along the pane normal, so a slanted view still catches it) makes it a backed
mirror, not glass. The bottles' own liquid (the same draw, outside the mask) does not count.
Small `BB_RT_GLASS_PS` draws can also carry a texture mask now (`GlassMaskSlots` no longer skips
draws under the triangle cap). Live with `BB_RT_GLASS_DEBUG=1`: the mirror is no longer magenta and
reflects the room again; the wardrobe panes and the bottles are still glass.

The mirror then went through the plain reflection path: the game's low spec colour as F0, so the
stock smudge texture dominated and it read as a dull diffuse board. A backed pane is now shaded
as silvered glass: roughness 0, F0 0.7 (`MirrorF0`, an aged silver mirror; fresh is ~0.9), and
the stock base is cut to the 30% the silvering leaves, which keeps the stock cracks over the
reflection. Live: the player and the room show sharp in it; stock shows a near-black pane.

## Messengers' bath lit again; magic may keep its light (2026-11-09)

User: the messengers in the Hunter's Dream bath were too dark (stock: pale, self-lit). Their light
is a cool-white 2 m point light over the bath (`LegacyDPointLight*` at world (-7.87 0.53 -4.35),
colour 0.63-1.20 x (1, 1, 1.32), breathing on a ~1 s cycle). The pulse latched it as an emitter,
but since 2c93dda ("loot is not fire") only flame-coloured lights count as emitters, so the fill
gate faded it to 0. A light whose colour pulses (`BB_RT_FILL_FLICKER`, 6 reversals) now latches
`pulse`, and a pulsing light that is not a flame keeps the game's light as is, like the loot
wisp light. Flames still take the emitter falloff.

New user rule (render-look skill): light from something magical and not real (this bath, arcane
and insight glows) may stay "fake", with no physical fixture. Live (view25): `flickers ... emitter`
at frame 3993, the light runs at the game's colour, the messengers and the flowers around the
bath are lit pale blue-white as in stock.

## A brighter fire reaches further (2026-11-09, `BB_RT_EMITTER_BRIGHT`)

User, at the burning Hunter's Dream house: "its a giant firing thing, but the surrounds doesnt get
the light". The house fires are game lights of r 7.5 with colour up to 12.8 (a lantern is 2.0),
and the emitter reach was `BB_RT_EMITTER_RANGE` 1.26 x the game radius whatever the intensity:
9.5 m, cut while still bright. With inverse-square light and one cut-off level the reach grows
with sqrt(intensity), so an emitter (not a lamp, not a candle) now gets reach x
sqrt(max channel / `BB_RT_EMITTER_BRIGHT`), default 2.0 (0 = off). Such a light (`Light::wide`)
also skips the stock depth-bounds test, which would clip it back to the game's sphere, as lamps do.
Live (view26): house fires reach 22.8-25.6 m; lanterns/torches (colour <= 2) unchanged. A/B on, off,
on at the burning side wall: lower half mean 67.2 / 50.2 / 67.4, red minus blue 44 / 15 / 44: the
flowers, ground and player take the fire's light, the gravestone ~10 m out too. 68 -> 64 FPS.

### The big fire dances, a bit brighter (2026-11-09)

User: "the fire is good, but theres not woobling why, its a giant fire", then "maybe a little more
bright from the fire". The game keeps the burning house's lights constant (6.4 / 12.8 every
frame), so their only motion was the steady-flame noise sized for candles (0.25-1.6 Hz swells)
and the fixed 1.5 cm sway. For an emitter brighter than a lantern (grow = sqrt(colour /
`BB_RT_EMITTER_BRIGHT`) > 1, computed once from the game colour, also used for the reach):
- sway `BB_RT_FIXED_SWAY` + `BB_RT_EMITTER_SWAY` x (grow - 1): house ~0.25 m, candles unchanged;
- steady flicker x grow^`BB_RT_EMITTER_BIG_FLICKER` (0.5), plus a faster octave of the same noise
  at 3x the rate (~0.75-5 Hz) weighted `BB_RT_EMITTER_BIG_FAST` (0.5);
- brightness x `BB_RT_EMITTER_BIG_GAIN` (1.5). All three are live knobs.
Live (view28), interleaved old (all off) / new x2 from the per-frame light log of the three house
fires: frame-to-frame change rms 1.26 / 2.96 / 1.18 / 3.13 %, mean lum 28.7 / 45.2 / 29.0 / 43.8.
Screen measures near the player (15 m out) stayed inside the adaptation drift. The sway amplitude
is not logged (the dlist prints the track position before the sway).

## Warm rule takes the lamp hue only (2026-11-10, `BB_RT_FILL_WARM_HUE`)

The grave enclosure under the big tree in the burning Hunter's Dream had a warm light on the
player and the flowers with no source in sight. It was `LegacyDPointLight` at world 11.02
-4.93 -19.38, game colour `2.000 1.243 0.352`, radius 5: emissive 0%, enclosed 0%, no
flicker, nearest solid 0.67-2.05 m. Only the warm rule (`Rt::WarmEmitter`, "Warm lights are
lamps" above) kept it. Sky fraction (0.39-0.88) and nearest solid both moved with what the TLAS
held, so neither split it from the sewer lamps. The colour does: the Dream's designer key
fills are a saturated amber (`2.000 1.243 0.352`, `2.000 1.110 0.603`: blue 0.18 and 0.30 x
red), the mirror of its blue fills (`0.586 1.243 2.000`, `0.692 1.243 2.000`). The sewer lamps
the rule is for are a pale warm (`2.000 1.356 0.997`, `1.000 0.678 0.498`: blue 0.50 x red).

The red >= 1 warm test now also needs blue >= 0.4 x red (`Rt::WarmLampBlue`), unless red > 3
(`Rt::WarmFirePeak`): the burning house (`12.800 5.039 0.806`) and its `6.400 2.520 0.403`
lights are fires whatever the hue. The lantern hue test is unchanged; flames still latch by
flicker. Live knob `BB_RT_FILL_WARM_HUE` (0 = old rule). The latch is sticky, so an A/B goes
new rule first. At the enclosure: player luma 47.6 / flowers 43.3 with the rule (moonlit, white
flowers), 59.9 / 46.0 with `BB_RT_FILL_WARM_HUE=0` (amber wash).

## Puddles: real water F0, soft rims (2026-11-10, `BB_RT_PUDDLE_EDGE`)

The Hunter's Dream path is G-buffer water (`BB_RT_WATER_DEBUG=1`: cyan), where stock draws
only wet mud. RT made every pixel with gloss >= 0.6 (`WaterPuddleGloss`) a mirror at
`BB_RT_WATER_F0` 0.5. Seen from above it mirrored half the bright sky, and the hard gloss cut
left jagged islands, so the puddles read as white blobs. Two changes:

- `BB_RT_WATER_F0` default 0.5 -> 0.02, real water. The 0.5 was picked for the dark sewer,
  where only the glint showed at 0.02; that water is now sheet water with its own
  `BB_RT_WATER_SHEET_F0`. From above a puddle is now dark wet ground; at grazing angles it mirrors
  the sky, as real puddles do.
- The puddle fades in over the gloss range [0.6, 0.6 + `BB_RT_PUDDLE_EDGE`] (default 0.25, live,
  0 = the old hard cut). The weight scales the mirror and the Fresnel share taken from the floor
  under it, so the rim blends into dry mud.

Live at the Dream path (puddle region mean luma): old 108.5, soft rim only 79.8, both 59.6.

## Puddles: wet bed, clear water (2026-11-10, `BB_RT_PUDDLE_WET`)

After the F0 fix the puddles still read foggy: the faint sky reflection lay over a bed lit as
bright as dry mud, a milky film (PHOTOREAL 13: wet ground is 0.5-0.8x its dry albedo). The bed
under a G-buffer puddle now keeps `BB_RT_PUDDLE_WET` (default 0.6, live, 1 = off) of its light,
weighted by the soft-rim weight so the rim blends. Sheet water is unchanged.

Live at the Dream path, interleaved off/on/off/on, puddle crop: p5 39/25/32/26, sd 36/44/39/45,
Laplacian sharpness 162/204/185/180. The bed reads dark and the sky as a reflection on it.
Ripple/rough to 0 were tried: no visible gain, the soft clouds are the sky itself.

## Moon haze: the dome's fog kept off the disc (2026-11-13, `rt/rt_moon_haze.h`)

From one side of Hemwick the moon was a flat grey disc (centre luma 54, contrast 1.26 to the sky;
the other side 148, 2.6). The haze is the game's own fog in the sky dome pixel shader `dda49e89`
(distance and height fog both mix the moon toward the fog colour), not our tracer and not the
cloud layer `765450df`. `Rt::MoonHazeFix` rewrites the dome's SPIR-V when the recompiler emits it,
so both fog factors are scaled by `1 - cut * moon`,
`moon = smoothstep(0.1, 0.2, min channel of the unfogged colour)`: only the disc reaches that range
in this shader (disc 0.25-1, horizon band about 0.02). Taking all the fog off clipped the disc to
white (247/250); 0.5 still clipped (239); cut 0.09 gives centre 175, max 215, contrast 3.8, maria
kept. The factors are found by shape (NClamp(x, 0, 1) feeding three FMul -> FAdd); anything else
leaves the shader as it is. The pass edits only the shader the player's own game produced, so no
game data ships with the port.

Knob `BB_RT_MOON_HAZE` (default 0.09, 0 = off), read once at boot. The log prints
`Moon haze: dome shader patched` (or `not matched, left as it is`).

`PipelineCache::GetSourcePatch` runs it on compile and on the cache preload, without
`BB_PATCH_SHADERS`. Dev override: a `<stage>_<hash>_src<FNV-1a 64 of the recompiler's SPIR-V>.spv`
file in `<user>/shader/patch` replaces that exact permutation's SPIR-V first.

## Moon penumbra: 1 extra ray instead of 2 (2026-11-13, `rt_direct.comp`)

`MoonPenumbraRays` 2 -> 1. The one ray takes the cone cell opposite the pixel's `MoonConeU` cell
(other ring, sector + 2), so a 4x4 block still covers all 8 cells twice and the 5x5 average stays
stratified. Hemwick, still camera, interleaved 2/1/2/1/2: direct 10.9-11.1 -> 9.7 ms; temporal
noise at the player's shadow edge p90 1.12-1.28 vs 1.17-1.47, wall shadow 0.76-0.80 vs 0.79-0.80,
speckle the same; zoomed edges look the same.

## Blood moon: screen moon search over warm samples (2026-11-13, `rt_sky.comp`)

Cathedral Ward under the blood moon: moonlight came in 5 degrees over the horizon and 25 degrees
to the side of the disc (long diagonal shadows, `BB_RT_DIRECT_DEBUG=11`). `FindScreenMoon` skips
warm sky samples (red > 2x blue) so fire over the sky is never the moon, and the blood moon (r/b
~5 on screen, as fire) was skipped with it; the search took pale clouds instead. A search over all
samples landed 7 degrees off: the red disc's luma is no higher than the lit clouds around it.

Now the search runs first over the warm samples alone (bright cut 0.5x the reference, samples
unweighted) and takes that result only when it is a big compact disc high in the sky: at least
150 samples in the cone, spread at most 10 degrees, centre over 10 degrees up. Otherwise it runs
again over the other samples, as before. Fire over the sky (2 samples in the Hunter's Dream) stays
out. Live, synced to `BB_RT_MOON_SCREEN_LOG=1`: the light lands on the disc's centre within 0.004
uv across and 1.8 degrees up (disc radius ~7 degrees); 1700-1800 samples, spread 7.3-7.4. Not yet
rechecked: the burning Hunter's Dream (fire with the pale moon).

Follow-up, same day (`rt_gi.comp` `FindLayerMoon`): the light still swung as the player walked.
With the disc out of view the search over pale samples took clouds 3-10 degrees up on any side
(one behind the camera, `-0.44 0.17 0.88`), and after `MoonUnlock` searches they replaced the moon.
A red disc's find now carries `ScreenMoonWarmTag` (0.5 in w); the first one of a run replaces any
kept pale moon at once and sets a flag in layer row 0 cell 3 (reset on load), after which only red
finds count. Live: 4 legs of walking and turning over 11 m, light `0.794 0.555 -0.247` throughout
(was jumping up to ~100 degrees); the moon cache entries of this sky's cloud draws moved to the disc.

## One moon rule for every world: a round mesh in front of the sky (2026-11-13, `rt_sky.comp`, `rt_gi.comp`)

The red-only lock above broke the Hunter's Dream: its lit red clouds passed as "red discs" and the
lock then dropped every pale moon find; the light sat 15 degrees off the moon. Every rule on the
picture failed one world: colour (above), the brightest patch (lit clouds, fire), and a level fall
at the disc's edge (the blood moon's shaded side and bright top at one threshold; the Dream moon's
soft limb under thin cloud, edge contrast 1.8-2.5 against 1.1-2.4 for cloud patches, at the other:
found in 1 search of 3, never enough to replace a kept light).

The moon is a mesh of its own and it writes depth, so `FindScreenMoon` now reads the depth buffer
and no colour. What the two skies look like there:

| | Hunter's Dream (pale moon) | Cathedral Ward (blood moon) |
|---|---|---|
| Disc | flat, 748-750 m | bulged and tilted, 556-570 m |
| Sky behind it | the dome, 1737 m (2.2x) | 565-684 m, uneven (1.0-1.2x); a cloud sheet meets the rim at the lower left |
| Other depth edges in the sky | far pillars (773 m), cloud sheets | rounded cloud sheets, an unlit round mesh at 672-680 m |

The search (one rule, both worlds):

- Candidates. (1) Rim samples: the sky one grid step to both sides along x or y is farther by
  0.4% of the range in sum. Each votes for the cells its centre may be in (inward along the step,
  radius 3-17 rings); the 8 best cells are voted on again per radius, and the 2 with rim samples
  from the most of 8 ways at one radius are tested. (2) Front samples: the sky 14 degrees of arc to
  either side at the same height is 1.25x farther; the screen cell with the most is tested first.
  The votes alone found the Dream moon in 1 search of 3 to 8 (pillars, cloud edges); the front
  samples alone found the blood moon now and then (its sky is 1.0-1.2x farther).
- The disc test, per candidate (`NearRange`, `DiscProfile` twice, from the candidate and from the
  fitted centre): the disc is the sky within 3% of the range at the centre; nearer sky hides it,
  as a branch does. In 8 directions the rim is where the share of samples on the disc falls under
  half. A circle `r(a) = R + d.(cos a, sin a)` comes from every 3 rims (exact solve), the one
  most rims agree with (within 1.25 rings of 0.75 degrees) wins, then least squares over those.
- A find: at least 5 of 8 rims on the circle, radius 3-17 rings, and in those directions at least
  85% of the sky inside the circle on the disc and at most 20% of the 3 rings past it; and the
  disc is a light: its luminance (geometric mean) is 1.7x the sky's in those 3 rings, or 1.25x
  when it is also 1.8x the whole sky's on screen (the Dream moon's halo fills the rings).
  Without the fill a far pillar's stub passed in the Dream (6 rims, 43% inside); without the
  glow the unlit round mesh and a rounded cloud sheet passed under the blood moon (0.4-1.5).
- The find carries the disc's range. `FindLayerMoon` (`rt_gi.comp`) places the moon in the world
  from it at once (camera + range x direction): the light points at that place from wherever the
  player walks, and one look is enough. It was two sightings 10 m of walking apart before.

Why the light went wrong without a look at the sky, and what changed (`rt_gi.comp`):

- The CPU's reset (the layer's draws came back after a second away: a load, but also a look at
  the ground or a walk under a roof) dropped the found moon; the light went back to the cached
  direction of the cloud draws, which maps share (Cathedral Ward: 30 degrees off) until the player
  looked at the disc again. Now a moon placed in the world this run is only dropped by a camera
  jump of 100 m between two searches (a warp, a load).
- A sky with no layer draws (Cathedral Ward under the blood moon) lost the moon 150 searches
  after the disc left the screen. A placed moon is now kept.
- `MoonUnlock` is 12 searches (4 frames apart: under a second); it was 40 (~3 s). The stray lock
  itself stays: a spot off the kept moon replaces it only when seen 12 times, less 2 per search
  with no disc.

The moon cache (`rt_moon.txt`, per layer draw) now gets exact places. Before, a screen find was
cached 2000 m out along its direction until two sightings a walk apart gave a distance: the
Dream's moon sat at `-611 814 1677` (it is at `-204 297 614`, 748 m), and from another spot of
the map the light started up to 30 degrees off. A wrong old entry is outvoted after 8 confirmed
finds (`MoonCache::SwitchVotes`). Live: second arrival in Cathedral Ward "found 0.18 degrees from
its cached position"; after a cold start facing away from the moon the light was on it from the
first frame (`0.833 0.481 -0.275`, w 149), and 4 s of looking at the ground did not move it.

`vk_ray_tracing.cpp` (eb0143e): a find is cached while the layer
draws even when no draw was in every frame of the run (`SkyKey::Key()` 0, logged as sky
`0000000000000001`); Cathedral Ward's first finds were dropped for that ("sky 0000000000000000
found 30.63 degrees from its cached position"). Built and running; the case itself did not come
up again live.

Still needed: the first look ever at a sky's moon (the game draws the moon's mesh only while it
is in view). A cache file filled in every area and shipped with the installer would cover it.

Live, `BB_RT_MOON_SCREEN_LOG=1` (its "peak / mean / spread / bright samples" are now: rims if it
passed, rims*1e6 + radius*1e4 + ring glow*100, the glow over the whole sky, the disc's range in m):

| | Hunter's Dream | Cathedral Ward |
|---|---|---|
| Find, moon in view | 8 of 8 rims, 8.0 rings, 748 m, glow 1.5-1.6 (rings) and 2.3-2.9 (sky), every search | 6-7 of 8 rims, 5.3-5.5 rings, 558 m, glow 1.7-2.4 (rings) and 3.1-3.6 (sky), every search |
| Light | `-0.326 0.416 0.849`, on the disc; right on arrival (cache) | `0.840 0.478 -0.257`, on the disc; was `0.593 0.384 -0.707` (30 degrees off) on the first visit, right on arrival on the second (cache) |
| Sweeps of 360 degrees | 24 views, no false find | 34 views, no false find (the unlit mesh: 6 rims, glow 0.9 and 0.8) |
| Walking, looking away | 6 legs, light on the moon's place | 2 x 6 legs, light on the moon's place |

Cost, same view, interleaved 3 x 3 readings of the FPS counter: the committed colour searches 61.3 FPS, this 64.7 (Hunter's Dream, moon out of view). The search runs when the layer search has taken the last result (every 4th frame); every frame it cost 0.64 ms (59.9 against 62.3).

## A third moon: Upper Cathedral Ward, and what it changed in the search (2026-11-13, `rt_sky.comp`, `rt_gi.comp`, `vk_ray_tracing.cpp`)

Upper Cathedral Ward's moon lit nothing: the light stayed on the game's own direction ("moon
light from the game's sets"). Three causes.

- The sky pass (`RecordSky`) only ran while the game's dome draw (`SkyDomePs`, a 4:1 panorama) was
  live, and that map's sky is another shader's: no dome, so no moon search at all. Now the moon
  search runs alone when there is no dome (the depth view stands in for the dome texture).
- That moon is a mesh 21 degrees wide at 450 m, its centre 2.7 degrees up and its lower half
  behind the ruins; nothing is behind it (depth 1), flat cloud sheets with straight edges sit
  above. The disc test needed 5 of 8 rims and a disc within 3% of its centre's range.
- With only the top half in view the first-order circle `r(a) = R + d.(cos a, sin a)` put the
  centre a degree low.

The search after it (still depth only for the shape, luminance only to say "this is a light"):

- Candidates from the votes are ranked by the luminance of their rim samples (the 2 brightest
  with rims from 4 of 8 ways are tested); by ways and samples the cloud sheets' corners won
  every time. The front cell's candidate is still tested first.
- The disc's range runs from its centre's to its rim samples' (that mesh is over 3% farther at
  the rim: about the centre alone the disc came out 8.6 rings wide, it is 14.3). When something
  hides the centre the rim samples' range is used.
- The circle is exact: `|q|^2 = 2 c.q + k` through the rim points, 3 at a time, then least
  squares.
- A half disc passes with 4 rims on the circle when at most one other direction has a rim and
  the disc is 8x the whole sky's luminance (that moon: 27-75).
- Where the sky behind the disc is at the disc's own range (Cathedral Ward on the second visit:
  a cloud sheet at 600-617 m behind the whole blood moon at 594-597 m; the disc came out 15-19
  rings wide and was dropped) a direction's rim is the first ring with rim samples (depth steps
  facing away from the centre), when that comes 1.5 rings or more before the range's fall. Such
  directions leave the test of the rings past the disc, and the disc must then pass the full
  glow (1.7).
- The glow is now the luminance inside the circle over the 3 rings past it, whatever the range
  there. The halo rule asks 1.55 (was 1.25): a moonlit round cloud in the Hunter's Dream
  (1320-1345 m, 6 rims, 1.42-1.46, 3.2-4.0 over the sky) passed at 1.25 and, seen for a second,
  took the light.
- `FindLayerMoon` keeps the glow of the kept moon and of the stray spot it is counting (row 0
  cell 5): a stray with less glow never replaces the kept moon, one with more does after
  `MoonUnlock` sightings. Found first, the cloud still gives way to the moon.
- A later find moves the placed moon a twentieth of the way (was a quarter): finds of one disc
  scatter 0.4-0.7 degrees with the clouds behind it.

Live, the same shader in the three maps (`BB_RT_MOON_SCREEN_LOG=1`; "peak" is now the ring glow
of the disc that passed):

| | Hunter's Dream | Cathedral Ward | Upper Cathedral Ward |
|---|---|---|---|
| Find, moon in clear view | 6-8 rims, 8.1 rings, 732 m, glow 2.0-2.5 | 5-7 rims, 6.0-7.1 rings, 593-598 m, glow 2.4-4.2 | 4 rims, 14.3-15.1 rings, 440-451 m, glow 75 |
| Light | on the disc, right on arrival (0.55 degrees from the cache) | on the disc, right on arrival (0.27-0.43 degrees from the cache) | on the disc: centre 2.5 degrees up, top 13.4 (the picture: 13.5); was the game's direction |
| Sweeps of 360 degrees | 14 views, no false find | 3 x 12 views, no false find | 2 x 10 views, no false find |
| Walking | light on the moon's place | 6 legs, light on the moon's place | 2 x 6 legs, light on the moon's place |

Not found, light kept: the blood moon with the hunter or a wall over part of it, Upper Cathedral
Ward's moon in about half the views (cloud corners take the 3 tries). Cost: 56.2 FPS before and after (Cathedral Ward, interleaved 3 x 3 readings).

## The first disc must be seen three times (2026-11-13, `rt_gi.comp`)

Nightmare Frontier, on arrival: one stray find (`0.799 0.150 -0.583`, never seen again) was taken
as the moon at once, and since a placed moon is kept until the next warp the light stayed behind
the rocks: the whole map in its shadow. `FindLayerMoon` now counts the finds of the first disc of
a run at one spot (within 3 degrees; row 0 cell 3, w 10 + the count) and replaces the kept
direction (the game's or the cache's) at the third (`MoonFirst`); a find elsewhere restarts the
count. The cells' tag went from 2 to 3, and a search with no tagged camera resets, so a session
never starts from another layout's cells.

Live: the stray moon was dropped on the swap (back to the game's sets). That sky does hold a
light: a veiled sun, a disc of 5 rings at 1010 m, 34 degrees up, 6 rims, glow 1.59 and 1.9 over the
sky. It was confirmed and placed (`-0.069 0.559 0.826`) and cached; after a cold start facing away
the light was on it from frame 2776.

Open, not a moon matter: Nightmare Frontier is far darker than stock where the view is against
the light (RT p5 4, p20 5, p50 10, 13.7% of the pixels under 5, p90/p20 23; stock p5 10, p20 17,
p50 60, 0.4%, 5.7). `BB_RT_GI_FILL_SKY=4` changed nothing; `BB_RT_GI_STRENGTH=0` took p20 from 5 to
3. `BB_RT_PICK` on three rock pixels facing the sun (n.L 0.33-0.86) from the player's own view:
moon visibility 0, the blocker a back face of a rock mesh 0.05, 0.46 and 3.71 m along the ray (ps
`dbb222eb`, `aa2a9925`, `6b45aeff`). The rocks there overlap each other, or the traced meshes sit
off the drawn ones; not looked into yet.

## No visible moon or sun: the game's direction stays (2026-11-13, `rt_sky.comp`, `rt_gi.comp`)

The user's rule: with no moon or sun to be seen in the sky, light 1 keeps the stock game's
direction. The Nightmare Frontier broke it twice after the first-disc fix above.

- Its sun is a disc at 1010 m behind the haze: only a glow in the clouds, ring glow 1.58-1.59, 1.9
  over the whole sky. The halo rule (1.55 with 1.8 over the sky) took it as the moon.
- A far rock (759 m, sky by the dome-range test; 6 rims, ring glow 1.97, 0.4 over the whole sky:
  darker than the sky, the haze around it darker still) passed on the rings alone. With more ring
  glow than the sun it replaced it ("a disc with less glow never replaces the kept one" worked
  against us), the cache took its place (`556 1623 -555`), and the light came from the rocks, 9
  degrees up: the map dark, the lighting moving as the finds went back and forth.

Now one glow rule: the luminance inside the circle is 1.8x the sky's in the 3 rings past it
(`ScreenMoonGlow`; the halo rule and `md_edge` are gone) and at least 1.3x the whole sky's
(`ScreenMoonSkyFloor`). In clear view the Dream moon has 2.0-2.5, the blood moon 2.4-4.2, Upper
Cathedral Ward's 10+; the Dream moon at the screen edge or behind branches (1.6) is no longer
confirmed there, the light keeps its place. In `FindLayerMoon` the kept moon's glow fades 3% per
search while it sits within 15 degrees of the view's axis and is not found, so a kept disc that
is one no longer gives way to the next.

Live, Nightmare Frontier: the two cache lines of the rock removed by hand; after a cold start
"moon light from the game's sets", 12 views of a sweep with the sky and the sun's glow in them,
no find; the open ground lit as stock lights it. Not run again after this change: the Hunter's
Dream, Cathedral Ward and Upper Cathedral Ward (their glows above are from the runs before it).

The darkness noted in the section above was mostly this (the light from the rocks, then from the
veiled sun behind the cliffs); from the game's direction the same map is lit. The `BB_RT_PICK`
blockers on back faces were read with those lights and are not checked again.

## Sky flicker under the fog pass, Nightmare Frontier: fixed (2026-11-14)

Not the moon code. `tools/live_vision.py measure` on the sky of one still view: flicker sd 12-16%,
peak to peak ~50%, with RT; 0.1% stock; the same with `BB_RT_MOON_FROM_SKY=0` and with the shaders
of 49f0157; 0.0-0.1% with `BB_RT_FOG_STOCK=1`. The fog there is thick (extinction 0.0034/m,
transmittance 0.07 over the 1115 m to the sky), so a sky pixel is 93% in-scatter, and
`BB_RT_PICK` on one sky pixel, every frame, has the march integral at 0.19, 0.20 or 0.35 and the
sky part between 0.006 and 0.157. Why the 0.9 history does not even that out is not found
(`BB_RT_FX_FOG_DEBUG=5` shows the history lost over the geometry in some frames).

Until it is fixed the live `rt.cfg` carries `BB_RT_FOG_STOCK=1` (the user's choice): the game's
own fog in every map.

**Fix (2026-11-14): the traced fog runs right after `GXLightComposite`.** `OnAfterComposite`
now records the fog after the GI (`RecordFog`), at the same point where the stock composite
applies its fog; the post trigger only runs it on frames this missed (`fog_ran_frame`).
`BB_RT_FX_FOG_EARLY=0` (live) goes back to the post trigger for A/B.

How it was found (fork-v1.9.8.6, FSR 3.1, one still view of the Nightmare Frontier sky):

- Per-frame luma of the sky: steady 124 with single-frame flashes to 174 on about 1 frame in 10;
  stock a steady 100. `BB_RT_FX_FOG_DEBUG` views showed the fog opacity of those pixels at 0 in
  those frames: the pixel's set (`G-buffer3.a * 255`) pointed at a blank set
  (`1 0 0 0.01 | 0 0 0 0 | 1 0 0 0.01 | 1 0 0 1506 | 0 ...`, transmittance 1).
- Not the cause: rounding the id (`+ 0.5`), the sky pixels' own id instead of the walk down,
  the sky skipped, the zero-fill of the game's table (kept the table: still blank sets), a second
  composite per frame (none logged), the dome flag, the 60 km sky clamp.
- At the post trigger the transparents and particles have drawn after the composite, so the set
  ids and the table no longer match what the composite used. Run at the composite, they do.

Measured with `knob_ab`, interleaved, step = mean frame-to-frame change in luma:

| Region | post trigger (old) | after composite (new) | stock |
|---|---|---|---|
| sky | 0.69-0.86 | 0.05-0.09 | 0.06-0.08 |
| cliff | 0.37-0.45 | 0.06-0.13 | 0.07-0.09 |
| mid | 0.96-1.21 | 0.07-0.10 | 0.10-0.13 |

The flashes come and go between sessions (one session showed none at the post trigger); in 4
sessions the new path never showed them. Noise (`pixel_noise.py`, per-pixel temporal sd p90):
sky 0.32 new vs 0.70 stock, cliff 0.72-0.78 vs 0.73-0.75, ground 0.78-0.81 vs 0.65-0.69, with the
4x4 tent upsample, history 0.05 and the 500 m depth test (`TestDepth`) kept from 2026-11-14.

## Sky too bright under the traced fog: the eye adaptation boosted the dome (fixed 2026-11-14)

The traced-fog sky read 237-243 luma (clipped) against stock 120. It was not the fog: with
`BB_RT_EXPOSURE=0` the traced sky is 118 vs stock 119. `rt_exposure.comp` skipped "the sky" by
depth 1 only, but the game's dome is a mesh that writes depth (539-1480 m, see rt_sky.comp), so the
boost reached it in every mode (stock fog 158 vs stock 120). The traced fog leaves far geometry
darker than the stock fog (away from the moon and in shadow, far cliff 33-35 vs 40-45), so the
metered log2 luminance fell (-6.8 vs -6.0), the boost rose (+1.25 vs +0.49 EV) and the dome clipped.

Fix: the exposure push carries the projection and the view-space up vector, and the boost skips a
pixel at 30% of the dome's distance or more, the fog pass's `IsDome` rule (90% left the dome's flat
facets as bright rectangles). The metering is unchanged, so `BB_RT_EXPOSURE_KEY` keeps its tuning.

| Nightmare Frontier, same view | sky | ground | far cliff |
|---|---|---|---|
| stock (F9) | 122-123 | 69-71 | 53-61 |
| RT, stock fog, before | 157-160 | 42-44 | 40-45 |
| RT, traced fog, before | 237-243 | 54-57 | 33-35 |
| RT, stock fog, after | 121 | 43-44 | 40-45 |
| RT, traced fog, after | 120 | 59-60 | 33-35 |

Tooling note: `knob_ab` sets separate knobs with `;`. A comma made the rest part of the first value.

## F9 off costs nothing again (2026-11-14, `vk_rasterizer.cpp`, `vk_ray_tracing.cpp`)

With the effects paused (F9 or `BB_RT_PAUSE=1`) the engine still did most of the RT work:
per-draw capture (geometry, bones, tags, alpha test), a TLAS rebuild of the 618 kept
off-screen instances every frame, and a slot hold that made BeginSlot wait for the GPU every
third frame. Now `RayTracing::Capturing()` (enabled and not paused) gates the capture, the
build is skipped while paused, and a paused frame holds no slot.

Nightmare Frontier start spot, FSR 3.1, in-game FPS overlay read by hand:

| Mode | Before | After |
|---|---|---|
| `BB_RT=0` (stock path) | 120 | 120 |
| RT on | ~60 | ~60 |
| RT paused (F9) | 62 | 120 |

Unpausing brings the lighting back on the next frame; hitch log 0 hitch frames, worst wait 0 ms.

F9 does nothing when path tracing is off in the launcher (`BB_RT=0`): `Rt::ToggleEffects()`
returns before touching the pause flag (`rt_toggle.h`).

## Traced mist glow flicker: the reflect pass read last frame's fog table (fixed 2026-11-14)

With `BB_RT_MIST_FOG` on, the traced mist over Nightmare Frontier flashed whole spheres in and
out every frame. The mist itself was steady: with a constant glow colour in the shader the frame
step fell from 8.7 to 0.5 luma. The glow colour is the pixel's fog set (`G-buffer3.a`) looked up
in the game's fog sets table, and the reflect pass (where the mist is drawn) runs at the light
pass, before the composite, so it read the table copied at the previous composite with this
frame's set ids. The table sits at one address (`0x105017e330`, 21600 bytes) and the game has
already written it for this frame at the light pass, so `CopyFogTableEarly` copies it again
before the reflect pass. It still works with the traced fog on (`BB_RT_FX_FOG=1`, which zeroes
the game's copy after the composite).

The glow now takes only the set colour's luma, the colour comes from the haze particles: the set
colour is cyan and made the mist bluer than stock (mist region blue minus red +24, stock +13,
now +15).

Nightmare Frontier start, `BB_RT_MIST=0.02`, frame-to-frame step in luma (`knob_ab`, interleaved):

| Region | glow off | glow on, before | glow on, after | stock |
|---|---|---|---|---|
| far cliff mist | 0.17-0.37 | 8.7-15.1 | 0.29-0.34 | 0.29-0.38 |
| mid mist | 0.22-0.36 | 6.7-13.5 | 0.32-0.39 | 0.27-0.38 |

At the valley view past the stone gate this version overshot stock (valley luma: stock 107,
`BB_RT_MIST=0` 101-102, traced mist glow off 127, `BB_RT_MIST_FOG=0.3` 150), while at the start
view it sat below stock (0.85x): the mist's level followed the particle count and size.

## Traced mist on by default: stock card opacity, sky-lit glow (2026-11-14, user-approved)

The user reported the mist overbright and asked for the glow to come from real light. Two
changes in `MistVolume` (`rt_gi.comp`):

- **Opacity per sphere = the game's card.** Each haze sphere's density is now
  `-ln(1 - min(alpha, 0.95)) x 15/16 / r`: through its centre the sphere is exactly as opaque as
  the game's card for that particle (the chord integral of the falloff is 16/15 r). Before, the
  density did not depend on the radius, so big far particles outweighed small near ones. With the
  spheres (`BB_RT_MIST_SHEETS` on) `BB_RT_MIST` is now a multiplier on that opacity and 1 is
  physical; it no longer suits the old slab mode (`BB_RT_MIST_SHEETS=0`).
- **Glow = sky light scattered by the mist.** The glow is now the mean sky radiance over the
  upper hemisphere (zenith plus four directions 27 degrees up, `SkyRadiance`) x 1/2 (isotropic
  phase, sky only above) x albedo x `BB_RT_MIST_FOG`, instead of the fog set colour x a constant.
  The fog set colour no longer drives the glow (the early fog table copy stays for the fog
  lookups). `BB_RT_MIST_FOG=0` still falls back to the `BB_RT_MIST_GLOW` path.

New defaults: `BB_RT_MIST` 0 -> 1, `BB_RT_MIST_FOG` 5 -> 0.25 (user approved "Turn it on").
Luma, interleaved `knob_ab` (cold boot, built-in shaders, no mist knobs in rt.cfg):

| View, region | stock | `BB_RT_MIST=0` | traced mist (new defaults) |
|---|---|---|---|
| valley past the stone gate, cloud | 76-107 | 101-102 | 94-104 (glow 0.2) to 102-112 (0.3) |
| valley, lower | 60-77 | - | 69-87 (0.2) to 74-95 (0.3) |
| hillside path, haze on the rock face | 75-93 | 81-97 | 82-91 |
| same spot turned 180, moonlit far rock | 84-85 | 169-170 | 127-133 |

The mist now sits at stock level. On the moonlit far rock the RT frame is brighter than stock
from the moon itself, and the mist pulls it down, not up. No flicker: frame step 0.14-0.66 with
the mist, 0.23-0.31 stock. Cost: FPS overlay 59 at the hillside view (about 58-61 with the mist
off).

### Mist, not a veil (2026-11-14)

The user: the mist looked like a light veil, not a mist. At the hillside view the spheres covered
the whole cliff in one smooth cyan wash and hid the game's light shaft, while stock shows
separate soft puffs. Three changes in `rt_gi.comp`:

- `MistRadius` 0.5: a sphere is half the captured radius. The game's card shows a soft puff
  inside its quad, not a disc that fills it. At full radius neighbouring spheres merged into one
  slab.
- The noise is scaled to each sphere (`MistBlobCells` 2.5 cells per radius) at contrast 0.9
  (`MistSheetNoise`, was 0.5). The fixed 2 m grain averaged out over a 30 m sphere.
- Moonlight scattered by the mist: the pixel's moon set, one shadow ray from the mist's weighted
  centre, Henyey-Greenstein g 0.5. Mist in the moon's shadow stays dark, and mist above the
  cliff in the moon glows (checked with a temporary x10 boost).

Hillside view, cliff region luma (interleaved): traced mist 98-100, `BB_RT_MIST=0` 106-128, stock
121-133. The cliff texture shows through and the puffs rise beside the player. Frame step
0.08-0.36, no boiling over 2 s.

### Stock haze sprites lit by the path tracer (2026-11-14)

The user still found the traced mist bad ("a white block, not a mist") and asked for the game's
own sprites with volumetric light. The sphere mist is parked again and the stock haze sprites
(pixel shader 0x71c96598) are drawn with traced light:

- **Light per puff (`FogLight`, `rt_gi.comp`, sky layer phase 2).** Every 4 frames one thread per
  captured haze particle traces the moon: 4 shadow rays from points 0.4 r around the centre,
  weighted by the moon colour, plus the sky over 5 upward directions with a shadow ray each. The
  result is divided by the unshadowed value (0..1 per channel), copied to a host ring and read
  back a few frames later (`ReadFogLight`, 2 m cell hash, nearest particle, else the mean).
- **Sprite colour.** `HazeColor` scales each particle's colour word by
  `1 - BB_RT_HAZE_SHADE + BB_RT_HAZE_SHADE x light`, plus the nearby lamps. `BB_RT_HAZE_SHADE`
  is the share of the traced shadow applied; the rest stands for light scattered inside the bank.
- **Density stays stock.** With traced light the alpha is the game's own; the old alpha cut
  (`BB_RT_HAZE_FLOOR`, 0.3) is only used when `BB_RT_HAZE_RT=0`. With both cuts the plume was
  about 12% of stock and vanished.

Hillside start view, read-back values: 194-220 particles, light 0 to 1, mean 0.12-0.16, about 11%
over 0.5 (the plume sits in the cliff's shadow; its top catches the moon). Luma, interleaved
`knob_ab`:

| Region | old (`HAZE_RT=0`, floor 0.3) | traced light |
|---|---|---|
| plume | 72.0-72.3, flicker 3.7-4.2% | 79.1, flicker 2.1-2.4% |
| plume base | 48.7-54.0 | 51.5-52.3 |

`BB_RT_HAZE_SHADE` 1 turns shadowed puffs into dark smoke over the arch, 0.6 flattens the plume;
0.8 keeps a moonlit top and a dim base. FPS overlay 65-67 at that view (traced mist was about 59).

New defaults: `BB_RT_MIST` 1 -> 0 (the user asked for the stock sprites), `BB_RT_HAZE_RT` 1,
`BB_RT_HAZE_SHADE` 0.8. `BB_RT_HAZE_LOG=1` prints the read-back range every 120 frames.

## Light that changed with the camera: far casters seen once (2026-11-14, `BB_RT_MOON_REACH`)

User report: "the light is changing when turning the camera". Hillside start, view A = camera
recentred behind the player (R3). A fresh load shows view A lit (player luma ~115, ground ~68).
After one look about 90 degrees left and back it is dark (player ~62, ground ~28) and stays dark.
pt2.3 does the same.

Ruled out (no effect on the switch): the traced haze light, the sky dome/probe path (never live
here), the live screen sky lookup, exposure, and the moon direction (light 1 is the game's own
`0.584 0.242 -0.775`, 14 degrees up, in both states; the sky moon slot stayed empty).

`BB_RT_PICK` on the player, the ground and the wall in the dark state: moon visibility 0, the
blocker a far rock mesh (ps `6b45aeff`) 74-85 m along the ray, mode "kept off-screen instance".
The TLAS holds only what the game has drawn: the rocks join it when the camera looks at them and
are then kept (`BB_RT_KEEP_UNUSED`), so the same view is lit before and shadowed after.

`BB_RT_MOON_REACH` (live, metres, 0 = 10 km as before) caps the moon shadow ray in the direct
pass (centre, cone, thin-card paths), GI bounce moon, the haze light and the traced mist. It
rides in `far_fade.x` of the direct and GI params (the far fade reads `.yzw` only).

| view A, same run | player | ground | left wall |
|---|---|---|---|
| fresh load | 113-118 | 68-69 | 108-110 |
| after the look left, reach 0 | 61-67 | 27-29 | 65-67 |
| after the look left, reach 60 | 113-116 | 68-69 | 106-109 |
| reach 40 | 115.5 | 67.9 | 107.6 |

Reach 60 held through 3 look-aways each way (player 111-117). Default still 0: a cap also drops
real long shadows (a 20 m cliff under a 14 degree moon throws 80 m), so the default waits for the
user and a second area.

## Haze light as a blended field (2026-11-14)

User report: the RT-lit stock mist flickers, "like our mist is slow frame". Each puff took the
light of the nearest read-back puff (2 m cells). The read-back is a few frames old, refreshes
every `SkyLayerEvery` (4) frames, and the per-puff values are near on/off (moon 4-tap
transmittance), so puffs stepped between lit and shadowed behind their motion. Hillside, static
camera, `vision_measure` low mist band: flicker sd 10.4-13.0% (p2p 38%) vs 3.4% with
`BB_RT_HAZE_RT=0` and 3.2% stock.

Now `ReadFogLight` averages each read-back per 2 m cell and blends it into a kept grid
(`HazeBlend` 0.25 per read-back, new cells start from the mean, cells unrefreshed for 60
read-backs drop); the mean blends the same way. `HazeColor` samples the grid trilinearly between
the 8 cell centres, a missing cell counting as the mean.

Hunter's Dream, static camera, `pixel_noise.py` per-pixel temporal sd (median), old and new exe
interleaved at the same save:

| region | old | new | stock |
|---|---|---|---|
| mist right | 1.4-2.2 | 0.9-2.4 (median 1.5) | 1.5 |
| mist centre | 1.5-3.9 (2.6) | 1.2-2.3 (1.65) | 1.8 |
| mist left | 2.6-3.4 (2.9) | 2.5-2.8 (2.65) | 2.8 |

## Blue smoke: the particle colour word is B, G, R, A (2026-11-14)

User report: "why all the smokes are blueish in any location". Forbidden Woods bonfires: the
smoke over them blue in RT, warm grey in stock. Smoke region, Lab b of the brighter half: patched
(`HAZE_LIT` on) -9.7 / -11.2, `BB_RT_HAZE_RT=0` -9.4 / -10.0, haze patch off (`BB_RT_HAZE_LIT=0`)
-1.2. So the blue came from `HazeColor`'s lamp tint, not from the traced light.

`BB_RT_FLAME_LOG=1` shows orange flames as `ff5018` (printed byte 2, 1, 0): red is byte 2, the
word is B, G, R, A in memory. `HazeColor` took byte 0 as red, so the fire's warm tint (and the
moon's) went onto blue. Fixed by mapping channel c to byte 2 - c.

Also `FogLight` (rt_gi.comp) now writes one luma share for all three channels: per channel, a puff
in moon shadow kept the blue sky's share and lost the warm moon's.

After (mist ahead of the Forbidden Woods lamp, two pairs): patched b -3.35 / +3.18, unpatched
-3.14 / +3.40 (hue now follows stock; before the patch moved b by about -9).

`rt-gi-test` had failed since 92eb368 (MaterialSlots grew by the fog light slots); fixed.

## Moon swung 50-60 degrees on the first look at the disc: one cache entry per moon (2026-11-14, `rt_moon_cache.h`)

User report: "the moon light is changing". In Cathedral Ward the light pointed at Forbidden Woods'
moon (`0.687 0.497 0.531`, exactly the direction from the camera to that moon's cached place) until
the blood moon came on screen, then swung to it: "found 50.78 degrees from its cached position".
Earlier the same run, Forbidden Woods: 59.71 degrees.

Cause: the cache kept one moon per layer draw, and the two maps share cloud segments (Cathedral
Ward draws `1e000000087 24000000093 2a000000081`, all also drawn in Forbidden Woods). Each visit's
finds took the shared draws over (`SwitchVotes`, 8 finds), so the next visit to the other map was
seeded with the first map's moon.

Now each moon is one entry with the draws seen while it was found (`moon x y z votes order
draws` lines; old per-draw lines merge by position on load). A run takes the moon whose draws best
match its own (shared over the union), so a map whose draws are a subset of another's still gets
its own; equal matches keep the old rules (within 150 m more finds win, farther the last one
confirmed once it has 8). A new log line names the seeded moon and the draws that chose it
("moon seed on/off").

Live, Cathedral Ward, cold start with a crafted cache in the bad state (Forbidden Woods' moon on
Cathedral Ward's three draws plus its own, 1000 finds, confirmed last): seeded Cathedral Ward's
moon (`0.846 0.443 -0.296`) from the first frame; the disc, found after a camera turn, was 0.48
degrees from it (was 50.78). The Forbidden Woods entry was left alone. Forbidden Woods itself is
covered by `rt-gi-test` (both maps, both orders, a run that has seen only the shared draws); not
run live (needs a lamp warp).

## Light that changes after a look away: casters the camera has not seen yet (2026-11-14)

User report after the cache fix above: "still not working". Not the moon: its direction stayed
`0.846 0.443 -0.296` in the log the whole time. Test (`recenter`: view A = camera behind the
player, measure, look 45 degrees away, recentre, measure), Cathedral Ward bridge, fresh launch
each row, mean luma of the player / the ground:

| Build and settings | A before | A after one look away |
|---|---|---|
| this branch, light at the sky's moon | 160 / 80 | 64 / 25 |
| shipped pt2.3 exe, same launcher variables | 163 / 82 | 62 / 25 |
| this branch, `BB_RT_SHELF=0` | 160 / 80 | 65 / 26 |
| this branch, `BB_RT_MOON_FROM_SKY=0` (the game's light direction) | 156 / 91 | 81 / 25 |
| the same with `BB_RT_OFF_INSTANCES=0` (no kept instances) | 157 / 93 | 152 / 88, then 152, 154 |

So the change is geometry: buildings the game had not submitted yet (it frustum-culls map pieces)
enter the TLAS during the look away and are then kept (`AppendKeptInstances`), and they block the
moon. `BB_RT_PICK` names them: kept instances (mode 2), 13 m along the ray to the sky's moon, 32-34
m along the ray to the game's light. It is in pt2.3 too, so not a regression of this branch.
`BB_RT_MOON_REACH` does not help here (the casters are within 60 m).

Tried and dropped the same day (user rule changed twice):

- Light on the game's own direction by default plus the game's shadow mask for off-screen casters
  (`min(traced without kept instances, smoothstep(0.2, 0.55, mask.z))`): player 49 / ground 27 from
  the first frame and the far cathedral dark, against 157 / 93 traced; `BB_RT_MOON=mask` alone gave
  27 / 14. The mask's `.z` is not "1 where lit" in this map (not looked into further). Reverted.
- Moving the moon mesh to the game's light direction: dropped by the user; on this bridge the
  game's light comes from behind the camera while the moon hangs over the cathedral.

Decision (user): the light stays anchored to the sky's moon or sun, in every map, with no cache
and no look at the sky; the game is to keep those meshes submitted outside the view. That needs a
patch of the game's own culling (reverse engineering, in progress); the same kind of patch for map
pieces would remove the caster pop above at its root.

How the sky is drawn in Cathedral Ward (`BB_RT_FAR_LOG=1`, new, live: every ~5 s the probed draws
over 250 m from the camera; `BB_LOG_PS=scene` / `gbuffer` toward and away from the moon):

- Cloud band: 6 draws of ps `765450df` (88-224 triangles, 510-1290 m), submitted every frame.
- Blood moon: a forward draw into the scene colour, ps `c31bd920` vs `c6cdba2b`, 360 indices,
  alpha blended (src alpha, inv src alpha), depth test and write on; only while in view. Being
  blended it is not a probed TLAS draw, so it is not in the far log.
- Two 4-index quads (ps `898dabb3`, `e55e45eb`, additive) also only while the moon is in view.

## The real fix for the light after a look away: placement identity (2026-11-15)

Plan, measurements and status of every step: `GEOMETRY_CACHE_PLAN.md` (study:
`GEOMETRY_CACHE_STUDY.md`). In short:

- The light changed because a static piece was known only by its draw order (`occurrence`),
  which changes with the view: 17.9 BLAS cache misses a frame on a turn, kept copies that
  disagreed with what was drawn, and a BLAS pool at its eviction line. Now a piece is known by
  its mesh and the game's own world matrix (`rt/rt_placement.h`, `BB_RT_PLACEMENT`), one BLAS
  per mesh is shared by its placements (`BB_RT_PLACEMENT_SHARED`), and the pool default is an
  eighth of the video memory (`BB_RT_BLAS_MB`). Misses 0.04-0.07 a frame; the look-away test
  holds (player luma 31.7 -> 34.1, 32.8, 31.8).
- Map pieces the game has not drawn, loaded from the game's files as shadow casters
  (`BB_RT_GEO_DIR`, `BB_RT_GEO_RADIUS`, `BB_RT_GEO_MIN_TRIS`, `BB_RT_GEO_LOG`): built, correct,
  and off by default. At the test bed they add no visible shadow in 11 views and cost 4 % of
  the frames.

New knobs (all live unless noted):

| Knob | Default | What it does |
|---|---|---|
| `BB_RT_PLACEMENT` | `1` | A static draw is keyed by mesh and world matrix once its shader's matrices agreed with the GPU probe. `0`: draw order, as before. |
| `BB_RT_PLACEMENT_SHARED` | `1` | One BLAS per mesh, placements as instances. Switching it drops what the camera has seen (used by the tests as a purge). |
| `BB_RT_BLAS_MB` | VRAM / 8, 512..2048 | Load time. BLAS pool size. |
| `BB_RT_GEO_DIR` | unset | Load time. Folder of `<map>.bbgeo` files (`tools/mapcache/build_cache.py`). Unset: the file pieces are off. |
| `BB_RT_GEO_RADIUS` | `50` | Metres around the camera in which file pieces stand in. `0` off. |
| `BB_RT_GEO_MIN_TRIS` | `32` | Load time. Smallest file mesh worth an instance. |
| `BB_RT_MOON_FILES` | `1` | The moon or sun of the sky the game draws, from the game's files (next section). `0`: search and moon cache only. |
| `BB_RT_SKY_LOG` | `0` | Every 120th frame, each sky draw with the vertices the CPU reads of it. |
| `BB_RT_FAR_LOG`, `BB_RT_MISS_LOG`, `BB_RT_GEO_LOG` | `0` | Diagnostics: far draws, cache misses, the pieces the game draws per map. |

## The moon and sun from the game's files (2026-11-15, `rt_sky_finder.h`, `BB_RT_MOON_FILES`)

User rule: the light is anchored to the sky's moon or sun in every map, with no cache and no
look at the sky. Until now the moon's place was only known after the screen search had seen the
disc (kept in `rt_moon.txt` per set of cloud draws, which maps share).

What the files hold (`tools/mapcache/sky_table.py`, survey in `tools/mapcache/SKY_SURVEY.md`):
every sky is a map piece or object fixed in the world, and its moon or sun is a mesh of that
model (or, in the Hunter's Dream and Yahar'gul under the blood moon, an object of its own that
the same event enables). So the moon's world centre is known for 28 of the 45 sky parts; the
other 17 have none (the Nightmare Frontier, the chalice dungeons, the Old Hunters' base skies).

What the engine sees: the cloud draws (ps `765450df`) come with their model-space vertex buffer.
Index counts do not name a sky (the night sky's 672/576/576/480/216 is in six maps, with moons
hundreds of metres apart), but the vertices do: `rt_sky_table.h` (generated, 45 skies, 680 draws)
holds, for every mesh and detail level, the index count and the four vertices the probe samples.
`SkyFinder` (unit-tested) takes the sky most of the meshes drawn in the last 300 frames belong
to; a tie keeps the sky already chosen, else is settled by where the probe finds the draw in
the world (two maps place one model at different spots), else there is no answer. The light
then points from the camera at the file's moon (seed `w = 2` in the sky layer pass, taken as it
is). A sky without a moon of its own takes the moon part of its map whose draw was seen (the
Old Hunters' moons). The moon cache now only speaks for a sky the table does not know.

Measured (new install: empty moon cache, the disc never looked at; `tools/rt-test/moon_test.ps1`,
`moon_ab.ps1`, `moon_look.ps1`):

| Map (save) | Sky named | When | Check |
|---|---|---|---|
| Central Yharnam bridge, blood moon | `m24_01 m900030_0000` | at the first sky search (4 frames into play) | the light's direction lands on the drawn disc: predicted uv 0.370 0.772, disc centre at 0.369 0.774 |
| Hunter's Dream | `m21_00 o210030`, moon object `o210020` | same | file moon (-204.4, 297.0, 614.6); the disc found by earlier sessions' search: (-203.9, 297.5, 614.7). From the spawn the disc is behind the workshop |
| Old Yharnam | `m23_00 m009000` (a low sun, 14 degrees up) | same | the sunset glow sits in that direction; the disc is behind buildings from the spawn |
| Upper Cathedral Ward (user's save, warp from the Dream) | `m24_02 m900030_0000` | 4 frames after arriving | the light lands on the drawn blood moon about 1 degree from its centre (predicted uv 0.745 0.249; the disc, half behind a statue, centred near 0.756 0.241, 5 degrees in radius) |
| Forbidden Woods (user walking) | `m27_00 m009000_0000` | at the first sky search | sky named only; the disc was not looked at |

| Central Yharnam bridge | Search and cache (`BB_RT_MOON_FILES=0`) | From the files |
|---|---|---|
| Light before any look at the sky | the game's own direction | the moon, from frame 4 |
| Light after two full turns with the view raised | a stray find 12.3 degrees off the moon (0.901 0.253 -0.352), held | on the disc (0.835 0.453 -0.313), changing only with the camera's place (0.5 degrees over 5 m) |
| Moon-facing pixels that change lit/shadowed against the file moon | 6.9 % | 0.02 % (the same state twice) |
| Old Yharnam, the same comparison | 7.3 %, mean luma 40.7 | 0.02 %, mean luma 39.2 |

The positions earlier sessions cached from the real disc agree with the files in four maps: the
find is a point on the near side of the moon's sphere, the file gives its centre (Cathedral
Ward 58 m apart = the radius, 0.4 degrees from the bridge; Forbidden Woods 19 m of a 21.6 m
radius; map m26 104 m of 109 m; Hunter's Dream, a flat disc, 0.7 m).

Not checked live: the maps not listed above; the tie rules
(Central Yharnam's evening sky against Cathedral Ward's daytime sky share four of five cloud
meshes and are told apart by the fifth; Yahar'gul's blood-moon sky is the Abandoned Workshop's
model at another place) are covered by unit tests only. A sun below the horizon is not taken
(the game's direction stays). `BB_RT_MOON_MIN_EL` still applies.

## Tessellated meshes were never in the TLAS: the big trees cast no shadow (2026-11-15, `BB_RT_PATCH_MESHES`)

User report, Forbidden Woods graveyard: "the tree doesnt casting shadow". `BB_RT_PICK` on the
trunk: the G-buffer surface 6.6 m along the camera ray, the first TLAS hit 112 m behind it. The
trunk was in no TLAS of any session, with placement identity on or off, also right after a
purge.

Cause (not from this branch: the rule is from 2026-09-30, so pt2.3 has it too): the trees are
drawn with tessellation (stages `LsHs`, patches of three control points, vs `cc0f4e5a`, 9-18
instances a batch, 5055 and 6762 indices). A draw reaches the ray tracer only through the
motion-vector variant of its pipeline, which stores the vertex shader's positions, and that
variant exists for plain vertex-shader pipelines only. Found with two new logs: `Ray tracing:
draw without captured positions (...)` (always on, a few lines a window, names the shaders and
the reason) and, with `BB_MOTION_SELECT_LOG=1`, `Motion select: not possible ...` (the state
that rules the variant out).

Fix: the control mesh on the CPU. For a tessellated G-buffer draw the rasterizer reads the
model-space position stream, the index list and the world matrix of every instance (the game
keeps a block of 208 bytes per instance in a vertex-shader buffer, the 3x4 matrix first;
`rt_placement.h ReadPatchMatrices`, unit-tested) and hands them over (`AddPatchDraw`). One
BLAS per mesh is built from that triangle soup (the build buffer of the geometry cache, now
made on first use); each instance stands in the TLAS as a shadow caster (mask `MaskKept`, no
material), and stays, like a kept placement, while the camera is near where it saw it.

Measured at the graveyard, moon debug view, the same frame with `BB_RT_PATCH_MESHES` 0 and 1:
2 BLAS (3,939 triangles), 68 tree instances; the tree in front of the moon now shades the
ground and the player (0.52 % of the moon-facing pixels of that view go from lit to shadowed,
none the other way; the same state twice: 0.00 %).

Limits: shadow casters only (bounce light and reflections do not see these trunks); the BLAS
is the control mesh, not the displaced surface (a trunk seen from its moonlit side was not
checked for self-shadowing); only the 208-byte instance block is read, so a tessellated draw
with another layout is still left out and shows up in the log above.

| Knob | Default | What it does |
|---|---|---|
| `BB_RT_PATCH_MESHES` | `1` | Live. Tessellated draws as shadow casters from their control mesh. `0`: left out, as before. |
| `BB_MOTION_SELECT_LOG` | `0` | Load time. Each G-buffer vertex shader's motion-variant choice, and why a draw cannot take it. |

## Draws that never reached the ray tracer: a full vertex store and a 395-bone skeleton (2026-11-15)

The log line added for the trees above (`draw without captured positions`) named two more
causes while the user played, both older than this branch:

**The vertex store was full in Central Yharnam.** Every G-buffer draw stores its positions in
the object-motion store while the ray tracer captures (the TLAS is built from them). The store
held 8,388,608 vertices a frame; near the bridge the frame used 8,388,591 of them and 28,800
draws in 600 frames (48 a frame) found no room: no motion parameter, so no `AddDraw`, so
never in the TLAS. Building pieces of 33,000-99,000 indices were among them. The store is
sized at start now (`BB_OBJECT_MOTION_VERTICES`, millions): 16 M with ray tracing on a card
with 11 GiB or more, 8 M otherwise (the three stores take 56 bytes a vertex: 448 MiB at 8 M,
896 MiB at 16 M). The shader constant that bounds the store is set from it before any motion
shader is compiled (those are built every session, never loaded from disk). The stats line
`Object motion: ... capacity skips ... the most asked for in a frame` prints by itself once a
draw is skipped.

Measured so far: Central Yharnam near the bridge at 8 M: 28,800 draws skipped per 600 frames,
8,388,591 of 8,388,608 vertices used. At 16 M, the same spot (camera -190.8 -27.2 71.1, the
user standing there for 30 s): no draw skipped and none logged without positions; TLAS
5,729,580 triangles in 941 instances. How many vertices that frame asks for is not known (the
stats line prints it only when a draw is skipped).

On a card under 11 GiB the store stays at 8 M and the skip line will show in heavy areas; the
cure there is not to store placed static draws every frame (not built).

**A skeleton of 395 bones.** `Motion::ClassifyBuffer` took any buffer over 16,384 bytes for
"not a palette", so a four-part creature in Upper Cathedral Ward (vs `3deaa573`, palette of
18,960 bytes, 15,140 triangles) got no motion variant and was in no TLAS. A buffer over that
size that is a whole number of 48-byte bones, up to 64 KiB, is a skeleton now (unit-tested).
Upper Cathedral Ward after the two fixes: 0 draws refused, TLAS 3,939,423 -> 3,954,593
triangles (the creature's 15,140 and a few more).

Tried and dropped: "every instanced G-buffer draw takes the motion variant" (the trees were
tessellated, not this) and "two 864-byte buffers mean an 18-bone skeleton" (the selection log
showed the 18,960-byte palette instead).

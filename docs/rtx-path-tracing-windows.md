# Official RTX Remix game renderer (experimental)

RTX Remix is connected to the game's Vulkan draw stream. Select **Graphics →
Renderer → RTX Remix (experimental)** in the Windows launcher, then launch the
game. Select **Vulkan** and restart to return to the regular renderer.

The packaged build includes the official **RTX Remix 1.5.2** runtime and SDK
**0.6.4**. The previous custom path tracer and the user-facing synthetic SDK
demonstration have been removed. The worker `bin/bb-remix-host.exe` receives
captured game scenes; it does not create a demo scene.

## What is connected

- Eligible direct G-buffer draws export the actual post-shader position and UV
  through **Vulkan EXT_transform_feedback**, with a separate driver-bounded
  range for each draw. Indexed geometry and vertex-shader animation use the
  same original draw commands as the game.
- Homogeneous positions are converted back into world coordinates using the
  game's camera constants. Primitive duplicates are welded; normals are
  reconstructed from triangles.
- Actual diffuse textures with sRGB semantics and compatible BC5 normal maps
  are exported to DDS. Normal-map Z is reconstructed. Export is cached by
  content and limited to 16 MiB per frame.
- World meshes retain stable identities and revisions. Only changed meshes are
  uploaded to the SDK. Recently visible static geometry remains available for
  secondary rays for up to 600 frames; dynamic geometry has a shorter lifetime.
- The official SDK creates materials, meshes, instances and camera state,
  performs path tracing and produces a shared output image.
- The same-adapter shared image is imported by the game renderer and composed
  after the original scene tonemap, **before the native HUD and menus**.

## Current limitations

This is an experimental renderer, not a validated replacement for every game
scene. Original analytical lights have not been translated; captured surfaces
currently receive an explicit uniform SDK environment light. Original lighting,
sky, fog and material appearance will therefore differ.

Capture currently covers eligible vertex-shader G-buffer triangle draws.
Indirect draws, tessellation, geometry-shader output, particles and some
transparent passes are not included. Shader-specific UV transforms, original
vertex normals, roughness and metalness semantics are incomplete. Retaining
recently visible meshes is not equivalent to loading the complete map: unseen
geometry can be absent from reflections and secondary rays. Moving objects
without a recognized dynamic-buffer signature can leave stale retained geometry.

Game HDR, temporal upscaling and frame generation are bypassed for this session
without rewriting saved graphics preferences. Remix owns its reconstruction;
its worker's frame generation is disabled because generated worker presents are
not transferred to the game's swapchain. The initial SDK profile uses the
importance-sampled path tracer, with NRC and ray reconstruction disabled. A real
uniform environment texture is supplied to the SDK dome light; an empty dome
texture caused an SDK null-pointer crash during integration testing.
Output resolution still follows the
launcher setting. The original raster work remains as a fallback, and capture
readback and SDK completion currently synchronize each frame. No gameplay
performance improvement is claimed.

Initial SDK shader compilation keeps the raster image visible until a nonblack
Remix output is ready, but initialization and synchronized worker calls can
pause gameplay during warmup. Unsupported capture/sharing, missing runtime, invalid
scene data, SDK failure or worker timeout retain raster output and log the error.
A blocked worker can take up to 90 seconds to time out. Switch back to Vulkan if
this experimental mode is unsuitable.

Gameplay appearance, camera alignment, scene coverage, map transitions and
frame time still require testing with the complete game assets. Internal GPU
and SDK tests cannot establish those results.

## Runtime, logs and source builds

Runtime files stay in `remix-runtime-1.5.2/.trex`; no system Direct3D libraries
are replaced. If the runtime folder is missing:

```powershell
.\Bloodborne.exe --script .\scripts\install_remix.py
```

The installer verifies the original release ZIP SHA-256, rejects unsafe archive
paths and preserves official licenses. Logs are written to
`logs/rtx-remix-game.log` and the normal session log. Texture exports are generated
locally under `remix-assets`; they are not included in distributed builds.

For a source launch, build with `build.sh`, install the pinned runtime at the
repository root, and set `BB_RTX_REMIX=1` before running `run.py`.
`BB_REMIX_RUNTIME` may point to an absolute official `.trex/d3d9.dll` path.
`BB_REMIX_CAPTURE_MB` accepts 32–256 MiB, further limited by the driver.

Internal validation uses `remix-scene-test` and `remix-game-test`. The latter
executes the production SPIR-V exporter, verifies complete-primitive capture and
buffer boundaries, and optionally exercises the production worker, official SDK,
shared Vulkan image, resize, mesh revision and invalid-index rejection.
It is a development test and is not packaged as a game feature.

The official runtime can log `common device objects were not disposed of` at
shutdown. Process isolation contains its lifetime; the upstream warning remains.

## Official sources

- [RTX Remix SDK](https://github.com/NVIDIAGameWorks/dxvk-remix/blob/b81a7b566b1eeb9edb4dc2b3c9d3972e0f253ad4/documentation/RemixSDK.md)
- [RTX Remix 1.5.2](https://github.com/NVIDIAGameWorks/rtx-remix/releases/tag/remix-1.5.2)
- [Vulkan transform feedback](https://docs.vulkan.org/spec/latest/chapters/vertexpostproc.html#vertexpostproc-transform-feedback)

SDK copyright and MIT notices are retained in `gpu/remix/LICENSE.txt` and the
unmodified official header. The original runtime ZIP SHA-256 is
`cc424be4dd1a0c6fd922bc6a7f8e5f6582baea7043a38afa6686d8b6faabad01`.

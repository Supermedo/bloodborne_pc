# Windows troubleshooting: AMD RDNA2 and online play

Notes from getting version 1.6 to run on one Windows 11 PC: AMD Radeon RX 6900 XT (RDNA2,
16 GB), 32 hardware threads, Xbox 360 controller, 4K display, game 1.09. Each entry gives the
line in `user\last_run.log`, what it means, and the setting that got past it. The settings go
in the launcher (**Advanced**, **Graphics**) or in **Advanced → Extra variables**.

## Crashes

| `last_run.log` | When | Cause | What helped |
|---|---|---|---|
| `Unimplemented PM4 type 0, base reg: 16384, size: 15810` then `STOP: GPU library assertion failed` | Right after a save loads | The GPU thread decoded a command buffer the game had already reused: the size is garbage | `BB_COPY_GPU_BUFFERS=1` (copy command buffers at submit) and **Two-stage GPU pipeline: Off** |
| `vk_scheduler.cpp: SubmitExecution: Device lost during submit` | When the world appears | GPU hang/reset (TDR) | `BB_PREP_WORKERS=0` (no draw-preparation workers) and **Object motion vectors** off |
| `Fault: read access to 0000005300000000 at guest offset 0x263b8e7` | Title screen, Two-stage GPU pipeline on | The game read a pointer before the GPU-side write it waits for had landed | **Two-stage GPU pipeline: Off**. Keeping it on with only its fence and memory-write stages off (`BB_TOGGLE_FILE` with bits 38–42) was not tested to the end |
| `vk_graphics_pipeline.cpp: Failed to create graphics pipeline: ErrorUnknown` | At start, after the crashes above | Pipeline cache written by crashed runs (`WarmUp: N stale pipelines were found` the run before) | **Advanced → Clear shader cache** |

The combination that runs stably on this PC:

```
Two-stage GPU pipeline: Off
Extra variables: BB_COPY_GPU_BUFFERS=1 BB_PREP_WORKERS=0
Object motion vectors: off
```

## Frame rate

- **FSR 4 on RDNA2: about 6 FPS at 4K (RX 6900 XT), 18 at 1080p (RX 6700 XT).** The model
  itself is not the cause: with `BB_FSR4_PROFILE=1` in Extra variables the log lists every pass,
  and at 1080p only the two from `fsr4_shaders\opt` were slow (`model 11` 19.6 ms, `post`
  26.2 ms of 47.9). AMD's Windows driver kept the per-index table copies glslang made in them
  in scratch memory; passes rebuilt by `tools/fsr4_optimize.sh` since `single_tables`
  (`tools/Fsr4SpirvCrossFixes.pm`) take 2.5 ms for the whole of FSR 4 there. With older files,
  `BB_FSR4_OPT=0` loads the original passes (3.1 ms, but at 1080p output it brings back the
  pass 11 race: a flickering strip at the left edge), or use **FSR 3.1** (`upscaler=fsr3` in
  `bbport.ini`). The 4K case was not measured again.
- The stability settings above cost some speed: the two-stage pipeline is the largest gain
  (20–30 %) when it works.
- **Graphics cards with 4 GB.** Discrete GPUs with up to 4.5 GB get a low-VRAM profile, shown in
  the log as `Low-VRAM profile (4096 MiB card): ...`. It raises the texture budget to the card's
  memory less 600 MiB. At 1080p output with an upscaler, it also patches the scene size at start
  instead of changing it live (preset changes then need a restart). On CPUs with up to 4 cores
  it turns the draw-preparation workers off. **Advanced → Low-VRAM profile** (or `BB_LOW_VRAM=0`/`1`
  in Extra variables) turns it off or forces it. The profile is from fixdag and bashiku's 4 GB mod.
- **The frame rate falls after some minutes (laptops).** `BB_THERMAL_LOG=1` in Extra variables
  writes `Thermal: GPU 87 C, 60 W: the driver is slowing the card down to cool it` to the log
  when an NVIDIA card is throttled for heat (checked every 20 seconds with `nvidia-smi`).

## Online play

- `HandleLoginReply: Login rejected error code 8`: shadNet error 8 is `LoginInvalidPassword`
  (7 would be an unknown account name). Re-enter the password under **Online co-op**.
- `cpp-httplib failed for GET https://thehuntersdream.com:20443/bb-eu/ss.info: error=10
  (SSL server verification failed)`, shown in the game as a failure to get server
  information. The server's certificate is valid (Let's Encrypt `YE2` → `Root YE` →
  `ISRG Root X2` → `ISRG Root X1`). cpp-httplib checks it twice on Windows: with OpenSSL
  against the Windows `ROOT` and `CA` stores, then with `CertGetCertificateChain` on the leaf
  alone. On this PC the user's intermediate store (`certmgr.msc` → Intermediate Certification
  Authorities) held an expired `ISRG Root X2` cross-certificate (valid until 2025-09-16,
  thumbprint `151682F5218C0A511C28F4060A73B9CA78CE9A53`), and building the chain from the
  leaf alone first failed with `NotTimeValid`. Outside the game both checks pass once Windows
  has cached the current intermediates, yet the game kept failing; the cause inside the game
  is not found yet. Removing the expired certificate is the next thing to try. More detail
  comes with `BB_GPU_LOG=info` in Extra variables.

## Launcher settings get overwritten

The launcher writes `%APPDATA%\bbport-launcher\settings.json` and `bbport.ini` from its own
state when it starts the game or closes. Edits made to those files while it is open are lost.
Close the launcher before editing them by hand, or start the game with `Play Bloodborne.exe`.

## Button layout

**Cheats → Japanese/Asian buttons** (`tweak_circle_confirm`) gives the layout of the Japanese
and Asian releases: Circle confirms and interacts, Cross cancels and dodges, and the button
icons follow. Two things that do not work for this:

- Swapping Cross and Circle in the pad input: the buttons move, but the game still draws the
  Cross icon for interacting.
- The PS4 system setting for the confirm button (`sceSystemServiceParamGetInt` 1000): the game
  asks for it but takes the layout from its region instead.

## Korean text

The game language list has no Korean. With the language left on English, the
[Enhanced + Vanilla Plus Korean translation](https://github.com/kairess/bb-enhanced-vanillaplus-korean)
mod (a Korean font and Korean `engus`/`enggb` message files) shows the game in Korean; put it
in `mods\` below any mods it translates. The *Unlock Game Region* patch is not needed for it.

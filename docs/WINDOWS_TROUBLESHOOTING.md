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

- **FSR 4 at 4K on RDNA2: about 6 FPS.** The log shows
  `FSR 4 v07 INT8 quality model, 2160 tier, output 3840x2160`. The INT8 model is tuned for
  RDNA3; on an RX 6900 XT use **FSR 3.1** (`upscaler=fsr3` in `bbport.ini`).
- The stability settings above cost some speed: the two-stage pipeline is the largest gain
  (20–30 %) when it works.

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

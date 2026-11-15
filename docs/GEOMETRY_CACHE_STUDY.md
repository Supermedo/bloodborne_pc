# Geometry cache study (2026-11-14/15)

Goal (user): the ray-traced scene holds every static map piece and the light stays on the
sky's moon or sun, whatever the camera has looked at, with no cache of finds and no look at the
sky. This file is the study before any design. Measurements and method: `PATH_TRACING.md`,
"Light that changes after a look away".

## 1. The problem, measured

The game frustum-culls map pieces. A piece enters the TLAS the first time it is drawn and is
then kept; the same view is lit before a look away and shadowed after (player 160 -> 64 on the
Cathedral Ward bridge; the shipped pt2.3 exe 163 -> 62). The moon's direction did not move.

## 2. The game's culling (static reverse engineering, eboot 1.09)

Patch `Address` = ELF vaddr + 0x400000; file offset = vaddr + 0x4000.

- Frustum test: `FUN_022928a0` (AABB against 6 planes; 0 out, 1 intersecting, 2 inside).
- Whole models: tree walk `FUN_00ee0280` (drops a subtree), then per model `FUN_00ec6380`
  (direct submit) or `FUN_00ec5970` (deferred list). Result at ctx+0x490.
- Pieces of a model: `FUN_0224df10` tests each mesh's box only when the model is
  "intersecting" and clears failed meshes in a 128-bit mask.
- No "never cull" flag exists; the debug flag "Disable Entity Culling" (0x054b0874) is BSS.

| Patch | Lines (`Type="bytes"`) | Live result (Cathedral Ward) |
|---|---|---|
| Everything inside | `0x012c63a2=eb18`, `0x012c59cd=eb1c`, `0x012e0298=9090` | kept instances 360 -> 74; RT 45.6 -> 35.2 FPS, stock 111 -> 87; frames over 50 ms 3 -> 53; the blood moon is still missing at 1 of 4 headings |
| No per-piece test | `0x0264e090=90e9b5030000` | blood moon submitted at 4 of 4 headings (1 of 4 missing without it); RT 44.0, stock 110 FPS |
| Frustum test returns 2 | `0x026928a0=b802000000c3` | not run |

A distance limit or a wider cull frustum needs injected code (the test's margin is a
hard-coded zero).

## 3. The game's files (prototype `tools/mapcache/`, pure Python, read-only)

- Loose files: `map/mapstudio/<map>.msb.dcx` (placements), `map/<map>/*.flver.dcx` (models),
  `obj/*.objbnd.dcx` (objects). DCX is DFLT/zlib; FLVER2 0x20014, little-endian, static triangle
  lists, float3 positions; MSB parts carry position, rotation (degrees), scale, entity id.
- Placement `T * Ry * Rz * Rx * S`. **World coordinates equal the engine's, zero offset.**
- m24_00: 731 models, 4048 map pieces, 13,643 placed meshes, 23.5 M unique and 47.0 M placed
  LOD-0 triangles. m24_01 (Central Yharnam: the bridge of these tests is in this map, m24_00 is Cathedral Ward): 960 models, 4056 pieces, 33.8 M unique
  LOD-0 triangles. No parse failure. Parsing m24_01 takes 13.8 s in pure Python.
- 12 of 13 triangle counts of live draws at the bridge match meshes of m24_01 whose boxes sit
  1.4-11.6 m from the player: m009014 (7 meshes), m003020 (3), m019011, m009009. One count
  (15720) matched nothing.

## 4. Sky, moon and sun in the files (`tools/mapcache/SKY_SURVEY.md`)

- A map's skies (day, evening, night, blood moon) are separate parts of the same MSB. The map's
  event script reads world-phase flags 9800 / 9801 / 9802 and enables one sky part by entity id.
  m24_01: 2414010 `m900000`, 2414011 `m900010`, 2414012 `m900020`, 2414013 `m900030`.
- The moon is a mesh of the sky model in m21_01, m22, m23, m24_00/01/02, m25, m26, m27, m32; a
  separate object or piece in the Hunter's Dream (`o210020`), m28 (`o280021`), m34, m36; m33, m35
  and the chalice skies have none.
- Cathedral Ward, blood moon: the live cloud band's index counts (672, 576, 576, 576, 480, 264)
  are exactly meshes 1-6 of `m900030`. The file's moon (mesh 7, 3968 triangles, `M_Moon[Dn]`)
  has its centre 0.3 degrees from the live moon's direction, but the live draw has 120 triangles
  and sits at 0.91 of the distance. Not explained.
- Hunter's Dream: `o210020` mesh 0 (32 triangles) has its centre at (-204.4, 297.0, 614.6); live
  (-204, 297, 615).

So the active sky can be told from the cloud band the game always draws, and the moon's place
read from the file, without a game patch.

## 5. Open questions (before a design)

1. Which map(s) are loaded: match live draws against the cache, or follow the game's file reads.
2. Size: 34-47 M LOD-0 triangles per map against 2-3.6 M kept today. Which LOD per distance,
   what radius, how much memory, how long the first-launch build takes for all maps.
3. What stays live: doors, breakable objects (`breakobj`), enemies, cloth, effects; pieces an
   event hides.
4. Alpha-tested foliage needs its textures (MTD, TPF) or it casts solid shadows.
5. Where the RT frame time goes today (static scene upkeep, ray passes, CPU), to size the gain.
6. The 120-triangle live moon and the unmatched 15720-triangle draw.

## 6. Where the RT frame time goes today (measured 2026-11-15)

Cathedral Ward bridge, 3172x1328, `BB_RT_HITCH_MS=1` (every frame logged), mean per frame. Stock
runs at 111-120 FPS here (about 9 ms a frame).

| | Standing still (64 FPS) | Turning, first and second full turn (55 FPS) |
|---|---|---|
| RT CPU, total | 3.7 ms | 5.7 ms |
| - taking the game's draws (558-590 calls) | 0.54 | 0.95 |
| - BLAS / TLAS build commands | 1.20 | 2.04 |
| - GI pass recording | 1.58 | 2.65 |
| - probe readback | 1.48 | 1.84 |
| RT GPU, total | 8.4 ms | 10.8 ms |
| - acceleration structure build | 1.11 | 1.81 |
| - bounce light (GI) | 4.32 | 4.98 |
| - direct light and moon shadows | 2.57 | 3.55 |
| - reflections | 0.22 | 0.27 |
| BLAS rebuilt per frame | 2.8 (38 k triangles) | 25 (190 k triangles) |
| BLAS refit per frame | 56 (756 k triangles) | 67 (670 k triangles) |
| Kept instances | 2 (20 k triangles) | 330 (2.09 M triangles) |

- The ray passes (GI, direct, reflections) are 7.1-8.8 ms of GPU time and do not depend on how the
  static scene got there. A cache does not shorten them, except through a better static BVH.
- The static-scene upkeep a cache can remove or shrink: taking static draws, their probes and
  readback, their share of the build commands, and the rebuilds: about 2-3 ms of the 3.7-5.7 ms
  RT CPU time and up to about 1 ms of GPU build time (estimate from the rows above, not measured
  by switching it off).
- While turning, 25 BLAS (190 k triangles) are rebuilt every frame on the second full turn too,
  when nothing is new: meshes that come back into view miss the cache and are built again. That
  is wasted work today, with or without a geometry cache.
- 56-67 BLAS (670-756 k triangles) are refit every frame even standing still: skinned and moving
  meshes. Those stay live.

Why meshes are rebuilt while turning (same run, mean per frame, hitch line counters):

| | Standing still | Second full turn |
|---|---|---|
| BLAS rebuilt | 2.8 | 25.4 |
| - cache miss ("new") | 0 | 17.9 |
| - triangle count changed | 0 | 3.8 |
| - skeleton or palette changed | 2.8 | 3.6 |
| superset hits | 0 | 43.8 |

On a turn over views the camera has already seen, 18 meshes a frame still miss the BLAS cache
(about 10,000 rebuilds per full turn). A map piece has no identity of its own in the cache: the
key is the draw's shader, buffers and counts plus `occurrence`, its order among identical draws
of the frame. Maps reuse models many times, the game culls by view, so the same placement gets
another occurrence number in another view, misses, and is built again while the old build is
kept or shelved. That is the source of the kept copies that do not agree with what is drawn
(`BB_RT_KEPT_PROBE` hides some), and of the light that changes after a look away. Placements
from the map files have a stable identity (the part name); matching a live draw to its placement
by position would replace `occurrence`.

Second spot, Forbidden Woods by the bonfires (user's pick: foliage, fires, enemies), same method:

| | Standing still (37.6 FPS) | Second full turn (35.5 FPS) |
|---|---|---|
| RT CPU, total | 3.4 ms | 3.9 ms |
| RT GPU, total | 19.7 ms | 21.8 ms |
| - acceleration structure build | 1.73 | 1.16 |
| - bounce light (GI) | 9.46 | 12.82 |
| - direct light and moon shadows | 7.03 | 6.91 |
| - reflections | 1.28 | 0.72 |
| BLAS rebuilt per frame, of which cache misses | 8.4, 0 | 4.8, 0.19 |
| BLAS refit per frame | 169 (787 k triangles) | 91 (450 k triangles) |
| Kept instances | 94 (0.57 M triangles) | 475 (1.22 M triangles) |

Stock runs at 120 FPS here. The rays are 17.8-20.4 ms of a 26.6-28 ms frame; the static upkeep is
small and almost nothing misses the cache on a turn (0.19 a frame against 17.9 in Cathedral
Ward: the woods reuse few models). A geometry cache does not make this spot faster; ray cost
through alpha-tested foliage and under many fire lights does that. The look-away test is not
valid here: R3 locks on to the enemies instead of recentring. Seen again: the player is nearly
black with RT on at this spot while stock lights them.

## 7. File data in numbers (`tools/mapcache/study/STUDY_DATA.md`, measured unless marked)

- Detail levels: a mesh has up to 3 index lists over one vertex buffer (LOD0, 1, 2; median size
  0.61 and 0.21 of LOD0). Many meshes have LOD0 only, so a whole map only drops to about half:
  m24_01 61.3 / 46.1 / 31.2 M placed triangles at LOD 0 / 1 / 2.
- Around the bridge (m24_01), placed / unique triangles: 25 m 4.8 / 4.0 M; 100 m 28.9 / 16.9 M at
  LOD0, 23.5 / 14.9 M with LOD0 under 30 m, LOD1 under 100 m. 143 unique models within 25 m, 539
  within 100 m. Forbidden Woods, 100 m: 7.8 / 5.4 M.
- Memory, estimated at 36 bytes a triangle and one BLAS per model: 100 m around the bridge 537-607
  MB; the whole map 649 MB (lowest LOD) to 1.2 GB (LOD0). The BLAS pool today is 543 MiB.
- First-launch build: reading every model outside the chalice dungeons takes about 126 s in pure
  Python (estimate from 16.4 s for m24_01); 7,254 model files, 6.95 GB. A cache on disk of
  positions and lowest-LOD indices is about 3.3 GB (estimate).
- Only 63 of 28,908 map pieces carry an entity id (skies, windows by time of day, candles, blood
  decals); the rest never change. Doors, lifts and breakable things are Object parts (m24_01:
  1,692 parts, 9.5 M triangles) and must stay live.
- Alpha-tested materials (MTD AlphaRef > 0): 10.6 % of the triangles in m24_01, 34.3 % in the
  Forbidden Woods. Their textures are in `map/mXX/*.tpfbhd`; the model has the UVs.
- m24_00, m24_01 and m24_02 share one origin; m27 is offset by (-153.7, -61.7, 214.2) from
  m24_01 (found by matching geometry; where the game stores it is not known).

## 8. What the study says about the design

1. The measured cause of the changing light and of 10,000 rebuilds a turn is identity: a
   placement is told apart only by its draw order. That can be fixed without loading any
   geometry from the files: give every draw the identity of its placement.
2. Loading the geometry itself is what makes shadows complete before the first look, and it is
   the expensive part: 0.5-1.2 GB of video memory for one city map against a 543 MiB pool today,
   so it needs a radius, the lowest LOD far away, and a budget.
3. The sky needs neither a patch nor a look: the cloud band names the active sky, the file gives
   the moon.
4. One live question decides how identity is done: can the engine read a draw's world matrix
   from the game's own constant buffer? If yes, a draw's placement is known exactly on the CPU,
   one BLAS per model serves every placement, and static meshes need no position capture or
   probe. If no, a draw is matched to a placement by the position the probe reports.

## 9. The open question, answered: a draw's placement is in the game's own buffers

`BB_LOG_PS_INDICES=42300` (new, live: logs the draws with that index count and their constant
buffers) on the bridge, for mesh 0 of model `m009014` (15 placements in the map file):

| Draw | Where the placement is | Value (rows of a 3x4 world matrix) | Map file part |
|---|---|---|---|
| vs `9da6aacb`, 1 instance | the 416-byte per-object buffer, rows 0-2 | rot Y -135, scale 1, at (-283.3763, -35.1857, 57.4331) | `m009014_3700`, same numbers |
| vs `6a833138`, 2 instances | a buffer of 48 bytes per instance | rot Y 135, scale 0.75, at (-212.5196, -40.4334, 69.7036) | `m009014_3600`, same numbers |
| | | rot Y 45, scale 1, at (-255.0370, -34.7928, 116.6749) | `m009014_3300`, same numbers |

So the game already tells the engine, per draw and per instance, exactly which placement it is
drawing, to four decimals of the map file. The game also batches identical pieces into one
instanced draw. Vertex positions are plain float3 at offset 0 of a 32-byte vertex, in model
space.

Consequences: a placement's identity is (mesh, world matrix), known on the CPU at draw time; one
BLAS per mesh in model space can serve every placement through the TLAS; static meshes need no
position capture, no probe and no `occurrence`.

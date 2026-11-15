# Geometry cache: plan and status

Why and the measurements: `GEOMETRY_CACHE_STUDY.md`. This file is the working plan; update the
status lines as steps are proven live. User rule (2026-11-15): the code is written by the
supervisor in small steps, each built and measured live before the next; nothing is pushed.

## Test bed

- Spot: the stone bridge in Central Yharnam (m24_01), player near (-216, -30, 64). The save loads
  there. Stock runs at 111-120 FPS.
- Scripts in `tools/rt-test/` of the development branch (not carried here: they drive the game
  through a pad file this branch does not have): `launch.ps1 -Name x -More @{...}` starts the
  game at the save with the launcher's RT variables; `turn_test.ps1 -Name x` launches, stands
  still 12 s, makes three full camera turns and prints the mean RT split and rebuild reasons per
  phase (`cpu_split.py`, `rebuild_reasons.py` read the `Ray tracing hitch: frame` lines;
  `BB_RT_HITCH_MS=1` logs every frame).
- Build: close the game first, then `ninja -C out/gpu bb-probe.exe rt-geometry-test
  rt-gi-test rt-direct-test` (clang64 on the PATH).
- Baseline before step 1 (commit 6c9278a), second full turn: 17.9 cache misses a frame, 25
  BLAS rebuilt a frame, RT CPU 5.7 ms, RT GPU 10.8 ms, 330 kept instances (2.09 M triangles).
  Look-away test on a fresh launch: player luma 160 -> 64.

## Steps

1. **Placement identity** (`rt/rt_placement.h`, `BB_RT_PLACEMENT`): a static piece is keyed by
   (mesh, the game's own world matrix). Target: about 0 cache misses a frame on a turn over
   seen views; no kept copy that disagrees with what is drawn; look unchanged.
   Status: **proven live 2026-11-15** (bridge, turn over seen views, default settings):

   | | Before (6c9278a) | Step 1 |
   |---|---|---|
   | Cache misses a frame, turn 2 and 3 | 17.9 | 0.04-0.07 |
   | BLAS rebuilt a frame (skeletons included) | 25 | 4.4-4.6 |
   | Kept instances | 330 | 1,740 |
   | BLAS pool used | 396 of 543 MiB (at its limit) | 462 of 1,783 MiB (669 uncompacted) |
   | RT CPU, turning | 5.7 ms | 8.4-8.5 ms |
   | Frames in a 10.4 s turn | 593 | 565 |
   | Look-away test, player / ground luma | 160 -> 64 | 31.7 -> 34.1, 32.8, 31.8 / 48.6 -> 45.6, 43.2, 43.6 |

   What it took, in order (each measured before the next):
   - Per-placement keys alone made it worse (22-27 misses): the batch's other buffers were taken
     as a palette, so pieces counted as deforming and were evicted after 2 s; movers were rebuilt
     every frame; the game draws one placement through two shaders (batched and single).
   - With those fixed, 21 misses remained: the pool (543 MiB, sized for one frame of draws) sat
     at its 75 % eviction line. A 3 GB pool gave 0.4 misses and showed a turn needs 892 MiB.
   - One BLAS per mesh, placements as relocated instances (`BB_RT_PLACEMENT_SHARED`): 670 MiB,
     461 MiB compacted, and a new copy of a known mesh needs no build at all.
   - Pool default: an eighth of the video memory, 512 MiB to 2 GiB (`BB_RT_BLAS_MB` overrides).
   - Kept placements are records (matrix + mesh), not BLAS copies; a record is kept once seen on
     two frames; a settled record whose object moved (a new placement of the same mesh within
     2 m on the next frame) is dropped, so doors and lifts leave no ghost.

   Per-instance CPU, cut after step 2 (same turn, cache off; the display note below applies to
   the frame counts, so they are compared within this table only):

   | | Step 1 | With the three cuts |
   |---|---|---|
   | RT CPU, turning | 8.6 ms | 5.7 ms |
   | of it: TLAS build | 3.5 ms | 2.8 ms |
   | of it: probe readback | 3.3 ms | under 0.1 ms |
   | Frames in a 10.4 s turn | 567 | 580 |

   The cuts: an entry is found by id through a pointer table (it was two hash lookups, one of
   them hashing the whole draw key, at 25 call sites); the probe results of instances that
   compare nothing (every kept placement) are not read back from write-combined GPU memory;
   the textures of a mesh are resolved once a frame, not once per kept placement.

   Open after step 1:
   - `BB_RT_COMPACT=1` is worth setting: with compaction the same turn needs 461 MiB of BLAS
     pool instead of 670 at the same frame rate. Every measurement in this file was taken
     with 1.
   - The file moon (step 3) staying on by default: user-approved 2026-11-15.
   - Regression found by the user playing Central Yharnam (2026-11-15): "the low version of
     an area loads and the light goes black there". Kept placements of shared meshes had been
     left out of the kept probe (`BB_RT_KEPT_PROBE`: a kept instance found in front of the
     drawn surface is one the game no longer draws there, the far low-detail twin of a
     block), on the reasoning that they stand where the game's matrix puts them; so does the
     twin. They are probed by their placement record now and stay out until the game draws
     that placement again. One camera turn at the bridge: 19 kept placements dropped that way;
     the same view one full turn apart: 0.04 % of the moon-facing pixels change. Whether the
     black spots are gone where the user saw them: to be confirmed by the user.
   - Another map, checked 2026-11-15: Old Yharnam (the save's spawn), the same turn test: 626
     frames per turn, RT CPU 2.7 ms, 370 kept instances, 6 shaders trusted. 0.65 misses a frame
     remain on seen views, every one of them the same 6-triangle effect mesh (vs `41ae600f`,
     18 indices, no index buffer) whose vertices the game rewrites each frame; no static piece
     misses. The Hunter's Dream ran with step 1 too (moon test only, no turn numbers).
   - A door or lift, used by the user (2026-11-15): no ghost seen. The mover rule itself
     fired 0 times in that session (`0 moved objects dropped`, `0 movers found`): that
     object is drawn through a shader whose matrices are not trusted, so it never became
     a placement. The rule is covered by unit tests only.
   - The partitioned TLAS (`BB_RT_PTLAS=1`, one BLAS per placement) starts and renders after
     all of the above: Central Yharnam, one camera turn, 10.9 M triangles in 2,186
     instances, no error in the log. Not measured further (it is off by default).
   - From 2026-11-15 ~01:30 the display changed to 864x1536 and the game window to 1080x452, so
     GPU times and luma after that are not comparable with the earlier 3172x1328 rows; counts
     and CPU times are.
2. **Pieces the game has not drawn, from the map files**: cache built by `tools/mapcache`
   (placements + models, lowest detail), loaded per map, added within a radius as shadow-only
   casters (TLAS mask `MaskKept`), skipped where the same placement is drawn or kept live.
   Target: a fresh launch is lit the same before and after a look away.
   Status: **built and measured live 2026-11-15; correct, but no gain at the test bed, so it is
   off unless asked for** (`BB_RT_GEO_DIR=<folder of .bbgeo>`; nothing sets it by default and
   the launcher does not build the files).

   How it works: `tools/mapcache/build_cache.py` writes `<map>.bbgeo` (version 3: models, meshes
   with the index counts of every detail level, placements with matrix, box and draw groups,
   the collisions' display-group sets, the coarsest triangles). `rt/rt_geo_cache.h` reads it.
   The engine (`LoadGeoMaps`, `QueueGeoBuilds`, `CollectGeoInstances`) builds at most 24 BLAS /
   8 MiB of triangles a frame for pieces within `BB_RT_GEO_RADIUS` (50 m; 0 = off, live) of at
   least `BB_RT_GEO_MIN_TRIS` (32) triangles, and adds them as shadow-only instances unless the
   same placement is drawn or kept live. Which pieces the game would show is inferred from the
   ones it draws (`GeoVisible`): the collision whose display groups fit what was drawn in the
   last 180 frames; a file piece stands in only if every fitting collision shows it.

   To try it: `python tools/mapcache/build_cache.py <dvdroot_ps4> m24_01_00_00 <folder>` (and
   `m24_00_00_00`; about a minute and 0.6 GB each), then launch with `BB_RT_GEO_DIR=<folder>`.
   The test copies were deleted after the measurements.

   Measured at the bridge (moon debug view `BB_RT_DIRECT_DEBUG=11`, share of moon-facing pixels
   whose lit/shadowed state differs; `tools/rt-test/geo_truth.ps1`, `geo_gain.ps1`,
   `shadow_diff.py`):

   | Comparison | Result |
   |---|---|
   | Cache on against off, everything already seen (3 views) | 0.03 %, 0.17 %, 0.03 %: no false shadow |
   | Cache on against off, nothing seen yet (8 headings + the 3 views) | 0.03-0.16 %: nothing added either |
   | Floor of the method (same state twice) | about 0.03-0.2 % |
   | Before against after a full turn, cache off | 0.7-2.3 %, in red/yellow pairs along edges: the camera does not return to the same pixel, not a caster |
   | Pieces the cache adds | 575-588 (about 97 placements the game never draws from the bridge, also after a full turn) |
   | Frames in a 10.4 s turn, cache off / on | 580 / 557 (-4 %) |
   | RT CPU / GPU, cache off -> on | 5.7 -> 6.5 ms / 11.3 -> 12.1 ms |
   | BLAS pool | 460 -> 537 MiB |

   Other areas, asked for by the user and run with them standing there (2026-11-15,
   `geo_gain.ps1`: 8 headings, what the camera has seen dropped at each, file pieces on
   against off in the same frame):

   | Area | Reach | Pieces standing in | Pixels that change |
   |---|---|---|---|
   | Forbidden Woods, graveyard | 50 m | 353 | 0.00-0.22 % (the floor) in all 8 |
   | Upper Cathedral Ward, by the lamp | 50 m | 551 | 0.00 % in 7, 0.10 % in 1 |
   | Upper Cathedral Ward, same spot | 150 m | 889 | 0.24-0.25 % in 2 (lit to shadowed only: a patch high on a wall, a spot on the ground), 0.00-0.02 % in 6 |
   | the same, after the camera saw everything, pieces on against off | 150 m | | 16 pixels: no false shadow |

   At 150 m the BLAS builds no longer fit the frame: 3,290 failed for lack of ids or pool
   space during the test. So the file pieces do cast a few correct shadows early when they
   reach far, a quarter of a percent of the picture in two directions of eight, at a cost
   that is higher than the 4 % measured at 50 m. Still off by default, and step 4 stays
   unbuilt.

   Reading: after step 1 the pieces that matter for shadows at the bridge are drawn by the game
   itself in every heading tried; the file pieces that are not drawn cast nothing visible
   there. The first version did cast a false shadow (a far-view stand-in model, `m050004`,
   0.3 m in front of the real walls), which is what the draw-group rule removed. A cost of 4 %
   for no visible change does not go on by default; the code stays as the reader step 3 needs
   and as a switch for a spot where a missing caster is found.
3. **Moon and sun from the files**: the cloud draws name the active sky, the file gives the
   moon's place; replaces the moon cache as the seed.
   Status: **proven live 2026-11-15 in five maps** (Upper Cathedral Ward and the Forbidden
   Woods with the user playing, and, before that, the Central Yharnam bridge under the blood
   moon, Hunter's Dream, Old Yharnam), on by default (`BB_RT_MOON_FILES`). Details and the
   tables: `PATH_TRACING.md`, "The moon and sun from the game's files".
   - Index counts do not name a sky (six maps share the night sky's counts), so a cloud draw is
     matched by its index count and four of its model-space vertices, which equal the file's
     (`tools/mapcache/sky_table.py` writes `rt/rt_sky_table.h`; `rt/rt_sky_finder.h` matches).
   - New install, disc never looked at: the sky is named at the first sky search (4 frames into
     play) and the light points at the file's moon. At the bridge that direction lands on the
     drawn disc within a pixel or two (uv 0.370 0.772 against 0.369 0.774); the old search with
     an empty cache ended 12.3 degrees off.
   - Decided alone (the user was away): on by default, as the user's rule asks ("anchored to
     the sky's moon or sun in every map, no cache, no look"). A sky with no moon or sun in its
     files (Nightmare Frontier, chalice dungeons) keeps the game's direction until the search
     sees a disc, as the earlier rule says. Old Yharnam now takes its low sun from the first
     frame instead of after a look: 7 % of the lit/shadowed pixels change at the spawn, mean
     luma 40.7 -> 39.2.
   - Not checked live: the other maps. The draw-based
     seed in `git stash` ("wip: sky disc seed") is superseded and can be dropped.
4. **Cut-out textures for file-loaded foliage** so unseen trees do not cast solid shadows.
   Status: **not built**. It only matters with step 2 on, and step 2 adds no visible shadow at
   the test bed; alpha-tested file meshes stay out of the casters, so no leaf card can cast a
   solid shadow. Reading the game's texture files for it is only worth doing if a spot is found
   where the file pieces help.
5. **Remove what the steps replace** (occurrence shelving, kept probe, moon cache) once proven.
   Status: **closed as narrowed, 2026-11-15**: nothing more is to be deleted, by what the
   later sessions showed:
   - The kept probe is needed for good. Left out for shared meshes in step 1, it let the far
     low-detail twin of a block shade the real walls black (user report); it now also
     covers kept placements.
   - Shelving serves the static draws of the shaders whose matrices fail the probe check
     (5 rejected in a long session over three maps: characters, and one that draws both
     still and moving meshes): 12,000 of 304,800 draws, 4 %. Placing those would need trust
     per mesh instead of per shader, for a part of the code that costs nothing measurable.
   - The moon cache stays as the answer for a sky the table does not know.
   First assessment, kept for the record: Measured on a 20,000-frame session at the
   bridge after steps 1-3:
   - Moon cache: no longer read or written for a sky the table knows (45 skies, every map and
     the chalice dungeons). It stays as the answer for a sky the table does not know (another
     game version, a modded sky); deleting it would leave those with no memory at all.
   - Occurrence shelving and the kept probe: still at work for the draws that are not placed
     (7 shaders trusted, 4 rejected, 2 pending: shelving moved an entry and the kept probe
     dropped 40 kept copies in that session). They no longer touch placed pieces (a placed
     draw is never shelved; shared meshes are not probed as kept). Deleting them would bring
     the old rebuilds and ghosts back for those shaders, so they stay until every static
     shader is placed.

## Game patches found (not applied; `GEOMETRY_CACHE_STUDY.md` section 2)

"No per-piece test" (`0x0264e090=90e9b5030000`) keeps the blood moon submitted at every heading at
no measurable cost; only needed if step 3 does not cover a map.

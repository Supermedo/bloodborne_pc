# Geometry cache study: data (Bloodborne PS4 1.09 dump)

Offline and read-only. Source: the `dvdroot_ps4` folder of a Bloodborne 1.09 dump. Scripts and raw outputs are in this folder.
Tags: **[M]** = measured from the files; **[E]** = estimated (an assumption or extrapolation, stated each time).
Tris are triangles. M = 10^6. MB = 10^6 bytes. "Placed" = summed over every MSB part instance. "Unique" = each model x mesh counted once per map.
The sky (m900xxx, `sky` flag) is excluded from the near tables. Memory assumptions **[E]**: BLAS = 36 B/tri, TLAS = 64 B/instance.

## A. LOD encoding

- FLVER2 version 0x20014. A face set's LOD level is in its flags: `0` = LOD0, `0x01000000` = LOD1, `0x02000000` = LOD2 **[M]**.
  Every face set is a triangle list (no strips, no edge-compressed sets); no mesh has the dynamic flag **[M]**.
- The LODs of a mesh share one vertex buffer; only the index lists differ **[M]**. In m24_01, the lowest LOD
  references only 22.3% of the vertices of the meshes that have LODs **[M]**.
- Across all folders: 46,530 meshes have L0+L1+L2, 3,101 have 2 sets and 16,630 have L0 only **[M]** (`explore_flags.out`).
- The switch distances are in `param/drawparam/default_drawparam.parambnd.dcx` → `default_LodBank` (64 rows) **[M]**:
  row 0 default 15 m / 30 m; row 1 40 m / 1000 m; row 2 15 m / 1000 m; row 3 "no LOD" 1000 m / 2000 m; rows 4-7 = 0-3 without texture LOD; row 8 m22 10 m / 25 m.
  The row is chosen by the part entity byte +0x0D (255 = row 0, 3 = no LOD). **[E]** That byte is identified only by its correlation with the 【LODなし】 tags and with 1-set models (`check_lodid.py`).

| per map (placed models only) | m24_01 Central Yharnam | m24_00 Cathedral Ward |
|---|---|---|
| models placed / meshes [M] | 935 / 5,798 | 715 / 4,854 |
| meshes with 1 / 2 / 3 sets [M] | 2,622 / 98 / 3,078 | 3,853 / 160 / 841 |
| unique tris L0 / L1 / L2 (missing level falls back to the finer one) [M] | 33.31 / 25.63 / 18.01 M | 23.36 / 21.53 / 19.89 M |
| placed tris L0 / L1 / L2 [M] | 61.29 / 46.15 / 31.16 M | 47.05 / 39.85 / 32.91 M |
| LOD1/LOD0 per mesh, median (p10-p90) [M] | 0.615 (0.44-1.0) | 0.600 |
| LOD2/LOD0 per mesh, median (p10-p90) [M] | 0.213 (0.09-0.79) | 0.200 |
| unique tris in L0-only meshes [M] | 14.1 M | 18.7 M |

Far / impostor models **[M]** (no separate impostor format exists; they are ordinary single-LOD map pieces):
- m24_00: m05xxxx parts tagged 【常駐】【LODなし】遠景 (resident, no LOD, distant view), ~110 parts but only ~0.4 M tris.
  m508xxx are 3 distant-view parts.
- m24_01: m809839 and m809846 "廃墟の遠景" (distant view of Old Yharnam), 0.03 M tris.
- m008xxx: very large terrain/background models (m008021 2,236 x 2,384 m; m008020 1,193 x 1,557 m) that repeat the neighbour map's geometry at offset 0.
- Sky domes m900xxx (~5.4 k tris each, 2.9 km); m900090 is a lower cover dome.

Model families in m24_01 (prefix of the model number, `out_families.txt`) **[M]**: m009 = 2,024 parts / 157 models / 24.95 M placed L0 (repeated props, placed with a transform);
m000 1,189 parts / 5.11 M; m001-m008 one part per model, mostly at position (0,0,0) (geometry baked in world space); m05x distant-view pieces ~0.6 M in total.

## B. Triangles near a point

Columns: placed tris M / unique-model tris M (BLAS count = model x level is in `out_near.txt`). Distance = point to the part's world AABB **[M]**.
Mixes: "L0<50,L1" = L0 within 50 m, L1 beyond. "3-band" = L0 <30 m, L1 <100 m, lowest beyond. "game" = LodBank row per part (row distances as above; **[E]** the game measures distance in some other way, maybe camera to AABB).

m24_01 reference spot (-216, -30, 64), the Great Bridge of Central Yharnam (4,052 pieces) **[M]**:

| radius | parts | L0 | L1 | lowest | L0<50,L1 | 3-band | game |
|---|---|---|---|---|---|---|---|
| 25 m | 195 | 4.82 / 3.97 | 3.94 / 3.27 | 3.09 / 2.59 | 4.82 / 3.97 | 4.82 / 3.97 | 4.45 / 3.83 |
| 50 m | 530 | 12.29 / 9.18 | 9.84 / 7.42 | 7.41 / 5.68 | 12.29 / 9.18 | 11.14 / 8.64 | 9.20 / 7.33 |
| 100 m | 1730 | 28.93 / 16.86 | 22.24 / 13.48 | 15.63 / 10.13 | 24.69 / 15.80 | 23.54 / 14.92 | 17.96 / 12.35 |
| 150 m | 3032 | 48.05 / 27.28 | 36.20 / 21.07 | 24.48 / 14.92 | 38.66 / 23.52 | 32.39 / 19.97 | 27.14 / 17.42 |
| 250 m | 4047 | 61.09 / 33.19 | 45.96 / 25.51 | 30.98 / 17.91 | 48.42 / 28.00 | 38.90 / 23.00 | 33.65 / 20.41 |
| whole map | 4051 | 61.27 / 33.29 | 46.14 / 25.61 | 31.16 / 18.01 | 48.60 / 28.11 | 39.07 / 23.10 | 33.83 / 20.51 |

Unique models in radius **[M]**: 143 (25 m), 288 (50 m), 539 (100 m), 794 (150 m), 928 (250 m).

Other points. Each is a player-start part (c0000_*) in a different kind of map: hub town, foliage-heavy woods, and a large late map **[M]**:

| point | radius | L0 | lowest | 3-band | game |
|---|---|---|---|---|---|
| m24_00 Cathedral Ward c0000_0000 (21.2, -9.5, 100.6) | 25 m | 3.55 / 3.30 | 3.03 / 2.98 | 3.55 / 3.30 | 3.47 / 3.28 |
| | 100 m | 25.97 / 15.04 | 19.29 / 13.60 | 22.91 / 14.54 | 20.06 / 14.11 |
| | whole map | 47.03 / 23.34 | 32.91 / 19.88 | 36.54 / 20.91 | 34.10 / 20.50 |
| m27 Forbidden Woods c0000_0000 (-156.5, -40, 192.3) | 25 m | 2.58 / 1.95 | 0.63 / 0.39 | 2.58 / 1.95 | 2.17 / 1.68 |
| | 100 m | 7.79 / 5.38 | 1.99 / 1.08 | 5.76 / 4.03 | 3.69 / 2.52 |
| | whole map | 42.44 / 28.21 | 10.80 / 6.00 | 14.57 / 8.97 | 12.60 / 7.54 |
| m34 Hunter's Nightmare c0000_0008 "initial respawn" (-477, 1490.5, -500.6) | 25 m | 3.04 / 2.84 | 2.31 / 2.27 | 3.04 / 2.84 | 2.85 / 2.68 |
| | 100 m | 16.41 / 11.69 | 8.19 / 7.24 | 12.72 / 9.86 | 8.81 / 7.75 |
| | whole map | 34.94 / 18.99 | 14.09 / 10.79 | 18.62 / 13.71 | 15.14 / 11.64 |

Draw-group visibility sets (`drawgroups.py`): the pieces whose draw groups (part +0x4C) intersect a collision's display groups (+0x6C).
The meaning of these groups is taken from SoulsFormats **[E]**.

| map | collisions | median set, L0 tris (% of map) | q90 | widest set |
|---|---|---|---|---|
| m24_01 | 85 | 349 pieces, 6.83 M (11%) | 9.38 M (15%) | h008900: 60.15 M (98%) |
| m24_00 | 80 | 479 pieces, 5.91 M (13%) | 10.77 M (23%) | h009000: 100% |
| m27 | 80 | 432 pieces, 4.28 M (10%) | 6.16 M (15%) | h009000: 100% |
| m34 | 39 | 882 pieces, 10.96 M (31%) | 14.00 M (40%) | 19.70 M (56%) |

m24_01 has 149 cutscene-only pieces (1.18 M tris) that use their own draw-group bit **[M]** (`opcut.py`).

## C. Memory, parse time, cache size

GPU memory for m24_01, from the B table **[E]** (instanced = 36 B x unique tris + 64 B x parts; flat = 36 B x placed tris):

| m24_01 | L0 | L1 | lowest | L0<50,L1 | 3-band | game |
|---|---|---|---|---|---|---|
| 25 m | 143 (174) MB | 118 (142) | 93 (111) | 143 (174) | 143 (174) | 138 (160) |
| 100 m | 607 (1041) | 485 (801) | 365 (563) | 569 (889) | 537 (847) | 445 (647) |
| 250 m | 1195 (2199) | 919 (1655) | 645 (1115) | 1008 (1743) | 828 (1400) | 735 (1211) |
| whole map | 1199 (2206) | 922 (1661) | 649 (1122) | 1012 (1750) | 832 (1407) | 739 (1218) |

m24_00 whole map: L0 840 (1693) MB, lowest 716 (1185) MB **[E]** (`out_memory.txt`).
Whole game at lowest LOD (sum of 16 base maps, models counted once per map): 118.5 M unique tris → ~4.3 GB instanced; 178.0 M placed → ~6.4 GB flat **[E]**. L0 placed, whole game: 400.2 M **[M]**.

Parse time for one map (m24_01: 960 `.flver.dcx`, 859 MB compressed → 2.25 GB inflated), single-thread CPython **[M]** (`timing.py`):

| mode | time | throughput | peak working set |
|---|---|---|---|
| inflate only (zlib) | 6.31 s | 136 MB/s | 43 MB |
| inflate + header stat | 6.64 s | 129 MB/s | 57 MB |
| full `mapcache.load_map` (vertices + indices) | 16.41 s | 52.4 MB/s | 1,316 MB |

Files **[M]**: 11,033 `.flver.dcx` in total. 7,254 are outside m29 (6.95 GB); 3,779 are in m29 (4.17 GB, all in `m29_00_00_00`).
2,709 MSBs: 24 top-level and 2,685 in m29 chalice folders.
Full parse at 52.4 MB/s **[E]**: ~126 s for non-m29, plus ~76 s for m29; a native or multi-threaded parser would be faster.

On-disk cache size (`cachesize.py`; all LODs of every model in the folder, not only placed ones) **[M]**:

| folder | verts M | L0 tris M | lowest tris M | pos f32 MB | pos q16 MB | idx L0 MB (16/32 mixed) | idx all LODs | idx lowest | idx L0 all 32-bit |
|---|---|---|---|---|---|---|---|---|---|
| m24_01 | 55.73 | 33.77 | 18.12 | 638 | 319 | 244 | 357 | 131 | 387 |
| m24_00 | 36.00 | 23.53 | 19.97 | 412 | 206 | 150 | 174 | 127 | 269 |
| m27 | 41.73 | 28.21 | 6.00 | 478 | 239 | 210 | 374 | 44 | 323 |
| total non-m29 | 386.91 | 264.27 | 123.06 | 4,428 | 2,214 | 1,858 | 2,900 | 848 | 3,024 |
| m29 | 208.88 | 193.82 | 45.08 | 2,390 | 1,195 | 1,535 | 2,720 | 354 | 2,218 |

Index lists are 16-bit when the mesh has ≤65,536 vertices; 970 non-m29 meshes have more **[M]**. UVs of alpha-tested meshes: 200 MB non-m29 **[M]**.
Smallest useful non-m29 cache, positions q16 + lowest indices + alpha UV: ~3.3 GB **[E]**. Every LOD at f32: ~7.5 GB **[E]**. Uncompressed, before any zlib.

## D. Parts and event toggles

Part counts for each map are in `out_maps.txt`. Examples **[M]**:

| map | MapPiece (with entity id) | Object (with id) | Collision | ConnectCollision |
|---|---|---|---|---|
| m24_01 | 4,056 (8) | 1,692 (100) | 107 | 9 |
| m24_00 | 4,048 (14) | 804 (80) | 85 | 25 |
| m21_00 | 629 (0) | 107 (76) | 14 | 0 |
| m22_00 | 1,058 (4) | 203 (83) | 48 | 16 |
| m32_00 | 548 (0) | 1,689 (71) | 57 | 4 |

EMEVD toggles, parameters resolved through the 2000[00]/2000[06] init calls, names from `common.emedf` (`toggles.py`, `out_toggles.txt`) **[M]**:
- **m24_00**, 2012[01] MapPartEnable on 14 pieces:
  - skies 2404000-02;
  - boss pre-cutscene candles m060060 (2404010);
  - NPC event blood decals m073000-m073010 and m071800 (2404600-10);
  - church/cathedral windows, evening m075000/m076000 (2404700/01) and night m075001/m076001 (2404750/51).
  - 2005[03] ObjEnable: 19 objects (fog walls o2403xx, chests o002100, lamps o009900, the Vicar statue o248016 88 k tris).
- **m24_01**, 2012[01] on 8 pieces: skies 2414010-13; clinic and sewer windows, day m075000/m076000 (2414050/51) and night (2414070/71).
  2005[03]: 30 objects + 3 dummy objects (boss fog walls, chests, lamps).
- **m21_00** Hunter's Dream: no map-piece toggles. 2005[03] on 75 objects:
  - workshop doors closed/open o210010-13 (2101010-31);
  - moon o210020/21 and sky o210030/31 (normal vs final boss, 2101300-11);
  - chalice altars o210001.
- **m22_00**: 2012[01] on 2 pieces, the skies (2206000/01). Entity id "2" on m003201 is a false positive: a small number that matched unrelated arguments.
- **m32_00**: no map-piece toggles. Objects: 2005[03] on 10, 2005[08] destroy replay on 21, 2005[07] anim replay on 28, 2005[06] ObjAct on 28; desk o320010 is destroyed by 3201300.

Doors, lifts and breakables. Classified from model names and ObjectParam hp (> 0 read as breakable **[E]**); counts **[M]**:

| m24_01 objects | parts | placed tris |
|---|---|---|
| breakable (hp > 0) | 1,117 | 6.25 M |
| doors | 22 | 0.47 M |
| lift o243003 (エレベーター) | 3 | 0.05 M |
| items / chests | 52 | 0.67 M |
| fog walls | 3 | 0 |
| other | 495 | 2.05 M |
| total (105 models) | 1,692 | 9.49 M |

m24_00 objects: 804 parts, 3.34 M tris, including 149 "resident layout" static parts (0.70 M) **[M]**.
Doors open and close by object animation (2005[07] anim replay, 2010[05] tells the sound occlusion). They are not map pieces **[M]**.
Breakable state persists through `breakobj` files: header 'OBJB', and the int at +8 equals the map's Object part count **[M]**. The rest of the file is not decoded.

Classification for the cache **[E]**:
- **Safe static:** map pieces with no entity id. That is 99.8% of all map pieces: only 63 of 28,908 have an id (8 in m24_01, 14 in m24_00).
- **Follow a flag (bake both versions, pick per flag):**
  - the skies (2012[01]);
  - day/evening/night window pieces;
  - NPC blood decals;
  - boss candles;
  - in m21_00, the door and moon/sky objects driven by 2005[03].
- **Must stay live (dynamic TLAS from the game's own transforms):**
  - every Object part: doors, lifts, breakables, chests, fog walls;
  - all characters.
  - Breakables alone are 6.25 M placed tris in m24_01.

## E. Alpha-tested materials

MTD archive `mtd/allmaterialbnd.mtdbnd.dcx` (BND4, 304 MTDs) **[M]**.
- **Alpha test** = `g_AlphaRef.x > 0` with `g_BlendMode` 0/1. Values: `_e` 10, `_l_e` 128, `_building_l_e` 50, grass 128.
- **Alpha blend** = `g_BlendMode` 2 (`_al`, AlphaRef 1).
- Per-MTD values are in `mtd_params.tsv`.

| | m24_01 | m27 Forbidden Woods |
|---|---|---|
| alpha-test meshes (unique) [M] | 952 (16.4%) | 803 (33.3%) |
| alpha-test tris, unique / placed [M] | 3.53 M (10.6%) / 9.1% | 9.68 M (34.3%) / 29.0% |
| alpha-blend meshes / tris (unique) [M] | 120 (2.1%) / 0.2% | 21 (0.9%) / 0.1% |
| clusters by name [M]: grass | 47 meshes, 1.53 M tris | 414 meshes, 6.42 M |
| tree / leaf | 84, 0.09 M | 104, 0.70 M |
| other (ornaments, lamps, glass, moss) | 812 | 61 moss + 213 other |
| top MTDs [M] | M[ARSN]_l_e 700, building_l_e 174, M[A]_l_e_grass 47 | M[ARSN]_l_e 452, M[A]_l_e_grass 340 |
| diffuse found in `map/<area>/*.tpfbhd` [M] | 951 / 952 | 801 / 803 |
| unique alpha diffuse textures, stored size [M] | 71, 34.1 MB | 35, 14.6 MB |

- **Textures [M].** Every alpha mesh has `g_DiffuseTexture`. It is a `.tif` path, matched by base name to a TPF inside `map/mXX/mXX_000N.tpfbhd/.tpfbdt`.
  `.tpfbhd` is BHF4 (entries 0x24 bytes); `.tpfbdt` is BDF4 with DCX'd TPFs.
- **UVs [M].** UV member type 0x16 (UVPair) on 874 of 952 m24_01 alpha meshes. Grass uses 0x15 UV + 0x13 Byte4C.
  The UV short is divided by 2048: raw values fit 0..1 at that scale (`uv_check.py`).
- **What the cache must store per alpha mesh [E]:**
  - one UV set (2 x s16 or f16, 4 B/vertex);
  - a material id → diffuse texture + AlphaRef (10 / 50 / 128) + blend mode;
  - the textures themselves (or a reference to the game's loaded copy): ~34 MB for m24_01 as stored.

## F. Map loading and connections

- `worldmsblist` lists 17 MSBs with Japanese names. `mapviewlist` is a debug list. `mXX.arealoadlist` names one base MSB per area: m24 lists only m24_00; m29 lists 5 **[M]**.
- ConnectCollision parts name the neighbour map that is loaded when the player stands on that collision **[M]**. m24_00 is the hub:
  → m24_01 x9, m22 x4, m24_02 x3, m21_01 x3, m28 x3, m27 x2, m23 x1. m24_01 → m27 x5, m24_00 x4. m27 → m24_00 x9, m32 x2, m24_01 x1.
  m34 → m35 x2; m35 → m34 x5, m36 x4.
- **Shared coordinates [M]** (`shared_geo.py`, matched by geometry signature):
  - m24_00 and m24_02 share 70 meshes at offset 0 (133 k tris); m24_00 and m24_01 share 9 at offset 0. The three m24 maps share one origin.
  - m24_01 vs m27: all 88 matches sit at offset (-153.7, -61.7, 214.2), so m27 has its own origin. The offset table was not found in params or the exe.
- Variants `_00_01` / `_00_11` repeat their base map's pieces (same counts) **[M]**; that they are event/online variants is **[E]**.
- Map identities **[M]**, from MSB names, sky and connections. Correction to the earlier study: m24_01 is Central Yharnam and m24_00 is the Cathedral Ward.

| map | name | map pieces | models | placed L0 M | placed lowest M | unique L0 M | unique lowest M |
|---|---|---|---|---|---|---|---|
| m21_00 | Hunter's Dream | 629 | 119 | 7.42 | 1.59 | 5.23 | 1.05 |
| m21_01 | Abandoned Old Workshop | 337 | 88 | 3.43 | 0.71 | 2.44 | 0.50 |
| m22_00 | Hemwick Charnel Lane | 1,058 | 402 | 18.79 | 4.90 | 13.69 | 3.07 |
| m23_00 | Old Yharnam | 1,493 | 311 | 18.19 | 6.53 | 14.02 | 5.48 |
| m24_00 | Cathedral Ward (hub) | 4,045 | 715 | 47.05 | 32.91 | 23.36 | 19.89 |
| m24_01 | Central Yharnam | 4,056 | 935 | 61.29 | 31.16 | 33.31 | 18.01 |
| m24_02 | Upper Cathedral Ward | 613 | 186 | 11.84 | 7.91 | 9.36 | 6.71 |
| m25_00 | Cainhurst | 232 | 225 | 16.65 | 3.40 | 16.24 | 3.31 |
| m26_00 | Nightmare Frontier | 2,324 | 631 | 35.97 | 28.06 | 24.72 | 20.43 |
| m27_00 | Forbidden Woods | 2,871 | 945 | 42.45 | 10.80 | 28.21 | 6.00 |
| m28_00 | Yahar'gul | 2,496 | 454 | 28.60 | 18.43 | 20.57 | 14.09 |
| m32_00 | Byrgenwerth + Lecture Building | 548 | 177 | 8.66 | 2.12 | 5.98 | 1.32 |
| m33_00 | Nightmare of Mensis | 927 | 276 | 13.49 | 3.04 | 11.92 | 2.72 |
| m34_00 | Hunter's Nightmare (DLC) | 2,600 | 498 | 34.96 | 14.10 | 19.00 | 10.79 |
| m35_00 | Research Hall / Clocktower (DLC) | 3,512 | 255 | 19.84 | 5.37 | 6.88 | 2.27 |
| m36_00 | Fishing Hamlet (DLC) | 1,167 | 264 | 31.55 | 6.99 | 14.29 | 2.90 |
| **total** | 16 base maps | 28,908 | | 400.2 | 178.0 | | 118.5 |

Pieces that reference models not present in the folder: m24_00 m053018/m053053/m053090; m34 m001503/m001528/m001529/m001530 **[M]**.

## Not determined

- The world offset for each map (e.g. m27 at (-153.7, -61.7, 214.2) from m24): found by matching geometry, not by reading a table.
- How the game measures LodBank distance (camera or player; to AABB or origin), and whether entity byte +0x0D really is the LodBank row.
- Whether the 0x4C/0x6C groups really mean draw/display groups at runtime, and which collision is under the reference spot.
- The `breakobj` record layout, and whether ObjectParam hp > 0 really means breakable.
- What the arealoadlist entries mean; the exact flags behind the cutscene-only draw group.
- The Byte4C channel used by grass (vertex color or second UV).

# mapcache (step 0)

Offline reader for Bloodborne PS4 1.09 map files: places every static map piece of one map in
world space. Pure Python standard library (numpy is not installed in `C:\msys64\clang64`).

```
python mapcache.py dump <dvdroot_ps4> <map id> <out dir> [--obj]
python selfcheck.py <dvdroot_ps4> <map id> [x y z] [tri counts ...]
python sky_survey.py <dvdroot_ps4> <out.md> [--no-m29]
python build_cache.py <dvdroot_ps4> <map id> <out dir>
python sky_table.py <dvdroot_ps4> list | check | header <out.h>
```

`sky_table.py header` writes `gpu/shadps4/video_core/renderer_vulkan/rt/rt_sky_table.h`: every
sky part of every map with the world centre of its moon or sun and, per mesh and detail level,
the index count and the four vertices the engine's probe samples (as the vertex buffer holds
them: an object's node transform is not applied). `rt_sky_finder.h` matches the game's sky draws
to it (`docs/PATH_TRACING.md`, "The moon and sun from the game's files"). `check` lists the skies
that share draws; `list` prints every sky part and mesh.

`build_cache.py` writes `<map>.bbgeo`, the file the ray tracer reads with `BB_RT_GEO_DIR`
(layout in its docstring and in `rt/rt_geo_cache.h`; status in `docs/GEOMETRY_CACHE_PLAN.md`).
`study/groups_fit.py <map>.bbgeo <game log>` checks the draw-group rule against the pieces a
running game drew (`BB_RT_GEO_LOG=1`).

`dump` writes `<map>.pieces.tsv` (one row per part x mesh), `<map>.parts.tsv` (MSB fields of
each map piece) and, with `--obj`, `<map>.obj` (world-space positions + LOD-0 faces, one `o`
per part). m24_00_00_00 takes about 26 s (parse 9 s).

## Formats as found

**DCX** (all `.dcx` here): big-endian header, `DCX\0` version `0x00010000`, then `DCS\0`
(uncompressed size, compressed size), `DCP\0` `DFLT` (0x20, 0x09000000 ...), `DCA\0` 8, zlib
stream (`78 DA`) at 0x4C. Only DFLT is handled; anything else fails loudly.

**FLVER2**: `FLVER\0` `L\0` (little-endian), version `0x00020014` in all 731 m24_00 files,
unicode strings. Header 0x80 bytes: counts at 0x14 (dummies, materials, bones, meshes, vertex
buffers), model AABB 0x28/0x34, index size byte 0x48 (0 = per face set), face set / layout /
texture counts at 0x50. Then dummies (0x40), materials (0x20: name, MTD, texture count/index,
flags, gx offset), bones (0x80: T, name, R (radians), parent, child, S, siblings, AABB),
meshes (0x30: dynamic, material, default bone, bone count, AABB offset, bone list, face sets,
vertex buffers), face sets (0x20: flags, strip byte, cull byte, count, offset, byte length, index
size), vertex buffers (0x20: layout, stride, count, length, offset), layouts (0x10 ->
members 0x14: struct offset, type, semantic, index). Data offsets are relative to header 0x0C.

Seen in m24_00_00_00 (and m24_01_00_00, 960 files): every mesh static (`dynamic=0`, no bone
list), one vertex buffer, triangle **lists** only (no strips seen; strip code is written but
untested), 16-bit (6716 sets) and 32-bit (52) indices, face-set flags `0` (LOD0),
`0x01000000` (LOD1), `0x02000000` (LOD2); no motion-blur / edge-compressed sets.
Vertex members (type/semantic): Position Float3 (0x02) at offset 0 always; Normal and Tangent
Byte4B (0x11); BoneIndices Byte4B (0x11); BoneWeights Byte4C (0x13); VertexColor Byte4C (0x13);
UV as UVPair (0x16), UV (0x15), Byte4C (0x13), Short2toFloat2 (0x12). Layout sizes are checked
against the stride; an unknown type, a gap, a non-Float3/4 position, a dynamic mesh or a
missing LOD0 set fails that mesh with its file name.

**Node transform**: every file has exactly one bone, named after the model, identity
transform. Mesh `DefaultBoneIndex` is out of range in 2315 of 4901 meshes (values like 9-17
with one bone), so it is not a bone-table index here. The parser applies the single bone's
world transform (or `DefaultBoneIndex` if a file has several bones and it is in range, else
fails). Proof that positions are already model space: for all 4901 meshes the vertex AABB
equals the stored mesh AABB to 1e-3.

**MSB** (Bloodborne): `MSB ` 1, 0x10, then a chain of params (int version 3, offset count,
name offset, entry offsets, next-param offset): MODEL_PARAM_ST, EVENT_PARAM_ST,
POINT_PARAM_ST, PARTS_PARAM_ST. Model entry: name, type (0 map piece, 1 object, 2 enemy,
4 player, 5 collision, 6 navmesh), type index, SIB path, instance count. Part entry:
0x00 description offset, 0x08 name offset, 0x10 instance id (name suffix), 0x14 type,
0x18 type index, 0x1C model index, 0x20 SIB offset, 0x28 position, 0x34 rotation (degrees),
0x40 scale, 0x4C/0x6C/0x8C three u32[8] group arrays, 0xB0 entity-data offset (first int =
entity id used by EMEVD), 0xB8 type-data offset. Part types seen: 0 MapPiece, 1 Object,
2 Enemy, 4 Player, 5 Collision, 8 Navmesh, 9 DummyObject, 10 DummyEnemy, 11 ConnectCollision.
Map-piece model `m009001` -> `map/m24_00_00_00/m24_00_00_00_009001.flver.dcx`.

**Placement**: world = T * R * S * v, R = Ry * Rz * Rx (X applied first, then Z, then Y),
right-hand-rule matrices on the raw coordinates = System.Numerics
`CreateRotationX(x) * CreateRotationZ(z) * CreateRotationY(y)` in row-vector form, the same as
DSMapStudio's `Transform.EulerToQuaternion` and SoulsFormats FLVER bones. Coordinates are kept
in the game's own (left-handed, Y up, metres) space; no axis flip. The angle sign is
measured: neighbouring parts share 3x more exact world vertices with + than with - (4589 vs
1503 points in m24_01). The X/Z order could not be separated by data (tilted pieces share no
vertices with neighbours), it relies on DSMapStudio.

## Unknown / guessed

- Group arrays: 0x4C looks like draw groups (bits set on all map pieces), 0x6C is zero on map
  pieces and set on collisions (display groups?), 0x8C mixes `ffffffff` runs with small values
  and may not be a group array. Names from SoulsFormats, not verified.
- MSB rotation order for parts with large X/Z tilt (134 in m24_00, up to 90-180 deg).
- 3 map pieces of m24_00 reference models with no file anywhere in the dump
  (m053018, m053053, m053090, distant-view `distantview_low_layout`); 16 of the 731 FLVER
  files are not placed by any part (still parsed by `selfcheck.py`).
- `dynamic` meshes, triangle strips, edge-compressed sets: not present, so not handled.
- Placed triangle totals count every part, including distant-view (遠景) LOD pieces and all
  three sky variants; which one the game shows is decided by EMEVD flags 9800/9801/9802 (see SKY_SURVEY.md).

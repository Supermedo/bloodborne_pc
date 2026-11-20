// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// The game's own map files, read in the engine (the same rules as tools/mapcache/mapcache.py and
// build_cache.py, which stay the reference): a map's MSB lists every static map piece with its
// position, rotation, scale, model and draw groups; each model is a FLVER whose coarsest face set
// is the shadow caster. No Vulkan here: tools/mapcache/geo_check.cpp builds this file alone and
// compares it with build_cache.py's output.
namespace Vulkan::Rt::MapFiles {

// The whole payload of a DCX file (zlib "DFLT"), or the raw file if it is not a DCX. False when
// the file is missing or damaged.
bool ReadDcx(const std::string& path, std::vector<uint8_t>& out);

// Alpha class of every material definition (mtd/allmaterialbnd.mtdbnd.dcx), by lower-case base
// name: 0 opaque, 1 cut-out, 2 blended, 3 additive or water.
struct MtdTable {
    std::vector<std::pair<std::string, int8_t>> rows; // sorted by name
    bool Load(const std::string& dvdroot);
    // Mesh flags (GeoMeshAlphaTest/Blend/Moon/Sky) of a material.
    uint32_t MeshFlags(const std::string& material, const std::string& mtd_path) const;
};

struct MsbPart {
    std::string model;
    int32_t type = 0, entity_id = -1;
    float pos[3]{}, rot[3]{}, scale[3]{};
    std::array<uint32_t, 8> draw_groups{}, display_groups{};
};

bool ReadMsb(const std::vector<uint8_t>& data, std::vector<MsbPart>& parts);

// One mesh of a map-piece model as the cache keeps it.
struct ModelMesh {
    uint32_t flags = 0;
    uint32_t index_count = 0; // of the kept triangles
    uint32_t lods[3]{};
    float box[6]{};
    std::vector<float> soup; // 9 floats a triangle, model space (only when asked for)
};

// The meshes of a FLVER that stand in (the ones build_cache.py writes, in the same order).
// False when the file is not a FLVER2 the reader knows; meshes it cannot read are left out.
bool ReadModel(const std::vector<uint8_t>& data, const MtdTable& mtd, bool want_soup,
               std::vector<ModelMesh>& out);

// Column-vector 3x4 (row-major, translation in the 4th column) of an MSB placement.
std::array<float, 12> PartMatrix(const float pos[3], const float rot_deg[3], const float scale[3]);

// map/<id>/<id>_<model without its first letter>.flver.dcx
std::string ModelPath(const std::string& dvdroot, const std::string& map_id,
                      const std::string& model);

} // namespace Vulkan::Rt::MapFiles

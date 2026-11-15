// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "rt_gi.h"

// The map's static pieces from the game's own files (docs/GEOMETRY_CACHE_STUDY.md), for the
// ones the game has not drawn yet. The game frustum-culls map pieces, so a shadow caster the
// camera has not seen is missing from the TLAS: the same view was lit before a look away and
// shadowed after. tools/mapcache/build_cache.py writes one `<map>.bbgeo` per map from the
// player's own game folder: every placement (the same world matrix the game draws with, to four
// decimals) and the coarsest triangles of its model. This reader keeps the tables in memory and
// reads a mesh's triangles from the file when its BLAS is built.
namespace Vulkan::Rt {

constexpr uint32_t GeoVersion = 3;
constexpr uint32_t GeoMeshAlphaTest = 1, GeoMeshBlend = 2, GeoMeshMoon = 4, GeoMeshSky = 8;
constexpr uint32_t GeoPlaceEvent = 1, GeoPlaceSky = 2;

// 256 group bits (the game's draw and display groups).
using GeoGroups = std::array<uint32_t, 8>;

constexpr bool GeoGroupsMeet(const GeoGroups& a, const GeoGroups& b) {
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i] & b[i]) {
            return true;
        }
    }
    return false;
}

struct GeoHeader {
    char magic[8];
    uint32_t version, models, meshes, places, collisions, pad;
    uint64_t vertex_at, index_at, size;
};
static_assert(sizeof(GeoHeader) == 56);

struct GeoModel {
    char name[8];
    uint32_t first_mesh, mesh_count;
};
static_assert(sizeof(GeoModel) == 16);

struct GeoMesh {
    uint32_t first_vertex, vertex_count, first_index, index_count, flags;
    // Index counts of the detail levels the game may draw this mesh with (0 = no such level):
    // a live draw is matched to its file mesh by them.
    uint32_t lod0_indices, lod1_indices, lod2_indices;
    float box[6];
};
static_assert(sizeof(GeoMesh) == 56);

struct GeoPlace {
    uint32_t model, flags;
    float m[12]; // 3x4 row-major world matrix, translation in the 4th column
    float box[6];
    // The game draws the piece only while these share a bit with the display groups of the
    // collision under the player. A block of buildings has a low-detail far-view model lying
    // over it (m050004 over the Central Yharnam bridge houses) that the game shows only from
    // other parts of the map: as a caster up close it stood 0.3 m in front of the real walls and
    // shadowed a whole window recess.
    GeoGroups groups;
};
static_assert(sizeof(GeoPlace) == 112);

class GeoMap {
public:
    GeoMap() = default;
    GeoMap(const GeoMap&) = delete;
    GeoMap& operator=(const GeoMap&) = delete;
    GeoMap(GeoMap&& o) noexcept {
        *this = std::move(o);
    }
    GeoMap& operator=(GeoMap&& o) noexcept {
        Close();
        file = o.file;
        o.file = nullptr;
        header = o.header;
        name = std::move(o.name);
        models = std::move(o.models);
        meshes = std::move(o.meshes);
        places = std::move(o.places);
        collisions = std::move(o.collisions);
        cells = std::move(o.cells);
        return *this;
    }
    ~GeoMap() {
        Close();
    }

    bool Open(const std::string& path) {
        Close();
        file = std::fopen(path.c_str(), "rb");
        if (!file) {
            return false;
        }
        const bool ok = std::fread(&header, sizeof(header), 1, file) == 1 &&
                        std::memcmp(header.magic, "BBGEO1\0\0", 8) == 0 &&
                        header.version == GeoVersion && header.models < (1u << 20) &&
                        header.meshes < (1u << 22) && header.places < (1u << 22);
        if (ok) {
            models.resize(header.models);
            meshes.resize(header.meshes);
            places.resize(header.places);
            collisions.resize(std::min<uint32_t>(header.collisions, 4096));
        }
        if (!ok || !ReadAll(models) || !ReadAll(meshes) || !ReadAll(places) ||
            !ReadAll(collisions)) {
            Close();
            return false;
        }
        const size_t slash = path.find_last_of("/\\");
        name = path.substr(slash == std::string::npos ? 0 : slash + 1);
        cells.clear();
        cells.reserve(places.size());
        for (uint32_t i = 0; i < places.size(); ++i) {
            cells.emplace(Cell(places[i].m[3], places[i].m[7], places[i].m[11]), i);
        }
        return true;
    }

    bool Valid() const {
        return file != nullptr;
    }

    // The placement with this world matrix (the game's, from its vertex shader constants), or -1.
    // Translation within 5 mm, the rest within 1e-3: the file's matrix comes from the same
    // position, rotation and scale the game composes its own from.
    int32_t FindPlace(const Affine& m) const {
        // The neighbour cells too: a value next to a cell edge may round either way.
        for (int dx = -1; dx <= 1; ++dx) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dz = -1; dz <= 1; ++dz) {
                    const auto [it, end] =
                        cells.equal_range(Cell(m[3] + CellSize * float(dx), m[7] + CellSize * float(dy),
                                               m[11] + CellSize * float(dz)));
                    for (auto at = it; at != end; ++at) {
                        if (SameMatrix(places[at->second].m, m)) {
                            return int32_t(at->second);
                        }
                    }
                }
            }
        }
        return -1;
    }

    // The mesh of a placement's model that the game draws with this many indices, or -1.
    int32_t FindMesh(uint32_t place, uint32_t index_count) const {
        if (place >= places.size() || places[place].model >= models.size() || !index_count) {
            return -1;
        }
        const GeoModel& model = models[places[place].model];
        for (uint32_t k = 0; k < model.mesh_count; ++k) {
            const GeoMesh& mesh = meshes[model.first_mesh + k];
            if (mesh.lod0_indices == index_count || mesh.lod1_indices == index_count ||
                mesh.lod2_indices == index_count) {
                return int32_t(model.first_mesh + k);
            }
        }
        return -1;
    }

    // The mesh's triangles as a soup: 9 floats a triangle, model space. False on a read error.
    bool ReadSoup(uint32_t mesh_index, float* out, std::vector<float>& vertices,
                  std::vector<uint32_t>& indices) {
        if (!file || mesh_index >= meshes.size()) {
            return false;
        }
        const GeoMesh& mesh = meshes[mesh_index];
        vertices.resize(size_t(mesh.vertex_count) * 3);
        indices.resize(mesh.index_count);
        if (!ReadAt(header.vertex_at + uint64_t(mesh.first_vertex) * 12, vertices.data(),
                    vertices.size() * 4) ||
            !ReadAt(header.index_at + uint64_t(mesh.first_index) * 4, indices.data(),
                    indices.size() * 4)) {
            return false;
        }
        for (size_t i = 0; i < indices.size(); ++i) {
            const uint32_t v = indices[i] < mesh.vertex_count ? indices[i] : 0u;
            std::memcpy(out + i * 3, vertices.data() + size_t(v) * 3, 12);
        }
        return true;
    }

    std::string name;
    std::vector<GeoModel> models;
    std::vector<GeoMesh> meshes;
    std::vector<GeoPlace> places;
    // The distinct display-group sets of the map's collisions.
    std::vector<GeoGroups> collisions;

private:
    static constexpr float CellSize = 0.02f;

    static uint64_t Cell(float x, float y, float z) {
        const auto q = [](float v) { return uint64_t(int64_t(std::floor(v / CellSize)) & 0x1FFFFF); };
        return q(x) | q(y) << 21 | q(z) << 42;
    }

    static bool SameMatrix(const float* a, const Affine& b) {
        for (int i = 0; i < 12; ++i) {
            const float tolerance = i % 4 == 3 ? 5.0e-3f : 1.0e-3f;
            if (!(std::fabs(a[i] - b[size_t(i)]) <= tolerance)) {
                return false;
            }
        }
        return true;
    }

    template <typename T>
    bool ReadAll(std::vector<T>& v) {
        return v.empty() || std::fread(v.data(), sizeof(T), v.size(), file) == v.size();
    }

    bool ReadAt(uint64_t offset, void* out, size_t bytes) {
#ifdef _WIN32
        if (_fseeki64(file, int64_t(offset), SEEK_SET) != 0) {
            return false;
        }
#else
        if (fseeko(file, off_t(offset), SEEK_SET) != 0) {
            return false;
        }
#endif
        return bytes == 0 || std::fread(out, 1, bytes, file) == bytes;
    }

    void Close() {
        if (file) {
            std::fclose(file);
            file = nullptr;
        }
    }

    std::FILE* file = nullptr;
    GeoHeader header{};
    std::unordered_multimap<uint64_t, uint32_t> cells;
};

// Distance from a point to an axis-aligned box (0 inside).
inline float BoxDistance(const float* box, const std::array<float, 3>& p) {
    float d2 = 0.0f;
    for (int a = 0; a < 3; ++a) {
        const float lo = box[a], hi = box[a + 3];
        const float d = p[size_t(a)] < lo ? lo - p[size_t(a)] : p[size_t(a)] > hi ? p[size_t(a)] - hi : 0.0f;
        d2 += d * d;
    }
    return std::sqrt(d2);
}

// A file mesh may stand in as a shadow caster: opaque (a cut-out without its texture would cast
// a solid shadow), not part of the sky, and worth an instance.
inline bool GeoCaster(const GeoMesh& mesh, uint32_t min_triangles) {
    return !(mesh.flags & (GeoMeshAlphaTest | GeoMeshBlend | GeoMeshMoon | GeoMeshSky)) &&
           mesh.index_count >= min_triangles * 3;
}

// A placement the cache may show: not a sky model, and not one an event script can hide (63 of
// 28,908 pieces: time-of-day windows, candles, blood decals).
inline bool GeoPlaceable(const GeoPlace& place) {
    return !(place.flags & (GeoPlaceEvent | GeoPlaceSky));
}

// Which collision is the player on? The engine does not know, but the pieces the game draws
// tell: nearly all of them share a group bit with that collision's display groups (measured on
// the Central Yharnam bridge: one set shows 15 of the 16 placements drawn, the next best 14, the
// odd piece carries a bit of its own). The pieces drawn in the last RecentFrames are counted
// against every set (Update); the set that misses the fewest fits, ties with it too, and a
// file piece may stand in only if every fitting set shows it (Shows), so a wrong guess can
// only leave a piece out, never add one the game would not show there. Sets with more than
// 100 bits (a catch-all that shows the whole map) are no evidence; when even the best set
// misses over a tenth of the pieces, nothing stands in.
struct GeoVisible {
    static constexpr uint64_t RecentFrames = 180;

    std::vector<uint8_t> fits;
    std::unordered_map<uint32_t, uint64_t> recent; // placement -> last frame the game drew it
    uint32_t noted = 0, best_misses = 0;

    void Begin(const GeoMap& map) {
        fits.assign(map.collisions.size(), 0);
        recent.clear();
        noted = best_misses = 0;
    }
    void Note(uint32_t place, uint64_t frame) {
        recent[place] = frame;
    }
    void Update(const GeoMap& map, uint64_t frame) {
        std::erase_if(recent, [&](const auto& item) { return item.second + RecentFrames < frame; });
        noted = uint32_t(recent.size());
        fits.assign(map.collisions.size(), 0);
        if (recent.empty()) {
            return;
        }
        std::vector<uint32_t> misses(map.collisions.size(), 0);
        uint32_t best = ~0u;
        for (size_t k = 0; k < map.collisions.size(); ++k) {
            uint32_t bits = 0;
            for (const uint32_t word : map.collisions[k]) {
                bits += uint32_t(__builtin_popcount(word));
            }
            if (bits > 100) {
                misses[k] = ~0u;
                continue;
            }
            for (const auto& [place, seen] : recent) {
                misses[k] += GeoGroupsMeet(map.places[place].groups, map.collisions[k]) ? 0u : 1u;
            }
            best = std::min(best, misses[k]);
        }
        best_misses = best;
        if (best == ~0u || best > std::max<uint32_t>(2, noted / 10)) {
            return;
        }
        for (size_t k = 0; k < fits.size(); ++k) {
            fits[k] = misses[k] == best ? 1 : 0;
        }
    }
    // How many collision sets fit; 0 = unknown (nothing stands in).
    uint32_t Fitting() const {
        uint32_t n = 0;
        for (const uint8_t f : fits) {
            n += f;
        }
        return n;
    }
    bool Shows(const GeoMap& map, const GeoPlace& place) const {
        bool any = false;
        for (size_t k = 0; k < fits.size(); ++k) {
            if (!fits[k]) {
                continue;
            }
            if (!GeoGroupsMeet(place.groups, map.collisions[k])) {
                return false;
            }
            any = true;
        }
        return any;
    }
};

} // namespace Vulkan::Rt

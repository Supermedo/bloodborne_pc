#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

namespace Vulkan::Rt {

struct DrawRecord {
    uint32_t triangle_base{};
    uint32_t triangle_count{};
    uint32_t store_base{};
    uint32_t index_lo{};
    uint32_t vertex_count{};
    uint32_t prims_per_instance{};
    uint32_t index_offset{};
    uint32_t flags{};
    uint32_t uv_base{0xFFFFFFFFu};
    uint32_t pad0{}, pad1{}, pad2{};
    std::array<float, 12> rebase{};
};
static_assert(sizeof(DrawRecord) == 96);

enum : uint32_t {
    FlagIndex16 = 1,
    FlagIndex32 = 2,
    FlagRestart = 4,
    FlagHasUv = 8,
    FlagHasNormal = 16,
    FlagRebase = 32
};

constexpr uint32_t VertexStride = 12;
constexpr uint32_t TriangleBytes = 3 * VertexStride;

struct DrawKey {
    uint64_t shader{}, geometry{}, indices{}, topology{};
    uint32_t index_count{}, instances{}, first_instance{}, first_vertex{}, vertex_count{};
    uint32_t occurrence{};
    bool operator==(const DrawKey&) const = default;
};

struct DrawKeyHash {
    size_t operator()(const DrawKey& k) const {
        uint64_t h = 0;
        const auto mix = [&](uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2); };
        mix(k.shader);
        mix(k.geometry);
        mix(k.indices);
        mix(k.topology);
        mix(uint64_t(k.index_count) << 32 | k.instances);
        mix(uint64_t(k.first_instance) << 32 | k.first_vertex);
        mix(uint64_t(k.vertex_count) << 32 | k.occurrence);
        return size_t(h);
    }
};

constexpr bool DepthOnlyDraw(uint32_t mrt_mask) {
    return mrt_mask == 0;
}

constexpr uint32_t MaskSolid = 0x01;
constexpr uint32_t MaskBlended = 0x02;
constexpr uint32_t MaskKept = 0x04;
// Painted window panes (BB_RT_PORTAL_PS) get a mask bit of their own, so a shadow ray can find
// the nearest pane with a query over the panes alone and then run the usual any-hit ray up
// to it (rt_direct.comp / rt_gi.comp Transmittance), instead of a closest-hit search.
constexpr uint32_t MaskPortal = 0x08;
constexpr uint32_t MaskWater = 0x10;
// The game's moon and cloud layer (rt_gi.h SkyLayerPs): only the sky layer pass traces it.
// Every other ray mask leaves this bit out (0x6F / 0x7F in the shaders).
constexpr uint32_t MaskSky = 0x80;

struct DrawTag {
    uint64_t ps{}, vs{};
    uint64_t cb0{}, zbuf{};
    uint32_t mrt_mask{}, bound_mask{}, target_mask{};
    uint32_t zfunc{}, zwrite{}, ztest{}, stencil{};
    uint32_t prim{}, caster{};
    uint32_t draw{};
    // The draw's cull mode (bit 0 front, bit 1 back; 0 = double-sided).
    uint32_t cull{};
};

constexpr uint8_t InstanceState(bool alpha_test, bool textured, uint32_t mask) {
    return uint8_t(1u | (alpha_test ? 2u : 0u) | (textured ? 4u : 0u) | (mask ? 8u : 0u));
}

constexpr bool AlphaUnresolved(bool alpha_test, bool texture, bool uv) {
    return alpha_test && !(texture && uv);
}

constexpr uint32_t InstanceMask(bool hidden, bool blended, bool unresolved, bool skip_unresolved,
                                bool kept_shadow_only = false, bool portal = false,
                                bool water = false) {
    if (hidden || (unresolved && skip_unresolved)) {
        return 0x00;
    }
    return water ? MaskWater : portal ? MaskPortal : blended ? MaskBlended : kept_shadow_only ? MaskKept : MaskSolid;
}

inline bool ParsePick(const char* text, std::array<float, 2>& out) {
    if (!text || !text[0]) {
        return false;
    }
    char* end = nullptr;
    const float x = std::strtof(text, &end);
    if (end == text || *end != ',') {
        return false;
    }
    const char* second = end + 1;
    const float y = std::strtof(second, &end);
    if (end == second || !(x >= 0.0f && x <= 1.0f && y >= 0.0f && y <= 1.0f)) {
        return false;
    }
    out = {x, y};
    return true;
}

inline std::vector<uint64_t> ParseHashList(const char* text) {
    std::vector<uint64_t> out;
    while (text && *text) {
        char* end = nullptr;
        const uint64_t value = std::strtoull(text, &end, 16);
        if (end == text) {
            ++text;
            continue;
        }
        if (value) {
            out.push_back(value);
        }
        text = end;
    }
    return out;
}

inline bool HashListed(const std::vector<uint64_t>& list, uint64_t ps, uint64_t vs) {
    return std::find_if(list.begin(), list.end(), [&](uint64_t h) {
               return (ps && h == ps) || (vs && h == vs);
           }) != list.end();
}

enum class PickRaster : uint32_t { Offscreen, Behind, Visible, Ghost };

constexpr PickRaster PickRasterCode(bool inside, bool raster_surface, float raster_z, float hit_z) {
    if (!inside) {
        return PickRaster::Offscreen;
    }
    if (!raster_surface) {
        return PickRaster::Ghost;
    }
    const float r = raster_z < 0.0f ? -raster_z : raster_z;
    const float h = hit_z < 0.0f ? -hit_z : hit_z;
    const float tol = 0.05f + 0.03f * h;
    if (r < h - tol) {
        return PickRaster::Behind;
    }
    return r > h + tol ? PickRaster::Ghost : PickRaster::Visible;
}

struct DrawInput {
    DrawKey key{};
    uint64_t palette{};
    bool skinned{};
    bool unreadable{};
    uint32_t store_base{};
    uint32_t first_vertex{};
    uint32_t vertex_count{};
    uint32_t instances{1};
    int32_t base_vertex{};
    const void* indices{};
    uint32_t index_count{};
    uint32_t index_size{};
    bool restart{};
    bool alpha_test{};
    bool uv_captured{};
    bool normal_captured{};
    bool depth_only{};
    float alpha_ref{0.5f};
    bool blended{};
    DrawTag tag{};
};

struct Capacity {
    uint32_t triangles{};
    uint32_t index_bytes{};
    uint32_t records{};
};

struct BlasInput {
    uint64_t vertex_bytes{};
    uint32_t max_vertex{};
    uint32_t primitives{};
};
constexpr BlasInput BlasInputFor(uint32_t triangles) {
    return {uint64_t(triangles) * TriangleBytes, triangles ? triangles * 3 - 1 : 0, triangles};
}

constexpr std::array<float, 3> ClipToView(const std::array<float, 4>& clip, float xs, float ys) {
    return {clip[0] / xs, clip[1] / ys, clip[3]};
}

constexpr std::array<float, 3> RayDirection(std::array<float, 2> ndc, float xs, float ys) {
    return {ndc[0] / xs, ndc[1] / ys, 1.0f};
}

class FrameGeometry {
public:
    struct Stats {
        uint64_t draws{}, accepted{}, capacity{}, invalid{};
    } stats;

    void Begin(DrawRecord* records_, uint8_t* indices_, Capacity capacity_) {
        records = records_;
        indices = indices_;
        capacity = capacity_;
        record_count = triangles = index_bytes = 0;
    }

    static bool Valid(const DrawInput& in) {
        const int64_t lo = int64_t(in.first_vertex) - in.base_vertex;
        return in.store_base && in.vertex_count && in.instances && in.index_count >= 3 &&
               in.index_count % 3 == 0 && lo >= 0 && lo <= std::numeric_limits<uint32_t>::max() &&
               (in.index_size == 0 || in.index_size == 2 || in.index_size == 4) &&
               (in.index_size == 0 || in.indices) && !HasRestartMarker(in);
    }

    static uint64_t TriangleCount(const DrawInput& in) {
        return uint64_t(in.index_count / 3) * in.instances;
    }

    bool Add(const DrawInput& in) {
        ++stats.draws;
        const int64_t lo = int64_t(in.first_vertex) - in.base_vertex;
        if (!Valid(in)) {
            ++stats.invalid;
            return false;
        }
        const uint32_t prims = in.index_count / 3;
        const uint64_t count = TriangleCount(in);
        const uint64_t bytes = (uint64_t(in.index_count) * in.index_size + 3) & ~uint64_t(3);
        if (count > capacity.triangles - triangles || bytes > capacity.index_bytes - index_bytes ||
            record_count >= capacity.records) {
            ++stats.capacity;
            return false;
        }
        records[record_count++] = {
            .triangle_base = triangles,
            .triangle_count = uint32_t(count),
            .store_base = in.store_base,
            .index_lo = uint32_t(lo),
            .vertex_count = in.vertex_count,
            .prims_per_instance = prims,
            .index_offset = index_bytes,
            .flags = (in.index_size == 2 ? FlagIndex16 : in.index_size == 4 ? FlagIndex32 : 0u) |
                     (in.restart && in.index_size ? FlagRestart : 0u),
            .uv_base = 0xFFFFFFFFu,
        };
        if (in.index_size) {
            std::memcpy(indices + index_bytes, in.indices, size_t(in.index_count) * in.index_size);
        }
        triangles += uint32_t(count);
        index_bytes += uint32_t(bytes);
        ++stats.accepted;
        return true;
    }

    void SetLastUvBase(uint32_t uv_base, bool uv_captured = true, bool normal_captured = false) {
        if (record_count) {
            DrawRecord& r = records[record_count - 1];
            r.uv_base = uv_base;
            r.flags = (r.flags & ~uint32_t(FlagHasUv | FlagHasNormal)) |
                      (uv_captured ? FlagHasUv : 0u) | (normal_captured ? FlagHasNormal : 0u);
        }
    }

    void SetRebase(uint32_t record, const std::array<float, 12>& rebase) {
        if (record < record_count) {
            records[record].rebase = rebase;
            records[record].flags |= FlagRebase;
        }
    }

    void PopLast() {
        if (!record_count) {
            return;
        }
        const DrawRecord& r = records[--record_count];
        triangles = r.triangle_base;
        index_bytes = r.index_offset;
        --stats.accepted;
    }

    uint32_t Records() const { return record_count; }
    uint32_t Triangles() const { return triangles; }
    uint32_t IndexBytes() const { return index_bytes; }

private:
    static bool HasRestartMarker(const DrawInput& in) {
        if (!in.restart || !in.indices) {
            return false;
        }
        if (in.index_size == 2) {
            const auto* p = static_cast<const uint16_t*>(in.indices);
            return std::find(p, p + in.index_count, uint16_t(0xFFFF)) != p + in.index_count;
        }
        if (in.index_size == 4) {
            const auto* p = static_cast<const uint32_t*>(in.indices);
            return std::find(p, p + in.index_count, 0xFFFFFFFFu) != p + in.index_count;
        }
        return false;
    }

    DrawRecord* records{};
    uint8_t* indices{};
    Capacity capacity{};
    uint32_t record_count{}, triangles{}, index_bytes{};
};

inline std::array<std::array<float, 3>, 3> GatherTriangle(std::span<const DrawRecord> records,
                                                          std::span<const uint8_t> indices,
                                                          std::span<const std::array<float, 4>> positions,
                                                          uint32_t triangle, float xs, float ys) {
    constexpr float nan = std::numeric_limits<float>::quiet_NaN();
    std::array<std::array<float, 3>, 3> out{{{nan, nan, nan}, {nan, nan, nan}, {nan, nan, nan}}};
    size_t lo = 0, hi = records.size();
    while (hi - lo > 1) {
        const size_t mid = (lo + hi) / 2;
        if (records[mid].triangle_base <= triangle) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    if (records.empty() || triangle - records[lo].triangle_base >= records[lo].triangle_count) {
        return out;
    }
    const DrawRecord& r = records[lo];
    const uint32_t local = triangle - r.triangle_base;
    const uint32_t instance = local / r.prims_per_instance;
    const uint32_t prim = local - instance * r.prims_per_instance;
    const uint32_t marker = r.flags & FlagIndex16 ? 0xFFFFu : 0xFFFFFFFFu;
    std::array<std::array<float, 3>, 3> vertices{};
    for (uint32_t k = 0; k < 3; ++k) {
        const uint32_t i = prim * 3 + k;
        uint32_t index = i;
        if (r.flags & FlagIndex16) {
            uint16_t v;
            std::memcpy(&v, indices.data() + r.index_offset + size_t(i) * 2, 2);
            index = v;
        } else if (r.flags & FlagIndex32) {
            std::memcpy(&index, indices.data() + r.index_offset + size_t(i) * 4, 4);
        }
        if (((r.flags & FlagRestart) && index == marker) || index < r.index_lo ||
            index - r.index_lo >= r.vertex_count) {
            return out;
        }
        const uint64_t slot = uint64_t(r.store_base) + uint64_t(instance) * r.vertex_count +
                              (index - r.index_lo);
        if (slot >= positions.size()) {
            return out;
        }
        vertices[k] = ClipToView(positions[size_t(slot)], xs, ys);
        if (r.flags & FlagRebase) {
            const auto& m = r.rebase;
            const auto v = vertices[k];
            for (int row = 0; row < 3; ++row) {
                vertices[k][row] = m[row * 4] * v[0] + m[row * 4 + 1] * v[1] +
                                   m[row * 4 + 2] * v[2] + m[row * 4 + 3];
            }
        }
        for (const float v : vertices[k]) {
            if (!std::isfinite(v)) {
                return out;
            }
        }
    }
    return vertices;
}

}

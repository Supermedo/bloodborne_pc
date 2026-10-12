// SPDX-License-Identifier: GPL-2.0-or-later
#include "remix_scene.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace BbRemix {
bool SceneCamera::Valid() const {
    for (float f : inverse_view) if (!std::isfinite(f)) return false;
    for (float f : projection) if (!std::isfinite(f)) return false;
    const auto* m=inverse_view;
    const float determinant=m[0]*(m[5]*m[10]-m[6]*m[9])-m[1]*(m[4]*m[10]-m[6]*m[8])+m[2]*(m[4]*m[9]-m[5]*m[8]);
    return std::abs(projection[0]) > 1e-6f && std::abs(projection[1]) > 1e-6f &&
           std::abs(projection[2]) > 1e-6f && std::abs(projection[3]) > 1e-6f &&
           std::abs(determinant)>1e-6f;
}
SceneMesh BuildMesh(std::span<const CapturedVertex> captured,
                    std::span<const uint32_t> indices, Topology topology,
                    const SceneCamera& camera) {
    SceneMesh result;
    if (!camera.Valid() || captured.empty() || captured.size() > (2u << 20) ||
        indices.size() > (8u << 20)) return result;
    std::vector<Vertex> world(captured.size());
    std::vector<bool> valid(captured.size());
    for (size_t i = 0; i < captured.size(); ++i) {
        const auto& c = captured[i];
        bool finite = c.written == 1;
        for (float f : c.clip) finite &= std::isfinite(f);
        for (float f : c.uv) finite &= std::isfinite(f);
        // Use homogeneous clip coordinates, including vertices behind the camera.
        // In a conventional perspective projection, clip.w IS view-space z.
        if (!finite || std::abs(c.clip[3]) > 1e6f) continue;
        const float view[3]{c.clip[0] / camera.projection[0],
                            c.clip[1] / camera.projection[1], c.clip[3]};
        for (unsigned r = 0; r < 3; ++r) {
            const float* row = camera.inverse_view + r * 4;
            const float value = row[0] * view[0] + row[1] * view[1] + row[2] * view[2] + row[3];
            finite &= std::isfinite(value) && std::abs(value) < 1e6f;
            // Stable millimetre positions avoid rebuilding static BLAS for camera roundoff.
            world[i].position[r] = std::round(value * 1024.f) / 1024.f;
        }
        std::copy(std::begin(c.uv), std::end(c.uv), world[i].uv);
        valid[i] = finite;
    }
    std::vector<uint32_t> remap(captured.size(), UINT32_MAX);
    struct VertexKey { std::array<uint32_t,5> bits; bool operator==(const VertexKey&) const = default; };
    struct Hash { size_t operator()(const VertexKey& k) const {
        size_t result=0; for(uint32_t value:k.bits) result^=size_t(value)+0x9e3779b9+(result<<6)+(result>>2); return result;
    }};
    std::unordered_map<VertexKey,uint32_t,Hash> welded;
    auto triangle = [&](uint32_t a, uint32_t b, uint32_t c) {
        if (a >= valid.size() || b >= valid.size() || c >= valid.size() ||
            a == b || b == c || a == c || !valid[a] || !valid[b] || !valid[c]) return;
        float u[3], v[3];
        for (unsigned j = 0; j < 3; ++j) {
            u[j] = world[b].position[j] - world[a].position[j];
            v[j] = world[c].position[j] - world[a].position[j];
        }
        const float n[3]{u[1]*v[2]-u[2]*v[1], u[2]*v[0]-u[0]*v[2], u[0]*v[1]-u[1]*v[0]};
        const float area = n[0]*n[0]+n[1]*n[1]+n[2]*n[2];
        if (!std::isfinite(area) || area < 1e-16f) return;
        for (uint32_t index : {a,b,c}) {
            if (remap[index] == UINT32_MAX) {
                const auto& vertex=world[index];
                VertexKey key{{std::bit_cast<uint32_t>(vertex.position[0]),std::bit_cast<uint32_t>(vertex.position[1]),
                    std::bit_cast<uint32_t>(vertex.position[2]),std::bit_cast<uint32_t>(vertex.uv[0]),std::bit_cast<uint32_t>(vertex.uv[1])}};
                const auto [it, inserted]=welded.try_emplace(key,uint32_t(result.vertices.size()));
                remap[index]=it->second;
                if(inserted) result.vertices.push_back(vertex);
            }
            auto& vertex = result.vertices[remap[index]];
            for (unsigned j = 0; j < 3; ++j) vertex.normal[j] += n[j];
            result.indices.push_back(remap[index]);
        }
    };
    if (topology == Topology::Triangles) {
        for (size_t i = 0; i + 2 < indices.size(); i += 3)
            triangle(indices[i], indices[i+1], indices[i+2]);
    } else {
        uint32_t previous[2]{}; unsigned count = 0;
        for (uint32_t index : indices) {
            if (index == UINT32_MAX) { count = 0; continue; }
            if (count >= 2) {
                if (count & 1) triangle(previous[1], previous[0], index);
                else triangle(previous[0], previous[1], index);
            }
            previous[0] = previous[1]; previous[1] = index; ++count;
        }
    }
    std::array<float,3> lo{INFINITY,INFINITY,INFINITY}, hi{-INFINITY,-INFINITY,-INFINITY};
    // FNV over initialized scalar values only; no padding, addresses or camera-dependent keys.
    uint64_t hash = 14695981039346656037ull;
    auto mix = [&](const void* data, size_t size) {
        for (size_t i=0; i<size; ++i) { hash ^= static_cast<const uint8_t*>(data)[i]; hash *= 1099511628211ull; }
    };
    for (auto& vertex : result.vertices) {
        const float length = std::sqrt(vertex.normal[0]*vertex.normal[0] +
            vertex.normal[1]*vertex.normal[1] + vertex.normal[2]*vertex.normal[2]);
        if (length > 1e-8f) for (float& f : vertex.normal) f /= length;
        else { vertex.normal[0] = vertex.normal[2] = 0; vertex.normal[1] = 1; }
        for (unsigned j=0; j<3; ++j) {
            lo[j] = std::min(lo[j], vertex.position[j]); hi[j] = std::max(hi[j], vertex.position[j]);
        }
        mix(&vertex, sizeof(vertex));
    }
    mix(result.indices.data(), result.indices.size()*sizeof(uint32_t));
    if (!result.vertices.empty()) for (unsigned j=0; j<3; ++j) result.center[j] = (lo[j]+hi[j])*0.5f;
    result.revision = hash ? hash : 1;
    return result;
}
} // namespace BbRemix

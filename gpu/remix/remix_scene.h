// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace BbRemix {
// Vertex shader output, before viewport/depth conversion. No guessed guest vertex layout.
struct CapturedVertex {
    float clip[4];
    float uv[2];
    float written;
    float pad;
};
static_assert(sizeof(CapturedVertex) == 32);
struct Vertex {
    float position[3], normal[3], uv[2];
};
static_assert(sizeof(Vertex) == 32);
struct SceneCamera {
    float inverse_view[12]{}; // row-major affine, view -> world
    float projection[4]{};   // x, y, z scale and z offset, before viewport signs
    float viewport_sign[2]{1, -1};
    bool Valid() const;
};
enum class Topology : uint32_t { Triangles, Strip };
struct SceneMesh {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::array<float, 3> center{};
    uint64_t revision{};
};
// Indices are relative to the captured range; restart values are UINT32_MAX.
// Invalid/unwritten/degenerate triangles are discarded, never sent to the SDK.
SceneMesh BuildMesh(std::span<const CapturedVertex> vertices,
                    std::span<const uint32_t> indices, Topology topology,
                    const SceneCamera& camera);
} // namespace BbRemix

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "rt_blas_cache.h"

namespace Vulkan::Rt {

// Placement identity (docs/GEOMETRY_CACHE_STUDY.md, sections 6 and 9). Maps reuse a model many
// times and the game culls by view, so a draw's order among identical draws (DrawKey::occurrence)
// changes with the view: a placement seen again missed the cache, was built again, and its old
// build stayed as a kept copy that no longer agreed with what is drawn. The game's own vertex
// shader constants hold every draw's world matrix (one 3x4 per instance), so (mesh, matrix)
// names a placement whatever else the frame draws.

constexpr uint32_t PlacementFloats = 12; // 3x4 row-major, translation in the 4th column
constexpr uint32_t PlacementBytes = PlacementFloats * 4;
// The per-frame camera block: its first rows can be a rigid view matrix, which would pass the
// test below, so it is never a candidate for a single draw. 864 bytes is also exactly the
// matrices of an 18-instance batch (18 x 48), which stays a candidate.
constexpr uint32_t CameraBlockBytes = 864;

// Whether 12 floats can be a placement: finite, the images of the three model axes of equal
// length (uniform scale) in (0.01, 100) within 1% and orthogonal within 1%, translation under
// 1e5. Anything else (projections, colours, bone data) fails and the draw keeps its old key.
// `uniform`: how far the longest axis may exceed the shortest (the tessellated trees are
// 1% shorter than wide, ReadPatchMatrices).
inline bool PlausiblePlacement(const float* m, float uniform = 1.01f) {
    float v[PlacementFloats];
    std::memcpy(v, m, sizeof(v));
    for (const float f : v) {
        if (!std::isfinite(f)) {
            return false;
        }
    }
    std::array<std::array<float, 3>, 3> axis{};
    std::array<float, 3> len{};
    for (int c = 0; c < 3; ++c) {
        axis[c] = {v[c], v[4 + c], v[8 + c]};
        len[c] = std::sqrt(axis[c][0] * axis[c][0] + axis[c][1] * axis[c][1] +
                           axis[c][2] * axis[c][2]);
        if (!(len[c] > 0.01f && len[c] < 100.0f)) {
            return false;
        }
    }
    const float lo = std::min({len[0], len[1], len[2]});
    const float hi = std::max({len[0], len[1], len[2]});
    if (hi > lo * uniform) {
        return false;
    }
    for (int a = 0; a < 3; ++a) {
        for (int b = a + 1; b < 3; ++b) {
            const float dot = axis[a][0] * axis[b][0] + axis[a][1] * axis[b][1] +
                              axis[a][2] * axis[b][2];
            if (std::fabs(dot) > 0.01f * len[a] * len[b]) {
                return false;
            }
        }
    }
    return std::sqrt(v[3] * v[3] + v[7] * v[7] + v[11] * v[11]) < 1e5f;
}

inline uint64_t PlacementMix(uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    h ^= h >> 31;
    h *= 0xbf58476d1ce4e5b9ULL;
    return h ^ (h >> 29);
}

// The id of a placement: its matrix rounded to 1 mm of translation and 1e-4 on the 3x3 part,
// hashed; never 0 (0 means "no placement" in DrawKey).
inline uint64_t PlacementId(const float* m) {
    float v[PlacementFloats];
    std::memcpy(v, m, sizeof(v));
    uint64_t h = 0x50ac3e11d0c0ffeeULL;
    for (uint32_t i = 0; i < PlacementFloats; ++i) {
        const double quantum = i % 4 == 3 ? 1e3 : 1e4;
        h = PlacementMix(h, uint64_t(std::llround(double(v[i]) * quantum)));
    }
    return h ? h : 1;
}

// One constant buffer of the draw's vertex shader, as the rasterizer sees it.
struct PlacementBuffer {
    uint32_t index{};
    uint32_t size{};
    const float* data{}; // readable guest memory of `size` bytes, or null
    // The skinning test's bone classification (motion_history.h ClassifyBuffer) calls it a
    // skeleton: a bone palette whose first bone is rigid would pass the fit test.
    bool skeleton{};
};

// Whether `b` has the shape of the draw's matrices: an instanced draw needs exactly one matrix
// per instance; a single draw takes neither the camera block nor a skeleton.
inline bool PlacementBufferShape(const PlacementBuffer& b, uint32_t instances) {
    if (!b.data || b.size < PlacementBytes || !instances) {
        return false;
    }
    if (instances > 1) {
        return uint64_t(b.size) == uint64_t(instances) * PlacementBytes;
    }
    return b.size != CameraBlockBytes && !b.skeleton;
}

// Whether `b` holds the draw's matrices, to choose the buffer: the right shape and every
// matrix plausible (a single draw: the first one).
inline bool PlacementBufferFits(const PlacementBuffer& b, uint32_t instances) {
    if (!PlacementBufferShape(b, instances)) {
        return false;
    }
    const uint32_t count = instances > 1 ? instances : 1;
    for (uint32_t i = 0; i < count; ++i) {
        if (!PlausiblePlacement(b.data + size_t(i) * PlacementFloats)) {
            return false;
        }
    }
    return true;
}

// Once a buffer was chosen for a shader: the right shape and finite matrices are enough for an
// id. A non-uniformly scaled part is still a placement, and must not send its whole batch to
// the occurrence key.
inline bool PlacementBufferReadable(const PlacementBuffer& b, uint32_t instances) {
    if (!PlacementBufferShape(b, instances)) {
        return false;
    }
    const size_t floats = size_t(instances > 1 ? instances : 1) * PlacementFloats;
    for (size_t i = 0; i < floats; ++i) {
        if (!std::isfinite(b.data[i])) {
            return false;
        }
    }
    return true;
}

// The first buffer that fits, or -1. The rasterizer caches the result per vertex shader (and
// instanced or not), then only checks that the cached buffer still fits on each draw.
inline int32_t ChoosePlacementBuffer(std::span<const PlacementBuffer> buffers, uint32_t instances) {
    for (const PlacementBuffer& b : buffers) {
        if (PlacementBufferFits(b, instances)) {
            return int32_t(b.index);
        }
    }
    return -1;
}

// Rasterizer cache of the chosen buffer per (vertex shader, instanced). A shader with no fitting
// buffer is scanned again on a few draws, then left alone.
struct PlacementSlot {
    int32_t slot{-1};
    uint32_t scans{};
};
constexpr uint32_t PlacementScanLimit = 16;

inline uint64_t PlacementSlotKey(uint64_t vs, bool instanced) {
    return PlacementMix(vs, instanced ? 2u : 1u);
}

// Verification against the GPU probe (rt_probe.comp): the probe gives the world position of one
// sample vertex of the drawn mesh; the matrix times that vertex's model-space position must land
// there. Until a (vertex shader, buffer) pair has passed, its draws keep the occurrence key.
enum class PlacementVerdict : uint8_t { Pending, Trusted, Rejected };

constexpr uint32_t PlacementTrustPasses = 8;
// Passes on this many frames and from this many meshes before trust: the pending checks are
// the first draws of each frame, so 8 passes could all be one mesh at one view.
constexpr uint32_t PlacementTrustFrames = 2;
constexpr uint32_t PlacementTrustGeometries = 3;

// Bits 8-9 of a probe record's pad1 (rt_probe.comp `placement`): the sample the probe reports
// first. A checked draw rotates it by frame, so a matrix is compared at more than one vertex
// (sample 0 alone can sit near the model origin, where a wrong rotation does not show).
constexpr uint32_t ProbeFirstSampleShift = 8;
inline uint32_t PlacementProbeRotation(uint64_t frame) {
    return uint32_t(frame % ProbeSamples) << ProbeFirstSampleShift;
}

// 2 cm, plus the probe's own 0.1% of the distance (rt_probe.comp compares view positions with
// 1e-3 * z: far samples carry that much capture noise).
constexpr float PlacementTolerance(float camera_distance) {
    return 0.02f + 0.001f * camera_distance;
}

struct PlacementTrust {
    uint32_t passes{}, fails{};
    float worst{};
    PlacementVerdict verdict{PlacementVerdict::Pending};
    uint64_t vs{}; // the vertex shader, for erasing its entries when the verdict changes
    // Distinct frames and meshes (DrawKey::geometry) that passed.
    uint64_t last_pass_frame{~0ull};
    uint32_t pass_frames{};
    std::array<uint64_t, PlacementTrustGeometries> geometries{};
    uint32_t geometry_count{};

    // Counts one check of probe frame `frame` on mesh `geometry`; true when the verdict
    // changed. Trusted after 8 passes per failure plus 8, on 2 frames and 3 meshes at least;
    // rejected (for good) once 3 checks failed and failures are over 1 in 8. One outlier does
    // not throw away a shader that agrees everywhere else, a wrong buffer fails nearly always.
    bool Record(float error, float tolerance, uint64_t frame, uint64_t geometry) {
        if (verdict == PlacementVerdict::Rejected) {
            return false;
        }
        const bool pass = std::isfinite(error) && error <= tolerance;
        passes += pass ? 1u : 0u;
        fails += pass ? 0u : 1u;
        worst = std::isfinite(error) ? std::max(worst, error) : worst;
        if (pass && frame != last_pass_frame) {
            last_pass_frame = frame;
            ++pass_frames;
        }
        if (pass && geometry_count < PlacementTrustGeometries &&
            std::find(geometries.begin(), geometries.begin() + geometry_count, geometry) ==
                geometries.begin() + geometry_count) {
            geometries[geometry_count++] = geometry;
        }
        const PlacementVerdict was = verdict;
        if (fails >= 3 && fails * 8 > passes) {
            verdict = PlacementVerdict::Rejected;
        } else if (passes >= PlacementTrustPasses * (fails + 1) &&
                   pass_frames >= PlacementTrustFrames &&
                   geometry_count >= PlacementTrustGeometries) {
            verdict = PlacementVerdict::Trusted;
        }
        return verdict != was;
    }
};

inline uint64_t PlacementTrustKey(uint64_t vs, uint32_t slot) {
    return PlacementMix(vs, 0x100u + slot);
}

// One pending comparison: probe record `probe` of the frame drew a mesh with `matrix`; `model`
// holds the model-space position of each probe sample's vertex (bit k of `mask` = known).
struct PlacementCheck {
    uint32_t probe{};
    uint64_t trust_key{}, vs{};
    uint64_t geometry{}; // DrawKey::geometry: trust needs passes from several meshes
    uint32_t slot{};
    std::array<float, PlacementFloats> matrix{};
    std::array<std::array<float, 3>, ProbeSamples> model{};
    uint32_t mask{};
    std::array<float, 3> camera{};
};

// The sample's error in metres, or NaN when the sample has no model position.
inline float PlacementError(const PlacementCheck& c, uint32_t k, const std::array<float, 3>& at) {
    if (k >= ProbeSamples || !((c.mask >> k) & 1u)) {
        return std::numeric_limits<float>::quiet_NaN();
    }
    Affine m{};
    std::copy(c.matrix.begin(), c.matrix.end(), m.begin());
    return std::sqrt(Distance2(Transform(m, c.model[k]), at));
}

// Model-space positions of the probe samples' vertices, read from the draw's position stream.
inline uint32_t ReadProbeModel(const DrawInput& in,
                               std::array<std::array<float, 3>, ProbeSamples>& out) {
    uint32_t mask = 0;
    if (!in.positions || in.position_stride < 12) {
        return 0;
    }
    const auto vertices = ProbeVertices(in);
    for (uint32_t k = 0; k < ProbeSamples; ++k) {
        if (vertices[k] >= in.position_vertices) {
            continue;
        }
        std::memcpy(out[k].data(), in.positions + size_t(vertices[k]) * in.position_stride, 12);
        if (std::isfinite(out[k][0]) && std::isfinite(out[k][1]) && std::isfinite(out[k][2])) {
            mask |= 1u << k;
        }
    }
    return mask;
}

// The verdict changes of one readback: each checked shader's verdict before its first check
// in it. The purges follow the net change: Pending -> Trusted erases the shader's
// occurrence-keyed entries, Trusted -> Rejected its placed ones; Pending -> Trusted -> Rejected
// inside one readback asks for neither (nothing was keyed by placement in between).
struct PlacementFlips {
    std::unordered_map<uint64_t, PlacementVerdict> before; // by trust key
    std::vector<uint64_t> trusted, rejected;               // vertex shaders, from Collect

    void Collect(const std::unordered_map<uint64_t, PlacementTrust>& table) {
        for (const auto& [key, was] : before) {
            const auto it = table.find(key);
            if (it == table.end()) {
                continue;
            }
            const PlacementVerdict now = it->second.verdict;
            if (was == PlacementVerdict::Pending && now == PlacementVerdict::Trusted) {
                trusted.push_back(it->second.vs);
            } else if (was == PlacementVerdict::Trusted && now == PlacementVerdict::Rejected) {
                rejected.push_back(it->second.vs);
            }
        }
        before.clear();
    }
};

// Counts one check read back from the probe; true when its verdict changed. Nothing is
// counted when BB_RT_PLACEMENT went off after the check was queued: the off switch already
// erased the placed entries, a late verdict must not start purging again.
inline bool RecordPlacementCheck(std::unordered_map<uint64_t, PlacementTrust>& table,
                                 PlacementFlips& flips, bool on, const PlacementCheck& c,
                                 float error, float tolerance, uint64_t frame) {
    if (!on) {
        return false;
    }
    PlacementTrust& t = table[c.trust_key];
    t.vs = c.vs;
    flips.before.try_emplace(c.trust_key, t.verdict);
    return t.Record(error, tolerance, frame, c.geometry);
}

// One instance of a trusted draw as a draw of its own. Captured positions are laid out
// instance after instance (rt_gather.comp slot = store_base + instance * vertex_count +
// vertex). A placed piece's geometry is given by (mesh, matrix): no palette, never skinned,
// so it is never rebuilt for a palette or skeleton change, never updatable for it and never
// "short lived" (measured with the batch's other buffers as its palette: 80,785 "palette
// changed" rebuilds in one run, and the pieces were evicted after 120 unused frames).
inline DrawInput PlacedInstance(const DrawInput& draw, uint32_t i) {
    DrawInput one = draw;
    one.instances = 1;
    one.store_base = draw.store_base + i * draw.vertex_count;
    one.palette = 0;
    one.skinned = false;
    one.unreadable = false;
    one.key.instances = 1;
    one.key.first_instance = 0;
    // The game draws one placement through the batched shader when several copies are in view
    // and through the single one otherwise (vs 6a833138 / 9da6aacb for one mesh): the shader is
    // not part of a placed piece's identity. Measured with it in the key: 27 cache misses a
    // frame on a turn over seen views, and 27,015 kept copies skipped as "the same placement
    // drawn" in one run.
    one.key.shader = 0;
    one.key.placement = PlacementId(draw.placements + size_t(i) * PlacementFloats);
    one.placements = draw.placements + size_t(i) * PlacementFloats;
    one.placement_count = 1;
    return one;
}

// One BLAS per mesh, shared by its placements (BB_RT_PLACEMENT_SHARED). Measured with one BLAS
// per placement: a 360 degree turn on the Central Yharnam bridge needs 892 MiB of BLAS, the pool
// is 543 MiB, so pieces seen a moment ago were evicted and built again when they came back (21
// cache misses a frame; 0.4 with a 3 GB pool). A placed draw is looked up under a mesh key
// (DrawKey::placement = SharedMeshPlacement); the entry remembers the matrix of the placement it
// was built from, and every other placement is that BLAS moved by Relocation().
constexpr uint64_t SharedMeshPlacement = 1;
// DrawKey::placement of a BLAS built from the geometry cache (rt_geo_cache.h), in model space.
constexpr uint64_t GeoMeshPlacement = 2;
// DrawKey::placement of a BLAS built on the CPU from the control mesh of a tessellated draw
// (vk_ray_tracing.cpp AddPatchDraw), in model space.
constexpr uint64_t PatchMeshPlacement = 3;

// The game's tessellated meshes (the big trees of the Forbidden Woods: stages LsHs, vertex
// shader cc0f4e5a) keep one block of 13 rows of 16 bytes per instance in a vertex-shader
// buffer, the 3x4 world matrix in its first three rows (measured: 9 instances in a 3328-byte
// buffer of 16 blocks, translations at the trees' places).
constexpr uint32_t PatchInstanceBytes = 208;

// The world matrices of `instances` such blocks, 12 floats each, appended to `out`. False (and
// `out` as it was) when the buffer is too small or a block's first rows are no placement, so a
// buffer of another kind is never taken for one.
inline bool ReadPatchMatrices(const float* data, uint32_t bytes, uint32_t first_instance,
                              uint32_t instances, std::vector<float>& out) {
    if (!data || !instances ||
        (uint64_t(first_instance) + instances) * PatchInstanceBytes > bytes) {
        return false;
    }
    const size_t before = out.size();
    for (uint32_t i = 0; i < instances; ++i) {
        const float* m = data + size_t(first_instance + i) * (PatchInstanceBytes / 4);
        if (!PlausiblePlacement(m, 2.0f)) {
            out.resize(before);
            return false;
        }
        out.insert(out.end(), m, m + PlacementFloats);
    }
    return true;
}

inline Affine PlacementAffine(const float* m) {
    Affine a{};
    std::copy_n(m, PlacementFloats, a.begin());
    return a;
}

// Inverse of a 3x4 affine with any rotation and scale; identity when it has no inverse.
inline Affine InverseAffine(const Affine& m) {
    const double a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9],
                 i = m[10];
    const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (!(std::abs(det) > 1e-12) || !std::isfinite(det)) {
        return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    }
    const double r = 1.0 / det;
    const double n[9] = {(e * i - f * h) * r, (c * h - b * i) * r, (b * f - c * e) * r,
                         (f * g - d * i) * r, (a * i - c * g) * r, (c * d - a * f) * r,
                         (d * h - e * g) * r, (b * g - a * h) * r, (a * e - b * d) * r};
    Affine out{};
    for (int row = 0; row < 3; ++row) {
        out[row * 4 + 0] = float(n[row * 3 + 0]);
        out[row * 4 + 1] = float(n[row * 3 + 1]);
        out[row * 4 + 2] = float(n[row * 3 + 2]);
        out[row * 4 + 3] = float(-(n[row * 3 + 0] * m[3] + n[row * 3 + 1] * m[7] + n[row * 3 + 2] * m[11]));
    }
    return out;
}

// World-space move that takes the placement a BLAS was built from onto another placement.
inline Affine Relocation(const Affine& placement, const Affine& built_from) {
    return MultiplyAffine(placement, InverseAffine(built_from));
}

// Rule: one placement of one mesh is in the TLAS once. The game switches detail levels per
// piece (another index range over the same vertex buffer: another DrawKey); when one level is
// drawn this frame, a kept entry of another level at the same placement is left out. Separate
// sub-meshes can share one vertex buffer and one matrix too: they are told apart by their
// vertex ranges, which overlap between detail levels and usually not between sub-meshes.
inline uint64_t PlacedMesh(uint64_t geometry, uint64_t placement) {
    return PlacementMix(geometry, placement) | 1u;
}

inline bool VertexRangesOverlap(uint32_t a_first, uint32_t a_count, uint32_t b_first,
                                uint32_t b_count) {
    // An unknown range (0 vertices) counts as overlapping: leaving out a kept copy is safe.
    if (!a_count || !b_count) {
        return true;
    }
    return uint64_t(a_first) < uint64_t(b_first) + b_count &&
           uint64_t(b_first) < uint64_t(a_first) + a_count;
}

// The placements drawn this frame: every draw whose matrix buffer was read, placed or keyed by
// occurrence (pending, rejected, a moving mesh), so a kept placed entry never stands in the
// TLAS beside a draw of the same placement under the other key.
struct DrawnPlacements {
    std::unordered_multimap<uint64_t, std::array<uint32_t, 2>> ranges;

    void Add(const DrawKey& mesh, uint64_t placement) {
        ranges.emplace(PlacedMesh(mesh.geometry, placement),
                       std::array<uint32_t, 2>{mesh.first_vertex, mesh.vertex_count});
    }
    void Add(const DrawInput& draw) {
        if (!draw.placements) {
            return;
        }
        for (uint32_t i = 0; i < draw.placement_count; ++i) {
            Add(draw.key, PlacementId(draw.placements + size_t(i) * PlacementFloats));
        }
    }
    bool Contains(const DrawKey& kept) const {
        if (!kept.placement || ranges.empty()) {
            return false;
        }
        auto [it, end] = ranges.equal_range(PlacedMesh(kept.geometry, kept.placement));
        for (; it != end; ++it) {
            if (VertexRangesOverlap(it->second[0], it->second[1], kept.first_vertex,
                                    kept.vertex_count)) {
                return true;
            }
        }
        return false;
    }
    bool Empty() const {
        return ranges.empty();
    }
    void Clear() {
        ranges.clear();
    }
};

// Movers: a piece whose matrix changes (a lift, a door, a swinging sign) gets another placement
// id every frame. Measured: the same mesh missed with the previous frame's id as its nearest
// match, a full rebuild every frame standing still. The tell, at the end of a frame: a placed
// key missed and an entry of the same key apart from the placement was drawn on the previous
// frame and not on this one. A static piece entering the view while another copy leaves shows
// that once; a mover shows it on every frame, so 4 frames in a row route the mesh (vertex
// buffer + index buffer) to the occurrence key for 600 frames, where it is probed, marked
// moving and refit as before placements existed, and its placed entries are erased.
constexpr uint32_t MoverStreak = 4;
constexpr uint64_t MoverRouteFrames = 600;

inline uint64_t PlacementMeshKey(DrawKey k) {
    k.placement = 0;
    k.occurrence = 0;
    k.instances = 0;
    return uint64_t(DrawKeyHash{}(k));
}

inline uint64_t PlacementRouteKey(const DrawKey& k) {
    return PlacementMix(k.geometry, k.indices) | 1u;
}

struct PlacementMovers {
    struct Streak {
        uint32_t frames{};
        uint64_t last{};
    };
    std::unordered_map<uint64_t, uint64_t> missed;  // mesh key -> route key, this frame
    std::unordered_map<uint64_t, Streak> streaks;   // by route key
    std::unordered_map<uint64_t, uint64_t> routed;  // route key -> last routed frame

    bool Routed(uint64_t route, uint64_t frame) const {
        const auto it = routed.find(route);
        return it != routed.end() && frame <= it->second;
    }
    void NoteMiss(const DrawKey& k) {
        missed.try_emplace(PlacementMeshKey(k), PlacementRouteKey(k));
    }
    // At the end of frame `frame`: `moved` gets the meshes that became movers, `released` the
    // ones whose route ran out (their occurrence-keyed entries go, so a placed draw of the mesh
    // never stands beside a kept copy under the other key).
    void EndFrame(const BlasCache& cache, uint64_t frame, std::vector<uint64_t>& moved,
                  std::vector<uint64_t>& released) {
        std::unordered_set<uint64_t> tells;
        if (!missed.empty()) {
            cache.ForEachKeyed([&](const DrawKey& k, const CacheEntry& e) {
                if (k.placement && e.last_used + 1 == frame) {
                    const auto it = missed.find(PlacementMeshKey(k));
                    if (it != missed.end()) {
                        tells.insert(it->second);
                    }
                }
            });
            missed.clear();
        }
        for (const uint64_t route : tells) {
            Streak& s = streaks[route];
            s.frames = s.frames && s.last + 1 == frame ? s.frames + 1 : 1;
            s.last = frame;
            if (s.frames >= MoverStreak) {
                routed[route] = frame + MoverRouteFrames;
                s.frames = 0;
                moved.push_back(route);
            }
        }
        // A streak not continued this frame is over.
        std::erase_if(streaks, [&](const auto& s) {
            return !s.second.frames || s.second.last != frame;
        });
        for (auto it = routed.begin(); it != routed.end();) {
            if (it->second < frame) {
                released.push_back(it->first);
                it = routed.erase(it);
            } else {
                ++it;
            }
        }
    }
};

}

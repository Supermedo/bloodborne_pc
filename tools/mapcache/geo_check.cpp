// geo_check: the engine's map reader (rt_map_files.cpp, game mode of rt_geo_cache.h) against a
// .bbgeo that build_cache.py wrote from the same files. Prints the first differences.
//
//   geo_check <dvdroot_ps4> <map id> <file.bbgeo> [meshes to compare triangles of, default 400]
//
// Build (MSYS2 clang64), from the repo root:
//   clang++ -std=c++20 -O2 -Igpu/third_party/miniz -Iout/gpu/third_party/miniz -Igpu/shadps4/video_core/renderer_vulkan/rt
//     tools/mapcache/geo_check.cpp gpu/shadps4/video_core/renderer_vulkan/rt/rt_map_files.cpp
//     gpu/third_party/miniz/miniz.c gpu/third_party/miniz/miniz_tinfl.c gpu/third_party/miniz/miniz_tdef.c -o geo_check.exe
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

#include "rt_geo_cache.h"

using namespace Vulkan::Rt;

static int diffs = 0;
static void Diff(const char* what, size_t i, double a, double b) {
    if (++diffs <= 20) {
        std::printf("  DIFF %s %zu: engine %.6g file %.6g\n", what, i, a, b);
    }
}

static bool Near(float a, float b, float tol) {
    return std::fabs(a - b) <= tol * std::max(1.0f, std::fabs(b));
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::printf("geo_check <dvdroot_ps4> <map id> <file.bbgeo> [meshes]\n");
        return 2;
    }
    const size_t sample = argc > 4 ? size_t(std::atoi(argv[4])) : 400;
    GeoMap file, game;
    if (!file.Open(argv[3])) {
        std::printf("cannot open %s\n", argv[3]);
        return 2;
    }
    const auto t0 = std::chrono::steady_clock::now();
    game.StartGame(argv[1], argv[2]);
    while (!game.PollGame()) {
        if (game.GameFailed()) {
            std::printf("game scan failed: %s\n", game.GameReport().c_str());
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::printf("engine scan: %s (%.1f s wall)\n", game.GameReport().c_str(),
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    std::printf("file: %zu models, %zu meshes, %zu placements, %zu collision sets\n",
                file.models.size(), file.meshes.size(), file.places.size(), file.collisions.size());
    if (game.models.size() != file.models.size() || game.meshes.size() != file.meshes.size() ||
        game.places.size() != file.places.size() ||
        game.collisions.size() != file.collisions.size()) {
        std::printf("COUNTS DIFFER\n");
        ++diffs;
    }
    for (size_t i = 0; i < std::min(game.models.size(), file.models.size()); ++i) {
        const GeoModel &a = game.models[i], &b = file.models[i];
        if (std::memcmp(a.name, b.name, 8) || a.first_mesh != b.first_mesh ||
            a.mesh_count != b.mesh_count) {
            Diff("model", i, a.mesh_count, b.mesh_count);
        }
    }
    for (size_t i = 0; i < std::min(game.meshes.size(), file.meshes.size()); ++i) {
        const GeoMesh &a = game.meshes[i], &b = file.meshes[i];
        if (a.index_count != b.index_count) Diff("mesh index count", i, a.index_count, b.index_count);
        if (a.flags != b.flags) Diff("mesh flags", i, a.flags, b.flags);
        if (a.lod0_indices != b.lod0_indices || a.lod1_indices != b.lod1_indices ||
            a.lod2_indices != b.lod2_indices)
            Diff("mesh lods", i, a.lod0_indices, b.lod0_indices);
        for (int k = 0; k < 6; ++k)
            if (!Near(a.box[k], b.box[k], 1e-5f)) Diff("mesh box", i, a.box[k], b.box[k]);
    }
    for (size_t i = 0; i < std::min(game.places.size(), file.places.size()); ++i) {
        const GeoPlace &a = game.places[i], &b = file.places[i];
        if (a.model != b.model) Diff("place model", i, a.model, b.model);
        if (a.flags != b.flags) Diff("place flags", i, a.flags, b.flags);
        if (a.groups != b.groups) Diff("place groups", i, a.groups[0], b.groups[0]);
        for (int k = 0; k < 12; ++k)
            if (!Near(a.m[k], b.m[k], 1e-5f)) Diff("place matrix", i, a.m[k], b.m[k]);
        for (int k = 0; k < 6; ++k)
            if (!Near(a.box[k], b.box[k], 1e-4f)) Diff("place box", i, a.box[k], b.box[k]);
    }
    for (size_t i = 0; i < std::min(game.collisions.size(), file.collisions.size()); ++i)
        if (game.collisions[i] != file.collisions[i]) Diff("collision", i, game.collisions[i][0], file.collisions[i][0]);
    // Triangles of a spread of meshes, through the loader thread.
    std::vector<float> va, vb, vertices;
    std::vector<uint32_t> indices;
    size_t compared = 0, triangles = 0;
    const size_t n = std::min(game.meshes.size(), file.meshes.size());
    const auto t1 = std::chrono::steady_clock::now();
    for (size_t s = 0; s < sample && n; ++s) {
        const size_t i = s * n / std::min(sample, n);
        if (game.meshes[i].index_count != file.meshes[i].index_count) continue;
        va.resize(size_t(game.meshes[i].index_count) * 3);
        vb.resize(va.size());
        int r;
        while ((r = game.ReadSoup(uint32_t(i), va.data(), vertices, indices)) == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (r < 0 || file.ReadSoup(uint32_t(i), vb.data(), vertices, indices) != 1) {
            Diff("soup read", i, r, 1);
            continue;
        }
        for (size_t k = 0; k < va.size(); ++k)
            if (!Near(va[k], vb[k], 1e-5f)) {
                Diff("soup", i, va[k], vb[k]);
                break;
            }
        ++compared;
        triangles += va.size() / 9;
    }
    std::printf("triangles of %zu meshes compared (%zu triangles, %.1f s)\n", compared, triangles,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count());
    std::printf("%s: %d differences\n", diffs ? "FAIL" : "PASS", diffs);
    return diffs ? 1 : 0;
}

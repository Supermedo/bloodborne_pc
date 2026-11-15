// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "rt_gi.h"
#include "rt_sky_table.h"

// Which sky is the game drawing, and where is its moon or sun? (docs/GEOMETRY_CACHE_PLAN.md
// step 3.) The game's files place every sky's moon in the world (rt_sky_table.h); the engine
// only has to know which sky it is looking at. The moon cache did that by the index counts of
// the cloud draws and by where a moon was found earlier, so a new install lit from the game's
// own direction until the player looked at the disc, and maps that share cloud meshes took each
// other's moon. Here a cloud draw is matched by its index count and four of its model-space
// vertices, which are exact copies of the file's: one draw names its sky, at the first frame,
// whatever the camera looks at.
//
// Skies share meshes (Central Yharnam's evening sky and Cathedral Ward's daytime sky: four of
// five cloud meshes), so the sky is the one most of the meshes drawn lately belong to; a tie
// keeps the sky already chosen, else it is broken by where the draw stands in the world (two
// maps place the same model at different spots), else there is no answer and the caller keeps
// its old way.
namespace Vulkan::Rt {

class SkyFinder {
public:
    // Evidence older than this many frames no longer counts (walking from one map's sky into
    // the next: the old sky's own meshes stop drawing).
    static constexpr uint64_t Window = 300;
    // A model-space vertex equals the file's (both are the same float; a little room for a
    // file that was re-exported).
    static constexpr float ModelTolerance = 2.0e-3f;
    // A probed world position against the file's placement: only used between skies whose
    // models are equal, which stand hundreds of metres apart.
    static constexpr float WorldTolerance = 10.0f;

    SkyFinder() : SkyFinder(SkyTableSkies, SkyTablePlaces, SkyTableDraws) {}
    SkyFinder(std::span<const SkyTableSky> skies_, std::span<const SkyTablePlace> places_,
              std::span<const SkyTableDraw> draws_)
        : skies(skies_), places(places_), draws(draws_), seen(draws_.size(), Never),
          placed(skies_.size(), Never) {}

    // A sky draw: its index count and the model-space positions of its four probe samples
    // (rt_placement.h ReadProbeModel; all four must be known). True when it is in the table.
    bool Draw(uint64_t frame, uint32_t indices,
              const std::array<std::array<float, 3>, 4>& model) {
        bool any = false;
        for (size_t i = First(indices); i < draws.size() && draws[i].indices == indices; ++i) {
            if (Same(draws[i].samples, model)) {
                seen[i] = frame;
                any = true;
            }
        }
        return any;
    }

    // A probe readback of a sky draw: sample k stands at `at` in the world. Marks the skies
    // that place a draw of this index count there.
    void Placed(uint64_t frame, uint32_t indices, uint32_t k, const std::array<float, 3>& at) {
        if (k >= 4) {
            return;
        }
        for (size_t i = First(indices); i < draws.size() && draws[i].indices == indices; ++i) {
            const SkyTableDraw& d = draws[i];
            Affine m{};
            std::copy(std::begin(places[d.place].matrix), std::end(places[d.place].matrix),
                      m.begin());
            const std::array<float, 3> world =
                Transform(m, {d.samples[k * 3], d.samples[k * 3 + 1], d.samples[k * 3 + 2]});
            if (Dist2(world, at) <= WorldTolerance * WorldTolerance) {
                placed[d.sky] = frame;
            }
        }
    }

    // Chooses the sky from the draws of the last Window frames. Returns its index, or -1 when
    // no draw is known or the draws fit several skies equally. With no draw in the window the
    // last choice stays (the camera looks at the ground; the caller decides for how long).
    int Update(uint64_t frame) {
        std::vector<uint32_t> meshes(skies.size(), 0);
        // A mesh counts once, whatever its detail levels seen.
        std::vector<uint32_t> counted; // sky << 16 | mesh
        for (size_t i = 0; i < draws.size(); ++i) {
            if (!Recent(seen[i], frame)) {
                continue;
            }
            const uint32_t id = uint32_t(draws[i].sky) << 16 | draws[i].mesh;
            if (std::find(counted.begin(), counted.end(), id) == counted.end()) {
                counted.push_back(id);
                ++meshes[draws[i].sky];
            }
        }
        if (counted.empty()) {
            return current;
        }
        const int chosen = Choose(meshes, frame);
        // A sky with no moon of its own may have one that is a part of its own (the Old
        // Hunters' maps: the moon and its halo are a map piece an event shows). That part's
        // draw, seen once, gives the moon for as long as the sky stays.
        if (chosen < 0 || skies[size_t(chosen)].has_moon) {
            companion = -1;
        } else {
            if (chosen != companion_of) {
                companion = -1;
            }
            uint32_t most = 0;
            for (size_t s = 0; s < skies.size(); ++s) {
                if (int(s) != chosen && skies[s].has_moon && meshes[s] > most &&
                    std::strncmp(skies[s].name, skies[size_t(chosen)].name, 6) == 0) {
                    most = meshes[s];
                    companion = int(s);
                }
            }
        }
        companion_of = chosen;
        return chosen;
    }

    // The sky chosen by the last Update (-1 none).
    int Sky() const {
        return current;
    }
    const char* Name() const {
        return current < 0 ? "none" : skies[size_t(current)].name;
    }
    // The part the moon comes from when it is not the sky itself ("" otherwise).
    const char* MoonPart() const {
        return current >= 0 && !skies[size_t(current)].has_moon && companion >= 0
                   ? skies[size_t(companion)].name : "";
    }
    // Skies the draws fitted equally at the last Update (1 = a clear answer).
    uint32_t Ties() const {
        return ties;
    }

    // The chosen sky's moon or sun in the world. False without a sky or for a sky that has none.
    bool Moon(std::array<float, 3>& moon) const {
        if (current < 0) {
            return false;
        }
        const int from = skies[size_t(current)].has_moon ? current : companion;
        if (from < 0) {
            return false;
        }
        const SkyTableSky& s = skies[size_t(from)];
        moon = {s.moon[0], s.moon[1], s.moon[2]};
        return true;
    }

    // A load: nothing seen so far says anything about the next map.
    void Reset() {
        std::fill(seen.begin(), seen.end(), Never);
        std::fill(placed.begin(), placed.end(), Never);
        current = companion = companion_of = -1;
        ties = 0;
    }

private:
    static constexpr uint64_t Never = ~0ull;

    // The sky most of the meshes drawn lately belong to (Update).
    int Choose(const std::vector<uint32_t>& meshes, uint64_t frame) {
        const uint32_t best = *std::max_element(meshes.begin(), meshes.end());
        std::vector<int> tied;
        for (size_t s = 0; s < skies.size(); ++s) {
            if (meshes[s] == best) {
                tied.push_back(int(s));
            }
        }
        ties = uint32_t(tied.size());
        if (tied.size() > 1) {
            // Where the draws stand in the world.
            std::vector<int> there;
            for (const int s : tied) {
                if (Recent(placed[size_t(s)], frame)) {
                    there.push_back(s);
                }
            }
            if (!there.empty()) {
                tied.swap(there);
            }
        }
        if (tied.size() > 1 && std::find(tied.begin(), tied.end(), current) != tied.end()) {
            return current;
        }
        if (tied.size() > 1) {
            // Equal skies with one moon (a part placed twice) are one answer.
            const SkyTableSky& a = skies[size_t(tied[0])];
            for (const int s : tied) {
                const SkyTableSky& b = skies[size_t(s)];
                if (a.has_moon != b.has_moon ||
                    (a.has_moon && Dist2({a.moon[0], a.moon[1], a.moon[2]},
                                         {b.moon[0], b.moon[1], b.moon[2]}) > 1.0f)) {
                    current = -1;
                    return current;
                }
            }
        }
        current = tied[0];
        return current;
    }

    static float Dist2(const std::array<float, 3>& a, const std::array<float, 3>& b) {
        const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
        return dx * dx + dy * dy + dz * dz;
    }

    static bool Recent(uint64_t then, uint64_t now) {
        return then != Never && now <= then + Window;
    }

    static bool Same(const float* file, const std::array<std::array<float, 3>, 4>& model) {
        for (size_t k = 0; k < 4; ++k) {
            for (size_t a = 0; a < 3; ++a) {
                if (!(std::fabs(file[k * 3 + a] - model[k][a]) <= ModelTolerance)) {
                    return false;
                }
            }
        }
        return true;
    }

    size_t First(uint32_t indices) const {
        return size_t(std::lower_bound(draws.begin(), draws.end(), indices,
                                       [](const SkyTableDraw& d, uint32_t n) {
                                           return d.indices < n;
                                       }) -
                      draws.begin());
    }

    std::span<const SkyTableSky> skies;
    std::span<const SkyTablePlace> places;
    std::span<const SkyTableDraw> draws;
    std::vector<uint64_t> seen;   // per table draw: the last frame a draw matched it
    std::vector<uint64_t> placed; // per sky: the last frame a probe stood where it places a draw
    int current = -1;
    int companion = -1, companion_of = -1; // the part that holds the moon of a sky without one
    uint32_t ties = 0;
};

} // namespace Vulkan::Rt

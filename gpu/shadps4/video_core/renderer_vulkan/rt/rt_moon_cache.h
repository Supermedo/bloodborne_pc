#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

// The moon's world position per sky model, so a load facing away from the moon still lights
// from it. The game draws its moon as one small mesh of the sky model (rt_gi.h SkyLayerPs) only
// while the moon is in view; the cloud segments of the same model draw every frame. The moon
// mesh is fixed in the world about 750 m out (Hemwick graveyard), not at infinity, so its
// direction moves with the player: a position is kept, and the light points from the camera at
// it. The sky is keyed by the cloud segments (index and vertex counts of the layer draws present
// in every frame since the layer appeared); the position found last is kept in a small text
// file, one "key x y z" line per sky.
namespace Vulkan::Rt {

class SkyKey {
public:
    // Layer draws missing for this many frames mean a load or an interior: start over.
    static constexpr uint64_t RestartFrames = 60;

    // One layer draw. Returns true when this draw starts a new layer run (first ever, or after
    // RestartFrames without the layer).
    bool Add(uint64_t frame_now, uint32_t index_count, uint32_t vertex_count) {
        bool restarted = false;
        if (frame_now != frame) {
            Close();
            if (frame == ~0ull || frame_now > frame + RestartFrames) {
                common.clear();
                seen.clear();
                have = false;
                restarted = true;
                ++generation;
            }
            frame = frame_now;
        }
        const uint64_t draw = uint64_t(index_count) << 32 | vertex_count;
        if (std::find(current.begin(), current.end(), draw) == current.end()) {
            current.push_back(draw);
        }
        const auto at = std::lower_bound(seen.begin(), seen.end(), draw);
        if (at == seen.end() || *at != draw) {
            seen.insert(at, draw);
        }
        return restarted;
    }

    // Every layer draw of this run (index count << 32 | vertex count), sorted. The moon cache
    // matches these one by one: Key() hashes the draws that were on screen in every frame, so it
    // changed with the view at the start of each run and missed the cached moon.
    const std::vector<uint64_t>& Draws() const {
        return seen;
    }

    // The draws present in every closed frame of this run, hashed; 0 before the first frame
    // closes.
    uint64_t Key() const {
        if (common.empty()) {
            return 0;
        }
        uint64_t h = 1469598103934665603ull;
        for (const uint64_t d : common) {
            for (int b = 0; b < 8; ++b) {
                h = (h ^ ((d >> (b * 8)) & 0xFF)) * 1099511628211ull;
            }
        }
        return h ? h : 1;
    }

    bool Recent(uint64_t frame_now, uint64_t frames) const {
        return frame != ~0ull && frame_now <= frame + frames;
    }

    uint64_t Generation() const {
        return generation;
    }

private:
    void Close() {
        if (current.empty()) {
            return;
        }
        std::sort(current.begin(), current.end());
        if (!have) {
            common = current;
            have = true;
        } else {
            std::vector<uint64_t> both;
            std::set_intersection(common.begin(), common.end(), current.begin(), current.end(),
                                  std::back_inserter(both));
            common.swap(both);
        }
        current.clear();
    }

    std::vector<uint64_t> current, common, seen;
    uint64_t frame = ~0ull;
    uint64_t generation = 0;
    bool have = false;
};

class MoonCache {
public:
    // Two finds within this angle (radians, about 1 degree) confirm the disc.
    static constexpr float Same = 0.0175f;
    // Each find lands up to ~25 m off the disc centre (the 512x128 layer grid is 0.7 x 0.9
    // degrees a cell at 750 m), and taking the newest find made the light hop between two cells as
    // the player walked: shadows crept. The position is a running mean of the finds (the last
    // MaxSamples weigh most), so it settles on the centre. The file is rewritten once the mean
    // moved Moved metres.
    // A find farther than Gate from the mean is a stray: one spot in Hemwick kept finding a point
    // 52 m off and the mean crept 4 degrees toward it while the player stood there; another find
    // was 123 degrees off at the horizon and, taken as a new moon, lit the field from there. The
    // other finds land within about 15 m. Strays gather in one candidate (a running mean of its
    // own); every find votes for the moon or the candidate it lands near, and the candidate
    // replaces the moon only once it has more votes. The true moon is found from everywhere, a
    // stray from one spot, so the moon stays; a wrong first find is outvoted in two finds.
    static constexpr float Gate = 30.0f;
    static constexpr float Moved = 2.0f;
    // Weight cap of the running mean. 64 made it follow the player: from one spot every find lands
    // ~10 m off the true centre the same way, and the mean walked there in 2 m steps (1 degree)
    // while the player stood still. Capped at MaxVotes it is the plain mean of all finds.
    static constexpr float MaxSamples = 1000.0f;
    // A position read from a file line without votes counts as this many finds.
    static constexpr float LoadedSamples = 8.0f;
    // Votes saved per moon are capped, so a candidate can still win in a long session.
    static constexpr float MaxVotes = 1000.0f;
    // A candidate confirmed this many times takes over even when the moon has more votes. One
    // session in Central Yharnam voted a wrong spot to 390, and the light kept pointing there
    // while the player faced the moon, 87 degrees off (the finds would have needed 390 more
    // sightings). The game also places its moon mesh per area, so a run's own finds must win
    // fast: 8 confirmed finds are about 2 s of the disc in view. Only a candidate SwitchFar away
    // (over ~10 degrees at 750 m) does: a near stray (52 m, 4 degrees, from one spot in Hemwick)
    // still needs more votes than the moon.
    static constexpr float SwitchVotes = 8.0f;
    static constexpr float SwitchFar = 150.0f;

    const std::array<float, 3>* Find(uint64_t key) const {
        const auto it = moons.find(key);
        return it == moons.end() ? nullptr : &it->second.moon.mean;
    }

    // Returns true when the position to save changed (the caller saves).
    bool Put(uint64_t key, std::array<float, 3> pos, float samples = 1.0f) {
        if (key == 0 || !std::isfinite(pos[0]) || !std::isfinite(pos[1]) ||
            !std::isfinite(pos[2])) {
            return false;
        }
        const auto it = moons.find(key);
        if (it == moons.end()) {
            moons[key] = {{pos, samples, samples}, {}, pos};
            return true;
        }
        Entry& e = it->second;
        if (Dist2(e.moon.mean, pos) > Gate * Gate) {
            if (e.cand.votes > 0.0f && Dist2(e.cand.mean, pos) <= Gate * Gate) {
                e.cand.Add(pos, samples);
            } else {
                e.cand = {pos, samples, samples};
            }
            const bool distant = Dist2(e.cand.mean, e.moon.mean) > SwitchFar * SwitchFar;
            if (e.cand.votes <= e.moon.votes && !(distant && e.cand.votes >= SwitchVotes)) {
                return false;
            }
            std::swap(e.moon, e.cand);
        } else {
            e.moon.Add(pos, samples);
        }
        if (Dist2(e.moon.mean, e.saved) < Moved * Moved) {
            return false;
        }
        e.saved = e.moon.mean;
        return true;
    }

    // Finds counted for the moon of this sky (0 when there is none).
    float Votes(uint64_t key) const {
        const auto it = moons.find(key);
        return it == moons.end() ? 0.0f : it->second.moon.votes;
    }

    // The unit direction from the camera to a cached moon, or false when there is none.
    bool DirFrom(uint64_t key, const std::array<float, 3>& cam, std::array<float, 3>& dir) const {
        const auto* pos = Find(key);
        return pos && DirTo(*pos, cam, dir);
    }

    static bool DirTo(const std::array<float, 3>& pos, const std::array<float, 3>& cam,
                      std::array<float, 3>& dir) {
        dir = {pos[0] - cam[0], pos[1] - cam[1], pos[2] - cam[2]};
        const float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        if (!(len > 1.0f) || !std::isfinite(len)) {
            return false;
        }
        for (float& c : dir) {
            c /= len;
        }
        return true;
    }

    // Per layer draw (SkyKey::Draws): every draw of the run keeps the moon found while it drew.
    // The moon of a run is the position most of its draws agree on (within Gate); ties go to the
    // one with more finds. A cloud segment shared by two skies is outvoted by the others.
    bool FindDraws(const std::vector<uint64_t>& draws, std::array<float, 3>& pos) const {
        int best_count = 0;
        float best_votes = 0.0f;
        for (const uint64_t a : draws) {
            const auto ia = moons.find(a);
            if (ia == moons.end()) {
                continue;
            }
            const Cluster& ca = ia->second.moon;
            int count = 0;
            for (const uint64_t b : draws) {
                const auto ib = moons.find(b);
                count += ib != moons.end() && Dist2(ib->second.moon.mean, ca.mean) <= Gate * Gate;
            }
            if (count > best_count || (count == best_count && ca.votes > best_votes)) {
                best_count = count;
                best_votes = ca.votes;
                pos = ca.mean;
            }
        }
        return best_count > 0;
    }

    // Returns true when a position to save changed (the caller saves).
    bool PutDraws(const std::vector<uint64_t>& draws, const std::array<float, 3>& pos) {
        bool changed = false;
        for (const uint64_t d : draws) {
            changed |= Put(d, pos);
        }
        return changed;
    }

    bool DirFromDraws(const std::vector<uint64_t>& draws, const std::array<float, 3>& cam,
                      std::array<float, 3>& dir) const {
        std::array<float, 3> pos{};
        return FindDraws(draws, pos) && DirTo(pos, cam, dir);
    }

    std::string Text() const {
        std::vector<uint64_t> keys;
        for (const auto& [k, e] : moons) {
            keys.push_back(k);
        }
        std::sort(keys.begin(), keys.end());
        std::string out;
        char line[128];
        for (const uint64_t k : keys) {
            const auto& m = moons.at(k).moon;
            std::snprintf(line, sizeof(line), "%016llx %.2f %.2f %.2f %.0f\n",
                          (unsigned long long)k, m.mean[0], m.mean[1], m.mean[2],
                          std::min(m.votes, MaxVotes));
            out += line;
        }
        return out;
    }

    // Lines that do not parse are skipped; a line without votes counts LoadedSamples.
    void Parse(const std::string& text) {
        size_t at = 0;
        while (at < text.size()) {
            size_t end = text.find('\n', at);
            if (end == std::string::npos) {
                end = text.size();
            }
            const std::string line = text.substr(at, end - at);
            unsigned long long k = 0;
            std::array<float, 3> d{};
            float votes = LoadedSamples;
            const int got =
                std::sscanf(line.c_str(), "%llx %f %f %f %f", &k, &d[0], &d[1], &d[2], &votes);
            if (got >= 4 && k != 0 && std::isfinite(d[0]) && std::isfinite(d[1]) &&
                std::isfinite(d[2])) {
                const float v = std::isfinite(votes) && votes >= 1.0f ? std::min(votes, MaxVotes)
                                                                      : LoadedSamples;
                moons[k] = {{d, std::min(v, MaxSamples), v}, {}, d};
            }
            at = end + 1;
        }
    }

    bool Load(const char* path) {
        std::FILE* f = std::fopen(path, "rb");
        if (!f) {
            return false;
        }
        std::string text;
        char buf[512];
        size_t n = 0;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
            text.append(buf, n);
        }
        std::fclose(f);
        Parse(text);
        return true;
    }

    bool Save(const char* path) const {
        std::FILE* f = std::fopen(path, "wb");
        if (!f) {
            return false;
        }
        const std::string text = Text();
        const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
        return std::fclose(f) == 0 && ok;
    }

    size_t Size() const {
        return moons.size();
    }

private:
    struct Cluster {
        std::array<float, 3> mean{};
        float n{};     // weight of the running mean, up to MaxSamples
        float votes{}; // finds that landed within Gate

        void Add(const std::array<float, 3>& pos, float samples) {
            votes += samples;
            n = std::min(n + samples, MaxSamples);
            for (int i = 0; i < 3; ++i) {
                mean[i] += (pos[i] - mean[i]) * (samples / n);
            }
        }
    };
    struct Entry {
        Cluster moon, cand;
        std::array<float, 3> saved{};
    };

    static float Dist2(const std::array<float, 3>& a, const std::array<float, 3>& b) {
        const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
        return dx * dx + dy * dy + dz * dz;
    }

    std::unordered_map<uint64_t, Entry> moons;
};

} // namespace Vulkan::Rt

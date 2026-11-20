#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <string>
#include <vector>

// The moon's world position per sky model, so a load facing away from the moon still lights
// from it. The game draws its moon as one small mesh of the sky model (rt_gi.h SkyLayerPs) only
// while the moon is in view; the cloud segments of the same model draw every frame. The moon
// mesh is fixed in the world about 750 m out (Hemwick graveyard), not at infinity, so its
// direction moves with the player: a position is kept, and the light points from the camera at
// it. A sky is known by its layer draws (index and vertex counts); each moon keeps the draws seen
// while it was found, in a small text file, one line per moon.
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

    // Frames since the last layer draw (~0 before the first).
    uint64_t Age(uint64_t frame_now) const {
        return frame == ~0ull ? ~0ull : frame_now - frame;
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

// One entry per moon, each with the layer draws seen while it was found. Maps share cloud
// segments (Forbidden Woods and Cathedral Ward: two of Cathedral Ward's three), and one moon per
// draw let each map's finds take the shared draws over (SwitchVotes): the next visit to the other
// map lit from the first map's moon until the player looked at the disc, then the light swung
// 50-60 degrees. A run now takes the moon whose draws best match its own (shared over all of
// both), so a map whose draws are a subset of another's still gets its own.
class MoonCache {
public:
    // Two finds within this angle (radians, about 1 degree) confirm the disc.
    static constexpr float Same = 0.0175f;
    // Each find lands up to ~25 m off the disc centre, so the position is a running mean of the
    // finds that land within Gate of it. A find farther off starts another moon: one spot in
    // Hemwick kept finding a point 52 m off, another find was 123 degrees off at the horizon.
    // The file is rewritten once a mean moved Moved metres.
    static constexpr float Gate = 30.0f;
    static constexpr float Moved = 2.0f;
    // Weight cap of the running mean; capped at MaxVotes it is the plain mean of all finds.
    static constexpr float MaxSamples = 1000.0f;
    // A position read from a file line without votes counts as this many finds.
    static constexpr float LoadedSamples = 8.0f;
    // Votes saved per moon are capped, so a newer moon can still win in a long session.
    static constexpr float MaxVotes = 1000.0f;
    // Two moons that match a run's draws equally well: within SwitchFar of each other (a stray
    // 52 m off the moon, from one spot) the one with more finds wins; farther apart (the game
    // places its moon per area; a wrong spot once had 390 votes in Central Yharnam, 87 degrees
    // off) the one confirmed last wins once it has SwitchVotes finds.
    static constexpr float SwitchVotes = 8.0f;
    static constexpr float SwitchFar = 150.0f;

    // A confirmed find while the run drew `draws`. Returns true when the file should be saved.
    bool PutDraws(const std::vector<uint64_t>& draws, const std::array<float, 3>& pos,
                  float samples = 1.0f) {
        if (draws.empty() || !std::isfinite(pos[0]) || !std::isfinite(pos[1]) ||
            !std::isfinite(pos[2])) {
            return false;
        }
        Moon* hit = nullptr;
        float best = Gate * Gate;
        for (Moon& m : moons) {
            const float d = Dist2(m.mean, pos);
            if (d <= best) {
                best = d;
                hit = &m;
            }
        }
        if (!hit) {
            Moon m;
            m.mean = pos;
            m.saved = pos;
            m.n = std::min(samples, MaxSamples);
            m.votes = std::min(samples, MaxVotes);
            m.seq = ++seq_last;
            m.draws = Sorted(draws);
            moons.push_back(std::move(m));
            return true;
        }
        hit->Add(pos, samples);
        hit->seq = ++seq_last;
        bool changed = false;
        for (const uint64_t d : draws) {
            const auto at = std::lower_bound(hit->draws.begin(), hit->draws.end(), d);
            if (at == hit->draws.end() || *at != d) {
                hit->draws.insert(at, d);
                changed = true;
            }
        }
        if (Dist2(hit->mean, hit->saved) >= Moved * Moved) {
            hit->saved = hit->mean;
            changed = true;
        }
        return changed;
    }

    // The moon of a run that drew `draws`: best share of draws in common (over the draws of both),
    // ties as SwitchFar says. False when no moon shares a draw.
    bool FindDraws(const std::vector<uint64_t>& draws, std::array<float, 3>& pos) const {
        const Moon* m = Best(draws);
        if (m) {
            pos = m->mean;
        }
        return m != nullptr;
    }

    // Finds counted for the moon of these draws (0 when there is none).
    float Votes(const std::vector<uint64_t>& draws) const {
        const Moon* m = Best(draws);
        return m ? m->votes : 0.0f;
    }

    bool DirFromDraws(const std::vector<uint64_t>& draws, const std::array<float, 3>& cam,
                      std::array<float, 3>& dir) const {
        std::array<float, 3> pos{};
        return FindDraws(draws, pos) && DirTo(pos, cam, dir);
    }

    // The unit direction from the camera to a moon, or false when the camera is at it.
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

    // One "moon x y z votes order draw,draw,..." line per moon (draws in hex).
    std::string Text() const {
        std::string out;
        char line[160];
        for (const Moon& m : moons) {
            std::snprintf(line, sizeof(line), "moon %.2f %.2f %.2f %.0f %llu ", m.mean[0],
                          m.mean[1], m.mean[2], std::min(m.votes, MaxVotes),
                          (unsigned long long)m.seq);
            out += line;
            for (size_t i = 0; i < m.draws.size(); ++i) {
                std::snprintf(line, sizeof(line), i ? ",%llx" : "%llx",
                              (unsigned long long)m.draws[i]);
                out += line;
            }
            out += '\n';
        }
        return out;
    }

    // Lines that do not parse are skipped. The old "draw x y z [votes]" lines (one moon per
    // draw) merge into moons by position.
    void Parse(const std::string& text) {
        size_t at = 0;
        while (at < text.size()) {
            size_t end = text.find('\n', at);
            if (end == std::string::npos) {
                end = text.size();
            }
            const std::string line = text.substr(at, end - at);
            at = end + 1;
            std::array<float, 3> d{};
            float votes = LoadedSamples;
            if (line.rfind("moon ", 0) == 0) {
                unsigned long long seq = 0;
                int used = 0;
                if (std::sscanf(line.c_str() + 5, "%f %f %f %f %llu %n", &d[0], &d[1], &d[2],
                                &votes, &seq, &used) < 5 ||
                    !Finite(d)) {
                    continue;
                }
                std::vector<uint64_t> draws;
                const char* p = line.c_str() + 5 + used;
                while (*p) {
                    char* next = nullptr;
                    const unsigned long long k = std::strtoull(p, &next, 16);
                    if (next == p) {
                        break;
                    }
                    if (k != 0) {
                        draws.push_back(k);
                    }
                    p = *next == ',' ? next + 1 : next;
                }
                if (draws.empty()) {
                    continue;
                }
                Moon m;
                m.mean = d;
                m.saved = d;
                m.votes = std::isfinite(votes) && votes >= 1.0f ? std::min(votes, MaxVotes)
                                                                : LoadedSamples;
                m.n = std::min(m.votes, MaxSamples);
                m.seq = seq;
                m.draws = Sorted(draws);
                seq_last = std::max<uint64_t>(seq_last, seq);
                moons.push_back(std::move(m));
                continue;
            }
            unsigned long long k = 0;
            const int got =
                std::sscanf(line.c_str(), "%llx %f %f %f %f", &k, &d[0], &d[1], &d[2], &votes);
            if (got >= 4 && k != 0 && Finite(d)) {
                const float v = std::isfinite(votes) && votes >= 1.0f ? std::min(votes, MaxVotes)
                                                                      : LoadedSamples;
                PutDraws({uint64_t(k)}, d, v);
            }
        }
        for (Moon& m : moons) {
            m.saved = m.mean;
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
    struct Moon {
        std::array<float, 3> mean{};
        std::array<float, 3> saved{};
        float n{};     // weight of the running mean, up to MaxSamples
        float votes{}; // finds that landed within Gate
        uint64_t seq{}; // order of the last find
        std::vector<uint64_t> draws; // sorted

        void Add(const std::array<float, 3>& pos, float samples) {
            votes = std::min(votes + samples, MaxVotes);
            n = std::min(n + samples, MaxSamples);
            for (int i = 0; i < 3; ++i) {
                mean[i] += (pos[i] - mean[i]) * (samples / n);
            }
        }
    };

    // a over b for a run whose draws both match equally well.
    static bool Wins(const Moon& a, const Moon& b) {
        if (Dist2(a.mean, b.mean) <= SwitchFar * SwitchFar) {
            return a.votes != b.votes ? a.votes > b.votes : a.seq > b.seq;
        }
        const bool ea = a.votes >= SwitchVotes, eb = b.votes >= SwitchVotes;
        if (ea != eb) {
            return ea;
        }
        return ea ? a.seq > b.seq : (a.votes != b.votes ? a.votes > b.votes : a.seq > b.seq);
    }

    const Moon* Best(const std::vector<uint64_t>& draws) const {
        const std::vector<uint64_t> run = Sorted(draws);
        const Moon* best = nullptr;
        float best_score = 0.0f;
        for (const Moon& m : moons) {
            size_t shared = 0;
            for (size_t i = 0, j = 0; i < run.size() && j < m.draws.size();) {
                if (run[i] == m.draws[j]) {
                    ++shared;
                    ++i;
                    ++j;
                } else if (run[i] < m.draws[j]) {
                    ++i;
                } else {
                    ++j;
                }
            }
            if (shared == 0) {
                continue;
            }
            const float score = float(shared) / float(run.size() + m.draws.size() - shared);
            if (!best || score > best_score + 1e-6f ||
                (score > best_score - 1e-6f && Wins(m, *best))) {
                best = &m;
                best_score = std::max(best_score, score);
            }
        }
        return best;
    }

    static std::vector<uint64_t> Sorted(std::vector<uint64_t> v) {
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
        return v;
    }

    static bool Finite(const std::array<float, 3>& p) {
        return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]);
    }

    static float Dist2(const std::array<float, 3>& a, const std::array<float, 3>& b) {
        const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
        return dx * dx + dy * dy + dz * dz;
    }

    std::vector<Moon> moons;
    uint64_t seq_last = 0;
};

} // namespace Vulkan::Rt

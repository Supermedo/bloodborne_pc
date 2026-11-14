// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// bbport: keeps most of the game's fog off the moon disc. The sky dome pixel shader (MoonDomePs)
// mixes every texel toward the fog colour twice, by a distance fog and a height fog factor:
//     f = NClamp(x, 0, 1);  out_c = f * (fog_c - base_c) + base_c   (c = r, g, b)
// From one side of Hemwick that left the moon a flat grey disc (centre luma 54, contrast 1.26;
// 148 and 2.6 from the other side). This pass rewrites the SPIR-V the recompiler emits so both
// factors are scaled by 1 - cut * smoothstep(lo, hi, min(base_r, base_g, base_b)): only the disc
// reaches that range in this shader (disc 0.25-1, horizon band about 0.02). cut 0.09 measured:
// centre 175, max 215, maria kept; 1.0 clips the disc to white. It edits only the shader the
// player's own game produced, so nothing of the game ships with the port.
//
// The factors are found by shape, not by id: an NClamp(x, 0.0, 1.0) whose result feeds exactly
// three FMul, each feeding one FAdd. The first such factor's FAdd operands are the unfogged
// colour. Anything else (another recompiler build, another layout) returns nullopt: the shader
// stays as it was.

#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace Vulkan::Rt {

constexpr uint64_t MoonDomePs = 0xdda49e89u;

inline std::optional<std::vector<uint32_t>> MoonHazeFix(std::span<const uint32_t> spv, float cut,
                                                        float lo = 0.1f, float hi = 0.2f) {
    constexpr uint32_t OpExtInstImport = 11, OpExtInst = 12, OpTypeFloat = 22, OpConstant = 43,
                       OpFunction = 54, OpFAdd = 129, OpFSub = 131, OpFMul = 133;
    constexpr uint32_t GlslFMin = 37, GlslSmoothStep = 49, GlslNClamp = 81;
    if (spv.size() < 5 || spv[0] != 0x07230203u || cut <= 0.0f) {
        return std::nullopt;
    }
    struct Inst {
        size_t at;
        uint32_t op, count;
    };
    std::vector<Inst> insts;
    for (size_t i = 5; i < spv.size();) {
        const uint32_t count = spv[i] >> 16;
        if (count == 0 || i + count > spv.size()) {
            return std::nullopt;
        }
        insts.push_back({i, spv[i] & 0xffffu, count});
        i += count;
    }
    const auto word = [&](const Inst& in, uint32_t k) { return spv[in.at + k]; };

    uint32_t glsl = 0;
    size_t first_function = 0;
    std::unordered_map<uint32_t, float> float_consts;
    std::unordered_map<uint32_t, size_t> def; // result id -> instruction index
    std::unordered_map<uint32_t, bool> is_float_type;
    for (size_t n = 0; n < insts.size(); ++n) {
        const Inst& in = insts[n];
        if (in.op == OpExtInstImport && in.count >= 3 &&
            std::memcmp(&spv[in.at + 2], "GLSL.std.450", 12) == 0) {
            glsl = word(in, 1);
        } else if (in.op == OpTypeFloat && in.count == 3 && word(in, 2) == 32) {
            is_float_type[word(in, 1)] = true;
        } else if (in.op == OpConstant && in.count == 4 && is_float_type.count(word(in, 1))) {
            float v;
            const uint32_t bits = word(in, 3);
            std::memcpy(&v, &bits, 4);
            float_consts[word(in, 2)] = v;
        } else if (in.op == OpFunction && !first_function) {
            first_function = n;
        }
        if ((in.op == OpExtInst || in.op == OpFMul || in.op == OpFAdd) && in.count >= 3) {
            def[word(in, 2)] = n;
        }
    }
    if (!glsl || !first_function) {
        return std::nullopt;
    }
    const auto is_const = [&](uint32_t id, float v) {
        const auto it = float_consts.find(id);
        return it != float_consts.end() && it->second == v;
    };

    // Fog factors: NClamp(x, 0, 1) -> 3x FMul(f, d) -> FAdd(mul, base).
    struct Factor {
        size_t index;
        uint32_t id, type;
        std::vector<size_t> muls;
        std::vector<uint32_t> bases;
    };
    std::vector<Factor> factors;
    for (size_t n = first_function; n < insts.size(); ++n) {
        const Inst& in = insts[n];
        if (in.op != OpExtInst || in.count != 8 || word(in, 3) != glsl ||
            word(in, 4) != GlslNClamp || !is_const(word(in, 6), 0.0f) ||
            !is_const(word(in, 7), 1.0f)) {
            continue;
        }
        Factor f{n, word(in, 2), word(in, 1), {}, {}};
        bool other_use = false;
        for (size_t u = n + 1; u < insts.size(); ++u) {
            const Inst& use = insts[u];
            bool uses = false;
            for (uint32_t k = 3; k < use.count; ++k) {
                uses |= word(use, k) == f.id;
            }
            if (!uses) {
                continue;
            }
            if (use.op != OpFMul || use.count != 5) {
                other_use = true;
                break;
            }
            const uint32_t mul = word(use, 2);
            uint32_t base = 0, adds = 0;
            for (size_t a = u + 1; a < insts.size(); ++a) {
                const Inst& add = insts[a];
                for (uint32_t k = 3; k < add.count; ++k) {
                    if (word(add, k) != mul) {
                        continue;
                    }
                    ++adds;
                    if (add.op == OpFAdd && add.count == 5) {
                        base = word(add, 3) == mul ? word(add, 4) : word(add, 3);
                    }
                }
            }
            if (adds != 1 || !base) {
                other_use = true;
                break;
            }
            f.muls.push_back(u);
            f.bases.push_back(base);
        }
        if (!other_use && f.muls.size() == 3) {
            factors.push_back(std::move(f));
        }
    }
    if (factors.size() != 2) {
        return std::nullopt;
    }
    const Factor& f1 = factors[0];
    const Factor& f2 = factors[1];
    const size_t insert_at = f1.muls[0]; // the mask goes right before the first fog multiply
    for (const uint32_t base : f1.bases) {
        const auto it = def.find(base);
        if (it != def.end() && it->second >= insert_at) {
            return std::nullopt; // a base defined later: not the expected layout
        }
    }
    if (f2.index < insert_at) {
        return std::nullopt;
    }

    uint32_t bound = spv[3];
    const uint32_t type = f1.type;
    const auto bits = [](float v) {
        uint32_t b;
        std::memcpy(&b, &v, 4);
        return b;
    };
    const uint32_t c_lo = bound++, c_hi = bound++, c_cut = bound++, c_one = bound++;
    const uint32_t min01 = bound++, min012 = bound++, moon = bound++, scaled = bound++,
                   keep = bound++, f1_new = bound++, f2_new = bound++;
    const std::vector<uint32_t> consts{
        (4u << 16) | OpConstant, type, c_lo,  bits(lo),  (4u << 16) | OpConstant, type, c_hi,
        bits(hi),                (4u << 16) | OpConstant, type, c_cut, bits(cut),
        (4u << 16) | OpConstant, type, c_one, bits(1.0f)};
    const std::vector<uint32_t> mask{
        (7u << 16) | OpExtInst, type, min01, glsl, GlslFMin, f1.bases[0], f1.bases[1],
        (7u << 16) | OpExtInst, type, min012, glsl, GlslFMin, min01, f1.bases[2],
        (8u << 16) | OpExtInst, type, moon, glsl, GlslSmoothStep, c_lo, c_hi, min012,
        (5u << 16) | OpFMul, type, scaled, moon, c_cut,
        (5u << 16) | OpFSub, type, keep, c_one, scaled,
        (5u << 16) | OpFMul, type, f1_new, f1.id, keep};
    const std::vector<uint32_t> f2_scale{(5u << 16) | OpFMul, type, f2_new, f2.id, keep};

    std::vector<uint32_t> out;
    out.reserve(spv.size() + consts.size() + mask.size() + f2_scale.size());
    out.insert(out.end(), spv.begin(), spv.begin() + 5);
    out[3] = bound;
    for (size_t n = 0; n < insts.size(); ++n) {
        const Inst& in = insts[n];
        if (n == first_function) {
            out.insert(out.end(), consts.begin(), consts.end());
        }
        if (n == insert_at) {
            out.insert(out.end(), mask.begin(), mask.end());
        }
        const size_t start = out.size();
        out.insert(out.end(), spv.begin() + in.at, spv.begin() + in.at + in.count);
        const Factor* f = nullptr;
        for (const Factor* c : {&f1, &f2}) {
            for (const size_t m : c->muls) {
                f = m == n ? c : f;
            }
        }
        if (f) {
            for (uint32_t k = 3; k < in.count; ++k) {
                if (out[start + k] == f->id) {
                    out[start + k] = f == &f1 ? f1_new : f2_new;
                }
            }
        }
        if (n == f2.index) {
            out.insert(out.end(), f2_scale.begin(), f2_scale.end());
        }
    }
    return out;
}

} // namespace Vulkan::Rt

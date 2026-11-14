#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace Vulkan::Rt {

constexpr uint32_t NoUv = 0xFFFFFFFFu;
constexpr uint32_t UvBytes = 4;
constexpr uint32_t TriangleWords = 8;
constexpr uint32_t TriangleNormalWord = 3;
constexpr uint32_t TriangleVertexNormalWord = 4;
constexpr uint32_t TriangleTangentWord = 7;
constexpr uint32_t TriangleUvBytes = TriangleWords * UvBytes;
constexpr uint32_t VertexNormalBytes = 8;
constexpr uint32_t SurfaceNormalMapShift = 16;
constexpr uint32_t NormalSelectAValid = 1u << 21;
constexpr uint32_t NormalSelectBValid = 1u << 27;

constexpr uint32_t NormalSelect(int32_t param_a, int32_t param_b) {
    uint32_t select = 0;
    if (param_a >= 0 && param_a <= 31) {
        select |= NormalSelectAValid | uint32_t(param_a) << 16;
    }
    if (param_b >= 0 && param_b <= 31) {
        select |= NormalSelectBValid | uint32_t(param_b) << 22;
    }
    return select;
}

constexpr std::array<int32_t, 2> NormalSelectParams(uint32_t select) {
    return {(select & NormalSelectAValid) ? int32_t((select >> 16) & 31u) : -1,
            (select & NormalSelectBValid) ? int32_t((select >> 22) & 31u) : -1};
}

constexpr bool NormalSelectValid(uint32_t select) {
    return (select & (NormalSelectAValid | NormalSelectBValid)) != 0;
}

constexpr float SurfDot(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

constexpr std::array<float, 3> SurfCross(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

inline std::array<float, 3> SurfUnit(std::array<float, 3> v) {
    const float len = std::sqrt(SurfDot(v, v));
    if (!(len > 1e-12f)) {
        return {0.0f, 0.0f, 0.0f};
    }
    return {v[0] / len, v[1] / len, v[2] / len};
}

constexpr float PickNormalMinDot = 0.5f;

inline std::array<float, 3> PickVertexNormal(const std::array<std::array<float, 3>, 4>& candidates,
                                             const std::array<float, 3>& flat) {
    std::array<float, 3> best{0.0f, 0.0f, 0.0f};
    float best_dot = PickNormalMinDot;
    for (const auto& c : candidates) {
        const std::array<float, 3> n = SurfUnit(c);
        const float d = SurfDot(n, flat);
        const float a = d < 0.0f ? -d : d;
        if (a > best_dot) {
            best_dot = a;
            best = d < 0.0f ? std::array<float, 3>{-n[0], -n[1], -n[2]} : n;
        }
    }
    return best;
}

inline std::array<float, 3> InterpolateNormal(const std::array<float, 3>& a,
                                              const std::array<float, 3>& b,
                                              const std::array<float, 3>& c,
                                              const std::array<float, 2>& barycentrics) {
    const float w = 1.0f - barycentrics[0] - barycentrics[1];
    return SurfUnit({w * a[0] + barycentrics[0] * b[0] + barycentrics[1] * c[0],
                  w * a[1] + barycentrics[0] * b[1] + barycentrics[1] * c[1],
                  w * a[2] + barycentrics[0] * b[2] + barycentrics[1] * c[2]});
}

inline std::array<float, 3> DecodeNormalMap(float r, float g, float b) {
    const float x = r * 2.0f - 1.0f;
    const float y = g * 2.0f - 1.0f;
    const float z = b > 0.0f ? b * 2.0f - 1.0f : std::sqrt(std::max(0.0f, 1.0f - x * x - y * y));
    return SurfUnit({x, y, z});
}

struct Tangent {
    std::array<float, 3> t{};
    float handedness{1.0f};
    bool valid{};
};

inline Tangent TangentFromUv(const std::array<float, 3>& p0, const std::array<float, 3>& p1,
                             const std::array<float, 3>& p2, const std::array<float, 2>& uv0,
                             const std::array<float, 2>& uv1, const std::array<float, 2>& uv2,
                             const std::array<float, 3>& n) {
    const std::array<float, 3> e1{p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
    const std::array<float, 3> e2{p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
    const float du1 = uv1[0] - uv0[0], dv1 = uv1[1] - uv0[1];
    const float du2 = uv2[0] - uv0[0], dv2 = uv2[1] - uv0[1];
    const float det = du1 * dv2 - du2 * dv1;
    Tangent out{};
    if (!(std::fabs(det) > 1e-12f)) {
        return out;
    }
    const float inv = 1.0f / det;
    std::array<float, 3> t{(e1[0] * dv2 - e2[0] * dv1) * inv, (e1[1] * dv2 - e2[1] * dv1) * inv,
                           (e1[2] * dv2 - e2[2] * dv1) * inv};
    const std::array<float, 3> bt{(e2[0] * du1 - e1[0] * du2) * inv,
                                  (e2[1] * du1 - e1[1] * du2) * inv,
                                  (e2[2] * du1 - e1[2] * du2) * inv};
    const float nt = SurfDot(t, n);
    t = SurfUnit({t[0] - n[0] * nt, t[1] - n[1] * nt, t[2] - n[2] * nt});
    if (SurfDot(t, t) < 0.5f) {
        return out;
    }
    out.t = t;
    out.handedness = SurfDot(SurfCross(n, t), bt) < 0.0f ? -1.0f : 1.0f;
    out.valid = true;
    return out;
}

inline std::array<float, 3> PerturbNormal(const std::array<float, 3>& n, const Tangent& tangent,
                                          const std::array<float, 3>& map) {
    if (!tangent.valid) {
        return n;
    }
    const std::array<float, 3> b0 = SurfCross(n, tangent.t);
    const std::array<float, 3> b{b0[0] * tangent.handedness, b0[1] * tangent.handedness,
                                 b0[2] * tangent.handedness};
    const std::array<float, 3> out = SurfUnit({tangent.t[0] * map[0] + b[0] * map[1] + n[0] * map[2],
                                            tangent.t[1] * map[0] + b[1] * map[1] + n[1] * map[2],
                                            tangent.t[2] * map[0] + b[2] * map[1] + n[2] * map[2]});
    return SurfDot(out, out) > 0.5f ? out : n;
}

constexpr uint32_t PassSurfaceSlot(uint64_t built_frame, uint32_t slots) {
    return uint32_t(built_frame % slots);
}

constexpr bool SurfaceSlotMatchesBuild(uint64_t written_frame, uint64_t built_frame) {
    return written_frame == built_frame;
}

constexpr uint32_t UvSelectValid = 0x80000000u;
constexpr uint32_t MaxSurfaceTextures = 1024;

constexpr uint32_t SurfaceAlphaTest = 1;
constexpr uint32_t SurfaceHasUv = 2;
constexpr uint32_t SurfaceHasTexture = 4;
constexpr uint32_t SurfaceHasNormal = 8;
constexpr uint32_t SurfaceOmm = 16;
constexpr uint32_t SurfaceBlended = 32;
constexpr uint32_t SurfaceAlphaSlotShift = 6;
constexpr uint32_t SurfaceAlphaSlotMask = 0x3FFu;
constexpr uint32_t SurfaceAlphaChannelShift = 26;
// Drawn by a BB_RT_GLASS_PS shader: rt_gi.comp (RT_REFLECT) lets glossy pixels of it see through.
constexpr uint32_t SurfaceGlass = 1u << 28;
// Skinned / animated mesh (characters, enemies): the fill-light enclosure test ignores it, so a
// character standing next to a fill light does not make it look like a lamp in its fixture.
constexpr uint32_t SurfaceSkinned = 1u << 29;
// Drawn by a BB_RT_PORTAL_PS shader: a painted window (lit pane on a wall, no opening). A shadow
// ray whose closest hit is a portal pane reaches the outside (rt_direct.comp / rt_gi.comp).
constexpr uint32_t SurfacePortal = 1u << 30;
constexpr uint32_t SurfaceWater = 1u << 31;

// flags2 bits.
// Drawn with face culling on: a closed solid, never a two-sided card. rt_direct.comp ThinCard only
// takes a back-face camera hit as a card when this is clear (a mirrored or reversed-winding wall
// mesh shows the camera its TLAS back face and let the moon through, Cathedral Ward coffin alcove).
constexpr uint32_t Surface2OneSided = 1;

struct SurfaceRecord {
    uint32_t texture{};
    uint32_t flags{};
    uint32_t uv_base{NoUv};
    float alpha_ref{0.5f};
    uint32_t flags2{};
};
static_assert(sizeof(SurfaceRecord) == 20);

constexpr uint32_t UvSelect(int32_t param, uint32_t component_u, uint32_t component_v) {
    if (param < 0 || param > 31 || component_u > 3 || component_v > 3) {
        return 0;
    }
    return UvSelectValid | uint32_t(param) | component_u << 8 | component_v << 12;
}

constexpr bool UvSelectIsValid(uint32_t select) {
    return (select & UvSelectValid) != 0;
}

constexpr uint32_t UvSelectParam(uint32_t select) {
    return select & 0xFFu;
}

constexpr std::array<uint32_t, 2> UvSelectComponents(uint32_t select) {
    return {(select >> 8) & 3u, (select >> 12) & 3u};
}

inline uint16_t FloatToHalf(float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    const uint32_t sign = (bits >> 16) & 0x8000u;
    int32_t exponent = int32_t((bits >> 23) & 0xFFu) - 127 + 15;
    uint32_t mantissa = bits & 0x7FFFFFu;
    if (((bits >> 23) & 0xFFu) == 0xFFu) {
        return uint16_t(sign | 0x7C00u | (mantissa ? 0x200u : 0u));
    }
    if (exponent >= 31) {
        return uint16_t(sign | 0x7C00u);
    }
    if (exponent <= 0) {
        if (exponent < -10) {
            return uint16_t(sign);
        }
        mantissa |= 0x800000u;
        const uint32_t shift = uint32_t(14 - exponent);
        uint32_t half_mantissa = mantissa >> shift;
        const uint32_t rest = mantissa & ((1u << shift) - 1u);
        const uint32_t halfway = 1u << (shift - 1);
        if (rest > halfway || (rest == halfway && (half_mantissa & 1u))) {
            ++half_mantissa;
        }
        return uint16_t(sign | half_mantissa);
    }
    uint32_t half = sign | uint32_t(exponent) << 10 | mantissa >> 13;
    const uint32_t rest = mantissa & 0x1FFFu;
    if (rest > 0x1000u || (rest == 0x1000u && (half & 1u))) {
        ++half;
    }
    return uint16_t(half);
}

inline float HalfToFloat(uint16_t h) {
    const uint32_t sign = uint32_t(h & 0x8000u) << 16;
    uint32_t exponent = (h >> 10) & 0x1Fu;
    uint32_t mantissa = h & 0x3FFu;
    uint32_t bits;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            exponent = 127 - 15 + 1;
            while ((mantissa & 0x400u) == 0) {
                mantissa <<= 1;
                --exponent;
            }
            mantissa &= 0x3FFu;
            bits = sign | exponent << 23 | mantissa << 13;
        }
    } else if (exponent == 31) {
        bits = sign | 0x7F800000u | mantissa << 13;
    } else {
        bits = sign | (exponent + 127 - 15) << 23 | mantissa << 13;
    }
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

inline uint32_t PackUv(float u, float v) {
    return uint32_t(FloatToHalf(u)) | uint32_t(FloatToHalf(v)) << 16;
}

inline std::array<float, 2> UnpackUv(uint32_t packed) {
    return {HalfToFloat(uint16_t(packed & 0xFFFFu)), HalfToFloat(uint16_t(packed >> 16))};
}

constexpr std::array<float, 2> InterpolateUv(const std::array<float, 2>& a,
                                             const std::array<float, 2>& b,
                                             const std::array<float, 2>& c,
                                             const std::array<float, 2>& barycentrics) {
    const float w = 1.0f - barycentrics[0] - barycentrics[1];
    return {w * a[0] + barycentrics[0] * b[0] + barycentrics[1] * c[0],
            w * a[1] + barycentrics[0] * b[1] + barycentrics[1] * c[1]};
}

constexpr uint64_t UvOffset(uint32_t uv_base, uint32_t triangle, uint32_t vertex) {
    return (uint64_t(uv_base) + uint64_t(triangle) * TriangleWords + vertex) * UvBytes;
}

class TextureTable {
public:
    struct Stats {
        uint64_t frames{}, assigned{}, reused{}, full{};
    } stats;

    explicit TextureTable(uint32_t capacity_ = MaxSurfaceTextures) : capacity{capacity_} {}

    void BeginFrame() {
        ++stats.frames;
        slots.clear();
        order.clear();
        order.push_back(0);
    }

    uint32_t Slot(uint64_t view) {
        if (!view) {
            return 0;
        }
        if (const auto it = slots.find(view); it != slots.end()) {
            ++stats.reused;
            return it->second;
        }
        if (order.size() >= capacity) {
            ++stats.full;
            return 0;
        }
        const uint32_t slot = uint32_t(order.size());
        order.push_back(view);
        slots.emplace(view, slot);
        ++stats.assigned;
        return slot;
    }

    uint32_t Used() const { return uint32_t(order.size()); }
    uint64_t View(uint32_t slot) const { return slot < order.size() ? order[slot] : 0; }

private:
    uint32_t capacity;
    std::unordered_map<uint64_t, uint32_t> slots;
    std::vector<uint64_t> order;
};

enum class CasterAlpha : uint8_t { None, Own, Prepass, Coverage, Blend };

constexpr CasterAlpha PickCasterAlpha(bool own_discard, bool inherit, bool prepass_match,
                                      bool alpha_to_mask, bool blended_no_depth_write) {
    if (own_discard) {
        return CasterAlpha::Own;
    }
    if (!inherit) {
        return CasterAlpha::None;
    }
    if (prepass_match) {
        return CasterAlpha::Prepass;
    }
    if (alpha_to_mask) {
        return CasterAlpha::Coverage;
    }
    return blended_no_depth_write ? CasterAlpha::Blend : CasterAlpha::None;
}

constexpr const char* CasterAlphaName(CasterAlpha c) {
    switch (c) {
    case CasterAlpha::Own:
        return "own discard";
    case CasterAlpha::Prepass:
        return "depth prepass discard";
    case CasterAlpha::Coverage:
        return "alpha to coverage";
    case CasterAlpha::Blend:
        return "blended, no depth write";
    default:
        return "opaque";
    }
}

constexpr uint64_t PrepassKey(uint64_t index_address, uint32_t index_count, uint32_t instances,
                              int32_t base_vertex, uint32_t first_instance) {
    if (!index_address || !index_count) {
        return 0;
    }
    uint64_t h = 0x9E3779B97F4A7C15ull;
    const uint64_t parts[3] = {index_address, uint64_t(index_count) | uint64_t(instances) << 32,
                               uint64_t(uint32_t(base_vertex)) | uint64_t(first_instance) << 32};
    for (const uint64_t p : parts) {
        h ^= p + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
        h ^= h >> 31;
        h *= 0xBF58476D1CE4E5B9ull;
        h ^= h >> 27;
    }
    return h ? h : 1;
}

constexpr SurfaceRecord MakeSurface(uint32_t texture, bool alpha_test, uint32_t uv_base,
                                    float alpha_ref, bool uv_captured = true,
                                    bool omm = false, uint32_t normal_map = 0,
                                    bool blended = false, uint32_t alpha_texture = 0,
                                    uint32_t alpha_channel = 3) {
    SurfaceRecord r{};
    r.texture = texture;
    r.uv_base = uv_base;
    r.alpha_ref = alpha_ref;
    const bool has_uv = uv_base != NoUv && uv_captured;
    const uint32_t alpha_slot =
        alpha_texture && alpha_texture < MaxSurfaceTextures && alpha_texture != texture ? alpha_texture
                                                                                       : 0u;
    const bool alpha_source = texture || alpha_slot;
    r.flags = (has_uv ? SurfaceHasUv : 0u) | (texture ? SurfaceHasTexture : 0u) |
              (uv_base != NoUv ? SurfaceHasNormal : 0u) | (omm ? SurfaceOmm : 0u) | (blended ? SurfaceBlended : 0u) |
              (alpha_test && alpha_source && has_uv ? SurfaceAlphaTest : 0u) |
              (has_uv && normal_map < MaxSurfaceTextures ? normal_map << SurfaceNormalMapShift : 0u) |
              alpha_slot << SurfaceAlphaSlotShift | ((alpha_channel & 3u) ^ 3u) << SurfaceAlphaChannelShift;
    return r;
}

constexpr uint32_t SurfaceAlphaTexture(const SurfaceRecord& r) {
    const uint32_t slot = (r.flags >> SurfaceAlphaSlotShift) & SurfaceAlphaSlotMask;
    return slot ? slot : r.texture;
}

constexpr uint32_t SurfaceAlphaChannel(const SurfaceRecord& r) {
    return ((r.flags >> SurfaceAlphaChannelShift) & 3u) ^ 3u;
}

constexpr uint32_t PackNormal(const std::array<float, 3>& n) {
    uint32_t packed = 0;
    for (int i = 0; i < 3; ++i) {
        const float c = n[i] < -1.0f ? -1.0f : n[i] > 1.0f ? 1.0f : n[i];
        const int v = int(c * 127.0f + (c >= 0.0f ? 0.5f : -0.5f));
        packed |= uint32_t(uint8_t(v)) << (8 * i);
    }
    return packed;
}

constexpr std::array<float, 3> UnpackNormal(uint32_t packed) {
    std::array<float, 3> n{};
    for (int i = 0; i < 3; ++i) {
        const int v = int(int8_t(uint8_t(packed >> (8 * i))));
        n[i] = float(v < -127 ? -127 : v) / 127.0f;
    }
    return n;
}

constexpr bool SurfaceTextureAlive(uint32_t slot, uint64_t view, uint32_t image, uint64_t uid_bound,
                                   uint64_t uid_now) {
    return slot != 0 && view != 0 && image != 0 && uid_bound != 0 && uid_now == uid_bound;
}

constexpr bool SurfaceUsable(const SurfaceRecord& r) {
    return (r.flags & (SurfaceHasUv | SurfaceHasTexture)) == (SurfaceHasUv | SurfaceHasTexture);
}

constexpr uint64_t HeldByRt = ~0ull;

template <class IsFree>
bool HoldReleased(uint64_t hold_tick, IsFree&& is_free) {
    return hold_tick != HeldByRt && (hold_tick == 0 || is_free(hold_tick));
}

constexpr bool KeptTextureAlive(bool allocated, bool same_uid, bool registered) {
    return allocated && same_uid && registered;
}

struct HeldImage {
    uint32_t image{};
    uint64_t uid{};
};

class SurfaceHolds {
public:
    static constexpr uint32_t NoSlot = ~0u;

    explicit SurfaceHolds(uint32_t slots = 3) : by_slot(slots) {}

    template <class Hold>
    void Bound(uint32_t image, uint64_t uid, Hold&& hold) {
        if (!uid) {
            return;
        }
        for (const HeldImage& h : frame) {
            if (h.image == image && h.uid == uid) {
                return;
            }
        }
        frame.push_back({image, uid});
        hold(image, uid, HeldByRt);
    }

    template <class Hold>
    void Finish(uint64_t tick, bool built, uint32_t slot, Hold&& hold) {
        for (const HeldImage& h : frame) {
            hold(h.image, h.uid, tick);
        }
        if (built && slot < by_slot.size()) {
            for (const HeldImage& h : by_slot[slot]) {
                hold(h.image, h.uid, tick);
            }
            if (held_slot != slot && held_slot < by_slot.size()) {
                for (const HeldImage& h : by_slot[held_slot]) {
                    hold(h.image, h.uid, tick);
                }
            }
            by_slot[slot] = frame;
            for (const HeldImage& h : by_slot[slot]) {
                hold(h.image, h.uid, HeldByRt);
            }
            held_slot = slot;
        }
        frame.clear();
    }

    template <class Hold>
    void ReleaseAll(uint64_t tick, Hold&& hold) {
        for (const HeldImage& h : frame) {
            hold(h.image, h.uid, tick);
        }
        frame.clear();
        for (auto& list : by_slot) {
            for (const HeldImage& h : list) {
                hold(h.image, h.uid, tick);
            }
            list.clear();
        }
        held_slot = NoSlot;
    }

    uint32_t HeldSlot() const { return held_slot; }
    size_t FrameCount() const { return frame.size(); }

private:
    std::vector<HeldImage> frame;
    std::vector<std::vector<HeldImage>> by_slot;
    uint32_t held_slot = NoSlot;
};

}

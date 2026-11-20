#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "rt_gi.h"
#include "rt_gi_pipeline.h"

namespace Vulkan::Rt {

struct LiveToggles {
    bool gi = false;
    bool direct = false;
    bool reflect = false;
    bool fog = true;
    bool ao = false;
    bool restir_gi = true;
    uint32_t gi_sparse = GiSparseDefault;
    bool restir_di = false;
    bool gi_pipeline = true;
    bool gi_ser = true;
    bool ptlas = false;
    bool rr = false;
    bool operator==(const LiveToggles&) const = default;
};

struct LiveChange {
    const char* key;
    uint32_t from;
    uint32_t to;
};

struct LiveResets {
    bool histories = false;
    bool ptlas_restart = false;
};

inline uint64_t ParseKnobU64(const char* value, uint64_t fallback, uint64_t lo, uint64_t hi) {
    if (!value || !*value) {
        return fallback;
    }
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    if (end == value) {
        return fallback;
    }
    return std::clamp<uint64_t>(parsed, lo, hi);
}

inline uint32_t GpuLineFrames(const char* value) {
    return uint32_t(ParseKnobU64(value, 600, 1, 100000));
}

template <class KnobFn>
LiveToggles ReadToggles(const KnobFn& knob, uint32_t vendor) {
    const auto is_one = [&](const char* name) {
        const char* value = knob(name);
        return value && value[0] == '1';
    };
    const auto not_zero = [&](const char* name) {
        const char* value = knob(name);
        return !value || value[0] != '0';
    };
    LiveToggles t;
    t.gi = is_one("BB_RT_GI");
    t.direct = is_one("BB_RT_DIRECT");
    const char* reflect = knob("BB_RT_REFLECT");
    t.reflect = reflect ? reflect[0] == '1' : t.direct;
    t.fog = not_zero("BB_RT_FX_FOG");
    t.ao = ParseKnobU64(knob("BB_RT_AO"), 0, 0, 1) == 1;
    t.restir_gi = RestirGiFromEnv(knob("BB_RT_RESTIR_GI"));
    t.gi_sparse = GiSparseLevel(uint32_t(ParseKnobU64(knob("BB_RT_GI_SPARSE"), GiSparseDefault, 1, 4)));
    t.restir_di = is_one("BB_RT_RESTIR");
    t.gi_pipeline = GiPipelineWanted(knob("BB_RT_GI_PIPELINE"), vendor);
    t.gi_ser = not_zero("BB_RT_GI_SER");
    t.ptlas = is_one("BB_RT_PTLAS");
    t.rr = RayReconstructionFromEnv(knob("BB_RT_DLSS_RR")) > 0;
    return t;
}

constexpr LiveToggles ResolveToggles(const LiveToggles& wanted, const LiveToggles& avail) {
    LiveToggles t;
    t.gi = wanted.gi && avail.gi;
    t.direct = wanted.direct && avail.direct;
    t.reflect = wanted.reflect && avail.reflect && t.gi;
    t.fog = wanted.fog && avail.fog;
    t.ao = wanted.ao && avail.ao && t.direct;
    t.restir_gi = wanted.restir_gi && avail.restir_gi && t.gi;
    t.gi_sparse = t.restir_gi ? GiSparseLevel(wanted.gi_sparse) : 1u;
    t.restir_di = wanted.restir_di && avail.restir_di && t.direct;
    t.gi_pipeline = wanted.gi_pipeline && avail.gi_pipeline && t.restir_gi;
    t.gi_ser = wanted.gi_ser && avail.gi_ser && t.gi_pipeline;
    t.ptlas = wanted.ptlas && avail.ptlas;
    t.rr = wanted.rr && avail.rr;
    return t;
}

inline std::vector<LiveChange> DiffToggles(const LiveToggles& a, const LiveToggles& b) {
    std::vector<LiveChange> out;
    const auto add = [&](const char* key, uint32_t from, uint32_t to) {
        if (from != to) {
            out.push_back({key, from, to});
        }
    };
    add("BB_RT_GI", a.gi, b.gi);
    add("BB_RT_DIRECT", a.direct, b.direct);
    add("BB_RT_REFLECT", a.reflect, b.reflect);
    add("BB_RT_FX_FOG", a.fog, b.fog);
    add("BB_RT_AO", a.ao, b.ao);
    add("BB_RT_RESTIR_GI", a.restir_gi, b.restir_gi);
    add("BB_RT_GI_SPARSE", a.gi_sparse, b.gi_sparse);
    add("BB_RT_RESTIR", a.restir_di, b.restir_di);
    add("BB_RT_GI_PIPELINE", a.gi_pipeline, b.gi_pipeline);
    add("BB_RT_GI_SER", a.gi_ser, b.gi_ser);
    add("BB_RT_PTLAS", a.ptlas, b.ptlas);
    add("BB_RT_DLSS_RR", a.rr, b.rr);
    return out;
}

constexpr LiveResets ResetsFor(const LiveToggles& a, const LiveToggles& b) {
    LiveToggles a_no_rr = a;
    LiveToggles b_no_rr = b;
    a_no_rr.rr = b_no_rr.rr = false;
    return {.histories = !(a_no_rr == b_no_rr), .ptlas_restart = !a.ptlas && b.ptlas};
}

constexpr LiveToggles NewFeatures(const LiveToggles& made, const LiveToggles& need) {
    LiveToggles t;
    t.gi = need.gi && !made.gi;
    t.direct = need.direct && !made.direct;
    t.reflect = need.reflect && !made.reflect;
    t.fog = need.fog && !made.fog;
    t.ao = need.ao && !made.ao;
    t.restir_gi = need.restir_gi && !made.restir_gi;
    t.gi_sparse = 1;
    t.restir_di = need.restir_di && !made.restir_di;
    t.gi_pipeline = need.gi_pipeline && !made.gi_pipeline;
    t.gi_ser = need.gi_ser && !made.gi_ser;
    t.ptlas = false;
    t.rr = false;
    return t;
}

constexpr LiveToggles NoFeatures() {
    LiveToggles t;
    t.fog = t.restir_gi = t.gi_pipeline = t.gi_ser = false;
    t.gi_sparse = 1;
    return t;
}

constexpr LiveToggles BuildSet(const LiveToggles& t) {
    LiveToggles b = t;
    b.gi_pipeline = t.gi_pipeline && !t.gi_ser;
    return b;
}

constexpr bool HasFeatures(const LiveToggles& t) {
    return t.gi || t.direct || t.reflect || t.fog || t.ao || t.restir_gi || t.restir_di ||
           t.gi_pipeline || t.gi_ser;
}

constexpr LiveToggles MergeFeatures(const LiveToggles& a, const LiveToggles& b) {
    LiveToggles t;
    t.gi = a.gi || b.gi;
    t.direct = a.direct || b.direct;
    t.reflect = a.reflect || b.reflect;
    t.fog = a.fog || b.fog;
    t.ao = a.ao || b.ao;
    t.restir_gi = a.restir_gi || b.restir_gi;
    t.gi_sparse = 1;
    t.restir_di = a.restir_di || b.restir_di;
    t.gi_pipeline = a.gi_pipeline || b.gi_pipeline;
    t.gi_ser = a.gi_ser || b.gi_ser;
    t.ptlas = false;
    t.rr = false;
    return t;
}

constexpr bool NeedsSurfaces(const LiveToggles& t) {
    return t.gi || t.direct;
}

inline std::vector<const char*> BlockedKeys(const LiveToggles& prev_wanted,
                                            const LiveToggles& wanted, const LiveToggles& avail) {
    std::vector<const char*> out;
    const auto add = [&](const char* key, bool before, bool now, bool ok) {
        if (now && !before && !ok) {
            out.push_back(key);
        }
    };
    add("BB_RT_GI", prev_wanted.gi, wanted.gi, avail.gi);
    add("BB_RT_DIRECT", prev_wanted.direct, wanted.direct, avail.direct);
    add("BB_RT_REFLECT", prev_wanted.reflect, wanted.reflect, avail.reflect);
    add("BB_RT_FX_FOG", prev_wanted.fog, wanted.fog, avail.fog);
    add("BB_RT_AO", prev_wanted.ao, wanted.ao, avail.ao);
    add("BB_RT_RESTIR_GI", prev_wanted.restir_gi, wanted.restir_gi, avail.restir_gi);
    add("BB_RT_RESTIR", prev_wanted.restir_di, wanted.restir_di, avail.restir_di);
    add("BB_RT_GI_PIPELINE", prev_wanted.gi_pipeline, wanted.gi_pipeline, avail.gi_pipeline);
    add("BB_RT_GI_SER", prev_wanted.gi_ser, wanted.gi_ser, avail.gi_ser);
    add("BB_RT_PTLAS", prev_wanted.ptlas, wanted.ptlas, avail.ptlas);
    add("BB_RT_DLSS_RR", prev_wanted.rr, wanted.rr, avail.rr);
    return out;
}

inline std::string LiveStateLine(const LiveToggles& t, bool paused) {
    const auto kv = [](const char* key, uint32_t value) {
        return std::string(" ") + key + "=" + std::to_string(value);
    };
    return kv("BB_RT_PAUSE", paused) + kv("BB_RT_GI", t.gi) + kv("BB_RT_DIRECT", t.direct) +
           kv("BB_RT_REFLECT", t.reflect) + kv("BB_RT_FX_FOG", t.fog) + kv("BB_RT_AO", t.ao) +
           kv("BB_RT_RESTIR_GI", t.restir_gi) + kv("BB_RT_GI_SPARSE", t.gi_sparse) +
           kv("BB_RT_RESTIR", t.restir_di) + kv("BB_RT_GI_PIPELINE", t.gi_pipeline) +
           kv("BB_RT_GI_SER", t.gi_ser) + kv("BB_RT_PTLAS", t.ptlas) + kv("BB_RT_DLSS_RR", t.rr);
}

}

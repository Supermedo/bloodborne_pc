#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace Vulkan::Rt {

enum class Crumb : uint32_t {
    None,
    Gather,
    OmmBake,
    OmmBuild,
    BlasBuild,
    CompactCopy,
    Probe,
    TlasBuild,
    RestirTemporal,
    RestirSpatial,
    Direct,
    Reflect,
    Gi,
    GiApply,
    RestirGiInit,
    RestirGiPass,
    RestirGiResolve,
    Shadow,
    Debug,
    Count,
};

constexpr const char* CrumbName(uint32_t id) {
    switch (Crumb(id)) {
    case Crumb::None:
        return "none";
    case Crumb::Gather:
        return "gather (rt_gather)";
    case Crumb::OmmBake:
        return "OMM bake (rt_omm_bake)";
    case Crumb::OmmBuild:
        return "micromap build";
    case Crumb::BlasBuild:
        return "BLAS build";
    case Crumb::CompactCopy:
        return "BLAS compaction copy";
    case Crumb::Probe:
        return "probe (rt_probe)";
    case Crumb::TlasBuild:
        return "TLAS build";
    case Crumb::RestirTemporal:
        return "ReSTIR DI temporal (rt_direct)";
    case Crumb::RestirSpatial:
        return "ReSTIR DI spatial (rt_direct)";
    case Crumb::Direct:
        return "direct (rt_direct)";
    case Crumb::Reflect:
        return "reflect (rt_gi)";
    case Crumb::Gi:
        return "GI (rt_gi)";
    case Crumb::GiApply:
        return "GI apply (rt_gi_apply)";
    case Crumb::RestirGiInit:
        return "ReSTIR GI init (rt_restir_gi)";
    case Crumb::RestirGiPass:
        return "ReSTIR GI temporal/spatial (rt_restir_gi)";
    case Crumb::RestirGiResolve:
        return "ReSTIR GI resolve (rt_restir_gi)";
    case Crumb::Shadow:
        return "shadows (rt_shadow)";
    case Crumb::Debug:
        return "debug (rt_debug)";
    case Crumb::Count:
        break;
    }
    return "unknown";
}

struct CrumbWords {
    uint32_t started{};
    uint32_t started_frame{};
    uint32_t finished{};
    uint32_t finished_frame{};
    uint32_t started_serial{};
    uint32_t finished_serial{};
    uint32_t pad0{}, pad1{};
};
static_assert(sizeof(CrumbWords) == 32);

constexpr bool CrumbInFlight(const CrumbWords& w) {
    return w.started != 0 && w.started_serial != w.finished_serial;
}

inline std::string DescribeCrumbs(const CrumbWords& w) {
    char line[256];
    if (w.started == 0 && w.finished == 0) {
        return "GPU: no ray tracing pass has run yet";
    }
    if (CrumbInFlight(w)) {
        std::snprintf(line, sizeof(line),
                      "GPU: ray tracing pass in flight at the fault: %s (frame %u, dispatch %u); last "
                      "finished: %s (frame %u, dispatch %u)",
                      CrumbName(w.started), w.started_frame, w.started_serial,
                      CrumbName(w.finished), w.finished_frame, w.finished_serial);
    } else {
        std::snprintf(line, sizeof(line),
                      "GPU: no ray tracing pass in flight at the fault; last finished: %s (frame %u, "
                      "dispatch %u)",
                      CrumbName(w.finished), w.finished_frame, w.finished_serial);
    }
    return line;
}

}

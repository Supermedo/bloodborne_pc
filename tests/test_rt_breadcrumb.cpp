#include <cassert>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

#include "gpu/shadps4/video_core/renderer_vulkan/rt/rt_breadcrumb.h"

using namespace Vulkan::Rt;

static void TestNames() {
    std::set<std::string> names;
    for (uint32_t id = 0; id < uint32_t(Crumb::Count); ++id) {
        const std::string name = CrumbName(id);
        assert(name != "unknown");
        assert(names.insert(name).second);
    }
    assert(std::string(CrumbName(uint32_t(Crumb::Count))) == "unknown");
    assert(std::string(CrumbName(0xFFFFFFFFu)) == "unknown");
    assert(std::string(CrumbName(uint32_t(Crumb::Direct))).find("rt_direct") != std::string::npos);
    assert(std::string(CrumbName(uint32_t(Crumb::TlasBuild))) == "TLAS build");
}

static void TestInFlight() {
    CrumbWords w{};
    assert(!CrumbInFlight(w));
    w.started = uint32_t(Crumb::Gi);
    w.started_frame = 7;
    w.started_serial = 3;
    assert(CrumbInFlight(w));
    w.finished = uint32_t(Crumb::Gi);
    w.finished_frame = 7;
    w.finished_serial = 3;
    assert(!CrumbInFlight(w));
    w.started = uint32_t(Crumb::GiApply);
    w.started_serial = 4;
    assert(CrumbInFlight(w));
    w.finished = uint32_t(Crumb::GiApply);
    w.finished_serial = 4;
    w.finished_frame = 8;
    assert(!CrumbInFlight(w));
}

static void TestDescribe() {
    CrumbWords w{};
    assert(DescribeCrumbs(w).find("no ray tracing pass has run") != std::string::npos);
    w.started = uint32_t(Crumb::RestirSpatial);
    w.started_frame = 17900;
    w.started_serial = 90001;
    w.finished = uint32_t(Crumb::RestirTemporal);
    w.finished_frame = 17900;
    w.finished_serial = 90000;
    const std::string in_flight = DescribeCrumbs(w);
    assert(in_flight.find("in flight at the fault: ReSTIR DI spatial (rt_direct) (frame 17900, "
                          "dispatch 90001)") != std::string::npos);
    assert(in_flight.find("last finished: ReSTIR DI temporal (rt_direct) (frame 17900, dispatch "
                          "90000)") != std::string::npos);
    w.finished = w.started;
    w.finished_serial = w.started_serial;
    const std::string idle = DescribeCrumbs(w);
    assert(idle.find("no ray tracing pass in flight") != std::string::npos);
    assert(idle.find("ReSTIR DI spatial") != std::string::npos);
    assert(idle.find("in flight at the fault: ") == std::string::npos);
}

static void TestLayout() {
    static_assert(sizeof(CrumbWords) == 32);
    static_assert(offsetof(CrumbWords, started) == 0);
    static_assert(offsetof(CrumbWords, started_frame) == 4);
    static_assert(offsetof(CrumbWords, finished) == 8);
    static_assert(offsetof(CrumbWords, finished_frame) == 12);
    static_assert(offsetof(CrumbWords, started_serial) == 16);
    static_assert(offsetof(CrumbWords, finished_serial) == 20);
    uint32_t raw[8] = {uint32_t(Crumb::Probe), 5, uint32_t(Crumb::Gather), 5, 12, 11, 0, 0};
    CrumbWords w{};
    std::memcpy(&w, raw, sizeof(w));
    assert(w.started == uint32_t(Crumb::Probe) && w.finished == uint32_t(Crumb::Gather));
    assert(CrumbInFlight(w));
}

int main() {
    TestNames();
    TestInFlight();
    TestDescribe();
    TestLayout();
    std::puts("rt-breadcrumb-test: ok");
    return 0;
}

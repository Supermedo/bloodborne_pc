#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

#include "gpu/shadps4/video_core/renderer_vulkan/rt/rt_hot.h"
#include "gpu/shadps4/video_core/renderer_vulkan/rt/rt_live.h"

using namespace Vulkan::Rt;

namespace {

constexpr uint32_t Nv = 0x10DEu;

struct Knobs {
    std::map<std::string, std::string> values;
    const char* operator()(const char* name) const {
        const auto it = values.find(name);
        return it == values.end() ? nullptr : it->second.c_str();
    }
};

LiveToggles All() {
    LiveToggles t;
    t.gi = t.direct = t.reflect = t.fog = t.ao = t.restir_gi = t.restir_di = true;
    t.gi_pipeline = t.gi_ser = t.ptlas = t.rr = true;
    t.gi_sparse = 4;
    return t;
}

void TestReadDefaults() {
    const LiveToggles t = ReadToggles(Knobs{}, Nv);
    assert(!t.gi && !t.direct && !t.reflect && t.fog && !t.ao && t.restir_gi);
    assert(t.gi_sparse == GiSparseDefault && !t.restir_di && t.gi_pipeline && t.gi_ser);
    assert(!t.ptlas && !t.rr);
    const LiveToggles amd = ReadToggles(Knobs{}, GiAmdVendor);
    assert(!amd.gi_pipeline);
}

void TestReadValues() {
    Knobs k;
    k.values = {{"BB_RT_GI", "1"},          {"BB_RT_DIRECT", "1"},  {"BB_RT_FX_FOG", "0"},
                {"BB_RT_AO", "1"},          {"BB_RT_RESTIR_GI", "0"}, {"BB_RT_GI_SPARSE", "2"},
                {"BB_RT_RESTIR", "1"},      {"BB_RT_GI_PIPELINE", "0"}, {"BB_RT_GI_SER", "0"},
                {"BB_RT_PTLAS", "1"},       {"BB_RT_DLSS_RR", "1"}};
    const LiveToggles t = ReadToggles(k, Nv);
    assert(t.gi && t.direct && t.reflect && !t.fog && t.ao && !t.restir_gi);
    assert(t.gi_sparse == 2 && t.restir_di && !t.gi_pipeline && !t.gi_ser && t.ptlas && t.rr);
    k.values["BB_RT_REFLECT"] = "0";
    k.values["BB_RT_GI_SPARSE"] = "3";
    assert(!ReadToggles(k, Nv).reflect);
    assert(ReadToggles(k, Nv).gi_sparse == 2);
    k.values["BB_RT_GI_SPARSE"] = "9";
    assert(ReadToggles(k, Nv).gi_sparse == 4);
    k.values["BB_RT_GI_SPARSE"] = "x";
    assert(ReadToggles(k, Nv).gi_sparse == GiSparseDefault);
    k.values["BB_RT_GI_SPARSE"] = "1";
    assert(ReadToggles(k, Nv).gi_sparse == 1);
    k.values["BB_RT_DIRECT"] = "0";
    k.values.erase("BB_RT_REFLECT");
    assert(!ReadToggles(k, Nv).reflect);
    k.values["BB_RT_GI_PIPELINE"] = "1";
    assert(ReadToggles(k, GiAmdVendor).gi_pipeline);
}

void TestResolve() {
    const LiveToggles avail = All();
    assert(ResolveToggles(All(), avail) == All());
    LiveToggles w = All();
    w.gi = false;
    LiveToggles r = ResolveToggles(w, avail);
    assert(!r.gi && !r.reflect && !r.restir_gi && !r.gi_pipeline && !r.gi_ser && r.gi_sparse == 1);
    assert(r.direct && r.ao && r.restir_di && r.fog && r.ptlas && r.rr);
    w = All();
    w.direct = false;
    r = ResolveToggles(w, avail);
    assert(!r.direct && !r.ao && !r.restir_di && r.gi && r.reflect);
    w = All();
    w.restir_gi = false;
    r = ResolveToggles(w, avail);
    assert(r.gi && !r.restir_gi && !r.gi_pipeline && !r.gi_ser && r.gi_sparse == 1);
    w = All();
    w.gi_pipeline = false;
    r = ResolveToggles(w, avail);
    assert(r.restir_gi && !r.gi_pipeline && !r.gi_ser && r.gi_sparse == 4);
    LiveToggles a = All();
    a.gi_ser = false;
    a.ptlas = false;
    r = ResolveToggles(All(), a);
    assert(r.gi_pipeline && !r.gi_ser && !r.ptlas);
    a = All();
    a.gi = false;
    r = ResolveToggles(All(), a);
    assert(!r.gi && !r.reflect && !r.restir_gi);
    w = All();
    w.gi_sparse = 3;
    assert(ResolveToggles(w, avail).gi_sparse == 2);
}

void TestDiff() {
    const LiveToggles a = All();
    assert(DiffToggles(a, a).empty());
    LiveToggles b = a;
    b.gi_sparse = 2;
    b.ptlas = false;
    const auto changes = DiffToggles(a, b);
    assert(changes.size() == 2);
    assert(std::strcmp(changes[0].key, "BB_RT_GI_SPARSE") == 0 && changes[0].from == 4 &&
           changes[0].to == 2);
    assert(std::strcmp(changes[1].key, "BB_RT_PTLAS") == 0 && changes[1].from == 1 &&
           changes[1].to == 0);
    LiveToggles c = a;
    c.gi = c.direct = c.reflect = c.fog = c.ao = c.restir_gi = c.restir_di = false;
    c.gi_pipeline = c.gi_ser = c.rr = false;
    const auto all = DiffToggles(a, c);
    assert(all.size() == 10);
    const char* keys[] = {"BB_RT_GI",     "BB_RT_DIRECT",    "BB_RT_REFLECT",  "BB_RT_FX_FOG",
                          "BB_RT_AO",     "BB_RT_RESTIR_GI", "BB_RT_RESTIR",   "BB_RT_GI_PIPELINE",
                          "BB_RT_GI_SER", "BB_RT_DLSS_RR"};
    for (size_t i = 0; i < all.size(); ++i) {
        assert(std::strcmp(all[i].key, keys[i]) == 0 && all[i].from == 1 && all[i].to == 0);
    }
}

void TestResets() {
    const LiveToggles a = All();
    LiveResets r = ResetsFor(a, a);
    assert(!r.histories && !r.ptlas_restart);
    LiveToggles b = a;
    b.rr = false;
    r = ResetsFor(a, b);
    assert(!r.histories && !r.ptlas_restart);
    b = a;
    b.fog = false;
    r = ResetsFor(a, b);
    assert(r.histories && !r.ptlas_restart);
    b = a;
    b.ptlas = false;
    r = ResetsFor(a, b);
    assert(r.histories && !r.ptlas_restart);
    r = ResetsFor(b, a);
    assert(r.histories && r.ptlas_restart);
    b = a;
    b.gi_sparse = 1;
    assert(ResetsFor(a, b).histories);
}

void TestGpuLineFrames() {
    assert(GpuLineFrames(nullptr) == 600);
    assert(GpuLineFrames("60") == 60);
    assert(GpuLineFrames("0") == 1);
    assert(GpuLineFrames("abc") == 600);
    assert(GpuLineFrames("999999999") == 100000);
}

void TestNewFeatures() {
    LiveToggles none = ResolveToggles(LiveToggles{}, All());
    none.fog = false;
    assert(!HasFeatures(NewFeatures(none, none)));
    const LiveToggles defaults = ResolveToggles(ReadToggles(Knobs{}, Nv), All());
    assert(!defaults.gi && !defaults.direct && !defaults.reflect && !defaults.restir_gi);
    assert(!defaults.gi_pipeline && !defaults.gi_ser && !defaults.ao && !defaults.restir_di);
    assert(!NeedsSurfaces(defaults));
    LiveToggles made = defaults;
    LiveToggles need = All();
    LiveToggles fresh = NewFeatures(made, need);
    assert(fresh.gi && fresh.direct && fresh.reflect && !fresh.fog && fresh.ao);
    assert(fresh.restir_gi && fresh.restir_di && fresh.gi_pipeline && fresh.gi_ser);
    assert(!fresh.ptlas && !fresh.rr && HasFeatures(fresh) && NeedsSurfaces(fresh));
    made = MergeFeatures(made, need);
    assert(!HasFeatures(NewFeatures(made, need)));
    assert(!HasFeatures(NewFeatures(made, defaults)));
    LiveToggles ser_only = made;
    ser_only.gi_ser = false;
    need = All();
    fresh = NewFeatures(ser_only, need);
    assert(fresh.gi_ser && !fresh.gi_pipeline && !fresh.gi && HasFeatures(fresh));
    assert(!NeedsSurfaces(fresh));
    LiveToggles ser = All();
    assert(!BuildSet(ser).gi_pipeline && BuildSet(ser).gi_ser);
    ser.gi_ser = false;
    assert(BuildSet(ser).gi_pipeline && !BuildSet(ser).gi_ser);
    LiveToggles made_ser = MergeFeatures(NoFeatures(), BuildSet(All()));
    fresh = NewFeatures(made_ser, BuildSet(ser));
    assert(fresh.gi_pipeline && !fresh.gi_ser && !fresh.gi && HasFeatures(fresh));
}

void TestBlocked() {
    LiveToggles avail = All();
    avail.ptlas = avail.gi_pipeline = avail.gi_ser = false;
    LiveToggles off;
    off.fog = off.restir_gi = off.gi_pipeline = off.gi_ser = false;
    LiveToggles w = off;
    w.ptlas = w.gi_pipeline = true;
    auto keys = BlockedKeys(off, w, avail);
    assert(keys.size() == 2);
    assert(std::strcmp(keys[0], "BB_RT_GI_PIPELINE") == 0 && std::strcmp(keys[1], "BB_RT_PTLAS") == 0);
    assert(BlockedKeys(w, w, avail).empty());
    w = off;
    w.gi = true;
    assert(BlockedKeys(off, w, avail).empty());
}

void TestStateLine() {
    LiveToggles t;
    t.gi = true;
    t.gi_sparse = 2;
    const std::string line = LiveStateLine(t, true);
    assert(line ==
           " BB_RT_PAUSE=1 BB_RT_GI=1 BB_RT_DIRECT=0 BB_RT_REFLECT=0 BB_RT_FX_FOG=1 BB_RT_AO=0 "
           "BB_RT_RESTIR_GI=1 BB_RT_GI_SPARSE=2 BB_RT_RESTIR=0 BB_RT_GI_PIPELINE=1 BB_RT_GI_SER=1 "
           "BB_RT_PTLAS=0 BB_RT_DLSS_RR=0");
}

void TestPollRetriesAfterFailedRead() {
#ifdef _WIN32
    const std::string path = "rt-live-test-poll.cfg";
    {
        FILE* f = std::fopen(path.c_str(), "w");
        assert(f);
        std::fputs("BB_RT_GI=1\n", f);
        std::fclose(f);
    }
    LiveConfig cfg;
    cfg.Open(path.c_str());
    assert(cfg.Poll() && std::strcmp(cfg.Get("BB_RT_GI"), "1") == 0);
    {
        FILE* f = std::fopen(path.c_str(), "w");
        assert(f);
        std::fputs("BB_RT_GI=0\nBB_RT_DIRECT=1\n", f);
        std::fclose(f);
    }
    HANDLE lock = CreateFileA(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(lock != INVALID_HANDLE_VALUE);
    assert(!cfg.Poll());
    CloseHandle(lock);
    assert(cfg.Poll());
    assert(std::strcmp(cfg.Get("BB_RT_GI"), "0") == 0 && std::strcmp(cfg.Get("BB_RT_DIRECT"), "1") == 0);
    std::remove(path.c_str());
#endif
}

}

int main() {
    TestNewFeatures();
    TestBlocked();
    TestStateLine();
    TestPollRetriesAfterFailedRead();
    TestReadDefaults();
    TestReadValues();
    TestResolve();
    TestDiff();
    TestResets();
    TestGpuLineFrames();
    std::puts("rt-live-test: ok");
    return 0;
}

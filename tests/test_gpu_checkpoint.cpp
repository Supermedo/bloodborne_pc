#include <cassert>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "gpu/shadps4/video_core/renderer_vulkan/gpu_checkpoint.h"

using namespace Vulkan::Checkpoint;

struct FakeCmdBuf {
    static inline std::vector<const void*> markers;
    void setCheckpointNV(const void* marker) {
        markers.push_back(marker);
    }
};

static void TestNames() {
    std::set<std::string> names;
    for (uint32_t op = 0; op < uint32_t(Op::Count); ++op) {
        const std::string name = OpName(op);
        assert(name != "unknown");
        assert(names.insert(name).second);
    }
    assert(std::string(OpName(uint32_t(Op::Count))) == "unknown");
    assert(std::string(OpName(0xFFFFFFFFu)) == "unknown");
}

static void TestDisabledSetsNothing() {
    FakeCmdBuf cb;
    enabled.store(false);
    Set(cb, Op::GameDraw, 0x1234);
    assert(cb.markers.empty());
}

static void TestSetRecordsMark() {
    FakeCmdBuf cb;
    enabled.store(true);
    frame.store(77);
    Set(cb, Op::GameDispatch, 0xABCDEF0123ull);
    assert(cb.markers.size() == 1);
    const auto* m = static_cast<const Mark*>(cb.markers[0]);
    assert(m->op == uint32_t(Op::GameDispatch));
    assert(m->hash == 0xABCDEF0123ull);
    assert(m->frame == 77);
    assert(ring.At(m->serial) == m);
    const std::string text = DescribeMark(*m);
    assert(text.find("game dispatch") != std::string::npos);
    assert(text.find("0xabcdef0123") != std::string::npos);
    assert(text.find("frame 77") != std::string::npos);
    enabled.store(false);
}

static void TestRingWrap() {
    Ring r;
    const Mark* first = r.Push(Op::GameDraw, 1, 1);
    const uint32_t first_serial = first->serial;
    for (uint32_t i = 0; i < RingSize + 5; ++i) {
        r.Push(Op::GameDraw, i, 1);
    }
    assert(r.At(first_serial) == nullptr);
    const Mark* last = r.Push(Op::Dlss, 9, 2);
    assert(r.At(last->serial) == last);
    assert(r.At(0) == nullptr);
}

static void TestWindow() {
    Ring r;
    std::vector<uint32_t> serials;
    for (uint32_t i = 0; i < 6; ++i) {
        serials.push_back(r.Push(Op(uint32_t(Op::GameDraw) + (i % 3)), 0x100 + i, 5)->serial);
    }
    const std::string window = DescribeWindow(r, serials[1], serials[4], 10);
    assert(window.find("done") != std::string::npos);
    assert(window.find("0x101") != std::string::npos);
    assert(window.find("0x104") != std::string::npos);
    assert(window.find("0x105") == std::string::npos);
    assert(window.find("0x100") == std::string::npos);
    size_t pending = 0;
    for (size_t pos = 0; (pos = window.find("pending", pos)) != std::string::npos; ++pos) {
        ++pending;
    }
    assert(pending == 3);
    const std::string capped = DescribeWindow(r, serials[0], serials[5], 2);
    assert(capped.find("0x104") != std::string::npos);
    assert(capped.find("0x105") != std::string::npos);
    assert(capped.find("0x103") == std::string::npos);
    assert(DescribeWindow(r, serials[3], serials[1], 10).empty());
    assert(DescribeWindow(r, 0, 0, 10).empty());
}

static void TestDescribeCheckpoint() {
    Ring r;
    assert(DescribeCheckpoint("queue 0 TopOfPipe", r, nullptr).find("no marker") !=
           std::string::npos);
    const Mark* m = r.Push(Op::ProxyResolve, 0x2a, 3);
    const std::string text = DescribeCheckpoint("queue 0 BottomOfPipe", r, m);
    assert(text.find("queue 0 BottomOfPipe") != std::string::npos);
    assert(text.find("scene proxy resolve") != std::string::npos);
    assert(text.find("op #1") != std::string::npos);
}

static void TestForeignMarker() {
    Ring r;
    const Mark* own = r.Push(Op::DlssRr, 0, 7660);
    assert(r.Owns(own));
    Mark stray{0x16fb79a8u, 0, 0x16fb79a8ull, 2632101764u, 0};
    assert(!r.Owns(&stray));
    assert(!r.Owns(reinterpret_cast<const char*>(own) + 4));
    const std::string text = DescribeCheckpoint("queue 0 TopOfPipe", r, &stray);
    assert(text.find("foreign marker") != std::string::npos);
    assert(text.find("unknown") == std::string::npos);
    assert(text.find("op #2632101764") == std::string::npos);
    const std::string tail = DescribeForeign(r, 8);
    assert(tail.find("DLSS/NGX") != std::string::npos);
    assert(tail.find("DLSS ray reconstruction evaluate start") != std::string::npos);
    assert(tail.find("frame 7660") != std::string::npos);
    Ring empty;
    assert(DescribeForeign(empty, 8).find("none") != std::string::npos);
}

int main() {
    TestNames();
    TestDisabledSetsNothing();
    TestSetRecordsMark();
    TestRingWrap();
    TestWindow();
    TestDescribeCheckpoint();
    TestForeignMarker();
    std::printf("gpu-checkpoint-test: ok\n");
    return 0;
}

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

#include "gpu/shim/bbport_stdout.h"
#include "gpu/shadps4/video_core/renderer_vulkan/rt/rt_hitch.h"

static std::vector<std::string> writes;

static void Capture(const char* data, std::size_t size) {
    writes.emplace_back(data, size);
}

static void TestPrintIsOneWrite() {
    writes.clear();
    BbOut::Print("Frame stats: %.1f FPS, worst frame %.1f ms (vblank %u Hz); %u shader/pipeline "
                 "compiles, %.1f ms; %llu recorder syncs\n",
                 119.96, 452.3, 480u, 3u, 12.5, 7ull);
    assert(writes.size() == 1);
    assert(writes[0] == "Frame stats: 120.0 FPS, worst frame 452.3 ms (vblank 480 Hz); 3 "
                        "shader/pipeline compiles, 12.5 ms; 7 recorder syncs\n");
}

static void TestLineIsOneWrite() {
    writes.clear();
    {
        BbOut::Line line;
        line.Add("GPU lag at frame start (frames not finished: count):");
        for (unsigned i = 0; i < 3; ++i) {
            line.Add(" %u:%llu", i, 10ull * (i + 1));
        }
        line.Add("\n");
        assert(writes.empty());
    }
    assert(writes.size() == 1);
    assert(writes[0] == "GPU lag at frame start (frames not finished: count): 0:10 1:20 2:30\n");
}

static void TestEmptyLineWritesNothing() {
    writes.clear();
    { BbOut::Line line; }
    assert(writes.empty());
}

static void TestLongLineTruncatesInOneWrite() {
    writes.clear();
    const std::string big(BbOut::Line::Capacity * 2, 'x');
    {
        BbOut::Line line;
        line.Add("%s", big.c_str());
        line.Add("tail\n");
    }
    assert(writes.size() == 1);
    assert(writes[0].size() == BbOut::Line::Capacity - 1);
}

static void TestHitchIgnoresGpuOnlyFrames() {
    using Vulkan::Rt::HitchSample;
    using Vulkan::Rt::IsHitch;
    assert(!IsHitch(HitchSample{.cpu_ms = 1.0, .wait_ms = 0.1, .gpu_ms = 40.0}, 8.0));
    assert(!IsHitch(HitchSample{.cpu_ms = 7.9, .wait_ms = 2.0, .gpu_ms = 400.0}, 8.0));
    assert(IsHitch(HitchSample{.cpu_ms = 8.1, .wait_ms = 0.0, .gpu_ms = 1.0}, 8.0));
    assert(IsHitch(HitchSample{.cpu_ms = 1.0, .wait_ms = 2.5, .gpu_ms = 1.0}, 8.0));
}

static void TestTimestampMs() {
    const double a = BbOut::Ms();
    const double b = BbOut::Ms();
    assert(a >= 0.0 && b >= a);
}

int main() {
    BbOut::CurrentSink() = Capture;
    TestPrintIsOneWrite();
    TestLineIsOneWrite();
    TestEmptyLineWritesNothing();
    TestLongLineTruncatesInOneWrite();
    TestHitchIgnoresGpuOnlyFrames();
    TestTimestampMs();
    std::printf("stdout-lines-test: all passed\n");
    return 0;
}

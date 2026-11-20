#include <cassert>
#include <cmath>

#include "gpu/shadps4/video_core/renderer_vulkan/rt/rt_far.h"

using namespace Vulkan::Rt;

static bool Near(float a, float b) {
    return std::fabs(a - b) < 1e-5f;
}

int main() {
    {
        const auto f = FarFadeParams(0.0f, 10.0f);
        assert(!FarFadeOn(f));
        assert(FarWeight(f, 0.0f) == 1.0f);
        assert(FarWeight(f, 1.0e5f) == 1.0f);
    }
    {
        const auto f = FarFadeParams(-5.0f, 10.0f);
        assert(!FarFadeOn(f));
        assert(FarWeight(f, 1.0e5f) == 1.0f);
    }
    {
        const auto f = FarFadeParams(40.0f, 10.0f);
        assert(FarFadeOn(f));
        assert(FarWeight(f, 0.0f) == 1.0f);
        assert(FarWeight(f, 40.0f) == 1.0f);
        assert(Near(FarWeight(f, 45.0f), 0.5f));
        assert(Near(FarWeight(f, 47.5f), 0.25f));
        assert(FarWeight(f, 50.0f) == 0.0f);
        assert(FarWeight(f, 500.0f) == 0.0f);
        assert(FarSkipped(f, 50.0f));
        assert(!FarSkipped(f, 49.9f));
        assert(!FarSkipped(f, 10.0f));
    }
    {
        const auto f = FarFadeParams(30.0f, 0.0f);
        assert(FarFadeOn(f));
        assert(FarWeight(f, 29.99f) == 1.0f);
        assert(FarWeight(f, 30.01f) == 0.0f);
        assert(FarSkipped(f, 30.01f));
    }
    {
        const auto f = FarFadeParams(30.0f, -3.0f);
        assert(FarWeight(f, 30.01f) == 0.0f);
    }
    {
        const auto f = FarFadeParams(30.0f, 10.0f);
        float last = 1.0f;
        for (float d = 0.0f; d < 60.0f; d += 0.25f) {
            const float w = FarWeight(f, d);
            assert(w <= last && w >= 0.0f && w <= 1.0f);
            last = w;
        }
    }
    return 0;
}

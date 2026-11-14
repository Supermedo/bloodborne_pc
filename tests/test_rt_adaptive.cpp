#include <cassert>
#include <cstdio>

#include "gpu/shadps4/video_core/renderer_vulkan/rt/rt_adaptive.h"

using namespace Vulkan::Rt;

static uint32_t Run(AdaptiveBounces& s, double ms, double budget, uint32_t wanted, int frames) {
    uint32_t used = wanted;
    for (int i = 0; i < frames; ++i) {
        used = AdaptiveStep(s, ms, budget, wanted);
    }
    return used;
}

int main() {
    {
        AdaptiveBounces s;
        assert(Run(s, 20.0, 0.0, 2, 500) == 2);
    }
    {
        AdaptiveBounces s;
        assert(Run(s, 11.0, 12.0, 2, 500) == 2);
    }
    {
        AdaptiveBounces s;
        assert(Run(s, 13.0, 12.0, 2, 500) == 1);
        assert(Run(s, 13.0, 12.0, 2, 500) == 1);
    }
    {
        AdaptiveBounces s;
        assert(Run(s, 13.0, 12.0, 2, 500) == 1);
        assert(Run(s, 11.0, 12.0, 2, 500) == 1);
        assert(Run(s, 9.0, 12.0, 2, 500) == 2);
    }
    {
        AdaptiveBounces s;
        assert(Run(s, 13.0, 12.0, 2, 500) == 1);
        assert(AdaptiveStep(s, 9.0, 12.0, 2) == 1);
    }
    {
        AdaptiveBounces s;
        assert(Run(s, 30.0, 12.0, 3, 2000) == 1);
    }
    {
        AdaptiveBounces s;
        assert(Run(s, 30.0, 12.0, 1, 500) == 1);
        AdaptiveBounces z;
        assert(Run(z, 30.0, 12.0, 0, 500) == 0);
    }
    {
        AdaptiveBounces s;
        assert(Run(s, 13.0, 12.0, 2, 500) == 1);
        assert(Run(s, 13.0, 0.0, 2, 1) == 2);
        assert(s.cap == AdaptiveNoCap);
    }
    {
        AdaptiveBounces s;
        assert(Run(s, 13.0, 12.0, 2, 500) == 1);
        assert(Run(s, 13.0, 12.0, 1, 1) == 1);
        assert(Run(s, 9.0, 12.0, 2, 500) == 2);
    }
    std::printf("rt-adaptive: ok\n");
    return 0;
}

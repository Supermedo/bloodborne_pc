// SPDX-License-Identifier: GPL-2.0-or-later
#include "sdk24_vulkan_features.h"
#include <cassert>
#include <cstdio>
int main() {
    unsigned cases = 0;
    for (unsigned mask = 0; mask < 128; ++mask) {
        const bool requested=mask&1, ce=mask&2, c=mask&4, fe=mask&8,
            f=mask&16, fc=mask&32, full=mask&64;
        const auto result=FsrSdk24::ChooseVulkanFeatures(requested,ce,c,fe,f,fc,full);
        assert(result.cooperative_matrix==(requested&&ce&&c));
        assert(result.float8==(requested&&ce&&c&&fe&&f&&fc));
        assert(result.full_subgroups==(requested&&full));
        ++cases;
    }
    const auto rdna23=FsrSdk24::ChooseVulkanFeatures(true,false,false,false,false,false,true);
    assert(!rdna23.cooperative_matrix&&!rdna23.float8&&rdna23.full_subgroups);
    const auto rdna4=FsrSdk24::ChooseVulkanFeatures(true,true,true,true,true,true,true);
    assert(rdna4.cooperative_matrix&&rdna4.float8&&rdna4.full_subgroups);
    for (unsigned mask=0; mask<64; ++mask) {
        const bool requested=mask&1;
        const FsrSdk24::VulkanCoreFeatures support{bool(mask&2),bool(mask&4),bool(mask&8),bool(mask&16),bool(mask&32)};
        const auto enabled=FsrSdk24::ChooseVulkanCoreFeatures(requested,support);
        assert(enabled.variable_pointers_storage_buffer==(requested&&support.variable_pointers_storage_buffer));
        assert(enabled.variable_pointers==(requested&&support.variable_pointers_storage_buffer&&support.variable_pointers));
        assert(enabled.memory_model==(requested&&support.memory_model));
        assert(enabled.memory_model_device_scope==(requested&&support.memory_model&&support.memory_model_device_scope));
        assert(enabled.partially_bound==(requested&&support.partially_bound));
        const auto required=FsrSdk24::ChooseVulkanCoreFeatures(true,support);
        // Files repaired after device creation require a restart only when enabled bits differ.
        if (requested) assert(enabled==required);
        else assert((enabled==required)==(!support.variable_pointers_storage_buffer&&!support.memory_model&&!support.partially_bound));
        ++cases;
    }
    std::printf("PASS: %u optional feature combinations, dependencies and late-file repair readiness\n",cases);
}

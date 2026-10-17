// SPDX-License-Identifier: GPL-2.0-or-later
// No Vulkan device or game dump: reject a guarded, truncated foreign ABI table.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <windows.h>
#include "video_core/renderer_vulkan/vk_dlss.h"

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

int main() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _putenv_s("BB_DLSS", "1");
    auto* dlss = Vulkan::Dlss::Get();
    assert(dlss && !dlss->Available());
    assert(dlss->Problem().find("another version") != std::string::npos);
    assert(!dlss->FgAvailable() && dlss->FgMaxGenerated() == 0);
    std::vector<const char*> extensions;
    dlss->AppendInstanceExtensions(extensions);
    dlss->AppendDeviceExtensions({}, {}, extensions);
    dlss->Initialize({}, {}, {});
    for (int repeat = 0; repeat < 2; ++repeat) {
        dlss->ReleaseFgFeature();
        dlss->ReleaseFeature();
        dlss->Shutdown();
    }
    assert(extensions.empty());
    std::puts("Unknown DLSS bridge rejected; truncated table untouched during cleanup");
}

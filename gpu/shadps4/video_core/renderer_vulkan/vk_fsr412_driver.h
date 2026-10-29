// SPDX-License-Identifier: GPL-2.0-or-later
// Local native AMD-driver prototype; no fsr4vk model/shader replacement.
#pragma once
#ifdef _WIN32
#include "video_core/renderer_vulkan/vk_fsr4.h"
#include <memory>
#include <string>
namespace Vulkan {
class Fsr412Driver {
public:
    Fsr412Driver(const Instance&,Scheduler&);
    ~Fsr412Driver();
    bool Record(Fsr4Upscaler::Frame&);
    const std::string& Error() const;
    /// Queries the original provider on this device without allocating an FSR context.
    static bool Available(const Instance&, int* status = nullptr);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
#endif

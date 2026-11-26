// SPDX-License-Identifier: GPL-2.0-or-later
// AMD FSR SDK 2.4 Vulkan wrapper; no fsr4vk model/shader replacement.
#pragma once
#ifdef _WIN32
#include "video_core/renderer_vulkan/vk_fsr4.h"
#include <memory>
#include <filesystem>
#include <string>
namespace Vulkan {
class Fsr412Driver {
public:
    Fsr412Driver(const Instance&,Scheduler&);
    ~Fsr412Driver();
    bool Record(Fsr4Upscaler::Frame&);
    const std::string& Error() const;
    /// Queries the public SDK provider on this device without allocating an FSR context.
    static bool Available(const Instance&, int* status = nullptr);
    static std::filesystem::path RuntimeDirectory();
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
#endif

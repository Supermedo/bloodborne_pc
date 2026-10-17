// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#ifdef _WIN32
#include "video_core/renderer_vulkan/vk_fsr4.h"
#include <memory>
#include <string>
namespace Vulkan {
class Instance;
class Scheduler;
class Fsr411Native {
public:
    Fsr411Native(const Instance& instance, Scheduler& scheduler);
    ~Fsr411Native();
    bool Record(Fsr4Upscaler::Frame& frame, bool asynchronous);
    /// Return the shared result to DX12 after the last consumer; a no-op for buffer output.
    void ReleaseOutput(vk::CommandBuffer command);
    const std::string& Error() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
#endif

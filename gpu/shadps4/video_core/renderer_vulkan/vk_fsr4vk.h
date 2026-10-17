// SPDX-License-Identifier: GPL-2.0-or-later
//
// bbport: FSR 4.1.1 on Windows through fsr4vk (github.com/dvj5411/fsr4vk, GPLv3), an FFX provider
// DLL that runs AMD's INT8 4.1.1 model with standard Vulkan features (no
// VK_VALVE_shader_mixed_float_dot_product). The port contains none of its code and does not link
// it: amd_fidelityfx_upscaler_vk.dll is loaded at run time from BB_FSR4VK_DIR or the fsr4vk folder
// next to the executable. run.py selects the persistent <data>/fsr4vk directory;
// scripts/fetch_fsr4vk.py downloads the pinned release. Without the DLL,
// with BB_FSR4VK=0 or on a GPU lacking the device features nothing changes. Experimental.

#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "video_core/renderer_vulkan/vk_fsr4.h"

namespace Vulkan {

class Instance;
class Scheduler;

class Fsr4Vk {
  public:
    /// The provider's file name in Directory(); shared with scripts/fetch_fsr4vk.py.
    static constexpr const char *LibraryName = "amd_fidelityfx_upscaler_vk.dll";
    /// The folder of the provider files: BB_FSR4VK_DIR, else `fsr4vk` next to the executable.
    static std::filesystem::path Directory();
    /// The provider DLL is installed there and BB_FSR4VK is not 0. Device creation asks this before
    /// requesting the extra device features, so nothing is requested without the files.
    static bool FilesPresent();
    /// The provider's path as UTF-8, for the log (path::string() is the ANSI page and can throw).
    static std::string LibraryPathUtf8();

    Fsr4Vk(const Instance &instance, Scheduler &scheduler);
    ~Fsr4Vk();

    /// Records FSR 4.1.1 into `frame.cmdbuf` (the same inputs as Fsr4Upscaler::Record); the
    /// inputs and the output stay in General. Retiring the provider's frame slots can replace
    /// frame.cmdbuf. False when it cannot run (Problem() says why).
    bool Record(Fsr4Upscaler::Frame &frame);

    [[nodiscard]] const char *Problem() const noexcept;
    /// The provider is missing, unusable or refused the frame: it will not run in this session.
    [[nodiscard]] bool Fatal() const noexcept;
    /// What runs (provider name, model, output size), for the log; empty before the first frame.
    [[nodiscard]] std::string Describe() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Vulkan

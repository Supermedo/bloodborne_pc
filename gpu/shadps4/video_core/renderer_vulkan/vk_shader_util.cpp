// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <filesystem>

#include "common/assert.h"
#include "common/io_file.h"
#include "common/path_util.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

vk::ShaderModule CompileSPV(std::span<const u32> code, vk::Device device) {
    if (code.size() < 5) {
        LOG_CRITICAL(Render_Vulkan, "Failed to compile SPIR-V shader: empty or truncated module (words={})",
                     code.size());
        ASSERT_MSG(false, "Failed to compile SPIR-V shader: empty module");
        return {};
    }
    const vk::ShaderModuleCreateInfo shader_info = {
        .codeSize = code.size() * sizeof(u32),
        .pCode = code.data(),
    };

    auto [module_result, module] = device.createShaderModule(shader_info);
    if (module_result != vk::Result::eSuccess) {
        const u32 magic = code.empty() ? 0 : code[0];
        const u32 version = code.size() > 1 ? code[1] : 0;
        LOG_CRITICAL(Render_Vulkan,
                     "Failed to compile SPIR-V shader: {} (words={}, magic={:#x}, version={:#x})",
                     vk::to_string(module_result), code.size(), magic, version);
        const auto dump_dir = Common::FS::GetUserPath(Common::FS::PathType::ShaderDir) / "dumps";
        std::error_code ec;
        std::filesystem::create_directories(dump_dir, ec);
        if (!ec) {
            const Common::FS::IOFile file{dump_dir / "failed_shader.spv",
                                          Common::FS::FileAccessMode::Create};
            file.WriteSpan(code);
        }
    }
    ASSERT_MSG(module_result == vk::Result::eSuccess, "Failed to compile SPIR-V shader: {}",
               vk::to_string(module_result));
    return module;
}

} // namespace Vulkan

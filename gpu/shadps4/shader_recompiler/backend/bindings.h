// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Shader::Backend {

struct Bindings {
    u32 unified{};
    u32 buffer{};
    u32 user_data{};
    /// Descriptor set index: FragmentSet or PreRasterSet for graphics, 0 for compute.
    u32 set{};

    auto operator<=>(const Bindings&) const = default;
};

/// Push descriptors allow one set per layout. The fragment set holds most of the textures
/// and is pushed; the pre-rasterization set is written from the descriptor heap.
inline constexpr u32 FragmentSet = 0;
inline constexpr u32 PreRasterSet = 1;
inline constexpr u32 NumGraphicsSets = 2;

} // namespace Shader::Backend

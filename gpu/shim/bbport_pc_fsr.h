// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "bbport_settings.h"

namespace BbPcFsr {
#ifdef _WIN32
inline constexpr bool Windows = true;
#else
inline constexpr bool Windows = false;
#endif

// The Windows table describes DLL/bridge routes; Linux uses the native 4.1.1 check.
template <bool windows = Windows>
int Backend(const BbSettings::Values& s, int version) {
    if constexpr (!windows) return -1;
    const int saved = s.fsr411_backend.load();
    if (s.fsr41_version == version && saved >= 0 && saved <= 1 &&
        s.FsrMethodSupported(version, saved)) return saved;
    const int first = version == BbSettings::UpscalerFsr412
                          ? BbSettings::Fsr411Vulkan : BbSettings::Fsr411Dx12;
    if (s.FsrMethodSupported(version, first)) return first;
    if (s.FsrMethodSupported(version, 1 - first)) return 1 - first;
    return -1;
}

template <bool windows = Windows>
bool Supported(const BbSettings::Values& s, int version) {
    if constexpr (windows) return Backend<true>(s, version) >= 0;
    return version == BbSettings::UpscalerFsr411 && s.fsr411_supported.load();
}

template <bool windows = Windows>
bool Select(BbSettings::Values& s, int version) {
    if constexpr (windows) {
        const int backend = Backend<true>(s, version);
        if (backend < 0) return false;
        s.fsr411_backend = backend;
        s.fsr41_version = version;
        return true;
    }
    return Supported<false>(s, version);
}
} // namespace BbPcFsr

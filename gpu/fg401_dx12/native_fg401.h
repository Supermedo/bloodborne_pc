// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

// The native AMD backend has no Vulkan dependency. Images cross the API
// boundary through shared D3D12 textures (color, motion, output) and a depth
// buffer, using GPU fences and explicit ownership transfers.
struct NativeFg401Config {
    uint32_t render_width, render_height, output_width, output_height;
    uint32_t color_format, depth_format; // DXGI_FORMAT
    uint8_t adapter_luid[8];
    uint32_t shared_texture_mask{};
    bool async_compute{};
    uint32_t provider_version{(4u << 22) | 1u};
};
struct NativeFg401Plane {
    uint64_t offset;
    uint32_t width, height, row_pitch, bytes_per_pixel;
};
struct NativeFg401Frame {
    float jitter_x, jitter_y, frame_ms, near_plane, far_plane, vertical_fov;
    bool reset;
    float position[3]{};
    float up[3]{0, 1, 0}, right[3]{1, 0, 0}, forward[3]{0, 0, 1};
};
class NativeFg401 {
  public:
    static bool Available();
    explicit NativeFg401(const NativeFg401Config &config);
    ~NativeFg401();
    NativeFg401(const NativeFg401 &) = delete;
    NativeFg401 &operator=(const NativeFg401 &) = delete;
    bool Ready() const;
    const char *Error() const;
    void *SharedTextureHandle(unsigned index) const;
    void *SharedHandle() const;
    uint64_t SharedSize() const;
    void *InputFenceHandle() const;
    void *OutputFenceHandle() const;
    bool DispatchAsync(const NativeFg401Frame &frame, uint64_t input_value, uint64_t &output_value);
    const NativeFg401Plane &Plane(unsigned index) const;
    bool Dispatch(const NativeFg401Frame &frame);

  private:
    struct Impl;
    Impl *impl;
};

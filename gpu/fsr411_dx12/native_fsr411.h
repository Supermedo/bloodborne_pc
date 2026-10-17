// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

// The native AMD backend has no Vulkan dependency. Images cross the API boundary
// through shared D3D12 textures (color, motion, output) and a depth buffer,
// using GPU fences and explicit ownership transfers.
struct NativeFsr411Config {
    uint32_t render_width, render_height, output_width, output_height;
    uint32_t color_format, depth_format; // DXGI_FORMAT
    bool hdr;
    uint8_t adapter_luid[8];
    uint32_t shared_texture_mask{};
};
struct NativeFsr411Plane {
    uint64_t offset;
    uint32_t width, height, row_pitch, bytes_per_pixel;
};
struct NativeFsr411Frame {
    float jitter_x, jitter_y, frame_ms, near_plane, far_plane, vertical_fov, sharpness;
    bool sharpen, reset;
};
class NativeFsr411 {
public:
    static bool Available();
    explicit NativeFsr411(const NativeFsr411Config& config);
    ~NativeFsr411();
    NativeFsr411(const NativeFsr411&) = delete;
    NativeFsr411& operator=(const NativeFsr411&) = delete;
    bool Ready() const;
    const char* Error() const;
    void *SharedTextureHandle(unsigned index) const;
    void* SharedHandle() const;
    uint64_t SharedSize() const;
    void* InputFenceHandle() const;
    void* OutputFenceHandle() const;
    bool DispatchAsync(const NativeFsr411Frame& frame, uint64_t input_value,
                       uint64_t& output_value);
    const NativeFsr411Plane& Plane(unsigned index) const;
    bool Dispatch(const NativeFsr411Frame& frame);
private:
    struct Impl;
    Impl* impl;
};

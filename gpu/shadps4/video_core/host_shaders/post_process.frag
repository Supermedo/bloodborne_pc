// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 450

layout (location = 0) in vec2 uv;
layout (location = 0) out vec4 color;

layout (binding = 0) uniform sampler2D texSampler;

layout (push_constant) uniform settings {
    float gamma;
    bool hdr;
    bool srgb_input;
    bool sdr_on_hdr;
} pp;

const float cutoff = 0.0031308, a = 1.055, b = 0.055, d = 12.92;
vec3 gamma(vec3 rgb) {
    return mix(
        a * pow(rgb, vec3(1.0 / (2.4 + 1.0 - pp.gamma))) - b,
        d * rgb / pp.gamma,
        lessThan(rgb, vec3(cutoff))
    );
}

// Exact inverse of gamma() at unit gamma, for buffers that are sRGB encoded but must be sampled
// through a UNORM view because Vulkan has no sRGB variant of the 10-bit format.
vec3 degamma(vec3 rgb) {
    return mix(
        pow(max(rgb + b, 0.0) / a, vec3(2.4)),
        rgb / d,
        lessThan(rgb, vec3(d * cutoff))
    );
}

// bbport: Windows decodes an SDR swapchain on an HDR display with the piecewise sRGB curve, while
// an SDR display (and the TV the game was graded on) applies gamma 2.2. Near black the sRGB curve
// is several times brighter, which lifts the blacks. Re-encode so that decode gives gamma 2.2.
vec3 gamma22_for_srgb_decode(vec3 encoded) {
    vec3 rgb = pow(max(encoded, 0.0), vec3(2.2));
    return mix(a * pow(rgb, vec3(1.0 / 2.4)) - b, d * rgb, lessThan(rgb, vec3(cutoff)));
}

void main() {
    vec4 color_linear = texture(texSampler, uv);
    if (pp.hdr) {
        color = color_linear;
    } else {
        if (pp.srgb_input) color_linear.rgb = degamma(color_linear.rgb);
        vec3 encoded = gamma(color_linear.rgb);
        if (pp.sdr_on_hdr) encoded = gamma22_for_srgb_decode(encoded);
        color = vec4(encoded, color_linear.a);
    }
}

// SPDX-License-Identifier: MIT
#pragma once
// Row-major matrices multiplying row vectors; no raster jitter in the matrices.
typedef struct BbFrameCamera {
    float view_to_clip[16], clip_to_view[16], clip_to_previous[16], previous_to_clip[16];
    float position[3], up[3], right[3], forward[3];
    float jitter[2], near_plane, far_plane, vertical_fov;
} BbFrameCamera;

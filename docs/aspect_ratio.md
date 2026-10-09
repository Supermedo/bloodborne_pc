# Windows aspect-ratio support

Bloodborne 1.09 can render wider, taller, ultrawide and portrait outputs while keeping
the logical 1920x1080 HUD centered. For example, set `output_res=3840x1600` in
bbport.ini (or type that size in the launcher after the custom-resolution change).
The camera keeps its vertical field of view. Floating enemy labels can reach the
expanded visible area. Title/loading screens retain their centered 16:9 stage.

Non-16:9 outputs use startup resolution patches even when live resolution is enabled.
Changing aspect ratio requires a restart; the renderer keeps the startup output until
the camera and Scaleform patches are prepared again. TAA renders these outputs natively.

The Windows camera hook checks the unmodified ELF PT_LOAD fingerprint and all replaced
instructions/constants before writing any hook. Unsupported eboots and conflicting
camera/UI patches are logged and skipped. This hook is currently Windows only.

## Attribution and license

Camera/UI corrections and the SysV trampoline are adapted from
[droogie/bbhost](https://github.com/droogie/bbhost), revision `7c790536`:
`src/engine/live_resolution.cpp`, `src/engine/graphics_patch.cpp` and `src/core/thunk.cpp`.
Those adaptations are GPL-3.0-or-later, as marked in their source headers; the license
text is in `licenses/bbhost-aspect-GPL-3.0.txt`. The port's existing GPL-2.0-or-later
code permits combining it under GPLv3. No game executables or data are included.

#!/usr/bin/env bash
# Linux build inside the Steam Runtime container (packaging/linux/compose.yaml):
#   docker compose -f packaging/linux/compose.yaml run --rm build
# Output: out/bb-probe, out/gpu/libbbgpu.so and out/gpu/libSDL3.so.0 (SDL3 from deps.sh, found
# through bb-probe's $ORIGIN/gpu run path), runnable on the host and on SteamOS without Proton.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
# libstdc++ and libgcc go into libbbgpu.so: gcc-14's are newer than many systems' own.
# -Bsymbolic: FFmpeg's static x86 assembly refers to its own constants (ff_pw_9, ...) with
# PC-relative relocations, which a shared library may only contain when they bind locally.
export LDFLAGS="${LDFLAGS:-} -static-libstdc++ -static-libgcc -Wl,-Bsymbolic"
bash build.sh "$@"
cp -L /opt/deps/lib/libSDL3.so.0 out/gpu/libSDL3.so.0

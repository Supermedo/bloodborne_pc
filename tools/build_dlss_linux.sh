#!/usr/bin/env bash
# Builds libbbport_dlss.so (gpu/dlss_bridge, the only code that uses the NVIDIA DLSS SDK) and puts
# it with NVIDIA's libnvidia-ngx-dlss.so.<version> next to out/bb-probe, where the port finds both.
# DLSS_SDK_ROOT: a checkout of https://github.com/NVIDIA/DLSS (default: its tag DLSS_SDK_TAG,
# v310.9.1, the version tested, cloned into out/sdk/DLSS).
# Needs the NVIDIA proprietary driver at run time (libnvidia-ngx.so.1 comes with it).
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
sdk=${DLSS_SDK_ROOT:-$PWD/out/sdk/DLSS}
if [[ ! -f $sdk/include/nvsdk_ngx_vk.h ]]; then
    if [[ -n ${DLSS_SDK_ROOT:-} ]]; then echo "No DLSS SDK in $sdk" >&2; exit 1; fi
    mkdir -p out/sdk
    git clone --depth 1 --branch "${DLSS_SDK_TAG:-v310.9.1}" https://github.com/NVIDIA/DLSS.git "$sdk"
fi
cmake -S gpu/dlss_bridge -B out/dlss-bridge -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DDLSS_SDK_ROOT="$sdk" >/dev/null
ninja -C out/dlss-bridge >/dev/null
rm -f out/libnvidia-ngx-dlss.so.*
cp out/dlss-bridge/libbbport_dlss.so out/
cp "$sdk"/lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.* out/
cp "$sdk/LICENSE.txt" out/NVIDIA-DLSS-LICENSE.txt
# Self-test without the game: out/bb-dlss-selftest (creates a DLSS feature, evaluates a frame).
"${CC:-cc}" -std=c11 -O2 -Wall -Wextra -I. tools/dlss_selftest.c -lvulkan -ldl -o out/bb-dlss-selftest
echo "DLSS bridge ready: out/libbbport_dlss.so, $(cd out && echo libnvidia-ngx-dlss.so.*)"

#!/usr/bin/env bash
# Optional native Vulkan neural-rendering adapter; the user supplies the model.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
source ./msys2-env.sh
sdk=${DLSS_SDK_ROOT:?Set DLSS_SDK_ROOT to a checkout of github.com/NVIDIA/DLSS}
cmake=${CMAKE_EXE:-cmake}
mkdir -p out/nr-vkinclude
cp -r /c/msys64/clang64/include/vulkan /c/msys64/clang64/include/vk_video out/nr-vkinclude/
"$cmake" -S gpu/nr_bridge -B out/nr-bridge -G "Visual Studio 17 2022" -A x64 \
    -DDLSS_SDK_ROOT="$(cygpath -m "$sdk")" \
    -DVULKAN_INCLUDE="$(cygpath -m "$PWD/out/nr-vkinclude")"
"$cmake" --build out/nr-bridge --config Release
cp out/nr-bridge/Release/bbport_nr_nvngx.dll out/
echo 'Neural-rendering adapter ready. Supply nvngx_dlssnr.dll separately.'

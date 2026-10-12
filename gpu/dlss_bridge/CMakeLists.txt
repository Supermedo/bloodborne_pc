# SPDX-FileCopyrightText: Copyright 2026 IFreemz (shadps4_dlss bridge), bbport contributors
# SPDX-License-Identifier: MIT
#
# bbport_dlss.dll / libbbport_dlss.so links the NVIDIA DLSS SDK and is built separately from the
# port. Windows (the SDK's libraries are MSVC only):
#   cmake -S gpu/dlss_bridge -B out/dlss-bridge -G "Visual Studio 17 2022" -A x64 \
#         -DDLSS_SDK_ROOT=<github.com/NVIDIA/DLSS checkout> -DVULKAN_INCLUDE=<Vulkan-Headers include>
#   cmake --build out/dlss-bridge --config Release
# Linux (GCC or Clang, libnvsdk_ngx.a): tools/build_dlss_linux.sh.

cmake_minimum_required(VERSION 3.20)
project(bbport_dlss LANGUAGES CXX)

set(DLSS_SDK_ROOT "" CACHE PATH "NVIDIA DLSS SDK root (contains include/ and lib/)")
set(VULKAN_INCLUDE "" CACHE PATH "Vulkan-Headers include directory")

if (NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR NOT (WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux"))
    message(FATAL_ERROR "The DLSS bridge is Windows x64 or Linux x86-64 only")
endif()
if (NOT EXISTS "${DLSS_SDK_ROOT}/include/nvsdk_ngx_vk.h")
    message(FATAL_ERROR "Set DLSS_SDK_ROOT to the NVIDIA DLSS SDK (github.com/NVIDIA/DLSS)")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# nvsdk_ngx_s.lib is the static-CRT (/MT) variant: no Visual C++ runtime install needed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

add_library(bbport_dlss SHARED bbport_dlss_bridge.cpp bbport_dlss_bridge.h)
target_include_directories(bbport_dlss PRIVATE "${DLSS_SDK_ROOT}/include")
if (VULKAN_INCLUDE)
    target_include_directories(bbport_dlss PRIVATE "${VULKAN_INCLUDE}")
endif()
if (WIN32)
    target_link_libraries(bbport_dlss PRIVATE "${DLSS_SDK_ROOT}/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib")
    target_compile_definitions(bbport_dlss PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    # Only BbDlssGetApi is exported; the C++ runtime is linked statically so that the bridge
    # does not depend on the host's libstdc++ version (the AppImage, Steam's runtime).
    set_target_properties(bbport_dlss PROPERTIES CXX_VISIBILITY_PRESET hidden
                                                 VISIBILITY_INLINES_HIDDEN ON)
    target_link_libraries(bbport_dlss PRIVATE "${DLSS_SDK_ROOT}/lib/Linux_x86_64/libnvsdk_ngx.a"
                                              ${CMAKE_DL_LIBS})
    target_link_options(bbport_dlss PRIVATE -static-libstdc++ -static-libgcc -Wl,--no-undefined
                                            -Wl,--exclude-libs,ALL)
endif()

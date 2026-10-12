# bbport GPU library: shadPS4 video core + shader recompiler + Gnm/VideoOut
# drivers, adapted to the native_probe runtime through shims in shim/.
cmake_minimum_required(VERSION 3.24)
project(bbgpu LANGUAGES C CXX)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION.txt")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION.txt" BBPORT_VERSION LIMIT_COUNT 1)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/bbport_version.c"
    "const char *bbgpu_version(void) { return \"v${BBPORT_VERSION}\"; }\n")
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
# Frame pointers keep the loader's thread dumps (watchdog, SIGUSR2 sampling) walkable.
add_compile_options(-fno-omit-frame-pointer)
if (MINGW)
    # Headers newer libc++ no longer includes transitively (see the file).
    add_compile_options(
        "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/mingw_libcxx_compat.h>")
endif()

# bbport: link-time optimization (BB_LTO, default on) and profile-guided optimization
# (BB_PGO: off, generate = instrumented build writing into BB_PGO_DIR, use = build with that
# profile). Covers bbgpu and the static libraries linked into it. No -march: the same build
# runs on the Steam Deck.
option(BB_LTO "Link-time optimization of bbgpu" ON)
set(BB_PGO "off" CACHE STRING "Profile-guided optimization: off, generate, use")
set(BB_PGO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../pgo" CACHE PATH "Profile directory")
if (BB_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT bb_ipo_ok OUTPUT bb_ipo_error)
    if (bb_ipo_ok)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    else()
        message(WARNING "LTO unavailable: ${bb_ipo_error}")
    endif()
endif()
if (BB_PGO STREQUAL "generate")
    # Atomic counters: the GPU, recording, preparation and copy threads run the same code.
    add_compile_options(-fprofile-generate=${BB_PGO_DIR} -fprofile-update=atomic)
    add_link_options(-fprofile-generate=${BB_PGO_DIR})
    add_compile_definitions(BB_PGO_GENERATE)
elseif (BB_PGO STREQUAL "use")
    # Partial training keeps unprofiled code optimized normally; counters from concurrent
    # threads can be slightly inconsistent (correction). Functions changed since the profile
    # (or built differently for it, bbgpu_init) are compiled without it.
    add_compile_options(-fprofile-use=${BB_PGO_DIR} -fprofile-partial-training
                        -fprofile-correction -Wno-missing-profile -Wno-coverage-mismatch)
endif()

find_package(Vulkan REQUIRED)
if (WIN32)
    add_subdirectory(remix)
endif()
find_package(fmt REQUIRED)
find_package(Boost REQUIRED)
# bbport (Windows): MSYS2 has no magic_enum/miniz packages; the submodules are used instead.
find_package(magic_enum QUIET)
if (NOT magic_enum_FOUND)
    add_library(magic_enum INTERFACE)
    add_library(magic_enum::magic_enum ALIAS magic_enum)
    target_include_directories(magic_enum INTERFACE third_party/magic_enum/include
                                                    third_party/magic_enum/include/magic_enum)
endif()
find_package(tsl-robin-map REQUIRED)
find_package(VulkanMemoryAllocator CONFIG QUIET)
if (NOT VulkanMemoryAllocator_FOUND)
    # MSYS2 ships the header (include/vma/vk_mem_alloc.h) without a CMake package.
    find_path(VMA_INCLUDE_DIR vk_mem_alloc.h PATH_SUFFIXES vma REQUIRED)
    add_library(VulkanMemoryAllocator INTERFACE)
    add_library(GPUOpen::VulkanMemoryAllocator ALIAS VulkanMemoryAllocator)
    target_include_directories(VulkanMemoryAllocator INTERFACE ${VMA_INCLUDE_DIR})
endif()
find_package(PkgConfig REQUIRED)
pkg_check_modules(XXHASH REQUIRED IMPORTED_TARGET libxxhash)
pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
if (NOT WIN32)
    pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)
endif()
find_package(miniz QUIET)
if (NOT miniz_FOUND)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    add_subdirectory(third_party/miniz EXCLUDE_FROM_ALL)
    unset(CMAKE_POLICY_VERSION_MINIMUM)
    add_library(miniz::miniz ALIAS miniz)
endif()
find_package(Zydis REQUIRED)
find_program(GLSLANG_COMPILER NAMES glslang glslangValidator REQUIRED)

set(SIRIT_USE_SYSTEM_SPIRV_HEADERS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/sirit EXCLUDE_FROM_ALL)

# bbport: FSR 3.1 temporal upscaling / frame generation, native Vulkan (MIT, FireBurn/FSR-Vulkan).
set(FFX_VK_PORTABLE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FFX_VK_PORTABLE_BUILD_PROBE OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/fsr-vulkan EXCLUDE_FROM_ALL)

# Host shaders -> headers (same scheme as shadPS4's host_shaders/CMakeLists.txt).
set(SHADER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/shadps4/video_core/host_shaders)
set(SHADER_INCLUDE ${CMAKE_CURRENT_BINARY_DIR}/include)
set(SHADER_DIR ${SHADER_INCLUDE}/video_core/host_shaders)
set(SHADER_HEADERS "")
function(add_host_shader FILENAME SHADER_NAME)
    set(OUT ${SHADER_DIR}/${SHADER_NAME}.h)
    string(TOUPPER ${SHADER_NAME} SYMBOL)
    set(DEFINES ${ARGN})
    list(TRANSFORM DEFINES PREPEND "-D")
    add_custom_command(OUTPUT ${OUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_DIR}
        COMMAND ${GLSLANG_COMPILER} --target-env vulkan1.3 -o ${OUT} --vn ${SYMBOL} ${DEFINES} ${SHADER_SRC}/${FILENAME}
        COMMAND ${CMAKE_COMMAND} "-DHEADER_FILE=${OUT}" -P ${SHADER_SRC}/AddShaderHeaderInclude.cmake
        DEPENDS ${SHADER_SRC}/${FILENAME} VERBATIM)
    set(SHADER_HEADERS ${SHADER_HEADERS} ${OUT} PARENT_SCOPE)
endfunction()
add_host_shader(color_to_ms_depth.frag color_to_ms_depth_frag)
add_host_shader(ms_image_blit.frag ms_image_blit_frag)
add_host_shader(ms_image_blit.frag ms_image_blit_msaa_frag SRC_MSAA=1)
add_host_shader(fault_buffer_process.comp fault_buffer_process_comp)
add_host_shader(fs_tri.vert fs_tri_vert)
add_host_shader(fsr.comp fsr_easu_comp SAMPLE_EASU=1)
add_host_shader(fsr.comp fsr_rcas_comp SAMPLE_RCAS=1)
add_host_shader(post_process.frag post_process_frag)
add_host_shader(camera_motion.comp camera_motion_comp)
add_host_shader(frame_generation.comp frame_generation_comp)
add_host_shader(frame_generation_depth.comp frame_generation_depth_comp)
add_host_shader(upscale_merge.comp upscale_merge_comp)
add_host_shader(upscale_reactive.comp upscale_reactive_comp)
add_host_shader(taa.comp taa_comp)
add_host_shader(taa_sharpen.comp taa_sharpen_comp)
add_host_shader(taa_sharpen.comp taa_sharpen_ldr_comp LDR_OUTPUT=1)
add_host_shader(fsr4_color.comp fsr4_decode_comp)
add_host_shader(fsr4_color.comp fsr4_encode_comp ENCODE=1)
add_host_shader(fsr4_reactive.comp fsr4_reactive_comp)
add_host_shader(buffer_multi_copy.comp buffer_multi_copy_comp)
add_host_shader(depth_resample.frag depth_resample_frag)
add_host_shader(depth_resample.frag depth_stencil_resample_frag HAS_STENCIL=1)
add_host_shader(depth_resample.frag depth_stencil_bits_frag STENCIL_BITS=1)
add_host_shader(camera_motion.comp camera_motion_debug_comp DEBUG_OVERLAY=1)
foreach(BPP IN ITEMS 8 16 32 64 96 128)
    add_host_shader(tiling.comp tiling_micro_${BPP}_comp BITS_PER_PIXEL=${BPP} IS_MACRO_TILED=0)
    add_host_shader(tiling.comp tiling_macro_${BPP}_comp BITS_PER_PIXEL=${BPP} IS_MACRO_TILED=1)
endforeach()
add_custom_target(host_shaders DEPENDS ${SHADER_HEADERS})

file(GLOB_RECURSE VIDEO_CORE CONFIGURE_DEPENDS shadps4/video_core/*.cpp)
file(GLOB_RECURSE RECOMPILER CONFIGURE_DEPENDS shadps4/shader_recompiler/*.cpp)
list(FILTER VIDEO_CORE EXCLUDE REGEX "renderdoc\\.cpp$")
set(COMMON
    shadps4/common/decoder.cpp shadps4/common/error.cpp
    shadps4/common/io_file.cpp shadps4/common/native_clock.cpp shadps4/common/number_utils.cpp
    shadps4/common/path_util.cpp shadps4/common/rdtsc.cpp shadps4/common/spin_lock.cpp shadps4/common/recursive_lock.cpp
    shadps4/common/string_util.cpp shadps4/common/thread.cpp shadps4/common/signal_context.cpp
    shim/common/logging/log.cpp)
file(GLOB AVPLAYER CONFIGURE_DEPENDS shadps4/core/libraries/avplayer/*.cpp)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswscale libswresample)
file(GLOB SHIM_SOURCES CONFIGURE_DEPENDS shim/*.cpp shim/*/*.cpp)
if (WIN32)
    list(FILTER SHIM_SOURCES EXCLUDE REGEX "bbport_(guest_memory|guest_hooks|gnm_hooks|free_check|heap_sites)\\.cpp$")
else()
    list(FILTER SHIM_SOURCES EXCLUDE REGEX "bbport_diagnostics_win\\.cpp$")
endif()
# Windows: a DLL cannot leave the runtime_* symbols for bb-probe.exe to resolve, so the
# library is static there and linked into the executable.
if (WIN32)
    set(BBGPU_KIND STATIC)
else()
    set(BBGPU_KIND SHARED)
endif()
add_library(bbgpu ${BBGPU_KIND} ${VIDEO_CORE} ${RECOMPILER} ${COMMON} ${SHIM_SOURCES}
    shadps4/core/libraries/gnmdriver/gnmdriver.cpp
    shadps4/core/libraries/videoout/driver.cpp shadps4/core/libraries/videoout/video_out.cpp
    shadps4/core/libraries/kernel/equeue.cpp ${AVPLAYER}
    shadps4/core/libraries/videodec/video_utils.cpp)
add_dependencies(bbgpu host_shaders)
# bbport co-op: PS4 network and PSN libraries with the shadNet client, from shadp2p
# (github.com/Wozzardman/shadp2p, GPL-2.0), for private Bloodborne co-op servers.
find_package(Protobuf CONFIG REQUIRED)
set(SHADNET_PROTO_OUT "${CMAKE_CURRENT_BINARY_DIR}/shadnet_proto_gen")
file(MAKE_DIRECTORY "${SHADNET_PROTO_OUT}")
add_custom_command(
    OUTPUT "${SHADNET_PROTO_OUT}/shadnet.pb.cc" "${SHADNET_PROTO_OUT}/shadnet.pb.h"
    COMMAND protobuf::protoc "--proto_path=${CMAKE_CURRENT_SOURCE_DIR}/shadps4/shadnet"
            "--cpp_out=${SHADNET_PROTO_OUT}" "${CMAKE_CURRENT_SOURCE_DIR}/shadps4/shadnet/shadnet.proto"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shadps4/shadnet/shadnet.proto")
file(GLOB_RECURSE BBNET_SOURCES CONFIGURE_DEPENDS shadps4/core/libraries/network/*.cpp
    shadps4/core/libraries/np/*.cpp shadps4/shadnet/*.cpp)
# Trophies and the system dialogs stay in the C runtime; their shadPS4 versions need the
# emulator's UI layers.
list(FILTER BBNET_SOURCES EXCLUDE REGEX
    "np_trophy|trophy_ui|np_sns_facebook_dialog|np_profile_dialog/|np_commerce/")
find_package(nlohmann_json CONFIG REQUIRED)
add_library(bbnet STATIC ${BBNET_SOURCES} "${SHADNET_PROTO_OUT}/shadnet.pb.cc" bbnet/bbnet_glue.cpp)
target_include_directories(bbnet PRIVATE shim shadps4 shadps4/shadnet "${SHADNET_PROTO_OUT}"
    third_party/imgui third_party/httplib)
target_link_libraries(bbnet PRIVATE nlohmann_json::nlohmann_json)
# HTTPS (the community game server only speaks TLS) and gzip replies in cpp-httplib.
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
target_compile_definitions(bbnet PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT CPPHTTPLIB_ZLIB_SUPPORT)
target_link_libraries(bbnet PRIVATE OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB)
if (WIN32)
    target_sources(bbnet PRIVATE third_party/wepoll/wepoll.c)
    target_include_directories(bbnet PRIVATE third_party/wepoll)
    find_path(MINIUPNPC_INCLUDE miniupnpc.h PATH_SUFFIXES miniupnpc REQUIRED)
    find_library(MINIUPNPC_LIBRARY NAMES libminiupnpc.a miniupnpc REQUIRED)
    target_include_directories(bbnet PRIVATE "${MINIUPNPC_INCLUDE}")
    target_compile_definitions(bbnet PRIVATE MINIUPNP_STATICLIB)
    target_link_libraries(bbnet PRIVATE "${MINIUPNPC_LIBRARY}")
endif()
target_compile_definitions(bbnet PRIVATE BOOST_ASIO_STANDALONE NDEBUG)
target_compile_options(bbnet PRIVATE -Wno-deprecated-declarations -Wno-missing-field-initializers)
target_link_libraries(bbnet PUBLIC Vulkan::Headers fmt::fmt Boost::headers magic_enum::magic_enum
    protobuf::libprotobuf)
if (WIN32)
    target_compile_definitions(bbnet PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(bbnet PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-include ${CMAKE_CURRENT_SOURCE_DIR}/shim/bb_win_compat.h>)
    target_link_libraries(bbnet PUBLIC ws2_32 iphlpapi bcrypt crypt32)
endif()
target_link_libraries(bbgpu PUBLIC bbnet)
# In-game settings menu: Dear ImGui (third_party/imgui, MIT) with its Vulkan backend, and an
# embedded Cyrillic font (third_party/fonts).
target_sources(bbgpu PRIVATE third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_vulkan.cpp)
target_include_directories(bbgpu PRIVATE third_party/imgui third_party/imgui/backends)
target_compile_definitions(bbgpu PRIVATE IMGUI_IMPL_VULKAN_NO_PROTOTYPES
    BB_FONT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/DejaVuSans.ttf")
set_source_files_properties(shim/bbport_overlay.cpp PROPERTIES
    OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/DejaVuSans.ttf)
target_include_directories(bbgpu PRIVATE shim shadps4 ${SHADER_INCLUDE} third_party/gcn/include third_party/half/include)
target_compile_definitions(bbgpu PRIVATE BOOST_ASIO_STANDALONE NDEBUG)
target_compile_options(bbgpu PRIVATE -Wno-deprecated-declarations -Wno-missing-field-initializers)
# Loaded with bb-probe at startup (DT_NEEDED, never dlopen): thread_local access without a
# TLS descriptor call (_dl_tlsdesc_return was ~2% of the draw recording thread).
if (NOT WIN32)
    target_compile_options(bbgpu PRIVATE -ftls-model=initial-exec)
endif()
# Runtime symbols (runtime_memory_*, runtime_process_*) resolve against bb-probe.
if (NOT WIN32)
    target_link_options(bbgpu PRIVATE -Wl,--allow-shlib-undefined)
endif()
target_link_libraries(bbgpu PRIVATE ffx-vulkan::portable ffx-vulkan::fsr4-v07-vulkan
    ffx-vulkan::fsr3-vk-framegeneration-3.1.6)
target_link_libraries(bbgpu PUBLIC Vulkan::Headers fmt::fmt Boost::headers magic_enum::magic_enum
    tsl::robin_map GPUOpen::VulkanMemoryAllocator PkgConfig::XXHASH PkgConfig::SDL3 PkgConfig::FFMPEG miniz::miniz Zydis::Zydis sirit)
if (WIN32)
    # Guest code is entered with the System V ABI; NOMINMAX/WIN32_LEAN_AND_MEAN keep
    # <windows.h> out of the way of std::min/max and the Vulkan/SDL headers.
    # Vulkan-Hpp's dynamic dispatch table has a platform-dependent layout. A static
    # Windows build must use the same layout in the renderer and its callers.
    target_compile_definitions(bbgpu PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN VK_USE_PLATFORM_WIN32_KHR)
    target_compile_options(bbgpu PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/shim/bb_win_compat.h)
    target_include_directories(bbgpu PRIVATE third_party/xbyak)
    target_link_libraries(bbgpu PUBLIC Vulkan::Vulkan ws2_32 bcrypt ntdll synchronization winmm)
    target_link_libraries(bbgpu PRIVATE bb-remix)
else()
    target_link_libraries(bbgpu PUBLIC PkgConfig::X11)
endif()

# bbport (Windows): bbgpu is a static library there, so the loader and the HLE runtime are
# linked here, where CMake knows its dependencies (build.sh builds them with ninja).
if (WIN32)
    enable_language(RC)
    string(REPLACE "." ";" BBPORT_VERSION_PARTS "${BBPORT_VERSION}")
    list(LENGTH BBPORT_VERSION_PARTS BBPORT_VERSION_LENGTH)
    while(BBPORT_VERSION_LENGTH LESS 4)
        list(APPEND BBPORT_VERSION_PARTS 0)
        list(LENGTH BBPORT_VERSION_PARTS BBPORT_VERSION_LENGTH)
    endwhile()
    string(JOIN "," BBPORT_VERSION_RC ${BBPORT_VERSION_PARTS})
    string(JOIN "." BBPORT_VERSION_FULL ${BBPORT_VERSION_PARTS})
    configure_file(../packaging/windows/port-version.rc.in bbport-version.rc @ONLY)
    configure_file(../packaging/windows/bb-probe.manifest.in bb-probe.manifest @ONLY)
    file(GLOB BB_RUNTIME CONFIGURE_DEPENDS ../src/runtime*.c)
    file(GLOB BB_ATRAC9 CONFIGURE_DEPENDS ../third_party/LibAtrac9/C/src/*.c)
    add_library(atrac9 STATIC ${BB_ATRAC9})
    target_compile_options(atrac9 PRIVATE -w)
    set_target_properties(atrac9 PROPERTIES C_STANDARD 99 INTERPROCEDURAL_OPTIMIZATION OFF)
    add_executable(bb-probe ../src/probe.c ${BB_RUNTIME} ../src/compat_win.c ../src/crash_win.c ../src/vulkan_smoke.c
        ../packaging/windows/bb-probe.rc)
    set_target_properties(bb-probe PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
    target_compile_options(bb-probe PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Werror>)
    # Clang 23 added -Wunused-but-set-global (on with -Wall): the runtime keeps a few
    # set-only state flags (e.g. common_initialized), so it stays a warning there.
    include(CheckCCompilerFlag)
    check_c_compiler_flag(-Wno-error=unused-but-set-global BB_HAS_UNUSED_SET_GLOBAL)
    if (BB_HAS_UNUSED_SET_GLOBAL)
        target_compile_options(bb-probe PRIVATE
            $<$<COMPILE_LANGUAGE:C>:-Wno-error=unused-but-set-global>)
    endif()
    target_include_directories(bb-probe PRIVATE .. ../src)
    target_link_libraries(bb-probe PRIVATE bbgpu atrac9 onecore bcrypt ws2_32 -pthread)
    # The PS4 range (64 GiB - 1 TiB) must stay free: no high-entropy ASLR for heaps and stacks.
    # The guest main thread runs on the process stack: 16 MiB, committed (no stack probes).
    target_link_options(bb-probe PRIVATE -Wl,--disable-high-entropy-va "SHELL:-Xlinker --stack -Xlinker 16777216,16777216")
    add_executable(bb-gpu-capabilities ../tools/gpu_capabilities.c ../packaging/windows/bb-gpu-capabilities.rc)
    target_link_libraries(bb-gpu-capabilities PRIVATE Vulkan::Vulkan PkgConfig::SDL3)
    # "Play Bloodborne.exe": the game with the launcher's saved settings, no launcher window.
    add_executable(bb-play WIN32 ../packaging/windows/play.c ../packaging/windows/play.rc)
    add_executable(bb-crash-monitor ../src/crash_monitor_win.c ../src/crash_win.c)
    target_compile_options(bb-crash-monitor PRIVATE -municode -Wall -Wextra -Werror)
    target_link_options(bb-crash-monitor PRIVATE -municode)
    target_link_libraries(bb-crash-monitor PRIVATE onecore)
    target_compile_options(bb-play PRIVATE -municode)
    target_link_options(bb-play PRIVATE -municode)
    foreach(target bb-probe bb-gpu-capabilities bb-play bb-crash-monitor)
        target_sources(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/bbport-version.rc")
        target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    endforeach()
endif()
target_sources(bbgpu PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/bbport_version.c")

# Explicit tests without game files; renderer tests execute on a Vulkan device.
add_executable(cache-storage-test EXCLUDE_FROM_ALL ../tests/test_cache_storage.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(cache-storage-test PRIVATE shim shadps4)
target_compile_definitions(cache-storage-test PRIVATE NDEBUG)
target_link_libraries(cache-storage-test PRIVATE bbgpu)
add_executable(command-buffer-test EXCLUDE_FROM_ALL ../tests/test_command_buffers.cpp)
target_compile_options(command-buffer-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(command-buffer-test PRIVATE ..)
add_executable(motion-history-test EXCLUDE_FROM_ALL ../tests/test_motion_history.cpp)
target_compile_options(motion-history-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(motion-history-test PRIVATE ..)
add_executable(motion-shader-test EXCLUDE_FROM_ALL ../tests/test_motion_shaders.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(motion-shader-test PRIVATE shim shadps4 third_party/gcn/include)
target_link_libraries(motion-shader-test PRIVATE bbgpu)
add_executable(ui-composition-test EXCLUDE_FROM_ALL ../tests/test_ui_composition.cpp)
target_compile_options(ui-composition-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(ui-composition-test PRIVATE ..)
add_executable(settings-test EXCLUDE_FROM_ALL ../tests/test_settings.cpp shim/bbport_settings.cpp)
target_compile_options(settings-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(settings-test PRIVATE .. shim)

add_executable(overlay-test EXCLUDE_FROM_ALL ../tests/test_overlay.cpp ../tests/test_gpu_runtime_stubs.cpp
    third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp)
target_include_directories(overlay-test PRIVATE shim shadps4 third_party/gcn/include
    third_party/imgui third_party/imgui/backends)
target_compile_definitions(overlay-test PRIVATE NDEBUG IMGUI_IMPL_VULKAN_NO_PROTOTYPES IMGUI_ENABLE_TEST_ENGINE
    BB_FONT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/DejaVuSans.ttf")
target_link_libraries(overlay-test PRIVATE bbgpu)

add_executable(scene-resolution-test EXCLUDE_FROM_ALL ../tests/test_scene_resolution.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(scene-resolution-test PRIVATE shim shadps4 third_party/gcn/include)
# The test's Vulkan-Hpp inline methods have a different set of platform extensions
# from vk_platform.cpp. Keep them local and use an explicit dispatcher in the test.
target_compile_definitions(scene-resolution-test PRIVATE NDEBUG)
target_compile_options(scene-resolution-test PRIVATE -fPIC)
set_source_files_properties(../tests/test_scene_resolution.cpp PROPERTIES
    COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
target_link_libraries(scene-resolution-test PRIVATE bbgpu)
add_executable(taa-shader-test EXCLUDE_FROM_ALL ../tests/test_taa_shader.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(taa-shader-test PRIVATE shim shadps4 ${SHADER_INCLUDE} third_party/gcn/include)
target_compile_definitions(taa-shader-test PRIVATE NDEBUG)
target_compile_options(taa-shader-test PRIVATE -fPIC)
set_source_files_properties(../tests/test_taa_shader.cpp PROPERTIES
    COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
target_link_libraries(taa-shader-test PRIVATE bbgpu)
add_dependencies(taa-shader-test host_shaders)

add_executable(camera-motion-test EXCLUDE_FROM_ALL ../tests/test_camera_motion.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(camera-motion-test PRIVATE shim shadps4 ${SHADER_INCLUDE} third_party/gcn/include)
target_compile_definitions(camera-motion-test PRIVATE NDEBUG)
target_compile_options(camera-motion-test PRIVATE -fPIC)
target_link_libraries(camera-motion-test PRIVATE bbgpu)
add_dependencies(camera-motion-test host_shaders)

add_executable(command-dispatch-test EXCLUDE_FROM_ALL ../tests/test_command_dispatch.cpp
    ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(command-dispatch-test PRIVATE shim shadps4 third_party/gcn/include)
target_compile_definitions(command-dispatch-test PRIVATE NDEBUG)
target_compile_options(command-dispatch-test PRIVATE -fPIC)
target_link_libraries(command-dispatch-test PRIVATE bbgpu)

add_executable(record-data-test EXCLUDE_FROM_ALL ../tests/test_record_data.cpp
    ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(record-data-test PRIVATE shim shadps4 third_party/gcn/include)
target_compile_definitions(record-data-test PRIVATE NDEBUG)
target_link_libraries(record-data-test PRIVATE bbgpu)

# bbport: FSR 4 benchmark outside the game (tools/fsr4_bench.cpp): ninja -C out/gpu fsr4-bench.
add_executable(upscaler-support-test EXCLUDE_FROM_ALL ../tests/test_upscaler_support.cpp
    shim/bbport_settings.cpp)
target_include_directories(upscaler-support-test PRIVATE shim)
target_compile_options(upscaler-support-test PRIVATE -UNDEBUG)

enable_testing()
add_executable(frame-generation-test EXCLUDE_FROM_ALL ../tests/test_frame_generation.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(frame-generation-test PRIVATE shim shadps4 third_party/gcn/include)
target_compile_definitions(frame-generation-test PRIVATE NDEBUG)
target_compile_options(frame-generation-test PRIVATE -fPIC)
target_link_libraries(frame-generation-test PRIVATE bbgpu)
if(WIN32)
    target_sources(frame-generation-test PRIVATE ../src/compat_win.c)
    target_link_libraries(frame-generation-test PRIVATE bcrypt onecore ws2_32 -pthread)
endif()
add_test(NAME frame-generation-test COMMAND frame-generation-test)
add_test(NAME dlss-frame-generation-test COMMAND frame-generation-test dlss)
set_tests_properties(frame-generation-test dlss-frame-generation-test PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77)
if(WIN32)
    add_test(NAME frame-generation-window-test COMMAND frame-generation-test window)
    set_tests_properties(frame-generation-window-test PROPERTIES TIMEOUT 180)
endif()
add_executable(dlss-test EXCLUDE_FROM_ALL ../tests/test_dlss.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(dlss-test PRIVATE shim shadps4 third_party/gcn/include)
target_compile_definitions(dlss-test PRIVATE NDEBUG)
target_compile_options(dlss-test PRIVATE -fPIC)
target_link_libraries(dlss-test PRIVATE bbgpu)
if(WIN32)
    target_sources(dlss-test PRIVATE ../src/compat_win.c)
    target_link_libraries(dlss-test PRIVATE bcrypt onecore ws2_32 -pthread)
endif()
add_test(NAME dlss-test COMMAND dlss-test)
set_tests_properties(dlss-test PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77)
set(BB_TEST_TARGETS motion-history-test motion-shader-test ui-composition-test settings-test overlay-test
    scene-resolution-test taa-shader-test camera-motion-test upscaler-support-test command-dispatch-test record-data-test cache-storage-test)
if (WIN32)
    # Exercise the actual Windows HLE and SysV guest ABI, including virtual SDL input.
    target_link_libraries(cache-storage-test PRIVATE onecore)
    add_executable(respawn-patch-test EXCLUDE_FROM_ALL ../tests/test_respawn_patch.c)
    target_include_directories(respawn-patch-test PRIVATE ../src)
    target_compile_options(respawn-patch-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
    list(APPEND BB_TEST_TARGETS respawn-patch-test)
    add_executable(mouse-camera-test EXCLUDE_FROM_ALL ../tests/test_mouse_camera.cpp
        ../tests/test_gpu_runtime_stubs.cpp ../src/compat_win.c)
    target_include_directories(mouse-camera-test PRIVATE shim shadps4 third_party/xbyak)
    target_compile_definitions(mouse-camera-test PRIVATE NDEBUG)
    target_link_libraries(mouse-camera-test PRIVATE bbgpu bcrypt onecore ws2_32 -pthread)
    list(APPEND BB_TEST_TARGETS mouse-camera-test)
    add_executable(mouse-orbit-test EXCLUDE_FROM_ALL ../tests/test_mouse_orbit.cpp
        ../tests/test_gpu_runtime_stubs.cpp ../src/compat_win.c)
    target_include_directories(mouse-orbit-test PRIVATE shim shadps4 third_party/xbyak)
    target_compile_definitions(mouse-orbit-test PRIVATE NDEBUG)
    target_link_libraries(mouse-orbit-test PRIVATE bbgpu bcrypt onecore ws2_32 -pthread)
    list(APPEND BB_TEST_TARGETS mouse-orbit-test)
    add_executable(mouse-trace-test EXCLUDE_FROM_ALL ../tests/test_mouse_trace.cpp)
    target_include_directories(mouse-trace-test PRIVATE shim)
    target_compile_definitions(mouse-trace-test PRIVATE NDEBUG)
    target_link_libraries(mouse-trace-test PRIVATE bbgpu -pthread)
    list(APPEND BB_TEST_TARGETS mouse-trace-test)
    add_executable(raw-mouse-test EXCLUDE_FROM_ALL ../tests/test_raw_mouse.cpp
        ../tests/test_gpu_runtime_stubs.cpp ../src/compat_win.c)
    target_include_directories(raw-mouse-test PRIVATE shim shadps4)
    target_compile_definitions(raw-mouse-test PRIVATE NDEBUG)
    target_link_libraries(raw-mouse-test PRIVATE bbgpu bcrypt onecore ws2_32 user32 -pthread)
    list(APPEND BB_TEST_TARGETS raw-mouse-test)
    foreach(mode IN ITEMS patched mixed convergence)
        add_test(NAME mouse-camera-${mode}-test COMMAND mouse-camera-test ${mode})
        set_tests_properties(mouse-camera-${mode}-test PROPERTIES TIMEOUT 120)
    endforeach()
    foreach(test IN ITEMS runtime sema)
        add_executable(${test}-test EXCLUDE_FROM_ALL ../tests/test_${test}.c
            ${BB_RUNTIME} ../src/compat_win.c ../packaging/windows/bb-probe.rc)
        target_include_directories(${test}-test PRIVATE .. ../src "${CMAKE_CURRENT_BINARY_DIR}")
        target_compile_options(${test}-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
        target_link_libraries(${test}-test PRIVATE bbgpu atrac9 onecore bcrypt ws2_32 -pthread)
        target_link_options(${test}-test PRIVATE -Wl,--disable-high-entropy-va
            "SHELL:-Xlinker --stack -Xlinker 16777216,16777216")
        set_target_properties(${test}-test PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
    endforeach()
    add_executable(pad-test EXCLUDE_FROM_ALL ../tests/test_pad.c)
    target_include_directories(pad-test PRIVATE .. ../src)
    target_compile_options(pad-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
    target_link_libraries(pad-test PRIVATE PkgConfig::SDL3 -pthread)
    add_executable(input-capture-test EXCLUDE_FROM_ALL ../tests/test_input_capture.c)
    target_compile_options(input-capture-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
    target_link_libraries(input-capture-test PRIVATE PkgConfig::SDL3)
    add_executable(content-test EXCLUDE_FROM_ALL ../tests/test_content.c ../src/runtime_content.c)
    target_include_directories(content-test PRIVATE ../src)
    target_compile_options(content-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
    foreach(test IN ITEMS motion-shader-test scene-resolution-test taa-shader-test camera-motion-test command-dispatch-test record-data-test overlay-test)
        target_sources(${test} PRIVATE ../src/compat_win.c)
        target_link_libraries(${test} PRIVATE bcrypt onecore ws2_32 -pthread)
    endforeach()
    list(APPEND BB_TEST_TARGETS runtime-test sema-test pad-test content-test input-capture-test)
    add_test(NAME vulkan-smoke-test COMMAND bb-probe --vulkan-only)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME prepare-cache-smoke-test COMMAND Python3::Interpreter
        ${CMAKE_CURRENT_SOURCE_DIR}/../tests/prepare_cache_smoke.py)
    set_tests_properties(prepare-cache-smoke-test PROPERTIES TIMEOUT 90)
    add_test(NAME probe-gpu-startup-test COMMAND Python3::Interpreter
        ${CMAKE_CURRENT_SOURCE_DIR}/../tests/probe_gpu_smoke.py $<TARGET_FILE:bb-probe>)
    set_tests_properties(probe-gpu-startup-test PROPERTIES TIMEOUT 90)
    add_test(NAME probe-crash-dump-test COMMAND Python3::Interpreter
        ${CMAKE_CURRENT_SOURCE_DIR}/../tests/probe_crash_dump.py $<TARGET_FILE:bb-probe>)
    set_tests_properties(probe-crash-dump-test PROPERTIES TIMEOUT 45)
    add_executable(gpu-assert-crash-test EXCLUDE_FROM_ALL ../tests/test_gpu_assert_crash.cpp
        ../src/crash_win.c)
    target_include_directories(gpu-assert-crash-test PRIVATE shim shadps4)
    target_link_libraries(gpu-assert-crash-test PRIVATE bbgpu)
    target_compile_definitions(gpu-assert-crash-test PRIVATE NDEBUG)
    add_test(NAME gpu-assert-crash-dump-test COMMAND Python3::Interpreter
        ${CMAKE_CURRENT_SOURCE_DIR}/../tests/gpu_assert_crash_dump.py $<TARGET_FILE:gpu-assert-crash-test>)
    set_tests_properties(gpu-assert-crash-dump-test PROPERTIES TIMEOUT 120)
    add_executable(crash-stop-test EXCLUDE_FROM_ALL ../tests/test_crash_stop_win.c)
    target_compile_options(crash-stop-test PRIVATE -municode -Wall -Wextra -Werror)
    target_link_options(crash-stop-test PRIVATE -municode)
    add_test(NAME crash-monitor-dump-test COMMAND Python3::Interpreter
        ${CMAKE_CURRENT_SOURCE_DIR}/../tests/crash_monitor_dump.py
        $<TARGET_FILE:bb-crash-monitor> $<TARGET_FILE:crash-stop-test>)
    set_tests_properties(crash-monitor-dump-test PROPERTIES TIMEOUT 120)
endif()
foreach(test IN LISTS BB_TEST_TARGETS)
    add_test(NAME ${test} COMMAND ${test})
    set_tests_properties(${test} PROPERTIES TIMEOUT 120)
endforeach()
if (WIN32)
    set_tests_properties(raw-mouse-test PROPERTIES SKIP_RETURN_CODE 77)
endif()
set_tests_properties(command-dispatch-test PROPERTIES ENVIRONMENT
    "BB_VK_RECORD_THREAD=1;BB_VK_RECORD_THREADS=3;BB_VK_SEGMENT_KB=1;BB_ASYNC_SUBMIT=1")
add_test(NAME command-dispatch-sync-test COMMAND command-dispatch-test)
set_tests_properties(command-dispatch-sync-test PROPERTIES TIMEOUT 120 ENVIRONMENT
    "BB_VK_RECORD_THREAD=1;BB_VK_RECORD_THREADS=3;BB_VK_SEGMENT_KB=1;BB_ASYNC_SUBMIT=0")
set_tests_properties(record-data-test PROPERTIES ENVIRONMENT
    "BB_VK_RECORD_THREAD=1;BB_VK_RECORD_THREADS=3;BB_VK_SEGMENT_KB=1;BB_ASYNC_SUBMIT=1")
foreach(mode IN ITEMS sync direct)
    add_test(NAME record-data-${mode}-test COMMAND record-data-test)
    set_tests_properties(record-data-${mode}-test PROPERTIES TIMEOUT 120 ENVIRONMENT
        "BB_VK_RECORD_THREAD=$<IF:$<STREQUAL:${mode},direct>,0,1>;BB_VK_RECORD_THREADS=3;BB_VK_SEGMENT_KB=1;BB_ASYNC_SUBMIT=0")
endforeach()
add_custom_target(bb-tests DEPENDS ${BB_TEST_TARGETS} dlss-test frame-generation-test)
if (WIN32)
    add_executable(remix-scene-test EXCLUDE_FROM_ALL ../tests/test_remix_scene.cpp)
    target_link_libraries(remix-scene-test PRIVATE bb-remix)
    add_test(NAME remix-scene-test COMMAND remix-scene-test)
    add_dependencies(bb-tests remix-scene-test)
    add_executable(remix-game-test EXCLUDE_FROM_ALL ../tests/test_remix_game.cpp ../tests/test_gpu_runtime_stubs.cpp)
    target_include_directories(remix-game-test PRIVATE shim shadps4 third_party/gcn/include)
    target_link_libraries(remix-game-test PRIVATE bbgpu bb-remix)
    target_compile_options(remix-game-test PRIVATE -municode)
    target_link_options(remix-game-test PRIVATE -municode)
    add_test(NAME remix-capture-test COMMAND remix-game-test ${CMAKE_CURRENT_BINARY_DIR}/remix-capture-test)
    set_tests_properties(remix-capture-test PROPERTIES TIMEOUT 60)
    add_dependencies(bb-tests remix-game-test)
endif()
if (WIN32)
    add_dependencies(bb-tests bb-probe gpu-assert-crash-test bb-crash-monitor crash-stop-test)
endif()

add_executable(fsr4-bench EXCLUDE_FROM_ALL tools/fsr4_bench.cpp
    shadps4/video_core/renderer_vulkan/fsr411/fsr411.cpp)
target_include_directories(fsr4-bench PRIVATE shadps4/video_core/renderer_vulkan/fsr411)
target_link_libraries(fsr4-bench PRIVATE ffx-vulkan::fsr4-v07-vulkan ffx-vulkan::fsr4-v07-assets Vulkan::Vulkan)

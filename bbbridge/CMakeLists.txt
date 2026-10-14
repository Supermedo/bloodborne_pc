cmake_minimum_required(VERSION 3.21)
project(bbbridge VERSION 0.3.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# One self-contained exe: the C++ runtime is linked in (no VC++ redistributable).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MSVC)
    add_compile_options(/utf-8 /permissive- /EHsc)
    add_compile_definitions(_WIN32_WINNT=0x0A00 WINVER=0x0A00 UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
endif()

# The bridge itself: everything but the front ends.
add_library(bbbridge_core STATIC
    src/util.cpp
    src/proto3.cpp
    src/http.cpp
    src/bbhost_client.cpp
    src/p2p.cpp
    src/bridge.cpp
)
target_include_directories(bbbridge_core PUBLIC src third_party)
target_link_libraries(bbbridge_core PUBLIC ws2_32 winhttp bcrypt)

target_sources(bbbridge_core PRIVATE src/setup.cpp src/account_link.cpp)

# Dear ImGui (third_party/imgui, v1.91.5) with the Win32 + DirectX 11 backends.
add_library(imgui STATIC
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/imgui_impl_win32.cpp
    third_party/imgui/imgui_impl_dx11.cpp
)
target_include_directories(imgui PUBLIC third_party/imgui)
target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_DEMO_WINDOWS)


# The app: bbbridge.exe, next to this CMakeLists (inside bbport's folder).
add_executable(bbbridge WIN32 src/app.cpp src/app.rc)
target_link_libraries(bbbridge PRIVATE bbbridge_core imgui d3d11 dxgi)
add_custom_command(TARGET bbbridge POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:bbbridge> ${CMAKE_SOURCE_DIR}/bbbridge.exe)

# Headless bridge: tests, servers, troubleshooting.
add_executable(bbbridge-cli src/main_cli.cpp)
target_link_libraries(bbbridge-cli PRIVATE bbbridge_core)

# Unit tests for the byte-level parts (framing, signaling, STUN, protobuf).
add_executable(bbbridge-unit tests/unit.cpp)
target_link_libraries(bbbridge-unit PRIVATE bbbridge_core)

# Warnings at /W4 for our code (not for third_party).
if(MSVC)
    foreach(target bbbridge_core bbbridge bbbridge-cli bbbridge-unit)
        target_compile_options(${target} PRIVATE /W4)
    endforeach()
endif()

# The CLI and the unit tests land in bin\.
foreach(target bbbridge-cli bbbridge-unit)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:${target}> ${CMAKE_SOURCE_DIR}/bin/$<TARGET_FILE_NAME:${target}>)
endforeach()

cmake_minimum_required(VERSION 3.24)
project(hunters_atlas_bbport LANGUAGES CXX)

if (NOT WIN32)
    message(FATAL_ERROR "The bbport native-module ABI is currently available on Windows only")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(Vulkan REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)

set(ATLAS_SOURCE "${CMAKE_CURRENT_LIST_DIR}/..")
set(NATIVE "${ATLAS_SOURCE}/native")
set(IMGUI "${NATIVE}/gpu/third_party/imgui")

add_library(hunters_atlas SHARED
    atlas_module.cpp
    runtime_bridge.cpp
    "${ATLAS_SOURCE}/compact/bbport_map.cpp"
    "${ATLAS_SOURCE}/compact/map_vector.cpp"
    "${ATLAS_SOURCE}/compact/vector_backend.cpp"
    "${NATIVE}/gpu/shim/bbport_map_art.cpp"
    "${NATIVE}/gpu/shim/bbport_map_controller.cpp"
    "${NATIVE}/gpu/shim/bbport_map_features.cpp"
    "${NATIVE}/gpu/shim/bbport_atlas_menu.cpp"
    "${IMGUI}/imgui.cpp"
    "${IMGUI}/imgui_draw.cpp"
    "${IMGUI}/imgui_tables.cpp"
    "${IMGUI}/imgui_widgets.cpp")

target_include_directories(hunters_atlas PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/include"
    "${ATLAS_SOURCE}/compact"
    "${ATLAS_SOURCE}/atlas2"
    "${NATIVE}/gpu/shim"
    "${NATIVE}/src"
    "${IMGUI}"
    "${IMGUI}/backends")
target_compile_definitions(hunters_atlas PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
# The renderer carries its own ImGui Vulkan backend and loads entry points
# through ImGui_ImplVulkan_LoadFunctions. The map geometry/upload code in
# map_vector.cpp calls Vulkan directly, so only the backend source disables
# prototypes.
set_source_files_properties("${ATLAS_SOURCE}/compact/vector_backend.cpp"
    PROPERTIES COMPILE_DEFINITIONS IMGUI_IMPL_VULKAN_NO_PROTOTYPES)
target_compile_options(hunters_atlas PRIVATE
    -Wall -Wextra -Wno-missing-field-initializers -Wno-unused-parameter)
target_link_libraries(hunters_atlas PRIVATE
    Vulkan::Vulkan PkgConfig::SDL3 nlohmann_json::nlohmann_json
    OpenSSL::Crypto ZLIB::ZLIB)

set_target_properties(hunters_atlas PROPERTIES
    PREFIX ""
    OUTPUT_NAME "hunters_atlas")

install(TARGETS hunters_atlas RUNTIME DESTINATION . LIBRARY DESTINATION .)

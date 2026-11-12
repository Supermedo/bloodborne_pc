# AMD ML frame generation, dynamically loaded on the selected Windows adapter.
# Canonical paths let CMake recognize the compiler's own include directory.
foreach(dependency IN ITEMS PkgConfig::XXHASH PkgConfig::SDL3)
    get_target_property(directories ${dependency} INTERFACE_INCLUDE_DIRECTORIES)
    if (directories)
        set(canonical "")
        foreach(directory IN LISTS directories)
            get_filename_component(directory "${directory}" REALPATH)
            list(APPEND canonical "${directory}")
        endforeach()
        set_property(TARGET ${dependency} PROPERTY INTERFACE_INCLUDE_DIRECTORIES "${canonical}")
    endif()
endforeach()
set(FG401_DIR "${CMAKE_CURRENT_SOURCE_DIR}/fg401_dx12")
set(FG401_API "${CMAKE_CURRENT_SOURCE_DIR}/fsr411_dx12/sdk/api/include")
set(FG401_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/fsr411_dx12/sdk/framegeneration/include")
target_sources(bbgpu PRIVATE
    "${FG401_DIR}/native_fg401.cpp" "${FG401_DIR}/vk_fg401_native.cpp"
    "${FG401_DIR}/fg401_support.cpp" "${FG401_DIR}/fused_fg_ui.cpp")
target_include_directories(bbgpu PRIVATE "${FG401_DIR}" "${FG401_HEADERS}")
set_source_files_properties(fg401_dx12/native_fg401.cpp fg401_dx12/fg401_support.cpp
    PROPERTIES COMPILE_DEFINITIONS _WINDOWS)
set(FG24_INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/fsr24/sdk/api/include;${CMAKE_CURRENT_SOURCE_DIR}/fsr24/sdk/framegeneration/include")
set_source_files_properties(fg401_dx12/native_fg401.cpp fg401_dx12/fg401_support.cpp
    PROPERTIES INCLUDE_DIRECTORIES "${FG24_INCLUDES}")
target_sources(bb-probe PRIVATE "${FG401_DIR}/agility.cpp")
target_sources(bb-gpu-capabilities PRIVATE "${FG401_DIR}/agility.cpp")

add_executable(bb-fg401-capabilities "${FG401_DIR}/fg401_capabilities.cpp"
    "${FG401_DIR}/fg401_support.cpp" "${FG401_DIR}/agility.cpp")
target_compile_definitions(bb-fg401-capabilities PRIVATE _WINDOWS NOMINMAX)
target_include_directories(bb-fg401-capabilities PRIVATE "${FG401_API}" "${FG401_HEADERS}")
target_link_libraries(bb-fg401-capabilities PRIVATE Vulkan::Vulkan d3d12 dxgi)

add_executable(fg402-sdk24-api-test EXCLUDE_FROM_ALL ../tests/test_fg402_sdk24_api.cpp)
target_include_directories(fg402-sdk24-api-test PRIVATE "${FG401_DIR}" ${FG24_INCLUDES})
target_compile_definitions(fg402-sdk24-api-test PRIVATE _WINDOWS NOMINMAX)
target_compile_options(fg402-sdk24-api-test PRIVATE -UNDEBUG)

foreach(test IN ITEMS bridge stress ui)
    add_executable(fg401-${test}-test EXCLUDE_FROM_ALL
        "../tests/test_fg401_${test}.cpp" "${FG401_DIR}/agility.cpp"
        ${BB_RUNTIME} ../src/compat_win.c)
    target_compile_definitions(fg401-${test}-test PRIVATE _WINDOWS)
    target_include_directories(fg401-${test}-test PRIVATE .. ../src shim shadps4
        third_party/gcn/include "${FG401_DIR}" "${FG401_API}" "${FG401_HEADERS}" "${SHADER_INCLUDE}")
    target_link_libraries(fg401-${test}-test PRIVATE bbgpu atrac9 onecore bcrypt ws2_32 -pthread)
endforeach()
add_executable(fg401-support-test EXCLUDE_FROM_ALL ../tests/test_fg401_support.cpp)
target_include_directories(fg401-support-test PRIVATE "${FG401_DIR}")
target_compile_options(fg401-support-test PRIVATE -UNDEBUG)
add_executable(fg401-preferences-test EXCLUDE_FROM_ALL ../tests/test_fg401_preferences.cpp shim/bbport_settings.cpp)
target_include_directories(fg401-preferences-test PRIVATE shim)
target_compile_options(fg401-preferences-test PRIVATE -UNDEBUG)

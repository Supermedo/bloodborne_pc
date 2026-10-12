# Official RTX Remix API is Windows x64 only; the runtime is loaded on demand.
add_library(bb-remix STATIC remix_renderer.cpp remix_vulkan.cpp remix_scene.cpp remix_client.cpp)
target_include_directories(bb-remix PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(bb-remix PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN VK_USE_PLATFORM_WIN32_KHR)
target_link_libraries(bb-remix PUBLIC Vulkan::Vulkan user32 bcrypt)
add_executable(bb-remix-host remix_host.cpp ../../src/crash_win.c)
target_link_libraries(bb-remix-host PRIVATE bb-remix)
target_compile_options(bb-remix-host PRIVATE -municode)
target_link_options(bb-remix-host PRIVATE -municode)
add_executable(bb-remix-check EXCLUDE_FROM_ALL remix_check.cpp)
target_link_libraries(bb-remix-check PRIVATE bb-remix)
target_compile_options(bb-remix-check PRIVATE -municode)
target_link_options(bb-remix-check PRIVATE -municode)

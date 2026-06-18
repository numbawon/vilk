add_library(vilk_render_vulkan STATIC
    vulkan_backend.cpp
)

target_include_directories(vilk_render_vulkan PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(vilk_render_vulkan PUBLIC
    vilk_platform_defs
    Vulkan::Vulkan
)

# Phase 1 test shaders -- hand-written GLSL compiled to SPIR-V at build time.
# Runtime GLSL->SPIR-V (for preset shaders) uses glslang/shaderc C API;
# that path is proven here in Phase 1 before Phase 3 wires it to projectM.
set(SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/assets/shaders")
file(MAKE_DIRECTORY "${SHADER_OUTPUT_DIR}")

compile_shaders(
    TARGET     vilk_render_vulkan
    OUTPUT_DIR "${SHADER_OUTPUT_DIR}"
    SOURCES
        # Phase 1: add test_vert.vert / test_frag.frag here
        # ${PROJECT_SOURCE_DIR}/assets/shaders/test_vert.vert
        # ${PROJECT_SOURCE_DIR}/assets/shaders/test_frag.frag
)

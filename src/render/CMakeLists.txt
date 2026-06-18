add_library(vilk_render STATIC
    render_backend.cpp
)

target_include_directories(vilk_render PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(vilk_render PUBLIC vilk_platform_defs)

if(VILK_VULKAN_BACKEND)
    add_subdirectory(vulkan)
    target_link_libraries(vilk_render PUBLIC vilk_render_vulkan)
endif()

if(VILK_GL_BACKEND)
    add_subdirectory(gl)
    target_link_libraries(vilk_render PUBLIC vilk_render_gl)
endif()

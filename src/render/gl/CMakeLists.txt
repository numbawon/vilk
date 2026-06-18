# Phase 4 -- GL fallback backend. Not built unless VILK_GL_BACKEND=ON.
add_library(vilk_render_gl STATIC
    gl_backend.cpp
)

target_include_directories(vilk_render_gl PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(vilk_render_gl PUBLIC vilk_platform_defs)

find_package(OpenGL REQUIRED)
target_link_libraries(vilk_render_gl PUBLIC OpenGL::GL)

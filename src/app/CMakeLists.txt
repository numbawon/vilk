add_executable(vilk
    main.cpp
    projectm_driver.cpp
)

target_include_directories(vilk PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/render
)

# Bake the absolute assets path so shaders are found regardless of CWD
target_compile_definitions(vilk PRIVATE
    "VILK_ASSETS_DIR=\"${PROJECT_SOURCE_DIR}/assets\""
    "VILK_PRESETS_DIR=\"${VILK_PRESETS_DIR}\""
)

target_link_libraries(vilk PRIVATE
    vilk_audio
    vilk_render
    glfw
    projectM  # libprojectM for preset loading + GLSL capture
)

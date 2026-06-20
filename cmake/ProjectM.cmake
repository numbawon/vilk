# cmake/ProjectM.cmake
# Builds libprojectM 4.x as a static library from the vendored submodule.
# Options are locked to the minimum needed for the GLSL-intercept path.

set(PROJECTM_DIR "${CMAKE_SOURCE_DIR}/third_party/projectm")

if(NOT EXISTS "${PROJECTM_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "[vilk] third_party/projectm submodule not initialized.\n"
        "Run: git submodule update --init --recursive third_party/projectm")
endif()

# Force static lib so we avoid DLL deployment during development.
set(BUILD_SHARED_LIBS      OFF CACHE BOOL "" FORCE)
set(ENABLE_SDL_UI          OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING          OFF CACHE BOOL "" FORCE)
set(BUILD_DOCS             OFF CACHE BOOL "" FORCE)
set(ENABLE_PLAYLIST        OFF CACHE BOOL "" FORCE)
# Use the bundled projectM-eval; requires the nested submodule to be present.
set(ENABLE_SYSTEM_PROJECTM_EVAL OFF CACHE BOOL "" FORCE)

add_subdirectory(${PROJECTM_DIR} third_party/projectm EXCLUDE_FROM_ALL)

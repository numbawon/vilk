include(FetchContent)

FetchContent_Declare(
    glslang
    GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
    GIT_TAG        14.3.0
    GIT_SHALLOW    TRUE
)

set(ENABLE_GLSLANG_BINARIES  OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL              OFF CACHE BOOL "" FORCE)
set(ENABLE_CTEST             OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL   OFF CACHE BOOL "" FORCE)
# Skip SPIRV-Tools (optimizer) -- we don't need it in Phase 1
set(ENABLE_OPT               OFF CACHE BOOL "" FORCE)
set(BUILD_EXTERNAL           OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(glslang)

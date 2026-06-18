cmake_minimum_required(VERSION 3.21)
project(vilk LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(VILK_VULKAN_BACKEND    "Build Vulkan render backend (Phase 1+)"   ON)
option(VILK_GL_BACKEND        "Build OpenGL fallback backend (Phase 4)"  OFF)
option(VILK_ENABLE_VALIDATION "Enable Vulkan validation layers in debug" ON)

include(cmake/Platform.cmake)
include(cmake/CompileShaders.cmake)

if(VILK_VULKAN_BACKEND)
    find_package(Vulkan REQUIRED)
endif()

add_subdirectory(src)

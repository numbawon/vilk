# compile_shaders(TARGET <target> SOURCES <file> ... OUTPUT_DIR <dir>)
#
# Compiles GLSL shaders to SPIR-V at build time via glslangValidator.
# Used for static test shaders (Phase 1). Runtime GLSL->SPIR-V for preset
# shaders uses the glslang/shaderc C API -- that path is validated in Phase 1
# and wired to projectM output in Phase 3.
find_program(GLSLANG_VALIDATOR
    NAMES glslangValidator
    HINTS ENV VULKAN_SDK
    PATH_SUFFIXES bin
)

if(NOT GLSLANG_VALIDATOR)
    message(STATUS "vilk: glslangValidator not found -- build-time shader compilation disabled")
endif()

function(compile_shaders)
    cmake_parse_arguments(ARGS "" "TARGET;OUTPUT_DIR" "SOURCES" ${ARGN})
    if(NOT GLSLANG_VALIDATOR)
        message(WARNING "compile_shaders: glslangValidator missing, skipping ${ARGS_TARGET} shaders")
        return()
    endif()
    foreach(SHADER IN LISTS ARGS_SOURCES)
        get_filename_component(SHADER_NAME "${SHADER}" NAME)
        set(SPV_OUT "${ARGS_OUTPUT_DIR}/${SHADER_NAME}.spv")
        add_custom_command(
            OUTPUT  "${SPV_OUT}"
            COMMAND "${GLSLANG_VALIDATOR}" -V "${SHADER}" -o "${SPV_OUT}"
            DEPENDS "${SHADER}"
            COMMENT "GLSL -> SPIR-V: ${SHADER_NAME}"
        )
        target_sources(${ARGS_TARGET} PRIVATE "${SPV_OUT}")
    endforeach()
endfunction()

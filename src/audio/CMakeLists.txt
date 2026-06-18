add_library(vilk_audio STATIC
    audio_capture.cpp
)

target_include_directories(vilk_audio PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(vilk_audio PUBLIC vilk_platform_defs)

# Platform-specific implementation
if(WIN32)
    target_sources(vilk_audio PRIVATE wasapi/wasapi_capture.cpp)
elseif(APPLE)
    target_sources(vilk_audio PRIVATE coreaudio/coreaudio_capture.cpp)
elseif(UNIX)
    target_sources(vilk_audio PRIVATE pulseaudio/pulseaudio_capture.cpp)
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(PULSEAUDIO libpulse)
        if(PULSEAUDIO_FOUND)
            target_link_libraries(vilk_audio PRIVATE ${PULSEAUDIO_LIBRARIES})
            target_include_directories(vilk_audio PRIVATE ${PULSEAUDIO_INCLUDE_DIRS})
        endif()
    endif()
endif()

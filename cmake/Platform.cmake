# Interface library carrying platform compile definitions and warning flags.
# All vilk targets link against this rather than setting flags globally.
add_library(vilk_platform_defs INTERFACE)

if(WIN32)
    target_compile_definitions(vilk_platform_defs INTERFACE
        VILK_PLATFORM_WINDOWS
        VILK_AUDIO_WASAPI
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
elseif(APPLE)
    target_compile_definitions(vilk_platform_defs INTERFACE
        VILK_PLATFORM_MACOS
        VILK_AUDIO_COREAUDIO
    )
elseif(UNIX)
    target_compile_definitions(vilk_platform_defs INTERFACE
        VILK_PLATFORM_LINUX
        VILK_AUDIO_PULSEAUDIO
    )
endif()

if(MSVC)
    target_compile_options(vilk_platform_defs INTERFACE /W4 /utf-8)
else()
    target_compile_options(vilk_platform_defs INTERFACE -Wall -Wextra -Wpedantic)
endif()

if(VILK_ENABLE_VALIDATION)
    target_compile_definitions(vilk_platform_defs INTERFACE VILK_ENABLE_VALIDATION)
endif()

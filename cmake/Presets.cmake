# cmake/Presets.cmake
# Optional community MilkDrop preset pack.
# Users can point VILK_PRESETS_DIR at any directory containing .milk files.
# If not set, the app starts without presets (using built-in feedback shader).

set(VILK_PRESETS_DIR "" CACHE PATH "Directory containing .milk preset files (optional)")

if(NOT VILK_PRESETS_DIR STREQUAL "")
    message(STATUS "[vilk] Preset directory: ${VILK_PRESETS_DIR}")
else()
    message(STATUS "[vilk] No preset directory set (VILK_PRESETS_DIR). "
                   "Pass -DVILK_PRESETS_DIR=<path> to enable MilkDrop presets.")
endif()

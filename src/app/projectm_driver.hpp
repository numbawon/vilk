#pragma once
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace vilk {

// Returns true if the shader was accepted/loaded, false to try the next preset.
using GlslCaptureCallback = std::function<bool(bool is_composite,
                                               std::string_view frag_glsl,
                                               std::string_view vert_glsl)>;

// Single-preset variant: one GL context, one preset, destroy on return.
bool load_preset_glsl(std::string_view preset_path,
                      uint32_t         width,
                      uint32_t         height,
                      GlslCaptureCallback callback);

// Batch variant: one GL context for all paths, stops at first preset that
// fires the composite GLSL callback. Returns true if one was found.
bool load_first_preset_glsl(const std::vector<std::string>& preset_paths,
                             uint32_t                        width,
                             uint32_t                        height,
                             GlslCaptureCallback             callback);

} // namespace vilk

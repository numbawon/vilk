#pragma once
#include <cstdint>
#include <string_view>
#include <vector>
#include <vulkan/vulkan.h>

namespace vilk::spirv {

// Call once before first compile; call shutdown() at app teardown.
bool init();
void shutdown();

// Compile GLSL source to SPIR-V words.
// Returns empty vector on failure (error printed to stderr).
std::vector<uint32_t> compile(std::string_view glsl,
                               VkShaderStageFlagBits stage,
                               std::string_view debug_name = "");

} // namespace vilk::spirv

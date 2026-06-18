#include "vilk/render/render_backend.hpp"

// Phase 1 stub -- Vulkan backend implementation lands here.
// Order of work per BUILD-PLAN.md Phase 1:
//   1. Swapchain creation + presentation loop
//   2. Ping-pong framebuffer pattern (two off-screen RTs, alternate read/write)
//   3. SPIR-V shader loading -- both from disk (.spv) and runtime-compiled via glslang C API
//   4. Validation layers on in debug builds (VILK_ENABLE_VALIDATION)
//   5. Exit criterion: textured quad through ping-pong pass, runtime-compiled SPIR-V, no projectM

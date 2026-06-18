#include "vilk/render/render_backend.hpp"
#include <stdexcept>

namespace vilk {

std::unique_ptr<IRenderBackend> make_render_backend(std::string_view backend_name) {
    // Phase 1: Vulkan backend factory wired here.
    // Phase 4: GL fallback factory wired here.
    (void)backend_name;
    throw std::runtime_error("no render backend built yet -- Phase 1 stub");
}

} // namespace vilk

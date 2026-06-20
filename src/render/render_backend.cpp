#include "vilk/render/render_backend.hpp"
#include <stdexcept>

#ifdef VILK_PLATFORM_WINDOWS
#  include "vulkan/vk_backend.hpp"
#elif defined(VILK_PLATFORM_LINUX)
#  include "vulkan/vk_backend.hpp"
#elif defined(VILK_PLATFORM_MACOS)
#  include "vulkan/vk_backend.hpp"
#endif

namespace vilk {

std::unique_ptr<IRenderBackend> make_render_backend(std::string_view backend_name) {
    if (backend_name == "vulkan") {
#if defined(VILK_PLATFORM_WINDOWS) || defined(VILK_PLATFORM_LINUX) || defined(VILK_PLATFORM_MACOS)
        return std::make_unique<VulkanBackend>();
#else
        throw std::runtime_error("Vulkan backend not available on this platform");
#endif
    }
    throw std::runtime_error("unknown backend: " + std::string(backend_name));
}

} // namespace vilk

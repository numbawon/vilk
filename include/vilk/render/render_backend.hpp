#pragma once
#include <cstdint>
#include <memory>
#include <string_view>

namespace vilk {

struct WindowHandle {
    void*    native_window;   // HWND / NSWindow* / GLFWwindow* etc
    void*    native_display;  // HINSTANCE / nil / Display* etc
    uint32_t width;
    uint32_t height;
};

// Opaque handle to a GPU-side shader program loaded from SPIR-V.
struct ShaderHandle {
    uint64_t id = 0;
    bool     valid() const { return id != 0; }
};

class IRenderBackend {
public:
    virtual ~IRenderBackend() = default;

    virtual bool init(const WindowHandle& window)     = 0;
    virtual void shutdown()                           = 0;

    virtual void begin_frame()                        = 0;
    virtual void end_frame()                          = 0;

    virtual void resize(uint32_t width, uint32_t height) = 0;

    // Runtime SPIR-V load -- used for both Phase 1 test shaders and
    // Phase 3 preset shaders produced by the GLSL->SPIR-V cross-compile step.
    virtual ShaderHandle load_spirv(const uint32_t* words, size_t word_count,
                                    std::string_view debug_name) = 0;
    virtual void         unload_shader(ShaderHandle handle)      = 0;
};

// Backend factory. Pass "vulkan" or "gl".
std::unique_ptr<IRenderBackend> make_render_backend(std::string_view backend_name);

} // namespace vilk

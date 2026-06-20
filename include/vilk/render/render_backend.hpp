#pragma once
#include "vilk/audio/audio_state.hpp"
#include <cstdint>
#include <memory>
#include <string_view>

// Forward-declare so consumers don't need to pull in GLFW headers here.
struct GLFWwindow;

namespace vilk {

struct WindowHandle {
    GLFWwindow*      window     = nullptr;
    uint32_t         width      = 0;
    uint32_t         height     = 0;
    std::string_view shader_dir = {}; // path to .glsl sources; empty = use embedded fallback
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

    // Store audio snapshot; copied into the GPU UBO at the next begin_frame().
    virtual void update_audio(const AudioSnapshot&)            {}
    // Upload raw PCM for waveform rendering (called same frame as update_audio).
    virtual void update_waveform(const WaveformSnapshot&)      {}

    // Recompile both shaders from GLSL source strings and hot-swap the pipeline.
    // Returns false (no swap) if compilation fails so the old pipeline keeps running.
    virtual bool reload_shaders(std::string_view /*vert_glsl*/, std::string_view /*frag_glsl*/) { return false; }

    // Swap the composite (final-pass) pipeline with a preset's GLSL 450 shaders.
    virtual bool reload_preset_composite(std::string_view /*vert_glsl*/, std::string_view /*frag_glsl*/) { return false; }

    // Swap the offscreen warp pipeline with a preset's GLSL 450 shaders.
    virtual bool reload_preset_warp(std::string_view /*vert_glsl*/, std::string_view /*frag_glsl*/) { return false; }

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

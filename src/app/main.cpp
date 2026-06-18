#include "vilk/audio/audio_capture.hpp"
#include "vilk/render/render_backend.hpp"

int main() {
    // Phase 0: stubs. Real init sequence lands in Phase 1.
    //
    // Expected flow:
    //   1. Parse args / load config
    //   2. Open window (GLFW or SDL3 -- decision pending, see BUILD-PLAN.md Section 7)
    //   3. make_render_backend("vulkan")->init(window_handle)
    //   4. make_audio_capture()->start(pcm_callback)
    //   5. Run present loop
    //   6. Shutdown in reverse order
    return 0;
}

#include "vilk/audio/audio_capture.hpp"

namespace vilk {

// Phase 0 stub -- WASAPI loopback capture implementation lands here.
// Target: system loopback (render endpoint monitor) so we capture whatever
// the user is playing without requiring mic access.

std::unique_ptr<IAudioCapture> make_audio_capture() {
    return nullptr; // stub
}

} // namespace vilk

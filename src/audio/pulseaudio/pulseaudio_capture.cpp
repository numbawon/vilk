#include "vilk/audio/audio_capture.hpp"

namespace vilk {

// Phase 0 stub -- PulseAudio/PipeWire monitor capture implementation lands here.
// Connect to the default sink's monitor source to capture system playback.
// PipeWire exposes a PulseAudio-compatible API, so the same code works on both.

std::unique_ptr<IAudioCapture> make_audio_capture() {
    return nullptr; // stub
}

} // namespace vilk

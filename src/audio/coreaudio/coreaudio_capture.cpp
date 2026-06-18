#include "vilk/audio/audio_capture.hpp"

namespace vilk {

// Phase 0 stub -- CoreAudio tap capture implementation lands here.
// macOS 14.2+: process audio tap via AudioHardwareCreateProcessTap.
// Earlier macOS: ScreenCaptureKit audio or BlackHole virtual device fallback.

std::unique_ptr<IAudioCapture> make_audio_capture() {
    return nullptr; // stub
}

} // namespace vilk

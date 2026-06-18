#pragma once
#include <cstdint>
#include <functional>
#include <memory>

namespace vilk {

struct PcmFrame {
    const float* samples;
    uint32_t     frame_count;
    uint32_t     channel_count;
    uint32_t     sample_rate;
};

using PcmCallback = std::function<void(const PcmFrame&)>;

class IAudioCapture {
public:
    virtual ~IAudioCapture() = default;

    virtual bool     start(PcmCallback callback) = 0;
    virtual void     stop()                      = 0;

    virtual uint32_t sample_rate()    const = 0;
    virtual uint32_t channel_count()  const = 0;
};

// Platform factory. Returns nullptr if audio capture is unavailable.
std::unique_ptr<IAudioCapture> make_audio_capture();

} // namespace vilk

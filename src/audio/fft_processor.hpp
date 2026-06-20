#pragma once
#include "vilk/audio/audio_capture.hpp"
#include "vilk/audio/audio_state.hpp"
#include <chrono>
#include <mutex>
#include <vector>

namespace vilk {

class FftProcessor {
public:
    static constexpr int kFftSize = 2048;
    static constexpr int kHopSize = 1024; // 50% overlap

    FftProcessor(uint32_t sample_rate, uint32_t channels);
    ~FftProcessor();

    // Thread-safe: called from WASAPI capture thread.
    void push(const PcmFrame& frame);

    // Thread-safe: called from render/main thread.
    AudioSnapshot    snapshot()  const;
    WaveformSnapshot waveform()  const;

private:
    void  run_fft();
    float sum_band(float lo_hz, float hi_hz) const;

    uint32_t sample_rate_;
    uint32_t channels_;
    void*    cfg_ = nullptr; // opaque kiss_fftr_cfg -- keeps kiss headers out of here

    std::vector<float> window_buf_; // sliding input window, size kFftSize
    std::vector<float> hann_;       // precomputed Hann coefficients
    std::vector<float> freq_buf_;   // kiss_fft_cpx[] as float pairs (r,i), size (kFftSize/2+1)*2
    std::vector<float> mag_;        // magnitude spectrum, size kFftSize/2+1
    int                write_pos_ = 0;

    float bass_max_   = 1e-6f;
    float mid_max_    = 1e-6f;
    float treble_max_ = 1e-6f;
    float vol_max_    = 1e-6f;

    mutable std::mutex snap_mutex_;
    AudioSnapshot      snap_;
    WaveformSnapshot   wave_snap_;
    std::chrono::steady_clock::time_point start_tp_;
};

} // namespace vilk

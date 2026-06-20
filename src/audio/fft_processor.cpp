#include "fft_processor.hpp"
#include <kiss_fftr.h>
#include <algorithm>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace vilk {

FftProcessor::FftProcessor(uint32_t sample_rate, uint32_t channels)
    : sample_rate_(sample_rate)
    , channels_(channels)
    , start_tp_(std::chrono::steady_clock::now())
{
    cfg_ = kiss_fftr_alloc(kFftSize, 0, nullptr, nullptr);

    window_buf_.resize(kFftSize, 0.f);
    freq_buf_.resize((kFftSize / 2 + 1) * 2, 0.f); // interleaved r,i pairs
    mag_.resize(kFftSize / 2 + 1, 0.f);

    hann_.resize(kFftSize);
    for (int i = 0; i < kFftSize; ++i)
        hann_[i] = 0.5f * (1.f - cosf(2.f * static_cast<float>(M_PI) * i / (kFftSize - 1)));
}

FftProcessor::~FftProcessor() {
    kiss_fftr_free(static_cast<kiss_fftr_cfg>(cfg_));
}

void FftProcessor::push(const PcmFrame& frame) {
    for (uint32_t i = 0; i < frame.frame_count; ++i) {
        float mono = 0.f;
        for (uint32_t c = 0; c < channels_; ++c)
            mono += frame.samples[i * channels_ + c];
        mono /= static_cast<float>(channels_);

        window_buf_[write_pos_++] = mono;

        if (write_pos_ == kFftSize) {
            run_fft();
            std::copy(window_buf_.begin() + kHopSize,
                      window_buf_.end(),
                      window_buf_.begin());
            write_pos_ = kFftSize - kHopSize;
        }
    }
}

void FftProcessor::run_fft() {
    std::vector<float> windowed(kFftSize);
    for (int i = 0; i < kFftSize; ++i)
        windowed[i] = window_buf_[i] * hann_[i];

    // kiss_fft_cpx and two consecutive floats have identical layout
    auto* cpx_out = reinterpret_cast<kiss_fft_cpx*>(freq_buf_.data());
    kiss_fftr(static_cast<kiss_fftr_cfg>(cfg_), windowed.data(), cpx_out);

    const float inv_n = 2.f / static_cast<float>(kFftSize);
    for (int i = 0; i <= kFftSize / 2; ++i) {
        float r  = cpx_out[i].r * inv_n;
        float im = cpx_out[i].i * inv_n;
        mag_[i]  = sqrtf(r * r + im * im);
    }

    float raw_bass   = sum_band(  20.f,   250.f);
    float raw_mid    = sum_band( 250.f,  4000.f);
    float raw_treble = sum_band(4000.f, 20000.f);

    float rms = 0.f;
    for (int i = 0; i < kFftSize; ++i)
        rms += window_buf_[i] * window_buf_[i];
    float raw_vol = sqrtf(rms / static_cast<float>(kFftSize));

    bass_max_   = std::max(bass_max_   * 0.998f, raw_bass);
    mid_max_    = std::max(mid_max_    * 0.998f, raw_mid);
    treble_max_ = std::max(treble_max_ * 0.998f, raw_treble);
    vol_max_    = std::max(vol_max_    * 0.998f, raw_vol);

    auto  now = std::chrono::steady_clock::now();
    float t   = std::chrono::duration<float>(now - start_tp_).count();

    std::lock_guard lk(snap_mutex_);
    snap_.bass   = raw_bass   / bass_max_;
    snap_.mid    = raw_mid    / mid_max_;
    snap_.treble = raw_treble / treble_max_;
    snap_.vol    = raw_vol    / vol_max_;
    snap_.time_s = t;

    // Decimate window_buf_ (2048 samples) down to kWaveformSize for display.
    constexpr int stride = kFftSize / kWaveformSize;
    for (int i = 0; i < kWaveformSize; ++i)
        wave_snap_.samples[i] = window_buf_[i * stride];
}

float FftProcessor::sum_band(float lo_hz, float hi_hz) const {
    const float bin_hz = static_cast<float>(sample_rate_) / kFftSize;
    int lo = std::max(1, static_cast<int>(lo_hz / bin_hz));
    int hi = std::min(kFftSize / 2, static_cast<int>(hi_hz / bin_hz));
    if (lo > hi) return 0.f;

    float sum = 0.f;
    for (int i = lo; i <= hi; ++i)
        sum += mag_[i];
    return sum / static_cast<float>(hi - lo + 1);
}

AudioSnapshot FftProcessor::snapshot() const {
    std::lock_guard lk(snap_mutex_);
    return snap_;
}

WaveformSnapshot FftProcessor::waveform() const {
    std::lock_guard lk(snap_mutex_);
    return wave_snap_;
}

} // namespace vilk

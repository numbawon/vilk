#pragma once

namespace vilk {

// Plain-old-data snapshot of the current audio analysis frame.
// Layout matches the AudioData std140 UBO in the fragment shader.
struct AudioSnapshot {
    float bass   = 0.f;   // 20-250 Hz, auto-normalized 0-1
    float mid    = 0.f;   // 250-4000 Hz
    float treble = 0.f;   // 4000-20000 Hz
    float vol    = 0.f;   // RMS amplitude, auto-normalized 0-1
    float time_s = 0.f;   // seconds since audio start
    float _pad[3] = {};   // pad to 32 bytes for std140
};

// Raw PCM snapshot for waveform rendering (mono, normalized [-1, 1]).
static constexpr int kWaveformSize = 512;
struct WaveformSnapshot {
    float samples[kWaveformSize] = {};
};

} // namespace vilk

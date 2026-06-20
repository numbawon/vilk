#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <atomic>
#include <cstdio>
#include <future>
#include <memory>
#include <thread>
#include <vector>

#include "vilk/audio/audio_capture.hpp"

namespace vilk {

namespace {

template<typename T>
struct ComRelease { void operator()(T* p) const noexcept { if (p) p->Release(); } };
template<typename T>
using ComPtr = std::unique_ptr<T, ComRelease<T>>;

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT without pulling in ksmedia.h
static const GUID kSubtypeIeeeFloat = {
    0x00000003, 0x0000, 0x0010,
    { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 }
};

} // namespace

class WasapiCapture final : public IAudioCapture {
public:
    ~WasapiCapture() override { stop(); }

    bool start(PcmCallback callback) override {
        if (running_.load(std::memory_order_acquire)) return false;
        callback_ = std::move(callback);

        std::promise<bool> ready;
        auto fut = ready.get_future();
        running_.store(true, std::memory_order_release);

        thread_ = std::thread([this, p = std::move(ready)]() mutable {
            capture_thread(std::move(p));
        });

        bool ok = fut.get();
        if (!ok) {
            running_.store(false, std::memory_order_release);
            thread_.join();
        }
        return ok;
    }

    void stop() override {
        running_.store(false, std::memory_order_release);
        if (thread_.joinable()) thread_.join();
    }

    uint32_t sample_rate()   const override { return sample_rate_; }
    uint32_t channel_count() const override { return channel_count_; }

private:
    void capture_thread(std::promise<bool> ready) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        IMMDeviceEnumerator* raw_enum = nullptr;
        HRESULT hr = CoCreateInstance(
            __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&raw_enum));
        if (FAILED(hr)) { ready.set_value(false); CoUninitialize(); return; }
        ComPtr<IMMDeviceEnumerator> enumerator(raw_enum);

        IMMDevice* raw_dev = nullptr;
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &raw_dev);
        if (FAILED(hr)) { ready.set_value(false); CoUninitialize(); return; }
        ComPtr<IMMDevice> device(raw_dev);

        IAudioClient* raw_client = nullptr;
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                               nullptr, reinterpret_cast<void**>(&raw_client));
        if (FAILED(hr)) { ready.set_value(false); CoUninitialize(); return; }
        ComPtr<IAudioClient> client(raw_client);

        WAVEFORMATEX* fmt = nullptr;
        hr = client->GetMixFormat(&fmt);
        if (FAILED(hr)) { ready.set_value(false); CoUninitialize(); return; }

        sample_rate_   = fmt->nSamplesPerSec;
        channel_count_ = fmt->nChannels;

        bool is_float = (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
        if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(fmt);
            is_float = (IsEqualGUID(ext->SubFormat, kSubtypeIeeeFloat) != 0);
        }

        constexpr REFERENCE_TIME kBufferDuration = 200000; // 20 ms in 100-ns units
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                AUDCLNT_STREAMFLAGS_LOOPBACK,
                                kBufferDuration, 0, fmt, nullptr);
        CoTaskMemFree(fmt);
        if (FAILED(hr)) { ready.set_value(false); CoUninitialize(); return; }

        IAudioCaptureClient* raw_cap = nullptr;
        hr = client->GetService(__uuidof(IAudioCaptureClient),
                                reinterpret_cast<void**>(&raw_cap));
        if (FAILED(hr)) { ready.set_value(false); CoUninitialize(); return; }
        ComPtr<IAudioCaptureClient> capture(raw_cap);

        hr = client->Start();
        if (FAILED(hr)) { ready.set_value(false); CoUninitialize(); return; }

        ready.set_value(true);

        // Conversion buffer -- reused across calls
        std::vector<float> conv_buf;

        while (running_.load(std::memory_order_acquire)) {
            UINT32 next_size = 0;
            if (FAILED(capture->GetNextPacketSize(&next_size)) || next_size == 0) {
                Sleep(5);
                continue;
            }

            BYTE*  data   = nullptr;
            UINT32 frames = 0;
            DWORD  flags  = 0;
            if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) {
                Sleep(5);
                continue;
            }

            const float* samples = nullptr;
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                conv_buf.assign(static_cast<size_t>(frames) * channel_count_, 0.0f);
                samples = conv_buf.data();
            } else if (is_float) {
                samples = reinterpret_cast<const float*>(data);
            } else {
                // 16-bit signed PCM -> float32
                const size_t n = static_cast<size_t>(frames) * channel_count_;
                conv_buf.resize(n);
                const int16_t* src = reinterpret_cast<const int16_t*>(data);
                for (size_t i = 0; i < n; ++i)
                    conv_buf[i] = src[i] / 32768.0f;
                samples = conv_buf.data();
            }

            PcmFrame frame{ samples, frames, channel_count_, sample_rate_ };
            callback_(frame);

            capture->ReleaseBuffer(frames);
        }

        client->Stop();
        CoUninitialize();
    }

    PcmCallback       callback_;
    std::thread       thread_;
    std::atomic<bool> running_{ false };
    uint32_t          sample_rate_   = 0;
    uint32_t          channel_count_ = 0;
};

std::unique_ptr<IAudioCapture> make_audio_capture() {
    return std::make_unique<WasapiCapture>();
}

} // namespace vilk

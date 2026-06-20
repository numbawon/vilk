#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include "vilk/audio/audio_capture.hpp"
#include "vilk/render/render_backend.hpp"
#include "audio/fft_processor.hpp"
#include "render/shader_watcher.hpp"
#include "projectm_driver.hpp"
#include "render/vulkan/preset_glsl_adapter.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#ifndef VILK_ASSETS_DIR
#define VILK_ASSETS_DIR "assets"
#endif

#ifndef VILK_PRESETS_DIR
#define VILK_PRESETS_DIR ""
#endif

namespace {

bool     g_resized    = false;
uint32_t g_new_w      = 0;
uint32_t g_new_h      = 0;
float    g_gain       = 0.5f;   // audio injection scale: = key raises, - key lowers
GLFWwindow* g_window  = nullptr;

static void update_title() {
    char buf[64];
    snprintf(buf, sizeof(buf), "vilk  gain: %.1f  (= / -)", g_gain);
    glfwSetWindowTitle(g_window, buf);
    fprintf(stderr, "[vilk] gain=%.2f\n", g_gain);
}

void on_framebuffer_resize(GLFWwindow*, int w, int h) {
    g_resized = true;
    g_new_w   = static_cast<uint32_t>(w);
    g_new_h   = static_cast<uint32_t>(h);
}

void on_key(GLFWwindow* win, int key, int /*scancode*/, int action, int /*mods*/) {
    if (action != GLFW_PRESS && action != GLFW_REPEAT) return;
    if (key == GLFW_KEY_ESCAPE) { glfwSetWindowShouldClose(win, GLFW_TRUE); return; }
    if (key == GLFW_KEY_EQUAL)  { g_gain = std::min(g_gain + 0.1f, 3.0f); update_title(); }
    if (key == GLFW_KEY_MINUS)  { g_gain = std::max(g_gain - 0.1f, 0.0f); update_title(); }
    if (key == GLFW_KEY_0)      { g_gain = 0.5f;                           update_title(); }
}

// Return all .milk files in VILK_PRESETS_DIR, sorted alphabetically.
std::vector<std::string> list_presets() {
    namespace fs = std::filesystem;
    std::string dir = VILK_PRESETS_DIR;
    if (dir.empty()) return {};
    std::vector<std::string> paths;
    std::error_code ec;
    for (auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (entry.path().extension() == ".milk")
            paths.push_back(entry.path().string());
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}

} // namespace

int main(int argc, char** argv) {
    bool no_preset = false;
    for (int i = 1; i < argc; ++i)
        if (std::string_view(argv[i]) == "--no-preset") no_preset = true;

    if (!glfwInit()) {
        fprintf(stderr, "[vilk] glfwInit failed\n");
        return 1;
    }

    // Vulkan window
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE,  GLFW_TRUE);

    constexpr uint32_t kWidth  = 1280;
    constexpr uint32_t kHeight = 720;

    GLFWwindow* window = glfwCreateWindow(
        static_cast<int>(kWidth), static_cast<int>(kHeight),
        "vilk", nullptr, nullptr);
    if (!window) {
        fprintf(stderr, "[vilk] glfwCreateWindow failed\n");
        glfwTerminate();
        return 1;
    }

    g_window = window;
    glfwSetFramebufferSizeCallback(window, on_framebuffer_resize);
    glfwSetKeyCallback(window, on_key);
    update_title();

    // Render backend
    auto renderer = vilk::make_render_backend("vulkan");
    {
        vilk::WindowHandle wh{ window, kWidth, kHeight, VILK_ASSETS_DIR "/shaders" };
        if (!renderer->init(wh)) {
            fprintf(stderr, "[vilk] render backend init failed\n");
            glfwDestroyWindow(window);
            glfwTerminate();
            return 1;
        }
    }

    // Shader hot-reload watcher (built-in feedback shaders)
    vilk::ShaderWatcher watcher(
        VILK_ASSETS_DIR "/shaders/vilk.vert.glsl",
        VILK_ASSETS_DIR "/shaders/vilk.frag.glsl");
    watcher.start();

    // Audio capture + FFT
    auto audio = vilk::make_audio_capture();
    std::unique_ptr<vilk::FftProcessor> fft;
    std::atomic<vilk::FftProcessor*>    fft_ptr{ nullptr };

    if (audio) {
        bool ok = audio->start([&fft_ptr](const vilk::PcmFrame& frame) {
            if (auto* f = fft_ptr.load(std::memory_order_acquire))
                f->push(frame);
        });
        if (ok) {
            fft = std::make_unique<vilk::FftProcessor>(
                audio->sample_rate(), audio->channel_count());
            fft_ptr.store(fft.get(), std::memory_order_release);
        } else {
            fprintf(stderr, "[vilk] audio capture start failed (non-fatal)\n");
        }
    }

    // --- Phase 3: load custom Kali Mix shaders (bypass .milk adapter entirely) ---
    if (!no_preset) {
        namespace fs = std::filesystem;
        auto read_shader = [](const std::string& path) -> std::string {
            std::ifstream f(path);
            if (!f) return {};
            return { std::istreambuf_iterator<char>(f), {} };
        };
        std::string shader_dir = VILK_ASSETS_DIR "/shaders";
        auto warp_frag = read_shader(shader_dir + "/kali_warp.frag.glsl");
        auto comp_vert = read_shader(shader_dir + "/kali_comp.vert.glsl");
        auto comp_frag = read_shader(shader_dir + "/kali_comp.frag.glsl");
        if (warp_frag.empty() || comp_vert.empty() || comp_frag.empty()) {
            fprintf(stderr, "[vilk] custom Kali Mix shaders not found in %s\n", shader_dir.c_str());
        } else {
            bool warp_ok = renderer->reload_preset_warp("", warp_frag);
            bool comp_ok = renderer->reload_preset_composite(comp_vert, comp_frag);
            if (warp_ok && comp_ok)
                fprintf(stderr, "[vilk] custom Kali Mix shaders loaded\n");
            else
                fprintf(stderr, "[vilk] custom Kali Mix shader load failed\n");
        }
    } // if (!no_preset)

    // Present loop
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        int fb_w = 0, fb_h = 0;
        glfwGetFramebufferSize(window, &fb_w, &fb_h);
        if (fb_w == 0 || fb_h == 0) continue;

        if (g_resized) {
            g_resized = false;
            renderer->resize(g_new_w, g_new_h);
            continue;
        }

        // Hot-reload built-in shaders
        {
            std::string vert_src, frag_src;
            if (watcher.poll(vert_src, frag_src)) {
                if (!renderer->reload_shaders(vert_src, frag_src))
                    fprintf(stderr, "[vilk] shader reload failed, keeping old pipeline\n");
                else
                    fprintf(stderr, "[vilk] shaders reloaded\n");
            }
        }

        if (fft) {
            auto snap = fft->snapshot();
            // Scale audio injection by gain (time_s is absolute, not scaled)
            snap.bass   *= g_gain;
            snap.mid    *= g_gain;
            snap.treble *= g_gain;
            snap.vol    *= g_gain;
            renderer->update_audio(snap);
            renderer->update_waveform(fft->waveform());
        }

        renderer->begin_frame();
        renderer->end_frame();
    }

    // Shutdown in reverse init order
    watcher.stop();
    fft_ptr.store(nullptr, std::memory_order_release);
    if (audio) audio->stop();
    fft.reset();

    renderer->shutdown();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

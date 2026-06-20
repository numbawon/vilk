#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace vilk {

// Polls two GLSL source files every 250 ms.
// When either file changes, stores the new source pair for poll() to drain.
// Thread-safe: watch thread writes, main thread reads via poll().
class ShaderWatcher {
public:
    ShaderWatcher(std::string vert_path, std::string frag_path);
    ~ShaderWatcher();

    void start();
    void stop();

    // Call from main thread before begin_frame.
    // Returns true and fills out params if new sources are ready.
    bool poll(std::string& out_vert, std::string& out_frag);

private:
    void watch_fn();
    static std::string read_file(const std::string& path);

    std::string vert_path_;
    std::string frag_path_;

    std::thread       thread_;
    std::atomic<bool> running_{ false };

    std::mutex  pending_mutex_;
    bool        has_pending_   = false;
    std::string pending_vert_;
    std::string pending_frag_;
};

} // namespace vilk

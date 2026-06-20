#include "shader_watcher.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstdio>

namespace vilk {

ShaderWatcher::ShaderWatcher(std::string vert_path, std::string frag_path)
    : vert_path_(std::move(vert_path))
    , frag_path_(std::move(frag_path))
{}

ShaderWatcher::~ShaderWatcher() { stop(); }

void ShaderWatcher::start() {
    if (running_.load(std::memory_order_acquire)) return;
    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { watch_fn(); });
}

void ShaderWatcher::stop() {
    running_.store(false, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
}

bool ShaderWatcher::poll(std::string& out_vert, std::string& out_frag) {
    std::lock_guard lk(pending_mutex_);
    if (!has_pending_) return false;
    out_vert    = std::move(pending_vert_);
    out_frag    = std::move(pending_frag_);
    has_pending_ = false;
    return true;
}

void ShaderWatcher::watch_fn() {
    namespace fs = std::filesystem;
    using clock  = fs::file_time_type;

    auto get_mtime = [](const std::string& p) -> clock {
        std::error_code ec;
        return fs::last_write_time(p, ec);
    };

    clock vert_t = get_mtime(vert_path_);
    clock frag_t = get_mtime(frag_path_);

    while (running_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));

        clock new_vert_t = get_mtime(vert_path_);
        clock new_frag_t = get_mtime(frag_path_);

        if (new_vert_t == vert_t && new_frag_t == frag_t) continue;

        vert_t = new_vert_t;
        frag_t = new_frag_t;

        // Small extra delay so editors finish writing before we read
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        std::string vert_src = read_file(vert_path_);
        std::string frag_src = read_file(frag_path_);

        if (vert_src.empty() || frag_src.empty()) {
            fprintf(stderr, "[watcher] failed to read shader file(s)\n");
            continue;
        }

        fprintf(stderr, "[watcher] shader change detected, queuing reload\n");
        std::lock_guard lk(pending_mutex_);
        pending_vert_ = std::move(vert_src);
        pending_frag_ = std::move(frag_src);
        has_pending_  = true;
    }
}

std::string ShaderWatcher::read_file(const std::string& path) {
    std::ifstream f(path, std::ios::in | std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace vilk

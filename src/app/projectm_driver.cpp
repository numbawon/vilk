#include "projectm_driver.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <projectM-4/core.h>
#include <projectM-4/glsl_callback.h>
#include <projectM-4/parameters.h>
#include <projectM-4/render_opengl.h>

#include <cstdio>
#include <string>

static void* glfw_proc_loader(const char* name, void* /*user_data*/) {
    return reinterpret_cast<void*>(glfwGetProcAddress(name));
}

namespace vilk {

bool load_preset_glsl(std::string_view    preset_path,
                      uint32_t            width,
                      uint32_t            height,
                      GlslCaptureCallback callback) {
    // Hidden GL 3.3 core window for projectM.
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_VISIBLE,        GLFW_FALSE);
    glfwWindowHint(GLFW_CLIENT_API,     GLFW_OPENGL_API);

    GLFWwindow* gl_win = glfwCreateWindow(
        static_cast<int>(width), static_cast<int>(height),
        "vilk-pm-offscreen", nullptr, nullptr);
    if (!gl_win) {
        fprintf(stderr, "[vilk-driver] failed to create hidden GL window\n");
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        return false;
    }
    glfwMakeContextCurrent(gl_win);

    projectm_handle pm = projectm_create_with_opengl_load_proc(glfw_proc_loader, nullptr);
    if (!pm) {
        fprintf(stderr, "[vilk-driver] projectm_create failed\n");
        glfwDestroyWindow(gl_win);
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        return false;
    }
    projectm_set_window_size(pm, width, height);

    // Wire the GLSL intercept callback.
    auto raw_cb = [](bool is_composite, const char* frag, const char* vert, void* ud) {
        auto* fn = static_cast<GlslCaptureCallback*>(ud);
        (*fn)(is_composite, frag ? frag : "", vert ? vert : "");
    };
    projectm_set_glsl_callback(raw_cb, &callback);


    // Load the preset — fires the callback if it has pixel shaders.
    std::string path(preset_path);
    projectm_load_preset_file(pm, path.c_str(), false);
    projectm_opengl_render_frame(pm);

    projectm_set_glsl_callback(nullptr, nullptr);
    projectm_destroy(pm);
    glfwMakeContextCurrent(nullptr);
    glfwDestroyWindow(gl_win);
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE,    GLFW_TRUE);
    return true;
}

// ---------------------------------------------------------------------------
// Batch variant: one GL context, try each path until callback fires.
// ---------------------------------------------------------------------------
bool load_first_preset_glsl(const std::vector<std::string>& preset_paths,
                             uint32_t                        width,
                             uint32_t                        height,
                             GlslCaptureCallback             callback) {
    if (preset_paths.empty()) return false;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_VISIBLE,        GLFW_FALSE);
    glfwWindowHint(GLFW_CLIENT_API,     GLFW_OPENGL_API);

    GLFWwindow* gl_win = glfwCreateWindow(
        static_cast<int>(width), static_cast<int>(height),
        "vilk-pm-offscreen", nullptr, nullptr);
    if (!gl_win) {
        fprintf(stderr, "[vilk-driver] failed to create hidden GL window\n");
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        return false;
    }
    glfwMakeContextCurrent(gl_win);

    projectm_handle pm = projectm_create_with_opengl_load_proc(glfw_proc_loader, nullptr);
    if (!pm) {
        fprintf(stderr, "[vilk-driver] projectm_create failed\n");
        glfwDestroyWindow(gl_win);
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        return false;
    }
    projectm_set_window_size(pm, width, height);

    bool got_composite = false;
    bool got_warp      = false;

    struct State { GlslCaptureCallback* cb; bool* got_c; bool* got_w; };
    State state{ &callback, &got_composite, &got_warp };

    auto raw_cb = [](bool is_composite, const char* frag, const char* vert, void* ud) {
        auto* s = static_cast<State*>(ud);
        bool ok = (*s->cb)(is_composite, frag ? frag : "", vert ? vert : "");
        if (is_composite && ok) *s->got_c = true;
        if (!is_composite && ok) *s->got_w = true;
    };

    constexpr int kMaxScan = 50;
    int scan_count = 0;
    for (const auto& path : preset_paths) {
        got_composite = false;
        got_warp      = false;
        projectm_set_glsl_callback(raw_cb, &state);
        projectm_load_preset_file(pm, path.c_str(), false);
        projectm_opengl_render_frame(pm);
        projectm_set_glsl_callback(nullptr, nullptr);

        ++scan_count;
        if (got_composite && (got_warp || scan_count >= kMaxScan)) {
            fprintf(stderr, "[vilk-driver] preset loaded (%s warp): %s\n",
                    got_warp ? "with" : "no", path.c_str());
            break;
        }
    }

    projectm_destroy(pm);
    glfwMakeContextCurrent(nullptr);
    glfwDestroyWindow(gl_win);
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE,    GLFW_TRUE);
    return got_composite;
}

} // namespace vilk

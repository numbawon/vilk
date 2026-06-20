#include "vk_backend.hpp"
#include "spirv_compiler.hpp"
#include "test_shaders.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace vilk {

// Warp mesh vertex shader: reads (pos, uv) from vertex buffer so the per-pixel
// rotation computed on the CPU is passed through to the warp fragment shader.
// Warp mesh vertex shader: must match the output locations expected by the adapted
// preset warp fragment shader (frag_COLOR@0, frag_TEXCOORD0@1, frag_TEXCOORD1@2).
// a_uv carries the per-pixel-equation-rotated sampling UV written by update_warp_mesh().
static const char* kWarpMeshVertGlsl = R"GLSL(
#version 450
layout(location = 0) in  vec2 a_pos;   // NDC screen position from warp mesh
layout(location = 1) in  vec2 a_uv;    // pre-rotated PP sampling UV from warp mesh

layout(location = 0) out vec4 frag_COLOR;      // tint: always white
layout(location = 1) out vec4 frag_TEXCOORD0;  // (rotated_u, rotated_v, 0, 0)
layout(location = 2) out vec2 frag_TEXCOORD1;  // (screen_rad*2, screen_ang) pre-rotation

void main() {
    gl_Position    = vec4(a_pos, 0.0, 1.0);
    frag_COLOR     = vec4(1.0);
    frag_TEXCOORD0 = vec4(a_uv, 0.0, 0.0);  // warp frag reads .xy as sampler_main UV
    vec2 c = a_pos * 0.5;                   // centered, [-0.5, 0.5]
    frag_TEXCOORD1 = vec2(length(c) * 2.0, atan(c.y, c.x));
}
)GLSL";

// ---------------------------------------------------------------------------
// Seed fragment shader: thin bright ring, pure audio-reactive, no prev-frame.
// Ring width ~1.5px at 720p → average brightness < warp drain (0.004/frame)
// so ping-pong never saturates, but ring peak (~0.4) exceeds composite
// threshold (0.176) → composite amplifies the ring → visible MilkDrop patterns.
// ---------------------------------------------------------------------------
// Kali Mix warp: shift = (tex2D(sampler_main, uv+1px).rg - 0.37) * 0.02
// Root cause of stripes: cells were 53px wide but the blur offset is only 1px.
// 98% of pixels had the same cell as their 1px neighbor → uniform shift → stripes.
// Fix: per-pixel noise (1px cells via gl_FragCoord). Every pixel's 1px neighbor
// is always a different cell → varied displacement at every pixel → turbulence.
// Equilibrium for alpha=0.20: PP* = seed_center - 0.004*(0.80/0.20) = 0.374 → center=0.39.
static const char* kSeedFragGlsl = R"GLSL(
#version 450
layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 out_color;
layout(set = 0, binding = 1, std140) uniform AudioUBO {
    float bass; float mid; float treble; float vol; float time_s;
    float _pad0; float _pad1; float _pad2;
} u;

float hash(uvec2 p) {
    uint n = p.x * 1664525u + p.y * 22695477u + 1013904223u;
    n ^= (n >> 16u); n *= 0x45d9f3bu; n ^= (n >> 16u);
    return float(n) / float(0xffffffffu);
}

void main() {
    // Per-pixel noise: gl_FragCoord gives exact pixel coords.
    // Each pixel's 1px neighbor is always a different cell → varied warp displacement.
    uint ts = uint(floor(u.time_s * 6.0));
    uvec2 px = uvec2(gl_FragCoord.xy);
    float r = 0.39 + (hash(px + uvec2(ts * 1337u, ts * 2027u))        - 0.5) * 0.28;
    float g = 0.39 + (hash(px + uvec2(ts * 2049u + 999u, ts * 1301u)) - 0.5) * 0.28;

    vec2  c     = v_uv - 0.5;
    float dist  = length(c);
    float ring_r = 0.12 + u.bass * 0.08;
    float diff   = (dist - ring_r) * 720.0;
    float ring   = exp(-diff * diff * 0.8);
    float bonus  = (u.bass * 0.2 + u.mid * 0.08) * ring;

    out_color = vec4(r + bonus, g + bonus, u.treble * 0.35 + bonus, 0.20);
}
)GLSL";

// ---------------------------------------------------------------------------
// Validation layer debug callback
// ---------------------------------------------------------------------------
static VKAPI_ATTR VkBool32 VKAPI_CALL vk_debug_cb(
    VkDebugUtilsMessageSeverityFlagBitsEXT  severity,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void*)
{
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        fprintf(stderr, "[Vulkan] %s\n", data->pMessage);
    return VK_FALSE;
}

// ---------------------------------------------------------------------------
// IRenderBackend impl
// ---------------------------------------------------------------------------
static std::string read_glsl_file(const std::string& path) {
    std::ifstream f(path, std::ios::in | std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool VulkanBackend::init(const WindowHandle& wh) {
    width_      = wh.width;
    height_     = wh.height;
    shader_dir_ = std::string(wh.shader_dir);

    if (!create_instance())                         return false;
    if (!setup_debug_messenger())                   return false;
    if (!create_surface(wh.window))                 return false;
    if (!pick_physical_device())                    return false;
    if (!create_device())                           return false;
    if (!create_swapchain(width_, height_))         return false;
    if (!create_render_passes())                    return false;
    if (!create_ping_pong_targets(width_, height_)) return false;
    if (!create_framebuffers())                     return false;
    if (!create_command_pool_and_buffers())         return false;
    if (!create_sync_objects())                     return false;
    transition_ping_pong_initial();
    if (!create_audio_ubo())                        return false;
    if (!create_noise_texture())                    return false;
    if (!create_warp_mesh_buffers())               return false;

    if (!spirv::init()) {
        fprintf(stderr, "[vilk] glslang init failed\n");
        return false;
    }

    // Try loading live GLSL sources; fall back to embedded if unavailable
    std::string vert_src, frag_src;
    if (!shader_dir_.empty()) {
        vert_src = read_glsl_file(shader_dir_ + "/vilk.vert.glsl");
        frag_src = read_glsl_file(shader_dir_ + "/vilk.frag.glsl");
    }
    if (vert_src.empty() || frag_src.empty()) {
        fprintf(stderr, "[vilk] shader files not found, using embedded fallback\n");
        vert_src = test_shaders::kFullscreenQuadVert;
        frag_src = test_shaders::kPassthroughFrag;
    }

    auto vert_spv = spirv::compile(vert_src, VK_SHADER_STAGE_VERTEX_BIT,   "vilk.vert.glsl");
    auto frag_spv = spirv::compile(frag_src, VK_SHADER_STAGE_FRAGMENT_BIT, "vilk.frag.glsl");
    if (vert_spv.empty() || frag_spv.empty()) return false;

    auto vert_mod = load_spirv(vert_spv.data(), vert_spv.size(), "vilk.vert.glsl");
    auto frag_mod = load_spirv(frag_spv.data(), frag_spv.size(), "vilk.frag.glsl");
    if (!vert_mod.valid() || !frag_mod.valid()) return false;

    if (!create_descriptor_layout()) return false;
    if (!create_pipelines(reinterpret_cast<VkShaderModule>(vert_mod.id),
                          reinterpret_cast<VkShaderModule>(frag_mod.id))) return false;

    // Seed pipeline: same vert, dedicated frag (no prev-frame read, pure audio-reactive ring)
    {
        auto seed_spv = spirv::compile(kSeedFragGlsl, VK_SHADER_STAGE_FRAGMENT_BIT, "seed.frag");
        if (seed_spv.empty()) return false;
        auto seed_mod = load_spirv(seed_spv.data(), seed_spv.size(), "seed.frag");
        if (!seed_mod.valid()) return false;
        bool ok = create_seed_pipeline(reinterpret_cast<VkShaderModule>(vert_mod.id),
                                       reinterpret_cast<VkShaderModule>(seed_mod.id));
        unload_shader(seed_mod);
        if (!ok) return false;
    }

    if (!create_descriptor_sets()) return false;
    update_descriptor_sets();

    if (!create_waveform_buffers())  return false;
    if (!create_waveform_pipeline()) return false;

    unload_shader(vert_mod);
    unload_shader(frag_mod);

    initialized_ = true;
    return true;
}

bool VulkanBackend::reload_shaders(std::string_view vert_glsl, std::string_view frag_glsl) {
    auto vert_spv = spirv::compile(vert_glsl, VK_SHADER_STAGE_VERTEX_BIT,   "vilk.vert.glsl");
    auto frag_spv = spirv::compile(frag_glsl, VK_SHADER_STAGE_FRAGMENT_BIT, "vilk.frag.glsl");
    if (vert_spv.empty() || frag_spv.empty()) return false;

    auto vert_mod = load_spirv(vert_spv.data(), vert_spv.size(), "vilk.vert.glsl");
    auto frag_mod = load_spirv(frag_spv.data(), frag_spv.size(), "vilk.frag.glsl");
    if (!vert_mod.valid() || !frag_mod.valid()) {
        if (vert_mod.valid()) unload_shader(vert_mod);
        if (frag_mod.valid()) unload_shader(frag_mod);
        return false;
    }

    vkDeviceWaitIdle(device_);
    vkDestroyPipeline(device_, pipeline_offscreen_, nullptr);
    vkDestroyPipeline(device_, pipeline_composite_, nullptr);

    bool ok = create_pipelines(reinterpret_cast<VkShaderModule>(vert_mod.id),
                               reinterpret_cast<VkShaderModule>(frag_mod.id));
    unload_shader(vert_mod);
    unload_shader(frag_mod);
    return ok;
}

void VulkanBackend::shutdown() {
    if (!initialized_) return;
    vkDeviceWaitIdle(device_);

    for (auto& s : frame_sync_) {
        vkDestroySemaphore(device_, s.image_available, nullptr);
        vkDestroyFence(device_, s.in_flight, nullptr);
    }

    // Warp mesh buffers
    if (warp_vb_map_)  vkUnmapMemory(device_, warp_vb_mem_);
    if (warp_vb_)      { vkDestroyBuffer(device_, warp_vb_, nullptr); vkFreeMemory(device_, warp_vb_mem_, nullptr); }
    if (warp_ib_)      { vkDestroyBuffer(device_, warp_ib_, nullptr); vkFreeMemory(device_, warp_ib_mem_, nullptr); }

    if (wave_vb_map_)  vkUnmapMemory(device_, wave_vb_mem_);
    if (wave_vb_)      { vkDestroyBuffer(device_, wave_vb_, nullptr); vkFreeMemory(device_, wave_vb_mem_, nullptr); }
    if (pipeline_waveform_) vkDestroyPipeline(device_, pipeline_waveform_, nullptr);

    // Preset warp pipeline
    if (warp_mode_)
        vkDestroyPipeline(device_, warp_pipeline_, nullptr);

    // Preset shared resources (descriptor layout, UBO, pool — exist if either mode was used)
    if (preset_desc_layout_ != VK_NULL_HANDLE) {
        if (preset_mode_)
            vkDestroyPipeline(device_, preset_pipeline_, nullptr);
        vkDestroyDescriptorPool(device_, preset_desc_pool_, nullptr);
        for (int i = 0; i < kMaxFramesInFlight; ++i) {
            if (preset_ubo_map_[i]) vkUnmapMemory(device_, preset_ubo_mem_[i]);
            vkDestroyBuffer(device_, preset_ubo_buf_[i], nullptr);
            vkFreeMemory(device_,   preset_ubo_mem_[i], nullptr);
        }
        vkDestroyPipelineLayout(device_, preset_pipe_layout_, nullptr);
        vkDestroyDescriptorSetLayout(device_, preset_desc_layout_, nullptr);
    }

    vkDestroyDescriptorPool(device_, desc_pool_, nullptr);

    for (int i = 0; i < kMaxFramesInFlight; ++i) {
        if (audio_ubo_map_[i]) vkUnmapMemory(device_, audio_ubo_mem_[i]);
        vkDestroyBuffer(device_, audio_ubo_buf_[i], nullptr);
        vkFreeMemory(device_,   audio_ubo_mem_[i], nullptr);
    }

    vkDestroyPipeline(device_, pipeline_composite_,  nullptr);
    vkDestroyPipeline(device_, pipeline_offscreen_,  nullptr);
    vkDestroyPipeline(device_, pipeline_seed_,       nullptr);
    vkDestroyPipelineLayout(device_, pipe_layout_,   nullptr);
    vkDestroyDescriptorSetLayout(device_, desc_layout_, nullptr);

    if (noise_image_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_,   noise_sampler_, nullptr);
        vkDestroyImageView(device_, noise_view_,    nullptr);
        vkDestroyImage(device_,     noise_image_,   nullptr);
        vkFreeMemory(device_,       noise_memory_,  nullptr);
    }

    spirv::shutdown();

    vkDestroyCommandPool(device_, cmd_pool_, nullptr);
    destroy_swapchain();

    for (auto& pp : ping_pong_) {
        vkDestroySampler(device_, pp.sampler, nullptr);
        vkDestroyFramebuffer(device_, pp.fb, nullptr);
        vkDestroyImageView(device_, pp.view, nullptr);
        vkDestroyImage(device_, pp.image, nullptr);
        vkFreeMemory(device_, pp.memory, nullptr);
    }

    vkDestroyRenderPass(device_, composite_pass_, nullptr);
    vkDestroyRenderPass(device_, offscreen_pass_, nullptr);
    vkDestroySurfaceKHR(instance_, surface_, nullptr);
    vkDestroyDevice(device_, nullptr);

    if (debug_messenger_ != VK_NULL_HANDLE) {
        auto fn = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
        if (fn) fn(instance_, debug_messenger_, nullptr);
    }

    vkDestroyInstance(instance_, nullptr);
    initialized_ = false;
}

void VulkanBackend::begin_frame() {
    // One-time status dump
    static bool s_printed = false;
    if (!s_printed) {
        s_printed = true;
        fprintf(stderr, "[vilk] pp=%ux%u  preset_mode=%d  warp_mode=%d  warp_vb=%s\n",
                pp_w_, pp_h_, (int)preset_mode_, (int)warp_mode_,
                warp_vb_ != VK_NULL_HANDLE ? "ok" : "NULL");
    }
    // Per-second audio dump (frame ~60 = 1 sec)
    static int s_frame = 0;
    if (++s_frame % 60 == 0)
        fprintf(stderr, "[vilk] bass=%.2f mid=%.2f treb=%.2f vol=%.2f t=%.1f\n",
                pending_audio_.bass, pending_audio_.mid,
                pending_audio_.treble, pending_audio_.vol, pending_audio_.time_s);

    auto& sync = frame_sync_[current_frame_];
    vkWaitForFences(device_, 1, &sync.in_flight, VK_TRUE, UINT64_MAX);

    VkResult r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                        sync.image_available, VK_NULL_HANDLE, &image_idx_);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        recreate_swapchain(width_, height_);
        return;
    }

    vkResetFences(device_, 1, &sync.in_flight);

    // Safe to write: fence guarantees GPU is done with this frame's UBO slot
    memcpy(audio_ubo_map_[current_frame_], &pending_audio_, sizeof(AudioUBO));
    if ((preset_mode_ || warp_mode_) && preset_ubo_map_[current_frame_])
        memcpy(preset_ubo_map_[current_frame_], &pending_preset_ubo_, sizeof(vilk::PresetUBO));

    vkResetCommandBuffer(cmd_bufs_[image_idx_], 0);

    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(cmd_bufs_[image_idx_], &bi);

    // Ping-pong offscreen pass
    VkClearValue clear_color = { .color = {{ 0.f, 0.f, 0.f, 1.f }} };
    VkRenderPassBeginInfo rp_bi{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rp_bi.renderPass        = offscreen_pass_;
    rp_bi.framebuffer       = ping_pong_[pp_write_].fb;
    rp_bi.renderArea.extent = { pp_w_, pp_h_ };  // must match ping-pong image size
    rp_bi.clearValueCount   = 1;
    rp_bi.pClearValues      = &clear_color;
    vkCmdBeginRenderPass(cmd_bufs_[image_idx_], &rp_bi, VK_SUBPASS_CONTENTS_INLINE);

    VkPipeline       warp_pipe = warp_mode_ ? warp_pipeline_                   : pipeline_offscreen_;
    VkPipelineLayout warp_lay  = warp_mode_ ? preset_pipe_layout_              : pipe_layout_;
    VkDescriptorSet  warp_ds   = warp_mode_ ? preset_desc_sets_[1 - pp_write_] : desc_sets_[1 - pp_write_];
    vkCmdBindPipeline(cmd_bufs_[image_idx_], VK_PIPELINE_BIND_POINT_GRAPHICS, warp_pipe);

    VkViewport vp{ 0.f, 0.f, static_cast<float>(pp_w_), static_cast<float>(pp_h_), 0.f, 1.f };
    VkRect2D scissor{ {0,0}, { pp_w_, pp_h_ } };
    vkCmdSetViewport(cmd_bufs_[image_idx_], 0, 1, &vp);
    vkCmdSetScissor (cmd_bufs_[image_idx_], 0, 1, &scissor);

    vkCmdBindDescriptorSets(cmd_bufs_[image_idx_], VK_PIPELINE_BIND_POINT_GRAPHICS,
                             warp_lay, 0, 1, &warp_ds, 0, nullptr);
    if (warp_mode_ && warp_vb_ != VK_NULL_HANDLE) {
        VkDeviceSize off = 0;
        vkCmdBindVertexBuffers(cmd_bufs_[image_idx_], 0, 1, &warp_vb_, &off);
        vkCmdBindIndexBuffer(cmd_bufs_[image_idx_], warp_ib_, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd_bufs_[image_idx_], warp_idx_count_, 1, 0, 0, 0);
    } else {
        vkCmdDraw(cmd_bufs_[image_idx_], 3, 1, 0, 0); // fullscreen triangle (no-warp fallback)
    }

    // Seed: alpha-blend (0.37±var, 0.37±var) into PP each frame.
    // Runs whenever any preset is active so PP stays centered at Kali Mix's neutral point.
    if (preset_mode_) {
        vkCmdBindPipeline(cmd_bufs_[image_idx_], VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_seed_);
        vkCmdBindDescriptorSets(cmd_bufs_[image_idx_], VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipe_layout_, 0, 1, &desc_sets_[1 - pp_write_], 0, nullptr);
        vkCmdDraw(cmd_bufs_[image_idx_], 3, 1, 0, 0);
    }

    // PCM waveform: draw after warp+seed so the line becomes part of the feedback.
    if (pipeline_waveform_ && wave_vb_ && wave_vertex_count_ > 0) {
        vkCmdBindPipeline(cmd_bufs_[image_idx_], VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_waveform_);
        vkCmdBindDescriptorSets(cmd_bufs_[image_idx_], VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipe_layout_, 0, 1, &desc_sets_[pp_write_], 0, nullptr);
        VkDeviceSize off = 0;
        vkCmdBindVertexBuffers(cmd_bufs_[image_idx_], 0, 1, &wave_vb_, &off);
        vkCmdDraw(cmd_bufs_[image_idx_], wave_vertex_count_, 1, 0, 0);
    }

    vkCmdEndRenderPass(cmd_bufs_[image_idx_]);
}

void VulkanBackend::end_frame() {
    // Composite pass -- blit ping-pong result to swapchain image
    VkClearValue clear_color = { .color = {{ 0.f, 0.f, 0.f, 1.f }} };
    VkRenderPassBeginInfo rp_bi{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rp_bi.renderPass        = composite_pass_;
    rp_bi.framebuffer       = sc_frames_[image_idx_].fb;
    rp_bi.renderArea.extent = sc_extent_;
    rp_bi.clearValueCount   = 1;
    rp_bi.pClearValues      = &clear_color;
    vkCmdBeginRenderPass(cmd_bufs_[image_idx_], &rp_bi, VK_SUBPASS_CONTENTS_INLINE);

    VkPipeline       composite_pipe = preset_mode_ ? preset_pipeline_            : pipeline_composite_;
    VkPipelineLayout composite_lay  = preset_mode_ ? preset_pipe_layout_         : pipe_layout_;
    VkDescriptorSet  composite_ds   = preset_mode_ ? preset_desc_sets_[pp_write_]: desc_sets_[pp_write_];
    vkCmdBindPipeline(cmd_bufs_[image_idx_],
                      VK_PIPELINE_BIND_POINT_GRAPHICS, composite_pipe);

    VkViewport vp{ 0.f, 0.f,
        static_cast<float>(sc_extent_.width), static_cast<float>(sc_extent_.height),
        0.f, 1.f };
    VkRect2D scissor{ {0,0}, sc_extent_ };
    vkCmdSetViewport(cmd_bufs_[image_idx_], 0, 1, &vp);
    vkCmdSetScissor (cmd_bufs_[image_idx_], 0, 1, &scissor);

    // Sample what we just wrote (ping_pong_[pp_write_]) onto the swapchain image
    vkCmdBindDescriptorSets(cmd_bufs_[image_idx_], VK_PIPELINE_BIND_POINT_GRAPHICS,
                             composite_lay, 0, 1, &composite_ds, 0, nullptr);
    vkCmdDraw(cmd_bufs_[image_idx_], 3, 1, 0, 0);

    vkCmdEndRenderPass(cmd_bufs_[image_idx_]);

    vkEndCommandBuffer(cmd_bufs_[image_idx_]);

    auto& sync = frame_sync_[current_frame_];
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.waitSemaphoreCount   = 1;
    si.pWaitSemaphores      = &sync.image_available;
    si.pWaitDstStageMask    = &wait_stage;
    si.commandBufferCount   = 1;
    si.pCommandBuffers      = &cmd_bufs_[image_idx_];
    VkSemaphore render_done = sc_frames_[image_idx_].render_finished;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores    = &render_done;
    vkQueueSubmit(graphics_queue_, 1, &si, sync.in_flight);

    VkPresentInfoKHR pi{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores    = &render_done;
    pi.swapchainCount     = 1;
    pi.pSwapchains        = &swapchain_;
    pi.pImageIndices      = &image_idx_;
    VkResult r = vkQueuePresentKHR(present_queue_, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
        recreate_swapchain(width_, height_);

    pp_write_     = 1 - pp_write_;
    current_frame_ = (current_frame_ + 1) % kMaxFramesInFlight;
}

void VulkanBackend::resize(uint32_t w, uint32_t h) {
    width_ = w; height_ = h;
    recreate_swapchain(w, h);
}

ShaderHandle VulkanBackend::load_spirv(const uint32_t* words, size_t word_count,
                                       std::string_view) {
    VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = word_count * sizeof(uint32_t);
    ci.pCode    = words;
    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device_, &ci, nullptr, &mod) != VK_SUCCESS)
        return {};
    return ShaderHandle{ reinterpret_cast<uint64_t>(mod) };
}

void VulkanBackend::unload_shader(ShaderHandle handle) {
    if (!handle.valid()) return;
    vkDestroyShaderModule(device_,
        reinterpret_cast<VkShaderModule>(handle.id), nullptr);
}

// ---------------------------------------------------------------------------
// Instance
// ---------------------------------------------------------------------------
bool VulkanBackend::create_instance() {
    VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName   = "vilk";
    app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app.apiVersion         = VK_API_VERSION_1_2;

    uint32_t glfw_ext_count = 0;
    const char** glfw_exts  = glfwGetRequiredInstanceExtensions(&glfw_ext_count);
    std::vector<const char*> exts(glfw_exts, glfw_exts + glfw_ext_count);

    std::vector<const char*> layers;

#ifdef VILK_ENABLE_VALIDATION
    exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    layers.push_back("VK_LAYER_KHRONOS_validation");
#endif

    VkInstanceCreateInfo ci{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ci.pApplicationInfo        = &app;
    ci.enabledExtensionCount   = static_cast<uint32_t>(exts.size());
    ci.ppEnabledExtensionNames = exts.data();
    ci.enabledLayerCount       = static_cast<uint32_t>(layers.size());
    ci.ppEnabledLayerNames     = layers.data();

    if (vkCreateInstance(&ci, nullptr, &instance_) != VK_SUCCESS) {
        fprintf(stderr, "[vilk] vkCreateInstance failed\n");
        return false;
    }
    return true;
}

bool VulkanBackend::setup_debug_messenger() {
#ifndef VILK_ENABLE_VALIDATION
    return true;
#else
    auto fn = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
    if (!fn) return true; // validation not available, non-fatal

    VkDebugUtilsMessengerCreateInfoEXT ci{
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
    ci.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    ci.messageType =
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    ci.pfnUserCallback = vk_debug_cb;

    fn(instance_, &ci, nullptr, &debug_messenger_);
    return true;
#endif
}

// ---------------------------------------------------------------------------
// Surface
// ---------------------------------------------------------------------------
bool VulkanBackend::create_surface(GLFWwindow* window) {
    if (glfwCreateWindowSurface(instance_, window, nullptr, &surface_) != VK_SUCCESS) {
        fprintf(stderr, "[vilk] glfwCreateWindowSurface failed\n");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Physical device
// ---------------------------------------------------------------------------
bool VulkanBackend::pick_physical_device() {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    if (count == 0) { fprintf(stderr, "[vilk] no Vulkan GPU found\n"); return false; }

    std::vector<VkPhysicalDevice> gpus(count);
    vkEnumeratePhysicalDevices(instance_, &count, gpus.data());

    for (auto candidate : gpus) {
        // Find graphics queue family that supports present
        uint32_t qcount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &qcount, nullptr);
        std::vector<VkQueueFamilyProperties> qfams(qcount);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &qcount, qfams.data());

        uint32_t gfx = UINT32_MAX, prs = UINT32_MAX;
        for (uint32_t i = 0; i < qcount; ++i) {
            if (qfams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) gfx = i;
            VkBool32 present_ok = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface_, &present_ok);
            if (present_ok) prs = i;
            if (gfx != UINT32_MAX && prs != UINT32_MAX) break;
        }
        if (gfx == UINT32_MAX || prs == UINT32_MAX) continue;

        // Require swapchain extension
        uint32_t ext_count = 0;
        vkEnumerateDeviceExtensionProperties(candidate, nullptr, &ext_count, nullptr);
        std::vector<VkExtensionProperties> ext_props(ext_count);
        vkEnumerateDeviceExtensionProperties(candidate, nullptr, &ext_count, ext_props.data());
        bool has_swapchain = false;
        for (auto& e : ext_props)
            if (strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0)
                { has_swapchain = true; break; }
        if (!has_swapchain) continue;

        gpu_             = candidate;
        graphics_family_ = gfx;
        present_family_  = prs;
        return true;
    }

    fprintf(stderr, "[vilk] no suitable GPU found\n");
    return false;
}

// ---------------------------------------------------------------------------
// Logical device
// ---------------------------------------------------------------------------
bool VulkanBackend::create_device() {
    float prio = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queue_cis;
    for (uint32_t fam : { graphics_family_, present_family_ }) {
        bool already = false;
        for (auto& q : queue_cis) if (q.queueFamilyIndex == fam) { already = true; break; }
        if (already) continue;
        VkDeviceQueueCreateInfo qi{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        qi.queueFamilyIndex = fam;
        qi.queueCount       = 1;
        qi.pQueuePriorities = &prio;
        queue_cis.push_back(qi);
    }

    const char* dev_exts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkPhysicalDeviceFeatures feats{};

    VkDeviceCreateInfo ci{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    ci.queueCreateInfoCount    = static_cast<uint32_t>(queue_cis.size());
    ci.pQueueCreateInfos       = queue_cis.data();
    ci.enabledExtensionCount   = 1;
    ci.ppEnabledExtensionNames = dev_exts;
    ci.pEnabledFeatures        = &feats;

    if (vkCreateDevice(gpu_, &ci, nullptr, &device_) != VK_SUCCESS) {
        fprintf(stderr, "[vilk] vkCreateDevice failed\n");
        return false;
    }

    vkGetDeviceQueue(device_, graphics_family_, 0, &graphics_queue_);
    vkGetDeviceQueue(device_, present_family_,  0, &present_queue_);
    return true;
}

// ---------------------------------------------------------------------------
// Swapchain
// ---------------------------------------------------------------------------
bool VulkanBackend::create_swapchain(uint32_t w, uint32_t h) {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu_, surface_, &caps);

    // Format
    uint32_t fmt_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &fmt_count, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(fmt_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &fmt_count, fmts.data());

    VkSurfaceFormatKHR chosen = fmts[0];
    for (auto& f : fmts)
        if (f.format == VK_FORMAT_B8G8R8A8_SRGB &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            { chosen = f; break; }

    // Present mode
    uint32_t pm_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(gpu_, surface_, &pm_count, nullptr);
    std::vector<VkPresentModeKHR> pms(pm_count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(gpu_, surface_, &pm_count, pms.data());

    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    for (auto pm : pms)
        if (pm == VK_PRESENT_MODE_MAILBOX_KHR) { present_mode = pm; break; }

    // Extent
    VkExtent2D extent;
    if (caps.currentExtent.width != UINT32_MAX) {
        extent = caps.currentExtent;
    } else {
        extent.width  = std::clamp(w, caps.minImageExtent.width,  caps.maxImageExtent.width);
        extent.height = std::clamp(h, caps.minImageExtent.height, caps.maxImageExtent.height);
    }

    uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0) image_count = std::min(image_count, caps.maxImageCount);

    VkSwapchainCreateInfoKHR ci{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    ci.surface          = surface_;
    ci.minImageCount    = image_count;
    ci.imageFormat      = chosen.format;
    ci.imageColorSpace  = chosen.colorSpace;
    ci.imageExtent      = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.preTransform     = caps.currentTransform;
    ci.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode      = present_mode;
    ci.clipped          = VK_TRUE;

    uint32_t indices[] = { graphics_family_, present_family_ };
    if (graphics_family_ != present_family_) {
        ci.imageSharingMode      = VK_SHARING_MODE_CONCURRENT;
        ci.queueFamilyIndexCount = 2;
        ci.pQueueFamilyIndices   = indices;
    } else {
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    if (vkCreateSwapchainKHR(device_, &ci, nullptr, &swapchain_) != VK_SUCCESS) {
        fprintf(stderr, "[vilk] vkCreateSwapchainKHR failed\n");
        return false;
    }

    sc_format_ = chosen.format;
    sc_extent_ = extent;

    uint32_t actual_count = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &actual_count, nullptr);
    std::vector<VkImage> images(actual_count);
    vkGetSwapchainImagesKHR(device_, swapchain_, &actual_count, images.data());

    VkSemaphoreCreateInfo sem_ci{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    sc_frames_.resize(actual_count);
    for (uint32_t i = 0; i < actual_count; ++i) {
        sc_frames_[i].image = images[i];
        VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image    = images[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format   = sc_format_;
        vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCreateImageView(device_, &vci, nullptr, &sc_frames_[i].view);
        vkCreateSemaphore(device_, &sem_ci, nullptr, &sc_frames_[i].render_finished);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Render passes
// ---------------------------------------------------------------------------
bool VulkanBackend::create_render_passes() {
    // Offscreen ping-pong pass (R8G8B8A8_UNORM — auto-clamps to [0,1] on write)
    {
        VkAttachmentDescription att{};
        att.format         = VK_FORMAT_R8G8B8A8_UNORM;
        att.samples        = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        att.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        att.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkAttachmentReference ref{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sub{};
        sub.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments    = &ref;

        VkSubpassDependency deps[2]{};
        // External → subpass: wait for previous sample read before writing
        deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass    = 0;
        deps[0].srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        // Subpass → external: composite pass can safely sample after write completes
        deps[1].srcSubpass    = 0;
        deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        VkRenderPassCreateInfo ci{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        ci.attachmentCount = 1; ci.pAttachments  = &att;
        ci.subpassCount    = 1; ci.pSubpasses    = &sub;
        ci.dependencyCount = 2; ci.pDependencies = deps;
        if (vkCreateRenderPass(device_, &ci, nullptr, &offscreen_pass_) != VK_SUCCESS)
            return false;
    }

    // Composite pass (swapchain format)
    {
        VkAttachmentDescription att{};
        att.format         = sc_format_;
        att.samples        = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        att.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        att.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference ref{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sub{};
        sub.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments    = &ref;

        VkSubpassDependency dep{};
        dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass    = 0;
        dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.srcAccessMask = 0;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo ci{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        ci.attachmentCount = 1; ci.pAttachments = &att;
        ci.subpassCount    = 1; ci.pSubpasses   = &sub;
        ci.dependencyCount = 1; ci.pDependencies = &dep;
        if (vkCreateRenderPass(device_, &ci, nullptr, &composite_pass_) != VK_SUCCESS)
            return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Ping-pong targets
// ---------------------------------------------------------------------------
uint32_t VulkanBackend::find_memory_type(uint32_t type_filter,
                                          VkMemoryPropertyFlags props) const {
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(gpu_, &mem_props);
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i)
        if ((type_filter & (1u << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & props) == props)
            return i;
    return UINT32_MAX;
}

bool VulkanBackend::alloc_image(uint32_t w, uint32_t h, VkFormat fmt,
                                 VkImageUsageFlags usage, PingPongTarget& out) {
    VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType   = VK_IMAGE_TYPE_2D;
    ici.format      = fmt;
    ici.extent      = { w, h, 1 };
    ici.mipLevels   = 1;
    ici.arrayLayers = 1;
    ici.samples     = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling      = VK_IMAGE_TILING_OPTIMAL;
    ici.usage       = usage;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device_, &ici, nullptr, &out.image) != VK_SUCCESS) return false;

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device_, out.image, &req);
    VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize  = req.size;
    ai.memoryTypeIndex = find_memory_type(req.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(device_, &ai, nullptr, &out.memory) != VK_SUCCESS) return false;
    vkBindImageMemory(device_, out.image, out.memory, 0);

    VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vci.image    = out.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format   = fmt;
    vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (vkCreateImageView(device_, &vci, nullptr, &out.view) != VK_SUCCESS) return false;

    VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter  = VK_FILTER_LINEAR;
    sci.minFilter  = VK_FILTER_LINEAR;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(device_, &sci, nullptr, &out.sampler) != VK_SUCCESS) return false;

    return true;
}

bool VulkanBackend::create_ping_pong_targets(uint32_t w, uint32_t h) {
    constexpr VkImageUsageFlags usage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT;  // needed for vkCmdClearColorImage to neutral point
    for (auto& pp : ping_pong_)
        if (!alloc_image(w, h, VK_FORMAT_R8G8B8A8_UNORM, usage, pp))
            return false;
    pp_w_ = w; pp_h_ = h;  // record image dimensions — framebuffers must never exceed these
    return true;
}

// ---------------------------------------------------------------------------
// Noise texture (256×256 smooth value noise, binding 2 in preset desc set)
// ---------------------------------------------------------------------------
bool VulkanBackend::create_noise_texture() {
    constexpr uint32_t kW = 256, kH = 256;

    // Generate smooth value noise: 16×16 random grid bilinearly interpolated.
    std::vector<uint8_t> pixels(kW * kH * 4);
    {
        std::mt19937 rng(0xDEADBEEF);
        std::uniform_int_distribution<int> d(0, 255);
        constexpr int CELL = 16;
        constexpr int GW = kW / CELL + 2;
        constexpr int GH = kH / CELL + 2;
        std::vector<uint8_t> grid(GW * GH * 4);
        for (auto& v : grid) v = static_cast<uint8_t>(d(rng));

        for (uint32_t py = 0; py < kH; ++py) {
            for (uint32_t px = 0; px < kW; ++px) {
                float fx = float(px) / CELL, fy = float(py) / CELL;
                int gx = int(fx), gy = int(fy);
                float tx = fx - gx, ty = fy - gy;
                // Smoothstep
                tx = tx * tx * (3.f - 2.f * tx);
                ty = ty * ty * (3.f - 2.f * ty);
                for (int c = 0; c < 4; ++c) {
                    float v00 = grid[(gy * GW + gx)       * 4 + c];
                    float v10 = grid[(gy * GW + gx + 1)   * 4 + c];
                    float v01 = grid[((gy+1) * GW + gx)   * 4 + c];
                    float v11 = grid[((gy+1) * GW + gx+1) * 4 + c];
                    float v   = v00*(1-tx)*(1-ty) + v10*tx*(1-ty)
                              + v01*(1-tx)*ty      + v11*tx*ty;
                    pixels[(py * kW + px) * 4 + c] = static_cast<uint8_t>(v + 0.5f);
                }
            }
        }
    }

    // Create device-local image
    VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType     = VK_IMAGE_TYPE_2D;
    ici.format        = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent        = { kW, kH, 1 };
    ici.mipLevels     = 1;
    ici.arrayLayers   = 1;
    ici.samples       = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ici.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device_, &ici, nullptr, &noise_image_) != VK_SUCCESS) return false;

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device_, noise_image_, &req);
    VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize  = req.size;
    mai.memoryTypeIndex = find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(device_, &mai, nullptr, &noise_memory_) != VK_SUCCESS) return false;
    vkBindImageMemory(device_, noise_image_, noise_memory_, 0);

    // Staging buffer upload
    VkDeviceSize data_size = kW * kH * 4;
    VkBuffer     stg_buf   = VK_NULL_HANDLE;
    VkDeviceMemory stg_mem = VK_NULL_HANDLE;
    {
        VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size        = data_size;
        bci.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device_, &bci, nullptr, &stg_buf) != VK_SUCCESS) return false;

        VkMemoryRequirements sreq{};
        vkGetBufferMemoryRequirements(device_, stg_buf, &sreq);
        VkMemoryAllocateInfo sai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        sai.allocationSize  = sreq.size;
        sai.memoryTypeIndex = find_memory_type(sreq.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(device_, &sai, nullptr, &stg_mem) != VK_SUCCESS) {
            vkDestroyBuffer(device_, stg_buf, nullptr);
            return false;
        }
        vkBindBufferMemory(device_, stg_buf, stg_mem, 0);
        void* mapped = nullptr;
        vkMapMemory(device_, stg_mem, 0, data_size, 0, &mapped);
        memcpy(mapped, pixels.data(), pixels.size());
        vkUnmapMemory(device_, stg_mem);
    }

    // One-time command: UNDEFINED→TRANSFER_DST, copy, TRANSFER_DST→SHADER_READ
    VkCommandBufferAllocateInfo cbai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbai.commandPool        = cmd_pool_;
    cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device_, &cbai, &cmd);
    VkCommandBufferBeginInfo cbbi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &cbbi);

    auto barrier = [&](VkImageLayout from, VkImageLayout to,
                       VkAccessFlags src, VkAccessFlags dst,
                       VkPipelineStageFlags sp, VkPipelineStageFlags dp) {
        VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.oldLayout           = from;
        b.newLayout           = to;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image               = noise_image_;
        b.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        b.srcAccessMask       = src;
        b.dstAccessMask       = dst;
        vkCmdPipelineBarrier(cmd, sp, dp, 0, 0, nullptr, 0, nullptr, 1, &b);
    };

    barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent                 = { kW, kH, 1 };
    vkCmdCopyBufferToImage(cmd, stg_buf, noise_image_,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &cmd;
    vkQueueSubmit(graphics_queue_, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(graphics_queue_);
    vkFreeCommandBuffers(device_, cmd_pool_, 1, &cmd);

    vkDestroyBuffer(device_, stg_buf, nullptr);
    vkFreeMemory(device_, stg_mem, nullptr);

    // Image view + repeat sampler
    VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vci.image            = noise_image_;
    vci.viewType         = VK_IMAGE_VIEW_TYPE_2D;
    vci.format           = VK_FORMAT_R8G8B8A8_UNORM;
    vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (vkCreateImageView(device_, &vci, nullptr, &noise_view_) != VK_SUCCESS) return false;

    VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter    = VK_FILTER_LINEAR;
    sci.minFilter    = VK_FILTER_LINEAR;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    if (vkCreateSampler(device_, &sci, nullptr, &noise_sampler_) != VK_SUCCESS) return false;

    fprintf(stderr, "[vilk] noise texture created (%ux%u smooth)\n", kW, kH);
    return true;
}

// ---------------------------------------------------------------------------
// Warp mesh: 64×64 grid, HOST_VISIBLE vertex + index buffers.
// Per-frame the CPU writes per-pixel-equation-rotated UVs into the VB.
// ---------------------------------------------------------------------------
bool VulkanBackend::create_warp_mesh_buffers() {
    const int N  = kWarpGridN;
    const int NV = (N + 1) * (N + 1);
    const int NI = N * N * 6;

    auto make_buf = [&](VkDeviceSize sz, VkBufferUsageFlags usage,
                        VkBuffer& buf, VkDeviceMemory& mem, void** map_out) -> bool {
        VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size  = sz;
        bci.usage = usage;
        if (vkCreateBuffer(device_, &bci, nullptr, &buf) != VK_SUCCESS) return false;
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(device_, buf, &mr);
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize  = mr.size;
        mai.memoryTypeIndex = find_memory_type(mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(device_, &mai, nullptr, &mem) != VK_SUCCESS) return false;
        vkBindBufferMemory(device_, buf, mem, 0);
        if (map_out) vkMapMemory(device_, mem, 0, sz, 0, map_out);
        return true;
    };

    if (!make_buf(NV * sizeof(WarpVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                  warp_vb_, warp_vb_mem_, &warp_vb_map_)) return false;

    if (!make_buf(NI * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                  warp_ib_, warp_ib_mem_, nullptr)) return false;

    // Write static index buffer (triangle list, wound CCW)
    uint32_t* idx = nullptr;
    vkMapMemory(device_, warp_ib_mem_, 0, NI * sizeof(uint32_t), 0,
                reinterpret_cast<void**>(&idx));
    int k = 0;
    for (int gy = 0; gy < N; ++gy) {
        for (int gx = 0; gx < N; ++gx) {
            uint32_t tl = gy * (N + 1) + gx;
            idx[k++] = tl;       idx[k++] = tl + 1;       idx[k++] = tl + N + 1;
            idx[k++] = tl + 1;   idx[k++] = tl + N + 2;   idx[k++] = tl + N + 1;
        }
    }
    vkUnmapMemory(device_, warp_ib_mem_);
    warp_idx_count_ = NI;

    // Write initial identity UV (will be updated per-frame)
    update_warp_mesh(0.f);
    return true;
}

void VulkanBackend::update_warp_mesh(float t) {
    if (!warp_vb_map_) return;
    const int N = kWarpGridN;
    auto* verts = static_cast<WarpVertex*>(warp_vb_map_);
    // MilkDrop warp=0.01010 noise: sinusoidal UV perturbation per vertex.
    // Without this, the warp's diagonal blurry sample (+1px,+1px) creates a uniform
    // drift direction → diagonal banding in PP → diagonal stripes on screen.
    // This noise makes adjacent vertices sample from different directions → turbulent flow.
    const float warp_amt = 0.0101f;
    const float wt = t * 0.5f;
    for (int gy = 0; gy <= N; ++gy) {
        for (int gx = 0; gx <= N; ++gx) {
            float u  = gx / float(N);
            float v  = gy / float(N);
            float cx = u - 0.5f, cy = v - 0.5f;
            float rad = sqrtf(cx * cx + cy * cy);
            float ang = atan2f(cy, cx);
            // Kali Mix per_pixel_2: rot = rot + 0.5*sin(0.5-rad)*cos(0.02*(0.5-rad)+time)
            float rot = 0.5f * sinf(0.5f - rad) * cosf(0.02f * (0.5f - rad) + t);
            ang += rot;
            float wu = 0.5f + rad * cosf(ang);
            float wv = 0.5f + rad * sinf(ang);
            // MilkDrop warp noise (warp=0.01010 parameter).
            float sx = u * 6.28f * 0.36f, sy = v * 6.28f * 0.36f;
            wu += warp_amt * (sinf(wt*0.9f + sx*2.f + wt*0.3f) * cosf(wt*0.6f - sy*2.f)
                            + 0.5f * cosf(wt*1.2f + sx*3.f));
            wv += warp_amt * (cosf(wt*0.7f + sx*2.f) * sinf(wt*0.4f + sy*2.f)
                            + 0.5f * sinf(wt*1.1f - sy*3.f));
            verts[gy * (N + 1) + gx] = { { u * 2.f - 1.f, v * 2.f - 1.f }, { wu, wv } };
        }
    }
}

// ---------------------------------------------------------------------------
// Waveform vertex buffer (HOST_VISIBLE, kWaveformSize * vec2, updated per-frame).
// ---------------------------------------------------------------------------
bool VulkanBackend::create_waveform_buffers() {
    VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size        = kWaveformSize * sizeof(float) * 2;
    bci.usage       = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device_, &bci, nullptr, &wave_vb_) != VK_SUCCESS) return false;

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device_, wave_vb_, &req);
    VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize  = req.size;
    ai.memoryTypeIndex = find_memory_type(req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(device_, &ai, nullptr, &wave_vb_mem_) != VK_SUCCESS) return false;
    vkBindBufferMemory(device_, wave_vb_, wave_vb_mem_, 0);
    vkMapMemory(device_, wave_vb_mem_, 0, bci.size, 0, &wave_vb_map_);
    wave_vertex_count_ = kWaveformSize;
    return true;
}

void VulkanBackend::update_waveform(const WaveformSnapshot& wave) {
    if (!wave_vb_map_) return;
    auto* verts = static_cast<float*>(wave_vb_map_);
    for (int i = 0; i < kWaveformSize; ++i) {
        verts[i * 2 + 0] = (i / float(kWaveformSize - 1)) * 2.f - 1.f;  // x: NDC -1 → +1
        verts[i * 2 + 1] = wave.samples[i] * 0.35f;                       // y: ±35% of PP height
    }
}

// ---------------------------------------------------------------------------
// Framebuffers
// ---------------------------------------------------------------------------
bool VulkanBackend::create_framebuffers() {
    // Swapchain framebuffers (composite pass)
    for (auto& frame : sc_frames_) {
        VkFramebufferCreateInfo fi{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fi.renderPass      = composite_pass_;
        fi.attachmentCount = 1;
        fi.pAttachments    = &frame.view;
        fi.width           = sc_extent_.width;
        fi.height          = sc_extent_.height;
        fi.layers          = 1;
        if (vkCreateFramebuffer(device_, &fi, nullptr, &frame.fb) != VK_SUCCESS)
            return false;
    }

    // Ping-pong framebuffers (offscreen pass)
    for (auto& pp : ping_pong_) {
        VkFramebufferCreateInfo fi{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fi.renderPass      = offscreen_pass_;
        fi.attachmentCount = 1;
        fi.pAttachments    = &pp.view;
        fi.width           = sc_extent_.width;
        fi.height          = sc_extent_.height;
        fi.layers          = 1;
        if (vkCreateFramebuffer(device_, &fi, nullptr, &pp.fb) != VK_SUCCESS)
            return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Command pool + buffers
// ---------------------------------------------------------------------------
bool VulkanBackend::create_command_pool_and_buffers() {
    VkCommandPoolCreateInfo ci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    ci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    ci.queueFamilyIndex = graphics_family_;
    if (vkCreateCommandPool(device_, &ci, nullptr, &cmd_pool_) != VK_SUCCESS) return false;

    cmd_bufs_.resize(sc_frames_.size());
    VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool        = cmd_pool_;
    ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = static_cast<uint32_t>(cmd_bufs_.size());
    return vkAllocateCommandBuffers(device_, &ai, cmd_bufs_.data()) == VK_SUCCESS;
}

// ---------------------------------------------------------------------------
// Sync objects
// ---------------------------------------------------------------------------
bool VulkanBackend::create_sync_objects() {
    VkSemaphoreCreateInfo si{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo fi{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (auto& s : frame_sync_) {
        if (vkCreateSemaphore(device_, &si, nullptr, &s.image_available) != VK_SUCCESS) return false;
        if (vkCreateFence(device_, &fi, nullptr, &s.in_flight) != VK_SUCCESS) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Audio UBO (host-visible, persistently mapped, one per frame-in-flight)
// ---------------------------------------------------------------------------
bool VulkanBackend::create_audio_ubo() {
    VkPhysicalDeviceMemoryProperties mem_props{};
    vkGetPhysicalDeviceMemoryProperties(gpu_, &mem_props);

    constexpr VkDeviceSize size = sizeof(AudioUBO);
    constexpr VkMemoryPropertyFlags flags =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    for (int i = 0; i < kMaxFramesInFlight; ++i) {
        VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bi.size        = size;
        bi.usage       = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device_, &bi, nullptr, &audio_ubo_buf_[i]) != VK_SUCCESS)
            return false;

        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device_, audio_ubo_buf_[i], &req);

        uint32_t mem_type = find_memory_type(req.memoryTypeBits, flags);
        if (mem_type == UINT32_MAX) return false;

        VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        ai.allocationSize  = req.size;
        ai.memoryTypeIndex = mem_type;
        if (vkAllocateMemory(device_, &ai, nullptr, &audio_ubo_mem_[i]) != VK_SUCCESS)
            return false;

        vkBindBufferMemory(device_, audio_ubo_buf_[i], audio_ubo_mem_[i], 0);
        vkMapMemory(device_, audio_ubo_mem_[i], 0, size, 0, &audio_ubo_map_[i]);

        // Zero-initialize so shader has valid data before first audio arrives
        memset(audio_ubo_map_[i], 0, size);
    }
    return true;
}

void VulkanBackend::update_audio(const AudioSnapshot& snap) {
    pending_audio_.bass   = snap.bass;
    pending_audio_.mid    = snap.mid;
    pending_audio_.treble = snap.treble;
    pending_audio_.vol    = snap.vol;
    pending_audio_.time_s = snap.time_s;

    if (preset_mode_ || warp_mode_) {
        float t = snap.time_s;
        if (warp_mode_) update_warp_mesh(t);
        // _c2 = {time, fps, frame, progress}
        pending_preset_ubo_.c[2] = { t, 60.f, t * 60.f, 0.f };
        // _c3 = {bass, mid, treb, vol}
        pending_preset_ubo_.c[3] = { snap.bass, snap.mid, snap.treble, snap.vol };
        // _c4 = {bassAtt, midAtt, trebAtt, volAtt} -- use same values for now
        pending_preset_ubo_.c[4] = { snap.bass, snap.mid, snap.treble, snap.vol };
        // _c5/_c6 = blur scale/offset; alias blur1/2/3 → noise texture, so scale=1 offset=0
        pending_preset_ubo_.c[5] = { 1.f, 0.f, 0.f, 0.f };
        pending_preset_ubo_.c[6] = { 1.f, 0.f, 0.f, 0.f };
        // _c7 = {vpW, vpH, 1/vpW, 1/vpH}
        float w = static_cast<float>(width_), h = static_cast<float>(height_);
        pending_preset_ubo_.c[7] = { w, h, 1.f / w, 1.f / h };
        // _c8.._c11 = trig-of-time combos (roam_cos/sin, slow_roam_cos/sin)
        pending_preset_ubo_.c[8]  = { std::cos(t),      std::sin(t),      std::cos(t*1.5f+3.f), std::sin(t*1.5f+3.f) };
        pending_preset_ubo_.c[9]  = { std::cos(t*2.f),  std::sin(t*2.f),  std::cos(t*3.f),       std::sin(t*3.f)      };
        pending_preset_ubo_.c[10] = { std::cos(t*4.f),  std::sin(t*4.f),  std::cos(t*5.f),       std::sin(t*5.f)      };
        pending_preset_ubo_.c[11] = { std::cos(t/2.f),  std::sin(t/2.f),  std::cos(t/3.f),       std::sin(t/3.f)      };
    }
}

bool VulkanBackend::reload_preset_composite(std::string_view vert_glsl, std::string_view frag_glsl) {
    auto vert_spv = spirv::compile(vert_glsl, VK_SHADER_STAGE_VERTEX_BIT,   "preset.vert");
    auto frag_spv = spirv::compile(frag_glsl, VK_SHADER_STAGE_FRAGMENT_BIT, "preset.frag");
    if (vert_spv.empty() || frag_spv.empty()) {
        fprintf(stderr, "[vilk] preset shader compile failed\n");
        return false;
    }
    auto vert_mod = load_spirv(vert_spv.data(), vert_spv.size(), "preset.vert");
    auto frag_mod = load_spirv(frag_spv.data(), frag_spv.size(), "preset.frag");
    if (!vert_mod.valid() || !frag_mod.valid()) {
        if (vert_mod.valid()) unload_shader(vert_mod);
        if (frag_mod.valid()) unload_shader(frag_mod);
        return false;
    }

    // Initialize preset descriptor layout and UBO on first call.
    bool first_call = (preset_desc_layout_ == VK_NULL_HANDLE);
    if (first_call) {
        if (!create_preset_descriptor_layout()) { unload_shader(vert_mod); unload_shader(frag_mod); return false; }
        if (!create_preset_ubo())               { unload_shader(vert_mod); unload_shader(frag_mod); return false; }
        if (!create_preset_descriptor_sets())   { unload_shader(vert_mod); unload_shader(frag_mod); return false; }
        update_preset_descriptor_sets();
    }

    vkDeviceWaitIdle(device_);
    if (preset_pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, preset_pipeline_, nullptr);
        preset_pipeline_ = VK_NULL_HANDLE;
    }

    bool ok = create_preset_pipeline(reinterpret_cast<VkShaderModule>(vert_mod.id),
                                     reinterpret_cast<VkShaderModule>(frag_mod.id));
    unload_shader(vert_mod);
    unload_shader(frag_mod);
    if (ok) {
        preset_mode_ = true;
        fprintf(stderr, "[vilk] preset composite pipeline loaded\n");
    }
    return ok;
}

bool VulkanBackend::reload_preset_warp(std::string_view /*vert_glsl*/, std::string_view frag_glsl) {
    // Ignore the adapter vert — warp uses kWarpMeshVertGlsl with per-pixel UV attributes.
    auto vert_spv = spirv::compile(kWarpMeshVertGlsl, VK_SHADER_STAGE_VERTEX_BIT, "warp_mesh.vert");
    auto frag_spv = spirv::compile(frag_glsl, VK_SHADER_STAGE_FRAGMENT_BIT, "preset_warp.frag");
    if (vert_spv.empty() || frag_spv.empty()) {
        fprintf(stderr, "[vilk] preset warp shader compile failed\n");
        return false;
    }
    auto vert_mod = load_spirv(vert_spv.data(), vert_spv.size(), "preset_warp.vert");
    auto frag_mod = load_spirv(frag_spv.data(), frag_spv.size(), "preset_warp.frag");
    if (!vert_mod.valid() || !frag_mod.valid()) {
        if (vert_mod.valid()) unload_shader(vert_mod);
        if (frag_mod.valid()) unload_shader(frag_mod);
        return false;
    }

    // Shared preset resources (descriptor layout, UBO, descriptor sets) — lazy init.
    bool first_call = (preset_desc_layout_ == VK_NULL_HANDLE);
    if (first_call) {
        if (!create_preset_descriptor_layout()) { unload_shader(vert_mod); unload_shader(frag_mod); return false; }
        if (!create_preset_ubo())               { unload_shader(vert_mod); unload_shader(frag_mod); return false; }
        if (!create_preset_descriptor_sets())   { unload_shader(vert_mod); unload_shader(frag_mod); return false; }
        update_preset_descriptor_sets();
    }

    vkDeviceWaitIdle(device_);
    if (warp_pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, warp_pipeline_, nullptr);
        warp_pipeline_ = VK_NULL_HANDLE;
    }

    bool ok = create_warp_pipeline(reinterpret_cast<VkShaderModule>(vert_mod.id),
                                   reinterpret_cast<VkShaderModule>(frag_mod.id));
    unload_shader(vert_mod);
    unload_shader(frag_mod);
    if (ok) {
        // Clear both PP textures to the warp neutral point (0.37) before enabling warp.
        // Without this, the startup transient (PP≈0 → shift = -0.37*delta uniform) drives
        // the system into the stripe attractor before the seed can establish varied content.
        {
            VkCommandBuffer cmd;
            VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            ai.commandPool        = cmd_pool_;
            ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            ai.commandBufferCount = 1;
            vkAllocateCommandBuffers(device_, &ai, &cmd);
            VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(cmd, &bi);

            VkImageSubresourceRange range{};
            range.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
            range.levelCount     = 1;
            range.layerCount     = 1;

            VkClearColorValue cv{};
            cv.float32[0] = 0.37f; cv.float32[1] = 0.37f;
            cv.float32[2] = 0.1f;  cv.float32[3] = 0.0f;

            // Transition both PP images to TRANSFER_DST, clear, then back to SHADER_READ.
            for (int i = 0; i < 2; ++i) {
                VkImageMemoryBarrier bar{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
                bar.srcAccessMask       = VK_ACCESS_SHADER_READ_BIT;
                bar.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
                bar.oldLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                bar.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                bar.image               = ping_pong_[i].image;
                bar.subresourceRange    = range;
                vkCmdPipelineBarrier(cmd,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    0, 0, nullptr, 0, nullptr, 1, &bar);

                vkCmdClearColorImage(cmd, ping_pong_[i].image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &range);

                bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                bar.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                bar.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                vkCmdPipelineBarrier(cmd,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                    0, 0, nullptr, 0, nullptr, 1, &bar);
            }

            vkEndCommandBuffer(cmd);
            VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
            si.commandBufferCount = 1;
            si.pCommandBuffers    = &cmd;
            vkQueueSubmit(graphics_queue_, 1, &si, VK_NULL_HANDLE);
            vkQueueWaitIdle(graphics_queue_);
            vkFreeCommandBuffers(device_, cmd_pool_, 1, &cmd);
        }
        warp_mode_ = true;
        fprintf(stderr, "[vilk] preset warp pipeline loaded (PP pre-cleared to neutral)\n");
    }
    return ok;
}

// ---------------------------------------------------------------------------
// Initial ping-pong layout transition
// ---------------------------------------------------------------------------
void VulkanBackend::transition_ping_pong_initial() {
    VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool        = cmd_pool_;
    ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device_, &ai, &cmd);

    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    for (auto& pp : ping_pong_) {
        VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image               = pp.image;
        barrier.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        barrier.srcAccessMask       = 0;
        barrier.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    vkEndCommandBuffer(cmd);

    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &cmd;
    vkQueueSubmit(graphics_queue_, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(graphics_queue_);
    vkFreeCommandBuffers(device_, cmd_pool_, 1, &cmd);
}

// ---------------------------------------------------------------------------
// Swapchain recreation (resize / out-of-date)
// ---------------------------------------------------------------------------
void VulkanBackend::destroy_swapchain() {
    for (auto& frame : sc_frames_) {
        vkDestroyFramebuffer(device_, frame.fb,             nullptr);
        vkDestroyImageView(device_,   frame.view,           nullptr);
        vkDestroySemaphore(device_,   frame.render_finished, nullptr);
    }
    sc_frames_.clear();
    vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
}

void VulkanBackend::recreate_swapchain(uint32_t w, uint32_t h) {
    vkDeviceWaitIdle(device_);
    for (auto& pp : ping_pong_) {
        vkDestroyFramebuffer(device_, pp.fb, nullptr);
        pp.fb = VK_NULL_HANDLE;
    }
    destroy_swapchain();
    create_swapchain(w, h);

    // Ping-pong images are FIXED at their creation size (pp_w_, pp_h_).
    // Framebuffers must never exceed the underlying image dimensions.
    for (auto& pp : ping_pong_) {
        VkFramebufferCreateInfo fi{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fi.renderPass      = offscreen_pass_;
        fi.attachmentCount = 1; fi.pAttachments = &pp.view;
        fi.width = pp_w_; fi.height = pp_h_; fi.layers = 1;
        vkCreateFramebuffer(device_, &fi, nullptr, &pp.fb);
    }
    // Swapchain FBs scale with the window
    for (auto& frame : sc_frames_) {
        VkFramebufferCreateInfo fi{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fi.renderPass      = composite_pass_;
        fi.attachmentCount = 1; fi.pAttachments = &frame.view;
        fi.width = sc_extent_.width; fi.height = sc_extent_.height; fi.layers = 1;
        vkCreateFramebuffer(device_, &fi, nullptr, &frame.fb);
    }
}

} // namespace vilk

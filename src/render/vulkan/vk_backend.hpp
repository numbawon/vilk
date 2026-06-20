#pragma once
#include "preset_glsl_adapter.hpp"
#include "vilk/render/render_backend.hpp"
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <string>
#include <string_view>
#include <vector>

// GPU-side audio uniform buffer -- must match std140 layout in the shader.
struct AudioUBO {
    float bass   = 0.f;
    float mid    = 0.f;
    float treble = 0.f;
    float vol    = 0.f;
    float time_s = 0.f;
    float _pad[3] = {}; // pad to 32 bytes
};

namespace vilk {

static constexpr uint32_t kMaxFramesInFlight = 2;
static constexpr int      kWarpGridN          = 64;

struct WarpVertex { float pos[2]; float uv[2]; };

struct SwapchainFrame {
    VkImage       image           = VK_NULL_HANDLE;
    VkImageView   view            = VK_NULL_HANDLE;
    VkFramebuffer fb              = VK_NULL_HANDLE;
    VkSemaphore   render_finished = VK_NULL_HANDLE; // one per swapchain image
};

struct PingPongTarget {
    VkImage        image   = VK_NULL_HANDLE;
    VkDeviceMemory memory  = VK_NULL_HANDLE;
    VkImageView    view    = VK_NULL_HANDLE;
    VkFramebuffer  fb      = VK_NULL_HANDLE;
    VkSampler      sampler = VK_NULL_HANDLE;
};

struct FrameSync {
    VkSemaphore image_available = VK_NULL_HANDLE; // one per frame-in-flight
    VkFence     in_flight       = VK_NULL_HANDLE;
};

class VulkanBackend final : public IRenderBackend {
public:
    VulkanBackend()  = default;
    ~VulkanBackend() override { shutdown(); }

    bool init(const WindowHandle& window) override;
    void shutdown() override;
    void begin_frame() override;
    void end_frame() override;
    void resize(uint32_t width, uint32_t height) override;

    void update_audio(const AudioSnapshot& snap) override;
    void update_waveform(const WaveformSnapshot& wave) override;
    bool reload_shaders(std::string_view vert_glsl, std::string_view frag_glsl) override;
    bool reload_preset_composite(std::string_view vert_glsl, std::string_view frag_glsl) override;
    bool reload_preset_warp(std::string_view vert_glsl, std::string_view frag_glsl) override;

    ShaderHandle load_spirv(const uint32_t* words, size_t word_count,
                            std::string_view debug_name) override;
    void unload_shader(ShaderHandle handle) override;

private:
    bool create_instance();
    bool setup_debug_messenger();
    bool create_surface(GLFWwindow* window);
    bool pick_physical_device();
    bool create_device();
    bool create_swapchain(uint32_t width, uint32_t height);
    bool create_render_passes();
    bool create_ping_pong_targets(uint32_t width, uint32_t height);
    bool create_framebuffers();
    bool create_command_pool_and_buffers();
    bool create_sync_objects();
    void transition_ping_pong_initial();
    bool create_audio_ubo();
    bool create_descriptor_layout();
    bool create_pipelines(VkShaderModule vert, VkShaderModule frag);
    bool create_descriptor_sets();
    void update_descriptor_sets();

    // Preset composite + warp pipelines (share layout / UBO / descriptor sets)
    bool create_preset_descriptor_layout();
    bool create_preset_pipeline(VkShaderModule vert, VkShaderModule frag);
    bool create_warp_pipeline(VkShaderModule vert, VkShaderModule frag);
    bool create_preset_descriptor_sets();
    void update_preset_descriptor_sets();
    bool create_preset_ubo();
    bool create_noise_texture();
    bool create_seed_pipeline(VkShaderModule vert, VkShaderModule frag);
    bool create_warp_mesh_buffers();
    void update_warp_mesh(float t);
    bool create_waveform_pipeline();
    bool create_waveform_buffers();

    void destroy_swapchain();
    void recreate_swapchain(uint32_t width, uint32_t height);

    uint32_t find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags props) const;
    bool     alloc_image(uint32_t w, uint32_t h, VkFormat fmt,
                         VkImageUsageFlags usage, PingPongTarget& out);

    VkInstance               instance_        = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debug_messenger_ = VK_NULL_HANDLE;
    VkPhysicalDevice         gpu_             = VK_NULL_HANDLE;
    VkDevice                 device_          = VK_NULL_HANDLE;
    VkQueue                  graphics_queue_  = VK_NULL_HANDLE;
    VkQueue                  present_queue_   = VK_NULL_HANDLE;
    VkSurfaceKHR             surface_         = VK_NULL_HANDLE;
    uint32_t                 graphics_family_ = UINT32_MAX;
    uint32_t                 present_family_  = UINT32_MAX;

    VkSwapchainKHR             swapchain_     = VK_NULL_HANDLE;
    VkFormat                   sc_format_     = VK_FORMAT_UNDEFINED;
    VkExtent2D                 sc_extent_     = {};
    std::vector<SwapchainFrame> sc_frames_;

    VkRenderPass offscreen_pass_ = VK_NULL_HANDLE;
    VkRenderPass composite_pass_ = VK_NULL_HANDLE;

    PingPongTarget ping_pong_[2];
    uint32_t       pp_write_     = 0;

    VkCommandPool                cmd_pool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> cmd_bufs_;

    FrameSync frame_sync_[kMaxFramesInFlight];
    uint32_t  current_frame_ = 0;
    uint32_t  image_idx_     = 0;

    // Pipeline (shared vert+frag; differs only in compatible render pass)
    VkDescriptorSetLayout desc_layout_         = VK_NULL_HANDLE;
    VkPipelineLayout      pipe_layout_         = VK_NULL_HANDLE;
    VkPipeline            pipeline_offscreen_  = VK_NULL_HANDLE;
    VkPipeline            pipeline_composite_  = VK_NULL_HANDLE;
    // Additive audio seed pass (same shader, additive blend, runs after warp)
    VkPipeline            pipeline_seed_       = VK_NULL_HANDLE;
    VkDescriptorPool      desc_pool_           = VK_NULL_HANDLE;
    VkDescriptorSet       desc_sets_[2]        = {};

    VkBuffer       audio_ubo_buf_[kMaxFramesInFlight] = {};
    VkDeviceMemory audio_ubo_mem_[kMaxFramesInFlight] = {};
    void*          audio_ubo_map_[kMaxFramesInFlight] = {};
    AudioUBO       pending_audio_ = {};

    // Preset composite path (separate descriptors / pipeline / UBO)
    VkDescriptorSetLayout preset_desc_layout_       = VK_NULL_HANDLE;
    VkPipelineLayout      preset_pipe_layout_       = VK_NULL_HANDLE;
    VkPipeline            preset_pipeline_          = VK_NULL_HANDLE;
    VkDescriptorPool      preset_desc_pool_         = VK_NULL_HANDLE;
    VkDescriptorSet       preset_desc_sets_[2]      = {};
    VkBuffer              preset_ubo_buf_[kMaxFramesInFlight] = {};
    VkDeviceMemory        preset_ubo_mem_[kMaxFramesInFlight] = {};
    void*                 preset_ubo_map_[kMaxFramesInFlight] = {};
    vilk::PresetUBO       pending_preset_ubo_       = {};
    bool                  preset_mode_              = false;
    VkPipeline            warp_pipeline_            = VK_NULL_HANDLE;
    bool                  warp_mode_                = false;

    // Static noise texture (binding 2 in preset descriptor set)
    VkImage        noise_image_   = VK_NULL_HANDLE;
    VkDeviceMemory noise_memory_  = VK_NULL_HANDLE;
    VkImageView    noise_view_    = VK_NULL_HANDLE;
    VkSampler      noise_sampler_ = VK_NULL_HANDLE;

    // Per-pixel warp mesh (64×64 grid, updated CPU-side each frame)
    VkBuffer       warp_vb_        = VK_NULL_HANDLE;
    VkDeviceMemory warp_vb_mem_    = VK_NULL_HANDLE;
    void*          warp_vb_map_    = nullptr;
    VkBuffer       warp_ib_        = VK_NULL_HANDLE;
    VkDeviceMemory warp_ib_mem_    = VK_NULL_HANDLE;
    uint32_t       warp_idx_count_ = 0;

    std::string shader_dir_; // path to live .glsl files; empty = embedded fallback

    // Waveform line-strip (PCM samples → LINE_STRIP into offscreen pass)
    VkPipeline     pipeline_waveform_ = VK_NULL_HANDLE;
    VkBuffer       wave_vb_           = VK_NULL_HANDLE;
    VkDeviceMemory wave_vb_mem_       = VK_NULL_HANDLE;
    void*          wave_vb_map_       = nullptr;
    uint32_t       wave_vertex_count_ = 0;

    uint32_t width_  = 0;
    uint32_t height_ = 0;
    uint32_t pp_w_   = 0;  // ping-pong image dimensions (fixed at init, never resized)
    uint32_t pp_h_   = 0;
    bool     initialized_ = false;
};

} // namespace vilk

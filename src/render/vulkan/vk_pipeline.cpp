#include "vk_backend.hpp"
#include "spirv_compiler.hpp"
#include "preset_glsl_adapter.hpp"
#include <cstdio>
#include <cstring>

namespace vilk {

// ---------------------------------------------------------------------------
// Descriptor set layout: set 0, binding 0 = combined image sampler (fragment)
// ---------------------------------------------------------------------------
bool VulkanBackend::create_descriptor_layout() {
    VkDescriptorSetLayoutBinding bindings[2]{};
    // binding 0: ping-pong sampler
    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    // binding 1: audio UBO
    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    ci.bindingCount = 2;
    ci.pBindings    = bindings;

    if (vkCreateDescriptorSetLayout(device_, &ci, nullptr, &desc_layout_) != VK_SUCCESS) {
        fprintf(stderr, "[vilk] vkCreateDescriptorSetLayout failed\n");
        return false;
    }

    VkPipelineLayoutCreateInfo pli{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount = 1;
    pli.pSetLayouts    = &desc_layout_;

    if (vkCreatePipelineLayout(device_, &pli, nullptr, &pipe_layout_) != VK_SUCCESS) {
        fprintf(stderr, "[vilk] vkCreatePipelineLayout failed\n");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Graphics pipelines (offscreen + composite share vert+frag, differ in renderPass)
// ---------------------------------------------------------------------------
bool VulkanBackend::create_pipelines(VkShaderModule vert, VkShaderModule frag) {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName  = "main";

    // Fullscreen triangle -- no vertex input
    VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };

    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // Dynamic viewport + scissor -- no pipeline recreation on resize
    VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1;
    vp.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_NONE;
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blend_att{};
    blend_att.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    blend.attachmentCount = 1;
    blend.pAttachments    = &blend_att;

    VkDynamicState dyn_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates    = dyn_states;

    VkGraphicsPipelineCreateInfo ci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    ci.stageCount          = 2;
    ci.pStages             = stages;
    ci.pVertexInputState   = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState      = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState   = &ms;
    ci.pColorBlendState    = &blend;
    ci.pDynamicState       = &dyn;
    ci.layout              = pipe_layout_;

    ci.renderPass = offscreen_pass_;
    if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &ci,
                                   nullptr, &pipeline_offscreen_) != VK_SUCCESS) {
        fprintf(stderr, "[vilk] vkCreateGraphicsPipelines (offscreen) failed\n");
        return false;
    }

    ci.renderPass = composite_pass_;
    if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &ci,
                                   nullptr, &pipeline_composite_) != VK_SUCCESS) {
        fprintf(stderr, "[vilk] vkCreateGraphicsPipelines (composite) failed\n");
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Seed pipeline -- alpha-blends (r=0.37±var, g=0.37±var) into PP each frame.
// Equilibrium = seed color regardless of warp decay, keeping PP centered at the
// Kali Mix warp's neutral point (0.37) so shift directions vary spatially.
// ---------------------------------------------------------------------------
bool VulkanBackend::create_seed_pipeline(VkShaderModule vert, VkShaderModule frag) {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName  = "main";

    VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1;
    vp.scissorCount  = 1;
    VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_NONE;
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Alpha blend: pp_new = pp_old*(1-src_alpha) + seed_rgb*src_alpha
    // Equilibrium converges to seed_color in ~25 frames (4% per frame).
    VkPipelineColorBlendAttachmentState add_att{};
    add_att.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    add_att.blendEnable         = VK_TRUE;
    add_att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    add_att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    add_att.colorBlendOp        = VK_BLEND_OP_ADD;
    add_att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    add_att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    add_att.alphaBlendOp        = VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo add_blend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    add_blend.attachmentCount = 1;
    add_blend.pAttachments    = &add_att;

    VkDynamicState dyn_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates    = dyn_states;

    VkGraphicsPipelineCreateInfo ci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    ci.stageCount          = 2;
    ci.pStages             = stages;
    ci.pVertexInputState   = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState      = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState   = &ms;
    ci.pColorBlendState    = &add_blend;
    ci.pDynamicState       = &dyn;
    ci.layout              = pipe_layout_;
    ci.renderPass          = offscreen_pass_;

    if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &ci,
                                   nullptr, &pipeline_seed_) != VK_SUCCESS) {
        fprintf(stderr, "[vilk] vkCreateGraphicsPipelines (seed) failed\n");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Waveform pipeline -- LINE_STRIP, additive blend, single vec2 vertex input.
// Draws PCM waveform into the offscreen (ping-pong) buffer each frame.
// ---------------------------------------------------------------------------
bool VulkanBackend::create_waveform_pipeline() {
    static constexpr const char* kWaveVert = R"GLSL(
#version 450
layout(location = 0) in vec2 a_pos;
void main() { gl_Position = vec4(a_pos, 0.0, 1.0); }
)GLSL";
    static constexpr const char* kWaveFrag = R"GLSL(
#version 450
layout(set=0, binding=1, std140) uniform AudioUBO {
    float bass; float mid; float treble; float vol; float time_s; float _p[3];
} u;
layout(location = 0) out vec4 out_color;
void main() {
    float b = clamp(0.3 + 0.7 * u.vol, 0.0, 1.0);
    out_color = vec4(b, b * 0.85, b * 0.6, 0.0);
}
)GLSL";

    auto vert_spv = spirv::compile(kWaveVert, VK_SHADER_STAGE_VERTEX_BIT,   "waveform.vert");
    auto frag_spv = spirv::compile(kWaveFrag, VK_SHADER_STAGE_FRAGMENT_BIT, "waveform.frag");
    if (vert_spv.empty() || frag_spv.empty()) {
        fprintf(stderr, "[vilk] waveform shader compile failed\n");
        return false;
    }

    auto make_mod = [&](const std::vector<uint32_t>& spv) {
        VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        ci.codeSize = spv.size() * 4;
        ci.pCode    = spv.data();
        VkShaderModule m = VK_NULL_HANDLE;
        vkCreateShaderModule(device_, &ci, nullptr, &m);
        return m;
    };
    VkShaderModule vert = make_mod(vert_spv);
    VkShaderModule frag = make_mod(frag_spv);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,   vert, "main" };
    stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main" };

    // Single vertex binding: vec2 pos (stride 8)
    VkVertexInputBindingDescription   vbd{ 0, sizeof(float) * 2, VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription vad{ 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 };
    VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vi.vertexBindingDescriptionCount   = 1; vi.pVertexBindingDescriptions   = &vbd;
    vi.vertexAttributeDescriptionCount = 1; vi.pVertexAttributeDescriptions = &vad;

    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;

    VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1; vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_NONE;
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState add_att{};
    add_att.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    add_att.blendEnable         = VK_TRUE;
    add_att.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    add_att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    add_att.colorBlendOp        = VK_BLEND_OP_ADD;
    add_att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    add_att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    add_att.alphaBlendOp        = VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo add_blend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    add_blend.attachmentCount = 1;
    add_blend.pAttachments    = &add_att;

    VkDynamicState dyn_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dyn.dynamicStateCount = 2; dyn.pDynamicStates = dyn_states;

    VkGraphicsPipelineCreateInfo ci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    ci.stageCount = 2; ci.pStages = stages;
    ci.pVertexInputState   = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState      = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState   = &ms;
    ci.pColorBlendState    = &add_blend;
    ci.pDynamicState       = &dyn;
    ci.layout              = pipe_layout_;
    ci.renderPass          = offscreen_pass_;

    bool ok = (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &ci,
                                          nullptr, &pipeline_waveform_) == VK_SUCCESS);
    vkDestroyShaderModule(device_, vert, nullptr);
    vkDestroyShaderModule(device_, frag, nullptr);
    if (!ok) fprintf(stderr, "[vilk] waveform pipeline create failed\n");
    return ok;
}

// ---------------------------------------------------------------------------
// Descriptor pool + sets (2 sets: one per ping-pong source)
// ---------------------------------------------------------------------------
bool VulkanBackend::create_descriptor_sets() {
    VkDescriptorPoolSize pool_sizes[2]{};
    pool_sizes[0].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_sizes[0].descriptorCount = 2;
    pool_sizes[1].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    pool_sizes[1].descriptorCount = 2;

    VkDescriptorPoolCreateInfo pi{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pi.maxSets       = 2;
    pi.poolSizeCount = 2;
    pi.pPoolSizes    = pool_sizes;

    if (vkCreateDescriptorPool(device_, &pi, nullptr, &desc_pool_) != VK_SUCCESS) {
        fprintf(stderr, "[vilk] vkCreateDescriptorPool failed\n");
        return false;
    }

    VkDescriptorSetLayout layouts[2] = { desc_layout_, desc_layout_ };
    VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool     = desc_pool_;
    ai.descriptorSetCount = 2;
    ai.pSetLayouts        = layouts;

    if (vkAllocateDescriptorSets(device_, &ai, desc_sets_) != VK_SUCCESS) {
        fprintf(stderr, "[vilk] vkAllocateDescriptorSets failed\n");
        return false;
    }
    return true;
}

// desc_sets_[i] samples from ping_pong_[i]
// In a given frame: offscreen writes ping_pong_[pp_write_], samples ping_pong_[1-pp_write_]
//                   composite samples ping_pong_[pp_write_] (just written)
void VulkanBackend::update_descriptor_sets() {
    for (int i = 0; i < 2; ++i) {
        VkDescriptorImageInfo img{};
        img.sampler     = ping_pong_[i].sampler;
        img.imageView   = ping_pong_[i].view;
        img.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorBufferInfo buf{};
        buf.buffer = audio_ubo_buf_[i];
        buf.offset = 0;
        buf.range  = sizeof(AudioUBO);

        VkWriteDescriptorSet writes[2]{};
        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = desc_sets_[i];
        writes[0].dstBinding      = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo      = &img;

        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet          = desc_sets_[i];
        writes[1].dstBinding      = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[1].pBufferInfo     = &buf;

        vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);
    }
}

// ---------------------------------------------------------------------------
// Preset composite — separate descriptor layout, pipeline layout, and pipeline.
// Descriptor set layout: binding 0 = sampler_main, binding 1 = PresetUBO.
// ---------------------------------------------------------------------------
bool VulkanBackend::create_preset_descriptor_layout() {
    VkDescriptorSetLayoutBinding bindings[3]{};
    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[2].binding         = 2;  // sampler_noise_lq
    bindings[2].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    ci.bindingCount = 3;
    ci.pBindings    = bindings;
    if (vkCreateDescriptorSetLayout(device_, &ci, nullptr, &preset_desc_layout_) != VK_SUCCESS) {
        fprintf(stderr, "[vilk] vkCreateDescriptorSetLayout (preset) failed\n");
        return false;
    }

    VkPipelineLayoutCreateInfo pli{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount = 1;
    pli.pSetLayouts    = &preset_desc_layout_;
    if (vkCreatePipelineLayout(device_, &pli, nullptr, &preset_pipe_layout_) != VK_SUCCESS) {
        fprintf(stderr, "[vilk] vkCreatePipelineLayout (preset) failed\n");
        return false;
    }
    return true;
}

bool VulkanBackend::create_preset_pipeline(VkShaderModule vert, VkShaderModule frag) {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName  = "main";

    VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1;
    vp.scissorCount  = 1;
    VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_NONE;
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend_att{};
    blend_att.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    blend.attachmentCount = 1;
    blend.pAttachments    = &blend_att;
    VkDynamicState dyn_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates    = dyn_states;

    VkGraphicsPipelineCreateInfo ci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    ci.stageCount          = 2;
    ci.pStages             = stages;
    ci.pVertexInputState   = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState      = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState   = &ms;
    ci.pColorBlendState    = &blend;
    ci.pDynamicState       = &dyn;
    ci.layout              = preset_pipe_layout_;
    ci.renderPass          = composite_pass_;
    if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &ci,
                                   nullptr, &preset_pipeline_) != VK_SUCCESS) {
        fprintf(stderr, "[vilk] vkCreateGraphicsPipelines (preset composite) failed\n");
        return false;
    }
    return true;
}

bool VulkanBackend::create_warp_pipeline(VkShaderModule vert, VkShaderModule frag) {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName  = "main";

    // Warp uses a 64×64 mesh: each vertex has pos(xy) + warp_uv(xy)
    VkVertexInputBindingDescription vib{};
    vib.binding   = 0;
    vib.stride    = sizeof(WarpVertex);
    vib.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription via[2]{};
    via[0] = { 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 };
    via[1] = { 1, 0, VK_FORMAT_R32G32_SFLOAT, sizeof(float) * 2 };
    VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vi.vertexBindingDescriptionCount   = 1;
    vi.pVertexBindingDescriptions      = &vib;
    vi.vertexAttributeDescriptionCount = 2;
    vi.pVertexAttributeDescriptions    = via;

    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1;
    vp.scissorCount  = 1;
    VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_NONE;
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend_att{};
    blend_att.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    blend.attachmentCount = 1;
    blend.pAttachments    = &blend_att;
    VkDynamicState dyn_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates    = dyn_states;

    VkGraphicsPipelineCreateInfo ci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    ci.stageCount          = 2;
    ci.pStages             = stages;
    ci.pVertexInputState   = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState      = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState   = &ms;
    ci.pColorBlendState    = &blend;
    ci.pDynamicState       = &dyn;
    ci.layout              = preset_pipe_layout_;
    ci.renderPass          = offscreen_pass_;
    if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &ci,
                                   nullptr, &warp_pipeline_) != VK_SUCCESS) {
        fprintf(stderr, "[vilk] vkCreateGraphicsPipelines (preset warp) failed\n");
        return false;
    }
    return true;
}

bool VulkanBackend::create_preset_ubo() {
    constexpr VkDeviceSize size = sizeof(PresetUBO);
    constexpr VkMemoryPropertyFlags flags =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    for (int i = 0; i < kMaxFramesInFlight; ++i) {
        VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bi.size        = size;
        bi.usage       = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device_, &bi, nullptr, &preset_ubo_buf_[i]) != VK_SUCCESS) return false;

        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device_, preset_ubo_buf_[i], &req);
        uint32_t mem_type = find_memory_type(req.memoryTypeBits, flags);
        if (mem_type == UINT32_MAX) return false;

        VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        ai.allocationSize  = req.size;
        ai.memoryTypeIndex = mem_type;
        if (vkAllocateMemory(device_, &ai, nullptr, &preset_ubo_mem_[i]) != VK_SUCCESS) return false;
        vkBindBufferMemory(device_, preset_ubo_buf_[i], preset_ubo_mem_[i], 0);
        vkMapMemory(device_, preset_ubo_mem_[i], 0, size, 0, &preset_ubo_map_[i]);
        memset(preset_ubo_map_[i], 0, size);
    }
    return true;
}

bool VulkanBackend::create_preset_descriptor_sets() {
    VkDescriptorPoolSize pool_sizes[2]{};
    pool_sizes[0].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_sizes[0].descriptorCount = 4;  // 2 sets × 2 samplers (main + noise)
    pool_sizes[1].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    pool_sizes[1].descriptorCount = 2;

    VkDescriptorPoolCreateInfo pi{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pi.maxSets       = 2;
    pi.poolSizeCount = 2;
    pi.pPoolSizes    = pool_sizes;
    if (vkCreateDescriptorPool(device_, &pi, nullptr, &preset_desc_pool_) != VK_SUCCESS) {
        fprintf(stderr, "[vilk] vkCreateDescriptorPool (preset) failed\n");
        return false;
    }

    VkDescriptorSetLayout layouts[2] = { preset_desc_layout_, preset_desc_layout_ };
    VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool     = preset_desc_pool_;
    ai.descriptorSetCount = 2;
    ai.pSetLayouts        = layouts;
    if (vkAllocateDescriptorSets(device_, &ai, preset_desc_sets_) != VK_SUCCESS) {
        fprintf(stderr, "[vilk] vkAllocateDescriptorSets (preset) failed\n");
        return false;
    }
    return true;
}

// preset_desc_sets_[i]:
//   binding 0 → ping_pong_[i]   (sampler_main)
//   binding 1 → preset_ubo_buf_[i] (PresetBlock)
//   binding 2 → noise_image_    (sampler_noise_lq)
void VulkanBackend::update_preset_descriptor_sets() {
    for (int i = 0; i < 2; ++i) {
        VkDescriptorImageInfo main_img{};
        main_img.sampler     = ping_pong_[i].sampler;
        main_img.imageView   = ping_pong_[i].view;
        main_img.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorBufferInfo buf{};
        buf.buffer = preset_ubo_buf_[i];
        buf.offset = 0;
        buf.range  = sizeof(PresetUBO);

        VkDescriptorImageInfo noise_img{};
        noise_img.sampler     = noise_sampler_;
        noise_img.imageView   = noise_view_;
        noise_img.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet writes[3]{};
        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = preset_desc_sets_[i];
        writes[0].dstBinding      = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo      = &main_img;

        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet          = preset_desc_sets_[i];
        writes[1].dstBinding      = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[1].pBufferInfo     = &buf;

        writes[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet          = preset_desc_sets_[i];
        writes[2].dstBinding      = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[2].pImageInfo      = &noise_img;

        vkUpdateDescriptorSets(device_, 3, writes, 0, nullptr);
    }
}

} // namespace vilk

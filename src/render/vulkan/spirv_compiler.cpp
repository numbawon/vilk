#include "spirv_compiler.hpp"
#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/SPIRV/GlslangToSpv.h>
#include <cstdio>
#include <string>

namespace vilk::spirv {

bool init() {
    return glslang::InitializeProcess();
}

void shutdown() {
    glslang::FinalizeProcess();
}

std::vector<uint32_t> compile(std::string_view glsl,
                               VkShaderStageFlagBits stage,
                               std::string_view debug_name) {
    EShLanguage lang;
    switch (stage) {
        case VK_SHADER_STAGE_VERTEX_BIT:   lang = EShLangVertex;   break;
        case VK_SHADER_STAGE_FRAGMENT_BIT: lang = EShLangFragment; break;
        case VK_SHADER_STAGE_COMPUTE_BIT:  lang = EShLangCompute;  break;
        default:
            fprintf(stderr, "[spirv] unsupported stage for '%.*s'\n",
                    (int)debug_name.size(), debug_name.data());
            return {};
    }

    glslang::TShader shader(lang);
    const char* src    = glsl.data();
    int         srclen = static_cast<int>(glsl.size());
    shader.setStringsWithLengths(&src, &srclen, 1);

    shader.setEnvInput(glslang::EShSourceGlsl, lang,
                       glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_2);
    shader.setEnvTarget(glslang::EShTargetSpv,    glslang::EShTargetSpv_1_5);

    const TBuiltInResource* resources = GetDefaultResources();
    EShMessages messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);

    if (!shader.parse(resources, 100, false, messages)) {
        fprintf(stderr, "[spirv] parse error '%.*s':\n%s\n%s\n",
                (int)debug_name.size(), debug_name.data(),
                shader.getInfoLog(), shader.getInfoDebugLog());
        return {};
    }

    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages)) {
        fprintf(stderr, "[spirv] link error '%.*s':\n%s\n%s\n",
                (int)debug_name.size(), debug_name.data(),
                program.getInfoLog(), program.getInfoDebugLog());
        return {};
    }

    std::vector<uint32_t> spirv;
    glslang::GlslangToSpv(*program.getIntermediate(lang), spirv);
    return spirv;
}

} // namespace vilk::spirv

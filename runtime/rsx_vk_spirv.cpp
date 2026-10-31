// GLSL -> SPIR-V for the Vulkan renderer (rsx_vk.cpp): programs are translated from guest microcode at run time, and the
// driver takes SPIR-V only. Safe to call from several threads.
#include "ppu_runtime.h"

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>

#include <mutex>
#include <string>
#include <vector>

namespace rsx::vk {
// stage: 0 vertex, 1 fragment, 2 compute
bool glsl_to_spirv(int stage, const std::string& src, std::vector<u32>& spirv, std::string& log) {
    static std::once_flag init;
    std::call_once(init, [] { glslang::InitializeProcess(); });
    static const EShLanguage langs[3] = {EShLangVertex, EShLangFragment, EShLangCompute};
    const EShLanguage lang = langs[stage];
    glslang::TShader shader(lang);
    const char* text = src.c_str();
    shader.setStrings(&text, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_3);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_6);
    const EShMessages msgs = (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules);
    if (!shader.parse(GetDefaultResources(), 450, false, msgs)) {
        log = shader.getInfoLog();
        return false;
    }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(msgs)) {
        log = program.getInfoLog();
        return false;
    }
    glslang::SpvOptions options;
    options.disableOptimizer = true;   // (the driver optimises; built without SPIRV-Tools)
    glslang::GlslangToSpv(*program.getIntermediate(lang), spirv, &options);
    return !spirv.empty();
}
}  // namespace rsx::vk

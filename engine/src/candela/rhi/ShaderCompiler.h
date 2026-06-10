#pragma once

#include "candela/rhi/VulkanCommon.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace candela {

enum class ShaderStage { Vertex, Fragment, Compute };

// Compiles Slang source to SPIR-V by invoking slangc from the Vulkan SDK.
// Phase 0 approach: good enough for startup compilation and hot reload on a
// dev machine; will move to the Slang API in-process later.
class ShaderCompiler {
public:
    ShaderCompiler();

    // Returns SPIR-V words, or nullopt on failure (errors are logged).
    std::optional<std::vector<uint32_t>> compile(
        const std::filesystem::path& sourcePath, const std::string& entry,
        ShaderStage stage);

private:
    std::filesystem::path m_slangc;
    std::filesystem::path m_outputDir;
};

VkShaderModule createShaderModule(VkDevice device,
                                  const std::vector<uint32_t>& spirv);

} // namespace candela

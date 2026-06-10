#include "candela/rhi/ShaderCompiler.h"

#include <cstdlib>
#include <fstream>

namespace candela {

namespace {

const char* stageName(ShaderStage stage) {
    switch (stage) {
    case ShaderStage::Vertex: return "vertex";
    case ShaderStage::Fragment: return "fragment";
    case ShaderStage::Compute: return "compute";
    }
    return "unknown";
}

std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        return {};
    }
    return {std::istreambuf_iterator<char>(file),
            std::istreambuf_iterator<char>()};
}

} // namespace

ShaderCompiler::ShaderCompiler() {
    const char* sdk = std::getenv("VULKAN_SDK");
    CD_ASSERT(sdk != nullptr,
              "VULKAN_SDK environment variable not set — install the Vulkan SDK");
    m_slangc = std::filesystem::path(sdk) / "Bin" / "slangc.exe";
    CD_ASSERT(std::filesystem::exists(m_slangc), "slangc not found at {}",
              m_slangc.string());

    m_outputDir = std::filesystem::temp_directory_path() / "candela-shaders";
    std::filesystem::create_directories(m_outputDir);
}

std::optional<std::vector<uint32_t>> ShaderCompiler::compile(
    const std::filesystem::path& sourcePath, const std::string& entry,
    ShaderStage stage) {
    const std::string baseName = sourcePath.stem().string() + "." + entry;
    const std::filesystem::path spvPath = m_outputDir / (baseName + ".spv");
    const std::filesystem::path errPath = m_outputDir / (baseName + ".log");

    // std::system goes through cmd.exe; the outer quotes make cmd preserve the
    // quoting of the individual paths.
    const std::string command =
        "\"\"" + m_slangc.string() + "\" \"" + sourcePath.string() +
        "\" -entry " + entry + " -stage " + stageName(stage) +
        " -target spirv -o \"" + spvPath.string() + "\" 2> \"" +
        errPath.string() + "\"\"";

    const int exitCode = std::system(command.c_str());
    if (exitCode != 0) {
        CD_ERROR("slangc failed for {}:{} (exit {}):\n{}", sourcePath.string(),
                 entry, exitCode, readTextFile(errPath));
        return std::nullopt;
    }

    std::ifstream spv(spvPath, std::ios::binary | std::ios::ate);
    if (!spv) {
        CD_ERROR("slangc produced no output for {}:{}", sourcePath.string(),
                 entry);
        return std::nullopt;
    }
    const std::streamsize size = spv.tellg();
    CD_ASSERT(size > 0 && size % 4 == 0, "Invalid SPIR-V size {}", size);
    std::vector<uint32_t> words(static_cast<size_t>(size) / 4);
    spv.seekg(0);
    spv.read(reinterpret_cast<char*>(words.data()), size);
    return words;
}

VkShaderModule createShaderModule(VkDevice device,
                                  const std::vector<uint32_t>& spirv) {
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = spirv.size() * sizeof(uint32_t);
    info.pCode = spirv.data();

    VkShaderModule module = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(device, &info, nullptr, &module));
    return module;
}

} // namespace candela

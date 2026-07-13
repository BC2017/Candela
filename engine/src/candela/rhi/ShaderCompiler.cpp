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

uint64_t fnv1aBytes(const void* data, size_t byteCount,
                    uint64_t hash = 14695981039346656037ull) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < byteCount; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

// Content hash over every .slang file in the directory — any of them can be
// #included by any other, so this is the conservative invalidation unit
// (mirrors ShaderCache::invalidateAll for hot reload).
uint64_t hashShaderDirectory(const std::filesystem::path& dir) {
    uint64_t hash = 14695981039346656037ull;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.path().extension() != ".slang") {
            continue;
        }
        const std::string name = entry.path().filename().string();
        hash = fnv1aBytes(name.data(), name.size(), hash);
        const std::string text = readTextFile(entry.path());
        hash = fnv1aBytes(text.data(), text.size(), hash);
    }
    return hash;
}

} // namespace

ShaderCompiler::ShaderCompiler() {
    const char* sdk = std::getenv("VULKAN_SDK");
    CD_ASSERT(sdk != nullptr,
              "VULKAN_SDK environment variable not set — install the Vulkan SDK");
#if defined(_WIN32)
    m_slangc = std::filesystem::path(sdk) / "Bin" / "slangc.exe";
#else
    m_slangc = std::filesystem::path(sdk) / "bin" / "slangc";
#endif
    CD_ASSERT(std::filesystem::exists(m_slangc), "slangc not found at {}",
              m_slangc.string());

    m_outputDir = std::filesystem::temp_directory_path() / "candela-shaders";
    std::filesystem::create_directories(m_outputDir);

    // Persistent SPIR-V cache: a warm start skips the slangc subprocess
    // entirely (the dominant startup cost).
    if (const char* localAppData = std::getenv("LOCALAPPDATA")) {
        m_cacheDir = std::filesystem::path(localAppData) / "Candela" /
                     "shader-cache";
    } else {
        m_cacheDir = m_outputDir / "cache";
    }
    std::filesystem::create_directories(m_cacheDir);
}

std::optional<std::vector<uint32_t>> ShaderCompiler::compile(
    const std::filesystem::path& sourcePath, const std::string& entry,
    ShaderStage stage) {
    const std::string baseName = sourcePath.stem().string() + "." + entry;
    const std::filesystem::path spvPath = m_outputDir / (baseName + ".spv");
    const std::filesystem::path errPath = m_outputDir / (baseName + ".log");

    // Disk cache lookup: keyed by entry+stage+the shader directory contents.
    const std::string keySource = sourcePath.filename().string() + "|" +
                                  entry + "|" + stageName(stage);
    uint64_t cacheKey = hashShaderDirectory(sourcePath.parent_path());
    cacheKey = fnv1aBytes(keySource.data(), keySource.size(), cacheKey);
    char keyHex[20];
    std::snprintf(keyHex, sizeof(keyHex), "%016llx",
                  static_cast<unsigned long long>(cacheKey));
    const std::filesystem::path cachePath =
        m_cacheDir / (baseName + "." + keyHex + ".spv");
    if (std::ifstream cached{cachePath, std::ios::binary | std::ios::ate}) {
        const std::streamsize size = cached.tellg();
        if (size > 0 && size % 4 == 0) {
            std::vector<uint32_t> words(static_cast<size_t>(size) / 4);
            cached.seekg(0);
            cached.read(reinterpret_cast<char*>(words.data()), size);
            return words;
        }
    }

    // Column-major matrix layout so GLM matrices upload without transposes.
    // -I the source's directory so shaders can #include "common.slang".
    std::string command =
        "\"" + m_slangc.string() + "\" \"" + sourcePath.string() +
        "\" -entry " + entry + " -stage " + stageName(stage) +
        " -target spirv -matrix-layout-column-major -I \"" +
        sourcePath.parent_path().string() + "\" -o \"" + spvPath.string() +
        "\" 2> \"" + errPath.string() + "\"";
#if defined(_WIN32)
    // std::system goes through cmd.exe; the outer quotes make cmd preserve
    // the quoting of the individual paths.
    command = "\"" + command + "\"";
#endif

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

    if (std::ofstream out{cachePath, std::ios::binary}) {
        out.write(reinterpret_cast<const char*>(words.data()),
                  static_cast<std::streamsize>(words.size() * 4));
    }
    return words;
}

namespace {

uint64_t fnv1aHash(const std::vector<uint32_t>& words) {
    uint64_t hash = 14695981039346656037ull;
    const auto* bytes = reinterpret_cast<const uint8_t*>(words.data());
    const size_t byteCount = words.size() * sizeof(uint32_t);
    for (size_t i = 0; i < byteCount; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

} // namespace

ShaderCache::ShaderCache(VkDevice device) : m_device(device) {}

ShaderCache::~ShaderCache() {
    for (auto& [hash, entry] : m_modules) {
        vkDestroyShaderModule(m_device, entry.first, nullptr);
    }
}

void ShaderCache::releaseModule(uint64_t hash) {
    auto it = m_modules.find(hash);
    if (it == m_modules.end()) {
        return;
    }
    if (--it->second.second == 0) {
        vkDestroyShaderModule(m_device, it->second.first, nullptr);
        m_modules.erase(it);
    }
}

void ShaderCache::invalidateAll() {
    for (auto& [key, entry] : m_entries) {
        entry.sourceTime = {};
    }
}

VkShaderModule ShaderCache::get(const std::filesystem::path& sourcePath,
                                const std::string& entry, ShaderStage stage) {
    const std::string key = sourcePath.string() + "|" + entry + "|" +
                            std::to_string(static_cast<int>(stage));

    std::error_code ec;
    const auto sourceTime = std::filesystem::last_write_time(sourcePath, ec);
    CD_ASSERT(!ec, "Shader source missing: {}", sourcePath.string());

    auto found = m_entries.find(key);
    if (found != m_entries.end() && found->second.sourceTime == sourceTime) {
        return found->second.module;
    }

    auto spirv = m_compiler.compile(sourcePath, entry, stage);
    if (!spirv) {
        return VK_NULL_HANDLE;
    }

    const uint64_t hash = fnv1aHash(*spirv);

    if (found != m_entries.end()) {
        if (found->second.spirvHash == hash) {
            // Source touched but SPIR-V unchanged — keep the module.
            found->second.sourceTime = sourceTime;
            return found->second.module;
        }
        releaseModule(found->second.spirvHash);
        m_entries.erase(found);
    }

    VkShaderModule module = VK_NULL_HANDLE;
    auto existing = m_modules.find(hash);
    if (existing != m_modules.end()) {
        module = existing->second.first;
        ++existing->second.second;
    } else {
        module = createShaderModule(m_device, *spirv);
        m_modules.emplace(hash, std::make_pair(module, 1u));
    }

    m_entries.emplace(key, Entry{hash, module, sourceTime});
    return module;
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

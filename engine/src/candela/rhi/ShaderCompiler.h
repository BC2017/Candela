#pragma once

#include "candela/rhi/VulkanCommon.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
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
    std::filesystem::path m_cacheDir; // persistent SPIR-V cache
};

VkShaderModule createShaderModule(VkDevice device,
                                  const std::vector<uint32_t>& spirv);

// Caches compiled shader modules keyed by (path, entry, stage). Modules with
// identical SPIR-V are shared. get() recompiles when the source file's write
// time changed; stale modules are destroyed once no key references them.
class ShaderCache {
public:
    explicit ShaderCache(VkDevice device);
    ~ShaderCache();

    ShaderCache(const ShaderCache&) = delete;
    ShaderCache& operator=(const ShaderCache&) = delete;

    // Returns VK_NULL_HANDLE on compile failure (previous module stays cached,
    // so callers can keep using their existing pipelines).
    VkShaderModule get(const std::filesystem::path& sourcePath,
                       const std::string& entry, ShaderStage stage);

    // Forces recompilation on the next get() for every entry. Used by hot
    // reload, where an edited #include can affect any shader. Modules whose
    // SPIR-V is unchanged are kept via the hash table.
    void invalidateAll();

private:
    struct Entry {
        uint64_t spirvHash = 0;
        VkShaderModule module = VK_NULL_HANDLE;
        std::filesystem::file_time_type sourceTime;
    };

    void releaseModule(uint64_t hash);

    VkDevice m_device;
    ShaderCompiler m_compiler;
    std::map<std::string, Entry> m_entries;          // key: path|entry|stage
    std::map<uint64_t, std::pair<VkShaderModule, uint32_t>> m_modules; // hash → {module, refcount}
};

} // namespace candela

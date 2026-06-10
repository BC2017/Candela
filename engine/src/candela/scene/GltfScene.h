#pragma once

#include "candela/rhi/Resources.h"

#include <glm/glm.hpp>

#include <filesystem>
#include <vector>

namespace candela {

class Context;
class Bindless;

struct Vertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 uv;
    glm::vec4 tangent; // xyz tangent, w handedness (glTF convention)
};
static_assert(sizeof(Vertex) == 48, "Vertex layout must match common.slang");

struct DrawFlags {
    static constexpr uint32_t kAlphaMask = 1u << 0;
    static constexpr uint32_t kHasNormalMap = 1u << 1; // requires tangents
};

struct DrawItem {
    glm::mat4 transform{1.0f};
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    uint32_t indexCount = 0;
    VkDeviceAddress vertexAddress = 0;
    uint32_t albedoTexture = 0;
    uint32_t normalTexture = 0;
    uint32_t metallicRoughnessTexture = 0;
    uint32_t occlusionTexture = 0;
    uint32_t flags = 0;
    float metallicFactor = 1.0f;
    float roughnessFactor = 1.0f;
    glm::vec4 baseColorFactor{1.0f};
};

struct Scene {
    std::vector<GpuBuffer> buffers;
    std::vector<GpuImage> textures;
    std::vector<DrawItem> draws;

    void destroy(Context& context);
};

// Loads a glTF 2.0 file: meshes become device-local vertex/index buffers
// (vertex data addressed via BDA), base-color textures register in the
// bindless table, node transforms are flattened to world space.
Scene loadGltfScene(Context& context, Bindless& bindless,
                    const std::filesystem::path& path);

} // namespace candela

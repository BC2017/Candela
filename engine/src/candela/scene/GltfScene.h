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
};
static_assert(sizeof(Vertex) == 32, "Vertex layout must match mesh.slang");

struct DrawFlags {
    static constexpr uint32_t kAlphaMask = 1u << 0;
};

struct DrawItem {
    glm::mat4 transform{1.0f};
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    uint32_t indexCount = 0;
    VkDeviceAddress vertexAddress = 0;
    uint32_t textureIndex = 0;
    uint32_t flags = 0;
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

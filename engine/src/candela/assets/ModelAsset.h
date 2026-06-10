#pragma once

#include "candela/rhi/Resources.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <filesystem>
#include <string>
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

// One glTF primitive on the GPU, material baked in (separate material assets
// arrive when the editor needs to edit them independently).
struct GpuPrimitive {
    GpuBuffer vertexBuffer;
    GpuBuffer indexBuffer;
    uint32_t indexCount = 0;
    uint32_t vertexCount = 0;
    uint32_t albedoTexture = 0;
    uint32_t normalTexture = 0;
    uint32_t metallicRoughnessTexture = 0;
    uint32_t occlusionTexture = 0;
    uint32_t flags = 0;
    float metallicFactor = 1.0f;
    float roughnessFactor = 1.0f;
    glm::vec4 baseColorFactor{1.0f};
    // Object-space bounds (frustum culling at per-primitive granularity —
    // big merged meshes like Sponza would otherwise cull all-or-nothing).
    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{0.0f};
};

struct GpuMesh {
    std::string name;
    std::vector<GpuPrimitive> primitives;
    // Object-space bounds across all primitives.
    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{0.0f};
    // Bottom-level acceleration structure (one geometry per primitive, in
    // order — ray-query geometry indices map 1:1 to primitives). Null when
    // the device lacks ray tracing.
    VkAccelerationStructureKHR blas = VK_NULL_HANDLE;
    GpuBuffer blasBuffer;
    VkDeviceAddress blasAddress = 0;
};

// glTF node hierarchy template, used to instantiate entities.
struct NodeTemplate {
    std::string name;
    glm::vec3 translation{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
    int parent = -1;    // index into nodes, -1 = root
    int meshIndex = -1; // index into meshes, -1 = no mesh
};

// An imported glTF file: GPU geometry + textures + the node hierarchy.
struct ModelAsset {
    std::vector<GpuMesh> meshes;
    std::vector<GpuImage> textures;
    std::vector<NodeTemplate> nodes;
    // Bounds of the instantiated hierarchy (node transforms applied) —
    // camera framing now, culling later.
    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{0.0f};

    void destroy(Context& context);
};

// Imports a glTF 2.0 file. Safe to call from a job thread (Context and
// Bindless serialize GPU work internally).
ModelAsset importGltfModel(Context& context, Bindless& bindless,
                           const std::filesystem::path& path);

} // namespace candela

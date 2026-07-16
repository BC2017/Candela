#pragma once

#include "candela/assets/AnimationSampling.h"
#include "candela/rhi/Resources.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
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

// Per-vertex skinning attributes. Joint indices are packed two-per-uint (low
// 16 bits = first, high 16 = second) so the skinning compute shader never
// needs 8/16-bit BDA storage. 24 bytes, matching skinning.slang's SkinVertex.
struct SkinVertex {
    uint32_t joints01 = 0; // joint[0] | joint[1] << 16
    uint32_t joints23 = 0; // joint[2] | joint[3] << 16
    float weights[4] = {0.0f, 0.0f, 0.0f, 0.0f};
};
static_assert(sizeof(SkinVertex) == 24,
              "SkinVertex layout must match skinning.slang");

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
    uint32_t emissiveTexture = 0;
    uint32_t flags = 0;
    float metallicFactor = 1.0f;
    float roughnessFactor = 1.0f;
    glm::vec4 baseColorFactor{1.0f};
    // Zero factor disables emission even with the white fallback texture.
    glm::vec3 emissiveFactor{0.0f};
    // Object-space bounds (frustum culling at per-primitive granularity —
    // big merged meshes like Sponza would otherwise cull all-or-nothing).
    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{0.0f};
    // Skinning: present iff the owning mesh's node carried a skinIndex. The
    // renderer pre-skins into a per-frame buffer and refits the BLAS from it.
    bool skinned = false;
    GpuBuffer skinVertexBuffer; // SkinVertex[vertexCount], BDA-addressable
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
    // Any primitive skinned → BLAS built ALLOW_UPDATE (never compacted) and
    // refit per frame from the skinned buffer. blasScratchSize is the UPDATE
    // scratch requirement queried at build time.
    bool skinned = false;
    VkDeviceSize blasScratchSize = 0;
};

// glTF node hierarchy template, used to instantiate entities.
struct NodeTemplate {
    std::string name;
    glm::vec3 translation{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
    int parent = -1;    // index into nodes, -1 = root
    int meshIndex = -1; // index into meshes, -1 = no mesh
    int skinIndex = -1; // index into skins, -1 = not skinned
};

// A glTF skin: which nodes are joints, their inverse-bind matrices (palette
// order), and the optional skeleton root node.
struct Skin {
    std::vector<int> jointNodes;      // indices into ModelAsset::nodes
    std::vector<glm::mat4> inverseBind; // one per joint, same order
    int skeletonRoot = -1;            // node index, -1 if unspecified
};

// An imported glTF file: GPU geometry + textures + the node hierarchy +
// skinning rigs and animation clips.
struct ModelAsset {
    std::vector<GpuMesh> meshes;
    std::vector<GpuImage> textures;
    std::vector<NodeTemplate> nodes;
    std::vector<Skin> skins;
    std::vector<AnimationClip> animations;
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

#include "candela/assets/ModelAsset.h"

#include "candela/rhi/Bindless.h"
#include "candela/rhi/Context.h"

#include <fastgltf/core.hpp>
#include <fastgltf/glm_element_traits.hpp>
#include <fastgltf/tools.hpp>
#include <stb_image.h>

#include <glm/gtc/matrix_transform.hpp>
#include <tracy/Tracy.hpp>

#include <algorithm>
#include <cfloat>
#include <cstring>
#include <unordered_map>

namespace candela {

void ModelAsset::destroy(Context& context) {
    for (GpuMesh& mesh : meshes) {
        if (mesh.blas != VK_NULL_HANDLE) {
            vkDestroyAccelerationStructureKHR(context.device(), mesh.blas,
                                              nullptr);
            destroyBuffer(context, mesh.blasBuffer);
        }
        for (GpuPrimitive& primitive : mesh.primitives) {
            destroyBuffer(context, primitive.vertexBuffer);
            destroyBuffer(context, primitive.indexBuffer);
            if (primitive.skinVertexBuffer.buffer != VK_NULL_HANDLE) {
                destroyBuffer(context, primitive.skinVertexBuffer);
            }
        }
    }
    for (GpuImage& texture : textures) {
        destroyImage(context, texture);
    }
    meshes.clear();
    textures.clear();
    nodes.clear();
    skins.clear();
    animations.clear();
}

namespace {

uint32_t loadTexture(Context& context, Bindless& bindless, ModelAsset& model,
                     const fastgltf::Asset& asset, const fastgltf::Image& image,
                     const std::filesystem::path& baseDir, bool srgb) {
    ZoneScoped;

    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* pixels = nullptr;

    if (const auto* uri = std::get_if<fastgltf::sources::URI>(&image.data)) {
        const std::filesystem::path file =
            baseDir / std::filesystem::path(uri->uri.path());
        pixels = stbi_load(file.string().c_str(), &width, &height, &channels,
                           STBI_rgb_alpha);
    } else if (const auto* view =
                   std::get_if<fastgltf::sources::BufferView>(&image.data)) {
        const auto& bufferView = asset.bufferViews[view->bufferViewIndex];
        const auto& buffer = asset.buffers[bufferView.bufferIndex];
        if (const auto* array =
                std::get_if<fastgltf::sources::Array>(&buffer.data)) {
            pixels = stbi_load_from_memory(
                reinterpret_cast<const stbi_uc*>(array->bytes.data()) +
                    bufferView.byteOffset,
                static_cast<int>(bufferView.byteLength), &width, &height,
                &channels, STBI_rgb_alpha);
        }
    }

    if (pixels == nullptr) {
        CD_WARN("Failed to load glTF image '{}'", image.name);
        return UINT32_MAX;
    }

    GpuImage texture = createTexture2D(context, pixels,
                                       static_cast<uint32_t>(width),
                                       static_cast<uint32_t>(height), srgb);
    stbi_image_free(pixels);

    const uint32_t index = bindless.add(Bindless::Kind::Texture2D,
                                        texture.view,
                                        bindless.defaultSampler());
    model.textures.push_back(texture);
    return index;
}

} // namespace

ModelAsset importGltfModel(Context& context, Bindless& bindless,
                           const std::filesystem::path& path) {
    ZoneScoped;
    ModelAsset model;

    auto data = fastgltf::GltfDataBuffer::FromPath(path);
    CD_ASSERT(data.error() == fastgltf::Error::None,
              "Failed to read glTF file: {}", path.string());

    fastgltf::Parser parser;
    auto loaded = parser.loadGltf(data.get(), path.parent_path(),
                                  fastgltf::Options::LoadExternalBuffers |
                                      fastgltf::Options::GenerateMeshIndices);
    CD_ASSERT(loaded.error() == fastgltf::Error::None,
              "Failed to parse glTF: {} ({})", path.string(),
              fastgltf::getErrorMessage(loaded.error()));
    fastgltf::Asset& asset = loaded.get();

    const uint32_t whitePixel = 0xFFFFFFFFu;
    GpuImage whiteTexture = createTexture2D(context, &whitePixel, 1, 1, true);
    const uint32_t whiteIndex = bindless.add(
        Bindless::Kind::Texture2D, whiteTexture.view, bindless.defaultSampler());
    model.textures.push_back(whiteTexture);

    std::unordered_map<uint64_t, uint32_t> imageToBindless;
    auto textureFor = [&](size_t gltfTextureIndex, bool srgb) -> uint32_t {
        const auto& texture = asset.textures[gltfTextureIndex];
        if (!texture.imageIndex) {
            return whiteIndex;
        }
        const size_t imageIndex = *texture.imageIndex;
        const uint64_t key = (imageIndex << 1) | (srgb ? 1u : 0u);
        if (auto it = imageToBindless.find(key); it != imageToBindless.end()) {
            return it->second;
        }
        const uint32_t loaded_ = loadTexture(context, bindless, model, asset,
                                             asset.images[imageIndex],
                                             path.parent_path(), srgb);
        const uint32_t index = loaded_ == UINT32_MAX ? whiteIndex : loaded_;
        imageToBindless.emplace(key, index);
        return index;
    };

    // --- Meshes (indexed identically to the glTF mesh array) ---
    model.meshes.resize(asset.meshes.size());
    for (size_t meshIndex = 0; meshIndex < asset.meshes.size(); ++meshIndex) {
        const auto& gltfMesh = asset.meshes[meshIndex];
        GpuMesh& mesh = model.meshes[meshIndex];
        mesh.name = gltfMesh.name;

        for (const auto& primitive : gltfMesh.primitives) {
            const auto* positionAttr = primitive.findAttribute("POSITION");
            if (positionAttr == primitive.attributes.end()) {
                continue;
            }
            const auto& positionAccessor =
                asset.accessors[positionAttr->accessorIndex];

            std::vector<Vertex> vertices(positionAccessor.count);
            glm::vec3 primMin{FLT_MAX};
            glm::vec3 primMax{-FLT_MAX};
            fastgltf::iterateAccessorWithIndex<glm::vec3>(
                asset, positionAccessor, [&](glm::vec3 value, size_t index) {
                    vertices[index].position = value;
                    vertices[index].normal = {0.0f, 1.0f, 0.0f};
                    vertices[index].tangent = {1.0f, 0.0f, 0.0f, 1.0f};
                    primMin = glm::min(primMin, value);
                    primMax = glm::max(primMax, value);
                });
            if (mesh.primitives.empty()) {
                mesh.boundsMin = primMin;
                mesh.boundsMax = primMax;
            } else {
                mesh.boundsMin = glm::min(mesh.boundsMin, primMin);
                mesh.boundsMax = glm::max(mesh.boundsMax, primMax);
            }

            // glTF requires every attribute accessor to share POSITION's count,
            // but fastgltf does not enforce it; a malformed asset with a larger
            // secondary accessor would otherwise write past `vertices`. Guard
            // each write (a no-op for conformant assets).
            if (const auto* normalAttr = primitive.findAttribute("NORMAL");
                normalAttr != primitive.attributes.end()) {
                fastgltf::iterateAccessorWithIndex<glm::vec3>(
                    asset, asset.accessors[normalAttr->accessorIndex],
                    [&](glm::vec3 value, size_t index) {
                        if (index < vertices.size())
                            vertices[index].normal = value;
                    });
            }
            if (const auto* uvAttr = primitive.findAttribute("TEXCOORD_0");
                uvAttr != primitive.attributes.end()) {
                fastgltf::iterateAccessorWithIndex<glm::vec2>(
                    asset, asset.accessors[uvAttr->accessorIndex],
                    [&](glm::vec2 value, size_t index) {
                        if (index < vertices.size())
                            vertices[index].uv = value;
                    });
            }
            bool hasTangents = false;
            if (const auto* tangentAttr = primitive.findAttribute("TANGENT");
                tangentAttr != primitive.attributes.end()) {
                hasTangents = true;
                fastgltf::iterateAccessorWithIndex<glm::vec4>(
                    asset, asset.accessors[tangentAttr->accessorIndex],
                    [&](glm::vec4 value, size_t index) {
                        if (index < vertices.size())
                            vertices[index].tangent = value;
                    });
            }

            // Skinning attributes (JOINTS_0 / WEIGHTS_0). Both must be present
            // for the primitive to skin. Joint indices pack two per uint.
            bool primSkinned = false;
            std::vector<SkinVertex> skinVerts;
            if (const auto* jointsAttr = primitive.findAttribute("JOINTS_0");
                jointsAttr != primitive.attributes.end()) {
                if (const auto* weightsAttr =
                        primitive.findAttribute("WEIGHTS_0");
                    weightsAttr != primitive.attributes.end()) {
                    primSkinned = true;
                    skinVerts.assign(positionAccessor.count, SkinVertex{});
                    fastgltf::iterateAccessorWithIndex<glm::u16vec4>(
                        asset, asset.accessors[jointsAttr->accessorIndex],
                        [&](glm::u16vec4 j, size_t index) {
                            skinVerts[index].joints01 =
                                uint32_t(j.x) | (uint32_t(j.y) << 16);
                            skinVerts[index].joints23 =
                                uint32_t(j.z) | (uint32_t(j.w) << 16);
                        });
                    fastgltf::iterateAccessorWithIndex<glm::vec4>(
                        asset, asset.accessors[weightsAttr->accessorIndex],
                        [&](glm::vec4 w, size_t index) {
                            // glTF weights need not sum to 1; renormalize.
                            const float s = w.x + w.y + w.z + w.w;
                            if (s > 0.0f) {
                                w /= s;
                            }
                            skinVerts[index].weights[0] = w.x;
                            skinVerts[index].weights[1] = w.y;
                            skinVerts[index].weights[2] = w.z;
                            skinVerts[index].weights[3] = w.w;
                        });
                }
            }

            CD_ASSERT(primitive.indicesAccessor.has_value(),
                      "GenerateMeshIndices should guarantee indices");
            const auto& indexAccessor =
                asset.accessors[*primitive.indicesAccessor];
            std::vector<uint32_t> indices(indexAccessor.count);
            fastgltf::iterateAccessorWithIndex<uint32_t>(
                asset, indexAccessor,
                [&](uint32_t value, size_t index) { indices[index] = value; });

            // BLAS builds read both buffers; reflections fetch indices via
            // device address for hit shading.
            const VkBufferUsageFlags rtUsage =
                context.rayTracingSupported()
                    ? VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR
                    : 0;
            GpuPrimitive gpu{};
            gpu.vertexBuffer = createBufferWithData(
                context, std::as_bytes(std::span(vertices)),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | rtUsage);
            gpu.indexBuffer = createBufferWithData(
                context, std::as_bytes(std::span(indices)),
                VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | rtUsage);
            gpu.indexCount = static_cast<uint32_t>(indices.size());
            gpu.vertexCount = static_cast<uint32_t>(vertices.size());
            gpu.boundsMin = primMin;
            gpu.boundsMax = primMax;
            gpu.albedoTexture = whiteIndex;
            gpu.normalTexture = whiteIndex;
            gpu.metallicRoughnessTexture = whiteIndex;
            gpu.occlusionTexture = whiteIndex;
            gpu.metallicFactor = 0.0f;
            gpu.roughnessFactor = 1.0f;

            if (primSkinned) {
                gpu.skinned = true;
                gpu.skinVertexBuffer = createBufferWithData(
                    context, std::as_bytes(std::span(skinVerts)),
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
                mesh.skinned = true;
            }

            if (primitive.materialIndex) {
                const auto& material =
                    asset.materials[*primitive.materialIndex];
                const auto& pbr = material.pbrData;
                gpu.baseColorFactor = {pbr.baseColorFactor[0],
                                       pbr.baseColorFactor[1],
                                       pbr.baseColorFactor[2],
                                       pbr.baseColorFactor[3]};
                gpu.metallicFactor = pbr.metallicFactor;
                gpu.roughnessFactor = pbr.roughnessFactor;
                if (pbr.baseColorTexture) {
                    gpu.albedoTexture =
                        textureFor(pbr.baseColorTexture->textureIndex, true);
                }
                if (pbr.metallicRoughnessTexture) {
                    gpu.metallicRoughnessTexture = textureFor(
                        pbr.metallicRoughnessTexture->textureIndex, false);
                }
                if (material.normalTexture && hasTangents) {
                    gpu.normalTexture =
                        textureFor(material.normalTexture->textureIndex, false);
                    gpu.flags |= DrawFlags::kHasNormalMap;
                }
                if (material.occlusionTexture) {
                    gpu.occlusionTexture = textureFor(
                        material.occlusionTexture->textureIndex, false);
                }
                gpu.emissiveFactor = {material.emissiveFactor[0],
                                      material.emissiveFactor[1],
                                      material.emissiveFactor[2]};
                if (material.emissiveTexture) {
                    gpu.emissiveTexture = textureFor(
                        material.emissiveTexture->textureIndex, true);
                    // glTF defaults emissiveFactor to zero; a texture with a
                    // zero factor would never show.
                    if (gpu.emissiveFactor == glm::vec3{0.0f}) {
                        gpu.emissiveFactor = glm::vec3{1.0f};
                    }
                }
                if (material.alphaMode == fastgltf::AlphaMode::Mask) {
                    gpu.flags |= DrawFlags::kAlphaMask;
                }
            }

            mesh.primitives.push_back(gpu);
        }
    }

    // --- Bottom-level acceleration structures (one per mesh) ---
    if (context.rayTracingSupported()) {
        VkDeviceSize blasBuiltBytes = 0;
        VkDeviceSize blasCompactedBytes = 0;
        for (GpuMesh& mesh : model.meshes) {
            if (mesh.primitives.empty()) {
                continue;
            }
            std::vector<VkAccelerationStructureGeometryKHR> geometries;
            std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
            std::vector<uint32_t> primitiveCounts;
            for (const GpuPrimitive& primitive : mesh.primitives) {
                VkAccelerationStructureGeometryKHR geometry{};
                geometry.sType =
                    VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
                geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
                // Alpha-masked geometry stays non-opaque so shadow rays can
                // inspect the albedo alpha at the candidate hit.
                geometry.flags = (primitive.flags & DrawFlags::kAlphaMask)
                                     ? 0
                                     : VK_GEOMETRY_OPAQUE_BIT_KHR;
                auto& triangles = geometry.geometry.triangles;
                triangles.sType =
                    VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
                triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
                triangles.vertexData.deviceAddress =
                    primitive.vertexBuffer.deviceAddress;
                triangles.vertexStride = sizeof(Vertex);
                triangles.maxVertex = primitive.vertexCount - 1;
                triangles.indexType = VK_INDEX_TYPE_UINT32;
                triangles.indexData.deviceAddress =
                    primitive.indexBuffer.deviceAddress;
                geometries.push_back(geometry);

                VkAccelerationStructureBuildRangeInfoKHR range{};
                range.primitiveCount = primitive.indexCount / 3;
                ranges.push_back(range);
                primitiveCounts.push_back(range.primitiveCount);
            }

            // Skinned meshes are refit (mode=UPDATE) from the per-frame
            // skinned vertex buffer, which requires ALLOW_UPDATE and rules out
            // compaction (a compacted AS cannot be updated).
            const bool allowUpdate = mesh.skinned;

            VkAccelerationStructureBuildGeometryInfoKHR buildInfo{};
            buildInfo.sType =
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
            buildInfo.type =
                VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            buildInfo.flags =
                VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                (allowUpdate
                     ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR
                     : VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR);
            buildInfo.mode =
                VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            buildInfo.geometryCount =
                static_cast<uint32_t>(geometries.size());
            buildInfo.pGeometries = geometries.data();

            VkAccelerationStructureBuildSizesInfoKHR sizes{};
            sizes.sType =
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
            vkGetAccelerationStructureBuildSizesKHR(
                context.device(),
                VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo,
                primitiveCounts.data(), &sizes);
            mesh.blasScratchSize = sizes.updateScratchSize;

            mesh.blasBuffer = createBuffer(
                context, sizes.accelerationStructureSize,
                VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);

            VkAccelerationStructureCreateInfoKHR createInfo{};
            createInfo.sType =
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
            createInfo.buffer = mesh.blasBuffer.buffer;
            createInfo.size = sizes.accelerationStructureSize;
            createInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            VK_CHECK(vkCreateAccelerationStructureKHR(
                context.device(), &createInfo, nullptr, &mesh.blas));

            const uint32_t alignment = context.scratchAlignment();
            GpuBuffer scratch = createBuffer(
                context, sizes.buildScratchSize + alignment,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);

            buildInfo.dstAccelerationStructure = mesh.blas;
            buildInfo.scratchData.deviceAddress =
                (scratch.deviceAddress + alignment - 1) & ~VkDeviceAddress(alignment - 1);

            const VkAccelerationStructureBuildRangeInfoKHR* rangePtr =
                ranges.data();
            context.immediateSubmit([&](VkCommandBuffer cmd) {
                vkCmdBuildAccelerationStructuresKHR(cmd, 1, &buildInfo,
                                                    &rangePtr);
            });
            destroyBuffer(context, scratch);

            if (!allowUpdate) {
                // --- Compaction: copy into a right-sized structure (~half the
                // memory on typical meshes) and drop the build-sized one. ---
                VkQueryPoolCreateInfo queryInfo{};
                queryInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
                queryInfo.queryType =
                    VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR;
                queryInfo.queryCount = 1;
                VkQueryPool query = VK_NULL_HANDLE;
                VK_CHECK(vkCreateQueryPool(context.device(), &queryInfo, nullptr,
                                           &query));
                context.immediateSubmit([&](VkCommandBuffer cmd) {
                    vkCmdResetQueryPool(cmd, query, 0, 1);
                    vkCmdWriteAccelerationStructuresPropertiesKHR(
                        cmd, 1, &mesh.blas,
                        VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR,
                        query, 0);
                });
                VkDeviceSize compactedSize = 0;
                VK_CHECK(vkGetQueryPoolResults(
                    context.device(), query, 0, 1, sizeof(compactedSize),
                    &compactedSize, sizeof(compactedSize),
                    VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
                vkDestroyQueryPool(context.device(), query, nullptr);

                GpuBuffer compactBuffer = createBuffer(
                    context, compactedSize,
                    VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
                VkAccelerationStructureCreateInfoKHR compactInfo{};
                compactInfo.sType =
                    VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
                compactInfo.buffer = compactBuffer.buffer;
                compactInfo.size = compactedSize;
                compactInfo.type =
                    VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
                VkAccelerationStructureKHR compactBlas = VK_NULL_HANDLE;
                VK_CHECK(vkCreateAccelerationStructureKHR(
                    context.device(), &compactInfo, nullptr, &compactBlas));
                context.immediateSubmit([&](VkCommandBuffer cmd) {
                    VkCopyAccelerationStructureInfoKHR copy{};
                    copy.sType =
                        VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR;
                    copy.src = mesh.blas;
                    copy.dst = compactBlas;
                    copy.mode =
                        VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;
                    vkCmdCopyAccelerationStructureKHR(cmd, &copy);
                });
                vkDestroyAccelerationStructureKHR(context.device(), mesh.blas,
                                                  nullptr);
                destroyBuffer(context, mesh.blasBuffer);
                mesh.blas = compactBlas;
                mesh.blasBuffer = compactBuffer;
                blasBuiltBytes += sizes.accelerationStructureSize;
                blasCompactedBytes += compactedSize;
            }

            VkAccelerationStructureDeviceAddressInfoKHR addressInfo{};
            addressInfo.sType =
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
            addressInfo.accelerationStructure = mesh.blas;
            mesh.blasAddress = vkGetAccelerationStructureDeviceAddressKHR(
                context.device(), &addressInfo);
        }
        if (blasBuiltBytes > 0) {
            CD_INFO("BLAS compaction: {:.2f} MB -> {:.2f} MB",
                    static_cast<double>(blasBuiltBytes) / (1024.0 * 1024.0),
                    static_cast<double>(blasCompactedBytes) /
                        (1024.0 * 1024.0));
        }
    }

    // --- Node hierarchy (TRS preserved for entity instantiation) ---
    model.nodes.resize(asset.nodes.size());
    for (size_t nodeIndex = 0; nodeIndex < asset.nodes.size(); ++nodeIndex) {
        const auto& gltfNode = asset.nodes[nodeIndex];
        NodeTemplate& node = model.nodes[nodeIndex];
        node.name = gltfNode.name;
        if (gltfNode.meshIndex) {
            node.meshIndex = static_cast<int>(*gltfNode.meshIndex);
        }
        if (gltfNode.skinIndex) {
            node.skinIndex = static_cast<int>(*gltfNode.skinIndex);
        }
        if (const auto* trs =
                std::get_if<fastgltf::TRS>(&gltfNode.transform)) {
            node.translation = {trs->translation[0], trs->translation[1],
                                trs->translation[2]};
            node.rotation = glm::quat(trs->rotation[3], trs->rotation[0],
                                      trs->rotation[1], trs->rotation[2]);
            node.scale = {trs->scale[0], trs->scale[1], trs->scale[2]};
        } else if (const auto* matrix = std::get_if<fastgltf::math::fmat4x4>(
                       &gltfNode.transform)) {
            // Decompose: translation from column 3, scale from column
            // lengths, rotation from the normalized basis.
            glm::mat4 m;
            for (int col = 0; col < 4; ++col) {
                for (int row = 0; row < 4; ++row) {
                    m[col][row] = (*matrix)[static_cast<size_t>(col)][
                        static_cast<size_t>(row)];
                }
            }
            node.translation = glm::vec3(m[3]);
            node.scale = {glm::length(glm::vec3(m[0])),
                          glm::length(glm::vec3(m[1])),
                          glm::length(glm::vec3(m[2]))};
            glm::mat3 rotation(glm::vec3(m[0]) / node.scale.x,
                               glm::vec3(m[1]) / node.scale.y,
                               glm::vec3(m[2]) / node.scale.z);
            node.rotation = glm::quat_cast(rotation);
        }
        for (size_t child : gltfNode.children) {
            model.nodes[child].parent = static_cast<int>(nodeIndex);
        }
    }

    // --- Skins: joint node lists + inverse-bind matrices (palette order) ---
    model.skins.resize(asset.skins.size());
    for (size_t s = 0; s < asset.skins.size(); ++s) {
        const auto& gltfSkin = asset.skins[s];
        Skin& skin = model.skins[s];
        skin.skeletonRoot =
            gltfSkin.skeleton ? static_cast<int>(*gltfSkin.skeleton) : -1;
        skin.jointNodes.reserve(gltfSkin.joints.size());
        for (size_t joint : gltfSkin.joints) {
            skin.jointNodes.push_back(static_cast<int>(joint));
        }
        skin.inverseBind.assign(skin.jointNodes.size(), glm::mat4(1.0f));
        if (gltfSkin.inverseBindMatrices) {
            fastgltf::iterateAccessorWithIndex<glm::mat4>(
                asset, asset.accessors[*gltfSkin.inverseBindMatrices],
                [&](glm::mat4 m, size_t index) {
                    if (index < skin.inverseBind.size()) {
                        skin.inverseBind[index] = m;
                    }
                });
        }
    }

    // --- Animation clips: TRS channels sampled by AnimationSampling.h ---
    model.animations.resize(asset.animations.size());
    for (size_t a = 0; a < asset.animations.size(); ++a) {
        const auto& gltfAnim = asset.animations[a];
        AnimationClip& clip = model.animations[a];
        clip.name = gltfAnim.name;
        for (const auto& ch : gltfAnim.channels) {
            if (!ch.nodeIndex) {
                continue;
            }
            if (ch.path == fastgltf::AnimationPath::Weights) {
                continue; // morph targets deferred
            }
            const auto& sampler = gltfAnim.samplers[ch.samplerIndex];
            AnimationChannel out;
            out.targetNode = static_cast<int>(*ch.nodeIndex);
            out.path = ch.path == fastgltf::AnimationPath::Translation
                           ? AnimationChannel::Path::Translation
                       : ch.path == fastgltf::AnimationPath::Scale
                           ? AnimationChannel::Path::Scale
                           : AnimationChannel::Path::Rotation;
            const bool cubic = sampler.interpolation ==
                               fastgltf::AnimationInterpolation::CubicSpline;
            out.interp = sampler.interpolation ==
                                 fastgltf::AnimationInterpolation::Step
                             ? AnimationChannel::Interp::Step
                             : AnimationChannel::Interp::Linear;

            fastgltf::iterateAccessor<float>(
                asset, asset.accessors[sampler.inputAccessor],
                [&](float t) { out.times.push_back(t); });

            // CubicSpline outputs 3 elements per key (in-tangent, vertex,
            // out-tangent); keep only the vertex and treat the track as
            // linear (correct cubic Hermite is a follow-up).
            const size_t keyCount = out.times.size();
            if (out.path == AnimationChannel::Path::Rotation) {
                std::vector<glm::vec4> raw;
                fastgltf::iterateAccessor<glm::vec4>(
                    asset, asset.accessors[sampler.outputAccessor],
                    [&](glm::vec4 q) { raw.push_back(q); });
                if (cubic && keyCount > 0 && raw.size() >= keyCount * 3) {
                    out.values.reserve(keyCount);
                    for (size_t k = 0; k < keyCount; ++k) {
                        out.values.push_back(raw[k * 3 + 1]);
                    }
                } else {
                    out.values = std::move(raw);
                }
            } else {
                std::vector<glm::vec3> raw;
                fastgltf::iterateAccessor<glm::vec3>(
                    asset, asset.accessors[sampler.outputAccessor],
                    [&](glm::vec3 v) { raw.push_back(v); });
                if (cubic && keyCount > 0 && raw.size() >= keyCount * 3) {
                    out.values.reserve(keyCount);
                    for (size_t k = 0; k < keyCount; ++k) {
                        out.values.push_back(glm::vec4(raw[k * 3 + 1], 0.0f));
                    }
                } else {
                    out.values.reserve(raw.size());
                    for (const glm::vec3& v : raw) {
                        out.values.push_back(glm::vec4(v, 0.0f));
                    }
                }
            }

            if (!out.times.empty()) {
                clip.duration = std::max(clip.duration, out.times.back());
            }
            clip.channels.push_back(std::move(out));
        }
    }

    // --- Model bounds: per-mesh AABBs through the node global transforms ---
    {
        std::vector<glm::mat4> globals(model.nodes.size(), glm::mat4{1.0f});
        std::vector<bool> computed(model.nodes.size(), false);
        auto globalOf = [&](auto&& self, size_t index) -> const glm::mat4& {
            if (computed[index]) {
                return globals[index];
            }
            const NodeTemplate& node = model.nodes[index];
            glm::mat4 local = glm::translate(glm::mat4(1.0f), node.translation);
            local *= glm::mat4_cast(node.rotation);
            local = glm::scale(local, node.scale);
            // Mark resolved BEFORE recursing so a malformed glTF with a node
            // parent cycle terminates against this placeholder instead of
            // recursing until the stack overflows.
            computed[index] = true;
            globals[index] = local;
            if (node.parent >= 0) {
                globals[index] =
                    self(self, static_cast<size_t>(node.parent)) * local;
            }
            return globals[index];
        };

        bool any = false;
        glm::vec3 modelMin{FLT_MAX};
        glm::vec3 modelMax{-FLT_MAX};
        for (size_t i = 0; i < model.nodes.size(); ++i) {
            const int meshIndex = model.nodes[i].meshIndex;
            if (meshIndex < 0) {
                continue;
            }
            const GpuMesh& mesh = model.meshes[static_cast<size_t>(meshIndex)];
            if (mesh.primitives.empty()) {
                continue;
            }
            const glm::mat4& global = globalOf(globalOf, i);
            for (int corner = 0; corner < 8; ++corner) {
                const glm::vec3 p{
                    (corner & 1) ? mesh.boundsMax.x : mesh.boundsMin.x,
                    (corner & 2) ? mesh.boundsMax.y : mesh.boundsMin.y,
                    (corner & 4) ? mesh.boundsMax.z : mesh.boundsMin.z};
                const glm::vec3 world = glm::vec3(global * glm::vec4(p, 1.0f));
                modelMin = glm::min(modelMin, world);
                modelMax = glm::max(modelMax, world);
            }
            any = true;
        }
        if (any) {
            model.boundsMin = modelMin;
            model.boundsMax = modelMax;
        }
    }

    size_t primitiveCount = 0;
    for (const GpuMesh& mesh : model.meshes) {
        primitiveCount += mesh.primitives.size();
    }
    CD_INFO("Imported {}: {} meshes ({} primitives), {} textures, {} nodes, "
            "{} skins, {} animations",
            path.filename().string(), model.meshes.size(), primitiveCount,
            model.textures.size(), model.nodes.size(), model.skins.size(),
            model.animations.size());
    return model;
}

} // namespace candela

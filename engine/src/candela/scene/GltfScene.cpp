#include "candela/scene/GltfScene.h"

#include "candela/rhi/Bindless.h"
#include "candela/rhi/Context.h"

#include <fastgltf/core.hpp>
#include <fastgltf/glm_element_traits.hpp>
#include <fastgltf/tools.hpp>
#include <stb_image.h>

#include <tracy/Tracy.hpp>

#include <unordered_map>

namespace candela {

void Scene::destroy(Context& context) {
    for (GpuBuffer& buffer : buffers) {
        destroyBuffer(context, buffer);
    }
    for (GpuImage& texture : textures) {
        destroyImage(context, texture);
    }
    buffers.clear();
    textures.clear();
    draws.clear();
}

namespace {

// Loads a glTF image source into a bindless-registered GPU texture.
// Returns UINT32_MAX on failure.
uint32_t loadTexture(Context& context, Bindless& bindless, Scene& scene,
                     const fastgltf::Asset& asset, const fastgltf::Image& image,
                     const std::filesystem::path& baseDir) {
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
                                       static_cast<uint32_t>(height),
                                       /*srgb=*/true);
    stbi_image_free(pixels);

    const uint32_t index = bindless.add(texture.view, bindless.defaultSampler());
    scene.textures.push_back(texture);
    return index;
}

} // namespace

Scene loadGltfScene(Context& context, Bindless& bindless,
                    const std::filesystem::path& path) {
    ZoneScoped;
    Scene scene;

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

    // 1x1 white fallback at whatever index it lands on; used by untextured
    // materials and failed loads.
    const uint32_t whitePixel = 0xFFFFFFFFu;
    GpuImage whiteTexture = createTexture2D(context, &whitePixel, 1, 1, true);
    const uint32_t whiteIndex =
        bindless.add(whiteTexture.view, bindless.defaultSampler());
    scene.textures.push_back(whiteTexture);

    // Lazily load base-color textures (Sponza ships normal/metallic maps too;
    // those wait for Phase 2's material model).
    std::unordered_map<size_t, uint32_t> imageToBindless;
    auto textureFor = [&](size_t gltfTextureIndex) -> uint32_t {
        const auto& texture = asset.textures[gltfTextureIndex];
        if (!texture.imageIndex) {
            return whiteIndex;
        }
        const size_t imageIndex = *texture.imageIndex;
        if (auto it = imageToBindless.find(imageIndex);
            it != imageToBindless.end()) {
            return it->second;
        }
        const uint32_t loaded_ = loadTexture(context, bindless, scene, asset,
                                             asset.images[imageIndex],
                                             path.parent_path());
        const uint32_t index = loaded_ == UINT32_MAX ? whiteIndex : loaded_;
        imageToBindless.emplace(imageIndex, index);
        return index;
    };

    // Per-primitive GPU mesh data, deduplicated across nodes by primitive
    // pointer identity.
    struct GpuPrimitive {
        VkBuffer indexBuffer;
        uint32_t indexCount;
        VkDeviceAddress vertexAddress;
        uint32_t textureIndex;
        uint32_t flags;
        glm::vec4 baseColorFactor;
    };
    std::unordered_map<const fastgltf::Primitive*, GpuPrimitive> gpuPrimitives;

    auto uploadPrimitive = [&](const fastgltf::Primitive& primitive)
        -> const GpuPrimitive* {
        if (auto it = gpuPrimitives.find(&primitive);
            it != gpuPrimitives.end()) {
            return &it->second;
        }

        const auto* positionAttr = primitive.findAttribute("POSITION");
        if (positionAttr == primitive.attributes.end()) {
            return nullptr;
        }
        const auto& positionAccessor =
            asset.accessors[positionAttr->accessorIndex];

        std::vector<Vertex> vertices(positionAccessor.count);
        fastgltf::iterateAccessorWithIndex<glm::vec3>(
            asset, positionAccessor, [&](glm::vec3 value, size_t index) {
                vertices[index].position = value;
                vertices[index].normal = {0.0f, 1.0f, 0.0f};
            });

        if (const auto* normalAttr = primitive.findAttribute("NORMAL");
            normalAttr != primitive.attributes.end()) {
            fastgltf::iterateAccessorWithIndex<glm::vec3>(
                asset, asset.accessors[normalAttr->accessorIndex],
                [&](glm::vec3 value, size_t index) {
                    vertices[index].normal = value;
                });
        }
        if (const auto* uvAttr = primitive.findAttribute("TEXCOORD_0");
            uvAttr != primitive.attributes.end()) {
            fastgltf::iterateAccessorWithIndex<glm::vec2>(
                asset, asset.accessors[uvAttr->accessorIndex],
                [&](glm::vec2 value, size_t index) {
                    vertices[index].uv = value;
                });
        }

        CD_ASSERT(primitive.indicesAccessor.has_value(),
                  "GenerateMeshIndices should guarantee indices");
        const auto& indexAccessor =
            asset.accessors[*primitive.indicesAccessor];
        std::vector<uint32_t> indices(indexAccessor.count);
        fastgltf::iterateAccessorWithIndex<uint32_t>(
            asset, indexAccessor,
            [&](uint32_t value, size_t index) { indices[index] = value; });

        GpuBuffer vertexBuffer = createBufferWithData(
            context,
            std::as_bytes(std::span(vertices)),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
        GpuBuffer indexBuffer = createBufferWithData(
            context, std::as_bytes(std::span(indices)),
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        scene.buffers.push_back(vertexBuffer);
        scene.buffers.push_back(indexBuffer);

        GpuPrimitive gpu{};
        gpu.indexBuffer = indexBuffer.buffer;
        gpu.indexCount = static_cast<uint32_t>(indices.size());
        gpu.vertexAddress = vertexBuffer.deviceAddress;
        gpu.textureIndex = whiteIndex;
        gpu.baseColorFactor = glm::vec4(1.0f);

        if (primitive.materialIndex) {
            const auto& material = asset.materials[*primitive.materialIndex];
            const auto& factor = material.pbrData.baseColorFactor;
            gpu.baseColorFactor = {factor[0], factor[1], factor[2], factor[3]};
            if (material.pbrData.baseColorTexture) {
                gpu.textureIndex =
                    textureFor(material.pbrData.baseColorTexture->textureIndex);
            }
            if (material.alphaMode == fastgltf::AlphaMode::Mask) {
                gpu.flags |= DrawFlags::kAlphaMask;
            }
        }

        return &gpuPrimitives.emplace(&primitive, gpu).first->second;
    };

    // Flatten the node hierarchy of the default scene.
    const size_t sceneIndex = asset.defaultScene.value_or(0);
    fastgltf::iterateSceneNodes(
        asset, sceneIndex, fastgltf::math::fmat4x4(),
        [&](fastgltf::Node& node, const fastgltf::math::fmat4x4& matrix) {
            if (!node.meshIndex) {
                return;
            }
            // Both are column-major; copy by element (memcpy into a glm type
            // trips GCC's -Wclass-memaccess).
            glm::mat4 transform;
            for (glm::length_t col = 0; col < 4; ++col) {
                for (glm::length_t row = 0; row < 4; ++row) {
                    transform[col][row] = matrix[static_cast<size_t>(col)][
                        static_cast<size_t>(row)];
                }
            }

            for (const auto& primitive :
                 asset.meshes[*node.meshIndex].primitives) {
                const GpuPrimitive* gpu = uploadPrimitive(primitive);
                if (gpu == nullptr) {
                    continue;
                }
                DrawItem draw;
                draw.transform = transform;
                draw.indexBuffer = gpu->indexBuffer;
                draw.indexCount = gpu->indexCount;
                draw.vertexAddress = gpu->vertexAddress;
                draw.textureIndex = gpu->textureIndex;
                draw.flags = gpu->flags;
                draw.baseColorFactor = gpu->baseColorFactor;
                scene.draws.push_back(draw);
            }
        });

    CD_INFO("Loaded {}: {} draws, {} textures, {} buffers", path.string(),
            scene.draws.size(), scene.textures.size(), scene.buffers.size());
    return scene;
}

} // namespace candela

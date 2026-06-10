#include "candela/renderer/Renderer.h"

#include "candela/assets/AssetRegistry.h"
#include "candela/platform/Window.h"
#include "candela/renderer/Camera.h"
#include "candela/renderer/Cascades.h"
#include "candela/scene/World.h"

#include <stb_image_write.h>
#include <tracy/Tracy.hpp>

#include <algorithm>
#include <cstring>
#include <vector>

namespace candela {

namespace {

// GPU-visible structs. Layouts must match common.slang exactly.
struct PointLightGPU {
    glm::vec4 positionRadius;
    glm::vec4 colorIntensity;
};

struct FrameConstants {
    glm::mat4 viewProjection;
    glm::mat4 invViewProjection;
    glm::mat4 cascadeViewProjection[4];
    glm::vec4 cascadeSplits;
    glm::vec4 cameraPosition;
    glm::vec4 sunDirection; // xyz to sun, w intensity
    glm::vec4 sunColor;
    uint32_t pointLightCount;
    uint32_t pad0, pad1, pad2;
    PointLightGPU pointLights[8];
    uint32_t rtFlags; // bit 0 shadows, 1 AO, 2 reflections
    uint32_t tlasSlot;
    uint32_t pad3, pad4;
    VkDeviceAddress instanceData; // InstanceDataGPU*
    uint64_t pad5;
    // Temporal: jitter-free matrices for velocity/reprojection.
    glm::mat4 viewProjectionNoJitter;
    glm::mat4 prevViewProjectionNoJitter;
    glm::vec2 jitter; // NDC offset applied to the raster projection
    uint32_t frameIndex;
    uint32_t taaEnabled;
};
static_assert(sizeof(FrameConstants) == 896,
              "FrameConstants must match common.slang");

// Per-TLAS-geometry shading data for ray-query hit evaluation.
// Must match InstanceData in common.slang.
struct InstanceDataGPU {
    VkDeviceAddress vertices;
    VkDeviceAddress indices;
    uint32_t albedoTexture;
    uint32_t flags;
    float metallicFactor;
    float roughnessFactor;
    glm::vec4 baseColorFactor;
};
static_assert(sizeof(InstanceDataGPU) == 48,
              "InstanceDataGPU must match common.slang");

constexpr uint32_t kRTShadowsBit = 1;
constexpr uint32_t kRTAOBit = 2;
constexpr uint32_t kRTReflectionsBit = 4;

constexpr VkShaderStageFlags kPushStages = VK_SHADER_STAGE_VERTEX_BIT |
                                           VK_SHADER_STAGE_FRAGMENT_BIT |
                                           VK_SHADER_STAGE_COMPUTE_BIT;

struct GBufferPush {
    glm::mat4 model;
    VkDeviceAddress vertices;
    VkDeviceAddress frame;
    uint32_t albedoTexture;
    uint32_t normalTexture;
    uint32_t metallicRoughnessTexture;
    uint32_t occlusionTexture;
    uint32_t flags;
    float metallicFactor;
    float roughnessFactor;
    uint32_t entityId; // entt id + 1; 0 = no entity (picking)
    glm::vec4 baseColorFactor;
};
static_assert(sizeof(GBufferPush) == 128, "GBufferPush must fit push budget");

struct ShadowPush {
    glm::mat4 lightViewProjModel;
    VkDeviceAddress vertices;
};

struct LightingPush {
    VkDeviceAddress frame;
    uint32_t gbAlbedo;
    uint32_t gbNormal;
    uint32_t gbMaterial;
    uint32_t gbDepth;
    uint32_t shadowCascades;
    uint32_t irradianceCube;
    uint32_t prefilteredCube;
    uint32_t brdfLut;
    glm::vec2 invResolution;
    float iblIntensity;
    uint32_t prefilteredMips;
    uint32_t debugView;
    uint32_t reflectionTexture; // accumulated RT reflections (bit 2)
    uint32_t aoTexture;         // accumulated RT AO (bit 1)
};

struct AOPush {
    VkDeviceAddress frame;
    uint32_t gbNormal;
    uint32_t gbDepth;
    uint32_t outputImage;
    uint32_t pad; // slang aligns uint2 to 8 — keep resolution at offset 24
    glm::uvec2 resolution;
};
static_assert(offsetof(AOPush, resolution) == 24,
              "AOPush must match ao.slang");

struct TemporalPush {
    uint32_t rawTexture;
    uint32_t historyTexture;
    uint32_t outputImage;
    uint32_t velocityTexture;
    glm::uvec2 resolution;
    float blendFactor;
    uint32_t historyValid;
};

struct ReflectionsPush {
    VkDeviceAddress frame;
    uint32_t gbNormal;
    uint32_t gbMaterial;
    uint32_t gbDepth;
    uint32_t outputImage; // storage slot
    glm::uvec2 resolution;
    uint32_t prefilteredCube;
    uint32_t irradianceCube;
    float iblIntensity;
    uint32_t prefilteredMips;
};

struct BloomPush {
    uint32_t sourceTexture;
    uint32_t isFirstDownsample;
    glm::vec2 sourceInvResolution;
    glm::vec2 targetInvResolution;
};

struct TonemapPush {
    uint32_t hdrTexture;
    uint32_t bloomTexture;
    float exposure;
    float bloomStrength;
    glm::vec2 invResolution;
};

void setFullViewport(VkCommandBuffer cmd, VkExtent2D extent) {
    // Negative height: Y up, matching glTF/GL conventions.
    VkViewport viewport{};
    viewport.y = static_cast<float>(extent.height);
    viewport.width = static_cast<float>(extent.width);
    viewport.height = -static_cast<float>(extent.height);
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor{{0, 0}, extent};
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

} // namespace

Renderer::Renderer(Window& window) : m_window(window) {
    m_context = std::make_unique<Context>(window);

    const glm::uvec2 fb = window.framebufferSize();
    m_swapchain = std::make_unique<Swapchain>(*m_context, fb.x, fb.y);
    m_bindless = std::make_unique<Bindless>(*m_context);
    m_renderGraph = std::make_unique<RenderGraph>(*m_context);
    m_shaderCache = std::make_unique<ShaderCache>(m_context->device());

    createFrameData();
    createPresentSemaphores();

#ifdef TRACY_ENABLE
    m_tracyCtx = TracyVkContext(m_context->physicalDevice(),
                                m_context->device(),
                                m_context->graphicsQueue(),
                                m_context->immediateCommandBuffer());
#endif

    // Shared graphics pipeline layout: bindless set + 128B push constants.
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = kPushStages;
    pushRange.offset = 0;
    pushRange.size = 128;

    VkDescriptorSetLayout setLayout = m_bindless->layout();
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &setLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    VK_CHECK(vkCreatePipelineLayout(m_context->device(), &layoutInfo, nullptr,
                                    &m_pipelineLayout));

    m_shaderDir = std::filesystem::path(CANDELA_SHADER_DIR);
    CD_ASSERT(std::filesystem::exists(m_shaderDir), "Shader dir not found: {}",
              m_shaderDir.string());

    // Pick readback buffer (one pixel of the entity-ID target).
    m_pickBuffer = createBuffer(*m_context, sizeof(uint32_t),
                                VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                    VMA_ALLOCATION_CREATE_MAPPED_BIT);
    {
        VmaAllocationInfo info{};
        vmaGetAllocationInfo(m_context->allocator(), m_pickBuffer.allocation,
                             &info);
        m_pickMapped = info.pMappedData;
    }

    // Shadow cascade array.
    m_shadowMap = createImage2D(*m_context, kDepthFormat,
                                {kShadowMapSize, kShadowMapSize},
                                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                    VK_IMAGE_USAGE_SAMPLED_BIT,
                                1, CascadeSet::kCount);
    for (uint32_t i = 0; i < CascadeSet::kCount; ++i) {
        m_shadowLayerViews[i] =
            createImageView(*m_context, m_shadowMap, VK_IMAGE_VIEW_TYPE_2D, 0,
                            1, i, 1);
    }
    m_shadowBindlessSlot = m_bindless->add(
        Bindless::Kind::Array2D, m_shadowMap.view, m_bindless->clampSampler());

    // Environment lighting bakes lazily (first frame with geometry); until
    // then 1×1 black stand-ins keep every bindless slot valid. Empty scenes
    // — the editor's startup state — never pay for the bake.
    m_ibl = placeholderIBL(*m_context);
    m_irradianceSlot = m_bindless->add(Bindless::Kind::Cube, m_ibl.irradiance.view,
                                       m_bindless->clampSampler());
    m_prefilteredSlot = m_bindless->add(Bindless::Kind::Cube,
                                        m_ibl.prefiltered.view,
                                        m_bindless->clampSampler());
    m_brdfLutSlot = m_bindless->add(Bindless::Kind::Texture2D,
                                    m_ibl.brdfLut.view,
                                    m_bindless->clampSampler());

    m_rtSupported = m_context->rayTracingSupported();
    if (m_rtSupported) {
        createRayTracingResources();
        CD_INFO("Ray tracing enabled (acceleration structures + ray queries)");
    }

    std::error_code ec;
    m_shaderTimestamp = std::filesystem::file_time_type{};
    for (const auto& entry :
         std::filesystem::directory_iterator(m_shaderDir, ec)) {
        m_shaderTimestamp =
            (std::max)(m_shaderTimestamp, entry.last_write_time(ec));
    }

    CD_ASSERT(createPipelines(), "Initial shader compilation failed");
    m_lastReloadCheck = std::chrono::steady_clock::now();
}

void Renderer::createRayTracingResources() {
    VkDevice device = m_context->device();

    // Sizes are queried once for the worst case; per-frame builds reuse the
    // same buffers with the actual instance count.
    VkAccelerationStructureGeometryKHR geometry{};
    geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geometry.geometry.instances.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;

    VkAccelerationStructureBuildGeometryInfoKHR buildInfo{};
    buildInfo.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    buildInfo.flags =
        VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries = &geometry;

    const uint32_t maxInstances = kMaxRTInstances;
    VkAccelerationStructureBuildSizesInfoKHR sizes{};
    sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    vkGetAccelerationStructureBuildSizesKHR(
        device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo,
        &maxInstances, &sizes);

    for (FrameData& frame : m_frames) {
        frame.tlasBuffer = createBuffer(
            *m_context, sizes.accelerationStructureSize,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);

        VkAccelerationStructureCreateInfoKHR createInfo{};
        createInfo.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        createInfo.buffer = frame.tlasBuffer.buffer;
        createInfo.size = sizes.accelerationStructureSize;
        createInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        VK_CHECK(vkCreateAccelerationStructureKHR(device, &createInfo, nullptr,
                                                  &frame.tlas));

        frame.tlasScratch = createBuffer(
            *m_context, sizes.buildScratchSize + m_context->scratchAlignment(),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);

        frame.tlasInstances = createBuffer(
            *m_context,
            kMaxRTInstances * sizeof(VkAccelerationStructureInstanceKHR),
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                VMA_ALLOCATION_CREATE_MAPPED_BIT);
        VmaAllocationInfo info{};
        vmaGetAllocationInfo(m_context->allocator(),
                             frame.tlasInstances.allocation, &info);
        frame.tlasInstancesMapped = info.pMappedData;

        frame.instanceData = createBuffer(
            *m_context, kMaxRTInstanceData * sizeof(InstanceDataGPU),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                VMA_ALLOCATION_CREATE_MAPPED_BIT);
        vmaGetAllocationInfo(m_context->allocator(),
                             frame.instanceData.allocation, &info);
        frame.instanceDataMapped = info.pMappedData;

        frame.tlasSlot = m_bindless->addAccelerationStructure(frame.tlas);
    }
}

uint32_t Renderer::fillRayTracingInstances(FrameData& frame, World& world,
                                           AssetRegistry& assets) {
    ZoneScoped;
    auto* instances = static_cast<VkAccelerationStructureInstanceKHR*>(
        frame.tlasInstancesMapped);
    auto* data = static_cast<InstanceDataGPU*>(frame.instanceDataMapped);
    uint32_t instanceCount = 0;
    uint32_t dataCount = 0;

    for (auto [entity, transform, mesh] :
         world.registry.view<WorldTransform, MeshRenderer>().each()) {
        const ModelAsset* model = assets.tryGetModel(mesh.model);
        if (model == nullptr || mesh.meshIndex >= model->meshes.size()) {
            continue;
        }
        const GpuMesh& gpuMesh = model->meshes[mesh.meshIndex];
        if (gpuMesh.blas == VK_NULL_HANDLE ||
            instanceCount >= kMaxRTInstances ||
            dataCount + gpuMesh.primitives.size() > kMaxRTInstanceData) {
            continue;
        }

        VkAccelerationStructureInstanceKHR& instance =
            instances[instanceCount++];
        instance = {};
        const glm::mat4& m = transform.value;
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 4; ++col) {
                instance.transform.matrix[row][col] = m[col][row];
            }
        }
        instance.instanceCustomIndex = dataCount;
        instance.mask = 0xFF;
        instance.flags =
            VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        instance.accelerationStructureReference = gpuMesh.blasAddress;

        for (const GpuPrimitive& primitive : gpuMesh.primitives) {
            InstanceDataGPU& entry = data[dataCount++];
            entry.vertices = primitive.vertexBuffer.deviceAddress;
            entry.indices = primitive.indexBuffer.deviceAddress;
            entry.albedoTexture = primitive.albedoTexture;
            entry.flags = primitive.flags;
            entry.metallicFactor = primitive.metallicFactor;
            entry.roughnessFactor = primitive.roughnessFactor;
            entry.baseColorFactor = primitive.baseColorFactor;
        }
    }
    return instanceCount;
}

void Renderer::buildTLAS(VkCommandBuffer cmd, FrameData& frame,
                         uint32_t instanceCount) {
    VkAccelerationStructureGeometryKHR geometry{};
    geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geometry.geometry.instances.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    geometry.geometry.instances.data.deviceAddress =
        frame.tlasInstances.deviceAddress;

    VkAccelerationStructureBuildGeometryInfoKHR buildInfo{};
    buildInfo.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    buildInfo.flags =
        VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries = &geometry;
    buildInfo.dstAccelerationStructure = frame.tlas;
    const VkDeviceAddress alignment = m_context->scratchAlignment();
    buildInfo.scratchData.deviceAddress =
        (frame.tlasScratch.deviceAddress + alignment - 1) & ~(alignment - 1);

    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = instanceCount;
    const VkAccelerationStructureBuildRangeInfoKHR* rangePtr = &range;
    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &buildInfo, &rangePtr);

    // Build → ray-query reads in fragment/compute.
    VkMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier.srcStageMask =
        VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    barrier.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

Renderer::~Renderer() {
    m_context->waitIdle();

    destroyBuffer(*m_context, m_pickBuffer);
    destroyBuffer(*m_context, m_screenshotBuffer);
    destroyImage(*m_context, m_viewportImage);
    destroyTemporalTarget(m_aoTemporal);
    destroyTemporalTarget(m_reflectionsTemporal);
    destroyTemporalTarget(m_taaTemporal);
    m_ibl.destroy(*m_context);
    m_iblPlaceholder.destroy(*m_context);
    for (VkImageView view : m_shadowLayerViews) {
        if (view != VK_NULL_HANDLE) {
            vkDestroyImageView(m_context->device(), view, nullptr);
        }
    }
    destroyImage(*m_context, m_shadowMap);

#ifdef TRACY_ENABLE
    if (m_tracyCtx != nullptr) {
        TracyVkDestroy(m_tracyCtx);
    }
#endif

    VkDevice device = m_context->device();
    for (VkPipeline pipeline :
         {m_gbufferPipeline, m_shadowPipeline, m_lightingPipeline,
          m_bloomDownPipeline, m_bloomUpPipeline, m_tonemapPipeline,
          m_reflectionsPipeline, m_aoPipeline, m_temporalPipeline}) {
        if (pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, pipeline, nullptr);
        }
    }
    if (m_pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, m_pipelineLayout, nullptr);
    }
    destroyPresentSemaphores();
    destroyFrameData();
    m_shaderCache.reset();
    m_renderGraph.reset();
    m_bindless.reset();
    m_swapchain.reset();
    m_context.reset();
}

void Renderer::createFrameData() {
    VkDevice device = m_context->device();
    for (FrameData& frame : m_frames) {
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = m_context->graphicsQueueFamily();
        VK_CHECK(vkCreateCommandPool(device, &poolInfo, nullptr, &frame.pool));

        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = frame.pool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(device, &allocInfo, &frame.cmd));

        VkSemaphoreCreateInfo semInfo{};
        semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VK_CHECK(vkCreateSemaphore(device, &semInfo, nullptr,
                                   &frame.acquireSemaphore));

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_CHECK(vkCreateFence(device, &fenceInfo, nullptr, &frame.inFlightFence));

        VkQueryPoolCreateInfo queryInfo{};
        queryInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
        queryInfo.queryCount = kMaxTimestampQueries;
        VK_CHECK(
            vkCreateQueryPool(device, &queryInfo, nullptr, &frame.queryPool));

        frame.constants = createBuffer(
            *m_context, sizeof(FrameConstants),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                VMA_ALLOCATION_CREATE_MAPPED_BIT);
        VmaAllocationInfo info{};
        vmaGetAllocationInfo(m_context->allocator(), frame.constants.allocation,
                             &info);
        frame.constantsMapped = info.pMappedData;
    }
}

void Renderer::destroyFrameData() {
    VkDevice device = m_context->device();
    for (FrameData& frame : m_frames) {
        if (frame.tlas != VK_NULL_HANDLE) {
            vkDestroyAccelerationStructureKHR(device, frame.tlas, nullptr);
            destroyBuffer(*m_context, frame.tlasBuffer);
            destroyBuffer(*m_context, frame.tlasScratch);
            destroyBuffer(*m_context, frame.tlasInstances);
            destroyBuffer(*m_context, frame.instanceData);
        }
        destroyBuffer(*m_context, frame.constants);
        vkDestroyQueryPool(device, frame.queryPool, nullptr);
        vkDestroyFence(device, frame.inFlightFence, nullptr);
        vkDestroySemaphore(device, frame.acquireSemaphore, nullptr);
        vkDestroyCommandPool(device, frame.pool, nullptr);
    }
}

void Renderer::createPresentSemaphores() {
    m_presentSemaphores.resize(m_swapchain->imageCount());
    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (VkSemaphore& semaphore : m_presentSemaphores) {
        VK_CHECK(vkCreateSemaphore(m_context->device(), &semInfo, nullptr,
                                   &semaphore));
    }
}

void Renderer::destroyPresentSemaphores() {
    for (VkSemaphore semaphore : m_presentSemaphores) {
        vkDestroySemaphore(m_context->device(), semaphore, nullptr);
    }
    m_presentSemaphores.clear();
}

VkPipeline Renderer::buildPipeline(const PipelineDesc& desc) {
    const std::filesystem::path shaderPath = m_shaderDir / desc.shaderFile;
    VkShaderModule vs =
        m_shaderCache->get(shaderPath, desc.vsEntry, ShaderStage::Vertex);
    if (vs == VK_NULL_HANDLE) {
        return VK_NULL_HANDLE;
    }
    VkShaderModule fs = VK_NULL_HANDLE;
    if (!desc.fsEntry.empty()) {
        fs = m_shaderCache->get(shaderPath, desc.fsEntry,
                                ShaderStage::Fragment);
        if (fs == VK_NULL_HANDLE) {
            return VK_NULL_HANDLE;
        }
    }

    // slangc emits a single OpEntryPoint named "main" per module.
    VkPipelineShaderStageCreateInfo stages[2]{};
    uint32_t stageCount = 1;
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    if (fs != VK_NULL_HANDLE) {
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fs;
        stages[1].pName = "main";
        stageCount = 2;
    }

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType =
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterization{};
    rasterization.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = desc.cullMode;
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0f;
    if (desc.depthBias) {
        rasterization.depthBiasEnable = VK_TRUE;
        rasterization.depthBiasConstantFactor = 1.25f;
        rasterization.depthBiasSlopeFactor = 1.75f;
    }

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType =
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType =
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = desc.depthTest ? VK_TRUE : VK_FALSE;
    depthStencil.depthWriteEnable = desc.depthWrite ? VK_TRUE : VK_FALSE;
    depthStencil.depthCompareOp = desc.depthCompare;

    std::vector<VkPipelineColorBlendAttachmentState> blendAttachments(
        desc.colorFormats.size());
    for (auto& attachment : blendAttachments) {
        attachment = {};
        attachment.colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        if (desc.additiveBlend) {
            attachment.blendEnable = VK_TRUE;
            attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            attachment.colorBlendOp = VK_BLEND_OP_ADD;
            attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            attachment.alphaBlendOp = VK_BLEND_OP_ADD;
        }
    }

    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = static_cast<uint32_t>(blendAttachments.size());
    blend.pAttachments = blendAttachments.data();

    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                            VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;

    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount =
        static_cast<uint32_t>(desc.colorFormats.size());
    rendering.pColorAttachmentFormats = desc.colorFormats.data();
    rendering.depthAttachmentFormat = desc.depthFormat;

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.pNext = &rendering;
    pipelineInfo.stageCount = stageCount;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewport;
    pipelineInfo.pRasterizationState = &rasterization;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &blend;
    pipelineInfo.pDynamicState = &dynamic;
    pipelineInfo.layout = m_pipelineLayout;

    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult result = vkCreateGraphicsPipelines(
        m_context->device(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
        &pipeline);
    if (result != VK_SUCCESS) {
        CD_ERROR("vkCreateGraphicsPipelines failed for {}: {}",
                 desc.shaderFile, string_VkResult(result));
        return VK_NULL_HANDLE;
    }
    return pipeline;
}

VkPipeline Renderer::buildComputePipeline(const std::string& shaderFile,
                                          const std::string& entry) {
    VkShaderModule shader = m_shaderCache->get(m_shaderDir / shaderFile, entry,
                                               ShaderStage::Compute);
    if (shader == VK_NULL_HANDLE) {
        return VK_NULL_HANDLE;
    }
    VkComputePipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = shader;
    info.stage.pName = "main";
    info.layout = m_pipelineLayout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult result = vkCreateComputePipelines(
        m_context->device(), VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
    if (result != VK_SUCCESS) {
        CD_ERROR("vkCreateComputePipelines failed for {}: {}", shaderFile,
                 string_VkResult(result));
        return VK_NULL_HANDLE;
    }
    return pipeline;
}

bool Renderer::createPipelines() {
    ZoneScoped;

    PipelineDesc gbuffer;
    gbuffer.shaderFile = "gbuffer.slang";
    gbuffer.fsEntry = "fsMain";
    gbuffer.colorFormats = {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R16G16_UNORM,
                            VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R32_UINT,
                            VK_FORMAT_R16G16_SFLOAT};
    gbuffer.depthFormat = kDepthFormat;
    gbuffer.depthTest = true;
    gbuffer.depthWrite = true;
    gbuffer.cullMode = VK_CULL_MODE_BACK_BIT;

    PipelineDesc shadow;
    shadow.shaderFile = "shadow.slang";
    shadow.depthFormat = kDepthFormat;
    shadow.depthTest = true;
    shadow.depthWrite = true;
    shadow.depthCompare = VK_COMPARE_OP_LESS_OR_EQUAL; // standard Z for ortho
    shadow.depthBias = true;
    shadow.cullMode = VK_CULL_MODE_BACK_BIT;

    PipelineDesc lighting;
    lighting.shaderFile = "lighting.slang";
    lighting.fsEntry = "fsMain";
    lighting.colorFormats = {kHdrFormat};

    PipelineDesc bloomDown;
    bloomDown.shaderFile = "bloom.slang";
    bloomDown.fsEntry = "fsDownsample";
    bloomDown.colorFormats = {kHdrFormat};

    PipelineDesc bloomUp = bloomDown;
    bloomUp.fsEntry = "fsUpsample";
    bloomUp.additiveBlend = true;

    PipelineDesc tonemap;
    tonemap.shaderFile = "tonemap.slang";
    tonemap.fsEntry = "fsMain";
    tonemap.colorFormats = {m_swapchain->format()};

    VkPipeline newPipelines[9] = {
        buildPipeline(gbuffer),
        buildPipeline(shadow),
        buildPipeline(lighting),
        buildPipeline(bloomDown),
        buildPipeline(bloomUp),
        buildPipeline(tonemap),
        buildComputePipeline("temporal.slang", "csMain"),
        m_rtSupported ? buildComputePipeline("reflections.slang", "csMain")
                      : VK_NULL_HANDLE,
        m_rtSupported ? buildComputePipeline("ao.slang", "csMain")
                      : VK_NULL_HANDLE,
    };
    const uint32_t required = m_rtSupported ? 9u : 7u;
    for (uint32_t i = 0; i < required; ++i) {
        if (newPipelines[i] == VK_NULL_HANDLE) {
            for (VkPipeline created : newPipelines) {
                if (created != VK_NULL_HANDLE) {
                    vkDestroyPipeline(m_context->device(), created, nullptr);
                }
            }
            return false;
        }
    }

    VkPipeline* slots[9] = {&m_gbufferPipeline,   &m_shadowPipeline,
                            &m_lightingPipeline,  &m_bloomDownPipeline,
                            &m_bloomUpPipeline,   &m_tonemapPipeline,
                            &m_temporalPipeline,  &m_reflectionsPipeline,
                            &m_aoPipeline};
    for (uint32_t i = 0; i < 9; ++i) {
        if (*slots[i] != VK_NULL_HANDLE) {
            vkDestroyPipeline(m_context->device(), *slots[i], nullptr);
        }
        *slots[i] = newPipelines[i];
    }
    return true;
}

void Renderer::ensureTemporalTarget(TemporalTarget& target, VkExtent2D extent) {
    if (target.images[0].image != VK_NULL_HANDLE &&
        target.images[0].extent.width == extent.width &&
        target.images[0].extent.height == extent.height) {
        return;
    }
    m_context->waitIdle();
    destroyTemporalTarget(target);
    for (GpuImage& image : target.images) {
        image = createImage2D(*m_context, kHdrFormat, extent,
                              VK_IMAGE_USAGE_STORAGE_BIT |
                                  VK_IMAGE_USAGE_SAMPLED_BIT);
    }
    target.everUsed[0] = false;
    target.everUsed[1] = false;
    target.writeIndex = 0;
    target.valid = false;
}

void Renderer::destroyTemporalTarget(TemporalTarget& target) {
    for (GpuImage& image : target.images) {
        destroyImage(*m_context, image);
    }
}

uint32_t Renderer::storageSlotFor(VkImageView view) {
    if (auto it = m_storageSlots.find(view); it != m_storageSlots.end()) {
        return it->second;
    }
    const uint32_t slot =
        m_bindless->add(Bindless::Kind::StorageImage, view, VK_NULL_HANDLE);
    m_storageSlots.emplace(view, slot);
    return slot;
}

void Renderer::ensureViewportImage(VkExtent2D extent) {
    if (m_viewportImage.image != VK_NULL_HANDLE &&
        m_viewportImage.extent.width == extent.width &&
        m_viewportImage.extent.height == extent.height) {
        return;
    }
    m_context->waitIdle();
    destroyImage(*m_context, m_viewportImage);
    m_viewportImage = createImage2D(*m_context, m_swapchain->format(), extent,
                                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                        VK_IMAGE_USAGE_SAMPLED_BIT);
    m_viewportEverRendered = false;
    ++m_viewportGeneration;
}

std::optional<uint32_t> Renderer::takePickResult() {
    auto result = m_pickResult;
    m_pickResult.reset();
    return result;
}

void Renderer::requestScreenshot(std::filesystem::path path) {
    if (m_screenshotPending) {
        CD_WARN("Screenshot already in flight, ignoring request");
        return;
    }
    m_screenshotPath = std::move(path);
}

void Renderer::recreateSwapchain() {
    glm::uvec2 fb = m_window.framebufferSize();
    while (fb.x == 0 || fb.y == 0) {
        m_window.waitEvents(); // minimized — sleep until restored
        fb = m_window.framebufferSize();
    }
    m_context->waitIdle();
    m_swapchain->recreate(fb.x, fb.y);
    destroyPresentSemaphores();
    createPresentSemaphores();
}

void Renderer::checkShaderHotReload() {
    const auto now = std::chrono::steady_clock::now();
    if (now - m_lastReloadCheck < std::chrono::milliseconds(500)) {
        return;
    }
    m_lastReloadCheck = now;

    std::error_code ec;
    auto newest = std::filesystem::file_time_type{};
    for (const auto& entry :
         std::filesystem::directory_iterator(m_shaderDir, ec)) {
        newest = (std::max)(newest, entry.last_write_time(ec));
    }
    if (ec || newest == m_shaderTimestamp) {
        return;
    }
    m_shaderTimestamp = newest;

    CD_INFO("Shader change detected, rebuilding pipelines");
    m_context->waitIdle();
    m_shaderCache->invalidateAll();
    if (createPipelines()) {
        CD_INFO("Shader reload OK");
    } else {
        CD_WARN("Shader reload failed — keeping previous pipelines");
    }
}

uint32_t Renderer::slotFor(VkImageView view) {
    if (auto it = m_viewSlots.find(view); it != m_viewSlots.end()) {
        return it->second;
    }
    const uint32_t slot = m_bindless->add(Bindless::Kind::Texture2D, view,
                                          m_bindless->clampSampler());
    m_viewSlots.emplace(view, slot);
    return slot;
}

namespace {

float haltonSequence(uint32_t index, uint32_t base) {
    float result = 0.0f;
    float fraction = 1.0f / static_cast<float>(base);
    while (index > 0) {
        result += fraction * static_cast<float>(index % base);
        index /= base;
        fraction /= static_cast<float>(base);
    }
    return result;
}

} // namespace

void Renderer::updateFrameConstants(FrameData& frame, const Camera& camera,
                                    const World& world, float aspect,
                                    VkExtent2D sceneExtent) {
    const SceneSettings& settings = world.settings;
    const CascadeSet cascades =
        computeCascades(camera, aspect, settings.toSun, kMaxShadowDistance,
                        kShadowMapSize);

    FrameConstants constants{};
    const glm::mat4 viewProjNoJitter = camera.viewProjection(aspect);

    // Sub-pixel projection jitter (Halton 2,3 over 8 frames) when TAA is on.
    glm::vec2 jitter{0.0f};
    if (settings.taa) {
        const uint32_t phase = static_cast<uint32_t>(m_frameCounter % 8) + 1;
        jitter = {(haltonSequence(phase, 2) - 0.5f) * 2.0f /
                      static_cast<float>(sceneExtent.width),
                  (haltonSequence(phase, 3) - 0.5f) * 2.0f /
                      static_cast<float>(sceneExtent.height)};
    }
    glm::mat4 jitteredProj = camera.projection(aspect);
    jitteredProj[2][0] += jitter.x; // column 2 scales by -z, so this lands as
    jitteredProj[2][1] += jitter.y; // a constant NDC offset after the w divide

    constants.viewProjection = jitteredProj * camera.view();
    constants.invViewProjection = glm::inverse(constants.viewProjection);
    constants.viewProjectionNoJitter = viewProjNoJitter;
    constants.prevViewProjectionNoJitter =
        m_hasPrevViewProj ? m_prevViewProjNoJitter : viewProjNoJitter;
    constants.jitter = jitter;
    constants.frameIndex = static_cast<uint32_t>(m_frameCounter);
    constants.taaEnabled = settings.taa ? 1u : 0u;
    m_prevViewProjNoJitter = viewProjNoJitter;
    m_hasPrevViewProj = true;
    for (uint32_t i = 0; i < CascadeSet::kCount; ++i) {
        constants.cascadeViewProjection[i] = cascades.viewProjection[i];
    }
    constants.cascadeSplits = cascades.splitDepths;
    constants.cameraPosition = glm::vec4(camera.position, 1.0f);
    constants.sunDirection =
        glm::vec4(glm::normalize(settings.toSun), settings.sunIntensity);
    constants.sunColor = glm::vec4(settings.sunColor, 0.0f);
    constants.pointLightCount =
        (std::min)(static_cast<uint32_t>(m_frameLights.size()), 8u);
    for (uint32_t i = 0; i < constants.pointLightCount; ++i) {
        const FrameLight& light = m_frameLights[i];
        constants.pointLights[i].positionRadius =
            glm::vec4(light.position, light.radius);
        constants.pointLights[i].colorIntensity =
            glm::vec4(light.color, light.intensity);
    }

    // Zero instances (e.g. model still importing) means the TLAS and the
    // RT passes don't run this frame — the shader must not sample them.
    if (m_rtSupported && m_rtInstanceCount > 0) {
        constants.rtFlags =
            (settings.rtShadows ? kRTShadowsBit : 0) |
            (settings.rtAmbientOcclusion ? kRTAOBit : 0) |
            (settings.rtReflections ? kRTReflectionsBit : 0);
        constants.tlasSlot = frame.tlasSlot;
        constants.instanceData = frame.instanceData.deviceAddress;
    }

    std::memcpy(frame.constantsMapped, &constants, sizeof(constants));
}

void Renderer::recordCommands(VkCommandBuffer cmd, uint32_t imageIndex,
                              const Camera& camera, const World& world,
                              const RenderOptions& options) {
    ZoneScoped;

    FrameData& frame = m_frames[m_frameIndex];
    const VkExtent2D swapExtent = m_swapchain->extent();
    const bool editorMode = options.viewportExtent.width != 0;
    const VkExtent2D extent = editorMode ? options.viewportExtent : swapExtent;
    const float aspect = static_cast<float>(extent.width) /
                         static_cast<float>(extent.height);
    const VkDeviceAddress frameAddress = frame.constants.deviceAddress;

    // CPU-side cascade matrices are needed for shadow draw pushes too.
    const CascadeSet cascades =
        computeCascades(camera, aspect, world.settings.toSun,
                        kMaxShadowDistance, kShadowMapSize);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &beginInfo));

    // GPU pass timings for this frame slot (read back when its fence clears).
    vkCmdResetQueryPool(cmd, frame.queryPool, 0, kMaxTimestampQueries);
    RenderGraph::GpuTimestamps timestamps;
    timestamps.pool = frame.queryPool;
    timestamps.capacity = kMaxTimestampQueries;

    // TLAS rebuild (cheap at this instance count; prepared in drawFrame).
    const bool rtActive = m_rtSupported && m_rtInstanceCount > 0 &&
                          (world.settings.rtShadows ||
                           world.settings.rtAmbientOcclusion ||
                           world.settings.rtReflections);
    if (rtActive) {
        TracyVkZone(m_tracyCtx, cmd, "tlas-build");
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                             timestamps.pool, timestamps.next);
        buildTLAS(cmd, frame, m_rtInstanceCount);
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
                             timestamps.pool, timestamps.next + 1);
        timestamps.names.push_back("tlas-build");
        timestamps.next += 2;
    }

    RenderGraph& graph = *m_renderGraph;
    graph.begin();

    const RenderGraph::Handle backbuffer = graph.importImage(
        "backbuffer", m_swapchain->image(imageIndex),
        m_swapchain->view(imageIndex), m_swapchain->format(), swapExtent,
        VK_IMAGE_LAYOUT_UNDEFINED);

    // The tonemapped scene lands either on the backbuffer (runtime) or the
    // persistent offscreen viewport image the editor shows via ImGui.
    RenderGraph::Handle sceneTarget = backbuffer;
    if (editorMode) {
        sceneTarget = graph.importImage(
            "viewport", m_viewportImage.image, m_viewportImage.view,
            m_viewportImage.format, extent,
            m_viewportEverRendered ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                   : VK_IMAGE_LAYOUT_UNDEFINED,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        m_viewportEverRendered = true;
    }
    const RenderGraph::Handle shadowMap = graph.importImage(
        "shadow-cascades", m_shadowMap.image, m_shadowMap.view, kDepthFormat,
        {kShadowMapSize, kShadowMapSize},
        m_shadowMapEverRendered ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                : VK_IMAGE_LAYOUT_UNDEFINED,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, /*isDepth=*/true);
    m_shadowMapEverRendered = true;

    const RenderGraph::Handle gbAlbedo = graph.createImage(
        "gb-albedo", VK_FORMAT_R8G8B8A8_SRGB, extent,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    const RenderGraph::Handle gbNormal = graph.createImage(
        "gb-normal", VK_FORMAT_R16G16_UNORM, extent,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    const RenderGraph::Handle gbMaterial = graph.createImage(
        "gb-material", VK_FORMAT_R8G8B8A8_UNORM, extent,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    const RenderGraph::Handle gbEntityId = graph.createImage(
        "gb-entity-id", VK_FORMAT_R32_UINT, extent,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    const RenderGraph::Handle gbVelocity = graph.createImage(
        "gb-velocity", VK_FORMAT_R16G16_SFLOAT, extent,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    const RenderGraph::Handle depth = graph.createImage(
        "depth", kDepthFormat, extent,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT);
    const RenderGraph::Handle hdr = graph.createImage(
        "hdr", kHdrFormat, extent,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);

    // Imports one half of a ping-pong history pair with its tracked state.
    auto importTemporal = [&](TemporalTarget& target, uint32_t index,
                              const char* name) {
        const RenderGraph::Handle handle = graph.importImage(
            name, target.images[index].image, target.images[index].view,
            kHdrFormat, extent,
            target.everUsed[index] ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                   : VK_IMAGE_LAYOUT_UNDEFINED,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        target.everUsed[index] = true;
        return handle;
    };

    // Accumulates `raw` into the target's history; returns the converged
    // image (this frame's write half) for downstream sampling.
    auto addTemporalPass = [&](const char* name, TemporalTarget& target,
                               RenderGraph::Handle raw, float blendFactor) {
        const uint32_t write = target.writeIndex;
        const uint32_t read = 1 - write;
        const RenderGraph::Handle historyHandle =
            importTemporal(target, read, name);
        const RenderGraph::Handle outputHandle =
            importTemporal(target, write, name);

        RenderGraph::Pass pass;
        pass.name = std::string(name) + "-accumulate";
        pass.sampledImages = {raw, historyHandle, gbVelocity};
        pass.storageImages = {outputHandle};
        TemporalPush push{};
        push.rawTexture = slotFor(graph.view(raw));
        push.historyTexture = slotFor(graph.view(historyHandle));
        push.outputImage = storageSlotFor(graph.view(outputHandle));
        push.velocityTexture = slotFor(graph.view(gbVelocity));
        push.resolution = {extent.width, extent.height};
        push.blendFactor = blendFactor;
        push.historyValid = target.valid ? 1u : 0u;
        const VkExtent2D dispatchExtent = extent;
        pass.execute = [this, push, dispatchExtent](VkCommandBuffer passCmd) {
            vkCmdBindPipeline(passCmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                              m_temporalPipeline);
            VkDescriptorSet set = m_bindless->set();
            vkCmdBindDescriptorSets(passCmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                    m_pipelineLayout, 0, 1, &set, 0, nullptr);
            vkCmdPushConstants(passCmd, m_pipelineLayout, kPushStages, 0,
                               sizeof(push), &push);
            vkCmdDispatch(passCmd, (dispatchExtent.width + 7) / 8,
                          (dispatchExtent.height + 7) / 8, 1);
        };
        graph.addPass(std::move(pass));

        target.valid = true;
        target.writeIndex = read;
        return outputHandle;
    };

    // --- Shadow cascades (skipped entirely when RT shadows replace them) ---
    const bool useRTShadows = rtActive && world.settings.rtShadows;
    for (uint32_t cascade = 0;
         !useRTShadows && cascade < CascadeSet::kCount; ++cascade) {
        RenderGraph::Pass pass;
        pass.name = "shadow-cascade-" + std::to_string(cascade);
        RenderGraph::Attachment depthAttachment;
        depthAttachment.handle = shadowMap;
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.clear = {.depthStencil = {1.0f, 0}}; // standard Z
        depthAttachment.viewOverride = m_shadowLayerViews[cascade];
        depthAttachment.extentOverride = {kShadowMapSize, kShadowMapSize};
        pass.depthAttachment = depthAttachment;
        const glm::mat4 cascadeMatrix = cascades.viewProjection[cascade];
        pass.execute = [this, cascadeMatrix](VkCommandBuffer passCmd) {
            setFullViewport(passCmd, {kShadowMapSize, kShadowMapSize});
            vkCmdBindPipeline(passCmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              m_shadowPipeline);
            VkDescriptorSet set = m_bindless->set();
            vkCmdBindDescriptorSets(passCmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    m_pipelineLayout, 0, 1, &set, 0, nullptr);
            for (const FrameDraw& draw : m_frameDraws) {
                ShadowPush push{};
                push.lightViewProjModel = cascadeMatrix * draw.transform;
                push.vertices = draw.primitive->vertexBuffer.deviceAddress;
                vkCmdPushConstants(passCmd, m_pipelineLayout, kPushStages,
                                   0, sizeof(push), &push);
                vkCmdBindIndexBuffer(passCmd,
                                     draw.primitive->indexBuffer.buffer, 0,
                                     VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(passCmd, draw.primitive->indexCount, 1, 0, 0,
                                 0);
            }
        };
        graph.addPass(std::move(pass));
    }

    // --- G-buffer ---
    {
        RenderGraph::Pass pass;
        pass.name = "gbuffer";
        pass.colorAttachments = {
            {gbAlbedo, VK_ATTACHMENT_LOAD_OP_CLEAR, {}, VK_NULL_HANDLE, {}},
            {gbNormal, VK_ATTACHMENT_LOAD_OP_CLEAR, {}, VK_NULL_HANDLE, {}},
            {gbMaterial, VK_ATTACHMENT_LOAD_OP_CLEAR, {}, VK_NULL_HANDLE, {}},
            {gbEntityId, VK_ATTACHMENT_LOAD_OP_CLEAR, {}, VK_NULL_HANDLE, {}},
            {gbVelocity, VK_ATTACHMENT_LOAD_OP_CLEAR, {}, VK_NULL_HANDLE, {}},
        };
        RenderGraph::Attachment depthAttachment;
        depthAttachment.handle = depth;
        depthAttachment.clear = {.depthStencil = {0.0f, 0}}; // reverse-Z
        pass.depthAttachment = depthAttachment;
        pass.execute = [this, extent, frameAddress](VkCommandBuffer passCmd) {
            setFullViewport(passCmd, extent);
            vkCmdBindPipeline(passCmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              m_gbufferPipeline);
            VkDescriptorSet set = m_bindless->set();
            vkCmdBindDescriptorSets(passCmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    m_pipelineLayout, 0, 1, &set, 0, nullptr);
            for (const FrameDraw& draw : m_frameDraws) {
                const GpuPrimitive& primitive = *draw.primitive;
                GBufferPush push{};
                push.model = draw.transform;
                push.vertices = primitive.vertexBuffer.deviceAddress;
                push.frame = frameAddress;
                push.albedoTexture = primitive.albedoTexture;
                push.normalTexture = primitive.normalTexture;
                push.metallicRoughnessTexture =
                    primitive.metallicRoughnessTexture;
                push.occlusionTexture = primitive.occlusionTexture;
                push.flags = primitive.flags;
                push.metallicFactor = primitive.metallicFactor;
                push.roughnessFactor = primitive.roughnessFactor;
                push.entityId = draw.entityId;
                push.baseColorFactor = primitive.baseColorFactor;
                vkCmdPushConstants(passCmd, m_pipelineLayout, kPushStages,
                                   0, sizeof(push), &push);
                vkCmdBindIndexBuffer(passCmd, primitive.indexBuffer.buffer, 0,
                                     VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(passCmd, primitive.indexCount, 1, 0, 0, 0);
            }
        };
        graph.addPass(std::move(pass));
    }

    // --- RT reflections (compute, before lighting consumes the buffer) ---
    RenderGraph::Handle rtReflections = 0;
    const bool reflectionsActive =
        rtActive && world.settings.rtReflections &&
        m_reflectionsPipeline != VK_NULL_HANDLE;
    if (reflectionsActive) {
        rtReflections = graph.createImage(
            "rt-reflections", kHdrFormat, extent,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);

        // Storage slots are stable per view (pooled images persist).
        const VkImageView view = graph.view(rtReflections);
        uint32_t storageSlot;
        if (auto it = m_storageSlots.find(view); it != m_storageSlots.end()) {
            storageSlot = it->second;
        } else {
            storageSlot = m_bindless->add(Bindless::Kind::StorageImage, view,
                                          VK_NULL_HANDLE);
            m_storageSlots.emplace(view, storageSlot);
        }

        RenderGraph::Pass pass;
        pass.name = "rt-reflections";
        pass.sampledImages = {gbNormal, gbMaterial, depth};
        pass.storageImages = {rtReflections};
        ReflectionsPush push{};
        push.frame = frameAddress;
        push.gbNormal = slotFor(graph.view(gbNormal));
        push.gbMaterial = slotFor(graph.view(gbMaterial));
        push.gbDepth = slotFor(graph.view(depth));
        push.outputImage = storageSlot;
        push.resolution = {extent.width, extent.height};
        push.prefilteredCube = m_prefilteredSlot;
        push.irradianceCube = m_irradianceSlot;
        push.iblIntensity = world.settings.iblIntensity;
        push.prefilteredMips = IBL::kPrefilteredMips;
        const VkExtent2D dispatchExtent = extent;
        pass.execute = [this, push, dispatchExtent](VkCommandBuffer passCmd) {
            vkCmdBindPipeline(passCmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                              m_reflectionsPipeline);
            VkDescriptorSet set = m_bindless->set();
            vkCmdBindDescriptorSets(passCmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                    m_pipelineLayout, 0, 1, &set, 0, nullptr);
            vkCmdPushConstants(passCmd, m_pipelineLayout, kPushStages, 0,
                               sizeof(push), &push);
            vkCmdDispatch(passCmd, (dispatchExtent.width + 7) / 8,
                          (dispatchExtent.height + 7) / 8, 1);
        };
        graph.addPass(std::move(pass));
    }

    // --- Temporal accumulation of the RT effects ---
    RenderGraph::Handle reflectionsAccumulated = 0;
    if (reflectionsActive) {
        reflectionsAccumulated = addTemporalPass(
            "rt-reflections", m_reflectionsTemporal, rtReflections, 0.1f);
    }

    // --- RT ambient occlusion (compute) + accumulation ---
    RenderGraph::Handle aoAccumulated = 0;
    const bool aoActive = rtActive && world.settings.rtAmbientOcclusion &&
                          m_aoPipeline != VK_NULL_HANDLE;
    if (aoActive) {
        const RenderGraph::Handle aoRaw = graph.createImage(
            "rt-ao", kHdrFormat, extent,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        RenderGraph::Pass pass;
        pass.name = "rt-ao";
        pass.sampledImages = {gbNormal, depth};
        pass.storageImages = {aoRaw};
        AOPush push{};
        push.frame = frameAddress;
        push.gbNormal = slotFor(graph.view(gbNormal));
        push.gbDepth = slotFor(graph.view(depth));
        push.outputImage = storageSlotFor(graph.view(aoRaw));
        push.resolution = {extent.width, extent.height};
        const VkExtent2D dispatchExtent = extent;
        pass.execute = [this, push, dispatchExtent](VkCommandBuffer passCmd) {
            vkCmdBindPipeline(passCmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                              m_aoPipeline);
            VkDescriptorSet set = m_bindless->set();
            vkCmdBindDescriptorSets(passCmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                    m_pipelineLayout, 0, 1, &set, 0, nullptr);
            vkCmdPushConstants(passCmd, m_pipelineLayout, kPushStages, 0,
                               sizeof(push), &push);
            vkCmdDispatch(passCmd, (dispatchExtent.width + 7) / 8,
                          (dispatchExtent.height + 7) / 8, 1);
        };
        graph.addPass(std::move(pass));

        aoAccumulated = addTemporalPass("rt-ao", m_aoTemporal, aoRaw, 0.15f);
    }

    // --- Deferred lighting ---
    {
        RenderGraph::Pass pass;
        pass.name = "lighting";
        pass.colorAttachments = {
            {hdr, VK_ATTACHMENT_LOAD_OP_CLEAR, {}, VK_NULL_HANDLE, {}}};
        pass.sampledImages = {gbAlbedo, gbNormal, gbMaterial, depth, shadowMap};
        if (reflectionsActive) {
            pass.sampledImages.push_back(reflectionsAccumulated);
        }
        if (aoActive) {
            pass.sampledImages.push_back(aoAccumulated);
        }
        LightingPush push{};
        push.frame = frameAddress;
        push.gbAlbedo = slotFor(graph.view(gbAlbedo));
        push.gbNormal = slotFor(graph.view(gbNormal));
        push.gbMaterial = slotFor(graph.view(gbMaterial));
        push.gbDepth = slotFor(graph.view(depth));
        push.shadowCascades = m_shadowBindlessSlot;
        push.irradianceCube = m_irradianceSlot;
        push.prefilteredCube = m_prefilteredSlot;
        push.brdfLut = m_brdfLutSlot;
        push.invResolution = {1.0f / static_cast<float>(extent.width),
                              1.0f / static_cast<float>(extent.height)};
        push.iblIntensity = world.settings.iblIntensity;
        push.prefilteredMips = IBL::kPrefilteredMips;
        push.debugView = static_cast<uint32_t>(options.debugView);
        push.reflectionTexture =
            reflectionsActive ? slotFor(graph.view(reflectionsAccumulated))
                              : 0;
        push.aoTexture = aoActive ? slotFor(graph.view(aoAccumulated)) : 0;
        pass.execute = [this, extent, push](VkCommandBuffer passCmd) {
            setFullViewport(passCmd, extent);
            vkCmdBindPipeline(passCmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              m_lightingPipeline);
            VkDescriptorSet set = m_bindless->set();
            vkCmdBindDescriptorSets(passCmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    m_pipelineLayout, 0, 1, &set, 0, nullptr);
            vkCmdPushConstants(passCmd, m_pipelineLayout, kPushStages,
                               0, sizeof(push), &push);
            vkCmdDraw(passCmd, 3, 1, 0, 0);
        };
        graph.addPass(std::move(pass));
    }

    // --- TAA on the HDR scene (before bloom/tonemap) ---
    RenderGraph::Handle sceneColor = hdr;
    if (world.settings.taa) {
        sceneColor = addTemporalPass("taa", m_taaTemporal, hdr, 0.1f);
    }

    // --- Bloom (dual Kawase): downsample chain, then additive upsample ---
    RenderGraph::Handle bloomLevels[kBloomLevels];
    VkExtent2D bloomExtents[kBloomLevels];
    {
        VkExtent2D levelExtent = extent;
        RenderGraph::Handle source = sceneColor;
        VkExtent2D sourceExtent = extent;
        for (uint32_t level = 0; level < kBloomLevels; ++level) {
            levelExtent = {(std::max)(levelExtent.width / 2, 1u),
                           (std::max)(levelExtent.height / 2, 1u)};
            bloomExtents[level] = levelExtent;
            bloomLevels[level] = graph.createImage(
                "bloom-" + std::to_string(level), kHdrFormat, levelExtent,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                    VK_IMAGE_USAGE_SAMPLED_BIT);

            RenderGraph::Pass pass;
            pass.name = "bloom-down-" + std::to_string(level);
            pass.colorAttachments = {{bloomLevels[level],
                                      VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                      {},
                                      VK_NULL_HANDLE,
                                      {}}};
            pass.sampledImages = {source};
            BloomPush push{};
            push.sourceTexture = slotFor(graph.view(source));
            push.isFirstDownsample = level == 0 ? 1u : 0u;
            push.sourceInvResolution = {
                1.0f / static_cast<float>(sourceExtent.width),
                1.0f / static_cast<float>(sourceExtent.height)};
            push.targetInvResolution = {
                1.0f / static_cast<float>(levelExtent.width),
                1.0f / static_cast<float>(levelExtent.height)};
            const VkExtent2D targetExtent = levelExtent;
            pass.execute = [this, targetExtent, push](VkCommandBuffer passCmd) {
                setFullViewport(passCmd, targetExtent);
                vkCmdBindPipeline(passCmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  m_bloomDownPipeline);
                VkDescriptorSet set = m_bindless->set();
                vkCmdBindDescriptorSets(passCmd,
                                        VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        m_pipelineLayout, 0, 1, &set, 0,
                                        nullptr);
                vkCmdPushConstants(passCmd, m_pipelineLayout, kPushStages,
                                   0, sizeof(push), &push);
                vkCmdDraw(passCmd, 3, 1, 0, 0);
            };
            graph.addPass(std::move(pass));

            source = bloomLevels[level];
            sourceExtent = levelExtent;
        }

        for (int level = kBloomLevels - 2; level >= 0; --level) {
            RenderGraph::Pass pass;
            pass.name = "bloom-up-" + std::to_string(level);
            pass.colorAttachments = {{bloomLevels[level],
                                      VK_ATTACHMENT_LOAD_OP_LOAD,
                                      {},
                                      VK_NULL_HANDLE,
                                      {}}};
            pass.sampledImages = {bloomLevels[level + 1]};
            BloomPush push{};
            push.sourceTexture = slotFor(graph.view(bloomLevels[level + 1]));
            push.sourceInvResolution = {
                1.0f / static_cast<float>(bloomExtents[level + 1].width),
                1.0f / static_cast<float>(bloomExtents[level + 1].height)};
            push.targetInvResolution = {
                1.0f / static_cast<float>(bloomExtents[level].width),
                1.0f / static_cast<float>(bloomExtents[level].height)};
            const VkExtent2D targetExtent = bloomExtents[level];
            pass.execute = [this, targetExtent, push](VkCommandBuffer passCmd) {
                setFullViewport(passCmd, targetExtent);
                vkCmdBindPipeline(passCmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  m_bloomUpPipeline);
                VkDescriptorSet set = m_bindless->set();
                vkCmdBindDescriptorSets(passCmd,
                                        VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        m_pipelineLayout, 0, 1, &set, 0,
                                        nullptr);
                vkCmdPushConstants(passCmd, m_pipelineLayout, kPushStages,
                                   0, sizeof(push), &push);
                vkCmdDraw(passCmd, 3, 1, 0, 0);
            };
            graph.addPass(std::move(pass));
        }
    }

    // --- Tonemap to the scene target ---
    {
        RenderGraph::Pass pass;
        pass.name = "tonemap";
        pass.colorAttachments = {{sceneTarget,
                                  VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                  {},
                                  VK_NULL_HANDLE,
                                  {}}};
        pass.sampledImages = {sceneColor, bloomLevels[0]};
        TonemapPush push{};
        push.hdrTexture = slotFor(graph.view(sceneColor));
        push.bloomTexture = slotFor(graph.view(bloomLevels[0]));
        push.exposure = world.settings.exposure;
        push.bloomStrength = world.settings.bloomStrength;
        push.invResolution = {1.0f / static_cast<float>(extent.width),
                              1.0f / static_cast<float>(extent.height)};
        pass.execute = [this, extent, push](VkCommandBuffer passCmd) {
            setFullViewport(passCmd, extent);
            vkCmdBindPipeline(passCmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              m_tonemapPipeline);
            VkDescriptorSet set = m_bindless->set();
            vkCmdBindDescriptorSets(passCmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    m_pipelineLayout, 0, 1, &set, 0, nullptr);
            vkCmdPushConstants(passCmd, m_pipelineLayout, kPushStages,
                               0, sizeof(push), &push);
            vkCmdDraw(passCmd, 3, 1, 0, 0);
        };
        graph.addPass(std::move(pass));
    }

    // --- Editor UI to the backbuffer (samples the viewport image) ---
    if (editorMode && options.recordUI) {
        RenderGraph::Pass pass;
        pass.name = "editor-ui";
        pass.colorAttachments = {{backbuffer,
                                  VK_ATTACHMENT_LOAD_OP_CLEAR,
                                  {.color = {{0.06f, 0.06f, 0.07f, 1.0f}}},
                                  VK_NULL_HANDLE,
                                  {}}};
        pass.sampledImages = {sceneTarget};
        pass.execute = [swapExtent, &options](VkCommandBuffer passCmd) {
            // ImGui supplies its own projection; default (positive) viewport
            // orientation, unlike the scene's negative-height convention.
            VkViewport viewport{};
            viewport.width = static_cast<float>(swapExtent.width);
            viewport.height = static_cast<float>(swapExtent.height);
            viewport.maxDepth = 1.0f;
            vkCmdSetViewport(passCmd, 0, 1, &viewport);
            VkRect2D scissor{{0, 0}, swapExtent};
            vkCmdSetScissor(passCmd, 0, 1, &scissor);
            options.recordUI(passCmd);
        };
        graph.addPass(std::move(pass));
    }

    graph.setFinalLayout(backbuffer, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    graph.execute(cmd, m_tracyCtx, &timestamps);
    frame.timestampNames = std::move(timestamps.names);

    // --- Screenshot readback: whole backbuffer after all passes ---
    if (!m_screenshotPath.empty() && !m_screenshotPending) {
        const VkDeviceSize byteSize =
            VkDeviceSize(swapExtent.width) * swapExtent.height * 4;
        if (m_screenshotBuffer.buffer == VK_NULL_HANDLE ||
            m_screenshotBuffer.size < byteSize) {
            destroyBuffer(*m_context, m_screenshotBuffer);
            m_screenshotBuffer = createBuffer(
                *m_context, byteSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                    VMA_ALLOCATION_CREATE_MAPPED_BIT);
            VmaAllocationInfo info{};
            vmaGetAllocationInfo(m_context->allocator(),
                                 m_screenshotBuffer.allocation, &info);
            m_screenshotMapped = info.pMappedData;
        }

        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = m_swapchain->image(imageIndex);
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dependency);

        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {swapExtent.width, swapExtent.height, 1};
        vkCmdCopyImageToBuffer(cmd, m_swapchain->image(imageIndex),
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               m_screenshotBuffer.buffer, 1, &region);

        std::swap(barrier.srcStageMask, barrier.dstStageMask);
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier2(cmd, &dependency);

        m_screenshotExtent = swapExtent;
        m_screenshotFrameSlot = m_frameIndex;
        m_screenshotPending = true;
    }

    // --- Pick readback: one texel of the entity-ID target ---
    if (options.pickPixel.has_value()) {
        const int32_t x = std::clamp(options.pickPixel->x, 0,
                                     static_cast<int32_t>(extent.width) - 1);
        const int32_t y = std::clamp(options.pickPixel->y, 0,
                                     static_cast<int32_t>(extent.height) - 1);

        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = graph.image(gbEntityId);
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dependency);

        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {x, y, 0};
        region.imageExtent = {1, 1, 1};
        vkCmdCopyImageToBuffer(cmd, graph.image(gbEntityId),
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               m_pickBuffer.buffer, 1, &region);
        // Transient image contents are discarded next frame; layout left as
        // TRANSFER_SRC is fine (pool re-imports as UNDEFINED).
        m_pickPending = true;
        m_pickFrameSlot = m_frameIndex;
    }

#ifdef TRACY_ENABLE
    if (m_tracyCtx != nullptr) {
        TracyVkCollect(m_tracyCtx, cmd);
    }
#endif

    VK_CHECK(vkEndCommandBuffer(cmd));
}

void Renderer::drawFrame(const Camera& camera, World& world,
                         AssetRegistry& assets, const RenderOptions& options) {
    ZoneScoped;

    checkShaderHotReload();

    const glm::uvec2 fb = m_window.framebufferSize();
    if (fb.x == 0 || fb.y == 0) {
        return; // minimized
    }
    if (m_window.consumeResizeFlag()) {
        recreateSwapchain();
    }

    const bool editorMode = options.viewportExtent.width != 0;
    if (editorMode) {
        ensureViewportImage(options.viewportExtent);
    }
    const VkExtent2D sceneExtent =
        editorMode ? options.viewportExtent : m_swapchain->extent();

    // Temporal history pairs (resize-aware; ensured before any recording so
    // the waitIdle inside recreation never lands mid-command-buffer).
    if (world.settings.taa) {
        ensureTemporalTarget(m_taaTemporal, sceneExtent);
    }
    if (m_rtSupported && world.settings.rtAmbientOcclusion) {
        ensureTemporalTarget(m_aoTemporal, sceneExtent);
    }
    if (m_rtSupported && world.settings.rtReflections) {
        ensureTemporalTarget(m_reflectionsTemporal, sceneExtent);
    }

    // Assemble this frame's draws and lights from the ECS. Models still
    // importing simply contribute nothing yet.
    m_frameDraws.clear();
    m_frameLights.clear();
    uint32_t triangles = 0;
    for (auto [entity, transform, mesh] :
         world.registry.view<WorldTransform, MeshRenderer>().each()) {
        const ModelAsset* model = assets.tryGetModel(mesh.model);
        if (model == nullptr || mesh.meshIndex >= model->meshes.size()) {
            continue;
        }
        const uint32_t entityId = static_cast<uint32_t>(entity) + 1;
        for (const GpuPrimitive& primitive :
             model->meshes[mesh.meshIndex].primitives) {
            m_frameDraws.push_back({transform.value, &primitive, entityId});
            triangles += primitive.indexCount / 3;
        }
    }
    for (auto [entity, transform, light] :
         world.registry.view<WorldTransform, PointLightComponent>().each()) {
        m_frameLights.push_back({glm::vec3(transform.value[3]), light.radius,
                                 light.color, light.intensity});
    }

    m_stats.drawCalls = static_cast<uint32_t>(m_frameDraws.size());
    m_stats.triangles = triangles;
    m_stats.pointLights = static_cast<uint32_t>(m_frameLights.size());
    m_stats.rayTracingSupported = m_rtSupported;
    m_stats.sceneExtent = sceneExtent;

    // First geometry in view → bake the environment lighting for real. The
    // placeholder stays alive until shutdown; frames in flight may still
    // sample its descriptors.
    if (!m_iblReady && !m_frameDraws.empty()) {
        m_iblPlaceholder = m_ibl;
        m_ibl = precomputeIBL(*m_context, *m_shaderCache,
                              std::filesystem::path(CANDELA_ASSET_DIR) /
                                  "hdri" /
                                  "kloofendal_48d_partly_cloudy_puresky_2k.hdr");
        m_irradianceSlot = m_bindless->add(
            Bindless::Kind::Cube, m_ibl.irradiance.view,
            m_bindless->clampSampler());
        m_prefilteredSlot = m_bindless->add(
            Bindless::Kind::Cube, m_ibl.prefiltered.view,
            m_bindless->clampSampler());
        m_brdfLutSlot = m_bindless->add(Bindless::Kind::Texture2D,
                                        m_ibl.brdfLut.view,
                                        m_bindless->clampSampler());
        m_iblReady = true;
    }

    FrameData& frame = m_frames[m_frameIndex];
    VkDevice device = m_context->device();

    VK_CHECK(vkWaitForFences(device, 1, &frame.inFlightFence, VK_TRUE,
                             UINT64_MAX));

    // Safe to write this frame slot's RT buffers now that its fence cleared.
    m_rtInstanceCount =
        m_rtSupported ? fillRayTracingInstances(frame, world, assets) : 0;
    m_stats.rtInstances = m_rtInstanceCount;

    // GPU timings from the work this slot recorded kFramesInFlight ago.
    if (!frame.timestampNames.empty()) {
        const uint32_t queryCount =
            static_cast<uint32_t>(frame.timestampNames.size()) * 2;
        uint64_t ticks[kMaxTimestampQueries];
        const VkResult queryResult = vkGetQueryPoolResults(
            device, frame.queryPool, 0, queryCount,
            sizeof(uint64_t) * queryCount, ticks, sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT);
        if (queryResult == VK_SUCCESS) {
            const float toMs = m_context->timestampPeriod() * 1e-6f;
            m_stats.gpuPasses.clear();
            for (size_t i = 0; i < frame.timestampNames.size(); ++i) {
                const float ms =
                    static_cast<float>(ticks[i * 2 + 1] - ticks[i * 2]) * toMs;
                m_stats.gpuPasses.push_back({frame.timestampNames[i], ms});
            }
            m_stats.gpuTotalMs =
                static_cast<float>(ticks[queryCount - 1] - ticks[0]) * toMs;
        }
        frame.timestampNames.clear();
    }

    // The pick recorded in this frame slot has now fully executed.
    if (m_pickPending && m_pickFrameSlot == m_frameIndex) {
        m_pickResult = *static_cast<const uint32_t*>(m_pickMapped);
        m_pickPending = false;
    }

    // Likewise the screenshot copy — swizzle BGRA→RGBA and write the PNG.
    if (m_screenshotPending && m_screenshotFrameSlot == m_frameIndex) {
        const uint32_t width = m_screenshotExtent.width;
        const uint32_t height = m_screenshotExtent.height;
        std::vector<uint8_t> rgba(size_t(width) * height * 4);
        const auto* src = static_cast<const uint8_t*>(m_screenshotMapped);
        for (size_t i = 0; i < rgba.size(); i += 4) {
            rgba[i + 0] = src[i + 2];
            rgba[i + 1] = src[i + 1];
            rgba[i + 2] = src[i + 0];
            rgba[i + 3] = 255;
        }
        stbi_write_png(m_screenshotPath.string().c_str(),
                       static_cast<int>(width), static_cast<int>(height), 4,
                       rgba.data(), static_cast<int>(width) * 4);
        CD_INFO("Screenshot written: {}", m_screenshotPath.string());
        m_screenshotPath.clear();
        m_screenshotPending = false;
    }

    uint32_t imageIndex = 0;
    const VkResult acquireResult =
        vkAcquireNextImageKHR(device, m_swapchain->handle(), UINT64_MAX,
                              frame.acquireSemaphore, VK_NULL_HANDLE,
                              &imageIndex);
    if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapchain();
        return;
    }
    CD_ASSERT(acquireResult == VK_SUCCESS || acquireResult == VK_SUBOPTIMAL_KHR,
              "vkAcquireNextImageKHR failed: {}", string_VkResult(acquireResult));

    updateFrameConstants(frame, camera, world,
                         static_cast<float>(sceneExtent.width) /
                             static_cast<float>(sceneExtent.height),
                         sceneExtent);

    VK_CHECK(vkResetFences(device, 1, &frame.inFlightFence));
    VK_CHECK(vkResetCommandPool(device, frame.pool, 0));

    recordCommands(frame.cmd, imageIndex, camera, world, options);
    ++m_frameCounter;

    VkCommandBufferSubmitInfo cmdInfo{};
    cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmdInfo.commandBuffer = frame.cmd;

    VkSemaphoreSubmitInfo waitInfo{};
    waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    waitInfo.semaphore = frame.acquireSemaphore;
    waitInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSemaphoreSubmitInfo signalInfo{};
    signalInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signalInfo.semaphore = m_presentSemaphores[imageIndex];
    signalInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkSubmitInfo2 submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submitInfo.waitSemaphoreInfoCount = 1;
    submitInfo.pWaitSemaphoreInfos = &waitInfo;
    submitInfo.commandBufferInfoCount = 1;
    submitInfo.pCommandBufferInfos = &cmdInfo;
    submitInfo.signalSemaphoreInfoCount = 1;
    submitInfo.pSignalSemaphoreInfos = &signalInfo;

    VkSwapchainKHR swapchain = m_swapchain->handle();
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &m_presentSemaphores[imageIndex];
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchain;
    presentInfo.pImageIndices = &imageIndex;

    VkResult presentResult = VK_SUCCESS;
    {
        // Serialize against worker-thread asset uploads (immediateSubmit).
        std::scoped_lock queueLock(m_context->queueMutex());
        VK_CHECK(vkQueueSubmit2(m_context->graphicsQueue(), 1, &submitInfo,
                                frame.inFlightFence));
        presentResult =
            vkQueuePresentKHR(m_context->graphicsQueue(), &presentInfo);
    }
    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR ||
        presentResult == VK_SUBOPTIMAL_KHR) {
        recreateSwapchain();
    } else {
        CD_ASSERT(presentResult == VK_SUCCESS, "vkQueuePresentKHR failed: {}",
                  string_VkResult(presentResult));
    }

    m_frameIndex = (m_frameIndex + 1) % kFramesInFlight;
}

} // namespace candela

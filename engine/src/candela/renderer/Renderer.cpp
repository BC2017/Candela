#include "candela/renderer/Renderer.h"

#include "candela/assets/AssetRegistry.h"
#include "candela/platform/Window.h"
#include "candela/renderer/Camera.h"
#include "candela/renderer/Cascades.h"
#include "candela/scene/World.h"

#include <tracy/Tracy.hpp>

#include <algorithm>
#include <cstring>

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
};
static_assert(sizeof(FrameConstants) == 720,
              "FrameConstants must match common.slang");

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
    float pad;
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
    pushRange.stageFlags =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
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

    // IBL precompute + registration.
    m_ibl = precomputeIBL(*m_context, *m_shaderCache,
                          std::filesystem::path(CANDELA_ASSET_DIR) / "hdri" /
                              "kloofendal_48d_partly_cloudy_puresky_2k.hdr");
    m_irradianceSlot = m_bindless->add(Bindless::Kind::Cube, m_ibl.irradiance.view,
                                       m_bindless->clampSampler());
    m_prefilteredSlot = m_bindless->add(Bindless::Kind::Cube,
                                        m_ibl.prefiltered.view,
                                        m_bindless->clampSampler());
    m_brdfLutSlot = m_bindless->add(Bindless::Kind::Texture2D,
                                    m_ibl.brdfLut.view,
                                    m_bindless->clampSampler());

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

Renderer::~Renderer() {
    m_context->waitIdle();

    m_ibl.destroy(*m_context);
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
          m_bloomDownPipeline, m_bloomUpPipeline, m_tonemapPipeline}) {
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
        destroyBuffer(*m_context, frame.constants);
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

bool Renderer::createPipelines() {
    ZoneScoped;

    PipelineDesc gbuffer;
    gbuffer.shaderFile = "gbuffer.slang";
    gbuffer.fsEntry = "fsMain";
    gbuffer.colorFormats = {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R16G16_UNORM,
                            VK_FORMAT_R8G8B8A8_UNORM};
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

    VkPipeline newPipelines[6] = {
        buildPipeline(gbuffer),  buildPipeline(shadow),
        buildPipeline(lighting), buildPipeline(bloomDown),
        buildPipeline(bloomUp),  buildPipeline(tonemap),
    };
    for (VkPipeline pipeline : newPipelines) {
        if (pipeline == VK_NULL_HANDLE) {
            for (VkPipeline created : newPipelines) {
                if (created != VK_NULL_HANDLE) {
                    vkDestroyPipeline(m_context->device(), created, nullptr);
                }
            }
            return false;
        }
    }

    VkPipeline* slots[6] = {&m_gbufferPipeline,   &m_shadowPipeline,
                            &m_lightingPipeline,  &m_bloomDownPipeline,
                            &m_bloomUpPipeline,   &m_tonemapPipeline};
    for (uint32_t i = 0; i < 6; ++i) {
        if (*slots[i] != VK_NULL_HANDLE) {
            vkDestroyPipeline(m_context->device(), *slots[i], nullptr);
        }
        *slots[i] = newPipelines[i];
    }
    return true;
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

void Renderer::updateFrameConstants(FrameData& frame, const Camera& camera,
                                    const World& world, float aspect) {
    const SceneSettings& settings = world.settings;
    const CascadeSet cascades =
        computeCascades(camera, aspect, settings.toSun, kMaxShadowDistance,
                        kShadowMapSize);

    FrameConstants constants{};
    constants.viewProjection = camera.viewProjection(aspect);
    constants.invViewProjection = glm::inverse(constants.viewProjection);
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

    std::memcpy(frame.constantsMapped, &constants, sizeof(constants));
}

void Renderer::recordCommands(VkCommandBuffer cmd, uint32_t imageIndex,
                              const Camera& camera, const World& world) {
    ZoneScoped;

    FrameData& frame = m_frames[m_frameIndex];
    const VkExtent2D extent = m_swapchain->extent();
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

    RenderGraph& graph = *m_renderGraph;
    graph.begin();

    const RenderGraph::Handle backbuffer = graph.importImage(
        "backbuffer", m_swapchain->image(imageIndex),
        m_swapchain->view(imageIndex), m_swapchain->format(), extent,
        VK_IMAGE_LAYOUT_UNDEFINED);
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
    const RenderGraph::Handle depth = graph.createImage(
        "depth", kDepthFormat, extent,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT);
    const RenderGraph::Handle hdr = graph.createImage(
        "hdr", kHdrFormat, extent,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);

    // --- Shadow cascades ---
    for (uint32_t cascade = 0; cascade < CascadeSet::kCount; ++cascade) {
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
                vkCmdPushConstants(passCmd, m_pipelineLayout,
                                   VK_SHADER_STAGE_VERTEX_BIT |
                                       VK_SHADER_STAGE_FRAGMENT_BIT,
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
                push.baseColorFactor = primitive.baseColorFactor;
                vkCmdPushConstants(passCmd, m_pipelineLayout,
                                   VK_SHADER_STAGE_VERTEX_BIT |
                                       VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof(push), &push);
                vkCmdBindIndexBuffer(passCmd, primitive.indexBuffer.buffer, 0,
                                     VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(passCmd, primitive.indexCount, 1, 0, 0, 0);
            }
        };
        graph.addPass(std::move(pass));
    }

    // --- Deferred lighting ---
    {
        RenderGraph::Pass pass;
        pass.name = "lighting";
        pass.colorAttachments = {
            {hdr, VK_ATTACHMENT_LOAD_OP_CLEAR, {}, VK_NULL_HANDLE, {}}};
        pass.sampledImages = {gbAlbedo, gbNormal, gbMaterial, depth, shadowMap};
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
        pass.execute = [this, extent, push](VkCommandBuffer passCmd) {
            setFullViewport(passCmd, extent);
            vkCmdBindPipeline(passCmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              m_lightingPipeline);
            VkDescriptorSet set = m_bindless->set();
            vkCmdBindDescriptorSets(passCmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    m_pipelineLayout, 0, 1, &set, 0, nullptr);
            vkCmdPushConstants(passCmd, m_pipelineLayout,
                               VK_SHADER_STAGE_VERTEX_BIT |
                                   VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(push), &push);
            vkCmdDraw(passCmd, 3, 1, 0, 0);
        };
        graph.addPass(std::move(pass));
    }

    // --- Bloom (dual Kawase): downsample chain, then additive upsample ---
    RenderGraph::Handle bloomLevels[kBloomLevels];
    VkExtent2D bloomExtents[kBloomLevels];
    {
        VkExtent2D levelExtent = extent;
        RenderGraph::Handle source = hdr;
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
                vkCmdPushConstants(passCmd, m_pipelineLayout,
                                   VK_SHADER_STAGE_VERTEX_BIT |
                                       VK_SHADER_STAGE_FRAGMENT_BIT,
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
                vkCmdPushConstants(passCmd, m_pipelineLayout,
                                   VK_SHADER_STAGE_VERTEX_BIT |
                                       VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof(push), &push);
                vkCmdDraw(passCmd, 3, 1, 0, 0);
            };
            graph.addPass(std::move(pass));
        }
    }

    // --- Tonemap to backbuffer ---
    {
        RenderGraph::Pass pass;
        pass.name = "tonemap";
        pass.colorAttachments = {{backbuffer,
                                  VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                  {},
                                  VK_NULL_HANDLE,
                                  {}}};
        pass.sampledImages = {hdr, bloomLevels[0]};
        TonemapPush push{};
        push.hdrTexture = slotFor(graph.view(hdr));
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
            vkCmdPushConstants(passCmd, m_pipelineLayout,
                               VK_SHADER_STAGE_VERTEX_BIT |
                                   VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(push), &push);
            vkCmdDraw(passCmd, 3, 1, 0, 0);
        };
        graph.addPass(std::move(pass));
    }

    graph.setFinalLayout(backbuffer, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    graph.execute(cmd, m_tracyCtx);

#ifdef TRACY_ENABLE
    if (m_tracyCtx != nullptr) {
        TracyVkCollect(m_tracyCtx, cmd);
    }
#endif

    VK_CHECK(vkEndCommandBuffer(cmd));
}

void Renderer::drawFrame(const Camera& camera, World& world,
                         AssetRegistry& assets) {
    ZoneScoped;

    checkShaderHotReload();

    const glm::uvec2 fb = m_window.framebufferSize();
    if (fb.x == 0 || fb.y == 0) {
        return; // minimized
    }
    if (m_window.consumeResizeFlag()) {
        recreateSwapchain();
    }

    // Assemble this frame's draws and lights from the ECS. Models still
    // importing simply contribute nothing yet.
    m_frameDraws.clear();
    m_frameLights.clear();
    for (auto [entity, transform, mesh] :
         world.registry.view<WorldTransform, MeshRenderer>().each()) {
        const ModelAsset* model = assets.tryGetModel(mesh.model);
        if (model == nullptr || mesh.meshIndex >= model->meshes.size()) {
            continue;
        }
        for (const GpuPrimitive& primitive :
             model->meshes[mesh.meshIndex].primitives) {
            m_frameDraws.push_back({transform.value, &primitive});
        }
    }
    for (auto [entity, transform, light] :
         world.registry.view<WorldTransform, PointLightComponent>().each()) {
        m_frameLights.push_back({glm::vec3(transform.value[3]), light.radius,
                                 light.color, light.intensity});
    }

    FrameData& frame = m_frames[m_frameIndex];
    VkDevice device = m_context->device();

    VK_CHECK(vkWaitForFences(device, 1, &frame.inFlightFence, VK_TRUE,
                             UINT64_MAX));

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

    const VkExtent2D extent = m_swapchain->extent();
    updateFrameConstants(frame, camera, world,
                         static_cast<float>(extent.width) /
                             static_cast<float>(extent.height));

    VK_CHECK(vkResetFences(device, 1, &frame.inFlightFence));
    VK_CHECK(vkResetCommandPool(device, frame.pool, 0));

    recordCommands(frame.cmd, imageIndex, camera, world);

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

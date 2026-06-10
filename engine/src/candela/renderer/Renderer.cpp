#include "candela/renderer/Renderer.h"

#include "candela/platform/Window.h"
#include "candela/renderer/Camera.h"

#include <tracy/Tracy.hpp>

#include <cstring>

namespace candela {

namespace {

// Must match DrawPush in mesh.slang.
struct DrawPush {
    glm::mat4 mvp;
    VkDeviceAddress vertices;
    uint32_t textureIndex;
    uint32_t flags;
    glm::vec4 baseColorFactor;
};
static_assert(sizeof(DrawPush) == 96, "DrawPush layout must match mesh.slang");

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

    VkPushConstantRange pushRange{};
    pushRange.stageFlags =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(DrawPush);

    VkDescriptorSetLayout setLayout = m_bindless->layout();
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &setLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    VK_CHECK(vkCreatePipelineLayout(m_context->device(), &layoutInfo, nullptr,
                                    &m_pipelineLayout));

    m_shaderPath = std::filesystem::path(CANDELA_SHADER_DIR) / "mesh.slang";
    CD_ASSERT(std::filesystem::exists(m_shaderPath), "Shader not found: {}",
              m_shaderPath.string());
    m_shaderTimestamp = std::filesystem::last_write_time(m_shaderPath);

    CD_ASSERT(createPipeline(), "Initial shader compilation failed");

    m_lastReloadCheck = std::chrono::steady_clock::now();
}

Renderer::~Renderer() {
    m_context->waitIdle();

    m_scene.destroy(*m_context);

#ifdef TRACY_ENABLE
    if (m_tracyCtx != nullptr) {
        TracyVkDestroy(m_tracyCtx);
    }
#endif

    VkDevice device = m_context->device();
    if (m_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, m_pipeline, nullptr);
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

void Renderer::setScene(Scene scene) {
    m_context->waitIdle();
    m_scene.destroy(*m_context);
    m_scene = std::move(scene);
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
    }
}

void Renderer::destroyFrameData() {
    VkDevice device = m_context->device();
    for (FrameData& frame : m_frames) {
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

bool Renderer::createPipeline() {
    ZoneScoped;

    VkShaderModule vs =
        m_shaderCache->get(m_shaderPath, "vsMain", ShaderStage::Vertex);
    VkShaderModule fs =
        m_shaderCache->get(m_shaderPath, "fsMain", ShaderStage::Fragment);
    if (vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE) {
        return false;
    }

    // slangc emits a single OpEntryPoint named "main" regardless of the
    // source entry name (verified with spirv-dis).
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

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
    rasterization.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType =
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Reverse-Z: clear to 0, pass what's nearer (greater depth value).
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType =
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAttachment;

    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                            VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;

    const VkFormat colorFormat = m_swapchain->format();
    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &colorFormat;
    rendering.depthAttachmentFormat = kDepthFormat;

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.pNext = &rendering;
    pipelineInfo.stageCount = 2;
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

    VkPipeline newPipeline = VK_NULL_HANDLE;
    const VkResult result = vkCreateGraphicsPipelines(
        m_context->device(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
        &newPipeline);
    if (result != VK_SUCCESS) {
        CD_ERROR("vkCreateGraphicsPipelines failed: {}", string_VkResult(result));
        return false;
    }

    if (m_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_context->device(), m_pipeline, nullptr);
    }
    m_pipeline = newPipeline;
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
    const auto timestamp = std::filesystem::last_write_time(m_shaderPath, ec);
    if (ec || timestamp == m_shaderTimestamp) {
        return;
    }
    m_shaderTimestamp = timestamp;

    CD_INFO("Shader changed, reloading: {}", m_shaderPath.string());
    m_context->waitIdle();
    if (createPipeline()) {
        CD_INFO("Shader reload OK");
    } else {
        CD_WARN("Shader reload failed — keeping previous pipeline");
    }
}

void Renderer::recordCommands(VkCommandBuffer cmd, uint32_t imageIndex,
                              const Camera& camera) {
    ZoneScoped;

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &beginInfo));

    const VkExtent2D extent = m_swapchain->extent();
    const float aspect = static_cast<float>(extent.width) /
                         static_cast<float>(extent.height);
    const glm::mat4 viewProjection = camera.viewProjection(aspect);

    RenderGraph& graph = *m_renderGraph;
    graph.begin();

    const RenderGraph::Handle backbuffer = graph.importImage(
        "backbuffer", m_swapchain->image(imageIndex),
        m_swapchain->view(imageIndex), m_swapchain->format(), extent,
        VK_IMAGE_LAYOUT_UNDEFINED);
    const RenderGraph::Handle depth =
        graph.createImage("depth", kDepthFormat, extent,
                          VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);

    RenderGraph::Pass forward;
    forward.name = "forward";
    forward.colorAttachments.push_back(
        {backbuffer, VK_ATTACHMENT_LOAD_OP_CLEAR,
         {.color = {{0.02f, 0.02f, 0.03f, 1.0f}}}});
    forward.depthAttachment = RenderGraph::Attachment{
        depth, VK_ATTACHMENT_LOAD_OP_CLEAR, {.depthStencil = {0.0f, 0}}};
    forward.execute = [&](VkCommandBuffer passCmd) {
        ZoneScopedN("forward");

        // Negative-height viewport: Y up, matching glTF/GL conventions.
        VkViewport viewport{};
        viewport.y = static_cast<float>(extent.height);
        viewport.width = static_cast<float>(extent.width);
        viewport.height = -static_cast<float>(extent.height);
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(passCmd, 0, 1, &viewport);

        VkRect2D scissor{{0, 0}, extent};
        vkCmdSetScissor(passCmd, 0, 1, &scissor);

        vkCmdBindPipeline(passCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

        VkDescriptorSet set = m_bindless->set();
        vkCmdBindDescriptorSets(passCmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_pipelineLayout, 0, 1, &set, 0, nullptr);

        for (const DrawItem& draw : m_scene.draws) {
            DrawPush push{};
            push.mvp = viewProjection * draw.transform;
            push.vertices = draw.vertexAddress;
            push.textureIndex = draw.textureIndex;
            push.flags = draw.flags;
            push.baseColorFactor = draw.baseColorFactor;
            vkCmdPushConstants(passCmd, m_pipelineLayout,
                               VK_SHADER_STAGE_VERTEX_BIT |
                                   VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(push), &push);
            vkCmdBindIndexBuffer(passCmd, draw.indexBuffer, 0,
                                 VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(passCmd, draw.indexCount, 1, 0, 0, 0);
        }
    };
    graph.addPass(std::move(forward));

    graph.setFinalLayout(backbuffer, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    graph.execute(cmd, m_tracyCtx);

#ifdef TRACY_ENABLE
    if (m_tracyCtx != nullptr) {
        TracyVkCollect(m_tracyCtx, cmd);
    }
#endif

    VK_CHECK(vkEndCommandBuffer(cmd));
}

void Renderer::drawFrame(const Camera& camera) {
    ZoneScoped;

    checkShaderHotReload();

    const glm::uvec2 fb = m_window.framebufferSize();
    if (fb.x == 0 || fb.y == 0) {
        return; // minimized
    }
    if (m_window.consumeResizeFlag()) {
        recreateSwapchain();
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

    VK_CHECK(vkResetFences(device, 1, &frame.inFlightFence));
    VK_CHECK(vkResetCommandPool(device, frame.pool, 0));

    recordCommands(frame.cmd, imageIndex, camera);

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
    VK_CHECK(vkQueueSubmit2(m_context->graphicsQueue(), 1, &submitInfo,
                            frame.inFlightFence));

    VkSwapchainKHR swapchain = m_swapchain->handle();
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &m_presentSemaphores[imageIndex];
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchain;
    presentInfo.pImageIndices = &imageIndex;

    const VkResult presentResult =
        vkQueuePresentKHR(m_context->graphicsQueue(), &presentInfo);
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

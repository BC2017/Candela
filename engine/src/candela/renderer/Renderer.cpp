#include "candela/renderer/Renderer.h"

#include "candela/platform/Window.h"

#include <tracy/Tracy.hpp>

namespace candela {

namespace {

void transitionImage(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout,
                     VkImageLayout newLayout, VkPipelineStageFlags2 srcStage,
                     VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                     VkAccessFlags2 dstAccess) {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = srcStage;
    barrier.srcAccessMask = srcAccess;
    barrier.dstStageMask = dstStage;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;

    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

} // namespace

Renderer::Renderer(Window& window) : m_window(window) {
    m_context = std::make_unique<Context>(window);

    const glm::uvec2 fb = window.framebufferSize();
    m_swapchain = std::make_unique<Swapchain>(*m_context, fb.x, fb.y);

    createFrameData();
    createPresentSemaphores();

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(float); // time

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    VK_CHECK(vkCreatePipelineLayout(m_context->device(), &layoutInfo, nullptr,
                                    &m_pipelineLayout));

    m_shaderPath = std::filesystem::path(CANDELA_SHADER_DIR) / "fullscreen.slang";
    CD_ASSERT(std::filesystem::exists(m_shaderPath), "Shader not found: {}",
              m_shaderPath.string());
    m_shaderTimestamp = std::filesystem::last_write_time(m_shaderPath);

    CD_ASSERT(createPipeline(), "Initial shader compilation failed");

    m_startTime = std::chrono::steady_clock::now();
    m_lastReloadCheck = m_startTime;
}

Renderer::~Renderer() {
    m_context->waitIdle();

    VkDevice device = m_context->device();
    if (m_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, m_pipeline, nullptr);
    }
    if (m_pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, m_pipelineLayout, nullptr);
    }
    destroyPresentSemaphores();
    destroyFrameData();
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

    auto vsWords =
        m_shaderCompiler.compile(m_shaderPath, "vsMain", ShaderStage::Vertex);
    auto fsWords =
        m_shaderCompiler.compile(m_shaderPath, "fsMain", ShaderStage::Fragment);
    if (!vsWords || !fsWords) {
        return false;
    }

    VkDevice device = m_context->device();
    VkShaderModule vs = createShaderModule(device, *vsWords);
    VkShaderModule fs = createShaderModule(device, *fsWords);

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
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType =
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

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
    pipelineInfo.pColorBlendState = &blend;
    pipelineInfo.pDynamicState = &dynamic;
    pipelineInfo.layout = m_pipelineLayout;

    VkPipeline newPipeline = VK_NULL_HANDLE;
    const VkResult result = vkCreateGraphicsPipelines(
        device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &newPipeline);

    vkDestroyShaderModule(device, vs, nullptr);
    vkDestroyShaderModule(device, fs, nullptr);

    if (result != VK_SUCCESS) {
        CD_ERROR("vkCreateGraphicsPipelines failed: {}", string_VkResult(result));
        return false;
    }

    if (m_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, m_pipeline, nullptr);
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

void Renderer::recordCommands(VkCommandBuffer cmd, uint32_t imageIndex) {
    ZoneScoped;

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &beginInfo));

    transitionImage(cmd, m_swapchain->image(imageIndex),
                    VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    VkRenderingAttachmentInfo colorAttachment{};
    colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttachment.imageView = m_swapchain->view(imageIndex);
    colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.clearValue.color = {{0.02f, 0.02f, 0.03f, 1.0f}};

    const VkExtent2D extent = m_swapchain->extent();
    VkRenderingInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    renderingInfo.renderArea = {{0, 0}, extent};
    renderingInfo.layerCount = 1;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachments = &colorAttachment;

    vkCmdBeginRendering(cmd, &renderingInfo);

    VkViewport viewport{};
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{{0, 0}, extent};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

    const float time = std::chrono::duration<float>(
                           std::chrono::steady_clock::now() - m_startTime)
                           .count();
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(float), &time);

    vkCmdDraw(cmd, 3, 1, 0, 0);

    vkCmdEndRendering(cmd);

    transitionImage(cmd, m_swapchain->image(imageIndex),
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, VK_ACCESS_2_NONE);

    VK_CHECK(vkEndCommandBuffer(cmd));
}

void Renderer::drawFrame() {
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

    recordCommands(frame.cmd, imageIndex);

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

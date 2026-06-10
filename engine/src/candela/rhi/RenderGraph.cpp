#include "candela/rhi/RenderGraph.h"

#include "candela/rhi/Context.h"

namespace candela {

RenderGraph::RenderGraph(Context& context) : m_context(context) {}

RenderGraph::~RenderGraph() {
    for (PooledImage& pooled : m_pool) {
        destroyImage(m_context, pooled.image);
    }
}

void RenderGraph::begin() {
    m_resources.clear();
    m_passes.clear();
    for (PooledImage& pooled : m_pool) {
        pooled.inUse = false;
    }
    ++m_frameNumber;
}

RenderGraph::Handle RenderGraph::importImage(std::string name, VkImage image,
                                             VkImageView view, VkFormat format,
                                             VkExtent2D extent,
                                             VkImageLayout currentLayout) {
    Resource resource;
    resource.name = std::move(name);
    resource.image = image;
    resource.view = view;
    resource.format = format;
    resource.extent = extent;
    resource.state.layout = currentLayout;
    m_resources.push_back(std::move(resource));
    return static_cast<Handle>(m_resources.size() - 1);
}

RenderGraph::Handle RenderGraph::createImage(std::string name, VkFormat format,
                                             VkExtent2D extent,
                                             VkImageUsageFlags usage) {
    // Reuse a pooled image with identical properties if one is free.
    PooledImage* found = nullptr;
    for (PooledImage& pooled : m_pool) {
        if (!pooled.inUse && pooled.image.format == format &&
            pooled.image.extent.width == extent.width &&
            pooled.image.extent.height == extent.height &&
            pooled.usage == usage) {
            found = &pooled;
            break;
        }
    }
    if (found == nullptr) {
        PooledImage pooled;
        pooled.image = createImage2D(m_context, format, extent, usage);
        pooled.usage = usage;
        m_pool.push_back(pooled);
        found = &m_pool.back();
        CD_TRACE("RenderGraph: allocated transient image '{}' {}x{} {}", name,
                 extent.width, extent.height, string_VkFormat(format));
    }
    found->inUse = true;
    found->lastUsedFrame = m_frameNumber;

    Resource resource;
    resource.name = std::move(name);
    resource.image = found->image.image;
    resource.view = found->image.view;
    resource.format = format;
    resource.extent = extent;
    resource.isDepth = (usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
    // Pooled images carry no contents across frames; treat as undefined.
    resource.state.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    m_resources.push_back(std::move(resource));
    return static_cast<Handle>(m_resources.size() - 1);
}

void RenderGraph::addPass(Pass pass) {
    m_passes.push_back(std::move(pass));
}

void RenderGraph::setFinalLayout(Handle handle, VkImageLayout layout) {
    m_resources[handle].finalLayout = layout;
}

void RenderGraph::barrierTo(VkCommandBuffer cmd, Resource& resource,
                            VkPipelineStageFlags2 stage, VkAccessFlags2 access,
                            VkImageLayout layout) {
    const bool layoutChange = resource.state.layout != layout;
    const bool hadWrites =
        (resource.state.access &
         (VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
          VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
          VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT)) != 0;
    const bool isWrite =
        (access & (VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                   VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                   VK_ACCESS_2_TRANSFER_WRITE_BIT |
                   VK_ACCESS_2_SHADER_WRITE_BIT)) != 0;
    if (!layoutChange && !hadWrites && !isWrite) {
        return; // read-after-read needs no barrier
    }

    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = resource.state.stage;
    barrier.srcAccessMask = resource.state.access;
    barrier.dstStageMask = stage;
    barrier.dstAccessMask = access;
    barrier.oldLayout = resource.state.layout;
    barrier.newLayout = layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = resource.image;
    barrier.subresourceRange.aspectMask = resource.isDepth
                                              ? VK_IMAGE_ASPECT_DEPTH_BIT
                                              : VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
    barrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;

    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);

    resource.state = {stage, access, layout};
}

void RenderGraph::execute(VkCommandBuffer cmd, TracyVkCtx tracyCtx) {
    for (Pass& pass : m_passes) {
        TracyVkZoneTransient(tracyCtx, tracyZone, cmd, pass.name.c_str(),
                             tracyCtx != nullptr);

        // Barriers for everything the pass touches.
        for (const Attachment& attachment : pass.colorAttachments) {
            barrierTo(cmd, m_resources[attachment.handle],
                      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        }
        if (pass.depthAttachment) {
            barrierTo(cmd, m_resources[pass.depthAttachment->handle],
                      VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                          VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                          VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                      VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
        }
        for (Handle handle : pass.sampledImages) {
            barrierTo(cmd, m_resources[handle],
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }

        // Dynamic rendering scope from the declared attachments.
        std::vector<VkRenderingAttachmentInfo> colorInfos;
        colorInfos.reserve(pass.colorAttachments.size());
        VkExtent2D renderArea{};
        for (const Attachment& attachment : pass.colorAttachments) {
            const Resource& resource = m_resources[attachment.handle];
            VkRenderingAttachmentInfo info{};
            info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            info.imageView = resource.view;
            info.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            info.loadOp = attachment.loadOp;
            info.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            info.clearValue = attachment.clear;
            colorInfos.push_back(info);
            renderArea = resource.extent;
        }

        VkRenderingAttachmentInfo depthInfo{};
        if (pass.depthAttachment) {
            const Resource& resource =
                m_resources[pass.depthAttachment->handle];
            depthInfo.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            depthInfo.imageView = resource.view;
            depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            depthInfo.loadOp = pass.depthAttachment->loadOp;
            depthInfo.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            depthInfo.clearValue = pass.depthAttachment->clear;
            renderArea = resource.extent;
        }

        VkRenderingInfo renderingInfo{};
        renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        renderingInfo.renderArea = {{0, 0}, renderArea};
        renderingInfo.layerCount = 1;
        renderingInfo.colorAttachmentCount =
            static_cast<uint32_t>(colorInfos.size());
        renderingInfo.pColorAttachments = colorInfos.data();
        renderingInfo.pDepthAttachment =
            pass.depthAttachment ? &depthInfo : nullptr;

        vkCmdBeginRendering(cmd, &renderingInfo);
        pass.execute(cmd);
        vkCmdEndRendering(cmd);
    }

    // Final layout requests (e.g. backbuffer → PRESENT_SRC).
    for (Resource& resource : m_resources) {
        if (resource.finalLayout &&
            resource.state.layout != *resource.finalLayout) {
            barrierTo(cmd, resource, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
                      VK_ACCESS_2_NONE, *resource.finalLayout);
        }
    }
}

} // namespace candela

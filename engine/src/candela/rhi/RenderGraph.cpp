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
    ++m_frameNumber;

    // Drop pooled images that have gone unused (e.g. pre-resize sizes).
    constexpr uint64_t kEvictAfterFrames = 120;
    std::erase_if(m_pool, [&](PooledImage& pooled) {
        if (!pooled.inUse &&
            m_frameNumber - pooled.lastUsedFrame > kEvictAfterFrames) {
            destroyImage(m_context, pooled.image);
            return true;
        }
        return false;
    });
    for (PooledImage& pooled : m_pool) {
        pooled.inUse = false;
    }
}

RenderGraph::Handle RenderGraph::importImage(std::string name, VkImage image,
                                             VkImageView view, VkFormat format,
                                             VkExtent2D extent,
                                             VkImageLayout currentLayout,
                                             VkPipelineStageFlags2 lastStage,
                                             VkAccessFlags2 lastAccess,
                                             bool isDepth) {
    Resource resource;
    resource.name = std::move(name);
    resource.image = image;
    resource.view = view;
    resource.format = format;
    resource.extent = extent;
    resource.isDepth = isDepth;
    resource.state = {lastStage, lastAccess, currentLayout};
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

void RenderGraph::execute(VkCommandBuffer cmd, TracyVkCtx tracyCtx,
                          GpuTimestamps* timestamps) {
    for (Pass& pass : m_passes) {
        TracyVkZoneTransient(tracyCtx, tracyZone, cmd, pass.name.c_str(),
                             tracyCtx != nullptr);

        const bool stamp = timestamps != nullptr &&
                           timestamps->next + 2 <= timestamps->capacity;
        if (stamp) {
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                                 timestamps->pool, timestamps->next);
        }

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
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        for (Handle handle : pass.storageImages) {
            barrierTo(cmd, m_resources[handle],
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                          VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                      VK_IMAGE_LAYOUT_GENERAL);
        }

        auto endStamp = [&] {
            if (stamp) {
                vkCmdWriteTimestamp2(cmd,
                                     VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
                                     timestamps->pool, timestamps->next + 1);
                timestamps->names.push_back(pass.name);
                timestamps->next += 2;
            }
        };

        // Compute pass: no attachments, no rendering scope.
        if (pass.colorAttachments.empty() && !pass.depthAttachment) {
            pass.execute(cmd);
            endStamp();
            continue;
        }

        // Dynamic rendering scope from the declared attachments.
        std::vector<VkRenderingAttachmentInfo> colorInfos;
        colorInfos.reserve(pass.colorAttachments.size());
        VkExtent2D renderArea{};
        auto attachmentView = [&](const Attachment& attachment) {
            return attachment.viewOverride != VK_NULL_HANDLE
                       ? attachment.viewOverride
                       : m_resources[attachment.handle].view;
        };
        auto attachmentExtent = [&](const Attachment& attachment) {
            return attachment.extentOverride.width != 0
                       ? attachment.extentOverride
                       : m_resources[attachment.handle].extent;
        };
        for (const Attachment& attachment : pass.colorAttachments) {
            VkRenderingAttachmentInfo info{};
            info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            info.imageView = attachmentView(attachment);
            info.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            info.loadOp = attachment.loadOp;
            info.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            info.clearValue = attachment.clear;
            colorInfos.push_back(info);
            renderArea = attachmentExtent(attachment);
        }

        VkRenderingAttachmentInfo depthInfo{};
        if (pass.depthAttachment) {
            depthInfo.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            depthInfo.imageView = attachmentView(*pass.depthAttachment);
            depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            depthInfo.loadOp = pass.depthAttachment->loadOp;
            depthInfo.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            depthInfo.clearValue = pass.depthAttachment->clear;
            renderArea = attachmentExtent(*pass.depthAttachment);
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
        endStamp();
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

#pragma once

#include "candela/rhi/Resources.h"
#include "candela/rhi/VulkanCommon.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <tracy/TracyVulkan.hpp>

namespace candela {

class Context;

// A frame-scoped graph of raster passes. Passes declare which images they
// render to and which they sample; the graph derives synchronization2
// barriers, allocates pooled transient images, and drives dynamic rendering.
//
// Built fresh each frame (begin → import/create → addPass* → execute); the
// transient image pool persists across frames inside the graph object.
class RenderGraph {
public:
    using Handle = uint32_t;

    explicit RenderGraph(Context& context);
    ~RenderGraph();

    RenderGraph(const RenderGraph&) = delete;
    RenderGraph& operator=(const RenderGraph&) = delete;

    void begin();

    // Wraps an externally owned image (e.g. the swapchain backbuffer).
    Handle importImage(std::string name, VkImage image, VkImageView view,
                       VkFormat format, VkExtent2D extent,
                       VkImageLayout currentLayout);

    // Creates (or reuses from the pool) a graph-owned transient image.
    Handle createImage(std::string name, VkFormat format, VkExtent2D extent,
                       VkImageUsageFlags usage);

    struct Attachment {
        Handle handle = 0;
        VkAttachmentLoadOp loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        VkClearValue clear{};
    };

    struct Pass {
        std::string name;
        std::vector<Attachment> colorAttachments;
        std::optional<Attachment> depthAttachment;
        std::vector<Handle> sampledImages;
        std::function<void(VkCommandBuffer)> execute;
    };

    void addPass(Pass pass);

    // Requests a layout the image must be in after the graph runs
    // (e.g. PRESENT_SRC for the backbuffer).
    void setFinalLayout(Handle handle, VkImageLayout layout);

    // Records barriers, dynamic rendering scopes, and pass bodies.
    // tracyCtx may be null.
    void execute(VkCommandBuffer cmd, TracyVkCtx tracyCtx);

private:
    struct ResourceState {
        VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        VkAccessFlags2 access = VK_ACCESS_2_NONE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    };

    struct Resource {
        std::string name;
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkExtent2D extent{};
        bool isDepth = false;
        ResourceState state;
        std::optional<VkImageLayout> finalLayout;
    };

    struct PooledImage {
        GpuImage image;
        VkImageUsageFlags usage = 0;
        uint64_t lastUsedFrame = 0;
        bool inUse = false;
    };

    void barrierTo(VkCommandBuffer cmd, Resource& resource,
                   VkPipelineStageFlags2 stage, VkAccessFlags2 access,
                   VkImageLayout layout);

    Context& m_context;
    std::vector<Resource> m_resources;
    std::vector<Pass> m_passes;
    std::vector<PooledImage> m_pool;
    uint64_t m_frameNumber = 0;
};

} // namespace candela

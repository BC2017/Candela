#include "candela/rhi/Resources.h"

#include "candela/rhi/Context.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace candela {

GpuBuffer createBuffer(Context& context, VkDeviceSize size,
                       VkBufferUsageFlags usage,
                       VmaAllocationCreateFlags allocFlags) {
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocInfo.flags = allocFlags;

    GpuBuffer result{};
    result.size = size;
    VK_CHECK(vmaCreateBuffer(context.allocator(), &bufferInfo, &allocInfo,
                             &result.buffer, &result.allocation, nullptr));

    if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
        VkBufferDeviceAddressInfo addressInfo{};
        addressInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        addressInfo.buffer = result.buffer;
        result.deviceAddress =
            vkGetBufferDeviceAddress(context.device(), &addressInfo);
    }
    return result;
}

void destroyBuffer(Context& context, GpuBuffer& buffer) {
    if (buffer.buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(context.allocator(), buffer.buffer, buffer.allocation);
        buffer = {};
    }
}

GpuBuffer createBufferWithData(Context& context, std::span<const std::byte> data,
                               VkBufferUsageFlags usage) {
    GpuBuffer staging = createBuffer(
        context, data.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
            VMA_ALLOCATION_CREATE_MAPPED_BIT);

    VmaAllocationInfo stagingInfo{};
    vmaGetAllocationInfo(context.allocator(), staging.allocation, &stagingInfo);
    std::memcpy(stagingInfo.pMappedData, data.data(), data.size());

    GpuBuffer result = createBuffer(context, data.size(),
                                    usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT);

    context.immediateSubmit([&](VkCommandBuffer cmd) {
        VkBufferCopy region{};
        region.size = data.size();
        vkCmdCopyBuffer(cmd, staging.buffer, result.buffer, 1, &region);
    });

    destroyBuffer(context, staging);
    return result;
}

GpuImage createImage2D(Context& context, VkFormat format, VkExtent2D extent,
                       VkImageUsageFlags usage, uint32_t mipLevels) {
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {extent.width, extent.height, 1};
    imageInfo.mipLevels = mipLevels;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = usage;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

    GpuImage result{};
    result.format = format;
    result.extent = extent;
    result.mipLevels = mipLevels;
    VK_CHECK(vmaCreateImage(context.allocator(), &imageInfo, &allocInfo,
                            &result.image, &result.allocation, nullptr));

    const bool isDepth =
        format == VK_FORMAT_D32_SFLOAT || format == VK_FORMAT_D16_UNORM ||
        format == VK_FORMAT_D24_UNORM_S8_UINT;

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = result.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask =
        isDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = mipLevels;
    viewInfo.subresourceRange.layerCount = 1;
    VK_CHECK(
        vkCreateImageView(context.device(), &viewInfo, nullptr, &result.view));
    return result;
}

void destroyImage(Context& context, GpuImage& image) {
    if (image.image != VK_NULL_HANDLE) {
        vkDestroyImageView(context.device(), image.view, nullptr);
        vmaDestroyImage(context.allocator(), image.image, image.allocation);
        image = {};
    }
}

namespace {

VkImageMemoryBarrier2 mipBarrier(VkImage image, uint32_t mipLevel,
                                 VkImageLayout oldLayout,
                                 VkImageLayout newLayout,
                                 VkPipelineStageFlags2 srcStage,
                                 VkAccessFlags2 srcAccess,
                                 VkPipelineStageFlags2 dstStage,
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
    barrier.subresourceRange.baseMipLevel = mipLevel;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    return barrier;
}

void emitBarrier(VkCommandBuffer cmd, const VkImageMemoryBarrier2& barrier) {
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

} // namespace

GpuImage createTexture2D(Context& context, const void* rgbaPixels,
                         uint32_t width, uint32_t height, bool srgb) {
    const uint32_t mipLevels =
        1 + static_cast<uint32_t>(
                std::floor(std::log2(static_cast<float>((std::max)(width, height)))));
    const VkFormat format =
        srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    const VkDeviceSize byteSize =
        static_cast<VkDeviceSize>(width) * height * 4;

    GpuImage image = createImage2D(context, format, {width, height},
                                   VK_IMAGE_USAGE_SAMPLED_BIT |
                                       VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                   mipLevels);

    GpuBuffer staging = createBuffer(
        context, byteSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
            VMA_ALLOCATION_CREATE_MAPPED_BIT);
    VmaAllocationInfo stagingInfo{};
    vmaGetAllocationInfo(context.allocator(), staging.allocation, &stagingInfo);
    std::memcpy(stagingInfo.pMappedData, rgbaPixels, byteSize);

    context.immediateSubmit([&](VkCommandBuffer cmd) {
        // Upload mip 0.
        emitBarrier(cmd, mipBarrier(image.image, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                                    VK_ACCESS_2_NONE,
                                    VK_PIPELINE_STAGE_2_COPY_BIT,
                                    VK_ACCESS_2_TRANSFER_WRITE_BIT));

        VkBufferImageCopy copy{};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = {width, height, 1};
        vkCmdCopyBufferToImage(cmd, staging.buffer, image.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

        // Blit each mip from the previous one.
        int32_t srcWidth = static_cast<int32_t>(width);
        int32_t srcHeight = static_cast<int32_t>(height);
        for (uint32_t mip = 1; mip < mipLevels; ++mip) {
            emitBarrier(cmd,
                        mipBarrier(image.image, mip - 1,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   VK_PIPELINE_STAGE_2_BLIT_BIT,
                                   VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                   VK_PIPELINE_STAGE_2_BLIT_BIT,
                                   VK_ACCESS_2_TRANSFER_READ_BIT));
            emitBarrier(cmd, mipBarrier(image.image, mip,
                                        VK_IMAGE_LAYOUT_UNDEFINED,
                                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                                        VK_ACCESS_2_NONE,
                                        VK_PIPELINE_STAGE_2_BLIT_BIT,
                                        VK_ACCESS_2_TRANSFER_WRITE_BIT));

            const int32_t dstWidth = (std::max)(srcWidth / 2, 1);
            const int32_t dstHeight = (std::max)(srcHeight / 2, 1);

            VkImageBlit blit{};
            blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.srcSubresource.mipLevel = mip - 1;
            blit.srcSubresource.layerCount = 1;
            blit.srcOffsets[1] = {srcWidth, srcHeight, 1};
            blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.dstSubresource.mipLevel = mip;
            blit.dstSubresource.layerCount = 1;
            blit.dstOffsets[1] = {dstWidth, dstHeight, 1};
            vkCmdBlitImage(cmd, image.image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                           VK_FILTER_LINEAR);

            // Source mip is final — move it to shader-read.
            emitBarrier(cmd,
                        mipBarrier(image.image, mip - 1,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                   VK_PIPELINE_STAGE_2_BLIT_BIT,
                                   VK_ACCESS_2_TRANSFER_READ_BIT,
                                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                   VK_ACCESS_2_SHADER_SAMPLED_READ_BIT));

            srcWidth = dstWidth;
            srcHeight = dstHeight;
        }

        // Last mip was only ever a blit destination.
        emitBarrier(cmd, mipBarrier(image.image, mipLevels - 1,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                    VK_PIPELINE_STAGE_2_BLIT_BIT,
                                    VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT));
    });

    destroyBuffer(context, staging);
    return image;
}

} // namespace candela

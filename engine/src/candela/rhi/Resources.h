#pragma once

#include "candela/core/Compiler.h"
#include "candela/rhi/VulkanCommon.h"

CD_PUSH_DISABLE_WARNINGS
#include <vk_mem_alloc.h>
CD_POP_WARNINGS

#include <cstddef>
#include <span>

namespace candela {

class Context;

struct GpuBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    VkDeviceAddress deviceAddress = 0; // nonzero iff created with BDA usage
};

struct GpuImage {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    uint32_t mipLevels = 1;
};

GpuBuffer createBuffer(Context& context, VkDeviceSize size,
                       VkBufferUsageFlags usage,
                       VmaAllocationCreateFlags allocFlags = 0);
void destroyBuffer(Context& context, GpuBuffer& buffer);

// Creates a device-local buffer and uploads `data` through a staging buffer.
GpuBuffer createBufferWithData(Context& context, std::span<const std::byte> data,
                               VkBufferUsageFlags usage);

GpuImage createImage2D(Context& context, VkFormat format, VkExtent2D extent,
                       VkImageUsageFlags usage, uint32_t mipLevels = 1);
void destroyImage(Context& context, GpuImage& image);

// Uploads RGBA8 pixels, generates a full mip chain via blits, and leaves the
// image in SHADER_READ_ONLY_OPTIMAL.
GpuImage createTexture2D(Context& context, const void* rgbaPixels,
                         uint32_t width, uint32_t height, bool srgb);

} // namespace candela

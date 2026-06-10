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
                       VkImageUsageFlags usage, uint32_t mipLevels = 1,
                       uint32_t arrayLayers = 1);
void destroyImage(Context& context, GpuImage& image);

// Uploads RGBA8 pixels, generates a full mip chain via blits, and leaves the
// image in SHADER_READ_ONLY_OPTIMAL.
GpuImage createTexture2D(Context& context, const void* rgbaPixels,
                         uint32_t width, uint32_t height, bool srgb);

// Uploads RGBA float pixels (no mips) into an RGBA32F image, ending in
// SHADER_READ_ONLY_OPTIMAL. Used for equirectangular HDRIs.
GpuImage createTextureHDR(Context& context, const float* rgbaPixels,
                          uint32_t width, uint32_t height);

// Cube map (6 layers, cube-compatible) with a CUBE view across all mips.
GpuImage createCubeImage(Context& context, VkFormat format, uint32_t size,
                         VkImageUsageFlags usage, uint32_t mipLevels = 1);

// Extra view into an existing image (e.g. one cascade layer of an array, or
// one mip of a cube as a 2D array for storage writes). Caller destroys it.
VkImageView createImageView(Context& context, const GpuImage& image,
                            VkImageViewType viewType, uint32_t baseMip,
                            uint32_t mipCount, uint32_t baseLayer,
                            uint32_t layerCount);

} // namespace candela

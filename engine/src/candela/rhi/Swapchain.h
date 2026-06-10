#pragma once

#include "candela/rhi/VulkanCommon.h"

#include <vector>

namespace candela {

class Context;

class Swapchain {
public:
    Swapchain(Context& context, uint32_t width, uint32_t height);
    ~Swapchain();

    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    // Caller must ensure the device is idle first.
    void recreate(uint32_t width, uint32_t height);

    VkSwapchainKHR handle() const { return m_swapchain; }
    VkFormat format() const { return m_format; }
    VkExtent2D extent() const { return m_extent; }
    uint32_t imageCount() const { return static_cast<uint32_t>(m_images.size()); }
    VkImage image(uint32_t index) const { return m_images[index]; }
    VkImageView view(uint32_t index) const { return m_views[index]; }

private:
    void create(uint32_t width, uint32_t height);
    void destroyViews();

    Context& m_context;
    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    VkFormat m_format = VK_FORMAT_UNDEFINED;
    VkExtent2D m_extent{};
    std::vector<VkImage> m_images;
    std::vector<VkImageView> m_views;
};

} // namespace candela

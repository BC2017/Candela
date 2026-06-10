#include "candela/rhi/Swapchain.h"

#include "candela/rhi/Context.h"

#include <VkBootstrap.h>

namespace candela {

Swapchain::Swapchain(Context& context, uint32_t width, uint32_t height)
    : m_context(context) {
    create(width, height);
}

Swapchain::~Swapchain() {
    destroyViews();
    vkDestroySwapchainKHR(m_context.device(), m_swapchain, nullptr);
}

void Swapchain::recreate(uint32_t width, uint32_t height) {
    create(width, height);
}

void Swapchain::create(uint32_t width, uint32_t height) {
    VkSwapchainKHR old = m_swapchain;

    auto result =
        vkb::SwapchainBuilder{m_context.physicalDevice(), m_context.device(),
                              m_context.surface()}
            .set_desired_format({VK_FORMAT_B8G8R8A8_SRGB,
                                 VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
            .set_desired_present_mode(VK_PRESENT_MODE_MAILBOX_KHR)
            .set_desired_extent(width, height)
            .set_old_swapchain(old)
            .build();
    CD_ASSERT(result.has_value(), "Failed to create swapchain: {}",
              result.error().message());
    vkb::Swapchain vkbSwapchain = result.value();

    destroyViews();
    if (old != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(m_context.device(), old, nullptr);
    }

    m_swapchain = vkbSwapchain.swapchain;
    m_format = vkbSwapchain.image_format;
    m_extent = vkbSwapchain.extent;
    m_images = vkbSwapchain.get_images().value();
    m_views = vkbSwapchain.get_image_views().value();

    CD_TRACE("Swapchain: {}x{}, {} images, format {}", m_extent.width,
             m_extent.height, m_images.size(), string_VkFormat(m_format));
}

void Swapchain::destroyViews() {
    for (VkImageView view : m_views) {
        vkDestroyImageView(m_context.device(), view, nullptr);
    }
    m_views.clear();
}

} // namespace candela

#pragma once

#include "candela/core/Compiler.h"
#include "candela/rhi/VulkanCommon.h"

CD_PUSH_DISABLE_WARNINGS
#include <vk_mem_alloc.h>
CD_POP_WARNINGS

#include <functional>
#include <mutex>
#include <string>

namespace candela {

class Window;

// Owns the Vulkan instance, device, queues, and the VMA allocator.
// Requires a Vulkan 1.3 device with dynamic rendering and synchronization2.
class Context {
public:
    explicit Context(Window& window);
    ~Context();

    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    VkInstance instance() const { return m_instance; }
    VkPhysicalDevice physicalDevice() const { return m_physicalDevice; }
    VkDevice device() const { return m_device; }
    VkSurfaceKHR surface() const { return m_surface; }
    VkQueue graphicsQueue() const { return m_graphicsQueue; }
    uint32_t graphicsQueueFamily() const { return m_graphicsQueueFamily; }
    VmaAllocator allocator() const { return m_allocator; }
    const std::string& gpuName() const { return m_gpuName; }

    // True when VK_KHR_acceleration_structure + VK_KHR_ray_query are enabled.
    // RT effects must fall back to raster when false.
    bool rayTracingSupported() const { return m_rayTracingSupported; }
    uint32_t scratchAlignment() const { return m_scratchAlignment; }
    // Nanoseconds per timestamp tick (GPU profiling).
    float timestampPeriod() const { return m_timestampPeriod; }

    void waitIdle() const;

    // Records, submits, and waits on the graphics queue. For uploads and
    // one-off GPU work; not for per-frame use. Thread-safe: callers serialize
    // on an internal mutex, so asset imports may run on job threads.
    // NOTE: the per-frame submit in Renderer stays main-thread only.
    void immediateSubmit(const std::function<void(VkCommandBuffer)>& record) const;

    // Command buffer backing immediateSubmit; in the initial state between
    // calls (Tracy's GPU context init wants exactly that).
    VkCommandBuffer immediateCommandBuffer() const { return m_immediateCmd; }

    // VkQueue access requires external synchronization across ALL submitters.
    // immediateSubmit and waitIdle lock this internally; the renderer must
    // hold it around its per-frame vkQueueSubmit2/vkQueuePresentKHR.
    std::mutex& queueMutex() const { return m_immediateMutex; }

private:
    VkInstance m_instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_graphicsQueue = VK_NULL_HANDLE;
    uint32_t m_graphicsQueueFamily = 0;
    VmaAllocator m_allocator = VK_NULL_HANDLE;
    VkCommandPool m_immediatePool = VK_NULL_HANDLE;
    VkCommandBuffer m_immediateCmd = VK_NULL_HANDLE;
    VkFence m_immediateFence = VK_NULL_HANDLE;
    mutable std::mutex m_immediateMutex;
    bool m_rayTracingSupported = false;
    uint32_t m_scratchAlignment = 256;
    float m_timestampPeriod = 1.0f;
    std::string m_gpuName;
};

} // namespace candela

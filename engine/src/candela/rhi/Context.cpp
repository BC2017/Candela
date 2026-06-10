#include "candela/rhi/Context.h"

#include "candela/platform/Window.h"

#include <VkBootstrap.h>

namespace candela {

namespace {

VkBool32 VKAPI_PTR debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*types*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void* /*userData*/) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        CD_ERROR("[vulkan] {}", data->pMessage);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        CD_WARN("[vulkan] {}", data->pMessage);
    } else {
        CD_TRACE("[vulkan] {}", data->pMessage);
    }
    return VK_FALSE;
}

} // namespace

Context::Context(Window& window) {
    VK_CHECK(volkInitialize());

    auto systemInfo = vkb::SystemInfo::get_system_info();
    CD_ASSERT(systemInfo.has_value(), "Failed to query Vulkan system info: {}",
              systemInfo.error().message());
    const bool validation = systemInfo.value().validation_layers_available;
    if (!validation) {
        CD_WARN("Vulkan validation layers are not available — running without");
    }

    auto instanceResult = vkb::InstanceBuilder{}
                              .set_app_name("Candela")
                              .set_engine_name("Candela")
                              .require_api_version(1, 3, 0)
                              .request_validation_layers(validation)
                              .set_debug_callback(debugCallback)
                              .build();
    CD_ASSERT(instanceResult.has_value(), "Failed to create Vulkan instance: {}",
              instanceResult.error().message());
    vkb::Instance vkbInstance = instanceResult.value();
    m_instance = vkbInstance.instance;
    m_debugMessenger = vkbInstance.debug_messenger;
    volkLoadInstance(m_instance);

    m_surface = window.createSurface(m_instance);

    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.dynamicRendering = VK_TRUE;
    features13.synchronization2 = VK_TRUE;

    VkPhysicalDeviceVulkan12Features features12{};
    features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features12.bufferDeviceAddress = VK_TRUE;
    features12.descriptorIndexing = VK_TRUE;

    // Slang's SV_VertexID lowering declares the SPIR-V DrawParameters
    // capability; also needed later for gl_DrawID in GPU-driven rendering.
    VkPhysicalDeviceVulkan11Features features11{};
    features11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    features11.shaderDrawParameters = VK_TRUE;

    auto physicalResult = vkb::PhysicalDeviceSelector{vkbInstance}
                              .set_surface(m_surface)
                              .set_minimum_version(1, 3)
                              .set_required_features_13(features13)
                              .set_required_features_12(features12)
                              .set_required_features_11(features11)
                              .select();
    CD_ASSERT(physicalResult.has_value(),
              "No GPU supports the required Vulkan 1.3 features: {}",
              physicalResult.error().message());
    vkb::PhysicalDevice vkbPhysical = physicalResult.value();
    m_physicalDevice = vkbPhysical.physical_device;
    m_gpuName = vkbPhysical.properties.deviceName;
    CD_INFO("Selected GPU: {} (driver {}.{}.{})", m_gpuName,
            VK_VERSION_MAJOR(vkbPhysical.properties.driverVersion),
            VK_VERSION_MINOR(vkbPhysical.properties.driverVersion),
            VK_VERSION_PATCH(vkbPhysical.properties.driverVersion));

    auto deviceResult = vkb::DeviceBuilder{vkbPhysical}.build();
    CD_ASSERT(deviceResult.has_value(), "Failed to create Vulkan device: {}",
              deviceResult.error().message());
    vkb::Device vkbDevice = deviceResult.value();
    m_device = vkbDevice.device;
    volkLoadDevice(m_device);

    m_graphicsQueue = vkbDevice.get_queue(vkb::QueueType::graphics).value();
    m_graphicsQueueFamily =
        vkbDevice.get_queue_index(vkb::QueueType::graphics).value();

    VmaVulkanFunctions vmaFunctions{};
    vmaFunctions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    vmaFunctions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo allocatorInfo{};
    allocatorInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    allocatorInfo.physicalDevice = m_physicalDevice;
    allocatorInfo.device = m_device;
    allocatorInfo.instance = m_instance;
    allocatorInfo.pVulkanFunctions = &vmaFunctions;
    allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3;
    VK_CHECK(vmaCreateAllocator(&allocatorInfo, &m_allocator));
}

Context::~Context() {
    vmaDestroyAllocator(m_allocator);
    vkDestroyDevice(m_device, nullptr);
    vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
    if (m_debugMessenger != VK_NULL_HANDLE) {
        vkDestroyDebugUtilsMessengerEXT(m_instance, m_debugMessenger, nullptr);
    }
    vkDestroyInstance(m_instance, nullptr);
}

void Context::waitIdle() const {
    VK_CHECK(vkDeviceWaitIdle(m_device));
}

} // namespace candela

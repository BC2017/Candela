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
    features12.runtimeDescriptorArray = VK_TRUE;
    features12.descriptorBindingPartiallyBound = VK_TRUE;
    features12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    features12.descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;
    features12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    features12.scalarBlockLayout = VK_TRUE;

    // Slang's SV_VertexID lowering declares the SPIR-V DrawParameters
    // capability; also needed later for gl_DrawID in GPU-driven rendering.
    VkPhysicalDeviceVulkan11Features features11{};
    features11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    features11.shaderDrawParameters = VK_TRUE;

    VkPhysicalDeviceFeatures features10{};
    features10.samplerAnisotropy = VK_TRUE;

    // Ray tracing (acceleration structures + ray queries) is requested first;
    // if no device qualifies, fall back to raster-only.
    VkPhysicalDeviceAccelerationStructureFeaturesKHR asFeatures{};
    asFeatures.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
    asFeatures.accelerationStructure = VK_TRUE;
    asFeatures.descriptorBindingAccelerationStructureUpdateAfterBind = VK_TRUE;

    VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures{};
    rayQueryFeatures.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
    rayQueryFeatures.rayQuery = VK_TRUE;

    auto selectDevice = [&](bool withRayTracing) {
        vkb::PhysicalDeviceSelector selector{vkbInstance};
        selector.set_surface(m_surface)
            .set_minimum_version(1, 3)
            .set_required_features_13(features13)
            .set_required_features_12(features12)
            .set_required_features_11(features11)
            .set_required_features(features10);
        if (withRayTracing) {
            selector
                .add_required_extension(
                    VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME)
                .add_required_extension(VK_KHR_RAY_QUERY_EXTENSION_NAME)
                .add_required_extension(
                    VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME)
                .add_required_extension_features(asFeatures)
                .add_required_extension_features(rayQueryFeatures);
        }
        return selector.select();
    };

    auto physicalResult = selectDevice(true);
    m_rayTracingSupported = physicalResult.has_value();
    if (!m_rayTracingSupported) {
        CD_WARN("No ray-tracing-capable GPU — RT effects fall back to raster");
        physicalResult = selectDevice(false);
    }
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

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = m_graphicsQueueFamily;
    VK_CHECK(vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_immediatePool));

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = m_immediatePool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(m_device, &allocInfo, &m_immediateCmd));

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VK_CHECK(vkCreateFence(m_device, &fenceInfo, nullptr, &m_immediateFence));
}

void Context::immediateSubmit(
    const std::function<void(VkCommandBuffer)>& record) const {
    std::scoped_lock lock(m_immediateMutex);
    VK_CHECK(vkResetCommandBuffer(m_immediateCmd, 0));

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(m_immediateCmd, &beginInfo));

    record(m_immediateCmd);

    VK_CHECK(vkEndCommandBuffer(m_immediateCmd));

    VkCommandBufferSubmitInfo cmdInfo{};
    cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmdInfo.commandBuffer = m_immediateCmd;

    VkSubmitInfo2 submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submitInfo.commandBufferInfoCount = 1;
    submitInfo.pCommandBufferInfos = &cmdInfo;
    VK_CHECK(vkQueueSubmit2(m_graphicsQueue, 1, &submitInfo, m_immediateFence));

    VK_CHECK(vkWaitForFences(m_device, 1, &m_immediateFence, VK_TRUE,
                             UINT64_MAX));
    VK_CHECK(vkResetFences(m_device, 1, &m_immediateFence));
}

Context::~Context() {
    vkDestroyFence(m_device, m_immediateFence, nullptr);
    vkDestroyCommandPool(m_device, m_immediatePool, nullptr);
    vmaDestroyAllocator(m_allocator);
    vkDestroyDevice(m_device, nullptr);
    vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
    if (m_debugMessenger != VK_NULL_HANDLE) {
        vkDestroyDebugUtilsMessengerEXT(m_instance, m_debugMessenger, nullptr);
    }
    vkDestroyInstance(m_instance, nullptr);
}

void Context::waitIdle() const {
    // vkDeviceWaitIdle requires external sync with queue submission — hold
    // the immediate-submit mutex so worker-thread uploads can't race it.
    std::scoped_lock lock(m_immediateMutex);
    VK_CHECK(vkDeviceWaitIdle(m_device));
}

} // namespace candela

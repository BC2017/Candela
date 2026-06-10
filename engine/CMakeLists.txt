add_library(candela STATIC
    src/candela/core/Log.cpp
    src/candela/platform/Window.cpp
    src/candela/rhi/Context.cpp
    src/candela/rhi/ShaderCompiler.cpp
    src/candela/rhi/Swapchain.cpp
    src/candela/rhi/VmaUsage.cpp
    src/candela/renderer/Renderer.cpp
)

target_include_directories(candela PUBLIC src)

target_compile_definitions(candela PUBLIC
    VK_NO_PROTOTYPES
    GLFW_INCLUDE_NONE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RADIANS
    CANDELA_SHADER_DIR="${CMAKE_SOURCE_DIR}/shaders"
)

target_link_libraries(candela PUBLIC
    volk::volk
    vk-bootstrap::vk-bootstrap
    GPUOpen::VulkanMemoryAllocator
    glfw
    glm::glm
    spdlog::spdlog
    Tracy::TracyClient
    Vulkan::Headers
)

candela_apply_warnings(candela)

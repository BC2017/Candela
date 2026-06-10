add_library(candela STATIC
    src/candela/assets/AssetRegistry.cpp
    src/candela/assets/ModelAsset.cpp
    src/candela/assets/StbUsage.cpp
    src/candela/core/Jobs.cpp
    src/candela/core/Log.cpp
    src/candela/platform/Input.cpp
    src/candela/platform/Window.cpp
    src/candela/rhi/Bindless.cpp
    src/candela/rhi/Context.cpp
    src/candela/rhi/RenderGraph.cpp
    src/candela/rhi/Resources.cpp
    src/candela/rhi/ShaderCompiler.cpp
    src/candela/rhi/Swapchain.cpp
    src/candela/rhi/VmaUsage.cpp
    src/candela/renderer/Camera.cpp
    src/candela/renderer/Cascades.cpp
    src/candela/renderer/IBL.cpp
    src/candela/renderer/Renderer.cpp
    src/candela/scene/SceneSerializer.cpp
    src/candela/scene/World.cpp
)

target_include_directories(candela PUBLIC src)

if(MSVC)
  # fastgltf's accessor templates trip C4100 when instantiated under /W4.
  set_source_files_properties(src/candela/assets/ModelAsset.cpp
      PROPERTIES COMPILE_OPTIONS "/wd4100")
else()
  # Same warning, GCC spelling; -Wredundant-move fires inside fastgltf 0.8
  # headers (fixed upstream after our pinned tag).
  set_source_files_properties(src/candela/assets/ModelAsset.cpp
      PROPERTIES COMPILE_OPTIONS "-Wno-unused-parameter;-Wno-redundant-move")
  # Third-party implementation TUs are too warning-heavy for -Wall -Werror.
  set_source_files_properties(
      src/candela/rhi/VmaUsage.cpp
      src/candela/assets/StbUsage.cpp
      PROPERTIES COMPILE_OPTIONS "-w")
endif()

target_compile_definitions(candela PUBLIC
    VK_NO_PROTOTYPES
    GLFW_INCLUDE_NONE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RADIANS
    CANDELA_SHADER_DIR="${CMAKE_SOURCE_DIR}/shaders"
    CANDELA_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets"
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
    fastgltf::fastgltf
    stb
    EnTT::EnTT
    nlohmann_json::nlohmann_json
)

candela_apply_warnings(candela)

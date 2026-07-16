add_library(candela STATIC
    src/candela/assets/AnimationInfo.cpp
    src/candela/assets/AssetRegistry.cpp
    src/candela/assets/BlendImporter.cpp
    src/candela/assets/ModelAsset.cpp
    src/candela/assets/StbUsage.cpp
    src/candela/audio/AudioEngine.cpp
    src/candela/audio/AudioSystem.cpp
    src/candela/audio/MiniaudioImpl.cpp
    src/candela/core/Jobs.cpp
    src/candela/core/Log.cpp
    src/candela/physics/PhysicsSystem.cpp
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
  set_source_files_properties(
      src/candela/assets/ModelAsset.cpp
      src/candela/assets/AnimationInfo.cpp
      PROPERTIES COMPILE_OPTIONS "/wd4100")
else()
  # Same warning, GCC spelling; -Wredundant-move fires inside fastgltf 0.8
  # headers (fixed upstream after our pinned tag).
  set_source_files_properties(
      src/candela/assets/ModelAsset.cpp
      src/candela/assets/AnimationInfo.cpp
      PROPERTIES COMPILE_OPTIONS "-Wno-unused-parameter;-Wno-redundant-move")
  # Third-party implementation TUs are too warning-heavy for -Wall -Werror.
  # MiniaudioImpl.cpp already wraps the header in CD_PUSH_DISABLE_WARNINGS
  # (which covers MSVC via warning(push,0)); GCC needs the blanket -w too.
  set_source_files_properties(
      src/candela/rhi/VmaUsage.cpp
      src/candela/assets/StbUsage.cpp
      src/candela/audio/MiniaudioImpl.cpp
      PROPERTIES COMPILE_OPTIONS "-w")
endif()

# PhysicsSystem.cpp includes Jolt's warning-heavy headers; disable warnings for
# the whole TU (same treatment as the VmaUsage.cpp / StbUsage.cpp third-party
# TUs above) so warnings-as-errors stays intact for the rest of the engine.
if(MSVC)
  set_source_files_properties(src/candela/physics/PhysicsSystem.cpp
      PROPERTIES COMPILE_OPTIONS "/W0")
else()
  set_source_files_properties(src/candela/physics/PhysicsSystem.cpp
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
    miniaudio
)

# Jolt is an implementation detail of PhysicsSystem.cpp: PRIVATE keeps its
# include dirs and ABI defines out of candela's consumers, while the static-lib
# link requirement still reaches the final executables at link time.
target_link_libraries(candela PRIVATE Jolt)

candela_apply_warnings(candela)

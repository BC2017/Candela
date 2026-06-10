#pragma once

#include "candela/rhi/Bindless.h"
#include "candela/rhi/Context.h"
#include "candela/rhi/RenderGraph.h"
#include "candela/rhi/ShaderCompiler.h"
#include "candela/rhi/Swapchain.h"
#include "candela/scene/GltfScene.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <vector>

namespace candela {

class Window;
class Camera;

// Phase 1 renderer: a render-graph-driven forward pass drawing a glTF scene
// with bindless textures, vertex pulling, reverse-Z depth, and Tracy GPU
// zones. Shader edits hot-reload the pipeline.
class Renderer {
public:
    explicit Renderer(Window& window);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Replaces the active scene (destroys the previous one).
    void setScene(Scene scene);

    void drawFrame(const Camera& camera);

    Context& context() { return *m_context; }
    Bindless& bindless() { return *m_bindless; }

private:
    static constexpr uint32_t kFramesInFlight = 2;
    static constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

    struct FrameData {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkSemaphore acquireSemaphore = VK_NULL_HANDLE;
        VkFence inFlightFence = VK_NULL_HANDLE;
    };

    void createFrameData();
    void destroyFrameData();
    void createPresentSemaphores();
    void destroyPresentSemaphores();
    bool createPipeline();
    void recreateSwapchain();
    void checkShaderHotReload();
    void recordCommands(VkCommandBuffer cmd, uint32_t imageIndex,
                        const Camera& camera);

    Window& m_window;
    std::unique_ptr<Context> m_context;
    std::unique_ptr<Swapchain> m_swapchain;
    std::unique_ptr<Bindless> m_bindless;
    std::unique_ptr<RenderGraph> m_renderGraph;
    std::unique_ptr<ShaderCache> m_shaderCache;

    FrameData m_frames[kFramesInFlight];
    std::vector<VkSemaphore> m_presentSemaphores;

    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;

    Scene m_scene;

    TracyVkCtx m_tracyCtx = nullptr;

    std::filesystem::path m_shaderPath;
    std::filesystem::file_time_type m_shaderTimestamp;
    std::chrono::steady_clock::time_point m_lastReloadCheck;
    uint32_t m_frameIndex = 0;
};

} // namespace candela

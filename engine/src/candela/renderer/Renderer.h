#pragma once

#include "candela/rhi/Context.h"
#include "candela/rhi/ShaderCompiler.h"
#include "candela/rhi/Swapchain.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <vector>

namespace candela {

class Window;

// Phase 0 renderer: clears the swapchain and draws a fullscreen triangle via
// dynamic rendering, with shader hot reload. The render graph replaces the
// hardcoded pass in Phase 1.
class Renderer {
public:
    explicit Renderer(Window& window);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    void drawFrame();

    Context& context() { return *m_context; }

private:
    static constexpr uint32_t kFramesInFlight = 2;

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

    // Compiles shaders and builds the pipeline. On failure the previous
    // pipeline is kept, so a broken hot-reload edit never crashes the app.
    bool createPipeline();

    void recreateSwapchain();
    void checkShaderHotReload();
    void recordCommands(VkCommandBuffer cmd, uint32_t imageIndex);

    Window& m_window;
    std::unique_ptr<Context> m_context;
    std::unique_ptr<Swapchain> m_swapchain;

    FrameData m_frames[kFramesInFlight];
    std::vector<VkSemaphore> m_presentSemaphores; // one per swapchain image

    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;

    ShaderCompiler m_shaderCompiler;
    std::filesystem::path m_shaderPath;
    std::filesystem::file_time_type m_shaderTimestamp;
    std::chrono::steady_clock::time_point m_lastReloadCheck;

    std::chrono::steady_clock::time_point m_startTime;
    uint32_t m_frameIndex = 0;
};

} // namespace candela

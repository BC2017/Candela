#pragma once

#include "candela/assets/ModelAsset.h"
#include "candela/renderer/IBL.h"
#include "candela/rhi/Bindless.h"
#include "candela/rhi/Context.h"
#include "candela/rhi/RenderGraph.h"
#include "candela/rhi/ShaderCompiler.h"
#include "candela/rhi/Swapchain.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <unordered_map>
#include <vector>

namespace candela {

class Window;
class Camera;
class World;
class AssetRegistry;

// Deferred PBR through the render graph — 4 shadow cascades → G-buffer →
// Cook-Torrance + IBL lighting → dual-Kawase bloom → ACES tonemap. Draws,
// lights, and scene settings come from the ECS world; geometry referenced by
// MeshRenderers streams in as the asset registry finishes imports.
class Renderer {
public:
    explicit Renderer(Window& window);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    void drawFrame(const Camera& camera, World& world, AssetRegistry& assets);

    Context& context() { return *m_context; }
    Bindless& bindless() { return *m_bindless; }

private:
    static constexpr uint32_t kFramesInFlight = 2;
    static constexpr uint32_t kShadowMapSize = 2048;
    static constexpr uint32_t kBloomLevels = 5;
    static constexpr float kMaxShadowDistance = 30.0f;
    static constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
    static constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

    struct FrameData {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkSemaphore acquireSemaphore = VK_NULL_HANDLE;
        VkFence inFlightFence = VK_NULL_HANDLE;
        GpuBuffer constants;
        void* constantsMapped = nullptr;
    };

    struct PipelineDesc {
        std::string shaderFile;
        std::string vsEntry = "vsMain";
        std::string fsEntry; // empty = depth-only
        std::vector<VkFormat> colorFormats;
        VkFormat depthFormat = VK_FORMAT_UNDEFINED;
        VkCompareOp depthCompare = VK_COMPARE_OP_GREATER_OR_EQUAL;
        bool depthTest = false;
        bool depthWrite = false;
        bool depthBias = false;
        bool additiveBlend = false;
        VkCullModeFlags cullMode = VK_CULL_MODE_NONE;
    };

    void createFrameData();
    void destroyFrameData();
    void createPresentSemaphores();
    void destroyPresentSemaphores();

    // A primitive scheduled for this frame (assembled from the ECS each
    // frame; 100s of draws — revisit when scenes grow).
    struct FrameDraw {
        glm::mat4 transform;
        const GpuPrimitive* primitive;
    };

    struct FrameLight {
        glm::vec3 position;
        float radius;
        glm::vec3 color;
        float intensity;
    };

    VkPipeline buildPipeline(const PipelineDesc& desc);
    bool createPipelines();

    void recreateSwapchain();
    void checkShaderHotReload();
    void updateFrameConstants(FrameData& frame, const Camera& camera,
                              const World& world, float aspect);
    void recordCommands(VkCommandBuffer cmd, uint32_t imageIndex,
                        const Camera& camera, const World& world);

    // Stable bindless slot for a (typically transient) image view, sampled
    // with the clamp sampler. Slots are appended, never freed — acceptable
    // until the editor phase introduces proper lifetime tracking.
    uint32_t slotFor(VkImageView view);

    Window& m_window;
    std::unique_ptr<Context> m_context;
    std::unique_ptr<Swapchain> m_swapchain;
    std::unique_ptr<Bindless> m_bindless;
    std::unique_ptr<RenderGraph> m_renderGraph;
    std::unique_ptr<ShaderCache> m_shaderCache;

    FrameData m_frames[kFramesInFlight];
    std::vector<VkSemaphore> m_presentSemaphores;

    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_gbufferPipeline = VK_NULL_HANDLE;
    VkPipeline m_shadowPipeline = VK_NULL_HANDLE;
    VkPipeline m_lightingPipeline = VK_NULL_HANDLE;
    VkPipeline m_bloomDownPipeline = VK_NULL_HANDLE;
    VkPipeline m_bloomUpPipeline = VK_NULL_HANDLE;
    VkPipeline m_tonemapPipeline = VK_NULL_HANDLE;

    // Persistent shadow cascade array (D32, kShadowMapSize², 4 layers).
    GpuImage m_shadowMap;
    VkImageView m_shadowLayerViews[4] = {};
    uint32_t m_shadowBindlessSlot = 0;
    bool m_shadowMapEverRendered = false;

    IBL m_ibl;
    uint32_t m_irradianceSlot = 0;
    uint32_t m_prefilteredSlot = 0;
    uint32_t m_brdfLutSlot = 0;

    std::unordered_map<VkImageView, uint32_t> m_viewSlots;

    std::vector<FrameDraw> m_frameDraws;
    std::vector<FrameLight> m_frameLights;
    TracyVkCtx m_tracyCtx = nullptr;

    std::filesystem::path m_shaderDir;
    std::filesystem::file_time_type m_shaderTimestamp;
    std::chrono::steady_clock::time_point m_lastReloadCheck;
    uint32_t m_frameIndex = 0;
};

} // namespace candela

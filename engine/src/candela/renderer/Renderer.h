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

// Renderer debug views (lighting-pass output switch).
enum class DebugView : uint32_t {
    Final = 0,
    Albedo = 1,
    Normals = 2,
    MetallicRoughness = 3,
    Occlusion = 4,
    Cascades = 5,
    Reflections = 6,
};

// Per-frame renderer diagnostics (editor Stats panel). GPU timings resolve a
// couple of frames late (read back when the recording frame's fence clears).
struct GpuPassTiming {
    std::string name;
    float milliseconds = 0.0f;
};

struct RenderStats {
    uint32_t drawCalls = 0; // scene draws per geometry pass
    uint32_t culledDraws = 0;
    uint32_t triangles = 0;
    uint32_t pointLights = 0;
    uint32_t rtInstances = 0;
    bool rayTracingSupported = false;
    VkExtent2D sceneExtent{};
    float gpuTotalMs = 0.0f;
    std::vector<GpuPassTiming> gpuPasses;
};

// Editor-facing per-frame options. With viewportExtent set, the scene renders
// into a persistent offscreen image (shown by the editor's viewport panel)
// and the swapchain receives only the UI pass.
struct RenderOptions {
    VkExtent2D viewportExtent{0, 0}; // 0 = render scene directly to backbuffer
    std::function<void(VkCommandBuffer)> recordUI; // drawn into the backbuffer
    std::optional<glm::ivec2> pickPixel; // viewport-relative; result polls later
    DebugView debugView = DebugView::Final;
};

// Deferred PBR through the render graph — 4 shadow cascades → G-buffer
// (+ entity-ID target) → Cook-Torrance + IBL lighting → dual-Kawase bloom →
// ACES tonemap. Draws, lights, and scene settings come from the ECS world;
// geometry referenced by MeshRenderers streams in as imports finish.
class Renderer {
public:
    explicit Renderer(Window& window);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    void drawFrame(const Camera& camera, World& world, AssetRegistry& assets,
                   const RenderOptions& options);
    void drawFrame(const Camera& camera, World& world, AssetRegistry& assets) {
        drawFrame(camera, world, assets, RenderOptions{});
    }

    // Offscreen viewport image (valid after the first editor-mode frame).
    // The generation counter bumps when the image is recreated (resize) so
    // the editor can re-register its ImGui texture.
    VkImageView viewportImageView() const { return m_viewportImage.view; }
    VkExtent2D viewportImageExtent() const { return m_viewportImage.extent; }
    uint64_t viewportGeneration() const { return m_viewportGeneration; }

    // Entity-ID readback from a previous pickPixel request: 0 = none/miss,
    // otherwise entt id + 1. Ready once that frame's fence has recycled.
    std::optional<uint32_t> takePickResult();

    // Saves the next presented backbuffer to a PNG (written a couple of
    // frames later when the GPU work completes). Headless-friendly
    // verification and flythrough capture.
    void requestScreenshot(std::filesystem::path path);

    const RenderStats& stats() const { return m_stats; }

    Context& context() { return *m_context; }
    Bindless& bindless() { return *m_bindless; }

    // For the editor's ImGui Vulkan backend setup.
    VkFormat swapchainFormat() const { return m_swapchain->format(); }
    uint32_t swapchainImageCount() const { return m_swapchain->imageCount(); }

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
        // Ray tracing (per frame in flight so rebuilds never race the GPU).
        VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
        GpuBuffer tlasBuffer;
        GpuBuffer tlasScratch;
        GpuBuffer tlasInstances; // VkAccelerationStructureInstanceKHR[], mapped
        void* tlasInstancesMapped = nullptr;
        GpuBuffer instanceData; // InstanceDataGPU[], mapped
        void* instanceDataMapped = nullptr;
        uint32_t tlasSlot = 0; // bindless AccelStruct index
        // GPU pass timings (2 timestamps per pass).
        VkQueryPool queryPool = VK_NULL_HANDLE;
        std::vector<std::string> timestampNames;
    };
    static constexpr uint32_t kMaxTimestampQueries = 64;

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
        uint32_t entityId; // entt id + 1
        // Inside the camera frustum (G-buffer pass). Shadow passes always
        // draw — casters behind the camera still shadow what's visible.
        bool visible = true;
    };

    struct FrameLight {
        glm::vec3 position;
        float radius;
        glm::vec3 color;
        float intensity;
    };

    VkPipeline buildPipeline(const PipelineDesc& desc);
    VkPipeline buildComputePipeline(const std::string& shaderFile,
                                    const std::string& entry);
    bool createPipelines();
    void createRayTracingResources();
    // Fills instance/data buffers from the ECS; returns instance count.
    uint32_t fillRayTracingInstances(FrameData& frame, World& world,
                                     AssetRegistry& assets);
    void buildTLAS(VkCommandBuffer cmd, FrameData& frame,
                   uint32_t instanceCount);

    // Ping-pong history pair for temporal accumulation (AO, reflections,
    // TAA). Persistent across frames; invalidated on resize.
    struct TemporalTarget {
        GpuImage images[2];
        bool everUsed[2] = {false, false};
        uint32_t writeIndex = 0;
        bool valid = false; // false → accumulation pass copies raw through
    };

    void recreateSwapchain();
    void checkShaderHotReload();
    void ensureViewportImage(VkExtent2D extent);
    void ensureTemporalTarget(TemporalTarget& target, VkExtent2D extent);
    void destroyTemporalTarget(TemporalTarget& target);
    uint32_t storageSlotFor(VkImageView view);
    void updateFrameConstants(FrameData& frame, const Camera& camera,
                              const World& world, float aspect,
                              VkExtent2D sceneExtent);
    void recordCommands(VkCommandBuffer cmd, uint32_t imageIndex,
                        const Camera& camera, const World& world,
                        const RenderOptions& options);

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

    VkPipelineCache m_pipelineCache = VK_NULL_HANDLE; // persisted to disk
    std::filesystem::path m_pipelineCachePath;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_gbufferPipeline = VK_NULL_HANDLE;
    VkPipeline m_shadowPipeline = VK_NULL_HANDLE;
    VkPipeline m_lightingPipeline = VK_NULL_HANDLE;
    VkPipeline m_bloomDownPipeline = VK_NULL_HANDLE;
    VkPipeline m_bloomUpPipeline = VK_NULL_HANDLE;
    VkPipeline m_tonemapPipeline = VK_NULL_HANDLE;
    VkPipeline m_reflectionsPipeline = VK_NULL_HANDLE; // null without RT
    VkPipeline m_aoPipeline = VK_NULL_HANDLE;          // null without RT
    VkPipeline m_temporalPipeline = VK_NULL_HANDLE;

    static constexpr uint32_t kMaxRTInstances = 1024;
    static constexpr uint32_t kMaxRTInstanceData = 4096;
    bool m_rtSupported = false;
    uint32_t m_rtInstanceCount = 0; // filled per frame before recording
    std::unordered_map<VkImageView, uint32_t> m_storageSlots;

    TemporalTarget m_aoTemporal;
    TemporalTarget m_reflectionsTemporal;
    TemporalTarget m_taaTemporal;

    glm::mat4 m_prevViewProjNoJitter{1.0f};
    bool m_hasPrevViewProj = false;
    uint64_t m_frameCounter = 0;

    RenderStats m_stats;

    // Persistent shadow cascade array (D32, kShadowMapSize², 4 layers).
    GpuImage m_shadowMap;
    VkImageView m_shadowLayerViews[4] = {};
    uint32_t m_shadowBindlessSlot = 0;
    bool m_shadowMapEverRendered = false;

    IBL m_ibl;
    IBL m_iblPlaceholder; // retired after the real bake; freed in the dtor
    bool m_iblReady = false;
    uint32_t m_irradianceSlot = 0;
    uint32_t m_prefilteredSlot = 0;
    uint32_t m_brdfLutSlot = 0;
    uint32_t m_environmentSlot = 0;

    std::unordered_map<VkImageView, uint32_t> m_viewSlots;

    std::vector<FrameDraw> m_frameDraws;
    std::vector<FrameLight> m_frameLights;

    // Editor offscreen viewport (persistent — ImGui's descriptor references
    // it across frames).
    GpuImage m_viewportImage;
    uint64_t m_viewportGeneration = 0;
    bool m_viewportEverRendered = false;

    // One in-flight pick: 4-byte host-visible readback buffer.
    GpuBuffer m_pickBuffer;
    void* m_pickMapped = nullptr;
    uint32_t m_pickFrameSlot = UINT32_MAX; // frame slot that recorded the copy
    bool m_pickPending = false;
    std::optional<uint32_t> m_pickResult;

    // One in-flight screenshot readback.
    GpuBuffer m_screenshotBuffer;
    void* m_screenshotMapped = nullptr;
    std::filesystem::path m_screenshotPath; // empty = none requested
    VkExtent2D m_screenshotExtent{};
    uint32_t m_screenshotFrameSlot = UINT32_MAX;
    bool m_screenshotPending = false;

    TracyVkCtx m_tracyCtx = nullptr;

    std::filesystem::path m_shaderDir;
    std::filesystem::file_time_type m_shaderTimestamp;
    std::chrono::steady_clock::time_point m_lastReloadCheck;
    uint32_t m_frameIndex = 0;
};

} // namespace candela

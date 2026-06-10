#pragma once

#include "candela/rhi/VulkanCommon.h"

#include <mutex>

namespace candela {

class Context;

// One global update-after-bind descriptor set (set 0) with three bindings:
//   binding 0 — 2D combined image samplers (material + render-target reads)
//   binding 1 — cube maps (IBL)
//   binding 2 — 2D array textures (shadow cascades)
// Resources register once and are addressed by index from any shader.
// Slots can be overwritten with update() when an underlying view is recreated.
class Bindless {
public:
    enum class Kind : uint32_t { Texture2D = 0, Cube = 1, Array2D = 2 };

    static constexpr uint32_t kMaxTextures2D = 16384;
    static constexpr uint32_t kMaxCubes = 64;
    static constexpr uint32_t kMaxArrays2D = 64;

    explicit Bindless(Context& context);
    ~Bindless();

    Bindless(const Bindless&) = delete;
    Bindless& operator=(const Bindless&) = delete;

    // Thread-safe: asset imports register textures from job threads.
    uint32_t add(Kind kind, VkImageView view, VkSampler sampler);
    void update(Kind kind, uint32_t index, VkImageView view, VkSampler sampler);

    VkDescriptorSetLayout layout() const { return m_layout; }
    VkDescriptorSet set() const { return m_set; }

    // Trilinear + anisotropic repeat — material textures.
    VkSampler defaultSampler() const { return m_defaultSampler; }
    // Linear clamp-to-edge — render-target reads, IBL, bloom.
    VkSampler clampSampler() const { return m_clampSampler; }

private:
    void write(Kind kind, uint32_t index, VkImageView view, VkSampler sampler);

    Context& m_context;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_layout = VK_NULL_HANDLE;
    VkDescriptorSet m_set = VK_NULL_HANDLE;
    VkSampler m_defaultSampler = VK_NULL_HANDLE;
    VkSampler m_clampSampler = VK_NULL_HANDLE;
    uint32_t m_nextIndex[3] = {0, 0, 0};
    std::mutex m_mutex;
};

} // namespace candela

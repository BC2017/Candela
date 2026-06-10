#pragma once

#include "candela/rhi/VulkanCommon.h"

namespace candela {

class Context;

// One global update-after-bind descriptor set: an array of combined image
// samplers at set 0, binding 0. Textures register once and are addressed by
// index from any shader for the lifetime of the engine.
class Bindless {
public:
    static constexpr uint32_t kMaxTextures = 16384;

    explicit Bindless(Context& context);
    ~Bindless();

    Bindless(const Bindless&) = delete;
    Bindless& operator=(const Bindless&) = delete;

    // Registers a texture and returns its global index.
    uint32_t add(VkImageView view, VkSampler sampler);

    VkDescriptorSetLayout layout() const { return m_layout; }
    VkDescriptorSet set() const { return m_set; }

    // Default trilinear+aniso repeat sampler for material textures.
    VkSampler defaultSampler() const { return m_defaultSampler; }

private:
    Context& m_context;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_layout = VK_NULL_HANDLE;
    VkDescriptorSet m_set = VK_NULL_HANDLE;
    VkSampler m_defaultSampler = VK_NULL_HANDLE;
    uint32_t m_nextIndex = 0;
};

} // namespace candela

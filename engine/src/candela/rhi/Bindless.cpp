#include "candela/rhi/Bindless.h"

#include "candela/rhi/Context.h"

namespace candela {

namespace {
constexpr uint32_t kBindingCounts[3] = {Bindless::kMaxTextures2D,
                                        Bindless::kMaxCubes,
                                        Bindless::kMaxArrays2D};
} // namespace

Bindless::Bindless(Context& context) : m_context(context) {
    VkDevice device = context.device();

    VkDescriptorSetLayoutBinding bindings[3]{};
    VkDescriptorBindingFlags bindingFlags[3]{};
    for (uint32_t i = 0; i < 3; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = kBindingCounts[i];
        bindings[i].stageFlags = VK_SHADER_STAGE_ALL;
        bindingFlags[i] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                          VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
    }

    VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{};
    flagsInfo.sType =
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    flagsInfo.bindingCount = 3;
    flagsInfo.pBindingFlags = bindingFlags;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.pNext = &flagsInfo;
    layoutInfo.flags =
        VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layoutInfo.bindingCount = 3;
    layoutInfo.pBindings = bindings;
    VK_CHECK(
        vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_layout));

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount =
        kMaxTextures2D + kMaxCubes + kMaxArrays2D;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    VK_CHECK(vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_pool));

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_pool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_layout;
    VK_CHECK(vkAllocateDescriptorSets(device, &allocInfo, &m_set));

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.anisotropyEnable = VK_TRUE;
    samplerInfo.maxAnisotropy = 16.0f;
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    VK_CHECK(
        vkCreateSampler(device, &samplerInfo, nullptr, &m_defaultSampler));

    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1.0f;
    VK_CHECK(vkCreateSampler(device, &samplerInfo, nullptr, &m_clampSampler));
}

Bindless::~Bindless() {
    VkDevice device = m_context.device();
    vkDestroySampler(device, m_clampSampler, nullptr);
    vkDestroySampler(device, m_defaultSampler, nullptr);
    vkDestroyDescriptorPool(device, m_pool, nullptr);
    vkDestroyDescriptorSetLayout(device, m_layout, nullptr);
}

void Bindless::write(Kind kind, uint32_t index, VkImageView view,
                     VkSampler sampler) {
    VkDescriptorImageInfo imageInfo{};
    imageInfo.sampler = sampler;
    imageInfo.imageView = view;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet writeInfo{};
    writeInfo.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writeInfo.dstSet = m_set;
    writeInfo.dstBinding = static_cast<uint32_t>(kind);
    writeInfo.dstArrayElement = index;
    writeInfo.descriptorCount = 1;
    writeInfo.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writeInfo.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(m_context.device(), 1, &writeInfo, 0, nullptr);
}

uint32_t Bindless::add(Kind kind, VkImageView view, VkSampler sampler) {
    const uint32_t slot = static_cast<uint32_t>(kind);
    CD_ASSERT(m_nextIndex[slot] < kBindingCounts[slot],
              "Bindless binding {} full", slot);
    const uint32_t index = m_nextIndex[slot]++;
    write(kind, index, view, sampler);
    return index;
}

void Bindless::update(Kind kind, uint32_t index, VkImageView view,
                      VkSampler sampler) {
    CD_ASSERT(index < m_nextIndex[static_cast<uint32_t>(kind)],
              "update() of unallocated bindless slot {}", index);
    write(kind, index, view, sampler);
}

} // namespace candela

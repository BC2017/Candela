#include "candela/rhi/Bindless.h"

#include "candela/rhi/Context.h"

namespace candela {

Bindless::Bindless(Context& context) : m_context(context) {
    VkDevice device = context.device();

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = kMaxTextures;
    binding.stageFlags = VK_SHADER_STAGE_ALL;

    const VkDescriptorBindingFlags bindingFlags =
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
        VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{};
    flagsInfo.sType =
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    flagsInfo.bindingCount = 1;
    flagsInfo.pBindingFlags = &bindingFlags;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.pNext = &flagsInfo;
    layoutInfo.flags =
        VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    VK_CHECK(
        vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_layout));

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = kMaxTextures;

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
}

Bindless::~Bindless() {
    VkDevice device = m_context.device();
    vkDestroySampler(device, m_defaultSampler, nullptr);
    vkDestroyDescriptorPool(device, m_pool, nullptr);
    vkDestroyDescriptorSetLayout(device, m_layout, nullptr);
}

uint32_t Bindless::add(VkImageView view, VkSampler sampler) {
    CD_ASSERT(m_nextIndex < kMaxTextures, "Bindless texture table full");
    const uint32_t index = m_nextIndex++;

    VkDescriptorImageInfo imageInfo{};
    imageInfo.sampler = sampler;
    imageInfo.imageView = view;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = m_set;
    write.dstBinding = 0;
    write.dstArrayElement = index;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(m_context.device(), 1, &write, 0, nullptr);

    return index;
}

} // namespace candela

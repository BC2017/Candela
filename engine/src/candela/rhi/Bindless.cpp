#include "candela/rhi/Bindless.h"

#include "candela/rhi/Context.h"

namespace candela {

namespace {

constexpr uint32_t kBindingCounts[Bindless::kBindingCount] = {
    Bindless::kMaxTextures2D, Bindless::kMaxCubes, Bindless::kMaxArrays2D,
    Bindless::kMaxAccelStructs, Bindless::kMaxStorageImages};

VkDescriptorType bindingType(uint32_t binding) {
    switch (binding) {
    case 3: return VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    case 4: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    default: return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    }
}

} // namespace

Bindless::Bindless(Context& context) : m_context(context) {
    VkDevice device = context.device();
    // The acceleration-structure binding only exists when the device has RT.
    const bool rayTracing = context.rayTracingSupported();

    VkDescriptorSetLayoutBinding bindings[kBindingCount]{};
    VkDescriptorBindingFlags bindingFlags[kBindingCount]{};
    uint32_t bindingCount = 0;
    for (uint32_t i = 0; i < kBindingCount; ++i) {
        if (i == 3 && !rayTracing) {
            continue;
        }
        bindings[bindingCount].binding = i;
        bindings[bindingCount].descriptorType = bindingType(i);
        bindings[bindingCount].descriptorCount = kBindingCounts[i];
        bindings[bindingCount].stageFlags = VK_SHADER_STAGE_ALL;
        bindingFlags[bindingCount] =
            VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
        ++bindingCount;
    }

    VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{};
    flagsInfo.sType =
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    flagsInfo.bindingCount = bindingCount;
    flagsInfo.pBindingFlags = bindingFlags;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.pNext = &flagsInfo;
    layoutInfo.flags =
        VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layoutInfo.bindingCount = bindingCount;
    layoutInfo.pBindings = bindings;
    VK_CHECK(
        vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_layout));

    VkDescriptorPoolSize poolSizes[3]{};
    uint32_t poolSizeCount = 0;
    poolSizes[poolSizeCount++] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  kMaxTextures2D + kMaxCubes + kMaxArrays2D};
    poolSizes[poolSizeCount++] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                  kMaxStorageImages};
    if (rayTracing) {
        poolSizes[poolSizeCount++] = {
            VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, kMaxAccelStructs};
    }

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = poolSizeCount;
    poolInfo.pPoolSizes = poolSizes;
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
    imageInfo.imageLayout = kind == Kind::StorageImage
                                ? VK_IMAGE_LAYOUT_GENERAL
                                : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet writeInfo{};
    writeInfo.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writeInfo.dstSet = m_set;
    writeInfo.dstBinding = static_cast<uint32_t>(kind);
    writeInfo.dstArrayElement = index;
    writeInfo.descriptorCount = 1;
    writeInfo.descriptorType = bindingType(static_cast<uint32_t>(kind));
    writeInfo.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(m_context.device(), 1, &writeInfo, 0, nullptr);
}

uint32_t Bindless::add(Kind kind, VkImageView view, VkSampler sampler) {
    std::scoped_lock lock(m_mutex);
    const uint32_t slot = static_cast<uint32_t>(kind);
    CD_ASSERT(m_nextIndex[slot] < kBindingCounts[slot],
              "Bindless binding {} full", slot);
    const uint32_t index = m_nextIndex[slot]++;
    write(kind, index, view, sampler);
    return index;
}

void Bindless::update(Kind kind, uint32_t index, VkImageView view,
                      VkSampler sampler) {
    std::scoped_lock lock(m_mutex);
    CD_ASSERT(index < m_nextIndex[static_cast<uint32_t>(kind)],
              "update() of unallocated bindless slot {}", index);
    write(kind, index, view, sampler);
}

uint32_t Bindless::addAccelerationStructure(VkAccelerationStructureKHR tlas) {
    std::scoped_lock lock(m_mutex);
    constexpr uint32_t slot = static_cast<uint32_t>(Kind::AccelStruct);
    CD_ASSERT(m_context.rayTracingSupported(),
              "addAccelerationStructure without ray tracing support");
    CD_ASSERT(m_nextIndex[slot] < kBindingCounts[slot],
              "Bindless AS binding full");
    const uint32_t index = m_nextIndex[slot]++;

    VkWriteDescriptorSetAccelerationStructureKHR asInfo{};
    asInfo.sType =
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    asInfo.accelerationStructureCount = 1;
    asInfo.pAccelerationStructures = &tlas;

    VkWriteDescriptorSet writeInfo{};
    writeInfo.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writeInfo.pNext = &asInfo;
    writeInfo.dstSet = m_set;
    writeInfo.dstBinding = slot;
    writeInfo.dstArrayElement = index;
    writeInfo.descriptorCount = 1;
    writeInfo.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    vkUpdateDescriptorSets(m_context.device(), 1, &writeInfo, 0, nullptr);
    return index;
}

} // namespace candela

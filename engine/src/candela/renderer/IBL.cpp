#include "candela/renderer/IBL.h"

#include "candela/rhi/Context.h"
#include "candela/rhi/ShaderCompiler.h"

#include <stb_image.h>
#include <tracy/Tracy.hpp>

#include <chrono>
#include <vector>

namespace candela {

void IBL::destroy(Context& context) {
    destroyImage(context, environment);
    destroyImage(context, irradiance);
    destroyImage(context, prefiltered);
    destroyImage(context, brdfLut);
}

namespace {

// Minimal one-shot compute plumbing for the precompute dispatches:
// set 0 = { binding 0: sampled input, binding 1: storage output }.
class ComputeRunner {
public:
    ComputeRunner(Context& context, VkSampler sampler)
        : m_context(context), m_sampler(sampler) {
        VkDescriptorSetLayoutBinding bindings[2]{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = 2;
        layoutInfo.pBindings = bindings;
        VK_CHECK(vkCreateDescriptorSetLayout(context.device(), &layoutInfo,
                                             nullptr, &m_setLayout));

        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pushRange.size = 16;

        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType =
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &m_setLayout;
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &pushRange;
        VK_CHECK(vkCreatePipelineLayout(context.device(), &pipelineLayoutInfo,
                                        nullptr, &m_pipelineLayout));

        VkDescriptorPoolSize poolSizes[2]{};
        poolSizes[0] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64};
        poolSizes[1] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 64};
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = 64;
        poolInfo.poolSizeCount = 2;
        poolInfo.pPoolSizes = poolSizes;
        VK_CHECK(vkCreateDescriptorPool(context.device(), &poolInfo, nullptr,
                                        &m_pool));
    }

    ~ComputeRunner() {
        VkDevice device = m_context.device();
        for (VkPipeline pipeline : m_pipelines) {
            vkDestroyPipeline(device, pipeline, nullptr);
        }
        vkDestroyDescriptorPool(device, m_pool, nullptr);
        vkDestroyPipelineLayout(device, m_pipelineLayout, nullptr);
        vkDestroyDescriptorSetLayout(device, m_setLayout, nullptr);
    }

    VkPipeline pipeline(VkShaderModule shader) {
        VkComputePipelineCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        info.stage.module = shader;
        info.stage.pName = "main";
        info.layout = m_pipelineLayout;
        VkPipeline pipeline = VK_NULL_HANDLE;
        VK_CHECK(vkCreateComputePipelines(m_context.device(), VK_NULL_HANDLE,
                                          1, &info, nullptr, &pipeline));
        m_pipelines.push_back(pipeline);
        return pipeline;
    }

    // Binds input/output, pushes 16 bytes of constants, dispatches.
    void dispatch(VkCommandBuffer cmd, VkPipeline pipeline, VkImageView input,
                  VkImageView output, const void* push, uint32_t groupsX,
                  uint32_t groupsY, uint32_t groupsZ) {
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = m_pool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &m_setLayout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        VK_CHECK(vkAllocateDescriptorSets(m_context.device(), &allocInfo, &set));

        VkDescriptorImageInfo inputInfo{};
        inputInfo.sampler = m_sampler;
        inputInfo.imageView = input;
        inputInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorImageInfo outputInfo{};
        outputInfo.imageView = output;
        outputInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet writes[2]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = set;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &inputInfo;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = set;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].pImageInfo = &outputInfo;
        vkUpdateDescriptorSets(m_context.device(), 2, writes, 0, nullptr);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                m_pipelineLayout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, 16, push);
        vkCmdDispatch(cmd, groupsX, groupsY, groupsZ);
    }

private:
    Context& m_context;
    VkSampler m_sampler;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    std::vector<VkPipeline> m_pipelines;
};

void wholeImageBarrier(VkCommandBuffer cmd, VkImage image,
                       VkImageLayout oldLayout, VkImageLayout newLayout,
                       VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                       VkPipelineStageFlags2 dstStage,
                       VkAccessFlags2 dstAccess) {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = srcStage;
    barrier.srcAccessMask = srcAccess;
    barrier.dstStageMask = dstStage;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
    barrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;

    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

} // namespace

IBL placeholderIBL(Context& context) {
    IBL ibl;
    const VkImageUsageFlags usage =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ibl.irradiance =
        createCubeImage(context, VK_FORMAT_R16G16B16A16_SFLOAT, 1, usage);
    ibl.prefiltered =
        createCubeImage(context, VK_FORMAT_R16G16B16A16_SFLOAT, 1, usage);
    ibl.brdfLut =
        createImage2D(context, VK_FORMAT_R16G16_SFLOAT, {1, 1}, usage);

    context.immediateSubmit([&](VkCommandBuffer cmd) {
        for (GpuImage* image :
             {&ibl.irradiance, &ibl.prefiltered, &ibl.brdfLut}) {
            wholeImageBarrier(cmd, image->image, VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                              VK_PIPELINE_STAGE_2_COPY_BIT,
                              VK_ACCESS_2_TRANSFER_WRITE_BIT);
            VkClearColorValue black{};
            VkImageSubresourceRange range{};
            range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            range.levelCount = VK_REMAINING_MIP_LEVELS;
            range.layerCount = VK_REMAINING_ARRAY_LAYERS;
            vkCmdClearColorImage(cmd, image->image,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black,
                                 1, &range);
            wholeImageBarrier(cmd, image->image,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              VK_PIPELINE_STAGE_2_COPY_BIT,
                              VK_ACCESS_2_TRANSFER_WRITE_BIT,
                              VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                              VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        }
    });
    return ibl;
}

IBL precomputeIBL(Context& context, ShaderCache& shaders,
                  const std::filesystem::path& hdriPath) {
    ZoneScoped;
    const auto start = std::chrono::steady_clock::now();

    // --- Load (or synthesize) the equirectangular source. ---
    GpuImage equirect;
    int width = 0;
    int height = 0;
    float* pixels = nullptr;
    if (std::filesystem::exists(hdriPath)) {
        int channels = 0;
        pixels = stbi_loadf(hdriPath.string().c_str(), &width, &height,
                            &channels, 4);
    }
    if (pixels != nullptr) {
        equirect = createTextureHDR(context, pixels,
                                    static_cast<uint32_t>(width),
                                    static_cast<uint32_t>(height));
        stbi_image_free(pixels);
        CD_INFO("IBL: loaded HDRI {} ({}x{})", hdriPath.string(), width,
                height);
    } else {
        // Neutral overcast gradient so the engine lights sensibly without
        // the asset download.
        CD_WARN("IBL: HDRI not found at {}, using neutral sky",
                hdriPath.string());
        const uint32_t w = 64;
        const uint32_t h = 32;
        std::vector<float> sky(w * h * 4);
        for (uint32_t y = 0; y < h; ++y) {
            const float t = static_cast<float>(y) / (h - 1); // 0 top, 1 bottom
            const float value = t < 0.5f ? 1.2f - t : 0.25f;
            for (uint32_t x = 0; x < w; ++x) {
                float* texel = &sky[(y * w + x) * 4];
                texel[0] = value;
                texel[1] = value;
                texel[2] = value * 1.05f;
                texel[3] = 1.0f;
            }
        }
        equirect = createTextureHDR(context, sky.data(), w, h);
    }

    const std::filesystem::path shaderPath =
        std::filesystem::path(CANDELA_SHADER_DIR) / "ibl.slang";
    VkShaderModule equirectToCube =
        shaders.get(shaderPath, "equirectToCube", ShaderStage::Compute);
    VkShaderModule irradianceConvolve =
        shaders.get(shaderPath, "irradianceConvolve", ShaderStage::Compute);
    VkShaderModule prefilterGGX =
        shaders.get(shaderPath, "prefilterGGX", ShaderStage::Compute);
    VkShaderModule integrateBRDF =
        shaders.get(shaderPath, "integrateBRDF", ShaderStage::Compute);
    CD_ASSERT(equirectToCube && irradianceConvolve && prefilterGGX &&
                  integrateBRDF,
              "IBL shader compilation failed");

    IBL ibl;
    constexpr uint32_t kEnvSize = 512;
    constexpr uint32_t kIrradianceSize = 32;
    constexpr uint32_t kPrefilteredSize = 128;
    constexpr uint32_t kBrdfLutSize = 512;
    const VkImageUsageFlags cubeUsage =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;

    // Environment cube gets a mip chain so prefilterGGX can sample rough
    // lobes without fireflies.
    const uint32_t envMips = 6;
    ibl.environment = createCubeImage(context, VK_FORMAT_R16G16B16A16_SFLOAT,
                                      kEnvSize, cubeUsage |
                                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                          VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                      envMips);
    ibl.irradiance = createCubeImage(context, VK_FORMAT_R16G16B16A16_SFLOAT,
                                     kIrradianceSize, cubeUsage);
    ibl.prefiltered = createCubeImage(context, VK_FORMAT_R16G16B16A16_SFLOAT,
                                      kPrefilteredSize, cubeUsage,
                                      IBL::kPrefilteredMips);
    ibl.brdfLut = createImage2D(context, VK_FORMAT_R16G16_SFLOAT,
                                {kBrdfLutSize, kBrdfLutSize}, cubeUsage);

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    VkSampler sampler = VK_NULL_HANDLE;
    VK_CHECK(vkCreateSampler(context.device(), &samplerInfo, nullptr, &sampler));

    // Per-mip 2D-array storage views for cube writes; destroyed after submit.
    std::vector<VkImageView> scratchViews;

    {
    ComputeRunner runner(context, sampler);

    VkPipeline equirectPipeline = runner.pipeline(equirectToCube);
    VkPipeline irradiancePipeline = runner.pipeline(irradianceConvolve);
    VkPipeline prefilterPipeline = runner.pipeline(prefilterGGX);
    VkPipeline brdfPipeline = runner.pipeline(integrateBRDF);

    auto cubeMipView = [&](const GpuImage& image, uint32_t mip) {
        VkImageView view = createImageView(context, image,
                                           VK_IMAGE_VIEW_TYPE_2D_ARRAY, mip, 1,
                                           0, 6);
        scratchViews.push_back(view);
        return view;
    };

    struct Push {
        float roughness;
        uint32_t size;
        uint32_t pad0;
        uint32_t pad1;
    };

    context.immediateSubmit([&](VkCommandBuffer cmd) {
        // Equirect → environment cube mip 0.
        wholeImageBarrier(cmd, ibl.environment.image,
                          VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                          VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        Push push{0.0f, kEnvSize, 0, 0};
        runner.dispatch(cmd, equirectPipeline, equirect.view,
                        cubeMipView(ibl.environment, 0), &push, kEnvSize / 8,
                        kEnvSize / 8, 6);

        // Mip chain for the environment cube via blits.
        wholeImageBarrier(cmd, ibl.environment.image, VK_IMAGE_LAYOUT_GENERAL,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_BLIT_BIT,
                          VK_ACCESS_2_TRANSFER_WRITE_BIT);
        int32_t mipSize = kEnvSize;
        for (uint32_t mip = 1; mip < envMips; ++mip) {
            VkImageMemoryBarrier2 toSrc{};
            toSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            toSrc.srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
            toSrc.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            toSrc.dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
            toSrc.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            toSrc.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            toSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toSrc.image = ibl.environment.image;
            toSrc.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            toSrc.subresourceRange.baseMipLevel = mip - 1;
            toSrc.subresourceRange.levelCount = 1;
            toSrc.subresourceRange.layerCount = 6;
            VkDependencyInfo dependency{};
            dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dependency.imageMemoryBarrierCount = 1;
            dependency.pImageMemoryBarriers = &toSrc;
            vkCmdPipelineBarrier2(cmd, &dependency);

            VkImageBlit blit{};
            blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.srcSubresource.mipLevel = mip - 1;
            blit.srcSubresource.layerCount = 6;
            blit.srcOffsets[1] = {mipSize, mipSize, 1};
            blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.dstSubresource.mipLevel = mip;
            blit.dstSubresource.layerCount = 6;
            blit.dstOffsets[1] = {mipSize / 2, mipSize / 2, 1};
            vkCmdBlitImage(cmd, ibl.environment.image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           ibl.environment.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                           VK_FILTER_LINEAR);
            mipSize /= 2;
        }
        // To shader-read for the convolution passes. After the blit loop,
        // mips [0, last) are TRANSFER_SRC and the last mip is TRANSFER_DST —
        // two ranged barriers (an UNDEFINED oldLayout would discard contents).
        {
            VkImageMemoryBarrier2 barriers[2]{};
            for (VkImageMemoryBarrier2& barrier : barriers) {
                barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                barrier.srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
                barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
                barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.image = ibl.environment.image;
                barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                barrier.subresourceRange.layerCount = 6;
            }
            barriers[0].srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barriers[0].subresourceRange.baseMipLevel = 0;
            barriers[0].subresourceRange.levelCount = envMips - 1;
            barriers[1].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barriers[1].subresourceRange.baseMipLevel = envMips - 1;
            barriers[1].subresourceRange.levelCount = 1;

            VkDependencyInfo dependency{};
            dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dependency.imageMemoryBarrierCount = 2;
            dependency.pImageMemoryBarriers = barriers;
            vkCmdPipelineBarrier2(cmd, &dependency);
        }

        // Irradiance convolution.
        wholeImageBarrier(cmd, ibl.irradiance.image, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_GENERAL,
                          VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        push = {0.0f, kIrradianceSize, 0, 0};
        runner.dispatch(cmd, irradiancePipeline, ibl.environment.view,
                        cubeMipView(ibl.irradiance, 0), &push,
                        kIrradianceSize / 8, kIrradianceSize / 8, 6);

        // GGX prefilter, one dispatch per roughness mip.
        wholeImageBarrier(cmd, ibl.prefiltered.image, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_GENERAL,
                          VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        for (uint32_t mip = 0; mip < IBL::kPrefilteredMips; ++mip) {
            const uint32_t mipSizePx = kPrefilteredSize >> mip;
            push.roughness = static_cast<float>(mip) /
                             static_cast<float>(IBL::kPrefilteredMips - 1);
            push.size = mipSizePx;
            runner.dispatch(cmd, prefilterPipeline, ibl.environment.view,
                            cubeMipView(ibl.prefiltered, mip), &push,
                            (mipSizePx + 7) / 8, (mipSizePx + 7) / 8, 6);
        }

        // BRDF LUT.
        wholeImageBarrier(cmd, ibl.brdfLut.image, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_GENERAL,
                          VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        push = {0.0f, kBrdfLutSize, 0, 0};
        runner.dispatch(cmd, brdfPipeline, equirect.view,
                        ibl.brdfLut.view, &push, kBrdfLutSize / 8,
                        kBrdfLutSize / 8, 1);

        // Results to sampled layout for the lighting pass.
        for (VkImage image : {ibl.irradiance.image, ibl.prefiltered.image,
                              ibl.brdfLut.image}) {
            wholeImageBarrier(cmd, image, VK_IMAGE_LAYOUT_GENERAL,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                              VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                              VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                              VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        }
    });

    } // ~ComputeRunner (destroys pipelines, pool, layouts)

    vkDestroySampler(context.device(), sampler, nullptr);
    for (VkImageView view : scratchViews) {
        vkDestroyImageView(context.device(), view, nullptr);
    }
    destroyImage(context, equirect);

    const auto elapsed = std::chrono::duration<float, std::milli>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    CD_INFO("IBL precompute finished in {:.0f} ms", elapsed);
    return ibl;
}

} // namespace candela

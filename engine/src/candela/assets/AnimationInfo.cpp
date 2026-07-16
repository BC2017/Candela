#include "candela/assets/AnimationInfo.h"

#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>

#include <cstdint>
#include <cstdio>
#include <string>

namespace candela {

void printAnimationInfo(const std::filesystem::path& path) {
    auto data = fastgltf::GltfDataBuffer::FromPath(path);
    if (data.error() != fastgltf::Error::None) {
        std::printf("animinfo: failed to read %s\n", path.string().c_str());
        return;
    }

    fastgltf::Parser parser;
    auto loaded = parser.loadGltf(data.get(), path.parent_path(),
                                  fastgltf::Options::LoadExternalBuffers);
    if (loaded.error() != fastgltf::Error::None) {
        std::printf("animinfo: failed to parse %s (%s)\n",
                    path.string().c_str(),
                    std::string(fastgltf::getErrorMessage(loaded.error()))
                        .c_str());
        return;
    }
    fastgltf::Asset& asset = loaded.get();

    std::printf("animinfo: %s — %zu skins, %zu animations\n",
                path.filename().string().c_str(), asset.skins.size(),
                asset.animations.size());

    for (size_t s = 0; s < asset.skins.size(); ++s) {
        const auto& skin = asset.skins[s];
        std::printf("  skin[%zu]: %zu joints, skeletonRoot=%d\n", s,
                    skin.joints.size(),
                    skin.skeleton ? static_cast<int>(*skin.skeleton) : -1);
    }

    for (size_t a = 0; a < asset.animations.size(); ++a) {
        const auto& anim = asset.animations[a];
        float duration = 0.0f;
        uint32_t tCount = 0;
        uint32_t rCount = 0;
        uint32_t sCount = 0;
        for (const auto& channel : anim.channels) {
            switch (channel.path) {
            case fastgltf::AnimationPath::Translation:
                ++tCount;
                break;
            case fastgltf::AnimationPath::Rotation:
                ++rCount;
                break;
            case fastgltf::AnimationPath::Scale:
                ++sCount;
                break;
            default:
                break;
            }
            const auto& sampler = anim.samplers[channel.samplerIndex];
            fastgltf::iterateAccessor<float>(
                asset, asset.accessors[sampler.inputAccessor],
                [&](float t) { duration = t > duration ? t : duration; });
        }
        std::printf(
            "  clip[%zu] \"%s\"  dur %.3fs  %zu channels (T:%u R:%u S:%u)\n", a,
            std::string(anim.name).c_str(), static_cast<double>(duration),
            anim.channels.size(), tCount, rCount, sCount);
    }
}

} // namespace candela

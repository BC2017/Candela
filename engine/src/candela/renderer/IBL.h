#pragma once

#include "candela/rhi/Resources.h"

#include <filesystem>

namespace candela {

class Context;
class ShaderCache;

// Image-based lighting data, precomputed on the GPU at startup from an
// equirectangular HDRI:
//   environment  — 512² RGBA16F cube (background / debugging)
//   irradiance   — 32² cube, cosine-convolved diffuse
//   prefiltered  — 128² cube, 5 GGX-prefiltered roughness mips
//   brdfLut      — 512² RG16F split-sum BRDF integration
// Roadmap calls for disk caching; recompute-at-startup is fast enough on
// current hardware (<200 ms) that caching waits for the asset pipeline.
struct IBL {
    GpuImage environment;
    GpuImage irradiance;
    GpuImage prefiltered;
    GpuImage brdfLut;
    static constexpr uint32_t kPrefilteredMips = 5;

    void destroy(Context& context);
};

// `hdriPath` may not exist — a neutral gray sky is synthesized instead.
IBL precomputeIBL(Context& context, ShaderCache& shaders,
                  const std::filesystem::path& hdriPath);

// 1×1 black stand-in (valid bindless descriptors, zero contribution) so the
// real bake can be deferred until there is geometry to light.
IBL placeholderIBL(Context& context);

} // namespace candela

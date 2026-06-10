// Shared temporal accumulation: reproject the history buffer through the
// velocity target, clamp it to the 3x3 neighborhood of the current raw input
// (handles disocclusion without prev-depth plumbing), and blend. Used for RT
// AO, RT reflections, and TAA on the HDR scene.

#include "common.slang"

struct TemporalPush
{
    uint rawTexture;     // current frame's input (sampled)
    uint historyTexture; // previous accumulated result (sampled)
    uint outputImage;    // new accumulated result (storage)
    uint velocityTexture;
    uint2 resolution;
    float blendFactor;   // weight of the CURRENT frame (e.g. 0.1)
    uint historyValid;   // 0 on first frame / after resize → pass-through
};

[[vk::push_constant]] ConstantBuffer<TemporalPush> gPush;

[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gPush.resolution.x || id.y >= gPush.resolution.y)
        return;

    float2 invResolution = 1.0 / float2(gPush.resolution);
    float2 uv = (float2(id.xy) + 0.5) * invResolution;

    float4 raw = gTextures[gPush.rawTexture].SampleLevel(uv, 0.0);

    float2 velocity =
        gTextures[gPush.velocityTexture].SampleLevel(uv, 0.0).rg;
    float2 prevUV = uv - velocity;

    if (gPush.historyValid == 0 || any(prevUV < 0.0) || any(prevUV > 1.0))
    {
        gStorageImages[gPush.outputImage][id.xy] = raw;
        return;
    }

    // Neighborhood min/max of the current input bounds the history — ghosting
    // from disocclusion gets clamped away.
    float4 neighborhoodMin = raw;
    float4 neighborhoodMax = raw;
    [unroll]
    for (int dy = -1; dy <= 1; ++dy)
    {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx)
        {
            if (dx == 0 && dy == 0)
                continue;
            float4 neighbor = gTextures[gPush.rawTexture].SampleLevel(
                uv + float2(dx, dy) * invResolution, 0.0);
            neighborhoodMin = min(neighborhoodMin, neighbor);
            neighborhoodMax = max(neighborhoodMax, neighbor);
        }
    }

    float4 history = gTextures[gPush.historyTexture].SampleLevel(prevUV, 0.0);
    history = clamp(history, neighborhoodMin, neighborhoodMax);

    gStorageImages[gPush.outputImage][id.xy] =
        lerp(history, raw, gPush.blendFactor);
}

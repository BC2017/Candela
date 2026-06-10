// HDR resolve: exposure, bloom composite, ACES tonemap. The swapchain is an
// sRGB format, so output stays linear and the hardware encodes.

#include "common.slang"

struct TonemapPush
{
    uint hdrTexture;
    uint bloomTexture;
    float exposure;
    float bloomStrength;
    float2 invResolution;
};

[[vk::push_constant]] ConstantBuffer<TonemapPush> gPush;

[shader("vertex")]
FullscreenVSOut vsMain(uint vertexId : SV_VertexID)
{
    return fullscreenTriangle(vertexId);
}

// Narkowicz ACES fit.
float3 acesTonemap(float3 x)
{
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

[shader("fragment")]
float4 fsMain(FullscreenVSOut input) : SV_Target
{
    float2 uv = input.position.xy * gPush.invResolution;
    float3 hdr = gTextures[gPush.hdrTexture].Sample(uv).rgb;
    float3 bloom = gTextures[gPush.bloomTexture].Sample(uv).rgb;

    float3 color = lerp(hdr, bloom, gPush.bloomStrength);
    color *= gPush.exposure;
    return float4(acesTonemap(color), 1.0);
}

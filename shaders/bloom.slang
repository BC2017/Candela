// Dual-Kawase bloom. Downsample halves resolution each step; upsample adds
// back into the previous level (additive blend state, set by the pipeline).

#include "common.slang"

struct BloomPush
{
    uint sourceTexture;
    uint isFirstDownsample; // Karis average to tame fireflies
    float2 sourceInvResolution;
    float2 targetInvResolution;
};

[[vk::push_constant]] ConstantBuffer<BloomPush> gPush;

[shader("vertex")]
FullscreenVSOut vsMain(uint vertexId : SV_VertexID)
{
    return fullscreenTriangle(vertexId);
}

float karisWeight(float3 color)
{
    return 1.0 / (1.0 + max(color.r, max(color.g, color.b)));
}

[shader("fragment")]
float4 fsDownsample(FullscreenVSOut input) : SV_Target
{
    float2 uv = input.position.xy * gPush.targetInvResolution;
    float2 halfTexel = 0.5 * gPush.sourceInvResolution;
    Sampler2D source = gTextures[gPush.sourceTexture];

    float3 center = source.Sample(uv).rgb;
    float3 tl = source.Sample(uv + float2(-halfTexel.x, -halfTexel.y)).rgb;
    float3 tr = source.Sample(uv + float2(halfTexel.x, -halfTexel.y)).rgb;
    float3 bl = source.Sample(uv + float2(-halfTexel.x, halfTexel.y)).rgb;
    float3 br = source.Sample(uv + float2(halfTexel.x, halfTexel.y)).rgb;

    if (gPush.isFirstDownsample != 0)
    {
        // Karis average per sample group prevents single hot pixels from
        // flickering the whole chain.
        float wc = karisWeight(center);
        float wtl = karisWeight(tl), wtr = karisWeight(tr);
        float wbl = karisWeight(bl), wbr = karisWeight(br);
        float3 sum = center * wc + tl * wtl + tr * wtr + bl * wbl + br * wbr;
        return float4(sum / (wc + wtl + wtr + wbl + wbr), 1.0);
    }
    return float4(center * 0.5 + (tl + tr + bl + br) * 0.125, 1.0);
}

[shader("fragment")]
float4 fsUpsample(FullscreenVSOut input) : SV_Target
{
    float2 uv = input.position.xy * gPush.targetInvResolution;
    float2 texel = gPush.sourceInvResolution;
    Sampler2D source = gTextures[gPush.sourceTexture];

    // 9-tap tent filter.
    float3 sum = source.Sample(uv + float2(-texel.x, -texel.y)).rgb * 1.0;
    sum += source.Sample(uv + float2(0.0, -texel.y)).rgb * 2.0;
    sum += source.Sample(uv + float2(texel.x, -texel.y)).rgb * 1.0;
    sum += source.Sample(uv + float2(-texel.x, 0.0)).rgb * 2.0;
    sum += source.Sample(uv).rgb * 4.0;
    sum += source.Sample(uv + float2(texel.x, 0.0)).rgb * 2.0;
    sum += source.Sample(uv + float2(-texel.x, texel.y)).rgb * 1.0;
    sum += source.Sample(uv + float2(0.0, texel.y)).rgb * 2.0;
    sum += source.Sample(uv + float2(texel.x, texel.y)).rgb * 1.0;
    return float4(sum / 16.0, 1.0);
}

// Ray-traced ambient occlusion: 4 cosine-hemisphere rays per pixel per frame
// with frame-animated jitter. Raw output (R of an RGBA16F target) feeds the
// temporal accumulation pass, which converges it to dozens of effective
// samples. Replaces the 8-ray inline version that lived in lighting.slang.

#include "common.slang"

struct AOPush
{
    FrameConstants* frame;
    uint gbNormal;
    uint gbDepth;
    uint outputImage; // storage slot
    uint pad;         // uint2 below aligns to 8
    uint2 resolution;
};

[[vk::push_constant]] ConstantBuffer<AOPush> gPush;

[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gPush.resolution.x || id.y >= gPush.resolution.y)
        return;

    FrameConstants* frame = gPush.frame;
    float2 uv = (float2(id.xy) + 0.5) / float2(gPush.resolution);

    float depth = gTextures[gPush.gbDepth].SampleLevel(uv, 0.0).r;
    if (depth <= 0.0)
    {
        gStorageImages[gPush.outputImage][id.xy] = float4(1.0, 0.0, 0.0, 1.0);
        return;
    }

    float4 clip = float4(uvToNDC(uv), depth, 1.0);
    float4 worldW = mul(frame.invViewProjection, clip);
    float3 worldPos = worldW.xyz / worldW.w;
    float3 N =
        decodeOctahedral(gTextures[gPush.gbNormal].SampleLevel(uv, 0.0).rg);

    const uint kRays = 4;
    const float kRange = 0.8;
    float3x3 basis = orthonormalBasis(N);
    float rotation =
        interleavedGradientNoise(id.xy, frame.frameIndex) * 6.28318530;

    float visible = 0.0;
    [unroll]
    for (uint i = 0; i < kRays; ++i)
    {
        float u1 = (float(i) + 0.5) / float(kRays);
        float phi = rotation + 2.39996323 * float(i); // golden angle
        float cosTheta = sqrt(1.0 - u1);
        float sinTheta = sqrt(u1);
        float3 local =
            float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
        float3 dir = normalize(mul(local, basis));
        visible += traceVisibility(gTLAS[frame.tlasSlot], frame.instanceData,
                                   worldPos + N * 0.02, dir, kRange);
    }
    gStorageImages[gPush.outputImage][id.xy] =
        float4(visible / float(kRays), 0.0, 0.0, 1.0);
}

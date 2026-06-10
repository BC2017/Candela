// Ray-traced reflections via ray queries: one GGX importance-sampled ray per
// pixel for surfaces below the roughness cutoff. Hits are shaded manually
// from InstanceData (vertex fetch through buffer device addresses, bindless
// albedo) with sun + irradiance; misses sample the prefiltered environment.
// Output alpha is confidence — lighting falls back to IBL where it is 0.
// 1 spp with IGN jitter; temporal accumulation is the denoising step that
// completes this phase later.

#include "common.slang"

struct ReflectionsPush
{
    FrameConstants* frame;
    uint gbNormal;
    uint gbMaterial;
    uint gbDepth;
    uint outputImage;
    uint2 resolution;
    uint prefilteredCube;
    uint irradianceCube;
    float iblIntensity;
    uint prefilteredMips;
};

[[vk::push_constant]] ConstantBuffer<ReflectionsPush> gPush;

static const float PI = 3.14159265359;
static const float kRoughnessCutoff = 0.45;

float3 importanceSampleGGX(float2 xi, float3 n, float roughness)
{
    float a = roughness * roughness;
    float phi = 2.0 * PI * xi.x;
    float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
    float3 h = float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
    return normalize(mul(h, orthonormalBasis(n)));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gPush.resolution.x || id.y >= gPush.resolution.y)
        return;

    FrameConstants* frame = gPush.frame;
    float2 uv = (float2(id.xy) + 0.5) / float2(gPush.resolution);

    float depth = gTextures[gPush.gbDepth].SampleLevel(uv, 0.0).r;
    float4 material = gTextures[gPush.gbMaterial].SampleLevel(uv, 0.0);
    float roughness = max(material.g, 0.04);
    if (depth <= 0.0 || roughness > kRoughnessCutoff)
    {
        gStorageImages[gPush.outputImage][id.xy] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    float4 clip = float4(uvToNDC(uv), depth, 1.0);
    float4 worldW = mul(frame.invViewProjection, clip);
    float3 worldPos = worldW.xyz / worldW.w;
    float3 N =
        decodeOctahedral(gTextures[gPush.gbNormal].SampleLevel(uv, 0.0).rg);
    float3 V = normalize(frame.cameraPosition.xyz - worldPos);

    // GGX half-vector sample, jittered with IGN (mirror below 0.08).
    float3 R;
    if (roughness < 0.08)
    {
        R = reflect(-V, N);
    }
    else
    {
        float noise = interleavedGradientNoise(id.xy);
        float2 xi = float2(noise, frac(noise * 7.0 + 0.31));
        float3 H = importanceSampleGGX(xi, N, roughness);
        R = reflect(-V, H);
        if (dot(R, N) <= 0.0)
            R = reflect(-V, N);
    }

    RayDesc ray;
    ray.Origin = worldPos + N * 0.02;
    ray.Direction = R;
    ray.TMin = 0.02;
    ray.TMax = 200.0;
    RayQuery<RAY_FLAG_NONE> query;
    query.TraceRayInline(gTLAS[frame.tlasSlot], RAY_FLAG_NONE, 0xFF, ray);
    query.Proceed();

    float3 color;
    if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        InstanceData data =
            frame.instanceData[query.CommittedInstanceID() +
                               query.CommittedGeometryIndex()];
        uint primitive = query.CommittedPrimitiveIndex();
        float2 bary = query.CommittedTriangleBarycentrics();
        float3 weights =
            float3(1.0 - bary.x - bary.y, bary.x, bary.y);

        uint i0 = data.indices[primitive * 3 + 0];
        uint i1 = data.indices[primitive * 3 + 1];
        uint i2 = data.indices[primitive * 3 + 2];
        Vertex v0 = data.vertices[i0];
        Vertex v1 = data.vertices[i1];
        Vertex v2 = data.vertices[i2];

        float2 hitUV =
            v0.uv() * weights.x + v1.uv() * weights.y + v2.uv() * weights.z;
        float3 localNormal = normalize(v0.normal() * weights.x +
                                       v1.normal() * weights.y +
                                       v2.normal() * weights.z);
        // Uniform-scale assumption, matching the raster path.
        float3x4 objectToWorld = query.CommittedObjectToWorld3x4();
        float3 hitNormal = normalize(mul((float3x3)objectToWorld, localNormal));

        float3 albedo =
            gTextures[data.albedoTexture].SampleLevel(hitUV, 2.0).rgb *
            data.baseColorFactor.rgb;
        float3 hitPos = ray.Origin + R * query.CommittedRayT();

        // Sun with a secondary visibility ray, plus irradiance ambient.
        float3 L = normalize(frame.sunDirection.xyz);
        float ndotl = saturate(dot(hitNormal, L));
        float visibility = 0.0;
        if (ndotl > 0.0)
        {
            visibility = traceVisibility(gTLAS[frame.tlasSlot],
                                         hitPos + hitNormal * 0.02, L,
                                         10000.0);
        }
        float3 sunLight = frame.sunColor.rgb * frame.sunDirection.w * ndotl *
                          visibility / PI;
        float3 ambient = gCubes[gPush.irradianceCube]
                             .SampleLevel(hitNormal, 0.0)
                             .rgb *
                         gPush.iblIntensity;
        color = albedo * (sunLight + ambient);
    }
    else
    {
        float lod = roughness * float(gPush.prefilteredMips - 1);
        color = gCubes[gPush.prefilteredCube].SampleLevel(R, lod).rgb *
                gPush.iblIntensity;
    }

    // Firefly clamp keeps 1 spp stable until temporal accumulation lands.
    color = min(color, 8.0);
    gStorageImages[gPush.outputImage][id.xy] = float4(color, 1.0);
}

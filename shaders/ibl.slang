// IBL precompute kernels, dispatched once at startup:
//   equirectToCube     — equirectangular HDRI to cube faces
//   irradianceConvolve — cosine-weighted diffuse irradiance
//   prefilterGGX       — roughness-prefiltered specular mips
//   integrateBRDF      — split-sum environment BRDF LUT
//
// These use their own descriptor layout (set 0: input sampler, output
// storage), independent of the bindless tables.

[[vk::binding(0, 0)]] Sampler2D gInput2D;
[[vk::binding(0, 0)]] SamplerCube gInputCube;
[[vk::binding(1, 0)]] [format("rgba16f")] RWTexture2DArray<float4> gOutputCube;
[[vk::binding(1, 0)]] [format("rg16f")] RWTexture2D<float2> gOutputLut;

struct IBLPush
{
    float roughness;
    uint size;
    uint pad0;
    uint pad1;
};

[[vk::push_constant]] ConstantBuffer<IBLPush> gPush;

static const float PI = 3.14159265359;

// Face texel → world direction (standard Vulkan cube face basis).
float3 cubeDirection(uint2 texel, uint face, uint size)
{
    float2 uv = (float2(texel) + 0.5) / float(size) * 2.0 - 1.0;
    switch (face)
    {
    case 0: return normalize(float3(1.0, -uv.y, -uv.x));   // +X
    case 1: return normalize(float3(-1.0, -uv.y, uv.x));   // -X
    case 2: return normalize(float3(uv.x, 1.0, uv.y));     // +Y
    case 3: return normalize(float3(uv.x, -1.0, -uv.y));   // -Y
    case 4: return normalize(float3(uv.x, -uv.y, 1.0));    // +Z
    default: return normalize(float3(-uv.x, -uv.y, -1.0)); // -Z
    }
}

float2 directionToEquirect(float3 dir)
{
    float u = atan2(dir.z, dir.x) / (2.0 * PI) + 0.5;
    float v = acos(clamp(dir.y, -1.0, 1.0)) / PI;
    return float2(u, v);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void equirectToCube(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gPush.size || id.y >= gPush.size)
        return;
    float3 dir = cubeDirection(id.xy, id.z, gPush.size);
    float3 color = gInput2D.SampleLevel(directionToEquirect(dir), 0.0).rgb;
    // HDRIs can carry sun texels in the tens of thousands; clamp to keep the
    // prefilter integrals stable.
    color = min(color, 500.0);
    gOutputCube[id] = float4(color, 1.0);
}

// --- Sampling helpers ---
float radicalInverseVdC(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

float2 hammersley(uint i, uint count)
{
    return float2(float(i) / float(count), radicalInverseVdC(i));
}

float3x3 tangentBasis(float3 n)
{
    float3 up = abs(n.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    float3 tangent = normalize(cross(up, n));
    float3 bitangent = cross(n, tangent);
    return float3x3(tangent, bitangent, n); // rows
}

float3 importanceSampleGGX(float2 xi, float3 n, float roughness)
{
    float a = roughness * roughness;
    float phi = 2.0 * PI * xi.x;
    float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);

    float3 h = float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
    float3x3 basis = tangentBasis(n);
    return normalize(mul(h, basis)); // row-vector * rows = tangent → world
}

[shader("compute")]
[numthreads(8, 8, 1)]
void irradianceConvolve(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gPush.size || id.y >= gPush.size)
        return;
    float3 n = cubeDirection(id.xy, id.z, gPush.size);
    float3x3 basis = tangentBasis(n);

    float3 irradiance = float3(0.0, 0.0, 0.0);
    const uint sampleCount = 2048;
    for (uint i = 0; i < sampleCount; ++i)
    {
        float2 xi = hammersley(i, sampleCount);
        // Cosine-weighted hemisphere sample.
        float phi = 2.0 * PI * xi.x;
        float cosTheta = sqrt(1.0 - xi.y);
        float sinTheta = sqrt(xi.y);
        float3 local =
            float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
        float3 dir = normalize(mul(local, basis));
        irradiance += gInputCube.SampleLevel(dir, 2.0).rgb;
    }
    // Cosine-weighted sampling folds the n·l and PI terms into the estimator.
    gOutputCube[id] = float4(irradiance / float(sampleCount), 1.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void prefilterGGX(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gPush.size || id.y >= gPush.size)
        return;
    float3 n = cubeDirection(id.xy, id.z, gPush.size);
    float3 v = n; // isotropic assumption (standard split-sum)

    float3 color = float3(0.0, 0.0, 0.0);
    float weight = 0.0;
    const uint sampleCount = 1024;
    for (uint i = 0; i < sampleCount; ++i)
    {
        float2 xi = hammersley(i, sampleCount);
        float3 h = importanceSampleGGX(xi, n, gPush.roughness);
        float3 l = normalize(2.0 * dot(v, h) * h - v);
        float ndotl = dot(n, l);
        if (ndotl > 0.0)
        {
            // Mip selection from sample density tames fireflies.
            float lod = gPush.roughness == 0.0 ? 0.0 : 2.0;
            color += gInputCube.SampleLevel(l, lod).rgb * ndotl;
            weight += ndotl;
        }
    }
    gOutputCube[id] = float4(color / max(weight, 1e-4), 1.0);
}

float geometrySmithIBL(float ndotv, float ndotl, float roughness)
{
    float k = (roughness * roughness) / 2.0;
    float gv = ndotv / (ndotv * (1.0 - k) + k);
    float gl = ndotl / (ndotl * (1.0 - k) + k);
    return gv * gl;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void integrateBRDF(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gPush.size || id.y >= gPush.size)
        return;
    float ndotv = (float(id.x) + 0.5) / float(gPush.size);
    float roughness = (float(id.y) + 0.5) / float(gPush.size);
    ndotv = max(ndotv, 1e-3);

    float3 v = float3(sqrt(1.0 - ndotv * ndotv), 0.0, ndotv);
    float3 n = float3(0.0, 0.0, 1.0);

    float scale = 0.0;
    float bias = 0.0;
    const uint sampleCount = 1024;
    for (uint i = 0; i < sampleCount; ++i)
    {
        float2 xi = hammersley(i, sampleCount);
        float3 h = importanceSampleGGX(xi, n, roughness);
        float3 l = normalize(2.0 * dot(v, h) * h - v);
        float ndotl = l.z;
        if (ndotl > 0.0)
        {
            float ndoth = max(h.z, 0.0);
            float vdoth = max(dot(v, h), 0.0);
            float g = geometrySmithIBL(ndotv, ndotl, roughness);
            float gVis = (g * vdoth) / max(ndoth * ndotv, 1e-6);
            float fc = pow(1.0 - vdoth, 5.0);
            scale += (1.0 - fc) * gVis;
            bias += fc * gVis;
        }
    }
    gOutputLut[id.xy] =
        float2(scale / float(sampleCount), bias / float(sampleCount));
}

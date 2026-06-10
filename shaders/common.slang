// Shared declarations: bindless tables, vertex layout, frame constants,
// octahedral normal encoding, fullscreen triangle helper.

// --- Bindless set 0 ---
[[vk::binding(0, 0)]] Sampler2D gTextures[];
[[vk::binding(1, 0)]] SamplerCube gCubes[];
[[vk::binding(2, 0)]] Sampler2DArray gTexArrays[];
// One TLAS per frame in flight. Fixed size: slang emits invalid SPIR-V for
// unbounded acceleration-structure arrays (verified against spirv-val).
[[vk::binding(3, 0)]] RaytracingAccelerationStructure gTLAS[2];
[[vk::binding(4, 0)]] [format("rgba16f")] RWTexture2D<float4> gStorageImages[];

// Matches candela::Vertex (48 bytes, scalar-packed).
struct Vertex
{
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
    float tx, ty, tz, tw;

    float3 position() { return float3(px, py, pz); }
    float3 normal() { return float3(nx, ny, nz); }
    float2 uv() { return float2(u, v); }
    float4 tangent() { return float4(tx, ty, tz, tw); }
};

struct PointLight
{
    float4 positionRadius;  // xyz world position, w radius
    float4 colorIntensity;  // rgb color, w intensity (lm-ish, artistic)
};

// Per-TLAS-geometry shading data for ray-query hit evaluation.
// Index = CommittedInstanceID() + CommittedGeometryIndex().
// Matches candela InstanceDataGPU (48 bytes).
struct InstanceData
{
    Vertex* vertices;
    uint* indices;
    uint albedoTexture;
    uint flags;
    float metallicFactor;
    float roughnessFactor;
    float4 baseColorFactor;
};

// Matches candela::FrameConstants exactly (all members 16-byte friendly).
struct FrameConstants
{
    float4x4 viewProjection;
    float4x4 invViewProjection;
    float4x4 cascadeViewProjection[4];
    float4 cascadeSplits;     // view-space far depth of each cascade
    float4 cameraPosition;    // xyz
    float4 sunDirection;      // xyz = direction TO the sun, w = intensity
    float4 sunColor;          // rgb
    uint pointLightCount;
    uint pad0, pad1, pad2;
    PointLight pointLights[8];
    uint rtFlags;             // kRT* bits
    uint tlasSlot;
    uint pad3, pad4;
    InstanceData* instanceData;
    uint pad5, pad6; // uint64 padding would drag in the Int64 capability
    // Temporal: jitter-free matrices for velocity/reprojection.
    float4x4 viewProjectionNoJitter;
    float4x4 prevViewProjectionNoJitter;
    float2 jitter;
    uint frameIndex;
    uint taaEnabled;
};

static const uint kFlagAlphaMask = 1u << 0;
static const uint kFlagHasNormalMap = 1u << 1;

static const uint kRTShadows = 1u << 0;
static const uint kRTAmbientOcclusion = 1u << 1;
static const uint kRTReflections = 1u << 2;

// --- Ray helpers ---

// 1 = unoccluded. Visibility ray; alpha-masked geometry (non-opaque in the
// BLAS) is resolved by sampling the albedo alpha at the candidate hit, so
// foliage casts cutout-shaped shadows instead of full-quad ones.
float traceVisibility(RaytracingAccelerationStructure tlas,
                      InstanceData* instanceData, float3 origin,
                      float3 direction, float tMax)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.02;
    ray.TMax = tMax;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
    query.TraceRayInline(tlas, RAY_FLAG_NONE, 0xFF, ray);
    while (query.Proceed())
    {
        if (query.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE)
            continue;
        InstanceData data = instanceData[query.CandidateInstanceID() +
                                         query.CandidateGeometryIndex()];
        uint triangle = query.CandidatePrimitiveIndex();
        float2 bary = query.CandidateTriangleBarycentrics();
        float2 uv0 = data.vertices[data.indices[triangle * 3 + 0]].uv();
        float2 uv1 = data.vertices[data.indices[triangle * 3 + 1]].uv();
        float2 uv2 = data.vertices[data.indices[triangle * 3 + 2]].uv();
        float2 uv = uv0 * (1.0 - bary.x - bary.y) + uv1 * bary.x +
                    uv2 * bary.y;
        float alpha = gTextures[data.albedoTexture].SampleLevel(uv, 0.0).a *
                      data.baseColorFactor.a;
        if (alpha >= 0.5)
            query.CommitNonOpaqueTriangleHit();
    }
    return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0 : 1.0;
}

// Interleaved gradient noise. Pass animate=frameIndex to rotate the pattern
// per frame (temporal accumulation converges it); 0 keeps it static.
float interleavedGradientNoise(uint2 pixel, uint animate = 0)
{
    // Golden-ratio frame offset decorrelates consecutive frames.
    float2 p = float2(pixel) + float(animate % 64) * 5.588238;
    return frac(52.9829189 * frac(0.06711056 * p.x + 0.00583715 * p.y));
}

float3x3 orthonormalBasis(float3 n)
{
    float3 up = abs(n.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    float3 tangent = normalize(cross(up, n));
    float3 bitangent = cross(n, tangent);
    return float3x3(tangent, bitangent, n); // rows
}

// --- Octahedral normal encoding (RG16_UNORM) ---
float2 octWrap(float2 v)
{
    return (1.0 - abs(v.yx)) * select(v.xy >= 0.0, 1.0, -1.0);
}

float2 encodeOctahedral(float3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    float2 enc = n.z >= 0.0 ? n.xy : octWrap(n.xy);
    return enc * 0.5 + 0.5;
}

float3 decodeOctahedral(float2 enc)
{
    enc = enc * 2.0 - 1.0;
    float3 n = float3(enc.x, enc.y, 1.0 - abs(enc.x) - abs(enc.y));
    float t = saturate(-n.z);
    n.xy += select(n.xy >= 0.0, -t, t);
    return normalize(n);
}

// --- Fullscreen triangle ---
struct FullscreenVSOut
{
    float4 position : SV_Position;
};

FullscreenVSOut fullscreenTriangle(uint vertexId)
{
    float2 uv = float2(float((vertexId << 1) & 2), float(vertexId & 2));
    FullscreenVSOut output;
    output.position = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    return output;
}

// Maps clip-space NDC to texture UV under our negative-viewport convention.
float2 ndcToUV(float2 ndc)
{
    return float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}

float2 uvToNDC(float2 uv)
{
    return float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
}

// Shared declarations: bindless tables, vertex layout, frame constants,
// octahedral normal encoding, fullscreen triangle helper.

// --- Bindless set 0 ---
[[vk::binding(0, 0)]] Sampler2D gTextures[];
[[vk::binding(1, 0)]] SamplerCube gCubes[];
[[vk::binding(2, 0)]] Sampler2DArray gTexArrays[];

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
};

static const uint kFlagAlphaMask = 1u << 0;
static const uint kFlagHasNormalMap = 1u << 1;

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

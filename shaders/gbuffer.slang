// Deferred G-buffer fill:
//   RT0 (RGBA8_SRGB)  albedo.rgb, occlusion strength in a
//   RT1 (RG16_UNORM)  octahedral world-space normal
//   RT2 (RGBA8_UNORM) metallic, roughness, AO, unused
//   RT3 (R32_UINT)    entity id (picking)
//   RT4 (RG16F)       UV-space velocity (jitter-free) for reprojection
//   RT5 (RGBA8_SRGB)  emission (texture * factor)

#include "common.slang"

// 128-byte budget is full: texture slots ride in 16-bit pairs, factors in
// packed unorms. Must match the C++ GBufferPush exactly.
struct GBufferPush
{
    float4x4 model;          // 64
    Vertex* vertices;        // 72
    FrameConstants* frame;   // 80
    uint albedoNormalTex;    // 84  albedo | normal << 16
    uint mrOcclusionTex;     // 88  metallicRoughness | occlusion << 16
    uint emissiveTexFlags;   // 92  emissive | flags << 16
    uint metallicRoughness;  // 96  16-bit unorm metallic | roughness << 16
    uint emissiveFactor;     // 100 RGB 8-bit unorm
    uint entityId;           // 104 (entt id + 1; 0 = none, for picking)
    uint2 pad;               // 112
    float4 baseColorFactor;  // 128
};

[[vk::push_constant]] ConstantBuffer<GBufferPush> gPush;

struct VSOut
{
    float4 position : SV_Position;
    float3 worldNormal : NORMAL0;
    float4 worldTangent : TANGENT0;
    float2 uv : TEXCOORD0;
    // Jitter-free clip positions for velocity (entities are static; the
    // model matrix doubles as last frame's until per-entity motion exists).
    float4 clipNoJitter : TEXCOORD1;
    float4 prevClipNoJitter : TEXCOORD2;
};

[shader("vertex")]
VSOut vsMain(uint vertexId : SV_VertexID)
{
    Vertex v = gPush.vertices[vertexId];
    float4 worldPos = mul(gPush.model, float4(v.position(), 1.0));

    VSOut output;
    output.position = mul(gPush.frame.viewProjection, worldPos);
    output.clipNoJitter = mul(gPush.frame.viewProjectionNoJitter, worldPos);
    output.prevClipNoJitter =
        mul(gPush.frame.prevViewProjectionNoJitter, worldPos);
    // Uniform-scale assumption; a proper normal matrix arrives with the
    // editor's non-uniform-scale support.
    output.worldNormal = normalize(mul((float3x3)gPush.model, v.normal()));
    output.worldTangent =
        float4(normalize(mul((float3x3)gPush.model, v.tangent().xyz)),
               v.tangent().w);
    output.uv = v.uv();
    return output;
}

struct GBufferOut
{
    float4 albedo : SV_Target0;
    float2 normal : SV_Target1;
    float4 material : SV_Target2;
    uint entityId : SV_Target3;
    float2 velocity : SV_Target4;
    float4 emissive : SV_Target5;
};

[shader("fragment")]
GBufferOut fsMain(VSOut input)
{
    const uint albedoTexture = gPush.albedoNormalTex & 0xffff;
    const uint normalTexture = gPush.albedoNormalTex >> 16;
    const uint mrTexture = gPush.mrOcclusionTex & 0xffff;
    const uint occlusionTexture = gPush.mrOcclusionTex >> 16;
    const uint emissiveTexture = gPush.emissiveTexFlags & 0xffff;
    const uint flags = gPush.emissiveTexFlags >> 16;
    const float metallicFactor = float(gPush.metallicRoughness & 0xffff) / 65535.0;
    const float roughnessFactor = float(gPush.metallicRoughness >> 16) / 65535.0;
    const float3 emissiveFactor =
        float3(float(gPush.emissiveFactor & 0xff),
               float((gPush.emissiveFactor >> 8) & 0xff),
               float((gPush.emissiveFactor >> 16) & 0xff)) / 255.0;

    float4 baseColor = gTextures[albedoTexture].Sample(input.uv)
                       * gPush.baseColorFactor;
    if ((flags & kFlagAlphaMask) != 0 && baseColor.a < 0.5)
        discard;

    float3 normal = normalize(input.worldNormal);
    if ((flags & kFlagHasNormalMap) != 0)
    {
        float3 tangent = normalize(input.worldTangent.xyz);
        // Re-orthogonalize after interpolation.
        tangent = normalize(tangent - normal * dot(normal, tangent));
        float3 bitangent = cross(normal, tangent) * input.worldTangent.w;
        float3 sampled =
            gTextures[normalTexture].Sample(input.uv).xyz * 2.0 - 1.0;
        normal = normalize(sampled.x * tangent + sampled.y * bitangent +
                           sampled.z * normal);
    }

    // glTF: metallic in B, roughness in G; occlusion in R (often the same
    // texture).
    float4 mr = gTextures[mrTexture].Sample(input.uv);
    float metallic = saturate(mr.b * metallicFactor);
    float roughness = saturate(mr.g * roughnessFactor);
    float occlusion = gTextures[occlusionTexture].Sample(input.uv).r;

    GBufferOut output;
    output.albedo = float4(baseColor.rgb, 1.0);
    output.normal = encodeOctahedral(normal);
    output.material = float4(metallic, roughness, occlusion, 0.0);
    output.entityId = gPush.entityId;
    output.emissive =
        float4(gTextures[emissiveTexture].Sample(input.uv).rgb * emissiveFactor,
               1.0);
    // UV-space motion: prevUV = uv - velocity.
    float2 currUV = ndcToUV(input.clipNoJitter.xy / input.clipNoJitter.w);
    float2 prevUV =
        ndcToUV(input.prevClipNoJitter.xy / input.prevClipNoJitter.w);
    output.velocity = currUV - prevUV;
    return output;
}

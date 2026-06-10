// Deferred G-buffer fill:
//   RT0 (RGBA8_SRGB)  albedo.rgb, occlusion strength in a
//   RT1 (RG16_UNORM)  octahedral world-space normal
//   RT2 (RGBA8_UNORM) metallic, roughness, AO, unused
//   RT3 (R32_UINT)    entity id (picking)
//   RT4 (RG16F)       UV-space velocity (jitter-free) for reprojection

#include "common.slang"

struct GBufferPush
{
    float4x4 model;          // 64
    Vertex* vertices;        // 72
    FrameConstants* frame;   // 80
    uint albedoTexture;      // 84
    uint normalTexture;      // 88
    uint metallicRoughnessTexture; // 92
    uint occlusionTexture;   // 96
    uint flags;              // 100
    float metallicFactor;    // 104
    float roughnessFactor;   // 108
    uint entityId;           // 112 (entt id + 1; 0 = none, for picking)
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
};

[shader("fragment")]
GBufferOut fsMain(VSOut input)
{
    float4 baseColor = gTextures[gPush.albedoTexture].Sample(input.uv)
                       * gPush.baseColorFactor;
    if ((gPush.flags & kFlagAlphaMask) != 0 && baseColor.a < 0.5)
        discard;

    float3 normal = normalize(input.worldNormal);
    if ((gPush.flags & kFlagHasNormalMap) != 0)
    {
        float3 tangent = normalize(input.worldTangent.xyz);
        // Re-orthogonalize after interpolation.
        tangent = normalize(tangent - normal * dot(normal, tangent));
        float3 bitangent = cross(normal, tangent) * input.worldTangent.w;
        float3 sampled =
            gTextures[gPush.normalTexture].Sample(input.uv).xyz * 2.0 - 1.0;
        normal = normalize(sampled.x * tangent + sampled.y * bitangent +
                           sampled.z * normal);
    }

    // glTF: metallic in B, roughness in G; occlusion in R (often the same
    // texture).
    float4 mr = gTextures[gPush.metallicRoughnessTexture].Sample(input.uv);
    float metallic = saturate(mr.b * gPush.metallicFactor);
    float roughness = saturate(mr.g * gPush.roughnessFactor);
    float occlusion = gTextures[gPush.occlusionTexture].Sample(input.uv).r;

    GBufferOut output;
    output.albedo = float4(baseColor.rgb, 1.0);
    output.normal = encodeOctahedral(normal);
    output.material = float4(metallic, roughness, occlusion, 0.0);
    output.entityId = gPush.entityId;
    // UV-space motion: prevUV = uv - velocity.
    float2 currUV = ndcToUV(input.clipNoJitter.xy / input.clipNoJitter.w);
    float2 prevUV =
        ndcToUV(input.prevClipNoJitter.xy / input.prevClipNoJitter.w);
    output.velocity = currUV - prevUV;
    return output;
}

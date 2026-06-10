// Deferred lighting: Cook-Torrance GGX direct (sun with cascaded PCF shadows
// + point lights) and split-sum IBL ambient, into an HDR RGBA16F target.

#include "common.slang"

struct LightingPush
{
    FrameConstants* frame;
    uint gbAlbedo;
    uint gbNormal;
    uint gbMaterial;
    uint gbDepth;
    uint shadowCascades;  // Array2D binding
    uint irradianceCube;  // Cube binding
    uint prefilteredCube; // Cube binding
    uint brdfLut;
    float2 invResolution;
    float iblIntensity;
    uint prefilteredMips;
    uint debugView; // 0 final, 1 albedo, 2 normals, 3 metal/rough, 4 AO, 5 cascades, 6 reflections
    uint reflectionTexture; // accumulated RT reflections (kRTReflections)
    uint aoTexture;         // accumulated RT AO (kRTAmbientOcclusion)
    uint environmentCube;   // Cube binding — sky background
    uint gbEmissive;
};

[[vk::push_constant]] ConstantBuffer<LightingPush> gPush;

static const float PI = 3.14159265359;

// --- Cook-Torrance pieces ---
float distributionGGX(float NdotH, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-6);
}

float geometrySmith(float NdotV, float NdotL, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    float gv = NdotV / (NdotV * (1.0 - k) + k);
    float gl = NdotL / (NdotL * (1.0 - k) + k);
    return gv * gl;
}

float3 fresnelSchlick(float cosTheta, float3 f0)
{
    return f0 + (1.0 - f0) * pow(saturate(1.0 - cosTheta), 5.0);
}

float3 brdf(float3 N, float3 V, float3 L, float3 albedo, float metallic,
            float roughness)
{
    float3 H = normalize(V + L);
    float NdotV = max(dot(N, V), 1e-4);
    float NdotL = max(dot(N, L), 0.0);
    float NdotH = max(dot(N, H), 0.0);

    float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    float D = distributionGGX(NdotH, roughness);
    float G = geometrySmith(NdotV, NdotL, roughness);
    float3 F = fresnelSchlick(max(dot(H, V), 0.0), f0);

    float3 specular = (D * G * F) / max(4.0 * NdotV * NdotL, 1e-6);
    float3 kd = (1.0 - F) * (1.0 - metallic);
    return (kd * albedo / PI + specular) * NdotL;
}

// --- Cascaded shadow lookup with 3x3 PCF ---
float sampleShadow(FrameConstants* frame, float3 worldPos, float viewDepth)
{
    // Pick the first cascade whose far split contains this depth.
    int cascade = 3;
    [unroll]
    for (int i = 3; i >= 0; --i)
    {
        if (viewDepth <= frame.cascadeSplits[i])
            cascade = i;
    }

    float4 lightClip =
        mul(frame.cascadeViewProjection[cascade], float4(worldPos, 1.0));
    float3 lightNDC = lightClip.xyz / lightClip.w;
    float2 shadowUV = ndcToUV(lightNDC.xy);
    if (any(shadowUV < 0.0) || any(shadowUV > 1.0))
        return 1.0;

    // Standard-Z ortho: nearer casters have SMALLER stored depth.
    float bias = 0.0015 + 0.001 * float(cascade);
    float fragmentDepth = lightNDC.z - bias;

    float texel = 1.0 / 2048.0;
    float lit = 0.0;
    [unroll]
    for (int dy = -1; dy <= 1; ++dy)
    {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx)
        {
            float stored = gTexArrays[gPush.shadowCascades]
                               .Sample(float3(shadowUV + float2(dx, dy) * texel,
                                              float(cascade)))
                               .r;
            lit += stored <= fragmentDepth ? 0.0 : 1.0;
        }
    }
    return lit / 9.0;
}

[shader("vertex")]
FullscreenVSOut vsMain(uint vertexId : SV_VertexID)
{
    return fullscreenTriangle(vertexId);
}

[shader("fragment")]
float4 fsMain(FullscreenVSOut input) : SV_Target
{
    FrameConstants* frame = gPush.frame;
    float2 uv = input.position.xy * gPush.invResolution;

    float depth = gTextures[gPush.gbDepth].Sample(uv).r;
    if (depth <= 0.0) // reverse-Z far plane — sky
    {
        // View ray through this pixel (any depth works for a direction —
        // subtract the camera position after unprojecting).
        float4 farH = mul(frame->invViewProjection,
                          float4(uvToNDC(uv), 0.5, 1.0));
        float3 viewDir =
            normalize(farH.xyz / farH.w - frame->cameraPosition.xyz);
        float3 sky = gCubes[gPush.environmentCube]
                         .SampleLevel(viewDir, 0.0)
                         .rgb;
        return float4(sky * gPush.iblIntensity, 1.0);
    }

    // Reconstruct world position.
    float4 clip = float4(uvToNDC(uv), depth, 1.0);
    float4 worldW = mul(frame.invViewProjection, clip);
    float3 worldPos = worldW.xyz / worldW.w;

    float3 albedo = gTextures[gPush.gbAlbedo].Sample(uv).rgb;
    float3 N = decodeOctahedral(gTextures[gPush.gbNormal].Sample(uv).rg);
    float4 material = gTextures[gPush.gbMaterial].Sample(uv);
    float metallic = material.r;
    float roughness = max(material.g, 0.04);
    float occlusion = material.b;

    float3 V = normalize(frame.cameraPosition.xyz - worldPos);
    float viewDepth = length(frame.cameraPosition.xyz - worldPos);

    // Ray-traced AO: traced + temporally accumulated by their own passes;
    // the lighting pass just samples the converged result.
    if ((frame.rtFlags & kRTAmbientOcclusion) != 0)
    {
        occlusion = min(occlusion, gTextures[gPush.aoTexture].Sample(uv).r);
    }

    // Debug views short-circuit shading.
    if (gPush.debugView != 0)
    {
        switch (gPush.debugView)
        {
        case 1: return float4(albedo, 1.0);
        case 2: return float4(N * 0.5 + 0.5, 1.0);
        case 3: return float4(metallic, roughness, 0.0, 1.0);
        case 4: return float4(occlusion, occlusion, occlusion, 1.0);
        case 5:
        {
            int cascade = 3;
            [unroll]
            for (int i = 3; i >= 0; --i)
            {
                if (viewDepth <= frame.cascadeSplits[i])
                    cascade = i;
            }
            static const float3 cascadeColors[4] = {
                float3(0.2, 1.0, 0.2), float3(1.0, 1.0, 0.2),
                float3(1.0, 0.5, 0.2), float3(1.0, 0.2, 0.2)
            };
            return float4(cascadeColors[cascade] * (0.3 + 0.7 * albedo), 1.0);
        }
        case 6:
            if ((frame.rtFlags & kRTReflections) != 0)
                return float4(
                    gTextures[gPush.reflectionTexture].Sample(uv).rgb, 1.0);
            return float4(0.0, 0.0, 0.0, 1.0);
        default: break;
        }
    }

    // Sun: ray-traced visibility when enabled, cascaded shadow maps as the
    // fallback.
    float3 L = normalize(frame.sunDirection.xyz);
    float shadow;
    if ((frame.rtFlags & kRTShadows) != 0)
    {
        shadow = traceVisibility(gTLAS[frame.tlasSlot], frame.instanceData,
                                 worldPos + N * 0.02, L, 10000.0);
    }
    else
    {
        shadow = sampleShadow(frame, worldPos, viewDepth);
    }
    float3 direct = brdf(N, V, L, albedo, metallic, roughness) *
                    frame.sunColor.rgb * frame.sunDirection.w * shadow;

    // Point lights (inverse-square falloff, windowed at the radius).
    // RT shadows give point lights occlusion the raster path never had.
    for (uint i = 0; i < frame.pointLightCount; ++i)
    {
        PointLight light = frame.pointLights[i];
        float3 toLight = light.positionRadius.xyz - worldPos;
        float distance2 = dot(toLight, toLight);
        float radius = light.positionRadius.w;
        if (distance2 > radius * radius)
            continue;
        float distance = sqrt(distance2);
        float lightShadow = 1.0;
        if ((frame.rtFlags & kRTShadows) != 0)
        {
            lightShadow = traceVisibility(
                gTLAS[frame.tlasSlot], frame.instanceData, worldPos + N * 0.02,
                toLight / distance, distance - 0.05);
        }
        float attenuation = 1.0 / max(distance2, 0.01);
        float window = pow(saturate(1.0 - pow(distance / radius, 4.0)), 2.0);
        direct += brdf(N, V, normalize(toLight), albedo, metallic, roughness) *
                  light.colorIntensity.rgb * light.colorIntensity.w *
                  attenuation * window * lightShadow;
    }

    // IBL ambient (split-sum).
    float NdotV = max(dot(N, V), 1e-4);
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    float3 F = f0 + (max(float3(1.0 - roughness, 1.0 - roughness,
                                1.0 - roughness), f0) - f0) *
                        pow(saturate(1.0 - NdotV), 5.0);
    float3 kd = (1.0 - F) * (1.0 - metallic);

    float3 irradiance = gCubes[gPush.irradianceCube].Sample(N).rgb;
    float3 diffuseIBL = kd * irradiance * albedo;

    float3 R = reflect(-V, N);
    float lod = roughness * float(gPush.prefilteredMips - 1);
    float3 prefiltered =
        gCubes[gPush.prefilteredCube].SampleLevel(R, lod).rgb;
    // Ray-traced reflections replace the prefiltered env where traced
    // (alpha carries confidence; rough surfaces stay on IBL).
    if ((frame.rtFlags & kRTReflections) != 0)
    {
        float4 traced = gTextures[gPush.reflectionTexture].Sample(uv);
        prefiltered = lerp(prefiltered, traced.rgb, traced.a);
    }
    float2 envBRDF =
        gTextures[gPush.brdfLut].Sample(float2(NdotV, roughness)).rg;
    float3 specularIBL = prefiltered * (F * envBRDF.x + envBRDF.y);

    float3 ambient = (diffuseIBL + specularIBL) * occlusion * gPush.iblIntensity;

    float3 emissive = gTextures[gPush.gbEmissive].Sample(uv).rgb;

    return float4(direct + ambient + emissive, 1.0);
}

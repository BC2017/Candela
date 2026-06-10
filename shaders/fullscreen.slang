// Fullscreen triangle with an animated gradient — Phase 0 "the toolchain works" shader.
// Edit this file while the sandbox runs to see hot reload.

struct VSOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
VSOut vsMain(uint vertexId : SV_VertexID)
{
    // One triangle covering the screen: (0,0), (2,0), (0,2) in UV space.
    float2 uv = float2(float((vertexId << 1) & 2), float(vertexId & 2));
    VSOut output;
    output.position = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    output.uv = uv;
    return output;
}

struct PushConstants
{
    float time;
};

[[vk::push_constant]]
ConstantBuffer<PushConstants> gPush;

[shader("fragment")]
float4 fsMain(VSOut input) : SV_Target
{
    float3 deepBlue = float3(0.05, 0.09, 0.22);
    float3 ember = float3(0.95, 0.42, 0.18);
    float wave = 0.5 + 0.5 * sin(gPush.time + input.uv.x * 6.28318);
    float3 color = lerp(deepBlue, ember, wave * input.uv.y);
    return float4(color, 1.0);
}

// Textured mesh rendering: vertex pulling via buffer device address,
// base-color textures through the global bindless table (set 0, binding 0).

// Scalar-packed to match candela::Vertex (32 bytes) exactly.
struct Vertex
{
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
};

struct DrawPush
{
    float4x4 mvp;            // 64
    Vertex* vertices;        // 72
    uint textureIndex;       // 76
    uint flags;              // 80  (bit 0: alpha-masked material)
    float4 baseColorFactor;  // 96
};

[[vk::push_constant]]
ConstantBuffer<DrawPush> gPush;

[[vk::binding(0, 0)]]
Sampler2D gTextures[];

static const uint kFlagAlphaMask = 1u << 0;

struct VSOut
{
    float4 position : SV_Position;
    float3 normal : NORMAL0;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
VSOut vsMain(uint vertexId : SV_VertexID)
{
    Vertex v = gPush.vertices[vertexId];
    VSOut output;
    output.position = mul(gPush.mvp, float4(v.px, v.py, v.pz, 1.0));
    output.normal = float3(v.nx, v.ny, v.nz);
    output.uv = float2(v.u, v.v);
    return output;
}

[shader("fragment")]
float4 fsMain(VSOut input) : SV_Target
{
    float4 base = gTextures[gPush.textureIndex].Sample(input.uv)
                  * gPush.baseColorFactor;

    if ((gPush.flags & kFlagAlphaMask) != 0 && base.a < 0.5)
        discard;

    // Placeholder shading so geometry reads before Phase 2's PBR: a fixed sun
    // plus ambient, applied to the base color.
    float3 normal = normalize(input.normal);
    float3 sunDirection = normalize(float3(0.35, 1.0, 0.25));
    float light = 0.35 + 0.65 * saturate(dot(normal, sunDirection));
    return float4(base.rgb * light, 1.0);
}

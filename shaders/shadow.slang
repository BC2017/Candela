// Depth-only cascade rendering. Alpha-tested shadows are deliberately not
// supported yet — foliage casts solid shadows until the material system
// settles.

#include "common.slang"

struct ShadowPush
{
    float4x4 lightViewProjModel; // cascadeViewProj * model, premultiplied CPU-side
    Vertex* vertices;
};

[[vk::push_constant]] ConstantBuffer<ShadowPush> gPush;

struct VSOut
{
    float4 position : SV_Position;
};

[shader("vertex")]
VSOut vsMain(uint vertexId : SV_VertexID)
{
    Vertex v = gPush.vertices[vertexId];
    VSOut output;
    output.position = mul(gPush.lightViewProjModel, float4(v.position(), 1.0));
    return output;
}

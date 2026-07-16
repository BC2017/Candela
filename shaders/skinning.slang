// Compute pre-skinning: reads bind-pose Vertex[] + per-vertex joint/weight data
// + a joint-matrix palette and writes fully-skinned vertices (identical 48-byte
// Vertex layout) into a per-instance destination buffer. The G-buffer VS,
// shadow VS, and RT hit shading are then just pointed at the destination — no
// vertex-shader skinning, so the raster geometry never desyncs from the BLAS
// (which is refit from this same buffer). Fresh 128-byte push, BDA pointers.

#include "common.slang"

// Matches candela::SkinVertex (24 bytes). Joint indices are packed two per uint
// to avoid slang 8/16-bit BDA storage requirements.
struct SkinVertex
{
    uint joints01; // joint[0] | joint[1] << 16
    uint joints23; // joint[2] | joint[3] << 16
    float w0, w1, w2, w3;
};

struct SkinningPush
{
    Vertex* bindPose;       // source bind-pose vertices
    SkinVertex* skin;       // per-vertex joints/weights
    float4x4* palette;      // joint matrices (already mesh-local)
    Vertex* dst;            // skinned destination sub-range
    uint vertexCount;
    uint pad0, pad1, pad2;
};

[[vk::push_constant]] ConstantBuffer<SkinningPush> gPush;

[shader("compute")]
[numthreads(64, 1, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    uint i = id.x;
    if (i >= gPush.vertexCount)
        return;

    Vertex v = gPush.bindPose[i];
    SkinVertex s = gPush.skin[i];

    uint j0 = s.joints01 & 0xffff;
    uint j1 = s.joints01 >> 16;
    uint j2 = s.joints23 & 0xffff;
    uint j3 = s.joints23 >> 16;

    // Palette folds inverse(meshWorld) in already, so the output is in
    // mesh-local space; the G-buffer VS applies meshWorld exactly as for a
    // static mesh (and the RT instance transform matches). No double transform.
    float4x4 m = s.w0 * gPush.palette[j0] + s.w1 * gPush.palette[j1] +
                 s.w2 * gPush.palette[j2] + s.w3 * gPush.palette[j3];

    float3 p = mul(m, float4(v.position(), 1.0)).xyz;
    float3x3 rot = (float3x3)m;
    float3 n = normalize(mul(rot, v.normal()));
    float3 tg = normalize(mul(rot, v.tangent().xyz));

    Vertex o = v;
    o.px = p.x; o.py = p.y; o.pz = p.z;
    o.nx = n.x; o.ny = n.y; o.nz = n.z;
    o.tx = tg.x; o.ty = tg.y; o.tz = tg.z; // tw (handedness) preserved from v
    gPush.dst[i] = o;
}

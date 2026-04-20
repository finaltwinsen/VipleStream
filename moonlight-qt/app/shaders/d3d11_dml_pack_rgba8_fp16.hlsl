// VipleStream: D3D11 compute shader that packs an RGBA8 render
// texture into a planar FP16 NCHW buffer shared with D3D12 / DML.
//
// Replaces the CPU-side SIMD roundtrip in DirectMLFRUC — the D3D11
// context can write directly into the same VRAM-resident buffer
// that DML will consume, so we drop both the D3D11 staging
// textures and the UPLOAD heap from the path entirely.
//
// Tensor layout: [1, 4, H, W] FP16. Plane 0 = R, 1 = G, 2 = B,
// 3 = A. Each plane is a contiguous W*H FP16 block; the total
// buffer has W*H*4 elements. Output type is R16_FLOAT so a typed
// UAV store is one texel per write.
//
// We fold the 0.5 scale in here (same as the CPU path did) so the
// DML_ELEMENT_WISE_ADD1 op yields 0.5*prev + 0.5*curr directly.

Texture2D<float4>       input  : register(t0);
RWBuffer<float>         output : register(u0);

cbuffer PackConsts : register(b0)
{
    uint  width;
    uint  height;
    uint  _pad0;
    uint  _pad1;
};

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height) return;

    // Texture2D<float4> already dequantizes R8G8B8A8_UNORM to
    // [0,1]; multiply by 0.5 so the downstream ADD1 is a mean.
    float4 px = input.Load(int3(id.xy, 0)) * 0.5;

    uint plane = width * height;
    uint idx   = id.y * width + id.x;

    output[0 * plane + idx] = px.r;
    output[1 * plane + idx] = px.g;
    output[2 * plane + idx] = px.b;
    output[3 * plane + idx] = px.a;
}

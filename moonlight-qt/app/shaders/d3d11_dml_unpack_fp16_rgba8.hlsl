// VipleStream: D3D11 compute shader that unpacks a planar FP16
// NCHW buffer written by DML back into an RGBA8 texture ready for
// the blit path.
//
// Inverse of d3d11_dml_pack_rgba8_fp16.hlsl. Input is the same
// shared D3D12/D3D11 buffer DML writes its output tensor into.
// Output is an R8G8B8A8_UNORM texture accessed via a typed UAV;
// D3D11 writes to UAVs using saturation on out-of-range values
// when the UAV format is _UNORM, so we don't need an explicit
// clamp.
//
// Note the DML graph output is already in [0, 1] because the pack
// shader pre-scaled by 0.5 and ADD1 yields the mean — no extra
// normalisation required here.

Buffer<float>             input  : register(t0);
RWTexture2D<unorm float4> output : register(u0);

cbuffer UnpackConsts : register(b0)
{
    uint  width;
    uint  height;
    uint  useAlpha; // 0 for 3-channel model outputs; unpack writes alpha = 1.
    uint  _pad1;
};

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height) return;

    uint plane = width * height;
    uint idx   = id.y * width + id.x;

    float4 px;
    px.r = input[0 * plane + idx];
    px.g = input[1 * plane + idx];
    px.b = input[2 * plane + idx];
    // 3-channel models (RIFE / IFRNet / most public FRUC exports)
    // never write plane 3, so reading it would hand us stale data.
    // Video streams don't carry a meaningful alpha anyway — force 1.
    px.a = (useAlpha != 0) ? input[3 * plane + idx] : 1.0f;

    output[id.xy] = saturate(px);
}

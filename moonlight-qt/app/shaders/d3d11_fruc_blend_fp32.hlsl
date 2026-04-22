// VipleStream: D3D12 compute shader — direct temporal blend of two
// FP32 NCHW frame tensors into an FP32 output tensor.
//
// This is the **Tier 3 fallback** path used when DirectML's graph
// compiler/runtime refuses to initialise on the user's hardware
// (observed on some driver revisions: BindingTable->Reset(exec)
// returns 0x887A0005 even though the device is not actually
// removed — a DML-internal validation failure). When that happens
// we still want frame interpolation to work, just without any
// model/graph machinery.
//
// What this replicates: the inline DML graph's
//   0.5·prev + 0.5·curr → CLIP[0,1]
// on the same FrameTensor[0..1] → OutputTensor buffers, bit-for-bit
// equivalent semantics for the intended RGBA8 pipeline.
//
// Tensor layout matches the pack/unpack shaders: NCHW FP32 with
// plane stride = W*H and 4 planes (R, G, B, A). We blend all 4
// planes per thread; unpack will later decide whether to honour
// the alpha plane or force alpha=1 based on the useAlpha CB flag.
//
// Compiled as cs_5_0 and consumed by both the D3D11 pipeline (for
// consistency with the rest of d3d11_*.fxc naming) and the D3D12
// DirectMLFRUC backend which reuses the DXBC bytecode on a D3D12
// PSO. 2D dispatch over (W, H), one thread per pixel, 4 channels
// per thread.

Buffer<float>     inputA : register(t0);
Buffer<float>     inputB : register(t1);
RWBuffer<float>   output : register(u0);

cbuffer BlendConsts : register(b0)
{
    uint  width;
    uint  height;
    uint  _pad0;  // matches PackConsts / UnpackConsts layout
    uint  _pad1;
};

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height) return;

    const uint plane = width * height;
    const uint idx   = id.y * width + id.x;

    // Unrolled 4-channel blend. Each plane of inputA and inputB
    // is in [0,1] (written by the pack CS which does no prescale)
    // so the average is guaranteed to stay in [0,1] — saturate()
    // is defensive only, matching the CLIP node the DML graph
    // would have emitted.
    [unroll]
    for (uint c = 0; c < 4; ++c) {
        const uint off = c * plane + idx;
        float a = inputA[off];
        float b = inputB[off];
        output[off] = saturate(0.5f * (a + b));
    }
}

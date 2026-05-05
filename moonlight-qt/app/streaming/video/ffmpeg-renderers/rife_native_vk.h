// VipleStream §J.3.e.X — hand-rolled RIFE-v4.25-lite Vulkan inference.
//
// Goal: replace the ncnn-Vulkan inference dependency (~10 MB DLL on
// Windows; blocks Linux build pipeline since ncnn isn't packaged in
// distro repos with vulkan support, requires from-source build).
//
// This first scaffold only PARSES the .param topology so we can drive
// later commits incrementally:
//   • Phase 1 (this scaffold): .param text parser + operator enum +
//     layer summary.  No GPU work yet.  Smoke gated behind env var.
//   • Phase 2: .bin weight loader → VkBuffer + first Conv2D 3x3
//     compute shader + correctness gate vs ncnn.
//   • Phase 3+: remaining operators (Deconvolution, BinaryOp, ReLU,
//     Crop, Concat, Interp, PixelShuffle, Sigmoid, rife.Warp) plus
//     graph executor.
//   • Final: PipelineCache + drop ncnn DLL + verify Linux build path.
//
// .param spec (ncnn text format):
//   line 1: magic = 7767517
//   line 2: <layer_count> <blob_count>
//   line 3+: <op_type> <name> <input_count> <output_count>
//            <input_tensor>...  <output_tensor>...  [<param_id>=<value>]...
//
// param_id syntax:
//   k=v        — scalar (int / float)
//   -23309=...  — array (counted, comma-separated values follow)
//
// rife-v4.25-lite stats (Apr 2026):
//   389 layers, 485 blobs, 11.3 MB .bin
//   Convolution × 56  Deconvolution × 7  rife.Warp × 18  Interp × 14
//   BinaryOp × 92  Split × 56  ReLU × 40  MemoryData × 40  Crop × 45
//   Concat × 9  Eltwise × 3  PixelShuffle × 5  Sigmoid × 1  Input × 3
#pragma once

#include <QString>
#include <QStringList>
#include <vector>
#include <unordered_map>
#include <memory>
#include <cstdint>

namespace viple::rife_native_vk {

// 14 operator types we actually see in rife-v4.25-lite/flownet.param.
// `Unknown` flags any future op kind we don't recognise — graph build
// fails fast rather than silently producing zeros.
enum class OpKind : uint8_t {
    Unknown = 0,
    Input,           // 3×: in0 (prev RGB), in1 (curr RGB), in2 (timestep)
    MemoryData,      // 40×: pre-loaded learnable beta scalars
    Split,           // 56×: tensor branching, no compute
    Convolution,     // 56×: 3×3 / 4×4 conv, optional stride/dilation/relu
    Deconvolution,   // 7×:  4×4 stride 2 transpose conv (×2 upsample)
    BinaryOp,        // 92×: per-element mul/add/sub with broadcast
    ReLU,            // 40×: LeakyReLU 0.2
    Crop,            // 45×: tensor slicing
    Concat,          // 9×:  tensor concatenation along channel axis
    Interp,          // 14×: bilinear resize
    PixelShuffle,    // 5×:  depth-to-space upscale
    Sigmoid,         // 1×:  output mask
    Eltwise,         // 3×:  element-wise ops
    RifeWarp,        // 18×: ncnn-registered "rife.Warp" optical flow warp
};

const char* opKindName(OpKind k);

// Param value — int or float depending on the operator.  We keep raw
// storage and let the operator dispatcher reinterpret as needed.
struct ParamValue {
    enum Kind { Int, Float, IntArray, FloatArray };
    Kind kind = Int;
    int64_t i = 0;
    double  f = 0.0;
    std::vector<int64_t> ia;
    std::vector<double>  fa;
};

// One layer in the graph.  `inputs`/`outputs` are tensor names (strings
// in the .param file); the executor maps these to VkBuffer slots.
struct Layer {
    OpKind        kind = OpKind::Unknown;
    QString       opType;     // raw string, e.g. "rife.Warp"
    QString       name;       // unique layer name (for debug)
    QStringList   inputs;     // tensor names produced by earlier layers
    QStringList   outputs;    // tensor names this layer produces
    std::unordered_map<int, ParamValue> params; // param_id → value
};

enum class TensorDType : uint8_t {
    Float32,    // raw fp32 stored in .bin
    Float16,    // fp16 packed (preceded by 0x01306B47 flag in .bin; flag
                // is consumed during load so byteOffset points at the
                // first fp16 value, not the flag)
};

// One weight tensor consumed by a layer.  `byteOffset` indexes into
// Model::weightBlob (a verbatim copy of .bin including flags), so
// reading code must respect the type.
struct WeightTensor {
    QString     name;        // e.g. "Conv_16/weight" or "block0.convblock.0.beta"
    int         layerIdx = -1;  // back-reference into Model::layers
    int         n = 1;       // num_output (out channels)
    int         c = 1;       // input channels
    int         h = 1;
    int         w = 1;
    TensorDType dtype = TensorDType::Float32;
    size_t      elemCount = 0;  // n * c * h * w
    size_t      byteOffset = 0; // into weightBlob; points at first
                                // value (not at the fp16 flag)
};

struct Model {
    int                magic = 0;       // expect 7767517
    int                layerCount = 0;
    int                blobCount = 0;
    std::vector<Layer> layers;

    // Stats (filled by parseParam)
    std::unordered_map<OpKind, int> opCounts;

    // Filled by loadWeights: heap-resident copy of flownet.bin.
    // Indices into this blob come from `tensors[i].byteOffset`.
    std::vector<uint8_t> weightBlob;

    // One tensor per layer-output that consumed weights from .bin.
    // Order matches .param appearance (= .bin layout).  Each
    // Convolution / Deconvolution layer contributes 1 weight tensor
    // (and optionally 1 bias tensor when bias_term=1).  MemoryData
    // contributes 1 tensor.  Indexed by name for executor lookup.
    std::vector<WeightTensor>                  tensors;
    std::unordered_map<QString, int>           tensorByName; // name → index
};

// Parses the ncnn-format text .param file at `path` into `out`.
// Returns true on success; on failure logs an error and leaves `out`
// in a partial state.
bool parseParam(const QString& path, Model& out);

// Returns the tensor data as fp32, unpacking fp16 entries when needed.
// `tensorName` matches Model::tensorByName keys (e.g. "Conv_16/weight",
// "Conv_16/bias", "block0.convblock.0.beta").  Returns empty vector on
// missing tensor.  fp16→fp32 unpack handles ±0, normals, and ±inf/nan;
// subnormals are flushed to ±0 (acceptable for trained conv weights).
std::vector<float> getTensorAsFp32(const Model& m, const QString& tensorName);

// CPU reference Conv2D 3×3 with arbitrary stride/pad + optional
// LeakyReLU activation.  Used as the trusted-truth implementation that
// the GLSL compute-shader port (Phase 3b) is correctness-gated against.
//
// Input layout:  NCHW  [in: C × H × W, weight: N × C × kH × kW, bias: N]
// Output layout: NCHW  [out: N × outH × outW]
//   outH = floor((H + 2*pad - kH) / stride) + 1
//   outW = floor((W + 2*pad - kW) / stride) + 1
// Activation: leakyReluSlope == 0 → no activation; > 0 → LeakyReLU
//   (negative values × leakyReluSlope, positive pass through unchanged).
void referenceConv2D(const float* in, int inW, int inH, int inC,
                     const float* weight, int outChan, int kernelH, int kernelW,
                     const float* bias /* nullable */,
                     int strideH, int strideW, int padH, int padW,
                     float leakyReluSlope,
                     float* out, int outW, int outH);

// Vulkan handles + ProcAddr accessor needed to run a self-contained
// compute test.  Caller (e.g. VkFrucRenderer / PlVkRenderer init) fills
// these with already-initialised handles after ncnn handoff is alive.
struct VulkanCtx {
    void* /*VkInstance*/        instance = nullptr;
    void* /*VkPhysicalDevice*/  physicalDevice = nullptr;
    void* /*VkDevice*/          device = nullptr;
    uint32_t                    computeQueueFamily = 0;
    void* /*VkQueue*/           computeQueue = nullptr;
    void* /*PFN_vkGetInstanceProcAddr*/ getInstanceProcAddr = nullptr;
};

// Phase 3b.2 / 4a — runs the Conv2D 3×3 GLSL compute shader on a chosen
// Conv layer's weights against a deterministic 64×64×inC input (where
// inC is read from the layer's weight tensor shape), reads back GPU
// output, and compares pixel-wise to the CPU reference (Phase 3a).
// Returns true only when every output element matches within
// `tolerance` (default 1e-4 — fp32 accumulator + fp16 unpack
// quantisation).  Logs PASS/FAIL + max-abs-error + first 5 GPU samples.
//
// Requires: ncnn::create_gpu_instance() already called on this thread
// (compile_spirv_module needs ncnn's glslang context) AND `ctx`'s
// VkDevice has a compute queue.  `m` must be already loaded (parseParam
// + loadWeights both succeeded).
//
// The shader supports any (kernel, stride, pad, dilation, activation,
// bias) values via push constants — Phase 4a audit confirmed all 56
// Conv layers in rife-v4.25-lite are k=3 p=1 d=1 bias=on, with stride
// in {1,2} and activation in {none, LeakyReLU 0.2}.  This function
// reads stride / pad / kernel / activation_type / activation_params
// directly from `m.layers[layerName].params` so any conv layer in the
// graph can be tested against the trusted CPU reference.
// Default tolerance bumped from 1e-4 → 5e-4 in §J.3.e.Y 4Y.5a: fp16
// weight storage adds ~5e-4 relative-precision floor that compounds
// through ~3000 MAC accumulation to ~1e-4 max_err.  Production
// vs-ncnn gate (Final.1) is unaffected — it uses its own thresholds
// and actually got TIGHTER under fp16 (mean 4.23e-4 → 4.72e-5).
bool runConv2DGpuTest(const VulkanCtx& ctx,
                      const Model& m,
                      const QString& layerName,
                      float tolerance = 5e-4f);

// Phase 4b — element-wise BinaryOp: ADD/SUB/MUL/DIV/RSUB across three
// broadcast modes.  Handles all 92 BinaryOp layers in rife-v4.25-lite.
// `mode` selects the layout of `b`:
//   0 = same-shape NCHW (length == count)
//   1 = channel broadcast (length == channels; each channel index gets
//       broadcast over hw spatial positions)
//   2 = scalar (length == 1; ignored, scalarB used instead)
// `opType` follows ncnn's BinaryOp param 0 (0=ADD ... 9=RPOW).
//
// Returns true on PASS (max abs error within `tolerance`).
struct BinaryOpDesc {
    int   opType  = 0;       // ncnn op_type
    int   mode    = 0;       // 0 same / 1 channel-bcast / 2 scalar
    int   channels = 1;      // used when mode==1
    int   hw       = 1;      // used when mode==1
    float scalarB  = 0.0f;   // used when mode==2
};
bool runBinaryOpGpuTest(const VulkanCtx& ctx,
                        const BinaryOpDesc& desc,
                        const std::vector<float>& a,
                        const std::vector<float>& b /* may be empty if mode==2 */,
                        float tolerance = 1e-5f);

// CPU reference for BinaryOp.  `out.size()` must equal `a.size()`.
// `b` semantics follow `desc.mode` exactly (see runBinaryOpGpuTest).
void referenceBinaryOp(const BinaryOpDesc& desc,
                       const float* a, size_t aCount,
                       const float* b /* may be nullptr if mode==2 */,
                       float* out);

const char* getBinaryOpShaderGlsl();

// Phase 4c — element-wise Activation: ReLU/LeakyReLU + Sigmoid.  Covers
// 40 ReLU layers (all slope=0.2 in this model) + 1 Sigmoid.  Note: the
// 16 Conv layers with fused LeakyReLU are NOT routed through here —
// their activation is folded into kConv2DShaderGlsl via leakyReluSlope.
struct ActivationDesc {
    int   actType = 0;       // 0 = ReLU/LeakyReLU; 1 = Sigmoid
    float slope   = 0.0f;    // negative-side multiplier for ReLU; 0 for pure ReLU
};
bool runActivationGpuTest(const VulkanCtx& ctx,
                          const ActivationDesc& desc,
                          const std::vector<float>& a,
                          float tolerance = 1e-5f);

// CPU reference for Activation.  `out.size()` must equal `a.size()`.
void referenceActivation(const ActivationDesc& desc,
                         const float* a, size_t aCount,
                         float* out);

const char* getActivationShaderGlsl();

// Phase 4d — shape ops.  See cpp for op-by-op coverage notes.

// Linear copy with src/dst element offsets.  Powers Concat (multi-pass)
// + Crop (single pass with srcOffset).  Pure positional shader, no
// math — GPU/CPU output match exactly.
const char* getCopyShaderGlsl();
void referenceCopy(const float* in, float* out,
                   size_t count, size_t srcOffset, size_t dstOffset);
bool runCropGpuTest(const VulkanCtx& ctx,
                    int inC, int inH, int inW,
                    int cStart, int cEnd,
                    float tolerance = 1e-5f);
bool runConcatGpuTest(const VulkanCtx& ctx,
                      int inA_C, int inB_C, int H, int W,
                      float tolerance = 1e-5f);

// Depth-to-space (PixelShuffle) with upscale factor r (audit: r=2 only
// in flownet).  Input (Cin, Hin, Win) → output (Cin/r², Hin*r, Win*r).
const char* getPixelShuffleShaderGlsl();
void referencePixelShuffle(const float* in, float* out,
                           int inC, int inH, int inW, int r);
bool runPixelShuffleGpuTest(const VulkanCtx& ctx,
                            int inC, int inH, int inW, int r,
                            float tolerance = 1e-5f);

// Bilinear resize (Interp resize_type=2, align_corners=false).
const char* getInterpBilinearShaderGlsl();
void referenceInterpBilinear(const float* in, float* out,
                             int channels, int inH, int inW,
                             int outH, int outW);
bool runInterpBilinearGpuTest(const VulkanCtx& ctx,
                              int channels, int inH, int inW,
                              int outH, int outW,
                              float tolerance = 1e-5f);

// Weighted SUM of 2 inputs: out[i] = c0 * a[i] + c1 * b[i].
const char* getEltwiseShaderGlsl();
void referenceEltwiseSum(const float* a, const float* b, float* out,
                         size_t count, float c0, float c1);
bool runEltwiseSumGpuTest(const VulkanCtx& ctx,
                          size_t count, float c0, float c1,
                          float tolerance = 1e-5f);

// Phase 4e — Deconvolution (transposed conv).  All 7 Deconv layers in
// flownet are k=4 s=2 p=1 bias=on no-activation; output is exactly 2×
// upsample.  Weight layout in ncnn is (in_ch, out_ch, kH, kW) — note
// the swap relative to Conv's (out_ch, in_ch, kH, kW).
const char* getDeconv2DShaderGlsl();
void referenceDeconv2D(const float* in, int inW, int inH, int inC,
                       const float* weight, int outChan, int kernelH, int kernelW,
                       const float* bias /* nullable */,
                       int strideH, int strideW, int padH, int padW,
                       float* out, int outW, int outH);
// Reads stride / pad / kernel / bias_term from the named layer's
// params; pulls weight + bias from the loaded model.
bool runDeconv2DGpuTest(const VulkanCtx& ctx,
                        const Model& m,
                        const QString& layerName,
                        float tolerance = 5e-4f);  // see Conv2D tolerance note above

// Phase 4f — rife.Warp custom op.  Optical-flow-driven bilinear sample.
// 2 inputs: image (C,H,W) + flow (2,H,W); 1 output (C,H,W).
const char* getRifeWarpShaderGlsl();
void referenceRifeWarp(const float* image, const float* flow,
                       float* out, int channels, int H, int W);
bool runRifeWarpGpuTest(const VulkanCtx& ctx,
                        int channels, int H, int W,
                        float tolerance = 1e-5f);

// ===== Phase 4g graph executor — incremental rollout starts here =====
//
// 4g.1: tensor shape inference.  Given input shapes (in0/in1/in2),
// propagates through every layer to compute each blob's (C, H, W) so
// downstream phases (buffer pool, dispatch handlers, end-to-end gate)
// have known sizes to allocate against.  Returns false on any
// unrecognised op or unresolvable shape.

struct BlobShape {
    int c = 0;
    int h = 0;
    int w = 0;
    bool valid() const { return c > 0 && h > 0 && w > 0; }
    bool operator==(const BlobShape& o) const { return c == o.c && h == o.h && w == o.w; }
};

struct InferInputs {
    BlobShape in0;  // prev RGB frame
    BlobShape in1;  // curr RGB frame
    BlobShape in2;  // timestep scalar (typically 1×1×1)
};

bool inferBlobShapes(const Model& m,
                     const InferInputs& inputs,
                     std::unordered_map<QString, BlobShape>& blobShapes);

// 4g.2: pipeline cache smoke.  Builds + tears down all 9 shader
// pipelines (Conv2D / Deconv2D / BinaryOp / Activation / Copy /
// PixelShuffle / InterpBilinear / EltwiseSum / RifeWarp) once, to
// verify the persistent-pipeline path works against the live device.
// 4g.3+ promotes the cache to executor lifetime ownership.
bool runPipelineCacheSmoke(const VulkanCtx& ctx);

// 4g.3: buffer pool smoke.  Loads model + .bin, infers shapes for a
// canonical 256×256 input, allocates one host-visible mapped buffer
// per blob (485) and per weight/bias tensor (~118), pre-fills
// MemoryData + Conv/Deconv weight/bias from the .bin blob.  Verifies
// counts + a known-value spot check (block0.convblock.0.beta first 3
// fp32 should be ~0.022, ~0.103, ~0.053 — the existing anchor from
// raw .bin inspection).  Tears down cleanly.
bool runBlobBufferPoolSmoke(const VulkanCtx& ctx, const QString& modelDir);

// 4g.4: graph executor traversal smoke.  Combines pipelines (4g.2) +
// buffers (4g.3), walks all 389 layers in declaration order, records
// a vkCmdDispatch per non-trivial op (Conv2D / Deconv2D / BinaryOp /
// Activation / Concat=Copy×N / Crop=Copy / PixelShuffle / Interp /
// Eltwise / RifeWarp / Split=Copy×N).  Input layers are seeded with
// deterministic synthetic data; MemoryData blobs were already
// pre-filled at buffer-pool init.  Submit + fence wait completes
// without VkResult errors → PASS.  Does not yet check output values
// (that's 4g.6 vs ncnn).
bool runGraphExecutorSmoke(const VulkanCtx& ctx, const QString& modelDir);

// ===== §J.3.e.X Final.1 — production-shape API =====
//
// RifeNativeExecutor encapsulates the graph executor lifecycle so
// callers (VkFrucRenderer when Final.3 lands) can:
//   1. initialize(opts)  — once: load model, build pipelines + buffers
//   2. runInference(...) — per-frame: upload inputs, dispatch, readback
//   3. shutdown()        — once: destroy everything
//
// Per-frame cost is just descriptor-set allocation + command buffer
// recording + submit + wait; shader compilation + pipeline build only
// happens at initialize().  The persistent state matches what 4g.2 +
// 4g.3 + 4g.4 standalone smoke functions assemble each call, but
// keeping it alive across frames is the whole point of Final.1.

class RifeNativeExecutor {
public:
    struct InitOptions {
        VulkanCtx  ctx;
        QString    modelDir;
        BlobShape  in0Shape;   // prev RGB frame  (typically (3, H, W))
        BlobShape  in1Shape;   // curr RGB frame  (matches in0Shape)
        BlobShape  in2Shape;   // timestep scalar (typically (1, 1, 1))
        // Final.2: optional VkPipelineCache file path for cross-launch
        // persistence.  "" = no persistence (every launch pays full
        // SPIR-V → driver-binary translation cost ≈ 50-300 ms total
        // for our 9 shaders).  When set, init loads existing cache
        // bytes if the file exists, and shutdown writes back the
        // accumulated cache (cheap; existing entries reused, new ones
        // appended).  Cache binaries are GPU+driver-version specific
        // — Vulkan validates header before using.
        QString    pipelineCachePath;
    };

    RifeNativeExecutor();
    ~RifeNativeExecutor();

    // Non-copyable, non-movable (owns Vulkan resources).
    RifeNativeExecutor(const RifeNativeExecutor&) = delete;
    RifeNativeExecutor& operator=(const RifeNativeExecutor&) = delete;

    bool initialize(const InitOptions& opts);
    bool initialized() const;
    void shutdown();

    // Output blob shape, valid after initialize() succeeds.
    BlobShape outputShape() const;

    // Per-frame inference.  in0Data / in1Data layouts match in0Shape /
    // in1Shape (CHW packed fp32).  timestep is a single fp32 scalar.
    // out0Data must be sized to outputShape().c * .h * .w fp32 floats;
    // CHW layout, ready for upload to the next stage in the FRUC chain.
    bool runInference(const float* in0Data,
                      const float* in1Data,
                      float        timestep,
                      float*       out0Data);

    // §J.3.e.Y 4Y.0 — per-phase wall-clock breakdown of the most
    // recent runInference call.  Phases:
    //   seedMs     — memcpy in0/in1/in2 from caller buffers into blobs
    //   recordMs   — reset pools + record 389 dispatches into cmdbuf
    //   gpuWaitMs  — submit + fence wait (= effective GPU compute time)
    //   readbackMs — memcpy out0 blob into caller buffer
    // Sum is the total wall-clock cost of runInference.
    struct LastTiming {
        double seedMs     = 0.0;
        double recordMs   = 0.0;
        double gpuWaitMs  = 0.0;
        double readbackMs = 0.0;
    };
    LastTiming lastTiming() const;

private:
    struct Impl;
    Impl* m_impl = nullptr;
};

// Final.1 smoke: init → run twice (deterministic check) → vs ncnn
// (same gate as 4g.6) → shutdown.  Verifies the lifecycle works AND
// the per-frame run produces identical output across calls (no state
// leakage between frames).
bool runProductionApiSmoke(const VulkanCtx& ctx, const QString& modelDir);

// Final.3a: per-frame latency benchmark of RifeNativeExecutor at the
// production input shape (typically 256×256 RGB pair + scalar t).
// Runs `warmup` discarded iterations, then `iterations` timed runs;
// reports min / median / max wall-clock ms.  Cross-checks against
// ncnn (same input → same iterations → same metrics) so the caller
// can see the relative perf difference before committing to the
// runtime swap (Final.3b).  Returns true when both engines produce
// metrics; the verdict is informational, not pass/fail.
bool runProductionApiBenchmark(const VulkanCtx& ctx,
                               const QString& modelDir,
                               int warmup = 5,
                               int iterations = 30);

// GLSL compute shader source for Conv2D with arbitrary stride/pad/kernel
// + optional fused LeakyReLU.  Returned string is a complete shader,
// ready to feed into ncnn::compile_spirv_module / glslangValidator.
//
// Binding layout (matches descriptor set 0):
//   binding 0: readonly storage buffer  — input  (fp32, NCHW)
//   binding 1: readonly storage buffer  — weight (fp32, NCHW)
//   binding 2: readonly storage buffer  — bias   (fp32, [N], may be zero-len if no bias)
//   binding 3: writeonly storage buffer — output (fp32, NCHW)
//
// Push constants (16 ints + 1 float = 68 bytes; round up to 80 for alignment):
//   ivec4 inDims;   // x=inW, y=inH, z=inC, w=hasBias (0/1)
//   ivec4 outDims;  // x=outW, y=outH, z=outChan, w=_pad
//   ivec4 conv;     // x=kernelW, y=kernelH, z=strideW (or strideH if same), w=pad
//   float leakyReluSlope;  // 0 = no activation
//
// Workgroup: local_size 8×8×1 (= 64 threads).  Dispatch
//   ((outW + 7) / 8, (outH + 7) / 8, outChan).
// Each thread computes one output element (n, oy, ox).
const char* getConv2DShaderGlsl();

// Phase 3a smoke — pulls Conv_16 weight+bias from the loaded Model,
// generates a deterministic 64×64×3 random input, runs reference Conv
// CPU, and logs first/min/max/mean output stats.  Catches any fp16-unpack
// bug or weight-extraction error in our pipeline before we sink time
// into shader plumbing.  Returns true on PASS (no NaN/Inf, output
// magnitudes plausible).
bool runConv2DCpuSmoke(const Model& m);

// Reads `binPath` (raw fp32 packed, no per-tensor headers — the ncnn
// "compact" .bin format produced by ncnn2bin / ncnn2int8 with the
// no-header flag), walks `m.layers` in order to attribute byte ranges
// to per-layer WeightTensor entries.  Convolution / Deconvolution /
// MemoryData are weight-consuming.  Other ops (BinaryOp / ReLU / etc.)
// don't read .bin.
//
// Layout (matches .param appearance order):
//   • Convolution: param 6 = weight_data_size (n*c*h*w) → fp32 weights;
//     if param 5 (bias_term) == 1, then num_output (param 0) more
//     fp32 follow as bias.  param 1 = kernel size; in_channels =
//     weight_data_size / (kernel*kernel*num_output).
//   • Deconvolution: same layout as Convolution.  Note: weight order
//     in ncnn for transposed conv is (in_ch, out_ch, kh, kw) — caller
//     must transpose if shader expects (out_ch, in_ch, kh, kw).
//   • MemoryData: param 0/1/2 = w/h/c → w*h*c fp32.
//
// On success, total bytes consumed must equal binPath file size; if
// not, logs a mismatch and returns false (likely .param/.bin out of
// sync — different model build).
bool loadWeights(const QString& binPath, Model& m);

// One-shot scaffold smoke — invoked by plvk.cpp init when
// VIPLE_RIFE_NATIVE_VK_DUMP=1 is set.  Logs layer count / op
// distribution / first/last 3 layers summary.  No GPU work yet.
//
// modelDir is the rife-v4.25-lite directory containing
// flownet.param + flownet.bin.
void dumpModelSmoke(const QString& modelDir);

// Phase 3b.2 — fully self-contained correctness gate.  Creates its own
// VkInstance/VkPhysicalDevice/VkDevice/queue, hands them to ncnn via
// create_gpu_instance_external (so ncnn::compile_spirv_module works),
// runs runConv2DGpuTest against Conv_16 weights, then tears everything
// down.  Designed as a CLI smoke target — invoked early in main.cpp when
// VIPLE_RIFE_NATIVE_VK_TEST=1.  Does NOT need an active streaming
// session or VkFrucRenderer.
//
// modelDir is the rife-v4.25-lite directory containing flownet.param +
// flownet.bin.  Returns true on PASS, false on any failure (logs all
// diagnostics via SDL_Log[Info|Warn|Error]).
//
// Default tolerance bumped to 5e-4 in §J.3.e.Y 4Y.5a alongside fp16
// weight storage; see Conv2D tolerance note above for full reasoning.
bool runConv2DGpuTestStandalone(const QString& modelDir,
                                float tolerance = 5e-4f);

} // namespace viple::rife_native_vk

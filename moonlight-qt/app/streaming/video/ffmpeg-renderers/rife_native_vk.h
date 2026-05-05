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

// Phase 3b.2 — runs the Conv2D 3×3 GLSL compute shader on real Conv_16
// weights against a deterministic 64×64×3 input, reads back GPU output,
// and compares pixel-wise to the CPU reference (Phase 3a).  Returns true
// only when every output element matches within `tolerance` (default
// 1e-4 — fp32 accumulator + fp16 unpack quantisation).  Logs PASS/FAIL +
// max-abs-error + first 5 GPU samples (for visual cross-check vs the
// CPU samples already logged by runConv2DCpuSmoke).
//
// Requires: ncnn::create_gpu_instance() already called on this thread
// (compile_spirv_module needs ncnn's glslang context) AND `ctx`'s
// VkDevice has a compute queue.
//
// modelDir is the directory containing flownet.param + flownet.bin.
bool runConv2DGpuTest(const VulkanCtx& ctx, const QString& modelDir,
                      float tolerance = 1e-4f);

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
bool runConv2DGpuTestStandalone(const QString& modelDir,
                                float tolerance = 1e-4f);

} // namespace viple::rife_native_vk

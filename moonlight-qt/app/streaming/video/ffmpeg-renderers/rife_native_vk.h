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

struct Model {
    int                magic = 0;       // expect 7767517
    int                layerCount = 0;
    int                blobCount = 0;
    std::vector<Layer> layers;

    // Stats (filled by parseParam)
    std::unordered_map<OpKind, int> opCounts;
};

// Parses the ncnn-format text .param file at `path` into `out`.
// Returns true on success; on failure logs an error and leaves `out`
// in a partial state.
bool parseParam(const QString& path, Model& out);

// One-shot scaffold smoke — invoked by plvk.cpp init when
// VIPLE_RIFE_NATIVE_VK_DUMP=1 is set.  Logs layer count / op
// distribution / first/last 3 layers summary.  No GPU work yet.
//
// modelDir is the rife-v4.25-lite directory containing
// flownet.param + flownet.bin.
void dumpModelSmoke(const QString& modelDir);

} // namespace viple::rife_native_vk
